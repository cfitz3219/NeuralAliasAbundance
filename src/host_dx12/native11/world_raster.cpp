#include "world_raster.hpp"
#include "host_dx12/diag/event_log.hpp"
#include <d3d11_3.h>
#include <d3dcompiler.h>

namespace na::native11 {
namespace {
using Microsoft::WRL::ComPtr;
constexpr char program[] = R"(
struct Vertex { float4 p : SV_Position; float2 uv : TEXCOORD0; };
Vertex vs(uint id : SV_VertexID) {
    Vertex v;
    v.uv = float2((id << 1) & 2, id & 2);
    v.p = float4(v.uv * float2(2,-2) + float2(-1,1), 0, 1);
    return v;
}
Texture2D<float4> color : register(t0);
Texture2D<float> depth : register(t1);
Texture2D<uint2> stencil : register(t2);
struct Result {
    float4 c : SV_Target;
    float z : SV_Depth;
#ifdef COPY_STENCIL
    uint s : SV_StencilRef;
#endif
};
Result ps(Vertex v) {
    uint w, h; depth.GetDimensions(w,h);
    int3 at = int3(min(uint2(v.uv * float2(w,h)), uint2(w-1,h-1)), 0);
    Result r; r.c = color.Load(at); r.z = depth.Load(at);
#ifdef COPY_STENCIL
    r.s = stencil.Load(at).y;
#endif
    return r;
}
// The stencil SRV exposes its value in the green component. One fixed-function
// write-mask pass per bit works on devices without shader stencil reference export.
cbuffer Bit : register(b0) { uint mask; };
void stencil_bit(Vertex v) {
    uint w,h; stencil.GetDimensions(w,h);
    int3 at = int3(min(uint2(v.uv * float2(w,h)),uint2(w-1,h-1)),0);
    if ((stencil.Load(at).y & mask) == 0) discard;
}
)";
struct DepthFormats {
  DXGI_FORMAT base, depth, stencil;
};
DepthFormats Formats(DXGI_FORMAT view) {
  switch (view) {
  case DXGI_FORMAT_D32_FLOAT:
    return {DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT,
            DXGI_FORMAT_UNKNOWN};
  case DXGI_FORMAT_D16_UNORM:
    return {DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM,
            DXGI_FORMAT_UNKNOWN};
  case DXGI_FORMAT_D24_UNORM_S8_UINT:
    return {DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_R24_UNORM_X8_TYPELESS,
            DXGI_FORMAT_X24_TYPELESS_G8_UINT};
  case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    return {DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS,
            DXGI_FORMAT_X32_TYPELESS_G8X24_UINT};
  default:
    return {};
  }
}
bool TextureOf(ID3D11View *view, ComPtr<ID3D11Texture2D> &texture) {
  ComPtr<ID3D11Resource> resource;
  if (view)
    view->GetResource(&resource);
  return resource && SUCCEEDED(resource.As(&texture));
}
bool ReadView(ID3D11Device *device, ID3D11Texture2D *texture,
              DXGI_FORMAT format, ComPtr<ID3D11ShaderResourceView> &view) {
  D3D11_SHADER_RESOURCE_VIEW_DESC d{};
  d.Format = format;
  d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  d.Texture2D.MipLevels = 1;
  return SUCCEEDED(device->CreateShaderResourceView(texture, &d, &view));
}
} // namespace

