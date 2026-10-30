#pragma once
// Unreal Engine console-variable probe (read-only, phase 1 of the Unreal
// adapter). Finds static TAutoConsoleVariable objects for a few rendering
// cvars inside the game's own executable, by locating each cvar's name string
// and the static initializer that registers it, and logs what it finds:
// the object address, its IConsoleVariable target and the stored values.
// Nothing is written to the game. The log decides how phase 2 (setting
// r.ScreenPercentage live, like the game's own DLSS menu does) is built.
namespace na::unreal {
// Starts a background thread that waits, probes once and writes
// <user ini folder>\unreal_probe.log. No-op for non-Unreal executables.
void StartCvarProbe();
}
