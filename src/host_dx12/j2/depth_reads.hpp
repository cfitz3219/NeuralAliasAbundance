#pragma once
#include <d3d12.h>
#include <cstddef>
#include <cstdint>

// Depth reads: while the host supplies the raster jitter through the game's viewports, the game's
// own shader reads of the scene depth are made on a twin with the jitter taken back out
// (core/depth_unjitter.hpp says why: a game with no jitter of its own rebuilds a pixel's place in
// the world along the centre ray, and far away a ground mark or a lamp's pool of light then
// switches with the jitter sample). Depth tests, DLSS and every host reader keep the game's depth.
//
// Variant name in the log: depth-reads/twin (docs/VARIANTS.md).
namespace na::j2::depth_reads {
// Once per presented frame, on the presenting thread, as host work. `want` = viewport jitter is
// supplying samples and the setting is on; `scene_depth` = the selected scene depth (registry id,
// 0 = none); `queue` = the game's queue (views go to the twin only after that queue has finished a
// recording that brought the twin up to date).
void Tick(ID3D12CommandQueue* queue, uint64_t scene_depth, bool want);
// One line for the status log: what is on, what was done, and why not where it was not.
int Format(char* out, size_t n);
// Tests: drop everything (views are handed back to the game's depth first).
void Shutdown();
} // namespace na::j2::depth_reads
