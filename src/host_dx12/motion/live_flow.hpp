// A52: live hardware optical flow at Present â€” the Tier-3 raw-flow supplier
// running inside the game (M-001 pipeline, "not done: live wiring" in A36).
//
// Every presented frame is converted to the display-encoded BGRA8 image the
// NVIDIA optical-flow engine (NVOFA, driver nvofapi64.dll) expects (PQ HDR10
// back buffers are decoded to SDR at the paper white the NR pass uses; SDR
// back buffers are re-encoded as BGRA8), the engine computes the flow from
// the CURRENT frame to the PREVIOUS one asynchronously, and FlowExpand turns
// the grid result into the arbiter's full-resolution RG32F raw flow
// (pixels, p_previous âˆ’ p_current) plus, when a raw-depth texture is
// supplied, a validity texture. The latest result is published with the
// fence value that completes it; consumers wait on that fence.
//
// Rules kept: no waits on the render thread (a frame whose ring slot is
// still busy is skipped and counted); the engine's teardown rule (A36) is
// honoured on Shutdown; inert unless [motion] enabled=1; a failure latches
// the supplier off with a plain-language reason, like the NR pass.
// Cost: grid 4 / perf 5 measured â‰ˆ 4 ms wall at 3440Ã—1440 (A36); the
// engine work is serialised behind the frame on the direct queue, so this
// is an EXPERIMENTAL, off-by-default feature until measured in a game.
#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdint>

namespace na::motion {

// Optional raw-depth source (R32_FLOAT, full display resolution, state
// NON_PIXEL_SHADER_RESOURCE at Present, caller-owned) for depth-aware
// expansion; null = plain bilinear expansion (bleeds at depth edges).
void SetDepthSource(ID3D12Resource* depth_full_res_r32f, float depth_clear);

// A57: inputs seen at the game's DLSS evaluate (DLSS titles only): the
// game's depth and its OWN motion vectors. Copied on the game's list like
// the NR pass does; at Present the depth is upsampled (nearest) for
// depth-aware expansion and the motion vectors (bilinear, scaled to display
// pixels) become the GROUND TRUTH the hardware flow is compared against:
// per-frame mean endpoint error and the share of pixels within 1 px,
// accumulated on the GPU and read back without waiting. This is the live
// accuracy measurement for the universal motion source, recorded in the
// status log; nothing consumes the flow in a DLSS game.
struct EvalRef {
    ID3D12Resource* depth = nullptr;   // NPSR at call time (plane 0 copied)
    ID3D12Resource* motion = nullptr;  // NPSR at call time
    uint32_t render_w = 0, render_h = 0;
    float mv_scale_x = 0, mv_scale_y = 0; // value * scale = render pixels (DLSS convention)
    uint32_t depth_inverted = 0;
};
bool WantsInputs(); // enabled and not latched: the evaluate detour should call OnEvaluate
void OnEvaluate(ID3D12GraphicsCommandList* list, const EvalRef& in);

// At Present, before the original Present (back buffer in PRESENT state).
void OnPresent(ID3D12CommandQueue* queue, ID3D12Resource* backbuffer, DXGI_COLOR_SPACE_TYPE color_space);

struct Output {
    ID3D12Resource* flow_rg32f;     // full resolution, NON_PIXEL_SHADER_RESOURCE once `fence` >= value
    ID3D12Resource* validity_r32f;  // null without a depth source
    ID3D12Fence* fence;
    uint64_t value;
    uint64_t frame;                 // presents processed so far (0 = none)
    uint32_t width, height, grid;
};
// The most recent completed-or-pending result; false until one exists.
bool Latest(Output* out);

struct Stats {
    uint32_t enabled;       // setting
    uint32_t active;        // produced a result on the last processed frame
    uint32_t latched_off;   // failed; reason set
    uint32_t hdr;           // input decoded from PQ
    uint32_t available;     // NVOFA present on this device (after the first attempt)
    uint64_t frames;        // results published
    uint64_t skipped;       // presents not processed (ring busy / first frame / disabled)
    float flow_ms;          // engine wall time of the last completed flow (CPU-observed, fence to fence)
    float avg_ms;
    uint32_t width, height, grid, perf;
    // A57 ground-truth comparison (DLSS titles): last completed frame.
    uint32_t ref_available;     // 1 when the game's motion vectors are being compared
    float ref_mean_err_px;      // mean endpoint error |flow - game MV| over compared pixels
    float ref_within_1px_pct;   // share of compared pixels within 1 px
    float ref_mean_mag_px;      // mean |game MV| (how much motion there was)
    uint64_t ref_frames;        // comparisons completed
    char reason[96];
};
Stats GetStats();
void ResetLatch();
void Shutdown(); // waits for in-flight work (bounded), honours the engine teardown rule

} // namespace na::motion
