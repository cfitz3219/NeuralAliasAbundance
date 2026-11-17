// CPU model of the AMD FidelityFX optical-flow search as this program runs it
// (src/host_dx12/ffx_flow/ffx_flow.cpp: 7 pyramid levels, per level search -> filter -> scale),
// written from the pinned shader sources in third_party/ffx-opticalflow (MIT, see its LICENSE.txt):
//   ffx_opticalflow_compute_luminance_pyramid.h  box means of level 0, truncated on store
//   ffx_opticalflow_compute_optical_flow_v5.h    8x8 block, offsets -8..+7, least SAD, ties to the
//                                                smallest |dy| then |dx|; level 0 keeps (0,0) when
//                                                it is no worse
//   ffx_opticalflow_filter_optical_flow_v5.h     of the 3x3 neighbours, the one closest to the rest
//   ffx_opticalflow_scale_optical_flow_advanced_v5.h  of the 2x2 coarser neighbours, least 4x4 SAD
// Not modelled: the scene-change histogram (a detected cut zeroes the whole field).
//
// Why it exists: CI's software device cannot run the real search (no 32/64-lane wave operations),
// so nothing in CI could say whether a fixture picture is one the search can follow. On Astra's
// RTX 5070 Ti run of 2026-11-16 this model gave the camera fit's own counters for the 512x256
// fixture exactly (1017 samples, energy 3.1823) and within 1 % at 1920x1080.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace ffx_model {

struct Luma {
    int w = 0, h = 0;
    std::vector<uint8_t> p;
    int at(int x, int y) const { return p[size_t(std::clamp(y, 0, h - 1)) * size_t(w) + size_t(std::clamp(x, 0, w - 1))]; }
};
struct Vector { int x = 0, y = 0; };
struct Grid {
    int w = 0, h = 0;
    std::vector<Vector> v;
    Vector at(int x, int y) const { return x < 0 || y < 0 || x >= w || y >= h ? Vector{} : v[size_t(y) * size_t(w) + size_t(x)]; }
    void set(int x, int y, Vector a) { if (x >= 0 && y >= 0 && x < w && y < h) v[size_t(y) * size_t(w) + size_t(x)] = a; }
};

constexpr int kLevels = 7;

inline std::vector<Luma> Pyramid(const Luma& level0)
{
    std::vector<Luma> out(kLevels);
    out[0] = level0;
    for (int n = 1; n < kLevels; ++n) {
        Luma& m = out[size_t(n)];
        m.w = level0.w >> n; m.h = level0.h >> n;
        m.p.resize(size_t(m.w) * size_t(m.h));
        const int s = 1 << n;
        for (int y = 0; y < m.h; ++y)
            for (int x = 0; x < m.w; ++x) {
                unsigned sum = 0;
                for (int j = 0; j < s; ++j)
                    for (int i = 0; i < s; ++i) sum += level0.p[size_t(y * s + j) * size_t(level0.w) + size_t(x * s + i)];
                m.p[size_t(y) * size_t(m.w) + size_t(x)] = uint8_t(sum / unsigned(s * s));
            }
    }
    return out;
}

