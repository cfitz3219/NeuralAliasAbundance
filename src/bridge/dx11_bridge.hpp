// DX11 bridge: runs DLSS (DLAA or real upscaling) for a DX11 game.
//
// Game-agnostic. The caller hands over this frame's colour, depth and motion
// (any sizes up to the session limits) plus a FrameRecord, and the target to
// write the result into. Where the inputs came from is not this module's
// business: hooks and discovery fill them in.
//
// Two backends, same interface:
//   * InProcess: DLSS runs on the game's own D3D11 device (NGX D3D11). No
//     second process, no copies across processes. 64-bit games only, needs a
//     build with the NGX library.
//   * Helper: inputs go through shared textures + shared fences to a 64-bit
//     helper process that runs DLSS on D3D12 (our existing DLSS backend).
//     Works for 32-bit games too, and is the route neural rendering needs
//     (DLSS NR is not available on D3D11). Rebuilt from the recovered Alien
//     bridge (recovered/dx11-enhancement, neural_bridge.cpp).
// Auto tries InProcess first and falls back to Helper.
//
// Misses never fall back to plain DLAA or to the game's own image: when a
// frame's result is late, the last good result is shown again (Cort's rule,
// 2026-11-03). The game's own state is preserved: everything the bridge does
// on the game's context (copies, draws, and in-process DLSS itself) runs inside
// its own swapped-in context state (ID3D11DeviceContext1::SwapDeviceContextState),
// so it works on single-threaded devices too and nothing leaks back.
//
// Robustness review against a Dunia-engine game (Far Cry 5 / New Dawn) is in
// docs/DX11-BRIDGE.md ("What could break it").
#pragma once

#include "bridge/frame_record.hpp"

#include <d3d11.h>

#include <memory>
#include <mutex>
#include <string>

namespace na::bridge {

enum class Backend { Auto, InProcess, Helper };
const char* BackendName(Backend b);

enum class WaitMode {
    // The game's GPU waits for the helper's result; the game's CPU only waits
    // until the helper has *queued* its work (sub-millisecond normally).
    Gpu,
    // The game's CPU waits for the finished result, with a deadline (the
    // original Alien behaviour). Safer if a helper might die mid-frame.
    CpuDeadline,
};

struct Config {
    SessionLimits limits;
    // Size of the motion texture DLSS will read: the render size (default, 0)
    // or the output size when motion is at display resolution.
    uint32_t motion_w = 0, motion_h = 0;
    Backend backend = Backend::Auto;
    WaitMode wait = WaitMode::Gpu;
    std::wstring helper_path; // na_bridge_helper64.exe (Helper backend)
    std::wstring data_path;   // writable folder for NGX logs/cache; empty = %LOCALAPPDATA%/NeuralAlias/bridge
                              // (game folders under Program Files are often read-only)
    uint32_t submit_deadline_ms = 20;  // Gpu mode: helper must queue its work by then
    uint32_t output_deadline_ms = 100; // CpuDeadline mode: result must be done by then
    uint32_t startup_deadline_ms = 15000;
    // Test only: the helper copies colour to output instead of running DLSS,
    // so the transport can be checked on any GPU (or WARP).
    bool copy_test = false;
};

// This frame's inputs, each given EITHER as a shader view OR as the raw
// texture (for textures the game made without shader access, such as most
// back buffers and many depth buffers: the bridge then copies them into its
// own readable texture, picking the right view format for typeless and depth
// formats). Textures are 2D, at least render size. Colour may be multisampled
// (it is resolved); depth and motion may not. With kCameraMotion in the frame
// record, motion is not needed (the helper builds it from depth + camera).
// Colour: any float/unorm colour format, sRGB or not. Depth: any depth format
// (D16, D24S8, D32, D32S8) or its red-channel view. Motion: two-channel.
struct Inputs {
    ID3D11ShaderResourceView* color = nullptr;
    ID3D11ShaderResourceView* depth = nullptr;
    ID3D11ShaderResourceView* motion = nullptr;
    ID3D11Resource* color_texture = nullptr;  // used when `color` is null
    ID3D11Resource* depth_texture = nullptr;  // used when `depth` is null
    ID3D11Resource* motion_texture = nullptr; // used when `motion` is null
};

enum class Result {
    Ok,        // target holds this frame's DLSS result
    Repeated,  // late result: target holds the last good result again
    NotReady,  // still starting, no good result yet, or NeedsRestart(): target untouched
    Failed,    // stopped for good; see Reason()
};
const char* ResultName(Result r);

class Dx11Bridge {
public:
    Dx11Bridge();
    ~Dx11Bridge();
    Dx11Bridge(const Dx11Bridge&) = delete;
    Dx11Bridge& operator=(const Dx11Bridge&) = delete;

    // `immediate` is the game's immediate context. Returns nullptr on success
    // (the backend may still be starting), else a plain-language reason.
    const char* Start(ID3D11DeviceContext* immediate, const Config& config);
    void Stop();
    bool Started() const;

    // Call on the immediate context, after the scene is drawn and before the
    // HUD (or wherever the caller chose). `frame.sequence` is assigned here.
    Result Process(ID3D11DeviceContext* immediate, const Inputs& in, ID3D11RenderTargetView* target, FrameRecord frame);

    Backend ActiveBackend() const;  // Auto resolves to one of the two once started
    const char* Reason() const;     // last failure or refusal, plain language ("" when none)
    // The game changed size (resize, Alt-Tab, resolution change) or its device
    // was replaced: Stop() and Start() again with the new sizes/context.
    bool NeedsRestart() const;
    double LastWaitMs() const;      // CPU time the last Process spent waiting
    uint64_t Frames() const;        // results delivered (Ok)
    uint64_t Repeats() const;       // late frames shown as the last good result
    uint32_t HelperMotionFrames() const; // frames whose camera motion the helper built (kCameraMotion)
    FlowStatus HelperFlow() const;       // the helper's optical-flow counters (kFlowMotion)

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    mutable std::mutex lock_; // hooks may call from the render thread and the present thread
};

} // namespace na::bridge
