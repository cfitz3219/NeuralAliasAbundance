// Live Neural Rendering pass (phase 3 of docs/NR-INSERTION-DESIGN.md, A30).
//
// Runs NVIDIA's neural renderer (NGX feature 18, "DLSSNR" community
// contract) on the finished frame at Present, on the game's direct queue,
// using the game's own NGX instance (entry points handed in by the proxy).
// Depth and motion vectors are copied into NeuralAlias-owned textures at the
// DLSS evaluate (the game's buffers rotate), published at queue submission.
// Scaled modes upsample depth/motion to the display extent before evaluation.
//
// Colour handling (docs/NR-TLOU-I-REPLAY.md): NR is proven on display-encoded
// SDR. An HDR10 (PQ, RGB10A2) chain is decoded by a compute pass
// (PQ -> nits / paper white -> sRGB-encoded RGBA16F) before NR and
// re-encoded after. SDR chains are processed in their own format.
//
// Safety: any NGX or D3D failure latches the pass OFF with a reason; the
// frame is then presented untouched. Never waits on the CPU in hooks. All
// settings come from na::settings (menu + ini); nothing runs unless
// `nr_enabled` is set and the runtime entry points are present.
#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdint>

namespace na::nr {

// NGX runtime entry points (the ORIGINAL functions of the loaded NGX core,
// not our detours), supplied by the proxy after the core is hooked.
struct Runtime {
    void* create_feature = nullptr;      // NVSDK_NGX_D3D12_CreateFeature
    void* evaluate_feature = nullptr;    // NVSDK_NGX_D3D12_EvaluateFeature
    void* release_feature = nullptr;     // NVSDK_NGX_D3D12_ReleaseFeature
    void* allocate_parameters = nullptr; // NVSDK_NGX_D3D12_AllocateParameters
    void* destroy_parameters = nullptr;  // NVSDK_NGX_D3D12_DestroyParameters
    void* get_capability_parameters = nullptr; // NVSDK_NGX_D3D12_GetCapabilityParameters
};
void SetRuntime(const Runtime& rt);
bool HasRuntime();

// Inputs seen at the DLSS evaluate (same meaning as capture::EvalInputs).
struct EvalInputs {
    ID3D12Resource* depth = nullptr;   // NPSR at call time
    ID3D12Resource* motion = nullptr;  // NPSR at call time
    uint32_t render_w = 0, render_h = 0, out_w = 0, out_h = 0;
    float mv_scale_x = 0, mv_scale_y = 0;
    uint32_t reset = 0;
    uint32_t depth_inverted = 0;
    // Subrect origins (0xFFFFFFFF = not supplied: treated as 0) and whether
    // motion vectors are at the render size (DLSS MVLowRes). Only the
    // rendered region is copied, so padded / dynamic-resolution inputs work.
    uint32_t depth_base_x = 0xFFFFFFFFu, depth_base_y = 0xFFFFFFFFu;
    uint32_t mv_base_x = 0xFFFFFFFFu, mv_base_y = 0xFFFFFFFFu;
    uint32_t mv_low_res = 1;
};

// True when the evaluate detour should read parameters and call OnEvaluate.
bool WantsInputs();
// Inside the DLSS evaluate detour, after the original evaluate, on the
// game's open list: copies depth + motion into our textures.
void OnEvaluate(ID3D12GraphicsCommandList* list, const EvalInputs& in);

// Wrap the actual queue forward. Recording a future list is NOT a ready frame.
// Publish its inputs only after submission, serialized with OnPresent. Different
// producer/presentation queues use only already-completed snapshots; a busy
// producer is deferred without inserting any wait into the presentation queue.
// Snapshots are not reused until both copies and NR have completed; game queues
// never wait for NR. The host
// calls this for every ExecuteCommandLists; test hosts
// must do likewise. This does not consume the adapter's Execute subscription.
using ExecuteFn = void (STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
void Execute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists, ExecuteFn forward);

// At Present, before the original Present, with the back buffer in PRESENT
// state. Runs the pass on `queue` if enabled and healthy; otherwise returns
// immediately. `color_space` is the swap chain's last SetColorSpace1 value
// (DXGI_COLOR_SPACE_CUSTOM when never set).
void OnPresent(ID3D12CommandQueue* queue, ID3D12Resource* backbuffer, DXGI_COLOR_SPACE_TYPE color_space);

struct Stats {
    uint32_t runtime_present;   // entry points supplied
    uint32_t active;            // feature created and passes running
    uint32_t latched_off;       // a failure turned the pass off (reason below)
    uint32_t hdr;               // 1 = HDR10 transform path in use
    uint32_t width, height;     // pass extent
    uint64_t evaluates;         // NR evaluates submitted
    uint64_t skipped;           // frames skipped (no inputs / extent mismatch)
    uint64_t inputs_seen, input_overwrites, missing_inputs, busy_slots;
    uint64_t gpu_submitted, gpu_completed;
    uint64_t input_submitted, input_completed, cross_queue_waits;
    float last_ms, avg_ms;      // GPU time of the whole pass (transform + NR)
    uint32_t gpu_samples;
    char reason[96];            // plain-language reason when not active
};
Stats GetStats();
// Clears a latch so the pass may try again (e.g. after the user re-enables).
void ResetLatch();
// Test seam (C12): builds the pass's device objects (compiling every shader) and
// records the 24/16-bit depth -> R32F conversion of a region on `list`.
bool RecordDepthToR32ForTests(ID3D12GraphicsCommandList* list, ID3D12Resource* depth, DXGI_FORMAT view, ID3D12Resource* out,
                              uint32_t bx, uint32_t by, uint32_t w, uint32_t h);
void Shutdown(); // releases the feature and all resources (game exit / tests)

} // namespace na::nr
