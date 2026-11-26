// Still-area steadying for x2 Frame Generation (Astra, research/fg-wire-shimmer, PR 202, 2026-11-26).
//
// On the generated world picture, before the HUD paste: where the motion around a pixel is under
// one display pixel AND the two real HUD-free frames agree on the local brightness pattern, the
// generated pixel is blended toward the two real frames' linear midpoint. The weight falls to
// zero as either test fails, so moving edges and changing areas keep the generated picture.
// Measured by Astra on the bunker cable (capture j2-20261126-023013): broad error 32-96 px from the
// cable down ~47 %, 0.171 ms median on an RTX 5070 Ti at 3440x1440; no effect in turns.
//
// Conditions (her README): linear FP16 colour only (the agreement test assumes linear light); the
// two frames are consecutive and the same size; typed UAV loads must be supported for the format;
// the motion is decoded to render pixels by the caller's scale and the pass converts to display
// pixels. The shader logic is hers, unchanged in substance; only the motion decode (`mv_scale`)
// was added so the live motion's units are used as they are.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

namespace na::fg {

class SteadyPass {
public:
    // false + reason (plain words) when this device or format cannot run it.
    bool Init(ID3D12Device* device, DXGI_FORMAT format, char* reason, size_t reason_size);
    bool Ready() const { return pso_ != nullptr; }
    static constexpr uint32_t kPages = 16;
    // `generated` UNORDERED_ACCESS (stays), `older`/`newer` (the real HUD-free frames before and after)
    // and `motion` (render size; value * mv_scale = render pixels) NON_PIXEL_SHADER_RESOURCE (stay).
    bool Record(ID3D12GraphicsCommandList* list, ID3D12Resource* generated, ID3D12Resource* older, ID3D12Resource* newer,
                ID3D12Resource* motion, uint32_t w, uint32_t h, float mv_scale_x, float mv_scale_y, uint32_t page);
    // Pixels the last Record on `page` changed (weight > 0). Call only after the GPU finished that
    // Record (its list's fence passed). false when not counted (no ID3D12GraphicsCommandList2).
    bool ReadChanged(uint32_t page, uint32_t* changed);
    // Test seam: the HLSL compiled by Init (so a test can run the same text against a reference).
    static const char* Source();

private:
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
    Microsoft::WRL::ComPtr<ID3D12Resource> counter_, counter_readback_; // UNORDERED_ACCESS / COPY_DEST at rest
    bool counted_[kPages] = {};
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    uint32_t inc_ = 0;
};

} // namespace na::fg
