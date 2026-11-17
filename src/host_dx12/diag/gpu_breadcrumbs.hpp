#pragma once
// AMD FidelityFX Breadcrumbs (MIT, vendored in third_party/ffx-breadcrumbs, FidelityFX SDK
// v1.1.4): vendor-neutral GPU markers around NeuralAlias's own passes. Each marker is a
// WriteBufferImmediate into memory that survives a device removal (VirtualAlloc +
// OpenExistingHeapFromAddress where the driver supports it), so after a hang or crash the
// report shows which of our passes the GPU had started and which it had finished.
// Our own DX12 backend (the four breadcrumbs callbacks of AMD's DX12 backend, adapted) so
// no FidelityFX runtime DLL is needed.
//   [diagnostics] gpu_breadcrumbs=0   off by default (two GPU writes per marked pass);
//                                     switchable live on the DLSS debug page.
// The trail is appended to the DRED report (diag/dred.hpp) when the device is removed.
// Nothing here changes rendering.
#include <string>
struct ID3D12Device;
struct ID3D12GraphicsCommandList;

namespace na::diag::crumbs {
void SetEnabled(bool on);
bool Enabled();
// Present (game thread): binds to the device on first use and starts the next frame.
void OnPresent(ID3D12Device* device);
// Marks one of our passes recorded into `list` (game's or ours). No-op while off or unbound.
class Scope {
public:
    Scope(ID3D12GraphicsCommandList* list, const char* name);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    ID3D12GraphicsCommandList* list_ = nullptr;
};
// The trail as text ("" when never enabled). Safe after a device removal.
std::string Status();
// Counters for the status log: markers written, lists registered, errors.
std::string StatusLine();
} // namespace na::diag::crumbs
