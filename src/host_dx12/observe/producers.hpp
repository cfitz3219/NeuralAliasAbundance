#include "host_dx12/j2/insertion_trace.hpp"
// C8a: H1 observation PRODUCERS. host_observe is the transport (ABI 0.1,
// frozen); this module hooks the D3D12 device/queue methods that create and
// use the objects the ABI describes and emits its events:
//   DEVICE_CREATED, RESOURCE_CREATED, RESOURCE_HEAP_CREATED,
//   DESCRIPTOR_HEAP_CREATED, DESCRIPTOR_WRITE, DESCRIPTOR_COPY, LIST_CREATED,
//   QUEUE_CREATED, QUEUE_EXECUTE, QUEUE_SIGNAL, QUEUE_WAIT, FENCE_CREATED,
//   OBJECT_RETIRED, CAPABILITY_REPORT (snapshot) and the inventory replay.
//
// Identity: every observed object gets a monotonic na_object_id at creation.
// Retirement uses a private-data sentinel (SetPrivateDataInterface): the
// runtime releases it when the object is destroyed, which emits
// OBJECT_RETIRED and forgets the id. No Release hooks.
//
// Objects are tracked from installation on, whether or not a consumer has
// started; na_observe_start/resync replays the live inventory (creation
// events, FOUNDATIONAL) through the snapshot provider. Objects that existed
// before installation are unknown until first used (then PREEXISTING with
// NA_UNKNOWN_* creation fields, never invented).
//
// Descriptor handles are resolved to (heap, slot) through the heaps observed
// at creation; an unresolvable handle produces no write event and is counted.
// Constant-buffer views resolve their GPU virtual address through the buffer
// VA table; overlapping (aliased) ranges resolve to UNKNOWN, never a guess.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include <cstdint>
#include <memory>

#include "host_dx12/host_observe.h"
#include "host_dx12/observe/shader_operation.hpp"

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12CommandList;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
struct ID3D12RootSignature;
struct ID3D12DescriptorHeap;
struct D3D12_RESOURCE_BARRIER;
struct IDXGISwapChain;

