// Frame Generation (A59): NVIDIA DLSS Frame Generation (NGX feature 11,
// "DLSSG") driven by NeuralAlias in a game that never asked for it.
//
// What this module does NOT do: present anything. It only produces the
// generated pictures. The presenter (src/host_dx12/overlay/presenter.cpp)
// owns the swap chain, the copies of the real frame and the output textures,
// calls Generate() once per real frame on the game's direct queue, and paces
// the extra presents on its own thread.
//
// Inputs come from the DLSS evaluate detour (OnEvaluate): the game's depth
// and motion vectors at render size, jitter and the motion-vector scale,
// copied into NeuralAlias-owned textures because the game's rotate. Camera
// matrices are supplied by the hash-pinned TLOU Streamline camera adapter.
//
// Modes (na::settings, [fg]): Fixed = always `multiplier` frames shown per
// rendered frame; Dynamic = the count is chosen every frame so the shown
// rate reaches the target (display refresh by default) without exceeding
// `multiplier`. ChooseCount() is the pure controller, exposed for tests.
//
// Safety: every NGX or D3D failure latches the feature OFF with a reason and
// the presenter falls back to showing real frames only. No GPU waits here.
#pragma once

#include <d3d12.h>
#include <dxgiformat.h>

#include <cstdint>
#include "host_dx12/fg/camera.hpp"

namespace na::fg {

// NGX runtime entry points (the ORIGINAL functions, not our detours).
struct Runtime {
    void* create_feature = nullptr;
    void* evaluate_feature = nullptr;
    void* release_feature = nullptr;
    void* allocate_parameters = nullptr;
    void* destroy_parameters = nullptr;
    void* get_capability_parameters = nullptr;
};
void SetRuntime(const Runtime& rt);
bool HasRuntime();

// Seen at the DLSS evaluate (render-size depth/motion, DLSS conventions:
// motion value * mv_scale = render pixels; jitter in render pixels).
struct EvalInputs {
    uint64_t engine_frame = 0;
    Camera camera{};
    bool camera_valid = false;
    ID3D12Resource* depth = nullptr;   // NPSR at call time; plane 0 copied
    ID3D12Resource* motion = nullptr;  // NPSR at call time
    uint32_t render_w = 0, render_h = 0, out_w = 0, out_h = 0;
    float jitter_x = 0, jitter_y = 0;
    float mv_scale_x = 1, mv_scale_y = 1;
    uint32_t depth_base_x = 0, depth_base_y = 0, mv_base_x = 0, mv_base_y = 0;
    bool mv_low_res = true;
    uint32_t reset = 0;
    uint32_t depth_inverted = 0;
    uint32_t mv_jittered = 0;
};
bool WantsInputs(); // enabled, runtime present, not latched
void OnEvaluate(ID3D12GraphicsCommandList* list, const EvalInputs& in);
// Publish recorded inputs only after their actual queue submission. Nested
// inside NR's submission wrapper; its small input lock never calls into NR.
using ExecuteFn = void (STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
void Execute(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*, ExecuteFn);

// One real frame, as the presenter sees it.
struct FrameInfo {
    uint64_t engine_frame = 0; // exact game submit record when available
    ID3D12Resource* color = nullptr;   // NeuralAlias copy of the finished frame, NON_PIXEL_SHADER_RESOURCE
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0, height = 0;
    bool hdr = false;                  // colour buffer is HDR (PQ or scRGB)
    float base_frame_ms = 0;           // smoothed interval between the game's presents
    float display_hz = 0;              // refresh rate of the display the game is on (0 = unknown)
    uint64_t frame_id = 0;             // increments once per real frame
    ID3D12CommandQueue* queue = nullptr; // queue on which Generate's list will execute
    // Optional, A62: a >= 4-byte buffer (UNORDERED_ACCESS at rest) the runtime
    // sets to non-zero in its first byte when the generated pictures should
    // NOT be shown this frame (its own full-screen-menu detection). The
    // presenter reads it back and shows the real frame only.
    ID3D12Resource* disable_flag = nullptr;
};

// Records the generated frames for `f` on `list` (open, ours, executed by
// the caller on the game's direct queue AFTER the game's frame). Writes
// outputs[0..n-1] (UNORDERED_ACCESS at rest, back-buffer format, display
// size) and returns n (0 when off, starting, paused or latched; `reason`
// then says why in plain words). Never more than `capacity`.
uint32_t Generate(ID3D12GraphicsCommandList* list, ID3D12Device* device, const FrameInfo& f,
                  ID3D12Resource* const* outputs, uint32_t capacity);

// Pure controller for Dynamic mode (A72, per NVIDIA's DLSS-G contract): how
// many frames to generate so that (count + 1) * base rate reaches
// `target_fps` (the display's refresh rate unless the user set a target),
// never below 1 (2x) and never above `max_count`; `previous` adds hysteresis
// so the count does not flicker on a borderline base rate. An unknown rate
// or target yields 1. Only max_count == 0 yields 0.
uint32_t ChooseCount(float base_frame_ms, float target_fps, uint32_t max_count, uint32_t previous);

struct Stats {
    uint32_t runtime_present;
    uint32_t enabled;
    uint32_t active;            // feature created, generating
    uint32_t latched_off;
    uint32_t mode;              // 0 fixed, 1 dynamic
    uint32_t multiplier;        // configured (fixed value / dynamic maximum)
    uint32_t max_supported;     // frames the runtime can generate per real frame (0 = unknown)
    uint32_t last_count;        // generated for the last real frame
    uint32_t width, height;     // feature extent
    uint64_t evaluates;         // DLSS-G evaluates submitted
    uint64_t generated;         // frames generated in total
    uint64_t real_frames;       // real frames offered to Generate
    uint64_t skipped;           // real frames with no generation (no fresh inputs, starting, ...)
    uint64_t inputs_recorded, inputs_submitted, inputs_overwritten, missing_inputs;
    float target_fps;           // what dynamic mode aims for
    char reason[112];
};
Stats GetStats();
void ResetLatch();
void Shutdown(); // releases the feature and resources (game exit / tests)
// A73: the presenter is not offering frames right now for a reason of its
// own (the NeuralAlias menu is open, the format allows no generation): the
// status text says so instead of showing the last message from before.
void NotePaused(const char* why);

} // namespace na::fg
