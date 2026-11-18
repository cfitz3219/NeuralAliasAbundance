#pragma once
#include "scene_group.hpp"

namespace na::scale {
// The game's scene depth with the raster jitter taken back out, written into a TWIN of the game's
// depth texture for the game's own shader reads (core/depth_unjitter.hpp says why and is the
// processor reference of the program here; the two are kept in step by hand).
//
// The game's depth is only read. The twin has the game's own description (typeless format, depth
// flag), so a view description the game made for its depth is valid on the twin as it is, and a
// copy the game makes from its depth can be made from the twin.
//
// First form: one mip, one slice, one sample. Anything else is refused with a reason and the game
// keeps reading its own depth, as it does today.
//
// Build under HostWorkScope on the setup worker; descriptors, resources and pipeline do not change
// for this pass's lifetime.
class DepthUnjitter {
public:
    static std::shared_ptr<const DepthUnjitter> Create(ID3D12Device* device, ID3D12Resource* game_depth, std::string& error);
    // The state the twin rests in, from its creation on: readable by any shader stage.
    static constexpr D3D12_RESOURCE_STATES kTwinRest =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    ID3D12Resource* Twin() const { return twin_.Get(); }
    // Record() between the twin's two transitions (rest -> DEPTH_WRITE -> rest). Every recording that
    // refreshes the twin does it this way, so the twin's state is the same at the start and end of each.
    bool RecordRefresh(ID3D12GraphicsCommandList* list, float jitter_x, float jitter_y) const;
    // Caller: game depth's depth plane readable by a pixel shader (PIXEL_SHADER_RESOURCE, with or
    // without DEPTH_READ), twin in DEPTH_WRITE (RecordRefresh does that), outside render passes; restores the game's
    // command-list state afterwards and keeps this pass alive through every execution of the
    // recording. jitter = how far the picture was moved, in pixels (+x right, +y down).
    // No waits, descriptor writes, allocation, compilation or readback here.
    bool Record(ID3D12GraphicsCommandList* list, float jitter_x, float jitter_y) const;
private:
    Microsoft::WRL::ComPtr<ID3D12Resource> depth_, twin_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv_, dsv_;
    Extent size_{};
};
} // namespace na::scale
