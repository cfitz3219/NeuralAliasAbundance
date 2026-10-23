// GPU cost of the DLSS evaluate, measured in-game (A23): timestamp queries
// recorded on the game's own command list immediately around the original
// evaluate call, resolved into a small ring of readback buffers, and read a
// few Presents later (the game executes the list before it presents). No
// fences, no waits: a slot is only read once it is old enough, and a slot
// whose stamps are still zero is skipped. Distinguishes GPU time of the DLSS
// pass from the Present-cadence fps the footer shows.
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

// From the Present hook: `queue` provides the timestamp frequency; reads
// completed slots. Cheap; call every frame.
void Poll(ID3D12CommandQueue* queue);

struct Stats {
    float last_ms;      // most recent completed evaluate
    float avg_ms;       // exponential average
    uint64_t samples;   // completed measurements
    uint32_t enabled;   // 0 if the device/driver refused the queries
};
Stats GetStats();
void SetEnabled(bool enabled);

} // namespace na::gputime
