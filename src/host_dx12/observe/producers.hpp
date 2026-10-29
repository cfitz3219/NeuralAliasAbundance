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
#include <vector>

#include <cstdint>
#include <memory>

#include "host_dx12/host_observe.h"

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12CommandList;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
struct IDXGISwapChain;

namespace na::observe {

// Hooks the device and queue methods (shared by every object of the same
// implementation, so `device`/`queue` may be throwaway objects) and registers
// the snapshot provider. `hook_queue_execute`: the game host already owns the
// ExecuteCommandLists hook and passes false, calling ExecuteBegin/End from its
// detour instead. Idempotent; returns false if a required hook failed.
bool InstallProducers(ID3D12Device* device, ID3D12CommandQueue* queue, bool hook_queue_execute);

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
// True when the recording bound `color` as a render target together with `depth` as its depth target.
bool ListRendersTo(ID3D12CommandList* list, na_object_id color, na_object_id depth);
// Inline insertion (J2). Called on the recording thread before a draw/dispatch uses
// bound SRVs (the resources listed), with no observer
// locks held; host work recorded into `list` from the callback must run under a
// HostWorkScope and end with RestoreListState(list).
using TableBindCallback = void (*)(ID3D12GraphicsCommandList* list, const na_object_id* srvs, uint32_t count, bool compute);
void SetTableBindCallback(TableBindCallback cb);
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
bool RestoreListState(ID3D12GraphicsCommandList* list);
bool ListStateRestorable(ID3D12CommandList* list); // RestoreListState would succeed
// First render target and depth currently bound in the recording.
bool ListCurrentTargets(ID3D12CommandList* list, na_object_id* rt, na_object_id* ds);
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
void DescribeStateEvidence(ID3D12CommandList* list, na_object_id id, uint32_t sub, char* text, size_t capacity);
bool ListLastStateSub(ID3D12CommandList* list, na_object_id id, uint32_t sub, uint32_t* state);
enum class StateLookup { Absent, Known, Unavailable };
StateLookup ListStateSub(ID3D12CommandList* list, na_object_id id, uint32_t sub, uint32_t* state);
// Texture LOD bias added to the game's mip-mapped samplers (sampler descriptors
// and root-signature static samplers) created from now on; 0 = off. For J2 DLAA
// with jitter (temporal reconstruction needs sharper per-frame texture samples).
void SetSamplerMipBias(float bias);
// Live mip bias (no restart): the game's root signatures and pipelines get twins at
// creation for the other menu levels (0, -0.5, -1); each command-list recording binds the
// twins of the level selected when it was Reset. SamplerMipBias() reports that level.
void SetSamplerMipBiasLive(float bias);
void SetSamplerMipBiasLiveEnabled(bool on); // before the game creates its pipelines
struct MipLiveStats {
    bool enabled = false;
    float game_bias = 0, live_bias = 0;
    uint64_t twin_root_signatures = 0, twin_pipelines = 0, untwinned_pipelines = 0, fallbacks = 0, twin_failures = 0;
};
MipLiveStats GetMipLiveStats();
float SamplerMipBias();
void MipBiasCounts(uint64_t* samplers, uint64_t* static_samplers);
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
ID3D12Resource* AcquireResource(na_object_id id);                         // AddRef'd, or null
// Current discovery picks (written by the discovery adapter, read by J2).
void SetLiveSelection(uint64_t depth, uint64_t color);
void GetLiveSelection(uint64_t* depth, uint64_t* color);

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

} // namespace na::observe
