// C8a: H1 observation producers. See producers.hpp.
#include "host_dx12/observe/producers.hpp"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include "queue_occurrences.hpp"
#include "host_dx12/j2/insertion_trace.hpp"
#include <tuple>
#include <algorithm>
#include <system_error>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <initializer_list>
#include <type_traits>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "host_dx12/hooking/variant_registry.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
#include "host_dx12/host_observe_internal.hpp"
#include "host_dx12/host_observe_samples.h"
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
    QueueOccurrences occurrences;
    // C9B-R3: while a sample is armed, each observed submission on this queue
    // holds this from ExecuteBegin to the end of the host copy that may follow.
    std::mutex submit_m;
};

struct ListState {
    std::atomic<uint64_t> generation{0};
    std::atomic<bool> generation_known{false};
    std::atomic<uint64_t> op_index{0};
    std::atomic<bool> recording_observed{false}; // this recording began under observation
    std::atomic<bool> coverage_complete{false};  // the list's current table is fully hooked
    // C9: constant-data bindings of the current recording, snapshotted at submission.
    struct Binding {
        uint32_t kind = 0, param = 0, compute = 0, size = 0;
        uint64_t op_index = 0, offset = 0;
        na_object_id resource = 0; // 0 = unresolved
        bool range_truncated = false; // the table range had more descriptors than were expanded
    };
    std::mutex bind_m;
    std::vector<Binding> bindings;
    const void* root_sig[2] = {nullptr, nullptr}; // graphics, compute (recording thread)
    // C9b: explicit transitions recorded in this recording, in order (under bind_m).
    struct StateNote {
        na_object_id resource;
        uint32_t subresource; // D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES or one
        uint32_t state_after;
    };
    std::vector<StateNote> states;
    bool evidence_complete = true; // C9B-R4: no truncated batch / overflow in this recording (under bind_m)
    // G3-06 review: resources this recording references (render/depth targets,
    // clears, copies, resolves, barriers), under bind_m. A carried state is
    // only used for a resource the submitted work references (the application
    // must keep it alive until that work runs); copy-queue submissions decay
    // every resource they reference. Bounded; overflow clears uses_complete.
    std::vector<na_object_id> uses;
    bool uses_complete = true;
    // (render target, depth target or 0) per OMSetRenderTargets in this recording, in order (under bind_m).
    struct Target {
        na_object_id rt = 0, ds = 0;
        uint32_t rt_sub = 0; // the render-target view's first subresource (mip + slice); NA_UNKNOWN_U32 if not one slice
        uint32_t ds_sub = 0; // the depth view's first subresource, likewise
        bool same(na_object_id r, na_object_id d) const { return rt == r && ds == d; }
    };
    std::vector<Target> targets;
    na::j2::InsertionTrace insertion;
    // Explicit bounded SRV-table observations are a pass-order hint, not proof
    // of a particular AA shader or a requirement for DLAA eligibility.
    std::vector<na_object_id> sampled_textures;
    std::vector<na_object_id> cleared; // render targets this recording cleared (ClearRenderTargetView)
    // Inline insertion (J2): the game's pipeline state in this recording, so host
    // work recorded INTO the game's list can hand the list back exactly as the game
    // left it. Written by the recording thread's observers (host calls are not
    // observed), under bind_m.
    struct RootArg {
        uint8_t type = 0; // 0 unset, 1 table, 2 CBV, 3 SRV, 4 UAV, 5 constants
        uint64_t value = 0;
        std::vector<uint32_t> consts;
    };
    ID3D12PipelineState* pso = nullptr;
    ID3D12DescriptorHeap* heaps[2] = {nullptr, nullptr};
    uint32_t nheaps = 0;
    std::vector<RootArg> args[2]; // [graphics, compute], by root parameter
    na_object_id cur_rt = 0, cur_ds = 0; // currently bound first render target / depth
    na_object_id cur_rts[8] = {};
    uint32_t cur_nrt = 0;
    uint32_t cur_ds_sub = NA_UNKNOWN_U32;
    bool restorable = true;              // false after work whose state effects we cannot replay
    bool in_render_pass = false;         // between BeginRenderPass and EndRenderPass (no host copy/dispatch allowed)
    int8_t mip_level = -1;               // live mip bias: twin level latched at Reset (-1 = the game's own objects)
};

struct Obj {
    na_object_id id = 0;
    uint32_t kind = 0;
    std::vector<uint8_t> created;          // creation event, replayed by the snapshot
    std::shared_ptr<QueueState> queue;     // queues
    std::unique_ptr<ListState> list;       // command lists
    std::vector<na_root_param> root_params; // root signatures (C9 table resolution)
    std::vector<na_root_range> root_ranges;
    uint64_t root_hash = 0;                 // structural layout hash (role classes; stable across sessions)
    // Live mip bias: per-level twins of a root signature / pipeline state (index = level).
    // same[i]: the game's own object is already correct for level i (nothing to swap).
    struct Twins { IUnknown* obj[3] = {}; bool same[3] = {}; ~Twins() { for (auto* o : obj) if (o) o->Release(); } };
    std::shared_ptr<Twins> twins;
    bool retirement_unobservable = false;   // the sentinel could not be attached
    uint64_t present_ordinal = 0;
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
        uint64_t cbv_offset = 0; // CBV: byte offset in `resource`
        uint32_t cbv_size = 0;   // CBV: SizeInBytes
    };
    std::vector<SlotInfo> info; // last observed write per slot (record-time CPU view)
};

struct VaRange {
    uint64_t begin = 0, end = 0; // [begin, end)
    na_object_id resource = 0;
    const void* key = nullptr;
};

std::shared_mutex g_m; // registry, heaps, VA table. Never held across a D3D12 call.
// Never destroyed: at process exit an entry's twins must not be Released (a twin's last
// Release can retire an object through its sentinel, which takes g_m and walks this map).
std::unordered_map<const void*, Obj>& g_objs = *new std::unordered_map<const void*, Obj>();
std::vector<std::unique_ptr<DescHeap>> g_heaps;
std::vector<VaRange> g_va; // sorted by begin
struct BufferInfo {
    uint32_t heap_type = NA_UNKNOWN_U32;
    uint64_t width = 0;
    uint8_t* cpu = nullptr; // live CPU mapping of subresource 0 (Map/Unmap observed)
    uint32_t maps = 0;
    // G3-02: CPU-visible memory (UPLOAD, or CUSTOM with a CPU page property),
    // and whether the application has mapped it (or it predates observation).
    bool cpu_visible = false;
    bool ever_mapped = false;
};
// UPLOAD, or a CUSTOM heap the CPU can address (WRITE_COMBINE / WRITE_BACK).
bool cpu_visible_heap(const D3D12_HEAP_PROPERTIES& p)
{
    return p.Type == D3D12_HEAP_TYPE_UPLOAD ||
           (p.Type == D3D12_HEAP_TYPE_CUSTOM && (p.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE ||
                                                 p.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK));
}
std::unordered_map<na_object_id, BufferInfo> g_buffers; // under g_m
std::unordered_map<na_object_id, const void*> g_res_keys; // resource id -> canonical object (C9b), under g_m
std::atomic<uint64_t> g_next_id{1};

struct Counters {
    std::atomic<uint64_t> nested_unlocked{0};
    std::atomic<uint64_t> snap_reread{0}, preexisting_resources{0}, preexisting_heaps{0}, sample_known_state{0}, known_clears{0};
    std::atomic<uint64_t> sample_missing_recording{0}, sample_partial_coverage{0}, sample_partial_states{0}, sample_no_transition{0};
    std::atomic<uint64_t> live{0}, created{0}, retired{0}, writes{0}, copies{0}, unresolved{0}, sentinel_fail{0},
        executes{0}, ambiguous{0};
    std::atomic<uint32_t> hooks{0}, missing{0};
} g_c;

std::atomic<uint64_t> g_unhooked_devices{0}, g_unhooked_queues{0};
std::atomic<bool> g_known_disabled{false}; // a submission could not be observed: carried states are never trusted

// ---------------------------------------------- G3-06: known resource states
// Explicit transition state carried across submissions, so a sample does not
// need a barrier in the very submission it follows. A wrong StateBefore in the
// host's copy barrier would be invalid GPU work, so a record is used only when
// it is provably current (Gate 3 review, PR40 items 1-4):
//  - Eligibility: committed, non-simultaneous-access textures registered at
//    creation. Placed/reserved (aliasable, so no wildcard aliasing barrier can
//    reach the store), buffers (implicit decay) and resources first met after
//    installation never get an entry. The store is capped; a texture that
//    does not fit is simply not eligible (fail-safe).
//  - Evidence is captured from the recordings BEFORE the submission is
//    forwarded (a list may be reset as soon as ExecuteCommandLists returns).
//  - While a submission is in flight (Begin..End), every resource it
//    transitions (and, on a copy queue, every resource it references) is
//    marked; a marked record is never read by another submission.
//  - Publication at End applies the captured transitions only when this
//    submission was the sole marker of that resource from its Begin to its
//    End, nothing order-ambiguous happened and no store clear intervened.
//    Otherwise the record becomes unknown. Copy-queue submissions decay every
//    resource they reference to COMMON (unknown).
//  - A submission whose recordings are incomplete may transition anything:
//    the store is reset at its Begin and nothing is readable or publishable
//    until it ends.
//  - Any unobservable submission path (an unhooked queue or device, a queue
//    that could not be registered) disables the store for the process.
//  - Records belong to the queue that last transitioned them; another queue
//    never reads them (cross-queue order needs fences we do not track). A
//    partial transition on a second queue makes the other subresources unknown.
// Pending split barriers (BEGIN_ONLY), aliased resources and COMMON are never usable.
constexpr uint32_t kStatePending = 0xFFFFFFFFu;
struct ResKnown {
    uint32_t in_flight = 0; // submissions between Begin and End that mark it
    uint64_t mark_seq = 0;  // incremented on every mark
    na_object_id queue = 0; // last queue that transitioned it; 0 = creation state only
    bool has_all = false;
    uint32_t all = kStatePending;
    std::unordered_map<uint32_t, uint32_t> subs;
};
constexpr size_t kMaxKnown = 65536, kMaxKnownSubs = 64;
std::mutex g_known_m;
std::unordered_map<na_object_id, ResKnown> g_known; // under g_known_m (never taken before g_m or g_samples_m)
uint64_t g_known_epoch = 0;     // under g_known_m: incremented by every store reset
uint32_t g_unknown_inflight = 0; // under g_known_m: incomplete submissions between Begin and End

void known_created(na_object_id id, uint32_t state)
{
    std::lock_guard<std::mutex> lock(g_known_m);
    if (g_known.size() >= kMaxKnown && !g_known.count(id)) return; // not eligible
    ResKnown k;
    k.has_all = true;
    k.all = state;
    g_known[id] = std::move(k);
}
void known_forget(na_object_id id)
{
    std::lock_guard<std::mutex> lock(g_known_m);
    g_known.erase(id);
}
void known_reset_locked(ResKnown& k)
{
    k.queue = 0;
    k.has_all = false;
    k.all = kStatePending;
    k.subs.clear();
}
// Entries stay (eligibility and in-flight marks are kept); states become unknown.
void known_clear_locked()
{
    for (auto& kv : g_known) known_reset_locked(kv.second);
    ++g_known_epoch;
    g_c.known_clears.fetch_add(1);
}
// self_marks: marks the asking submission itself holds on this resource.
bool known_state(na_object_id res, uint32_t sub, na_object_id qid, uint32_t self_marks, uint32_t* state)
{
    // An unhooked queue's submissions are unobserved. (An unhooked device only
    // leaves resources unregistered, and unregistered resources are never eligible.)
    if (g_known_disabled.load() || g_unhooked_queues.load()) return false;
    std::lock_guard<std::mutex> lock(g_known_m);
    if (g_unknown_inflight) return false;
    auto it = g_known.find(res);
    if (it == g_known.end() || it->second.in_flight > self_marks || (it->second.queue && it->second.queue != qid)) return false;
    auto s = it->second.subs.find(sub);
    const uint32_t v = s != it->second.subs.end() ? s->second : (it->second.has_all ? it->second.all : kStatePending);
    if (v == kStatePending || v == D3D12_RESOURCE_STATE_COMMON) return false;
    *state = v;
    return true;
}

thread_local uint64_t t_seq = 0;
// C9b: set while the host records/submits its OWN sample copies, so none of
// that self-generated work is reported as the game's.
thread_local bool t_host_work = false;
// The submission this thread is forwarding (ExecuteBegin..ExecuteEnd), for
// host work done inside that forward (J2) that asks for carried state.
struct SubmissionCapture;
thread_local const SubmissionCapture* t_cur_cap = nullptr;
thread_local na_object_id t_cur_qid = 0;
// ASTRA-P3-R8: number of submission locks this thread holds, i.e. whether it is
// inside a forwarded game submission. Any observed queue operation nested in
// one (driver/runtime re-entry, on ANY queue) takes no lock, so two threads
// re-entering each other's queues can never deadlock.
thread_local int t_submit_depth = 0;
// Copy ordering window (ASTRA-P3-R8 follow-up, per queue): from the start of
// the evidence submission until the host copy is submitted, no other operation
// may start on that queue. Unlocked operations (nested re-entry from another
// thread's forwarded submission, or a submitter that began before any sample
// was armed) are detected because every observed operation begins an
// occurrence and takes a ticket. Once the copy is submitted, the queue's FIFO
// order puts every later operation (including application fence signals)
// after it, so later operations cannot invalidate it.
thread_local QueueState* t_service_qs = nullptr;
thread_local uint64_t t_service_ticket = 0;
bool ordering_window_intact()
{
    return t_service_qs && t_service_qs->occurrences.LastTicket() == t_service_ticket;
}

std::atomic<bool> g_lists_hooked{false};
void** g_device_table = nullptr; // the device table whose entries are ours
void** g_queue_table = nullptr;

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

// Canonical COM identity (review: interface pointers of one object may differ
// under wrapping layers): the registry is keyed by the object's IUnknown.
// A one-entry per-thread cache keeps the hot recording path cheap; any
// retirement invalidates every cache (an address may be reused afterwards).
std::atomic<uint64_t> g_retire_gen{1};
const void* canon(const void* p)
{
    if (!p) return nullptr;
    thread_local const void* c_in = nullptr;
    thread_local const void* c_out = nullptr;
    thread_local uint64_t c_gen = 0;
    const uint64_t gen = g_retire_gen.load(std::memory_order_acquire);
    if (p == c_in && gen == c_gen) return c_out;
    IUnknown* u = nullptr;
    auto* obj = static_cast<IUnknown*>(const_cast<void*>(p));
    const void* out = p;
    if (SUCCEEDED(obj->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&u))) && u) {
        out = u;
        u->Release(); // the caller still holds the object
    }
    c_in = p;
    c_out = out;
    c_gen = gen;
    return out;
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
    g_retire_gen.fetch_add(1, std::memory_order_acq_rel);
    std::shared_ptr<Obj::Twins> drop; // released after g_m: a twin's Release is a D3D12 call
    {
        std::unique_lock<std::shared_mutex> lock(g_m);
        auto it = g_objs.find(key);
        if (it != g_objs.end() && it->second.id == id) { drop = std::move(it->second.twins); g_objs.erase(it); }
        g_heaps.erase(std::remove_if(g_heaps.begin(), g_heaps.end(), [&](const std::unique_ptr<DescHeap>& h) { return h->id == id; }),
                      g_heaps.end());
        g_va.erase(std::remove_if(g_va.begin(), g_va.end(), [&](const VaRange& r) { return r.resource == id; }), g_va.end());
        g_buffers.erase(id);
        g_res_keys.erase(id);
    }
    if (kind == NA_OBJ_RESOURCE) known_forget(id);
    g_c.live.fetch_sub(1);
    g_c.retired.fetch_add(1);
    na_ev_object_retired e;
    init_header(e, NA_EV_OBJECT_RETIRED, NA_EVF_FOUNDATIONAL);
    e.object = id;
    e.object_kind = kind;
    emit(e, kind == NA_OBJ_RESOURCE ? NA_GROUP_RESOURCE : NA_GROUP_SESSION);
}

// Attaches the retirement sentinel; records when that is impossible.
void attach_sentinel(IUnknown* object, const void* key, na_object_id id, uint32_t kind)
{
    ID3D12Object* d3d = nullptr;
    bool attached = false;
    if (SUCCEEDED(object->QueryInterface(__uuidof(ID3D12Object), reinterpret_cast<void**>(&d3d))) && d3d) {
        auto* s = new RetireSentinel(key, id, kind);
        attached = SUCCEEDED(d3d->SetPrivateDataInterface(kSentinelGuid, s));
        s->Release(); // the object now holds the only reference (or it is gone)
        d3d->Release();
    }
    if (!attached) {
        IDXGIObject* dxgi = nullptr;
        if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&dxgi))) && dxgi) {
            auto* s = new RetireSentinel(key, id, kind);
            attached = SUCCEEDED(dxgi->SetPrivateDataInterface(kSentinelGuid, s));
            s->Release();
            dxgi->Release();
        }
    }
    if (attached) return;
    // Review: no retirement will ever be observed for this identity. Its
    // creation event carries METHOD_UNOBSERVED (see remember_creation).
    g_c.sentinel_fail.fetch_add(1);
    std::unique_lock<std::shared_mutex> lock(g_m);
    auto it = g_objs.find(key);
    if (it != g_objs.end() && it->second.id == id) it->second.retirement_unobservable = true;
}

Obj fresh_obj(na_object_id id, uint32_t kind)
{
    Obj o;
    o.id = id;
    o.kind = kind;
    if (kind == NA_OBJ_QUEUE) o.queue = std::make_shared<QueueState>();
    if (kind == NA_OBJ_LIST) o.list = std::make_unique<ListState>();
    return o;
}

// Registers a NEWLY CREATED object (a reused address replaces the stale entry).
na_object_id register_object(IUnknown* object, uint32_t kind)
{
    if (!object) return 0;
    const void* key = canon(object);
    const na_object_id id = g_next_id.fetch_add(1);
    std::shared_ptr<Obj::Twins> drop; // stale entry's twins, released after g_m
    {
        std::unique_lock<std::shared_mutex> lock(g_m);
        Obj& o = g_objs[key];
        if (o.id) g_c.live.fetch_sub(1); // address reused without a retirement we saw
        drop = std::move(o.twins);
        o = fresh_obj(id, kind);
    }
    g_c.live.fetch_add(1);
    g_c.created.fetch_add(1);
    attach_sentinel(object, key, id, kind);
    return id;
}

// An object first met in use (it predates installation). Check-and-insert is
// one critical section, so two threads meeting it at once get ONE identity;
// *created tells the single winner to emit its PREEXISTING event.
na_object_id ensure_registered(IUnknown* object, uint32_t kind, bool* created)
{
    *created = false;
    if (!object) return 0;
    const void* key = canon(object);
    na_object_id id = 0;
    std::shared_ptr<Obj::Twins> drop; // stale entry's twins, released after g_m
    {
        std::unique_lock<std::shared_mutex> lock(g_m);
        auto it = g_objs.find(key);
        if (it != g_objs.end() && it->second.kind == kind) return it->second.id;
        id = g_next_id.fetch_add(1);
        if (it != g_objs.end()) { g_c.live.fetch_sub(1); drop = std::move(it->second.twins); }
        g_objs[key] = fresh_obj(id, kind);
    }
    *created = true;
    g_c.live.fetch_add(1);
    g_c.created.fetch_add(1);
    attach_sentinel(object, key, id, kind);
    return id;
}

template <typename E>
void remember_creation(const void* object, E& e)
{
    const void* key = canon(object);
    std::unique_lock<std::shared_mutex> lock(g_m);
    auto it = g_objs.find(key);
    if (it == g_objs.end()) return;
    if (it->second.retirement_unobservable) e.hdr.flags |= NA_EVF_METHOD_UNOBSERVED;
    const auto* b = reinterpret_cast<const uint8_t*>(&e);
    it->second.created.assign(b, b + sizeof(E));
}

na_object_id id_locked(const void* object)
{
    auto it = g_objs.find(canon(object));
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
    bool created = false;
    const na_object_id id = ensure_registered(dev, NA_OBJ_DEVICE, &created);
    if (!created) return id;
    if (g_device_table && *reinterpret_cast<void***>(dev) != g_device_table) g_unhooked_devices.fetch_add(1);
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
    if (t_host_work) return nullptr; // host-owned sample objects are not the game's
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
    bool cpu_visible = false;
    if (props) {
        heap_type = props->Type;
        cpu_visible = cpu_visible_heap(*props);
    } else if (heap) {
        const D3D12_HEAP_PROPERTIES hp = heap->GetDesc().Properties;
        heap_type = hp.Type;
        cpu_visible = cpu_visible_heap(hp);
    }
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
    {
        std::unique_lock<std::shared_mutex> lock(g_m);
        g_res_keys[id] = canon(obj);
    }
    if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        std::unique_lock<std::shared_mutex> lock(g_m);
        BufferInfo& bi = g_buffers[id];
        bi.heap_type = heap_type;
        bi.width = d.Width;
        bi.cpu_visible = cpu_visible;
    }
    // G3-06: committed, non-simultaneous textures are eligible for carried
    // state from creation (a COMMON creation state is recorded but never
    // usable until an explicit transition). Placed/reserved resources alias
    // and are never eligible; simultaneous-access textures decay.
    if (creation_kind == NA_CREATE_COMMITTED && d.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
        !(d.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS))
        known_created(id, static_cast<uint32_t>(initial));
    if (va) {
        std::unique_lock<std::shared_mutex> lock(g_m);
        VaRange r{va, va + d.Width, id, obj};
        g_va.insert(std::upper_bound(g_va.begin(), g_va.end(), r, [](const VaRange& a, const VaRange& b) { return a.begin < b.begin; }), r);
    }
    emit(e, NA_GROUP_RESOURCE);
}

