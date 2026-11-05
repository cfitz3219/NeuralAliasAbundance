#include "host_dx12/background/work.hpp"
#include "host_dx12/j2/nis_scaler.hpp"

#include <d3dcompiler.h>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "NIS_Config.h"          // NVIDIA Image Scaling (MIT), third_party/nis
#include "nis_scaler_embed.hpp"  // NIS_Scaler.h text for run-time compilation

namespace na::j2 {
namespace {
constexpr UINT kCfgDwords = static_cast<UINT>((offsetof(NISConfig, reserved1) + sizeof(float)) / 4);
static_assert(kCfgDwords == 28, "NISConfig layout changed");
template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }
std::string g_error;

std::string build_source(bool hdr, uint32_t bw, uint32_t bh, uint32_t tg)
{
    char defs[512];
    std::snprintf(defs, sizeof defs,
                  "#define NIS_HLSL 1\n#define NIS_SCALER 1\n#define NIS_HDR_MODE %d\n#define NIS_BLOCK_WIDTH %u\n"
                  "#define NIS_BLOCK_HEIGHT %u\n#define NIS_THREAD_GROUP_SIZE %u\n#define NIS_USE_HALF_PRECISION 0\n"
                  "#define NIS_VIEWPORT_SUPPORT 0\n#define NIS_NV12_SUPPORT 0\n#define NIS_CLAMP_OUTPUT 1\n",
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
Texture2D coef_scaler : register(t1);
Texture2D coef_usm : register(t2);
)";
    for (unsigned i = 0; i < na::nis_embed::kScalerChunkCount; ++i) s += na::nis_embed::kScalerChunks[i];
    s += "\n[numthreads(NIS_THREAD_GROUP_SIZE, 1, 1)]\nvoid main(uint3 blockIdx : SV_GroupID, uint3 threadIdx : SV_GroupThreadID)\n"
         "{ NVScaler(blockIdx.xy, threadIdx.x); }\n";
    return s;
}
} // namespace

struct NisScaler::Compiled {
    std::atomic<int> state{0}; // 0 running, 1 ok, -1 failed
    std::vector<uint8_t> code[2];
    uint32_t bw = 32, bh = 24;
    std::string error;
};

bool NisScaler::start_compile()
{
    if (compiled_) return true;
    auto c = std::make_shared<Compiled>();
    NISOptimizer opt(true, NISGPUArchitecture::NVIDIA_Generic);
    c->bw = opt.GetOptimalBlockWidth();
    c->bh = opt.GetOptimalBlockHeight();
    const uint32_t tg = opt.GetOptimalThreadGroupSize();
    try {
        if(!na::background::Submit([c, tg] {
            for (int hdr = 0; hdr < 2; ++hdr) {
                const std::string src = build_source(hdr == 1, c->bw, c->bh, tg);
                ID3DBlob* code = nullptr;
                ID3DBlob* cerr = nullptr;
                const HRESULT hr = D3DCompile(src.data(), src.size(), "j2_nis_scaler.hlsl", nullptr, nullptr, "main", "cs_5_0",
                                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &cerr);
                if (FAILED(hr)) {
                    c->error = "NIS scaler compile failed: ";
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
        }))return false;
    } catch (...) {
        return false;
    }
    compiled_ = c;
    return true;
}

bool NisScaler::WaitCompiled()
{
    if (!start_compile()) return false;
    while (compiled_->state.load() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return compiled_->state.load() == 1;
}

const char* NisScaler::Error() const
{
    if (compiled_ && compiled_->state.load() == -1) return compiled_->error.c_str();
    return g_error.c_str();
}

bool NisScaler::init(ID3D12Device* device)
{
    if (tried_ && device_ == device) return ok_;
    if (device_ && device_ != device) Reset();
    if (!start_compile() || compiled_->state.load() == 0) { device_ = device; return false; }
    tried_ = true;
    device_ = device;
    if (compiled_->state.load() != 1) return false;
    block_w_ = compiled_->bw;
    block_h_ = compiled_->bh;
    D3D12_DESCRIPTOR_RANGE r[2]{};
    r[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    r[0].NumDescriptors = 3; // t0 input, t1 coef_scaler, t2 coef_usm
    r[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    r[1].NumDescriptors = 1;
    r[1].OffsetInDescriptorsFromTableStart = 3;
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
    const HRESULT shr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    HRESULT chr = E_FAIL;
    if (SUCCEEDED(shr)) chr = device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root_));
    release(sig);
    release(err);
    if (FAILED(shr) || FAILED(chr)) { g_error = "NIS scaler root signature failed"; return false; }
    for (int hdr = 0; hdr < 2; ++hdr) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = root_;
        pd.CS = {compiled_->code[hdr].data(), compiled_->code[hdr].size()};
        if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso_[hdr])))) { g_error = "NIS scaler pipeline failed"; return false; }
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kRing * 4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)))) { g_error = "NIS scaler descriptor heap failed"; return false; }
    inc_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    // Coefficient tables: 64 phases x 8 taps = 2 texels of RGBA32F per row.
    constexpr UINT kPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT; // 256 >= 2 * 16 bytes
    D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT}, up{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = kFilterSize / 4;
    td.Height = static_cast<UINT>(kPhaseCount);
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    td.SampleDesc.Count = 1;
    for (auto*& c : coef_)
        if (FAILED(device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&c)))) {
            g_error = "NIS scaler coefficient textures failed";
            return false;
        }
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 2ull * kPitch * kPhaseCount;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload_)))) {
        g_error = "NIS scaler upload buffer failed";
        return false;
    }
    uint8_t* m = nullptr;
    if (FAILED(upload_->Map(0, nullptr, reinterpret_cast<void**>(&m))) || !m) { g_error = "NIS scaler upload map failed"; return false; }
    std::memset(m, 0, static_cast<size_t>(bd.Width));
    for (size_t row = 0; row < kPhaseCount; ++row) {
        std::memcpy(m + row * kPitch, coef_scale[row], sizeof(float) * kFilterSize);
        std::memcpy(m + (kPhaseCount + row) * kPitch, coef_usm[row], sizeof(float) * kFilterSize);
    }
    upload_->Unmap(0, nullptr);
    uploaded_ = false;
    for (UINT i = 0; i < 2; ++i) { // SRVs of the tables in every ring entry (slots +1, +2)
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        for (UINT s = 0; s < kRing; ++s) {
            D3D12_CPU_DESCRIPTOR_HANDLE c = heap_->GetCPUDescriptorHandleForHeapStart();
            c.ptr += static_cast<SIZE_T>(s * 4 + 1 + i) * inc_;
            device->CreateShaderResourceView(coef_[i], &sv, c);
        }
    }
    ok_ = true;
    return true;
}

