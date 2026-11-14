// Native DX11 route: real sub-pixel jitter through the game's viewports (the DX12
// host's method, observe/producers.cpp vpj_before_draw). A draw into the scene
// depth with depth test on gets the game's viewports moved by (x, y) render
// pixels (+x right, +y down); the rasterised content moves with them and DLSS is
// told exactly that value. No camera-matrix write, so it does not depend on
// knowing which matrix the game's vertex shader reads.
//
// Portable C++17 (Linux-tested); the D3D11 layer copies viewports in and out.
#pragma once

#include "core/temporal_jitter.hpp"
#include <array>
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
// Returns whether the border expansion was applied. Carry this with the executed
// draw, including deferred lists: it is part of the raster projection, not jitter.
inline bool JitterViewports(ViewportRect* v, uint32_t n, float dx, float dy, uint32_t target_w, uint32_t target_h)
{
    if (!v || !n) return false;
    const bool expanded = n == 1 && target_w && target_h && v[0].x == 0.f && v[0].y == 0.f &&
        v[0].w == static_cast<float>(target_w) && v[0].h == static_cast<float>(target_h);
    if (expanded) {
        v[0].x -= .5f;
        v[0].y -= .5f;
        v[0].w += 1.f;
        v[0].h += 1.f;
    }
    for (uint32_t i = 0; i < n; ++i) {
        v[i].x += dx;
        v[i].y += dy;
    }
    return expanded;
}

// VARIANT: jitter11/expanded-raster
// Column-vector VP in the original W x H viewport -> the same unjittered
// geometry as viewport (-.5, -.5, W+1, H+1). The centre is unchanged; clip X/Y
// scale, while depth and W do not. Apply BEFORE the camera tracker so its
// current, inverse and previous matrices all describe the actual raster grid,
// including the transition back to the original viewport when jitter turns off.
// The separately declared pixel jitter is not scaled or baked into this matrix.
inline std::array<double, 16> RasterViewProjection(std::array<double, 16> vp,
                                                  bool expanded, uint32_t w, uint32_t h)
{
    if (expanded && w && h) {
        const double sx = (double(w) + 1.0) / w, sy = (double(h) + 1.0) / h;
        for (uint32_t c = 0; c < 4; ++c) {
            vp[c] *= sx;
            vp[4 + c] *= sy;
        }
    }
    return vp;
}

// Native-resolution R2 now visits 16 positions before repeating. Halton retains
// its existing eight phases. Both stay inside the same [-.5, .5) pixel range.
inline neural_alias::JitterSample DlaaJitterSample(uint32_t phase, bool r2)
{
    return r2 ? neural_alias::temporal_jitter_r2(phase, 16)
              : neural_alias::temporal_jitter(phase, 8);
}

// Whether a draw is a scene draw for jitter: it writes the selected scene depth with depth test on.
inline bool WantsViewportJitter(bool active, uint64_t bound_depth, uint64_t scene_depth, bool depth_test, uint32_t viewports)
{
    return active && scene_depth && bound_depth == scene_depth && depth_test && viewports > 0;
}

} // namespace na::native11
