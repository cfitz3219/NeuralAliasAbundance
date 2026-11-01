// P4-4 (slider route): set the game's render scale as it reads its own settings file
// (Cort, 2026-11-01: "set it to Quality and it just works"; the user's slider is the exception).
//
// Hooks CreateFileW / CreateFileA / ReadFile / WriteFile in KernelBase. For a small text
// settings file (.ini .cfg .json .xml .txt .yaml .yml .conf .config, at most 1 MB) opened for
// reading, the bytes a SYNCHRONOUS ReadFile returns are passed to render_scale_patch.hpp, which
// rewrites a known render-scale value in place (same length). We never open the file for writing.
// When the game later saves synchronously to the same file (same final path), a value we put
// there (the substituted one, or a live value we wrote, in any number formatting) is put back to
// the game's original, in the same width, in the bytes it writes. Not covered, and logged where
// seen: asynchronous (OVERLAPPED) saves are forwarded untouched; saves through a temporary file
// renamed over the original; a width the original cannot be written in. So the user's file keeps
// its value in the cases we intercept, not unconditionally.
// A tracked handle is re-identified by volume + file index (handle values are reused).
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
// at read time is tagged (last decimal nudged by one) when its width allows, so after the game
// parsed it the float/double holding it can be found in the game's writable memory.
//
// A rare value identifies DATA RELATED to the setting, not its live owner (Astra 2026-11-01):
// the parsed, menu, last-applied, save and scratch copies can all hold it. So copies are never
// written all at once. Each candidate is tried ALONE; the caller watches J2 and reports whether
// the scene extent actually changed (and DLSS upscaled). A candidate that did not move the
// picture is put back exactly as it was and the next is tried; the one that did is the control
// and the only one written from then on. Writing memory is never itself evidence of success.
//
// Threading: SetLiveFraction never scans on the caller's thread and never holds the file-hook
// lock while scanning. The first request after a tagged read starts ONE background scan for that
// read (no repeated whole-process scan per Apply); the latest request is applied when it ends.
enum class LiveResult : int {
    Off,            // no request, or the fraction is out of range
    NoTag,          // no MARKED value was read at start-up (startup substitution may still have worked)
    Scanning,       // the background scan is running; the request is applied when it ends
    NotFound,       // the marker is not in memory (the game did not keep the parsed value)
    Ambiguous,      // more candidates than are worth trying one at a time; nothing written
    Trying,         // candidate written; caller must report the observed extent (ReportLiveOutcome)
    Applied,        // written to the confirmed control
    NoLongerOurs,   // the control no longer holds our last value (the user moved the game's slider)
    Exhausted,      // every candidate was tried and none changed the picture: not live-controllable
};
const char* LiveResultName(LiveResult r);
LiveResult SetLiveFraction(double fraction);
// After Trying: `extent_changed` = J2 saw the requested render extent AND submitted a DLSS
// upscale. false puts the candidate back and moves on to the next (the caller then calls
// SetLiveFraction again, which writes the next candidate). Ignored in other states.
void ReportLiveOutcome(bool extent_changed);
bool WaitLiveScan(uint32_t timeout_ms); // diagnostics / tests: true when no scan is running

struct LiveStats {
    LiveResult result = LiveResult::Off;
    uint32_t float_hits = 0, double_hits = 0, candidates = 0;
    int32_t trying = -1, control = -1;   // candidate index being tried / confirmed
    uint32_t writes = 0, restores = 0, generation = 0;
    uint64_t scanned_kb = 0, scan_ms = 0;
    uint32_t regions = 0, writable_regions = 0, stack_allocations_skipped = 0;
    bool tagged = false;                 // the read carried a marker that survives float conversion
    double tag = 0;                      // the marked value the game parsed (its unit)
    double requested = 0;                // last requested value (its unit)
    char note[200] = {};
};
LiveStats GetLiveStats();

} // namespace na::scale
