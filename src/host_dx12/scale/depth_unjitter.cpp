#include "depth_unjitter.hpp"
#include "core/depth_unjitter.hpp"
#include "depth_unjitter_shader.hpp"
#include <d3dcompiler.h>
#include <climits>
#include <utility>
#include <cstring>

namespace na::scale {
namespace {
// core/depth_unjitter.hpp, line for line. `precise` keeps the compiler from fusing or reordering
// the arithmetic, so the card decides "continues" on the same numbers as the processor.
using na::depth::unjitter::kSource;
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
    // The widening of a full-size scene viewport is part of where the picture landed (Astra, PR 162);
    // the term is compiled in and its factor is zero for a scene that was not widened.
    const D3D_SHADER_MACRO definitions[]{{"NA_RASTER_EXPANSION", "1"}, {nullptr, nullptr}};
    return SUCCEEDED(D3DCompile(kSource, std::strlen(kSource), "depth with the jitter taken out", definitions, nullptr,
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
    params[1].Constants = {0, 0, 8}; params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
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
bool DepthUnjitter::Record(ID3D12GraphicsCommandList* list, float jitter_x, float jitter_y, bool expanded) const
{
    if (!list || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) return false;
    const na::depth::unjitter::Params defaults;
    struct Args { float jx, jy; int32_t last_x, last_y; float curve, noise, inverse_width, inverse_height; };
    const Args args{jitter_x, jitter_y, static_cast<int32_t>(size_.width) - 1, static_cast<int32_t>(size_.height) - 1, defaults.curve,
        defaults.noise, expanded ? 1.f / static_cast<float>(size_.width) : 0.f, expanded ? 1.f / static_cast<float>(size_.height) : 0.f};
    static_assert(sizeof(Args) == 32, "eight 32-bit root constants");
    ID3D12DescriptorHeap* heaps[]{srv_.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(root_.Get()); list->SetPipelineState(pso_.Get());
    list->SetGraphicsRootDescriptorTable(0, srv_->GetGPUDescriptorHandleForHeapStart());
    list->SetGraphicsRoot32BitConstants(1, 8, &args, 0);
    const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(size_.width), static_cast<float>(size_.height), 0, 1};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(size_.width), static_cast<LONG>(size_.height)};
    list->RSSetViewports(1, &viewport); list->RSSetScissorRects(1, &scissor);
    const auto target = dsv_->GetCPUDescriptorHandleForHeapStart();
    list->OMSetRenderTargets(0, nullptr, FALSE, &target);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); list->DrawInstanced(3, 1, 0, 0);
    return true;
}
namespace {
D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, UINT subresource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = resource;
    b.Transition.Subresource = subresource; b.Transition.StateBefore = before; b.Transition.StateAfter = after;
    return b;
}
}
bool DepthUnjitter::RecordRefresh(ID3D12GraphicsCommandList* list, float jitter_x, float jitter_y, bool expanded,
    D3D12_RESOURCE_STATES game_state, bool all_planes) const
{
    if (!list || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) return false;
    const UINT planes = all_planes ? D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES : 0;
    // A depth the game made readable for other stages only is lent to the pixel stage for this one draw.
    const bool lend = !(game_state & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    D3D12_RESOURCE_BARRIER b[2]{
        transition(twin_.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, kTwinRest, D3D12_RESOURCE_STATE_DEPTH_WRITE),
        transition(depth_.Get(), planes, game_state, game_state | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)};
    list->ResourceBarrier(lend ? 2 : 1, b);
    const bool ok = Record(list, jitter_x, jitter_y, expanded);
    for (auto& x : b) std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
    list->ResourceBarrier(lend ? 2 : 1, b);
    return ok;
}
bool DepthUnjitter::RecordCopy(ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES game_state, bool) const
{
    if (!list || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) return false;
    // The depth plane alone (subresource 0): the stencil plane may be in another state, and nothing reads the twin's.
    D3D12_RESOURCE_BARRIER b[2]{
        transition(twin_.Get(), 0, kTwinRest, D3D12_RESOURCE_STATE_COPY_DEST),
        transition(depth_.Get(), 0, game_state, D3D12_RESOURCE_STATE_COPY_SOURCE)};
    list->ResourceBarrier(2, b);
    D3D12_TEXTURE_COPY_LOCATION to{}, from{};
    to.pResource = twin_.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; to.SubresourceIndex = 0;
    from.pResource = depth_.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; from.SubresourceIndex = 0;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    for (auto& x : b) std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
    list->ResourceBarrier(2, b);
    return true;
}
} // namespace na::scale
