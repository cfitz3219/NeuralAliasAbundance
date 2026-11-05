#include "host_dx12/gpu/mem_alloc.hpp"
#include "host_dx12/observe/producers.hpp"

#include <windows.h>
#include <dxgi1_4.h>
#include <D3D12MemAlloc.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <memory>
#include <vector>
#include <new>

namespace na::gpu::mem {
namespace {
// {6E3B2A1C-5D44-4F3A-9C1E-7A2B8D4F0C11}: private data slot holding the allocation.
constexpr GUID kAllocationSlot = {0x6e3b2a1c, 0x5d44, 0x4f3a, {0x9c, 0x1e, 0x7a, 0x2b, 0x8d, 0x4f, 0x0c, 0x11}};

struct Context {ID3D12Device* device=nullptr;D3D12MA::Allocator* allocator=nullptr;};
struct Registry {std::mutex mutex;std::vector<Context> contexts;};
// Each allocator retains its actual device. Allocation holders can outlive host shutdown;
// the bounded registry remains valid during native/NVRHI deferred destruction as well.
Registry& registry(){static auto* r=new Registry;return *r;}
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
        const na::observe::HostWorkScope own;
        if (a_) a_->Release();
        g_live.fetch_sub(1, std::memory_order_relaxed);
    }
    D3D12MA::Allocation* a_;
    std::atomic<ULONG> refs_{1};
};

D3D12MA::Allocator* allocator_locked(Registry& r,ID3D12Device* device)
{
    for(const auto& c:r.contexts)if(c.device==device)return c.allocator;
    if(r.contexts.size()>=8)return nullptr;
    IDXGIFactory4* f=nullptr;IDXGIAdapter* adapter=nullptr;
    if(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))){
        f->EnumAdapterByLuid(device->GetAdapterLuid(),IID_PPV_ARGS(&adapter));f->Release();
    }
    if(!adapter)return nullptr;
    D3D12MA::ALLOCATOR_DESC d{};d.pDevice=device;d.pAdapter=adapter;
    d.PreferredBlockSize=64ull*1024*1024; // lazy blocks; no forced pre-reservation / NOT_ZEROED policy
    D3D12MA::Allocator* allocator=nullptr;const HRESULT hr=D3D12MA::CreateAllocator(&d,&allocator);adapter->Release();
    if(FAILED(hr)||!allocator)return nullptr;
    r.contexts.push_back({device,allocator});return allocator;
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
        auto& r=registry();std::lock_guard<std::mutex> lock(r.mutex);
        a = allocator_locked(r,device);
    }
    if (a) {
        D3D12MA::ALLOCATION_DESC ad{};
        ad.HeapType = heap;
        if(a->GetD3D12Options().ResourceHeapTier==D3D12_RESOURCE_HEAP_TIER_1){
            ad.ExtraHeapFlags=desc.Dimension==D3D12_RESOURCE_DIMENSION_BUFFER?D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS:
                (desc.Flags&(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))?
                D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES:D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
        }
        const D3D12_RESOURCE_ALLOCATION_INFO info = device->GetResourceAllocationInfo(0, 1, &desc);
        D3D12MA::Allocation* alloc = nullptr;
        if (info.SizeInBytes != UINT64_MAX && SUCCEEDED(a->AllocateMemory(&ad, &info, &alloc)) && alloc) {
            ID3D12Resource* r = nullptr;
            if (SUCCEEDED(a->CreateAliasingResource(alloc, 0, &desc, state, clear, IID_PPV_ARGS(&r))) && r) {
                Holder* h = new(std::nothrow) Holder(alloc);
                if(h){
                    const HRESULT pd=r->SetPrivateDataInterface(kAllocationSlot,h);
                    if(SUCCEEDED(pd)){
                        h->Release(); // native and NVRHI COM owners keep this allocation alive
                        g_placed.fetch_add(1,std::memory_order_relaxed);*out=r;return S_OK;
                    }
                    // Free the placed resource BEFORE its backing allocation on this failure path.
                    r->Release();h->Release();
                }else{r->Release();alloc->Release();}
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
    const na::observe::HostWorkScope own;
    auto& r=registry();std::lock_guard<std::mutex> lock(r.mutex);
    s.devices=static_cast<uint32_t>(r.contexts.size());
    for(const auto& c:r.contexts){
        D3D12MA::Budget local{},nonlocal{};c.allocator->GetBudget(&local,&nonlocal);
        s.blocks+=local.Stats.BlockCount+nonlocal.Stats.BlockCount;
        s.block_bytes+=local.Stats.BlockBytes+nonlocal.Stats.BlockBytes;
        s.allocation_bytes+=local.Stats.AllocationBytes+nonlocal.Stats.AllocationBytes;
        // The adapter-wide usage is not counted once per device on the same GPU.
        s.local_usage=local.UsageBytes;s.local_budget=local.BudgetBytes;
    }
    s.available=!r.contexts.empty();
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
