// DX9 transport (DX9 plan step D4): D3D9On12 unwrap -> the D3D12 game bridge ->
// the unchanged 64-bit helper -> result back into the scene colour.
#pragma once

#include <d3d12.h>

#include <cstdint>
#include <string>

namespace na::dx9 {

struct TransportConfig {
    bool enabled = true;      // [dx9] dlss
    bool copy_test = false;   // [dx9] copy_test: helper copies, the way back swaps red/blue
    uint32_t preset = 0;      // [dx9] preset (DLSS render preset; 0 = NVIDIA default)
    std::wstring helper_path; // na_bridge_helper64.exe beside the proxy
};

struct TransportStats {
    uint32_t ok = 0, repeated = 0, not_ready = 0, failed = 0, skipped = 0, restarts = 0;
    uint32_t at_read = 0, at_hud = 0, at_present = 0;
    double wait_ms = 0;
};

// device/queue: ours when 9On12 runs on our D3D12 device; null when 9On12 made
// its own (a queue is then made on it at the first frame).
void StartTransport(const TransportConfig& cfg, ID3D12Device* device, ID3D12CommandQueue* queue);
TransportStats GetTransportStats();

} // namespace na::dx9
