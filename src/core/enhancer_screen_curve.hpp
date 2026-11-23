#pragma once

// How bright a scene value ends up on the screen (docs/NA1-ENHANCER.md section 4). The game's
// tone curve, exposure and grading are not known to us, so they are measured: the Referee already
// keeps a small picture of the DLSS output and one of the presented screen, cell for cell. Bin the
// cells by output brightness, take the median screen value of each bin (the HUD and anything else
// drawn over the scene are outliers a median ignores), and keep the result non-decreasing.

#include "core/na1_reference.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace na::enhancer {

constexpr uint32_t kCurveKnots = 16;
constexpr uint32_t kCurveLeastSamples = 64;   // cells needed before any fit is tried
constexpr uint32_t kCurveLeastPerKnot = 6;    // cells a knot needs for its median to mean something
constexpr uint32_t kCurveLeastKnots = 5;      // measured knots needed; the rest are filled in between
constexpr float kCurveLeastRange = 1.f;       // stops of output brightness the picture must span
constexpr float kCurveLeastRise = 0.02f;      // the screen must get at least this much brighter along it
constexpr float kRefereeLumaFloor = 1e-4f;    // the Referee's thumbnails are log2(luma + this)

// A log2(luminance + floor) value moved to NA1's own y = log2(luminance + 1e-3).
inline float YFromLog2(float value, float floor_used)
{
    if (floor_used == na::na1::ref::kLumFloor) return value;
    return na::na1::ref::LogLum((std::max)(std::exp2(value) - floor_used, 0.f));
}

// output_log2: log2(luminance + floor_used) of the DLSS output, one value per cell (the Referee's
// output thumbnail as it is). screen_value: the same cells of the presented screen as linear
// luminance, 0..1. Returns false with a plain reason, and leaves `out` alone, when no curve can be
// trusted; the caller then uses AssumedCurve.
inline bool FitScreenCurve(const float* output_log2, const float* screen_value, uint32_t count, na::na1::ref::Curve& out,
                           const char** why, float floor_used = kRefereeLumaFloor)
{
    const char* unused = nullptr;
    const char*& reason = why ? *why : unused;
    reason = "";
    if (!output_log2 || !screen_value) { reason = "no thumbnails to fit the display curve from"; return false; }
    std::vector<float> ys, ss;
    ys.reserve(count);
    ss.reserve(count);
    double sum_y = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (!std::isfinite(output_log2[i]) || !std::isfinite(screen_value[i])) continue;
        ys.push_back(YFromLog2(output_log2[i], floor_used));
        ss.push_back((std::clamp)(screen_value[i], 0.f, 1.f));
        sum_y += ys.back();
    }
    if (ys.size() < kCurveLeastSamples) { reason = "too few thumbnail cells to fit the display curve"; return false; }

    // The knots span the middle 99% of the picture's brightness; the ends hold their value.
    std::vector<float> sorted = ys;
    std::sort(sorted.begin(), sorted.end());
    const auto at = [&](float share) { return sorted[size_t(share * float(sorted.size() - 1))]; };
    const float first = at(0.005f), last = at(0.995f);
    if (!(last - first >= kCurveLeastRange)) { reason = "the picture spans too little brightness to fit the display curve"; return false; }
    const float dx = (last - first) / float(kCurveKnots - 1);

    std::vector<float> bin[kCurveKnots];
    for (size_t i = 0; i < ys.size(); ++i) {
        const float t = (ys[i] - first) / dx + 0.5f;
        if (t < 0 || t >= float(kCurveKnots)) continue;
        bin[size_t(t)].push_back(ss[i]);
    }
    float value[kCurveKnots] = {}, weight[kCurveKnots] = {};
    uint32_t measured = 0;
    for (uint32_t k = 0; k < kCurveKnots; ++k) {
        if (bin[k].size() < kCurveLeastPerKnot) continue;
        std::nth_element(bin[k].begin(), bin[k].begin() + std::ptrdiff_t(bin[k].size() / 2), bin[k].end());
        value[k] = bin[k][bin[k].size() / 2];
        weight[k] = float(bin[k].size());
        ++measured;
    }
    if (measured < kCurveLeastKnots) { reason = "too few brightness levels in the picture to fit the display curve"; return false; }

    // Non-decreasing: neighbours that run downhill are pooled into their weighted mean, so one
    // odd bin (a bright HUD panel over a dark part of the scene) cannot lift everything after it.
    struct Block { float sum, weight; uint32_t first, last; };
    Block blocks[kCurveKnots];
    uint32_t n = 0;
    for (uint32_t k = 0; k < kCurveKnots; ++k) {
        if (!(weight[k] > 0)) continue;
        blocks[n++] = {value[k] * weight[k], weight[k], k, k};
        while (n > 1 && blocks[n - 2].sum * blocks[n - 1].weight > blocks[n - 1].sum * blocks[n - 2].weight) {
            blocks[n - 2].sum += blocks[n - 1].sum;
            blocks[n - 2].weight += blocks[n - 1].weight;
            blocks[n - 2].last = blocks[n - 1].last;
            --n;
        }
    }
    for (uint32_t b = 0; b < n; ++b)
        for (uint32_t k = blocks[b].first; k <= blocks[b].last; ++k)
            if (weight[k] > 0) value[k] = blocks[b].sum / blocks[b].weight;
    // Knots nobody measured take the straight line between their measured neighbours.
    int before = -1;
    for (uint32_t k = 0; k < kCurveKnots; ++k) {
        if (!(weight[k] > 0)) continue;
        if (before < 0)
            for (uint32_t j = 0; j < k; ++j) value[j] = value[k];
        else
            for (uint32_t j = uint32_t(before) + 1; j < k; ++j)
                value[j] = value[before] + (value[k] - value[before]) * float(j - uint32_t(before)) / float(k - uint32_t(before));
        before = int(k);
    }
    for (uint32_t j = uint32_t(before) + 1; j < kCurveKnots; ++j) value[j] = value[before];

    if (!(value[kCurveKnots - 1] - value[0] >= kCurveLeastRise)) {
        reason = "the screen does not get brighter where the picture does: not the same picture, or covered";
        return false;
    }
    out.mode = 1;
    out.x0 = first;
    out.dx = dx;
    for (uint32_t k = 0; k < kCurveKnots; ++k) out.cy[k] = value[k];
    out.mean_y = float(sum_y / double(ys.size()));
    reason = "fitted from the Referee's output and screen thumbnails";
    return true;
}

// No fit yet, or an HDR screen: middle grey of the picture's own average, with a soft shoulder.
inline na::na1::ref::Curve AssumedCurve(float mean_y)
{
    na::na1::ref::Curve c;
    c.mode = 2;
    c.mean_y = std::isfinite(mean_y) ? mean_y : 0.f;
    return c;
}

// An 8-bit sRGB picture is already what the screen shows.
inline na::na1::ref::Curve DisplayReferredCurve()
{
    na::na1::ref::Curve c;
    c.mode = 0;
    return c;
}

}  // namespace na::enhancer
