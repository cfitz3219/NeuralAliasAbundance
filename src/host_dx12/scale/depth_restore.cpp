#include "depth_restore.hpp"
#include <d3dcompiler.h>
#include <algorithm>
#include <climits>
#include <cstring>

namespace na::scale {
namespace {
constexpr char kSource[] = R"(
Texture2D<float> plane : register(t0);
cbuffer Sizes : register(b0) { uint2 source_size; uint2 destination_size; };
float4 vs(uint i : SV_VertexID) : SV_Position {
    float2 p = float2((i << 1) & 2, i & 2);
    return float4(p * 2 - 1, 0, 1);
}
float ps(float4 position : SV_Position) : SV_Depth {
    uint2 p = min(uint2(position.xy * float2(source_size) / destination_size), source_size - 1);
    return plane.Load(int3(p, 0));
})";
DXGI_FORMAT depth_format(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_D32_FLOAT;
    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_D16_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}
bool compile(const char* entry, const char* target, Microsoft::WRL::ComPtr<ID3DBlob>& code) {
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    return SUCCEEDED(D3DCompile(kSource, std::strlen(kSource), "Layer2 depth restoration", nullptr, nullptr,
        entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors));
}
}
std::shared_ptr<const DepthRestore> DepthRestore::Create(ID3D12Device* device, ID3D12Resource* plane,
    ID3D12Resource* target, uint32_t subresource, std::string& error)
{
    error.clear();
    const auto fail = [&](const char* reason) -> std::shared_ptr<const DepthRestore> { error = reason; return {}; };
    if (!device || !plane || !target || plane == target) return fail("invalid depth restoration resources");
    const auto src = plane->GetDesc(), dst = target->GetDesc();
    const auto format = depth_format(dst.Format);
    if (src.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || src.Format != DXGI_FORMAT_R32_FLOAT ||
        src.DepthOrArraySize != 1 || src.MipLevels != 1 || src.SampleDesc.Count != 1 ||
        (src.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) ||
        dst.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || !dst.MipLevels ||
        !(dst.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) || format == DXGI_FORMAT_UNKNOWN ||
        subresource >= uint32_t(dst.MipLevels) * dst.DepthOrArraySize)
        return fail("depth restoration requires a plain depth plane and a depth-plane-zero destination");
    Microsoft::WRL::ComPtr<IUnknown> expected;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&expected)))) return fail("device identity unavailable");
    for (auto* resource : {plane, target}) {
        Microsoft::WRL::ComPtr<ID3D12Device> owner;
        Microsoft::WRL::ComPtr<IUnknown> actual;
        if (FAILED(resource->GetDevice(IID_PPV_ARGS(&owner))) ||
            FAILED(owner->QueryInterface(IID_PPV_ARGS(&actual))) || actual.Get() != expected.Get())
            return fail("depth restoration resources belong to different devices");
    }
    auto pass = std::shared_ptr<DepthRestore>(new DepthRestore);
    pass->plane_ = plane; pass->target_ = target;
    pass->input_ = {static_cast<uint32_t>(src.Width), src.Height};
    const UINT mip = subresource % dst.MipLevels, slice = subresource / dst.MipLevels;
    pass->output_ = {static_cast<uint32_t>(std::max<UINT64>(1, dst.Width >> mip)), std::max(1u, dst.Height >> mip)};
    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&pass->srv_)))) return fail("depth SRV heap allocation failed");
    heap = {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&pass->dsv_)))) return fail("depth DSV heap allocation failed");
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R32_FLOAT; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(plane, &sv, pass->srv_->GetCPUDescriptorHandleForHeapStart());
    D3D12_DEPTH_STENCIL_VIEW_DESC dv{}; dv.Format = format;
    if (format == DXGI_FORMAT_D24_UNORM_S8_UINT || format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
        dv.Flags = D3D12_DSV_FLAG_READ_ONLY_STENCIL;
    if (dst.SampleDesc.Count > 1) {
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
        dv.Texture2DMSArray.FirstArraySlice = slice; dv.Texture2DMSArray.ArraySize = 1;
    } else {
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dv.Texture2DArray.MipSlice = mip; dv.Texture2DArray.FirstArraySlice = slice; dv.Texture2DArray.ArraySize = 1;
    }
    device->CreateDepthStencilView(target, &dv, pass->dsv_->GetCPUDescriptorHandleForHeapStart());
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = {1, &range}; params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = {0, 0, 4}; params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rd{}; rd.NumParameters = 2; rd.pParameters = params;
    Microsoft::WRL::ComPtr<ID3DBlob> blob, errors, vs, ps;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
        FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&pass->root_))) ||
        !compile("vs", "vs_5_0", vs) || !compile("ps", "ps_5_0", ps)) return fail("depth restoration shader preparation failed");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = pass->root_.Get(); pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.DepthStencilState.DepthEnable = TRUE; pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pd.DepthStencilState.StencilEnable = FALSE;
    pd.DepthStencilState.FrontFace = pd.DepthStencilState.BackFace = {
        D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    pd.SampleMask = UINT_MAX; pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.DSVFormat = format; pd.SampleDesc = dst.SampleDesc;
    if (FAILED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pass->pso_)))) return fail("depth restoration pipeline preparation failed");
    return pass;
}
bool DepthRestore::Record(ID3D12GraphicsCommandList* list) const {
    if (!list || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) return false;
    ID3D12DescriptorHeap* heaps[]{srv_.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(root_.Get()); list->SetPipelineState(pso_.Get());
    list->SetGraphicsRootDescriptorTable(0, srv_->GetGPUDescriptorHandleForHeapStart());
    const uint32_t sizes[]{input_.width, input_.height, output_.width, output_.height};
    list->SetGraphicsRoot32BitConstants(1, 4, sizes, 0);
    const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(output_.width), static_cast<float>(output_.height), 0, 1};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(output_.width), static_cast<LONG>(output_.height)};
    list->RSSetViewports(1, &viewport); list->RSSetScissorRects(1, &scissor);
    const auto target = dsv_->GetCPUDescriptorHandleForHeapStart();
    list->OMSetRenderTargets(0, nullptr, FALSE, &target);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); list->DrawInstanced(3, 1, 0, 0);
    return true;
}
} // namespace na::scale
