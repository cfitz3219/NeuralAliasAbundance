// Optical-flow object motion for the 64-bit DLSS helper (bridge v4, kFlowMotion).
//
// The same pieces the DX12 host runs inline (src/host_dx12/j2/compute_flow.cpp),
// rebuilt around the helper's own queue instead of the host's recording leases:
//   1. encode: the scene colour into a jitter-stabilised 8-bit image (j2 kFlowShader)
//   2. AMD FidelityFX optical flow on (this image, the previous one) -> 8x8 grid
//   3. FlowExpand: depth-aware expansion to full size, with validity
//   4. DeriveResidual: flow minus the camera motion = object motion
//   5. accept: the Anno-run gates (image check, texture, temporal agreement,
//      AMD scene change), with the same looseness setting as the DX12 host
// The result is the arbiter's tier 3 (residual, valid, mask). Games without a
// camera send the identity reprojection, so the camera motion is zero and the
// accepted flow is the whole motion.
//
// Threading: one helper thread, one direct queue. Every per-frame descriptor
// lives in the caller's ring slot, and the caller waits for that slot's
// previous work before calling Record with it again, so nothing here waits.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>

namespace na::motion::gpu {
struct Inputs;
class Arbiter;
}

namespace na::bridge::flow {

struct Frame {
    ID3D12Resource* color = nullptr; // NON_PIXEL_SHADER_RESOURCE; valid region top-left (w x h)
    ID3D12Resource* depth = nullptr; // R32_FLOAT, NON_PIXEL_SHADER_RESOURCE
    uint32_t w = 0, h = 0;
    uint64_t frame = 0;              // consecutive numbers; a gap starts a new pair
    float jitter[2] = {0, 0};        // render pixels; colour sampled at pixel centre - jitter
    bool hdr = false, reset = false;
    uint32_t loosen = 50;            // 0..100, as compute_flow::SetLoosen
};

struct Tier3 {
    ID3D12Resource *residual = nullptr, *valid = nullptr, *mask = nullptr; // NPSR after Record
};

struct Stats {
    uint32_t engine = 0;             // 0 none, 1 AMD FidelityFX
    uint64_t recorded = 0, pairs = 0, unavailable = 0, resets = 0;
    // Last completed pair, pixel counts: accepted, invalid, small, cost, offscreen, image, flicker.
    uint64_t last[7] = {};
    uint64_t sum[7] = {};            // every completed pair summed
    uint64_t sum_pairs = 0;
    char reason[96] = {};
};

class HelperFlow {
public:
    HelperFlow();
    ~HelperFlow();
    HelperFlow(const HelperFlow&) = delete;
    HelperFlow& operator=(const HelperFlow&) = delete;

    // slots = the caller's ring size (<= 8). Returns false (Stats().reason)
    // when the AMD shaders are not in this build or the GPU cannot run them;
    // the helper then keeps camera motion only.
    bool Init(ID3D12Device* device, uint32_t slots);
    bool Ready() const;

    // Records the whole chain on `list`. `camera` is the arbiter input the
    // helper uses for tier 2 this frame (depth, reprojection, jitter); the
    // residual is derived against exactly that. Returns true and fills `out`
    // when tier 3 is usable this frame (false on the first frame of a pair,
    // after a reset or size change, or when a step refused).
    bool Record(ID3D12GraphicsCommandList* list, uint32_t slot, const Frame& frame,
                const na::motion::gpu::Inputs& camera, Tier3& out);
    // The caller executed the list recorded with Record (the encoded image now
    // exists on the GPU and becomes the next frame's previous image).
    void Submitted(uint32_t slot);
    // Collects the counters of `slot`'s last recording. Call only once the GPU
    // finished that slot (the helper's allocator wait), before Record reuses it.
    void Collect(uint32_t slot);

    const Stats& GetStats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace na::bridge::flow