// G3-03: a resource created before the producers were installed, first met in
// a recorded barrier or copy. The object is live (the application just passed
// it), so it is registered PREEXISTING with what the runtime still reports;
// creation-time facts (initial state, creation kind, placement) stay UNKNOWN.
na_object_id ensure_resource_registered(ID3D12Resource* res)
{
    if (!res) return 0;
    if (const na_object_id known = lookup(res)) return known;
    if (t_host_work) return 0; // host-owned objects are never the game's
    bool created = false;
    const na_object_id id = ensure_registered(res, NA_OBJ_RESOURCE, &created);
    if (!created) return id;
    g_c.preexisting_resources.fetch_add(1);
    const D3D12_RESOURCE_DESC d = res->GetDesc();
    D3D12_HEAP_PROPERTIES hp{};
    D3D12_HEAP_FLAGS hf{};
    const bool has_heap = SUCCEEDED(res->GetHeapProperties(&hp, &hf)); // fails for reserved resources
    na_object_id dev_id = 0;
    ID3D12Device* dev = nullptr;
    if (SUCCEEDED(res->GetDevice(IID_PPV_ARGS(&dev))) && dev) {
        dev_id = device_id(dev);
        dev->Release();
    }
    const uint64_t va = d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? res->GetGPUVirtualAddress() : 0;
    na_ev_resource_created e;
    init_header(e, NA_EV_RESOURCE_CREATED, NA_EVF_FOUNDATIONAL | NA_EVF_PREEXISTING | NA_EVF_METHOD_UNOBSERVED);
    e.resource = id;
    e.device = dev_id;
    e.heap = 0;
    e.heap_offset = NA_UNKNOWN_U64;
    e.dimension = d.Dimension;
    e.format = d.Format;
    e.width = d.Width;
    e.height = d.Height;
    e.depth_or_array = d.DepthOrArraySize;
    e.mip_levels = d.MipLevels;
    e.sample_count = d.SampleDesc.Count;
    e.creation_kind = NA_UNKNOWN_U32;
    e.resource_flags = d.Flags;
    e.initial_state = NA_UNKNOWN_U32;
    e.heap_type = has_heap ? static_cast<uint32_t>(hp.Type) : NA_UNKNOWN_U32;
    remember_creation(res, e);
    {
        std::unique_lock<std::shared_mutex> lock(g_m);
        g_res_keys[id] = canon(res);
        if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            BufferInfo& bi = g_buffers[id];
            bi.heap_type = e.heap_type;
            bi.width = d.Width;
            bi.cpu_visible = has_heap && cpu_visible_heap(hp);
            bi.ever_mapped = bi.cpu_visible; // it may have been mapped before we could see it
        }
        if (va) {
            VaRange r{va, va + d.Width, id, res};
            g_va.insert(std::upper_bound(g_va.begin(), g_va.end(), r, [](const VaRange& a, const VaRange& b) { return a.begin < b.begin; }), r);
        }
    }
    emit(e, NA_GROUP_RESOURCE);
    return id;
}
// Resource id for a recorded op: registered lazily on first sight.
na_object_id resource_id(ID3D12Resource* r)
{
    if (!r) return 0;
    const na_object_id id = ensure_resource_registered(r);
    return id ? id : NA_UNKNOWN_U64;
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
        h->info[slot] = DescHeap::SlotInfo{kind, e.resource, format, first_sub, 0, 0};
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
    if (alloc && !alloc_id) {
        bool created = false;
        alloc_id = ensure_registered(alloc, NA_OBJ_ALLOCATOR, &created);
    }
    e.initial_allocator = alloc_id;
    e.list_type = type;
    e.is_bundle = type == D3D12_COMMAND_LIST_TYPE_BUNDLE ? 1u : 0u;
    e.initially_open = open ? 1u : 0u;
    {
        std::unique_lock<std::shared_mutex> lock(g_m);
        auto it = g_objs.find(canon(obj));
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

void register_heap(ID3D12Device* dev, IUnknown* obj, ID3D12DescriptorHeap* h, const D3D12_DESCRIPTOR_HEAP_DESC& d, na_object_id id, uint32_t flags);
HRESULT STDMETHODCALLTYPE D_CreateDescriptorHeap(ID3D12Device* dev, const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID riid, void** out)
{
    Depth depth("CreateDescriptorHeap");
    const HRESULT hr = o_CreateDescriptorHeap(dev, desc, riid, out);
    IUnknown* obj = depth.outermost() ? out_object(hr, out) : nullptr;
    if (!obj || !desc) return hr;
    ID3D12DescriptorHeap* h = nullptr;
    if (FAILED(obj->QueryInterface(__uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&h))) || !h) return hr;
    register_heap(dev, obj, h, *desc, register_object(obj, NA_OBJ_DESCRIPTOR_HEAP), NA_EVF_FOUNDATIONAL);
    h->Release();
    return hr;
}

// Builds the heap record and its creation event. `h` is borrowed.
void register_heap(ID3D12Device* dev, IUnknown* obj, ID3D12DescriptorHeap* h, const D3D12_DESCRIPTOR_HEAP_DESC& d, na_object_id id, uint32_t flags)
{
    const D3D12_DESCRIPTOR_HEAP_DESC* desc = &d;
    auto dh = std::make_unique<DescHeap>();
    dh->key = obj;
    dh->cpu_start = h->GetCPUDescriptorHandleForHeapStart().ptr;
    const bool visible = (desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0;
    dh->gpu_start = visible ? h->GetGPUDescriptorHandleForHeapStart().ptr : 0;
    dh->increment = descriptor_increment(dev, desc->Type);
    dh->count = desc->NumDescriptors;
    dh->type = desc->Type;
    dh->versions.assign(desc->NumDescriptors, 0);
    dh->info.assign(desc->NumDescriptors, DescHeap::SlotInfo{});
    const na_object_id dev_id = device_id(dev);
    na_ev_descriptor_heap_created e;
    init_header(e, NA_EV_DESCRIPTOR_HEAP_CREATED, flags);
    e.heap = id;
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
}

// G3-03: a shader-visible heap created before installation, first met in
// SetDescriptorHeaps. Its range becomes resolvable; descriptors written before
// installation stay UNKNOWN (their contents cannot be read back), later
// writes resolve normally.
void ensure_heap_registered(ID3D12DescriptorHeap* h)
{
    if (!h || t_host_work) return;
    {
        std::shared_lock<std::shared_mutex> lock(g_m);
        for (auto& dh : g_heaps)
            if (dh->key == h) return;
    }
    bool created = false;
    const na_object_id id = ensure_registered(h, NA_OBJ_DESCRIPTOR_HEAP, &created);
    if (!created) return;
    ID3D12Device* dev = nullptr;
    if (FAILED(h->GetDevice(IID_PPV_ARGS(&dev))) || !dev) return;
    g_c.preexisting_heaps.fetch_add(1);
    register_heap(dev, h, h, h->GetDesc(), id, NA_EVF_FOUNDATIONAL | NA_EVF_PREEXISTING | NA_EVF_METHOD_UNOBSERVED);
    dev->Release();
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
        dh->info[slot] = DescHeap::SlotInfo{NA_DESC_CBV, desc && desc->BufferLocation ? (known ? res : NA_UNKNOWN_U64) : 0, NA_UNKNOWN_U32, 0,
                                            known ? off : 0, desc ? desc->SizeInBytes : 0};
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
    else if (r && d->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2DARRAY && d->Texture2DArray.ArraySize == 1) {
        // One slice of an array: subresource = mip + slice * mip count (plane 0).
        const D3D12_RESOURCE_DESC rd = r->GetDesc();
        first = d->Texture2DArray.MipSlice + d->Texture2DArray.FirstArraySlice * rd.MipLevels;
        count = 1;
    }
    else if (d->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2DMS) { first = 0; count = 1; }
    else if (d->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY && d->Texture2DMSArray.ArraySize == 1) {
        first = d->Texture2DMSArray.FirstArraySlice; count = 1; // MSAA textures have exactly one mip
    }
    note_descriptor_write(h, NA_DESC_RTV, r, d ? static_cast<uint32_t>(d->Format) : NA_UNKNOWN_U32, first, count);
}

void STDMETHODCALLTYPE D_CreateDepthStencilView(ID3D12Device* dev, ID3D12Resource* r, const D3D12_DEPTH_STENCIL_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    o_CreateDepthStencilView(dev, r, d, h);
    uint32_t first = NA_UNKNOWN_U32, count = NA_UNKNOWN_U32;
    if (!d || d->ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2D) { first = d ? d->Texture2D.MipSlice : 0; count = 1; }
    else if (r && d->ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2DARRAY && d->Texture2DArray.ArraySize == 1) {
        const D3D12_RESOURCE_DESC rd = r->GetDesc();
        first = d->Texture2DArray.MipSlice + d->Texture2DArray.FirstArraySlice * rd.MipLevels; // plane 0
        count = 1;
    }
    else if (d->ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2DMS) { first = 0; count = 1; }
    else if (d->ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY && d->Texture2DMSArray.ArraySize == 1) {
        first = d->Texture2DMSArray.FirstArraySlice; count = 1;
    }
    note_descriptor_write(h, NA_DESC_DSV, r, d ? static_cast<uint32_t>(d->Format) : NA_UNKNOWN_U32, first, count);
}

// Texture LOD (mip) bias for temporal reconstruction (J2 DLAA): with jitter and
// accumulation, textures must be sampled one mip sharper than for a single
// frame, or every frame is already low-passed and the accumulated picture loses
// texture detail (wood planks, roof tiles). Applied to samplers that actually
// use mips (linear / anisotropic mip filtering, MaxLOD > 0); never to
// comparison (shadow) samplers or point-mip lookups. 0 = off.
std::atomic<float> g_mip_bias{0.0f};
// Live mip bias (Cort: compare without restarting). The game's static samplers are baked
// into its root signatures and pipelines at load, so every root signature and pipeline
// gets twins built at creation for the other menu levels; command lists are switched to
// the twins of the level latched at their Reset. Levels = the menu's choices.
constexpr float kMipLevels[3] = {0.0f, -0.5f, -1.0f};
std::atomic<int> g_mip_game_level{0};      // level the game's own objects are built with (0 = no bias unless J2 sets it)
std::atomic<int> g_mip_live_level{0};      // level selected in the menu
std::atomic<bool> g_mip_switched{false};   // a level other than the game's has been selected (enables lookups)
std::atomic<bool> g_mip_live_enabled{false}; // twins are built only when J2 enables live switching
std::atomic<uint64_t> g_mip_twin_rs{0}, g_mip_twin_pso{0}, g_mip_untwinned{0}, g_mip_fallbacks{0}, g_mip_twin_fail{0};
int nearest_mip_level(float b)
{
    int best = 0;
    for (int i = 1; i < 3; ++i) if (std::fabs(b - kMipLevels[i]) < std::fabs(b - kMipLevels[best])) best = i;
    return best;
}
std::atomic<uint64_t> g_biased_samplers{0}, g_biased_static{0};
template <typename SD>
bool sampler_wants_bias(const SD& d)
{
    // Standard filtering only: comparison (shadow maps) and min/max reductions
    // (depth pyramids) are data lookups, never biased.
    if (D3D12_DECODE_FILTER_REDUCTION(d.Filter) != D3D12_FILTER_REDUCTION_TYPE_STANDARD) return false;
    // Material textures: anisotropic, or linear minification (with linear OR point
    // mip selection: the bias moves the chosen mip either way). All-point samplers
    // are data/UI lookups and stay as they are. No mips (MaxLOD 0): nothing to bias.
    const bool aniso = D3D12_DECODE_IS_ANISOTROPIC_FILTER(d.Filter);
    const bool linear_min = D3D12_DECODE_MIN_FILTER(d.Filter) == D3D12_FILTER_TYPE_LINEAR;
    // Screen-space passes (bloom/blur mip chains, SSAO, depth of field, tonemap
    // LUTs) sample with CLAMP/BORDER addressing; biasing those makes the whole
    // image harsh and noisy (Sniper Elite Resistance: every sampler is static).
    // Surface textures tile (WRAP/MIRROR). So a linear sampler is biased only
    // when it tiles; anisotropic filtering is a surface-texture choice anyway.
    const auto tiles = [](D3D12_TEXTURE_ADDRESS_MODE m) {
        return m == D3D12_TEXTURE_ADDRESS_MODE_WRAP || m == D3D12_TEXTURE_ADDRESS_MODE_MIRROR || m == D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    };
    const bool surface = aniso || (linear_min && (tiles(d.AddressU) || tiles(d.AddressV)));
    return surface && d.MaxLOD > 0.0f;
}
float clamp_lod(float v) { return v < -16.0f ? -16.0f : (v > 15.99f ? 15.99f : v); }
// Target-based bias (Cort, aiming for a total of `target`, e.g. -1): a sampler
// with a neutral or positive native bias gets NVIDIA's NativeBias + target; one
// the game already biases negative is brought to `target` at most and never
// past it (no stacking -1 on top of the game's own sharpening).
float biased(float native, float target)
{
    if (target == 0.0f) return native;
    if (native >= 0.0f) return clamp_lod(native + target);
    return native < target ? native : target;
}
// The game's own biases on surface samplers, observed whether or not we bias.
std::mutex g_native_m;
uint64_t g_native_neg = 0, g_native_zero = 0, g_native_pos = 0;
float g_native_min = 0.0f, g_native_max = 0.0f;
void note_native(float b)
{
    std::lock_guard<std::mutex> lock(g_native_m);
    const bool first = !(g_native_neg || g_native_zero || g_native_pos);
    if (b < 0.0f) ++g_native_neg; else if (b > 0.0f) ++g_native_pos; else ++g_native_zero;
    if (first || b < g_native_min) g_native_min = b;
    if (first || b > g_native_max) g_native_max = b;
}

void STDMETHODCALLTYPE D_CreateSampler(ID3D12Device* dev, const D3D12_SAMPLER_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    const float bias = g_mip_bias.load(std::memory_order_relaxed);
    const bool surface = d && !t_host_work && sampler_wants_bias(*d);
    if (surface) note_native(d->MipLODBias);
    if (surface && bias != 0.0f && biased(d->MipLODBias, bias) != d->MipLODBias) {
        D3D12_SAMPLER_DESC m = *d;
        m.MipLODBias = biased(m.MipLODBias, bias);
        o_CreateSampler(dev, &m, h);
        g_biased_samplers.fetch_add(1, std::memory_order_relaxed);
    } else {
        o_CreateSampler(dev, d, h);
    }
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
std::shared_ptr<QueueState> queue_state(ID3D12CommandQueue* q, na_object_id* id)
{
    // Serialize first-use registration so two callers cannot replace the queue
    // sentinel/state before either receives its retained token.
    static std::mutex registration;
    std::lock_guard<std::mutex> first_use(registration);
    {
        std::shared_lock<std::shared_mutex> lock(g_m);
        auto it = g_objs.find(canon(q));
        if (it != g_objs.end() && it->second.queue) {
            *id = it->second.id;
            return it->second.queue;
        }
    }
    // A queue created before installation: identity from now on, flagged.
    const na_object_id qid = register_object(q, NA_OBJ_QUEUE);
    if (g_queue_table && *reinterpret_cast<void***>(q) != g_queue_table) g_unhooked_queues.fetch_add(1); // Signal/Wait unobserved
    {
        ID3D12Device* qd = nullptr;
        if (SUCCEEDED(q->GetDevice(IID_PPV_ARGS(&qd))) && qd) {
            if (g_device_table && *reinterpret_cast<void***>(qd) != g_device_table) g_unhooked_devices.fetch_add(1);
            qd->Release();
        }
    }
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
    auto it = g_objs.find(canon(q));
    *id = qid;
    return (it != g_objs.end() && it->second.queue) ? it->second.queue : nullptr;
}

// All queue operation kinds share the same entry/exit linearization.
void unlock_submission(const ExecuteToken& t)
{
    if (!t.submit_locked) return;
    if (auto qs = std::static_pointer_cast<QueueState>(t.queue_state)) {
        --t_submit_depth;
        qs->submit_m.unlock();
    }
}
bool end_occurrence(const ExecuteToken& t)
{
    auto qs = std::static_pointer_cast<QueueState>(t.queue_state);
    const bool overlap = !qs || qs->occurrences.End({t.ticket,t.overlap_gen,t.overlapped_at_entry});
    return overlap || t.nested_unlocked; // a nested occurrence bypassed serialization
}
void sync_event(uint32_t kind, const ExecuteToken& t, ID3D12Fence* f, UINT64 value, HRESULT hr)
{
    const bool ambiguous = end_occurrence(t);
    na_ev_queue_sync e;
    init_header(e, kind, ambiguous ? NA_EVF_ORDER_AMBIGUOUS : 0u);
    e.queue = t.queue_id;
    e.fence = lookup(f);
    e.value = value;
    e.queue_ticket = t.ticket;
    e.hresult = hr;
    // Failed calls still consume an occurrence, but establish no dependency.
    // H1 game-fence waits never imply a cross-queue ordering edge.
    e.provenance = kind == NA_EV_QUEUE_SIGNAL ? NA_FENCE_PROV_OBSERVED_QUEUE_SIGNAL : NA_FENCE_PROV_UNKNOWN;
    emit(e, NA_GROUP_QUEUE);
    unlock_submission(t);
}
HRESULT STDMETHODCALLTYPE D_Signal(ID3D12CommandQueue* q, ID3D12Fence* f, UINT64 v)
{
    if (t_host_work) return o_Signal(q, f, v); // the host's own sample fence
    const auto token = ExecuteBegin(q);
    const HRESULT hr = o_Signal(q, f, v);
    sync_event(NA_EV_QUEUE_SIGNAL, token, f, v, hr);
    return hr;
}
HRESULT STDMETHODCALLTYPE D_Wait(ID3D12CommandQueue* q, ID3D12Fence* f, UINT64 v)
{
    const auto token = ExecuteBegin(q);
    const HRESULT hr = o_Wait(q, f, v);
    sync_event(NA_EV_QUEUE_WAIT, token, f, v, hr);
    return hr;
}

void STDMETHODCALLTYPE D_ExecuteCommandLists(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists)
{
    const ExecuteToken t = ExecuteBegin(q, n, lists);
    o_ExecuteCommandLists(q, n, lists);
    ExecuteEnd(t, q, n, lists);
}

// ---------------------------------------------------------------- snapshot
bool snapshot()
{
    // Snapshot runs after start/resync publishes the current session/epoch.
    // Query the coherent status; never emit an unscoped capability report.
    na_observe_status status{};
    status.size_bytes = sizeof(status);
    if (na_observe_query_status(&status) != NA_OK || status.size_bytes < sizeof(status) ||
        status.session == 0 || status.session == NA_UNKNOWN_U64) return false;
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
    rep.r.session = status.session;
    rep.r.host_version_major = NA_OBSERVE_ABI_MAJOR;
    rep.r.host_version_minor = NA_OBSERVE_ABI_MINOR;
    rep.r.entry_count = NA_GROUP_COUNT_V1;
    rep.r.entries_offset = offsetof(Report, entries);
    rep.r.entry_stride = sizeof(na_cap_entry);
    // PARTIAL = observed with documented gaps (bundles, ExecuteIndirect,
    // GPU-written data, unhooked variants, pre-installation objects).
    const bool device_gaps = g_c.missing.load() > 0 || g_unhooked_devices.load() > 0;
    const bool lists = g_lists_hooked.load();
    const uint32_t creation = static_cast<uint32_t>(device_gaps ? NA_CAP_PARTIAL : NA_CAP_SUPPORTED);
    const uint32_t recorded = static_cast<uint32_t>(lists ? NA_CAP_PARTIAL : NA_CAP_UNSUPPORTED); // bundles/indirect/variants: never complete
    const uint32_t queue_cap = static_cast<uint32_t>(g_unhooked_queues.load() ? NA_CAP_PARTIAL : NA_CAP_SUPPORTED);
    const uint32_t support[NA_GROUP_COUNT_V1] = {
        NA_CAP_SUPPORTED /* session */, creation /* resource */, NA_CAP_PARTIAL /* descriptor: CPU record-time view */,
        recorded /* list reset/close */, queue_cap /* queue */, NA_CAP_UNSUPPORTED /* swapchain: not emitted yet */,
        creation /* pipeline */, recorded /* pass */, recorded /* resource use */, recorded /* CPU constants */,
        recorded /* snapshot: upload-heap CBVs, app-sync-unknown */};
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
    // Mip bias for static samplers: re-serialise the root signature with biased
    // samplers (versions 1.0 / 1.1; anything else is passed through unchanged). The game
    // gets the startup level; twins for the other menu levels are built right after.
    ID3DBlob* rebuilt = nullptr;
    const int game_level = g_mip_game_level.load(std::memory_order_relaxed);
    ID3DBlob* level_blob[3] = {};
    bool level_same[3] = {true, true, true};
    bool decodable = false;
    if (blob && size && !t_host_work && depth.outermost()) { // also when off: native biases are recorded
        ID3D12VersionedRootSignatureDeserializer* rd = nullptr;
        const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* u = nullptr;
        if (SUCCEEDED(D3D12CreateVersionedRootSignatureDeserializer(blob, size, IID_PPV_ARGS(&rd))) && rd &&
            (u = rd->GetUnconvertedRootSignatureDesc()) != nullptr &&
            (u->Version == D3D_ROOT_SIGNATURE_VERSION_1_0 || u->Version == D3D_ROOT_SIGNATURE_VERSION_1_1)) {
            decodable = true;
            const UINT ns = u->Version == D3D_ROOT_SIGNATURE_VERSION_1_0 ? u->Desc_1_0.NumStaticSamplers : u->Desc_1_1.NumStaticSamplers;
            const D3D12_STATIC_SAMPLER_DESC* src = u->Version == D3D_ROOT_SIGNATURE_VERSION_1_0 ? u->Desc_1_0.pStaticSamplers : u->Desc_1_1.pStaticSamplers;
            for (UINT i = 0; i < ns; ++i) if (sampler_wants_bias(src[i])) note_native(src[i].MipLODBias);
            const bool live = g_mip_live_enabled.load(std::memory_order_relaxed);
            for (int level = 0; level < 3; ++level) {
                if (level != game_level && !live) continue;
                D3D12_VERSIONED_ROOT_SIGNATURE_DESC copy = *u;
                std::vector<D3D12_STATIC_SAMPLER_DESC> ss(src, src + ns);
                uint32_t changed = 0;
                for (auto& x : ss)
                    if (sampler_wants_bias(x)) {
                        const float nb = biased(x.MipLODBias, kMipLevels[level]);
                        if (nb != x.MipLODBias) { x.MipLODBias = nb; ++changed; }
                    }
                if (!changed) continue; // the original blob is right for this level
                if (u->Version == D3D_ROOT_SIGNATURE_VERSION_1_0) copy.Desc_1_0.pStaticSamplers = ss.data();
                else copy.Desc_1_1.pStaticSamplers = ss.data();
                ID3DBlob* b = nullptr;
                ID3DBlob* err = nullptr;
                if (SUCCEEDED(D3D12SerializeVersionedRootSignature(&copy, &b, &err)) && b) {
                    level_blob[level] = b;
                    level_same[level] = false;
                    if (level == game_level) g_biased_static.fetch_add(changed, std::memory_order_relaxed);
                } else if (b) b->Release();
                if (err) err->Release();
            }
            if (level_blob[game_level]) { rebuilt = level_blob[game_level]; rebuilt->AddRef(); }
        }
        if (rd) rd->Release();
    }
    const HRESULT hr = rebuilt ? o_CreateRootSignature(dev, mask, rebuilt->GetBufferPointer(), rebuilt->GetBufferSize(), riid, out)
                               : o_CreateRootSignature(dev, mask, blob, size, riid, out);
    if (rebuilt) rebuilt->Release();
    std::shared_ptr<Obj::Twins> rs_twins;
    if (decodable && SUCCEEDED(hr) && out && *out) {
        // Twins for the other levels: same as the game's blob -> nothing to swap.
        for (int level = 0; level < 3; ++level) {
            if (level == game_level) continue;
            const bool same_as_game = level_same[level] && level_same[game_level];
            if (same_as_game) continue; // marked below if any other level needs twins
            if (!rs_twins) rs_twins = std::make_shared<Obj::Twins>();
            const void* bytes = level_blob[level] ? level_blob[level]->GetBufferPointer() : blob;
            const SIZE_T n = level_blob[level] ? level_blob[level]->GetBufferSize() : size;
            ID3D12RootSignature* twin = nullptr;
            if (SUCCEEDED(o_CreateRootSignature(dev, mask, bytes, n, IID_PPV_ARGS(&twin))) && twin) {
                rs_twins->obj[level] = twin;
                g_mip_twin_rs.fetch_add(1, std::memory_order_relaxed);
            } else g_mip_twin_fail.fetch_add(1, std::memory_order_relaxed);
        }
        if (rs_twins)
            for (int level = 0; level < 3; ++level)
                rs_twins->same[level] = level == game_level || (level_same[level] && level_same[game_level]);
    }
    for (auto* b : level_blob) if (b) b->Release();
    IUnknown* obj = depth.outermost() ? out_object(hr, out) : nullptr;
    if (!obj) return hr;
    constexpr uint32_t kMaxParams = 64, kMaxRanges = 256;
    struct Ev {
        na_ev_root_signature_created e;
        na_root_param p[kMaxParams];
        na_root_range r[kMaxRanges]; // packed right after the used params (offset field says where)
    };
    static thread_local Ev ev;
    std::memset(&ev, 0, sizeof(ev));
    na_root_param* params = ev.p;
    na_root_range ranges[kMaxRanges];
    uint32_t nranges = 0;
    ev.e.hdr.kind = NA_EV_ROOT_SIGNATURE_CREATED;
    ev.e.hdr.thread_seq = ++t_seq;
    ev.e.hdr.producer_thread = GetCurrentThreadId();
    ev.e.hdr.flags = NA_EVF_FOUNDATIONAL;
    ev.e.blob_hash = blob ? fnv1a(blob, size) : 0;
    ev.e.blob_bytes = size;
    ev.e.flags = NA_UNKNOWN_U32;
    ev.e.params_offset = offsetof(Ev, p);
    ev.e.param_stride = sizeof(na_root_param);
    ev.e.range_stride = sizeof(na_root_range);
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
            na_root_param& o = params[i];
            o.visibility = rp.ShaderVisibility;
            o.range_type = NA_UNKNOWN_U32;
            switch (rp.ParameterType) {
            case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
                o.param_type = NA_ROOT_TABLE;
                o.count = rp.DescriptorTable.NumDescriptorRanges;
                o.first_range = nranges;
                uint32_t next = 0; // running table offset; NA_UNKNOWN_U32 after an unbounded range
                for (uint32_t k = 0; k < o.count; ++k) {
                    const D3D12_DESCRIPTOR_RANGE1& rr = rp.DescriptorTable.pDescriptorRanges[k];
                    if (k == 0) {
                        o.range_type = rr.RangeType;
                        o.range_descriptors = rr.NumDescriptors;
                        o.shader_register = rr.BaseShaderRegister;
                        o.register_space = rr.RegisterSpace;
                    }
                    const uint32_t off = rr.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND ? next
                                                                                                                       : rr.OffsetInDescriptorsFromTableStart;
                    next = (off == NA_UNKNOWN_U32 || rr.NumDescriptors == UINT_MAX) ? NA_UNKNOWN_U32 : off + rr.NumDescriptors;
                    if (nranges >= kMaxRanges) {
                        ev.e.hdr.flags |= NA_EVF_TRUNCATED;
                        continue;
                    }
                    ranges[nranges++] = na_root_range{static_cast<uint32_t>(rr.RangeType), rr.NumDescriptors, rr.BaseShaderRegister,
                                                      rr.RegisterSpace, off, static_cast<uint32_t>(rr.Flags)};
                }
                break;
            }
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
    // Ranges follow the used params directly.
    const uint32_t ranges_at = static_cast<uint32_t>(offsetof(Ev, p) + ev.e.param_count * sizeof(na_root_param));
    auto* raw = reinterpret_cast<uint8_t*>(&ev);
    std::memcpy(raw + ranges_at, ranges, nranges * sizeof(na_root_range));
    ev.e.range_count = nranges;
    ev.e.ranges_offset = ranges_at;
    ev.e.hdr.size_bytes = ranges_at + nranges * static_cast<uint32_t>(sizeof(na_root_range));
    ev.e.device = device_id(dev);
    ev.e.root_signature = register_object(obj, NA_OBJ_PIPELINE_HOST);
    {
        std::unique_lock<std::shared_mutex> lock(g_m);
        auto it = g_objs.find(canon(obj));
        if (it != g_objs.end()) {
            if (it->second.retirement_unobservable) ev.e.hdr.flags |= NA_EVF_METHOD_UNOBSERVED;
            it->second.created.assign(raw, raw + ev.e.hdr.size_bytes);
            it->second.root_params.assign(params, params + ev.e.param_count);
            it->second.root_ranges.assign(ranges, ranges + nranges);
            rs_twins.swap(it->second.twins); // any previous set is released after g_m (end of scope)
            // Structural hash of the layout (parameter and range records), not the object:
            // identical layouts created in another session hash the same.
            uint64_t layout_hash = 1469598103934665603ull;
            const auto mix = [&layout_hash](const void* data, size_t len) {
                for (size_t b = 0; b < len; ++b) layout_hash = (layout_hash ^ static_cast<const uint8_t*>(data)[b]) * 1099511628211ull;
            };
            mix(params, ev.e.param_count * sizeof(na_root_param));
            mix(ranges, nranges * sizeof(na_root_range));
            it->second.root_hash = layout_hash ? layout_hash : 1;
        }
    }
    na::observe_internal::Emit(&ev, NA_GROUP_PIPELINE);
    return hr;
}

// Live mip bias: the root signature's twins (null when it needs none).
std::shared_ptr<Obj::Twins> rs_twins_of(const void* rs)
{
    if (!rs) return nullptr;
    std::shared_lock<std::shared_mutex> lock(g_m);
    const auto it = g_objs.find(canon(rs));
    return it == g_objs.end() ? nullptr : it->second.twins;
}
// Builds pipeline twins with `make(level, twin_rs)` for every level whose root-signature
// twin differs; levels whose root signature is unchanged need no twin (same[]).
// A pipeline without a known root signature gets an empty twin set (never swapped:
// a list using it falls back to the game's objects).
template <typename Make>
void make_pso_twins(IUnknown* pso, const void* rs, Make make)
{
    if (!pso || !g_mip_live_enabled.load(std::memory_order_relaxed)) return;
    const auto rt = rs_twins_of(rs);
    std::shared_ptr<Obj::Twins> pt;
    if (!rs) {
        pt = std::make_shared<Obj::Twins>(); // embedded root signature: not swappable
        pt->same[g_mip_game_level.load()] = true;
        g_mip_untwinned.fetch_add(1, std::memory_order_relaxed);
    } else if (rt) {
        pt = std::make_shared<Obj::Twins>();
        for (int level = 0; level < 3; ++level) {
            if (rt->same[level]) { pt->same[level] = true; continue; }
            if (!rt->obj[level]) continue; // twin root signature failed: no swap for this level
            IUnknown* twin = make(level, static_cast<ID3D12RootSignature*>(rt->obj[level]));
            if (twin) { pt->obj[level] = twin; g_mip_twin_pso.fetch_add(1, std::memory_order_relaxed); }
            else g_mip_twin_fail.fetch_add(1, std::memory_order_relaxed);
        }
    } else return; // root signature needs no twins: the pipeline needs none either
    {
        std::unique_lock<std::shared_mutex> lock(g_m);
        const auto it = g_objs.find(canon(pso));
        if (it != g_objs.end()) pt.swap(it->second.twins);
    }
    pt.reset(); // a replaced set (if any) is released outside g_m
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
    make_pso_twins(obj, d->pRootSignature, [&](int, ID3D12RootSignature* trs) -> IUnknown* {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC c = *d;
        c.pRootSignature = trs;
        c.CachedPSO = {}; // the cache belongs to the game's variant
        ID3D12PipelineState* t = nullptr;
        return SUCCEEDED(o_CreateGraphicsPipelineState(dev, &c, IID_PPV_ARGS(&t))) ? t : nullptr;
    });
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
    make_pso_twins(obj, d->pRootSignature, [&](int, ID3D12RootSignature* trs) -> IUnknown* {
        D3D12_COMPUTE_PIPELINE_STATE_DESC c = *d;
        c.pRootSignature = trs;
        c.CachedPSO = {};
        ID3D12PipelineState* t = nullptr;
        return SUCCEEDED(o_CreateComputePipelineState(dev, &c, IID_PPV_ARGS(&t))) ? t : nullptr;
    });
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
    size_t rs_offset = SIZE_MAX, cached_offset = SIZE_MAX; // live mip bias: subobjects patched in the twins
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
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: rs = inner_of<ID3D12RootSignature*>(s); rs_offset = at; break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: cached_offset = at; break;
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
    if (rs_offset != SIZE_MAX)
        make_pso_twins(obj, rs, [&](int, ID3D12RootSignature* trs) -> IUnknown* {
            // Copy the stream, point its root-signature subobject at the twin, drop any cache.
            std::vector<uint8_t> copy(p, p + d->SizeInBytes);
            std::memcpy(copy.data() + rs_offset + offsetof(Sub<ID3D12RootSignature*>, inner), &trs, sizeof(trs));
            if (cached_offset != SIZE_MAX) std::memset(copy.data() + cached_offset + offsetof(Sub<D3D12_CACHED_PIPELINE_STATE>, inner), 0, sizeof(D3D12_CACHED_PIPELINE_STATE));
            D3D12_PIPELINE_STATE_STREAM_DESC c{d->SizeInBytes, copy.data()};
            ID3D12PipelineState* t = nullptr;
            return SUCCEEDED(o_CreatePipelineState(dev, &c, IID_PPV_ARGS(&t))) ? t : nullptr;
        });
    else make_pso_twins(obj, nullptr, [](int, ID3D12RootSignature*) -> IUnknown* { return nullptr; });
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
    auto it = g_objs.find(canon(l));
    if (it == g_objs.end() || !it->second.list) return nullptr;
    *id = it->second.id;
    return it->second.list.get();
}

template <typename F>
void with_list_state(const void* l, F&& f)
{
    std::shared_lock<std::shared_mutex> lock(g_m);
    na_object_id id = 0;
    if (ListState* ls = list_state_locked(l, &id)) f(*ls);
}

constexpr size_t kMaxListUses = 4096;
void add_use_locked(ListState& ls, na_object_id id)
{
    if (!id || id == NA_UNKNOWN_U64 || (!ls.uses.empty() && ls.uses.back() == id)) return;
    if (ls.uses.size() >= kMaxListUses) { // compact before giving up (targets are rebound often)
        std::sort(ls.uses.begin(), ls.uses.end());
        ls.uses.erase(std::unique(ls.uses.begin(), ls.uses.end()), ls.uses.end());
        if (ls.uses.size() >= kMaxListUses * 3 / 4) {
            ls.uses_complete = false;
            return;
        }
    }
    ls.uses.push_back(id);
}
void note_uses(const void* l, std::initializer_list<na_object_id> ids)
{
    with_list_state(l, [&](ListState& ls) {
        std::lock_guard<std::mutex> bl(ls.bind_m);
        for (na_object_id id : ids) add_use_locked(ls, id);
    });
}

constexpr uint32_t kRootCbvBytes = 512; // a root CBV has no size: capture this much (bounded by the buffer)

void add_binding(ListState& ls, const ListState::Binding& b)
{
    std::lock_guard<std::mutex> lock(ls.bind_m);
    if (ls.bindings.size() < 4096) ls.bindings.push_back(b);
}

// A list first met through a recorded op (created before installation).
void preexisting_list(ID3D12GraphicsCommandList* l)
{
    bool created = false;
    const na_object_id id = ensure_registered(l, NA_OBJ_LIST, &created);
    if (!created) return; // another thread met it first and reports it
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
    if (t_host_work || !na::observe_internal::Started() || !l) return false;
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
        if (t_host_work) return;
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
                {
                    std::lock_guard<std::mutex> bl(ls->bind_m);
                    ls->bindings.clear();
                    ls->states.clear();
                    ls->evidence_complete = true;
                    ls->uses.clear();
                    ls->uses_complete = true;
                    ls->targets.clear();
                    ls->insertion.Reset();
                    ls->sampled_textures.clear();
                    ls->cleared.clear();
                    ls->pso = pso;
                    ls->heaps[0] = ls->heaps[1] = nullptr;
                    ls->nheaps = 0;
                    ls->args[0].clear();
                    ls->args[1].clear();
                    ls->cur_rt = ls->cur_ds = 0;
                    ls->cur_nrt = 0;
                    ls->restorable = true;
                    ls->in_render_pass = false;
                }
                ls->root_sig[0] = ls->root_sig[1] = nullptr;
                ls->coverage_complete.store(cov == na::hooking::variants::Coverage::Complete);
            }
            gen = ls->generation_known.load() ? ls->generation.load() : NA_UNKNOWN_U64;
            auto it = g_objs.find(canon(l));
            bundle = it != g_objs.end() && it->second.created.size() >= sizeof(na_ev_list_created) &&
                     reinterpret_cast<const na_ev_list_created*>(it->second.created.data())->is_bundle;
        }
        if (!na::observe_internal::Started()) return;
        na_object_id alloc_id = alloc ? lookup(alloc) : 0;
        if (alloc && !alloc_id) {
        bool created = false;
        alloc_id = ensure_registered(alloc, NA_OBJ_ALLOCATOR, &created);
    }
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
void note_draw(GL* l) {
    std::shared_lock<std::shared_mutex> lock(g_m);
    na_object_id id = 0;
    auto* ls = list_state_locked(l, &id);
    if (!ls) return;
    std::lock_guard<std::mutex> bl(ls->bind_m);
    uint64_t sel_depth = 0, sel_color = 0;
    GetLiveSelection(&sel_depth, &sel_color);
    for (uint32_t i = 0; i < ls->cur_nrt; ++i)
        if (ls->cur_rts[i]) {
            na::j2::InsertionEvent ev{ls->cur_rts[i], 0, reinterpret_cast<uint64_t>(ls->pso), true, na::j2::Op::DrawWrite};
            ev.with_depth = sel_depth && ls->cur_ds == sel_depth; // scene content, not a reuse of the texture
            ls->insertion.Add(ev);
        }
    auto pipeline = g_objs.find(canon(ls->pso));
    if (ls->cur_ds && ls->cur_ds_sub != NA_UNKNOWN_U32 && pipeline != g_objs.end() &&
        pipeline->second.created.size() >= sizeof(na_ev_pipeline_created)) {
        const auto* p = reinterpret_cast<const na_ev_pipeline_created*>(pipeline->second.created.data());
        if (p->depth_enable == 1 && p->depth_write == 1 && ls->states.size() < 4096)
            ls->states.push_back({ls->cur_ds, ls->cur_ds_sub, D3D12_RESOURCE_STATE_DEPTH_WRITE});
    }
}
template <>
struct On<sl::list::DrawInstanced> {
    static void run(GL* l, UINT v, UINT i, UINT, UINT)
    {
        Rec r;
        if (begin_op(l, &r)) emit_op(r, NA_OP_DRAW, {v, i});
        if (r.list) note_draw(l);
    }
};
template <>
struct On<sl::list::DrawIndexedInstanced> {
    static void run(GL* l, UINT n, UINT i, UINT, INT, UINT)
    {
        Rec r;
        if (begin_op(l, &r)) emit_op(r, NA_OP_DRAW_INDEXED, {n, i});
        if (r.list) note_draw(l);
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
        if (!begin_op(l, &r)) return;
        const na_object_id d = resource_id(dst), s = resource_id(src);
        emit_op(r, NA_OP_COPY_BUFFER_REGION, {d, doff, s, soff, n}, nullptr, 0, 0, 0, NA_GROUP_RESOURCE_USE);
        note_uses(l, {d, s});
    }
};
template <>
struct On<sl::list::CopyTextureRegion> {
    static void run(GL* l, const D3D12_TEXTURE_COPY_LOCATION* dst, UINT dx, UINT dy, UINT dz, const D3D12_TEXTURE_COPY_LOCATION* src,
                    const D3D12_BOX* box)
    {
        Rec r;
        if (!begin_op(l, &r) || !dst || !src) return;
        const auto sub = [](const D3D12_TEXTURE_COPY_LOCATION* c) {
            return c->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX ? static_cast<uint64_t>(c->SubresourceIndex) : uint64_t{NA_UNKNOWN_U32};
        };
        na_copy_region cr{};
        cr.dst_x = dx;
        cr.dst_y = dy;
        cr.dst_z = dz;
        if (box) {
            cr.has_box = 1;
            const uint32_t b6[6] = {box->left, box->top, box->front, box->right, box->bottom, box->back};
            std::memcpy(cr.box, b6, sizeof(b6));
        }
        const auto fpinfo = [](const D3D12_TEXTURE_COPY_LOCATION* c, uint64_t* off, uint32_t* f) {
            if (c->Type != D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) return;
            const D3D12_SUBRESOURCE_FOOTPRINT& fp = c->PlacedFootprint.Footprint;
            *off = c->PlacedFootprint.Offset;
            f[0] = static_cast<uint32_t>(fp.Format);
            f[1] = fp.Width;
            f[2] = fp.Height;
            f[3] = fp.Depth;
            f[4] = fp.RowPitch;
        };
        fpinfo(dst, &cr.dst_footprint_offset, cr.dst_footprint);
        fpinfo(src, &cr.src_footprint_offset, cr.src_footprint);
        const na_object_id d = resource_id(dst->pResource), s = resource_id(src->pResource);
        emit_op(r, NA_OP_COPY_TEXTURE_REGION, {d, sub(dst), s, sub(src)}, &cr, sizeof(cr), sizeof(cr), 0, NA_GROUP_RESOURCE_USE);
        note_uses(l, {d, s});
    }
};
template <>
struct On<sl::list::CopyResource> {
    static void run(GL* l, ID3D12Resource* dst, ID3D12Resource* src)
    {
        Rec r;
        if (!begin_op(l, &r)) return;
        const na_object_id d = resource_id(dst), s = resource_id(src);
        emit_op(r, NA_OP_COPY_RESOURCE, {d, s}, nullptr, 0, 0, 0, NA_GROUP_RESOURCE_USE);
        note_uses(l, {d, s});
    }
};
template <>
struct On<sl::list::ResolveSubresource> {
    static void run(GL* l, ID3D12Resource* dst, UINT ds, ID3D12Resource* src, UINT ss, DXGI_FORMAT f)
    {
        Rec r;
        if (!begin_op(l, &r)) return;
        const na_object_id d = resource_id(dst), s = resource_id(src);
        emit_op(r, NA_OP_RESOLVE, {d, ds, s, ss, static_cast<uint64_t>(f)}, nullptr, 0, 0, 0, NA_GROUP_RESOURCE_USE);
        note_uses(l, {d, s});
    }
};
template <>
struct On<sl::list::RSSetViewports> {
    static void run(GL* l, UINT n, const D3D12_VIEWPORT* v)
    {
        Rec r;
        if (!begin_op(l, &r) || !n || !v) return;
        // Every viewport (review): 6 floats each, first one also in a[].
        constexpr UINT kMax = D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        float all[kMax * 6];
        const UINT m = std::min<UINT>(n, kMax);
        for (UINT i = 0; i < m; ++i) {
            const float vals[6] = {v[i].TopLeftX, v[i].TopLeftY, v[i].Width, v[i].Height, v[i].MinDepth, v[i].MaxDepth};
            std::memcpy(all + i * 6, vals, sizeof(vals));
        }
        emit_op(r, NA_OP_RS_VIEWPORT, {n, float_bits(v->TopLeftX, v->TopLeftY), float_bits(v->Width, v->Height), float_bits(v->MinDepth, v->MaxDepth)},
                all, m * 6 * 4, 6 * 4, m < n ? NA_EVF_TRUNCATED : 0u);
    }
};
template <>
struct On<sl::list::RSSetScissorRects> {
    static void run(GL* l, UINT n, const D3D12_RECT* rc)
    {
        Rec r;
        if (!begin_op(l, &r) || !n || !rc) return;
        const auto pack = [](LONG a, LONG b) { return static_cast<uint64_t>(static_cast<uint32_t>(a)) | (static_cast<uint64_t>(static_cast<uint32_t>(b)) << 32); };
        constexpr UINT kMax = D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        int32_t all[kMax * 4];
        const UINT m = std::min<UINT>(n, kMax);
        for (UINT i = 0; i < m; ++i) {
            all[i * 4 + 0] = rc[i].left;
            all[i * 4 + 1] = rc[i].top;
            all[i * 4 + 2] = rc[i].right;
            all[i * 4 + 3] = rc[i].bottom;
        }
        emit_op(r, NA_OP_RS_SCISSOR, {n, pack(rc->left, rc->top), pack(rc->right, rc->bottom)}, all, m * 16, 16, m < n ? NA_EVF_TRUNCATED : 0u);
    }
};
template <>
struct On<sl::list::SetPipelineState> {
    static void run(GL* l, ID3D12PipelineState* p)
    {
        Rec r;
        if (!begin_op(l, &r)) return;
        emit_op(r, NA_OP_SET_PIPELINE, {id_or_unknown(p)});
        with_list_state(l, [&](ListState& ls) { std::lock_guard<std::mutex> bl(ls.bind_m); ls.pso = p; });
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
                o.resource = resource_id(b[i].Transition.pResource);
                o.subresource = b[i].Transition.Subresource;
                o.state_before = b[i].Transition.StateBefore;
                o.state_after = b[i].Transition.StateAfter;
                break;
            case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:
                o.resource = resource_id(b[i].Aliasing.pResourceBefore);
                o.resource_after = resource_id(b[i].Aliasing.pResourceAfter);
                break;
            default:
                o.resource = resource_id(b[i].UAV.pResource);
                break;
            }
        }
        emit_op(r, NA_OP_RESOURCE_BARRIER, {n}, out, m * static_cast<uint32_t>(sizeof(na_barrier)), sizeof(na_barrier), m < n ? NA_EVF_TRUNCATED : 0u,
                NA_GROUP_RESOURCE_USE);
        // C9b: state evidence for sample copies (explicit transitions only).
        // G3-06: the host records EVERY transition of the call; only the wire
        // event is limited to 64 entries (it stays flagged TRUNCATED). An
        // overflow of the recording's evidence store still invalidates it
        // (C9B-R4). BEGIN_ONLY leaves the state pending until its END_ONLY half.
        // Aliasing barriers make the named resources' states unknown (both
        // sides). Only placed/reserved resources alias, and those never enter
        // the carried-state store, so a null (wildcard) aliasing barrier needs
        // no store-wide action. Allocation is bounded by the 8192-entry
        // recording store before anything proportional to n is reserved.
        constexpr size_t kMaxStates = 8192;
        std::vector<ListState::StateNote> add;
        std::vector<na_object_id> used;
        add.reserve(std::min<size_t>(n, kMaxStates));
        used.reserve(std::min<size_t>(n, 2 * kMaxListUses));
        bool overflow = false;
        for (uint32_t i = 0; i < n && !overflow; ++i) {
            const auto push = [&](na_object_id id, uint32_t subresource, uint32_t state) {
                if (add.size() >= kMaxStates) overflow = true;
                else add.push_back({id, subresource, state});
            };
            const auto known_id = [](na_object_id id) { return id && id != NA_UNKNOWN_U64; };
            if (b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
                const na_object_id id = i < m ? out[i].resource : resource_id(b[i].Transition.pResource);
                if (!known_id(id)) continue;
                const bool begin_only = (b[i].Flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY) != 0;
                push(id, b[i].Transition.Subresource, begin_only ? kStatePending : static_cast<uint32_t>(b[i].Transition.StateAfter));
                if (used.size() < 2 * kMaxListUses) used.push_back(id);
            } else if (b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
                const na_object_id before = i < m ? out[i].resource : resource_id(b[i].Aliasing.pResourceBefore);
                const na_object_id after = i < m ? out[i].resource_after : resource_id(b[i].Aliasing.pResourceAfter);
                for (na_object_id id : {before, after})
                    if (known_id(id)) push(id, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, kStatePending);
            } else {
                const na_object_id id = i < m ? out[i].resource : resource_id(b[i].UAV.pResource);
                if (known_id(id) && used.size() < 2 * kMaxListUses) used.push_back(id);
            }
        }
        with_list_state(l, [&](ListState& ls) {
            std::lock_guard<std::mutex> bl(ls.bind_m);
            if (overflow || ls.states.size() + add.size() > kMaxStates) ls.evidence_complete = false;
            else ls.states.insert(ls.states.end(), add.begin(), add.end());
            for (na_object_id id : used) add_use_locked(ls, id);
            if (used.size() >= 2 * kMaxListUses) ls.uses_complete = false;
        });
    }
};
template <>
struct On<sl::list::ExecuteBundle> {
    static void run(GL* l, ID3D12GraphicsCommandList* bundle)
    {
        Rec r;
        if (!begin_op(l, &r)) return;
        emit_op(r, NA_OP_EXECUTE_BUNDLE, {id_or_unknown(bundle)}, nullptr, 0, 0, NA_EVF_METHOD_UNOBSERVED);
        with_list_state(l, [&](ListState& ls) { std::lock_guard<std::mutex> bl(ls.bind_m); ls.restorable = false; }); // a bundle's state changes are not observed
    }
};
template <>
struct On<sl::list::SetDescriptorHeaps> {
    static void run(GL* l, UINT n, ID3D12DescriptorHeap* const* h)
    {
        Rec r;
        if (!begin_op(l, &r)) return;
        for (UINT i = 0; h && i < n; ++i) ensure_heap_registered(h[i]); // G3-03
        na_object_id res = 0, smp = 0;
        for (UINT i = 0; h && i < n; ++i) {
            if (!h[i]) continue;
            std::shared_lock<std::shared_mutex> lock(g_m);
            for (auto& dh : g_heaps)
                if (dh->key == h[i]) (dh->type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? smp : res) = dh->id;
        }
        emit_op(r, NA_OP_SET_DESCRIPTOR_HEAPS, {res, smp});
        with_list_state(l, [&](ListState& ls) {
            std::lock_guard<std::mutex> bl(ls.bind_m);
            ls.nheaps = 0;
            for (UINT i = 0; h && i < n && i < 2; ++i) ls.heaps[ls.nheaps++] = h[i];
        });
    }
};
template <uint32_t Compute>
void root_sig(GL* l, ID3D12RootSignature* rs)
{
    Rec r;
    if (!begin_op(l, &r)) return;
    emit_op(r, NA_OP_SET_ROOT_SIGNATURE, {id_or_unknown(rs), Compute});
    with_list_state(l, [&](ListState& ls) {
        const bool changed = ls.root_sig[Compute] != rs;
        ls.root_sig[Compute] = rs;
        std::lock_guard<std::mutex> bl(ls.bind_m);
        if (changed) ls.args[Compute].clear(); // a different root signature invalidates every root argument
    });
}
template <>
struct On<sl::list::SetComputeRootSignature> { static void run(GL* l, ID3D12RootSignature* rs) { root_sig<1>(l, rs); } };
template <>
struct On<sl::list::SetGraphicsRootSignature> { static void run(GL* l, ID3D12RootSignature* rs) { root_sig<0>(l, rs); } };
void shadow_arg(GL* l, uint32_t compute, UINT param, uint8_t type, uint64_t value, const void* consts = nullptr, UINT count = 0, UINT dest = 0)
{
    if (param >= 64) return;
    with_list_state(l, [&](ListState& ls) {
        std::lock_guard<std::mutex> bl(ls.bind_m);
        auto& v = ls.args[compute];
        if (v.size() <= param) v.resize(param + 1);
        ListState::RootArg& a = v[param];
        if (type == 5) {
            if (a.type != 5) a.consts.clear();
            if (dest + count > 64) return;
            if (a.consts.size() < dest + count) a.consts.resize(dest + count, 0u);
            std::memcpy(a.consts.data() + dest, consts, count * 4u);
        } else {
            a.consts.clear();
        }
        a.type = type;
        a.value = value;
    });
}
std::atomic<TableBindCallback> g_table_cb{nullptr};
template <uint32_t Compute>
void root_table(GL* l, UINT param, D3D12_GPU_DESCRIPTOR_HANDLE h)
{
    Rec r;
    if (!begin_op(l, &r)) return;
    na_object_id heap = 0;
    uint32_t slot = 0;
    uint64_t version = 0;
    shadow_arg(l, Compute, param, 1, h.ptr);
    {
        std::shared_lock<std::shared_mutex> lock(g_m);
        DescHeap* dh = heap_for_gpu_locked(h.ptr, &slot);
        if (dh) {
            heap = dh->id;
            version = dh->versions[slot];
        }
        // C9: CBV ranges of this table (first range, per the bound root signature).
        na_object_id lid = 0;
        ListState* ls = list_state_locked(l, &lid);
        if (dh && ls && ls->root_sig[Compute]) {
            auto it = g_objs.find(canon(ls->root_sig[Compute]));
            if (it != g_objs.end() && param < it->second.root_params.size()) {
                const na_root_param& rp = it->second.root_params[param];
                if (rp.param_type == NA_ROOT_TABLE) {
                    const auto& rr = it->second.root_ranges;
                    for (uint32_t q = 0; q < rp.count && rp.first_range + q < rr.size(); ++q) {
                        const na_root_range& range = rr[rp.first_range + q];
                        if (range.range_type == D3D12_DESCRIPTOR_RANGE_TYPE_SRV && range.table_offset != NA_UNKNOWN_U32) {
                            std::lock_guard<std::mutex> bl(ls->bind_m);
                            for (uint32_t k = 0; k < std::min<uint32_t>(range.num_descriptors, 64); ++k) {
                                const uint64_t index = uint64_t(slot) + range.table_offset + k;
                                if (index >= dh->count) break;
                                const auto& si = dh->info[static_cast<size_t>(index)];
                                if (si.kind == NA_DESC_SRV && si.resource && si.resource != NA_UNKNOWN_U64 &&
                                    ls->sampled_textures.size() < 256 &&
                                    std::find(ls->sampled_textures.begin(), ls->sampled_textures.end(), si.resource) == ls->sampled_textures.end())
                                    ls->sampled_textures.push_back(si.resource);
                            }
                        }
                        if (range.range_type != D3D12_DESCRIPTOR_RANGE_TYPE_CBV || range.table_offset == NA_UNKNOWN_U32) continue;
                        const uint32_t n = std::min<uint32_t>(range.num_descriptors, 8);
                        const bool cut = range.num_descriptors > 8; // incl. unbounded: coverage flagged
                        for (uint32_t k = 0; k < n && slot + range.table_offset + k < dh->count; ++k) {
                            const DescHeap::SlotInfo& si = dh->info[slot + range.table_offset + k];
                            if (si.kind != NA_DESC_CBV) continue;
                            ListState::Binding b;
                            b.kind = NA_SNAP_TABLE_CBV;
                            b.param = param;
                            b.compute = Compute;
                            b.size = si.cbv_size;
                            b.op_index = r.op_index;
                            b.offset = si.cbv_offset;
                            b.resource = si.resource == NA_UNKNOWN_U64 ? 0 : si.resource;
                            b.range_truncated = cut;
                            add_binding(*ls, b);
                        }
                    }
                }
            }
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
    shadow_arg(l, Compute, param, 5, 0, data, count, dest);
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
    shadow_arg(l, Compute, param, Kind == NA_DESC_CBV ? 2 : (Kind == NA_DESC_SRV ? 3 : 4), va);
    na_object_id res = 0;
    uint64_t off = 0;
    bool known = false;
    {
        std::shared_lock<std::shared_mutex> lock(g_m);
        known = resolve_va_locked(va, &res, &off);
    }
    if (Kind == NA_DESC_CBV) {
        ListState::Binding b;
        b.kind = NA_SNAP_ROOT_CBV;
        b.param = param;
        b.compute = Compute;
        b.size = kRootCbvBytes;
        b.op_index = r.op_index;
        b.offset = known ? off : 0;
        b.resource = known ? res : 0;
        with_list_state(l, [&](ListState& ls) { add_binding(ls, b); });
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
// Current targets + target history of a recording (OMSetRenderTargets and BeginRenderPass).
// refs: render targets, then the depth view last when has_dsv.
void set_targets_locked(ListState& ls, const na_view_ref* refs, uint32_t m, bool has_dsv)
{
    const uint32_t nrt = has_dsv ? (m ? m - 1 : 0) : m;
    ls.cur_rt = nrt ? refs[0].resource : 0;
    ls.cur_nrt = nrt;
    for (uint32_t i = 0; i < nrt; ++i) ls.cur_rts[i] = refs[i].resource;
    ls.cur_ds = has_dsv && m ? refs[m - 1].resource : 0;
    ls.cur_ds_sub = has_dsv && m ? refs[m - 1].first_subresource : NA_UNKNOWN_U32;
    for (uint32_t i = 0; i < m; ++i) add_use_locked(ls, refs[i].resource);
    if (m > 0 && ls.targets.size() < 4096) {
        const na_object_id ds = has_dsv ? refs[m - 1].resource : 0; // 0: no depth target
        const uint32_t ds_sub = has_dsv ? refs[m - 1].first_subresource : 0;
        for (uint32_t i = 0; i < nrt; ++i)
            if (refs[i].resource && (ls.targets.empty() || !ls.targets.back().same(refs[i].resource, ds) ||
                                     ls.targets.back().rt_sub != refs[i].first_subresource))
                ls.targets.push_back({refs[i].resource, ds, refs[i].first_subresource, ds_sub}); // UNKNOWN: a view J2 cannot place
    }
}
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
                DescHeap* heap = heap_for_cpu_locked(h.ptr, &slot);
                refs[m++] = view_ref_locked(heap, slot);
            }
            if (dsv) {
                uint32_t slot = 0;
                DescHeap* heap = heap_for_cpu_locked(dsv->ptr, &slot);
                refs[m++] = view_ref_locked(heap, slot);
            }
        }
        emit_op(r, NA_OP_OM_SET_RENDER_TARGETS, {std::min<UINT>(n, 8), dsv ? 1u : 0u}, refs, m * static_cast<uint32_t>(sizeof(na_view_ref)), sizeof(na_view_ref), 0,
                NA_GROUP_RESOURCE_USE);
        with_list_state(l, [&](ListState& ls) {
            std::lock_guard<std::mutex> bl(ls.bind_m);
            set_targets_locked(ls, refs, m, dsv != nullptr);
        });
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
        note_uses(l, {v.resource});
        if ((f & D3D12_CLEAR_FLAG_DEPTH) && v.resource && v.first_subresource != NA_UNKNOWN_U32)
            with_list_state(l, [&](ListState& ls) {
                std::lock_guard<std::mutex> bl(ls.bind_m);
                if (ls.states.size() < 4096) ls.states.push_back({v.resource, v.first_subresource, D3D12_RESOURCE_STATE_DEPTH_WRITE});
            });
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
        note_uses(l, {v.resource});
        with_list_state(l, [&](ListState& ls) {
            std::lock_guard<std::mutex> bl(ls.bind_m);
            if (v.resource) ls.insertion.Add({v.resource, 0, 0, true, na::j2::Op::ClearWrite});
            if (v.resource && ls.cleared.size() < 64 && std::find(ls.cleared.begin(), ls.cleared.end(), v.resource) == ls.cleared.end())
                ls.cleared.push_back(v.resource);
        });
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

// ID3D12GraphicsCommandList4 render passes (Kind::CommandList4). BeginRenderPass binds
// the attachments (and may clear them); EndRenderPass leaves bindings UNDEFINED
// (Microsoft's contract), so current bindings are cleared, history kept. Host
// copies/dispatches are illegal inside a pass: ListStateRestorable is false there.
template <>
struct On<sl::list::BeginRenderPass> {
    static void run(GL* l, UINT n, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* rts, const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* ds,
                    D3D12_RENDER_PASS_FLAGS)
    {
        Rec r;
        if (!begin_op(l, &r)) return;
        na_view_ref refs[9];
        uint32_t m = 0;
        {
            std::shared_lock<std::shared_mutex> lock(g_m);
            for (UINT i = 0; rts && i < n && i < 8; ++i) {
                uint32_t slot = 0;
                refs[m++] = view_ref_locked(heap_for_cpu_locked(rts[i].cpuDescriptor.ptr, &slot), slot);
            }
            if (ds) {
                uint32_t slot = 0;
                refs[m++] = view_ref_locked(heap_for_cpu_locked(ds->cpuDescriptor.ptr, &slot), slot);
            }
        }
        const uint32_t nrt = ds ? (m ? m - 1 : 0) : m;
        emit_op(r, NA_OP_OM_SET_RENDER_TARGETS, {nrt, ds ? 1u : 0u}, refs, m * static_cast<uint32_t>(sizeof(na_view_ref)), sizeof(na_view_ref), 0,
                NA_GROUP_RESOURCE_USE);
        // Beginning clears are clears of those attachments (as separate ops, like the Clear* calls).
        for (uint32_t i = 0; i < nrt; ++i) {
            if (rts[i].BeginningAccess.Type != D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR) continue;
            Rec rc;
            if (!begin_op(l, &rc)) break;
            const FLOAT* c = rts[i].BeginningAccess.Clear.ClearValue.Color;
            emit_op(rc, NA_OP_CLEAR_RTV, {float_bits(c[0], c[1]), float_bits(c[2], c[3])}, &refs[i], sizeof(refs[i]), sizeof(refs[i]), 0, NA_GROUP_RESOURCE_USE);
        }
        const bool depth_clear = ds && ds->DepthBeginningAccess.Type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
        if (depth_clear) {
            Rec rc;
            if (begin_op(l, &rc)) {
                const auto& v = ds->DepthBeginningAccess.Clear.ClearValue.DepthStencil;
                emit_op(rc, NA_OP_CLEAR_DSV, {static_cast<uint64_t>(D3D12_CLEAR_FLAG_DEPTH), float_bits(v.Depth, 0.f), v.Stencil}, &refs[m - 1],
                        sizeof(refs[m - 1]), sizeof(refs[m - 1]), 0, NA_GROUP_RESOURCE_USE);
            }
        }
        with_list_state(l, [&](ListState& ls) {
            std::lock_guard<std::mutex> bl(ls.bind_m);
            set_targets_locked(ls, refs, m, ds != nullptr);
            ls.in_render_pass = true;
            for (uint32_t i = 0; i < nrt; ++i) {
                if (rts[i].BeginningAccess.Type != D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR || !refs[i].resource) continue;
                ls.insertion.Add({refs[i].resource, 0, 0, true, na::j2::Op::ClearWrite});
                if (ls.cleared.size() < 64 && std::find(ls.cleared.begin(), ls.cleared.end(), refs[i].resource) == ls.cleared.end())
                    ls.cleared.push_back(refs[i].resource);
            }
            if (depth_clear && refs[m - 1].resource && refs[m - 1].first_subresource != NA_UNKNOWN_U32 && ls.states.size() < 4096)
                ls.states.push_back({refs[m - 1].resource, refs[m - 1].first_subresource, D3D12_RESOURCE_STATE_DEPTH_WRITE});
        });
    }
};
template <>
struct On<sl::list::EndRenderPass> {
    static void run(GL* l)
    {
        Rec r;
        if (!begin_op(l, &r)) return;
        with_list_state(l, [&](ListState& ls) {
            std::lock_guard<std::mutex> bl(ls.bind_m);
            ls.in_render_pass = false;
            ls.cur_rt = ls.cur_ds = 0; // attachments are not bound after the pass
            ls.cur_nrt = 0;
            for (auto& t : ls.cur_rts) t = 0;
            ls.cur_ds_sub = NA_UNKNOWN_U32;
        });
    }
};

// Width/height/format of a resource from its recorded creation (no COM calls under g_m).
bool resource_dims_locked(na_object_id id, uint32_t* w, uint32_t* h, uint32_t* fmt)
{
    const auto key = g_res_keys.find(id);
    if (key == g_res_keys.end()) return false;
    const auto o = g_objs.find(key->second);
    if (o == g_objs.end() || o->second.created.size() < sizeof(na_ev_resource_created)) return false;
    const auto* e = reinterpret_cast<const na_ev_resource_created*>(o->second.created.data());
    if (e->hdr.kind != NA_EV_RESOURCE_CREATED || e->dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return false;
    *w = static_cast<uint32_t>(e->width);
    *h = e->height;
    *fmt = e->format;
    return true;
}
// Minimal lineage (Astra #3): textures copied/resolved from the scene colour this frame
// (and from those), so a read of the copy counts as continuation of the scene chain.
// Bounded; reset by J2 at each Present.
std::mutex g_derived_m;
na_object_id g_derived[16] = {};
std::atomic<uint32_t> g_derived_n{0}; // read without the lock as a fast "nothing derived" check
bool is_derived_any(const na_object_id* ids, uint32_t n)
{
    if (!n || !g_derived_n.load(std::memory_order_relaxed)) return false; // the common case: no copy of the scene yet
    std::lock_guard<std::mutex> lk(g_derived_m);
    const uint32_t m = g_derived_n.load(std::memory_order_relaxed);
    for (uint32_t k = 0; k < n; ++k)
        for (uint32_t i = 0; i < m; ++i) if (ids[k] && g_derived[i] == ids[k]) return true;
    return false;
}
bool is_derived(na_object_id id) { return is_derived_any(&id, 1); }
void add_derived(na_object_id id)
{
    if (!id) return;
    std::lock_guard<std::mutex> lk(g_derived_m);
    const uint32_t m = g_derived_n.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < m; ++i) if (g_derived[i] == id) return;
    if (m < 16) { g_derived[m] = id; g_derived_n.store(m + 1, std::memory_order_release); }
}

// Resolve bindings at use time: a table bind may precede PSO/target changes.
void before_read_transition(GL* l, UINT count, const D3D12_RESOURCE_BARRIER* barriers)
{
    if (t_host_work || !na::observe_internal::Started() || !barriers) return;
    uint64_t depth = 0, color = 0;
    GetLiveSelection(&depth, &color);
    if (!color) return;
    constexpr UINT reads = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                           D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
    for (UINT i = 0; i < count; ++i) {
        const auto& b = barriers[i];
        if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || b.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE ||
            !(b.Transition.StateBefore & D3D12_RESOURCE_STATE_RENDER_TARGET) || !(b.Transition.StateAfter & reads) ||
            resource_id(b.Transition.pResource) != color) continue;
        uint32_t cs = 0, ds = 0;
        if (!ListSceneSubresource(l, color, depth, &cs, &ds) ||
            (b.Transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES && b.Transition.Subresource != cs)) continue;
        {
            std::shared_lock<std::shared_mutex> lock(g_m);
            na_object_id lid = 0;
            auto* ls = list_state_locked(l, &lid);
            if (!ls) return;
            std::lock_guard<std::mutex> bl(ls->bind_m);
            ls->insertion.Add({color, 0, UINT64_MAX, false, na::j2::Op::Barrier});
        }
        if (auto cb = g_table_cb.load(std::memory_order_acquire)) cb(l, &color, 1, true);
        break; // our pass runs before the whole original barrier batch
    }
}

void before_texture_copy(GL* l, ID3D12Resource* dst, ID3D12Resource* src)
{
    if (t_host_work || !na::observe_internal::Started() || !dst || !src) return;
    uint64_t depth = 0, color = 0;
    GetLiveSelection(&depth, &color);
    const auto source = resource_id(src), destination = resource_id(dst);
    if (!color) return;
    const bool from_color = source == color;
    if (!from_color && !is_derived(source)) return;
    add_derived(destination); // lineage: the copy carries the scene on
    {
        std::shared_lock<std::shared_mutex> lock(g_m);
        na_object_id lid = 0;
        auto* ls = list_state_locked(l, &lid);
        if (!ls) return;
        std::lock_guard<std::mutex> bl(ls->bind_m);
        na::j2::InsertionEvent ev{color, destination, 0, false, from_color ? na::j2::Op::CopyRead : na::j2::Op::DerivedRead};
        resource_dims_locked(destination, &ev.dst_w, &ev.dst_h, &ev.dst_fmt);
        ev.color_srvs = from_color ? 1 : 0;
        ls->insertion.Add(ev);
    }
    if (from_color) if (auto cb = g_table_cb.load(std::memory_order_acquire)) cb(l, &color, 1, true);
}

void after_texture_copy(GL* l, ID3D12Resource* dst)
{
    if (t_host_work || !na::observe_internal::Started() || !dst) return;
    const auto destination = resource_id(dst);
    std::shared_lock<std::shared_mutex> lock(g_m);
    na_object_id lid = 0;
    auto* ls = list_state_locked(l, &lid);
    if (!ls) return;
    std::lock_guard<std::mutex> bl(ls->bind_m);
    ls->insertion.Add({destination, 0, 0, true, na::j2::Op::CopyWrite});
}

void before_shader_use(GL* l, bool compute)
{
    if (t_host_work || !na::observe_internal::Started()) return;
    uint64_t depth = 0, color = 0;
    GetLiveSelection(&depth, &color);
    if (!color) return;
    bool reads_color = false;
    {
        std::shared_lock<std::shared_mutex> lock(g_m);
        na_object_id lid = 0;
        auto* ls = list_state_locked(l, &lid);
        if (!ls) return;
        std::lock_guard<std::mutex> bl(ls->bind_m);
        const auto signature = g_objs.find(canon(ls->root_sig[compute]));
        if (signature == g_objs.end()) return;
        na_object_id destination = compute ? 0 : ls->cur_rt;
        uint8_t color_srvs = 0;
        na_object_id srv_ids[16] = {};
        uint32_t nsrv = 0;
        bool reads_derived = false;
        const auto& params = signature->second.root_params;
        const auto& ranges = signature->second.root_ranges;
        for (size_t p = 0; p < ls->args[compute].size() && p < params.size(); ++p) {
            const auto& arg = ls->args[compute][p];
            if (arg.type != 1 || params[p].param_type != NA_ROOT_TABLE) continue;
            uint32_t slot = 0;
            auto* heap = heap_for_gpu_locked(arg.value, &slot);
            if (!heap) continue;
            bool bound = false;
            for (uint32_t h = 0; h < ls->nheaps; ++h) bound |= heap->key == ls->heaps[h];
            if (!bound) continue;
            for (uint32_t r = 0; r < params[p].count && params[p].first_range + r < ranges.size(); ++r) {
                const auto& range = ranges[params[p].first_range + r];
                if (range.table_offset == NA_UNKNOWN_U32) continue;
                for (uint32_t k = 0; k < std::min<uint32_t>(range.num_descriptors, 64); ++k) {
                    const uint64_t index = uint64_t(slot) + range.table_offset + k;
                    if (index >= heap->count) break;
                    const auto& info = heap->info[static_cast<size_t>(index)];
                    if (range.range_type == D3D12_DESCRIPTOR_RANGE_TYPE_SRV && info.kind == NA_DESC_SRV) {
                        if (info.resource == color) {
                            reads_color = true;
                            if (color_srvs < 255) ++color_srvs;
                        } else if (info.resource) {
                            if (nsrv < 16) srv_ids[nsrv++] = info.resource;
                        }
                    }
                    if (compute && !destination && range.range_type == D3D12_DESCRIPTOR_RANGE_TYPE_UAV && info.kind == NA_DESC_UAV)
                        destination = info.resource;
                }
            }
        }
        if (reads_color) {
            // Role-class inputs (only for the few draws that read the scene colour).
            na::j2::InsertionEvent ev{color, destination, reinterpret_cast<uint64_t>(ls->pso), false,
                                      compute ? na::j2::Op::ComputeRead : na::j2::Op::GraphicsRead,
                                      signature->second.root_hash};
            ev.color_srvs = color_srvs;
            resource_dims_locked(destination, &ev.dst_w, &ev.dst_h, &ev.dst_fmt);
            uint32_t cw = 0, ch = 0, cf = 0;
            if (resource_dims_locked(color, &cw, &ch, &cf))
                for (uint32_t si = 0; si < nsrv; ++si) {
                    uint32_t sw = 0, sh = 0, sf = 0;
                    if (resource_dims_locked(srv_ids[si], &sw, &sh, &sf) && sw == cw && sh == ch && ev.view_srvs < 255) ++ev.view_srvs;
                }
            ls->insertion.Add(ev);
        } else {
            reads_derived = is_derived_any(srv_ids, nsrv);
            if (reads_derived) // lineage only: the scene chain continues through a copy
                ls->insertion.Add({color, destination, reinterpret_cast<uint64_t>(ls->pso), false, na::j2::Op::DerivedRead});
        }
    }
    if (reads_color) if (auto cb = g_table_cb.load(std::memory_order_acquire)) cb(l, &color, 1, compute);
}

// ---- Live mip bias: bind the twins of the level latched at this recording's Reset.
// A pipeline without a twin for that level (pipeline-library loads, embedded root
// signatures) switches the recording back to the game's own objects: the state shadow
// re-binds the game's root signatures and arguments first, so pipeline and root
// signature always match.
template <typename T>
T* mip_swap_locked(int level, T* obj, bool pso, bool* untwinned)
{
    if (level < 0 || !obj) return obj;
    const auto it = g_objs.find(canon(obj));
    if (it == g_objs.end()) {
        // Unknown pipeline: ours (host work, e.g. DLAA's own passes) is left alone; the
        // game's (a pipeline-library load) has no twin.
        if (pso && !t_host_work) *untwinned = true;
        return obj;
    }
    if (!it->second.twins) return obj; // its root signature needs no twins
    const auto& t = *it->second.twins;
    if (t.same[level]) return obj;
    if (t.obj[level]) return static_cast<T*>(t.obj[level]);
    *untwinned = true;
    return obj;
}
void mip_on_pso(GL* l, ID3D12PipelineState*& p)
{
    if (!g_mip_switched.load(std::memory_order_relaxed) || !p) return;
    bool fallback = false;
    {
        std::shared_lock<std::shared_mutex> lock(g_m);
        na_object_id lid = 0;
        ListState* ls = list_state_locked(l, &lid);
        if (!ls) return;
        std::lock_guard<std::mutex> bl(ls->bind_m);
        if (ls->mip_level < 0) return;
        bool untwinned = false;
        ID3D12PipelineState* t = mip_swap_locked(ls->mip_level, p, true, &untwinned);
        if (untwinned) { ls->mip_level = -1; fallback = true; }
        else p = t;
    }
    if (fallback) {
        g_mip_fallbacks.fetch_add(1, std::memory_order_relaxed);
        const HostWorkScope host;
        RestoreListState(l); // re-binds the game's own root signatures + arguments (latch now -1)
    }
}
void mip_on_rs(GL* l, ID3D12RootSignature*& r)
{
    if (!g_mip_switched.load(std::memory_order_relaxed) || !r) return;
    std::shared_lock<std::shared_mutex> lock(g_m);
    na_object_id lid = 0;
    ListState* ls = list_state_locked(l, &lid);
    if (!ls) return;
    std::lock_guard<std::mutex> bl(ls->bind_m);
    bool untwinned = false;
    r = mip_swap_locked(ls->mip_level, r, false, &untwinned);
}
void mip_on_reset(GL* l, ID3D12PipelineState*& p)
{
    if (!g_mip_switched.load(std::memory_order_relaxed)) return;
    const int live = g_mip_live_level.load(std::memory_order_relaxed), game = g_mip_game_level.load(std::memory_order_relaxed);
    std::shared_lock<std::shared_mutex> lock(g_m);
    na_object_id lid = 0;
    ListState* ls = list_state_locked(l, &lid);
    if (!ls) return;
    std::lock_guard<std::mutex> bl(ls->bind_m);
    ls->mip_level = static_cast<int8_t>(live != game ? live : -1);
    if (ls->mip_level >= 0 && p) {
        bool untwinned = false;
        ID3D12PipelineState* t = mip_swap_locked(ls->mip_level, p, true, &untwinned);
        if (untwinned) ls->mip_level = -1; // nothing bound yet: simply record with the game's objects
        else p = t;
    }
}

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
        // Live mip bias: substitute twins before forwarding (also for host re-binds).
        if constexpr (Slot == sl::list::SetPipelineState) { auto&& t = std::forward_as_tuple(a...); mip_on_pso(l, std::get<0>(t)); }
        if constexpr (Slot == sl::list::SetGraphicsRootSignature || Slot == sl::list::SetComputeRootSignature) {
            auto&& t = std::forward_as_tuple(a...);
            mip_on_rs(l, std::get<0>(t));
        }
        if constexpr (Slot == sl::list::Reset) { auto&& t = std::forward_as_tuple(a...); mip_on_reset(l, std::get<1>(t)); }
        if constexpr (std::is_void_v<R>) {
            if constexpr (Slot == sl::list::ResourceBarrier)
                if (scope.outermost && !scope.ambiguous) before_read_transition(l, a...);
            if constexpr (Slot == sl::list::CopyResource || Slot == sl::list::ResolveSubresource || Slot == sl::list::CopyTextureRegion) {
                if (scope.outermost && !scope.ambiguous) {
                    auto args = std::forward_as_tuple(a...);
                    if constexpr (Slot == sl::list::CopyResource) before_texture_copy(l, std::get<0>(args), std::get<1>(args));
                    else if constexpr (Slot == sl::list::ResolveSubresource) before_texture_copy(l, std::get<0>(args), std::get<2>(args));
                    else if (std::get<0>(args) && std::get<4>(args)) before_texture_copy(l, std::get<0>(args)->pResource, std::get<4>(args)->pResource);
                }
            }
            if constexpr (Slot == sl::list::DrawInstanced || Slot == sl::list::DrawIndexedInstanced || Slot == sl::list::Dispatch)
                if (scope.outermost && !scope.ambiguous) before_shader_use(l, Slot == sl::list::Dispatch);
            fn(l, a...);
            if constexpr (Slot == sl::list::CopyResource || Slot == sl::list::ResolveSubresource || Slot == sl::list::CopyTextureRegion) {
                if (scope.outermost && !scope.ambiguous) {
                    auto args = std::forward_as_tuple(a...);
                    if constexpr (Slot == sl::list::CopyTextureRegion) { if (std::get<0>(args)) after_texture_copy(l, std::get<0>(args)->pResource); }
                    else after_texture_copy(l, std::get<0>(args));
                }
            }
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
// Render passes: optional, own kind (snapshotted only through a List4 pointer).
bool register_list4_slots()
{
    namespace L = sl::list;
    const na::hooking::variants::SlotSpec specs[] = {
        ListHook<L::BeginRenderPass, void(UINT, const D3D12_RENDER_PASS_RENDER_TARGET_DESC*, const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*,
                                          D3D12_RENDER_PASS_FLAGS)>::spec(),
        ListHook<L::EndRenderPass, void()>::spec(),
    };
    return na::hooking::variants::Register(na::hooking::variants::Kind::CommandList4, specs,
                                           static_cast<uint32_t>(sizeof(specs) / sizeof(specs[0])));
}
// The List4 interface of a submitted list, noted for render-pass coverage.
void note_list4(ID3D12CommandList* list)
{
    if (!na::hooking::variants::Active(na::hooking::variants::Kind::CommandList4)) return;
    ID3D12GraphicsCommandList4* l4 = nullptr;
    if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&l4))) && l4) {
        na::hooking::variants::NoteObject(na::hooking::variants::Kind::CommandList4, l4);
        l4->Release();
    }
}

