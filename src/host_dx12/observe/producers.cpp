// C8a: H1 observation producers. See producers.hpp.
#include "host_dx12/observe/producers.hpp"

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <initializer_list>
#include <type_traits>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "host_dx12/hooking/variant_registry.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
#include "host_dx12/host_observe_internal.hpp"
#include "host_dx12/observe/d3d12_slots.hpp"

namespace na::observe {
namespace {

namespace sl = na::observe::slots;

// {6E2B39A4-1C0D-4C55-9A3F-2F6B8E1D7A10}: our retirement sentinel's private-data key.
constexpr GUID kSentinelGuid = {0x6e2b39a4, 0x1c0d, 0x4c55, {0x9a, 0x3f, 0x2f, 0x6b, 0x8e, 0x1d, 0x7a, 0x10}};

// Pipelines and root signatures have no na_object_kind in ABI 0.1; their
// retirement reports this value (documented in the C8 proposal).
constexpr uint32_t NA_OBJ_PIPELINE_HOST = 10;

// ---------------------------------------------------------------- registry
struct QueueState {
    std::atomic<uint64_t> ticket{0};
    std::atomic<uint32_t> inflight{0};
    std::atomic<uint64_t> overlap_gen{0};
};

struct ListState {
    std::atomic<uint64_t> generation{0};
    std::atomic<bool> generation_known{false};
    std::atomic<uint64_t> op_index{0};
    std::atomic<bool> recording_observed{false}; // this recording began under observation
    std::atomic<bool> coverage_complete{false};  // the list's current table is fully hooked
};

struct Obj {
    na_object_id id = 0;
    uint32_t kind = 0;
    std::vector<uint8_t> created;          // creation event, replayed by the snapshot
    std::unique_ptr<QueueState> queue;     // queues
    std::unique_ptr<ListState> list;       // command lists
};

struct DescHeap {
    const void* key = nullptr;
    na_object_id id = 0;
    uint64_t cpu_start = 0, gpu_start = 0;
    uint32_t increment = 0, count = 0, type = 0;
    std::vector<uint64_t> versions;
    struct SlotInfo {
        uint32_t kind = NA_UNKNOWN_U32;
        na_object_id resource = NA_UNKNOWN_U64;
        uint32_t format = NA_UNKNOWN_U32;
        uint32_t first_sub = NA_UNKNOWN_U32;
    };
    std::vector<SlotInfo> info; // last observed write per slot (record-time CPU view)
};

struct VaRange {
    uint64_t begin = 0, end = 0; // [begin, end)
    na_object_id resource = 0;
    const void* key = nullptr;
};

std::shared_mutex g_m; // registry, heaps, VA table. Never held across a D3D12 call.
std::unordered_map<const void*, Obj> g_objs;
std::vector<std::unique_ptr<DescHeap>> g_heaps;
std::vector<VaRange> g_va; // sorted by begin
std::atomic<uint64_t> g_next_id{1};

struct Counters {
    std::atomic<uint64_t> live{0}, created{0}, retired{0}, writes{0}, copies{0}, unresolved{0}, sentinel_fail{0},
        executes{0}, ambiguous{0};
    std::atomic<uint32_t> hooks{0}, missing{0};
} g_c;

thread_local uint64_t t_seq = 0;

template <typename E>
void init_header(E& e, uint32_t kind, uint32_t flags)
{
    std::memset(&e, 0, sizeof(e));
    e.hdr.size_bytes = static_cast<uint32_t>(sizeof(E));
    e.hdr.kind = kind;
    e.hdr.thread_seq = ++t_seq;
    e.hdr.producer_thread = GetCurrentThreadId();
    e.hdr.flags = flags;
}

template <typename E>
void emit(const E& e, uint32_t group)
{
    static_assert(sizeof(E) % 8 == 0, "events are 8-byte multiples");
    na::observe_internal::Emit(&e, group);
}

// Sentinel attached as private data: its last Release is the object's destruction.
class RetireSentinel final : public IUnknown {
public:
    RetireSentinel(const void* key, na_object_id id, uint32_t kind) : key_(key), id_(id), kind_(kind) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown)) {
            *out = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG r = --ref_;
        if (r == 0) {
            retire(key_, id_, kind_);
            delete this;
        }
        return r;
    }
    static void retire(const void* key, na_object_id id, uint32_t kind);

private:
    std::atomic<ULONG> ref_{1};
    const void* key_;
    na_object_id id_;
    uint32_t kind_;
};

void RetireSentinel::retire(const void* key, na_object_id id, uint32_t kind)
{
    {
        std::unique_lock<std::shared_mutex> lock(g_m);
        auto it = g_objs.find(key);
        if (it != g_objs.end() && it->second.id == id) g_objs.erase(it);
        g_heaps.erase(std::remove_if(g_heaps.begin(), g_heaps.end(), [&](const std::unique_ptr<DescHeap>& h) { return h->id == id; }),
                      g_heaps.end());
        g_va.erase(std::remove_if(g_va.begin(), g_va.end(), [&](const VaRange& r) { return r.resource == id; }), g_va.end());
    }
    g_c.live.fetch_sub(1);
    g_c.retired.fetch_add(1);
    na_ev_object_retired e;
    init_header(e, NA_EV_OBJECT_RETIRED, NA_EVF_FOUNDATIONAL);
    e.object = id;
    e.object_kind = kind;
    emit(e, kind == NA_OBJ_RESOURCE ? NA_GROUP_RESOURCE : NA_GROUP_SESSION);
}

// Registers a newly created object (or re-registers a reused address) and
// attaches the retirement sentinel. Returns its id. The object is used only
// for the sentinel call, before any lock is taken.
na_object_id register_object(IUnknown* object, uint32_t kind)
{
    if (!object) return 0;
    const na_object_id id = g_next_id.fetch_add(1);
    {
        std::unique_lock<std::shared_mutex> lock(g_m);
        Obj& o = g_objs[object];
        if (o.id) g_c.live.fetch_sub(1); // address reused without a retirement we saw
        o = Obj{};
        o.id = id;
        o.kind = kind;
        if (kind == NA_OBJ_QUEUE) o.queue = std::make_unique<QueueState>();
        if (kind == NA_OBJ_LIST) o.list = std::make_unique<ListState>();
    }
    g_c.live.fetch_add(1);
    g_c.created.fetch_add(1);
    ID3D12Object* d3d = nullptr;
    bool attached = false;
    if (SUCCEEDED(object->QueryInterface(__uuidof(ID3D12Object), reinterpret_cast<void**>(&d3d))) && d3d) {
        auto* s = new RetireSentinel(object, id, kind);
        attached = SUCCEEDED(d3d->SetPrivateDataInterface(kSentinelGuid, s));
        s->Release(); // the object now holds the only reference (or it is gone)
        d3d->Release();
    }
    if (!attached) g_c.sentinel_fail.fetch_add(1);
    return id;
}

template <typename E>
void remember_creation(const void* key, const E& e)
{
    std::unique_lock<std::shared_mutex> lock(g_m);
    auto it = g_objs.find(key);
    if (it == g_objs.end()) return;
    const auto* b = reinterpret_cast<const uint8_t*>(&e);
    it->second.created.assign(b, b + sizeof(E));
}

na_object_id id_locked(const void* key)
{
    auto it = g_objs.find(key);
    return it == g_objs.end() ? 0 : it->second.id;
}

na_object_id lookup(const void* key)
{
    if (!key) return 0;
    std::shared_lock<std::shared_mutex> lock(g_m);
    return id_locked(key);
}

// Device: seen first through a hook if it predates installation.
na_object_id device_id(ID3D12Device* dev)
{
    if (const na_object_id id = lookup(dev)) return id;
    const na_object_id id = register_object(dev, NA_OBJ_DEVICE);
    na_ev_device_created e;
    init_header(e, NA_EV_DEVICE_CREATED, NA_EVF_FOUNDATIONAL | NA_EVF_PREEXISTING);
    e.device = id;
    const LUID luid = dev->GetAdapterLuid();
    e.adapter_luid = (static_cast<uint64_t>(static_cast<uint32_t>(luid.HighPart)) << 32) | luid.LowPart;
    e.hooked_interface_version = 8;
    remember_creation(dev, e);
    emit(e, NA_GROUP_SESSION);
    return id;
}

IUnknown* out_object(HRESULT hr, void** out)
{
    return (SUCCEEDED(hr) && out && *out) ? static_cast<IUnknown*>(*out) : nullptr;
}

// ---------------------------------------------------------- resources / VA
void note_resource(ID3D12Device* dev, IUnknown* obj, uint32_t creation_kind, ID3D12Heap* heap, uint64_t heap_offset,
                   const D3D12_HEAP_PROPERTIES* props, D3D12_RESOURCE_STATES initial)
{
    if (!obj) return;
    ID3D12Resource* res = nullptr;
    if (FAILED(obj->QueryInterface(__uuidof(ID3D12Resource), reinterpret_cast<void**>(&res))) || !res) return;
    const D3D12_RESOURCE_DESC d = res->GetDesc();
    const uint64_t va = d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? res->GetGPUVirtualAddress() : 0;
    uint32_t heap_type = NA_UNKNOWN_U32;
    if (props) heap_type = props->Type;
    else if (heap) heap_type = heap->GetDesc().Properties.Type;
    res->Release();

    const na_object_id dev_id = device_id(dev);
    const na_object_id id = register_object(obj, NA_OBJ_RESOURCE);
    na_ev_resource_created e;
    init_header(e, NA_EV_RESOURCE_CREATED, NA_EVF_FOUNDATIONAL);
    e.resource = id;
    e.device = dev_id;
    e.heap = heap ? lookup(heap) : 0;
    e.heap_offset = heap ? heap_offset : NA_UNKNOWN_U64;
    e.dimension = d.Dimension;
    e.format = d.Format;
    e.width = d.Width;
    e.height = d.Height;
    e.depth_or_array = d.DepthOrArraySize;
    e.mip_levels = d.MipLevels;
    e.sample_count = d.SampleDesc.Count;
    e.creation_kind = creation_kind;
    e.resource_flags = d.Flags;
    e.initial_state = initial;
    e.heap_type = heap_type;
    remember_creation(obj, e);
    if (va) {
        std::unique_lock<std::shared_mutex> lock(g_m);
        VaRange r{va, va + d.Width, id, obj};
        g_va.insert(std::upper_bound(g_va.begin(), g_va.end(), r, [](const VaRange& a, const VaRange& b) { return a.begin < b.begin; }), r);
    }
    emit(e, NA_GROUP_RESOURCE);
}

bool resolve_va_locked(uint64_t va, na_object_id* resource, uint64_t* offset)
{
    if (!va) return false;
    // Every range containing va: exactly one -> resolved, more -> ambiguous (aliasing).
    na_object_id found = 0;
    uint64_t base = 0;
    int hits = 0;
    auto it = std::upper_bound(g_va.begin(), g_va.end(), va, [](uint64_t v, const VaRange& r) { return v < r.begin; });
    while (it != g_va.begin()) {
        --it;
        if (va < it->end) {
            if (++hits > 1) return false;
            found = it->resource;
            base = it->begin;
        }
        if (it->end + (1ull << 36) < va) break; // ranges far below cannot contain it
    }
    if (hits != 1) return false;
    *resource = found;
    *offset = va - base;
    return true;
}

// ------------------------------------------------------------ descriptors
DescHeap* heap_for_cpu_locked(uint64_t ptr, uint32_t* slot)
{
    for (auto& h : g_heaps) {
        if (!h->increment || ptr < h->cpu_start) continue;
        const uint64_t off = ptr - h->cpu_start;
        if (off % h->increment) continue;
        const uint64_t s = off / h->increment;
        if (s < h->count) {
            *slot = static_cast<uint32_t>(s);
            return h.get();
        }
    }
    return nullptr;
}

