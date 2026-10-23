// C4 (2026-10) rewrite of the A23 GPU timer. Fixes R-001 and, after Gate 1,
// Astra's R-202 (a GPU-written ticket orders GPU writes but cannot order the
// CPU's earlier reads of the same mapped memory, so old stamps could be
// accepted with a new ticket). The protocol is now fence-based:
//
//  1. Begin/End record two timestamps + ResolveQueryData into the slot's
//     readback buffer on the game's list. Pairs are tracked PER LIST
//     (evaluates arrive from many engine threads).
//  2. OnExecuted() runs in the host's ExecuteCommandLists detour right after
//     the ORIGINAL call returned. For every armed slot whose list is in that
//     submission it signals the host's own fence on THAT queue and records the
//     value and the queue's timestamp frequency. A queue Signal is ordered
//     after all work previously submitted to that queue, so fence >= value
//     proves the resolve completed (the documented D3D12 CPU/GPU contract).
//  3. Poll() (Present hook) checks the fence with GetCompletedValue() — never a
//     wait — and only then maps and reads the stamps.
//
// Slots are reused only after their submission is proven complete. A slot that
// is never submitted through the hook or never completes is QUARANTINED after
// kAbandonPresents and never reused, so a late execution can't overwrite a newer
// sample. Frequency comes from the queue that executed the list.
//
// Known limit: list OBJECTS are reused every frame (Reset + re-record), so a
// list pointer seen again is normally a new recording and is ignored. A game
// that re-executes the SAME recording (legal, rare) without Reset would resolve
// into the old slot a second time; if that slot was reused meanwhile, one sample
// could be wrong. Distinguishing that needs Reset observation (recording
// generations, available via the variant registry once wired) — not a crash or
// a stall, at worst one bad timing value.
#include "host_dx12/ngx_observe/gpu_timing.hpp"

#include <wrl/client.h>

#include <atomic>
#include <cstring>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace na::gputime {
namespace {

constexpr uint32_t kSlots = 16;
constexpr uint64_t kAbandonPresents = 240; // ~4 s at 60 fps without completion

enum class SlotState : uint8_t { Free, Begun, Armed, Submitted, Quarantined };

struct Slot {
    ComPtr<ID3D12Resource> readback;           // 16 bytes: begin/end stamps
    SlotState state = SlotState::Free;
    ID3D12GraphicsCommandList* list = nullptr; // identity while Begun; never dereferenced after End
    ID3D12CommandList* last_list = nullptr;    // identity of the list carrying this slot's resolve (until completion)
    uint64_t fence_value = 0;
    uint64_t freq = 0;
    uint64_t armed_present = 0;
};

std::mutex g_m;
ComPtr<ID3D12Device> g_device;
ComPtr<ID3D12QueryHeap> g_heap;
ComPtr<ID3D12Fence> g_fence;
uint64_t g_fence_next = 0;
Slot g_slots[kSlots];
uint64_t g_present_counter = 0;
std::atomic<bool> g_enabled{true};
std::atomic<bool> g_failed{false};
std::atomic<float> g_last{0.0f}, g_avg{0.0f};
std::atomic<uint64_t> g_samples{0};
std::atomic<uint64_t> g_skipped_busy{0}, g_unsupported{0}, g_quarantined{0};
std::atomic<bool> g_any_resolved{false}; // fast path: no slot has ever resolved -> OnExecuted is a no-op

bool ensure()
{
    if (g_heap) return true;
    if (g_failed.load() || !g_device) return false;
    D3D12_QUERY_HEAP_DESC qh{};
    qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qh.Count = kSlots * 2;
    if (FAILED(g_device->CreateQueryHeap(&qh, IID_PPV_ARGS(&g_heap))) ||
        FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) {
        g_heap.Reset();
        g_fence.Reset();
        g_failed.store(true);
        return false;
    }
    for (Slot& s : g_slots) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 16;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                     nullptr, IID_PPV_ARGS(&s.readback)))) {
            g_failed.store(true);
            g_heap.Reset();
            g_fence.Reset();
            return false;
        }
    }
    return true;
}

int find_begun(ID3D12GraphicsCommandList* list)
{
    for (uint32_t i = 0; i < kSlots; ++i)
        if (g_slots[i].state == SlotState::Begun && g_slots[i].list == list) return static_cast<int>(i);
    return -1;
}

void quarantine(Slot& s)
{
    s.state = SlotState::Quarantined;
    s.list = nullptr;
    g_quarantined.fetch_add(1);
}

} // namespace

void SetEnabled(bool enabled) { g_enabled.store(enabled); }

void SetDevice(ID3D12Device* device)
{
    std::lock_guard<std::mutex> lock(g_m);
    if (!g_device && device) g_device = device;
}

