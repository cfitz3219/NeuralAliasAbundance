// See live_driver.hpp.
#include "host_dx12/scale/live_driver.hpp"
#include "host_dx12/scale/settings_read_hook.hpp"

#include <cstdio>
#include <mutex>

namespace na::scale {
namespace {
std::mutex g_dm;
bool g_active = false;
DriverStats g_ds;
bool g_busy = false;         // a request is being applied
int g_tracked = -2;          // candidate index being judged
uint32_t g_frames = 0;       // presents since that candidate was written
uint32_t g_no_evidence = 0;  // presents with no scene size while judging
uint32_t g_last_w = 0;       // J2's scene width at the last present
constexpr uint32_t kNoEvidenceLimit = 600; // ~10 s at 60 fps: put the candidate back, stop

void state(const char* fmt, double v = 0)
{
    std::snprintf(g_ds.state, sizeof g_ds.state, fmt, v);
}
void finish_locked(const char* fmt, double v = 0)
{
    g_busy = false;
    g_tracked = -2;
    state(fmt, v);
}
} // namespace

bool StartSliderRoute(int mode, const wchar_t* log_path)
{
    const double f = FractionForMode(mode);
    Configure(f, log_path);
    const bool ok = InstallSettingsReadHook();
    std::lock_guard<std::mutex> lk(g_dm);
    g_active = ok;
    g_ds.mode = mode;
    g_ds.fraction = f;
    state(ok ? "Set at start-up: %.0f%% render scale" : "Could not watch the game's settings file", f * 100.0);
    return ok;
}

bool SliderRouteActive()
{
    std::lock_guard<std::mutex> lk(g_dm);
    return g_active;
}

void RequestMode(int mode)
{
    std::lock_guard<std::mutex> lk(g_dm);
    if (!g_active || mode < 0 || mode > 4) return;
    const double f = FractionForMode(mode);
    g_ds.mode = mode;
    if (!g_busy && f == g_ds.fraction) { state("Render scale %.0f%%", f * 100.0); return; }
    g_ds.requested = f;
    g_ds.baseline_w = g_last_w;
    g_busy = true;
    g_tracked = -2;
    g_frames = g_no_evidence = 0;
    SetLiveFraction(f); // never blocks: the first request starts the background scan
    state("Switching to %.0f%% render scale...", f * 100.0);
}

void TickLive(uint32_t scene_w)
{
    std::lock_guard<std::mutex> lk(g_dm);
    if (!g_active || !g_busy) { g_last_w = scene_w; return; }
    const LiveStats st = GetLiveStats();
    switch (st.result) {
    case LiveResult::Scanning:
        break;
    case LiveResult::Trying: {
        if (st.trying != g_tracked) { g_tracked = st.trying; g_frames = 0; g_no_evidence = 0; }
        ++g_frames;
        if (!scene_w || !g_ds.baseline_w) {
            if (!g_ds.baseline_w) g_ds.baseline_w = scene_w; // DLAA started after the request
            if (++g_no_evidence >= kNoEvidenceLimit) {
                ReportLiveOutcome(false); // never leave an unconfirmed write in the game's memory
                finish_locked("Could not confirm a live change (Live DLAA is not running); applies at next start");
            }
            break;
        }
        const Judge v = JudgeExtent(g_ds.baseline_w, g_ds.fraction, g_ds.requested, scene_w, g_frames);
        if (v == Judge::Wait) break;
        ++g_ds.judged;
        if (v == Judge::Changed) {
            ++g_ds.changed;
            ReportLiveOutcome(true);
            g_ds.fraction = g_ds.requested;
            finish_locked("Render scale %.0f%% (switched live)", g_ds.fraction * 100.0);
        } else {
            ++g_ds.unchanged;
            ReportLiveOutcome(false);       // puts this candidate back
            SetLiveFraction(g_ds.requested); // writes the next one (or reports Exhausted)
            g_tracked = -2;
        }
        break;
    }
    case LiveResult::Applied:
        // The control is known: written directly. The scene size follows within a few frames.
        g_ds.fraction = g_ds.requested;
        finish_locked("Render scale %.0f%% (switched live)", g_ds.fraction * 100.0);
        break;
    case LiveResult::NoLongerOurs:
        finish_locked("The game's own Render Scale slider was moved: left alone");
        break;
    default: // NoTag, NotFound, Ambiguous, Exhausted, Off
        finish_locked("This game cannot switch live: applies at next start");
        break;
    }
    g_last_w = scene_w;
}

DriverStats GetDriverStats()
{
    std::lock_guard<std::mutex> lk(g_dm);
    return g_ds;
}

} // namespace na::scale
