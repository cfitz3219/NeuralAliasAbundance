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

#include <cstdint>

#include "host_dx12/host_observe.h"

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12CommandList;
struct ID3D12Resource;

namespace na::observe {

// Hooks the device and queue methods (shared by every object of the same
// implementation, so `device`/`queue` may be throwaway objects) and registers
// the snapshot provider. `hook_queue_execute`: the game host already owns the
// ExecuteCommandLists hook and passes false, calling ExecuteBegin/End from its
// detour instead. Idempotent; returns false if a required hook failed.
bool InstallProducers(ID3D12Device* device, ID3D12CommandQueue* queue, bool hook_queue_execute);

// ExecuteCommandLists observation for a host that owns the hook: Begin before
// the original call, End after it (R3 ambiguity is decided at End).
struct ExecuteToken {
    uint64_t ticket = 0;
    uint64_t overlap_gen = 0;
    bool overlapped_at_entry = false;
    void* queue_state = nullptr;
};
ExecuteToken ExecuteBegin(ID3D12CommandQueue* queue);
void ExecuteEnd(const ExecuteToken& token, ID3D12CommandQueue* queue, uint32_t count, ID3D12CommandList* const* lists);

// Identity of an observed object (0 = unknown). Any interface pointer of it.
na_object_id IdOf(const void* object);

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
    uint32_t hooks_missing;        // optional interfaces the device does not expose
};
ProducerStats GetProducerStats();

} // namespace na::observe
