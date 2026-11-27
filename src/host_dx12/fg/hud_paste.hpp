// VARIANT: hudpaste/screen-fixed (see fg/hud_move.hpp for hudpaste/moves-with-scene, the default)
// The HUD of a generated picture, exact by the T2 method (offline 2026-11-25): Frame Generation is
// given the HUD-free picture as its back buffer, so it never warps the HUD; afterwards every pixel
// of the generated picture within one pixel of the newer real frame's HUD is replaced by that real
// frame's pixel. A pixel is HUD where, in display terms, the real frame differs from its HUD-free
// picture by more than 4/255 in any channel (CPU reference: core/live_frame_gen.hpp PasteHud).
// "Display terms" follow how the picture stores its values (na::color::ForStoredPicture): linear
// FP16 is raised to 1/2.2 (the proven rule, unchanged), SDR is already display-encoded, and PQ is
// decoded to the same linear scale first.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

namespace na::fg {

class HudPaste {
public:
    // Compiles the pass (call while nothing is shown with it, so its first use never stalls a frame).
    bool Init(ID3D12Device* device);
    bool Ready() const { return pso_ != nullptr; }
    // Records the paste on `list`. `generated` is in UNORDERED_ACCESS state and stays there; `real`
    // and `hudfree` are in NON_PIXEL_SHADER_RESOURCE and stay there. All three are w x h, the same
    // format family. `slot` picks a descriptor page (< kPages) not used by work still in flight.
    // `encoding` is na::color::ShaderMode(na::color::ForStoredPicture(format, hdr)): 0 SDR, 1 PQ,
    // 2 linear FP16. false when that encoding's variant is not available.
    static constexpr uint32_t kPages = 16;
    static constexpr uint32_t kEncodings = 3, kLinear = 2;
    bool Record(ID3D12GraphicsCommandList* list, ID3D12Resource* generated, ID3D12Resource* real, ID3D12Resource* hudfree,
                uint32_t w, uint32_t h, DXGI_FORMAT format, uint32_t slot, uint32_t encoding = kLinear);

private:
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_; // the linear FP16 variant (Ready() means it built)
    Microsoft::WRL::ComPtr<ID3D12PipelineState> variants_[kEncodings];
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
    uint32_t inc_ = 0;
};

} // namespace na::fg