// ---------------------------------------------------- C9: CPU mappings
using MapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Resource*, UINT, const D3D12_RANGE*, void**);
using UnmapFn = void(STDMETHODCALLTYPE*)(ID3D12Resource*, UINT, const D3D12_RANGE*);
MapFn o_Map[2] = {};
UnmapFn o_Unmap[2] = {};

void note_map(ID3D12Resource* r, void* cpu)
{
    const na_object_id id = lookup(r);
    if (!id) return;
    std::unique_lock<std::shared_mutex> lock(g_m);
    auto it = g_buffers.find(id);
    if (it == g_buffers.end()) return;
    it->second.cpu = static_cast<uint8_t*>(cpu);
    it->second.ever_mapped = true;
    ++it->second.maps;
}
std::atomic<void (*)(void*)> g_unmap_probe{nullptr}; // tests: Unmap reached the contested lock
std::atomic<void*> g_unmap_probe_user{nullptr};
void note_unmap(ID3D12Resource* r)
{
    const na_object_id id = lookup(r);
    if (!id) return;
    if (auto probe = g_unmap_probe.load(std::memory_order_acquire)) probe(g_unmap_probe_user.load());
    std::unique_lock<std::shared_mutex> lock(g_m);
    auto it = g_buffers.find(id);
    if (it == g_buffers.end() || !it->second.maps) return;
    if (--it->second.maps == 0) it->second.cpu = nullptr;
}
template <int I>
HRESULT STDMETHODCALLTYPE D_Map(ID3D12Resource* r, UINT sub, const D3D12_RANGE* range, void** out)
{
    const HRESULT hr = o_Map[I](r, sub, range, out);
    if (!t_host_work && SUCCEEDED(hr) && sub == 0 && out && *out) note_map(r, *out); // host reads are not the app's mapping
    return hr;
}
// C9-H3-REVIEW-1 #1: the mapping is forgotten BEFORE the runtime unmaps it.
// note_unmap takes g_m exclusively, which waits for any snapshot copy holding
// g_m shared, so no copy can be reading the pointer when the original runs.
template <int I>
void STDMETHODCALLTYPE D_Unmap(ID3D12Resource* r, UINT sub, const D3D12_RANGE* range)
{
    if (sub == 0 && !t_host_work) note_unmap(r);
    o_Unmap[I](r, sub, range);
}

