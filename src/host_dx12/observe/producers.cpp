// C8a: H1 observation producers. See producers.hpp.
#include "host_dx12/diag/event_log.hpp"
#include "host_dx12/observe/producers.hpp"

#include <utility>
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "queue_occurrences.hpp"
#include "host_dx12/j2/insertion_trace.hpp"
#include "host_dx12/observe/texture_operation.hpp"
#include "host_dx12/observe/operation_snapshot.hpp"
#include "host_dx12/observe/shader_dimensions.hpp"
#include "host_dx12/observe/reader_slots_lock.hpp"
#include <tuple>
#include <deque>
#include <functional>
#include <string>
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

struct DescHeap;
// Recordings with a pinned viewport sample may still need it after the global
// request goes away. Only an entirely idle jitter path can skip the list locks.
std::atomic<uint64_t> g_vpj_recordings{0};
struct ListState {
    uint32_t list_type=NA_UNKNOWN_U32; // immutable creation metadata
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
    uint64_t root_serial[2]{},host_restore_serial=0;
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
        uint64_t serial = 0;
        std::vector<uint32_t> consts;
    };
    ID3D12PipelineState* pso = nullptr;
    ID3D12DescriptorHeap* heaps[2] = {nullptr, nullptr};
    DescHeap* heap_metadata[2]{}; // borrowed only under g_m, invalidated by heap_registry_revision
    uint64_t heap_metadata_revision=0;
    uint32_t nheaps = 0;
    struct KeyCache {
        OperationKey prefix;
        uint64_t state_revision=0,registry_revision=0,dimension_cbvs=0;
        std::vector<uint64_t> members;
        bool valid=false,rebase=false;
    } operation_key[2];
    uint64_t key_revision[2]{},read_revision[2]{};
    struct ShaderReads {
        uint64_t state_revision=0,registry_revision=0,color=0;
        na_object_id srvs[16]{},uav=0;
        uint32_t count=0;uint8_t color_srvs=0;bool valid=false;
    } shader_reads[2];
    std::vector<RootArg> args[2]; // [graphics, compute], by root parameter
    na_object_id cur_rt = 0, cur_ds = 0; // currently bound first render target / depth
    na_object_id cur_rts[8] = {};
    uint32_t cur_nrt = 0;
    uint32_t cur_ds_sub = NA_UNKNOWN_U32;
    na_object_id pending_entry = 0; // selected colour whose handoff awaits its first actual operation
    uint32_t entry_barrier=0;
    bool restorable = true;              // false after work whose state effects we cannot replay
    bool in_render_pass = false;         // between BeginRenderPass and EndRenderPass (no host copy/dispatch allowed)
    D3D12_VIEWPORT raster_viewports[D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    D3D12_RECT raster_scissors[D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT raster_viewport_count = 0, raster_scissor_count = 0, raster_rt_count = 0;
    D3D12_PRIMITIVE_TOPOLOGY raster_topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    bool raster_viewports_known = false, raster_scissors_known = false, raster_topology_known = false;
    bool raster_targets_known = false, raster_has_depth = false;
    bool raster_tracking = false; // latched at Reset; an Off request cannot undo restoration mid-recording
    D3D12_VERTEX_BUFFER_VIEW vertex_buffers[32]{};
    uint32_t vertex_mask=0;
    D3D12_INDEX_BUFFER_VIEW index_buffer{};
    bool index_known=false;
    float blend_factor[4]{};UINT stencil_ref=0;
    ID3D12Resource* predicate=nullptr;UINT64 predicate_offset=0;D3D12_PREDICATION_OP predicate_op=D3D12_PREDICATION_OP_EQUAL_ZERO;
    uint32_t active_queries=0;
    std::vector<std::shared_ptr<const DescriptorDetails>> native_colors;
    uint64_t target_serial=0,viewport_serial=0,scissor_serial=0;
    std::shared_ptr<const DescriptorDetails> native_depth;
    int8_t mip_level = -1;               // live sampler variant latched at Reset: mip level (-1 = the game's own objects)
    int8_t af_latched = 0;               // ...and its anisotropic index (0 off, 1 4x, 2 8x, 3 16x)
    // Binds of this recording (under bind_m; added to the totals at the next Reset): twin
    // passed to the driver / the game's own object kept (sampler-independent or identical twin).
    uint32_t mip_pso_twin = 0, mip_pso_game = 0, mip_rs_twin = 0, mip_rs_game = 0;
    // Viewport jitter: our offset copies of the game's viewports are bound now (vpj_value),
    // and the value the last scene draw of this recording was drawn with (vpj_scene, 0 = none).
    bool vpj_applied = false;
    uint64_t vpj_value = 0, vpj_scene = 0, vpj_depth = 0;
    ~ListState() { if (vpj_scene) g_vpj_recordings.fetch_sub(1, std::memory_order_release); }
};

struct VariantRecipe; // live sampler variants: see the definition
struct Obj {
    na_object_id id = 0;
    uint32_t kind = 0;
    std::vector<uint8_t> created;          // creation event, replayed by the snapshot
    std::shared_ptr<QueueState> queue;     // queues
    std::unique_ptr<ListState> list;       // command lists
    std::vector<na_root_param> root_params; // root signatures (C9 table resolution)
    std::vector<na_root_range> root_ranges;
    uint64_t root_hash = 0;                 // structural layout hash (role classes; stable across sessions)
    std::vector<ShaderDimension> dimensions;
    std::vector<ShaderResource> shader_resources;
    bool reflection_complete=false;
    bool native_hud_affine=false;
    uint8_t blend_class=0; // BlendClass (producers.hpp)
    bool shader_readonly=false;
    uint32_t shader_rejection=ShaderUnknown;
    bool pixel_position=false;
    // Live mip bias: per-level twins of a root signature / pipeline state (index = level).
    // same[i]: the game's own object is already correct for level i (nothing to swap).
    // Immutable once published; owns a reference on every twin. A twin is never one of
    // the game's own objects (see D_CreateRootSignature), so twins cannot form cycles.
    // Never destroyed inside another D3D12 object's destruction: see ReleaseTwinsLater.
    // built[i]: level i was prepared (same or obj, or it failed: then neither and lookups fall back).
    struct Twins {
        IUnknown* obj[7] = {}; bool same[7] = {}; bool built[7] = {}; // = kMipCount
        Twins() = default;
        Twins(const Twins& o) { for (int i = 0; i < 7; ++i) { obj[i] = o.obj[i]; same[i] = o.same[i]; built[i] = o.built[i]; if (obj[i]) obj[i]->AddRef(); } }
        Twins& operator=(const Twins&) = delete;
        ~Twins() { for (auto* o : obj) if (o) o->Release(); }
    };
    std::shared_ptr<Twins> twins;          // raster storage on command lists (not sampler variants)
    // Live sampler variants (mip bias x anisotropic filtering): af_sets[a] holds the twins at
    // anisotropic index a, per mip level. variant_kind: 0 = sampler-independent (the game's
    // object is right for every variant), 1 = has variants, 2 = cannot be twinned (a pipeline
    // with an embedded root signature): recordings fall back to the game's objects.
    std::shared_ptr<Twins> af_sets[4];
    std::shared_ptr<const VariantRecipe> recipe; // creation copy for background builds
    uint8_t variant_kind = 0;
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
        std::shared_ptr<const DescriptorDetails> details;
    };
    std::vector<SlotInfo> info; // last observed write per slot (record-time CPU view)
    // Slot stripes (64 slots each): descriptor copies, the game's most frequent call (10-20k
    // per frame in SE5), update versions/info under the SHARED g_m plus these, exclusive for
    // the destination slots and shared for the source slots. Every other reader of
    // versions/info under the shared g_m takes the covering stripe shared, one stripe at a
    // time. Writers under the exclusive g_m need none (no copy can be running then).
    std::unique_ptr<std::shared_mutex[]> stripes;
    uint32_t nstripes = 0;
};
constexpr uint32_t kStripeShift = 6;
// Shared lock of the stripe holding a slot. Holds at most one stripe: lock() moves to
// another stripe by releasing the current one first (never two at once, so readers cannot
// form a cycle with a copy, which takes its stripes in one global order).
struct SlotShared {
    std::shared_mutex* m = nullptr;
    SlotShared() = default;
    SlotShared(const DescHeap* h, uint64_t slot) { lock(h, slot); }
    SlotShared(const SlotShared&) = delete;
    SlotShared& operator=(const SlotShared&) = delete;
    void lock(const DescHeap* h, uint64_t slot)
    {
        std::shared_mutex* n = h && h->stripes && (slot >> kStripeShift) < h->nstripes ? &h->stripes[static_cast<size_t>(slot >> kStripeShift)] : nullptr;
        if (n == m) return;
        unlock();
        m = n;
        if (m) m->lock_shared();
    }
    void unlock() { if (m) { m->unlock_shared(); m = nullptr; } }
    ~SlotShared() { unlock(); }
};

struct VaRange {
    uint64_t begin = 0, end = 0; // [begin, end)
    na_object_id resource = 0;
    const void* key = nullptr;
};

// registry, heaps, VA table. Never held across a D3D12 call. Readers take per-thread slots
// instead of one shared lock word (reader_slots_lock.hpp: SE5 spun ~3.7 s/s on that word).
using RegistryLock = na::observe::ReaderSlotsLock;
RegistryLock g_m;
// Never destroyed: at process exit an entry's twins must not be Released (a twin's last
// Release can retire an object through its sentinel, which takes g_m and walks this map).
std::unordered_map<const void*, Obj>& g_objs = *new std::unordered_map<const void*, Obj>();
std::atomic<bool> g_raster_tracking{false};
std::atomic<bool> g_operation_snapshots{false};
void ReleaseTwinsLater(std::shared_ptr<Obj::Twins>&& t); // live mip bias: see the definition
void ReleaseParkedTwins();
std::atomic<uint64_t> g_mip_shared_skips{0}; // twin levels dropped: D3D12 returned one of the game's own root signatures
// Twins whose owner retired. The retirement sentinel runs INSIDE the D3D12 runtime's
// destruction of the owner (private data is released as the object dies, under the
// runtime's own locks: root signatures live in a per-device dedup cache, pipelines in
// the device's object list). Releasing another root signature or pipeline from there
// re-enters that destruction path and can wait on the lock the runtime already holds:
// the host J2 tests exited "ok" and then never returned (028a182..783fb26). So the
// sentinel only parks the twins here; they are released at the next safe point (a
// creation hook or ExecuteCommandLists, which run in the game's call, not inside the
// runtime's teardown of an object). Twins still parked at process exit are leaked.
std::mutex g_twin_grave_m;
std::vector<std::shared_ptr<void>>& g_twin_grave = *new std::vector<std::shared_ptr<void>>();
void ReleaseTwinsLater(std::shared_ptr<Obj::Twins>&& t)
{
    if (!t) return;
    std::lock_guard<std::mutex> lk(g_twin_grave_m);
    g_twin_grave.push_back(std::move(t));
}
// An entry's twins, variant sets and creation copy (which holds a device reference).
struct ObjParcel { std::shared_ptr<Obj::Twins> twins, sets[4]; std::shared_ptr<const VariantRecipe> recipe; };
ObjParcel take_parcel(Obj& o)
{
    ObjParcel p;
    p.twins = std::move(o.twins);
    for (int a = 0; a < 4; ++a) p.sets[a] = std::move(o.af_sets[a]);
    p.recipe = std::move(o.recipe);
    return p;
}
void ReleaseLater(ObjParcel&& p)
{
    std::lock_guard<std::mutex> lk(g_twin_grave_m);
    if (p.twins) g_twin_grave.push_back(std::move(p.twins));
    for (auto& t : p.sets) if (t) g_twin_grave.push_back(std::move(t));
    if (p.recipe) g_twin_grave.push_back(std::const_pointer_cast<VariantRecipe>(std::move(p.recipe)));
}
void ReleaseParkedTwins()
{
    std::vector<std::shared_ptr<void>> dead;
    {
        std::lock_guard<std::mutex> lk(g_twin_grave_m);
        if (g_twin_grave.empty()) return;
        dead.swap(g_twin_grave);
    }
    dead.clear(); // plain Release calls from the game's own call stack
}
std::vector<std::unique_ptr<DescHeap>> g_heaps;
// CPU-handle index of g_heaps, sorted by cpu_start (rebuilt under the exclusive g_m whenever
// g_heaps changes): every descriptor write/copy and many binds looked their heap up by
// walking all heaps under the lock.
std::vector<std::pair<uint64_t, DescHeap*>> g_heap_cpu_index;
void heap_index_rebuild()
{
    g_heap_cpu_index.clear();
    g_heap_cpu_index.reserve(g_heaps.size());
    for (auto& h : g_heaps) g_heap_cpu_index.emplace_back(h->cpu_start, h.get());
    // Stable: for an equal start the later (newer) heap stays last and is found first.
    std::stable_sort(g_heap_cpu_index.begin(), g_heap_cpu_index.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
}
std::vector<VaRange> g_va; // sorted by begin
// g_va_maxend[i] = the largest end among g_va[0..i]: a backwards walk stops as soon as no
// earlier range can reach va. Exact (overlapping placed buffers are still all found), where
// the old 64 GB window crossed nearly every buffer on each root CBV/SRV bind.
std::vector<uint64_t> g_va_maxend;
void va_reindex(size_t from)
{
    g_va_maxend.resize(g_va.size());
    for (size_t i = from; i < g_va.size(); ++i)
        g_va_maxend[i] = i ? (std::max)(g_va_maxend[i - 1], g_va[i].end) : g_va[i].end;
}
void va_insert(const VaRange& r)
{
    const auto it = std::upper_bound(g_va.begin(), g_va.end(), r, [](const VaRange& a, const VaRange& b) { return a.begin < b.begin; });
    const size_t at = static_cast<size_t>(it - g_va.begin());
    g_va.insert(it, r);
    va_reindex(at);
}
// Protected by the existing g_m. No additional hot-path setter lock. Identity
// reuse, descriptor writes/copies and heap retirement invalidate borrowed data.
std::atomic<uint64_t> g_operation_revision{1}; // atomic: descriptor copies advance it under the shared g_m
uint64_t g_heap_registry_revision=1;
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
    std::atomic<uint64_t> nested_unlocked{0}, serialized{0};
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
    uint8_t reset_why = 0;  // diagnostics: why its states last became unknown (KnownReset)
};
// Why a carried state became unknown (diagnostics only; DescribeStateEvidence names it).
enum KnownReset : uint8_t { kResetNone, kResetStoreClear, kResetOverlap, kResetUnordered, kResetOtherQueue, kResetSubs, kResetPendingNote,
                            kResetAmbiguous, kResetTainted, kResetEpoch, kResetCopyQueue };
const char* const kResetNames[] = {"none", "store_clear", "overlapped_submissions", "unordered_submission", "other_queue", "too_many_subresources", "split_or_aliasing_barrier",
                                   "ambiguous_queue_order", "unknown_submission_in_flight", "store_cleared_meanwhile", "copy_queue_decay"};
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
void known_reset_locked(ResKnown& k, KnownReset why)
{
    k.reset_why = why;
    k.queue = 0;
    k.has_all = false;
    k.all = kStatePending;
    k.subs.clear();
}
// Entries stay (eligibility and in-flight marks are kept); states become unknown.
void known_clear_locked()
{
    for (auto& kv : g_known) known_reset_locked(kv.second, kResetStoreClear);
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

// Like init_header, but for a kind the session's consumer does not read
// (na_observe_skip_kinds): the event is zeroed and NOT numbered, so skipping it leaves
// no thread_seq gap (a gap would read as lost events). Returns whether to emit it.
template <typename E>
bool init_header_wanted(E& e, uint32_t kind, uint32_t flags)
{
    if (!na::observe_internal::KindSkipped(kind)) {
        init_header(e, kind, flags);
        return true;
    }
    std::memset(&e, 0, sizeof(e));
    return false;
}

template <typename E>
void emit(const E& e, uint32_t group)
{
    static_assert(sizeof(E) % 8 == 0, "events are 8-byte multiples");
    na::observe_internal::Emit(&e, group);
}

// ---- Hook cost (2026-11-05: with us loaded SE5 ran 35 fps CPU-bound vs 165 without) ----------
// Wall time spent inside each outermost command-list hook (ours + the driver's call), per
// slot, plus ExecuteCommandLists and pipeline creation. Thread-local sums are folded into the
// shared totals every 512 calls, so the measurement itself adds no shared write per call.
constexpr uint32_t kCostSlots = 128, kCostExec = 125, kCostPso = 126;
// Pseudo slots (inside the hooks, inclusive, nested in the slot above): 100.. (printed by name).
constexpr uint32_t kCostPseudo = 100, kCostVpj = 100, kCostShaderUse = 101, kCostLaneBefore = 102, kCostLaneAfter = 103,
                   kCostDriver = 104, kCostRun = 105, kCostRootTable = 106, kCostSampled = 107, kCostEmit = 108,
                   kCostNoteDraw = 109, kCostCanonMiss = 110,
                   kCostDescCopy = 111, kCostDescCopyWait = 112, kCostDescWrite = 113, kCostDescWriteWait = 114; // descriptor copies/writes and their wait for the exclusive g_m
std::atomic<bool> g_cost_enabled{false};
std::atomic<uint64_t> g_cost_ticks[kCostSlots]{}, g_cost_calls[kCostSlots]{};
thread_local uint64_t t_cost_ticks[kCostSlots]{}, t_cost_calls[kCostSlots]{};
thread_local uint32_t t_cost_n = 0;
inline uint64_t cost_now() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return static_cast<uint64_t>(c.QuadPart); }
void cost_flush()
{
    for (uint32_t i = 0; i < kCostSlots; ++i)
        if (t_cost_calls[i]) {
            g_cost_ticks[i].fetch_add(t_cost_ticks[i], std::memory_order_relaxed);
            g_cost_calls[i].fetch_add(t_cost_calls[i], std::memory_order_relaxed);
            t_cost_ticks[i] = t_cost_calls[i] = 0;
        }
    t_cost_n = 0;
}
struct CostScope {
    uint32_t slot; uint64_t t0;
    explicit CostScope(uint32_t s, bool on = true) : slot(on && s < kCostSlots && g_cost_enabled.load(std::memory_order_relaxed) ? s : kCostSlots), t0(slot < kCostSlots ? cost_now() : 0) {}
    ~CostScope()
    {
        if (slot >= kCostSlots) return;
        t_cost_ticks[slot] += cost_now() - t0; ++t_cost_calls[slot];
        if (++t_cost_n >= 512 || slot >= kCostExec) cost_flush();
    }
    CostScope(const CostScope&) = delete;
    CostScope& operator=(const CostScope&) = delete;
};


// Canonical COM identity (review: interface pointers of one object may differ
// under wrapping layers): the registry is keyed by the object's IUnknown.
// A small per-thread cache keeps the hot recording path cheap; any retirement invalidates
// every cache (an address may be reused afterwards). 2026-11-05: one entry thrashed, since a
// draw canonicalises its list AND its pipeline (note_draw, viewport jitter), so every call fell
// through to QueryInterface + Release: two COM calls, and interlocked refcount traffic on the
// game's shared PSOs from every recording thread. Eight entries, round-robin replacement.
std::atomic<uint64_t> g_retire_gen{1};
const void* canon(const void* p)
{
    if (!p) return nullptr;
    constexpr unsigned kWays = 8;
    thread_local const void* c_in[kWays] = {};
    thread_local const void* c_out[kWays] = {};
    thread_local uint64_t c_gen = 0;
    thread_local unsigned c_next = 0;
    const uint64_t gen = g_retire_gen.load(std::memory_order_acquire);
    if (gen != c_gen) { for (unsigned i = 0; i < kWays; ++i) c_in[i] = nullptr; c_gen = gen; }
    for (unsigned i = 0; i < kWays; ++i) if (c_in[i] == p) return c_out[i];
    const CostScope miss(kCostCanonMiss);
    IUnknown* u = nullptr;
    auto* obj = static_cast<IUnknown*>(const_cast<void*>(p));
    const void* out = p;
    if (SUCCEEDED(obj->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&u))) && u) {
        out = u;
        u->Release(); // the caller still holds the object
    }
    const unsigned w = c_next++ % kWays;
    c_in[w] = p;
    c_out[w] = out;
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

std::atomic<bool> g_process_exiting{false};
void RetireSentinel::retire(const void* key, na_object_id id, uint32_t kind)
{
    // SE5 exit crashes (c0000005 at retire, symbolicated 2026-11-02): exit-time destructors of
    // other statics released D3D12 objects after the CRT had destroyed g_m / g_heaps / g_va.
    if (g_process_exiting.load(std::memory_order_acquire)) return;
    g_retire_gen.fetch_add(1, std::memory_order_acq_rel);
    ObjParcel drop; // parked, never released here (we are inside the owner's destruction)
    {
        std::unique_lock<RegistryLock> lock(g_m);
        ++g_operation_revision;++g_heap_registry_revision;
        auto it = g_objs.find(key);
        if (it != g_objs.end() && it->second.id == id) { drop = take_parcel(it->second); g_objs.erase(it); }
        g_heaps.erase(std::remove_if(g_heaps.begin(), g_heaps.end(), [&](const std::unique_ptr<DescHeap>& h) { return h->id == id; }),
                      g_heaps.end());
        heap_index_rebuild();
        {
            const auto first = std::find_if(g_va.begin(), g_va.end(), [&](const VaRange& r) { return r.resource == id; });
            if (first != g_va.end()) {
                const size_t at = static_cast<size_t>(first - g_va.begin());
                g_va.erase(std::remove_if(first, g_va.end(), [&](const VaRange& r) { return r.resource == id; }), g_va.end());
                va_reindex(at);
            }
        }
        g_buffers.erase(id);
        g_res_keys.erase(id);
    }
    ReleaseLater(std::move(drop));
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
    std::unique_lock<RegistryLock> lock(g_m);
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
    ObjParcel drop; // stale entry's twins, released after g_m
    {
        std::unique_lock<RegistryLock> lock(g_m);
        Obj& o = g_objs[key];
        ++g_operation_revision;
        if (o.id) g_c.live.fetch_sub(1); // address reused without a retirement we saw
        drop = take_parcel(o);
        o = fresh_obj(id, kind);
    }
    ReleaseLater(std::move(drop));
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
    ObjParcel drop; // stale entry's twins, released after g_m
    {
        std::unique_lock<RegistryLock> lock(g_m);
        auto it = g_objs.find(key);
        if (it != g_objs.end() && it->second.kind == kind) return it->second.id;
        id = g_next_id.fetch_add(1);
        ++g_operation_revision;
        if (it != g_objs.end()) { g_c.live.fetch_sub(1); drop = take_parcel(it->second); }
        g_objs[key] = fresh_obj(id, kind);
    }
    ReleaseLater(std::move(drop));
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
    std::unique_lock<RegistryLock> lock(g_m);
    auto it = g_objs.find(key);
    if (it == g_objs.end()) return;
    if (it->second.retirement_unobservable) e.hdr.flags |= NA_EVF_METHOD_UNOBSERVED;
    const auto* b = reinterpret_cast<const uint8_t*>(&e);
    it->second.created.assign(b, b + sizeof(E));
    if constexpr(std::is_same_v<E,na_ev_list_created>)if(it->second.list)it->second.list->list_type=e.list_type;
}

na_object_id id_locked(const void* object)
{
    auto it = g_objs.find(canon(object));
    return it == g_objs.end() ? 0 : it->second.id;
}

na_object_id lookup(const void* key)
{
    if (!key) return 0;
    std::shared_lock<RegistryLock> lock(g_m);
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
        std::unique_lock<RegistryLock> lock(g_m);
        g_res_keys[id] = canon(obj);
    }
    if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        std::unique_lock<RegistryLock> lock(g_m);
        BufferInfo& bi = g_buffers[id];
        bi.heap_type = heap_type;
        bi.width = d.Width;
        bi.cpu_visible = cpu_visible;
    }
    // Placed textures also have persistent states. Named aliasing barriers
    // invalidate their entries; wildcard barriers invalidate the whole store.
    // Reserved/tiled and simultaneous-access resources remain outside this store.
    if ((creation_kind == NA_CREATE_COMMITTED ||
         (creation_kind == NA_CREATE_PLACED && (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))) && d.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
        !(d.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS))
        known_created(id, static_cast<uint32_t>(initial));
    if (va) {
        std::unique_lock<RegistryLock> lock(g_m);
        va_insert(VaRange{va, va + d.Width, id, obj});
        ++g_operation_revision;
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
        std::unique_lock<RegistryLock> lock(g_m);
        g_res_keys[id] = canon(res);
        if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            BufferInfo& bi = g_buffers[id];
            bi.heap_type = e.heap_type;
            bi.width = d.Width;
            bi.cpu_visible = has_heap && cpu_visible_heap(hp);
            bi.ever_mapped = bi.cpu_visible; // it may have been mapped before we could see it
        }
        if (va) {
            va_insert(VaRange{va, va + d.Width, id, res});
            ++g_operation_revision;
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
        // No range at or before the previous one ends above va: none can contain it.
        const size_t i = static_cast<size_t>(it - g_va.begin());
        if (i == 0 || g_va_maxend[i - 1] <= va) break;
    }
    if (hits != 1) return false;
    *resource = found;
    *offset = va - base;
    return true;
}

// ------------------------------------------------------------ descriptors
DescHeap* heap_for_cpu_locked(uint64_t ptr, uint32_t* slot)
{
    // Fast path: the heap with the greatest start at or below ptr.
    auto it = std::upper_bound(g_heap_cpu_index.begin(), g_heap_cpu_index.end(), ptr,
                               [](uint64_t v, const std::pair<uint64_t, DescHeap*>& e) { return v < e.first; });
    if (it != g_heap_cpu_index.begin()) {
        DescHeap* h = std::prev(it)->second;
        if (h->increment && ptr >= h->cpu_start) {
            const uint64_t off = ptr - h->cpu_start;
            if (off % h->increment == 0 && off / h->increment < h->count) {
                *slot = static_cast<uint32_t>(off / h->increment);
                return h;
            }
        }
    }
    // Overlapping or unindexed heaps (not expected): the original full walk.
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
                           uint32_t first_sub, uint32_t sub_count, std::shared_ptr<DescriptorDetails> view = {})
{
    na_ev_descriptor_write e;
    const bool want = init_header_wanted(e, NA_EV_DESCRIPTOR_WRITE, 0);
    {
        const CostScope cost(kCostDescWrite);
        std::shared_lock<RegistryLock> lock(g_m, std::defer_lock);
        { const CostScope wait(kCostDescWriteWait); lock.lock(); }
        uint32_t slot = 0;
        DescHeap* h = heap_for_cpu_locked(dst.ptr, &slot);
        if (!h) {
            g_c.unresolved.fetch_add(1);
            return;
        }
        const na_object_id rid=resource?id_locked(resource):0;
        // The registry lock retains the heap/resource identity; only this slot's
        // stripe is exclusive. Publish compact metadata and replay details together.
        std::unique_lock<std::shared_mutex> stripe(h->stripes[slot >> kStripeShift], std::defer_lock);
        { const CostScope wait(kCostDescWriteWait); stripe.lock(); }
        e.heap = h->id;
        e.slot = slot;
        e.slot_version = ++h->versions[slot];
        ++g_operation_revision;
        e.resource = rid;
        if (resource && !e.resource) {
            e.resource = NA_UNKNOWN_U64; // resource-backed, but created before observation
            e.hdr.flags |= NA_EVF_METHOD_UNOBSERVED;
        }
        h->info[slot] = DescHeap::SlotInfo{kind, e.resource, format, first_sub, 0, 0};
        if (view) {
            view->kind=kind;view->resource=e.resource;
            h->info[slot].details=std::move(view);
        }
    }
    e.descriptor_kind = kind;
    e.format = format;
    e.first_subresource = first_sub;
    e.subresource_count = sub_count;
    g_c.writes.fetch_add(1);
    if (want) emit(e, NA_GROUP_DESCRIPTOR);
}

