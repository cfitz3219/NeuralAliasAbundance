#pragma once
#include <d3d12.h>
#include <cstdint>
#include "host_dx12/latency/native_audit.hpp"
#include "host_dx12/latency/engine_frames.hpp"

namespace na::latency {
struct Stats {
    uint32_t available = 0, status_valid = 0, low_latency = 0;
    uint32_t game_sleep = 0, foreground = 0, fullscreen_vrr = 0, vsync_override = 0, independent_flip = 0;
    int32_t set_result = -1, query_result = -1;
    uint64_t game_mode_calls = 0, native_samples = 0;
    float native_render_ms = 0; // native sim-start to GPU-end, NOT scanout or full FG latency
    uint32_t sleep_observer = 0, marker_observer = 0;
    uint64_t audit_dropped = 0;
    NativeAudit audit;
    uint32_t pcl_available = 0;
    uint32_t engine_attached = 0, engine_available = 0, external_owner = 0;
    uint64_t engine_sleep = 0, engine_markers = 0, engine_complete = 0, engine_rejected = 0, engine_errors = 0;
    int32_t engine_last_result = -1;
};
// Init thread only. Official NVAPI entry points, system driver, process lifetime.
void Initialize();
// Present only binds the device and polls diagnostics. Sleep belongs to the
// hash-pinned engine simulation entry, never to Present or the message loop.
void Update(ID3D12Device* device, HWND window);
void EngineBegin(uint64_t frame);
void EngineMark(uint64_t frame, Phase phase);
void GeneratedPresent(uint64_t frame, bool end);
Stats GetStats();
}
