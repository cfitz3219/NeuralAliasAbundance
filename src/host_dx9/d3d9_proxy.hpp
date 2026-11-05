// d3d9.dll proxy status (DX9 plan D2). Read by tests and by the collect step
// through the NA_D3D9_QueryStatus export.
#pragma once

#include <cstdint>

#include "host_dx9/dx9_hooks.hpp"

namespace na::dx9 {

struct ProxyStatus {
    uint32_t size;          // = sizeof(ProxyStatus), set by the caller
    uint32_t enabled;       // [dx9] enabled=1 in NeuralAliasHost.ini beside the DLL
    uint32_t on12;          // 1 = the last Direct3DCreate9(Ex) went through D3D9On12
    uint32_t our_device;    // 1 = 9On12 runs on our own D3D12 device and queue
    uint32_t creates;       // Direct3DCreate9(Ex) calls seen
    uint32_t fallbacks;     // ...that fell back to plain system d3d9
    uint32_t devices;       // IDirect3D9(Ex)::CreateDevice(Ex) calls that succeeded
    uint32_t borderless;    // fullscreen requests turned into borderless windowed
    uint32_t resets;        // IDirect3DDevice9::Reset(Ex) calls seen
    HookStats hooks;        // what the device hooks found (D3)
    char log_path[260];     // UTF-8, empty when there is no log
};

} // namespace na::dx9

// Fills *status when status->size matches; returns 1 on success.
extern "C" int __cdecl NA_D3D9_QueryStatus(na::dx9::ProxyStatus* status);
