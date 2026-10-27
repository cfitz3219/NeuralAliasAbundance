#include "host_dx12/j2/nis_sharpen.hpp"

#include <d3dcompiler.h>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <chrono>
#include <thread>
#include <vector>

#include "NIS_Config.h"          // NVIDIA Image Scaling (MIT), third_party/nis
#include "nis_scaler_embed.hpp"  // NIS_Scaler.h text for run-time compilation

namespace na::j2 {
namespace {
// NISConfig is declared align(256), so sizeof() is 256 bytes (64 DWORDs: over the
// root-signature limit). Only its 28 fields are root constants.
constexpr UINT kCfgDwords = static_cast<UINT>((offsetof(NISConfig, reserved1) + sizeof(float)) / 4);
static_assert(kCfgDwords == 28, "NISConfig layout changed");
template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

// NIS sharpen-only (NIS_SCALER 0). The cbuffer mirrors NISConfig exactly (NIS_Main.hlsl);
// reserved0 carries our fade-in weight, applied at NIS's single store.
std::string build_source(bool hdr, uint32_t bw, uint32_t bh, uint32_t tg, bool* patched)
{
    char defs[512];
    std::snprintf(defs, sizeof defs,
                  "#define NIS_HLSL 1\n#define NIS_SCALER 0\n#define NIS_HDR_MODE %d\n#define NIS_BLOCK_WIDTH %u\n"
                  "#define NIS_BLOCK_HEIGHT %u\n#define NIS_THREAD_GROUP_SIZE %u\n#define NIS_USE_HALF_PRECISION 0\n"
                  "#define NIS_VIEWPORT_SUPPORT 0\n#define NIS_NV12_SUPPORT 0\n#define NIS_CLAMP_OUTPUT 0\n",
                  hdr ? 1 : 0, bw, bh, tg);
    std::string s = defs;
    s += R"(cbuffer cb : register(b0) {
    float kDetectRatio; float kDetectThres; float kMinContrastRatio; float kRatioNorm;
    float kContrastBoost; float kEps; float kSharpStartY; float kSharpScaleY;
    float kSharpStrengthMin; float kSharpStrengthScale; float kSharpLimitMin; float kSharpLimitScale;
    float kScaleX; float kScaleY;
    float kDstNormX; float kDstNormY; float kSrcNormX; float kSrcNormY;
    uint kInputViewportOriginX; uint kInputViewportOriginY; uint kInputViewportWidth; uint kInputViewportHeight;
    uint kOutputViewportOriginX; uint kOutputViewportOriginY; uint kOutputViewportWidth; uint kOutputViewportHeight;
    float reserved0; float reserved1;
};
SamplerState samplerLinearClamp : register(s0);
Texture2D in_texture : register(t0);
RWTexture2D<float4> out_texture : register(u0);
)";
    std::string scaler;
    for (unsigned i = 0; i < na::nis_embed::kScalerChunkCount; ++i) scaler += na::nis_embed::kScalerChunks[i];
    // Fade-in: blend NIS's result with its input by reserved0 (1 = NIS as designed).
    const std::string from = "NVTEX_STORE(out_texture, dstCoord, NVCLAMP(op));";
    const std::string to = "NVTEX_STORE(out_texture, dstCoord, lerp(NVTEX_SAMPLE(in_texture, samplerLinearClamp, coord), NVCLAMP(op), reserved0));";
    size_t n = 0;
    for (size_t at = scaler.find(from); at != std::string::npos; at = scaler.find(from, at + to.size())) {
        scaler.replace(at, from.size(), to);
        ++n;
    }
    *patched = n > 0;
    s += scaler;
    s += "\n[numthreads(NIS_THREAD_GROUP_SIZE, 1, 1)]\nvoid main(uint3 blockIdx : SV_GroupID, uint3 threadIdx : SV_GroupThreadID)\n"
         "{ NVSharpen(blockIdx.xy, threadIdx.x); }\n";
    return s;
}
} // namespace

struct NisSharpen::Compiled {
    std::atomic<int> state{0}; // 0 running, 1 ok, -1 failed
    std::vector<uint8_t> code[2];
    uint32_t bw = 32, bh = 32;
    std::string error; // written by the compile thread before state is set
};
namespace { std::string g_nis_error; }

bool NisSharpen::start_compile()
{
    if (compiled_) return true;
    auto c = std::make_shared<Compiled>();
    NISOptimizer opt(false, NISGPUArchitecture::NVIDIA_Generic);
    c->bw = opt.GetOptimalBlockWidth();
    c->bh = opt.GetOptimalBlockHeight();
    const uint32_t tg = opt.GetOptimalThreadGroupSize();
    try {
        std::thread([c, tg] {
            for (int hdr = 0; hdr < 2; ++hdr) {
                bool patched = false;
                const std::string src = build_source(hdr == 1, c->bw, c->bh, tg, &patched);
                ID3DBlob* code = nullptr;
                ID3DBlob* cerr = nullptr;
                const HRESULT hr = patched ? D3DCompile(src.data(), src.size(), "j2_nis_sharpen.hlsl", nullptr, nullptr, "main", "cs_5_0",
                                                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &cerr)
                                           : E_FAIL; // the vendored NIS text no longer has the store we wrap
                if (FAILED(hr)) {
                    c->error = patched ? "NIS shader compile failed: " : "NIS source no longer has the wrapped store";
                    if (cerr) c->error.append(static_cast<const char*>(cerr->GetBufferPointer()), cerr->GetBufferSize());
                    OutputDebugStringA(c->error.c_str());
                }
                release(cerr);
                if (FAILED(hr) || !code) { release(code); c->state.store(-1); return; }
                const uint8_t* b = static_cast<const uint8_t*>(code->GetBufferPointer());
                c->code[hdr].assign(b, b + code->GetBufferSize());
                release(code);
            }
            c->state.store(1);
        }).detach();
    } catch (...) {
        return false;
    }
    compiled_ = c;
    return true;
}

