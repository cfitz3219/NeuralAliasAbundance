// Phase-1 frame capture for the NR design (docs/NR-INSERTION-DESIGN.md).
//
// Records, for N consecutive frames, the DLSS inputs/output as seen at the
// evaluate call (copied on the GAME's command list, transitioning around the
// documented NON_PIXEL_SHADER_RESOURCE / UAV states) and the finished back
// buffer at Present (copied on the direct queue). A fence signalled at
// Present covers both; a worker thread waits on it and writes raw files plus
// meta.json. Nothing here touches NGX types: callers pass plain resources.
#pragma once

#include <d3d12.h>
#include <dxgi.h>

#include <cstdint>

namespace na::capture {

struct EvalInputs {
    ID3D12Resource* color = nullptr;   // NPSR at call time (DLSS restores)
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motion = nullptr;
    ID3D12Resource* output = nullptr;  // UAV at call time
    uint32_t render_w = 0, render_h = 0, out_w = 0, out_h = 0;
    float jitter_x = 0, jitter_y = 0, mv_scale_x = 0, mv_scale_y = 0;
    uint32_t reset = 0;
};

// Arms a capture of `frames` consecutive frames into `out_dir` (created).
// A44: `delay_ms` postpones the first captured frame so the user can close
// the menu and move the camera first (Pending() stays false until then).
void Request(uint32_t frames, const wchar_t* out_dir, uint32_t delay_ms = 0);
bool Pending();
// Milliseconds until an armed capture starts (0 when none or already started).
uint32_t DelayRemainingMs();

// Inside the DLSS evaluate detour, after the original evaluate, with the
// game's (still open) command list. Records copies; returns false if this
// frame could not be captured (loss is counted, capture continues).
bool OnEvaluate(ID3D12GraphicsCommandList* list, const EvalInputs& in);

// At Present, before the original Present: `backbuffer` is in PRESENT state.
// Records the back-buffer copy on our own list, executes it on `queue`,
// signals the fence and hands the frame to the writer thread.
void OnPresent(ID3D12CommandQueue* queue, ID3D12Resource* backbuffer);

struct Stats {
    uint32_t frames_requested;
    uint32_t frames_evaluated;  // evaluate-side copies recorded
    uint32_t frames_written;    // files on disk
    uint32_t errors;
};
Stats GetStats();
void Shutdown(); // waits for the writer; releases everything

} // namespace na::capture
