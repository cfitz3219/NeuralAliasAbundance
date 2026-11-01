// P4-4 (slider route): set the game's render scale as it reads its own settings file
// (Cort, 2026-11-01: "set it to Quality and it just works"; the user's slider is the exception).
//
// Hooks CreateFileW / CreateFileA / ReadFile / WriteFile in KernelBase. For a small text
// settings file (.ini .cfg .json .xml .txt .yaml .yml .conf .config, at most 1 MB) opened for
// reading, the bytes ReadFile returns are passed to render_scale_patch.hpp, which rewrites a
// known render-scale value in place (same length). The file on disk is never written by us.
// If the game later saves its settings to the same file, our value is put back to the game's
// original in the bytes it writes, so the user's file keeps what they had.
// J2 confirms in the same launch (its detected render extent); nothing here assumes success.
//
// Off until Configure(fraction > 0). Install from a worker thread (never under the loader lock),
// before the game reads its settings. Every patched file is appended to the log file given to
// Configure (Cort's rule: test packages save their own results).
#pragma once
#include <cstdint>

namespace na::scale {

// fraction: render / output (e.g. 0.666667 for DLSS Quality); 0 = off. log_path may be null.
void Configure(double fraction, const wchar_t* log_path);
double ConfiguredFraction();
bool InstallSettingsReadHook();   // idempotent; false if a hook could not be installed

// Nominal DLSS ratios for a mode (0 DLAA, 1 Quality, 2 Balanced, 3 Performance, 4 Ultra Performance).
double FractionForMode(int mode);

struct ReadHookStats {
    uint64_t files_opened = 0;    // candidate settings files opened for reading
    uint64_t reads_patched = 0;   // ReadFile results rewritten
    uint64_t writes_restored = 0; // game writes where our value was put back to the original
    uint64_t reads_unpatched = 0; // a key was found but the value could not be written in its width
    double original = 0;          // the value the game had stored (its own unit)
    char key[40] = {};
    char file[260] = {};          // last patched file (UTF-8)
    char note[160] = {};
};
ReadHookStats GetReadHookStats();

// Live mode change (Sniper applies Render Scale instantly in its own menu). The value substituted
// at read time is tagged (last decimal nudged by one), so after the game parsed it the exact
// float/double holding it can be found in the game's writable memory. SetLiveFraction finds those
// copies once (worker thread only: it scans memory; never from a render callback), then writes
// the new value into each copy that still holds the last value we put there. Returns how many
// copies were written; 0 = live change not available (the menu says "applies on next launch").
// More than 8 copies is treated as ambiguous and nothing is written. The caller confirms the
// change from J2's detected render extent.
int SetLiveFraction(double fraction);
struct LiveStats {
    uint32_t float_hits = 0, double_hits = 0, writes = 0;
    uint64_t scanned_mb = 0;
    double tag = 0;          // the tagged value the game parsed (its unit)
    char note[160] = {};
};
LiveStats GetLiveStats();

} // namespace na::scale
