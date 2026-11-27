#include "host_dx12/fg/hud_paste.hpp"

#include "host_dx12/color/color_space.hpp"

#include <d3dcompiler.h>

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace na::fg {
namespace {
// 16x16 threads; the HUD flag of an 18x18 tile (one-pixel apron) is computed once into shared
// memory, then each pixel looks at its 3x3 neighbourhood. Same rule as the CPU reference.
constexpr char kShader[] = R"(
Texture2D<float4> real_frame : register(t0);
Texture2D<float4> hud_free : register(t1);
RWTexture2D<float4> generated : register(u0);
cbuffer K : register(b0) { uint w, h; };
groupshared uint flags[18 * 18];
// NA_PICTURE (na::color::ShaderMode): 2 linear FP16 (the proven rule), 0 SDR display-encoded
// (the stored value is already in display terms), 1 PQ (decoded to the linear scale of mode 2).
#if NA_PICTURE == 0
float3 display(float3 c) { return saturate(c); }
#elif NA_PICTURE == 1
float3 linear80(float3 e)
{
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 p = pow(saturate(e), 1.0 / m2);
    return pow(max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1) * 125.0;
}
float3 display(float3 c) { return min(pow(max(linear80(c), 0.0), 1.0 / 2.2), 1.0); }
#else
float3 display(float3 c) { return min(pow(max(c, 0.0), 1.0 / 2.2), 1.0); }
#endif
uint hud_at(int2 p)
{
    if (p.x < 0 || p.y < 0 || p.x >= int(w) || p.y >= int(h)) return 0;
    float3 d = abs(display(real_frame[p].rgb) - display(hud_free[p].rgb));
    return max(d.r, max(d.g, d.b)) > 4.0 / 255.0 ? 1u : 0u;
}
[numthreads(16, 16, 1)]
void main(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    int2 origin = int2(gid.xy) * 16 - 1;
    for (uint i = gi; i < 18 * 18; i += 256) flags[i] = hud_at(origin + int2(i % 18, i / 18));
    GroupMemoryBarrierWithGroupSync();
    uint2 p = gid.xy * 16 + tid.xy;
    if (p.x >= w || p.y >= h) return;
    uint any = 0;
    [unroll] for (int dy = 0; dy < 3; ++dy)
        [unroll] for (int dx = 0; dx < 3; ++dx) any |= flags[(tid.y + dy) * 18 + tid.x + dx];
    if (any) generated[p] = real_frame[p];
}
)";
} // namespace

bool HudPaste::Init(ID3D12Device* device)
{
    if (pso_) return true;
    if (!device) return false;
    D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 2}};
    D3D12_ROOT_PARAMETER p[2]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[0].DescriptorTable = {2, ranges};
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[1].Constants = {0, 0, 2};
    D3D12_ROOT_SIGNATURE_DESC rd{2, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> sig, err, code;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
        FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root_))))
        return false;
    // One pipeline per picture encoding. The linear FP16 one (the proven rule) decides whether the
    // pass is usable at all; a failed SDR or PQ variant only leaves that encoding unsupported
    // (its pictures are then not shown, with the reason, as before this change).
    ComPtr<ID3D12PipelineState> made[kEncodings];
    for (uint32_t e = 0; e < kEncodings; ++e) {
        char mode[2] = {char('0' + e), 0};
        const D3D_SHADER_MACRO defines[] = {{"NA_PICTURE", mode}, {nullptr, nullptr}};
        code.Reset();
        err.Reset();
        if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "hud_paste", defines, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err)))
            continue;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = root_.Get();
        pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
        if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&made[e])))) made[e].Reset();
    }
    if (!made[kLinear]) return false;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3 * kPages, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_))))
        return false;
    inc_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    device_ = device;
    for (uint32_t e = 0; e < kEncodings; ++e) variants_[e] = made[e];
    pso_ = made[kLinear];
    return true;
}

bool HudPaste::Record(ID3D12GraphicsCommandList* list, ID3D12Resource* generated, ID3D12Resource* real, ID3D12Resource* hudfree,
                      uint32_t w, uint32_t h, DXGI_FORMAT format, uint32_t slot, uint32_t encoding)
{
    if (!pso_ || !list || !generated || !real || !hudfree || !w || !h || encoding >= kEncodings) return false;
    ID3D12PipelineState* pso = variants_[encoding].Get();
    if (!pso) return false; // this picture encoding's variant did not build (Init kept the others)
    const DXGI_FORMAT vf = na::color::ViewFormat(format);
    auto cpu = heap_->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += SIZE_T(slot % kPages) * 3 * inc_;
    auto gpu = heap_->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += UINT64(slot % kPages) * 3 * inc_;
    ID3D12Resource* srvs[2] = {real, hudfree};
    for (ID3D12Resource* r : srvs) {
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = vf;
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(r, &v, cpu);
        cpu.ptr += inc_;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
    u.Format = vf;
    u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device_->CreateUnorderedAccessView(generated, nullptr, &u, cpu);
    ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root_.Get());
    list->SetPipelineState(pso);
    list->SetComputeRootDescriptorTable(0, gpu);
    const uint32_t k[2] = {w, h};
    list->SetComputeRoot32BitConstants(1, 2, k, 0);
    // The runtime's last writes to `generated` must land before ours.
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = generated;
    list->ResourceBarrier(1, &b);
    list->Dispatch((w + 15) / 16, (h + 15) / 16, 1);
    list->ResourceBarrier(1, &b);
    return true;
}

} // namespace na::fg