bool WorldRaster::Prepare(ID3D11RenderTargetView *color,
                          ID3D11DepthStencilView *depth, UINT w, UINT h) {
  ComPtr<ID3D11Texture2D> original_color, original_depth;
  if (!w || !h || !TextureOf(color, original_color) ||
      !TextureOf(depth, original_depth))
    return false;
  ComPtr<ID3D11Device> device;
  color->GetDevice(&device);
  D3D11_TEXTURE2D_DESC c{}, d{};
  original_color->GetDesc(&c);
  original_depth->GetDesc(&d);
  D3D11_RENDER_TARGET_VIEW_DESC cv{};
  color->GetDesc(&cv);
  D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
  depth->GetDesc(&dv);
  const auto df = Formats(dv.Format);
  if (!df.base || c.Width != d.Width || c.Height != d.Height || w >= c.Width ||
      h >= c.Height || c.ArraySize != 1 || d.ArraySize != 1 ||
      c.MipLevels != 1 || d.MipLevels != 1 || c.SampleDesc.Count != 1 ||
      d.SampleDesc.Count != 1 ||
      cv.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D ||
      dv.ViewDimension != D3D11_DSV_DIMENSION_TEXTURE2D || dv.Flags ||
      c.Usage != D3D11_USAGE_DEFAULT || d.Usage != D3D11_USAGE_DEFAULT ||
      (c.BindFlags & D3D11_BIND_UNORDERED_ACCESS) || c.MiscFlags || d.MiscFlags)
    return false;
  const bool stencil = df.stencil != DXGI_FORMAT_UNKNOWN;
  bool stencil_export = false;
  if (stencil) {
    D3D11_FEATURE_DATA_D3D11_OPTIONS2 options{};
    stencil_export =
        SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2,
                                              &options, sizeof options)) &&
        options.PSSpecifiedStencilRefSupported;
  }
  const D3D_SHADER_MACRO macros[]{{"COPY_STENCIL", "1"}, {nullptr, nullptr}};
  ComPtr<ID3DBlob> vsb, psb;
  if (FAILED(D3DCompile(program, sizeof program, nullptr, nullptr, nullptr,
                        "vs", "vs_5_0", 0, 0, &vsb, nullptr)))
    return false;
  if (stencil_export &&
      FAILED(D3DCompile(program, sizeof program, nullptr, macros, nullptr, "ps",
                        "ps_5_0", 0, 0, &psb, nullptr)))
    stencil_export = false;
  if (!psb && FAILED(D3DCompile(program, sizeof program, nullptr, nullptr,
                                nullptr, "ps", "ps_5_0", 0, 0, &psb, nullptr)))
    return false;
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  if (FAILED(device->CreateVertexShader(vsb->GetBufferPointer(),
                                        vsb->GetBufferSize(), nullptr, &vs)))
    return false;
  if (FAILED(device->CreatePixelShader(psb->GetBufferPointer(),
                                       psb->GetBufferSize(), nullptr, &ps))) {
    if (!stencil_export)
      return false;
    stencil_export = false;
    psb.Reset();
    if (FAILED(D3DCompile(program, sizeof program, nullptr, nullptr, nullptr,
                          "ps", "ps_5_0", 0, 0, &psb, nullptr)) ||
        FAILED(device->CreatePixelShader(psb->GetBufferPointer(),
                                         psb->GetBufferSize(), nullptr, &ps)))
      return false;
  }
  D3D11_DEPTH_STENCIL_DESC state{};
  state.DepthEnable = TRUE;
  state.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  state.DepthFunc = D3D11_COMPARISON_ALWAYS;
  state.StencilEnable = stencil_export;
  state.StencilReadMask = state.StencilWriteMask = 0xff;
  state.FrontFace =
      state.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                        D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_ALWAYS};
  ComPtr<ID3D11DepthStencilState> zstate;
  D3D11_RASTERIZER_DESC raster{};
  raster.FillMode = D3D11_FILL_SOLID;
  raster.CullMode = D3D11_CULL_NONE;
  raster.DepthClipEnable = TRUE;
  ComPtr<ID3D11RasterizerState> rs;
  if (FAILED(device->CreateDepthStencilState(&state, &zstate)) ||
      FAILED(device->CreateRasterizerState(&raster, &rs)))
    return false;
  ComPtr<ID3D11Buffer> bits[8];
  ComPtr<ID3D11DepthStencilState> write_bits[8];
  ComPtr<ID3D11PixelShader> bit_shader;
  if (stencil && !stencil_export) {
    ComPtr<ID3DBlob> b;
    if (FAILED(D3DCompile(program, sizeof program, nullptr, nullptr, nullptr,
                          "stencil_bit", "ps_5_0", 0, 0, &b, nullptr)) ||
        FAILED(device->CreatePixelShader(
            b->GetBufferPointer(), b->GetBufferSize(), nullptr, &bit_shader)))
      return false;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    auto bit_state = state;
    bit_state.DepthEnable = FALSE;
    bit_state.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    bit_state.StencilEnable = TRUE;
    for (unsigned bit = 0; bit < 8; ++bit) {
      const UINT data[4]{1u << bit};
      D3D11_SUBRESOURCE_DATA init{};
      init.pSysMem = data;
      bit_state.StencilWriteMask = UINT8(data[0]);
      if (FAILED(device->CreateBuffer(&bd, &init, &bits[bit])) ||
          FAILED(device->CreateDepthStencilState(&bit_state, &write_bits[bit])))
        return false;
    }
  }
  native_width = c.Width;
  native_height = c.Height;
  width = w;
  height = h;
  ComPtr<ID3D11Texture2D> seed_color, seed_depth;
  // A DSV-only resource cannot be sampled. Copy to a compatible typeless
  // texture first.
  c.BindFlags = d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  d.Format = df.base;
  if (FAILED(device->CreateTexture2D(&c, nullptr, &seed_color)) ||
      FAILED(device->CreateTexture2D(&d, nullptr, &seed_depth)))
    return false;
  c.Width = d.Width = w;
  c.Height = d.Height = h;
  c.BindFlags |= D3D11_BIND_RENDER_TARGET;
  d.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
  if (FAILED(device->CreateTexture2D(&c, nullptr, &color_)) ||
      FAILED(device->CreateTexture2D(&d, nullptr, &depth_)) ||
      FAILED(device->CreateRenderTargetView(color_.Get(), &cv, &rtv_)) ||
      FAILED(device->CreateDepthStencilView(depth_.Get(), &dv, &dsv_)))
    return false;
  ComPtr<ID3D11DepthStencilState> depth_only_state;
  if (stencil) {
    state.StencilEnable = FALSE;
    if (FAILED(device->CreateDepthStencilState(&state, &depth_only_state)))
      return false;
  }
  // The common path never changes stencil. Leave the game's native stencil
  // exactly as it is.
  for (unsigned transfer = 0; transfer < (stencil ? 3u : 2u); ++transfer) {
    const bool seed = transfer == 0, depth_only = transfer == 2;
    ComPtr<ID3D11DeviceContext> dc;
    if (FAILED(device->CreateDeferredContext(0, &dc)))
      return false;
    auto *input_z = seed ? seed_depth.Get() : depth_.Get();
    ComPtr<ID3D11ShaderResourceView> rgba, z, s;
    if (!ReadView(device.Get(), seed ? seed_color.Get() : color_.Get(),
                  cv.Format, rgba) ||
        !ReadView(device.Get(), input_z, df.depth, z) ||
        (stencil && !ReadView(device.Get(), input_z, df.stencil, s)))
      return false;
    if (seed) {
      dc->CopyResource(seed_color.Get(), original_color.Get());
      dc->CopyResource(seed_depth.Get(), original_depth.Get());
    }
    auto *dest = seed ? rtv_.Get() : color;
    dc->OMSetRenderTargets(1, &dest, seed ? dsv_.Get() : depth);
    dc->OMSetDepthStencilState(
        depth_only ? depth_only_state.Get() : zstate.Get(), 0);
    dc->RSSetState(rs.Get());
    const D3D11_VIEWPORT vp{
        0, 0, float(seed ? w : native_width), float(seed ? h : native_height),
        0, 1};
    dc->RSSetViewports(1, &vp);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc->VSSetShader(vs.Get(), nullptr, 0);
    dc->PSSetShader(ps.Get(), nullptr, 0);
    ID3D11ShaderResourceView *views[]{rgba.Get(), z.Get(), s.Get()};
    dc->PSSetShaderResources(0, stencil ? 3 : 2, views);
    dc->Draw(3, 0);
    if (bit_shader && !depth_only) {
      auto *dest_depth = seed ? dsv_.Get() : depth;
      dc->OMSetRenderTargets(0, nullptr, dest_depth);
      dc->ClearDepthStencilView(dest_depth, D3D11_CLEAR_STENCIL, 0, 0);
      dc->PSSetShader(bit_shader.Get(), nullptr, 0);
      for (unsigned bit = 0; bit < 8; ++bit) {
        dc->OMSetDepthStencilState(write_bits[bit].Get(), 0xff);
        dc->PSSetConstantBuffers(0, 1, bits[bit].GetAddressOf());
        dc->Draw(3, 0);
      }
    }
    auto &list = seed ? seed_ : depth_only ? restore_depth_only_ : restore_;
    if (FAILED(dc->FinishCommandList(FALSE, &list)))
      return false;
  }
  na::eventlog::Emit("native11_world_transfer", "depth=%u stencil=%s",
                     unsigned(dv.Format),
                     !stencil         ? "none"
                     : stencil_export ? "shader_export"
                                      : "masked_copy");
  return true;
}
} // namespace na::native11
