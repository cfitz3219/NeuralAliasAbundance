#include "host_dx12/ngx_observe/gpu_timing.hpp"

#include <wrl/client.h>

#include <atomic>
#include <cstring>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace na::gputime {
namespace {

constexpr uint32_t kSlots = 8;     // ring of in-flight measurements
constexpr uint32_t kReadDelay = 3; // presents before a slot is read

struct Slot {
    ComPtr<ID3D12Resource> readback;   // 16 bytes: begin/end stamps
    uint64_t issued_present = 0;       // present counter when End() ran
    bool armed = false;                // End() recorded, not yet read
};

std::mutex g_m;
ComPtr<ID3D12Device> g_device;
ComPtr<ID3D12QueryHeap> g_heap;
Slot g_slots[kSlots];
uint32_t g_next = 0;         // slot used by the current Begin/End pair
bool g_begun = false;
uint64_t g_present_counter = 0;
std::atomic<bool> g_enabled{true};
std::atomic<bool> g_failed{false};
std::atomic<float> g_last{0.0f}, g_avg{0.0f};
std::atomic<uint64_t> g_samples{0};
uint64_t g_freq = 0;

bool ensure()
{
    if (g_heap) return true;
    if (g_failed.load() || !g_device) return false;
    ComPtr<ID3D12Device> dev = g_device;
    D3D12_QUERY_HEAP_DESC qh{};
    qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qh.Count = kSlots * 2;
    if (FAILED(dev->CreateQueryHeap(&qh, IID_PPV_ARGS(&g_heap)))) { g_failed.store(true); return false; }
    for (Slot& s : g_slots) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 16;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                nullptr, IID_PPV_ARGS(&s.readback)))) {
            g_failed.store(true);
            g_heap.Reset();
            return false;
        }
    }
    return true;
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
    Slot& s = g_slots[g_next];
    if (s.armed) return; // ring full (reader starved): skip this frame
    list->EndQuery(g_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, g_next * 2);
    g_begun = true;
}

void End(ID3D12GraphicsCommandList* list)
{
    if (!g_enabled.load() || !list) return;
    std::lock_guard<std::mutex> lock(g_m);
    if (!g_heap || !g_begun) return;
    g_begun = false;
    Slot& s = g_slots[g_next];
    list->EndQuery(g_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, g_next * 2 + 1);
    list->ResolveQueryData(g_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, g_next * 2, 2, s.readback.Get(), 0);
    s.issued_present = g_present_counter;
    s.armed = true;
    g_next = (g_next + 1) % kSlots;
}

void Poll(ID3D12CommandQueue* queue)
{
    if (!g_enabled.load()) return;
    std::lock_guard<std::mutex> lock(g_m);
    ++g_present_counter;
    if (!g_heap) return;
    if (!g_freq && queue) queue->GetTimestampFrequency(&g_freq);
    if (!g_freq) return;
    for (Slot& s : g_slots) {
        if (!s.armed || g_present_counter - s.issued_present < kReadDelay) continue;
        uint64_t stamps[2] = {0, 0};
        void* mapped = nullptr;
        D3D12_RANGE r{0, 16};
        if (SUCCEEDED(s.readback->Map(0, &r, &mapped))) {
            std::memcpy(stamps, mapped, 16);
            D3D12_RANGE none{0, 0};
            s.readback->Unmap(0, &none);
        }
        s.armed = false;
        if (stamps[0] == 0 || stamps[1] < stamps[0]) continue; // not executed / invalid
        float ms = static_cast<float>(static_cast<double>(stamps[1] - stamps[0]) * 1000.0 /
                                      static_cast<double>(g_freq));
        g_last.store(ms);
        float prev = g_avg.load();
        g_avg.store(prev > 0 ? prev * 0.95f + ms * 0.05f : ms);
        g_samples.fetch_add(1);
    }
}

Stats GetStats()
{
    return Stats{g_last.load(), g_avg.load(), g_samples.load(),
                 (g_enabled.load() && !g_failed.load()) ? 1u : 0u};
}

} // namespace na::gputime
