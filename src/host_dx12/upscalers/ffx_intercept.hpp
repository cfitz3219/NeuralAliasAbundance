// C7a (MASTER-PLAN Phase 2): FSR 3.1+ call interception (AMD FidelityFX API).
//
// Many DX12 games without DLSS still hand an upscaler everything DLSS needs:
// colour, depth, motion vectors, jitter, sizes and exposure, already labelled.
// FSR 3.1 and later reach the upscaler through the stable, MIT-licensed
// FidelityFX API exported by the game's own amd_fidelityfx_*dx12.dll
// (ffxCreateContext / ffxDispatch / ffxDestroyContext ...). This module hooks
// those exports in the GAME's process (never modifies the DLL file), reads each
// upscale dispatch, and either
//   * Observe (default): records what FSR was given and passes the call on, or
//   * ReplaceWithDlss: feeds the same inputs to the C6 DLSS backend, writing
//     into FSR's own output texture, and skips FSR for that frame. Any problem
//     (unknown context, incompatible resource states, DLSS unavailable or
//     failing) falls back to the original FSR call for that frame, with a reason.
//
// Statically linked FSR 2.x and FSR 3.0 (no DLL exports) are not covered here.
//
// Threading: detours take a short internal lock; no GPU waits. Hooks are
// installed only from the watch thread or test code, never from a detour or
// under the loader lock (hooking rules in vtable_hook.hpp).
#pragma once

#include <d3d12.h>
#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace na::dlss {
struct EvalDesc;
}

namespace na::ffx {

enum class Mode : uint32_t { Observe = 0, ReplaceWithDlss = 1 };
void SetMode(Mode m);
Mode GetMode();

// One FSR upscale dispatch exactly as the game issued it. Resource pointers are
// identity only (no references are taken).
struct Observation {
    uint64_t serial = 0;                 // 1-based count of upscale dispatches
    void* context = nullptr;             // ffxContext the game dispatched on
    bool context_known = false;          // its ffxCreateContext was observed
    uint32_t context_flags = 0;          // FfxApiCreateContextUpscaleFlags (when known)
    uint32_t max_render_w = 0, max_render_h = 0, max_upscale_w = 0, max_upscale_h = 0;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Resource* color = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motion = nullptr;
    ID3D12Resource* exposure = nullptr;
    ID3D12Resource* reactive = nullptr;
    ID3D12Resource* transparency = nullptr;
    ID3D12Resource* output = nullptr;
    uint32_t color_state = 0, depth_state = 0, motion_state = 0, output_state = 0; // FFX_API_RESOURCE_STATE_*
    uint32_t render_w = 0, render_h = 0, upscale_w = 0, upscale_h = 0;
    float jitter_x = 0, jitter_y = 0;
    float mv_scale_x = 0, mv_scale_y = 0;
    float pre_exposure = 0, frame_time_ms = 0;
    float camera_near = 0, camera_far = 0, camera_fov_y = 0;
    bool reset = false;
    bool sharpening = false;
    float sharpness = 0;
    uint32_t dispatch_flags = 0;
};

// Pure conversion used by the replace path (and tests). Returns false with a
// plain-language reason when these inputs cannot drive DLSS faithfully.
bool ToDlss(const Observation& o, na::dlss::EvalDesc* out, const char** reason);

// Hooks the FidelityFX API exports `module` provides (each module keeps its
// own originals). Returns the number of functions hooked (0 = none / not FFX).
uint32_t InstallForModule(HMODULE module);
// Game-side FidelityFX API module names (loader/runtime DLLs that export ffx*).
bool IsFfxModuleName(const wchar_t* name, size_t len);
// Watches for FidelityFX modules (already loaded ones included) and hooks them
// from a worker thread, outside the loader lock.
bool StartModuleWatch();
void StopModuleWatch();

bool LastObservation(Observation* out);

struct Stats {
    uint32_t modules_hooked = 0, hooked_functions = 0;
    uint64_t contexts_created = 0, contexts_destroyed = 0, upscale_contexts = 0;
    uint64_t upscale_dispatches = 0, other_dispatches = 0;
    uint64_t replaced = 0;      // frames DLSS produced instead of FSR
    uint64_t fallbacks = 0;     // replace requested but FSR ran (reason below)
    uint64_t unknown_context = 0;
    uint64_t barriers = 0;      // state transitions added around DLSS (ARGON-G2-003)
    char reason[128] = {};      // last fallback reason
};
Stats GetStats();
void ResetForTests(); // clears contexts, stats and the last observation (hooks stay)

} // namespace na::ffx
