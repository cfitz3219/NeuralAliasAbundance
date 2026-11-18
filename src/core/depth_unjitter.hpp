// Scene depth with the raster jitter taken back out, for the GAME'S OWN shader reads of it.
//
// Why (Cort's Sniper Elite Resistance run on 7933efc, 2026-11-18: a ground mark and a lamp's pool
// of light switched on and off with the jitter sample, far away only): viewport jitter moves the
// scene's geometry, and so its depth, by a fraction of a pixel. A game that does not jitter by
// itself does not know that. Its lighting, decal and shadow passes take a pixel's depth and rebuild
// the place in the world along the ray through the pixel's CENTRE, while the depth was measured
// along the nudged ray. On a surface the two differ by j . grad(depth); in distance from the camera
// that is about j * z^2 / (f * h) for ground seen from height h with a lens of f pixels, so
// centimetres nearby and metres far away. A thin decal volume, the edge of a light, a shadow bias:
// each is a threshold on the rebuilt place, and a whole patch crosses it with the jitter.
//
// What this gives: for a jittered depth picture D_j, where D_j(p) = D(p - j) (the picture moved by
// +j pixels, +x right, +y down, |j| < 1), the depth the same surface has at the pixel's centre,
//   D'(p) = D_j(p) + j . grad D_j(p).
// Depth as the depth buffer stores it is a straight line across the screen on any flat surface, so
// on a flat surface this is exact, not an approximation. The jittered depth itself is left alone:
// depth tests and the DLAA input keep using it.
//
// The slope, one axis at a time:
//  - on the side the nudge points to, when the surface continues there (the next two samples lie
//    on one line with this one): then the answer lies BETWEEN two samples of the same surface;
//  - else on the other side, when the surface continues there: the surface's line carried on past
//    its edge, which is the depth this pixel's own colour and normal belong to;
//  - else none: the depth stays as it is on that axis (something a pixel or two wide; no worse
//    than without this).
// "Continues" = the second difference is small against the first (`curve`), or below what the
// number format can tell apart (`noise`, a fraction of the depth's size). Past the picture's border
// nothing continues: a repeated border sample would look like a surface with no slope.
// Known limit: where two surfaces meet at an angle (a wall's foot) the one line of pixels on the
// fold can be judged as continuing onto the other surface; the error there is at most
// |j| * curve of the local slope, against |j| of it with no correction.
//
// The card's program (host_dx12/scale/depth_unjitter.cpp) is kept in step with this by hand.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace na::depth::unjitter {

struct Params {
    float curve = 0.25f;              // second difference allowed, as a share of the first
    float noise = 1.0f / 1048576.0f;  // ... or as a share of the depth's own size (8 float steps)
};

// The slope of depth per +1 pixel at sample `c`, from the samples at -2, -1, +1, +2 on one axis.
// `j` is the nudge on that axis; its sign picks the side that is tried first. `minus_inside` /
// `plus_inside`: both samples of that side lie inside the picture.
inline float Slope(float m2, float m1, float c, float p1, float p2, float j, const Params& p = {}, bool minus_inside = true,
    bool plus_inside = true)
{
    if (j == 0.0f) return 0.0f;
    const float floor_ = p.noise * (std::max)({std::fabs(c), std::fabs(m1), std::fabs(p1)});
    const auto continues = [&](float n1, float n2) {
        const float first = n1 - c, second = c - 2.0f * n1 + n2;
        return std::fabs(second) <= (std::max)(p.curve * std::fabs(first), floor_);
    };
    const bool plus = plus_inside && continues(p1, p2), minus = minus_inside && continues(m1, m2);
    const float g_plus = p1 - c, g_minus = c - m1;
    if (j > 0.0f) return plus ? g_plus : minus ? g_minus : 0.0f;
    return minus ? g_minus : plus ? g_plus : 0.0f;
}

// One pixel of a w x h row-major depth picture.
inline float Pixel(const float* d, int w, int h, int x, int y, float jx, float jy, const Params& p = {})
{
    const auto at = [&](int sx, int sy) {
        return d[static_cast<size_t>((std::clamp)(sy, 0, h - 1)) * w + (std::clamp)(sx, 0, w - 1)];
    };
    const float c = at(x, y);
    const float gx = Slope(at(x - 2, y), at(x - 1, y), c, at(x + 1, y), at(x + 2, y), jx, p, x >= 2, x + 2 < w);
    const float gy = Slope(at(x, y - 2), at(x, y - 1), c, at(x, y + 1), at(x, y + 2), jy, p, y >= 2, y + 2 < h);
    return (std::clamp)(c + jx * gx + jy * gy, 0.0f, 1.0f);
}

} // namespace na::depth::unjitter
