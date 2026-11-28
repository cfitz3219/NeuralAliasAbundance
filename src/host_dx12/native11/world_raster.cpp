#include "world_raster.hpp"
#include "host_dx12/diag/event_log.hpp"
#include <d3d11_3.h>
#include <d3dcompiler.h>
#include <algorithm>

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
  dsv_desc_ = dv;
  for (auto &view : dsv_flags_) view.Reset();
  for (auto &tried : dsv_flags_tried_) tried = false;
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

ID3D11DepthStencilView *WorldRaster::DepthView(UINT flags) {
  if (!flags) return dsv_.Get();
  if (flags > 3 || !depth_) return nullptr;
  if (!dsv_flags_tried_[flags]) {
    dsv_flags_tried_[flags] = true;
    ComPtr<ID3D11Device> device;
    depth_->GetDevice(&device);
    auto desc = dsv_desc_;
    desc.Flags = flags;
    if (FAILED(device->CreateDepthStencilView(depth_.Get(), &desc,
                                              &dsv_flags_[flags])))
      dsv_flags_[flags].Reset();
  }
  return dsv_flags_[flags].Get();
}

namespace {
constexpr char corner_program[] = R"(
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2,-2) + float2(-1,1), 0, 1);
}
Texture2D<float4> source : register(t0);
cbuffer Corner : register(b0) { float2 extent; float2 spare; };
// Position p of the corner shows what the same place of the whole picture shows.
// A few pixels beyond the corner repeat its edge for filtered reads.
float4 corner(float4 p : SV_Position) : SV_Target {
    uint w, h; source.GetDimensions(w, h);
    float2 at = min(p.xy, extent - 0.5) / extent;
    return source.Load(int3(min(uint2(at * float2(w, h)), uint2(w - 1, h - 1)), 0));
}
SamplerState smooth : register(s0);
// Brightness on a perceptual scale, so dark and bright scenes are judged alike.
float4 probe(float4 p : SV_Position) : SV_Target {
    float3 c = source.SampleLevel(smooth, p.xy * extent, 0).rgb;
    return float4(sqrt(saturate(c)), 1);
}
)";
bool Compile(const char *entry, const char *target, ComPtr<ID3DBlob> &out) {
  return SUCCEEDED(D3DCompile(corner_program, sizeof corner_program, nullptr,
                              nullptr, nullptr, entry, target, 0, 0, &out,
                              nullptr));
}
// The format a copy of this view is drawn into.
DXGI_FORMAT CornerFormat(DXGI_FORMAT view) {
  switch (view) {
  case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
  case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
  case DXGI_FORMAT_R32_FLOAT:
    return DXGI_FORMAT_R32_FLOAT;
  case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
  case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
    return DXGI_FORMAT_UNKNOWN; // stencil values cannot be filtered or drawn
  default:
    return view;
  }
}
} // namespace

bool CornerCopies::Make(ID3D11ShaderResourceView *source, UINT w, UINT h,
                        Entry &e, const char **why) {
  const auto fail = [&](const char *text) {
    if (why) *why = text;
    return false;
  };
  ComPtr<ID3D11Device> device;
  source->GetDevice(&device);
  ComPtr<ID3D11Resource> resource;
  source->GetResource(&resource);
  ComPtr<ID3D11Texture2D> texture;
  D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
  source->GetDesc(&sv);
  if (!resource || FAILED(resource.As(&texture)) ||
      sv.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D)
    return fail("a full-size picture the world reads is not a plain 2D picture");
  D3D11_TEXTURE2D_DESC d{};
  texture->GetDesc(&d);
  const DXGI_FORMAT format = CornerFormat(sv.Format);
  UINT support = 0;
  if (!format || FAILED(device->CheckFormatSupport(format, &support)) ||
      !(support & D3D11_FORMAT_SUPPORT_RENDER_TARGET) ||
      !(support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE))
    return fail("a full-size picture the world reads has a format that cannot be made small");
  if (!vs_ && !shaders_failed_) {
    ComPtr<ID3DBlob> v, p;
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    if (!Compile("vs", "vs_5_0", v) || !Compile("corner", "ps_5_0", p) ||
        FAILED(device->CreateVertexShader(v->GetBufferPointer(),
                                          v->GetBufferSize(), nullptr, &vs_)) ||
        FAILED(device->CreatePixelShader(p->GetBufferPointer(),
                                         p->GetBufferSize(), nullptr, &ps_)) ||
        FAILED(device->CreateRasterizerState(&raster, &rs_))) {
      vs_.Reset();
      shaders_failed_ = true;
    }
  }
  if (!vs_) return fail("the copy pass could not be prepared on this graphics card");
  D3D11_TEXTURE2D_DESC t{};
  t.Width = d.Width;
  t.Height = d.Height;
  t.MipLevels = t.ArraySize = 1;
  t.Format = format;
  t.SampleDesc.Count = 1;
  t.Usage = D3D11_USAGE_DEFAULT;
  t.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  ComPtr<ID3D11RenderTargetView> target;
  const float size[4]{float(w), float(h), 0, 0};
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = sizeof size;
  bd.Usage = D3D11_USAGE_IMMUTABLE;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  D3D11_SUBRESOURCE_DATA init{};
  init.pSysMem = size;
  ComPtr<ID3D11Buffer> constants;
  ComPtr<ID3D11DeviceContext> dc;
  if (FAILED(device->CreateTexture2D(&t, nullptr, &e.texture)) ||
      FAILED(device->CreateRenderTargetView(e.texture.Get(), nullptr, &target)) ||
      FAILED(device->CreateShaderResourceView(e.texture.Get(), nullptr, &e.view)) ||
      FAILED(device->CreateBuffer(&bd, &init, &constants)) ||
      FAILED(device->CreateDeferredContext(0, &dc)))
    return fail("the video memory for a small copy of a full-size picture could not be had");
  auto *rtv = target.Get();
  dc->OMSetRenderTargets(1, &rtv, nullptr);
  dc->RSSetState(rs_.Get());
  const D3D11_VIEWPORT vp{0, 0, float((std::min)(w + 4, d.Width)),
                          float((std::min)(h + 4, d.Height)), 0, 1};
  dc->RSSetViewports(1, &vp);
  dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  dc->VSSetShader(vs_.Get(), nullptr, 0);
  dc->PSSetShader(ps_.Get(), nullptr, 0);
  dc->PSSetShaderResources(0, 1, &source);
  dc->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
  dc->Draw(3, 0);
  if (FAILED(dc->FinishCommandList(FALSE, &e.fill)))
    return fail("the copy pass could not be recorded");
  e.source = source;
  e.w = w;
  e.h = h;
  e.run = ~uint64_t{0};
  return true;
}

