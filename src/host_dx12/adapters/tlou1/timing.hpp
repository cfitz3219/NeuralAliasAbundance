#pragma once
#include <cstdint>
namespace na::tlou1 {
// Enabled only for the inspected executable hash. Does not modify game data.
void InitializeTiming();
// Read the pinned engine fiber context; unrelated Present calls return 0.
uint64_t TimingRenderFrame();
bool TimingAttached();
struct TimingTrace { uint64_t seen=0, with_frame=0; uintptr_t callers[16]{}; uint32_t count=0; };
TimingTrace GetTimingTrace();
}