// Destruction of a still-mapped buffer is the one path whose ordering (the
// runtime frees the mapping vs. releases our sentinel) we cannot see. The copy
// is therefore guarded: a fault yields an UNRESOLVED snapshot, never a crash.
bool guarded_copy(void* dst, const void* src, size_t n)
{
#if defined(_MSC_VER)
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    std::memcpy(dst, src, n);
    return true;
#endif
}

std::atomic<uint64_t> g_snapshot_id{0};
std::atomic<void (*)(void*)> g_copy_probe{nullptr}; // tests: runs while a copy holds the mapping
std::atomic<void*> g_copy_probe_user{nullptr};

// Bounded copies of every constant-data binding of the submitted recordings,
// at the submission boundary. APP_SYNC_UNKNOWN always.
void snapshot_submission(na_object_id qid, uint64_t ticket, uint32_t count, ID3D12CommandList* const* lists)
{
    if (!na::observe_internal::Started() || !lists) return;
    constexpr uint32_t kMaxSnaps = 64, kMaxBytes = 4096, kBudget = 64 * 1024;
    struct Out {
        na_ev_snapshot e;
        uint8_t bytes[kMaxBytes];
    };
    static thread_local Out o; // 4 KiB: kept off the game's stack
    uint32_t snaps = 0, total = 0;
    bool budget_reported = false;
    for (uint32_t li = 0; li < count; ++li) {
        std::vector<ListState::Binding> binds;
        na_object_id lid = 0;
        uint64_t gen = NA_UNKNOWN_U64;
        {
            std::shared_lock<std::shared_mutex> lock(g_m);
            ListState* ls = lists[li] ? list_state_locked(lists[li], &lid) : nullptr;
            if (!ls) continue;
            gen = ls->generation_known.load() ? ls->generation.load() : NA_UNKNOWN_U64;
            std::lock_guard<std::mutex> bl(ls->bind_m);
            binds = ls->bindings;
        }
        for (const ListState::Binding& b : binds) {
            // C9-H3-REVIEW-1 #3: no de-duplication. Every binding keeps its own
            // list/generation/op/param association and requested size; the
            // count/byte budget is what bounds the cost.
            const bool over = snaps >= kMaxSnaps || total >= kBudget;
            if (over && budget_reported) return;
            std::memset(&o.e, 0, sizeof(o.e));
            o.e.hdr.kind = NA_EV_SNAPSHOT;
            o.e.hdr.thread_seq = ++t_seq;
            o.e.hdr.producer_thread = GetCurrentThreadId();
            o.e.snapshot_id = g_snapshot_id.fetch_add(1) + 1;
            o.e.queue = qid;
            o.e.queue_ticket = ticket;
            o.e.list = lid;
            o.e.recording_generation = gen;
            o.e.op_index = b.op_index;
            o.e.resource = b.resource;
            o.e.offset = b.offset;
            o.e.binding = b.kind;
            o.e.param = b.param;
            o.e.compute = b.compute;
            o.e.sync = NA_SNAP_SYNC_APP_UNKNOWN;
            o.e.requested_bytes = b.size;
            uint32_t n = 0;
            if (over) {
                o.e.status = NA_SNAP_BUDGET;
                budget_reported = true;
            } else if (!b.resource) {
                o.e.status = NA_SNAP_UNRESOLVED;
            } else {
                ID3D12Resource* reread = nullptr; // G3-02: host read of an app-unmapped buffer
                {
                    std::shared_lock<std::shared_mutex> lock(g_m); // Unmap waits for the copy
                    auto it = g_buffers.find(b.resource);
                    if (it == g_buffers.end()) o.e.status = NA_SNAP_UNRESOLVED;
                    else if (!it->second.cpu_visible) o.e.status = NA_SNAP_GPU_WRITTEN;
                    else if (b.offset >= it->second.width) o.e.status = NA_SNAP_UNRESOLVED;
                    else if (!it->second.cpu && !it->second.ever_mapped) o.e.status = NA_SNAP_NOT_MAPPED; // never written by the CPU
                    else {
                        const uint64_t avail = it->second.width - b.offset;
                        n = static_cast<uint32_t>(std::min<uint64_t>({b.size, kMaxBytes, avail, kBudget - total}));
                        if (it->second.cpu) {
                            if (auto probe = g_copy_probe.load(std::memory_order_acquire)) probe(g_copy_probe_user.load());
                            if (guarded_copy(o.bytes, it->second.cpu + b.offset, n)) {
                                o.e.status = NA_SNAP_CAPTURED;
                            } else {
                                n = 0;
                                o.e.status = NA_SNAP_UNRESOLVED;
                            }
                        } else {
                            // The app mapped, wrote and unmapped it before this
                            // submission (or mapped it before observation began).
                            // The resource is bound by a list being submitted, so
                            // it is alive: take a reference and map it read-only
                            // ourselves, outside g_m (Map/Unmap are free-threaded
                            // and reference counted by the runtime).
                            auto key = g_res_keys.find(b.resource);
                            if (key != g_res_keys.end())
                                static_cast<IUnknown*>(const_cast<void*>(key->second))->QueryInterface(IID_PPV_ARGS(&reread));
                            if (!reread) {
                                n = 0;
                                o.e.status = NA_SNAP_NOT_MAPPED;
                            }
                        }
                    }
                }
                if (reread) {
                    bool ok = false;
                    t_host_work = true; // not the application's mapping: no Map/Unmap bookkeeping
                    const D3D12_RANGE rr{static_cast<SIZE_T>(b.offset), static_cast<SIZE_T>(b.offset + n)};
                    void* p = nullptr;
                    if (SUCCEEDED(reread->Map(0, &rr, &p)) && p) {
                        ok = guarded_copy(o.bytes, static_cast<const uint8_t*>(p) + b.offset, n);
                        const D3D12_RANGE none{0, 0}; // nothing written by the CPU
                        reread->Unmap(0, &none);
                    }
                    t_host_work = false;
                    reread->Release();
                    if (ok) {
                        o.e.status = NA_SNAP_CAPTURED;
                        g_c.snap_reread.fetch_add(1);
                    } else {
                        n = 0;
                        o.e.status = NA_SNAP_NOT_MAPPED;
                    }
                }
            }
            o.e.captured_bytes = n;
            if (n < b.size && o.e.status == NA_SNAP_CAPTURED) o.e.hdr.flags |= NA_EVF_TRUNCATED;
            if (b.range_truncated) o.e.hdr.flags |= NA_EVF_TRUNCATED; // the table's later descriptors were not expanded
            o.e.payload_offset = n ? static_cast<uint32_t>(offsetof(Out, bytes)) : 0u;
            const uint32_t padded = (n + 7u) & ~7u;
            if (padded > n) std::memset(o.bytes + n, 0, padded - n);
            o.e.hdr.size_bytes = static_cast<uint32_t>(offsetof(Out, bytes) + padded);
            ++snaps;
            total += n;
            na::observe_internal::Emit(&o, NA_GROUP_SNAPSHOT);
        }
    }
}