// For every 8x8 block of `current`, the whole-pixel vector to the same content in `previous`.
// `per_level` (optional) receives each level's filtered field, coarsest first.
inline Grid Flow(const Luma& current, const Luma& previous, std::vector<Grid>* per_level = nullptr)
{
    const auto pc = Pyramid(current), pp = Pyramid(previous);
    const auto blocks = [&](int n, int size) { const int scale = 8 << n; return (size + scale - 1) / scale; };
    Grid predicted, result;
    for (int n = kLevels - 1; n >= 0; --n) {
        const Luma& F = pc[size_t(n)];
        const Luma& S = pp[size_t(n)];
        const int gw = blocks(n, current.w), gh = blocks(n, current.h);
        Grid a;
        if (n == kLevels - 1) { a.w = gw; a.h = gh; a.v.assign(size_t(gw) * size_t(gh), Vector{}); }
        else a = predicted;
        const int groups_x = ((F.w + 3) / 4 * 16 + 63) / 64, groups_y = (F.h + 15) / 16;
        for (int cy = 0; cy < groups_y * 2; ++cy)
            for (int cx = 0; cx < groups_x * 2; ++cx) {
                const Vector start = a.at(cx, cy);
                const int px = cx * 8, py = cy * 8;
                uint32_t best = 0xffffffffu;
                for (int oy = 0; oy < 16; ++oy)
                    for (int ox = 0; ox < 16; ++ox) {
                        const uint32_t limit = best >> 16;
                        uint32_t sad = 0;
                        for (int dy = 0; dy < 8 && sad <= limit; ++dy)
                            for (int dx = 0; dx < 8; ++dx)
                                sad += uint32_t(std::abs(F.at(px + dx, py + dy) - S.at(px + start.x - 8 + ox + dx, py + start.y - 8 + oy + dy)));
                        if (sad > limit) continue; // cannot win: the SAD is the key's high bits
                        best = (std::min)(best, (sad << 16) | (uint32_t(std::abs(oy - 8)) << 12) | (uint32_t(std::abs(ox - 8)) << 8) |
                                                    (uint32_t(oy) << 4) | uint32_t(ox));
                    }
                Vector found{start.x + int(best & 0xfu) - 8, start.y + int((best >> 4) & 0xfu) - 8};
                if (n == 0) {
                    uint32_t still = 0;
                    for (int dy = 0; dy < 8; ++dy)
                        for (int dx = 0; dx < 8; ++dx) still += uint32_t(std::abs(F.at(px + dx, py + dy) - S.at(px + dx, py + dy)));
                    if (still <= (best >> 16)) found = Vector{};
                }
                a.set(cx, cy, found);
            }
        Grid b;
        b.w = gw; b.h = gh; b.v.resize(size_t(gw) * size_t(gh));
        for (int y = 0; y < gh; ++y)
            for (int x = 0; x < gw; ++x) {
                Vector t[9];
                int count = 0;
                for (int xx = -1; xx < 2; ++xx)
                    for (int yy = -1; yy < 2; ++yy) t[count++] = a.at(x + xx, y + yy);
                uint32_t chosen = 0xffffffffu;
                for (int i = 0; i < 9; ++i) {
                    uint32_t distance = 0;
                    for (int j = 0; j < 9; ++j) {
                        const int dx = t[i].x - t[j].x, dy = t[i].y - t[j].y;
                        distance += uint32_t(dx * dx + dy * dy);
                    }
                    chosen = (std::min)((distance << 4) | uint32_t(i), chosen);
                }
                b.v[size_t(y) * size_t(gw) + size_t(x)] = t[chosen & 0xfu];
            }
        if (per_level) per_level->push_back(b);
        if (n == 0) { result = b; break; }
        predicted.w = blocks(n - 1, current.w); predicted.h = blocks(n - 1, current.h);
        predicted.v.assign(size_t(predicted.w) * size_t(predicted.h), Vector{});
        for (int Y = 0; Y < predicted.h; ++Y)
            for (int X = 0; X < predicted.w; ++X) {
                uint32_t least = 0xffffffffu;
                Vector kept{};
                for (int z = 0; z < 4; ++z) {
                    const Vector candidate = b.at(X / 2 + (z % 2) - 1 + X % 2, Y / 2 + (z / 2) - 1 + Y % 2);
                    uint32_t sad = 0;
                    for (int r = 0; r < 4; ++r)
                        for (int i = 0; i < 4; ++i)
                            sad += uint32_t(std::abs(F.at(X * 4 + i, Y * 4 + r) - S.at(X * 4 + i + candidate.x, Y * 4 + r + candidate.y)));
                    if (sad < least) { least = sad; kept = candidate; }
                }
                predicted.v[size_t(Y) * size_t(predicted.w) + size_t(X)] = Vector{kept.x * 2, kept.y * 2};
            }
    }
    return result;
}

} // namespace ffx_model