namespace na::observe {

// Hooks the device and queue methods (shared by every object of the same
// implementation, so `device`/`queue` may be throwaway objects) and registers
// the snapshot provider. `hook_queue_execute`: the game host already owns the
// ExecuteCommandLists hook and passes false, calling ExecuteBegin/End from its
// detour instead. Idempotent; returns false if a required hook failed.
bool InstallProducers(ID3D12Device* device, ID3D12CommandQueue* queue, bool hook_queue_execute);
// Lightweight recording lifetime hook when the full graphics observer is off.
// Install-thread only; shares the Reset thunk with the full observer.
bool InstallListResetTracking(ID3D12Device* device, void (*listener)(ID3D12GraphicsCommandList*),
    void (*barriers)(ID3D12GraphicsCommandList*,unsigned int,const D3D12_RESOURCE_BARRIER*)=nullptr);

// Game host wiring: [observe] producers=1 requests installation; the overlay
// installs on its throwaway device/queue and then reports ProducersActive().
void RequestInstall(bool requested);
bool InstallRequested();
bool ProducersActive();

// ExecuteCommandLists observation for a host that owns the hook: Begin before
// the original call, End after it (R3 ambiguity is decided at End).
struct ExecuteToken {
    uint64_t ticket = 0;
    uint64_t overlap_gen = 0;
    bool overlapped_at_entry = false;
    // Retains bookkeeping through finalization; not a COM reference or GPU lifetime.
    std::shared_ptr<void> queue_state;
    na_object_id queue_id = 0;
    bool submit_locked = false; // C9B-R3: holds the queue's submission lock until End
    bool nested_unlocked = false; // this occurrence bypassed serialization (nested in a forwarded submission)
    // Evidence captured before forwarding (sampling + known-state store); opaque here.
    std::shared_ptr<void> capture;
    // The thread's previous current submission, restored at End (nesting).
    const void* prev_cap = nullptr;
    na_object_id prev_qid = 0;
    bool cur_set = false;
    uint32_t list_type = 0; // D3D12_COMMAND_LIST_TYPE of the queue
};
// C9b: the game host's original ExecuteCommandLists, used to submit the host's
// own sample copies without re-entering the host's execute detour.
void SetExecuteForwarder(void(__stdcall* fwd)(ID3D12CommandQueue*, unsigned int, ID3D12CommandList* const*));
// Reuse the established original forwarder for host preparation. Never recurse
// through the presenter/J2/NR execute detours; no waits or ownership inference.
bool HostExecuteAvailable();
bool ExecuteHostCommands(ID3D12CommandQueue*,uint32_t,ID3D12CommandList* const*);

// Begin also takes the C9 constant-data snapshots of the submitted recordings.
ExecuteToken ExecuteBegin(ID3D12CommandQueue* queue, uint32_t count = 0, ID3D12CommandList* const* lists = nullptr);
void ExecuteEnd(const ExecuteToken& token, ID3D12CommandQueue* queue, uint32_t count, ID3D12CommandList* const* lists);

// Actual presentation call boundary only; no queue, camera or frame ownership
// is inferred from the ordinal. The caller retains the chain through End.
struct PresentToken { na_object_id swapchain{}; uint64_t ordinal{}; uint32_t index{NA_UNKNOWN_U32}; };
PresentToken PresentBegin(IDXGISwapChain* chain);
void PresentEnd(const PresentToken& token, uint32_t sync, uint32_t flags, int32_t result);

// Tests: `probe` runs inside each snapshot copy while the copy holds the
// buffer's mapping (C9-H3-REVIEW-1 race regression). nullptr removes it.
void SetSnapshotCopyProbeForTests(void (*probe)(void* user), void* user);
// Tests: `probe` runs inside Unmap right before it contends for the mapping
// lock (explicit boundary handshake for the race regression).
void SetUnmapBoundaryProbeForTests(void (*probe)(void* user), void* user);
// Tests (C9B-REVIEW-2): `probe` runs after the game's submission returned and
// before armed samples are serviced (the R2 acquisition boundary); pausing
// stops the sample worker's maintenance ticks so scope checks at each
// operation are exercised without the worker masking the window.
void SetSampleAcquireProbeForTests(void (*probe)());
void SetSampleWorkerPausedForTests(bool paused);
// Tests: the sample worker thread fails to start (requests must be refused).
void SetSampleWorkerStartFailsForTests(bool fail);
// Tests: the host fence Signal after a submitted sample copy fails (quarantine path).
void SetSampleSignalFailsForTests(bool fail);

// Identity of an observed object (0 = unknown). Any interface pointer of it.
na_object_id IdOf(const void* object);

// J2 live consumer support (called from inside a game submission; bounded, no waits).
struct LiveBinding { na_object_id resource; uint64_t offset; uint32_t size; uint64_t op_index; uint32_t param = 0; bool table = false; };
std::vector<LiveBinding> ListRootConstantBuffers(ID3D12CommandList* list); // graphics CBVs of the recording: root CBVs and CBVs reached through descriptor tables
bool ListReferences(ID3D12CommandList* list, na_object_id id);            // the recording referenced the resource
// The CBVs currently bound for graphics (the latest binding of each root parameter / table slot), at
// record time; for a draw hook. Returns the count written (at most `max`).
uint32_t ListCurrentGraphicsCbvs(ID3D12CommandList* list, LiveBinding* out, uint32_t max);
// The textures the current graphics descriptor tables expose as SRVs, at record time (for a draw
// hook): distinct resource ids, at most `max`. Root SRVs and bindless heaps are not included.
// incomplete (optional): set when the answer may miss textures: a range wider than the 1024
// descriptors walked (incl. unbounded), `max` reached, or a table on an unknown heap (Astra
// 20cddb8 H1b point 2: never infer a unique source from an incomplete prefix).
uint32_t ListCurrentGraphicsSrvs(ID3D12CommandList* list, na_object_id* out, uint32_t max, bool* incomplete = nullptr);
// True when the recording bound `color` as a render target together with `depth` as its depth target.
bool ListRendersTo(ID3D12CommandList* list, na_object_id color, na_object_id depth);
// Inline insertion (J2). Called on the recording thread before a draw/dispatch uses
// bound SRVs (the resources listed), with no observer
// locks held; host work recorded into `list` from the callback must run under a
// HostWorkScope and end with RestoreListState(list).
using TableBindCallback = void (*)(ID3D12GraphicsCommandList* list, const na_object_id* srvs, uint32_t count, bool compute);
void SetTableBindCallback(TableBindCallback cb);
using EntryCallback = void (*)(ID3D12GraphicsCommandList*, const na::j2::InsertionEvent&);
void SetEntryCallback(EntryCallback cb);
na::j2::InsertionDecision ListInsertionDecision(ID3D12CommandList* list, na_object_id color);
// P4-0D consumer anchor: the first actual consumer of `color` after its final write in
// this recording (learned at submission), and the latest event as seen by a live trigger.
na::j2::ConsumerSignature ListConsumerSignature(ID3D12CommandList* list, na_object_id color);
na::j2::LiveConsumer ListLiveConsumer(ID3D12CommandList* list, na_object_id color);
// Placement design: the recording's writes and actual consumers of `color` in order (false:
// unobserved or truncated). ResetDerived clears the per-frame lineage set (J2, at Present).
bool ListColorEvents(ID3D12CommandList* list, na_object_id color, std::vector<na::j2::ColorEvent>& out);
void ResetDerived();
// Re-applies the game's descriptor heaps, root signatures, every root argument and the
// pipeline state of this recording. False (nothing issued) if the state is not fully
// known (unobserved recording, a bundle was executed).
bool RestoreListState(ID3D12GraphicsCommandList* list,ID3D12GraphicsCommandList* destination=nullptr,bool graphics_only=false,
    std::vector<uint64_t>* graphics_reads=nullptr);
// Scoped bindings change neither PSO/root signatures nor IA state. Restore only
// touched bindings, with bounded inline snapshots and no RootArg vector clones.
// Changing a resource heap invalidates tables in BOTH shader pipelines.
bool RestoreBindingState(ID3D12GraphicsCommandList*,bool compute,bool heaps,uint64_t cbvs,
    bool targets,bool viewports,bool scissors,ID3D12RootSignature* expected_root=nullptr);
// Layer2 raster work also changes RTV/DSV, viewport/scissor and IA topology.
// Opt-in: private CPU descriptor snapshots are taken before OMSetRenderTargets
// (the game's source descriptors may legally be reused immediately afterwards).
// Enable is latched at each observed Reset so Disable cannot interrupt a scope's
// restoration. Existing executable recordings keep their captured state.
void SetRasterStateTracking(bool enabled);
bool ListRasterStateRestorable(ID3D12CommandList* list);
bool ListStateRestorable(ID3D12CommandList* list); // RestoreListState would succeed
// Bounded snapshot of the current native shader bindings, for scoped private
// descriptor heaps. Rebind ALL resource tables when changing their heap, and
// preserve the sampler heap/tables. No descriptor memory is copied here.
struct ShaderState {
    uint32_t list_type=NA_UNKNOWN_U32;
    ID3D12RootSignature* root = nullptr;
    ID3D12DescriptorHeap* resources = nullptr;
    ID3D12DescriptorHeap* samplers = nullptr;
    uint64_t cbv_params = 0;
    bool raster_restorable=false;
    uint64_t target_serial=0,viewport_serial=0,scissor_serial=0,cbv_serial[64]{};
    uint64_t viewport_jitter=0; // exact sample actually bound before this operation
    uint64_t root_serial=0,restore_serial=0;
    struct Table { uint32_t param; uint64_t handle; bool sampler; } tables[64]{};
    uint32_t count = 0;
};
bool ListShaderState(ID3D12CommandList* list, bool compute, ShaderState& out);
// After a private viewport has been bound with this exact recording's sample,
// keep the jitter owner aware of it. Otherwise the next draw replaces the
// private viewport with the game's full-size one. No sample may be invented.
void PreserveAppliedViewportJitter(ID3D12CommandList*,uint64_t sample);
// First render target and depth currently bound in the recording.
bool ListCurrentTargets(ID3D12CommandList* list, na_object_id* rt, na_object_id* ds);
// The game's own scissor rectangles and depth target as last set in this recording
// (host work is not observed, so a host override never replaces them). False when the
// recording or its scissors are not known (then leave the list alone).
struct RasterScissors {
    uint64_t recording = 0, scissor_serial = 0;
    na_object_id ds = 0;
    uint32_t count = 0;
    struct Rect { int32_t left = 0, top = 0, right = 0, bottom = 0; } rects[16]{}; // D3D12_RECT layout
};
bool ListRasterScissors(ID3D12CommandList* list, RasterScissors& out);
// Called on the recording thread after the game's OMSetRenderTargets / RSSetScissorRects
// was forwarded and observed, with the list's new state; no observer locks held, inside
// HostWorkScope (commands it records are host work). Null = none (default).
using RasterStateListener = void (*)(ID3D12GraphicsCommandList* list, const RasterScissors& state);
void SetRasterStateListener(RasterStateListener listener);
// Native HUD (render-small): what the next draw of this recording will use, read in one
// lock: first render target and depth, the game's viewports and scissors (host overrides
// are never observed), and the bound pipeline's target formats and blending. False when the
// recording is unknown. `restorable` = RestoreBindingState can hand back targets, viewports
// and scissors (raster tracking was on at this recording's Reset).
struct DrawInfo {
    na_object_id list = 0; // object identity, independent of reused COM addresses/recording counts
    uint64_t recording = 0;
    na_object_id rt = 0, ds = 0;
    uint32_t rt_count = 0;
    bool restorable = false, pipeline_known = false;
    bool native_hud_affine = false; // RT0 RGB representable by the black/grey HUD replay
    // RT0 colour blend for the HUD un-blend: 0 unknown, 1 blending off, 2 linear in the
    // destination (reversible), 3 blending on but the destination is replaced (DestBlend ZERO),
    // 4 not reversible (MIN/MAX, logic op, destination-alpha factors).
    uint8_t blend_class = 0;
    bool shader_readonly = false, query_active = false; // replay metadata, independent of table-prefix guesses
    bool pixel_position=false;
    uint32_t shader_rejection=0;
    uint32_t pso_rt_count = 0, rtv_format = 0, dsv_format = 0, samples = 0;
    uint32_t blend_mask = NA_UNKNOWN_U32, depth_enable = NA_UNKNOWN_U32, depth_func = NA_UNKNOWN_U32;
    uint32_t viewport_count = 0, scissor_count = 0;
    float viewports[16][6]{};   // x, y, width, height, min depth, max depth
    int32_t scissors[16][4]{};  // left, top, right, bottom
};
bool ListDrawState(ID3D12CommandList* list, DrawInfo& out);
na_object_id ListPipelineId(ID3D12CommandList* list);
// Identity of the list's current recording (changes on every Reset); 0 if unknown.
uint64_t ListRecording(ID3D12CommandList* list);
// True when this recording cleared `target` with ClearRenderTargetView.
bool ListClearsTarget(ID3D12CommandList* list, na_object_id target);
// An explicit SRV-table binding referenced the texture. Positive ordering hint
// only: unobserved/bindless/late descriptor changes are not ruled out by false.
bool ListSamplesTexture(ID3D12CommandList* list, na_object_id texture);
// The subresources (mip + array slice; depth: plane 0) the last bind of `color`
// with `depth` in this recording rendered into, from the views' first
// subresources. NA_UNKNOWN_U32 = a view spanning several slices or unusual.
bool ListSceneSubresource(ID3D12CommandList* list, na_object_id color, na_object_id depth, uint32_t* sub, uint32_t* depth_sub);
// Distinct other render targets bound in this recording after its last bind of
// `color` with `depth` (later passes in the same list, e.g. post-processing that
// may consume the scene colour before a list-boundary insertion).
uint32_t ListTargetsAfter(ID3D12CommandList* list, na_object_id color, na_object_id depth);
bool ListLastState(ID3D12CommandList* list, na_object_id id, uint32_t* state); // last whole-resource transition in it
uint32_t ReadBuffer(na_object_id res, uint64_t offset, void* dst, uint32_t n); // CPU-visible buffer bytes (0 = unavailable)
// The state `id` (whole resource) carried into the submission this thread is
// forwarding right now, from the known-state store (earlier submissions /
// creation). False when not provable. Only for host work inside that forward.
bool KnownStateInCurrentSubmission(na_object_id id, uint32_t* state);
// Per-subresource variants (depth/stencil planes, mips and array slices transitioned
// separately): the state of subresource `sub` only.
bool KnownSubStateInCurrentSubmission(na_object_id id, uint32_t sub, uint32_t* state);
// The subresource's state after all work submitted so far (no submission context:
// for host work recorded during the game's recording). False when unknown/in flight.
bool KnownSubStateNow(na_object_id id, uint32_t sub, uint32_t* state);
// Recording-time consumer with an established queue: also require that this
// queue owns the carried state. A state from another queue is not an ordering edge.
bool KnownSubStateOnQueueNow(ID3D12CommandQueue* queue, na_object_id id, uint32_t sub, uint32_t* state);
void DescribeStateEvidence(ID3D12CommandList* list, na_object_id id, uint32_t sub, char* text, size_t capacity);
bool ListLastStateSub(ID3D12CommandList* list, na_object_id id, uint32_t sub, uint32_t* state);
enum class StateLookup { Absent, Known, Unavailable };
StateLookup ListStateSub(ID3D12CommandList* list, na_object_id id, uint32_t sub, uint32_t* state);
// Texture LOD bias for the game's mip-mapped surface samplers (sampler descriptors and
// root-signature static samplers); 0 = off. For J2 DLAA with jitter (temporal
// reconstruction needs sharper per-frame texture samples).
// Live sampler variants (Cort 2026-11-05: no restarts, and the saved choice must work from
// the first frame): with live switching on, the game's own root signatures and pipelines
// stay exactly as the game made them, and twins with the chosen mip bias and anisotropic
// filtering are bound instead. Twins for the saved choice are built when the game creates
// its objects; any other choice is built in the background from copies of the creation
// descriptions and switches over (all at once) when complete. With live switching off,
// objects created from now on are changed directly (the old behaviour).
void SetSamplerMipBias(float bias);     // the effective choice (saved-setting activation)
void SetSamplerMipBiasLive(float bias); // a menu change; same as SetSamplerMipBias
void SetSamplerMipBiasLiveEnabled(bool on); // before the game creates its pipelines
// Background warm-up of the other selectable levels once the chosen one is live ([j2] mip_prewarm).
void SetSamplerMipPrewarm(bool on);
// Pool for the variant pipelines' compiles (the host passes na::background::Submit). Unset: one thread.
using BackgroundSubmit = bool (*)(std::function<void()>);
void SetBackgroundSubmitter(BackgroundSubmit submit);
// The biases most likely to be chosen (the saved choice and the DLSS modes' automatic
// levels): their twins are built at creation, so switching to them is immediate.
void SetSamplerMipLevelsWanted(const float* biases, int n);
// Waits until the chosen variant is in use (tests); false on timeout.
bool WaitSamplerVariant(uint32_t timeout_ms);
struct MipLiveStats {
    bool enabled = false;
    float game_bias = 0, live_bias = 0;  // game_bias: the game's own objects (always unbiased now)
    float want_bias = 0;                 // chosen; live_bias is the one in use (differs while preparing)
    uint32_t want_aniso = 0, live_aniso = 0;
    bool preparing = false;
    uint64_t prepare_done = 0, prepare_total = 0; // objects of the last background preparation
    uint64_t live_static_changed = 0;   // static samplers changed in the twins of the variant in use
    uint64_t twin_root_signatures = 0, twin_pipelines = 0, untwinned_pipelines = 0, fallbacks = 0, twin_failures = 0;
    uint64_t parallel_passes = 0; // passes that shared their pipeline compiles with the background pool
    uint64_t warmed_levels = 0; // levels the background warm-up completed (switches to them are immediate)
    uint64_t shared_skips = 0; // twins D3D12 returned as one of the game's own root signatures (identical bytes): not kept
    // Binds at record time (summed per recording at its next Reset): a twin went to the driver,
    // or the game's own object was kept (sampler-independent, or the twin is identical).
    uint64_t bound_pso_twin = 0, bound_pso_game = 0, bound_rs_twin = 0, bound_rs_game = 0;
};
MipLiveStats GetMipLiveStats();
float SamplerMipBias();
void MipBiasCounts(uint64_t* samplers, uint64_t* static_samplers);
// Every distinct sampler the game created (heap, heap v2, root-signature static), most used first.
std::string FormatSamplerTable(uint64_t* total = nullptr);
// Anisotropic filtering (0 off, 4 / 8 / 16) on eligible standard linear samplers,
// including clamp-addressed materials. Live static variants; newly created sampler descriptors.
void SetSamplerAnisotropy(uint32_t level);
// The saved anisotropic level: twins for it are built at creation (before activation).
void SetSamplerAnisotropyPrebuild(uint32_t level);
struct AnisoStats {
    uint32_t level = 0;
    uint64_t samplers = 0, static_samplers = 0, already = 0;
    uint64_t native_surface=0,native_16=0; // original observed surface samplers, before our override
    uint64_t sampler_calls = 0, sampler2_calls = 0; // the game's CreateSampler / CreateSampler2 calls (all, eligible or not)
};
AnisoStats GetAnisoStats();
// The game's own MipLODBias on surface-texture samplers (recorded whether or
// not NeuralAlias biases): how many are negative / zero / positive, and range.
struct NativeBiasStats { uint64_t negative = 0, zero = 0, positive = 0; float min = 0.0f, max = 0.0f; };
void NativeMipBiases(NativeBiasStats* s);
// While alive on this thread, D3D12 objects created and commands recorded are the
// host's own (never observed as the game's), e.g. J2's inserted command list.
class HostWorkScope {
public:
    HostWorkScope();
    ~HostWorkScope();
    HostWorkScope(const HostWorkScope&) = delete;
    HostWorkScope& operator=(const HostWorkScope&) = delete;
private:
    bool prev_;
};
// Writes into a CPU-visible (upload) buffer of the game, e.g. J1 jittering the
// camera constants of a submission before it is forwarded. Returns bytes written.
uint32_t WriteBuffer(na_object_id res, uint64_t offset, const void* src, uint32_t n);
// Read-only lookup over recorded creations: 2D textures with one of `formats` and exactly w x h.
uint32_t FindTextures(const uint32_t* formats, uint32_t format_count, uint32_t w, uint32_t h, na_object_id* out, uint32_t max);
ID3D12Resource* AcquireResource(na_object_id id);                         // AddRef'd, or null
// Current discovery picks (written by the discovery adapter, read by J2).
void SetLiveSelection(uint64_t depth, uint64_t color);
void GetLiveSelection(uint64_t* depth, uint64_t* color);
// Changes whenever the published depth/colour pair changes.
uint64_t LiveSelectionVersion();
// Viewport jitter: real sub-pixel samples. While set (frame != 0), every draw whose bound depth
// is the live scene depth and whose pipeline tests depth is drawn with the game's viewports moved
// by (x, y) render pixels (+x right, +y down). Other draws get the game's viewports back.
void SetSceneViewportJitter(uint64_t frame, float x, float y);
// Draw-replay probe: per frame, the scene-depth draws and how many match a draw of the frame
// before (same pipeline, vertex shader, counts, vertex layout); of those, how many kept the same
// vertex/index buffers. untracked: vertex buffers unknown ([observe] raster tracking off).
struct ReplayProbeStats { uint32_t draws, prev_draws, unique, matched, same_buffers, moved_buffers, untracked, extra_stages, instanced; };
void SetReplayProbe(bool on);
// Draw replay (per-object motion): every qualifying scene draw is drawn again with a stream-output
// twin of its pipeline, writing each expanded vertex's clip position (float4) into this frame's
// slot buffer. Capture and current-frame consumption must share one recording.
// A submitted previous recording can supply history while a retired slot can be reused.
struct ReplayUse {
    virtual ~ReplayUse() = default;
    virtual bool Ready(ID3D12CommandQueue*) const = 0;
    virtual bool Retired() const = 0;
    // The GPU finished every submission of this recording (or it was discarded).
    // The game's list need not be reset: SE5 keeps lists for several frames.
    virtual bool Completed() const = 0;
};
using ReplayRetain = std::shared_ptr<ReplayUse> (*)(ID3D12GraphicsCommandList*);
struct ReplayRecord {
    uint64_t sig = 0, vb = 0;
    uint32_t first = 0, verts = 0, width = 0, height = 0;
    int32_t scissor[4]{};
    // 0: usable as is. Else the writer list was not submitted when J2 took the
    // frame: use only where stamps[check - 1] == token on the GPU.
    uint32_t check = 0, token = 0;
};
struct ReplayFrame {
    ID3D12Resource* cur = nullptr;  // this frame's positions (draws in cur_draws)
    ID3D12Resource* prev = nullptr; // the frame before (null when not continuous)
    uint64_t data_offset = 0; // positions start at data_offset (16 bytes each)
    bool cur_in_list = false; // cur is in STREAM_OUT in the reading list (it wrote some itself); else COMMON
    ID3D12Resource* stamps = nullptr; // uint per writer segment (COMMON); see ReplayRecord::check
    std::vector<ReplayRecord> cur_draws, prev_draws;
};
struct DrawReplayStats {
    uint64_t replayed = 0, vertices = 0, twin_queued = 0, twin_building = 0, twin_failed = 0, overflow = 0, topology = 0, stages = 0;
    uint64_t untwinned = 0, no_recipe = 0, not_ready = 0, built = 0, build_failed = 0, noted = 0, hash_mismatch = 0;
    // Why J2 could not use a frame: slot never claimed this frame, claimed by another list/recording, or already taken.
    uint64_t other_list = 0, take_unclaimed = 0, take_other_list = 0, take_sealed = 0, taken = 0;
    uint64_t segments = 0, pending_segments = 0; // list segments written; other-list segments not yet submitted at J2 and unusable
    uint64_t pending_stamped = 0, stamps = 0; // unsubmitted segments used under a GPU stamp check; stamps written at Close
    uint64_t reuse_unretired = 0; // slot reused while a writer list was not reset yet (could execute again)
};
void SetDrawReplay(uint64_t frame, ReplayRetain retain);
bool ReplayTakeFrame(ID3D12GraphicsCommandList* list, ID3D12CommandQueue* queue, ReplayFrame* f);
void GetDrawReplayStats(DrawReplayStats* out);
bool ReplayProbeFrame(ReplayProbeStats* out); // call once per frame (Present); swaps frames
bool DecodeSceneViewportJitter(uint64_t packed, uint32_t* frame, float* x, float* y);
// Packed value the last scene draw of this recording used (0 = none since Reset).
uint64_t ListSceneViewportJitter(ID3D12CommandList* list,uint64_t scene_depth=0);
struct ViewportJitterStats { uint64_t draws, sets, restores, unknown; };
ViewportJitterStats GetViewportJitterStats();
// Wall time inside each outermost command-list hook (ours + driver), ExecuteCommandLists
// ("exec") and pipeline creation ("pso") since the previous call: one status line,
// "hook_cost ... s<slot>{ms_per_s calls_per_s us}" (slot = ID3D12GraphicsCommandList vtable index).
void SetHookProfiling(bool enabled); // opt-in diagnostic timers, off in normal play
void FormatHookCost(char* out, size_t n);

// Buffer GPU VA -> (resource id, offset). False when unknown or ambiguous.
bool ResolveGpuVa(uint64_t va, na_object_id* resource, uint64_t* offset);

struct ProducerStats {
    uint64_t objects_live;
    uint64_t objects_created;
    uint64_t objects_retired;
    uint64_t descriptor_writes;
    uint64_t descriptor_copies;
    uint64_t unresolved_handles;   // descriptor handle outside every observed heap
    uint64_t sentinel_failures;    // SetPrivateDataInterface refused: no retirement tracking
    uint64_t executes;
    uint64_t ambiguous_executes;
    uint32_t hooks_installed;
    uint32_t hooks_missing;        // optional interfaces the device does not expose / refused slots
    const char* shared_target_slots; // slots refused because another hooked slot has the same function
    uint64_t samples_live;           // C9b entries incl. released ones awaiting GPU completion
    uint32_t sample_workers_started; // C9B-REVIEW-2 #1: at most 1 per process
    uint64_t sample_missing_recording, sample_partial_coverage, sample_partial_states, sample_no_transition;
    uint64_t nested_unlocked;        // ASTRA-P3-R8: queue ops nested in a forwarded submission (no lock taken)
    uint64_t serialized_submissions; // game submissions forwarded under their queue's lock for the known-state store
    uint64_t snapshots_reread;       // G3-02: app-unmapped upload buffers captured by a host read-only Map
    uint64_t preexisting_resources;  // G3-03: resources registered lazily on first sight
    uint64_t preexisting_heaps;      // G3-03: descriptor heaps registered lazily in SetDescriptorHeaps
    uint64_t sample_known_state;     // G3-06: copies whose state came from an earlier submission / creation
    uint64_t known_state_clears;     // G3-06: known-state store dropped by an incomplete submission
    // G3-06 review: why carried states may be unreadable right now.
    uint32_t known_store_disabled;   // an unobservable submission path was seen (unhooked/unregistered queue)
    uint32_t known_unknown_inflight; // incomplete submissions currently between Begin and End
    uint64_t known_entries;          // eligible textures in the store
    uint64_t unhooked_queues;        // queues whose table differs from the hooked one
};
ProducerStats GetProducerStats();
// Process exit: object retirement becomes a no-op (the CRT is about to destroy, or has
// destroyed, the tables it would update; the OS reclaims everything).
void SetProcessExiting();

} // namespace na::observe