bool NisSharpen::WaitCompiled()
{
    if (!start_compile()) return false;
    while (compiled_->state.load() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return compiled_->state.load() == 1;
}

const char* NisSharpen::Error() const
{
    if (compiled_ && compiled_->state.load() == -1) return compiled_->error.c_str();
    return g_nis_error.c_str();
}

void NisSharpen::Amount(uint32_t percent, float* sharpness, float* mix)
{
    const float t = percent <= 100 ? 0.f : (percent >= 200 ? 1.f : (percent - 100) / 100.f);
    *sharpness = t;
    // NIS at its own 0% still sharpens a little; fading it in over the first
    // quarter keeps 100% -> 101% continuous.
    *mix = t >= 0.25f ? 1.f : t * 4.f;
}

bool NisSharpen::init(ID3D12Device* device)
{
    if (tried_ && device_ == device) return ok_;
    if (device_ != device) { release(pso_[0]); release(pso_[1]); release(root_); release(heap_); tried_ = ok_ = false; }
    if (!start_compile() || compiled_->state.load() == 0) { device_ = device; return false; } // still compiling: RCAS only
    tried_ = true;
    device_ = device;
    if (compiled_->state.load() != 1) return false;
    block_w_ = compiled_->bw;
    block_h_ = compiled_->bh;

    D3D12_DESCRIPTOR_RANGE r[2]{};
    r[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    r[0].NumDescriptors = 1;
    r[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    r[1].NumDescriptors = 1;
    r[1].OffsetInDescriptorsFromTableStart = 1;
    D3D12_ROOT_PARAMETER p[2]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[0].DescriptorTable.NumDescriptorRanges = 2;
    p[0].DescriptorTable.pDescriptorRanges = r;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[1].Constants.Num32BitValues = kCfgDwords;
    D3D12_STATIC_SAMPLER_DESC ss{};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.MaxLOD = D3D12_FLOAT32_MAX;
    ss.MaxAnisotropy = 1;
    ss.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rd{2, p, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ID3DBlob* sig = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT shr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    HRESULT chr = E_FAIL;
    if (SUCCEEDED(shr)) chr = device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root_));
    if (FAILED(shr) || FAILED(chr)) {
        char m[96];
        std::snprintf(m, sizeof m, "NIS root signature failed (serialize 0x%08X, create 0x%08X) ", static_cast<unsigned>(shr), static_cast<unsigned>(chr));
        g_nis_error = m;
        if (err) g_nis_error.append(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize());
    }
    release(sig);
    release(err);
    if (FAILED(shr) || FAILED(chr)) return false;
    for (int hdr = 0; hdr < 2; ++hdr) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = root_;
        pd.CS = {compiled_->code[hdr].data(), compiled_->code[hdr].size()};
        if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso_[hdr])))) { g_nis_error = "NIS pipeline creation failed"; return false; }
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kRing * 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)))) { g_nis_error = "NIS descriptor heap failed"; return false; }
    inc_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ok_ = true;
    return true;
}

bool NisSharpen::Dispatch(ID3D12Device* device, ID3D12GraphicsCommandList* l, ID3D12Resource* in, ID3D12Resource* out,
                          uint32_t w, uint32_t h, DXGI_FORMAT fmt, bool hdr, uint32_t percent, uint32_t slot)
{
    float sharpness = 0.f, mix = 0.f;
    Amount(percent, &sharpness, &mix);
    if (mix <= 0.f || !device || !l || !in || !out || !w || !h || !init(device)) return false;
    NISConfig cfg{};
    if (!NVSharpenUpdateConfig(cfg, sharpness, 0, 0, w, h, w, h, 0, 0, hdr ? NISHDRMode::Linear : NISHDRMode::None)) return false;
    cfg.reserved0 = mix;
    const UINT base = (slot % kRing) * 2;
    D3D12_CPU_DESCRIPTOR_HANDLE c = heap_->GetCPUDescriptorHandleForHeapStart();
    c.ptr += static_cast<SIZE_T>(base) * inc_;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = fmt;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(in, &sv, c);
    c.ptr += inc_;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.Format = fmt;
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(out, nullptr, &uv, c);
    D3D12_GPU_DESCRIPTOR_HANDLE gh = heap_->GetGPUDescriptorHandleForHeapStart();
    gh.ptr += static_cast<UINT64>(base) * inc_;
    ID3D12DescriptorHeap* heaps[] = {heap_};
    l->SetDescriptorHeaps(1, heaps);
    l->SetComputeRootSignature(root_);
    l->SetPipelineState(pso_[hdr ? 1 : 0]);
    l->SetComputeRootDescriptorTable(0, gh);
    l->SetComputeRoot32BitConstants(1, kCfgDwords, &cfg, 0);
    l->Dispatch((w + block_w_ - 1) / block_w_, (h + block_h_ - 1) / block_h_, 1);
    return true;
}

void NisSharpen::Reset()
{
    release(pso_[0]);
    release(pso_[1]);
    release(root_);
    release(heap_);
    device_ = nullptr;
    tried_ = ok_ = false; // compiled_ (device-independent bytecode) is kept
}

} // namespace na::j2
