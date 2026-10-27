#pragma once
// Extra-strength sharpening for J2 live DLAA: NVIDIA Image Scaling (NIS) in
// sharpen-only mode (NVIDIAGameWorks/NVIDIAImageScaling v1.0.3, MIT licence,
// vendored in third_party/nis). It runs AFTER RCAS when the Sharpness slider is
// above 100%: RCAS covers 0..100% at AMD's designed maximum, and NIS fades in
// on top for 101..200% (edge-directional, with its own HDR-linear mode).
#include <d3d12.h>
#include <atomic>
#include <cstdint>
#include <memory>

namespace na::j2 {

class NisSharpen {
public:
    // Slider percent (0..200) -> NIS sharpness (0..1) and the fade-in weight
    // that makes 101% start from nothing. Both 0 at or below 100%.
    static void Amount(uint32_t percent, float* sharpness, float* mix);

    // Records NIS from `in` (NON_PIXEL_SHADER_RESOURCE) into `out`
    // (UNORDERED_ACCESS), w x h, typed format `fmt` for both views. `hdr`
    // selects NIS's linear-HDR mode (float colour). `slot` picks a descriptor
    // ring entry so in-flight frames never share descriptors. Returns false
    // (records nothing) when the amount is zero or the pass cannot start.
    bool Dispatch(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* in, ID3D12Resource* out,
                  uint32_t w, uint32_t h, DXGI_FORMAT fmt, bool hdr, uint32_t percent, uint32_t slot);

    // The two shader variants are compiled on a background thread the first
    // time NIS is wanted (D3DCompile of NIS takes long enough to hitch a frame).
    // Until they are ready Dispatch returns false and RCAS alone is shown.
    // Tests call WaitCompiled() to make the next Dispatch deterministic.
    bool WaitCompiled();

    void Reset();
    static constexpr uint32_t kRing = 8;

private:
    struct Compiled; // shared with the compile thread
    bool start_compile();
    bool init(ID3D12Device* device);
    std::shared_ptr<Compiled> compiled_;
    ID3D12Device* device_ = nullptr;
    ID3D12RootSignature* root_ = nullptr;
    ID3D12PipelineState* pso_[2] = {}; // [0] SDR, [1] HDR linear
    ID3D12DescriptorHeap* heap_ = nullptr;
    uint32_t inc_ = 0, block_w_ = 32, block_h_ = 32;
    bool tried_ = false, ok_ = false;
};

} // namespace na::j2
