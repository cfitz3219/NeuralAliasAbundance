#include "depth_reads.hpp"
#include "core/depth_unjitter.hpp"
#include "host_dx12/scale/depth_unjitter_shader.hpp"
#include <d3dcompiler.h>
#include <cmath>

namespace na::native11 {
namespace {
using Microsoft::WRL::ComPtr;

struct Formats { DXGI_FORMAT dsv, srv; };
Formats FormatsFor(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_R16_TYPELESS: return {DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_R16_UNORM};
    case DXGI_FORMAT_R24G8_TYPELESS: return {DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_R24_UNORM_X8_TYPELESS};
    case DXGI_FORMAT_R32_TYPELESS: return {DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT};
    case DXGI_FORMAT_R32G8X24_TYPELESS: return {DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS};
    default: return {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN};
    }
}

bool Compile(const char* entry, const char* profile, ComPtr<ID3DBlob>& code)
{
    const D3D_SHADER_MACRO definitions[]{{"NA_RASTER_EXPANSION", "1"}, {nullptr, nullptr}};
    const auto& shader = na::depth::unjitter::kSource;
    return SUCCEEDED(D3DCompile(shader, sizeof shader - 1, "depth_unjitter", definitions, nullptr,
        entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS, 0, &code, nullptr));
}

struct Constants {
    float jitter_x, jitter_y;
    int last_x, last_y;
    float curve, noise, inverse_width, inverse_height;
};
static_assert(sizeof(Constants) == 32);

template<class Shader>
struct ShaderState {
    ComPtr<Shader> shader;
    ID3D11ClassInstance* instances[256]{};
    UINT count = ARRAYSIZE(instances);
    ~ShaderState()
    {
        for (UINT index = 0; index < count; ++index)
            if (instances[index]) instances[index]->Release();
    }
};

struct DrawState {
    ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    UINT target_count = 0;
    ComPtr<ID3D11DepthStencilView> depth;
    ComPtr<ID3D11DepthStencilState> depth_state;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11ShaderResourceView> input;
    ShaderState<ID3D11VertexShader> vs;
    ShaderState<ID3D11PixelShader> ps;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT viewport_count = ARRAYSIZE(viewports);
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    UINT stencil = 0, mask = 0, first_constant = 0, constant_count = 0;
    float blend_factor[4]{};
    ComPtr<ID3D11DeviceContext1> context1;

    ~DrawState()
    {
        for (auto* view : targets) if (view) view->Release();
    }
    bool Save(ID3D11DeviceContext* context, D3D_FEATURE_LEVEL level)
    {
        ComPtr<ID3D11GeometryShader> geometry;
        ComPtr<ID3D11HullShader> hull;
        ComPtr<ID3D11DomainShader> domain;
        ComPtr<ID3D11Predicate> predicate;
        BOOL predicated = FALSE;
        context->GSGetShader(&geometry, nullptr, nullptr);
        context->HSGetShader(&hull, nullptr, nullptr);
        context->DSGetShader(&domain, nullptr, nullptr);
        context->GetPredication(&predicate, &predicated);
        if (geometry || hull || domain || predicate) return false;
        ID3D11UnorderedAccessView* uavs[D3D11_1_UAV_SLOT_COUNT]{};
        const UINT count = level >= D3D_FEATURE_LEVEL_11_1 ? ARRAYSIZE(uavs) : D3D11_PS_CS_UAV_REGISTER_COUNT;
        context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, count, uavs);
        bool output_busy = false;
        for (auto* view : uavs) if (view) { output_busy = true; view->Release(); }
        ID3D11Buffer* stream_out[D3D11_SO_BUFFER_SLOT_COUNT]{};
        context->SOGetTargets(ARRAYSIZE(stream_out), stream_out);
        for (auto* buffer : stream_out) if (buffer) { output_busy = true; buffer->Release(); }
        if (output_busy) return false;
        context->OMGetRenderTargets(ARRAYSIZE(targets), targets, &depth);
        for (UINT slot = 0; slot < ARRAYSIZE(targets); ++slot) if (targets[slot]) target_count = slot + 1;
        context->OMGetDepthStencilState(&depth_state, &stencil);
        context->OMGetBlendState(&blend, blend_factor, &mask);
        context->RSGetState(&raster);
        context->RSGetViewports(&viewport_count, viewports);
        context->IAGetInputLayout(&layout);
        context->IAGetPrimitiveTopology(&topology);
        context->VSGetShader(&vs.shader, vs.instances, &vs.count);
        context->PSGetShader(&ps.shader, ps.instances, &ps.count);
        context->PSGetShaderResources(0, 1, &input);
        if (SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&context1))))
            context1->PSGetConstantBuffers1(0, 1, &constants, &first_constant, &constant_count);
        else context->PSGetConstantBuffers(0, 1, &constants);
        return true;
    }
    void Restore(ID3D11DeviceContext* context)
    {
        ID3D11ShaderResourceView* none = nullptr;
        context->PSSetShaderResources(0, 1, &none);
        context->OMSetRenderTargets(target_count, targets, depth.Get());
        context->OMSetDepthStencilState(depth_state.Get(), stencil);
        context->OMSetBlendState(blend.Get(), blend_factor, mask);
        context->RSSetViewports(viewport_count, viewports);
        context->RSSetState(raster.Get());
        context->IASetInputLayout(layout.Get());
        context->IASetPrimitiveTopology(topology);
        context->VSSetShader(vs.shader.Get(), vs.instances, vs.shader ? vs.count : 0);
        context->PSSetShader(ps.shader.Get(), ps.instances, ps.shader ? ps.count : 0);
        if (context1 && constants) context1->PSSetConstantBuffers1(0, 1, constants.GetAddressOf(), &first_constant, &constant_count);
        else context->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
        context->PSSetShaderResources(0, 1, input.GetAddressOf());
    }
};
}

