// P4-4 (slider route), step 1: our menu's mode (DLAA, Quality, Balanced, Performance, Ultra
// Performance) sets the game's render scale, at start-up (read-time substitution) and live
// (settings_read_hook.hpp's one-candidate-at-a-time switch, confirmed by the picture).
//
// The driver runs once per game Present. It turns a menu request into SetLiveFraction calls
// and answers each Trying with ReportLiveOutcome, judging from the scene size J2 observes:
// the candidate moved the picture when the scene width reaches the expected width (baseline x
// new / old fraction, within 3 %) within kJudgeFrames presents. No J2 scene size: no verdict
// (waits), so a candidate is never rejected for lack of evidence.
#pragma once
#include <cmath>
#include <cstdint>

namespace na::scale {

// Pure verdict (unit-tested): after `frames` presents with candidate `trying` written.
enum class Judge { Wait, Changed, Unchanged };
constexpr uint32_t kJudgeFrames = 45;
inline Judge JudgeExtent(uint32_t baseline_w, double from_fraction, double to_fraction, uint32_t observed_w, uint32_t frames)
{
    if (!baseline_w || !(from_fraction > 0) || !(to_fraction > 0)) return Judge::Wait; // no evidence either way
    const double expected = static_cast<double>(baseline_w) * to_fraction / from_fraction;
    if (observed_w && std::fabs(static_cast<double>(observed_w) - expected) <= 0.03 * expected + 1.0) return Judge::Changed;
    return frames >= kJudgeFrames && observed_w ? Judge::Unchanged : Judge::Wait;
}

// Host side (Windows): start-up and the menu.
// Configures and installs the read hook for `mode` (0..4) when enabled; log_path may be null.
bool StartSliderRoute(int mode, const wchar_t* log_path);
bool SliderRouteActive();
// Menu: the user picked `mode` (0..4). Live when the game kept a marked value; else at next start.
void RequestMode(int mode);
// Once per game Present, with J2's current scene width (0 = unknown).
void TickLive(uint32_t scene_w);

struct DriverStats {
    int mode = -1;               // last requested mode
    double fraction = 0;         // the fraction now in effect (as far as confirmed)
    double requested = 0;        // the fraction being applied
    uint32_t baseline_w = 0;     // scene width when the request started
    uint32_t judged = 0, changed = 0, unchanged = 0; // verdicts given
    char state[96] = {};         // plain words for the menu
};
DriverStats GetDriverStats();

} // namespace na::scale
