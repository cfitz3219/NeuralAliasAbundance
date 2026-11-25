#pragma once

// Turns the Measure pass's 128 x 72 grid of cell sums into the numbers the governor steers by and
// the scene's feel (docs/NA1-ENHANCER.md section 3). The same code reads the card's readback and
// the processor reference's cells, so a replayed capture and a live game give the same numbers.

#include "core/na1_reference.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace na::enhancer {

struct Numbers {
    bool valid = false;
    float texture = 1, broad = 1;
    float colour_before = 0, colour_after = 0, variety_before = 0, variety_after = 0;
    float dark_before = 0, dark_after = 0, white_before = 0, white_after = 0;  // percent of all pixels
    float glow = 0, key_shift = 0;                                              // stops
    // Scene feel, from the before picture.
    float m0 = 0.18f, key_stops = 0, w_dark = 0, range = 0, w_range = 0, mid = 0, p85 = 0.5f, p97 = 0.9f;
    float tint[3] = {1, 1, 1};
    float mean_y = 0;
    uint32_t solid_cells = 0;
};

constexpr float kBroadSigma = 3.f;      // cells
constexpr float kBandLeast = 200.f;     // samples the sky band needs before its glow is believed

// The value below which `percent` of the sorted list falls, with a straight line between
// neighbours. `v` is reordered.
inline float Percentile(std::vector<float>& v, float percent)
{
    if (v.empty()) return 0.f;
    std::sort(v.begin(), v.end());
    const float at = (std::clamp)(percent, 0.f, 100.f) / 100.f * float(v.size() - 1);
    const size_t i = size_t(at);
    const size_t j = (std::min)(i + 1, v.size() - 1);
    return v[i] + (v[j] - v[i]) * (at - float(i));
}

// Spread of the thumbnail's broad shapes: T minus its Gaussian blur (sigma 3 cells), over the
// cells in `use`. Cells with no samples take no part in the blur.
inline float BroadSpread(const std::vector<float>& t, const std::vector<unsigned char>& filled, const std::vector<unsigned char>& use)
{
    using na::na1::ref::kCellsH;
    using na::na1::ref::kCellsW;
    constexpr int radius = 9;
    float kernel[radius + 1];
    for (int i = 0; i <= radius; ++i) kernel[i] = std::exp(-0.5f * float(i * i) / (kBroadSigma * kBroadSigma));
    const int cw = int(kCellsW), ch = int(kCellsH);
    std::vector<float> num(t.size()), den(t.size());
    for (int y = 0; y < ch; ++y)
        for (int x = 0; x < cw; ++x) {
            float n = 0, d = 0;
            for (int o = -radius; o <= radius; ++o) {
                const size_t i = size_t(y) * kCellsW + size_t((std::clamp)(x + o, 0, cw - 1));
                if (!filled[i]) continue;
                const float k = kernel[o < 0 ? -o : o];
                n += k * t[i];
                d += k;
            }
            num[size_t(y) * kCellsW + size_t(x)] = n;
            den[size_t(y) * kCellsW + size_t(x)] = d;
        }
    double sum = 0, sum2 = 0;
    uint32_t count = 0;
    for (int y = 0; y < ch; ++y)
        for (int x = 0; x < cw; ++x) {
            const size_t c = size_t(y) * kCellsW + size_t(x);
            if (!use[c]) continue;
            float n = 0, d = 0;
            for (int o = -radius; o <= radius; ++o) {
                const size_t i = size_t((std::clamp)(y + o, 0, ch - 1)) * kCellsW + size_t(x);
                const float k = kernel[o < 0 ? -o : o];
                n += k * num[i];
                d += k * den[i];
            }
            if (!(d > 0)) continue;
            const double v = double(t[c]) - double(n / d);
            sum += v;
            sum2 += v * v;
            ++count;
        }
    if (count < 2) return 0.f;
    const double mean = sum / count;
    return float(std::sqrt((std::max)(sum2 / count - mean * mean, 0.0)));
}