void install_map_hooks(ID3D12Device* device)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 65536;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* committed = nullptr;
    ID3D12Resource* placed = nullptr;
    ID3D12Heap* heap = nullptr;
    device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&committed));
    D3D12_HEAP_DESC hd{};
    hd.SizeInBytes = 65536;
    hd.Properties.Type = D3D12_HEAP_TYPE_UPLOAD;
    hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    if (SUCCEEDED(device->CreateHeap(&hd, IID_PPV_ARGS(&heap))))
        device->CreatePlacedResource(heap, 0, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&placed));
    void* first_map = nullptr;
    int used = 0;
    for (ID3D12Resource* r : {committed, placed}) {
        if (!r || used >= 2) continue;
        void* target = (*reinterpret_cast<void***>(r))[sl::resource::Map];
        if (target == first_map) continue; // same implementation: already covered
        bool ok = false;
        if (used == 0)
            ok = na::hooking::HookVTableSlot(r, sl::resource::Map, reinterpret_cast<void*>(&D_Map<0>), reinterpret_cast<void**>(&o_Map[0])) &&
                 na::hooking::HookVTableSlot(r, sl::resource::Unmap, reinterpret_cast<void*>(&D_Unmap<0>), reinterpret_cast<void**>(&o_Unmap[0]));
        else
            ok = na::hooking::HookVTableSlot(r, sl::resource::Map, reinterpret_cast<void*>(&D_Map<1>), reinterpret_cast<void**>(&o_Map[1])) &&
                 na::hooking::HookVTableSlot(r, sl::resource::Unmap, reinterpret_cast<void*>(&D_Unmap<1>), reinterpret_cast<void**>(&o_Unmap[1]));
        if (ok) {
            if (!first_map) first_map = target;
            ++used;
            g_c.hooks.fetch_add(2);
        } else {
            g_c.missing.fetch_add(2);
        }
    }
    if (placed) placed->Release();
    if (heap) heap->Release();
    if (committed) committed->Release();
}

// ---------------------------------------------------------- C9b: samples
using ExecFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
std::atomic<ExecFn> g_exec_forward{nullptr}; // the game host's original ExecuteCommandLists

uint64_t now_ms() { return GetTickCount64(); }
constexpr uint32_t kStateUnknown = 0xFFFFFFFFu; // pending split barrier / invalidated evidence

struct Sample {
    uint64_t token = 0;
    na_sample_request req{};
    uint32_t status = NA_SAMPLE_ARMED, reason = NA_SAMPLE_OK;
    uint64_t armed_at = 0, ready_at = 0;
    na_session_id session = 0;
    na_epoch epoch = 0;
    ID3D12Resource* source = nullptr; // held until the host fence completes (C9B-R2)
    ID3D12Resource* readback = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    uint64_t fence_value = 1;
    bool signaled = false;            // the host fence signal was submitted
    std::vector<uint8_t> data;        // READY: owned host copy (C9B-R5)
    uint32_t row_pitch = 0, x = 0, y = 0, w = 0, h = 0, format = 0, state_before = 0;
    uint64_t bytes = 0;
    na_object_id queue = 0;
    uint64_t ticket = 0;
    bool released = false;            // consumer released; freed once the GPU is done
    bool buffer = false;              // C10: upload-heap buffer range (execution-time constant data)
    uint64_t offset = 0;              // buffer byte offset (x | y << 32)
    bool planar = false;              // packed depth-stencil: plane 0 (depth) copied
    bool order_ambiguous = false;     // another operation began on the queue before the copy was submitted
    bool gpu_objects() const { return readback || alloc || list || fence || source; }
};
// C9B-REVIEW-2 #1: the sample store is immortal (never destroyed), so the
// detached worker can never touch a destroyed mutex/vector during CRT
// teardown; entries still holding D3D objects at exit are deliberately leaked.
std::mutex& g_samples_m = *new std::mutex;
std::condition_variable& g_samples_cv = *new std::condition_variable;
std::vector<std::unique_ptr<Sample>>& g_samples = *new std::vector<std::unique_ptr<Sample>>;
std::atomic<uint64_t> g_next_token{1};
std::atomic<uint32_t> g_armed{0}; // armed requests: submissions on observed queues serialize while > 0
bool g_worker_started = false;               // under g_samples_m: exactly one worker per process
std::atomic<uint32_t> g_workers_started{0};  // stats
std::atomic<bool> g_samples_quit{false};     // set at teardown: the worker stops before any static it reads is destroyed
std::atomic<bool> g_worker_paused{false};    // test seam: no maintenance ticks
std::atomic<bool> g_fail_worker_start{false}; // test seam: thread creation fails
std::atomic<bool> g_fail_sample_signal{false}; // test seam: the host fence signal is refused after the copy was submitted
std::thread* g_worker = nullptr;             // immortal, never detached: teardown waits on its own handle
void (*g_acquire_probe)() = nullptr;         // test seam: after the game's submission returned, before servicing
constexpr size_t kMaxSamples = 8;     // active leases (released-in-flight included, quarantined excluded)
constexpr size_t kMaxSampleEntries = 64; // hard ceiling including quarantined entries (G3-07)
constexpr uint64_t kMaxSampleBytes = 64ull << 20;

void recount_armed_locked()
{
    uint32_t n = 0;
    for (auto& s : g_samples) n += s->status == NA_SAMPLE_ARMED ? 1u : 0u;
    g_armed.store(n, std::memory_order_release);
}

void free_gpu_objects(Sample& s)
{
    if (s.readback) s.readback->Release();
    if (s.list) s.list->Release();
    if (s.alloc) s.alloc->Release();
    if (s.fence) s.fence->Release();
    if (s.source) s.source->Release();
    s.readback = nullptr;
    s.list = nullptr;
    s.alloc = nullptr;
    s.fence = nullptr;
    s.source = nullptr;
}

