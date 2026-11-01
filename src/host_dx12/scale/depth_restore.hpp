#pragma once
#include "scene_group.hpp"

namespace na::scale {
// A prepared point-resample draw, not a CopyTextureRegion pretending to scale.
// Reads J2's plain R32_FLOAT depth plane and restores the game's full-size depth
// subresource. Stencil and other slices/mips are preserved. Build on the setup
// worker under HostWorkScope; descriptors/resources/pipeline are immutable for
// this pass's lifetime. No game resource/view is changed during preparation.
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
    friend class ColorRestore;
    static std::shared_ptr<const DepthRestore> CreateInternal(ID3D12Device*, ID3D12Resource*, ID3D12Resource*,
        uint32_t subresource, bool color, DXGI_FORMAT color_view, std::string& error);
    Microsoft::WRL::ComPtr<ID3D12Resource> plane_, target_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv_, dsv_;
    Extent input_{}, output_{};
    bool color_ = false;
};
// Full-output spatial bypass for a reduced scene; explicitly not DLSS. Setup,
// resource-state, restoration and recording-lifetime rules match DepthRestore.
class ColorRestore {
public:
    static std::shared_ptr<const ColorRestore> Create(ID3D12Device*, ID3D12Resource* plain_color,
        ID3D12Resource* native_target, uint32_t subresource, DXGI_FORMAT view_format, std::string& error);
    bool Record(ID3D12GraphicsCommandList* list) const { return pass_->Record(list); }
private:
    std::shared_ptr<const DepthRestore> pass_;
};
} // namespace na::scale
