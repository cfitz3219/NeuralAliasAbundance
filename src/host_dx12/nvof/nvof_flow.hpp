// NVIDIA hardware optical flow (NVOFA) through the driver's nvofapi64.dll â€”
// the Route A Tier-3 raw-flow supplier for the MotionArbiter (M-001).
//
// Thin C++ wrapper over the Optical Flow SDK 5.0 D3D12 interface
// (third_party/nvidia-optical-flow-sdk, MIT-licensed headers; the runtime is
// the driver's). No SDK helper classes are used. Everything is asynchronous:
// Execute() waits on a fence the caller signals when the inputs are ready and
// signals another fence when the flow is written; nothing here blocks.
//
// Conventions: with input = CURRENT frame and reference = PREVIOUS frame the
// forward flow at a pixel is p_previous - p_current in pixels (S10.5 fixed
// point in the output), which is exactly the arbiter's v convention.
// Input frames: B8G8R8A8_UNORM textures (state COMMON), display-encoded SDR.
// Output: R16G16_SINT texture of (width/grid) x (height/grid), state COMMON.
//
// TEARDOWN RULE (found empirically with driver 32.0.16.1692, A36): every
// ID3D12Resource that was registered with the engine must be Unregistered
// AND have its last reference released BEFORE Shutdown()/destruction of the
// Flow object, and nvofapi64.dll is never unloaded. Releasing a registered
// resource after nvOFDestroy crashes at that release (the engine attaches
// per-resource state that Destroy frees). Fences handed to Execute must
// also outlive the Flow object. nvofapi64.dll is never unloaded, on any
// path, once LoadLibrary succeeded. Shutdown() refuses while registrations
// are outstanding; the class is non-copyable and non-movable.
// Output dimensions are ceil(width/grid) x ceil(height/grid).
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

namespace na::nvof {

struct Caps {
    bool available = false;
    uint32_t grid_sizes[8] = {};
    uint32_t grid_count = 0;
    uint32_t min_w = 0, min_h = 0, max_w = 0, max_h = 0;
};
// Loads nvofapi64.dll and asks the driver; false when NVOFA is not present.
bool QueryCaps(ID3D12Device* device, Caps* out, char* error, size_t error_cap);

using BufferHandle = void*;

class Flow {
public:
    Flow() = default;
    ~Flow();
    // Owns raw engine/API pointers: never copied or moved (A36-NVOF-ACK).
    Flow(const Flow&) = delete;
    Flow& operator=(const Flow&) = delete;
    Flow(Flow&&) = delete;
    Flow& operator=(Flow&&) = delete;
    // perf_level: 5 slow/best, 10 medium, 20 fast. grid: 1, 2 or 4 (must be
    // in Caps::grid_sizes). Returns false with LastError() filled on failure.
    // Refuses to re-initialise while registrations are outstanding.
    bool Init(ID3D12Device* device, uint32_t width, uint32_t height, uint32_t grid, uint32_t perf_level);
    // Registers caller-owned resources. `fence` is used for the registration
    // handshake (the API waits for `wait_value` and signals `signal_value`).
    bool RegisterInput(ID3D12Resource* bgra8, ID3D12Fence* fence, uint64_t wait_value, uint64_t signal_value, BufferHandle* out);
    bool RegisterOutput(ID3D12Resource* rg16_sint, ID3D12Fence* fence, uint64_t wait_value, uint64_t signal_value, BufferHandle* out);
    // Returns false (LastError() filled) when the driver refuses; the
    // registration then still counts as outstanding.
    bool Unregister(BufferHandle h);
    // Registrations not yet unregistered. Must be 0 before Shutdown().
    uint32_t OutstandingRegistrations() const { return registered_; }
    // Asynchronous flow computation input -> reference. The engine waits until
    // `wait_fence` >= wait_value and sets `signal_fence` = signal_value when done.
    bool Execute(BufferHandle input_current, BufferHandle reference_previous, BufferHandle output,
                 ID3D12Fence* wait_fence, uint64_t wait_value, ID3D12Fence* signal_fence, uint64_t signal_value,
                 bool disable_temporal_hints);
    // Destroys the engine. Returns false and does NOTHING while registrations
    // are outstanding (destroying then would break the teardown rule). The
    // destructor calls Shutdown() and, if refused, leaks the engine rather
    // than crash; callers verify with OutstandingRegistrations() first.
    bool Shutdown();
    const char* LastError() const { return error_; }
    uint32_t Grid() const { return grid_; }
    uint32_t OutWidth() const { return (width_ + grid_ - 1) / grid_; }
    uint32_t OutHeight() const { return (height_ + grid_ - 1) / grid_; }
    // S10.5 -> pixels.
    static float Decode(int16_t v) { return static_cast<float>(v) / 32.0f; }

private:
    void set_error(const char* what, int status);
    void* api_ = nullptr;      // NV_OF_D3D12_API_FUNCTION_LIST*
    void* handle_ = nullptr;   // NvOFHandle
    void* module_ = nullptr;   // HMODULE
    uint32_t width_ = 0, height_ = 0, grid_ = 1;
    uint32_t registered_ = 0;
    char error_[192] = {};
};

} // namespace na::nvof
