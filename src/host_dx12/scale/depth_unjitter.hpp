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
    // Every recording that brings the twin up to date does it one of these two ways, so the twin rests
    // in kTwinRest at the start and end of each. Both: a DIRECT recording outside render passes;
    // game_state = the state the game's depth plane is in here (all_planes: the game's barrier named
    // every subresource); this pass must outlive every execution of the recording. No waits,
    // descriptor writes, allocation, compilation or readback.
    //
    // The corrected depth. jitter = how far the picture was moved, in pixels (+x right, +y down);
    // expanded = the scene's one full-size viewport was also widened by half a pixel on each side.
    // Changes the list's graphics pipeline state: the caller hands the game's back afterwards.
    bool RecordRefresh(ID3D12GraphicsCommandList* list, float jitter_x, float jitter_y, bool expanded,
        D3D12_RESOURCE_STATES game_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, bool all_planes = true) const;
    // The game's depth plane as it is (what the game reads today). Touches no pipeline state.
    bool RecordCopy(ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES game_state, bool all_planes) const;
    // The draw alone: game depth's depth plane readable by a pixel shader, twin in DEPTH_WRITE.
    bool Record(ID3D12GraphicsCommandList* list, float jitter_x, float jitter_y, bool expanded = false) const;
private:
    Microsoft::WRL::ComPtr<ID3D12Resource> depth_, twin_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv_, dsv_;
    Extent size_{};
};
} // namespace na::scale