bool DepthReadTwin::Prepare(ID3D11Resource* depth)
{
    if (source_.Get() == depth) return twin_ != nullptr;
    source_ = depth;
    twin_.Reset(); input_.Reset(); output_.Reset(); target_.Reset();
    ComPtr<ID3D11Texture2D> original;
    if (FAILED(depth->QueryInterface(IID_PPV_ARGS(&original)))) {
        reason_ = "scene depth is not a 2D texture";
        return false;
    }
    original->GetDesc(&desc_);
    if (desc_.SampleDesc.Count != 1) { reason_ = "multisampled depth"; return false; }
    if (desc_.MipLevels != 1 || desc_.ArraySize != 1) { reason_ = "depth with several slices or mips"; return false; }
    const auto formats = FormatsFor(desc_.Format);
    if (formats.dsv == DXGI_FORMAT_UNKNOWN ||
        (desc_.BindFlags & (D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE)) !=
            (D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE)) {
        reason_ = "scene depth has no compatible depth-plane shader view";
        return false;
    }
    if (!raster_) {
        ComPtr<ID3DBlob> vertex, pixel;
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = sizeof(Constants);
        buffer.Usage = D3D11_USAGE_DEFAULT;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_DEPTH_STENCIL_DESC state{};
        state.DepthEnable = TRUE;
        state.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        state.DepthFunc = D3D11_COMPARISON_ALWAYS;
        state.FrontFace = state.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                                           D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        if (!Compile("vs", "vs_5_0", vertex) || !Compile("ps", "ps_5_0", pixel) ||
            FAILED(device_->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, vs_.ReleaseAndGetAddressOf())) ||
            FAILED(device_->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, ps_.ReleaseAndGetAddressOf())) ||
            FAILED(device_->CreateBuffer(&buffer, nullptr, constants_.ReleaseAndGetAddressOf())) ||
            FAILED(device_->CreateDepthStencilState(&state, depth_state_.ReleaseAndGetAddressOf())) ||
            FAILED(device_->CreateRasterizerState(&raster, raster_.ReleaseAndGetAddressOf()))) {
            reason_ = "depth correction programs unavailable";
            return false;
        }
    }
    D3D11_TEXTURE2D_DESC twin_desc = desc_;
    twin_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    twin_desc.Usage = D3D11_USAGE_DEFAULT;
    twin_desc.CPUAccessFlags = twin_desc.MiscFlags = 0;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = formats.srv;
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = formats.dsv;
    dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11Texture2D> twin;
    if (FAILED(device_->CreateTexture2D(&twin_desc, nullptr, &twin)) ||
        FAILED(device_->CreateShaderResourceView(depth, &srv, &input_)) ||
        FAILED(device_->CreateShaderResourceView(twin.Get(), &srv, &output_)) ||
        FAILED(device_->CreateDepthStencilView(twin.Get(), &dsv, &target_))) {
        reason_ = "could not create the corrected depth texture and views";
        return false;
    }
    twin_ = std::move(twin);
    view_format_ = formats.srv;
    return true;
}

