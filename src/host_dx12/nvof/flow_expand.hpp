// Expands the NVOFA output grid (R16G16_SINT, S10.5 fixed point, one vector
// per gridÃ—grid block; NVOFA-specific decode Ã·32) into the MotionArbiter's
// raw-flow texture (RG32_FLOAT, full resolution, pixels, p_previous âˆ’
// p_current).
//
// Two modes:
//  - Plain: bilinear between block centres (no depth). Bleeds motion across
//    depth edges (A37-MOTION-CHAIN-ACK); use only when no depth exists.
//  - Depth-aware (recommended): the four neighbouring block vectors are
//    weighted by bilinear weight Ã— depth similarity between THIS pixel's raw
//    depth and the raw depth at each block centre, so a background pixel next
//    to a moving foreground takes the background block's vector. A validity
//    texture (arbiter mask domain: 1 valid, 0 not usable) is written: 0 when
//    no neighbour block matches the pixel's depth, or when the depth-matching
//    neighbours disagree by more than `consistency_px` (flow inconsistency at
//    disocclusions), or when the pixel's depth is the clear value. Every
//    neighbouring block-centre depth is validated with the same
//    finite / [0,1] / not-clear rule before it can contribute
//    (A39-DEPTH-EXPAND-REVIEW R1).
// Parameters are validated before recording (R2): tolerance and
// consistency finite and >= 0, clear depth finite in [0,1]; otherwise
// DispatchDepthAware returns false and records nothing.
// CONSUMER CONTRACT: the validity output must be consumed. A pixel can hold a
// finite vector with validity 0; feed the validity as `raw_valid` to
// Arbiter::DispatchDeriveResidual so it is folded into the derived validity,
// and combine any supplier validity the same way. "Finite" never means
// "trusted".
// Dispatch records only; caller owns resources and barriers:
//   grid and depth in NON_PIXEL_SHADER_RESOURCE, outputs in UNORDERED_ACCESS.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

namespace na::nvof {

struct ExpandParams {
    ID3D12Resource* depth = nullptr;      // R32_FLOAT raw z/w, full resolution (optional)
    ID3D12Resource* validity_out = nullptr; // R32_FLOAT, full resolution (optional; requires depth)
    float depth_clear = 1.0f;
    float depth_tolerance = 0.02f;        // |Î” raw depth| accepted as "same surface" (relative to |depth|+0.01)
    float consistency_px = 2.0f;          // max spread among depth-matching neighbours before validity = 0
};

class FlowExpand {
public:
    bool Init(ID3D12Device* device, uint32_t max_frames_in_flight = 4);
    // A58: when the engine ran on an image downscaled by `s` (block size in
    // output pixels = grid * s), decoded vectors are multiplied by `s`.
    // Rejects non-finite or non-positive values (keeps the previous scale).
    bool SetVectorScale(float s)
    {
        if (!(s == s) || s <= 0.0f || s - s != 0.0f) return false;
        vector_scale_ = s;
        return true;
    }
    // Plain bilinear expansion.
    bool Dispatch(ID3D12GraphicsCommandList* list, ID3D12Resource* grid_rg16_sint, uint32_t grid,
                  ID3D12Resource* out_rg32f, uint32_t out_w, uint32_t out_h, uint32_t frame_slot);
    // Depth-aware expansion with validity.
    bool DispatchDepthAware(ID3D12GraphicsCommandList* list, ID3D12Resource* grid_rg16_sint, uint32_t grid,
                            const ExpandParams& params, ID3D12Resource* out_rg32f, uint32_t out_w, uint32_t out_h,
                            uint32_t frame_slot);

private:
    bool dispatch(ID3D12GraphicsCommandList* list, ID3D12Resource* grid_tex, uint32_t grid, const ExpandParams* params,
                  ID3D12Resource* out, uint32_t out_w, uint32_t out_h, uint32_t frame_slot);
    float vector_scale_ = 1.0f;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_plain_, pso_depth_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
    Microsoft::WRL::ComPtr<ID3D12Resource> dummy_srv_, dummy_uav_;
    UINT stride_ = 0;
    uint32_t slots_ = 0;
};

} // namespace na::nvof