CornerCopies::Entry *CornerCopies::Find(ID3D11ShaderResourceView *source,
                                        UINT w, UINT h, const char **why) {
  if (!source || !w || !h) return nullptr;
  for (auto &e : entries_)
    if (e.source.Get() == source && e.w == w && e.h == h) return &e;
  constexpr size_t kMost = 8;
  if (entries_.size() >= kMost) {
    if (why) *why = "the world reads more full-size pictures than can be made small";
    return nullptr;
  }
  Entry e;
  if (!Make(source, w, h, e, why)) return nullptr;
  entries_.push_back(std::move(e));
  return &entries_.back();
}

bool CornerCopies::Can(ID3D11ShaderResourceView *source, UINT small_w,
                       UINT small_h, uint64_t frame, const char **why) {
  Entry *found = Find(source, small_w, small_h, why);
  if (found) found->used = frame;
  return found != nullptr;
}

ID3D11ShaderResourceView *CornerCopies::Get(ID3D11DeviceContext *ctx,
                                            ID3D11ShaderResourceView *source,
                                            UINT small_w, UINT small_h,
                                            uint64_t run, uint64_t frame,
                                            const char **why) {
  Entry *found = ctx ? Find(source, small_w, small_h, why) : nullptr;
  if (!found) return nullptr;
  if (found->run != run) {
    // The game's own state comes back with the list's end.
    ctx->ExecuteCommandList(found->fill.Get(), TRUE);
    found->run = run;
    ++fills;
  }
  found->used = frame;
  return found->view.Get();
}

void CornerCopies::Trim(uint64_t frame) {
  for (size_t i = 0; i < entries_.size();)
    if (frame > entries_[i].used + 120)
      entries_.erase(entries_.begin() + i);
    else
      ++i;
}

