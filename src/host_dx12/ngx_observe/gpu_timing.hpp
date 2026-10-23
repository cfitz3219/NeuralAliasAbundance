// GPU cost of the DLSS evaluate, measured in-game (A23, completion fixed in
// C4/R-001): timestamp queries recorded on the game's own command list around
// the original evaluate, resolved into per-slot readback buffers, followed by
// a GPU-written completion ticket. Poll() reads a slot only when its ticket
// proves the list executed; no fences, no waits, no Present-count guessing.
// Distinguishes GPU time of the DLSS pass from the Present-cadence fps.
#pragma once

#include <d3d12.h>

#include <cstdint>

namespace na::gputime {

// The device comes from the overlay host (which owns a real device); until it
// is set, Begin/End are no-ops and the list pointer is never dereferenced
// (the NGX stub tests pass fake list pointers).
void SetDevice(ID3D12Device* device);

// Called around the original evaluate, on the game's list. Safe if the
// device refuses query heaps: the module disables itself and returns.
void Begin(ID3D12GraphicsCommandList* list);
void End(ID3D12GraphicsCommandList* list);

// From the Present hook: `queue` is the host's captured DIRECT queue and
// provides the timestamp frequency (DIRECT lists only are measured); reads
// slots whose completion ticket has landed. Cheap; call every frame.
void Poll(ID3D12CommandQueue* queue);

struct Stats {
    float last_ms;         // most recent completed evaluate
    float avg_ms;          // exponential average
    uint64_t samples;      // completed measurements
    uint32_t enabled;      // 0 if refused by the device, disabled, or every slot quarantined
    uint64_t skipped_busy; // Begin found no free slot (never stalls)
    uint64_t unsupported;  // non-DIRECT list, or no ID3D12GraphicsCommandList2 for the ticket
    uint64_t quarantined;  // slots retired after no completion evidence for kAbandonPresents
};
Stats GetStats();
void SetEnabled(bool enabled);

} // namespace na::gputime