void fail(Sample& s, uint32_t status, uint32_t reason)
{
    s.status = status;
    s.reason = reason;
    s.data.clear();
    s.data.shrink_to_fit();
}

// C9B-REVIEW-2 #3: scope is checked at every operation's linearization point
// (service, poll, read, worker), not only on the maintenance tick. Under
// g_samples_m. Returns whether the entry is still in its session/epoch; an
// out-of-scope live entry is failed on the spot.
bool scope_check_locked(Sample& s, na_session_id session, na_epoch epoch)
{
    const bool in_scope = session && session == s.session && epoch == s.epoch;
    if (!in_scope && (s.status == NA_SAMPLE_ARMED || s.status == NA_SAMPLE_READY || s.status == NA_SAMPLE_IN_FLIGHT))
        fail(s, NA_SAMPLE_FAILED, NA_SAMPLE_E_SESSION_ENDED);
    return in_scope;
}
bool scope_check_locked(Sample& s)
{
    na_session_id session = 0;
    na_epoch epoch = 0;
    na::observe_internal::CurrentScope(&session, &epoch);
    return scope_check_locked(s, session, epoch);
}

// Host-owned worker (C9B review): completion, readback, expiry, scope and
// freeing happen here, never inside a game submission callback.
void worker_tick_locked()
{
    const uint64_t now = now_ms();
    na_session_id session = 0;
    na_epoch epoch = 0;
    na::observe_internal::CurrentScope(&session, &epoch);
    for (auto it = g_samples.begin(); it != g_samples.end();) {
        Sample& s = **it;
        const bool in_scope = session && session == s.session && epoch == s.epoch;
        if (s.fence) {
            const uint64_t done = s.fence->GetCompletedValue();
            if (done == UINT64_MAX) { // device removed: nothing can still run
                free_gpu_objects(s);
                if (s.status == NA_SAMPLE_IN_FLIGHT || s.status == NA_SAMPLE_FAILED) fail(s, NA_SAMPLE_DEVICE_LOST, NA_SAMPLE_OK);
            } else if (s.signaled && done >= s.fence_value) {
                if (s.status == NA_SAMPLE_IN_FLIGHT && !s.released && in_scope) {
                    void* p = nullptr;
                    const D3D12_RANGE rr{0, static_cast<SIZE_T>(s.bytes)};
                    if (SUCCEEDED(s.readback->Map(0, &rr, &p)) && p) {
                        s.data.assign(static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + s.bytes);
                        const D3D12_RANGE none{0, 0};
                        s.readback->Unmap(0, &none);
                        s.status = NA_SAMPLE_READY;
                        s.ready_at = now;
                    } else {
                        fail(s, NA_SAMPLE_FAILED, NA_SAMPLE_E_HOST_OBJECTS);
                    }
                }
                free_gpu_objects(s);
            }
            // !signaled: quarantined (SUBMIT_FAILED) until the device is removed.
        }
        scope_check_locked(s, session, epoch);
        const uint32_t expiry = s.req.expiry_ms ? s.req.expiry_ms : 2000;
        if (s.status == NA_SAMPLE_READY && now - s.ready_at > expiry) fail(s, NA_SAMPLE_EXPIRED, NA_SAMPLE_OK);
        const uint32_t arm = s.req.arm_timeout_ms ? s.req.arm_timeout_ms : 5000;
        if (s.status == NA_SAMPLE_ARMED && now - s.armed_at > arm) fail(s, NA_SAMPLE_FAILED, NA_SAMPLE_E_TIMEOUT);
        if (s.released && !s.gpu_objects()) {
            it = g_samples.erase(it);
            continue;
        }
        ++it;
    }
    recount_armed_locked();
}

// Teardown quiescence (C9B-REVIEW-2 #1, C9B-REVIEW-3 #2). A function-local
// static constructed when the worker starts, i.e. after every namespace-scope
// static the worker reads, so it is destroyed before them. Its destructor
// returns only once the worker THREAD HAS EXITED, which is the proof that no
// tick is in progress or can start:
//  - worker already gone (the OS terminates every other thread before DLL
//    static destruction at process exit, possibly while it held the lock):
//    the handle is signaled, nothing to do, the lock is never touched;
//  - worker alive (EXE exit(), static destruction before ExitProcess): it is
//    not holding the lock forever, so we take it, set quit under it (no lost
//    wakeup against the predicate wait), and wait for the thread to exit.
struct WorkerQuiesce {
    ~WorkerQuiesce()
    {
        if (!g_worker) return;
        // The thread object is immortal and never detached, so its handle is
        // valid for the whole process: there is no failure path without one.
        const HANDLE h = static_cast<HANDLE>(g_worker->native_handle());
        if (WaitForSingleObject(h, 0) != WAIT_OBJECT_0) {
            {
                std::lock_guard<std::mutex> lock(g_samples_m);
                g_samples_quit.store(true, std::memory_order_release);
            }
            g_samples_cv.notify_all();
            WaitForSingleObject(h, INFINITE);
        }
    }
};

// False when the worker could not be started (the request is then refused).
bool ensure_worker_locked()
{
    if (g_worker_started) return true; // exactly once, whatever the thread object's state
    static WorkerQuiesce quiesce;      // constructed before the thread can exist
    (void)quiesce;
    auto body = [] {
        std::unique_lock<std::mutex> lock(g_samples_m);
        for (;;) {
            if (g_samples_quit.load(std::memory_order_acquire)) return;
            if (g_samples.empty()) g_samples_cv.wait(lock, [] { return !g_samples.empty() || g_samples_quit.load(); });
            else g_samples_cv.wait_for(lock, std::chrono::milliseconds(2));
            if (g_samples_quit.load(std::memory_order_acquire)) return;
            if (!g_worker_paused.load(std::memory_order_acquire)) worker_tick_locked();
        }
    };
    try {
        if (g_fail_worker_start.load()) throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
        g_worker = new std::thread(body); // immortal: never joined, detached or destroyed
    } catch (...) {
        g_worker = nullptr;
        return false; // not started: a later request retries
    }
    g_worker_started = true;
    g_workers_started.fetch_add(1);
    return true;
}

bool state_ok_for_queue(uint32_t state, D3D12_COMMAND_LIST_TYPE type)
{
    if (state == kStateUnknown || state == D3D12_RESOURCE_STATE_COMMON) return false; // pending split / implicit promotion
    if (type == D3D12_COMMAND_LIST_TYPE_DIRECT) return true;
    // Compute queues cannot transition graphics-only states.
    const uint32_t graphics_only = D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_DEPTH_READ |
                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
                                   D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_STREAM_OUT | D3D12_RESOURCE_STATE_RESOLVE_DEST |
                                   D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
    return type == D3D12_COMMAND_LIST_TYPE_COMPUTE && !(state & graphics_only);
}

bool block_compressed(DXGI_FORMAT f)
{
    return (f >= DXGI_FORMAT_BC1_TYPELESS && f <= DXGI_FORMAT_BC5_SNORM) || (f >= DXGI_FORMAT_BC6H_TYPELESS && f <= DXGI_FORMAT_BC7_UNORM_SRGB);
}
bool depth_format(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_D32_FLOAT || f == DXGI_FORMAT_D16_UNORM || f == DXGI_FORMAT_D24_UNORM_S8_UINT || f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
}
// C9B-REVIEW-2 #4: a depth-stencil resource is recognised by its flag, not its
// format: R32_TYPELESS / R16_TYPELESS with ALLOW_DEPTH_STENCIL carry the same
// whole-subresource copy restriction as the typed D formats.
bool depth_resource(const D3D12_RESOURCE_DESC& d)
{
    return depth_format(d.Format) || (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
}

// The copyable footprint of plane 0 is R32_TYPELESS for BOTH packed families,
// but the bits differ (24-bit UNORM in the low bits vs a 32-bit float), so the
// sample reports an unambiguous format describing the copied bytes.
DXGI_FORMAT depth_plane_format(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
        return DXGI_FORMAT_R32_FLOAT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

// Validates the request against the resource (C9B-R6). Fills the region.
uint32_t validate_locked(Sample& s, ID3D12Device* dev, const D3D12_RESOURCE_DESC& d)
{
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.SampleDesc.Count != 1) return NA_SAMPLE_E_UNSUPPORTED;
    if (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) return NA_SAMPLE_E_UNSUPPORTED; // implicit decay
    D3D12_FEATURE_DATA_FORMAT_INFO fi{d.Format, 0};
    if (FAILED(dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &fi, sizeof(fi)))) return NA_SAMPLE_E_UNSUPPORTED;
    // Packed depth-stencil (D24S8, D32S8X24 and their typeless families): the
    // request names a plane-0 (depth) subresource; the stencil plane is never
    // copied or transitioned. Any other multi-plane format is refused.
    if (fi.PlaneCount != 1 && !(fi.PlaneCount == 2 && depth_resource(d))) return NA_SAMPLE_E_UNSUPPORTED;
    s.planar = fi.PlaneCount == 2;
    const uint32_t mips = d.MipLevels ? d.MipLevels : 1;
    if (s.req.subresource >= mips * static_cast<uint32_t>(d.DepthOrArraySize)) return NA_SAMPLE_E_UNSUPPORTED;
    const uint32_t mip = s.req.subresource % mips;
    const uint32_t sub_w = static_cast<uint32_t>(std::max<uint64_t>(1, d.Width >> mip)), sub_h = std::max<uint32_t>(1, d.Height >> mip);
    const bool whole = s.req.width == 0;
    if (!whole && (s.req.x >= sub_w || s.req.y >= sub_h || !s.req.height)) return NA_SAMPLE_E_UNSUPPORTED;
    s.x = whole ? 0 : s.req.x;
    s.y = whole ? 0 : s.req.y;
    s.w = whole ? sub_w : std::min(s.req.width, sub_w - s.x);
    s.h = whole ? sub_h : std::min(s.req.height, sub_h - s.y);
    const bool partial = s.x || s.y || s.w != sub_w || s.h != sub_h;
    if (depth_resource(d) && partial) return NA_SAMPLE_E_UNSUPPORTED; // depth/stencil: whole subresource only
    if (block_compressed(d.Format)) { // 4x4 blocks: origin aligned, extent aligned unless it ends at the edge
        const auto aligned = [](uint32_t v) { return (v & 3u) == 0; };
        if (!aligned(s.x) || !aligned(s.y) || (!aligned(s.w) && s.x + s.w != sub_w) || (!aligned(s.h) && s.y + s.h != sub_h))
            return NA_SAMPLE_E_UNSUPPORTED;
    }
    return NA_SAMPLE_OK;
}

// Records and submits the host's copy for one armed sample (caller holds the
// queue's submission lock and g_samples_m). `state` is established evidence.
// `res` is the reference acquired BEFORE the game's submission (see
// acquire_sample_sources); ownership passes to the sample.
// C10: execution-time constant data. Upload-heap buffers are permanently
// GENERIC_READ (no transition) and cannot be written by the GPU. The copy is
// queued right after the game's submission and before the application can
// signal its own fence for that work, so the application may not overwrite
// the range until the copy has run: the bytes are those the game's GPU work
// read, unlike the APP_SYNC_UNKNOWN CPU snapshot taken at submission.
void start_buffer_copy_locked(Sample& s, ID3D12Resource* res, ID3D12Device* dev, ID3D12CommandQueue* q, D3D12_COMMAND_LIST_TYPE type,
                              na_object_id qid, uint64_t ticket)
{
    const D3D12_RESOURCE_DESC d = res->GetDesc();
    D3D12_HEAP_PROPERTIES hp{};
    D3D12_HEAP_FLAGS hf{};
    const uint64_t len = s.req.width;
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || FAILED(res->GetHeapProperties(&hp, &hf)) || hp.Type != D3D12_HEAP_TYPE_UPLOAD ||
        s.offset > d.Width || len > d.Width - s.offset) {
        dev->Release();
        free_gpu_objects(s);
        return fail(s, NA_SAMPLE_FAILED, NA_SAMPLE_E_UNSUPPORTED);
    }
    t_host_work = true;
    D3D12_HEAP_PROPERTIES rp{};
    rp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = len;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bool ok = SUCCEEDED(dev->CreateCommittedResource(&rp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s.readback))) &&
              SUCCEEDED(dev->CreateCommandAllocator(type, IID_PPV_ARGS(&s.alloc))) &&
              SUCCEEDED(dev->CreateCommandList(0, type, s.alloc, nullptr, IID_PPV_ARGS(&s.list))) &&
              SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.fence)));
    bool submitted = false;
    if (ok) {
        s.list->CopyBufferRegion(s.readback, 0, res, s.offset, len);
        ok = SUCCEEDED(s.list->Close());
    }
    if (ok) {
        ID3D12CommandList* ls[] = {s.list};
        ExecFn fwd = g_exec_forward.load(std::memory_order_acquire);
        if (!fwd) fwd = o_ExecuteCommandLists;
        if (fwd) fwd(q, 1, ls);
        else q->ExecuteCommandLists(1, ls);
        submitted = true;
        // An operation that began before this point may precede the copy.
        if (!ordering_window_intact()) s.order_ambiguous = true;
        s.signaled = !g_fail_sample_signal.load() && SUCCEEDED(q->Signal(s.fence, s.fence_value));
    }
    t_host_work = false;
    dev->Release();
    if (!submitted) {
        free_gpu_objects(s);
        return fail(s, NA_SAMPLE_FAILED, NA_SAMPLE_E_HOST_OBJECTS);
    }
    s.x = static_cast<uint32_t>(s.offset);
    s.y = static_cast<uint32_t>(s.offset >> 32);
    s.w = static_cast<uint32_t>(len);
    s.h = 1;
    s.row_pitch = static_cast<uint32_t>(len);
    s.bytes = len;
    s.format = DXGI_FORMAT_UNKNOWN;
    s.queue = qid;
    s.ticket = ticket;
    s.state_before = D3D12_RESOURCE_STATE_GENERIC_READ;
    if (!s.signaled) return fail(s, NA_SAMPLE_FAILED, NA_SAMPLE_E_SUBMIT_FAILED); // quarantined, see worker
    if (s.order_ambiguous) return fail(s, NA_SAMPLE_FAILED, NA_SAMPLE_E_ORDER_AMBIGUOUS); // objects stay until the fence
    s.status = NA_SAMPLE_IN_FLIGHT;
}

void start_copy_locked(Sample& s, ID3D12Resource* res, ID3D12CommandQueue* q, D3D12_COMMAND_LIST_TYPE type, uint32_t state, na_object_id qid,
                       uint64_t ticket)
{
    s.source = res; // held until the host fence completes
    ID3D12Device* dev = nullptr;
    q->GetDevice(IID_PPV_ARGS(&dev));
    if (!dev) return fail(s, NA_SAMPLE_FAILED, NA_SAMPLE_E_HOST_OBJECTS), free_gpu_objects(s);
    if (s.buffer) return start_buffer_copy_locked(s, res, dev, q, type, qid, ticket);
    const D3D12_RESOURCE_DESC d = res->GetDesc();
    if (const uint32_t why = validate_locked(s, dev, d)) {
        dev->Release();
        free_gpu_objects(s);
        return fail(s, NA_SAMPLE_FAILED, why);
    }
    const bool whole = !(s.x || s.y || s.w != std::max<uint64_t>(1, d.Width >> (s.req.subresource % (d.MipLevels ? d.MipLevels : 1))) ||
                         s.h != std::max<uint32_t>(1, d.Height >> (s.req.subresource % (d.MipLevels ? d.MipLevels : 1))));
    // Exact footprint: the subresource itself when whole, else a region desc.
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0;
    UINT64 row_bytes = 0, total = 0;
    if (whole) {
        dev->GetCopyableFootprints(&d, s.req.subresource, 1, 0, &fp, &rows, &row_bytes, &total);
    } else {
        D3D12_RESOURCE_DESC region = d;
        region.Width = s.w;
        region.Height = s.h;
        region.MipLevels = 1;
        region.DepthOrArraySize = 1;
        region.Flags = D3D12_RESOURCE_FLAG_NONE;
        dev->GetCopyableFootprints(&region, 0, 1, 0, &fp, &rows, &row_bytes, &total);
    }
    fp.Offset = 0;
    bool ok = total && total != UINT64_MAX && total <= kMaxSampleBytes;
    t_host_work = true;
    if (ok) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ok = SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s.readback))) &&
             SUCCEEDED(dev->CreateCommandAllocator(type, IID_PPV_ARGS(&s.alloc))) &&
             SUCCEEDED(dev->CreateCommandList(0, type, s.alloc, nullptr, IID_PPV_ARGS(&s.list))) &&
             SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.fence)));
    }
    bool submitted = false;
    if (ok) {
        const bool transition = state != D3D12_RESOURCE_STATE_COPY_SOURCE;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = res;
        b.Transition.Subresource = s.req.subresource;
        b.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(state);
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        if (transition) s.list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = s.readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        src.pResource = res;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = s.req.subresource;
        const D3D12_BOX box{s.x, s.y, 0, s.x + s.w, s.y + s.h, 1};
        s.list->CopyTextureRegion(&dst, 0, 0, 0, &src, whole ? nullptr : &box); // null box for whole (depth rule)
        if (transition) {
            std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
            s.list->ResourceBarrier(1, &b); // the game finds the state it left
        }
        ok = SUCCEEDED(s.list->Close());
    }
    if (ok) {
        ID3D12CommandList* ls[] = {s.list};
        ExecFn fwd = g_exec_forward.load(std::memory_order_acquire);
        if (!fwd) fwd = o_ExecuteCommandLists;
        if (fwd) fwd(q, 1, ls);
        else q->ExecuteCommandLists(1, ls);
        submitted = true;
        // An operation that began before this point may precede the copy.
        if (!ordering_window_intact()) s.order_ambiguous = true;
        s.signaled = !g_fail_sample_signal.load() && SUCCEEDED(q->Signal(s.fence, s.fence_value));
    }
    t_host_work = false;
    dev->Release();
    if (!submitted) { // nothing reached the GPU: safe to free now
        free_gpu_objects(s);
        return fail(s, NA_SAMPLE_FAILED, NA_SAMPLE_E_HOST_OBJECTS);
    }
    s.row_pitch = fp.Footprint.RowPitch;
    s.bytes = total;
    s.format = s.planar ? depth_plane_format(d.Format) : d.Format;
    s.queue = qid;
    s.ticket = ticket;
    s.state_before = state;
    if (!s.signaled) return fail(s, NA_SAMPLE_FAILED, NA_SAMPLE_E_SUBMIT_FAILED); // quarantined, see worker
    if (s.order_ambiguous) return fail(s, NA_SAMPLE_FAILED, NA_SAMPLE_E_ORDER_AMBIGUOUS); // objects stay until the fence
    s.status = NA_SAMPLE_IN_FLIGHT;
}

// A submission's evidence, captured from its recordings BEFORE it is forwarded
// (Gate 3 review PR40 #2: a list may be reset as soon as ExecuteCommandLists
// returns, so nothing is reread afterwards). Shared by sampling (Begin/End)
// and the known-state store. Every accumulation is bounded before allocation.
struct SubmissionCapture {
    bool lists = false;    // the submission carried command lists
    bool notes_ok = false; // C9B-R4: every list observed, fully covered, evidence complete
    bool uses_ok = false;  // every referenced resource recorded
    uint32_t reject = 0;   // first reason notes_ok is false: 1 missing recording, 2 partial coverage, 3 partial states
    std::vector<ListState::StateNote> notes;
    std::vector<ListState::Binding> binds;
    std::vector<na_object_id> uses; // sorted, unique
    struct Mark {
        na_object_id id;
        uint64_t seq;
        bool alone; // no other submission held a mark when this one was taken
    };
    std::vector<Mark> marks; // under g_known_m
    uint64_t epoch = 0;
    bool in_store = false, tainted = false, unknown_marked = false, copy_queue = false;
    uint32_t SelfMarks(na_object_id id) const
    {
        uint32_t n = 0;
        for (const auto& m : marks) n += m.id == id;
        return n;
    }
};
constexpr size_t kMaxSubmissionNotes = 65536, kMaxSubmissionBinds = 16384, kMaxSubmissionUses = 65536;

void capture_submission(uint32_t count, ID3D12CommandList* const* lists, SubmissionCapture& c)
{
    if (!lists || !count) return;
    c.lists = true;
    bool notes_ok = true, uses_ok = true;
    const auto reject = [&](uint32_t why) {
        if (notes_ok) c.reject = why;
        notes_ok = uses_ok = false;
    };
    std::shared_lock<std::shared_mutex> rl(g_m);
    for (uint32_t i = 0; i < count; ++i) {
        na_object_id lid = 0;
        ListState* ls = lists[i] ? list_state_locked(lists[i], &lid) : nullptr;
        if (!ls || !ls->recording_observed.load()) {
            reject(1);
            continue;
        }
        if (!ls->coverage_complete.load()) reject(2);
        std::lock_guard<std::mutex> bl(ls->bind_m);
        if (!ls->evidence_complete || c.notes.size() + ls->states.size() > kMaxSubmissionNotes) {
            if (notes_ok) c.reject = 3;
            notes_ok = false;
        } else if (notes_ok) {
            c.notes.insert(c.notes.end(), ls->states.begin(), ls->states.end());
        }
        const size_t room = kMaxSubmissionBinds - std::min(kMaxSubmissionBinds, c.binds.size());
        c.binds.insert(c.binds.end(), ls->bindings.begin(), ls->bindings.begin() + std::min(room, ls->bindings.size()));
        if (!ls->uses_complete || c.uses.size() + ls->uses.size() > kMaxSubmissionUses) uses_ok = false;
        else if (uses_ok) c.uses.insert(c.uses.end(), ls->uses.begin(), ls->uses.end());
    }
    if (!notes_ok) c.notes.clear();
    std::sort(c.uses.begin(), c.uses.end());
    c.uses.erase(std::unique(c.uses.begin(), c.uses.end()), c.uses.end());
    c.notes_ok = notes_ok;
    c.uses_ok = uses_ok;
}

// Begin, before forwarding: mark what this submission may change.
void known_mark(SubmissionCapture& c, bool copy_queue)
{
    if (!c.lists) return; // queue sync operations change no state
    std::lock_guard<std::mutex> lock(g_known_m);
    c.in_store = true;
    c.copy_queue = copy_queue;
    if (!c.notes_ok || (copy_queue && !c.uses_ok)) { // may transition/decay anything
        known_clear_locked();
        ++g_unknown_inflight;
        c.unknown_marked = true;
        return;
    }
    c.epoch = g_known_epoch;
    c.tainted = g_unknown_inflight > 0;
    std::vector<na_object_id> ids;
    ids.reserve(c.notes.size() + (copy_queue ? c.uses.size() : 0));
    for (const auto& n : c.notes) ids.push_back(n.resource);
    if (copy_queue) ids.insert(ids.end(), c.uses.begin(), c.uses.end());
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    for (na_object_id id : ids) {
        auto it = g_known.find(id);
        if (it == g_known.end()) continue; // not eligible
        ResKnown& k = it->second;
        c.marks.push_back({id, ++k.mark_seq, k.in_flight == 0});
        ++k.in_flight;
    }
}