// cells: kCellsW*kCellsH*kCellFloats floats, laid out as na::na1::ref::Cell.
// curve_is_measured: false when the display curve is only assumed; the scene is then treated as
// ordinary (w_dark forced to 0), because an assumed curve cannot tell a dark scene from a dim one.
inline Numbers Summarise(const float* cells, bool curve_is_measured)
{
    using namespace na::na1::ref;
    Numbers n;
    if (!cells) return n;
    const uint32_t count = kCellsW * kCellsH;
    double total[kCellFloats] = {};
    for (uint32_t i = 0; i < count; ++i)
        for (uint32_t k = 0; k < kCellFloats; ++k) {
            const float v = cells[size_t(i) * kCellFloats + k];
            if (!std::isfinite(v)) return n;  // a readback that holds a non-number tells us nothing
            total[k] += v;
        }
    if (!(total[kCount] > 0)) return n;
    const double all = total[kCount], solid = total[kSolid];

    n.dark_before = float(100.0 * total[kBlackBefore] / all);
    n.dark_after = float(100.0 * total[kBlackAfter] / all);
    n.white_before = float(100.0 * total[kWhiteBefore] / all);
    n.white_after = float(100.0 * total[kWhiteAfter] / all);
    n.mean_y = float(total[kYBefore] / all);
    n.m0 = float(total[kShownBefore] / all);
    n.key_stops = n.m0 > 0 ? -std::log2(n.m0) : 20.f;
    n.w_dark = curve_is_measured ? (std::clamp)((n.key_stops - 2.6f) / 1.4f, 0.f, 1.f) : 0.f;
    n.key_shift = total[kShownBefore] > 0 && total[kShownAfter] > 0 ? float(std::log2(total[kShownAfter] / total[kShownBefore])) : 0.f;
    n.glow = total[kBand] >= kBandLeast ? float(total[kBandGlow] / total[kBand]) : 0.f;

    if (solid > 0) {
        n.colour_before = float(total[kStrengthBefore] / solid);
        n.colour_after = float(total[kStrengthAfter] / solid);
        n.variety_before = float(total[kVarietyBefore] / solid);
        n.variety_after = float(total[kVarietyAfter] / solid);
        const double mean_b = total[kFineBefore] / solid, mean_a = total[kFineAfter] / solid;
        const double var_b = total[kFineBefore2] / solid - mean_b * mean_b, var_a = total[kFineAfter2] / solid - mean_a * mean_a;
        // A picture with no fine contrast at all has nothing to compare: say "unchanged".
        n.texture = var_b > 1e-12 ? float(std::sqrt((std::max)(var_a, 0.0) / var_b)) : 1.f;
    }

    // Per-cell means: the thumbnails and the solid cells' own statistics.
    std::vector<float> thumb_b(count), thumb_a(count), shown, solid_y, chroma[3];
    std::vector<unsigned char> filled(count), mostly_solid(count);
    shown.reserve(count);
    solid_y.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const float* c = cells + size_t(i) * kCellFloats;
        if (!(c[kCount] > 0)) continue;
        filled[i] = 1;
        thumb_b[i] = c[kYBefore] / c[kCount];
        thumb_a[i] = c[kYAfter] / c[kCount];
        shown.push_back(c[kShownBefore] / c[kCount]);
        if (!(c[kSolid] > 0.5f * c[kCount])) continue;
        mostly_solid[i] = 1;
        ++n.solid_cells;
        solid_y.push_back(c[kSolidY] / c[kSolid]);
        const float lum = Lum(c[kSolidR], c[kSolidG], c[kSolidB]);  // of the sums: the scale cancels
        if (lum > 0)
            for (int k = 0; k < 3; ++k) chroma[k].push_back(c[kSolidR + k] / lum);
    }
    n.p85 = Percentile(shown, 85.f);
    n.p97 = Percentile(shown, 97.f);
    if (!solid_y.empty()) {
        n.range = Percentile(solid_y, 95.f) - Percentile(solid_y, 5.f);
        n.mid = Percentile(solid_y, 50.f);
    }
    n.w_range = (std::clamp)((n.range - 4.f) / 1.f, 0.f, 1.f);
    if (!chroma[0].empty()) {
        float t[3];
        for (int k = 0; k < 3; ++k) t[k] = Percentile(chroma[k], 50.f);
        const float scale = t[0] * 0.2126f + t[1] * 0.7152f + t[2] * 0.0722f;
        if (scale > 1e-6f)
            for (int k = 0; k < 3; ++k) n.tint[k] = t[k] / scale;
    }
    const float spread_b = BroadSpread(thumb_b, filled, mostly_solid), spread_a = BroadSpread(thumb_a, filled, mostly_solid);
    n.broad = spread_b > 1e-6f ? spread_a / spread_b : 1.f;

    n.valid = std::isfinite(n.texture) && std::isfinite(n.broad) && std::isfinite(n.key_shift) && std::isfinite(n.m0) &&
              std::isfinite(n.mid) && std::isfinite(n.glow);
    return n;
}

}  // namespace na::enhancer