bool DepthReadTwin::Refresh(ID3D11DeviceContext* context, ID3D11Resource* depth, float jitter_x, float jitter_y, bool expanded)
{
    if (!context || !depth || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
        !std::isfinite(jitter_x) || !std::isfinite(jitter_y) || std::fabs(jitter_x) > .5f || std::fabs(jitter_y) > .5f) {
        reason_ = "unqualified context or raster sample";
        return false;
    }
    ComPtr<ID3D11Device> owner;
    context->GetDevice(&owner);
    if (device_.Get() != owner.Get()) {
        device_ = owner;
        vs_.Reset(); ps_.Reset(); constants_.Reset(); depth_state_.Reset(); raster_.Reset();
        source_.Reset();
    }
    // A debug or proxy device may return wrapped resources to the underlying
    // context's detours. Only use private resources on the device's own context.
    ComPtr<ID3D11DeviceContext> immediate;
    owner->GetImmediateContext(&immediate);
    if (immediate.Get() != context) {
        reason_ = "wrapped device and context use different resource interfaces";
        return false;
    }
    if (!Prepare(depth)) return false;
    DrawState saved;
    if (!saved.Save(context, device_->GetFeatureLevel())) {
        reason_ = "the game's draw state cannot be borrowed for depth correction";
        return false;
    }
    const na::depth::unjitter::Params defaults;
    const Constants args{jitter_x, jitter_y, static_cast<int>(desc_.Width) - 1, static_cast<int>(desc_.Height) - 1,
        defaults.curve, defaults.noise, expanded ? 1.f / float(desc_.Width) : 0.f, expanded ? 1.f / float(desc_.Height) : 0.f};
    context->UpdateSubresource(constants_.Get(), 0, nullptr, &args, 0, 0);
    const D3D11_VIEWPORT viewport{0, 0, float(desc_.Width), float(desc_.Height), 0, 1};
    context->RSSetViewports(1, &viewport);
    context->RSSetState(raster_.Get());
    context->OMSetRenderTargets(0, nullptr, target_.Get());
    context->OMSetDepthStencilState(depth_state_.Get(), 0);
    context->OMSetBlendState(nullptr, nullptr, UINT_MAX);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs_.Get(), nullptr, 0);
    context->PSSetShader(ps_.Get(), nullptr, 0);
    context->PSSetConstantBuffers(0, 1, constants_.GetAddressOf());
    context->PSSetShaderResources(0, 1, input_.GetAddressOf());
    context->Draw(3, 0);
    saved.Restore(context);
    reason_ = "current scene depth corrected";
    return true;
}

ID3D11ShaderResourceView* DepthReadTwin::ViewFor(ID3D11ShaderResourceView* original) const
{
    if (!original || !twin_) return nullptr;
    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
    original->GetDesc(&desc);
    if (desc.Format != view_format_ || desc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
        desc.Texture2D.MostDetailedMip != 0 || (desc.Texture2D.MipLevels != 1 && desc.Texture2D.MipLevels != UINT(-1))) return nullptr;
    ComPtr<ID3D11Resource> resource;
    original->GetResource(&resource);
    return resource.Get() == source_.Get() ? output_.Get() : nullptr;
}

ID3D11Resource* DepthReadTwin::SourceForCopy(ID3D11Resource* destination) const
{
    if (!twin_ || !destination || (desc_.Format != DXGI_FORMAT_R32_TYPELESS && desc_.Format != DXGI_FORMAT_R16_TYPELESS))
        return nullptr;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(destination->QueryInterface(IID_PPV_ARGS(&texture)))) return nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.BindFlags != D3D11_BIND_SHADER_RESOURCE || desc.Usage != D3D11_USAGE_DEFAULT ||
        desc.CPUAccessFlags || desc.MiscFlags || desc.Format != desc_.Format || desc.Width != desc_.Width ||
        desc.Height != desc_.Height || desc.ArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1) return nullptr;
    return twin_.Get();
}
} // namespace na::native11
