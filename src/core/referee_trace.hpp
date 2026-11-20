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
    static constexpr unsigned subdivisions = 4;
    static constexpr float scoped_minimum = .018f;
    FlickerStage stage = FlickerStage::Unknown;
    uint32_t region = 0;
    std::array<float, 3> stops{};
    float recovery_floor = 0.f;
};

// VARIANT: referee/accepted-regional-trace
inline FlickerTransition LocateTransition(const float* before, const float* after, const float* screen = nullptr,
                                          uint32_t recovery_region = 0, float recovery_floor = 0.f)
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
    const bool scoped = recovery_floor > 0.f && recovery_region < Result::count;
    const float floor = scoped ? std::clamp(recovery_floor, Result::scoped_minimum, .12f) : .12f;
    const auto peak = scoped ? presented.begin() + recovery_region :
        std::max_element(presented.begin(), presented.end(), [](float a, float b) { return std::fabs(a) < std::fabs(b); });
    const uint32_t region = static_cast<uint32_t>(peak - presented.begin());
    const auto quiet = std::count_if(presented.begin(), presented.end(), [](float s) { return std::fabs(s) < .055f; });
    if (quiet < Result::count * 3 / 4) return {};
    std::array<float, 3> stops{delta[0][region], delta[1][region], *peak};
    if (scoped) {
        // The accepted comparison identified this footprint by a spatial
        // pattern. Opposite changes at its edges can cancel in the tile mean.
        // Use the same subdivisions and match the upstream change to the
        // presented pattern before assigning a stage.
        constexpr unsigned n = Result::subdivisions;
        constexpr unsigned columns = Result::columns * n, rows = Result::rows * n;
        const unsigned col = region % Result::columns * n, row = region / Result::columns * n;
        std::array<std::array<float, n * n>, 3> changes{};
        for (unsigned sy = 0; sy < n; ++sy) for (unsigned sx = 0; sx < n; ++sx) {
            const unsigned x0 = (col + sx) * kThumbW / columns;
            const unsigned x1 = x0 + kThumbW / columns;
            const unsigned y0 = ((row + sy) * kThumbH + rows - 1) / rows;
            const unsigned y1 = ((row + sy + 1) * kThumbH + rows - 1) / rows;
            for (unsigned stage = 0; stage < changes.size(); ++stage) {
                const float* a = before + stage * kThumbFloats;
                const float* b = screen && stage == 2 ? screen : after + stage * kThumbFloats;
                float sum = 0.f;
                for (unsigned y = y0; y < y1; ++y)
                    for (unsigned x = x0; x < x1; ++x) sum += b[y * kThumbW + x] - a[y * kThumbW + x];
                changes[stage][sy * n + sx] = sum / ((y1 - y0) * (x1 - x0));
            }
        }
        const auto& visible = changes[2];
        const auto strongest = std::max_element(visible.begin(), visible.end(),
            [](float a, float b) { return std::fabs(a) < std::fabs(b); });
        float magnitude = 0.f, power = 0.f;
        for (float v : visible) { magnitude += std::fabs(v); power += v * v; }
        magnitude /= visible.size();
        if (magnitude < floor) return {};
        stops[2] = std::copysign(magnitude, *strongest);
        for (unsigned stage = 0; stage < 2; ++stage) {
            float dot = 0.f, upstream_power = 0.f;
            for (unsigned i = 0; i < visible.size(); ++i) {
                const float v = changes[stage][i];
                dot += v * visible[i];
                upstream_power += v * v;
            }
            stops[stage] = dot * dot >= .64f * power * upstream_power ? stops[2] * dot / power : 0.f;
        }
    } else if (std::fabs(stops[2]) < floor) return {};
    const auto begins = [&stops, floor](float s) { return std::fabs(s) >= floor * (.085f / .12f) && s * stops[2] > 0; };
    const auto stage = begins(stops[0]) ? FlickerStage::Input :
                       begins(stops[1]) ? FlickerStage::Reconstruction : FlickerStage::Composition;
    return {stage, region, stops, scoped ? floor : 0.f};
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
    // Remove pixel-grid aliasing before estimating a subpixel displacement.
    // Apply the same one-pixel binomial filter to both registered patches.
    constexpr unsigned filtered = kTile - 2;
    const auto smooth = [](const float* src) {
        std::array<float, filtered * filtered> patch{};
        for (unsigned y = 0; y < filtered; ++y) {
            for (unsigned x = 0; x < filtered; ++x) {
                const auto* p = src + y*kTile + x;
                const auto row = [](const float* r) { return r[0] + 2.f*r[1] + r[2]; };
                patch[y*filtered + x] = (row(p) + 2.f*row(p + kTile) + row(p + 2*kTile)) / 16.f;
            }
        }
        return patch;
    };
    const auto a = smooth(before), b = smooth(after);
    double sx = 0, sy = 0, sd = 0, xx = 0, xy = 0, yy = 0, xd = 0, yd = 0, dd = 0;
    unsigned n = 0;
    for (unsigned y = 2; y + 2 < filtered; ++y) {
        for (unsigned x = 2; x + 2 < filtered; ++x) {
            const unsigned i = y * filtered + x;
            const double gx = .25 * ((a[i + 1] - a[i - 1]) + (b[i + 1] - b[i - 1]));
            const double gy = .25 * ((a[i + filtered] - a[i - filtered]) + (b[i + filtered] - b[i - filtered]));
            const double d = a[i] - b[i];
            if (!std::isfinite(gx) || !std::isfinite(gy) || !std::isfinite(d) ||
                (std::max)(a[i], b[i]) <= -10.f) continue;
            sx += gx; sy += gy; sd += d;
            xx += gx * gx; xy += gx * gy; yy += gy * gy;
            xd += gx * d; yd += gy * d; dd += d * d;
            ++n;
        }
    }
    constexpr auto inner = (filtered - 4) * (filtered - 4);
    if (n < inner * 3 / 4) return {};
    const double inv_n = 1.0 / n;
    xx -= sx * sx * inv_n; xy -= sx * sy * inv_n; yy -= sy * sy * inv_n;
    xd -= sx * sd * inv_n; yd -= sy * sd * inv_n; dd -= sd * sd * inv_n;
    const double power = xx + yy, det = xx * yy - xy * xy;
    if (!(power >= .0001 * n && det > .015 * power * power)) return {};
    const double dx = (yy * xd - xy * yd) / det, dy = (xx * yd - xy * xd) / det;
    const double unexplained = dd - dx * xd - dy * yd;
    if (!(std::hypot(dx, dy) < 1.5 && unexplained < power * .16)) return {};
    return {float(dx), float(dy), true};
}

