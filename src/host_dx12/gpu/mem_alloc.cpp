#include "host_dx12/gpu/mem_alloc.hpp"
#include "host_dx12/observe/producers.hpp"

#include <windows.h>
#include <dxgi1_4.h>
#include <D3D12MemAlloc.h>

#include <atomic>
#include <cstdio>
#include <mutex>

namespace na::gpu::mem {
namespace {
// {6E3B2A1C-5D44-4F3A-9C1E-7A2B8D4F0C11}: private data slot holding the allocation.
constexpr GUID kAllocationSlot = {0x6e3b2a1c, 0x5d44, 0x4f3a, {0x9c, 0x1e, 0x7a, 0x2b, 0x8d, 0x4f, 0x0c, 0x11}};

std::mutex g_m;
ID3D12Device* g_device = nullptr;          // the device the allocator serves (one: the game's)
D3D12MA::Allocator* g_allocator = nullptr; // never destroyed: allocations may outlive anything
bool g_failed = false;
std::atomic<uint64_t> g_placed{0}, g_fallbacks{0}, g_live{0};

// Owns one allocation; released with the resource's private data.
class Holder final : public IUnknown {
public:
    explicit Holder(D3D12MA::Allocation* a) : a_(a) { g_live.fetch_add(1, std::memory_order_relaxed); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown)) { *out = static_cast<IUnknown*>(this); AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG n = --refs_;
        if (!n) delete this;
        return n;
    }
private:
    ~Holder()
    {
        if (a_) a_->Release();
        g_live.fetch_sub(1, std::memory_order_relaxed);
    }
    D3D12MA::Allocation* a_;
    std::atomic<ULONG> refs_{1};
};

D3D12MA::Allocator* allocator_locked(ID3D12Device* device)
{
    if (g_allocator) return g_device == device ? g_allocator : nullptr;
    if (g_failed) return nullptr;
    IDXGIFactory4* f = nullptr;
    IDXGIAdapter* adapter = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) {
        f->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter));
        f->Release();
    }
    if (!adapter) { g_failed = true; return nullptr; }
    D3D12MA::ALLOCATOR_DESC d{};
    d.pDevice = device;
    d.pAdapter = adapter;
    const HRESULT hr = D3D12MA::CreateAllocator(&d, &g_allocator);
    adapter->Release();
    if (FAILED(hr) || !g_allocator) { g_allocator = nullptr; g_failed = true; return nullptr; }
    g_device = device;
    return g_allocator;
}
} // namespace

HRESULT CreateResource(ID3D12Device* device, D3D12_HEAP_TYPE heap, const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES state,
                       const D3D12_CLEAR_VALUE* clear, ID3D12Resource** out)
{
    if (!device || !out) return E_INVALIDARG;
    *out = nullptr;
    const na::observe::HostWorkScope own; // our memory, never observed as the game's
    D3D12MA::Allocator* a = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_m);
        a = allocator_locked(device);
    }
    if (a && a->GetD3D12Options().ResourceHeapTier >= D3D12_RESOURCE_HEAP_TIER_2) {
        D3D12MA::ALLOCATION_DESC ad{};
        ad.HeapType = heap;
        const D3D12_RESOURCE_ALLOCATION_INFO info = device->GetResourceAllocationInfo(0, 1, &desc);
        D3D12MA::Allocation* alloc = nullptr;
        if (info.SizeInBytes != UINT64_MAX && SUCCEEDED(a->AllocateMemory(&ad, &info, &alloc)) && alloc) {
            ID3D12Resource* r = nullptr;
            if (SUCCEEDED(a->CreateAliasingResource(alloc, 0, &desc, state, clear, IID_PPV_ARGS(&r))) && r) {
                Holder* h = new Holder(alloc);
                const HRESULT pd = r->SetPrivateDataInterface(kAllocationSlot, h);
                h->Release(); // the resource holds it now (or, on failure, this frees the allocation)
                if (SUCCEEDED(pd)) {
                    g_placed.fetch_add(1, std::memory_order_relaxed);
                    *out = r;
                    return S_OK;
                }
                r->Release();
            } else {
                alloc->Release();
            }
        }
    }
    g_fallbacks.fetch_add(1, std::memory_order_relaxed);
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap;
    return device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, state, clear, IID_PPV_ARGS(out));
}

Stats GetStats()
{
    Stats s;
    s.placed = g_placed.load(std::memory_order_relaxed);
    s.committed_fallbacks = g_fallbacks.load(std::memory_order_relaxed);
    s.live = g_live.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_m);
    if (!g_allocator) return s;
    s.available = true;
    D3D12MA::Budget local{};
    g_allocator->GetBudget(&local, nullptr);
    s.local_usage = local.UsageBytes;
    s.local_budget = local.BudgetBytes;
    s.blocks = local.Stats.BlockCount;
    s.block_bytes = local.Stats.BlockBytes;
    s.allocation_bytes = local.Stats.AllocationBytes;
    return s;
}

std::string StatusLine()
{
    const Stats s = GetStats();
    char b[256];
    std::snprintf(b, sizeof b,
                  "gpu_mem d3d12ma=%d placed=%llu live=%llu committed_fallbacks=%llu blocks=%llu block_mb=%.1f alloc_mb=%.1f vram_used_mb=%.0f vram_budget_mb=%.0f",
                  s.available ? 1 : 0, (unsigned long long)s.placed, (unsigned long long)s.live, (unsigned long long)s.committed_fallbacks,
                  (unsigned long long)s.blocks, s.block_bytes / 1048576.0, s.allocation_bytes / 1048576.0, s.local_usage / 1048576.0,
                  s.local_budget / 1048576.0);
    return b;
}
} // namespace na::gpu::mem
