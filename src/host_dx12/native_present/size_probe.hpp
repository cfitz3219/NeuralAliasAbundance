// Read-only size-query probe (COMMS 4bd474c I1.5, approved by Astra in her 7f02ef4 review, point 7).
// Counts the Windows/DXGI calls a game can use to learn a size: window, client, monitor,
// display modes, screen metrics, DXGI output desc / mode lists. Every call returns the
// real value unchanged; the probe only records which call, which caller (module + RVA,
// top 8 per API), the last size returned and how many calls came within 3 s after one
// of our resize requests. No stack walks, no per-call file I/O.
#pragma once
#include <cstdint>

namespace na::native_present {
enum class SizeApi : uint32_t {
    ClientRect, WindowRect, MonitorFromWindowCall, MonitorInfo, DisplaySettings, SystemMetrics,
    OutputGetDesc, OutputGetDesc1, OutputModeList, OutputModeList1, Count
};
// Boot/watch worker only. Installs the user32 + DXGI output probes in one hook transaction.
bool InstallSizeProbe();
bool SizeProbeInstalled();
// Called by the size_spoof client/window detours (already hooked there) and by the probe's own detours.
void NoteSizeQuery(SizeApi api, const void* caller, uint32_t w, uint32_t h);
// One status line: "size_probe ..." (empty string when not installed).
void FormatSizeProbe(char* out, uint32_t capacity);
// For tests: clears all counts.
void ResetSizeProbeForTest();
} // namespace na::native_present
