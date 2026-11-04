// DX11 host: the "ears" for DX11 games. Hooks the game's D3D11 device,
// contexts (immediate and deferred) and swap chain, feeds what the game does
// into the game-agnostic frame model (frame_model.hpp), finds the camera in its
// constant buffers (camera_pick.hpp + our camera tracker), and at the learned
// point (just before the game first reads the finished scene colour) runs DLSS
// through the DX11 bridge, with camera motion built in the DX12 helper.
//
// Nothing here knows any game. Off unless [dx11] enabled=1 in
// NeuralAliasHost.ini beside our dxgi.dll. Every run writes a log
// (%LOCALAPPDATA%\NeuralAlias\dx11\NeuralAliasDX11-<pid>.log by default).
//
// First stage: DLAA (render size = screen size). Jitter (and the mip bias that
// goes with it) is behind [dx11] jitter=1. Render-small upscaling, RCAS and the
// in-game menu come later.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

namespace na::dx11 {

struct HostConfig {
    bool jitter = false;          // inject sub-pixel camera jitter (and mip bias)
    float mip_bias = -1.f;        // applied to new mip-mapped samplers when jitter is on
    uint32_t jitter_phases = 8;   // DLAA: 8 phases
    uint32_t preset = 12;         // DLSS preset L for DLAA (Cort, 2026-11-04)
    bool gpu_wait = true;         // bridge WaitMode::Gpu (false: CpuDeadline)
    bool copy_test = false;       // tests: helper copies colour instead of DLSS
    std::wstring helper_path;     // default: na_bridge_helper64.exe beside our DLL
    std::wstring data_path;       // default: %LOCALAPPDATA%\NeuralAlias\bridge
    std::wstring log_path;        // default: %LOCALAPPDATA%\NeuralAlias\dx11\NeuralAliasDX11.log (pid added)
};

// From the dxgi proxy's DllMain: reads [dx11] from `ini_name` beside `self`
// and, when enabled=1, starts an init thread that installs the hooks.
bool StartFromIni(HMODULE self, const wchar_t* ini_name);

// Installs the hooks now (init thread or tests). Idempotent.
bool Install(const HostConfig& config);

struct HostStats {
    bool installed = false, device_found = false, selected = false, camera = false;
    uint64_t presents = 0, fired = 0, delivered = 0, repeated = 0, not_ready = 0, failed = 0;
    uint64_t no_camera = 0, refused = 0, jitter_writes = 0, samplers_biased = 0;
    uint64_t lists_recorded = 0, lists_executed = 0;
    uint32_t last_draws = 0, last_scene_draws = 0; // the last frame, as the model counted them
    uint32_t color_w = 0, color_h = 0, color_format = 0, depth_format = 0;
    uint64_t camera_offset = 0;
    bool depth_reversed = false;
    uint32_t helper_motion_frames = 0;
    char reason[192] = {};
    char log_path[MAX_PATH] = {};
};
HostStats GetStats();

} // namespace na::dx11
