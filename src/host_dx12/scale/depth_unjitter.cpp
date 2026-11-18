#include "depth_unjitter.hpp"
#include "core/depth_unjitter.hpp"
#include <d3dcompiler.h>
#include <climits>
#include <utility>
#include <cstring>

namespace na::scale {
namespace {
// core/depth_unjitter.hpp, line for line. `precise` keeps the compiler from fusing or reordering
// the arithmetic, so the card decides "continues" on the same numbers as the processor.
constexpr char kSource[] = R"(
Texture2D<float> depth : register(t0);
cbuffer Args : register(b0) { float2 jitter; int2 last; float curve; float noise; };
float4 vs(uint i : SV_VertexID) : SV_Position {
    float2 p = float2((i << 1) & 2, i & 2);
    return float4(p * 2 - 1, 0, 1);
}
bool continues(float c, float n1, float n2, float floor_) {
    precise float first = n1 - c;
    precise float twice = 2.0 * n1;
    precise float second = (c - twice) + n2;
    precise float allowed = curve * abs(first);
    return abs(second) <= max(allowed, floor_);
}
float slope(float m2, float m1, float c, float p1, float p2, float j, bool minus_inside, bool plus_inside) {
    if (j == 0.0) return 0.0;
    precise float floor_ = noise * max(abs(c), max(abs(m1), abs(p1)));
    bool plus = plus_inside && continues(c, p1, p2, floor_);
    bool minus = minus_inside && continues(c, m1, m2, floor_);
    precise float g_plus = p1 - c;
    precise float g_minus = c - m1;
    if (j > 0.0) return plus ? g_plus : (minus ? g_minus : 0.0);
    return minus ? g_minus : (plus ? g_plus : 0.0);
}
float at(int2 p) { return depth.Load(int3(clamp(p, int2(0, 0), last), 0)); }
float ps(float4 position : SV_Position) : SV_Depth {
    int2 p = int2(position.xy);
    float c = at(p);
    float gx = slope(at(p + int2(-2, 0)), at(p + int2(-1, 0)), c, at(p + int2(1, 0)), at(p + int2(2, 0)), jitter.x,
        p.x >= 2, p.x + 2 <= last.x);
    float gy = slope(at(p + int2(0, -2)), at(p + int2(0, -1)), c, at(p + int2(0, 1)), at(p + int2(0, 2)), jitter.y,
        p.y >= 2, p.y + 2 <= last.y);
    precise float mx = jitter.x * gx;
    precise float my = jitter.y * gy;
    precise float r = (c + mx) + my;
    return clamp(r, 0.0, 1.0);
})";
struct Formats { DXGI_FORMAT dsv, srv; };
Formats formats(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: return {DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT};
    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return {DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS};
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: return {DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_R24_UNORM_X8_TYPELESS};
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_D16_UNORM: return {DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_R16_UNORM};
    default: return {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN};
    }
}
bool compile(const char* entry, const char* target, Microsoft::WRL::ComPtr<ID3DBlob>& code) {
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    return SUCCEEDED(D3DCompile(kSource, std::strlen(kSource), "depth with the jitter taken out", nullptr, nullptr,
        entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS, 0, &code, &errors));
}
}
std::shared_ptr<const DepthUnjitter> DepthUnjitter::Create(ID3D12Device* device, ID3D12Resource* depth, std::string& error)
{
    error.clear();
    const auto fail = [&](const char* reason) -> std::shared_ptr<const DepthUnjitter> { error = reason; return {}; };
    if (!device || !depth) return fail("no device or no depth");
    const auto desc = depth->GetDesc();
    const Formats f = formats(desc.Format);
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) ||
        f.dsv == DXGI_FORMAT_UNKNOWN)
        return fail("not a depth texture of a known format");
    if (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) return fail("the game's depth cannot be read by a shader");
    if (desc.DepthOrArraySize != 1 || desc.MipLevels != 1) return fail("depth with several slices or mips");
    if (desc.SampleDesc.Count != 1) return fail("multisampled depth");
    Microsoft::WRL::ComPtr<ID3D12Device> owner;
    Microsoft::WRL::ComPtr<IUnknown> expected, actual;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&expected))) || FAILED(depth->GetDevice(IID_PPV_ARGS(&owner))) ||
        FAILED(owner->QueryInterface(IID_PPV_ARGS(&actual))) || actual.Get() != expected.Get())
        return fail("the depth belongs to another device");

    auto pass = std::shared_ptr<DepthUnjitter>(new DepthUnjitter);
    pass->depth_ = depth;
    pass->size_ = {static_cast<uint32_t>(desc.Width), desc.Height};
    // The twin: the game's own description, so the game's views and copies fit it unchanged.
    D3D12_HEAP_PROPERTIES heap_properties{}; heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC twin = desc; twin.Alignment = 0;
    D3D12_CLEAR_VALUE clear{}; clear.Format = f.dsv;
    if (FAILED(device->CreateCommittedResource(&heap_properties, D3D12_HEAP_FLAG_NONE, &twin, kTwinRest, &clear,
            IID_PPV_ARGS(&pass->twin_))))
        return fail("the twin could not be allocated");
    pass->twin_->SetName(L"NeuralAlias depth, jitter taken out");

    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&pass->srv_)))) return fail("view heap allocation failed");
    heap = {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&pass->dsv_)))) return fail("depth view heap allocation failed");
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = f.srv; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(depth, &sv, pass->srv_->GetCPUDescriptorHandleForHeapStart());
    D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
    dv.Format = f.dsv; dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(pass->twin_.Get(), &dv, pass->dsv_->GetCPUDescriptorHandleForHeapStart());

    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = {1, &range}; params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = {0, 0, 6}; params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rd{}; rd.NumParameters = 2; rd.pParameters = params;
    Microsoft::WRL::ComPtr<ID3DBlob> blob, errors, vs, ps;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
        FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&pass->root_))) ||
        !compile("vs", "vs_5_0", vs) || !compile("ps", "ps_5_0", ps))
        return fail("shader preparation failed");
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
    pd.DSVFormat = f.dsv; pd.SampleDesc = {1, 0};
    if (FAILED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pass->pso_)))) return fail("pipeline preparation failed");
    return pass;
}
bool DepthUnjitter::Record(ID3D12GraphicsCommandList* list, float jitter_x, float jitter_y) const
{
    if (!list || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) return false;
    const na::depth::unjitter::Params defaults;
    struct Args { float jx, jy; int32_t last_x, last_y; float curve, noise; };
    const Args args{jitter_x, jitter_y, static_cast<int32_t>(size_.width) - 1, static_cast<int32_t>(size_.height) - 1, defaults.curve,
        defaults.noise};
    static_assert(sizeof(Args) == 24, "six 32-bit root constants");
    ID3D12DescriptorHeap* heaps[]{srv_.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(root_.Get()); list->SetPipelineState(pso_.Get());
    list->SetGraphicsRootDescriptorTable(0, srv_->GetGPUDescriptorHandleForHeapStart());
    list->SetGraphicsRoot32BitConstants(1, 6, &args, 0);
    const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(size_.width), static_cast<float>(size_.height), 0, 1};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(size_.width), static_cast<LONG>(size_.height)};
    list->RSSetViewports(1, &viewport); list->RSSetScissorRects(1, &scissor);
    const auto target = dsv_->GetCPUDescriptorHandleForHeapStart();
    list->OMSetRenderTargets(0, nullptr, FALSE, &target);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); list->DrawInstanced(3, 1, 0, 0);
    return true;
}
bool DepthUnjitter::RecordRefresh(ID3D12GraphicsCommandList* list, float jitter_x, float jitter_y) const
{
    if (!list || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) return false;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = twin_.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = kTwinRest; b.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    list->ResourceBarrier(1, &b);
    const bool ok = Record(list, jitter_x, jitter_y);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    list->ResourceBarrier(1, &b);
    return ok;
}
} // namespace na::scale
