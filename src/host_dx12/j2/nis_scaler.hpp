#pragma once
// NVIDIA Image Scaling (NIS) upscaler (NIS_SCALER 1, NVIDIAGameWorks/NVIDIAImageScaling
// v1.0.3, MIT, vendored in third_party/nis). Render-small uses it for the game's HUD only:
// HUD pixels are kept out of DLSS and upscaled with NIS's edge-adaptive scaler instead.
#include <d3d12.h>
#include <dxgiformat.h>
#include <atomic>
#include <cstdint>
#include <memory>

namespace na::j2 {

class NisScaler {
public:
    // Records NIS from `in` (iw x ih, NON_PIXEL_SHADER_RESOURCE) into `out` (ow x oh,
    // UNORDERED_ACCESS). `sharpness` 0..1. The first call on a device also records the
    // upload of NIS's coefficient tables into `l`. Returns false (records nothing) while
    // the shader is still compiling in the background or if the pass cannot start.
    bool Dispatch(ID3D12Device* device, ID3D12GraphicsCommandList* l, ID3D12Resource* in, DXGI_FORMAT in_fmt, uint32_t iw, uint32_t ih,
                  ID3D12Resource* out, DXGI_FORMAT out_fmt, uint32_t ow, uint32_t oh, bool hdr, float sharpness, uint32_t slot);
    bool WaitCompiled();      // tests
    const char* Error() const; // empty when fine
    void Reset();
    static constexpr uint32_t kRing = 8;

private:
    struct Compiled;
    bool start_compile();
    bool init(ID3D12Device* device);
    std::shared_ptr<Compiled> compiled_;
    ID3D12Device* device_ = nullptr;
    ID3D12RootSignature* root_ = nullptr;
    ID3D12PipelineState* pso_[2] = {}; // [0] SDR, [1] HDR linear
    ID3D12DescriptorHeap* heap_ = nullptr;
    ID3D12Resource* coef_[2] = {};     // scaler, usm: R32G32B32A32_FLOAT 2 x 64
    ID3D12Resource* upload_ = nullptr; // kept until Reset (the upload may still be in flight)
    bool uploaded_ = false;
    uint32_t inc_ = 0, block_w_ = 32, block_h_ = 24;
    bool tried_ = false, ok_ = false;
};

} // namespace na::j2
