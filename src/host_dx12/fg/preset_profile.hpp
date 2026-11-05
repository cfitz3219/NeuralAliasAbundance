#pragma once
#include <cstdint>
namespace na::fg::preset {
struct Status {
    bool available = false, pending = false, restart = false;
    uint32_t requested = 1, stored = 0;
    int32_t result = -1;
};
// Published NVIDIA DRS per-application setting. This is a driver request,
// not evidence of which model the NGX runtime selected. Never edits global DRS.
void Initialize(); // init thread, TLOU I or explicit per-game profile_override; read/persist before our NGX preload
void Request(uint32_t preset); // menu thread: queue only, no DRS I/O
Status GetStatus();

// Read-only: NVIDIA driver (NVIDIA app "DLSS Override") preset overrides for
// Super Resolution and Ray Reconstruction, game profile over global profile.
// Reports configured overrides, not proof of the model selected by the runtime.
struct UpscalerOverrides {
    bool available = false;
    uint32_t sr_enabled = 0, sr_preset = 0, rr_enabled = 0, rr_preset = 0;
    bool sr_global = false, rr_global = false;
    // A preset selection in the driver profile (0 = off / the application
    // decides; 0x00FFFFFF = NVIDIA's "Latest"). Both the enable switch and a
    // selection must be present; a leftover selection with enable OFF is inert.
    bool SrForced() const { return available && sr_enabled == 1 && sr_preset != 0; }
    bool RrForced() const { return available && rr_enabled == 1 && rr_preset != 0; }
};
void ReadUpscalerOverrides(); // init thread only (DRS session); never writes
// Menu thread: ask a background worker to re-read the overrides (throttled to
// one read per 2 s), so a change made in the NVIDIA app while the game runs is
// reflected when the menu is opened. Never blocks; never writes.
void RequestUpscalerOverridesRefresh();
UpscalerOverrides GetUpscalerOverrides();

// NVIDIA driver anisotropic-filtering override (Control Panel "Anisotropic filtering"). Astra,
// COMMS 2026-11-05 14:38Z: Cort's global AND sniper5_dx12.exe profile resolve to selector USER
// with level NONE_LINEAR (Control Panel "Off"), which forces linear filtering over whatever
// the game or we ask for, so 16x had no visible effect. Selector APP = application-controlled.
struct AnisoOverride {
    bool available = false;    // the driver settings could be read
    bool app_profile = false;  // this executable has its own NVIDIA profile (writable target)
    uint32_t selector = 0, level = 0; // effective values for this executable
    // What the driver had when the game started: it reads its profile then, so a change made
    // while the game runs applies from the next start.
    bool start_known = false;
    uint32_t start_selector = 0, start_level = 0;
    bool write_pending = false, written = false;
    int32_t write_result = 0;
    // The driver forces a level below `wanted` (4 / 8 / 16) over the application's samplers.
    bool Limits(uint32_t wanted) const { return available && selector == 1u && level < wanted; }
    // The driver replaces the application's texture filtering (anything but application-
    // controlled) in this run / in its current profile.
    bool ForcedThisRun() const { return start_known && start_selector != 0u; }
    bool ForcedNow() const { return available && selector != 0u; }
};
void ReadAnisoOverride();     // init thread / background worker only (DRS session)
AnisoOverride GetAnisoOverride();
// Menu thread, on the user's click: set THIS game's NVIDIA profile to application-controlled
// anisotropic filtering (the previous value is saved for RestoreAnisoOverride). Never touches
// the global profile. The driver reads the profile when the game starts: takes effect on the
// next launch.
void RequestAppControlledAniso();
void RequestRestoreAniso();
}