void note_descriptor_write(D3D12_CPU_DESCRIPTOR_HANDLE dst, uint32_t kind, IUnknown* resource, uint32_t format,
                           uint32_t first_sub, uint32_t sub_count)
{
    na_ev_descriptor_write e;
    init_header(e, NA_EV_DESCRIPTOR_WRITE, 0);
    {
        std::unique_lock<std::shared_mutex> lock(g_m);
        uint32_t slot = 0;
        DescHeap* h = heap_for_cpu_locked(dst.ptr, &slot);
        if (!h) {
            g_c.unresolved.fetch_add(1);
            return;
        }
        e.heap = h->id;
        e.slot = slot;
        e.slot_version = ++h->versions[slot];
        e.resource = resource ? id_locked(resource) : 0;
        if (resource && !e.resource) {
            e.resource = NA_UNKNOWN_U64; // resource-backed, but created before observation
            e.hdr.flags |= NA_EVF_METHOD_UNOBSERVED;
        }
        h->info[slot] = DescHeap::SlotInfo{kind, e.resource, format, first_sub};
    }
    e.descriptor_kind = kind;
    e.format = format;
    e.first_subresource = first_sub;
    e.subresource_count = sub_count;
    g_c.writes.fetch_add(1);
    emit(e, NA_GROUP_DESCRIPTOR);
}

// Copies of `count` descriptors from src to dst, split wherever either side
// crosses a heap boundary (each event names one source and one destination heap).
void note_descriptor_copy(uint64_t dst, uint64_t src, uint32_t count, uint32_t increment)
{
    while (count) {
        na_ev_descriptor_copy e;
        init_header(e, NA_EV_DESCRIPTOR_COPY, 0);
        uint32_t run = 0;
        {
            std::unique_lock<std::shared_mutex> lock(g_m);
            uint32_t ds = 0, ss = 0;
            DescHeap* dh = heap_for_cpu_locked(dst, &ds);
            DescHeap* sh = heap_for_cpu_locked(src, &ss);
            if (!dh || !sh) {
                g_c.unresolved.fetch_add(1);
                return;
            }
            run = std::min({count, dh->count - ds, sh->count - ss});
            for (uint32_t i = 0; i < run; ++i) {
                ++dh->versions[ds + i];
                dh->info[ds + i] = sh->info[ss + i]; // contents follow the source's record-time view
            }
            e.src_heap = sh->id;
            e.dst_heap = dh->id;
            e.src_slot = ss;
            e.dst_slot = ds;
            e.slot_count = run;
        }
        g_c.copies.fetch_add(1);
        emit(e, NA_GROUP_DESCRIPTOR);
        count -= run;
        dst += static_cast<uint64_t>(run) * increment;
        src += static_cast<uint64_t>(run) * increment;
    }
}

std::atomic<uint32_t> g_increment[D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES] = {};
uint32_t descriptor_increment_cached(D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    return type < D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES ? g_increment[type].load(std::memory_order_relaxed) : 0;
}
uint32_t descriptor_increment(ID3D12Device* dev, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    auto& cache = g_increment;
    if (type >= D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES) return 0;
    uint32_t v = cache[type].load(std::memory_order_relaxed);
    if (!v) {
        v = dev->GetDescriptorHandleIncrementSize(type);
        cache[type].store(v, std::memory_order_relaxed);
    }
    return v;
}

// ------------------------------------------------------------ device hooks
#define NA_ORIG(name, ret, ...) using name##_fn = ret(STDMETHODCALLTYPE*)(__VA_ARGS__); name##_fn o_##name = nullptr

