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
struct LiveBinding { na_object_id resource; uint64_t offset; uint32_t size; uint64_t op_index; };
std::vector<LiveBinding> ListRootConstantBuffers(ID3D12CommandList* list); // graphics root CBVs of the recording
bool ListReferences(ID3D12CommandList* list, na_object_id id);            // the recording referenced the resource
// True when the recording bound `color` as a render target together with `depth` as its depth target.
bool ListRendersTo(ID3D12CommandList* list, na_object_id color, na_object_id depth);
bool ListLastState(ID3D12CommandList* list, na_object_id id, uint32_t* state); // last whole-resource transition in it
uint32_t ReadBuffer(na_object_id res, uint64_t offset, void* dst, uint32_t n); // CPU-visible buffer bytes (0 = unavailable)
// The state `id` (whole resource) carried into the submission this thread is
// forwarding right now, from the known-state store (earlier submissions /
// creation). False when not provable. Only for host work inside that forward.
bool KnownStateInCurrentSubmission(na_object_id id, uint32_t* state);
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
