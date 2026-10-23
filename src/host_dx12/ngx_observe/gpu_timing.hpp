// GPU cost of the DLSS evaluate, measured in-game (A23; completion fixed in
// C4 for R-001 and Gate 1 R-202): timestamp queries recorded on the game's own
// command list around the original evaluate and resolved into a per-slot
// readback buffer. When the host's ExecuteCommandLists detour sees that list
// submitted, it signals the host's fence on that same queue; Poll() reads a
// slot only after GetCompletedValue() proves that signal passed. No CPU waits,
// no Present-count guessing. Distinguishes the DLSS pass's GPU time from the
// Present-cadence fps.
#pragma once

#include <d3d12.h>

#include <cstdint>

namespace na::gputime {

// The device comes from the overlay host (which owns a real device); until it
// is set, Begin/End are no-ops and the list pointer is never dereferenced
// (the NGX stub tests pass fake list pointers).
void SetDevice(ID3D12Device* device);

// Called around the original evaluate, on the game's list. Safe if the
// device refuses query heaps/fences: the module disables itself and returns.
void Begin(ID3D12GraphicsCommandList* list);
void End(ID3D12GraphicsCommandList* list);

// From the host's ExecuteCommandLists detour, AFTER the original call returned
// (game thread; no waits): associates measured lists with the queue that
// executed them, signals the completion fence there, records its frequency.
void OnExecuted(ID3D12CommandQueue* queue, uint32_t count, ID3D12CommandList* const* lists);

// From the Present hook: reads slots whose submission fence has completed.
// The queue argument is unused (kept for the existing call site). Cheap.
void Poll(ID3D12CommandQueue* queue);

struct Stats {
    float last_ms;         // most recent completed evaluate
    float avg_ms;          // exponential average
    uint64_t samples;      // completed measurements
    uint32_t enabled;      // 0 if refused by the device, disabled, device removed, or every slot quarantined
    uint64_t skipped_busy; // Begin found no free slot (never stalls)
    uint64_t unsupported;  // list type without timestamp support here (bundle/copy/video)
    uint64_t quarantined;  // slots retired: never seen submitted, or not completed within kAbandonPresents
};
Stats GetStats();
void SetEnabled(bool enabled);

} // namespace na::gputime