bool NisScaler::Dispatch(ID3D12Device* device, ID3D12GraphicsCommandList* l, ID3D12Resource* in, DXGI_FORMAT in_fmt, uint32_t iw, uint32_t ih,
                         ID3D12Resource* out, DXGI_FORMAT out_fmt, uint32_t ow, uint32_t oh, bool hdr, float sharpness, uint32_t slot)
{
    if (!device || !l || !in || !out || !iw || !ih || !ow || !oh || !init(device)) return false;
    NISConfig cfg{};
    if (!NVScalerUpdateConfig(cfg, sharpness, 0, 0, iw, ih, iw, ih, 0, 0, ow, oh, ow, oh, hdr ? NISHDRMode::Linear : NISHDRMode::None))
        return false;
    if (!uploaded_) {
        constexpr UINT kPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
        for (UINT i = 0; i < 2; ++i) {
            D3D12_TEXTURE_COPY_LOCATION dst{coef_[i], D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
            D3D12_TEXTURE_COPY_LOCATION src{upload_, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
            src.PlacedFootprint.Offset = static_cast<UINT64>(i) * kPitch * kPhaseCount;
            src.PlacedFootprint.Footprint = {DXGI_FORMAT_R32G32B32A32_FLOAT, static_cast<UINT>(kFilterSize / 4), static_cast<UINT>(kPhaseCount), 1, kPitch};
            l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        D3D12_RESOURCE_BARRIER b[2]{};
        for (UINT i = 0; i < 2; ++i) {
            b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b[i].Transition.pResource = coef_[i];
            b[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            b[i].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        }
        l->ResourceBarrier(2, b);
        uploaded_ = true;
    }
    const UINT base = (slot % kRing) * 4;
    D3D12_CPU_DESCRIPTOR_HANDLE c = heap_->GetCPUDescriptorHandleForHeapStart();
    c.ptr += static_cast<SIZE_T>(base) * inc_;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = in_fmt;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(in, &sv, c);
    c.ptr += static_cast<SIZE_T>(3) * inc_;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.Format = out_fmt;
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
    l->Dispatch((ow + block_w_ - 1) / block_w_, (oh + block_h_ - 1) / block_h_, 1);
    return true;
}

void NisScaler::Reset()
{
    release(pso_[0]);
    release(pso_[1]);
    release(root_);
    release(heap_);
    release(coef_[0]);
    release(coef_[1]);
    release(upload_);
    uploaded_ = false;
    device_ = nullptr;
    tried_ = ok_ = false;
}

} // namespace na::j2