NA_ORIG(CreateCommandQueue, HRESULT, ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, void**);
NA_ORIG(CreateCommandList, HRESULT, ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*, ID3D12PipelineState*, REFIID, void**);
NA_ORIG(CreateCommandList1, HRESULT, ID3D12Device4*, UINT, D3D12_COMMAND_LIST_TYPE, D3D12_COMMAND_LIST_FLAGS, REFIID, void**);
NA_ORIG(CreateDescriptorHeap, HRESULT, ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**);
NA_ORIG(CreateConstantBufferView, void, ID3D12Device*, const D3D12_CONSTANT_BUFFER_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
NA_ORIG(CreateShaderResourceView, void, ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
NA_ORIG(CreateUnorderedAccessView, void, ID3D12Device*, ID3D12Resource*, ID3D12Resource*, const D3D12_UNORDERED_ACCESS_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
NA_ORIG(CreateRenderTargetView, void, ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
NA_ORIG(CreateDepthStencilView, void, ID3D12Device*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
NA_ORIG(CreateSampler, void, ID3D12Device*, const D3D12_SAMPLER_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
NA_ORIG(CopyDescriptors, void, ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE);
NA_ORIG(CopyDescriptorsSimple, void, ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
NA_ORIG(CreateCommittedResource, HRESULT, ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
NA_ORIG(CreateCommittedResource1, HRESULT, ID3D12Device4*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**);
NA_ORIG(CreateCommittedResource2, HRESULT, ID3D12Device8*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC1*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**);
NA_ORIG(CreateHeap, HRESULT, ID3D12Device*, const D3D12_HEAP_DESC*, REFIID, void**);
NA_ORIG(CreateHeap1, HRESULT, ID3D12Device4*, const D3D12_HEAP_DESC*, ID3D12ProtectedResourceSession*, REFIID, void**);
NA_ORIG(CreatePlacedResource, HRESULT, ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
NA_ORIG(CreatePlacedResource1, HRESULT, ID3D12Device8*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC1*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
NA_ORIG(CreateReservedResource, HRESULT, ID3D12Device*, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
NA_ORIG(CreateReservedResource1, HRESULT, ID3D12Device4*, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**);
NA_ORIG(CreateFence, HRESULT, ID3D12Device*, UINT64, D3D12_FENCE_FLAGS, REFIID, void**);
NA_ORIG(ExecuteCommandLists, void, ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
NA_ORIG(Signal, HRESULT, ID3D12CommandQueue*, ID3D12Fence*, UINT64);
NA_ORIG(Wait, HRESULT, ID3D12CommandQueue*, ID3D12Fence*, UINT64);

// Recursion guard: a runtime implementing one Create* through another hooked
// one must not produce two creations for one object.
thread_local int t_depth = 0;
bool trace_on()
{
    static const bool on = GetEnvironmentVariableW(L"NEURALALIAS_OBSERVE_TRACE", nullptr, 0) != 0;
    return on;
}
struct Depth {
    const char* name;
    explicit Depth(const char* n) : name(n)
    {
        ++t_depth;
        if (trace_on()) { std::printf("checkpoint trace enter %s depth %d\n", name, t_depth); std::fflush(stdout); }
    }
    ~Depth()
    {
        if (trace_on()) { std::printf("checkpoint trace exit %s\n", name); std::fflush(stdout); }
        --t_depth;
    }
    bool outermost() const { return t_depth == 1; }
};

HRESULT STDMETHODCALLTYPE D_CreateCommandQueue(ID3D12Device* dev, const D3D12_COMMAND_QUEUE_DESC* desc, REFIID riid, void** out)
{
    Depth depth("CreateCommandQueue");
    const HRESULT hr = o_CreateCommandQueue(dev, desc, riid, out);
    if (IUnknown* obj = depth.outermost() ? out_object(hr, out) : nullptr) {
        const na_object_id dev_id = device_id(dev);
        na_ev_queue_created e;
        init_header(e, NA_EV_QUEUE_CREATED, NA_EVF_FOUNDATIONAL);
        e.queue = register_object(obj, NA_OBJ_QUEUE);
        e.device = dev_id;
        e.queue_type = desc ? static_cast<uint32_t>(desc->Type) : NA_UNKNOWN_U32;
        e.queue_flags = desc ? static_cast<uint32_t>(desc->Flags) : NA_UNKNOWN_U32;
        remember_creation(obj, e);
        emit(e, NA_GROUP_QUEUE);
    }
    return hr;
}

void list_created(ID3D12Device* dev, IUnknown* obj, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* alloc, bool open)
{
    const na_object_id dev_id = device_id(dev);
    na_ev_list_created e;
    init_header(e, NA_EV_LIST_CREATED, NA_EVF_FOUNDATIONAL);
    e.list = register_object(obj, NA_OBJ_LIST);
    e.device = dev_id;
    // Allocators have no creation event in ABI 0.1; they get an id when first used.
    na_object_id alloc_id = alloc ? lookup(alloc) : 0;
    if (alloc && !alloc_id) alloc_id = register_object(alloc, NA_OBJ_ALLOCATOR);
    e.initial_allocator = alloc_id;
    e.list_type = type;
    e.is_bundle = type == D3D12_COMMAND_LIST_TYPE_BUNDLE ? 1u : 0u;
    e.initially_open = open ? 1u : 0u;
    {
        std::unique_lock<std::shared_mutex> lock(g_m);
        auto it = g_objs.find(obj);
        if (it != g_objs.end() && it->second.list) {
            it->second.list->generation.store(open ? 1 : 0);
            it->second.list->generation_known.store(true);
            it->second.list->recording_observed.store(open);
        }
    }
    remember_creation(obj, e);
    emit(e, NA_GROUP_LIST);
}

HRESULT STDMETHODCALLTYPE D_CreateCommandList(ID3D12Device* dev, UINT mask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* alloc,
                                              ID3D12PipelineState* pso, REFIID riid, void** out)
{
    Depth depth("CreateCommandList");
    const HRESULT hr = o_CreateCommandList(dev, mask, type, alloc, pso, riid, out);
    if (IUnknown* obj = depth.outermost() ? out_object(hr, out) : nullptr) list_created(dev, obj, type, alloc, true);
    return hr;
}

HRESULT STDMETHODCALLTYPE D_CreateCommandList1(ID3D12Device4* dev, UINT mask, D3D12_COMMAND_LIST_TYPE type, D3D12_COMMAND_LIST_FLAGS flags,
                                               REFIID riid, void** out)
{
    Depth depth("CreateCommandList1");
    const HRESULT hr = o_CreateCommandList1(dev, mask, type, flags, riid, out);
    if (IUnknown* obj = depth.outermost() ? out_object(hr, out) : nullptr) list_created(dev, obj, type, nullptr, false);
    return hr;
}

HRESULT STDMETHODCALLTYPE D_CreateDescriptorHeap(ID3D12Device* dev, const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID riid, void** out)
{
    Depth depth("CreateDescriptorHeap");
    const HRESULT hr = o_CreateDescriptorHeap(dev, desc, riid, out);
    IUnknown* obj = depth.outermost() ? out_object(hr, out) : nullptr;
    if (!obj || !desc) return hr;
    ID3D12DescriptorHeap* h = nullptr;
    if (FAILED(obj->QueryInterface(__uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&h))) || !h) return hr;
    auto dh = std::make_unique<DescHeap>();
    dh->key = obj;
    dh->cpu_start = h->GetCPUDescriptorHandleForHeapStart().ptr;
    const bool visible = (desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0;
    dh->gpu_start = visible ? h->GetGPUDescriptorHandleForHeapStart().ptr : 0;
    h->Release();
    dh->increment = descriptor_increment(dev, desc->Type);
    dh->count = desc->NumDescriptors;
    dh->type = desc->Type;
    dh->versions.assign(desc->NumDescriptors, 0);
    dh->info.assign(desc->NumDescriptors, DescHeap::SlotInfo{});
    const na_object_id dev_id = device_id(dev);
    na_ev_descriptor_heap_created e;
    init_header(e, NA_EV_DESCRIPTOR_HEAP_CREATED, NA_EVF_FOUNDATIONAL);
    e.heap = register_object(obj, NA_OBJ_DESCRIPTOR_HEAP);
    e.device = dev_id;
    e.heap_kind = desc->Type;
    e.descriptor_count = desc->NumDescriptors;
    e.shader_visible = visible ? 1u : 0u;
    dh->id = e.heap;
    {
        std::unique_lock<std::shared_mutex> lock(g_m);
        // A heap at the same CPU address replaces a destroyed one we never saw retire.
        g_heaps.erase(std::remove_if(g_heaps.begin(), g_heaps.end(), [&](const std::unique_ptr<DescHeap>& x) { return x->key == obj; }),
                      g_heaps.end());
        g_heaps.push_back(std::move(dh));
    }
    remember_creation(obj, e);
    emit(e, NA_GROUP_DESCRIPTOR);
    return hr;
}

void STDMETHODCALLTYPE D_CreateConstantBufferView(ID3D12Device* dev, const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    o_CreateConstantBufferView(dev, desc, h);
    na_object_id res = 0;
    uint64_t off = 0;
    bool known = false;
    if (desc && desc->BufferLocation) {
        std::shared_lock<std::shared_mutex> lock(g_m);
        known = resolve_va_locked(desc->BufferLocation, &res, &off);
    }
    // ABI 0.1 DESCRIPTOR_WRITE carries no byte offset; the resource identity is
    // what H1 promises. Unresolved / ambiguous ranges are UNKNOWN, never guessed.
    na_ev_descriptor_write e;
    init_header(e, NA_EV_DESCRIPTOR_WRITE, known ? 0u : NA_EVF_METHOD_UNOBSERVED);
    {
        std::unique_lock<std::shared_mutex> lock(g_m);
        uint32_t slot = 0;
        DescHeap* dh = heap_for_cpu_locked(h.ptr, &slot);
        if (!dh) {
            g_c.unresolved.fetch_add(1);
            return;
        }
        e.heap = dh->id;
        e.slot = slot;
        e.slot_version = ++dh->versions[slot];
        dh->info[slot] = DescHeap::SlotInfo{NA_DESC_CBV, desc && desc->BufferLocation ? (known ? res : NA_UNKNOWN_U64) : 0, NA_UNKNOWN_U32, 0};
    }
    e.descriptor_kind = NA_DESC_CBV;
    e.resource = desc && desc->BufferLocation ? (known ? res : NA_UNKNOWN_U64) : 0;
    e.format = NA_UNKNOWN_U32;
    e.first_subresource = 0;
    e.subresource_count = 1;
    g_c.writes.fetch_add(1);
    emit(e, NA_GROUP_DESCRIPTOR);
}

void STDMETHODCALLTYPE D_CreateShaderResourceView(ID3D12Device* dev, ID3D12Resource* r, const D3D12_SHADER_RESOURCE_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    o_CreateShaderResourceView(dev, r, d, h);
    uint32_t first = NA_UNKNOWN_U32, count = NA_UNKNOWN_U32;
    if (d && d->ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D) { first = d->Texture2D.MostDetailedMip; count = d->Texture2D.MipLevels; }
    note_descriptor_write(h, NA_DESC_SRV, r, d ? static_cast<uint32_t>(d->Format) : NA_UNKNOWN_U32, first, count);
}

void STDMETHODCALLTYPE D_CreateUnorderedAccessView(ID3D12Device* dev, ID3D12Resource* r, ID3D12Resource* counter,
                                                    const D3D12_UNORDERED_ACCESS_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    o_CreateUnorderedAccessView(dev, r, counter, d, h);
    uint32_t first = NA_UNKNOWN_U32, count = NA_UNKNOWN_U32;
    if (d && d->ViewDimension == D3D12_UAV_DIMENSION_TEXTURE2D) { first = d->Texture2D.MipSlice; count = 1; }
    note_descriptor_write(h, NA_DESC_UAV, r, d ? static_cast<uint32_t>(d->Format) : NA_UNKNOWN_U32, first, count);
}

void STDMETHODCALLTYPE D_CreateRenderTargetView(ID3D12Device* dev, ID3D12Resource* r, const D3D12_RENDER_TARGET_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    o_CreateRenderTargetView(dev, r, d, h);
    uint32_t first = NA_UNKNOWN_U32, count = NA_UNKNOWN_U32;
    if (!d || d->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2D) { first = d ? d->Texture2D.MipSlice : 0; count = 1; }
    note_descriptor_write(h, NA_DESC_RTV, r, d ? static_cast<uint32_t>(d->Format) : NA_UNKNOWN_U32, first, count);
}

void STDMETHODCALLTYPE D_CreateDepthStencilView(ID3D12Device* dev, ID3D12Resource* r, const D3D12_DEPTH_STENCIL_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    o_CreateDepthStencilView(dev, r, d, h);
    uint32_t first = NA_UNKNOWN_U32, count = NA_UNKNOWN_U32;
    if (!d || d->ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2D) { first = d ? d->Texture2D.MipSlice : 0; count = 1; }
    note_descriptor_write(h, NA_DESC_DSV, r, d ? static_cast<uint32_t>(d->Format) : NA_UNKNOWN_U32, first, count);
}

void STDMETHODCALLTYPE D_CreateSampler(ID3D12Device* dev, const D3D12_SAMPLER_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    o_CreateSampler(dev, d, h);
    note_descriptor_write(h, NA_DESC_SAMPLER, nullptr, NA_UNKNOWN_U32, NA_UNKNOWN_U32, NA_UNKNOWN_U32);
}

void STDMETHODCALLTYPE D_CopyDescriptors(ID3D12Device* dev, UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE* ds, const UINT* dsz, UINT ns,
                                         const D3D12_CPU_DESCRIPTOR_HANDLE* ss, const UINT* ssz, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    o_CopyDescriptors(dev, nd, ds, dsz, ns, ss, ssz, type);
    if (!ds || !ss) return;
    const uint32_t inc = descriptor_increment(dev, type);
    // Walk both range lists in step; null size arrays mean ranges of 1.
    uint32_t di = 0, si = 0, doff = 0, soff = 0;
    while (di < nd && si < ns) {
        const uint32_t dlen = dsz ? dsz[di] : 1, slen = ssz ? ssz[si] : 1;
        if (doff >= dlen) { ++di; doff = 0; continue; }
        if (soff >= slen) { ++si; soff = 0; continue; }
        const uint32_t run = std::min(dlen - doff, slen - soff);
        note_descriptor_copy(ds[di].ptr + static_cast<uint64_t>(doff) * inc, ss[si].ptr + static_cast<uint64_t>(soff) * inc, run, inc);
        doff += run;
        soff += run;
    }
}

void STDMETHODCALLTYPE D_CopyDescriptorsSimple(ID3D12Device* dev, UINT n, D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src,
                                               D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    o_CopyDescriptorsSimple(dev, n, dst, src, type);
    note_descriptor_copy(dst.ptr, src.ptr, n, descriptor_increment(dev, type));
}

HRESULT STDMETHODCALLTYPE D_CreateCommittedResource(ID3D12Device* dev, const D3D12_HEAP_PROPERTIES* p, D3D12_HEAP_FLAGS f, const D3D12_RESOURCE_DESC* d,
                                                    D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c, REFIID riid, void** out)
{
    Depth depth("CreateCommittedResource");
    const HRESULT hr = o_CreateCommittedResource(dev, p, f, d, s, c, riid, out);
    if (IUnknown* obj = depth.outermost() ? out_object(hr, out) : nullptr) note_resource(dev, obj, NA_CREATE_COMMITTED, nullptr, 0, p, s);
    return hr;
}
HRESULT STDMETHODCALLTYPE D_CreateCommittedResource1(ID3D12Device4* dev, const D3D12_HEAP_PROPERTIES* p, D3D12_HEAP_FLAGS f, const D3D12_RESOURCE_DESC* d,
                                                     D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c, ID3D12ProtectedResourceSession* ps, REFIID riid, void** out)
{
    Depth depth("CreateCommittedResource1");
    const HRESULT hr = o_CreateCommittedResource1(dev, p, f, d, s, c, ps, riid, out);
    if (IUnknown* obj = depth.outermost() ? out_object(hr, out) : nullptr) note_resource(dev, obj, NA_CREATE_COMMITTED, nullptr, 0, p, s);
    return hr;
}
HRESULT STDMETHODCALLTYPE D_CreateCommittedResource2(ID3D12Device8* dev, const D3D12_HEAP_PROPERTIES* p, D3D12_HEAP_FLAGS f, const D3D12_RESOURCE_DESC1* d,
                                                     D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c, ID3D12ProtectedResourceSession* ps, REFIID riid, void** out)
{
    Depth depth("CreateCommittedResource2");
    const HRESULT hr = o_CreateCommittedResource2(dev, p, f, d, s, c, ps, riid, out);
    if (IUnknown* obj = depth.outermost() ? out_object(hr, out) : nullptr) note_resource(dev, obj, NA_CREATE_COMMITTED, nullptr, 0, p, s);
    return hr;
}

void heap_created(ID3D12Device* dev, IUnknown* obj, const D3D12_HEAP_DESC* desc)
{
    const na_object_id dev_id = device_id(dev);
    na_ev_resource_heap_created e;
    init_header(e, NA_EV_RESOURCE_HEAP_CREATED, NA_EVF_FOUNDATIONAL);
    e.heap = register_object(obj, NA_OBJ_RESOURCE_HEAP);
    e.device = dev_id;
    e.size_in_bytes = desc ? desc->SizeInBytes : NA_UNKNOWN_U64;
    e.heap_type = desc ? static_cast<uint32_t>(desc->Properties.Type) : NA_UNKNOWN_U32;
    e.heap_flags = desc ? static_cast<uint32_t>(desc->Flags) : NA_UNKNOWN_U32;
    remember_creation(obj, e);
    emit(e, NA_GROUP_RESOURCE);
}
HRESULT STDMETHODCALLTYPE D_CreateHeap(ID3D12Device* dev, const D3D12_HEAP_DESC* desc, REFIID riid, void** out)
{
    Depth depth("CreateHeap");
    const HRESULT hr = o_CreateHeap(dev, desc, riid, out);
    if (IUnknown* obj = depth.outermost() ? out_object(hr, out) : nullptr) heap_created(dev, obj, desc);
    return hr;
}
HRESULT STDMETHODCALLTYPE D_CreateHeap1(ID3D12Device4* dev, const D3D12_HEAP_DESC* desc, ID3D12ProtectedResourceSession* ps, REFIID riid, void** out)
{
    Depth depth("CreateHeap1");
    const HRESULT hr = o_CreateHeap1(dev, desc, ps, riid, out);
    if (IUnknown* obj = depth.outermost() ? out_object(hr, out) : nullptr) heap_created(dev, obj, desc);
    return hr;
}

HRESULT STDMETHODCALLTYPE D_CreatePlacedResource(ID3D12Device* dev, ID3D12Heap* heap, UINT64 off, const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES s,
                                                 const D3D12_CLEAR_VALUE* c, REFIID riid, void** out)
{
    Depth depth("CreatePlacedResource");
    const HRESULT hr = o_CreatePlacedResource(dev, heap, off, d, s, c, riid, out);
    if (IUnknown* obj = depth.outermost() ? out_object(hr, out) : nullptr) note_resource(dev, obj, NA_CREATE_PLACED, heap, off, nullptr, s);
    return hr;
}
HRESULT STDMETHODCALLTYPE D_CreatePlacedResource1(ID3D12Device8* dev, ID3D12Heap* heap, UINT64 off, const D3D12_RESOURCE_DESC1* d, D3D12_RESOURCE_STATES s,
                                                  const D3D12_CLEAR_VALUE* c, REFIID riid, void** out)
{
    Depth depth("CreatePlacedResource1");
    const HRESULT hr = o_CreatePlacedResource1(dev, heap, off, d, s, c, riid, out);
    if (IUnknown* obj = depth.outermost() ? out_object(hr, out) : nullptr) note_resource(dev, obj, NA_CREATE_PLACED, heap, off, nullptr, s);
    return hr;
}
HRESULT STDMETHODCALLTYPE D_CreateReservedResource(ID3D12Device* dev, const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c,
                                                   REFIID riid, void** out)
{
    Depth depth("CreateReservedResource");
    const HRESULT hr = o_CreateReservedResource(dev, d, s, c, riid, out);
    if (IUnknown* obj = depth.outermost() ? out_object(hr, out) : nullptr) note_resource(dev, obj, NA_CREATE_RESERVED, nullptr, 0, nullptr, s);
    return hr;
}
HRESULT STDMETHODCALLTYPE D_CreateReservedResource1(ID3D12Device4* dev, const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c,
                                                    ID3D12ProtectedResourceSession* ps, REFIID riid, void** out)
{
    Depth depth("CreateReservedResource1");
    const HRESULT hr = o_CreateReservedResource1(dev, d, s, c, ps, riid, out);
    if (IUnknown* obj = depth.outermost() ? out_object(hr, out) : nullptr) note_resource(dev, obj, NA_CREATE_RESERVED, nullptr, 0, nullptr, s);
    return hr;
}

HRESULT STDMETHODCALLTYPE D_CreateFence(ID3D12Device* dev, UINT64 initial, D3D12_FENCE_FLAGS flags, REFIID riid, void** out)
{
    Depth depth("CreateFence");
    const HRESULT hr = o_CreateFence(dev, initial, flags, riid, out);
    if (IUnknown* obj = depth.outermost() ? out_object(hr, out) : nullptr) {
        const na_object_id dev_id = device_id(dev);
        na_ev_fence_created e;
        init_header(e, NA_EV_FENCE_CREATED, NA_EVF_FOUNDATIONAL);
        e.fence = register_object(obj, NA_OBJ_FENCE);
        e.device = dev_id;
        e.initial_value = initial;
        e.fence_flags = flags;
        remember_creation(obj, e);
        emit(e, NA_GROUP_QUEUE);
    }
    return hr;
}

// ------------------------------------------------------------------ queues
QueueState* queue_state(ID3D12CommandQueue* q, na_object_id* id)
{
    {
        std::shared_lock<std::shared_mutex> lock(g_m);
        auto it = g_objs.find(q);
        if (it != g_objs.end() && it->second.queue) {
            *id = it->second.id;
            return it->second.queue.get();
        }
    }
    // A queue created before installation: identity from now on, flagged.
    const na_object_id qid = register_object(q, NA_OBJ_QUEUE);
    na_ev_queue_created e;
    init_header(e, NA_EV_QUEUE_CREATED, NA_EVF_FOUNDATIONAL | NA_EVF_PREEXISTING);
    e.queue = qid;
    e.device = 0;
    const D3D12_COMMAND_QUEUE_DESC d = q->GetDesc();
    e.queue_type = d.Type;
    e.queue_flags = d.Flags;
    remember_creation(q, e);
    emit(e, NA_GROUP_QUEUE);
    std::shared_lock<std::shared_mutex> lock(g_m);
    auto it = g_objs.find(q);
    *id = qid;
    return (it != g_objs.end() && it->second.queue) ? it->second.queue.get() : nullptr;
}

void sync_event(uint32_t kind, ID3D12CommandQueue* q, ID3D12Fence* f, UINT64 value, HRESULT hr, uint64_t ticket)
{
    na_ev_queue_sync e;
    init_header(e, kind, 0);
    na_object_id qid = 0;
    queue_state(q, &qid);
    e.queue = qid;
    e.fence = lookup(f);
    e.value = value;
    e.queue_ticket = ticket;
    e.hresult = hr;
    // H1 WAIT-CORRELATION RULE: no game-fence wait is correlated to a source.
    e.provenance = kind == NA_EV_QUEUE_SIGNAL ? NA_FENCE_PROV_OBSERVED_QUEUE_SIGNAL : NA_FENCE_PROV_UNKNOWN;
    emit(e, NA_GROUP_QUEUE);
}

HRESULT STDMETHODCALLTYPE D_Signal(ID3D12CommandQueue* q, ID3D12Fence* f, UINT64 v)
{
    na_object_id qid = 0;
    QueueState* qs = queue_state(q, &qid);
    const uint64_t ticket = qs ? qs->ticket.fetch_add(1) + 1 : 0;
    const HRESULT hr = o_Signal(q, f, v);
    sync_event(NA_EV_QUEUE_SIGNAL, q, f, v, hr, ticket);
    return hr;
}
HRESULT STDMETHODCALLTYPE D_Wait(ID3D12CommandQueue* q, ID3D12Fence* f, UINT64 v)
{
    na_object_id qid = 0;
    QueueState* qs = queue_state(q, &qid);
    const uint64_t ticket = qs ? qs->ticket.fetch_add(1) + 1 : 0;
    const HRESULT hr = o_Wait(q, f, v);
    sync_event(NA_EV_QUEUE_WAIT, q, f, v, hr, ticket);
    return hr;
}

void STDMETHODCALLTYPE D_ExecuteCommandLists(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists)
{
    const ExecuteToken t = ExecuteBegin(q);
    o_ExecuteCommandLists(q, n, lists);
    ExecuteEnd(t, q, n, lists);
}

// ---------------------------------------------------------------- snapshot
bool snapshot()
{
    // Capability report first: what this producer set observes.
    struct Report {
        na_ev_capability_report r;
        na_cap_entry entries[NA_GROUP_COUNT_V1];
    } rep;
    std::memset(&rep, 0, sizeof(rep));
    rep.r.hdr.size_bytes = sizeof(rep);
    rep.r.hdr.kind = NA_EV_CAPABILITY_REPORT;
    rep.r.hdr.flags = NA_EVF_FOUNDATIONAL;
    rep.r.hdr.producer_thread = GetCurrentThreadId();
    rep.r.host_version_major = NA_OBSERVE_ABI_MAJOR;
    rep.r.host_version_minor = NA_OBSERVE_ABI_MINOR;
    rep.r.entry_count = NA_GROUP_COUNT_V1;
    rep.r.entries_offset = offsetof(Report, entries);
    rep.r.entry_stride = sizeof(na_cap_entry);
    const uint32_t support[NA_GROUP_COUNT_V1] = {
        NA_CAP_SUPPORTED, NA_CAP_SUPPORTED, NA_CAP_PARTIAL /* CPU-side record-time descriptors */, NA_CAP_PARTIAL /* reset/close: C8b */,
        NA_CAP_SUPPORTED, NA_CAP_UNSUPPORTED, NA_CAP_UNSUPPORTED, NA_CAP_UNSUPPORTED, NA_CAP_UNSUPPORTED, NA_CAP_UNSUPPORTED,
        NA_CAP_UNSUPPORTED};
    for (uint32_t g = 0; g < NA_GROUP_COUNT_V1; ++g) {
        rep.entries[g].group = g;
        rep.entries[g].support = support[g];
    }
    bool ok = na::observe_internal::Emit(&rep, NA_GROUP_SESSION);
    // Live inventory: creation events in id (= creation) order.
    std::vector<std::pair<na_object_id, std::vector<uint8_t>>> events;
    {
        std::shared_lock<std::shared_mutex> lock(g_m);
        events.reserve(g_objs.size());
        for (const auto& kv : g_objs)
            if (!kv.second.created.empty()) events.emplace_back(kv.second.id, kv.second.created);
    }
    std::sort(events.begin(), events.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& ev : events) {
        auto* hdr = reinterpret_cast<na_event_header*>(ev.second.data());
        hdr->thread_seq = ++t_seq;
        hdr->producer_thread = GetCurrentThreadId();
        uint32_t group = NA_GROUP_SESSION;
        switch (hdr->kind) {
        case NA_EV_RESOURCE_CREATED:
        case NA_EV_RESOURCE_HEAP_CREATED: group = NA_GROUP_RESOURCE; break;
        case NA_EV_DESCRIPTOR_HEAP_CREATED: group = NA_GROUP_DESCRIPTOR; break;
        case NA_EV_LIST_CREATED: group = NA_GROUP_LIST; break;
        case NA_EV_QUEUE_CREATED:
        case NA_EV_FENCE_CREATED: group = NA_GROUP_QUEUE; break;
        default: break;
        }
        ok = na::observe_internal::Emit(ev.second.data(), group) && ok;
    }
    return ok;
}

// ======================================================================
// C8b / ABI 0.2: pipelines, root signatures, recorded list operations.
// ======================================================================
uint64_t fnv1a(const void* data, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

void bytecode(na_ev_pipeline_created& e, uint32_t stage, const D3D12_SHADER_BYTECODE& b)
{
    if (!b.pShaderBytecode || !b.BytecodeLength) return;
    e.stage_hash[stage] = fnv1a(b.pShaderBytecode, b.BytecodeLength);
    e.stage_bytes[stage] = static_cast<uint32_t>(b.BytecodeLength);
}

void depth_blend(na_ev_pipeline_created& e, BOOL depth_enable, D3D12_DEPTH_WRITE_MASK mask, D3D12_COMPARISON_FUNC func)
{
    e.depth_enable = depth_enable ? 1u : 0u;
    e.depth_write = mask == D3D12_DEPTH_WRITE_MASK_ALL ? 1u : 0u;
    e.depth_func = static_cast<uint32_t>(func);
}

void blend_mask(na_ev_pipeline_created& e, const D3D12_BLEND_DESC& b)
{
    e.blend_enable_mask = 0;
    for (uint32_t i = 0; i < 8; ++i) {
        const BOOL on = b.IndependentBlendEnable ? b.RenderTarget[i].BlendEnable : b.RenderTarget[0].BlendEnable;
        if (on) e.blend_enable_mask |= 1u << i;
    }
}

void pipeline_unknowns(na_ev_pipeline_created& e)
{
    for (auto& f : e.rtv_formats) f = 0;
    e.dsv_format = 0;
    e.sample_count = NA_UNKNOWN_U32;
    e.depth_enable = e.depth_write = e.depth_func = NA_UNKNOWN_U32;
    e.blend_enable_mask = NA_UNKNOWN_U32;
}

void emit_pipeline(ID3D12Device* dev, IUnknown* obj, na_ev_pipeline_created& e, ID3D12RootSignature* rs)
{
    e.device = device_id(dev);
    e.pipeline = register_object(obj, NA_OBJ_PIPELINE_HOST);
    e.root_signature = rs ? lookup(rs) : 0;
    remember_creation(obj, e);
    emit(e, NA_GROUP_PIPELINE);
}

NA_ORIG(CreateRootSignature, HRESULT, ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);
NA_ORIG(CreateGraphicsPipelineState, HRESULT, ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
NA_ORIG(CreateComputePipelineState, HRESULT, ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
NA_ORIG(CreatePipelineState, HRESULT, ID3D12Device2*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);

HRESULT STDMETHODCALLTYPE D_CreateRootSignature(ID3D12Device* dev, UINT mask, const void* blob, SIZE_T size, REFIID riid, void** out)
{
    Depth depth("CreateRootSignature");
    const HRESULT hr = o_CreateRootSignature(dev, mask, blob, size, riid, out);
    IUnknown* obj = depth.outermost() ? out_object(hr, out) : nullptr;
    if (!obj) return hr;
    constexpr uint32_t kMaxParams = 64;
    struct Ev {
        na_ev_root_signature_created e;
        na_root_param p[kMaxParams];
    } ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.e.hdr.kind = NA_EV_ROOT_SIGNATURE_CREATED;
    ev.e.hdr.thread_seq = ++t_seq;
    ev.e.hdr.producer_thread = GetCurrentThreadId();
    ev.e.hdr.flags = NA_EVF_FOUNDATIONAL;
    ev.e.blob_hash = blob ? fnv1a(blob, size) : 0;
    ev.e.blob_bytes = size;
    ev.e.flags = NA_UNKNOWN_U32;
    ev.e.params_offset = offsetof(Ev, p);
    ev.e.param_stride = sizeof(na_root_param);
    ID3D12VersionedRootSignatureDeserializer* des = nullptr;
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* vd = nullptr;
    if (blob && SUCCEEDED(D3D12CreateVersionedRootSignatureDeserializer(blob, size, IID_PPV_ARGS(&des))) && des &&
        SUCCEEDED(des->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &vd)) && vd) {
        const D3D12_ROOT_SIGNATURE_DESC1& rd = vd->Desc_1_1;
        ev.e.flags = rd.Flags;
        const uint32_t n = std::min<uint32_t>(rd.NumParameters, kMaxParams);
        if (n < rd.NumParameters) ev.e.hdr.flags |= NA_EVF_TRUNCATED;
        ev.e.param_count = n;
        for (uint32_t i = 0; i < n; ++i) {
            const D3D12_ROOT_PARAMETER1& rp = rd.pParameters[i];
            na_root_param& o = ev.p[i];
            o.visibility = rp.ShaderVisibility;
            o.range_type = NA_UNKNOWN_U32;
            switch (rp.ParameterType) {
            case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
                o.param_type = NA_ROOT_TABLE;
                o.count = rp.DescriptorTable.NumDescriptorRanges;
                if (o.count) {
                    const D3D12_DESCRIPTOR_RANGE1& r0 = rp.DescriptorTable.pDescriptorRanges[0];
                    o.range_type = r0.RangeType;
                    o.range_descriptors = r0.NumDescriptors;
                    o.shader_register = r0.BaseShaderRegister;
                    o.register_space = r0.RegisterSpace;
                }
                break;
            case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
                o.param_type = NA_ROOT_CONSTANTS;
                o.shader_register = rp.Constants.ShaderRegister;
                o.register_space = rp.Constants.RegisterSpace;
                o.count = rp.Constants.Num32BitValues;
                break;
            default:
                o.param_type = rp.ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV ? NA_ROOT_CBV
                             : rp.ParameterType == D3D12_ROOT_PARAMETER_TYPE_SRV ? NA_ROOT_SRV : NA_ROOT_UAV;
                o.shader_register = rp.Descriptor.ShaderRegister;
                o.register_space = rp.Descriptor.RegisterSpace;
                break;
            }
        }
    } else {
        ev.e.hdr.flags |= NA_EVF_METHOD_UNOBSERVED; // blob not decodable here
    }
    if (des) des->Release();
    ev.e.hdr.size_bytes = static_cast<uint32_t>(offsetof(Ev, p) + ev.e.param_count * sizeof(na_root_param));
    ev.e.device = device_id(dev);
    ev.e.root_signature = register_object(obj, NA_OBJ_PIPELINE_HOST);
    {
        std::unique_lock<std::shared_mutex> lock(g_m);
        auto it = g_objs.find(obj);
        if (it != g_objs.end()) {
            const auto* b = reinterpret_cast<const uint8_t*>(&ev);
            it->second.created.assign(b, b + ev.e.hdr.size_bytes);
        }
    }
    na::observe_internal::Emit(&ev, NA_GROUP_PIPELINE);
    return hr;
}

HRESULT STDMETHODCALLTYPE D_CreateGraphicsPipelineState(ID3D12Device* dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, REFIID riid, void** out)
{
    Depth depth("CreateGraphicsPipelineState");
    const HRESULT hr = o_CreateGraphicsPipelineState(dev, d, riid, out);
    IUnknown* obj = depth.outermost() ? out_object(hr, out) : nullptr;
    if (!obj || !d) return hr;
    na_ev_pipeline_created e;
    init_header(e, NA_EV_PIPELINE_CREATED, NA_EVF_FOUNDATIONAL);
    e.pipeline_kind = NA_PIPELINE_GRAPHICS;
    bytecode(e, NA_STAGE_VS, d->VS);
    bytecode(e, NA_STAGE_PS, d->PS);
    bytecode(e, NA_STAGE_DS, d->DS);
    bytecode(e, NA_STAGE_HS, d->HS);
    bytecode(e, NA_STAGE_GS, d->GS);
    e.rt_count = d->NumRenderTargets;
    for (uint32_t i = 0; i < 8; ++i) e.rtv_formats[i] = d->RTVFormats[i];
    e.dsv_format = d->DSVFormat;
    e.sample_count = d->SampleDesc.Count;
    depth_blend(e, d->DepthStencilState.DepthEnable, d->DepthStencilState.DepthWriteMask, d->DepthStencilState.DepthFunc);
    blend_mask(e, d->BlendState);
    emit_pipeline(dev, obj, e, d->pRootSignature);
    return hr;
}

HRESULT STDMETHODCALLTYPE D_CreateComputePipelineState(ID3D12Device* dev, const D3D12_COMPUTE_PIPELINE_STATE_DESC* d, REFIID riid, void** out)
{
    Depth depth("CreateComputePipelineState");
    const HRESULT hr = o_CreateComputePipelineState(dev, d, riid, out);
    IUnknown* obj = depth.outermost() ? out_object(hr, out) : nullptr;
    if (!obj || !d) return hr;
    na_ev_pipeline_created e;
    init_header(e, NA_EV_PIPELINE_CREATED, NA_EVF_FOUNDATIONAL);
    e.pipeline_kind = NA_PIPELINE_COMPUTE;
    pipeline_unknowns(e);
    e.sample_count = 0;
    e.depth_enable = e.depth_write = 0;
    e.blend_enable_mask = 0;
    bytecode(e, NA_STAGE_CS, d->CS);
    emit_pipeline(dev, obj, e, d->pRootSignature);
    return hr;
}

// Pipeline state stream: subobjects are pointer-aligned {type; payload}.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324) // padding to pointer alignment IS the stream layout
#endif
template <typename T>
struct alignas(void*) Sub {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
    T inner;
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
template <typename T>
const T& inner_of(const uint8_t* p) { return reinterpret_cast<const Sub<T>*>(p)->inner; }

size_t sub_size(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t)
{
    switch (t) {
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: return sizeof(Sub<ID3D12RootSignature*>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: return sizeof(Sub<D3D12_SHADER_BYTECODE>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: return sizeof(Sub<D3D12_STREAM_OUTPUT_DESC>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: return sizeof(Sub<D3D12_BLEND_DESC>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: return sizeof(Sub<UINT>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: return sizeof(Sub<D3D12_RASTERIZER_DESC>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: return sizeof(Sub<D3D12_DEPTH_STENCIL_DESC>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: return sizeof(Sub<D3D12_INPUT_LAYOUT_DESC>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: return sizeof(Sub<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: return sizeof(Sub<D3D12_PRIMITIVE_TOPOLOGY_TYPE>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: return sizeof(Sub<D3D12_RT_FORMAT_ARRAY>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: return sizeof(Sub<DXGI_FORMAT>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: return sizeof(Sub<DXGI_SAMPLE_DESC>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: return sizeof(Sub<UINT>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: return sizeof(Sub<D3D12_CACHED_PIPELINE_STATE>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: return sizeof(Sub<D3D12_PIPELINE_STATE_FLAGS>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: return sizeof(Sub<D3D12_DEPTH_STENCIL_DESC1>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: return sizeof(Sub<D3D12_VIEW_INSTANCING_DESC>);
    default: return 0; // newer subobject: stop here, the rest is reported unobserved
    }
}

HRESULT STDMETHODCALLTYPE D_CreatePipelineState(ID3D12Device2* dev, const D3D12_PIPELINE_STATE_STREAM_DESC* d, REFIID riid, void** out)
{
    Depth depth("CreatePipelineState");
    const HRESULT hr = o_CreatePipelineState(dev, d, riid, out);
    IUnknown* obj = depth.outermost() ? out_object(hr, out) : nullptr;
    if (!obj || !d || !d->pPipelineStateSubobjectStream) return hr;
    na_ev_pipeline_created e;
    init_header(e, NA_EV_PIPELINE_CREATED, NA_EVF_FOUNDATIONAL);
    e.pipeline_kind = NA_PIPELINE_STREAM;
    pipeline_unknowns(e);
    e.sample_count = 1;
    ID3D12RootSignature* rs = nullptr;
    const auto* p = static_cast<const uint8_t*>(d->pPipelineStateSubobjectStream);
    size_t at = 0;
    bool saw_depth = false;
    while (at + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) <= d->SizeInBytes) {
        const auto type = *reinterpret_cast<const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE*>(p + at);
        const size_t n = sub_size(type);
        if (!n || at + n > d->SizeInBytes) {
            e.hdr.flags |= NA_EVF_METHOD_UNOBSERVED;
            break;
        }
        const uint8_t* s = p + at;
        switch (type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: rs = inner_of<ID3D12RootSignature*>(s); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: bytecode(e, NA_STAGE_VS, inner_of<D3D12_SHADER_BYTECODE>(s)); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS: bytecode(e, NA_STAGE_PS, inner_of<D3D12_SHADER_BYTECODE>(s)); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS: bytecode(e, NA_STAGE_DS, inner_of<D3D12_SHADER_BYTECODE>(s)); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS: bytecode(e, NA_STAGE_HS, inner_of<D3D12_SHADER_BYTECODE>(s)); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: bytecode(e, NA_STAGE_GS, inner_of<D3D12_SHADER_BYTECODE>(s)); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS: bytecode(e, NA_STAGE_CS, inner_of<D3D12_SHADER_BYTECODE>(s)); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS: bytecode(e, NA_STAGE_AS, inner_of<D3D12_SHADER_BYTECODE>(s)); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: bytecode(e, NA_STAGE_MS, inner_of<D3D12_SHADER_BYTECODE>(s)); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: blend_mask(e, inner_of<D3D12_BLEND_DESC>(s)); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: {
            const auto& ds = inner_of<D3D12_DEPTH_STENCIL_DESC>(s);
            depth_blend(e, ds.DepthEnable, ds.DepthWriteMask, ds.DepthFunc);
            saw_depth = true;
            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: {
            const auto& ds = inner_of<D3D12_DEPTH_STENCIL_DESC1>(s);
            depth_blend(e, ds.DepthEnable, ds.DepthWriteMask, ds.DepthFunc);
            saw_depth = true;
            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: {
            const auto& f = inner_of<D3D12_RT_FORMAT_ARRAY>(s);
            e.rt_count = f.NumRenderTargets;
            for (uint32_t i = 0; i < 8; ++i) e.rtv_formats[i] = f.RTFormats[i];
            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: e.dsv_format = inner_of<DXGI_FORMAT>(s); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: e.sample_count = inner_of<DXGI_SAMPLE_DESC>(s).Count; break;
        default: break;
        }
        at += n;
    }
    // A stream without a depth-stencil subobject uses the defaults (depth on, write all, LESS).
    if (!saw_depth && e.stage_hash[NA_STAGE_CS] == 0) depth_blend(e, TRUE, D3D12_DEPTH_WRITE_MASK_ALL, D3D12_COMPARISON_FUNC_LESS);
    if (e.blend_enable_mask == NA_UNKNOWN_U32 && e.stage_hash[NA_STAGE_CS] == 0) e.blend_enable_mask = 0; // default: no blending
    emit_pipeline(dev, obj, e, rs);
    return hr;
}

// ---------------------------------------------------- command-list ops
struct Rec {
    na_object_id list = 0;
    uint64_t gen = NA_UNKNOWN_U64;
    uint64_t op_index = 0;
    uint32_t flags = 0;
};

ListState* list_state_locked(const void* l, na_object_id* id)
{
    auto it = g_objs.find(l);
    if (it == g_objs.end() || !it->second.list) return nullptr;
    *id = it->second.id;
    return it->second.list.get();
}

// A list first met through a recorded op (created before installation).
void preexisting_list(ID3D12GraphicsCommandList* l)
{
    const na_object_id id = register_object(l, NA_OBJ_LIST);
    na_ev_list_created e;
    init_header(e, NA_EV_LIST_CREATED, NA_EVF_FOUNDATIONAL | NA_EVF_PREEXISTING);
    e.list = id;
    e.device = 0;
    e.initial_allocator = 0;
    e.list_type = l->GetType();
    e.is_bundle = e.list_type == D3D12_COMMAND_LIST_TYPE_BUNDLE ? 1u : 0u;
    e.initially_open = NA_UNKNOWN_U32;
    remember_creation(l, e);
    emit(e, NA_GROUP_LIST);
}

bool begin_op(ID3D12GraphicsCommandList* l, Rec* r)
{
    if (!na::observe_internal::Started() || !l) return false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        {
            std::shared_lock<std::shared_mutex> lock(g_m);
            na_object_id id = 0;
            if (ListState* ls = list_state_locked(l, &id)) {
                r->list = id;
                r->gen = ls->generation_known.load() ? ls->generation.load() : NA_UNKNOWN_U64;
                r->op_index = ls->op_index.fetch_add(1) + 1;
                if (!ls->recording_observed.load() || !ls->coverage_complete.load() || r->gen == NA_UNKNOWN_U64)
                    r->flags |= NA_EVF_METHOD_UNOBSERVED;
                return true;
            }
        }
        preexisting_list(l);
    }
    return false;
}

void emit_op(const Rec& r, uint32_t op, std::initializer_list<uint64_t> a, const void* payload = nullptr, uint32_t bytes = 0,
             uint32_t stride = 0, uint32_t extra_flags = 0, uint32_t group = NA_GROUP_PASS)
{
    constexpr uint32_t kMaxPayload = 64 * sizeof(na_barrier);
    struct Ev {
        na_ev_list_op e;
        uint8_t payload[kMaxPayload];
    } ev;
    std::memset(&ev.e, 0, sizeof(ev.e));
    uint32_t flags = r.flags | extra_flags;
    if (bytes > kMaxPayload) {
        bytes = stride ? (kMaxPayload / stride) * stride : kMaxPayload;
        flags |= NA_EVF_TRUNCATED;
    }
    const uint32_t padded = (bytes + 7u) & ~7u;
    ev.e.hdr.size_bytes = static_cast<uint32_t>(sizeof(na_ev_list_op) + padded);
    ev.e.hdr.kind = NA_EV_LIST_OP;
    ev.e.hdr.thread_seq = ++t_seq;
    ev.e.hdr.producer_thread = GetCurrentThreadId();
    ev.e.hdr.flags = flags;
    ev.e.list = r.list;
    ev.e.recording_generation = r.gen;
    ev.e.op_index = r.op_index;
    ev.e.op = op;
    if (bytes) {
        ev.e.payload_offset = sizeof(na_ev_list_op);
        ev.e.payload_bytes = bytes;
        ev.e.payload_stride = stride;
        std::memcpy(ev.payload, payload, bytes);
        if (padded > bytes) std::memset(ev.payload + bytes, 0, padded - bytes);
    }
    uint32_t i = 0;
    for (uint64_t v : a) if (i < 6) ev.e.a[i++] = v;
    na::observe_internal::Emit(&ev, group);
}

na_object_id id_or_unknown(const void* obj)
{
    if (!obj) return 0;
    const na_object_id id = lookup(obj);
    return id ? id : NA_UNKNOWN_U64;
}

DescHeap* heap_for_gpu_locked(uint64_t ptr, uint32_t* slot)
{
    for (auto& h : g_heaps) {
        if (!h->gpu_start || !h->increment || ptr < h->gpu_start) continue;
        const uint64_t off = ptr - h->gpu_start;
        if (off % h->increment) continue;
        const uint64_t s = off / h->increment;
        if (s < h->count) {
            *slot = static_cast<uint32_t>(s);
            return h.get();
        }
    }
    return nullptr;
}

na_view_ref view_ref_locked(DescHeap* h, uint32_t slot)
{
    na_view_ref v{};
    if (!h) {
        v.resource = NA_UNKNOWN_U64;
        v.descriptor_kind = NA_UNKNOWN_U32;
        v.format = v.first_subresource = NA_UNKNOWN_U32;
        return v;
    }
    const DescHeap::SlotInfo& si = h->info[slot];
    v.heap = h->id;
    v.slot = slot;
    v.slot_version = h->versions[slot];
    v.descriptor_kind = si.kind;
    v.resource = si.resource;
    v.format = si.format;
    v.first_subresource = si.first_sub;
    return v;
}

na_view_ref cpu_ref(D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    std::shared_lock<std::shared_mutex> lock(g_m);
    uint32_t slot = 0;
    DescHeap* dh = heap_for_cpu_locked(h.ptr, &slot);
    return view_ref_locked(dh, slot);
}

uint64_t float_bits(float a, float b)
{
    uint32_t x = 0, y = 0;
    std::memcpy(&x, &a, 4);
    std::memcpy(&y, &b, 4);
    return static_cast<uint64_t>(x) | (static_cast<uint64_t>(y) << 32);
}

using GL = ID3D12GraphicsCommandList;

// Per-slot recording, run after the original call (only on the outermost scope).
template <uint32_t Slot>
struct On;

template <>
struct On<sl::list::Reset> {
    static void run(GL* l, HRESULT hr, ID3D12CommandAllocator* alloc, ID3D12PipelineState* pso)
    {
        (void)pso;
        na_object_id id = 0;
        bool known = false;
        uint64_t gen = 0;
        bool bundle = false;
        {
            std::shared_lock<std::shared_mutex> lock(g_m);
            known = list_state_locked(l, &id) != nullptr;
        }
        if (!known) preexisting_list(l);
        const auto cov = na::hooking::variants::CoverageOf(na::hooking::variants::Kind::CommandList, l);
        {
            std::shared_lock<std::shared_mutex> lock(g_m);
            ListState* ls = list_state_locked(l, &id);
            if (!ls) return;
            if (SUCCEEDED(hr)) {
                // H1: generation increments only on a successful Reset; an
                // unknown generation stays unknown (we never invent a count).
                if (ls->generation_known.load()) ls->generation.fetch_add(1);
                ls->op_index.store(0);
                ls->recording_observed.store(true);
                ls->coverage_complete.store(cov == na::hooking::variants::Coverage::Complete);
            }
            gen = ls->generation_known.load() ? ls->generation.load() : NA_UNKNOWN_U64;
            auto it = g_objs.find(l);
            bundle = it != g_objs.end() && it->second.created.size() >= sizeof(na_ev_list_created) &&
                     reinterpret_cast<const na_ev_list_created*>(it->second.created.data())->is_bundle;
        }
        if (!na::observe_internal::Started()) return;
        na_object_id alloc_id = alloc ? lookup(alloc) : 0;
        if (alloc && !alloc_id) alloc_id = register_object(alloc, NA_OBJ_ALLOCATOR);
        na_ev_list_reset_close e;
        init_header(e, NA_EV_LIST_RESET, 0);
        e.list = id;
        e.allocator = alloc_id;
        e.recording_generation = gen;
        e.hresult = hr;
        e.list_flags = (FAILED(hr) ? NA_LISTF_RESET_FAILED : 0u) | (bundle ? NA_LISTF_IS_BUNDLE : 0u);
        emit(e, NA_GROUP_LIST);
    }
};
template <>
struct On<sl::list::Close> {
    static void run(GL* l, HRESULT hr)
    {
        Rec r;
        if (!begin_op(l, &r)) return;
        na_ev_list_reset_close e;
        init_header(e, NA_EV_LIST_CLOSE, 0);
        e.list = r.list;
        e.recording_generation = r.gen;
        e.hresult = hr;
        emit(e, NA_GROUP_LIST);
    }
};
template <>
struct On<sl::list::DrawInstanced> {
    static void run(GL* l, UINT v, UINT i, UINT, UINT)
    {
        Rec r;
        if (begin_op(l, &r)) emit_op(r, NA_OP_DRAW, {v, i});
    }
};
template <>
struct On<sl::list::DrawIndexedInstanced> {
    static void run(GL* l, UINT n, UINT i, UINT, INT, UINT)
    {
        Rec r;
        if (begin_op(l, &r)) emit_op(r, NA_OP_DRAW_INDEXED, {n, i});
    }
};
template <>
struct On<sl::list::Dispatch> {
    static void run(GL* l, UINT x, UINT y, UINT z)
    {
        Rec r;
        if (begin_op(l, &r)) emit_op(r, NA_OP_DISPATCH, {x, y, z});
    }
};
template <>
struct On<sl::list::CopyBufferRegion> {
    static void run(GL* l, ID3D12Resource* dst, UINT64 doff, ID3D12Resource* src, UINT64 soff, UINT64 n)
    {
        Rec r;
        if (begin_op(l, &r)) emit_op(r, NA_OP_COPY_BUFFER_REGION, {id_or_unknown(dst), doff, id_or_unknown(src), soff, n}, nullptr, 0, 0, 0, NA_GROUP_RESOURCE_USE);
    }
};
template <>
struct On<sl::list::CopyTextureRegion> {
    static void run(GL* l, const D3D12_TEXTURE_COPY_LOCATION* dst, UINT, UINT, UINT, const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX*)
    {
        Rec r;
        if (!begin_op(l, &r) || !dst || !src) return;
        const auto sub = [](const D3D12_TEXTURE_COPY_LOCATION* c) {
            return c->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX ? static_cast<uint64_t>(c->SubresourceIndex) : uint64_t{NA_UNKNOWN_U32};
        };
        emit_op(r, NA_OP_COPY_TEXTURE_REGION, {id_or_unknown(dst->pResource), sub(dst), id_or_unknown(src->pResource), sub(src)}, nullptr, 0, 0, 0,
                NA_GROUP_RESOURCE_USE);
    }
};
template <>
struct On<sl::list::CopyResource> {
    static void run(GL* l, ID3D12Resource* dst, ID3D12Resource* src)
    {
        Rec r;
        if (begin_op(l, &r)) emit_op(r, NA_OP_COPY_RESOURCE, {id_or_unknown(dst), id_or_unknown(src)}, nullptr, 0, 0, 0, NA_GROUP_RESOURCE_USE);
    }
};
template <>
struct On<sl::list::ResolveSubresource> {
    static void run(GL* l, ID3D12Resource* dst, UINT ds, ID3D12Resource* src, UINT ss, DXGI_FORMAT f)
    {
        Rec r;
        if (begin_op(l, &r)) emit_op(r, NA_OP_RESOLVE, {id_or_unknown(dst), ds, id_or_unknown(src), ss, static_cast<uint64_t>(f)}, nullptr, 0, 0, 0, NA_GROUP_RESOURCE_USE);
    }
};
template <>
struct On<sl::list::RSSetViewports> {
    static void run(GL* l, UINT n, const D3D12_VIEWPORT* v)
    {
        Rec r;
        if (!begin_op(l, &r) || !n || !v) return;
        emit_op(r, NA_OP_RS_VIEWPORT, {n, float_bits(v->TopLeftX, v->TopLeftY), float_bits(v->Width, v->Height), float_bits(v->MinDepth, v->MaxDepth)});
    }
};
template <>
struct On<sl::list::RSSetScissorRects> {
    static void run(GL* l, UINT n, const D3D12_RECT* rc)
    {
        Rec r;
        if (!begin_op(l, &r) || !n || !rc) return;
        const auto pack = [](LONG a, LONG b) { return static_cast<uint64_t>(static_cast<uint32_t>(a)) | (static_cast<uint64_t>(static_cast<uint32_t>(b)) << 32); };
        emit_op(r, NA_OP_RS_SCISSOR, {n, pack(rc->left, rc->top), pack(rc->right, rc->bottom)});
    }
};
template <>
struct On<sl::list::SetPipelineState> {
    static void run(GL* l, ID3D12PipelineState* p)
    {
        Rec r;
        if (begin_op(l, &r)) emit_op(r, NA_OP_SET_PIPELINE, {id_or_unknown(p)});
    }
};
template <>
struct On<sl::list::ResourceBarrier> {
    static void run(GL* l, UINT n, const D3D12_RESOURCE_BARRIER* b)
    {
        Rec r;
        if (!begin_op(l, &r) || !n || !b) return;
        constexpr uint32_t kMax = 64;
        na_barrier out[kMax];
        const uint32_t m = std::min<uint32_t>(n, kMax);
        for (uint32_t i = 0; i < m; ++i) {
            na_barrier& o = out[i];
            std::memset(&o, 0, sizeof(o));
            o.barrier_type = b[i].Type;
            o.barrier_flags = b[i].Flags;
            o.state_before = o.state_after = NA_UNKNOWN_U32;
            switch (b[i].Type) {
            case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION:
                o.resource = id_or_unknown(b[i].Transition.pResource);
                o.subresource = b[i].Transition.Subresource;
                o.state_before = b[i].Transition.StateBefore;
                o.state_after = b[i].Transition.StateAfter;
                break;
            case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:
                o.resource = id_or_unknown(b[i].Aliasing.pResourceBefore);
                o.resource_after = id_or_unknown(b[i].Aliasing.pResourceAfter);
                break;
            default:
                o.resource = id_or_unknown(b[i].UAV.pResource);
                break;
            }
        }
        emit_op(r, NA_OP_RESOURCE_BARRIER, {n}, out, m * static_cast<uint32_t>(sizeof(na_barrier)), sizeof(na_barrier), m < n ? NA_EVF_TRUNCATED : 0u,
                NA_GROUP_RESOURCE_USE);
    }
};
template <>
struct On<sl::list::ExecuteBundle> {
    static void run(GL* l, ID3D12GraphicsCommandList* bundle)
    {
        Rec r;
        if (begin_op(l, &r)) emit_op(r, NA_OP_EXECUTE_BUNDLE, {id_or_unknown(bundle)}, nullptr, 0, 0, NA_EVF_METHOD_UNOBSERVED);
    }
};
template <>
struct On<sl::list::SetDescriptorHeaps> {
    static void run(GL* l, UINT n, ID3D12DescriptorHeap* const* h)
    {
        Rec r;
        if (!begin_op(l, &r)) return;
        na_object_id res = 0, smp = 0;
        for (UINT i = 0; h && i < n; ++i) {
            if (!h[i]) continue;
            std::shared_lock<std::shared_mutex> lock(g_m);
            for (auto& dh : g_heaps)
                if (dh->key == h[i]) (dh->type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? smp : res) = dh->id;
        }
        emit_op(r, NA_OP_SET_DESCRIPTOR_HEAPS, {res, smp});
    }
};
template <uint32_t Compute>
void root_sig(GL* l, ID3D12RootSignature* rs)
{
    Rec r;
    if (begin_op(l, &r)) emit_op(r, NA_OP_SET_ROOT_SIGNATURE, {id_or_unknown(rs), Compute});
}
template <>
struct On<sl::list::SetComputeRootSignature> { static void run(GL* l, ID3D12RootSignature* rs) { root_sig<1>(l, rs); } };
template <>
struct On<sl::list::SetGraphicsRootSignature> { static void run(GL* l, ID3D12RootSignature* rs) { root_sig<0>(l, rs); } };
template <uint32_t Compute>
void root_table(GL* l, UINT param, D3D12_GPU_DESCRIPTOR_HANDLE h)
{
    Rec r;
    if (!begin_op(l, &r)) return;
    na_object_id heap = 0;
    uint32_t slot = 0;
    uint64_t version = 0;
    {
        std::shared_lock<std::shared_mutex> lock(g_m);
        if (DescHeap* dh = heap_for_gpu_locked(h.ptr, &slot)) {
            heap = dh->id;
            version = dh->versions[slot];
        }
    }
    emit_op(r, NA_OP_SET_ROOT_TABLE, {heap, heap ? slot : uint64_t{NA_UNKNOWN_U32}, heap ? version : NA_UNKNOWN_U64, param, Compute}, nullptr, 0, 0,
            heap ? 0u : NA_EVF_METHOD_UNOBSERVED);
}
template <>
struct On<sl::list::SetComputeRootDescriptorTable> { static void run(GL* l, UINT p, D3D12_GPU_DESCRIPTOR_HANDLE h) { root_table<1>(l, p, h); } };
template <>
struct On<sl::list::SetGraphicsRootDescriptorTable> { static void run(GL* l, UINT p, D3D12_GPU_DESCRIPTOR_HANDLE h) { root_table<0>(l, p, h); } };
template <uint32_t Compute>
void root_consts(GL* l, UINT param, UINT count, const void* data, UINT dest)
{
    Rec r;
    if (!begin_op(l, &r) || !data) return;
    emit_op(r, NA_OP_SET_ROOT_CONSTANTS, {param, dest, count, Compute}, data, count * 4u, 4u, 0, NA_GROUP_CPU_CONSTANTS);
}
template <>
struct On<sl::list::SetComputeRoot32BitConstant> { static void run(GL* l, UINT p, UINT v, UINT d) { root_consts<1>(l, p, 1, &v, d); } };
template <>
struct On<sl::list::SetGraphicsRoot32BitConstant> { static void run(GL* l, UINT p, UINT v, UINT d) { root_consts<0>(l, p, 1, &v, d); } };
template <>
struct On<sl::list::SetComputeRoot32BitConstants> { static void run(GL* l, UINT p, UINT n, const void* v, UINT d) { root_consts<1>(l, p, n, v, d); } };
template <>
struct On<sl::list::SetGraphicsRoot32BitConstants> { static void run(GL* l, UINT p, UINT n, const void* v, UINT d) { root_consts<0>(l, p, n, v, d); } };
template <uint32_t Kind, uint32_t Compute>
void root_view(GL* l, UINT param, D3D12_GPU_VIRTUAL_ADDRESS va)
{
    Rec r;
    if (!begin_op(l, &r)) return;
    na_object_id res = 0;
    uint64_t off = 0;
    bool known = false;
    {
        std::shared_lock<std::shared_mutex> lock(g_m);
        known = resolve_va_locked(va, &res, &off);
    }
    // A bare GPU VA is never an identity (A2-R02): unresolved -> UNKNOWN.
    emit_op(r, NA_OP_SET_ROOT_VIEW, {known ? res : NA_UNKNOWN_U64, known ? off : NA_UNKNOWN_U64, param, Kind, Compute}, nullptr, 0, 0,
            known ? 0u : NA_EVF_METHOD_UNOBSERVED);
}
template <>
struct On<sl::list::SetComputeRootConstantBufferView> { static void run(GL* l, UINT p, D3D12_GPU_VIRTUAL_ADDRESS a) { root_view<NA_DESC_CBV, 1>(l, p, a); } };
template <>
struct On<sl::list::SetGraphicsRootConstantBufferView> { static void run(GL* l, UINT p, D3D12_GPU_VIRTUAL_ADDRESS a) { root_view<NA_DESC_CBV, 0>(l, p, a); } };
template <>
struct On<sl::list::SetComputeRootShaderResourceView> { static void run(GL* l, UINT p, D3D12_GPU_VIRTUAL_ADDRESS a) { root_view<NA_DESC_SRV, 1>(l, p, a); } };
template <>
struct On<sl::list::SetGraphicsRootShaderResourceView> { static void run(GL* l, UINT p, D3D12_GPU_VIRTUAL_ADDRESS a) { root_view<NA_DESC_SRV, 0>(l, p, a); } };
template <>
struct On<sl::list::SetComputeRootUnorderedAccessView> { static void run(GL* l, UINT p, D3D12_GPU_VIRTUAL_ADDRESS a) { root_view<NA_DESC_UAV, 1>(l, p, a); } };
template <>
struct On<sl::list::SetGraphicsRootUnorderedAccessView> { static void run(GL* l, UINT p, D3D12_GPU_VIRTUAL_ADDRESS a) { root_view<NA_DESC_UAV, 0>(l, p, a); } };
template <>
struct On<sl::list::OMSetRenderTargets> {
    static void run(GL* l, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, BOOL single, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv)
    {
        Rec r;
        if (!begin_op(l, &r)) return;
        na_view_ref refs[9];
        uint32_t m = 0;
        {
            std::shared_lock<std::shared_mutex> lock(g_m);
            for (UINT i = 0; rts && i < n && i < 8; ++i) {
                // Single range: consecutive descriptors from rts[0].
                const D3D12_CPU_DESCRIPTOR_HANDLE h = single ? D3D12_CPU_DESCRIPTOR_HANDLE{rts[0].ptr + static_cast<SIZE_T>(i) * descriptor_increment_cached(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)} : rts[i];
                uint32_t slot = 0;
                refs[m++] = view_ref_locked(heap_for_cpu_locked(h.ptr, &slot), slot);
            }
            if (dsv) {
                uint32_t slot = 0;
                refs[m++] = view_ref_locked(heap_for_cpu_locked(dsv->ptr, &slot), slot);
            }
        }
        emit_op(r, NA_OP_OM_SET_RENDER_TARGETS, {std::min<UINT>(n, 8), dsv ? 1u : 0u}, refs, m * static_cast<uint32_t>(sizeof(na_view_ref)), sizeof(na_view_ref), 0,
                NA_GROUP_RESOURCE_USE);
    }
};
template <>
struct On<sl::list::ClearDepthStencilView> {
    static void run(GL* l, D3D12_CPU_DESCRIPTOR_HANDLE h, D3D12_CLEAR_FLAGS f, FLOAT depth, UINT8 stencil, UINT, const D3D12_RECT*)
    {
        Rec r;
        if (!begin_op(l, &r)) return;
        const na_view_ref v = cpu_ref(h);
        emit_op(r, NA_OP_CLEAR_DSV, {static_cast<uint64_t>(f), float_bits(depth, 0.f), stencil}, &v, sizeof(v), sizeof(v), 0, NA_GROUP_RESOURCE_USE);
    }
};
template <>
struct On<sl::list::ClearRenderTargetView> {
    static void run(GL* l, D3D12_CPU_DESCRIPTOR_HANDLE h, const FLOAT* c, UINT, const D3D12_RECT*)
    {
        Rec r;
        if (!begin_op(l, &r)) return;
        const na_view_ref v = cpu_ref(h);
        const float z[4] = {0, 0, 0, 0};
        const float* cc = c ? c : z;
        emit_op(r, NA_OP_CLEAR_RTV, {float_bits(cc[0], cc[1]), float_bits(cc[2], cc[3])}, &v, sizeof(v), sizeof(v), 0, NA_GROUP_RESOURCE_USE);
    }
};
template <>
struct On<sl::list::ExecuteIndirect> {
    static void run(GL* l, ID3D12CommandSignature*, UINT max, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64)
    {
        Rec r;
        if (begin_op(l, &r)) emit_op(r, NA_OP_EXECUTE_INDIRECT, {max}, nullptr, 0, 0, NA_EVF_METHOD_UNOBSERVED);
    }
};

// Thunk pools (variant registry, A48/A49): one detour per implementation variant.
constexpr uint32_t kThunks = 4;
template <uint32_t Slot, typename Sig>
struct ListHook;
template <uint32_t Slot, typename R, typename... A>
struct ListHook<Slot, R(A...)> {
    using Pfn = R(STDMETHODCALLTYPE*)(GL*, A...);
    static inline void* orig[kThunks] = {};
    template <uint32_t I>
    static R STDMETHODCALLTYPE thunk(GL* l, A... a)
    {
        na::hooking::variants::CallScope scope(l, Slot);
        auto fn = reinterpret_cast<Pfn>(orig[I]);
        if constexpr (std::is_void_v<R>) {
            fn(l, a...);
            if (scope.outermost && !scope.ambiguous) On<Slot>::run(l, a...);
        } else {
            const R r = fn(l, a...);
            if (scope.outermost && !scope.ambiguous) On<Slot>::run(l, r, a...);
            return r;
        }
    }
    static inline void* thunks[kThunks] = {reinterpret_cast<void*>(&thunk<0>), reinterpret_cast<void*>(&thunk<1>),
                                           reinterpret_cast<void*>(&thunk<2>), reinterpret_cast<void*>(&thunk<3>)};
    static na::hooking::variants::SlotSpec spec() { return {Slot, kThunks, thunks, orig}; }
};

bool register_list_slots()
{
    namespace L = sl::list;
    const na::hooking::variants::SlotSpec specs[] = {
        ListHook<L::Close, HRESULT()>::spec(),
        ListHook<L::Reset, HRESULT(ID3D12CommandAllocator*, ID3D12PipelineState*)>::spec(),
        ListHook<L::DrawInstanced, void(UINT, UINT, UINT, UINT)>::spec(),
        ListHook<L::DrawIndexedInstanced, void(UINT, UINT, UINT, INT, UINT)>::spec(),
        ListHook<L::Dispatch, void(UINT, UINT, UINT)>::spec(),
        ListHook<L::CopyBufferRegion, void(ID3D12Resource*, UINT64, ID3D12Resource*, UINT64, UINT64)>::spec(),
        ListHook<L::CopyTextureRegion, void(const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT, const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*)>::spec(),
        ListHook<L::CopyResource, void(ID3D12Resource*, ID3D12Resource*)>::spec(),
        ListHook<L::ResolveSubresource, void(ID3D12Resource*, UINT, ID3D12Resource*, UINT, DXGI_FORMAT)>::spec(),
        ListHook<L::RSSetViewports, void(UINT, const D3D12_VIEWPORT*)>::spec(),
        ListHook<L::RSSetScissorRects, void(UINT, const D3D12_RECT*)>::spec(),
        ListHook<L::SetPipelineState, void(ID3D12PipelineState*)>::spec(),
        ListHook<L::ResourceBarrier, void(UINT, const D3D12_RESOURCE_BARRIER*)>::spec(),
        ListHook<L::ExecuteBundle, void(ID3D12GraphicsCommandList*)>::spec(),
        ListHook<L::SetDescriptorHeaps, void(UINT, ID3D12DescriptorHeap* const*)>::spec(),
        ListHook<L::SetComputeRootSignature, void(ID3D12RootSignature*)>::spec(),
        ListHook<L::SetGraphicsRootSignature, void(ID3D12RootSignature*)>::spec(),
        ListHook<L::SetComputeRootDescriptorTable, void(UINT, D3D12_GPU_DESCRIPTOR_HANDLE)>::spec(),
        ListHook<L::SetGraphicsRootDescriptorTable, void(UINT, D3D12_GPU_DESCRIPTOR_HANDLE)>::spec(),
        ListHook<L::SetComputeRoot32BitConstant, void(UINT, UINT, UINT)>::spec(),
        ListHook<L::SetGraphicsRoot32BitConstant, void(UINT, UINT, UINT)>::spec(),
        ListHook<L::SetComputeRoot32BitConstants, void(UINT, UINT, const void*, UINT)>::spec(),
        ListHook<L::SetGraphicsRoot32BitConstants, void(UINT, UINT, const void*, UINT)>::spec(),
        ListHook<L::SetComputeRootConstantBufferView, void(UINT, D3D12_GPU_VIRTUAL_ADDRESS)>::spec(),
        ListHook<L::SetGraphicsRootConstantBufferView, void(UINT, D3D12_GPU_VIRTUAL_ADDRESS)>::spec(),
        ListHook<L::SetComputeRootShaderResourceView, void(UINT, D3D12_GPU_VIRTUAL_ADDRESS)>::spec(),
        ListHook<L::SetGraphicsRootShaderResourceView, void(UINT, D3D12_GPU_VIRTUAL_ADDRESS)>::spec(),
        ListHook<L::SetComputeRootUnorderedAccessView, void(UINT, D3D12_GPU_VIRTUAL_ADDRESS)>::spec(),
        ListHook<L::SetGraphicsRootUnorderedAccessView, void(UINT, D3D12_GPU_VIRTUAL_ADDRESS)>::spec(),
        ListHook<L::OMSetRenderTargets, void(UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*)>::spec(),
        ListHook<L::ClearDepthStencilView, void(D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT*)>::spec(),
        ListHook<L::ClearRenderTargetView, void(D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT*, UINT, const D3D12_RECT*)>::spec(),
        ListHook<L::ExecuteIndirect, void(ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64)>::spec(),
    };
    return na::hooking::variants::Register(na::hooking::variants::Kind::CommandList, specs,
                                           static_cast<uint32_t>(sizeof(specs) / sizeof(specs[0])));
}

std::once_flag g_install_once;
bool g_install_ok = false;
bool g_lists_hooked = false;

std::vector<void*> g_targets; // install thread only
char g_shared_report[256] = {};

// Vtable-ENTRY patching (not function patching): the device and queue
// vtables are static tables in the runtime, shared by every object of the
// implementation. Replacing the entry leaves the runtime's code untouched
// (WARP crashed in an unhooked method after prologue patching of its
// neighbours) and keeps the runtime's internal direct calls unobserved,
// which is exactly the game-facing surface H1 describes.
bool patch_entry(void** entry, void* detour, void** orig)
{
    DWORD old = 0;
    if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *orig = *entry;
    InterlockedExchangePointer(entry, detour);
    DWORD ignored = 0;
    VirtualProtect(entry, sizeof(void*), old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), entry, sizeof(void*));
    return true;
}

bool hook(void* object, uint32_t slot, void* detour, void** orig, bool required)
{
    if (!object) {
        if (required) return false;
        g_c.missing.fetch_add(1);
        return true;
    }
    void** entry = &(*static_cast<void***>(object))[slot];
    if (*entry == detour) return true; // already ours
    // Two of our slots resolving to ONE function would be fine for entry
    // patching, but means the runtime treats them alike: report it.
    if (std::find(g_targets.begin(), g_targets.end(), *entry) != g_targets.end()) {
        const size_t len = std::strlen(g_shared_report);
        std::snprintf(g_shared_report + len, sizeof(g_shared_report) - len, "%u ", slot);
    }
    g_targets.push_back(*entry);
    if (patch_entry(entry, detour, orig)) {
        g_c.hooks.fetch_add(1);
        return true;
    }
    if (!required) g_c.missing.fetch_add(1);
    return !required;
}

template <typename F>
void* fn(F f) { return reinterpret_cast<void*>(f); }

} // namespace

bool InstallProducers(ID3D12Device* device, ID3D12CommandQueue* queue, bool hook_queue_execute)
{
    std::call_once(g_install_once, [&] {
        if (!device || !queue) return;
        for (uint32_t ty = 0; ty < D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES; ++ty)
            descriptor_increment(device, static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(ty));
        ID3D12Device2* d2 = nullptr;
        device->QueryInterface(__uuidof(ID3D12Device2), reinterpret_cast<void**>(&d2));
        ID3D12Device4* d4 = nullptr;
        ID3D12Device8* d8 = nullptr;
        device->QueryInterface(__uuidof(ID3D12Device4), reinterpret_cast<void**>(&d4));
        device->QueryInterface(__uuidof(ID3D12Device8), reinterpret_cast<void**>(&d8));
#define NA_HOOK(obj, ns, name, req) hook(obj, sl::ns::name, fn(&D_##name), reinterpret_cast<void**>(&o_##name), req)
        bool ok = NA_HOOK(device, device, CreateCommandQueue, true) &&
                  NA_HOOK(device, device, CreateCommandList, true) && NA_HOOK(device, device, CreateDescriptorHeap, true) &&
                  NA_HOOK(device, device, CreateConstantBufferView, true) && NA_HOOK(device, device, CreateShaderResourceView, true) &&
                  NA_HOOK(device, device, CreateUnorderedAccessView, true) && NA_HOOK(device, device, CreateRenderTargetView, true) &&
                  NA_HOOK(device, device, CreateDepthStencilView, true) && NA_HOOK(device, device, CreateSampler, true) &&
                  NA_HOOK(device, device, CopyDescriptors, true) && NA_HOOK(device, device, CopyDescriptorsSimple, true) &&
                  NA_HOOK(device, device, CreateCommittedResource, true) && NA_HOOK(device, device, CreateHeap, true) &&
                  NA_HOOK(device, device, CreatePlacedResource, true) && NA_HOOK(device, device, CreateReservedResource, true) &&
                  NA_HOOK(device, device, CreateFence, true) &&
                  NA_HOOK(device, device, CreateRootSignature, true) && NA_HOOK(device, device, CreateGraphicsPipelineState, true) &&
                  NA_HOOK(device, device, CreateComputePipelineState, true) && NA_HOOK(d2, device, CreatePipelineState, false) &&
                  NA_HOOK(d4, device, CreateCommandList1, false) && NA_HOOK(d4, device, CreateCommittedResource1, false) &&
                  NA_HOOK(d4, device, CreateHeap1, false) && NA_HOOK(d4, device, CreateReservedResource1, false) &&
                  NA_HOOK(d8, device, CreateCommittedResource2, false) && NA_HOOK(d8, device, CreatePlacedResource1, false) &&
                  NA_HOOK(queue, queue, Signal, true) && NA_HOOK(queue, queue, Wait, true);
        if (ok && hook_queue_execute) ok = NA_HOOK(queue, queue, ExecuteCommandLists, true);
#undef NA_HOOK
        if (d2) d2->Release();
        if (d4) d4->Release();
        if (d8) d8->Release();
        // C8b: command-list methods live in per-implementation (often per-list)
        // tables; the variant registry hooks their concrete functions.
        if (ok) {
            const bool reg = na::hooking::Initialize() && register_list_slots();
            if (reg) {
                na::hooking::variants::WarmUpCommandLists(device);
                na::hooking::variants::StartDiscoveryThread(nullptr, nullptr);
            }
            g_lists_hooked = reg;
        }
        if (ok) na::observe_internal::SetSnapshotProvider(&snapshot);
        g_install_ok = ok;
    });
    return g_install_ok;
}

ExecuteToken ExecuteBegin(ID3D12CommandQueue* queue)
{
    ExecuteToken t;
    na_object_id qid = 0;
    auto* qs = queue ? queue_state(queue, &qid) : nullptr;
    if (!qs) return t;
    t.queue_state = qs;
    t.ticket = qs->ticket.fetch_add(1) + 1;
    if (qs->inflight.fetch_add(1) > 0) {
        t.overlapped_at_entry = true;
        qs->overlap_gen.fetch_add(1);
    }
    t.overlap_gen = qs->overlap_gen.load();
    return t;
}

void ExecuteEnd(const ExecuteToken& t, ID3D12CommandQueue* queue, uint32_t count, ID3D12CommandList* const* lists)
{
    auto* qs = static_cast<QueueState*>(t.queue_state);
    if (!qs) return;
    // R3: ambiguous if another occurrence entered during our window, or we
    // entered while another was inside.
    const bool ambiguous = t.overlapped_at_entry || qs->overlap_gen.load() != t.overlap_gen;
    if (g_lists_hooked && lists) // late discovery of list tables (A48/A49)
        for (uint32_t i = 0; i < count; ++i)
            if (lists[i]) na::hooking::variants::NoteObject(na::hooking::variants::Kind::CommandList, lists[i]);
    qs->inflight.fetch_sub(1);
    g_c.executes.fetch_add(1);
    if (ambiguous) g_c.ambiguous.fetch_add(1);

    constexpr uint32_t kMaxRefs = 64;
    struct Exec {
        na_ev_queue_execute e;
        na_list_ref refs[kMaxRefs];
    } ex;
    const uint32_t n = std::min<uint32_t>(lists ? count : 0, kMaxRefs);
    std::memset(&ex, 0, sizeof(ex));
    ex.e.hdr.size_bytes = static_cast<uint32_t>(offsetof(Exec, refs) + n * sizeof(na_list_ref));
    ex.e.hdr.kind = NA_EV_QUEUE_EXECUTE;
    ex.e.hdr.thread_seq = ++t_seq;
    ex.e.hdr.producer_thread = GetCurrentThreadId();
    ex.e.hdr.flags = (ambiguous ? NA_EVF_ORDER_AMBIGUOUS : 0u) | (n < count ? NA_EVF_TRUNCATED : 0u);
    na_object_id qid = 0;
    queue_state(queue, &qid);
    ex.e.queue = qid;
    ex.e.queue_ticket = t.ticket;
    ex.e.list_count = n;
    ex.e.refs_offset = offsetof(Exec, refs);
    ex.e.ref_stride = sizeof(na_list_ref);
    {
        std::shared_lock<std::shared_mutex> lock(g_m);
        for (uint32_t i = 0; i < n; ++i) {
            auto it = g_objs.find(lists[i]);
            if (it == g_objs.end()) {
                ex.refs[i].list = 0;
                ex.refs[i].recording_generation = NA_UNKNOWN_U64;
                ex.e.hdr.flags |= NA_EVF_METHOD_UNOBSERVED;
                continue;
            }
            ex.refs[i].list = it->second.id;
            const ListState* ls = it->second.list.get();
            ex.refs[i].recording_generation = (ls && ls->generation_known.load()) ? ls->generation.load() : NA_UNKNOWN_U64;
            if (ls && it->second.created.size() >= sizeof(na_ev_list_created) &&
                reinterpret_cast<const na_ev_list_created*>(it->second.created.data())->is_bundle)
                ex.e.contains_bundles = 1;
        }
    }
    na::observe_internal::Emit(&ex, NA_GROUP_QUEUE);
}

na_object_id IdOf(const void* object) { return lookup(object); }

bool ResolveGpuVa(uint64_t va, na_object_id* resource, uint64_t* offset)
{
    if (!resource || !offset) return false;
    std::shared_lock<std::shared_mutex> lock(g_m);
    return resolve_va_locked(va, resource, offset);
}

ProducerStats GetProducerStats()
{
    ProducerStats s{};
    s.objects_live = g_c.live.load();
    s.objects_created = g_c.created.load();
    s.objects_retired = g_c.retired.load();
    s.descriptor_writes = g_c.writes.load();
    s.descriptor_copies = g_c.copies.load();
    s.unresolved_handles = g_c.unresolved.load();
    s.sentinel_failures = g_c.sentinel_fail.load();
    s.executes = g_c.executes.load();
    s.ambiguous_executes = g_c.ambiguous.load();
    s.hooks_installed = g_c.hooks.load();
    s.hooks_missing = g_c.missing.load();
    s.shared_target_slots = g_shared_report;
    return s;
}

} // namespace na::observe
