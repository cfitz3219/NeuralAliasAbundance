// The synthetic scene pictures of the flow fixtures, shared by tests/host/j2_compute_flow_test.cpp
// (real GPU, real AMD search) and tests/cpu/flow_fixture_pictures_test.cpp (CPU model of that
// search, runs in CI). Pure: (X, Y) are world pixel coordinates, the result is in 0..1.
#pragma once

#include <cmath>

namespace test_pictures {

// Four sine waves. It nearly repeats: a shift of about (-1, -94) pixels matches the picture almost
// as well as no shift. Fine for small pictures and sub-pixel work; at 1920x1080 a 6 pixel pan makes
// AMD's pyramid search lock onto that repeat on most of the frame (R3, Astra's audit 2026-11-16;
// measured in flow_fixture_pictures_test.cpp). Kept unchanged: other cases' numbers depend on it.
inline float RepeatingWaves(float X, float Y)
{
    return .5f + .18f * std::sin(.17f * X) + .14f * std::sin(.11f * X + .13f * Y) + .12f * std::cos(.07f * Y) +
           .05f * std::sin(.09f * X - .21f * Y);
}

// Smooth value noise in four sizes (96, 40, 17 and 7 pixel cells): detail at every scale of the
// search pyramid and no shift that matches except the true one, as in a game picture.
inline float NonRepeating(float X, float Y)
{
    const auto lattice = [](int ix, int iy, unsigned seed) {
        unsigned t = unsigned(ix) * 1664525u + unsigned(iy) * 1013904223u + seed * 747796405u;
        t ^= t >> 13; t *= 2246822519u; t ^= t >> 16; t *= 3266489917u; t ^= t >> 15;
        return float(t & 0xffffu) / 65535.f;
    };
    const auto noise = [&](float cell, unsigned seed) {
        const float u = X / cell, v = Y / cell, fu = std::floor(u), fv = std::floor(v);
        const int ix = int(fu), iy = int(fv);
        float a = u - fu, b = v - fv;
        a = a * a * (3.f - 2.f * a); b = b * b * (3.f - 2.f * b);
        return (lattice(ix, iy, seed) * (1.f - a) + lattice(ix + 1, iy, seed) * a) * (1.f - b) +
               (lattice(ix, iy + 1, seed) * (1.f - a) + lattice(ix + 1, iy + 1, seed) * a) * b;
    };
    return .5f + .34f * (noise(96.f, 1) - .5f) + .26f * (noise(40.f, 2) - .5f) + .2f * (noise(17.f, 3) - .5f) +
           .12f * (noise(7.f, 4) - .5f);
}

} // namespace test_pictures
