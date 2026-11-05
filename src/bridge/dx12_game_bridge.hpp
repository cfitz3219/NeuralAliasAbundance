// Game-side bridge from D3D12 (DX9 plan step D4): hands a frame's colour and
// depth, as D3D12 resources, to the unchanged 64-bit helper
// (na_bridge_helper64.exe) through NT-shared textures and fences, and writes
// the helper's result back. Same protocol, record and helper as the DX11
// bridge (dx11_bridge.hpp, win_shared.hpp, frame_record.hpp); only the game's
// end is D3D12, which is what D3D9On12 gives a DX9 game. Works at 32 and 64
// bits (D3D12 shares across both).
//
// Everything runs on the caller's queue: the inputs are converted into the
// shared textures with small shader copies (any colour format; depth from
// D24S8/D32/D16, also when the game's depth is not shader-readable: it is
// copied first), then the helper's output is copied back into the target in
// the target's format. Misses repeat the last good result (Cort's rule).
//
// The caller passes resources in D3D12_RESOURCE_STATE_COMMON and gets them
// back in COMMON; Process returns the fence value to hand back with them
// (D3D9On12's ReturnUnderlyingResource takes exactly that).
#pragma once

#include "bridge/frame_record.hpp"
#include "bridge/dx11_bridge.hpp" // Result

#include <d3d12.h>

#include <memory>
#include <string>

namespace na::bridge {

struct Dx12Config {
    SessionLimits limits;              // render <= max, output = target size
    std::wstring helper_path;          // na_bridge_helper64.exe
    std::wstring data_path;            // NGX logs/cache; empty = %LOCALAPPDATA%/NeuralAlias/bridge
    uint32_t submit_deadline_ms = 20;  // helper must queue its work by then, else the last good frame repeats
    uint32_t startup_deadline_ms = 15000;
    // Test only: the helper copies colour to output instead of DLSS, and the
    // copy back swaps red and blue, so a test sees exactly where the result landed.
    bool copy_test = false;
};

struct Dx12Inputs {
    ID3D12Resource* color = nullptr;  // the game's scene colour (any colour format, 1 sample)
    ID3D12Resource* depth = nullptr;  // the scene depth (D24S8 / D32 / D16 / D32S8 or a typeless/R32F view of one)
    ID3D12Resource* target = nullptr; // where the result goes (often the colour itself), output size
};

class Dx12GameBridge {
public:
    Dx12GameBridge();
    ~Dx12GameBridge();
    Dx12GameBridge(const Dx12GameBridge&) = delete;
    Dx12GameBridge& operator=(const Dx12GameBridge&) = delete;

    // Starts the helper. nullptr on success (it may still be starting), else a reason.
    const char* Start(ID3D12Device* device, ID3D12CommandQueue* queue, const Dx12Config& config);
    void Stop();
    bool Started() const;
    const SessionLimits& Limits() const;

    // Records and executes on the queue given to Start. `*fence` and `*value`
    // (when the result is not NotReady) are what the GPU signals when the
    // resources may be used again.
    Result Process(const Dx12Inputs& in, FrameRecord frame, ID3D12Fence** fence, uint64_t* value);

    const char* Reason() const;
    double LastWaitMs() const;
    uint64_t Frames() const;
    uint64_t Repeats() const;
    uint32_t HelperMotionFrames() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace na::bridge