struct JitterDisplacement {
    TileShift raster, reconstructed, presented;
    float phase_x = 0, phase_y = 0;
    bool Valid() const { return raster.valid && reconstructed.valid && presented.valid; }
    float PhaseProjection(const TileShift& shift) const
    {
        return ((shift.x - raster.x + phase_x) * phase_x +
                (shift.y - raster.y + phase_y) * phase_y) / std::hypot(phase_x, phase_y);
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
    unsigned count = 0, cursor = 0, misses = 0;

    void Add(const JitterDisplacement& d)
    {
        if (!d.Valid() || std::hypot(d.phase_x, d.phase_y) < .25f) {
            if (count && ++misses >= 2) count = cursor = 0;
            return;
        }
        misses = 0;
        pairs[cursor] = {d.phase_x, d.phase_y, d.PhaseProjection(d.presented), d.PhaseProjection(d.reconstructed)};
        cursor = (cursor + 1) % pairs.size();
        count = (std::min)(count + 1, static_cast<unsigned>(pairs.size()));
    }
    struct Estimate { float shift = 0; bool valid = false, locked = false; };
    Estimate Measure(bool output) const
    {
        if (count < 6) return {};
        float xmin = 0, xmax = 0, ymin = 0, ymax = 0, sum = 0;
        unsigned same = 0, opposite = 0;
        for (unsigned i = 0; i < count; ++i) {
            const auto& sample = pairs[i];
            xmin = (std::min)(xmin, sample.phase_x); xmax = (std::max)(xmax, sample.phase_x);
            ymin = (std::min)(ymin, sample.phase_y); ymax = (std::max)(ymax, sample.phase_y);
            const float shift = output ? sample.reconstructed : sample.presented;
            same += shift >= .03f;
            opposite += shift <= -.03f;
            sum += shift;
        }
        if (xmin > -.15f || xmax < .15f || ymin > -.15f || ymax < .15f) return {};
        return {sum / count, true, same >= count - 1 || opposite >= count - 1};
    }
};

} // namespace na::referee