// End, after forwarding: publish the captured transitions where provably ordered.
void known_publish(SubmissionCapture& c, na_object_id qid, bool ambiguous)
{
    if (!c.in_store) return;
    std::lock_guard<std::mutex> lock(g_known_m);
    if (c.unknown_marked) {
        --g_unknown_inflight;
        return;
    }
    const bool valid = !ambiguous && !c.tainted && c.epoch == g_known_epoch && !c.copy_queue;
    std::vector<na_object_id> good;
    for (const auto& m : c.marks) {
        auto it = g_known.find(m.id);
        if (it == g_known.end()) continue; // retired meanwhile
        if (valid && m.alone && it->second.mark_seq == m.seq) good.push_back(m.id);
        else known_reset_locked(it->second); // overlapped / ambiguous / cleared / copy-queue decay
    }
    std::sort(good.begin(), good.end());
    std::vector<na_object_id> claimed; // ids whose queue ownership was settled in this publication
    for (const auto& n : c.notes) {
        if (!std::binary_search(good.begin(), good.end(), n.resource)) continue;
        ResKnown& k = g_known[n.resource];
        if (std::find(claimed.begin(), claimed.end(), n.resource) == claimed.end()) {
            claimed.push_back(n.resource);
            if (k.queue && k.queue != qid) known_reset_locked(k); // another queue's timeline: start over
            k.queue = qid;
        }
        if (n.subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
            k.has_all = true;
            k.all = n.state_after;
            k.subs.clear();
        } else if (k.subs.size() >= kMaxKnownSubs && !k.subs.count(n.subresource)) {
            known_reset_locked(k);
            k.queue = qid;
        } else {
            k.subs[n.subresource] = n.state_after;
        }
    }
    for (const auto& m : c.marks) {
        auto it = g_known.find(m.id);
        if (it != g_known.end() && it->second.in_flight) --it->second.in_flight;
    }
}

// A buffer sample is eligible when the submission bound a range containing it.
bool binding_covers(const std::vector<ListState::Binding>& binds, const Sample& s)
{
    for (const auto& b : binds)
        if (b.resource == s.req.resource && b.offset <= s.offset && s.req.width <= b.size && s.offset - b.offset <= b.size - s.req.width) return true;
    return false;
}

bool last_state(const std::vector<ListState::StateNote>& notes, const Sample& s, uint32_t* state)
{
    bool found = false;
    for (const auto& n : notes)
        if (n.resource == s.req.resource && (n.subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES || n.subresource == s.req.subresource)) {
            found = true;
            *state = n.state_after;
        }
    return found;
}

bool sample_eligible(const Sample& s, const SubmissionCapture& c, D3D12_COMMAND_LIST_TYPE type, na_object_id qid, uint32_t* state,
                     bool* from_known = nullptr)
{
    if (s.buffer) {
        *state = D3D12_RESOURCE_STATE_GENERIC_READ;
        return binding_covers(c.binds, s);
    }
    *state = kStateUnknown;
    if (!c.notes_ok) return false; // unobserved/incomplete recordings may have transitioned it
    if (last_state(c.notes, s, state)) return state_ok_for_queue(*state, type);
    // G3-06: no transition in this (complete) submission. A carried state is
    // used only for a resource this submission references, so the application
    // is obliged to keep it alive while we take our reference (review PR40 #1).
    if (!std::binary_search(c.uses.begin(), c.uses.end(), s.req.resource)) return false;
    if (!known_state(s.req.resource, s.req.subresource, qid, c.SelfMarks(s.req.resource), state) || !state_ok_for_queue(*state, type)) return false;
    if (from_known) *from_known = true;
    return true;
}

// Source references acquired before the game's submission is forwarded
// (C9B-REVIEW-2 #2), owned by the submitting thread until ExecuteEnd.
struct PreRef {
    uint64_t token;
    ID3D12Resource* res;
};
thread_local std::vector<PreRef> t_prerefs;

void drop_prerefs()
{
    for (auto& p : t_prerefs) p.res->Release();
    t_prerefs.clear();
}

// ExecuteBegin, under the queue's submission lock and BEFORE the original
// ExecuteCommandLists runs. Every candidate resource is referenced by a
// recorded barrier in a list being submitted, and that work cannot have run
// yet, so the application is obliged to keep the resource alive right now:
// taking our own reference here is defined. After the forward returns it no
// longer is: the GPU may finish and another thread may observe its fence and
// release the resource before we could acquire it.
void acquire_sample_sources(ID3D12CommandQueue* q, na_object_id qid, D3D12_COMMAND_LIST_TYPE type, const SubmissionCapture& cap)
{
    drop_prerefs();
    if (!q || g_armed.load(std::memory_order_acquire) == 0) return;
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COMPUTE) return;
    if (!cap.lists) return;
    if (cap.reject == 1) ++g_c.sample_missing_recording;
    else if (cap.reject == 2) ++g_c.sample_partial_coverage;
    else if (cap.reject == 3) ++g_c.sample_partial_states;
    std::vector<std::pair<uint64_t, na_object_id>> want;
    {
        na_session_id session = 0;
        na_epoch epoch = 0;
        na::observe_internal::CurrentScope(&session, &epoch);
        std::lock_guard<std::mutex> lock(g_samples_m);
        for (auto& sp : g_samples) {
            Sample& s = *sp;
            if (s.status != NA_SAMPLE_ARMED || s.released || !scope_check_locked(s, session, epoch)) continue;
            uint32_t state = kStateUnknown;
            if (sample_eligible(s, cap, type, qid, &state)) want.push_back({s.token, s.req.resource});
            else ++g_c.sample_no_transition;
        }
        recount_armed_locked();
    }
    if (want.empty()) return;
    std::shared_lock<std::shared_mutex> lock(g_m);
    for (const auto& w : want) {
        auto it = g_res_keys.find(w.second);
        ID3D12Resource* res = nullptr;
        if (it != g_res_keys.end()) static_cast<IUnknown*>(const_cast<void*>(it->second))->QueryInterface(IID_PPV_ARGS(&res));
        if (res) t_prerefs.push_back({w.first, res});
    }
}

// End of a clean (non-ambiguous) game submission, under the queue's
// submission lock: copies for every armed sample whose state this
// submission's evidence establishes (C9B-R4) and whose source was acquired
// before the submission (C9B-REVIEW-2 #2).
void service_samples(ID3D12CommandQueue* q, QueueState* qs, na_object_id qid, uint64_t ticket, D3D12_COMMAND_LIST_TYPE type, const SubmissionCapture& cap)
{
    if (t_prerefs.empty()) return;
    if (g_acquire_probe) g_acquire_probe();
    t_service_qs = qs;
    t_service_ticket = ticket;
    struct Reset { ~Reset() { t_service_qs = nullptr; } } reset;
    if (!ordering_window_intact()) return; // another operation began on this queue: no copy (stays armed)
    // Eligibility uses the evidence captured before the forward (the lists may
    // already be reset); unused prerefs are dropped by the caller.
    na_session_id session = 0;
    na_epoch epoch = 0;
    na::observe_internal::CurrentScope(&session, &epoch);
    std::lock_guard<std::mutex> lock(g_samples_m);
    for (auto& sp : g_samples) {
        if (!ordering_window_intact()) break;
        Sample& s = *sp;
        if (s.status != NA_SAMPLE_ARMED || s.released || !scope_check_locked(s, session, epoch)) continue;
        auto pre = std::find_if(t_prerefs.begin(), t_prerefs.end(), [&](const PreRef& p) { return p.token == s.token; });
        if (pre == t_prerefs.end()) continue; // armed after the submission began: no safe reference, stays armed
        uint32_t state = kStateUnknown;
        bool from_known = false;
        if (!sample_eligible(s, cap, type, qid, &state, &from_known)) continue;
        if (from_known) g_c.sample_known_state.fetch_add(1);
        ID3D12Resource* res = pre->res;
        t_prerefs.erase(pre);
        start_copy_locked(s, res, q, type, state, qid, ticket);
    }
    recount_armed_locked();
    g_samples_cv.notify_one();
}

std::once_flag g_install_once;
bool g_install_ok = false;

std::vector<void*> g_targets; // install thread only
char g_shared_report[256] = {};

// Vtable-ENTRY patching (not function patching): the device and queue
// vtables are static tables in the runtime, shared by every object of the
// implementation. Replacing the entry leaves the runtime's code untouched
// (WARP crashed in an unhooked method after prologue patching of its
// neighbours) and keeps the runtime's internal direct calls unobserved,
// which is exactly the game-facing surface H1 describes.
struct Patched {
    void** entry;
    void* original;
};
std::vector<Patched> g_patched; // install thread only: rollback on a failed installation

bool write_entry(void** entry, void* value)
{
    DWORD old = 0;
    if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &old)) return false;
    InterlockedExchangePointer(entry, value);
    DWORD ignored = 0;
    VirtualProtect(entry, sizeof(void*), old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), entry, sizeof(void*));
    return true;
}

bool patch_entry(void** entry, void* detour, void** orig)
{
    DWORD old = 0;
    if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *orig = *entry;
    g_patched.push_back({entry, *orig});
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

// Review: a failed installation must not leave a partial set of patched
// entries behind (calls would reach detours whose siblings never ran).
void rollback_patches()
{
    for (auto it = g_patched.rbegin(); it != g_patched.rend(); ++it) write_entry(it->entry, it->original);
    g_patched.clear();
    g_c.hooks.store(0);
}

// Review: runtime tables now point into this module, so it must never unload
// while the process lives. Pin it once any entry is patched.
void pin_module()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&pin_module), &self);
}

template <typename F>
void* fn(F f) { return reinterpret_cast<void*>(f); }

} // namespace

namespace {
std::atomic<bool> g_requested{false};
std::atomic<bool> g_active{false};
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
        if (!ok) rollback_patches();
        else pin_module();
        if (ok) {
            // Remember the patched tables: a device or queue whose table differs
            // (another implementation, a wrapping layer) is reported, not hidden.
            g_device_table = *reinterpret_cast<void***>(device);
            g_queue_table = *reinterpret_cast<void***>(queue);
        }
        if (d2) d2->Release();
        if (d4) d4->Release();
        if (d8) d8->Release();
        // C8b: command-list methods live in per-implementation (often per-list)
        // tables; the variant registry hooks their concrete functions.
        if (ok) {
            const bool reg = na::hooking::Initialize() && register_list_slots();
            if (reg) register_list4_slots(); // optional: render-pass observation
            // G3-07: the device hooks are live; host warm-up/probe objects
            // are not the game's (no ids, sentinels, events or counters).
            t_host_work = true;
            if (reg) {
                na::hooking::variants::WarmUpCommandLists(device);
                na::hooking::variants::StartDiscoveryThread(nullptr, nullptr);
            }
            g_lists_hooked = reg;
            // C9: Map/Unmap of buffers, hooked on the implementations a
            // committed and a placed upload buffer use (variants get own thunks).
            if (reg) install_map_hooks(device);
            t_host_work = false;
        }
        if (ok) na::observe_internal::SetSnapshotProvider(&snapshot);
        g_install_ok = ok;
    });
    if (g_install_ok) g_active.store(true, std::memory_order_release);
    return g_install_ok;
}

ExecuteToken ExecuteBegin(ID3D12CommandQueue* queue, uint32_t count, ID3D12CommandList* const* lists)
{
    ExecuteToken t;
    na_object_id qid = 0;
    auto qs = queue ? queue_state(queue, &qid) : nullptr;
    if (!qs) {
        if (lists && count && !t_host_work) g_known_disabled.store(true); // its transitions are unobservable
        return t;
    }
    // A runtime may re-enter Wait/Signal (on this or another queue) while
    // forwarding a submission on this thread. Observe the nested occurrence
    // without taking any submission lock or replacing the outer pre-submit
    // references (ASTRA-P3-R8; same-queue nesting is also ambiguous through
    // QueueOccurrences). Nested occurrences never service samples.
    if (t_submit_depth > 0 && !t_host_work) {
        g_c.nested_unlocked.fetch_add(1);
        t.nested_unlocked = true;
    }
    // Evidence for sampling and the known-state store, captured before the
    // original runs (review PR40 #2); the store marks what it may change.
    std::shared_ptr<SubmissionCapture> cap; // none for queue sync operations and host work
    D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (!t_host_work && lists && count) {
        cap = std::make_shared<SubmissionCapture>();
        type = queue->GetDesc().Type;
        capture_submission(count, lists, *cap);
        known_mark(*cap, type == D3D12_COMMAND_LIST_TYPE_COPY);
    }
    t.capture = cap;
    t.prev_cap = static_cast<const void*>(t_cur_cap);
    t.prev_qid = t_cur_qid;
    t_cur_cap = cap.get();
    t_cur_qid = qid;
    t.cur_set = true;
    t.list_type = static_cast<uint32_t>(type);
    if (!t_host_work && t_submit_depth == 0 && g_armed.load(std::memory_order_acquire)) { // C9B-R3
        qs->submit_m.lock();
        ++t_submit_depth;
        t.submit_locked = true;
        static const SubmissionCapture kNoLists;
        acquire_sample_sources(queue, qid, type, cap ? *cap : kNoLists); // before the original runs (C9B-REVIEW-2 #2)
    }
    const auto occurrence = qs->occurrences.Begin();
    t.queue_state = qs;
    t.queue_id = qid;
    t.ticket = occurrence.ticket;
    t.overlap_gen = occurrence.generation;
    t.overlapped_at_entry = occurrence.overlapped;
    snapshot_submission(qid, t.ticket, count, lists);
    return t;
}

