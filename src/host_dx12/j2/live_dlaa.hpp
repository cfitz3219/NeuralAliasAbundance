// J2: live DLAA for games with no upscaler of their own (first target: Anno 1800).
//
// Discovery (the adapter's UniversalHeuristics) publishes the scene depth and
// the scene colour drawn with it (na::observe::SetLiveSelection). At each game
// submission this module finds the LAST list in the batch that drew the scene
// (it binds that colour as a render target with that depth), and inserts ONE NeuralAlias command list right after
// it in the same ExecuteCommandLists call. The game's own lists are never
// edited, so no game command-list state has to be restored. At most one batch
// receives DLAA between game Presents; scene-boundary selection remains the
// caller's existing integration policy. The inserted list:
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

void SetEnabled(bool on);   // from the INI at start-up
bool Enabled();
bool Configured();          // enabled at start-up (the overlay shows its section)
void SetPaused(bool paused); // the overlay's on/off switch (history resets on resume)
bool Paused();
// Debug view (replaces the DLAA result): 0 off, 1 motion vectors, 2 fine motion
// (sub-pixel), 3 depth.
void SetDebugView(int mode);
// Test hook: records the RCAS sharpening pass from `in` (NON_PIXEL_SHADER_RESOURCE)
// into `out` (UNORDERED_ACCESS). percent 0 records nothing and returns false.
bool RecordRcasForTests(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* in, ID3D12Resource* out,
                        uint32_t w, uint32_t h, uint32_t percent, DXGI_FORMAT fmt);
// Test hook: the 101..200 % stage (NVIDIA Image Scaling sharpen-only) from `in`
// (NON_PIXEL_SHADER_RESOURCE) into `out` (UNORDERED_ACCESS). <= 100 % records nothing.
bool RecordNisForTests(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* in, ID3D12Resource* out,
                       uint32_t w, uint32_t h, uint32_t percent, DXGI_FORMAT fmt, bool hdr);
int DebugView();
void SetJitter(bool on);     // the overlay's camera-jitter switch (J1), on by default
bool Jitter();

// Called once at the game view's non-TEST Present boundary, including while
// paused. Presenter-generated frames must not call this. Single-view J2 uses
// its execution queue; other queues do not advance its camera history.
void OnPresent(ID3D12CommandQueue* queue);

// Wraps the forward of a game ExecuteCommandLists (after observation began
// for it). Forwards the original batch unchanged unless a frame is processed.
void Execute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists, ExecuteFn forward);

struct Stats {
    uint64_t inline_presents = 0, inline_covered = 0, inline_late_writes = 0;
    uint64_t inline_jittered = 0;
    float inline_jitter_x = 0, inline_jitter_y = 0;
    char insertion_last_miss[256] = {};
    char insertion_frame_trace[2048] = {};
    uint64_t submissions = 0;     // batches seen while enabled
    uint64_t frames = 0;          // DLAA frames inserted
    uint64_t duplicate_submissions = 0; // later batches in an already processed game frame
    uint64_t no_selection = 0;    // discovery has not chosen depth + colour yet
    uint64_t no_scene_list = 0;   // batch without the scene list
    uint64_t no_state = 0;        // resource state at the scene list's end not established
    uint64_t no_state_color = 0, no_state_depth = 0; // which one was unknown
    uint64_t no_camera = 0;       // no camera ViewProjection + inverse in the scene list
    uint64_t busy = 0;            // ring slot / resize still in flight
    uint64_t dlss_failed = 0;     // DLSS refused the frame (reason in na::dlss::GetStats)
    uint64_t cuts = 0;            // camera cuts (history reset)
    uint64_t camera_offset = 0;   // byte offset of the camera matrix inside its constant block
    uint32_t later_passes = 0;
    uint64_t sharpened_frames = 0; // RCAS applied to the DLAA result
    uint64_t nis_frames = 0;       // NIS added on top (Sharpness above 100 %)
    uint64_t jitter_writes = 0;   // J1: camera matrices replaced by jittered copies
    uint64_t camera_learned_hits = 0, camera_scans = 0; // camera found at the learned slot / by a full search
    int64_t camera_inv_delta = 0; // bytes from the ViewProjection to its inverse in the camera block
    int64_t camera_proj_delta = 0; // bytes to the camera's separate projection matrix (0 = none found)
    uint64_t jitter_proj_writes = 0; // J1: projection matrices replaced by jittered copies
    // J1 coverage: lists drawn after this frame's DLAA that still use the jittered
    // camera / the scene depth (unresolved shake), and camera-slot blocks holding a
    // different camera (not jittered).
    uint64_t post_camera_lists = 0, post_depth_lists = 0, other_camera_blocks = 0;
    // Multi-submission scenes: scene batches in the last frame (and the most seen),
    // batches skipped because a later scene batch was expected, and scene batches
    // that still came after our DLAA (insertion too early).
    uint32_t scene_batches = 0, scene_batches_max = 0, scene_batches_target = 0;
    uint64_t waited_batches = 0, post_scene_batches = 0;
    // Inline insertion (post-processing recorded in the scene's own list).
    uint32_t inline_mode = 0, inline_target = 0;
    uint64_t insertion_list = 0, insertion_destination = 0, insertion_pipeline = 0;
    uint32_t insertion_reads = 0, insertion_final = 0, insertion_writes = 0;
    uint64_t insertion_traces = 0, insertion_changes = 0;
    uint64_t inline_candidates = 0, inline_waited = 0, inline_recorded = 0, inline_frames = 0, inline_refused = 0;
    bool depth_reversed = false;  // reverse-Z as classified from the camera    // other render targets bound after the scene in the same list (insertion may be late)
    uint32_t width = 0, height = 0, color_format = 0;
    // Shape of the selected resources (reported even when J2 refuses them).
    uint32_t color_sub = 0, color_mips = 0, color_slices = 0, color_samples = 0, depth_slices = 0, depth_samples = 0;
    bool active = false;          // the last batch with the scene was processed
    char reason[160] = {};
};
Stats GetStats();

// Releases GPU objects (test hosts; the caller guarantees GPU idle).
void Shutdown();

} // namespace na::j2
