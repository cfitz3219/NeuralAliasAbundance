// J2: live DLAA for games with no upscaler of their own (first target: Anno 1800).
//
// Discovery (the adapter's UniversalHeuristics) publishes the scene depth and
// the scene colour drawn with it (na::observe::SetLiveSelection). At each game
// submission this module finds the LAST list in the batch that drew the scene
// (it references both), and inserts ONE NeuralAlias command list right after
// it in the same ExecuteCommandLists call. The game's own lists are never
// edited, so no game command-list state has to be restored. The inserted list:
//   1. copies depth plane 0 into an R32F texture (DLSS and the motion arbiter);
//   2. finds this frame's camera ViewProjection (with its inverse) in the
//      scene list's root constant buffers, tracks it (na::camera::CameraTracker:
//      cuts, previous frame);
//   3. builds camera motion vectors (na::motion::gpu::Arbiter, tier 2);
//   4. runs DLSS at native resolution (DLAA) on the scene colour, HDR;
//   5. copies the result back into the scene colour and restores every
//      resource to the state the game left it in.
// The game's later passes (tonemap, HUD) then read the reconstructed scene.
//
// Safety: off unless enabled; any missing evidence (selection, scene list,
// resource state, camera) skips the frame untouched with a reason; no CPU
// waits; a busy ring slot skips. Jitter injection (J1) is a separate step:
// without it DLSS gets jitter 0 (temporally stable, not super-sampled).
#pragma once
#include <d3d12.h>
#include <cstdint>

namespace na::j2 {

using ExecuteFn = void(__stdcall*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

void SetEnabled(bool on);
bool Enabled();

// Wraps the forward of a game ExecuteCommandLists (after observation began
// for it). Forwards the original batch unchanged unless a frame is processed.
void Execute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists, ExecuteFn forward);

struct Stats {
    uint64_t submissions = 0;     // batches seen while enabled
    uint64_t frames = 0;          // DLAA frames inserted
    uint64_t no_selection = 0;    // discovery has not chosen depth + colour yet
    uint64_t no_scene_list = 0;   // batch without the scene list
    uint64_t no_state = 0;        // resource state at the scene list's end not established
    uint64_t no_camera = 0;       // no camera ViewProjection + inverse in the scene list
    uint64_t busy = 0;            // ring slot / resize still in flight
    uint64_t dlss_failed = 0;     // DLSS refused the frame (reason in na::dlss::GetStats)
    uint64_t cuts = 0;            // camera cuts (history reset)
    uint64_t camera_offset = 0;   // byte offset of the camera matrix inside its constant block
    uint32_t width = 0, height = 0, color_format = 0;
    bool active = false;          // the last batch with the scene was processed
    char reason[160] = {};
};
Stats GetStats();

// Releases GPU objects (test hosts; the caller guarantees GPU idle).
void Shutdown();

} // namespace na::j2
