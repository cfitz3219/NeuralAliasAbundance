#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <memory>

namespace na::ffx_flow {
// The pinned search shader reduces a 64-thread block with one wave64 or two
// wave32 groups. WaveOps alone is insufficient (WARP also exposes smaller waves).
bool DeviceSupported(ID3D12Device*);
// AMD FSR3-3.0.3 optical-flow shaders, unmodified. Explicit image pair:
// no CPU-side temporal state advances when a recorded list is discarded.
// Caller serializes recordings/submission on ONE direct queue, keeps this
// object/images alive through submission, and fences descriptor slots before reuse.
// Input: matching display-encoded RGB textures, NON_PIXEL_SHADER_RESOURCE.
// Output: 8x8 blocks, R16G16_SINT INTEGRAL PIXELS, current -> previous (NOT NVOFA S10.5).
class PairFlow {
public:
    PairFlow(); ~PairFlow();
    PairFlow(const PairFlow&)=delete;PairFlow& operator=(const PairFlow&)=delete;
    bool Init(ID3D12Device*,unsigned width,unsigned height,unsigned slots=8);
    bool Record(ID3D12GraphicsCommandList*,ID3D12Resource* current,ID3D12Resource* previous,unsigned slot);
    void Finish(ID3D12GraphicsCommandList*); // after consumers: return outputs to UAV, so discarded/replayed lists have no CPU state
    ID3D12Resource* Grid() const; // NPSR after Record, valid until next Record on this queue
    ID3D12Resource* SceneChange() const; // R32_UINT 3x1, NPSR; AMD's scene-change/history output
    const char* Error() const;
private:
    struct Impl;std::unique_ptr<Impl> p;
};
}