bool WorldProbe::Prepare(ID3D11Device *device, ID3D11Texture2D *picture,
                         DXGI_FORMAT view_format) {
  list_.Reset();
  if (!device || !picture) return false;
  constexpr UINT kW = na::referee::kWorldGridW * 8, kH = na::referee::kWorldGridH * 8;
  ComPtr<ID3DBlob> v, p;
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  ComPtr<ID3D11ShaderResourceView> source;
  ComPtr<ID3D11RenderTargetView> target;
  ComPtr<ID3D11SamplerState> smooth;
  ComPtr<ID3D11RasterizerState> rs;
  ComPtr<ID3D11Buffer> constants;
  ComPtr<ID3D11DeviceContext> dc;
  D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
  sv.Format = view_format;
  sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  sv.Texture2D.MipLevels = 1;
  D3D11_TEXTURE2D_DESC t{};
  t.Width = kW;
  t.Height = kH;
  t.MipLevels = 4; // the last is the grid: 64 x 32
  t.ArraySize = 1;
  t.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  t.SampleDesc.Count = 1;
  t.Usage = D3D11_USAGE_DEFAULT;
  t.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  t.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
  D3D11_RENDER_TARGET_VIEW_DESC tv{};
  tv.Format = t.Format;
  tv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
  D3D11_RASTERIZER_DESC raster{};
  raster.FillMode = D3D11_FILL_SOLID;
  raster.CullMode = D3D11_CULL_NONE;
  raster.DepthClipEnable = TRUE;
  const float size[4]{1.f / kW, 1.f / kH, 0, 0};
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = sizeof size;
  bd.Usage = D3D11_USAGE_IMMUTABLE;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  D3D11_SUBRESOURCE_DATA init{};
  init.pSysMem = size;
  if (!Compile("vs", "vs_5_0", v) || !Compile("probe", "ps_5_0", p) ||
      FAILED(device->CreateVertexShader(v->GetBufferPointer(), v->GetBufferSize(), nullptr, &vs)) ||
      FAILED(device->CreatePixelShader(p->GetBufferPointer(), p->GetBufferSize(), nullptr, &ps)) ||
      FAILED(device->CreateShaderResourceView(picture, &sv, &source)) ||
      FAILED(device->CreateTexture2D(&t, nullptr, &thumb_)) ||
      FAILED(device->CreateRenderTargetView(thumb_.Get(), &tv, &target)) ||
      FAILED(device->CreateShaderResourceView(thumb_.Get(), nullptr, &thumb_view_)) ||
      FAILED(device->CreateSamplerState(&sd, &smooth)) ||
      FAILED(device->CreateRasterizerState(&raster, &rs)) ||
      FAILED(device->CreateBuffer(&bd, &init, &constants)) ||
      FAILED(device->CreateDeferredContext(0, &dc)))
    return false;
  D3D11_TEXTURE2D_DESC st{};
  st.Width = na::referee::kWorldGridW;
  st.Height = na::referee::kWorldGridH;
  st.MipLevels = st.ArraySize = 1;
  st.Format = t.Format;
  st.SampleDesc.Count = 1;
  st.Usage = D3D11_USAGE_STAGING;
  st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  for (auto &slot : slots_) {
    slot = {};
    if (FAILED(device->CreateTexture2D(&st, nullptr, &slot.staging))) return false;
  }
  auto *rtv = target.Get();
  dc->OMSetRenderTargets(1, &rtv, nullptr);
  dc->RSSetState(rs.Get());
  const D3D11_VIEWPORT vp{0, 0, float(kW), float(kH), 0, 1};
  dc->RSSetViewports(1, &vp);
  dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  dc->VSSetShader(vs.Get(), nullptr, 0);
  dc->PSSetShader(ps.Get(), nullptr, 0);
  dc->PSSetShaderResources(0, 1, source.GetAddressOf());
  dc->PSSetSamplers(0, 1, smooth.GetAddressOf());
  dc->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
  dc->Draw(3, 0);
  return SUCCEEDED(dc->FinishCommandList(FALSE, &list_));
}

bool WorldProbe::Note(ID3D11DeviceContext *ctx, uint64_t tag) {
  if (!ctx || !list_ || !tag) return false;
  Slot *free_slot = nullptr;
  for (auto &slot : slots_)
    if (!slot.tag && !free_slot) free_slot = &slot;
  if (!free_slot) return false;
  ctx->ExecuteCommandList(list_.Get(), TRUE);
  ctx->GenerateMips(thumb_view_.Get());
  ctx->CopySubresourceRegion(free_slot->staging.Get(), 0, 0, 0, 0, thumb_.Get(), 3, nullptr);
  free_slot->tag = tag;
  free_slot->polls = 0;
  free_slot->order = ++order_;
  return true;
}

bool WorldProbe::Poll(ID3D11DeviceContext *ctx, uint64_t &tag,
                      na::referee::WorldGrid &cells) {
  if (!ctx) return false;
  Slot *oldest = nullptr;
  for (auto &slot : slots_)
    if (slot.tag && (!oldest || slot.order < oldest->order)) oldest = &slot;
  // The GPU is given two frames before the first look; it is never waited for.
  if (!oldest || ++oldest->polls < 3) return false;
  D3D11_MAPPED_SUBRESOURCE map{};
  if (FAILED(ctx->Map(oldest->staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map))) {
    if (oldest->polls > 120) oldest->tag = 0; // lost: the look starts again by itself
    return false;
  }
  for (UINT y = 0; y < na::referee::kWorldGridH; ++y) {
    const auto *row = static_cast<const uint8_t *>(map.pData) + size_t(y) * map.RowPitch;
    for (UINT x = 0; x < na::referee::kWorldGridW; ++x)
      cells[y * na::referee::kWorldGridW + x] =
          uint8_t((row[x * 4] * 54u + row[x * 4 + 1] * 183u + row[x * 4 + 2] * 19u) >> 8);
  }
  ctx->Unmap(oldest->staging.Get(), 0);
  tag = oldest->tag;
  oldest->tag = 0;
  return true;
}
} // namespace na::native11
