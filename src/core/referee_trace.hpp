#pragma once

#include "core/referee.hpp"
#include <array>

namespace na::referee {

enum class FlickerStage : uint8_t { Unknown, Input, Reconstruction, Composition };

inline const char* StageName(FlickerStage stage)
{
    switch (stage) {
    case FlickerStage::Input: return "input";
    case FlickerStage::Reconstruction: return "reconstruction";
    case FlickerStage::Composition: return "composition";
    default: return "unknown";
    }
}

// These regions partition the existing 128x72 thumbnails. The measurement
// compares each stage to itself, so a static tonemap cannot look like motion.
struct FlickerTransition {
    static constexpr uint32_t columns = 8, rows = 8;
    static constexpr uint32_t count = columns * rows;
    FlickerStage stage = FlickerStage::Unknown;
    uint32_t region = 0;
    std::array<float, 3> stops{};
};

// VARIANT: referee/accepted-regional-trace
inline FlickerTransition LocateTransition(const float* before, const float* after, const float* screen = nullptr)
{
    using Result = FlickerTransition;
    std::array<std::array<float, Result::count>, 3> delta{};
    for (unsigned stage = 0; stage < 3; ++stage) {
        const float* a = before + stage * kThumbFloats;
        const float* b = screen && stage == 2 ? screen : after + stage * kThumbFloats;
        // Cuts and broadly moving pictures do not provide a still baseline.
        if (!(EdgeCorrelation(a, b, kThumbW, kThumbH) >= .90f)) return {};
        for (uint32_t y = 0; y < kThumbH; ++y)
            for (uint32_t x = 0; x < kThumbW; ++x) {
                const uint32_t pixel = y * kThumbW + x;
                const float shift = b[pixel] - a[pixel];
                if (!std::isfinite(shift)) return {};
                delta[stage][(y * Result::rows / kThumbH) * Result::columns + x * Result::columns / kThumbW] += shift;
            }
        for (auto& v : delta[stage]) v *= float(Result::count) / kThumbFloats;
    }
    const auto& presented = delta[2];
    const auto peak = std::max_element(presented.begin(), presented.end(), [](float a, float b) { return std::fabs(a) < std::fabs(b); });
    const uint32_t region = static_cast<uint32_t>(peak - presented.begin());
    if (std::fabs(*peak) < .12f) return {};
    const auto quiet = std::count_if(presented.begin(), presented.end(), [](float s) { return std::fabs(s) < .055f; });
    if (quiet < Result::count * 3 / 4) return {};
    const auto begins = [peak](float s) { return std::fabs(s) >= .085f && s * *peak > 0; };
    const auto stage = begins(delta[0][region]) ? FlickerStage::Input :
                       begins(delta[1][region]) ? FlickerStage::Reconstruction : FlickerStage::Composition;
    return {stage, region, {delta[0][region], delta[1][region], *peak}};
}

// One real highlight change can look exactly like a flicker edge. Require a
// return in the same neighbourhood, on another pair, before reporting flicker.
inline bool Reverses(const FlickerTransition& before, const FlickerTransition& after)
{
    if (before.stage == FlickerStage::Unknown || before.stage != after.stage ||
        before.stops[2] * after.stops[2] >= 0) return false;
    const auto x = std::abs(int(before.region % FlickerTransition::columns) - int(after.region % FlickerTransition::columns));
    const auto y = std::abs(int(before.region / FlickerTransition::columns) - int(after.region / FlickerTransition::columns));
    const float a = std::fabs(before.stops[2]), b = std::fabs(after.stops[2]);
    return x <= 1 && y <= 1 && (std::min)(a, b) * 3 >= (std::max)(a, b);
}

// Full-resolution patches distinguish a moving edge from a changing light.
// Fit log-luminance bias along with translation. A uniform exposure change
// disappears from the translation. Require a low residual to reject a
// changed image or an occlusion that translation alone cannot explain.
struct TileShift {
    float x = 0, y = 0;
    bool valid = false;
};

inline TileShift MeasureTileShift(const float* before, const float* after)
{
    double sx = 0, sy = 0, sd = 0, xx = 0, xy = 0, yy = 0, xd = 0, yd = 0, dd = 0;
    unsigned n = 0;
    for (unsigned y = 2; y + 2 < kTile; ++y) {
        for (unsigned x = 2; x + 2 < kTile; ++x) {
            const unsigned i = y * kTile + x;
            const double gx = .25 * ((before[i + 1] - before[i - 1]) + (after[i + 1] - after[i - 1]));
            const double gy = .25 * ((before[i + kTile] - before[i - kTile]) + (after[i + kTile] - after[i - kTile]));
            const double d = before[i] - after[i];
            if (!std::isfinite(gx) || !std::isfinite(gy) || !std::isfinite(d) ||
                (std::max)(before[i], after[i]) <= -10.f) continue;
            sx += gx; sy += gy; sd += d;
            xx += gx * gx; xy += gx * gy; yy += gy * gy;
            xd += gx * d; yd += gy * d; dd += d * d;
            ++n;
        }
    }
    constexpr auto inner = (kTile - 4) * (kTile - 4);
    if (n < inner * 3 / 4) return {};
    const double inv_n = 1.0 / n;
    xx -= sx * sx * inv_n; xy -= sx * sy * inv_n; yy -= sy * sy * inv_n;
    xd -= sx * sd * inv_n; yd -= sy * sd * inv_n; dd -= sd * sd * inv_n;
    const double power = xx + yy, det = xx * yy - xy * xy;
    if (!(power >= .0002 * n && det > .015 * power * power)) return {};
    const double dx = (yy * xd - xy * yd) / det, dy = (xx * yd - xy * xd) / det;
    const double unexplained = dd - dx * xd - dy * yd;
    if (!(std::hypot(dx, dy) < 1.5 && unexplained < power * .04)) return {};
    return {float(dx), float(dy), true};
}

struct JitterDisplacement {
    TileShift raster, reconstructed, presented;
    float phase_x = 0, phase_y = 0;
    bool Valid() const { return raster.valid && reconstructed.valid && presented.valid; }
    float ScreenError() const
    {
        return std::hypot(presented.x - raster.x + phase_x, presented.y - raster.y + phase_y);
    }
    float ModelError() const
    {
        return std::hypot(reconstructed.x - raster.x + phase_x, reconstructed.y - raster.y + phase_y);
    }
    float PhaseProjection() const
    {
        return ((presented.x - raster.x + phase_x) * phase_x +
                (presented.y - raster.y + phase_y) * phase_y) / std::hypot(phase_x, phase_y);
    }
    float ReconstructedProjection() const
    {
        return ((reconstructed.x - raster.x + phase_x) * phase_x +
                (reconstructed.y - raster.y + phase_y) * phase_y) / std::hypot(phase_x, phase_y);
    }
};

// Accumulate agreeing pairs at one fixed point, in opposite directions for
// both raster axes. This is scoped to the same displayed scene by the caller.
// A camera drifting or panning in one direction cannot supply that witness.
struct JitterVibration {
    struct Sample {
        float phase_x = 0, phase_y = 0;
        float presented = 0, reconstructed = 0;
    };
    std::array<Sample, 8> pairs{};
    unsigned count = 0, cursor = 0;

    void Add(const JitterDisplacement& d)
    {
        if (!d.Valid() || std::hypot(d.phase_x, d.phase_y) < .25f) return;
        pairs[cursor] = {d.phase_x, d.phase_y, d.PhaseProjection(), d.ReconstructedProjection()};
        cursor = (cursor + 1) % pairs.size();
        count = (std::min)(count + 1, static_cast<unsigned>(pairs.size()));
    }
    float Leakage(bool output) const
    {
        if (count < 6) return 0;
        float xmin = 0, xmax = 0, ymin = 0, ymax = 0, sum = 0;
        unsigned agrees = 0;
        for (unsigned i = 0; i < count; ++i) {
            const auto& pair = pairs[i];
            const float error = output ? pair.reconstructed : pair.presented;
            if (error >= .03f) {
                xmin = (std::min)(xmin, pair.phase_x); xmax = (std::max)(xmax, pair.phase_x);
                ymin = (std::min)(ymin, pair.phase_y); ymax = (std::max)(ymax, pair.phase_y);
                ++agrees;
            }
            sum += error;
        }
        if (agrees < count - 1 || xmin > -.15f || xmax < .15f || ymin > -.15f || ymax < .15f) return 0;
        return sum / count;
    }
};

} // namespace na::referee
