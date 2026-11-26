#include "host_dx12/fg/hud_paste.hpp"

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
float3 display(float3 c) { return min(pow(max(c, 0.0), 1.0 / 2.2), 1.0); }
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
DXGI_FORMAT view_format(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return f;
    }
}
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
    if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "hud_paste", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err)))
        return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root_.Get();
    pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3 * kPages, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso))) || FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_))))
        return false;
    inc_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    device_ = device;
    pso_ = pso;
    return true;
}

bool HudPaste::Record(ID3D12GraphicsCommandList* list, ID3D12Resource* generated, ID3D12Resource* real, ID3D12Resource* hudfree,
                      uint32_t w, uint32_t h, DXGI_FORMAT format, uint32_t slot)
{
    if (!pso_ || !list || !generated || !real || !hudfree || !w || !h) return false;
    const DXGI_FORMAT vf = view_format(format);
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
    list->SetPipelineState(pso_.Get());
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
