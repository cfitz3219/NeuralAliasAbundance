#include "host_dx12/fg/motion_prep.hpp"

#include <d3dcompiler.h>

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace na::fg {
namespace {
constexpr char kShader[] = R"(
Texture2D<float2> motion_in : register(t0);
RWTexture2D<float2> filled : register(u0);
RWTexture2D<float2> for_fg : register(u1);
cbuffer K : register(b0) { uint w, h; float2 scale; float deadzone; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= w || id.y >= h) return;
    float2 v = motion_in[id.xy];
    if (v.x == 0 && v.y == 0) {
        // nearest known vectors on a 9x9 grid, 4 pixels apart (16 render pixels each way), nearest ring first
        float2 sum = 0; float n = 0;
        [loop] for (int r = 1; r <= 4 && n == 0; ++r) {
            [loop] for (int dy = -r; dy <= r; ++dy) {
                [loop] for (int dx = -r; dx <= r; ++dx) {
                    if (max(abs(dx), abs(dy)) != r) continue;
                    int2 q = clamp(int2(id.xy) + int2(dx, dy) * 4, int2(0, 0), int2(w - 1, h - 1));
                    float2 u = motion_in[q];
                    if (u.x != 0 || u.y != 0) { sum += u; n += 1; }
                }
            }
        }
        if (n > 0) v = sum / n;
    }
    filled[id.xy] = v;
    for_fg[id.xy] = length(v * scale) < deadzone ? float2(0, 0) : v;
}
)";
bool texture(ID3D12Device* d, const D3D12_RESOURCE_DESC& like, ComPtr<ID3D12Resource>& out)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC t{};
    t.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    t.Width = like.Width;
    t.Height = like.Height;
    t.DepthOrArraySize = 1;
    t.MipLevels = 1;
    t.Format = like.Format;
    t.SampleDesc.Count = 1;
    t.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return SUCCEEDED(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &t, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&out)));
}
D3D12_RESOURCE_BARRIER move(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER t{};
    t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    t.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
    return t;
}
} // namespace

bool MotionPrep::init(ID3D12Device* device)
{
    if (pso_) return true;
    if (tried_) return false;
    tried_ = true;
    D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 1}};
    D3D12_ROOT_PARAMETER p[2]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[0].DescriptorTable = {2, ranges};
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[1].Constants = {0, 0, 5};
    D3D12_ROOT_SIGNATURE_DESC rd{2, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> sig, err, code;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
        FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root_))))
        return false;
    if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "fg_motion_prep", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err)))
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

bool MotionPrep::Record(ID3D12GraphicsCommandList* list, ID3D12Device* device, ID3D12Resource* in, float sx, float sy, float deadzone, uint32_t page)
{
    if (!list || !device || !in || !init(device)) return false;
    const D3D12_RESOURCE_DESC d = in->GetDesc();
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{};
    fs.Format = d.Format;
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs)) || !(fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)) return false;
    if (filled_) {
        const auto c = filled_->GetDesc();
        if (c.Width != d.Width || c.Height != d.Height || c.Format != d.Format) return false; // the caller retires and releases first
    }
    if (!filled_ && (!texture(device, d, filled_) || !texture(device, d, for_fg_))) { filled_.Reset(); for_fg_.Reset(); return false; }
    auto cpu = heap_->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += SIZE_T(page % kPages) * 3 * inc_;
    auto gpu = heap_->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += UINT64(page % kPages) * 3 * inc_;
    D3D12_SHADER_RESOURCE_VIEW_DESC v{};
    v.Format = d.Format;
    v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    v.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(in, &v, cpu);
    cpu.ptr += inc_;
    ID3D12Resource* outs[2] = {filled_.Get(), for_fg_.Get()};
    for (ID3D12Resource* r : outs) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format = d.Format;
        u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device_->CreateUnorderedAccessView(r, nullptr, &u, cpu);
        cpu.ptr += inc_;
    }
    D3D12_RESOURCE_BARRIER pre[2] = {move(filled_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                                     move(for_fg_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
    list->ResourceBarrier(2, pre);
    ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root_.Get());
    list->SetPipelineState(pso_.Get());
    list->SetComputeRootDescriptorTable(0, gpu);
    struct { uint32_t w, h; float sx, sy, dz; } k{static_cast<uint32_t>(d.Width), d.Height, sx, sy, deadzone};
    list->SetComputeRoot32BitConstants(1, 5, &k, 0);
    list->Dispatch((static_cast<uint32_t>(d.Width) + 7) / 8, (d.Height + 7) / 8, 1);
    D3D12_RESOURCE_BARRIER post[2] = {move(filled_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                                      move(for_fg_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
    list->ResourceBarrier(2, post);
    return true;
}

} // namespace na::fg
