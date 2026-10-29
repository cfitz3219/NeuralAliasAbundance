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
UpscalerOverrides GetUpscalerOverrides();
}
