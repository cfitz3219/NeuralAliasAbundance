// Motion handed to Frame Generation on Route B (2026-11-26 offline studies, NA-fg-offline\shimmer2, thin, steady):
//  1. "Nothing known" vectors (exactly zero: our flow found nothing and no fit covered the pixel) are filled
//     from the nearest known vectors within 16 render pixels (fast-flick capture: generated-picture error
//     -2 %). A pixel with no known neighbour stays zero.
//  2. For Frame Generation only, vectors shorter than one render pixel become exactly zero. With the motion
//     read in render pixels (MvecScale = mv_scale, exact-motion test: error 1.5 against 10.3), the near-still
//     bunker lost fine detail to our sub-pixel flow noise (detail 84.9 % -> 75.8 %, Astra's scorer); the dead
//     zone restores it (84.7 %) and keeps the turn's gain (slow capture 19.72 against 19.71).
// Two outputs: `filled` (step 1, for the still-area steadying's motion test, which was tuned on unclipped
// vectors) and `for_fg` (steps 1 and 2).
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

namespace na::fg {

class MotionPrep {
public:
    // `in` NON_PIXEL_SHADER_RESOURCE (stays). Outputs are made at the input's size and format and left in
    // NON_PIXEL_SHADER_RESOURCE. mv_scale: value * mv_scale = render pixels. false = not done (format without
    // typed UAV stores, or no resources): the caller hands Frame Generation `in` unchanged.
    bool Record(ID3D12GraphicsCommandList* list, ID3D12Device* device, ID3D12Resource* in, float mv_scale_x, float mv_scale_y,
                float deadzone_px, uint32_t page);
    ID3D12Resource* Filled() const { return filled_.Get(); }
    ID3D12Resource* ForFg() const { return for_fg_.Get(); }
    static constexpr uint32_t kPages = 16;
    // The outputs are replaced only when the input's size or format changes; the caller retires the old ones.
    void Release() { filled_.Reset(); for_fg_.Reset(); }

private:
    bool init(ID3D12Device* device);
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
    Microsoft::WRL::ComPtr<ID3D12Resource> filled_, for_fg_;
    uint32_t inc_ = 0;
    bool tried_ = false;
};

} // namespace na::fg