void Begin(ID3D12GraphicsCommandList* list)
{
    if (!g_enabled.load() || !list) return;
    std::lock_guard<std::mutex> lock(g_m);
    if (!ensure()) return;
    if (find_begun(list) >= 0) return; // nested/duplicate Begin on the same list: keep the outer pair
    const D3D12_COMMAND_LIST_TYPE type = list->GetType();
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COMPUTE) {
        g_unsupported.fetch_add(1);
        return;
    }
    for (uint32_t i = 0; i < kSlots; ++i) {
        Slot& s = g_slots[i];
        if (s.state != SlotState::Free) continue;
        s.state = SlotState::Begun;
        s.list = list;
        list->EndQuery(g_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i * 2);
        return;
    }
    g_skipped_busy.fetch_add(1); // every slot in flight or quarantined: skip, never stall
}

void End(ID3D12GraphicsCommandList* list)
{
    if (!g_enabled.load() || !list) return;
    std::lock_guard<std::mutex> lock(g_m);
    if (!g_heap) return;
    const int idx = find_begun(list);
    if (idx < 0) return;
    Slot& s = g_slots[idx];
    const uint32_t i = static_cast<uint32_t>(idx);
    list->EndQuery(g_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i * 2 + 1);
    list->ResolveQueryData(g_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i * 2, 2, s.readback.Get(), 0);
    s.last_list = list;
    s.list = nullptr;
    s.fence_value = 0;
    s.armed_present = g_present_counter;
    s.state = SlotState::Armed;
    g_any_resolved.store(true, std::memory_order_release);
}

void OnExecuted(ID3D12CommandQueue* queue, uint32_t count, ID3D12CommandList* const* lists)
{
    if (!queue || !lists || !count || !g_any_resolved.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(g_m);
    if (!g_heap || !g_fence) return;
    bool signal = false;
    for (Slot& s : g_slots) {
        if (s.state != SlotState::Armed) continue; // only the first submission after End carries our resolve
        for (uint32_t k = 0; k < count; ++k) {
            if (lists[k] == s.last_list) { s.state = SlotState::Submitted; signal = true; break; }
        }
    }
    if (!signal) return;
    uint64_t freq = 0;
    if (FAILED(queue->GetTimestampFrequency(&freq))) freq = 0;
    const uint64_t value = ++g_fence_next;
    const bool signalled = SUCCEEDED(queue->Signal(g_fence.Get(), value));
    for (Slot& s : g_slots) {
        if (s.state != SlotState::Submitted || s.fence_value != 0) continue;
        // On a failed Signal no completion evidence can ever arrive: leave fence_value 0
        // so the abandon window quarantines the slot instead of reading it.
        if (signalled) { s.fence_value = value; s.freq = freq; }
    }
}

void Poll(ID3D12CommandQueue* /*queue*/)
{
    if (!g_enabled.load()) return;
    std::lock_guard<std::mutex> lock(g_m);
    ++g_present_counter;
    if (!g_heap || !g_fence) return;
    const uint64_t completed = g_fence->GetCompletedValue();
    if (completed == UINT64_MAX) { // device removed: nothing is trustworthy any more
        g_failed.store(true);
        return;
    }
    uint32_t quarantined = 0;
    for (Slot& s : g_slots) {
        if (s.state == SlotState::Quarantined) { ++quarantined; continue; }
        if (s.state != SlotState::Armed && s.state != SlotState::Submitted) continue;
        const bool done = s.state == SlotState::Submitted && s.fence_value != 0 && completed >= s.fence_value;
        if (!done) {
            if (g_present_counter - s.armed_present >= kAbandonPresents) { quarantine(s); ++quarantined; }
            continue;
        }
        // Completion proven by the fence: the resolve's writes are finished; read the payload.
        uint64_t stamps[2] = {0, 0};
        void* mapped = nullptr;
        D3D12_RANGE r{0, sizeof(stamps)};
        const bool mapped_ok = SUCCEEDED(s.readback->Map(0, &r, &mapped));
        if (mapped_ok) {
            std::memcpy(stamps, mapped, sizeof(stamps));
            D3D12_RANGE none{0, 0};
            s.readback->Unmap(0, &none);
        }
        const uint64_t freq = s.freq;
        s.state = SlotState::Free;
        s.fence_value = 0;
        s.last_list = nullptr;
        if (!mapped_ok || !freq || stamps[1] < stamps[0]) continue;
        const float ms = static_cast<float>(static_cast<double>(stamps[1] - stamps[0]) * 1000.0 /
                                            static_cast<double>(freq));
        g_last.store(ms);
        const float prev = g_avg.load();
        g_avg.store(prev > 0 ? prev * 0.95f + ms * 0.05f : ms);
        g_samples.fetch_add(1);
    }
    if (quarantined == kSlots) g_failed.store(true);
}

Stats GetStats()
{
    return Stats{g_last.load(), g_avg.load(), g_samples.load(),
                 (g_enabled.load() && !g_failed.load()) ? 1u : 0u,
                 g_skipped_busy.load(), g_unsupported.load(), g_quarantined.load()};
}

} // namespace na::gputime
