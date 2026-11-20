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

} // namespace na::referee