void ExecuteEnd(const ExecuteToken& t, ID3D12CommandQueue* queue, uint32_t count, ID3D12CommandList* const* lists)
{
    if (t.cur_set) {
        t_cur_cap = static_cast<const SubmissionCapture*>(t.prev_cap);
        t_cur_qid = t.prev_qid;
    }
    if (!t.queue_state) return;
    if (g_lists_hooked && lists)
        for (uint32_t i = 0; i < count; ++i)
            if (lists[i]) {
                na::hooking::variants::NoteObject(na::hooking::variants::Kind::CommandList, lists[i]);
                note_list4(lists[i]);
            }
    const bool ambiguous = end_occurrence(t);
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
    ex.e.queue = t.queue_id;
    ex.e.queue_ticket = t.ticket;
    ex.e.list_count = n;
    ex.e.refs_offset = offsetof(Exec, refs);
    ex.e.ref_stride = sizeof(na_list_ref);
    {
        std::shared_lock<std::shared_mutex> lock(g_m);
        for (uint32_t i = 0; i < n; ++i) {
            auto it = g_objs.find(canon(lists[i]));
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
    // C9b: only a clean occurrence whose submission we serialized is sampled;
    // nothing observed can run on this queue between it and the host copy.
    const auto cap = std::static_pointer_cast<SubmissionCapture>(t.capture);
    const auto type = static_cast<D3D12_COMMAND_LIST_TYPE>(t.list_type);
    if (!t_host_work && t.submit_locked && !ambiguous && cap)
        service_samples(queue, std::static_pointer_cast<QueueState>(t.queue_state).get(), t.queue_id, t.ticket, type, *cap);
    if (t.submit_locked) drop_prerefs(); // unused references (ambiguous, released, no evidence)
    if (cap) known_publish(*cap, t.queue_id, ambiguous); // G3-06, after servicing (pre-submission states)
    unlock_submission(t);
}

void RequestInstall(bool requested) { g_requested.store(requested); }
PresentToken PresentBegin(IDXGISwapChain* chain)
{
    PresentToken token;
    if (!chain || !ProducersActive()) return token;
    bool created = false;
    token.swapchain = ensure_registered(chain, NA_OBJ_SWAPCHAIN, &created);
    if (created) {
        na_ev_swapchain_created e;
        init_header(e, NA_EV_SWAPCHAIN_CREATED, NA_EVF_FOUNDATIONAL | NA_EVF_PREEXISTING | NA_EVF_METHOD_UNOBSERVED);
        e.swapchain = token.swapchain;
        // Creation queue, resize history and color-space calls were not observed.
        e.color_space = e.buffer_generation = NA_UNKNOWN_U32;
        e.buffer_count = e.format = e.width = e.height = NA_UNKNOWN_U32;
        DXGI_SWAP_CHAIN_DESC d{};
        if (SUCCEEDED(chain->GetDesc(&d))) {
            e.buffer_count=d.BufferCount;e.format=d.BufferDesc.Format;
            e.width=d.BufferDesc.Width;e.height=d.BufferDesc.Height;
        }
        remember_creation(chain,e);emit(e,NA_GROUP_SWAPCHAIN);
    }
    IDXGISwapChain3* sc3 = nullptr;
    if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3) {
        token.index=sc3->GetCurrentBackBufferIndex();sc3->Release();
    }
    const void* key=canon(chain);
    {
        std::unique_lock<std::shared_mutex> lock(g_m);
        auto it=g_objs.find(key);
        if(it!=g_objs.end() && it->second.id==token.swapchain) token.ordinal=++it->second.present_ordinal;
    }
    return token;
}
void PresentEnd(const PresentToken& token,uint32_t sync,uint32_t flags,int32_t result)
{
    if(!token.swapchain || !token.ordinal) return;
    na_ev_present e;init_header(e,NA_EV_PRESENT,0);
    e.swapchain=token.swapchain;e.present_ordinal=token.ordinal;e.hresult=result;
    e.sync_interval=sync;e.present_flags=flags;e.backbuffer_index=token.index;
    emit(e,NA_GROUP_SWAPCHAIN);
}
bool InstallRequested() { return g_requested.load(); }
bool ProducersActive() { return g_active.load(std::memory_order_acquire); }

na_object_id IdOf(const void* object) { return lookup(object); }

// ---------------------------------------------------------------- J2 live consumer support
// Called by the live DLAA path from inside a game submission (the lists are
// being submitted, so every resource they reference is alive). Bounded, no waits.
std::vector<LiveBinding> ListRootConstantBuffers(ID3D12CommandList* list)
{
    std::vector<LiveBinding> out;
    std::shared_lock<std::shared_mutex> rl(g_m);
    na_object_id lid = 0;
    ListState* ls = list ? list_state_locked(list, &lid) : nullptr;
    if (!ls || !ls->recording_observed.load()) return out;
    std::lock_guard<std::mutex> bl(ls->bind_m);
    out.reserve(std::min<size_t>(ls->bindings.size(), 4096));
    for (const auto& b : ls->bindings)
        if (b.resource && (b.kind == NA_SNAP_ROOT_CBV || b.kind == NA_SNAP_TABLE_CBV) && !b.compute)
            out.push_back({b.resource, b.offset, b.size, b.op_index, b.param, b.kind == NA_SNAP_TABLE_CBV});
    return out;
}
bool ListReferences(ID3D12CommandList* list, na_object_id id)
{
    if (!id) return false;
    std::shared_lock<std::shared_mutex> rl(g_m);
    na_object_id lid = 0;
    ListState* ls = list ? list_state_locked(list, &lid) : nullptr;
    if (!ls || !ls->recording_observed.load()) return false;
    std::lock_guard<std::mutex> bl(ls->bind_m);
    return std::find(ls->uses.begin(), ls->uses.end(), id) != ls->uses.end();
}
bool ListRendersTo(ID3D12CommandList* list, na_object_id color, na_object_id depth)
{
    if (!color || !depth) return false;
    std::shared_lock<std::shared_mutex> rl(g_m);
    na_object_id lid = 0;
    ListState* ls = list ? list_state_locked(list, &lid) : nullptr;
    if (!ls || !ls->recording_observed.load()) return false;
    std::lock_guard<std::mutex> bl(ls->bind_m);
    return std::any_of(ls->targets.begin(), ls->targets.end(), [&](const ListState::Target& t) { return t.same(color, depth); });
}
na::j2::InsertionDecision ListInsertionDecision(ID3D12CommandList* list, na_object_id color)
{
    std::shared_lock<std::shared_mutex> rl(g_m);
    na_object_id lid = 0;
    ListState* ls = list ? list_state_locked(list, &lid) : nullptr;
    if (!ls || !ls->recording_observed.load()) return {};
    std::lock_guard<std::mutex> bl(ls->bind_m);
    return ls->insertion.Analyze(color);
}
na::j2::ConsumerSignature ListConsumerSignature(ID3D12CommandList* list, na_object_id color)
{
    std::shared_lock<std::shared_mutex> rl(g_m);
    na_object_id lid = 0;
    ListState* ls = list ? list_state_locked(list, &lid) : nullptr;
    if (!ls || !ls->recording_observed.load()) return {};
    std::lock_guard<std::mutex> bl(ls->bind_m);
    return ls->insertion.AnalyzeConsumer(color);
}
bool ListColorEvents(ID3D12CommandList* list, na_object_id color, std::vector<na::j2::ColorEvent>& out)
{
    std::shared_lock<std::shared_mutex> rl(g_m);
    na_object_id lid = 0;
    ListState* ls = list ? list_state_locked(list, &lid) : nullptr;
    if (!ls || !ls->recording_observed.load()) return false;
    std::lock_guard<std::mutex> bl(ls->bind_m);
    return ls->insertion.ColorEvents(color, out);
}
void ResetDerived()
{
    std::lock_guard<std::mutex> lk(g_derived_m);
    g_derived_n.store(0, std::memory_order_release);
}
na::j2::LiveConsumer ListLiveConsumer(ID3D12CommandList* list, na_object_id color)
{
    std::shared_lock<std::shared_mutex> rl(g_m);
    na_object_id lid = 0;
    ListState* ls = list ? list_state_locked(list, &lid) : nullptr;
    if (!ls || !ls->recording_observed.load()) return {};
    std::lock_guard<std::mutex> bl(ls->bind_m);
    return ls->insertion.Live(color);
}
void SetTableBindCallback(TableBindCallback cb) { g_table_cb.store(cb, std::memory_order_release); }
bool ListCurrentTargets(ID3D12CommandList* list, na_object_id* rt, na_object_id* ds)
{
    std::shared_lock<std::shared_mutex> rl(g_m);
    na_object_id lid = 0;
    ListState* ls = list ? list_state_locked(list, &lid) : nullptr;
    if (!ls || !ls->recording_observed.load()) return false;
    std::lock_guard<std::mutex> bl(ls->bind_m);
    if (rt) *rt = ls->cur_rt;
    if (ds) *ds = ls->cur_ds;
    return true;
}
uint64_t ListRecording(ID3D12CommandList* list)
{
    std::shared_lock<std::shared_mutex> rl(g_m);
    na_object_id lid = 0;
    ListState* ls = list ? list_state_locked(list, &lid) : nullptr;
    if (!ls || !ls->recording_observed.load() || !ls->generation_known.load()) return 0;
    return ls->generation.load() + 1; // never 0
}
bool ListStateRestorable(ID3D12CommandList* l)
{
    std::shared_lock<std::shared_mutex> rl(g_m);
    na_object_id lid = 0;
    ListState* ls = l ? list_state_locked(l, &lid) : nullptr;
    if (!ls || !ls->recording_observed.load() || !ls->coverage_complete.load()) return false;
    std::lock_guard<std::mutex> bl(ls->bind_m);
    return ls->restorable && !ls->in_render_pass;
}
bool RestoreListState(ID3D12GraphicsCommandList* l)
{
    // Snapshot under the locks, issue the calls without them (these calls run
    // under the caller's HostWorkScope, so they are not observed as the game's).
    ID3D12PipelineState* pso = nullptr;
    ID3D12DescriptorHeap* heaps[2] = {};
    uint32_t nheaps = 0;
    const void* rs[2] = {};
    std::vector<ListState::RootArg> args[2];
    {
        std::shared_lock<std::shared_mutex> rl(g_m);
        na_object_id lid = 0;
        ListState* ls = l ? list_state_locked(l, &lid) : nullptr;
        if (!ls || !ls->recording_observed.load() || !ls->coverage_complete.load()) return false;
        std::lock_guard<std::mutex> bl(ls->bind_m);
        if (!ls->restorable || ls->in_render_pass) return false;
        pso = ls->pso;
        nheaps = ls->nheaps;
        heaps[0] = ls->heaps[0];
        heaps[1] = ls->heaps[1];
        rs[0] = ls->root_sig[0];
        rs[1] = ls->root_sig[1];
        args[0] = ls->args[0];
        args[1] = ls->args[1];
    }
    if (nheaps) l->SetDescriptorHeaps(nheaps, heaps);
    for (uint32_t c = 0; c < 2; ++c) {
        if (!rs[c]) continue;
        auto* sig = static_cast<ID3D12RootSignature*>(const_cast<void*>(rs[c]));
        if (c) l->SetComputeRootSignature(sig); else l->SetGraphicsRootSignature(sig);
        for (UINT p = 0; p < args[c].size(); ++p) {
            const ListState::RootArg& a = args[c][p];
            switch (a.type) {
            case 1: { D3D12_GPU_DESCRIPTOR_HANDLE h{a.value}; if (c) l->SetComputeRootDescriptorTable(p, h); else l->SetGraphicsRootDescriptorTable(p, h); break; }
            case 2: if (c) l->SetComputeRootConstantBufferView(p, a.value); else l->SetGraphicsRootConstantBufferView(p, a.value); break;
            case 3: if (c) l->SetComputeRootShaderResourceView(p, a.value); else l->SetGraphicsRootShaderResourceView(p, a.value); break;
            case 4: if (c) l->SetComputeRootUnorderedAccessView(p, a.value); else l->SetGraphicsRootUnorderedAccessView(p, a.value); break;
            case 5: if (!a.consts.empty()) { if (c) l->SetComputeRoot32BitConstants(p, static_cast<UINT>(a.consts.size()), a.consts.data(), 0); else l->SetGraphicsRoot32BitConstants(p, static_cast<UINT>(a.consts.size()), a.consts.data(), 0); } break;
            default: break;
            }
        }
    }
    if (pso) l->SetPipelineState(pso);
    return true;
}
bool ListClearsTarget(ID3D12CommandList* list, na_object_id target)
{
    std::shared_lock<std::shared_mutex> rl(g_m);
    na_object_id lid = 0;
    ListState* ls = list ? list_state_locked(list, &lid) : nullptr;
    if (!ls || !ls->recording_observed.load()) return false;
    std::lock_guard<std::mutex> bl(ls->bind_m);
    return std::find(ls->cleared.begin(), ls->cleared.end(), target) != ls->cleared.end();
}
bool ListSamplesTexture(ID3D12CommandList* list, na_object_id texture)
{
    std::shared_lock<std::shared_mutex> rl(g_m);
    na_object_id lid = 0;
    ListState* ls = list ? list_state_locked(list, &lid) : nullptr;
    if (!ls || !ls->recording_observed.load()) return false;
    std::lock_guard<std::mutex> bl(ls->bind_m);
    return std::find(ls->sampled_textures.begin(), ls->sampled_textures.end(), texture) != ls->sampled_textures.end();
}
bool ListSceneSubresource(ID3D12CommandList* list, na_object_id color, na_object_id depth, uint32_t* sub, uint32_t* depth_sub)
{
    if (!sub || !depth_sub) return false;
    std::shared_lock<std::shared_mutex> rl(g_m);
    na_object_id lid = 0;
    ListState* ls = list ? list_state_locked(list, &lid) : nullptr;
    if (!ls || !ls->recording_observed.load()) return false;
    std::lock_guard<std::mutex> bl(ls->bind_m);
    auto last = std::find_if(ls->targets.rbegin(), ls->targets.rend(), [&](const ListState::Target& t) { return t.same(color, depth); });
    if (last == ls->targets.rend()) return false;
    *sub = last->rt_sub;
    *depth_sub = last->ds_sub;
    return true;
}
uint32_t ListTargetsAfter(ID3D12CommandList* list, na_object_id color, na_object_id depth)
{
    std::shared_lock<std::shared_mutex> rl(g_m);
    na_object_id lid = 0;
    ListState* ls = list ? list_state_locked(list, &lid) : nullptr;
    if (!ls || !ls->recording_observed.load()) return 0;
    std::lock_guard<std::mutex> bl(ls->bind_m);
    auto last = std::find_if(ls->targets.rbegin(), ls->targets.rend(), [&](const ListState::Target& t) { return t.same(color, depth); });
    if (last == ls->targets.rend()) return 0;
    std::vector<na_object_id> others;
    for (auto it = last.base(); it != ls->targets.end(); ++it)
        if (it->rt != color && std::find(others.begin(), others.end(), it->rt) == others.end()) others.push_back(it->rt);
    return static_cast<uint32_t>(others.size());
}
// State of ONE subresource after `list`: the last barrier naming that subresource
// or all of them. Games commonly transition depth and stencil planes (or mips,
// array slices) separately; the whole-resource query refuses those.
bool ListLastStateSub(ID3D12CommandList* list, na_object_id id, uint32_t sub, uint32_t* state)
{
    std::shared_lock<std::shared_mutex> rl(g_m);
    na_object_id lid = 0;
    ListState* ls = list ? list_state_locked(list, &lid) : nullptr;
    if (!ls || !ls->recording_observed.load() || !ls->coverage_complete.load()) return false;
    std::lock_guard<std::mutex> bl(ls->bind_m);
    if (!ls->evidence_complete) return false;
    for (auto it = ls->states.rbegin(); it != ls->states.rend(); ++it) {
        if (it->resource != id) continue;
        if (it->subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES && it->subresource != sub) continue; // another plane/mip/slice
        if (it->state_after == kStatePending) return false;
        *state = it->state_after;
        return true;
    }
    return false;
}
bool ListLastState(ID3D12CommandList* list, na_object_id id, uint32_t* state)
{
    std::shared_lock<std::shared_mutex> rl(g_m);
    na_object_id lid = 0;
    ListState* ls = list ? list_state_locked(list, &lid) : nullptr;
    if (!ls || !ls->recording_observed.load() || !ls->coverage_complete.load()) return false;
    std::lock_guard<std::mutex> bl(ls->bind_m);
    if (!ls->evidence_complete) return false;
    for (auto it = ls->states.rbegin(); it != ls->states.rend(); ++it) {
        if (it->resource != id) continue;
        if (it->subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) return false; // per-subresource: not handled
        if (it->state_after == kStatePending) return false;
        *state = it->state_after;
        return true;
    }
    return false;
}
uint32_t WriteBuffer(na_object_id res, uint64_t offset, const void* src, uint32_t n)
{
    if (!res || !src || !n) return 0;
    ID3D12Resource* rewrite = nullptr;
    {
        std::shared_lock<std::shared_mutex> lock(g_m);
        auto it = g_buffers.find(res);
        if (it == g_buffers.end() || !it->second.cpu_visible || offset >= it->second.width || it->second.width - offset < n) return 0;
        if (it->second.cpu) return guarded_copy(it->second.cpu + offset, src, n) ? n : 0;
        if (!it->second.ever_mapped) return 0;
        auto key = g_res_keys.find(res);
        if (key != g_res_keys.end()) static_cast<IUnknown*>(const_cast<void*>(key->second))->QueryInterface(IID_PPV_ARGS(&rewrite));
    }
    if (!rewrite) return 0;
    bool ok = false;
    const bool was_host = t_host_work;
    t_host_work = true;
    const D3D12_RANGE none{0, 0};
    void* p = nullptr;
    if (SUCCEEDED(rewrite->Map(0, &none, &p)) && p) {
        ok = guarded_copy(static_cast<uint8_t*>(p) + offset, src, n);
        const D3D12_RANGE written{static_cast<SIZE_T>(offset), static_cast<SIZE_T>(offset + n)};
        rewrite->Unmap(0, &written);
    }
    t_host_work = was_host;
    rewrite->Release();
    return ok ? n : 0;
}
uint32_t ReadBuffer(na_object_id res, uint64_t offset, void* dst, uint32_t n)
{
    if (!res || !dst || !n) return 0;
    ID3D12Resource* reread = nullptr;
    {
        std::shared_lock<std::shared_mutex> lock(g_m);
        auto it = g_buffers.find(res);
        if (it == g_buffers.end() || !it->second.cpu_visible || offset >= it->second.width) return 0;
        n = static_cast<uint32_t>(std::min<uint64_t>(n, it->second.width - offset));
        if (it->second.cpu) return guarded_copy(dst, it->second.cpu + offset, n) ? n : 0;
        if (!it->second.ever_mapped) return 0;
        auto key = g_res_keys.find(res);
        if (key != g_res_keys.end()) static_cast<IUnknown*>(const_cast<void*>(key->second))->QueryInterface(IID_PPV_ARGS(&reread));
    }
    if (!reread) return 0;
    bool ok = false;
    const bool was_host = t_host_work;
    t_host_work = true;
    const D3D12_RANGE rr{static_cast<SIZE_T>(offset), static_cast<SIZE_T>(offset + n)};
    void* p = nullptr;
    if (SUCCEEDED(reread->Map(0, &rr, &p)) && p) {
        ok = guarded_copy(dst, static_cast<const uint8_t*>(p) + offset, n);
        const D3D12_RANGE none{0, 0};
        reread->Unmap(0, &none);
    }
    t_host_work = was_host;
    reread->Release();
    return ok ? n : 0;
}
bool KnownSubStateNow(na_object_id id, uint32_t sub, uint32_t* state)
{
    if (!state || !id || g_known_disabled.load() || g_unhooked_queues.load()) return false;
    std::lock_guard<std::mutex> lock(g_known_m);
    if (g_unknown_inflight) return false;
    auto it = g_known.find(id);
    if (it == g_known.end() || it->second.in_flight) return false;
    auto s = it->second.subs.find(sub);
    const uint32_t v = s != it->second.subs.end() ? s->second : (it->second.has_all ? it->second.all : kStatePending);
    if (v == kStatePending || v == D3D12_RESOURCE_STATE_COMMON) return false;
    *state = v;
    return true;
}
bool KnownSubStateInCurrentSubmission(na_object_id id, uint32_t sub, uint32_t* state)
{
    const SubmissionCapture* c = t_cur_cap;
    if (!c || !state || !id || !c->in_store || c->unknown_marked || c->copy_queue) return false;
    if (g_known_disabled.load() || g_unhooked_queues.load()) return false;
    std::lock_guard<std::mutex> lock(g_known_m);
    if (g_unknown_inflight) return false;
    auto it = g_known.find(id);
    if (it == g_known.end()) return false;
    if (it->second.in_flight > c->SelfMarks(id) || (it->second.queue && it->second.queue != t_cur_qid)) return false;
    auto s = it->second.subs.find(sub);
    const uint32_t v = s != it->second.subs.end() ? s->second : (it->second.has_all ? it->second.all : kStatePending);
    if (v == kStatePending || v == D3D12_RESOURCE_STATE_COMMON) return false;
    *state = v;
    return true;
}
bool KnownStateInCurrentSubmission(na_object_id id, uint32_t* state)
{
    const SubmissionCapture* c = t_cur_cap;
    if (!c || !state || !id || !c->in_store || c->unknown_marked || c->copy_queue) return false;
    if (g_known_disabled.load() || g_unhooked_queues.load()) return false;
    std::lock_guard<std::mutex> lock(g_known_m);
    if (g_unknown_inflight) return false;
    auto it = g_known.find(id);
    if (it == g_known.end() || !it->second.subs.empty() || !it->second.has_all) return false; // per-subresource: not handled
    if (it->second.in_flight > c->SelfMarks(id) || (it->second.queue && it->second.queue != t_cur_qid)) return false;
    const uint32_t v = it->second.all;
    if (v == kStatePending || v == D3D12_RESOURCE_STATE_COMMON) return false;
    *state = v;
    return true;
}
void SetSamplerMipBias(float bias)
{
    // Startup value: the game's own objects are built at the nearest menu level.
    const int level = nearest_mip_level(bias);
    g_mip_bias.store(kMipLevels[level], std::memory_order_relaxed);
    g_mip_game_level.store(level, std::memory_order_relaxed);
    g_mip_live_level.store(level, std::memory_order_relaxed);
}
float SamplerMipBias() { return kMipLevels[g_mip_live_level.load(std::memory_order_relaxed)]; }
void SetSamplerMipBiasLive(float bias)
{
    const int level = nearest_mip_level(bias);
    g_mip_live_level.store(level, std::memory_order_relaxed);
    if (level != g_mip_game_level.load(std::memory_order_relaxed)) g_mip_switched.store(true, std::memory_order_release);
}
void SetSamplerMipBiasLiveEnabled(bool on) { g_mip_live_enabled.store(on, std::memory_order_relaxed); }
MipLiveStats GetMipLiveStats()
{
    MipLiveStats m;
    m.enabled = g_mip_live_enabled.load(std::memory_order_relaxed);
    m.game_bias = kMipLevels[g_mip_game_level.load(std::memory_order_relaxed)];
    m.live_bias = kMipLevels[g_mip_live_level.load(std::memory_order_relaxed)];
    m.twin_root_signatures = g_mip_twin_rs.load(std::memory_order_relaxed);
    m.twin_pipelines = g_mip_twin_pso.load(std::memory_order_relaxed);
    m.untwinned_pipelines = g_mip_untwinned.load(std::memory_order_relaxed);
    m.fallbacks = g_mip_fallbacks.load(std::memory_order_relaxed);
    m.twin_failures = g_mip_twin_fail.load(std::memory_order_relaxed);
    return m;
}
void NativeMipBiases(NativeBiasStats* s)
{
    if (!s) return;
    std::lock_guard<std::mutex> lock(g_native_m);
    s->negative = g_native_neg; s->zero = g_native_zero; s->positive = g_native_pos;
    s->min = g_native_min; s->max = g_native_max;
}
void MipBiasCounts(uint64_t* samplers, uint64_t* static_samplers)
{
    if (samplers) *samplers = g_biased_samplers.load(std::memory_order_relaxed);
    if (static_samplers) *static_samplers = g_biased_static.load(std::memory_order_relaxed);
}
HostWorkScope::HostWorkScope() : prev_(t_host_work) { t_host_work = true; }
HostWorkScope::~HostWorkScope() { t_host_work = prev_; }
ID3D12Resource* AcquireResource(na_object_id id)
{
    std::shared_lock<std::shared_mutex> lock(g_m);
    auto key = g_res_keys.find(id);
    if (key == g_res_keys.end()) return nullptr;
    ID3D12Resource* r = nullptr;
    static_cast<IUnknown*>(const_cast<void*>(key->second))->QueryInterface(IID_PPV_ARGS(&r));
    return r;
}
namespace {
std::atomic<uint64_t> g_live_depth{0}, g_live_color{0};
}
void SetLiveSelection(uint64_t depth, uint64_t color) { g_live_depth.store(depth); g_live_color.store(color); }
void GetLiveSelection(uint64_t* depth, uint64_t* color) { *depth = g_live_depth.load(); *color = g_live_color.load(); }

void SetSampleAcquireProbeForTests(void (*probe)()) { g_acquire_probe = probe; }
void SetSampleWorkerPausedForTests(bool paused) { g_worker_paused.store(paused, std::memory_order_release); }
void SetSampleWorkerStartFailsForTests(bool fail) { g_fail_worker_start.store(fail); }
void SetSampleSignalFailsForTests(bool fail) { g_fail_sample_signal.store(fail); }

void SetUnmapBoundaryProbeForTests(void (*probe)(void* user), void* user)
{
    g_unmap_probe_user.store(user);
    g_unmap_probe.store(probe, std::memory_order_release);
}

void SetSnapshotCopyProbeForTests(void (*probe)(void* user), void* user)
{
    g_copy_probe_user.store(user);
    g_copy_probe.store(probe, std::memory_order_release);
}

bool ResolveGpuVa(uint64_t va, na_object_id* resource, uint64_t* offset)
{
    if (!resource || !offset) return false;
    std::shared_lock<std::shared_mutex> lock(g_m);
    return resolve_va_locked(va, resource, offset);
}

void SetExecuteForwarder(void(STDMETHODCALLTYPE* fwd)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*))
{
    g_exec_forward.store(fwd, std::memory_order_release);
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
    {
        std::lock_guard<std::mutex> lock(g_samples_m);
        s.samples_live = g_samples.size();
    }
    s.sample_workers_started = g_workers_started.load();
    s.sample_missing_recording = g_c.sample_missing_recording.load();
    s.sample_partial_coverage = g_c.sample_partial_coverage.load();
    s.sample_partial_states = g_c.sample_partial_states.load();
    s.sample_no_transition = g_c.sample_no_transition.load();
    s.nested_unlocked = g_c.nested_unlocked.load();
    s.snapshots_reread = g_c.snap_reread.load();
    s.preexisting_resources = g_c.preexisting_resources.load();
    s.preexisting_heaps = g_c.preexisting_heaps.load();
    s.sample_known_state = g_c.sample_known_state.load();
    s.known_state_clears = g_c.known_clears.load();
    s.known_store_disabled = g_known_disabled.load() ? 1u : 0u;
    s.unhooked_queues = g_unhooked_queues.load();
    {
        std::lock_guard<std::mutex> lock(g_known_m);
        s.known_unknown_inflight = g_unknown_inflight;
        s.known_entries = g_known.size();
    }
    return s;
}

} // namespace na::observe

// ------------------------------------------------- C9b public C interface
extern "C" int __cdecl na_observe_sample_request(const na_sample_request* r, uint64_t* out_token)
{
    using namespace na::observe;
    if (!r || !out_token || r->size_bytes < sizeof(na_sample_request)) return NA_E_ARG;
    *out_token = 0;
    na_session_id session = 0;
    na_epoch epoch = 0;
    na::observe_internal::CurrentScope(&session, &epoch);
    if (!session) return NA_E_STATE; // leases are scoped to a running session
    bool buffer = false;
    {
        std::shared_lock<std::shared_mutex> lock(g_m);
        auto key = g_res_keys.find(r->resource);
        if (key == g_res_keys.end()) return NA_E_ARG; // not an observed resource
        // The creation record (no object dereference) says whether it is a buffer.
        auto obj = g_objs.find(key->second);
        if (obj != g_objs.end() && obj->second.created.size() >= sizeof(na_ev_resource_created)) {
            na_ev_resource_created e{};
            std::memcpy(&e, obj->second.created.data(), sizeof(e));
            buffer = e.dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
        }
    }
    // C10 buffer range: x | y << 32 = byte offset, width = bytes (1..65536, the
    // largest constant buffer view), height 0 or 1, subresource 0.
    if (buffer && (!r->width || r->width > 65536 || r->height > 1 || r->subresource)) return NA_E_ARG;
    std::lock_guard<std::mutex> lock(g_samples_m);
    // G3-07: a released, quarantined (SUBMIT_FAILED) entry is kept until the
    // device is removed but no longer holds one of the consumer's leases.
    size_t active = 0;
    for (const auto& e : g_samples)
        active += (e->released && e->status == NA_SAMPLE_FAILED && e->reason == NA_SAMPLE_E_SUBMIT_FAILED) ? 0u : 1u;
    if (active >= kMaxSamples || g_samples.size() >= kMaxSampleEntries) return NA_E_STATE; // released-in-flight entries count too
    auto s = std::make_unique<Sample>();
    s->token = g_next_token.fetch_add(1);
    s->req = *r;
    s->req.size_bytes = sizeof(na_sample_request);
    s->armed_at = now_ms();
    s->session = session;
    s->epoch = epoch;
    s->buffer = buffer;
    s->offset = static_cast<uint64_t>(r->x) | (static_cast<uint64_t>(r->y) << 32);
    *out_token = s->token;
    if (!ensure_worker_locked()) { // no maintenance possible: refuse rather than arm an unserviced lease
        *out_token = 0;
        return NA_E_STATE;
    }
    g_samples.push_back(std::move(s));
    recount_armed_locked();
    g_samples_cv.notify_one(); // C9B-REVIEW-3 #1: an idle worker waits for exactly this
    return NA_OK;
}

extern "C" int __cdecl na_observe_sample_poll(uint64_t token, na_sample_info* info)
{
    using namespace na::observe;
    if (!info || info->size_bytes < 16) return NA_E_ARG;
    std::lock_guard<std::mutex> lock(g_samples_m);
    for (auto& sp : g_samples) {
        Sample& s = *sp;
        if (s.token != token || s.released) continue;
        scope_check_locked(s); // C9B-REVIEW-2 #3: report the scope as of now
        recount_armed_locked();
        na_sample_info out{};
        out.size_bytes = static_cast<uint32_t>(std::min<size_t>(info->size_bytes, sizeof(na_sample_info)));
        out.status = s.status;
        out.reason = s.reason;
        out.format = s.format;
        out.resource = s.req.resource;
        out.subresource = s.req.subresource;
        out.x = s.x;
        out.y = s.y;
        out.width = s.w;
        out.height = s.h;
        out.row_pitch = s.row_pitch;
        out.data_bytes = s.status == NA_SAMPLE_READY ? s.data.size() : 0;
        out.queue = s.queue;
        out.queue_ticket = s.ticket;
        out.state_before = s.state_before;
        out.session = s.session;
        out.epoch = s.epoch;
        std::memcpy(info, &out, out.size_bytes);
        return NA_OK;
    }
    return NA_E_ARG;
}

extern "C" int __cdecl na_observe_sample_read(uint64_t token, void* dst, uint64_t capacity, uint64_t* written)
{
    using namespace na::observe;
    if (written) *written = 0;
    if (!dst) return NA_E_ARG;
    std::lock_guard<std::mutex> lock(g_samples_m);
    for (auto& sp : g_samples) {
        Sample& s = *sp;
        if (s.token != token || s.released) continue;
        scope_check_locked(s); // C9B-REVIEW-2 #3: stale-scope data is never handed out
        if (s.status != NA_SAMPLE_READY) return NA_E_STATE;
        if (capacity < s.data.size()) return NA_E_ARG;
        std::memcpy(dst, s.data.data(), s.data.size()); // the caller owns its copy
        if (written) *written = s.data.size();
        return NA_OK;
    }
    return NA_E_ARG;
}

extern "C" int __cdecl na_observe_sample_release(uint64_t token)
{
    using namespace na::observe;
    std::lock_guard<std::mutex> lock(g_samples_m);
    for (auto it = g_samples.begin(); it != g_samples.end(); ++it) {
        Sample& s = **it;
        if (s.token != token) continue;
        if (s.released) return NA_OK; // C9B-R1: repeat release keeps the deferred free
        if (s.gpu_objects()) { // the GPU may still use them: the worker frees after completion
            s.released = true;
            if (s.status == NA_SAMPLE_ARMED || s.status == NA_SAMPLE_IN_FLIGHT) s.status = NA_SAMPLE_CANCELLED;
            s.data.clear();
        } else {
            g_samples.erase(it);
        }
        recount_armed_locked();
        return NA_OK;
    }
    return NA_OK; // idempotent
}
