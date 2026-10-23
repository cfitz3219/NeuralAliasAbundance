// C4 (2026-10) rewrite of the A23 GPU timer, fixing R-001 (Astra's review):
//
//  * Completion is PROVEN, not assumed from a Present count. After resolving
//    the two timestamps, the same command list writes a unique 64-bit ticket
//    into the slot's readback buffer with WriteBufferImmediate in
//    MARKER_OUT mode, which the API guarantees lands only after all preceding
//    commands in that list have finished writing. Poll() accepts a slot only
//    when it reads back exactly the ticket it issued, so stale stamps from an
//    earlier use of the slot or an unexecuted list are never measured.
//  * Slots are never reused before their ticket is observed. A slot that
//    stays unconfirmed for kAbandonPresents (list discarded or executed very
//    late) is QUARANTINED for the rest of the process rather than reused, so
//    a late execution can never overwrite a newer measurement. If every slot
//    ends up quarantined the module disables itself.
//  * Begin/End pairs are tracked PER COMMAND LIST (evaluates arrive from many
//    engine threads), not through one global "current slot".
//  * Frequency: the timestamp frequency belongs to the queue that executed
//    the queries. The host supplies its captured DIRECT queue to Poll(); only
//    lists of type DIRECT are measured. Other list types are counted as
//    unsupported instead of being converted with the wrong frequency.
#include "host_dx12/ngx_observe/gpu_timing.hpp"

#include <wrl/client.h>

#include <atomic>
#include <cstring>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace na::gputime {
namespace {

constexpr uint32_t kSlots = 16;
constexpr uint64_t kAbandonPresents = 240; // ~4 s at 60 fps without completion evidence
constexpr uint64_t kReadbackBytes = 24;    // [0] begin stamp, [8] end stamp, [16] ticket

enum class SlotState : uint8_t { Free, Begun, Armed, Quarantined };

struct Slot {
    ComPtr<ID3D12Resource> readback;
    SlotState state = SlotState::Free;
    ID3D12GraphicsCommandList* list = nullptr; // identity only while Begun; never dereferenced later
    uint64_t ticket = 0;
    uint64_t issued_present = 0;
};

std::mutex g_m;
ComPtr<ID3D12Device> g_device;
ComPtr<ID3D12QueryHeap> g_heap;
Slot g_slots[kSlots];
uint64_t g_next_ticket = 1;
uint64_t g_present_counter = 0;
uint64_t g_freq = 0;
std::atomic<bool> g_enabled{true};
std::atomic<bool> g_failed{false};
std::atomic<float> g_last{0.0f}, g_avg{0.0f};
std::atomic<uint64_t> g_samples{0};
std::atomic<uint64_t> g_skipped_busy{0}, g_unsupported{0}, g_quarantined{0};

bool ensure()
{
    if (g_heap) return true;
    if (g_failed.load() || !g_device) return false;
    // The completion ticket needs WriteBufferImmediate on DIRECT lists. Without
    // it no measurement can be proven complete, so the module stays off rather
    // than guessing (and never issues an unsupported command into a game list).
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3{};
    if (FAILED(g_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &o3, sizeof(o3))) ||
        !(o3.WriteBufferImmediateSupportFlags & D3D12_COMMAND_LIST_SUPPORT_FLAG_DIRECT)) {
        g_failed.store(true);
        return false;
    }
    D3D12_QUERY_HEAP_DESC qh{};
    qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qh.Count = kSlots * 2;
    if (FAILED(g_device->CreateQueryHeap(&qh, IID_PPV_ARGS(&g_heap)))) { g_failed.store(true); return false; }
    for (Slot& s : g_slots) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = kReadbackBytes;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                     nullptr, IID_PPV_ARGS(&s.readback)))) {
            g_failed.store(true);
            g_heap.Reset();
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
    if (list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) { g_unsupported.fetch_add(1); return; }
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
    s.list = nullptr;
    ComPtr<ID3D12GraphicsCommandList2> list2;
    if (FAILED(list->QueryInterface(IID_PPV_ARGS(&list2)))) {
        // No completion evidence possible on this list: release the slot
        // without measuring (the Begin stamp is simply never read).
        s.state = SlotState::Free;
        g_unsupported.fetch_add(1);
        return;
    }
    const uint32_t i = static_cast<uint32_t>(idx);
    list->EndQuery(g_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i * 2 + 1);
    list->ResolveQueryData(g_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i * 2, 2, s.readback.Get(), 0);
    s.ticket = g_next_ticket++;
    const D3D12_GPU_VIRTUAL_ADDRESS base = s.readback->GetGPUVirtualAddress();
    D3D12_WRITEBUFFERIMMEDIATE_PARAMETER params[2] = {
        {base + 16, static_cast<UINT>(s.ticket & 0xffffffffu)},
        {base + 20, static_cast<UINT>(s.ticket >> 32)}};
    D3D12_WRITEBUFFERIMMEDIATE_MODE modes[2] = {D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT,
                                                D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT};
    list2->WriteBufferImmediate(2, params, modes);
    s.issued_present = g_present_counter;
    s.state = SlotState::Armed;
}

void Poll(ID3D12CommandQueue* queue)
{
    if (!g_enabled.load()) return;
    std::lock_guard<std::mutex> lock(g_m);
    ++g_present_counter;
    if (!g_heap) return;
    if (!g_freq && queue) {
        const D3D12_COMMAND_QUEUE_DESC desc = queue->GetDesc();
        if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) queue->GetTimestampFrequency(&g_freq);
    }
    uint32_t quarantined = 0;
    for (Slot& s : g_slots) {
        if (s.state == SlotState::Quarantined) { ++quarantined; continue; }
        if (s.state != SlotState::Armed) continue;
        uint64_t data[3] = {0, 0, 0};
        void* mapped = nullptr;
        D3D12_RANGE r{0, kReadbackBytes};
        if (SUCCEEDED(s.readback->Map(0, &r, &mapped))) {
            std::memcpy(data, mapped, sizeof(data));
            D3D12_RANGE none{0, 0};
            s.readback->Unmap(0, &none);
        }
        if (data[2] != s.ticket) {
            if (g_present_counter - s.issued_present >= kAbandonPresents) {
                s.state = SlotState::Quarantined; // never reused: a late execution must not clobber a newer sample
                g_quarantined.fetch_add(1);
                ++quarantined;
            }
            continue;
        }
        s.state = SlotState::Free; // proven complete: the slot may be reused
        if (!g_freq || data[1] < data[0]) continue;
        const float ms = static_cast<float>(static_cast<double>(data[1] - data[0]) * 1000.0 /
                                            static_cast<double>(g_freq));
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
