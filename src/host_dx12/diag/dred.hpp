#pragma once
// DRED (Device Removed Extended Data, built into Windows' D3D12 runtime, no download).
// Turned on by the host's init thread BEFORE the game creates its D3D12 device (it only
// applies to devices created afterwards). When any part of the host sees the device removed,
// one report is written beside the status log (NeuralAlias-dred-<pid>.txt): the removal
// reason, the GPU page-fault address with the allocations alive / recently freed there (our
// own objects are named "NeuralAlias ..."), and, if auto-breadcrumbs are on, each command
// list that had not finished with the operations around the last one the GPU completed.
//   [diagnostics] dred=1          page-fault reporting (default on; no measurable cost)
//   [diagnostics] dred_breadcrumbs=0  auto-breadcrumbs (default off: Microsoft measures a
//                                 few percent of GPU time; turn on while chasing a crash)
// Nothing here changes rendering.
#include <cstdint>
#include <string>
struct ID3D12Device;

namespace na::diag {
// Init thread, before the game's device exists. `log_dir` is where the report goes.
// Returns a one-line summary for the boot log.
std::string EnableDred(bool page_faults, bool breadcrumbs, const wchar_t* log_dir);
// Anyone who sees the device removed: writes the report once per process (cheap no-op after
// that, and when the device is fine). Never throws, never blocks on the GPU.
void ReportIfRemoved(ID3D12Device* device);
// The breadcrumb/page-fault text for a removed device (empty when not removed); for tests.
std::string DescribeRemoval(ID3D12Device* device);
bool DredReported();
} // namespace na::diag
