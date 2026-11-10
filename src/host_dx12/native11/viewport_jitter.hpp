// Native DX11 route: real sub-pixel jitter through the game's viewports (the DX12
// host's method, observe/producers.cpp vpj_before_draw). A draw into the scene
// depth with depth test on gets the game's viewports moved by (x, y) render
// pixels (+x right, +y down); the rasterised content moves with them and DLSS is
// told exactly that value. No camera-matrix write, so it does not depend on
// knowing which matrix the game's vertex shader reads.
//
// Portable C++17 (Linux-tested); the D3D11 layer copies viewports in and out.
#pragma once

#include <cstdint>

namespace na::native11 {

struct ViewportRect {
    float x = 0, y = 0, w = 0, h = 0;
};

// Moves `n` viewports by (dx, dy) pixels. A single full-target viewport is first
// widened by half a pixel on each side: a fractional translation would otherwise
// truncate the right/bottom coverage and leave a row or column of clear pixels.
// The expansion is centred, so the declared jitter is unchanged; the depth
// attachment and the game's scissor still clip writes.
inline void JitterViewports(ViewportRect* v, uint32_t n, float dx, float dy, uint32_t target_w, uint32_t target_h)
{
    if (!v || !n) return;
    if (n == 1 && target_w && target_h && v[0].x == 0.f && v[0].y == 0.f && v[0].w == static_cast<float>(target_w) &&
        v[0].h == static_cast<float>(target_h)) {
        v[0].x -= .5f;
        v[0].y -= .5f;
        v[0].w += 1.f;
        v[0].h += 1.f;
    }
    for (uint32_t i = 0; i < n; ++i) {
        v[i].x += dx;
        v[i].y += dy;
    }
}

// Whether a draw is a scene draw for jitter: it writes the selected scene depth with depth test on.
inline bool WantsViewportJitter(bool active, uint64_t bound_depth, uint64_t scene_depth, bool depth_test, uint32_t viewports)
{
    return active && scene_depth && bound_depth == scene_depth && depth_test && viewports > 0;
}

} // namespace na::native11
