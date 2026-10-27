// J2 evidence capture (COMMS-Continuation, Opus; requested by Astra).
//
// Bounded, asynchronous, per-submission capture of what live DLAA actually ran on:
// J2's colour input, depth32, motion and DLSS output (copied in the SAME command list
// right after the evaluate, so GPU order makes them exactly this evaluation's data,
// whichever route recorded it), plus the image handed to Present.
//
// Ownership rules (Astra's request):
//   * one immutable ring slot per evaluation, with its own readback buffers and its own
//     allocator/list for the back-buffer copy: no shared allocator reset, no reuse of a
//     slot whose GPU work may still be pending;
//   * files are written by a worker thread only after the slot's fence values complete;
//     the render thread never waits on the GPU;
//   * an evaluation whose list was not submitted is discarded; if the list may still be
//     submitted later (inline: the copies live in the game's list) the slot is retired
//     for the rest of the session instead of being reused.
//
// Output: <dir>\NNNNN-{color,depth,motion,dlssout,present}.raw, NNNNN-meta.json and a
// one-line-per-frame index.csv. Raw = the readback footprint (row_pitch in meta).
// Separate from capture/frame_capture (native-NGX path), which is left unchanged.
#pragma once

#include <d3d12.h>

#include <cstdint>

namespace na::capture::j2 {

// Identity of one evaluation, as J2 knew it while recording.
struct Identity {
    uint64_t present_ordinal = 0;  // J2 frame counter (Present ordinal) at record time
    uint64_t submission = 0;       // J2 submission counter when recorded
    uint64_t recording = 0;        // producers' recording id of the list holding the copies (inline), else 0
    uint64_t view_color = 0, view_depth = 0; // selected scene colour / depth object ids
    uint32_t view_id = 0;          // DLSS view id
    uint32_t route = 0;            // 1 between, 2 inline (route_selector::Route)
    uint32_t route_mode = 0;       // 0 auto, 1 forced between, 2 forced inline
    uint32_t history_generation = 0;
    uint64_t insertion_signature = 0; // the boundary the inline trigger matched (writer pso, occurrence)
    uint32_t color_sub = 0;        // scene colour subresource copied
    uint32_t width = 0, height = 0, color_format = 0;
    float jitter_x = 0, jitter_y = 0; // pixels, as passed to DLSS
    float mv_scale_x = 1, mv_scale_y = 1;
    uint32_t reset = 0, camera_cut = 0;
    uint32_t preset = 0, sharpness = 0, debug_view = 0;
};

// Planes recorded after the evaluate, in the states they are in at that point.
struct Planes {
    ID3D12Resource* color = nullptr;  D3D12_RESOURCE_STATES color_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    ID3D12Resource* depth = nullptr;  D3D12_RESOURCE_STATES depth_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    ID3D12Resource* motion = nullptr; D3D12_RESOURCE_STATES motion_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    ID3D12Resource* output = nullptr; D3D12_RESOURCE_STATES output_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
};

using ExecuteFn = void(__stdcall*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

// Arms a capture of `frames` consecutive evaluations (bounded by kMaxFrames) into `dir`.
void Request(uint32_t frames, const wchar_t* dir, uint32_t delay_ms = 0);
// Same, into %LOCALAPPDATA%\NeuralAlias\captures\j2-<timestamp> (hotkey / menu / ini).
bool RequestTimestamped(uint32_t frames, uint32_t delay_ms = 0);
bool Armed(); // a capture is requested, its delay has passed, frames remain

// In J2's recording, right after a successful DLSS evaluate on `list`. Returns a token
// (0 = not captured: not armed, no free slot, allocation failed).
uint32_t RecordEvaluation(ID3D12GraphicsCommandList* list, const Planes& planes, const Identity& id);
// The list holding the token's copies was submitted on `queue` (signals the slot fence).
void Submitted(uint32_t token, ID3D12CommandQueue* queue, uint64_t submission);
// The token's list will not be submitted by J2. `may_still_run`: the copies are in a
// list someone else may still submit (inline) -> the slot is retired, never reused.
void Discard(uint32_t token, bool may_still_run);
// J2 closed a frame at Present: late scene writes observed after our insertion in it.
void OnFrameClosed(uint64_t present_ordinal, uint64_t late_writes);
// The image handed to Present (PRESENT state). Attached to this frame's submitted
// evaluation; copied on the slot's own list, executed with `exec` (the unhooked queue
// entry point, so J2 never observes it).
void OnPresent(ID3D12CommandQueue* queue, ID3D12Resource* presented, ExecuteFn exec);

struct Stats {
    uint32_t requested = 0, recorded = 0, submitted = 0, written = 0;
    uint32_t dropped_busy = 0, discarded = 0, retired = 0, errors = 0;
    uint32_t without_present = 0; // frames written without a presented image
};
Stats GetStats();
const wchar_t* Directory();
void Shutdown();

constexpr uint32_t kSlots = 8;
constexpr uint32_t kMaxFrames = 240;

} // namespace na::capture::j2
