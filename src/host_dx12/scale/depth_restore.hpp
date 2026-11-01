#pragma once
#include "scene_group.hpp"

namespace na::scale {
// A prepared point-resample draw, not a CopyTextureRegion pretending to scale.
// Reads J2's plain R32_FLOAT depth plane and restores the game's full-size depth
// subresource. Stencil and other slices/mips are preserved. Build on the setup
// worker; descriptors/resources/pipeline are immutable for this pass's lifetime.
class DepthRestore {
public:
    static std::shared_ptr<const DepthRestore> Create(ID3D12Device* device, ID3D12Resource* depth_plane,
        ID3D12Resource* native_depth, uint32_t subresource, std::string& error);
    // Caller transitions plane->PIXEL_SHADER_RESOURCE, target depth->DEPTH_WRITE
    // and (when present) the target's stencil plane->DEPTH_READ,
    // records only outside render passes, restores the game's command-list state,
    // and retains this pass through recording/reset + ALL submitted executions.
    // No waits, descriptor writes, allocation, compilation or readback here.
    bool Record(ID3D12GraphicsCommandList* list) const;
private:
    Microsoft::WRL::ComPtr<ID3D12Resource> plane_, target_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv_, dsv_;
    Extent input_{}, output_{};
};
} // namespace na::scale