// Copies of `count` descriptors from src to dst, split wherever either side
// crosses a heap boundary (each event names one source and one destination heap).
void note_descriptor_copy(uint64_t dst, uint64_t src, uint32_t count, uint32_t increment)
{
    while (count) {
        na_ev_descriptor_copy e;
        const bool emit_it = init_header_wanted(e, NA_EV_DESCRIPTOR_COPY, 0);
        uint32_t run = 0;
        bool resolved = false, striped = false;
        {
            // Shared g_m + the slots' stripes (exact same result as the exclusive section,
            // which remains the path for a copy spanning more than kMaxStripes stripes).
            const CostScope cost(kCostDescCopy);
            std::shared_lock<RegistryLock> lock(g_m, std::defer_lock);
            { const CostScope wait(kCostDescCopyWait); lock.lock(); }
            uint32_t ds = 0, ss = 0;
            DescHeap* dh = heap_for_cpu_locked(dst, &ds);
            DescHeap* sh = heap_for_cpu_locked(src, &ss);
            if (dh && sh) {
                resolved = true;
                run = std::min({count, dh->count - ds, sh->count - ss});
                constexpr uint32_t kMaxStripes = 32;
                struct Want { std::shared_mutex* m; bool ex; };
                Want want[kMaxStripes];
                uint32_t nw = 0;
                const auto add = [&](DescHeap* h, uint32_t first, bool ex) {
                    if (!run || !h->stripes) return false;
                    for (uint32_t st = first >> kStripeShift; st <= (first + run - 1) >> kStripeShift; ++st) {
                        if (st >= h->nstripes) return false;
                        std::shared_mutex* m = &h->stripes[st];
                        bool merged = false;
                        for (uint32_t i = 0; i < nw; ++i) if (want[i].m == m) { want[i].ex |= ex; merged = true; }
                        if (merged) continue;
                        if (nw == kMaxStripes) return false;
                        want[nw++] = {m, ex};
                    }
                    return true;
                };
                if (run && add(dh, ds, true) && add(sh, ss, false)) {
                    std::sort(want, want + nw, [](const Want& a, const Want& b) { return std::less<std::shared_mutex*>()(a.m, b.m); });
                    {
                        const CostScope wait(kCostDescCopyWait);
                        for (uint32_t i = 0; i < nw; ++i) { if (want[i].ex) want[i].m->lock(); else want[i].m->lock_shared(); }
                    }
                    ++g_operation_revision;
                    for (uint32_t i = 0; i < run; ++i) {
                        ++dh->versions[ds + i];
                        dh->info[ds + i] = sh->info[ss + i]; // contents follow the source's record-time view
                    }
                    for (uint32_t i = nw; i-- > 0;) { if (want[i].ex) want[i].m->unlock(); else want[i].m->unlock_shared(); }
                    striped = true;
                } else if (!run) striped = true; // nothing to copy
                e.src_heap = sh->id;
                e.dst_heap = dh->id;
                e.src_slot = ss;
                e.dst_slot = ds;
                e.slot_count = run;
            }
        }
        if (!resolved) {
            g_c.unresolved.fetch_add(1);
            return;
        }
        if (!striped) {
            // Rare (a copy over more than 32 stripes, 2048+ slots): the exclusive section.
            const CostScope cost(kCostDescCopy);
            std::unique_lock<RegistryLock> lock(g_m, std::defer_lock);
            { const CostScope wait(kCostDescCopyWait); lock.lock(); }
            uint32_t ds = 0, ss = 0;
            DescHeap* dh = heap_for_cpu_locked(dst, &ds);
            DescHeap* sh = heap_for_cpu_locked(src, &ss);
            if (!dh || !sh) {
                g_c.unresolved.fetch_add(1);
                return;
            }
            run = std::min({count, dh->count - ds, sh->count - ss});
            ++g_operation_revision;
            for (uint32_t i = 0; i < run; ++i) {
                ++dh->versions[ds + i];
                dh->info[ds + i] = sh->info[ss + i];
            }
            e.src_heap = sh->id;
            e.dst_heap = dh->id;
            e.src_slot = ss;
            e.dst_slot = ds;
            e.slot_count = run;
        }
        g_c.copies.fetch_add(1);
        if (emit_it) emit(e, NA_GROUP_DESCRIPTOR);
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
#if defined(__ID3D12Device11_INTERFACE_DEFINED__)
NA_ORIG(CreateSampler2, void, ID3D12Device11*, const D3D12_SAMPLER_DESC2*, D3D12_CPU_DESCRIPTOR_HANDLE);
#endif
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
        std::unique_lock<RegistryLock> lock(g_m);
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
    dh->nstripes = (desc->NumDescriptors + (1u << kStripeShift) - 1) >> kStripeShift;
    dh->stripes.reset(new std::shared_mutex[dh->nstripes ? dh->nstripes : 1]);
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
        std::unique_lock<RegistryLock> lock(g_m);
        // A heap at the same CPU address replaces a destroyed one we never saw retire.
        g_heaps.erase(std::remove_if(g_heaps.begin(), g_heaps.end(), [&](const std::unique_ptr<DescHeap>& x) { return x->key == obj; }),
                      g_heaps.end());
        g_heaps.push_back(std::move(dh));
        heap_index_rebuild();
        ++g_operation_revision;++g_heap_registry_revision;
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
        std::shared_lock<RegistryLock> lock(g_m);
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
    std::shared_ptr<DescriptorDetails> view;
    if(g_operation_snapshots.load()&&!t_host_work&&desc){view=std::make_shared<DescriptorDetails>();view->explicit_view=true;view->cbv=*desc;}
    na_object_id res = 0;
    uint64_t off = 0;
    bool known = false;
    // ABI 0.1 DESCRIPTOR_WRITE carries no byte offset; the resource identity is
    // what H1 promises. Unresolved / ambiguous ranges are UNKNOWN, never guessed.
    na_ev_descriptor_write e;
    const bool want = init_header_wanted(e, NA_EV_DESCRIPTOR_WRITE, 0);
    {
        const CostScope cost(kCostDescWrite);
        std::shared_lock<RegistryLock> lock(g_m, std::defer_lock);
        { const CostScope wait(kCostDescWriteWait); lock.lock(); }
        if(desc&&desc->BufferLocation)known=resolve_va_locked(desc->BufferLocation,&res,&off);
        if(!known)e.hdr.flags|=NA_EVF_METHOD_UNOBSERVED;
        uint32_t slot = 0;
        DescHeap* dh = heap_for_cpu_locked(h.ptr, &slot);
        if (!dh) {
            g_c.unresolved.fetch_add(1);
            return;
        }
        std::unique_lock<std::shared_mutex> stripe(dh->stripes[slot >> kStripeShift], std::defer_lock);
        { const CostScope wait(kCostDescWriteWait); stripe.lock(); }
        e.heap = dh->id;
        e.slot = slot;
        e.slot_version = ++dh->versions[slot];
        ++g_operation_revision;
        dh->info[slot] = DescHeap::SlotInfo{NA_DESC_CBV, desc && desc->BufferLocation ? (known ? res : NA_UNKNOWN_U64) : 0, NA_UNKNOWN_U32, 0,
                                            known ? off : 0, desc ? desc->SizeInBytes : 0};
        if(view){view->kind=NA_DESC_CBV;view->resource=dh->info[slot].resource;dh->info[slot].details=std::move(view);}
    }
    e.descriptor_kind = NA_DESC_CBV;
    e.resource = desc && desc->BufferLocation ? (known ? res : NA_UNKNOWN_U64) : 0;
    e.format = NA_UNKNOWN_U32;
    e.first_subresource = 0;
    e.subresource_count = 1;
    g_c.writes.fetch_add(1);
    if (want) emit(e, NA_GROUP_DESCRIPTOR);
}

void STDMETHODCALLTYPE D_CreateShaderResourceView(ID3D12Device* dev, ID3D12Resource* r, const D3D12_SHADER_RESOURCE_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    o_CreateShaderResourceView(dev, r, d, h);
    uint32_t first = NA_UNKNOWN_U32, count = NA_UNKNOWN_U32;
    if (d && d->ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D) { first = d->Texture2D.MostDetailedMip; count = d->Texture2D.MipLevels; }
    std::shared_ptr<DescriptorDetails> view;
    if (g_operation_snapshots.load() && !t_host_work) {
        view = std::make_shared<DescriptorDetails>(); view->explicit_view = d != nullptr; if (d) view->srv = *d;
    }
    note_descriptor_write(h, NA_DESC_SRV, r, d ? static_cast<uint32_t>(d->Format) : NA_UNKNOWN_U32, first, count,std::move(view));
}

void STDMETHODCALLTYPE D_CreateUnorderedAccessView(ID3D12Device* dev, ID3D12Resource* r, ID3D12Resource* counter,
                                                    const D3D12_UNORDERED_ACCESS_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    o_CreateUnorderedAccessView(dev, r, counter, d, h);
    uint32_t first = NA_UNKNOWN_U32, count = NA_UNKNOWN_U32;
    if (d && d->ViewDimension == D3D12_UAV_DIMENSION_TEXTURE2D) { first = d->Texture2D.MipSlice; count = 1; }
    std::shared_ptr<DescriptorDetails> view;
    if (g_operation_snapshots.load() && !t_host_work) {
        view = std::make_shared<DescriptorDetails>(); view->explicit_view = d != nullptr; if (d) view->uav = *d;
        bool created = false; view->counter = counter ? ensure_registered(counter, NA_OBJ_RESOURCE, &created) : 0;
    }
    note_descriptor_write(h, NA_DESC_UAV, r, d ? static_cast<uint32_t>(d->Format) : NA_UNKNOWN_U32, first, count,std::move(view));
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
    std::shared_ptr<DescriptorDetails> view;
    if (g_operation_snapshots.load() && !t_host_work) {
        view = std::make_shared<DescriptorDetails>(); view->explicit_view = d != nullptr; if (d) view->rtv = *d;
    }
    note_descriptor_write(h, NA_DESC_RTV, r, d ? static_cast<uint32_t>(d->Format) : NA_UNKNOWN_U32, first, count,std::move(view));
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
    std::shared_ptr<DescriptorDetails> view;
    if (g_operation_snapshots.load() && !t_host_work) {
        view = std::make_shared<DescriptorDetails>(); view->explicit_view = d != nullptr; if (d) view->dsv = *d;
    }
    note_descriptor_write(h, NA_DESC_DSV, r, d ? static_cast<uint32_t>(d->Format) : NA_UNKNOWN_U32, first, count,std::move(view));
}

// Texture LOD (mip) bias for temporal reconstruction (J2 DLAA): with jitter and
// accumulation, textures must be sampled one mip sharper than for a single
// frame, or every frame is already low-passed and the accumulated picture loses
// texture detail (wood planks, roof tiles). Applied to samplers that actually
// use mips (linear / anisotropic mip filtering, MaxLOD > 0); never to
// comparison (shadow) samplers or point-mip lookups. 0 = off.
std::atomic<float> g_mip_bias{0.0f};
// Live sampler variants (Cort 2026-11-03: compare without restarting; 2026-11-05: no
// restarts at all, and the saved choice must work from the first frame). The game's static
// samplers are baked into its root signatures and pipelines at load. Saved effects are only
// activated once renderer recovery is known (971a7b3), which is AFTER the game builds them,
// so the game's own objects are left exactly as the game made them and every choice is a
// twin: variant (mip level, anisotropic index). Command lists bind the twins of the variant
// latched at their Reset. The saved choice (and the DLSS modes' automatic levels) get twins
// at creation; any other choice is built by a background worker from copies of the creation
// descriptions and goes live, for all lists at once, when complete.
// Render-small's DLSS modes (Cort, 2026-11-03): Quality -1.585 (NVIDIA's -1 + log2(1/1.5)),
// Balanced -2, Performance -2.25 (Cort 2026-11-04; was -2.5).
// Level 6 (+4) is the menu's visible check (Cort 2026-11-06: "0 and -1 look the same"): if the
// surfaces do not turn obviously blurry at +4, the samplers we change are not the ones the
// game's materials use, whatever the counters say. Never saved.
constexpr int kMipCount = 7;
constexpr float kMipLevels[kMipCount] = {0.0f, -0.5f, -1.0f, -1.5849625f, -2.0f, -2.25f, 4.0f};
static_assert(kMipCount <= 7, "Obj::Twins holds 7 levels");
constexpr uint32_t kAfLevels[4] = {0, 4, 8, 16};
std::atomic<uint32_t> g_mip_level_mask{0x7u}; // levels built at creation (bit i = kMipLevels[i])
// Background warm-up (Cort 2026-11-06: "mip does nothing"; a choice not built at load took
// 25-30 s, so a quick A/B never switched). After the chosen level is live, the worker builds the
// menu's levels (0, -0.5, -1) and the DLSS modes' automatic levels at the in-use filtering, one
// at a time at below-normal priority, so a later switch is immediate. [j2] mip_prewarm=0 = off.
std::atomic<bool> g_mip_prewarm{true};
std::atomic<uint64_t> g_mip_warmed{0}; // levels completed by warm-up (statistics)
std::atomic<uint64_t> g_mip_parallel_passes{0}; // passes whose pipelines were shared with the background pool
std::atomic<int> g_pre_af{0};              // anisotropic index built at creation (the saved one)
// Variants packed as level | af << 4; 0 = the game's own objects.
std::atomic<uint32_t> g_var_want{0};       // chosen (menu / saved activation)
std::atomic<uint32_t> g_var_live{0};       // in use: what recordings latch at Reset
std::atomic<uint64_t> g_var_request_tick{0}; // event log: when the current choice was asked for
std::atomic<bool> g_var_missing{false};    // a lookup met an object without the live variant: refill
std::atomic<uint64_t> g_var_done{0}, g_var_total{0};
// Never destroyed (the worker is detached and may still wait at process exit).
std::mutex& g_var_m = *new std::mutex();   // the worker's wake-up and the want/live hand-over
std::condition_variable& g_var_cv = *new std::condition_variable();
bool g_var_started = false;
std::atomic<uint64_t> g_var_static_changed[kMipCount * 4]; // static samplers changed, per variant
std::atomic<uint64_t> g_var_aniso_changed[kMipCount * 4]; // AF changes only, not mip-bias changes
std::atomic<bool> g_mip_switched{false};   // a variant other than the game's has been in use (enables lookups)
std::atomic<bool> g_mip_live_enabled{false}; // twins are built only when J2 enables live switching
std::atomic<uint64_t> g_mip_twin_rs{0}, g_mip_twin_pso{0}, g_mip_untwinned{0}, g_mip_fallbacks{0}, g_mip_twin_fail{0};
// Draw-time proof (Cort 2026-11-05: "x16 shows no difference"): binds that really went to the
// driver as twins vs kept the game's object, summed once per recording at its next Reset.
std::atomic<uint64_t> g_mip_bound_pso_twin{0}, g_mip_bound_pso_game{0}, g_mip_bound_rs_twin{0}, g_mip_bound_rs_game{0};
int var_level(uint32_t v) { return static_cast<int>(v & 15u) < kMipCount ? static_cast<int>(v & 15u) : 0; }
int var_af(uint32_t v) { return static_cast<int>(v >> 4 & 3u); }
uint32_t var_pack(int level, int af) { return static_cast<uint32_t>(level) | static_cast<uint32_t>(af) << 4; }
int af_index(uint32_t level) { return level >= 16 ? 3 : level >= 8 ? 2 : level ? 1 : 0; }
int nearest_mip_level(float b)
{
    int best = 0;
    for (int i = 1; i < kMipCount; ++i)
        if (std::fabs(b - kMipLevels[i]) < std::fabs(b - kMipLevels[best])) best = i;
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
template <typename SD>
bool sampler_wants_aniso(const SD& d)
{
    // Addressing does not determine the texture footprint. Atlased/unique-UV
    // materials commonly clamp; exclude data/comparison/point lookups, but do
    // not apply mip-bias's WRAP-only material heuristic to filtering.
    return D3D12_DECODE_FILTER_REDUCTION(d.Filter)==D3D12_FILTER_REDUCTION_TYPE_STANDARD&&d.MaxLOD>0&&
        (D3D12_DECODE_IS_ANISOTROPIC_FILTER(d.Filter)||D3D12_DECODE_MIN_FILTER(d.Filter)==D3D12_FILTER_TYPE_LINEAR);
}
float clamp_lod(float v) { return v < -16.0f ? -16.0f : (v > 15.99f ? 15.99f : v); }
// Anisotropic filtering 4x/8x/16x: eligible standard linear samplers, including
// clamp-addressed materials, are made anisotropic at that level.
// It is applied where the game samples its textures, before DLSS, so DLSS gets the sharper
// oblique detail. Comparison / min-max / point samplers are untouched. Unlike mip bias,
// an isotropic screen-space footprint needs no addressing-based exclusion.
std::atomic<uint32_t> g_aniso_level{0}; // 0 off, else 4 / 8 / 16 (menu)
std::atomic<uint64_t> g_aniso_samplers{0}, g_aniso_static{0}, g_aniso_already{0};
std::atomic<uint64_t> g_sampler_calls{0}, g_sampler2_calls{0}; // the game's CreateSampler / CreateSampler2 calls
template <typename SD>
bool apply_aniso(SD& d)
{
    const uint32_t level = g_aniso_level.load(std::memory_order_relaxed);
    if (!level || !sampler_wants_aniso(d)) return false;
    // Never lowers the game's own: an anisotropic sampler at or above the chosen level stays.
    if (D3D12_DECODE_IS_ANISOTROPIC_FILTER(d.Filter) && d.MaxAnisotropy >= level) {
        g_aniso_already.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    d.Filter = D3D12_FILTER_ANISOTROPIC;
    d.MaxAnisotropy = level;
    return true;
}
// Cort's total-bias rule: DLAA automatically targets exactly -1, including the
// native value. Native -1 needs no change; native 0 -> -1, native -0.5 -> -1,
// native -1.5 -> -1. Zero is the temporary comparison with the original sampler.
float biased(float native, float target)
{
    if (target == 0.0f) return native;
    return clamp_lod(target);
}
// The anisotropic part of a variant (no counters): as apply_aniso, at an explicit level.
template <typename SD>
bool aniso_to(SD& d, uint32_t level)
{
    if (!level || (D3D12_DECODE_IS_ANISOTROPIC_FILTER(d.Filter) && d.MaxAnisotropy >= level)) return false;
    d.Filter = D3D12_FILTER_ANISOTROPIC;
    d.MaxAnisotropy = level;
    return true;
}
// The game's static samplers as variant (level, af) wants them; returns how many changed.
uint32_t variant_samplers(std::vector<D3D12_STATIC_SAMPLER_DESC>& ss, int level, int af,uint32_t* aniso_changed=nullptr)
{
    uint32_t changed = 0;
    for (auto& x : ss) {
        const bool bias=sampler_wants_bias(x),aniso=sampler_wants_aniso(x);
        if(!bias&&!aniso)continue;
        bool c = false;
        const float nb = bias?biased(x.MipLODBias, kMipLevels[level]):x.MipLODBias;
        if (nb != x.MipLODBias) { x.MipLODBias = nb; c = true; }
        if (aniso&&aniso_to(x, kAfLevels[af])) { c = true;if(aniso_changed)++*aniso_changed; }
        if (c) ++changed;
    }
    return changed;
}
// The game's own biases on surface samplers, observed whether or not we bias.
std::mutex g_native_m;
uint64_t g_native_neg = 0, g_native_zero = 0, g_native_pos = 0;
uint64_t g_native_surface = 0,g_native_af16 = 0;
float g_native_min = 0.0f, g_native_max = 0.0f;
void note_native(float b,uint32_t anisotropy,bool bias_eligible)
{
    std::lock_guard<std::mutex> lock(g_native_m);
    if(bias_eligible){
        const bool first = !(g_native_neg || g_native_zero || g_native_pos);
        if (b < 0.0f) ++g_native_neg; else if (b > 0.0f) ++g_native_pos; else ++g_native_zero;
        if (first || b < g_native_min) g_native_min = b;
        if (first || b > g_native_max) g_native_max = b;
    }
    ++g_native_surface;if(anisotropy>=16)++g_native_af16;
}

// The game's own sampler table (event log, Cort 2026-11-06): every distinct sampler the game
// creates (heap, heap v2, static in a root signature) with how many times, so a log answers
// "are the materials already 16x / already biased?" without a capture.
struct SamplerRow { uint32_t kind, filter, aniso, u, v, w, cmp; float bias, min_lod, max_lod; uint64_t count; };
std::mutex g_sampler_table_m;
std::vector<SamplerRow> g_sampler_table;
std::atomic<uint64_t> g_sampler_table_total{0}, g_sampler_table_overflow{0};
template <typename D>
void note_sampler_desc(uint32_t kind, const D& d)
{
    g_sampler_table_total.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_sampler_table_m);
    for (SamplerRow& r : g_sampler_table)
        if (r.kind == kind && r.filter == uint32_t(d.Filter) && r.aniso == d.MaxAnisotropy && r.u == uint32_t(d.AddressU) &&
            r.v == uint32_t(d.AddressV) && r.w == uint32_t(d.AddressW) && r.cmp == uint32_t(d.ComparisonFunc) &&
            r.bias == d.MipLODBias && r.min_lod == d.MinLOD && r.max_lod == d.MaxLOD) { ++r.count; return; }
    if (g_sampler_table.size() >= 256) { g_sampler_table_overflow.fetch_add(1, std::memory_order_relaxed); return; }
    g_sampler_table.push_back({kind, uint32_t(d.Filter), d.MaxAnisotropy, uint32_t(d.AddressU), uint32_t(d.AddressV), uint32_t(d.AddressW),
                               uint32_t(d.ComparisonFunc), d.MipLODBias, d.MinLOD, d.MaxLOD, 1});
}

void STDMETHODCALLTYPE D_CreateSampler(ID3D12Device* dev, const D3D12_SAMPLER_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    if (!t_host_work) g_sampler_calls.fetch_add(1, std::memory_order_relaxed);
    if (d && !t_host_work) note_sampler_desc(0, *d);
    const float bias = g_mip_bias.load(std::memory_order_relaxed);
    const bool surface = d && !t_host_work && sampler_wants_bias(*d);
    const bool aniso_surface = d && !t_host_work && sampler_wants_aniso(*d);
    if (aniso_surface) note_native(d->MipLODBias,D3D12_DECODE_IS_ANISOTROPIC_FILTER(d->Filter)?d->MaxAnisotropy:0,surface);
    D3D12_SAMPLER_DESC m = d ? *d : D3D12_SAMPLER_DESC{};
    const bool bias_it = surface && bias != 0.0f && biased(d->MipLODBias, bias) != d->MipLODBias;
    if (bias_it) m.MipLODBias = biased(m.MipLODBias, bias);
    const bool aniso_it = aniso_surface && apply_aniso(m);
    if (bias_it || aniso_it) {
        o_CreateSampler(dev, &m, h);
        if (bias_it) g_biased_samplers.fetch_add(1, std::memory_order_relaxed);
        if (aniso_it) g_aniso_samplers.fetch_add(1, std::memory_order_relaxed);
    } else {
        o_CreateSampler(dev, d, h);
    }
    note_descriptor_write(h, NA_DESC_SAMPLER, nullptr, NA_UNKNOWN_U32, NA_UNKNOWN_U32, NA_UNKNOWN_U32);
}
#if defined(__ID3D12Device11_INTERFACE_DEFINED__)
// ID3D12Device11::CreateSampler2 (Agility SDK games): the same sampler as CreateSampler with
// integer border colours / flags. It was never hooked, so a game that creates its material
// samplers this way got neither the mip bias nor anisotropic filtering (Cort 2026-11-05:
// "x16 shows no difference", raised=0). Same rules as D_CreateSampler.
void STDMETHODCALLTYPE D_CreateSampler2(ID3D12Device11* dev, const D3D12_SAMPLER_DESC2* d, D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    if (!t_host_work) g_sampler2_calls.fetch_add(1, std::memory_order_relaxed);
    if (d && !t_host_work) note_sampler_desc(1, *d);
    const float bias = g_mip_bias.load(std::memory_order_relaxed);
    const bool surface = d && !t_host_work && sampler_wants_bias(*d);
    const bool aniso_surface = d && !t_host_work && sampler_wants_aniso(*d);
    if (aniso_surface) note_native(d->MipLODBias,D3D12_DECODE_IS_ANISOTROPIC_FILTER(d->Filter)?d->MaxAnisotropy:0,surface);
    D3D12_SAMPLER_DESC2 m = d ? *d : D3D12_SAMPLER_DESC2{};
    const bool bias_it = surface && bias != 0.0f && biased(d->MipLODBias, bias) != d->MipLODBias;
    if (bias_it) m.MipLODBias = biased(m.MipLODBias, bias);
    const bool aniso_it = aniso_surface && apply_aniso(m);
    if (bias_it || aniso_it) {
        o_CreateSampler2(dev, &m, h);
        if (bias_it) g_biased_samplers.fetch_add(1, std::memory_order_relaxed);
        if (aniso_it) g_aniso_samplers.fetch_add(1, std::memory_order_relaxed);
    } else {
        o_CreateSampler2(dev, d, h);
    }
    note_descriptor_write(h, NA_DESC_SAMPLER, nullptr, NA_UNKNOWN_U32, NA_UNKNOWN_U32, NA_UNKNOWN_U32);
}
#endif

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
        std::shared_lock<RegistryLock> lock(g_m);
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
    std::shared_lock<RegistryLock> lock(g_m);
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
    if (t_host_work) return o_Wait(q, f, v); // private completion, as for D_Signal
    const auto token = ExecuteBegin(q);
    const HRESULT hr = o_Wait(q, f, v);
    sync_event(NA_EV_QUEUE_WAIT, token, f, v, hr);
    return hr;
}

void STDMETHODCALLTYPE D_ExecuteCommandLists(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists)
{
    const CostScope cost(kCostExec, !t_host_work);
    ReleaseParkedTwins();
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
        std::shared_lock<RegistryLock> lock(g_m);
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

thread_local std::vector<ShaderDimension> g_next_dimensions;
thread_local std::vector<ShaderResource> g_next_shader_resources;
thread_local bool g_next_reflection_complete=true;
thread_local bool g_next_shader_readonly=true;
thread_local uint32_t g_next_shader_rejection=0;
thread_local bool g_next_pixel_position=false;
void bytecode(na_ev_pipeline_created& e, uint32_t stage, const D3D12_SHADER_BYTECODE& b)
{
    if (!b.pShaderBytecode || !b.BytecodeLength) return;
    e.stage_hash[stage] = fnv1a(b.pShaderBytecode, b.BytecodeLength);
    e.stage_bytes[stage] = static_cast<uint32_t>(b.BytecodeLength);
    if(g_operation_snapshots.load()&&!t_host_work){
        const uint32_t visibility[]={D3D12_SHADER_VISIBILITY_VERTEX,D3D12_SHADER_VISIBILITY_PIXEL,D3D12_SHADER_VISIBILITY_DOMAIN,D3D12_SHADER_VISIBILITY_HULL,D3D12_SHADER_VISIBILITY_GEOMETRY};
        bool pixel_position=false;
        const bool reflected=ReflectDimensions(b,g_next_dimensions,stage<5?visibility[stage]:D3D12_SHADER_VISIBILITY_ALL,&g_next_shader_resources,
            stage==NA_STAGE_PS?&pixel_position:nullptr);
        g_next_reflection_complete &= reflected;
        uint32_t rejection=0;
        g_next_shader_readonly &= ExecutableReadOnly(b,stage==NA_STAGE_PS?&pixel_position:nullptr,&rejection);
        g_next_shader_rejection|=rejection;
        if(stage==NA_STAGE_PS)g_next_pixel_position=pixel_position;
    }
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

// RGB replay uses out=a+dst*b. Opaque, straight/premultiplied alpha-over,
// and pure colour multiplication fit it without saturating the grey witness.
// MIN/MAX/logic/additive-clipping and destination-alpha coupling instead keep
// the original full picture and use the draw-derived protection method.
thread_local bool g_next_hud_affine=false;
thread_local uint8_t g_next_blend_class=0;
// RT0 colour blend as the HUD un-blend replay sees it (BlendClass in producers.hpp): the result
// must be A + M * destination per channel with A, M independent of the destination, which
// rules out MIN/MAX, logic ops and any factor reading the destination's ALPHA (the replay
// targets carry alpha 1, the game's do not). A destination COLOUR factor on the source side is
// still linear in the destination. DestBlend ZERO with no destination factor: the draw replaces
// what is there, exactly like an opaque draw, although blending is switched on.
uint8_t blend_class(const D3D12_BLEND_DESC& blend)
{
    const auto& b=blend.RenderTarget[0];
    if(b.LogicOpEnable)return 4;
    if(!b.BlendEnable)return 1;
    if(b.BlendOp==D3D12_BLEND_OP_MIN||b.BlendOp==D3D12_BLEND_OP_MAX)return 4;
    const auto dest_alpha=[](D3D12_BLEND f){return f==D3D12_BLEND_DEST_ALPHA||f==D3D12_BLEND_INV_DEST_ALPHA||f==D3D12_BLEND_SRC_ALPHA_SAT;};
    const auto dest_colour=[](D3D12_BLEND f){return f==D3D12_BLEND_DEST_COLOR||f==D3D12_BLEND_INV_DEST_COLOR;};
    if(dest_alpha(b.SrcBlend)||dest_alpha(b.DestBlend)||dest_colour(b.DestBlend))return 4;
    if(b.DestBlend==D3D12_BLEND_ZERO&&!dest_colour(b.SrcBlend))return 3;
    return 2;
}
thread_local bool g_next_stream_output=false;
bool hud_affine(const D3D12_BLEND_DESC& blend)
{
    const auto& b=blend.RenderTarget[0];
    if(b.LogicOpEnable)return false;
    if(!b.BlendEnable)return true;
    if(b.BlendOp!=D3D12_BLEND_OP_ADD)return false;
    return ((b.SrcBlend==D3D12_BLEND_SRC_ALPHA||b.SrcBlend==D3D12_BLEND_ONE)&&b.DestBlend==D3D12_BLEND_INV_SRC_ALPHA)||
           (b.SrcBlend==D3D12_BLEND_ZERO&&(b.DestBlend==D3D12_BLEND_SRC_COLOR||b.DestBlend==D3D12_BLEND_INV_SRC_COLOR));
}

void pipeline_unknowns(na_ev_pipeline_created& e)
{
    g_next_hud_affine=false;
    g_next_blend_class=0;
    g_next_stream_output=false;
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
    if(g_operation_snapshots.load()&&!t_host_work){
        std::unique_lock<RegistryLock> lock(g_m);
        const auto found=g_objs.find(canon(obj));if(found!=g_objs.end()){
            found->second.dimensions=g_next_dimensions;found->second.shader_resources=g_next_shader_resources;
            found->second.reflection_complete=g_next_reflection_complete;
            found->second.shader_rejection=g_next_shader_rejection|(g_next_stream_output?ShaderStreamOutput:0u);}
        if(found!=g_objs.end())found->second.shader_readonly=g_next_shader_readonly&&!g_next_stream_output&&
            std::none_of(g_next_shader_resources.begin(),g_next_shader_resources.end(),[](const auto& r){return r.type==D3D12_DESCRIPTOR_RANGE_TYPE_UAV;});
    }
    {std::unique_lock<RegistryLock> lock(g_m);const auto found=g_objs.find(canon(obj));if(found!=g_objs.end()){
        found->second.native_hud_affine=g_next_hud_affine;found->second.blend_class=g_next_blend_class;found->second.pixel_position=g_next_pixel_position;}}
    emit(e, NA_GROUP_PIPELINE);
}

NA_ORIG(CreateRootSignature, HRESULT, ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);
NA_ORIG(CreateGraphicsPipelineState, HRESULT, ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
NA_ORIG(CreateComputePipelineState, HRESULT, ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
NA_ORIG(CreatePipelineState, HRESULT, ID3D12Device2*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);

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

// ---- Live sampler variants: creation copies and twin building.
// A creation copy owns everything its description pointed at (shader bytecode, shared
// between pipelines that use the same shader; input layouts; stream-output declarations;
// view-instancing locations), so the background worker can build more twins long after
// the game's call returned. The cached-PSO blob is never copied: it belongs to the game's
// variant. A device reference is held for as long as the entry exists.
struct VariantRecipe {
    enum Kind : uint8_t { RootSignature, Graphics, Compute, Stream } kind = RootSignature;
    Microsoft::WRL::ComPtr<ID3D12Device> dev;
    UINT node_mask = 0;               // root signatures
    std::vector<uint8_t> blob;
    const void* rs_key = nullptr;     // pipelines: the root signature they were built with
    na_object_id rs_id = 0;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC g{};
    D3D12_COMPUTE_PIPELINE_STATE_DESC c{};
    std::vector<uint8_t> stream;
    size_t rs_offset = SIZE_MAX;
    std::vector<std::shared_ptr<const std::vector<uint8_t>>> code;
    std::vector<D3D12_INPUT_ELEMENT_DESC> elems;
    std::vector<D3D12_SO_DECLARATION_ENTRY> so;
    std::vector<UINT> strides;
    std::vector<D3D12_VIEW_INSTANCE_LOCATION> vil;
    std::deque<std::string> names;    // stable addresses for semantic names
    VariantRecipe() = default;
    VariantRecipe(const VariantRecipe&) = delete;
    VariantRecipe& operator=(const VariantRecipe&) = delete;
    IUnknown* make(ID3D12RootSignature* rs) const;
};
std::mutex g_code_m;
std::unordered_map<uint64_t, std::vector<std::weak_ptr<const std::vector<uint8_t>>>>& g_code =
    *new std::unordered_map<uint64_t, std::vector<std::weak_ptr<const std::vector<uint8_t>>>>();
D3D12_SHADER_BYTECODE own_code(VariantRecipe& r, const D3D12_SHADER_BYTECODE& b)
{
    if (!b.pShaderBytecode || !b.BytecodeLength) return {};
    const auto* bytes = static_cast<const uint8_t*>(b.pShaderBytecode);
    // DXBC/DXIL containers carry their content digest right after the magic; the size and
    // the first bytes key the cache, and a hit is compared in full before it is shared.
    const uint64_t k = fnv1a(bytes, b.BytecodeLength < 64 ? b.BytecodeLength : 64) ^ (static_cast<uint64_t>(b.BytecodeLength) * 0x9E3779B97F4A7C15ull);
    std::shared_ptr<const std::vector<uint8_t>> sp;
    {
        std::lock_guard<std::mutex> lk(g_code_m);
        auto& bucket = g_code[k];
        bucket.erase(std::remove_if(bucket.begin(), bucket.end(), [](const auto& w) { return w.expired(); }), bucket.end());
        for (const auto& w : bucket)
            if (auto hit = w.lock())
                if (hit->size() == b.BytecodeLength && std::memcmp(hit->data(), bytes, b.BytecodeLength) == 0) { sp = hit; break; }
        if (!sp) {
            sp = std::make_shared<const std::vector<uint8_t>>(bytes, bytes + b.BytecodeLength);
            bucket.push_back(sp);
        }
    }
    r.code.push_back(sp);
    return {sp->data(), sp->size()};
}
D3D12_INPUT_LAYOUT_DESC own_layout(VariantRecipe& r, const D3D12_INPUT_LAYOUT_DESC& d)
{
    if (!d.NumElements || !d.pInputElementDescs) return {nullptr, 0};
    r.elems.assign(d.pInputElementDescs, d.pInputElementDescs + d.NumElements);
    for (auto& e : r.elems)
        if (e.SemanticName) { r.names.emplace_back(e.SemanticName); e.SemanticName = r.names.back().c_str(); }
    return {r.elems.data(), d.NumElements};
}
D3D12_STREAM_OUTPUT_DESC own_so(VariantRecipe& r, const D3D12_STREAM_OUTPUT_DESC& d)
{
    D3D12_STREAM_OUTPUT_DESC o = d;
    if (d.NumEntries && d.pSODeclaration) {
        r.so.assign(d.pSODeclaration, d.pSODeclaration + d.NumEntries);
        for (auto& e : r.so)
            if (e.SemanticName) { r.names.emplace_back(e.SemanticName); e.SemanticName = r.names.back().c_str(); }
        o.pSODeclaration = r.so.data();
    } else { o.pSODeclaration = nullptr; o.NumEntries = 0; }
    if (d.NumStrides && d.pBufferStrides) { r.strides.assign(d.pBufferStrides, d.pBufferStrides + d.NumStrides); o.pBufferStrides = r.strides.data(); }
    else { o.pBufferStrides = nullptr; o.NumStrides = 0; }
    return o;
}
std::shared_ptr<const VariantRecipe> recipe_of(ID3D12Device* dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d)
{
    auto r = std::make_shared<VariantRecipe>();
    r->kind = VariantRecipe::Graphics;
    r->dev = dev;
    r->g = d;
    r->g.VS = own_code(*r, d.VS); r->g.PS = own_code(*r, d.PS); r->g.DS = own_code(*r, d.DS);
    r->g.HS = own_code(*r, d.HS); r->g.GS = own_code(*r, d.GS);
    r->g.StreamOutput = own_so(*r, d.StreamOutput);
    r->g.InputLayout = own_layout(*r, d.InputLayout);
    r->g.CachedPSO = {};
    r->g.pRootSignature = nullptr;
    return r;
}
std::shared_ptr<const VariantRecipe> recipe_of(ID3D12Device* dev, const D3D12_COMPUTE_PIPELINE_STATE_DESC& d)
{
    auto r = std::make_shared<VariantRecipe>();
    r->kind = VariantRecipe::Compute;
    r->dev = dev;
    r->c = d;
    r->c.CS = own_code(*r, d.CS);
    r->c.CachedPSO = {};
    r->c.pRootSignature = nullptr;
    return r;
}
// A stream with a subobject this build does not know cannot be copied safely: no recipe
// (its twins at creation still work; a background build falls back for it).
std::shared_ptr<const VariantRecipe> recipe_of(ID3D12Device* dev, const D3D12_PIPELINE_STATE_STREAM_DESC& d)
{
    if (!d.pPipelineStateSubobjectStream || !d.SizeInBytes) return nullptr;
    auto r = std::make_shared<VariantRecipe>();
    r->kind = VariantRecipe::Stream;
    r->dev = dev;
    const auto* p = static_cast<const uint8_t*>(d.pPipelineStateSubobjectStream);
    r->stream.assign(p, p + d.SizeInBytes);
    uint8_t* q = r->stream.data();
    size_t at = 0;
    while (at + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) <= d.SizeInBytes) {
        const auto type = *reinterpret_cast<const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE*>(q + at);
        const size_t n = sub_size(type);
        if (!n || at + n > d.SizeInBytes) return nullptr;
        uint8_t* s = q + at;
        switch (type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: r->rs_offset = at; break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: {
            auto& b = reinterpret_cast<Sub<D3D12_SHADER_BYTECODE>*>(s)->inner;
            b = own_code(*r, b);
            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: {
            auto& so = reinterpret_cast<Sub<D3D12_STREAM_OUTPUT_DESC>*>(s)->inner;
            so = own_so(*r, so);
            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: {
            auto& il = reinterpret_cast<Sub<D3D12_INPUT_LAYOUT_DESC>*>(s)->inner;
            il = own_layout(*r, il);
            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
            reinterpret_cast<Sub<D3D12_CACHED_PIPELINE_STATE>*>(s)->inner = {};
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: {
            auto& vi = reinterpret_cast<Sub<D3D12_VIEW_INSTANCING_DESC>*>(s)->inner;
            if (vi.ViewInstanceCount && vi.pViewInstanceLocations) {
                r->vil.assign(vi.pViewInstanceLocations, vi.pViewInstanceLocations + vi.ViewInstanceCount);
                vi.pViewInstanceLocations = r->vil.data();
            } else vi.pViewInstanceLocations = nullptr;
            break;
        }
        default: break;
        }
        at += n;
    }
    if (r->rs_offset == SIZE_MAX) return nullptr; // no root-signature subobject: nothing to swap
    return r;
}
IUnknown* VariantRecipe::make(ID3D12RootSignature* trs) const
{
    const HostWorkScope host;
    ID3D12PipelineState* t = nullptr;
    switch (kind) {
    case Graphics: {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = g;
        d.pRootSignature = trs;
        return SUCCEEDED(o_CreateGraphicsPipelineState(dev.Get(), &d, IID_PPV_ARGS(&t))) ? t : nullptr;
    }
    case Compute: {
        D3D12_COMPUTE_PIPELINE_STATE_DESC d = c;
        d.pRootSignature = trs;
        return SUCCEEDED(o_CreateComputePipelineState(dev.Get(), &d, IID_PPV_ARGS(&t))) ? t : nullptr;
    }
    case Stream: {
        Microsoft::WRL::ComPtr<ID3D12Device2> d2;
        if (FAILED(dev.As(&d2))) return nullptr;
        std::vector<uint8_t> copy(stream);
        std::memcpy(copy.data() + rs_offset + offsetof(Sub<ID3D12RootSignature*>, inner), &trs, sizeof(trs));
        D3D12_PIPELINE_STATE_STREAM_DESC sd{copy.size(), copy.data()};
        return SUCCEEDED(o_CreatePipelineState(d2.Get(), &sd, IID_PPV_ARGS(&t))) ? t : nullptr;
    }
    default: return nullptr;
    }
}
// Root-signature twins for anisotropic index `af` at the mip levels in `levels` that `base`
// (the current set, may be null) has not prepared yet. Level 0 at index 0 is the game's own.
// Returns null when nothing new was prepared.
std::shared_ptr<Obj::Twins> build_rs_set(const VariantRecipe& r, int af, uint32_t levels, const Obj::Twins* base)
{
    ID3D12VersionedRootSignatureDeserializer* rd = nullptr;
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* u = nullptr;
    if (FAILED(D3D12CreateVersionedRootSignatureDeserializer(r.blob.data(), r.blob.size(), IID_PPV_ARGS(&rd))) || !rd ||
        !(u = rd->GetUnconvertedRootSignatureDesc()) ||
        (u->Version != D3D_ROOT_SIGNATURE_VERSION_1_0 && u->Version != D3D_ROOT_SIGNATURE_VERSION_1_1)) {
        if (rd) rd->Release();
        return nullptr;
    }
    const UINT ns = u->Version == D3D_ROOT_SIGNATURE_VERSION_1_0 ? u->Desc_1_0.NumStaticSamplers : u->Desc_1_1.NumStaticSamplers;
    const D3D12_STATIC_SAMPLER_DESC* src = u->Version == D3D_ROOT_SIGNATURE_VERSION_1_0 ? u->Desc_1_0.pStaticSamplers : u->Desc_1_1.pStaticSamplers;
    auto set = base ? std::make_shared<Obj::Twins>(*base) : std::make_shared<Obj::Twins>();
    bool added = false;
    for (int level = 0; level < kMipCount; ++level) {
        if (!(levels >> level & 1u) || set->built[level]) continue;
        set->built[level] = true;
        added = true;
        std::vector<D3D12_STATIC_SAMPLER_DESC> ss(src, src + ns);
        uint32_t aniso_changed=0;
        const uint32_t changed = (level == 0 && af == 0) ? 0u : variant_samplers(ss, level, af,&aniso_changed);
        if (!changed) { set->same[level] = true; continue; }
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC copy = *u;
        if (u->Version == D3D_ROOT_SIGNATURE_VERSION_1_0) copy.Desc_1_0.pStaticSamplers = ss.data();
        else copy.Desc_1_1.pStaticSamplers = ss.data();
        ID3DBlob* b = nullptr;
        ID3DBlob* err = nullptr;
        ID3D12RootSignature* twin = nullptr;
        if (SUCCEEDED(D3D12SerializeVersionedRootSignature(&copy, &b, &err)) && b) {
            const HostWorkScope host;
            if (FAILED(o_CreateRootSignature(r.dev.Get(), r.node_mask, b->GetBufferPointer(), b->GetBufferSize(), IID_PPV_ARGS(&twin)))) twin = nullptr;
        }
        if (b) b->Release();
        if (err) err->Release();
        if (!twin) { g_mip_twin_fail.fetch_add(1, std::memory_order_relaxed); continue; }
        // D3D12 returns the same object for identical bytes: a twin that is one of the game's
        // own root signatures is not kept (holding it could make two game root signatures own
        // each other). That level has no twin: recordings at it fall back to the game's objects.
        bool game_obj = false;
        {
            std::shared_lock<RegistryLock> lock(g_m);
            game_obj = g_objs.find(canon(twin)) != g_objs.end();
        }
        if (game_obj) {
            twin->Release();
            g_mip_shared_skips.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        set->obj[level] = twin;
        g_mip_twin_rs.fetch_add(1, std::memory_order_relaxed);
        g_var_static_changed[level * 4 + af].fetch_add(changed, std::memory_order_relaxed);
        g_var_aniso_changed[level * 4 + af].fetch_add(aniso_changed, std::memory_order_relaxed);
    }
    rd->Release();
    return added ? set : nullptr;
}
// Pipeline twins matching a root-signature set: one per level that set prepared and `base`
// has not. Same root signature -> the game's pipeline is right; no root twin -> none.
template <typename Make>
std::shared_ptr<Obj::Twins> build_pso_set(Make&& make, const Obj::Twins& rs_set, const Obj::Twins* base,uint32_t levels)
{
    auto set = base ? std::make_shared<Obj::Twins>(*base) : std::make_shared<Obj::Twins>();
    bool added = false;
    for (int level = 0; level < kMipCount; ++level) {
        if (!(levels>>level&1u) || !rs_set.built[level] || set->built[level]) continue;
        set->built[level] = true;
        added = true;
        if (rs_set.same[level]) { set->same[level] = true; continue; }
        if (!rs_set.obj[level]) continue; // the root twin failed: recordings at this level fall back
        IUnknown* t = make(static_cast<ID3D12RootSignature*>(rs_set.obj[level]));
        if (t) { set->obj[level] = t; g_mip_twin_pso.fetch_add(1, std::memory_order_relaxed); }
        else g_mip_twin_fail.fetch_add(1, std::memory_order_relaxed);
    }
    return added ? set : nullptr;
}
// Levels to prepare at creation for anisotropic index `af`: the saved choice's levels
// (g_mip_level_mask at g_pre_af) plus the chosen and the in-use variants.
uint32_t creation_levels(int af)
{
    uint32_t m = 0;
    if (af == g_pre_af.load(std::memory_order_relaxed)) m |= g_mip_level_mask.load(std::memory_order_relaxed);
    for (const uint32_t v : {g_var_want.load(std::memory_order_relaxed), g_var_live.load(std::memory_order_relaxed)})
        if (var_af(v) == af) m |= 1u << var_level(v);
    if (af == 0) m &= ~1u; // the game's own
    return m;
}

HRESULT STDMETHODCALLTYPE D_CreateRootSignature(ID3D12Device* dev, UINT mask, const void* blob, SIZE_T size, REFIID riid, void** out)
{
    Depth depth("CreateRootSignature");
    // Static samplers. Live variants on (the default): the game's root signature is created
    // exactly as the game asked, and twins carry the mip bias / anisotropic filtering (see
    // the live sampler variants above). Off ([j2] mip_bias_live=0): it is re-serialised with
    // the chosen variant (versions 1.0 / 1.1; objects created from then on only).
    ID3DBlob* rebuilt = nullptr;
    bool decodable = false, has_surface = false;
    const bool live = g_mip_live_enabled.load(std::memory_order_relaxed);
    if (blob && size && !t_host_work && depth.outermost()) { // also when off: native biases are recorded
        ID3D12VersionedRootSignatureDeserializer* rd = nullptr;
        const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* u = nullptr;
        if (SUCCEEDED(D3D12CreateVersionedRootSignatureDeserializer(blob, size, IID_PPV_ARGS(&rd))) && rd &&
            (u = rd->GetUnconvertedRootSignatureDesc()) != nullptr &&
            (u->Version == D3D_ROOT_SIGNATURE_VERSION_1_0 || u->Version == D3D_ROOT_SIGNATURE_VERSION_1_1)) {
            decodable = true;
            const UINT ns = u->Version == D3D_ROOT_SIGNATURE_VERSION_1_0 ? u->Desc_1_0.NumStaticSamplers : u->Desc_1_1.NumStaticSamplers;
            const D3D12_STATIC_SAMPLER_DESC* src = u->Version == D3D_ROOT_SIGNATURE_VERSION_1_0 ? u->Desc_1_0.pStaticSamplers : u->Desc_1_1.pStaticSamplers;
            for (UINT i = 0; i < ns; ++i) note_sampler_desc(2, src[i]);
            for (UINT i = 0; i < ns; ++i) if (sampler_wants_aniso(src[i])) {
                has_surface=true;note_native(src[i].MipLODBias,D3D12_DECODE_IS_ANISOTROPIC_FILTER(src[i].Filter)?src[i].MaxAnisotropy:0,sampler_wants_bias(src[i]));
            }
            const uint32_t want = g_var_want.load(std::memory_order_relaxed);
            if (!live && has_surface && want) {
                std::vector<D3D12_STATIC_SAMPLER_DESC> ss(src, src + ns);
                uint32_t biased_n = 0, aniso_n = 0;
                for (auto& x : ss) {
                    const bool bias=sampler_wants_bias(x),aniso=sampler_wants_aniso(x);
                    if(!bias&&!aniso)continue;
                    const float nb = bias?biased(x.MipLODBias, kMipLevels[var_level(want)]):x.MipLODBias;
                    if (nb != x.MipLODBias) { x.MipLODBias = nb; ++biased_n; }
                    if (aniso&&aniso_to(x, kAfLevels[var_af(want)])) ++aniso_n;
                }
                if (biased_n || aniso_n) {
                    D3D12_VERSIONED_ROOT_SIGNATURE_DESC copy = *u;
                    if (u->Version == D3D_ROOT_SIGNATURE_VERSION_1_0) copy.Desc_1_0.pStaticSamplers = ss.data();
                    else copy.Desc_1_1.pStaticSamplers = ss.data();
                    ID3DBlob* err = nullptr;
                    if (FAILED(D3D12SerializeVersionedRootSignature(&copy, &rebuilt, &err))) rebuilt = nullptr;
                    if (err) err->Release();
                    if (rebuilt) {
                        g_biased_static.fetch_add(biased_n, std::memory_order_relaxed);
                        g_aniso_static.fetch_add(aniso_n, std::memory_order_relaxed);
                    }
                }
            }
        }
        if (rd) rd->Release();
    }
    const HRESULT hr = rebuilt ? o_CreateRootSignature(dev, mask, rebuilt->GetBufferPointer(), rebuilt->GetBufferSize(), riid, out)
                               : o_CreateRootSignature(dev, mask, blob, size, riid, out);
    if (rebuilt) rebuilt->Release();
    std::shared_ptr<const VariantRecipe> recipe;
    std::shared_ptr<Obj::Twins> sets[4];
    if (live && decodable && has_surface && SUCCEEDED(hr) && out && *out) {
        ReleaseParkedTwins();
        auto r = std::make_shared<VariantRecipe>();
        r->kind = VariantRecipe::RootSignature;
        r->dev = dev;
        r->node_mask = mask;
        r->blob.assign(static_cast<const uint8_t*>(blob), static_cast<const uint8_t*>(blob) + size);
        for (int af = 0; af < 4; ++af)
            if (const uint32_t lv = creation_levels(af)) sets[af] = build_rs_set(*r, af, lv, nullptr);
        recipe = std::move(r);
    }
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
        std::unique_lock<RegistryLock> lock(g_m);
        auto it = g_objs.find(canon(obj));
        if (it != g_objs.end()) {
            if (it->second.retirement_unobservable) ev.e.hdr.flags |= NA_EVF_METHOD_UNOBSERVED;
            it->second.created.assign(raw, raw + ev.e.hdr.size_bytes);
            it->second.root_params.assign(params, params + ev.e.param_count);
            it->second.root_ranges.assign(ranges, ranges + nranges);
            for (int af = 0; af < 4; ++af) sets[af].swap(it->second.af_sets[af]); // previous sets released after g_m
            recipe.swap(it->second.recipe);
            it->second.variant_kind = it->second.recipe ? 1 : 0;
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

// Live sampler variants: a pipeline's twins follow its root signature's. A pipeline on a
// sampler-dependent root signature keeps a creation copy (`recipe`, null when its stream
// cannot be copied) and gets twins for every set its root signature has now; later sets
// are built by the background worker. A pipeline with an embedded root signature cannot
// be twinned: recordings that bind it fall back to the game's objects.
template <typename Make, typename RecipeFn>
void make_pso_twins(IUnknown* pso, const void* rs, Make make, RecipeFn&& recipe_fn)
{
    if (!pso || !g_mip_live_enabled.load(std::memory_order_relaxed)) return;
    ReleaseParkedTwins();
    uint8_t kind = 0;
    na_object_id rs_id = 0;
    std::shared_ptr<Obj::Twins> rs_sets[4], sets[4];
    if (!rs) {
        kind = 2;
        g_mip_untwinned.fetch_add(1, std::memory_order_relaxed);
    } else {
        std::shared_lock<RegistryLock> lock(g_m);
        const auto it = g_objs.find(canon(rs));
        if (it != g_objs.end() && it->second.variant_kind == 1) {
            kind = 1;
            rs_id = it->second.id;
            for (int af = 0; af < 4; ++af) rs_sets[af] = it->second.af_sets[af];
        }
    }
    if (!kind) return; // root signature needs no twins: the pipeline needs none either
    std::shared_ptr<const VariantRecipe> recipe;
    if (kind == 1) {
        recipe = recipe_fn();
        // No pipeline twin is compiled here: this is the game's own creation call, often on the
        // thread that pumps its window, and compiling a twin per level doubled or tripled the
        // load (c3853df hung SE5's start-up for 40 s+ with AF 16x saved). The background worker
        // builds the chosen variant; until then lookups use the game's pipeline and wake it.
        (void)make;
        for (int af = 0; af < 4; ++af) (void)rs_sets[af];
        if (recipe) {
            auto r = std::const_pointer_cast<VariantRecipe>(recipe);
            r->rs_key = canon(rs);
            r->rs_id = rs_id;
        }
    }
    bool published_recipe = false;
    {
        std::unique_lock<RegistryLock> lock(g_m);
        const auto it = g_objs.find(canon(pso));
        if (it != g_objs.end()) {
            it->second.variant_kind = kind;
            for (int af = 0; af < 4; ++af) sets[af].swap(it->second.af_sets[af]);
            recipe.swap(it->second.recipe);
            published_recipe = it->second.recipe != nullptr;
        }
    }
    // Wake only after publication: a worker woken before the swap could miss this recipe.
    if (published_recipe && (g_var_want.load(std::memory_order_relaxed) || g_mip_prewarm.load(std::memory_order_relaxed)) &&
        !g_var_missing.exchange(true, std::memory_order_relaxed)) g_var_cv.notify_one();
    // replaced sets (if any) are released outside g_m
}

HRESULT STDMETHODCALLTYPE D_CreateGraphicsPipelineState(ID3D12Device* dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, REFIID riid, void** out)
{
    const CostScope cost(kCostPso, !t_host_work);
    Depth depth("CreateGraphicsPipelineState");
    const HRESULT hr = o_CreateGraphicsPipelineState(dev, d, riid, out);
    IUnknown* obj = depth.outermost() ? out_object(hr, out) : nullptr;
    if (!obj || !d) return hr;
    g_next_dimensions.clear();g_next_shader_resources.clear();g_next_reflection_complete=true;g_next_shader_readonly=true;g_next_shader_rejection=0;g_next_pixel_position=false;
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
    g_next_hud_affine=hud_affine(d->BlendState);
    g_next_blend_class=blend_class(d->BlendState);
    g_next_stream_output=d->StreamOutput.NumEntries!=0;
    emit_pipeline(dev, obj, e, d->pRootSignature);
    make_pso_twins(obj, d->pRootSignature, [&](int, ID3D12RootSignature* trs) -> IUnknown* {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC c = *d;
        c.pRootSignature = trs;
        c.CachedPSO = {}; // the cache belongs to the game's variant
        ID3D12PipelineState* t = nullptr;
        return SUCCEEDED(o_CreateGraphicsPipelineState(dev, &c, IID_PPV_ARGS(&t))) ? t : nullptr;
    }, [&] { return recipe_of(dev, *d); });
    return hr;
}

HRESULT STDMETHODCALLTYPE D_CreateComputePipelineState(ID3D12Device* dev, const D3D12_COMPUTE_PIPELINE_STATE_DESC* d, REFIID riid, void** out)
{
    const CostScope cost(kCostPso, !t_host_work);
    Depth depth("CreateComputePipelineState");
    const HRESULT hr = o_CreateComputePipelineState(dev, d, riid, out);
    IUnknown* obj = depth.outermost() ? out_object(hr, out) : nullptr;
    if (!obj || !d) return hr;
    g_next_dimensions.clear();g_next_shader_resources.clear();g_next_reflection_complete=true;g_next_shader_readonly=true;g_next_shader_rejection=0;g_next_pixel_position=false;
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
    }, [&] { return recipe_of(dev, *d); });
    return hr;
}


HRESULT STDMETHODCALLTYPE D_CreatePipelineState(ID3D12Device2* dev, const D3D12_PIPELINE_STATE_STREAM_DESC* d, REFIID riid, void** out)
{
    const CostScope cost(kCostPso, !t_host_work);
    Depth depth("CreatePipelineState");
    const HRESULT hr = o_CreatePipelineState(dev, d, riid, out);
    IUnknown* obj = depth.outermost() ? out_object(hr, out) : nullptr;
    if (!obj || !d || !d->pPipelineStateSubobjectStream) return hr;
    g_next_dimensions.clear();g_next_shader_resources.clear();g_next_reflection_complete=true;g_next_shader_readonly=true;g_next_shader_rejection=0;g_next_pixel_position=false;
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
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: blend_mask(e, inner_of<D3D12_BLEND_DESC>(s)); g_next_hud_affine=hud_affine(inner_of<D3D12_BLEND_DESC>(s)); g_next_blend_class=blend_class(inner_of<D3D12_BLEND_DESC>(s)); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: g_next_stream_output=inner_of<D3D12_STREAM_OUTPUT_DESC>(s).NumEntries!=0;break;
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
        }, [&] { return recipe_of(dev, *d); });
    else make_pso_twins(obj, nullptr, [](int, ID3D12RootSignature*) -> IUnknown* { return nullptr; },
                        [] { return std::shared_ptr<const VariantRecipe>(); });
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
    std::shared_lock<RegistryLock> lock(g_m);
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
            std::shared_lock<RegistryLock> lock(g_m);
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

template<class Fn>
bool recorded_locked(ID3D12GraphicsCommandList* l,Rec& r,Fn&& fn){
    if(t_host_work||!na::observe_internal::Started()||!l)return false;
    for(int attempt=0;attempt<2;++attempt){
        {std::shared_lock<RegistryLock> lock(g_m);na_object_id id=0;
            if(auto* ls=list_state_locked(l,&id)){
                r.list=id;r.gen=ls->generation_known.load()?ls->generation.load():NA_UNKNOWN_U64;r.op_index=ls->op_index.fetch_add(1)+1;
                if(!ls->recording_observed.load()||!ls->coverage_complete.load()||r.gen==NA_UNKNOWN_U64)r.flags|=NA_EVF_METHOD_UNOBSERVED;
                std::lock_guard<std::mutex> bind(ls->bind_m);fn(*ls);return true;
            }
        }
        preexisting_list(l);
    }
    return false;
}

void emit_op(const Rec& r, uint32_t op, std::initializer_list<uint64_t> a, const void* payload = nullptr, uint32_t bytes = 0,
             uint32_t stride = 0, uint32_t extra_flags = 0, uint32_t group = NA_GROUP_PASS)
{
    const CostScope cost(kCostEmit);
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

// g_m and the list's bind_m are held. Registry changes invalidate the raw
// pointers BEFORE they may be inspected; game-owned heaps retain their native
// lifetime rules. A table lookup examines at most the two currently bound heaps.
void bound_heaps_locked(ListState& ls){
    if(ls.heap_metadata_revision==g_heap_registry_revision)return;
    ls.heap_metadata[0]=ls.heap_metadata[1]=nullptr;
    for(uint32_t i=0;i<ls.nheaps;++i){const auto* key=canon(ls.heaps[i]);
        for(const auto& h:g_heaps)if(h->key==ls.heaps[i]||h->key==key){ls.heap_metadata[i]=h.get();break;}}
    ls.heap_metadata_revision=g_heap_registry_revision;
}
DescHeap* bound_heap_for_gpu_locked(ListState& ls,uint64_t ptr,uint32_t* slot){
    bound_heaps_locked(ls);
    for(uint32_t i=0;i<ls.nheaps;++i){auto* h=ls.heap_metadata[i];if(!h||!h->gpu_start||!h->increment||ptr<h->gpu_start)continue;
        const auto offset=ptr-h->gpu_start;if(offset%h->increment||offset/h->increment>=h->count)continue;
        *slot=static_cast<uint32_t>(offset/h->increment);return h;}
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
    const SlotShared stripe(h, slot);
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
    std::shared_lock<RegistryLock> lock(g_m);
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
            std::shared_lock<RegistryLock> lock(g_m);
            known = list_state_locked(l, &id) != nullptr;
        }
        if (!known) preexisting_list(l);
        const auto cov = na::hooking::variants::CoverageOf(na::hooking::variants::Kind::CommandList, l);
        {
            std::shared_lock<RegistryLock> lock(g_m);
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
                    ls->pending_entry = 0;
                    ls->entry_barrier=0;
                    ls->sampled_textures.clear();
                    ls->cleared.clear();
                    ls->pso = pso;
                    ls->heaps[0] = ls->heaps[1] = nullptr;
                    ls->nheaps = 0;
                    ls->heap_metadata_revision=0;
                    ls->operation_key[0].valid=ls->operation_key[1].valid=false;
                    ++ls->key_revision[0];++ls->key_revision[1];++ls->read_revision[0];++ls->read_revision[1];
                    ls->args[0].clear();
                    ls->args[1].clear();
                    ls->cur_rt = ls->cur_ds = 0;
                    ls->cur_nrt = 0;
                    ls->restorable = true;
                    ls->active_queries=0;
                    ls->in_render_pass = false;
                    ls->raster_viewports_known = ls->raster_scissors_known = ls->raster_topology_known = false;
                    ls->vertex_mask=0;ls->index_known=false;ls->index_buffer={};ls->stencil_ref=0;
                    std::fill_n(ls->blend_factor,4,0.f);ls->predicate=nullptr;ls->predicate_offset=0;
                    ls->raster_targets_known = false;
                    ls->raster_tracking = g_raster_tracking.load(std::memory_order_acquire);
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
void note_draw_locked(ListState* ls){
    const CostScope cost(kCostNoteDraw);
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
        if(p->depth_enable==1&&p->depth_write==1&&ls->states.size()<4096){
            const bool same=!ls->states.empty()&&ls->states.back().resource==ls->cur_ds&&ls->states.back().subresource==ls->cur_ds_sub&&ls->states.back().state_after==D3D12_RESOURCE_STATE_DEPTH_WRITE;
            if(!same)ls->states.push_back({ls->cur_ds,ls->cur_ds_sub,D3D12_RESOURCE_STATE_DEPTH_WRITE});
        }
    }
}
void note_draw(GL* l){with_list_state(l,[&](ListState& ls){std::lock_guard<std::mutex> lock(ls.bind_m);note_draw_locked(&ls);});}
void draw_operation(GL* l,uint32_t op,uint64_t count,uint64_t instances){
    Rec r;if(recorded_locked(l,r,[&](ListState& ls){note_draw_locked(&ls);}))emit_op(r,op,{count,instances});
}

template <>
struct On<sl::list::DrawInstanced> {
    static void run(GL* l, UINT v, UINT i, UINT, UINT)
    {
        draw_operation(l,NA_OP_DRAW,v,i);
    }
};
template<> struct On<sl::list::BeginQuery>{static void run(GL* l,ID3D12QueryHeap*,D3D12_QUERY_TYPE type,UINT){
    if(type!=D3D12_QUERY_TYPE_TIMESTAMP)with_list_state(l,[](ListState& s){std::lock_guard<std::mutex> lock(s.bind_m);++s.active_queries;});
}};
template<> struct On<sl::list::EndQuery>{static void run(GL* l,ID3D12QueryHeap*,D3D12_QUERY_TYPE type,UINT){
    if(type!=D3D12_QUERY_TYPE_TIMESTAMP)with_list_state(l,[](ListState& s){std::lock_guard<std::mutex> lock(s.bind_m);if(s.active_queries)--s.active_queries;});
}};
template <>
struct On<sl::list::DrawIndexedInstanced> {
    static void run(GL* l, UINT n, UINT i, UINT, INT, UINT)
    {
        draw_operation(l,NA_OP_DRAW_INDEXED,n,i);
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
std::atomic<RasterStateListener> g_raster_listener{nullptr};
void notify_raster_state(GL* l)
{
    const auto listener = g_raster_listener.load(std::memory_order_acquire);
    if (!listener) return;
    RasterScissors s;
    if (!ListRasterScissors(l, s)) return;
    HostWorkScope own;
    listener(l, s);
}
template <>
struct On<sl::list::RSSetViewports> {
    static void run(GL* l, UINT n, const D3D12_VIEWPORT* v)
    {
        Rec r;
        if (!begin_op(l, &r)) return;
        with_list_state(l, [&](ListState& ls) {
            std::lock_guard<std::mutex> bl(ls.bind_m);
            ++ls.viewport_serial;ls.raster_viewports_known = n <= D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE && (!n || v);
            if (ls.raster_viewports_known) {
                ls.raster_viewport_count = n;
                if (n) std::copy_n(v, n, ls.raster_viewports);
            }
        });
        if (!n || !v) return;
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
        if (!begin_op(l, &r)) return;
        with_list_state(l, [&](ListState& ls) {
            std::lock_guard<std::mutex> bl(ls.bind_m);
            ++ls.scissor_serial;ls.raster_scissors_known = n <= D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE && (!n || rc);
            if (ls.raster_scissors_known) {
                ls.raster_scissor_count = n;
                if (n) std::copy_n(rc, n, ls.raster_scissors);
            }
        });
        notify_raster_state(l);
        if (!n || !rc) return;
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
struct On<sl::list::IASetPrimitiveTopology> {
    static void run(GL* l, D3D12_PRIMITIVE_TOPOLOGY topology) {
        if (t_host_work) return;
        with_list_state(l, [&](ListState& ls) {
            std::lock_guard<std::mutex> bl(ls.bind_m);
            ls.raster_topology = topology; ls.raster_topology_known = true;
        });
    }
};
template <>
struct On<sl::list::ClearState> {
    static void run(GL* l,ID3D12PipelineState* pso){with_list_state(l,[&](ListState& s){std::lock_guard<std::mutex> lock(s.bind_m);
        s.pso=pso;s.root_sig[0]=s.root_sig[1]=nullptr;s.args[0].clear();s.args[1].clear();s.nheaps=0;
        s.cur_rt=s.cur_ds=0;s.cur_nrt=0;s.native_colors.clear();s.native_depth.reset();
        s.raster_rt_count=s.raster_viewport_count=s.raster_scissor_count=0;s.raster_has_depth=false;
        s.raster_targets_known=s.raster_viewports_known=s.raster_scissors_known=s.raster_topology_known=true;
        s.raster_topology=D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;s.restorable=true;
        s.vertex_mask=0;s.index_known=false;s.index_buffer={};s.stencil_ref=0;std::fill_n(s.blend_factor,4,0.f);
        s.predicate=nullptr;s.predicate_offset=0;++s.key_revision[0];++s.key_revision[1];++s.read_revision[0];++s.read_revision[1];
    });}
};
template <>
struct On<sl::list::SetPredication> {
    static void run(GL* l,ID3D12Resource* buffer,UINT64 offset,D3D12_PREDICATION_OP op){with_list_state(l,[&](ListState& s){std::lock_guard<std::mutex> lock(s.bind_m);
        s.predicate=buffer;s.predicate_offset=offset;s.predicate_op=op;});}
};
template <>
struct On<sl::list::IASetVertexBuffers> {
    static void run(GL* l,UINT first,UINT count,const D3D12_VERTEX_BUFFER_VIEW* views){
        if(first>32||count>32-first)return;
        with_list_state(l,[&](ListState& s){std::lock_guard<std::mutex> lock(s.bind_m);
            if(!s.raster_tracking)return;
            for(UINT i=0;i<count;++i){s.vertex_buffers[first+i]=views?views[i]:D3D12_VERTEX_BUFFER_VIEW{};s.vertex_mask|=1u<<(first+i);}});
    }
};
template <>
struct On<sl::list::IASetIndexBuffer> {
    static void run(GL* l,const D3D12_INDEX_BUFFER_VIEW* view){with_list_state(l,[&](ListState& s){std::lock_guard<std::mutex> lock(s.bind_m);
        if(s.raster_tracking){s.index_buffer=view?*view:D3D12_INDEX_BUFFER_VIEW{};s.index_known=true;}});}
};
template <>
struct On<sl::list::OMSetBlendFactor> {
    static void run(GL* l,const FLOAT factor[4]){with_list_state(l,[&](ListState& s){std::lock_guard<std::mutex> lock(s.bind_m);
        if(s.raster_tracking)for(UINT i=0;i<4;++i)s.blend_factor[i]=factor?factor[i]:1.f;});}
};
template <>
struct On<sl::list::OMSetStencilRef> {
    static void run(GL* l,UINT value){with_list_state(l,[&](ListState& s){std::lock_guard<std::mutex> lock(s.bind_m);if(s.raster_tracking)s.stencil_ref=value;});}
};
template <>
struct On<sl::list::SetPipelineState> {
    static void run(GL* l, ID3D12PipelineState* p)
    {
        Rec r;
        if (!begin_op(l, &r)) return;
        emit_op(r, NA_OP_SET_PIPELINE, {id_or_unknown(p)});
        with_list_state(l, [&](ListState& ls) { std::lock_guard<std::mutex> bl(ls.bind_m);
            if(ls.pso!=p){++ls.key_revision[0];++ls.key_revision[1];ls.operation_key[0].valid=ls.operation_key[1].valid=false;}ls.pso = p; });
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
        // sides). A wildcard alias can name ANY placed resource, so it marks
        // this recording incomplete and invalidates carried states at submission.
        // Allocation is bounded by the 8192-entry
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
                if (!b[i].Aliasing.pResourceBefore || !b[i].Aliasing.pResourceAfter) overflow = true;
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
            std::shared_lock<RegistryLock> lock(g_m);
            for (auto& dh : g_heaps)
                if (dh->key == h[i]) (dh->type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? smp : res) = dh->id;
        }
        emit_op(r, NA_OP_SET_DESCRIPTOR_HEAPS, {res, smp});
        with_list_state(l, [&](ListState& ls) {
            std::lock_guard<std::mutex> bl(ls.bind_m);
            ls.nheaps = 0;
            ls.heap_metadata_revision=0;++ls.key_revision[0];++ls.key_revision[1];++ls.read_revision[0];++ls.read_revision[1];
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
        std::lock_guard<std::mutex> bl(ls.bind_m);
        const bool changed = ls.root_sig[Compute] != rs;ls.root_sig[Compute] = rs;++ls.root_serial[Compute];
        if(changed){++ls.key_revision[Compute];++ls.read_revision[Compute];ls.operation_key[Compute].valid=false;}
        if (changed) ls.args[Compute].clear(); // a different root signature invalidates every root argument
    });
}
template <>
struct On<sl::list::SetComputeRootSignature> { static void run(GL* l, ID3D12RootSignature* rs) { root_sig<1>(l, rs); } };
template <>
struct On<sl::list::SetGraphicsRootSignature> { static void run(GL* l, ID3D12RootSignature* rs) { root_sig<0>(l, rs); } };
void shadow_arg_locked(ListState& ls,uint32_t compute,UINT param,uint8_t type,uint64_t value,const void* consts=nullptr,UINT count=0,UINT dest=0){
    if(param>=64)return;
        auto& v = ls.args[compute];
        if (v.size() <= param) v.resize(param + 1);
        ListState::RootArg& a = v[param];
        if(a.type!=type||a.value!=value||type==5){
            if(type==1||a.type==1){
                bool texture_table=true;
                const auto root=g_objs.find(canon(ls.root_sig[compute]));
                if(type==1&&a.type==1&&root!=g_objs.end()&&param<root->second.root_params.size()){
                    const auto& p=root->second.root_params[param];
                    if(p.param_type==NA_ROOT_TABLE&&p.first_range<=root->second.root_ranges.size()&&p.count<=root->second.root_ranges.size()-p.first_range){
                        texture_table=false;
                        for(UINT i=0;i<p.count;++i){const auto& range=root->second.root_ranges[p.first_range+i];
                            texture_table|=range.range_type==D3D12_DESCRIPTOR_RANGE_TYPE_SRV||range.range_type==D3D12_DESCRIPTOR_RANGE_TYPE_UAV;}
                    }
                }
                if(texture_table)++ls.read_revision[compute];
            }
            const auto& cached=ls.operation_key[compute];
            if(type!=2||(a.type!=2&&a.type!=0)||!cached.valid||(cached.dimension_cbvs&(uint64_t(1)<<param)))++ls.key_revision[compute];
        }
        ++a.serial;
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
}
void shadow_arg(GL* l,uint32_t compute,UINT param,uint8_t type,uint64_t value,const void* consts=nullptr,UINT count=0,UINT dest=0){
    if(param>=64)return;
    with_list_state(l,[&](ListState& ls){std::lock_guard<std::mutex> lock(ls.bind_m);shadow_arg_locked(ls,compute,param,type,value,consts,count,dest);});
}
std::atomic<TableBindCallback> g_table_cb{nullptr};
std::atomic<EntryCallback> g_entry_cb{nullptr};
template <uint32_t Compute>
void root_table(GL* l,UINT param,D3D12_GPU_DESCRIPTOR_HANDLE h){
    const CostScope cost(kCostRootTable);
    Rec r;na_object_id heap=0;uint32_t slot=0;uint64_t version=0;
    if(!recorded_locked(l,r,[&](ListState& state){auto* ls=&state;
        shadow_arg_locked(state,Compute,param,1,h.ptr);
        // Legal shader tables live in one of the recording's two bound heaps.
        // Reuse their generation-checked metadata before searching every heap
        // in a large game. Keep the original fallback for out-of-order evidence.
        DescHeap* dh = bound_heap_for_gpu_locked(state,h.ptr,&slot);
        if (!dh) dh = heap_for_gpu_locked(h.ptr, &slot);
        if (dh) {
            heap = dh->id;
            const SlotShared stripe(dh, slot);
            version = dh->versions[slot];
        }
        // C9: CBV ranges of this table (first range, per the bound root signature).

        if (dh && ls->root_sig[Compute]) {
            auto it = g_objs.find(canon(ls->root_sig[Compute]));
            if (it != g_objs.end() && param < it->second.root_params.size()) {
                const na_root_param& rp = it->second.root_params[param];
                if (rp.param_type == NA_ROOT_TABLE) {
                    const auto& rr = it->second.root_ranges;
                    for (uint32_t q = 0; q < rp.count && rp.first_range + q < rr.size(); ++q) {
                        const na_root_range& range = rr[rp.first_range + q];
                        if (range.range_type == D3D12_DESCRIPTOR_RANGE_TYPE_SRV && range.table_offset != NA_UNKNOWN_U32 && ls->sampled_textures.size()<256) {
                            const CostScope sampled(kCostSampled);

                            SlotShared stripe;
                            for (uint32_t k = 0; k < std::min<uint32_t>(range.num_descriptors, 64) && ls->sampled_textures.size()<256; ++k) {
                                const uint64_t index = uint64_t(slot) + range.table_offset + k;
                                if (index >= dh->count) break;
                                stripe.lock(dh, index);
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
                        SlotShared stripe;
                        for (uint32_t k = 0; k < n && slot + range.table_offset + k < dh->count; ++k) {
                            stripe.lock(dh, uint64_t(slot) + range.table_offset + k);
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
                            if(ls->bindings.size()<4096)ls->bindings.push_back(b);
                        }
                    }
                }
            }
        }
    }))return;
    emit_op(r,NA_OP_SET_ROOT_TABLE,{heap,heap?slot:uint64_t{NA_UNKNOWN_U32},heap?version:NA_UNKNOWN_U64,param,Compute},nullptr,0,0,heap?0u:NA_EVF_METHOD_UNOBSERVED);
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
        std::shared_lock<RegistryLock> lock(g_m);
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
            std::shared_lock<RegistryLock> lock(g_m);
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
        notify_raster_state(l);
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
            if (v.resource == ls.pending_entry) ls.pending_entry = 0;
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
        bool graphics=false;
        {
            std::shared_lock<RegistryLock> lock(g_m);
            na_object_id id=0;auto* ls=list_state_locked(l,&id);
            if(ls) {
                std::lock_guard<std::mutex> bl(ls->bind_m);
                const auto p=g_objs.find(canon(ls->pso));
                if(p!=g_objs.end() && p->second.created.size()>=sizeof(na_ev_pipeline_created)) {
                    const auto* d=reinterpret_cast<const na_ev_pipeline_created*>(p->second.created.data());
                    graphics=d->pipeline_kind==NA_PIPELINE_GRAPHICS || (d->pipeline_kind==NA_PIPELINE_STREAM && !d->stage_bytes[NA_STAGE_CS]);
                }
            }
        }
        if(r.list && graphics)note_draw(l);
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
            std::shared_lock<RegistryLock> lock(g_m);
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
            ls.raster_targets_known = false; // render-pass targets need their own snapshot before Layer2 work
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
        const bool scene_here = ListSceneSubresource(l, color, depth, &cs, &ds);
        // Whole-resource handoff is exact evidence even in a pure post list.
        // Keep that entry for submitted-scene placement; it cannot insert inline
        // without this recording's scene metadata. No subresource is guessed.
        if ((!scene_here && b.Transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) ||
            (scene_here && b.Transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES && b.Transition.Subresource != cs)) continue;
        std::vector<ListState::StateNote> entry_states;
        // The callback runs BEFORE this entire barrier batch. Its first
        // transition for each selected subresource supplies the exact entry
        // state, even when depth was only read and prior GPU work is in flight.
        // Never use StateAfter here, nor the before-state of an END_ONLY half.
        if(scene_here)for (const auto selected : {std::pair<na_object_id, uint32_t>{color, cs}, {depth, ds}}) {
            for (UINT j = 0; j < count; ++j) {
                const auto& entry = barriers[j];
                if (entry.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) break;
                if (entry.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ||
                    resource_id(entry.Transition.pResource) != selected.first ||
                    (entry.Transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES &&
                     entry.Transition.Subresource != selected.second)) continue;
                if (!(entry.Flags & D3D12_RESOURCE_BARRIER_FLAG_END_ONLY))
                    entry_states.push_back({selected.first, selected.second, static_cast<uint32_t>(entry.Transition.StateBefore)});
                break;
            }
        }
        {
            std::shared_lock<RegistryLock> lock(g_m);
            na_object_id lid = 0;
            auto* ls = list_state_locked(l, &lid);
            if (!ls) return;
            std::lock_guard<std::mutex> bl(ls->bind_m);
            if (ls->states.size() + entry_states.size() <= 8192)
                ls->states.insert(ls->states.end(), entry_states.begin(), entry_states.end());
            else ls->evidence_complete = false;
            ls->insertion.Add({color, 0, UINT64_MAX, false, na::j2::Op::Barrier});
            ls->pending_entry = color;
            ++ls->entry_barrier;
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
    // A real copy consumes or replaces the handoff before any shader operation.
    // Do not later label an unrelated draw as its first entry.
    if (source == color || destination == color) {
        with_list_state(l, [&](ListState& ls) {
            std::lock_guard<std::mutex> bl(ls.bind_m);
            ls.pending_entry = 0;
        });
    }
    const bool from_color = source == color;
    if (!from_color && !is_derived(source)) return;
    add_derived(destination); // lineage: the copy carries the scene on
    {
        std::shared_lock<RegistryLock> lock(g_m);
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
    std::shared_lock<RegistryLock> lock(g_m);
    na_object_id lid = 0;
    auto* ls = list_state_locked(l, &lid);
    if (!ls) return;
    std::lock_guard<std::mutex> bl(ls->bind_m);
    ls->insertion.Add({destination, 0, 0, true, na::j2::Op::CopyWrite});
}

void before_shader_use(GL* l, bool compute, bool indirect=false)
{
    if (t_host_work || !na::observe_internal::Started()) return;
    uint64_t depth = 0, color = 0;
    GetLiveSelection(&depth, &color);
    if (!color) return;
    bool reads_color = false;
    na::j2::InsertionEvent entry;
    {
        std::shared_lock<RegistryLock> lock(g_m);
        na_object_id lid = 0;
        auto* ls = list_state_locked(l, &lid);
        if (!ls) return;
        std::lock_guard<std::mutex> bl(ls->bind_m);
        const auto current_pso = g_objs.find(canon(ls->pso));
        if(indirect) {
            if(current_pso==g_objs.end() || current_pso->second.created.size()<sizeof(na_ev_pipeline_created))return;
            const auto* pd=reinterpret_cast<const na_ev_pipeline_created*>(current_pso->second.created.data());
            compute=pd->pipeline_kind==NA_PIPELINE_COMPUTE || pd->stage_bytes[NA_STAGE_CS]!=0;
        }
        const auto signature = g_objs.find(canon(ls->root_sig[compute]));
        if (signature == g_objs.end()) return;
        if(ls->pending_entry==color) {
            ls->pending_entry=0;
            entry={color,compute?0:ls->cur_rt,current_pso==g_objs.end()?0:current_pso->second.id,false,
                indirect?(compute?na::j2::Op::IndirectComputeEntry:na::j2::Op::IndirectGraphicsEntry):
                         (compute?na::j2::Op::ComputeEntry:na::j2::Op::GraphicsEntry),signature->second.root_hash};
            resource_dims_locked(entry.destination,&entry.dst_w,&entry.dst_h,&entry.dst_fmt);
            entry.recording=ls->generation.load();entry.operation=ls->op_index.load()+1;entry.barrier=ls->entry_barrier;
            ls->insertion.SetEntryContext(entry);
            ls->insertion.Add(entry);
        }
        na_object_id destination = compute ? 0 : ls->cur_rt;
        uint8_t color_srvs = 0;
        na_object_id srv_ids[16] = {};
        uint32_t nsrv = 0;
        bool reads_derived = false;
        auto& cached=ls->shader_reads[compute];
        const uint64_t revision_now=g_operation_revision.load(); // before reading: a concurrent copy then invalidates
        if(cached.valid&&cached.state_revision==ls->read_revision[compute]&&cached.registry_revision==revision_now&&cached.color==color){
            color_srvs=cached.color_srvs;reads_color=color_srvs!=0;nsrv=cached.count;
            std::copy_n(cached.srvs,nsrv,srv_ids);if(compute)destination=cached.uav;
        }else{
        const auto& params = signature->second.root_params;
        const auto& ranges = signature->second.root_ranges;
        for (size_t p = 0; p < ls->args[compute].size() && p < params.size(); ++p) {
            const auto& arg = ls->args[compute][p];
            if (arg.type != 1 || params[p].param_type != NA_ROOT_TABLE) continue;
            uint32_t slot = 0;
            auto* heap = bound_heap_for_gpu_locked(*ls,arg.value, &slot);
            if (!heap) continue;
            bool bound = false;
            for (uint32_t h = 0; h < ls->nheaps; ++h) bound |= heap->key == ls->heaps[h];
            if (!bound) continue;
            for (uint32_t r = 0; r < params[p].count && params[p].first_range + r < ranges.size(); ++r) {
                const auto& range = ranges[params[p].first_range + r];
                if (range.table_offset == NA_UNKNOWN_U32) continue;
                SlotShared stripe;
                for (uint32_t k = 0; k < std::min<uint32_t>(range.num_descriptors, 64); ++k) {
                    const uint64_t index = uint64_t(slot) + range.table_offset + k;
                    if (index >= heap->count) break;
                    stripe.lock(heap, index);
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
        cached.state_revision=ls->read_revision[compute];cached.registry_revision=revision_now;cached.color=color;
        cached.color_srvs=color_srvs;cached.count=nsrv;cached.uav=compute?destination:0;
        std::copy_n(srv_ids,nsrv,cached.srvs);cached.valid=true;
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
    if(entry.resource)if(auto cb=g_entry_cb.load(std::memory_order_acquire))cb(l,entry);
    if (reads_color) if (auto cb = g_table_cb.load(std::memory_order_acquire)) cb(l, &color, 1, compute);
}

// ---- Live mip bias: bind the twins of the level latched at this recording's Reset.
// A pipeline without a twin for that level (pipeline-library loads, embedded root
// signatures) switches the recording back to the game's own objects: the state shadow
// re-binds the game's root signatures and arguments first, so pipeline and root
// signature always match.
template <typename T>
T* mip_swap_locked(int level, int af, T* obj, bool pso, bool* untwinned)
{
    if (level < 0 || !obj) return obj;
    const auto it = g_objs.find(canon(obj));
    if (it == g_objs.end()) {
        // Unknown pipeline: ours (host work, e.g. DLAA's own passes) is left alone; the
        // game's (a pipeline-library load) has no twin.
        if (pso && !t_host_work) *untwinned = true;
        return obj;
    }
    const Obj& o = it->second;
    if (o.variant_kind == 0) return obj; // sampler-independent: the game's object is right
    if (o.variant_kind == 2) { *untwinned = true; return obj; }
    const auto& t = o.af_sets[af & 3];
    if (!t || !t->built[level]) {
        // Created while the variant was being prepared: the worker fills it in (it also
        // re-checks every second, so a wake-up lost here only delays it).
        *untwinned = true;
        if (o.recipe && !g_var_missing.exchange(true, std::memory_order_relaxed)) g_var_cv.notify_one();
        return obj;
    }
    if (t->same[level]) return obj;
    if (t->obj[level]) return static_cast<T*>(t->obj[level]);
    *untwinned = true;
    return obj;
}
void mip_on_pso(GL* l, ID3D12PipelineState*& p)
{
    if (!g_mip_switched.load(std::memory_order_relaxed) || !p) return;
    bool fallback = false;
    {
        std::shared_lock<RegistryLock> lock(g_m);
        na_object_id lid = 0;
        ListState* ls = list_state_locked(l, &lid);
        if (!ls) return;
        std::lock_guard<std::mutex> bl(ls->bind_m);
        if (ls->mip_level < 0) return;
        bool untwinned = false;
        ID3D12PipelineState* t = mip_swap_locked(ls->mip_level, ls->af_latched, p, true, &untwinned);
        if (untwinned) { ls->mip_level = -1; fallback = true; }
        else { if (t != p) ++ls->mip_pso_twin; else ++ls->mip_pso_game; p = t; }
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
    bool fallback = false;
    {
        std::shared_lock<RegistryLock> lock(g_m);
        na_object_id lid = 0;
        ListState* ls = list_state_locked(l, &lid);
        if (!ls) return;
        std::lock_guard<std::mutex> bl(ls->bind_m);
        if (ls->mip_level < 0) return;
        bool untwinned = false;
        ID3D12RootSignature* t = mip_swap_locked(ls->mip_level, ls->af_latched, r, false, &untwinned);
        if (untwinned) { ls->mip_level = -1; fallback = true; } // r (the game's) is bound as is
        else { if (t != r) ++ls->mip_rs_twin; else ++ls->mip_rs_game; r = t; }
    }
    if (fallback) {
        // A pipeline twin may be bound: switch the whole recording to the game's objects
        // (as mip_on_pso does) before the game's root signature goes in.
        g_mip_fallbacks.fetch_add(1, std::memory_order_relaxed);
        const HostWorkScope host;
        RestoreListState(l);
    }
}
void mip_on_reset(GL* l, ID3D12PipelineState*& p)
{
    if (!g_mip_switched.load(std::memory_order_relaxed)) return;
    const uint32_t v = g_var_live.load(std::memory_order_acquire);
    std::shared_lock<RegistryLock> lock(g_m);
    na_object_id lid = 0;
    ListState* ls = list_state_locked(l, &lid);
    if (!ls) return;
    std::lock_guard<std::mutex> bl(ls->bind_m);
    // The previous recording's binds into the totals (one add per counter per recording).
    if (ls->mip_pso_twin) g_mip_bound_pso_twin.fetch_add(std::exchange(ls->mip_pso_twin, 0u), std::memory_order_relaxed);
    if (ls->mip_pso_game) g_mip_bound_pso_game.fetch_add(std::exchange(ls->mip_pso_game, 0u), std::memory_order_relaxed);
    if (ls->mip_rs_twin) g_mip_bound_rs_twin.fetch_add(std::exchange(ls->mip_rs_twin, 0u), std::memory_order_relaxed);
    if (ls->mip_rs_game) g_mip_bound_rs_game.fetch_add(std::exchange(ls->mip_rs_game, 0u), std::memory_order_relaxed);
    ls->mip_level = static_cast<int8_t>(v ? var_level(v) : -1); // 0 = the game's own objects
    ls->af_latched = static_cast<int8_t>(var_af(v));
    if (ls->mip_level >= 0 && p) {
        bool untwinned = false;
        ID3D12PipelineState* t = mip_swap_locked(ls->mip_level, ls->af_latched, p, true, &untwinned);
        if (untwinned) ls->mip_level = -1; // nothing bound yet: simply record with the game's objects
        else { if (t != p) ++ls->mip_pso_twin; else ++ls->mip_pso_game; p = t; }
    }
}

// Thunk pools (variant registry, A48/A49): one detour per implementation variant.
constexpr uint32_t kThunks = 4;
std::atomic<void (*)(ID3D12GraphicsCommandList*)> g_reset_listener{nullptr};
bool g_reset_registered = false; // installation thread only
bool g_barriers_registered=false;
std::atomic<void (*)(ID3D12GraphicsCommandList*,UINT,const D3D12_RESOURCE_BARRIER*)> g_barrier_listener{nullptr};
std::atomic<bool> g_full_list_tracking{false};
struct ShaderHooks { std::shared_ptr<ShaderOperationHook> lane[2]; };
// Like the existing live registry, do not run D3D12-owning destructors during
// static teardown. Registration changes happen on the control/setup worker.
auto& g_shader_hooks = *new std::shared_ptr<const ShaderHooks>();
std::mutex g_shader_hooks_m;
std::atomic<bool> g_shader_hooks_enabled{false};
auto& g_texture_hook = *new std::shared_ptr<TextureOperationHook>();
std::atomic<DepthClearListener> g_depth_clear_listener{nullptr};

void capture_raster_targets(GL* list, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* targets,
    BOOL contiguous, const D3D12_CPU_DESCRIPTOR_HANDLE* depth)
{
    if (t_host_work) return;
    bool enabled = false;
    with_list_state(list, [&](ListState& ls) {
        std::lock_guard<std::mutex> bl(ls.bind_m); enabled = ls.raster_tracking; ls.raster_targets_known = false;
    });
    if (!enabled) return;
    if (count > 8 || (count && !targets)) return;
    if (g_operation_snapshots.load()) {
        std::shared_lock<RegistryLock> lock(g_m);
        na_object_id id = 0; auto* ls = list_state_locked(list, &id);
        if (ls) {
            std::lock_guard<std::mutex> bl(ls->bind_m);
            ++ls->target_serial;ls->native_colors.clear(); ls->native_depth.reset();
            UINT stride = 0;
            if (count) { uint32_t slot = 0; if (auto* heap = heap_for_cpu_locked(targets[0].ptr, &slot)) stride = heap->increment; }
            for (UINT i = 0; i < count; ++i) {
                uint32_t slot = 0;
                const auto handle = contiguous ? targets[0].ptr + SIZE_T(i) * stride : targets[i].ptr;
                const auto* heap = heap_for_cpu_locked(handle, &slot);
                std::shared_ptr<const DescriptorDetails> det;
                if (heap) { const SlotShared stripe(heap, slot); det = heap->info[slot].details; }
                ls->native_colors.push_back(std::move(det));
            }
            if (depth) { uint32_t slot = 0; if (const auto* heap = heap_for_cpu_locked(depth->ptr, &slot)) { const SlotShared stripe(heap, slot); ls->native_depth = heap->info[slot].details; } }
        }
    }
    std::shared_ptr<Obj::Twins> storage;
    {
        std::shared_lock<RegistryLock> lock(g_m);
        const auto found = g_objs.find(canon(list));
        if (found == g_objs.end() || !found->second.list) return;
        storage = found->second.twins;
    }
    // CPU-only descriptor storage, once per observed list. Use the existing
    // deferred child-object release path: releasing these from a list's private
    // retirement sentinel would re-enter D3D12 under its destruction lock.
    HostWorkScope own;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&device)))) return;
    if (!storage) {
        auto candidate = std::make_shared<Obj::Twins>();
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rt, ds;
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 8, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rt)))) return;
        hd = {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&ds)))) return;
        candidate->obj[0] = rt.Detach(); candidate->obj[1] = ds.Detach();
        {
            std::unique_lock<RegistryLock> lock(g_m);
            const auto found = g_objs.find(canon(list));
            if (found == g_objs.end() || !found->second.list) return;
            if (!found->second.twins) found->second.twins = candidate;
            storage = found->second.twins;
        }
    }
    auto* rt = static_cast<ID3D12DescriptorHeap*>(storage->obj[0]);
    auto* ds = static_cast<ID3D12DescriptorHeap*>(storage->obj[1]);
    if (!rt || !ds) return;
    const UINT stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    auto dst = rt->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < count; ++i) {
        const auto src = contiguous ? D3D12_CPU_DESCRIPTOR_HANDLE{targets[0].ptr + SIZE_T(i) * stride} : targets[i];
        device->CopyDescriptorsSimple(1, dst, src, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        dst.ptr += stride;
    }
    if (depth) device->CopyDescriptorsSimple(1, ds->GetCPUDescriptorHandleForHeapStart(), *depth, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    with_list_state(list, [&](ListState& ls) {
        std::lock_guard<std::mutex> bl(ls.bind_m);
        ls.raster_rt_count = count; ls.raster_has_depth = depth != nullptr; ls.raster_targets_known = true;
    });
}
// ---- Viewport jitter (real sub-pixel samples) ------------------------------------------
// The game's camera block is only half the story in some games (SE5: our matrix write never
// moved the rasterised picture). Every D3D12 game sets its viewports, so the sample offset is
// applied there instead: a draw whose bound depth is the live scene depth and whose pipeline
// tests depth gets the game's viewports moved by (x, y) render pixels (+x right, +y down), the
// content moves with them, and DLSS is told exactly that value. Post passes (no scene depth,
// or depth test off: full-screen passes) keep the game's own viewports, so nothing is shifted
// twice. Packed: frame (low 32 bits, never 0) << 32 | x << 16 | y, x/y int16 in 1/65536 px.
std::atomic<uint64_t> g_vpj{0};
std::atomic<bool> g_vpj_ever{false};
std::atomic<uint64_t> g_vpj_sets{0}, g_vpj_restores{0}, g_vpj_draws{0}, g_vpj_unknown{0};
thread_local bool t_vpj_self = false; // our own RSSetViewports: not a change of the game's
float vpj_axis(uint64_t v, unsigned shift) { return static_cast<float>(static_cast<int16_t>(static_cast<uint16_t>(v >> shift))) / 65536.f; }
// The game (or host work restoring the game's state) set viewports, or the list restarted:
// our offset copies are no longer bound.
void vpj_viewports_replaced(GL* l, bool reset)
{
    if (!g_vpj_ever.load(std::memory_order_relaxed) || t_vpj_self) return;
    std::shared_lock<RegistryLock> lock(g_m);
    na_object_id id = 0;
    auto* ls = list_state_locked(l, &id);
    if (!ls) return;
    std::lock_guard<std::mutex> bl(ls->bind_m);
    ls->vpj_applied = false;
    if (reset) {
        if (ls->vpj_scene) g_vpj_recordings.fetch_sub(1, std::memory_order_release);
        ls->vpj_scene = 0; ls->vpj_depth = 0;
    }
}
void vpj_before_draw(GL* l)
{
    if (!g_vpj_ever.load(std::memory_order_relaxed)) return;
    const uint64_t cur = g_vpj.load(std::memory_order_acquire);
    if (!cur && !g_vpj_recordings.load(std::memory_order_acquire)) return;
    D3D12_VIEWPORT v[D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT n = 0;
    {
        uint64_t sel_depth = 0, sel_color = 0;
        GetLiveSelection(&sel_depth, &sel_color);
        std::shared_lock<RegistryLock> lock(g_m);
        na_object_id id = 0;
        auto* ls = list_state_locked(l, &id);
        if (!ls) return;
        std::lock_guard<std::mutex> bl(ls->bind_m);
        // A Present on another thread must not change half a scene recording's
        // sample. The first eligible draw owns it until this list is Reset.
        const uint64_t sample=ls->vpj_scene&&ls->vpj_depth==sel_depth?ls->vpj_scene:cur;
        bool want = sample && sel_depth && ls->cur_ds == sel_depth;
        if (want) {
            const auto pipeline = g_objs.find(canon(ls->pso));
            want = pipeline != g_objs.end() && pipeline->second.created.size() >= sizeof(na_ev_pipeline_created) &&
                   reinterpret_cast<const na_ev_pipeline_created*>(pipeline->second.created.data())->depth_enable == 1;
        }
        if (want && !(ls->raster_viewports_known && ls->raster_viewport_count)) { g_vpj_unknown.fetch_add(1, std::memory_order_relaxed); want = false; }
        if (want) {
            if (!ls->vpj_scene) g_vpj_recordings.fetch_add(1, std::memory_order_release);
            ls->vpj_scene = sample;
            ls->vpj_depth=sel_depth;
            g_vpj_draws.fetch_add(1, std::memory_order_relaxed);
            if (ls->vpj_applied && ls->vpj_value == sample) return;
            const float dx = vpj_axis(sample, 16), dy = vpj_axis(sample, 0);
            n = ls->raster_viewport_count;
            for (UINT i = 0; i < n; ++i) { v[i] = ls->raster_viewports[i]; v[i].TopLeftX += dx; v[i].TopLeftY += dy; }
            ls->vpj_applied = true; ls->vpj_value = sample;
            g_vpj_sets.fetch_add(1, std::memory_order_relaxed);
        } else {
            if (!ls->vpj_applied) return;
            ls->vpj_applied = false;
            if (!ls->raster_viewports_known) return; // nothing known to hand back (the game sets its own before relying on them)
            n = ls->raster_viewport_count;
            std::copy_n(ls->raster_viewports, n, v);
            g_vpj_restores.fetch_add(1, std::memory_order_relaxed);
        }
    }
    HostWorkScope own;
    t_vpj_self = true;
    l->RSSetViewports(n, n ? v : nullptr);
    t_vpj_self = false;
}
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
        const CostScope cost(Slot, scope.outermost && !t_host_work);
        auto fn = reinterpret_cast<Pfn>(orig[I]);
        if constexpr (Slot == sl::list::DrawInstanced || Slot == sl::list::DrawIndexedInstanced ||
                      Slot == sl::list::ExecuteIndirect || Slot == sl::list::ExecuteBundle)
            if (!t_host_work && scope.outermost && !scope.ambiguous) { const CostScope c(kCostVpj); vpj_before_draw(l); }
        if constexpr (Slot == sl::list::RSSetViewports || Slot == sl::list::Reset)
            if (scope.outermost && !scope.ambiguous) vpj_viewports_replaced(l, Slot == sl::list::Reset);
        if constexpr (Slot == sl::list::ResourceBarrier) {
            if(scope.outermost && !scope.ambiguous)if(auto listener=g_barrier_listener.load())listener(l,a...);
            if(!g_full_list_tracking.load()){fn(l,a...);return;}
        }
        if constexpr (Slot == sl::list::Reset) {
            if (!g_full_list_tracking.load(std::memory_order_acquire)) {
                const R r = fn(l, a...);
                if (SUCCEEDED(r) && scope.outermost && !scope.ambiguous)
                    if (auto listener = g_reset_listener.load(std::memory_order_acquire)) listener(l);
                return r;
            }
        }
        // Live mip bias: the DRIVER gets the twin of the latched level; the OBSERVER always
        // gets the game's own object. Twins are not in the registry (no root layout, no
        // pipeline identity), so observing them blinded colour-read detection, producer
        // pipelines and the learned profile at every level except the game's own.
        constexpr bool kMipSlot = Slot == sl::list::SetPipelineState || Slot == sl::list::SetGraphicsRootSignature ||
                                  Slot == sl::list::SetComputeRootSignature || Slot == sl::list::Reset;
        if constexpr (Slot == sl::list::ExecuteBundle || Slot == sl::list::ExecuteIndirect || Slot == sl::list::BeginRenderPass) {
            if (!t_host_work && scope.outermost && !scope.ambiguous && g_shader_hooks_enabled.load(std::memory_order_acquire)) {
                const auto hooks = std::atomic_load_explicit(&g_shader_hooks, std::memory_order_acquire);
                if (hooks) for (const auto& hook : hooks->lane) if (hook) {
                    HostWorkScope own; hook->NativeBoundary(l);
                }
            }
        }
        if constexpr (Slot == sl::list::Close) {
            if (!t_host_work && scope.outermost && !scope.ambiguous && g_shader_hooks_enabled.load(std::memory_order_acquire)) {
                const auto hooks = std::atomic_load_explicit(&g_shader_hooks, std::memory_order_acquire);
                if (hooks) for (const auto& hook : hooks->lane) if (hook) {
                    HostWorkScope own; hook->Closing(l);
                }
            }
        }
        if constexpr (kMipSlot) {
            const std::tuple<std::decay_t<A>...> game{a...}; // what the game passed
            {
                auto&& t = std::forward_as_tuple(a...);
                if constexpr (Slot == sl::list::SetPipelineState) mip_on_pso(l, std::get<0>(t));
                else if constexpr (Slot == sl::list::Reset) mip_on_reset(l, std::get<1>(t));
                else mip_on_rs(l, std::get<0>(t));
            }
            if constexpr (std::is_void_v<R>) {
                fn(l, a...);
                if (scope.outermost && !scope.ambiguous) std::apply([&](auto... g) { On<Slot>::run(l, g...); }, game);
                return;
            } else {
                const R r = fn(l, a...);
                if constexpr (Slot == sl::list::Reset)
                    if (SUCCEEDED(r) && scope.outermost && !scope.ambiguous)
                        if (auto listener = g_reset_listener.load(std::memory_order_acquire)) listener(l);
                if (scope.outermost && !scope.ambiguous) std::apply([&](auto... g) { On<Slot>::run(l, r, g...); }, game);
                return r;
            }
        } else
        if constexpr (std::is_void_v<R>) {
            if constexpr (Slot == sl::list::OMSetRenderTargets)
                if (scope.outermost && !scope.ambiguous) capture_raster_targets(l, a...);
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
                if (scope.outermost && !scope.ambiguous) { const CostScope c(kCostShaderUse); before_shader_use(l, Slot == sl::list::Dispatch); }
            if constexpr(Slot==sl::list::ExecuteIndirect)
                if(scope.outermost && !scope.ambiguous){ const CostScope c(kCostShaderUse); before_shader_use(l,false,true); }
            if constexpr (Slot == sl::list::DrawInstanced || Slot == sl::list::DrawIndexedInstanced || Slot == sl::list::Dispatch) {
                if (!t_host_work && scope.outermost && !scope.ambiguous && g_shader_hooks_enabled.load(std::memory_order_acquire)) {
                    const auto hooks = std::atomic_load_explicit(&g_shader_hooks, std::memory_order_acquire);
                    if (hooks) {
                        ShaderCall call;
                        if constexpr (Slot == sl::list::Dispatch) call.kind = ShaderKind::Dispatch;
                        else if constexpr (Slot == sl::list::DrawInstanced) call.kind = ShaderKind::Draw;
                        else call.kind = ShaderKind::DrawIndexed;
                        size_t arg_index = 0;
                        ((call.args[arg_index++] = static_cast<int64_t>(a)), ...);
                        ShaderScope saved[2];
                        unsigned entered = 0;
                        bool replaced = false;
                        for (unsigned i = 0; i < 2; ++i) {
                            entered = i + 1;
                            if (!hooks->lane[i]) continue;
                            { const CostScope c(kCostLaneBefore); HostWorkScope own; saved[i] = hooks->lane[i]->Before(l, call); }
                            if (saved[i].action == ShaderAction::Replaced) { replaced = true; break; }
                        }
                        if (!replaced) {
                            if constexpr (Slot == sl::list::Dispatch) {
                                { const CostScope c(kCostDriver); fn(l, UINT(call.args[0]), UINT(call.args[1]), UINT(call.args[2])); }
                                { const CostScope c(kCostRun); On<Slot>::run(l, UINT(call.args[0]), UINT(call.args[1]), UINT(call.args[2])); }
                            } else if constexpr (Slot == sl::list::DrawInstanced) {
                                { const CostScope c(kCostDriver); fn(l, UINT(call.args[0]), UINT(call.args[1]), UINT(call.args[2]), UINT(call.args[3])); }
                                { const CostScope c(kCostRun); On<Slot>::run(l, UINT(call.args[0]), UINT(call.args[1]), UINT(call.args[2]), UINT(call.args[3])); }
                            } else {
                                { const CostScope c(kCostDriver); fn(l, UINT(call.args[0]), UINT(call.args[1]), UINT(call.args[2]), INT(call.args[3]), UINT(call.args[4])); }
                                { const CostScope c(kCostRun); On<Slot>::run(l, UINT(call.args[0]), UINT(call.args[1]), UINT(call.args[2]), INT(call.args[3]), UINT(call.args[4])); }
                            }
                        } // A skipped native command is NOT emitted as an executed game draw/write.
                        for (unsigned i = entered; i > 0; --i) if (hooks->lane[i - 1]) {
                            const CostScope c(kCostLaneAfter); HostWorkScope own; hooks->lane[i - 1]->After(l, call, saved[i - 1]);
                        }
                        return;
                    }
                }
            }
            if constexpr (Slot == sl::list::ClearRenderTargetView || Slot == sl::list::ClearDepthStencilView ||
                          Slot == sl::list::CopyResource || Slot == sl::list::CopyTextureRegion || Slot == sl::list::ResolveSubresource) {
                if constexpr (Slot == sl::list::ClearDepthStencilView) {
                    if (!t_host_work && scope.outermost && !scope.ambiguous) {
                        const auto listen = g_depth_clear_listener.load(std::memory_order_acquire);
                        const auto args = std::forward_as_tuple(a...);
                        if (listen && (std::get<1>(args) & D3D12_CLEAR_FLAG_DEPTH)) listen(l, cpu_ref(std::get<0>(args)).resource);
                    }
                }
                if (!t_host_work && scope.outermost && !scope.ambiguous) {
                    const auto hook = std::atomic_load_explicit(&g_texture_hook, std::memory_order_acquire);
                    if (hook) {
                        TextureCall call;
                        const auto args = std::forward_as_tuple(a...);
                        if constexpr (Slot == sl::list::ClearRenderTargetView) {
                            call.kind = TextureKind::ClearColor; call.target = std::get<0>(args);
                            call.target_id = cpu_ref(call.target).resource;
                            if (std::get<1>(args)) std::memcpy(call.color, std::get<1>(args), sizeof call.color);
                            call.rect_count = std::get<2>(args); call.rects = std::get<3>(args);
                        } else if constexpr (Slot == sl::list::ClearDepthStencilView) {
                            call.kind = TextureKind::ClearDepth; call.target = std::get<0>(args);
                            call.target_id = cpu_ref(call.target).resource; call.flags = std::get<1>(args);
                            call.depth = std::get<2>(args); call.stencil = std::get<3>(args);
                            call.rect_count = std::get<4>(args); call.rects = std::get<5>(args);
                        } else if constexpr (Slot == sl::list::CopyResource) {
                            call.kind = TextureKind::Copy; call.destination = std::get<0>(args); call.source = std::get<1>(args);
                        } else if constexpr (Slot == sl::list::CopyTextureRegion) {
                            call.kind = TextureKind::CopyRegion;
                            if (std::get<0>(args)) call.destination_location = *std::get<0>(args);
                            if (std::get<4>(args)) call.source_location = *std::get<4>(args);
                            call.destination = call.destination_location.pResource; call.source = call.source_location.pResource;
                            call.x = std::get<1>(args); call.y = std::get<2>(args); call.z = std::get<3>(args); call.box = std::get<5>(args);
                        } else {
                            call.kind = TextureKind::Resolve; call.destination = std::get<0>(args); call.destination_sub = std::get<1>(args);
                            call.source = std::get<2>(args); call.source_sub = std::get<3>(args); call.format = std::get<4>(args);
                        }
                        ShaderScope saved;
                        { HostWorkScope own; saved = hook->Before(l, call); }
                        if (saved.action == ShaderAction::Replaced) {
                            HostWorkScope own; hook->After(l, call, saved); return;
                        }
                        fn(l, a...);
                        if constexpr (Slot == sl::list::CopyResource || Slot == sl::list::CopyTextureRegion || Slot == sl::list::ResolveSubresource)
                            after_texture_copy(l, call.destination);
                        On<Slot>::run(l, a...);
                        { HostWorkScope own; hook->After(l, call, saved); }
                        return;
                    }
                }
            }
            { const CostScope c(kCostDriver); fn(l, a...); }
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
        ListHook<L::BeginQuery,void(ID3D12QueryHeap*,D3D12_QUERY_TYPE,UINT)>::spec(),
        ListHook<L::EndQuery,void(ID3D12QueryHeap*,D3D12_QUERY_TYPE,UINT)>::spec(),
        ListHook<L::DrawIndexedInstanced, void(UINT, UINT, UINT, INT, UINT)>::spec(),
        ListHook<L::Dispatch, void(UINT, UINT, UINT)>::spec(),
        ListHook<L::CopyBufferRegion, void(ID3D12Resource*, UINT64, ID3D12Resource*, UINT64, UINT64)>::spec(),
        ListHook<L::CopyTextureRegion, void(const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT, const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*)>::spec(),
        ListHook<L::CopyResource, void(ID3D12Resource*, ID3D12Resource*)>::spec(),
        ListHook<L::ResolveSubresource, void(ID3D12Resource*, UINT, ID3D12Resource*, UINT, DXGI_FORMAT)>::spec(),
        ListHook<L::IASetPrimitiveTopology, void(D3D12_PRIMITIVE_TOPOLOGY)>::spec(),
        ListHook<L::ClearState,void(ID3D12PipelineState*)>::spec(),
        ListHook<L::SetPredication,void(ID3D12Resource*,UINT64,D3D12_PREDICATION_OP)>::spec(),
        ListHook<L::IASetVertexBuffers,void(UINT,UINT,const D3D12_VERTEX_BUFFER_VIEW*)>::spec(),
        ListHook<L::IASetIndexBuffer,void(const D3D12_INDEX_BUFFER_VIEW*)>::spec(),
        ListHook<L::OMSetBlendFactor,void(const FLOAT*)>::spec(),
        ListHook<L::OMSetStencilRef,void(UINT)>::spec(),
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
    std::vector<na::hooking::variants::SlotSpec> remaining;
    for (const auto& spec : specs) if ((spec.slot != L::Reset || !g_reset_registered) &&
        (spec.slot!=L::ResourceBarrier || !g_barriers_registered)) remaining.push_back(spec);
    const bool ok = na::hooking::variants::Register(na::hooking::variants::Kind::CommandList, remaining.data(),
                                                    static_cast<uint32_t>(remaining.size()));
    if (ok) { g_reset_registered = true; g_barriers_registered=true;g_full_list_tracking.store(true, std::memory_order_release); }
    return ok;
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
    std::unique_lock<RegistryLock> lock(g_m);
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
    std::unique_lock<RegistryLock> lock(g_m);
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
            std::shared_lock<RegistryLock> lock(g_m);
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
                    std::shared_lock<RegistryLock> lock(g_m); // Unmap waits for the copy
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
    std::shared_lock<RegistryLock> rl(g_m);
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
        else known_reset_locked(it->second, valid ? kResetOverlap : ambiguous ? kResetAmbiguous : c.tainted ? kResetTainted :
                                             c.epoch != g_known_epoch ? kResetEpoch : kResetCopyQueue); // the exact cause, for the log
    }
    std::sort(good.begin(), good.end());
    std::vector<na_object_id> claimed; // ids whose queue ownership was settled in this publication
    for (const auto& n : c.notes) {
        if (!std::binary_search(good.begin(), good.end(), n.resource)) continue;
        ResKnown& k = g_known[n.resource];
        if (std::find(claimed.begin(), claimed.end(), n.resource) == claimed.end()) {
            claimed.push_back(n.resource);
            if (k.queue && k.queue != qid) known_reset_locked(k, kResetOtherQueue); // another queue's timeline: start over
            k.queue = qid;
        }
        if (n.subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
            k.has_all = true;
            k.all = n.state_after;
            k.subs.clear();
        } else if (k.subs.size() >= kMaxKnownSubs && !k.subs.count(n.subresource)) {
            known_reset_locked(k, kResetSubs);
            k.queue = qid;
        } else {
            k.subs[n.subresource] = n.state_after;
        }
        if (n.state_after == kStatePending) k.reset_why = kResetPendingNote;
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
    std::shared_lock<RegistryLock> lock(g_m);
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

bool InstallListResetTracking(ID3D12Device* device, void (*listener)(ID3D12GraphicsCommandList*),
    void (*barriers)(ID3D12GraphicsCommandList*,UINT,const D3D12_RESOURCE_BARRIER*))
{
    if (!device || !listener) return false;
    g_reset_listener.store(listener, std::memory_order_release);
    if(barriers) {
        g_barrier_listener.store(barriers);
        if(!g_barriers_registered){
            const auto spec=ListHook<sl::list::ResourceBarrier,void(UINT,const D3D12_RESOURCE_BARRIER*)>::spec();
            if(!na::hooking::Initialize() || !na::hooking::variants::Register(na::hooking::variants::Kind::CommandList,&spec,1))return false;
            g_barriers_registered=true;
        }
    }
    if (!g_reset_registered) {
        const auto spec = ListHook<sl::list::Reset, HRESULT(ID3D12CommandAllocator*, ID3D12PipelineState*)>::spec();
        if (!na::hooking::Initialize() ||
            !na::hooking::variants::Register(na::hooking::variants::Kind::CommandList, &spec, 1)) return false;
        g_reset_registered = true;
    }
    // The lightweight NR tracker is warmed only at initialization. Do not
    // suspend game/Streamline threads to install new hooks during playback.
    return na::hooking::variants::WarmUpCommandLists(device) != 0;
}

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
#if defined(__ID3D12Device11_INTERFACE_DEFINED__)
        ID3D12Device11* d11 = nullptr;
        device->QueryInterface(__uuidof(ID3D12Device11), reinterpret_cast<void**>(&d11));
#endif
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
#if defined(__ID3D12Device11_INTERFACE_DEFINED__)
        if (ok) ok = NA_HOOK(d11, device, CreateSampler2, false);
        if (d11) d11->Release();
#endif
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
    // Serialize game work per queue FIRST (C9B-R3 for samples; SE5 2026-11-02 for the
    // known-state store): two threads submitting to one queue at once leave the GPU order
    // unknown, which resets every carried state they touch. The store's marks must be taken
    // under the same lock, or a second thread marks inside the first one's submission
    // (Astra reproduced that race on db60492: kResetOverlap). The runtime serializes a queue's
    // submissions anyway, so the lock only fixes the observed order to the queue's.
    const bool armed = g_armed.load(std::memory_order_acquire);
    if (!t_host_work && t_submit_depth == 0) { // Signal/Wait too: they share the queue's order
        qs->submit_m.lock();
        ++t_submit_depth;
        t.submit_locked = true;
        if (!armed) g_c.serialized.fetch_add(1);
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
    if (t.submit_locked && armed) {
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
        std::shared_lock<RegistryLock> lock(g_m);
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
        std::unique_lock<RegistryLock> lock(g_m);
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
    std::shared_lock<RegistryLock> rl(g_m);
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
uint32_t ListCurrentGraphicsCbvs(ID3D12CommandList* list, LiveBinding* out, uint32_t max)
{
    if (!out || !max) return 0;
    std::shared_lock<RegistryLock> rl(g_m);
    na_object_id lid = 0;
    ListState* ls = list ? list_state_locked(list, &lid) : nullptr;
    if (!ls || !ls->recording_observed.load()) return 0;
    std::lock_guard<std::mutex> bl(ls->bind_m);
    uint32_t n = 0;
    size_t scanned = 0;
    for (auto it = ls->bindings.rbegin(); it != ls->bindings.rend() && n < max && scanned < 512; ++it, ++scanned) {
        const auto& b = *it;
        if (!b.resource || b.compute || (b.kind != NA_SNAP_ROOT_CBV && b.kind != NA_SNAP_TABLE_CBV)) continue;
        const bool table = b.kind == NA_SNAP_TABLE_CBV;
        bool seen = false; // a later binding of the same root parameter replaced this one
        for (uint32_t i = 0; i < n && !seen; ++i) seen = out[i].param == b.param && out[i].table == table && !table;
        if (seen) continue;
        out[n++] = {b.resource, b.offset, b.size, b.op_index, b.param, table};
    }
    return n;
}
uint32_t ListCurrentGraphicsSrvs(ID3D12CommandList* list, na_object_id* out, uint32_t max, bool* incomplete)
{
    bool cut = false;
    struct Report { bool* to; bool* cut; ~Report() { if (to) *to = *cut; } } report{incomplete, &cut};
    if (!out || !max) { cut = true; return 0; }
    std::shared_lock<RegistryLock> rl(g_m);
    na_object_id lid = 0;
    ListState* ls = list ? list_state_locked(list, &lid) : nullptr;
    if (!ls || !ls->recording_observed.load()) { cut = true; return 0; }
    std::lock_guard<std::mutex> bl(ls->bind_m);
    const auto signature = g_objs.find(canon(ls->root_sig[0]));
    if (signature == g_objs.end()) { cut = true; return 0; }
    const auto& params = signature->second.root_params;
    const auto& ranges = signature->second.root_ranges;
    uint32_t n = 0;
    for (size_t p = 0; p < ls->args[0].size() && p < params.size(); ++p) {
        const auto& arg = ls->args[0][p];
        if (params[p].param_type != NA_ROOT_TABLE) continue;
        if (arg.type != 1) continue;
        uint32_t slot = 0;
        auto* heap = bound_heap_for_gpu_locked(*ls, arg.value, &slot);
        if (!heap) { cut = true; continue; }
        bool bound = false;
        for (uint32_t h = 0; h < ls->nheaps; ++h) bound |= heap->key == ls->heaps[h];
        if (!bound) { cut = true; continue; }
        for (uint32_t r = 0; r < params[p].count && params[p].first_range + r < ranges.size(); ++r) {
            const auto& range = ranges[params[p].first_range + r];
            if (range.range_type != D3D12_DESCRIPTOR_RANGE_TYPE_SRV) continue;
            // Wide ranges are walked up to kWide descriptors (a bindless table beyond that is
            // reported incomplete rather than read as its first entries).
            constexpr uint32_t kWide = 1024;
            if (range.table_offset == NA_UNKNOWN_U32 || range.num_descriptors > kWide) cut = true;
            if (range.table_offset == NA_UNKNOWN_U32) continue;
            SlotShared stripe;
            for (uint32_t k = 0; k < std::min<uint32_t>(range.num_descriptors, kWide); ++k) {
                const uint64_t index = uint64_t(slot) + range.table_offset + k;
                if (index >= heap->count) break;
                stripe.lock(heap, index);
                const auto& info = heap->info[static_cast<size_t>(index)];
                if (info.kind != NA_DESC_SRV || !info.resource) continue;
                bool seen = false;
                for (uint32_t i = 0; i < n && !seen; ++i) seen = out[i] == info.resource;
                if (seen) continue;
                if (n == max) { cut = true; continue; }
                out[n++] = info.resource;
            }
        }
    }
    return n;
}
bool ListReferences(ID3D12CommandList* list, na_object_id id)
{
    if (!id) return false;
    std::shared_lock<RegistryLock> rl(g_m);
    na_object_id lid = 0;
    ListState* ls = list ? list_state_locked(list, &lid) : nullptr;
    if (!ls || !ls->recording_observed.load()) return false;
    std::lock_guard<std::mutex> bl(ls->bind_m);
    return std::find(ls->uses.begin(), ls->uses.end(), id) != ls->uses.end();
}
bool ListRendersTo(ID3D12CommandList* list, na_object_id color, na_object_id depth)
{
    if (!color || !depth) return false;
    std::shared_lock<RegistryLock> rl(g_m);
    na_object_id lid = 0;
    ListState* ls = list ? list_state_locked(list, &lid) : nullptr;
    if (!ls || !ls->recording_observed.load()) return false;
    std::lock_guard<std::mutex> bl(ls->bind_m);
    return std::any_of(ls->targets.begin(), ls->targets.end(), [&](const ListState::Target& t) { return t.same(color, depth); });
}
na::j2::InsertionDecision ListInsertionDecision(ID3D12CommandList* list, na_object_id color)
{
    std::shared_lock<RegistryLock> rl(g_m);
    na_object_id lid = 0;
    ListState* ls = list ? list_state_locked(list, &lid) : nullptr;
    if (!ls || !ls->recording_observed.load()) return {};
    std::lock_guard<std::mutex> bl(ls->bind_m);
    return ls->insertion.Analyze(color);
}
na::j2::ConsumerSignature ListConsumerSignature(ID3D12CommandList* list, na_object_id color)
{
    std::shared_lock<RegistryLock> rl(g_m);
    na_object_id lid = 0;
    ListState* ls = list ? list_state_locked(list, &lid) : nullptr;
    if (!ls || !ls->recording_observed.load()) return {};
    std::lock_guard<std::mutex> bl(ls->bind_m);
    return ls->insertion.AnalyzeConsumer(color);
}
bool ListColorEvents(ID3D12CommandList* list, na_object_id color, std::vector<na::j2::ColorEvent>& out)
{
    std::shared_lock<RegistryLock> rl(g_m);
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
    std::shared_lock<RegistryLock> rl(g_m);
    na_object_id lid = 0;
    ListState* ls = list ? list_state_locked(list, &lid) : nullptr;
    if (!ls || !ls->recording_observed.load()) return {};
    std::lock_guard<std::mutex> bl(ls->bind_m);
    return ls->insertion.Live(color);
}
void SetTableBindCallback(TableBindCallback cb) { g_table_cb.store(cb, std::memory_order_release); }
void SetEntryCallback(EntryCallback cb) { g_entry_cb.store(cb, std::memory_order_release); }
bool ListCurrentTargets(ID3D12CommandList* list, na_object_id* rt, na_object_id* ds)
{
    std::shared_lock<RegistryLock> rl(g_m);
    na_object_id lid = 0;
    ListState* ls = list ? list_state_locked(list, &lid) : nullptr;
    if (!ls || !ls->recording_observed.load()) return false;
    std::lock_guard<std::mutex> bl(ls->bind_m);
    if (rt) *rt = ls->cur_rt;
    if (ds) *ds = ls->cur_ds;
    return true;
}
void SetRasterStateListener(RasterStateListener listener) { g_raster_listener.store(listener, std::memory_order_release); }
bool ListRasterScissors(ID3D12CommandList* list, RasterScissors& out)
{
    std::shared_lock<RegistryLock> rl(g_m);
    na_object_id lid = 0;
    ListState* ls = list ? list_state_locked(list, &lid) : nullptr;
    if (!ls || !ls->recording_observed.load() || !ls->generation_known.load()) return false;
    std::lock_guard<std::mutex> bl(ls->bind_m);
    if (!ls->raster_scissors_known || ls->in_render_pass) return false;
    out.recording = ls->generation.load() + 1;
    out.scissor_serial = ls->scissor_serial;
    out.ds = ls->cur_ds;
    out.count = ls->raster_scissor_count < 16 ? ls->raster_scissor_count : 16;
    static_assert(sizeof(RasterScissors::Rect) == sizeof(D3D12_RECT), "D3D12_RECT layout");
    std::memcpy(out.rects, ls->raster_scissors, out.count * sizeof(D3D12_RECT));
    return true;
}
bool ListDrawState(ID3D12CommandList* list, DrawInfo& out)
{
    DrawInfo r;
    std::shared_lock<RegistryLock> rl(g_m);
    na_object_id lid = 0;
    ListState* ls = list ? list_state_locked(list, &lid) : nullptr;
    if (!ls || !ls->recording_observed.load() || !ls->generation_known.load()) return false;
    r.list = lid;
    r.recording = ls->generation.load() + 1;
    const bool complete = ls->coverage_complete.load();
    std::lock_guard<std::mutex> bl(ls->bind_m);
    r.rt = ls->cur_rt; r.ds = ls->cur_ds; r.rt_count = ls->cur_nrt;
    r.query_active=ls->active_queries!=0;
    r.restorable = complete && ls->raster_tracking && ls->restorable && !ls->in_render_pass && ls->raster_targets_known &&
                   ls->raster_viewports_known && ls->raster_scissors_known;
    if (ls->raster_viewports_known) {
        r.viewport_count = ls->raster_viewport_count < 16 ? ls->raster_viewport_count : 16;
        for (uint32_t i = 0; i < r.viewport_count; ++i) {
            const D3D12_VIEWPORT& v = ls->raster_viewports[i];
            const float f[6] = {v.TopLeftX, v.TopLeftY, v.Width, v.Height, v.MinDepth, v.MaxDepth};
            std::memcpy(r.viewports[i], f, sizeof f);
        }
    }
    if (ls->raster_scissors_known) {
        r.scissor_count = ls->raster_scissor_count < 16 ? ls->raster_scissor_count : 16;
        for (uint32_t i = 0; i < r.scissor_count; ++i) {
            const D3D12_RECT& c = ls->raster_scissors[i];
            r.scissors[i][0] = c.left; r.scissors[i][1] = c.top; r.scissors[i][2] = c.right; r.scissors[i][3] = c.bottom;
        }
    }
    const auto pipeline = ls->pso ? g_objs.find(canon(ls->pso)) : g_objs.end();
    if (pipeline != g_objs.end() && pipeline->second.created.size() >= sizeof(na_ev_pipeline_created)) {
        const auto* p = reinterpret_cast<const na_ev_pipeline_created*>(pipeline->second.created.data());
        if (p->pipeline_kind == NA_PIPELINE_GRAPHICS || (p->pipeline_kind == NA_PIPELINE_STREAM && p->stage_hash[NA_STAGE_CS] == 0)) {
            r.pipeline_known = true;
            r.pso_rt_count = p->rt_count; r.rtv_format = p->rtv_formats[0]; r.dsv_format = p->dsv_format;
            r.samples = p->sample_count; r.blend_mask = p->blend_enable_mask;
            r.native_hud_affine=pipeline->second.native_hud_affine;
            r.blend_class=pipeline->second.blend_class;
            r.shader_readonly=pipeline->second.shader_readonly;
            r.shader_rejection=pipeline->second.shader_rejection;
            r.pixel_position=pipeline->second.pixel_position;
            r.depth_enable = p->depth_enable; r.depth_func = p->depth_func;
        }
    }
    out = r;
    return true;
}
uint64_t ListRecording(ID3D12CommandList* list)
{
    std::shared_lock<RegistryLock> rl(g_m);
    na_object_id lid = 0;
    ListState* ls = list ? list_state_locked(list, &lid) : nullptr;
    if (!ls || !ls->recording_observed.load() || !ls->generation_known.load()) return 0;
    return ls->generation.load() + 1; // never 0
}
bool ListStateRestorable(ID3D12CommandList* l)
{
    std::shared_lock<RegistryLock> rl(g_m);
    na_object_id lid = 0;
    ListState* ls = l ? list_state_locked(l, &lid) : nullptr;
    if (!ls || !ls->recording_observed.load() || !ls->coverage_complete.load()) return false;
    std::lock_guard<std::mutex> bl(ls->bind_m);
    return ls->restorable && !ls->in_render_pass;
}
bool ListShaderState(ID3D12CommandList* l, bool compute, ShaderState& out)
{
    ShaderState result;
    std::shared_lock<RegistryLock> rl(g_m);
    na_object_id lid = 0;
    ListState* ls = l ? list_state_locked(l, &lid) : nullptr;
    if (!ls || !ls->recording_observed.load() || !ls->coverage_complete.load()) return false;
    std::lock_guard<std::mutex> bl(ls->bind_m);
    if (!ls->restorable || ls->in_render_pass) return false;
    result.root = static_cast<ID3D12RootSignature*>(const_cast<void*>(ls->root_sig[compute ? 1 : 0]));
    result.list_type=ls->list_type;
    result.raster_restorable=ls->raster_tracking&&ls->raster_targets_known&&ls->raster_viewports_known&&ls->raster_scissors_known;
    result.target_serial=ls->target_serial;result.viewport_serial=ls->viewport_serial;result.scissor_serial=ls->scissor_serial;
    result.root_serial=ls->root_serial[compute];result.restore_serial=ls->host_restore_serial;
    bound_heaps_locked(*ls);
    for (uint32_t i = 0; i < ls->nheaps; ++i) {
        const DescHeap* heap = ls->heap_metadata[i];
        if (!heap) return false;
        if (heap->type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) result.resources = ls->heaps[i];
        else if (heap->type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) result.samplers = ls->heaps[i];
        else return false;
    }
    const auto& args = ls->args[compute ? 1 : 0];
    if (args.size() > 64) return false;
    for (uint32_t p = 0; p < args.size(); ++p) {
        const auto& a = args[p];
        if (a.type == 2){result.cbv_params |= uint64_t(1) << p;result.cbv_serial[p]=a.serial;}
        if (a.type != 1) continue;
        uint32_t slot=0;const DescHeap* heap=bound_heap_for_gpu_locked(*ls,a.value,&slot);
        if (!heap) return false;
        result.tables[result.count++] = {p, a.value, heap->type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER};
    }
    out = result;
    return result.root != nullptr;
}
void SetShaderOperationHook(ShaderLane lane, std::shared_ptr<ShaderOperationHook> hook)
{
    const auto index = static_cast<unsigned>(lane);
    if (index >= 2) return;
    std::shared_ptr<const ShaderHooks> prior;
    {
        std::lock_guard<std::mutex> lock(g_shader_hooks_m);
        prior = std::atomic_load_explicit(&g_shader_hooks, std::memory_order_acquire);
        auto next = std::make_shared<ShaderHooks>();
        if (prior) *next = *prior;
        next->lane[index] = std::move(hook);
        const bool enabled = !!next->lane[0] || !!next->lane[1];
        std::atomic_store_explicit(&g_shader_hooks, std::shared_ptr<const ShaderHooks>(next), std::memory_order_release);
        g_shader_hooks_enabled.store(enabled, std::memory_order_release);
    } // old callbacks/resources can be destroyed outside the registration lock
}
void SetDepthClearListener(DepthClearListener listener) { g_depth_clear_listener.store(listener, std::memory_order_release); }
void SetTextureOperationHook(std::shared_ptr<TextureOperationHook> hook)
{
    auto prior = std::atomic_exchange_explicit(&g_texture_hook, std::move(hook), std::memory_order_acq_rel);
}
void SetRasterStateTracking(bool enabled) { g_raster_tracking.store(enabled, std::memory_order_release); }
bool ListRasterStateRestorable(ID3D12CommandList* list)
{
    std::shared_lock<RegistryLock> lock(g_m);
    na_object_id id = 0;
    ListState* ls = list ? list_state_locked(list, &id) : nullptr;
    if (!ls || !ls->recording_observed.load() || !ls->coverage_complete.load()) return false;
    std::lock_guard<std::mutex> bl(ls->bind_m);
    return ls->raster_tracking && ls->restorable && !ls->in_render_pass && ls->raster_targets_known &&
        ls->raster_viewports_known && ls->raster_scissors_known && ls->raster_topology_known;
}
bool RestoreListState(ID3D12GraphicsCommandList* l,ID3D12GraphicsCommandList* destination,bool graphics_only,std::vector<uint64_t>* graphics_reads)
{
    // Snapshot under the locks, issue the calls without them (these calls run
    // under the caller's HostWorkScope, so they are not observed as the game's).
    ID3D12PipelineState* pso = nullptr;
    ID3D12DescriptorHeap* heaps[2] = {};
    uint32_t nheaps = 0;
    const void* rs[2] = {};
    struct RootReplay {uint8_t type=0;uint64_t value=0;UINT count=0;uint32_t consts[64]{};};
    RootReplay args[2][64];UINT nargs[2]{};
    std::shared_ptr<Obj::Twins> raster_storage;
    D3D12_VIEWPORT viewports[D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    D3D12_RECT scissors[D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT viewport_count = 0, scissor_count = 0, rt_count = 0;
    D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    bool vp_known = false, sc_known = false, topology_known = false, has_depth = false;
    D3D12_VERTEX_BUFFER_VIEW vertices[32]{};UINT vertex_mask=0;D3D12_INDEX_BUFFER_VIEW index{};bool index_known=false;
    FLOAT blend[4]={1,1,1,1};UINT stencil=0;
    ID3D12Resource* predicate=nullptr;UINT64 predicate_offset=0;D3D12_PREDICATION_OP predicate_op=D3D12_PREDICATION_OP_EQUAL_ZERO;
    {
        std::shared_lock<RegistryLock> rl(g_m);
        na_object_id lid = 0;
        ListState* ls = l ? list_state_locked(l, &lid) : nullptr;
        if (!ls || !ls->recording_observed.load() || !ls->coverage_complete.load()) return false;
        std::lock_guard<std::mutex> bl(ls->bind_m);
        if (!ls->restorable || ls->in_render_pass) return false;
        pso = ls->pso;
        vertex_mask=ls->vertex_mask;std::copy_n(ls->vertex_buffers,32,vertices);index=ls->index_buffer;index_known=ls->index_known;
        std::copy_n(ls->blend_factor,4,blend);stencil=ls->stencil_ref;
        predicate=ls->predicate;predicate_offset=ls->predicate_offset;predicate_op=ls->predicate_op;
        if(graphics_reads){
            graphics_reads->clear();
            const auto keep=[&](uint64_t id){if(id&&std::find(graphics_reads->begin(),graphics_reads->end(),id)==graphics_reads->end())graphics_reads->push_back(id);};
            const auto address=[&](uint64_t va){uint64_t id=0,offset=0;if(!va)return true;if(!resolve_va_locked(va,&id,&offset))return false;keep(id);return true;};
            for(UINT i=0;i<32;++i)if((vertex_mask&(1u<<i))&&!address(vertices[i].BufferLocation))return false;
            if(index_known&&!address(index.BufferLocation))return false;
            if(predicate){const auto p=g_objs.find(canon(predicate));if(p==g_objs.end())return false;keep(p->second.id);}
            const auto root=g_objs.find(canon(ls->root_sig[0]));if(root==g_objs.end())return false;
            for(UINT p=0;p<ls->args[0].size();++p){const auto& a=ls->args[0][p];
                if(a.type==2||a.type==3){if(!address(a.value))return false;}
                else if(a.type==4)return false;
                else if(a.type==1){
                    if(p>=root->second.root_params.size())return false;const auto& param=root->second.root_params[p];
                    if(param.first_range>root->second.root_ranges.size()||param.count>root->second.root_ranges.size()-param.first_range)return false;
                    UINT slot=0;const auto* heap=heap_for_gpu_locked(a.value,&slot);if(!heap)return false;
                    for(UINT j=0;j<param.count;++j){const auto& range=root->second.root_ranges[param.first_range+j];
                        if(range.range_type==D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER)continue;
                        if(range.range_type==D3D12_DESCRIPTOR_RANGE_TYPE_UAV||range.num_descriptors==UINT_MAX||range.table_offset==UINT_MAX||
                            range.table_offset>4096||range.num_descriptors>4096-range.table_offset||slot+range.table_offset>heap->count||range.num_descriptors>heap->count-slot-range.table_offset)return false;
                        for(UINT j2=0;j2<range.num_descriptors;++j2){
                            std::shared_ptr<const DescriptorDetails> det;
                            {const SlotShared stripe(heap,uint64_t(slot)+range.table_offset+j2);det=heap->info[slot+range.table_offset+j2].details;}
                            if(!det)return false;
                            if(det->kind==NA_DESC_CBV){if(!address(det->cbv.BufferLocation))return false;}
                            else keep(det->resource);
                        }
                    }
                }
            }
        }
        ++ls->host_restore_serial;
        nheaps = ls->nheaps;
        heaps[0] = ls->heaps[0];
        heaps[1] = ls->heaps[1];
        rs[0] = ls->root_sig[0];
        rs[1] = ls->root_sig[1];
        for(UINT c=0;c<(graphics_only?1u:2u);++c){if(ls->args[c].size()>64)return false;nargs[c]=UINT(ls->args[c].size());
            for(UINT p=0;p<nargs[c];++p){const auto& from=ls->args[c][p];auto& to=args[c][p];
                if(from.consts.size()>64)return false;to.type=from.type;to.value=from.value;to.count=UINT(from.consts.size());
                if(to.count)std::copy_n(from.consts.data(),to.count,to.consts);}}
        if (ls->raster_tracking) {
            vp_known = ls->raster_viewports_known; viewport_count = ls->raster_viewport_count;
            sc_known = ls->raster_scissors_known; scissor_count = ls->raster_scissor_count;
            topology_known = ls->raster_topology_known; topology = ls->raster_topology;
            if (vp_known) std::copy_n(ls->raster_viewports, viewport_count, viewports);
            if (sc_known) std::copy_n(ls->raster_scissors, scissor_count, scissors);
            if (ls->raster_targets_known) {
                const auto owner = g_objs.find(canon(l));
                if (owner != g_objs.end()) raster_storage = owner->second.twins;
                rt_count = ls->raster_rt_count; has_depth = ls->raster_has_depth;
            }
        }
    }
    if(destination)l=destination;
    if (nheaps) l->SetDescriptorHeaps(nheaps, heaps);
    for (uint32_t c = 0; c < (graphics_only?1u:2u); ++c) {
        if (!rs[c]) continue;
        auto* sig = static_cast<ID3D12RootSignature*>(const_cast<void*>(rs[c]));
        if (c) l->SetComputeRootSignature(sig); else l->SetGraphicsRootSignature(sig);
        for (UINT p = 0; p < nargs[c]; ++p) {
            const auto& a = args[c][p];
            switch (a.type) {
            case 1: { D3D12_GPU_DESCRIPTOR_HANDLE h{a.value}; if (c) l->SetComputeRootDescriptorTable(p, h); else l->SetGraphicsRootDescriptorTable(p, h); break; }
            case 2: if (c) l->SetComputeRootConstantBufferView(p, a.value); else l->SetGraphicsRootConstantBufferView(p, a.value); break;
            case 3: if (c) l->SetComputeRootShaderResourceView(p, a.value); else l->SetGraphicsRootShaderResourceView(p, a.value); break;
            case 4: if (c) l->SetComputeRootUnorderedAccessView(p, a.value); else l->SetGraphicsRootUnorderedAccessView(p, a.value); break;
            case 5: if (a.count) { if (c) l->SetComputeRoot32BitConstants(p, a.count, a.consts, 0); else l->SetGraphicsRoot32BitConstants(p, a.count, a.consts, 0); } break;
            default: break;
            }
        }
    }
    if (pso) l->SetPipelineState(pso);
    if(destination){
        for(UINT i=0;i<32;++i)if(vertex_mask&(1u<<i))l->IASetVertexBuffers(i,1,&vertices[i]);
        if(index_known)l->IASetIndexBuffer(index.BufferLocation?&index:nullptr);
        l->OMSetBlendFactor(blend);l->OMSetStencilRef(stencil);
        l->SetPredication(predicate,predicate_offset,predicate_op);
    }
    if (raster_storage && raster_storage->obj[0] && raster_storage->obj[1]) {
        const auto rt = static_cast<ID3D12DescriptorHeap*>(raster_storage->obj[0])->GetCPUDescriptorHandleForHeapStart();
        const auto ds = static_cast<ID3D12DescriptorHeap*>(raster_storage->obj[1])->GetCPUDescriptorHandleForHeapStart();
        l->OMSetRenderTargets(rt_count, rt_count ? &rt : nullptr, TRUE, has_depth ? &ds : nullptr);
    }
    if (vp_known) l->RSSetViewports(viewport_count, viewport_count ? viewports : nullptr);
    if (sc_known) l->RSSetScissorRects(scissor_count, scissor_count ? scissors : nullptr);
    if (topology_known) l->IASetPrimitiveTopology(topology);
    return true;
}
bool RestoreBindingState(ID3D12GraphicsCommandList* l,bool compute,bool restore_heaps,uint64_t cbvs,bool targets,bool vp,bool sc,ID3D12RootSignature* expected_root){
    ID3D12DescriptorHeap* heaps[2]{};UINT heap_count=0;
    struct Table{UINT param;uint64_t address;};Table tables[2][64]{};UINT table_count[2]{};
    uint64_t addresses[64]{};D3D12_VIEWPORT viewports[16]{};D3D12_RECT scissors[16]{};
    UINT viewport_count=0,scissor_count=0,rt_count=0;bool depth=false;
    std::shared_ptr<Obj::Twins> storage;
    {std::shared_lock<RegistryLock> lock(g_m);na_object_id id=0;auto* ls=l?list_state_locked(l,&id):nullptr;
        if(!ls||!ls->recording_observed.load()||!ls->coverage_complete.load())return false;
        std::lock_guard<std::mutex> bind(ls->bind_m);if(!ls->restorable||ls->in_render_pass)return false;
        if(expected_root&&ls->root_sig[compute]!=expected_root)cbvs=0;
        if(restore_heaps){heap_count=ls->nheaps;std::copy_n(ls->heaps,heap_count,heaps);
            for(UINT c=0;c<2;++c){if(ls->args[c].size()>64)return false;
                for(UINT p=0;p<ls->args[c].size();++p)if(ls->args[c][p].type==1)tables[c][table_count[c]++]={p,ls->args[c][p].value};}}
        for(UINT p=0;p<64;++p)if(cbvs&(uint64_t(1)<<p)){
            if(p>=ls->args[compute].size()||ls->args[compute][p].type!=2)return false;addresses[p]=ls->args[compute][p].value;}
        if(targets||vp||sc){if(!ls->raster_tracking)return false;
            if(targets){if(!ls->raster_targets_known)return false;const auto owner=g_objs.find(canon(l));
                if(owner==g_objs.end()||!owner->second.twins||!owner->second.twins->obj[0]||!owner->second.twins->obj[1])return false;
                storage=owner->second.twins;rt_count=ls->raster_rt_count;depth=ls->raster_has_depth;}
            if(vp){if(!ls->raster_viewports_known)return false;viewport_count=ls->raster_viewport_count;std::copy_n(ls->raster_viewports,viewport_count,viewports);}
            if(sc){if(!ls->raster_scissors_known)return false;scissor_count=ls->raster_scissor_count;std::copy_n(ls->raster_scissors,scissor_count,scissors);}}
    }
    if(restore_heaps){l->SetDescriptorHeaps(heap_count,heap_count?heaps:nullptr);
        for(UINT c=0;c<2;++c)for(UINT i=0;i<table_count[c];++i){const auto& table=tables[c][i];
            if(c)l->SetComputeRootDescriptorTable(table.param,{table.address});else l->SetGraphicsRootDescriptorTable(table.param,{table.address});}}
    for(UINT p=0;p<64;++p)if(cbvs&(uint64_t(1)<<p)){
        if(compute)l->SetComputeRootConstantBufferView(p,addresses[p]);else l->SetGraphicsRootConstantBufferView(p,addresses[p]);}
    if(targets){if(!storage->obj[0]||!storage->obj[1])return false;
        const auto rt=static_cast<ID3D12DescriptorHeap*>(storage->obj[0])->GetCPUDescriptorHandleForHeapStart();
        const auto ds=static_cast<ID3D12DescriptorHeap*>(storage->obj[1])->GetCPUDescriptorHandleForHeapStart();
        l->OMSetRenderTargets(rt_count,rt_count?&rt:nullptr,TRUE,depth?&ds:nullptr);}
    if(vp)l->RSSetViewports(viewport_count,viewport_count?viewports:nullptr);
    if(sc)l->RSSetScissorRects(scissor_count,scissor_count?scissors:nullptr);
    return true;
}
bool ListClearsTarget(ID3D12CommandList* list, na_object_id target)
{
    std::shared_lock<RegistryLock> rl(g_m);
    na_object_id lid = 0;
    ListState* ls = list ? list_state_locked(list, &lid) : nullptr;
    if (!ls || !ls->recording_observed.load()) return false;
    std::lock_guard<std::mutex> bl(ls->bind_m);
    return std::find(ls->cleared.begin(), ls->cleared.end(), target) != ls->cleared.end();
}
bool ListSamplesTexture(ID3D12CommandList* list, na_object_id texture)
{
    std::shared_lock<RegistryLock> rl(g_m);
    na_object_id lid = 0;
    ListState* ls = list ? list_state_locked(list, &lid) : nullptr;
    if (!ls || !ls->recording_observed.load()) return false;
    std::lock_guard<std::mutex> bl(ls->bind_m);
    return std::find(ls->sampled_textures.begin(), ls->sampled_textures.end(), texture) != ls->sampled_textures.end();
}
bool ListSceneSubresource(ID3D12CommandList* list, na_object_id color, na_object_id depth, uint32_t* sub, uint32_t* depth_sub)
{
    if (!sub || !depth_sub) return false;
    std::shared_lock<RegistryLock> rl(g_m);
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
    std::shared_lock<RegistryLock> rl(g_m);
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
StateLookup ListStateSub(ID3D12CommandList* list, na_object_id id, uint32_t sub, uint32_t* state)
{
    std::shared_lock<RegistryLock> rl(g_m);
    na_object_id lid = 0;
    ListState* ls = list ? list_state_locked(list, &lid) : nullptr;
    if (!ls || !ls->recording_observed.load() || !ls->coverage_complete.load()) return StateLookup::Unavailable;
    std::lock_guard<std::mutex> bl(ls->bind_m);
    if (!ls->evidence_complete) return StateLookup::Unavailable;
    for (auto it = ls->states.rbegin(); it != ls->states.rend(); ++it) {
        if (it->resource != id) continue;
        if (it->subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES && it->subresource != sub) continue; // another plane/mip/slice
        if (it->state_after == kStatePending) return StateLookup::Unavailable;
        *state = it->state_after;
        return StateLookup::Known;
    }
    return StateLookup::Absent;
}
bool ListLastStateSub(ID3D12CommandList* list, na_object_id id, uint32_t sub, uint32_t* state)
{
    return ListStateSub(list, id, sub, state) == StateLookup::Known;
}
bool ListLastState(ID3D12CommandList* list, na_object_id id, uint32_t* state)
{
    std::shared_lock<RegistryLock> rl(g_m);
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
        std::shared_lock<RegistryLock> lock(g_m);
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
        std::shared_lock<RegistryLock> lock(g_m);
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
bool KnownSubStateOnQueueNow(ID3D12CommandQueue* queue, na_object_id id, uint32_t sub, uint32_t* state)
{
    const auto qid=queue?lookup(queue):0;
    return qid && state && id && known_state(id,sub,qid,0,state);
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
void DescribeStateEvidence(ID3D12CommandList* list, na_object_id id, uint32_t sub, char* text, size_t capacity)
{
    uint32_t kind = NA_UNKNOWN_U32;
    bool covered = false, complete = false;
    {
        std::shared_lock<RegistryLock> lock(g_m);
        const auto key = g_res_keys.find(id);
        if (key != g_res_keys.end()) {
            const auto obj = g_objs.find(key->second);
            if (obj != g_objs.end() && obj->second.created.size() >= sizeof(na_ev_resource_created))
                kind = reinterpret_cast<const na_ev_resource_created*>(obj->second.created.data())->creation_kind;
        }
        na_object_id lid = 0;
        auto* ls = list ? list_state_locked(list, &lid) : nullptr;
        if (ls) {
            std::lock_guard<std::mutex> bl(ls->bind_m);
            covered = ls->recording_observed.load() && ls->coverage_complete.load();
            complete = ls->evidence_complete;
        }
    }
    std::lock_guard<std::mutex> lock(g_known_m);
    const auto k = g_known.find(id);
    uint32_t value = kStatePending;
    const char* why = kResetNames[kResetNone];
    if (k != g_known.end()) {
        const auto s = k->second.subs.find(sub);
        value = s != k->second.subs.end() ? s->second : k->second.has_all ? k->second.all : kStatePending;
        if (k->second.reset_why < sizeof kResetNames / sizeof kResetNames[0]) why = kResetNames[k->second.reset_why];
    }
    std::snprintf(text, capacity, "kind=%u list_covered=%u list_complete=%u registered=%u state=0x%x last_unknown=%s marks=%u unknown_submissions=%u disabled=%u unhooked_queues=%llu",
                  kind, covered ? 1u : 0u, complete ? 1u : 0u, k != g_known.end() ? 1u : 0u, value, why,
                  k != g_known.end() ? k->second.in_flight : 0u, g_unknown_inflight,
                  g_known_disabled.load() ? 1u : 0u, static_cast<unsigned long long>(g_unhooked_queues.load()));
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
namespace {
// ---- Live sampler variants: the background worker. A chosen variant that some objects
// lack is built here (root signatures first, then the pipelines on them), outside g_m,
// and published per object; recordings switch to it (g_var_live, latched at Reset) only
// once every object that can have it has it, so a frame never mixes old and new.
struct FillItem {
    const void* key = nullptr;
    na_object_id id = 0;
    std::shared_ptr<const VariantRecipe> recipe;
    std::shared_ptr<Obj::Twins> base;
};
// One pass for variant (level, af). Returns how many objects lacked it, or SIZE_MAX when
// the choice changed meanwhile (the caller starts over with the new one).
// Background pool for pipeline compiles (Cort 2026-11-06: use the Taskflow workers). Set by the
// host after it starts na::background; unset (tests, early boot) = everything on this thread.
std::atomic<BackgroundSubmit> g_bg_submit{nullptr};
void publish_variant(const FillItem& it, int af, std::shared_ptr<Obj::Twins> built)
{
    if (built) {
        std::unique_lock<RegistryLock> lock(g_m);
        const auto f = g_objs.find(it.key);
        if (f != g_objs.end() && f->second.id == it.id && f->second.af_sets[af] == it.base) f->second.af_sets[af].swap(built);
    }
    // `built` now holds the replaced set (or an unused new one): released here, outside g_m
    built.reset();
    g_var_done.fetch_add(1, std::memory_order_relaxed);
}
void fill_pso_item(const FillItem& it, int level, int af)
{
    std::shared_ptr<Obj::Twins> rs_set;
    {
        std::shared_lock<RegistryLock> lock(g_m);
        const auto f = g_objs.find(it.recipe->rs_key);
        if (f != g_objs.end() && f->second.id == it.recipe->rs_id) rs_set = f->second.af_sets[af];
    }
    if (!rs_set || !rs_set->built[level]) { g_var_done.fetch_add(1, std::memory_order_relaxed); return; } // next pass
    const VariantRecipe& r = *it.recipe;
    // Static root variants are cheap and may already cover several levels.
    // Compile only the requested PSO level before making this choice live.
    publish_variant(it, af, build_pso_set([&r](ID3D12RootSignature* trs) { return r.make(trs); }, *rs_set, it.base.get(), 1u << level));
}
// Pipelines of one pass, shared out item by item between this thread and up to two pool jobs
// (D3D12 pipeline creation is free-threaded). Owned by a shared_ptr so a job that starts late,
// or one still running when this thread gives up at process exit, never touches a dead frame.
struct PsoBatch {
    std::vector<FillItem> items;
    std::atomic<size_t> next{0};
    std::atomic<bool> stopped{false};
    std::mutex m;
    std::condition_variable cv;
    int running = 0;
    int level = 0, af = 0;
    uint32_t v = 0;
    void run()
    {
        for (;;) {
            if (g_var_want.load(std::memory_order_relaxed) != v || g_process_exiting.load(std::memory_order_acquire)) { stopped = true; return; }
            const size_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= items.size()) return;
            fill_pso_item(items[i], level, af);
        }
    }
};
size_t fill_pass(int level, int af, uint32_t v)
{
    std::vector<FillItem> rs_items, pso_items;
    {
        std::shared_lock<RegistryLock> lock(g_m);
        for (const auto& kv : g_objs) {
            const Obj& o = kv.second;
            if (o.variant_kind != 1 || !o.recipe) continue;
            const auto& set = o.af_sets[af];
            if (set && set->built[level]) continue;
            auto& to = o.recipe->kind == VariantRecipe::RootSignature ? rs_items : pso_items;
            to.push_back({kv.first, o.id, o.recipe, set});
        }
    }
    const size_t found = rs_items.size() + pso_items.size();
    g_var_total.store(found, std::memory_order_relaxed);
    g_var_done.store(0, std::memory_order_relaxed);
    for (const auto& it : rs_items) { // root signatures: cheap, kept in order on this thread
        if (g_var_want.load(std::memory_order_relaxed) != v) return SIZE_MAX;
        publish_variant(it, af, build_rs_set(*it.recipe, af, 1u << level, it.base.get()));
    }
    if (pso_items.empty()) return found;
    auto batch = std::make_shared<PsoBatch>();
    batch->items = std::move(pso_items);
    batch->level = level; batch->af = af; batch->v = v;
    if (const BackgroundSubmit submit = g_bg_submit.load(std::memory_order_acquire); submit && batch->items.size() >= 16) {
        for (int k = 0; k < 2; ++k) {
            { std::lock_guard<std::mutex> l(batch->m); ++batch->running; }
            const bool queued = submit([batch] {
                batch->run();
                { std::lock_guard<std::mutex> l(batch->m); --batch->running; }
                batch->cv.notify_all();
            });
            if (!queued) { std::lock_guard<std::mutex> l(batch->m); --batch->running; }
        }
    }
    batch->run(); // this thread takes items too
    {
        std::unique_lock<std::mutex> l(batch->m);
        while (batch->running > 0) {
            batch->cv.wait_for(l, std::chrono::seconds(1));
            if (g_process_exiting.load(std::memory_order_acquire)) return SIZE_MAX;
        }
    }
    g_mip_parallel_passes.fetch_add(batch->items.size() >= 16 && g_bg_submit.load(std::memory_order_relaxed) ? 1 : 0, std::memory_order_relaxed);
    return batch->stopped.load() ? SIZE_MAX : found;
}
void variant_worker()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); // compiles yield to the game's threads
    for (;;) {
        uint32_t v = 0;
        bool refill = false;
        {
            std::unique_lock<std::mutex> lk(g_var_m);
            while (!g_var_cv.wait_for(lk, std::chrono::seconds(1), [] {
                return g_var_want.load(std::memory_order_relaxed) != g_var_live.load(std::memory_order_relaxed) ||
                       g_var_missing.load(std::memory_order_relaxed);
            })) {
                if (g_process_exiting.load(std::memory_order_acquire)) return;
            }
            if (g_process_exiting.load(std::memory_order_acquire)) return;
            v = g_var_want.load(std::memory_order_relaxed);
            refill = v == g_var_live.load(std::memory_order_relaxed);
            g_var_missing.store(false, std::memory_order_relaxed);
        }
        bool aborted = false;
        if (v && g_mip_live_enabled.load(std::memory_order_relaxed)) {
            for (int pass = 0; pass < 6; ++pass) { // objects created during a pass are caught by the next
                const size_t found = fill_pass(var_level(v), var_af(v), v);
                if (found == SIZE_MAX) { aborted = true; break; }
                if (!found) break;
            }
        }
        if (aborted) continue;
        if (refill) {
            // Idle: warm the other selectable levels (see g_mip_prewarm) so a switch is immediate.
            // fill_pass gives up as soon as a new choice arrives; the loop above then serves it.
            if (g_mip_live_enabled.load(std::memory_order_relaxed) && g_mip_prewarm.load(std::memory_order_relaxed)) {
                const int af = var_af(v);
                const uint32_t levels = (0x7u | g_mip_level_mask.load(std::memory_order_relaxed)) & ~(af == 0 ? 1u : 0u);
                for (int level = 0; level < kMipCount; ++level) {
                    if (!(levels >> level & 1u) || level == var_level(v)) continue;
                    bool built_any = false, stop = false;
                    for (int pass = 0; pass < 6; ++pass) {
                        const size_t found = fill_pass(level, af, v);
                        if (found == SIZE_MAX || g_process_exiting.load(std::memory_order_acquire)) { stop = true; break; }
                        if (!found) break;
                        built_any = true;
                    }
                    if (stop) break;
                    if (built_any) g_mip_warmed.fetch_add(1, std::memory_order_relaxed);
                }
                // the in-use level's own counters for the menu/status line
                g_var_total.store(0, std::memory_order_relaxed);
                g_var_done.store(0, std::memory_order_relaxed);
            }
            // Rate-limit refill notifications, but let a user's new choice preempt immediately.
            std::unique_lock<std::mutex> lk(g_var_m);
            g_var_cv.wait_for(lk, std::chrono::seconds(1), [v] {
                return g_var_want.load(std::memory_order_relaxed) != v || g_process_exiting.load(std::memory_order_acquire);
            });
            continue;
        } // never spin on stragglers
        {
            std::lock_guard<std::mutex> lk(g_var_m);
            if (g_var_want.load(std::memory_order_relaxed) == v) {
                if (v && g_mip_live_enabled.load(std::memory_order_relaxed)) g_mip_switched.store(true, std::memory_order_release);
                const uint32_t was_live = g_var_live.load(std::memory_order_relaxed);
                g_var_live.store(v, std::memory_order_release);
                if (was_live != v)
                    na::eventlog::Emit("mip_af", "live bias=%.3f af=%u was{bias=%.3f af=%u} took_ms=%llu objects=%llu/%llu static_changed=%llu",
                                       kMipLevels[var_level(v)], kAfLevels[var_af(v)], kMipLevels[var_level(was_live)], kAfLevels[var_af(was_live)],
                                       (unsigned long long)(GetTickCount64() - g_var_request_tick.load(std::memory_order_relaxed)),
                                       (unsigned long long)g_var_done.load(std::memory_order_relaxed), (unsigned long long)g_var_total.load(std::memory_order_relaxed),
                                       (unsigned long long)g_var_static_changed[var_level(v) * 4 + var_af(v)].load(std::memory_order_relaxed));
                // Activation is itself an idle-work request. No subsequent game draw or
                // missing-object lookup is required to start warming the other choices.
                if (g_mip_prewarm.load(std::memory_order_relaxed)) g_var_missing.store(true, std::memory_order_relaxed);
            }
        }
        g_var_cv.notify_all();
    }
}
template <typename F>
void request_variant(F&& change)
{
    {
        std::lock_guard<std::mutex> lk(g_var_m);
        const uint32_t w = g_var_want.load(std::memory_order_relaxed);
        g_var_want.store(change(w), std::memory_order_relaxed);
        g_var_request_tick.store(GetTickCount64(), std::memory_order_relaxed);
        if (!g_var_started) {
            g_var_started = true;
            g_var_missing.store(true, std::memory_order_relaxed); // original/Off can warm alternatives too
            std::thread(variant_worker).detach();
        }
    }
    g_var_cv.notify_all();
}
} // namespace
void SetSamplerMipBias(float bias)
{
    const int level = nearest_mip_level(bias);
    {
        static na::eventlog::Edge e;
        int64_t was = 0;
        if (e.Changed(level, &was))
            na::eventlog::Emit("mip", "requested bias=%.3f level=%d asked=%.3f was_level=%lld", kMipLevels[level], level, bias, (long long)was);
    }
    g_mip_bias.store(kMipLevels[level], std::memory_order_relaxed); // sampler descriptors created from now on
    request_variant([level](uint32_t w) { return var_pack(level, var_af(w)); });
}
float SamplerMipBias() { return kMipLevels[var_level(g_var_want.load(std::memory_order_relaxed))]; }
void SetSamplerMipBiasLive(float bias) { SetSamplerMipBias(bias); }
void SetSamplerMipBiasLiveEnabled(bool on) { g_mip_live_enabled.store(on, std::memory_order_relaxed); }
void SetSamplerMipPrewarm(bool on) {
    if (g_mip_prewarm.exchange(on, std::memory_order_relaxed) || !on) return;
    g_var_missing.store(true, std::memory_order_relaxed);
    g_var_cv.notify_one();
}
void SetBackgroundSubmitter(BackgroundSubmit submit) { g_bg_submit.store(submit, std::memory_order_release); }
void SetSamplerMipLevelsWanted(const float* biases, int n)
{
    uint32_t mask = 0;
    for (int i = 0; i < n; ++i) mask |= 1u << nearest_mip_level(biases[i]);
    g_mip_level_mask.store(mask, std::memory_order_relaxed);
}
bool WaitSamplerVariant(uint32_t timeout_ms)
{
    std::unique_lock<std::mutex> lk(g_var_m);
    return g_var_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [] {
        return g_var_want.load(std::memory_order_relaxed) == g_var_live.load(std::memory_order_relaxed);
    });
}
MipLiveStats GetMipLiveStats()
{
    MipLiveStats m;
    const uint32_t want = g_var_want.load(std::memory_order_relaxed), live = g_var_live.load(std::memory_order_relaxed);
    m.enabled = g_mip_live_enabled.load(std::memory_order_relaxed);
    m.game_bias = 0.0f;
    m.live_bias = kMipLevels[var_level(live)];
    m.want_bias = kMipLevels[var_level(want)];
    m.want_aniso = kAfLevels[var_af(want)];
    m.live_aniso = kAfLevels[var_af(live)];
    m.preparing = want != live;
    m.prepare_done = g_var_done.load(std::memory_order_relaxed);
    m.prepare_total = g_var_total.load(std::memory_order_relaxed);
    m.live_static_changed = g_var_static_changed[var_level(live) * 4 + var_af(live)].load(std::memory_order_relaxed);
    m.twin_root_signatures = g_mip_twin_rs.load(std::memory_order_relaxed);
    m.twin_pipelines = g_mip_twin_pso.load(std::memory_order_relaxed);
    m.untwinned_pipelines = g_mip_untwinned.load(std::memory_order_relaxed);
    m.fallbacks = g_mip_fallbacks.load(std::memory_order_relaxed);
    m.bound_pso_twin = g_mip_bound_pso_twin.load(std::memory_order_relaxed);
    m.bound_pso_game = g_mip_bound_pso_game.load(std::memory_order_relaxed);
    m.bound_rs_twin = g_mip_bound_rs_twin.load(std::memory_order_relaxed);
    m.bound_rs_game = g_mip_bound_rs_game.load(std::memory_order_relaxed);
    m.twin_failures = g_mip_twin_fail.load(std::memory_order_relaxed);
    m.shared_skips = g_mip_shared_skips.load(std::memory_order_relaxed);
    m.warmed_levels = g_mip_warmed.load(std::memory_order_relaxed);
    m.parallel_passes = g_mip_parallel_passes.load(std::memory_order_relaxed);
    return m;
}
void NativeMipBiases(NativeBiasStats* s)
{
    if (!s) return;
    std::lock_guard<std::mutex> lock(g_native_m);
    s->negative = g_native_neg; s->zero = g_native_zero; s->positive = g_native_pos;
    s->min = g_native_min; s->max = g_native_max;
}
std::string FormatSamplerTable(uint64_t* total)
{
    if (total) *total = g_sampler_table_total.load(std::memory_order_relaxed);
    std::vector<SamplerRow> rows;
    {
        std::lock_guard<std::mutex> lk(g_sampler_table_m);
        rows = g_sampler_table;
    }
    std::sort(rows.begin(), rows.end(), [](const SamplerRow& a, const SamplerRow& b) { return a.count > b.count; });
    static const char* kinds[] = {"heap", "heap2", "static"};
    std::string out;
    char line[320];
    snprintf(line, sizeof line, "sampler_table distinct=%zu created=%llu overflow=%llu (kind filter aniso address bias lod cmp count)\r\n", rows.size(),
             (unsigned long long)g_sampler_table_total.load(std::memory_order_relaxed),
             (unsigned long long)g_sampler_table_overflow.load(std::memory_order_relaxed));
    out += line;
    for (const SamplerRow& r : rows) {
        snprintf(line, sizeof line, "sampler_row kind=%s filter=0x%X anisotropic=%u max_aniso=%u address=%u/%u/%u bias=%.3f lod=%.2f..%.2f cmp=%u count=%llu\r\n",
                 kinds[r.kind < 3 ? r.kind : 0], r.filter, D3D12_DECODE_IS_ANISOTROPIC_FILTER(D3D12_FILTER(r.filter)) ? 1u : 0u, r.aniso,
                 r.u, r.v, r.w, r.bias, r.min_lod, r.max_lod, r.cmp, (unsigned long long)r.count);
        out += line;
    }
    return out;
}
void MipBiasCounts(uint64_t* samplers, uint64_t* static_samplers)
{
    if (samplers) *samplers = g_biased_samplers.load(std::memory_order_relaxed);
    // Static samplers: changed in the game's objects (live switching off), or in the twins
    // of the variant in use (live switching on: the game's own objects are never changed).
    const uint32_t live = g_var_live.load(std::memory_order_relaxed);
    if (static_samplers)
        *static_samplers = g_biased_static.load(std::memory_order_relaxed) +
                           (var_level(live) ? g_var_static_changed[var_level(live) * 4 + var_af(live)].load(std::memory_order_relaxed) : 0);
}
void SetSamplerAnisotropy(uint32_t level)
{
    const int af = af_index(level);
    {
        static na::eventlog::Edge e;
        int64_t was = 0;
        if (e.Changed(af, &was))
            na::eventlog::Emit("af", "requested level=%u asked=%u was_index=%lld", kAfLevels[af], level, (long long)was);
    }
    g_aniso_level.store(kAfLevels[af], std::memory_order_relaxed); // sampler descriptors created from now on
    request_variant([af](uint32_t w) { return var_pack(var_level(w), af); });
}
void SetSamplerAnisotropyPrebuild(uint32_t level)
{
    g_pre_af.store(af_index(level), std::memory_order_relaxed);
    // Descriptor-heap samplers are created once, at load, long before the saved settings are
    // activated: give them the saved level from the start (as the static twins get it), or
    // a game whose materials use heap samplers never gets the filtering at all.
    g_aniso_level.store(kAfLevels[af_index(level)], std::memory_order_relaxed);
}
AnisoStats GetAnisoStats()
{
    AnisoStats a;
    const uint32_t live = g_var_live.load(std::memory_order_relaxed);
    a.level = g_mip_live_enabled.load(std::memory_order_relaxed) ? kAfLevels[var_af(live)] : g_aniso_level.load(std::memory_order_relaxed);
    a.samplers = g_aniso_samplers.load(std::memory_order_relaxed);
    a.static_samplers = g_aniso_static.load(std::memory_order_relaxed) +
                        (var_af(live) ? g_var_aniso_changed[var_level(live) * 4 + var_af(live)].load(std::memory_order_relaxed) : 0);
    a.already = g_aniso_already.load(std::memory_order_relaxed);
    {std::lock_guard<std::mutex> lock(g_native_m);a.native_surface=g_native_surface;a.native_16=g_native_af16;}
    a.sampler_calls = g_sampler_calls.load(std::memory_order_relaxed);
    a.sampler2_calls = g_sampler2_calls.load(std::memory_order_relaxed);
    return a;
}
HostWorkScope::HostWorkScope() : prev_(t_host_work) { t_host_work = true; }
HostWorkScope::~HostWorkScope() { t_host_work = prev_; }
void SetOperationSnapshots(bool enabled) { g_operation_snapshots.store(enabled); }
namespace {
bool shader_uses(const Obj& pipeline,const na_root_param& param,const na_root_range& range,uint32_t index){
    if(!pipeline.reflection_complete)return true;
    for(const auto& used:pipeline.shader_resources){
        if(param.visibility!=D3D12_SHADER_VISIBILITY_ALL&&used.visibility!=D3D12_SHADER_VISIBILITY_ALL&&param.visibility!=used.visibility)continue;
        if(used.type==range.range_type&&used.space==range.register_space&&index>=used.first&&
            (used.count==UINT_MAX||index-used.first<used.count))return true;}
    return false;
}
}
bool NativeDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle,OperationDescriptor& out) {
    out={};DescriptorDetails details;uint64_t version=0;
    {
        std::shared_lock<RegistryLock> lock(g_m);uint32_t slot=0;
        const auto* heap=heap_for_cpu_locked(handle.ptr,&slot);if(!heap)return false;
        const SlotShared stripe(heap,slot);
        if(!heap->info[slot].details)return false;
        details=*heap->info[slot].details;version=heap->versions[slot];
    }
    OperationDescriptor next;next.view=details;next.version=version;
    if(details.resource){next.resource.Attach(AcquireResource(details.resource));if(!next.resource)return false;}
    if(details.counter){next.counter.Attach(AcquireResource(details.counter));if(!next.counter)return false;}
    out=std::move(next);return true;
}
bool ListNativeOperationKey(ID3D12GraphicsCommandList* list,bool compute,const uint64_t* members,size_t count,NativeOperationKey& out){
    out={};OperationKey key;
    std::shared_lock<RegistryLock> lock(g_m);
    auto* ls=list?list_state_locked(list,&out.list):nullptr;
    if(!ls||!ls->recording_observed.load()||!ls->coverage_complete.load()||!ls->generation_known.load())return false;
    std::lock_guard<std::mutex> bl(ls->bind_m);
    if(!ls->restorable||ls->in_render_pass)return false;
    out.recording=ls->generation.load()+1;
    auto& cached=ls->operation_key[compute];bool rebase=false;
    const uint64_t revision_now=g_operation_revision.load(); // before reading: a concurrent copy then invalidates
    if(cached.valid&&cached.state_revision==ls->key_revision[compute]&&cached.registry_revision==revision_now&&
       count==cached.members.size()&&std::equal(cached.members.begin(),cached.members.end(),members)){
        key=cached.prefix;rebase=cached.rebase;
    }else{
    cached.valid=false;cached.dimension_cbvs=0;
    const auto rs=g_objs.find(canon(ls->root_sig[compute])),pipeline=g_objs.find(canon(ls->pso));
    if(rs==g_objs.end()||pipeline==g_objs.end()||rs->second.root_params.size()>64)return false;
    OperationKey::add(key.state,pipeline->second.id);OperationKey::add(key.state,compute);
    const auto root=reinterpret_cast<uintptr_t>(ls->root_sig[compute]);OperationKey::add(key.state,root);
    const auto& args=ls->args[compute];uint32_t descriptor_count=0,table_first[64]{};
    const auto member=[&](uint64_t id){return count&&std::binary_search(members,members+count,id);};
    for(uint32_t param=0;param<rs->second.root_params.size();++param){const auto& p=rs->second.root_params[param];
        if(p.param_type!=NA_ROOT_TABLE||param>=args.size()||args[param].type!=1)continue;
        if(p.first_range>rs->second.root_ranges.size()||p.count>rs->second.root_ranges.size()-p.first_range)return false;
        uint32_t span=0;bool sampler=false;
        for(uint32_t r=0;r<p.count;++r){const auto& range=rs->second.root_ranges[p.first_range+r];
            if(range.num_descriptors==UINT_MAX||range.table_offset==UINT_MAX||range.num_descriptors>4096||range.table_offset>4096-range.num_descriptors)return false;
            sampler|=range.range_type==D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;span=std::max(span,range.table_offset+range.num_descriptors);}
        if(sampler)continue;
        uint32_t slot=0;const auto* heap=bound_heap_for_gpu_locked(*ls,args[param].value,&slot);
        if(!heap||span>heap->count-slot||span>4096-descriptor_count)return false;
        table_first[param]=descriptor_count;
        for(uint32_t i=0;i<span;++i){bool used=!pipeline->second.reflection_complete;
            if(!used)for(uint32_t r=0;r<p.count;++r){const auto& range=rs->second.root_ranges[p.first_range+r];
                if(i>=range.table_offset&&i-range.table_offset<range.num_descriptors)used|=shader_uses(pipeline->second,p,range,range.base_register+i-range.table_offset);}
            std::shared_ptr<const DescriptorDetails> details;
            {const SlotShared stripe(heap,uint64_t(slot)+i);details=heap->info[slot+i].details;}
            if(used&&!details)return false;
            if(details){key.descriptor(*details,used);rebase|=used&&(member(details->resource)||member(details->counter));}
            else key.descriptor({},false);
        }
        descriptor_count+=span;
    }
    // Hash only named, used dimension bindings. Per-object/material CBVs and
    // native table addresses are not copied or retained by passthrough plans.
    for(const auto& field:pipeline->second.dimensions)for(uint32_t param=0;param<rs->second.root_params.size();++param){
        const auto& p=rs->second.root_params[param];uint32_t descriptor=UINT_MAX;uint64_t address=0;
        if(p.visibility!=D3D12_SHADER_VISIBILITY_ALL&&field.visibility!=D3D12_SHADER_VISIBILITY_ALL&&p.visibility!=field.visibility)continue;
        if(p.param_type==NA_ROOT_CBV&&p.shader_register==field.shader_register&&p.register_space==field.space)cached.dimension_cbvs|=uint64_t(1)<<param;
        if(param>=args.size())continue;
        if(p.param_type==NA_ROOT_CBV&&p.shader_register==field.shader_register&&p.register_space==field.space){
            if(args[param].type==2)address=args[param].value;}
        else if(p.param_type==NA_ROOT_TABLE&&args[param].type==1){
            uint32_t slot=0;const auto* heap=bound_heap_for_gpu_locked(*ls,args[param].value,&slot);if(!heap)return false;
            for(uint32_t r=0;r<p.count;++r){const auto& range=rs->second.root_ranges[p.first_range+r];
                if(range.range_type!=D3D12_DESCRIPTOR_RANGE_TYPE_CBV||range.register_space!=field.space||field.shader_register<range.base_register||field.shader_register-range.base_register>=range.num_descriptors)continue;
                const auto offset=range.table_offset+field.shader_register-range.base_register;
                std::shared_ptr<const DescriptorDetails> details;
                {const SlotShared stripe(heap,uint64_t(slot)+offset);details=heap->info[slot+offset].details;}
                if(!details)return false;address=details->cbv.BufferLocation;descriptor=table_first[param]+offset;}
        }
        uint64_t resource=0,offset=0;if(address&&resolve_va_locked(address,&resource,&offset)){key.constant(param,descriptor,resource,offset,field);rebase|=descriptor!=UINT_MAX;}
    }
    cached.prefix=key;cached.rebase=rebase;cached.state_revision=ls->key_revision[compute];cached.registry_revision=revision_now;
    cached.members.clear();if(count)cached.members.assign(members,members+count);cached.valid=true;
    }
    if(!compute){if(!ls->raster_tracking||!ls->raster_targets_known||!ls->raster_viewports_known||!ls->raster_scissors_known)return false;
        for(const auto& rt:ls->native_colors){if(!rt)return false;OperationKey::add(key.state,rt->resource);OperationKey::add(key.state,rt->rtv);}
        const DescriptorDetails empty{};const auto& depth=ls->native_depth?*ls->native_depth:empty;
        if(!ls->native_depth&&ls->raster_has_depth)return false;
        OperationKey::add(key.state,depth.resource);OperationKey::add(key.state,depth.dsv);
        for(UINT i=0;i<ls->raster_viewport_count;++i)OperationKey::add(key.state,ls->raster_viewports[i]);
        for(UINT i=0;i<ls->raster_scissor_count;++i)OperationKey::add(key.state,ls->raster_scissors[i]);}
    out.key=key.finish(rebase);return true;
}
bool ListNativeOperation(ID3D12GraphicsCommandList* list, bool compute, NativeOperation& out)
{
    out = {};
    NativeOperation next; next.compute = compute;
    {
        std::shared_lock<RegistryLock> lock(g_m);
        auto* ls = list ? list_state_locked(list, &next.list) : nullptr;
        if (!ls || !ls->recording_observed.load() || !ls->coverage_complete.load() || !ls->generation_known.load()) return false;
        std::lock_guard<std::mutex> bl(ls->bind_m);
        if (!ls->restorable || ls->in_render_pass) return false;
        next.recording = ls->generation.load() + 1; // same public identity as ListRecording
        auto rs = g_objs.find(canon(ls->root_sig[compute]));
        if (rs == g_objs.end() || !ls->pso || rs->second.root_params.size() > 64) return false;
        next.root = static_cast<ID3D12RootSignature*>(const_cast<void*>(ls->root_sig[compute]));
        next.pipeline = ls->pso;
        const auto pipeline = g_objs.find(canon(ls->pso));
        if (pipeline != g_objs.end()) next.pipeline_id = pipeline->second.id;
        next.reflection_complete=pipeline!=g_objs.end()&&pipeline->second.reflection_complete;
        const auto& args = ls->args[compute];
        for (uint32_t param = 0; param < rs->second.root_params.size(); ++param) {
            const auto& p = rs->second.root_params[param];
            if (p.param_type != NA_ROOT_TABLE || param >= args.size() || args[param].type != 1) continue;
            if (p.first_range > rs->second.root_ranges.size() || p.count > rs->second.root_ranges.size() - p.first_range) return false;
            uint32_t span = 0; bool sampler = false;
            for (uint32_t r = 0; r < p.count; ++r) {
                const auto& range = rs->second.root_ranges[p.first_range + r];
                if (range.num_descriptors == UINT_MAX || range.table_offset == UINT_MAX ||
                    range.num_descriptors > 4096 || range.table_offset > 4096 - range.num_descriptors) return false;
                sampler |= range.range_type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
                span = std::max(span, range.table_offset + range.num_descriptors);
            }
            if (sampler) continue;
            uint32_t slot = 0; const auto* heap = heap_for_gpu_locked(args[param].value, &slot);
            if (!heap || span > heap->count - slot || span > 4096 - next.descriptors.size()) return false;
            next.tables.push_back({param, static_cast<uint32_t>(next.descriptors.size()), span});
            SlotShared stripe;
            for (uint32_t i = 0; i < span; ++i) {
                stripe.lock(heap, uint64_t(slot) + i);
                const auto& info = heap->info[slot + i];
                OperationDescriptor d;d.used=!next.reflection_complete;
                if(next.reflection_complete)for(uint32_t r=0;r<p.count;++r){const auto& range=rs->second.root_ranges[p.first_range+r];
                    if(i>=range.table_offset&&i-range.table_offset<range.num_descriptors)
                        d.used|=shader_uses(pipeline->second,p,range,range.base_register+i-range.table_offset);}
                if (!info.details&&d.used) return false;
                if(info.details)d.view = *info.details; d.version = heap->versions[slot + i];
                next.descriptors.push_back(std::move(d));
            }
        }
        if (!compute) {
            if (!ls->raster_tracking || !ls->raster_targets_known || !ls->raster_viewports_known || !ls->raster_scissors_known) return false;
            for (const auto& c : ls->native_colors) {
                if (!c) return false;
                OperationDescriptor d; d.view = *c; next.colors.push_back(std::move(d));
            }
            if (ls->native_depth) next.depth.view = *ls->native_depth;
            else if (ls->raster_has_depth) return false;
            next.viewports.assign(ls->raster_viewports, ls->raster_viewports + ls->raster_viewport_count);
            next.scissors.assign(ls->raster_scissors, ls->raster_scissors + ls->raster_scissor_count);
        }
        if(pipeline!=g_objs.end())for(const auto& field:pipeline->second.dimensions) {
            for(uint32_t param=0;param<rs->second.root_params.size()&&param<args.size();++param) {
                const auto& p=rs->second.root_params[param];uint32_t descriptor=UINT_MAX;uint64_t address=0;
                if(p.visibility!=D3D12_SHADER_VISIBILITY_ALL&&field.visibility!=D3D12_SHADER_VISIBILITY_ALL&&p.visibility!=field.visibility)continue;
                if(p.param_type==NA_ROOT_CBV&&args[param].type==2&&p.shader_register==field.shader_register&&p.register_space==field.space)address=args[param].value;
                else if(p.param_type==NA_ROOT_TABLE&&args[param].type==1) {
                    const auto table=std::find_if(next.tables.begin(),next.tables.end(),[&](const auto& t){return t.param==param;});
                    if(table==next.tables.end())continue;
                    for(uint32_t r=0;r<p.count;++r){const auto& range=rs->second.root_ranges[p.first_range+r];
                        if(range.range_type!=D3D12_DESCRIPTOR_RANGE_TYPE_CBV||range.register_space!=field.space||
                           field.shader_register<range.base_register||field.shader_register-range.base_register>=range.num_descriptors)continue;
                        descriptor=table->first+range.table_offset+field.shader_register-range.base_register;
                        if(descriptor<next.descriptors.size())address=next.descriptors[descriptor].view.cbv.BufferLocation;
                    }
                }
                uint64_t resource=0,offset=0;if(!address||!resolve_va_locked(address,&resource,&offset))continue;
                auto c=std::find_if(next.constants.begin(),next.constants.end(),[&](const auto& x){return x.param==param&&x.descriptor==descriptor;});
                if(c==next.constants.end()){NativeOperation::Constant value;value.param=param;value.descriptor=descriptor;value.resource_id=resource;value.offset=offset;
                    next.constants.push_back(std::move(value));c=std::prev(next.constants.end());}
                c->bytes=std::max(c->bytes,field.bytes);c->dimensions.push_back(field);
            }
        }
    }
    const auto acquire = [](OperationDescriptor& d) {
        if (d.view.resource) { d.resource.Attach(AcquireResource(d.view.resource)); if (!d.resource) return false; }
        if (d.view.counter) { d.counter.Attach(AcquireResource(d.view.counter)); if (!d.counter) return false; }
        return true;
    };
    for (auto& d : next.descriptors) if (d.used&&!acquire(d)) return false;
    for (auto& d : next.colors) if (!acquire(d)) return false;
    if (next.depth.view.resource && !acquire(next.depth)) return false;
    for(auto& c:next.constants){c.resource.Attach(AcquireResource(c.resource_id));if(!c.resource)return false;}
    out = std::move(next); return true;
}
// Read-only: 2D textures whose recorded creation has one of `formats` and exactly w x h
// (Claude 2026-11-06, Cort's OK: J2 captures a game velocity candidate). Walks every known
// resource under the shared lock; call rarely (capture time), never per draw.
uint32_t FindTextures(const uint32_t* formats, uint32_t format_count, uint32_t w, uint32_t h, na_object_id* out, uint32_t max)
{
    if (!formats || !format_count || !out || !max) return 0;
    std::shared_lock<RegistryLock> lock(g_m);
    uint32_t n = 0;
    for (const auto& kv : g_res_keys) {
        uint32_t rw = 0, rh = 0, rf = 0;
        if (!resource_dims_locked(kv.first, &rw, &rh, &rf) || rw != w || rh != h) continue;
        bool match = false;
        for (uint32_t i = 0; i < format_count && !match; ++i) match = formats[i] == rf;
        if (!match) continue;
        out[n++] = kv.first;
        if (n == max) break;
    }
    return n;
}
ID3D12Resource* AcquireResource(na_object_id id)
{
    std::shared_lock<RegistryLock> lock(g_m);
    auto key = g_res_keys.find(id);
    if (key == g_res_keys.end()) return nullptr;
    ID3D12Resource* r = nullptr;
    static_cast<IUnknown*>(const_cast<void*>(key->second))->QueryInterface(IID_PPV_ARGS(&r));
    return r;
}
namespace {
// Depth and colour are one role: published and read together (two separate atomics let a
// reader see a new depth with the old colour, Astra 20cddb8).
// Readers are per draw on every recording thread, so they never take a lock: a sequence
// counter (odd while a writer is mid-update) lets them retry instead (seqlock). Writers stay
// serialised by g_live_m.
std::mutex g_live_m;
std::atomic<uint64_t> g_live_depth{0}, g_live_color{0}, g_live_seq{0};
uint64_t g_live_version = 0;
}
void SetLiveSelection(uint64_t depth, uint64_t color)
{
    std::lock_guard<std::mutex> lock(g_live_m);
    const uint64_t d0 = g_live_depth.load(std::memory_order_relaxed), c0 = g_live_color.load(std::memory_order_relaxed);
    if (depth == d0 && color == c0) return;
    ++g_live_version;
    g_live_seq.fetch_add(1, std::memory_order_acq_rel); // odd: update in progress
    g_live_depth.store(depth, std::memory_order_relaxed);
    g_live_color.store(color, std::memory_order_relaxed);
    g_live_seq.fetch_add(1, std::memory_order_release); // even: consistent
}
void GetLiveSelection(uint64_t* depth, uint64_t* color)
{
    for (;;) {
        const uint64_t s0 = g_live_seq.load(std::memory_order_acquire);
        if (s0 & 1) continue;
        const uint64_t d = g_live_depth.load(std::memory_order_acquire), c = g_live_color.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_live_seq.load(std::memory_order_relaxed) == s0) { *depth = d; *color = c; return; }
    }
}
void SetSceneViewportJitter(uint64_t frame, float x, float y)
{
    if (!frame) { g_vpj.store(0, std::memory_order_release); return; }
    const auto q = [](float f) { return static_cast<uint16_t>(static_cast<int16_t>(std::clamp(std::lround(double(f) * 65536.0), -32767L, 32767L))); };
    uint32_t f32 = static_cast<uint32_t>(frame);
    if (!f32) f32 = 1;
    g_vpj_ever.store(true, std::memory_order_relaxed);
    g_vpj.store(uint64_t(f32) << 32 | uint64_t(q(x)) << 16 | q(y), std::memory_order_release);
}
bool DecodeSceneViewportJitter(uint64_t packed, uint32_t* frame, float* x, float* y)
{
    if (!packed) return false;
    if (frame) *frame = static_cast<uint32_t>(packed >> 32);
    if (x) *x = vpj_axis(packed, 16);
    if (y) *y = vpj_axis(packed, 0);
    return true;
}
uint64_t ListSceneViewportJitter(ID3D12CommandList* list,uint64_t scene_depth)
{
    std::shared_lock<RegistryLock> lock(g_m);
    na_object_id id = 0;
    auto* ls = list ? list_state_locked(list, &id) : nullptr;
    if (!ls) return 0;
    std::lock_guard<std::mutex> bl(ls->bind_m);
    return !scene_depth||scene_depth==ls->vpj_depth?ls->vpj_scene:0;
}
void SetHookProfiling(bool enabled){g_cost_enabled.store(enabled,std::memory_order_relaxed);}
void FormatHookCost(char* out, size_t n)
{
    if (!out || !n) return;
    out[0] = 0;
    if(!g_cost_enabled.load(std::memory_order_relaxed)){std::snprintf(out,n,"hook_cost disabled");return;}
    cost_flush(); // this thread's share
    static uint64_t prev_ticks[kCostSlots]{}, prev_calls[kCostSlots]{}, prev_at = 0;
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    const uint64_t now = cost_now();
    const double secs = prev_at ? double(now - prev_at) / double(f.QuadPart) : 0.0;
    struct Row { uint32_t slot; double ms_per_s, calls_per_s, us_per_call; };
    Row rows[kCostSlots]; uint32_t nr = 0; double list_total = 0;
    for (uint32_t i = 0; i < kCostSlots; ++i) {
        const uint64_t t = g_cost_ticks[i].load(std::memory_order_relaxed), c = g_cost_calls[i].load(std::memory_order_relaxed);
        const uint64_t dt = t - prev_ticks[i], dc = c - prev_calls[i];
        prev_ticks[i] = t; prev_calls[i] = c;
        if (!dc || secs <= 0) continue;
        const double ms = double(dt) * 1000.0 / double(f.QuadPart);
        rows[nr++] = {i, ms / secs, double(dc) / secs, ms * 1000.0 / double(dc)};
        if (i < kCostPseudo) list_total += ms / secs;
    }
    prev_at = now;
    std::sort(rows, rows + nr, [](const Row& a, const Row& b) { return a.ms_per_s > b.ms_per_s; });
    int w = std::snprintf(out, n, "hook_cost window_s=%.1f list_ms_per_s=%.1f", secs, list_total);
    static const char* const kPseudo[] = {"vpj", "shader_use", "lane_before", "lane_after", "driver", "run", "root_table",
                                           "sampled", "emit", "note_draw", "canon_miss", "desc_copy", "desc_copy_wait",
                                           "desc_write", "desc_write_wait"};
    for (uint32_t i = 0; i < nr && i < 24 && w > 0 && size_t(w) < n; ++i) {
        const uint32_t sl = rows[i].slot;
        const char* tag = sl == kCostExec ? "exec" : sl == kCostPso ? "pso" :
                          (sl >= kCostPseudo && sl <= kCostDescWriteWait) ? kPseudo[sl - kCostPseudo] : nullptr;
        if (tag) w += std::snprintf(out + w, n - w, " %s{ms_per_s=%.1f calls_per_s=%.0f us=%.2f}", tag, rows[i].ms_per_s, rows[i].calls_per_s, rows[i].us_per_call);
        else w += std::snprintf(out + w, n - w, " s%u{ms_per_s=%.1f calls_per_s=%.0f us=%.2f}", rows[i].slot, rows[i].ms_per_s, rows[i].calls_per_s, rows[i].us_per_call);
    }
}
ViewportJitterStats GetViewportJitterStats()
{
    return {g_vpj_draws.load(std::memory_order_relaxed), g_vpj_sets.load(std::memory_order_relaxed),
            g_vpj_restores.load(std::memory_order_relaxed), g_vpj_unknown.load(std::memory_order_relaxed)};
}
uint64_t LiveSelectionVersion()
{
    std::lock_guard<std::mutex> lock(g_live_m);
    return g_live_version;
}

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
    std::shared_lock<RegistryLock> lock(g_m);
    return resolve_va_locked(va, resource, offset);
}

void SetExecuteForwarder(void(STDMETHODCALLTYPE* fwd)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*))
{
    g_exec_forward.store(fwd, std::memory_order_release);
}
bool HostExecuteAvailable(){return g_exec_forward.load(std::memory_order_acquire)||o_ExecuteCommandLists;}
bool ExecuteHostCommands(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList* const* lists){
    auto forward=g_exec_forward.load(std::memory_order_acquire);if(!forward)forward=o_ExecuteCommandLists;
    if(!queue||!forward||!count||!lists)return false;const HostWorkScope host;forward(queue,count,lists);return true;
}

void SetProcessExiting() { g_process_exiting.store(true, std::memory_order_release); }

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
    s.serialized_submissions = g_c.serialized.load();
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
        std::shared_lock<RegistryLock> lock(g_m);
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
