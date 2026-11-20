// The native DX11 input provider (plan B1) on a small synthetic DX11 "game", under the real hooks.
// No game names, no special cases. Each frame, like a real game: a shadow pass (own depth, a shadow
// camera upload), the main camera upload (ViewProjection + inverse in a dynamic constant buffer),
// scene draws into an FP16 colour with a D24S8 depth (no shader access), a tonemap reading the scene
// colour into the back buffer, HUD draws, Present. Phase 1 records on the immediate context; phase 2
// records scene and post on deferred contexts and executes them on the immediate one; then a resize.
//
// The consumer here stands in for B2: it checks the facts it is handed and copies the scene colour
// on the game's context inside the callback, as the shared-texture handoff will.
//
// Checks: the scene colour/depth (not the shadow map or back buffer) with their formats, extents and
// lifetime ids; the camera at byte 64 with its CPU bytes; the callback runs before the tonemap read
// (immediate) / before the post list (deferred), once per frame, with the scene already drawn; the
// consumer's own D3D11 work is not observed; jitter from SetNextJitter reaches the GPU's camera (rows
// 0/1 only) and is reported back; the stream id holds across frames and changes after a resize.
// Hardware first, then WARP; exit 77 (SKIP) only without any DX11 device.
#include "host_dx12/native11/camera_pick.hpp"
#include "host_dx12/native11/input_provider.hpp"
#include "host_dx12/native11/viewport_jitter.hpp"
#include "host_dx12/diag/event_log.hpp"
#if defined(NA_TEST_NATIVE_DLAA)
#include "host_dx12/adapters/wwii/scene_camera.hpp"
#include "host_dx12/capture/frame_capture.hpp"
#include "host_dx12/capture/j2_capture.hpp"
#include "host_dx12/capture/referee_gpu.hpp"
#include "core/referee_recovery.hpp"
#include "host_dx12/native_d3d11/live_dlaa.hpp"
#include "host_dx12/j2/live_dlaa.hpp"
#include "host_dx12/j2/compute_flow.hpp"
#include "host_dx12/j2/j2_flow.hpp"
#include "host_dx12/effects/xegtao.hpp"
#include "host_dx12/effects/donut.hpp"
#include "host_dx12/host_boot/host_boot.hpp"
#include "host_dx12/host_boot/process_log.hpp"
#include "host_dx12/on12/d3d11_on12.hpp"
#include "host_dx12/overlay/overlay.hpp"
#include "host_dx12/settings.hpp"
#include "host_dx12/native11/samplers.hpp"
#include "host_dx12/native11/texture_loader.hpp"
#include "host_dx12/native11/textures.hpp"
#include "materials/texture_subresources_v1.hpp"
#include "host_dx12/observe/producers.hpp"
#include <DirectXPackedVector.h>
#include <d3d11on12.h>
#include <filesystem>
#include <fstream>
#endif

#include <d3d11_4.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

#if defined(NA_TEST_NATIVE_DLAA)
// Same packed HDR colour format as the recorded WWII campaign.
constexpr DXGI_FORMAT kSceneFormat = DXGI_FORMAT_R11G11B10_FLOAT;
#else
constexpr DXGI_FORMAT kSceneFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
#endif

int g_failed = 0;
void expect(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    std::fflush(stdout);
    if (!ok) ++g_failed;
}

constexpr char kShaders[] = R"(
cbuffer Camera : register(b0) { float4 pad; row_major float4x4 vp; };
cbuffer Object : register(b1) { float4 offset; };
Texture2D<float4> scene : register(t0);
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id & 1) ? 0.5 : -0.5, (id & 2) ? 0.5 : -0.5) + offset.xy;
    return float4(p, 0.5, 1.0) + vp[0] * 0.0; // the camera block is bound and referenced
}
float4 vs_post(uint id : SV_VertexID) : SV_Position {
    return float4((id & 1) ? 1.0 : -1.0, (id & 2) ? 1.0 : -1.0, 0.5, 1.0);
}
struct GsVertex { float4 pos : SV_Position; };
[maxvertexcount(3)]
void gs_passthrough(triangle GsVertex input[3], inout TriangleStream<GsVertex> output) {
    [unroll] for (uint i = 0; i < 3; ++i) output.Append(input[i]);
}
float4 ps_scene(float4 p : SV_Position) : SV_Target { return float4(0.25, 0.5, 0.75, 1.0); }
float4 ps_scene_variant(float4 p : SV_Position) : SV_Target { return float4(0.3, 0.6, 0.25, 1.0); }
float4 ps_tonemap(float4 p : SV_Position) : SV_Target { return scene.Load(int3(int2(p.xy), 0)); }
float4 ps_letterbox(float4 p : SV_Position) : SV_Target { return scene.Load(int3(int2(p.xy) - int2(0,2), 0)); }
float4 ps_hud(float4 p : SV_Position) : SV_Target { return float4(1, 1, 1, 0.5); }
float4 ps_screen_check(float4 p : SV_Position) : SV_Target { return float4(scene.Load(int3(int2(p.xy), 0)).rgb * 0.75, 1); }
struct LitVertex { float4 p : SV_Position; float2 surface : TEXCOORD0; };
LitVertex vs_reflection(uint id : SV_VertexID) {
    LitVertex v;
    v.p = vs_post(id);
    v.surface = float2((v.p.x+1)*160, (1-v.p.y)*90);
    return v;
}
float4 ps_reflection(float4 p : SV_Position, float2 surface : TEXCOORD0) : SV_Target {
    float3 base = scene.Load(int3(int2(p.xy), 0)).rgb;
    float2 q = (surface - float2(148,88))/float2(46,25);
    float light = exp(-dot(q,q)) * (.45 + .10 * cos((surface.x+surface.y)*1.57));
    return float4(base * .45 + light, 1);
}
float4 ps_trial_surface(float4 p : SV_Position) : SV_Target {
    float v = .28 + .10 * sin(p.x * .16) * cos(p.y * .27);
    return float4(v, v, v, 1);
}
float4 ps_trial_light(float4 p : SV_Position, float2 surface : TEXCOORD0) : SV_Target {
    float3 base = scene.Load(int3(int2(p.xy), 0)).rgb;
    float2 q = (surface - float2(148,88))/float2(35,22);
    float light = saturate(1 - dot(q,q)) * (.20 + .09 * cos((surface.x+surface.y)*1.57));
    return float4(base + light, 1);
}
float4 ps_trial_screen(float4 p : SV_Position) : SV_Target {
    float3 base = scene.Load(int3(int2(p.xy), 0)).rgb;
    bool region = p.x >= 120 && p.x < 160 && p.y >= 68 && p.y < 90;
    return float4(base * (region ? 1 + offset.x : 1), 1);
}
Texture2D<float> test_depth : register(t1);
RWTexture2D<float> mask_out : register(u0);
float4 vs_depth_ramp(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2) * 2 - 1;
    return float4(p, 0.45 + 0.05 * p.x + 0.13 * p.y, 1);
}
float slab(uint2 pixel) {
    float2 p = float2(pixel) + 0.5;
    float expected = 0.45 + 0.05 * (p.x / 160.0 - 1) + 0.13 * (1 - p.y / 90.0);
    return abs(test_depth.Load(int3(pixel, 0)) - expected) < 0.00010 ? 1 : 0;
}
float ps_depth_slab(float4 p : SV_Position) : SV_Target { return slab(uint2(p.xy)); }
[numthreads(8, 8, 1)]
void cs_depth_slab(uint3 id : SV_DispatchThreadID) {
    if (id.x < 320 && id.y < 180) mask_out[id.xy] = slab(id.xy);
}
SamplerState material : register(s0);
float4 ps_mips(float4 p : SV_Position) : SV_Target {
    return scene.SampleGrad(material, float2(0.5, 0.5), float2(2.0/64.0,0), float2(0,2.0/64.0));
}
)";

ComPtr<ID3DBlob> Compile(const char* entry, const char* target)
{
    ComPtr<ID3DBlob> code, err;
    if (FAILED(D3DCompile(kShaders, sizeof(kShaders) - 1, "test", nullptr, nullptr, entry, target, 0, 0, &code, &err))) {
        std::printf("shader %s: %s\n", entry, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return nullptr;
    }
    return code;
}

na::camera::Matrix MainVp(double t)
{
    const double fy = 1.0 / std::tan(0.5), fx = fy / (320.0 / 180.0), n = 0.1, f = 500.0;
    const na::camera::Matrix P{fx, 0, 0, 0, 0, fy, 0, 0, 0, 0, f / (f - n), -n * f / (f - n), 0, 0, 1, 0};
    const double c = std::cos(0.02 * t), s = std::sin(0.02 * t); // slow yaw
    const na::camera::Matrix V{c, 0, -s, -0.1 * t, 0, 1, 0, -2, s, 0, c, -10, 0, 0, 0, 1};
    na::camera::Matrix m{};
    for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 4; ++k)
            for (int j = 0; j < 4; ++j) m[r * 4 + j] += P[r * 4 + k] * V[k * 4 + j];
    return m;
}
na::camera::Matrix ShadowVp() // orthographic: never mistaken for the main camera
{
    return na::camera::Matrix{0.01, 0, 0, 0, 0, 0.01, 0, 0, 0, 0, 0.002, 0.5, 0, 0, 0, 1};
}

struct Game {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGISwapChain> swap;
    HWND wnd = nullptr;
    UINT w = 320, h = 180;
    bool padded_hdr = false;
    bool shader_depth = false;
    bool campaign_camera = false;
    bool campaign_pixel_range = false;
    uint32_t camera_ring_offset = 24576;
    uint32_t camera_snapshot_size = 32768;
    bool campaign_mixed_ring = false;
    bool camera_no_overwrite = false; // the scene camera is appended with D3D11_MAP_WRITE_NO_OVERWRITE
    bool campaign_native_pose = false;
    bool campaign_single_origin = false;
    bool campaign_blank_witness = false; // native p37752: the origin copy at 928 left all zero
    ComPtr<ID3D11Texture2D> shadow_depth, shadow_rt, depth, color;
    ComPtr<ID3D11DepthStencilView> shadow_dsv, dsv;
    ComPtr<ID3D11RenderTargetView> shadow_rtv, color_rtv, back_rtv;
    ComPtr<ID3D11ShaderResourceView> color_srv;
    bool campaign_split_camera = false, split_tie = false;
    // A layered (deferred) renderer: the scene's geometry goes into two 8-bit material layers,
    // lighting reads them into the picture, and the rest of the scene is drawn into the picture.
    bool layered = false;
    // The scene drawn straight onto the screen, never read: 1 = the HUD is drawn with no depth
    // bound, 2 = the HUD keeps the scene depth bound with the depth test off.
    int on_screen = 0;
    // The camera's constants come from a pool: a different buffer object every frame, same place.
    bool rotate_camera = false;
    ComPtr<ID3D11Buffer> cam_pool[2];
    ComPtr<ID3D11DepthStencilState> depth_off;
    ComPtr<ID3D11Texture2D> layer, layer2;
    ComPtr<ID3D11RenderTargetView> layer_rtv, layer2_rtv;
    ComPtr<ID3D11ShaderResourceView> layer_srv;
    ComPtr<ID3D11Buffer> cam_cb, obj_cb, split_world, split_projection;
    ComPtr<ID3D11VertexShader> vs, vs_post;
    ComPtr<ID3D11PixelShader> ps_scene, ps_tonemap, ps_hud;
    ComPtr<ID3D11BlendState> blend;
    na::camera::Matrix last_written{};
};

bool MakeTargets(Game& g)
{
    g.back_rtv.Reset();
    g.color_srv.Reset();
    g.color_rtv.Reset();
    g.dsv.Reset();
    g.color.Reset();
    g.depth.Reset();
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(g.swap->GetBuffer(0, IID_PPV_ARGS(&back))) || FAILED(g.device->CreateRenderTargetView(back.Get(), nullptr, &g.back_rtv))) return false;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = g.w;
    d.Height = g.h;
    d.MipLevels = d.ArraySize = 1;
    d.SampleDesc.Count = 1;
    d.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    D3D11_DEPTH_STENCIL_VIEW_DESC depth_view{};
    depth_view.Format = DXGI_FORMAT_D32_FLOAT;
    depth_view.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (g.shader_depth) {
        d.Format = DXGI_FORMAT_R32_TYPELESS;
        d.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    }
    if (FAILED(g.device->CreateTexture2D(&d, nullptr, &g.depth)) ||
        FAILED(g.device->CreateDepthStencilView(g.depth.Get(), g.shader_depth ? &depth_view : nullptr, &g.dsv))) return false;
    d.Format = g.padded_hdr ? DXGI_FORMAT_R16G16B16A16_TYPELESS : kSceneFormat;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    D3D11_RENDER_TARGET_VIEW_DESC rv{};
    rv.Format = g.padded_hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : kSceneFormat;
    rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = rv.Format;
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    if (FAILED(g.device->CreateTexture2D(&d, nullptr, &g.color)) || FAILED(g.device->CreateRenderTargetView(g.color.Get(), &rv, &g.color_rtv)) ||
        FAILED(g.device->CreateShaderResourceView(g.color.Get(), &sv, &g.color_srv)))
        return false;
    return true;
}

bool MakeGame(Game& g, bool warp, bool debug = false)
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"NeuralAliasDX11TestGame";
    RegisterClassW(&wc);
    g.wnd = CreateWindowExW(0, wc.lpszClassName, L"test", WS_OVERLAPPEDWINDOW, 0, 0, int(g.w), int(g.h), nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = g.w;
    sd.BufferDesc.Height = g.h + (g.padded_hdr ? 3 : 0);
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g.wnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    const UINT flags = debug ? D3D11_CREATE_DEVICE_DEBUG : 0;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            &fl, 1, D3D11_SDK_VERSION, &sd, &g.swap, &g.device, nullptr, &g.ctx))) return false;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = d.Height = 512;
    d.MipLevels = d.ArraySize = 1;
    d.SampleDesc.Count = 1;
    d.Format = DXGI_FORMAT_D32_FLOAT;
    d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    g.device->CreateTexture2D(&d, nullptr, &g.shadow_depth);
    g.device->CreateDepthStencilView(g.shadow_depth.Get(), nullptr, &g.shadow_dsv);
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    g.device->CreateTexture2D(&d, nullptr, &g.shadow_rt);
    g.device->CreateRenderTargetView(g.shadow_rt.Get(), nullptr, &g.shadow_rtv);
    D3D11_BUFFER_DESC b{};
    b.ByteWidth = 512;
    b.Usage = D3D11_USAGE_DYNAMIC;
    b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    b.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g.device->CreateBuffer(&b, nullptr, &g.cam_cb);
    b.ByteWidth = 16;
    g.device->CreateBuffer(&b, nullptr, &g.obj_cb);
    auto vs = Compile("vs", "vs_5_0"), pss = Compile("ps_scene", "ps_5_0"), pst = Compile("ps_tonemap", "ps_5_0"), psh = Compile("ps_hud", "ps_5_0");
    auto vsp = Compile("vs_post", "vs_5_0");
    if (!vs || !vsp || !pss || !pst || !psh) return false;
    g.device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g.vs);
    if (FAILED(g.device->CreateVertexShader(vsp->GetBufferPointer(), vsp->GetBufferSize(), nullptr, &g.vs_post))) return false;
    g.device->CreatePixelShader(pss->GetBufferPointer(), pss->GetBufferSize(), nullptr, &g.ps_scene);
    g.device->CreatePixelShader(pst->GetBufferPointer(), pst->GetBufferSize(), nullptr, &g.ps_tonemap);
    g.device->CreatePixelShader(psh->GetBufferPointer(), psh->GetBufferSize(), nullptr, &g.ps_hud);
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g.device->CreateBlendState(&bd, &g.blend);
    return g.shadow_dsv && g.shadow_rtv && g.cam_cb && g.obj_cb && g.blend && MakeTargets(g);
}

void UploadCamera(ID3D11DeviceContext* c, Game& g, const na::camera::Matrix& vp, bool remember)
{
    D3D11_MAPPED_SUBRESOURCE m{};
    // A frame allocator: one discard at the start of the frame (the shadow upload here), then
    // pieces appended without overwrite wherever the allocator has reached.
    const D3D11_MAP how = g.camera_no_overwrite && remember ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD;
    if (FAILED(c->Map(g.cam_cb.Get(), 0, how, 0, &m))) return;
    std::vector<uint8_t> bytes(g.campaign_camera ? 1280 : 512, 0);
    const float pad[4] = {1.f, 2.f, 3.f, 4.f};
    std::memcpy(bytes.data(), pad, 16);
    na::native11::StoreMatrix(bytes.data(), bytes.size(), 64, na::camera::Layout::RowMajor, vp);
    na::camera::Matrix inv{};
    na::camera::Invert(vp, inv);
    na::native11::StoreMatrix(bytes.data(), bytes.size(), 192, na::camera::Layout::RowMajor, inv);
    if (g.campaign_camera && remember) {
        std::fill(bytes.begin(), bytes.end(), uint8_t{0});
        const na::camera::Matrix relative{double(g.h) / g.w, 0, 0, 0, 0, 1, 0, 0,
                                          0, 0, 0, 4, 0, 0, 1, 0};
        const uint32_t pose_offset = g.campaign_native_pose ? (g.campaign_single_origin ? 192u : 208u) : 64u;
        for (const uint32_t offset : {pose_offset, 320u})
            na::native11::StoreMatrix(bytes.data(), bytes.size(), offset, na::camera::Layout::ColumnMajor, relative);
        const float origin[]{static_cast<float>(vp[3]), -10183.53808594f, -50.8396759f, 0.f};
        const float extent[]{float(g.w), float(g.h), 1.f / g.w, 1.f / g.h};
        if (!g.campaign_split_camera && !g.campaign_single_origin)
            std::memcpy(bytes.data() + (g.campaign_native_pose ? 800 : 384), origin, sizeof origin);
        if (!g.campaign_split_camera && !g.campaign_blank_witness)
            std::memcpy(bytes.data() + (g.campaign_native_pose ? 928 : 944), origin, sizeof origin);
        std::memcpy(bytes.data() + 704, extent, sizeof extent);
    }
    auto* destination = static_cast<uint8_t*>(m.pData);
    if (g.campaign_pixel_range) destination += g.camera_ring_offset;
    std::memcpy(destination, bytes.data(), bytes.size());
    c->Unmap(g.cam_cb.Get(), 0);
    if (remember) g.last_written = vp;
}

void SetObject(ID3D11DeviceContext* c, Game& g, int i)
{
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(c->Map(g.obj_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    const float o[4] = {float(i % 7) * 0.05f - 0.15f, float(i % 5) * 0.05f - 0.1f, 0, 0};
    std::memcpy(m.pData, o, 16);
    c->Unmap(g.obj_cb.Get(), 0);
}

void UploadSplitWorld(ID3D11DeviceContext* c, Game& g, float x)
{
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(c->Map(g.split_world.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    std::memset(m.pData, 0, 160);
    auto* dst = static_cast<uint8_t*>(m.pData);
    for (const size_t offset : {0u, 96u}) {
        const std::array<float, 16> world{1, 0, 0, x, 0, 1, 0, -8018, 0, 0, 1, 1746, 0, 0, 0, 1};
        std::memcpy(dst + offset, world.data(), 64);
    }
    c->Unmap(g.split_world.Get(), 0);
}

void BindSplitCamera(ID3D11DeviceContext* c, Game& g)
{
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(c->Map(g.split_projection.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        const na::camera::Matrix relative{double(g.h) / g.w, 0, 0, 0, 0, 1, 0, 0,
                                          0, 0, 0, 4, 0, 0, 1, 0};
        na::native11::StoreMatrix(static_cast<uint8_t*>(m.pData), 64, 0, na::camera::Layout::RowMajor, relative);
        c->Unmap(g.split_projection.Get(), 0);
    }
    UploadSplitWorld(c, g, 11659.f);
    ID3D11Buffer* split[]{g.split_world.Get(), g.split_projection.Get()};
    c->VSSetConstantBuffers(0, 2, split);
}

std::atomic<bool> g_tonemap_bound{false}; // the immediate context's tonemap read was bound this frame

void Viewport(ID3D11DeviceContext* c, UINT w, UINT h)
{
    const D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    c->RSSetViewports(1, &vp);
}


void Scene(ID3D11DeviceContext* c, Game& g, int frame, bool no_pose = false)
{
    g_tonemap_bound = false;
    if (g.rotate_camera) g.cam_cb = g.cam_pool[frame & 1];
    ID3D11Buffer* cbs[2] = {g.cam_cb.Get(), g.obj_cb.Get()};
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    c->VSSetShader(g.vs.Get(), nullptr, 0);
    if (g.campaign_pixel_range) {
        ID3D11Buffer* empty = nullptr;
        c->VSSetConstantBuffers(0, 1, &empty);
        c->VSSetConstantBuffers(1, 1, &cbs[1]);
        ComPtr<ID3D11DeviceContext1> c1;
        if (SUCCEEDED(c->QueryInterface(IID_PPV_ARGS(&c1)))) {
            const UINT first = g.camera_ring_offset / 16, count = 1280 / 16;
            c1->PSSetConstantBuffers1(0, 1, &cbs[0], &first, &count);
        }
    } else if (g.campaign_native_pose) {
        c->VSSetConstantBuffers(1, 1, &cbs[1]);
        c->VSSetConstantBuffers(3, 1, &cbs[0]);
        c->PSSetConstantBuffers(3, 1, &cbs[0]);
    } else {
        c->VSSetConstantBuffers(0, 2, cbs);
    }
    // Shadow pass.
    UploadCamera(c, g, ShadowVp(), false);
    ID3D11RenderTargetView* srt = g.shadow_rtv.Get();
    c->OMSetRenderTargets(1, &srt, g.shadow_dsv.Get());
    c->ClearDepthStencilView(g.shadow_dsv.Get(), D3D11_CLEAR_DEPTH, 1.f, 0);
    Viewport(c, 512, 512);
    c->PSSetShader(g.ps_scene.Get(), nullptr, 0);
    for (int i = 0; i < 20; ++i) { SetObject(c, g, i); c->Draw(4, 0); }
    // Main scene.
    UploadCamera(c, g, no_pose ? na::camera::Matrix{} : MainVp(frame), true);
    ID3D11RenderTargetView* rt = g.on_screen ? g.back_rtv.Get() : g.color_rtv.Get();
    ID3D11RenderTargetView* layers[2] = {g.layer_rtv.Get(), g.layer2_rtv.Get()};
    if (g.layered) c->OMSetRenderTargets(2, layers, g.dsv.Get());
    else c->OMSetRenderTargets(1, &rt, g.dsv.Get());
    const float clear[4] = {0.1f, 0.2f, 0.3f, 1.f};
    c->ClearRenderTargetView(rt, clear);
    c->ClearDepthStencilView(g.dsv.Get(), g.shader_depth ? D3D11_CLEAR_DEPTH : D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.f, 0);
    Viewport(c, g.w, g.h);
    if (g.campaign_split_camera) BindSplitCamera(c, g);
    for (int i = 0; i < 40; ++i) {
        // Rewrite the same buffer to an identity-basis object pose. These
        // draws must never get the support of the earlier world-buffer version.
        if (g.campaign_split_camera && i == (g.split_tie ? 20 : 39)) UploadSplitWorld(c, g, 7.f);
        if (g.campaign_mixed_ring && i == 20) {
            // Real campaign rings carry camera and non-camera slices at the
            // same binding. Discard/reuse it after the camera's scene draws.
            D3D11_MAPPED_SUBRESOURCE map{};
            if (SUCCEEDED(c->Map(g.cam_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
                std::memset(map.pData, 0, 32768);
                c->Unmap(g.cam_cb.Get(), 0);
            }
            ComPtr<ID3D11DeviceContext1> c1;
            if (SUCCEEDED(c->QueryInterface(IID_PPV_ARGS(&c1)))) {
                const UINT first = 0, count = 16;
                auto* ring = g.cam_cb.Get();
                c1->PSSetConstantBuffers1(0, 1, &ring, &first, &count);
                c1->VSSetConstantBuffers1(2, 1, &ring, &first, &count);
            }
        }
        SetObject(c, g, i); c->Draw(4, 0);
    }
    if (g.layered) {
        // Lighting: reads the layers, writes the picture, no depth.
        c->OMSetRenderTargets(1, &rt, nullptr);
        ID3D11ShaderResourceView* layer = g.layer_srv.Get();
        c->PSSetShaderResources(0, 1, &layer);
        c->VSSetShader(g.vs_post.Get(), nullptr, 0);
        c->PSSetShader(g.ps_tonemap.Get(), nullptr, 0);
        c->Draw(4, 0);
        ID3D11ShaderResourceView* none = nullptr;
        c->PSSetShaderResources(0, 1, &none);
        // The rest of the scene (sky, glass, smoke): into the picture, with the scene depth.
        c->VSSetShader(g.vs.Get(), nullptr, 0);
        c->PSSetShader(g.ps_scene.Get(), nullptr, 0);
        c->OMSetRenderTargets(1, &rt, g.dsv.Get());
        for (int i = 0; i < 4; ++i) { SetObject(c, g, i); c->Draw(4, 0); }
    }
}



void Post(ID3D11DeviceContext* c, Game& g, bool immediate)
{
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    c->VSSetShader(g.vs_post.Get(), nullptr, 0);
    // Tonemap: reads the scene colour (the insertion point is just before this bind).
    ID3D11RenderTargetView* bb = g.back_rtv.Get();
    if (g.on_screen) {
        // No tonemap and no read: the picture is already on the screen. Only the HUD follows.
        c->OMSetRenderTargets(1, &bb, g.on_screen == 2 ? g.dsv.Get() : nullptr);
        if (g.on_screen == 2) c->OMSetDepthStencilState(g.depth_off.Get(), 0);
        const float factor[4] = {};
        c->OMSetBlendState(g.blend.Get(), factor, 0xffffffffu);
        c->PSSetShader(g.ps_hud.Get(), nullptr, 0);
        Viewport(c, g.w, g.h);
        for (int i = 0; i < 6; ++i) c->Draw(4, 0);
        c->OMSetBlendState(nullptr, factor, 0xffffffffu);
        c->OMSetDepthStencilState(nullptr, 0);
        return;
    }
    c->OMSetRenderTargets(1, &bb, nullptr);
    ID3D11ShaderResourceView* srv = g.color_srv.Get();
    c->PSSetShaderResources(0, 1, &srv);
    if (immediate) g_tonemap_bound = true;
    c->PSSetShader(g.ps_tonemap.Get(), nullptr, 0);
    Viewport(c, g.w, g.h);
    c->Draw(4, 0);
    ID3D11ShaderResourceView* none = nullptr;
    c->PSSetShaderResources(0, 1, &none);
    // HUD.
    const float bf[4] = {};
    c->OMSetBlendState(g.blend.Get(), bf, 0xffffffffu);
    c->PSSetShader(g.ps_hud.Get(), nullptr, 0);
    for (int i = 0; i < 6; ++i) c->Draw(4, 0);
    c->OMSetBlendState(nullptr, bf, 0xffffffffu);
}

void Pump()
{
#if defined(NA_TEST_NATIVE_DLAA)
    // Exercise the target cadence without flooding the fixed in-flight ring
    // from an invisible, uncapped test window.
    Sleep(20);
#endif
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
}

// ---- the stand-in consumer ----
#if defined(NA_TEST_NATIVE_DLAA)
uint64_t evaluations = 0;
#endif
struct Seen {
    Game* game = nullptr;
    ComPtr<ID3D11Texture2D> copy; // the consumer's own texture (what B2's shared input would be)
    uint64_t calls = 0, wrong_inputs = 0, after_tonemap = 0, no_scene = 0, no_camera = 0, twice = 0;
    uint64_t before_read = 0, before_list = 0, jitter_reported = 0, changed = 0;
    uint64_t last_frame = ~0ull, last_stream = 0, streams = 0;
    // No-camera phase: frames handed over without a camera, frames that broke what the phase expects
    // (a camera where none may be, or camera facts on a frame that says it has none), scene changes
    // (colour or depth object) as the provider reports them apart from camera changes.
    uint64_t without_camera = 0, camera_leak = 0, scene_changed = 0, reversed_depth = 0;
    uint32_t color_w = 0, color_h = 0;
    uint64_t color_inc = 0, depth_inc = 0;
    na::native11::SceneFrame last{};
};
Seen g_seen;
// 0: every frame carries the camera (all phases but one). 1: no frame may carry one. 2: either.
int g_no_camera_phase = 0;

bool Consume(void* user, void* context, const na::native11::SceneFrame& f)
{
    Seen& s = *static_cast<Seen*>(user);
    Game& g = *s.game;
    ++s.calls;
    if (f.frame == s.last_frame) ++s.twice;
    s.last_frame = f.frame;
    ComPtr<ID3D11Resource> picture = g.color;
    if (g.on_screen && g.back_rtv) { picture.Reset(); g.back_rtv->GetResource(&picture); }
    const bool inputs = f.color.resource == reinterpret_cast<uint64_t>(picture.Get()) &&
                        f.depth.resource == reinterpret_cast<uint64_t>(static_cast<ID3D11Resource*>(g.depth.Get())) &&
                        f.color.format == static_cast<uint32_t>(g.on_screen ? DXGI_FORMAT_R8G8B8A8_UNORM : kSceneFormat) && f.depth.format == DXGI_FORMAT_D24_UNORM_S8_UINT &&
                        f.color.width == g.w && f.color.height == g.h && f.color.active_x == 0 && f.color.active_y == 0 &&
                        f.color.active_w == g.w && f.color.active_h == g.h && f.color.incarnation && f.depth.incarnation &&
                        f.color.d3d11_resource == picture.Get();
    if (!inputs) ++s.wrong_inputs;
    if (g_tonemap_bound && f.boundary == na::native11::Boundary::BeforeRead) ++s.after_tonemap;
    if (f.scene_draws != 40) ++s.no_scene;
    const UINT camera_size = g.campaign_split_camera ? 64u : g.campaign_pixel_range ? g.camera_snapshot_size : g.campaign_camera ? 1280u : 512u;
    const int vs_slot = g.campaign_split_camera ? 1 : g.campaign_pixel_range ? -1 : g.campaign_native_pose ? 3 : 0;
    auto* expected_cb = g.campaign_split_camera ? g.split_projection.Get() : g.cam_cb.Get();
    if (f.scene_changed) ++s.scene_changed;
    if (f.reversed_depth) ++s.reversed_depth;
    const bool camera_absent = g_no_camera_phase != 0 && !f.camera_known;
    if (camera_absent) {
        ++s.without_camera;
        if (f.camera.bytes || f.camera.size || f.camera.buffer || f.camera.vs_slot >= 0 || f.camera.ps_slot >= 0) ++s.camera_leak;
    } else if (g_no_camera_phase == 1) {
        ++s.camera_leak;
    }
    if (camera_absent) {
        // nothing camera-related to check: the frame says it has none
    } else if (!f.camera.bytes || (g.campaign_pixel_range && g.camera_ring_offset >= 65536 ?
        f.camera.size < 1280 || f.camera.size > 16 * 1024 : f.camera.size != camera_size) || f.camera.buffer != reinterpret_cast<uint64_t>(static_cast<ID3D11Resource*>(expected_cb)) ||
        f.camera.vs_slot != vs_slot) {
        if (!s.no_camera)
            std::printf("camera handed over: bytes=%p size=%u buffer=%llx (game %llx) slot=%d first=%u num=%u version=%llu\n",
                        static_cast<const void*>(f.camera.bytes), f.camera.size, static_cast<unsigned long long>(f.camera.buffer),
                        static_cast<unsigned long long>(reinterpret_cast<uint64_t>(static_cast<ID3D11Resource*>(g.cam_cb.Get()))),
                        f.camera.vs_slot, f.camera.first_constant, f.camera.num_constants, static_cast<unsigned long long>(f.camera.version));
        ++s.no_camera;
    }
    if (f.boundary == na::native11::Boundary::BeforeRead) ++s.before_read;
    if (f.boundary == na::native11::Boundary::BeforeList) ++s.before_list;
    if (f.camera.jitter_applied && (f.camera.jitter_px[0] != 0.f || f.camera.jitter_px[1] != 0.f)) ++s.jitter_reported;
    if (f.selection_changed) ++s.changed;
    if (f.stream != s.last_stream) {
        ++s.streams;
        std::printf("stream %llu at call %llu (changed=%d colour=%llu depth=%llu)\n", static_cast<unsigned long long>(f.stream),
                    static_cast<unsigned long long>(s.calls), f.selection_changed ? 1 : 0, static_cast<unsigned long long>(f.color.incarnation),
                    static_cast<unsigned long long>(f.depth.incarnation));
    }
    s.last_stream = f.stream;
    s.color_w = f.color.active_w;
    s.color_h = f.color.active_h;
    s.color_inc = f.color.incarnation;
    s.depth_inc = f.depth.incarnation;
    s.last = f;
    // The consumer's own work on the game's context, inside the callback (B2's input copy).
    auto* c = static_cast<ID3D11DeviceContext*>(context);
    if (s.copy) {
        D3D11_TEXTURE2D_DESC d{};
        s.copy->GetDesc(&d);
        if (d.Width != g.w || d.Height != g.h) s.copy.Reset();
    }
    if (!s.copy) {
        D3D11_TEXTURE2D_DESC d{};
        g.color->GetDesc(&d);
        d.BindFlags = 0;
        d.Usage = D3D11_USAGE_DEFAULT;
        g.device->CreateTexture2D(&d, nullptr, &s.copy);
    }
    if (s.copy && !g.on_screen) c->CopyResource(s.copy.Get(), static_cast<ID3D11Resource*>(f.color.d3d11_resource));
#if defined(NA_TEST_NATIVE_DLAA)
    if (na::native_d3d11::EvaluateFrame(nullptr, context, f)) ++evaluations;
#endif
    return true;
}

// Reads one pixel of the consumer's copy (x = 0, y = 0).
bool ReadCopy(Game& g, float out[4], ID3D11Texture2D* source = nullptr)
{
    if (!source) source = g_seen.copy.Get();
    if (!source) return false;
    D3D11_TEXTURE2D_DESC d{};
    source->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> st;
    if (FAILED(g.device->CreateTexture2D(&d, nullptr, &st))) return false;
    g.ctx->CopyResource(st.Get(), source);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g.ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
#if defined(NA_TEST_NATIVE_DLAA)
    DirectX::PackedVector::XMFLOAT3PK packed;
    std::memcpy(&packed, m.pData, sizeof packed);
    DirectX::XMFLOAT3 rgb;
    DirectX::XMStoreFloat3(&rgb, DirectX::PackedVector::XMLoadFloat3PK(&packed));
    out[0] = rgb.x; out[1] = rgb.y; out[2] = rgb.z; out[3] = 1.f;
#else
    const uint16_t* p = static_cast<const uint16_t*>(m.pData);
    for (int i = 0; i < 4; ++i) {
        const uint16_t h = p[i];
        const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 31, man = h & 1023;
        float v = exp == 0 ? std::ldexp(float(man), -24) : std::ldexp(float(man | 1024), int(exp) - 25);
        out[i] = sign ? -v : v;
    }
#endif
    g.ctx->Unmap(st.Get(), 0);
    return true;
}

} // namespace

#if !defined(NA_TEST_NATIVE_DLAA)
int DepthReadFixture(Game& g, bool debug_layer)
{
    const auto ramp = Compile("vs_depth_ramp", "vs_5_0");
    const auto slab = Compile("ps_depth_slab", "ps_5_0");
    const auto compute = Compile("cs_depth_slab", "cs_5_0");
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11ComputeShader> cs;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11GeometryShader> gs;
    const auto geom = Compile("gs_passthrough", "gs_5_0");
    if (!geom || FAILED(g.device->CreateGeometryShader(geom->GetBufferPointer(), geom->GetBufferSize(), nullptr, &gs)))
        return 1;
    D3D11_RASTERIZER_DESC raster_desc{};
    raster_desc.FillMode = D3D11_FILL_SOLID;
    raster_desc.CullMode = D3D11_CULL_NONE;
    raster_desc.DepthClipEnable = TRUE;
    D3D11_DEPTH_STENCIL_DESC no_test{};
    no_test.DepthFunc = D3D11_COMPARISON_ALWAYS;
    no_test.FrontFace = no_test.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                                           D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
    if (!ramp || !slab || !compute ||
        FAILED(g.device->CreateVertexShader(ramp->GetBufferPointer(), ramp->GetBufferSize(), nullptr, &vs)) ||
        FAILED(g.device->CreatePixelShader(slab->GetBufferPointer(), slab->GetBufferSize(), nullptr, &ps)) ||
        FAILED(g.device->CreateComputeShader(compute->GetBufferPointer(), compute->GetBufferSize(), nullptr, &cs)) ||
        FAILED(g.device->CreateDepthStencilState(&no_test, &g.depth_off)) ||
        FAILED(g.device->CreateRasterizerState(&raster_desc, &raster))) return 1;
    D3D11_SHADER_RESOURCE_VIEW_DESC read{};
    read.Format = DXGI_FORMAT_R32_FLOAT;
    read.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    read.Texture2D.MipLevels = 1;
    ComPtr<ID3D11ShaderResourceView> original, copy_view;
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DXGI_FORMAT_D32_FLOAT;
    dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    dsv.Flags = D3D11_DSV_READ_ONLY_DEPTH;
    ComPtr<ID3D11DepthStencilView> read_only;
    if (FAILED(g.device->CreateShaderResourceView(g.depth.Get(), &read, &original)) ||
        FAILED(g.device->CreateDepthStencilView(g.depth.Get(), &dsv, &read_only))) return 1;
    D3D11_TEXTURE2D_DESC desc{};
    g.depth->GetDesc(&desc);
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> copy, mask, staging;
    if (FAILED(g.device->CreateTexture2D(&desc, nullptr, &copy)) ||
        FAILED(g.device->CreateShaderResourceView(copy.Get(), &read, &copy_view))) return 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11RenderTargetView> mask_rtv;
    ComPtr<ID3D11UnorderedAccessView> mask_uav;
    if (FAILED(g.device->CreateTexture2D(&desc, nullptr, &mask)) ||
        FAILED(g.device->CreateRenderTargetView(mask.Get(), nullptr, &mask_rtv)) ||
        FAILED(g.device->CreateUnorderedAccessView(mask.Get(), nullptr, &mask_uav))) return 1;
    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(g.device->CreateTexture2D(&desc, nullptr, &staging))) return 1;
    const auto read_mask = [&] {
        g.ctx->CopyResource(staging.Get(), mask.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(g.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return -1.f;
        const auto* row = static_cast<const uint8_t*>(mapped.pData) + 67 * mapped.RowPitch;
        const float result = reinterpret_cast<const float*>(row)[173];
        g.ctx->Unmap(staging.Get(), 0);
        return result;
    };
    int frame = 0;
    for (; frame < 40; ++frame) {
        Scene(g.ctx.Get(), g, frame);
        Post(g.ctx.Get(), g, true);
        g.swap->Present(0, 0);
    }
    expect(g_seen.calls != 0, "the provider learned the scene with shader-readable depth");
    na::native11::SetJitterRoute(na::native11::JitterRoute::Viewport);
    ComPtr<ID3D11Query> raster_query;
    const D3D11_QUERY_DESC query_desc{D3D11_QUERY_OCCLUSION, 0};
    if (FAILED(g.device->CreateQuery(&query_desc, &raster_query))) return 1;
    const auto render = [&](float jitter, bool enable, bool use_compute, bool use_copy, bool defer,
                            bool query = false, bool mixed = false, bool geometry_shader = false) {
        na::native11::SetDepthReads(enable);
        na::native11::SetNextJitter(-jitter, jitter);
        g.swap->Present(0, 0);
        Scene(g.ctx.Get(), g, frame++);
        g.ctx->ClearDepthStencilView(g.dsv.Get(), D3D11_CLEAR_DEPTH, 1.f, 0);
        g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.ctx->RSSetState(raster.Get());
        g.ctx->VSSetShader(vs.Get(), nullptr, 0);
        g.ctx->PSSetShader(g.ps_scene.Get(), nullptr, 0);
        g.ctx->Draw(3, 0);
        if (mixed) {
            // A write through a small viewport into the same depth texture.
            // Its footprint is incompatible with the full-size correction.
            Viewport(g.ctx.Get(), 12, 10);
            g.ctx->Draw(3, 0);
        }
        if (defer) {
            ComPtr<ID3D11DeviceContext> deferred;
            ComPtr<ID3D11CommandList> list;
            if (FAILED(g.device->CreateDeferredContext(0, &deferred)) ||
                FAILED(deferred->FinishCommandList(FALSE, &list))) return -1.f;
            g.ctx->ExecuteCommandList(list.Get(), TRUE);
        }
        if (use_copy) g.ctx->CopyResource(copy.Get(), g.depth.Get());
        auto* view = use_copy ? copy_view.Get() : original.Get();
        auto* target = mask_rtv.Get();
        const float sentinel[]{-1.f, -1.f, -1.f, -1.f};
        g.ctx->ClearRenderTargetView(target, sentinel);
        g.ctx->OMSetDepthStencilState(g.depth_off.Get(), 0);
        Viewport(g.ctx.Get(), g.w, g.h);
        if (use_compute) {
            g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
            auto* uav = mask_uav.Get();
            g.ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
            g.ctx->CSSetShaderResources(1, 1, &view);
            g.ctx->CSSetShader(cs.Get(), nullptr, 0);
            g.ctx->Dispatch(40, 23, 1);
            ComPtr<ID3D11ShaderResourceView> bound;
            g.ctx->CSGetShaderResources(1, 1, &bound);
            expect(bound.Get() == view, "compute still sees its original SRV after dispatch");
            uav = nullptr;
            g.ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
            view = nullptr;
            g.ctx->CSSetShaderResources(1, 1, &view);
        } else {
            g.ctx->OMSetRenderTargets(1, &target, read_only.Get());
            g.ctx->PSSetShaderResources(1, 1, &view);
            g.ctx->PSSetShader(ps.Get(), nullptr, 0);
            if (geometry_shader) g.ctx->GSSetShader(gs.Get(), nullptr, 0);
            if (query) g.ctx->Begin(raster_query.Get());
            g.ctx->Draw(3, 0);
            if (query) g.ctx->End(raster_query.Get());
            if (geometry_shader) {
                ComPtr<ID3D11GeometryShader> saved;
                g.ctx->GSGetShader(&saved, nullptr, nullptr);
                expect(saved.Get() == gs.Get(), "depth correction preserves the consumer's geometry shader");
                g.ctx->GSSetShader(nullptr, nullptr, 0);
            }
            ComPtr<ID3D11ShaderResourceView> bound;
            ComPtr<ID3D11DepthStencilView> bound_depth;
            g.ctx->PSGetShaderResources(1, 1, &bound);
            g.ctx->OMGetRenderTargets(0, nullptr, &bound_depth);
            expect(bound.Get() == view && bound_depth.Get() == read_only.Get(), "the draw restores the game's SRV and read-only DSV");
            view = nullptr;
            g.ctx->PSSetShaderResources(1, 1, &view);
            g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
        }
        const float result = read_mask();
        if (result < 0) std::printf("lighting draw left its destination unwritten: compute=%d copy=%d geometry=%d\n",
                                    use_compute, use_copy, geometry_shader);
        g.ctx->OMSetDepthStencilState(nullptr, 0);
        Post(g.ctx.Get(), g, true);
        return result;
    };
    const float raw_zero = render(0.f, false, false, false, false);
    const float raw_positive = render(.375f, false, false, false, false);
    const float raw_negative = render(-.375f, false, false, false, false);
    const float corrected_positive = render(.375f, true, false, false, false);
    const float corrected_negative = render(-.375f, true, false, false, false);
    const float corrected_compute = render(.375f, true, true, false, false);
    const float corrected_copy = render(.375f, true, false, true, false);
    if (!debug_layer) {
        expect(render(.375f, true, false, false, false, false, false, true) == 1.f,
               "the isolated depth correction works with a game geometry shader bound");
        bool held = true;
        for (int i = 0; i < 4; ++i) {
            held &= render(.375f, true, false, false, false, false, true) == raw_positive;
            held &= render(.375f, true, false, false, false) == raw_positive;
        }
        expect(held, "alternating usable and mixed writes leave the same lighting reader on original depth");
        expect(render(.375f, true, true, false, false) == 1.f,
               "an independent compute reader still receives corrected current depth");
        expect(render(.375f, true, false, false, false, false, true) == raw_positive,
               "an unqualified lighting read restarts only its own recovery");
        const float first = render(.375f, true, false, false, false);
        const float second = render(.375f, true, false, false, false);
        const float third = render(.375f, true, false, false, false);
        expect(first == raw_positive && second == raw_positive && third == 1.f,
               "the lighting reader resumes correction after three usable frames");
    }
    const float deferred = render(.375f, true, false, false, true);
    const float queried = render(.375f, true, false, false, false, true);
    std::printf("depth slab: vanilla=%g jittered=%g,%g corrected=%g,%g compute=%g copy=%g deferred=%g\n",
                double(raw_zero), double(raw_positive), double(raw_negative), double(corrected_positive),
                double(corrected_negative), double(corrected_compute), double(corrected_copy), double(deferred));
    expect(raw_zero == 1.f && raw_positive == 0.f && raw_negative == 0.f, "actual viewport jitter causes the slab to disappear");
    if (debug_layer) {
        expect(corrected_positive == raw_positive && corrected_negative == raw_negative &&
               corrected_compute == raw_positive && corrected_copy == raw_positive,
               "the debug wrapper preserves the original depth for all consumers");
    } else {
        expect(corrected_positive == 1.f && corrected_negative == 1.f, "depth shader reads preserve the unjittered slab at both offsets");
        expect(corrected_compute == 1.f && corrected_copy == 1.f, "dispatch and compatible depth copies get the corrected values");
    }
    expect(deferred == raw_positive, "an external deferred list leaves the game's depth reads alone");
    expect(queried == raw_positive, "the correction does not add work to an active game occlusion query");
    ComPtr<ID3D11InfoQueue> debug;
    if (SUCCEEDED(g.device.As(&debug))) {
        for (UINT64 index = 0; index < debug->GetNumStoredMessages(); ++index) {
            SIZE_T size = 0;
            if (FAILED(debug->GetMessage(index, nullptr, &size))) continue;
            std::vector<uint8_t> data(size);
            auto* message = reinterpret_cast<D3D11_MESSAGE*>(data.data());
            if (SUCCEEDED(debug->GetMessage(index, message, &size)) && message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
                expect(false, message->pDescription);
        }
    }
    if (g_failed) {
        for (const auto& event : na::eventlog::Drain())
            if (event.find(" depth_reads ") != std::string::npos) std::printf("%s", event.c_str());
    }
    na::native11::SetDepthReads(false);
    na::native11::Uninstall();
    return g_failed ? 1 : 0;
}
#endif

int ScreenCheckFixture(Game& game, bool adaptive, bool missed_order = false, [[maybe_unused]] bool rotating_lights = false,
                       [[maybe_unused]] bool compare_routes = false, [[maybe_unused]] bool composition_fault = false,
                       [[maybe_unused]] bool reuse_depth = false)
{
    struct Surface {
        ComPtr<ID3D11Texture2D> picture;
        ComPtr<ID3D11RenderTargetView> rtv;
        ComPtr<ID3D11ShaderResourceView> srv;
    } decoy, canvas, other_canvas, third_canvas, second_output;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = game.w + (adaptive ? 1 : 0); // one padding column after the active viewport
    desc.Height = game.h;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (auto* surface : {&decoy, &canvas, &other_canvas, &third_canvas, &second_output}) {
        if (FAILED(game.device->CreateTexture2D(&desc, nullptr, &surface->picture)) ||
            FAILED(game.device->CreateRenderTargetView(surface->picture.Get(), nullptr, &surface->rtv)) ||
            FAILED(game.device->CreateShaderResourceView(surface->picture.Get(), nullptr, &surface->srv))) return 1;
    }
    desc.Width = game.w;
#if defined(NA_TEST_NATIVE_DLAA)
    const auto color_copy = [&](DXGI_FORMAT format) -> ComPtr<ID3D11Texture2D> {
        auto staging = desc;
        staging.Format = format;
        staging.BindFlags = 0;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> result;
        game.device->CreateTexture2D(&staging, nullptr, &result);
        return result;
    };
    const auto format = game.padded_hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : kSceneFormat;
    const auto before = color_copy(format), after = color_copy(format);
    if (game.padded_hdr) desc.Height += 3;
    const auto shown = color_copy(DXGI_FORMAT_R8G8B8A8_UNORM);
    if (!before || !after || !shown) return 1;
#endif
    desc.Width = desc.Height = 1;
    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> pixel;
    if (FAILED(game.device->CreateTexture2D(&desc, nullptr, &pixel))) return 1;
    auto code = Compile("ps_screen_check", "ps_5_0");
    auto variant_code = Compile("ps_scene_variant", "ps_5_0");
    ComPtr<ID3D11PixelShader> variant;
    if (!variant_code || FAILED(game.device->CreatePixelShader(variant_code->GetBufferPointer(),
        variant_code->GetBufferSize(), nullptr, &variant))) return 1;
    ComPtr<ID3D11PixelShader> composite;
    if (!code || FAILED(game.device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &composite))) return 1;
    bool multiple_outputs = false;
    auto reader = [&](ID3D11RenderTargetView* target, ID3D11ShaderResourceView* input, ID3D11PixelShader* shader) {
        ID3D11RenderTargetView* outputs[]{target, second_output.rtv.Get()};
        game.ctx->OMSetRenderTargets(multiple_outputs && target != game.back_rtv.Get() ? 2 : 1, outputs, nullptr);
        game.ctx->PSSetShaderResources(0, 1, &input);
        game.ctx->PSSetShader(shader, nullptr, 0);
        game.ctx->Draw(4, 0);
        input = nullptr;
        game.ctx->PSSetShaderResources(0, 1, &input);
    };
    auto read = [&](ID3D11Texture2D* source) -> std::array<BYTE, 4> {
        na::native11::OwnWorkScope own;
        const D3D11_BOX box{2, 2, 0, 3, 3, 1};
        game.ctx->CopySubresourceRegion(pixel.Get(), 0, 0, 0, 0, source, 0, &box);
        D3D11_MAPPED_SUBRESOURCE map{};
        if (FAILED(game.ctx->Map(pixel.Get(), 0, D3D11_MAP_READ, 0, &map))) return {};
        std::array<BYTE, 4> rgba{};
        std::memcpy(rgba.data(), map.pData, rgba.size());
        game.ctx->Unmap(pixel.Get(), 0);
        return rgba;
    };

    struct Evaluator {
        Game& game;
        bool ordinary = true;
        uint64_t writes = 0;
        uint64_t proof = 0;
#if defined(NA_TEST_NATIVE_DLAA)
        ID3D11Texture2D* before;
        ID3D11Texture2D* after;
        ID3D11Resource* peer = nullptr;
        ID3D11Resource* received = nullptr;
        ID3D11RenderTargetView* expected_reader = nullptr;
        uint64_t scene_stream = 0, camera_stream = 0;
        bool reset = false, displayed_reader = false;
        unsigned sharpening_trial_frames = 0;
#endif
    } evaluator{game
#if defined(NA_TEST_NATIVE_DLAA)
        , true, 0, 0, before.Get(), after.Get()
#endif
    };
    auto consume = [](void* user, void* context, const na::native11::SceneFrame& frame) {
        auto& state = *static_cast<Evaluator*>(user);
        auto& g = state.game;
        if (!state.ordinary && !frame.screen_proof) return false;
        auto* source = static_cast<ID3D11Resource*>(frame.color.d3d11_resource);
        if (frame.depth.d3d11_resource != g.depth.Get()) return false;
#if defined(NA_TEST_NATIVE_DLAA)
        if (source != g.color.Get() && (!state.peer || source != state.peer)) return false;
#else
        if (source != g.color.Get()) return false;
#endif
        auto* ctx = static_cast<ID3D11DeviceContext*>(context);
#if defined(NA_TEST_NATIVE_DLAA)
        ctx->CopyResource(state.before, source);
        if (state.expected_reader) {
            ComPtr<ID3D11RenderTargetView> actual;
            ctx->OMGetRenderTargets(1, &actual, nullptr);
            state.displayed_reader = actual.Get() == state.expected_reader;
        }
        if (!na::native_d3d11::EvaluateFrame(nullptr, context, frame)) return false;
        if (frame.recovery_tool == na::referee::RecoveryTool::Sharpen) ++state.sharpening_trial_frames;
        ctx->CopyResource(state.after, source);
        state.received = source;
        state.scene_stream = frame.scene_stream;
        state.camera_stream = frame.stream;
        state.reset = frame.scene_changed || frame.selection_changed;
#else
        const float replacement[]{.75f, .25f, .125f, 1.f};
        ctx->ClearRenderTargetView(g.color_rtv.Get(), replacement);
#endif
        ++state.writes;
        state.proof = frame.screen_proof;
        return true;
    };
#if defined(NA_TEST_NATIVE_DLAA)
    const auto recovery_evidence = +[](uint64_t ticket, na::referee::TrialSample& sample) {
        if (!na::referee::gpu::TakeTrialSample(ticket, sample)) return false;
        std::printf("accepted frame=%llu ticket=%llx delivered=%d comparable=%d first=%s region=%u tools=%u applied=%u\n",
            static_cast<unsigned long long>(sample.ordinal), static_cast<unsigned long long>(ticket),
            sample.delivered, sample.comparable, na::referee::StageName(sample.origin.stage), sample.origin.region,
            unsigned(sample.tools), unsigned(sample.applied));
        return true;
    };
    if (!na::native11::Install(game.device.Get(), consume, &evaluator, compare_routes,
            composition_fault ? recovery_evidence : compare_routes ? &na::referee::gpu::TakeTrialSample : nullptr)) return 1;
#else
    if (!na::native11::Install(game.device.Get(), consume, &evaluator)) return 1;
#endif
    na::native11::SetCameraUse(adaptive ? na::native11::CameraUse::Off : na::native11::CameraUse::Optional);
    // The synthetic quads use the opposite winding to the default cull state.
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> visible;
    if (FAILED(game.device->CreateRasterizerState(&raster, &visible))) return 1;
    game.ctx->RSSetState(visible.Get());
#if defined(NA_TEST_NATIVE_DLAA)
    if (rotating_lights) {
        Surface peer;
        D3D11_TEXTURE2D_DESC hdr{};
        game.color->GetDesc(&hdr);
        D3D11_RENDER_TARGET_VIEW_DESC rv{};
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        game.color_rtv->GetDesc(&rv);
        game.color_srv->GetDesc(&sv);
        if (FAILED(game.device->CreateTexture2D(&hdr, nullptr, &peer.picture)) ||
            FAILED(game.device->CreateRenderTargetView(peer.picture.Get(), &rv, &peer.rtv)) ||
            FAILED(game.device->CreateShaderResourceView(peer.picture.Get(), &sv, &peer.srv))) return 1;
        auto box_code = Compile("ps_letterbox", "ps_5_0");
        ComPtr<ID3D11PixelShader> letterbox;
        if (!box_code || FAILED(game.device->CreatePixelShader(box_code->GetBufferPointer(), box_code->GetBufferSize(), nullptr, &letterbox))) return 1;
        evaluator.peer = peer.picture.Get();
        evaluator.expected_reader = game.padded_hdr ? other_canvas.rtv.Get() : game.back_rtv.Get();
        auto blob = Compile(compare_routes ? "ps_trial_light" : "ps_reflection", "ps_5_0");
        auto vertex = Compile("vs_reflection", "vs_5_0");
        ComPtr<ID3D11VertexShader> lit_vs;
        ComPtr<ID3D11PixelShader> reflection;
        if (!blob || FAILED(game.device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &reflection)) ||
            !vertex || FAILED(game.device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &lit_vs))) return 1;
        ComPtr<ID3D11PixelShader> trial_surface, trial_screen;
        ComPtr<ID3D11Buffer> trial_flash;
        if (compare_routes) {
            auto surface = Compile("ps_trial_surface", "ps_5_0");
            auto screen_shader = Compile("ps_trial_screen", "ps_5_0");
            const D3D11_BUFFER_DESC flags{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
            if (!surface || !screen_shader ||
                FAILED(game.device->CreatePixelShader(surface->GetBufferPointer(), surface->GetBufferSize(), nullptr, &trial_surface)) ||
                FAILED(game.device->CreatePixelShader(screen_shader->GetBufferPointer(), screen_shader->GetBufferSize(), nullptr, &trial_screen)) ||
                FAILED(game.device->CreateBuffer(&flags, nullptr, &trial_flash))) return 1;
        }
        na::native11::SetCameraUse(na::native11::CameraUse::Off);
        ComPtr<ID3D11Texture2D> back;
        if (FAILED(game.swap->GetBuffer(0, IID_PPV_ARGS(&back)))) return 1;
        struct Sample {
            int correct = 0, evaluated = 0, proven = 0, fixed_history = 0, displayed_readers = 0;
            size_t changed = 0, reached = 0, comparisons = 0;
            double movement = 0, brightness = 0;
            uint64_t scene = 0, camera = 0;
            std::vector<int> previous;
        };
        bool start_fault = !composition_fault;
        auto record = [&](int i, bool light_on, Sample* sample, bool pinned = false, bool overwrite = false,
                          bool clear_before_reader = false) {
            Pump();
            const auto start = evaluator.writes;
            evaluator.received = nullptr;
            evaluator.proof = 0;
            // Include consecutive frames with the same owner: parity cannot solve this route.
            const bool swap_roles = !pinned && (i % 7 == 2 || i % 7 == 4 || i % 7 == 5);
            auto* early_rt = swap_roles ? peer.rtv.Get() : game.color_rtv.Get();
            auto* late_rt = swap_roles ? game.color_rtv.Get() : peer.rtv.Get();
            auto* early = swap_roles ? peer.srv.Get() : game.color_srv.Get();
            auto* late = swap_roles ? game.color_srv.Get() : peer.srv.Get();
            auto* expected = swap_roles ? game.color.Get() : peer.picture.Get();
            auto* c = game.ctx.Get();
            const D3D11_VIEWPORT vp{0, 0, float(game.w), float(game.h), 0, 1};
            c->RSSetViewports(1, &vp);
            c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            c->VSSetShader(game.vs_post.Get(), nullptr, 0);
            c->ClearDepthStencilView(game.dsv.Get(), D3D11_CLEAR_DEPTH, 1, 0);
            c->OMSetDepthStencilState(nullptr, 0);
            ID3D11RenderTargetView* layers[]{canvas.rtv.Get(), second_output.rtv.Get()};
            c->OMSetRenderTargets(2, layers, game.dsv.Get());
            c->PSSetShader(compare_routes ? trial_surface.Get() : game.ps_scene.Get(), nullptr, 0);
            for (int n = 0; n < 34; ++n) c->Draw(4, 0);
            // Keep the depth test active against the layer depth and draw coplanar fullscreen lighting.
            c->ClearDepthStencilView(game.dsv.Get(), D3D11_CLEAR_DEPTH, 1, 0);
            c->OMSetRenderTargets(1, &early_rt, game.dsv.Get());
            auto* material = canvas.srv.Get();
            c->PSSetShaderResources(0, 1, &material);
            c->PSSetShader(game.ps_tonemap.Get(), nullptr, 0);
            for (int n = 0; n < 9; ++n) c->Draw(4, 0);
            c->ClearDepthStencilView(game.dsv.Get(), D3D11_CLEAR_DEPTH, 1, 0);
            c->OMSetRenderTargets(1, &late_rt, game.dsv.Get());
            c->PSSetShaderResources(0, 1, &early);
            c->VSSetShader(lit_vs.Get(), nullptr, 0);
            // Keep the exposure identical across the rotating owners in the
            // isolated composition case. The other fixture's .75 gain would
            // introduce a second, whole-picture change at the earlier route.
            c->PSSetShader(light_on ? reflection.Get() : composition_fault ? game.ps_tonemap.Get() : composite.Get(), nullptr, 0);
            for (int n = 0; n < 6; ++n) c->Draw(4, 0);
            if (overwrite) {
                const float new_picture[]{.2f, .3f, .4f, 1};
                c->ClearRenderTargetView(late_rt, new_picture);
            }
            if (clear_before_reader) c->ClearDepthStencilView(game.dsv.Get(), D3D11_CLEAR_DEPTH, 1, 0);
            c->VSSetShader(game.vs_post.Get(), nullptr, 0);
            reader(decoy.rtv.Get(), late, game.ps_tonemap.Get());
            if (game.padded_hdr) {
                reader(other_canvas.rtv.Get(), late, game.ps_tonemap.Get());
                D3D11_VIEWPORT screen_vp = vp;
                screen_vp.Height += 3;
                c->RSSetViewports(1, &screen_vp);
                reader(game.back_rtv.Get(), other_canvas.srv.Get(), letterbox.Get());
            } else if (compare_routes) {
                // This fixture deliberately flashes one region when DLAA has
                // already evaluated the early surface. The controlled path
                // uses the same final shader and resources for every frame.
                const bool flash_on = composition_fault ? (start_fault && (i & 1) != 0) : evaluator.received && evaluator.received != expected;
                const float flash[4]{flash_on ? .80f : 0.f};
                c->UpdateSubresource(trial_flash.Get(), 0, nullptr, flash, 0, 0);
                auto* control = trial_flash.Get();
                c->PSSetConstantBuffers(1, 1, &control);
                reader(game.back_rtv.Get(), late, trial_screen.Get());
            } else reader(game.back_rtv.Get(), late, game.ps_tonemap.Get());
            // A post layer can clear/reuse the depth attachment after the
            // lighting reader has consumed the scene color.
            if (reuse_depth) c->ClearDepthStencilView(game.dsv.Get(), D3D11_CLEAR_DEPTH, 1, 0);
            if (sample) {
                auto& s = *sample;
                s.correct += evaluator.received == expected;
                s.evaluated += evaluator.writes == start + 1;
                s.proven += evaluator.proof != 0;
                s.displayed_readers += evaluator.proof && evaluator.displayed_reader;
                s.fixed_history += !evaluator.reset && (!s.scene ||
                    (s.scene == evaluator.scene_stream && s.camera == evaluator.camera_stream));
                s.scene = evaluator.scene_stream;
                s.camera = evaluator.camera_stream;
                na::native11::OwnWorkScope own;
                c->CopyResource(shown.Get(), back.Get());
                D3D11_MAPPED_SUBRESOURCE display{}, input{}, output{};
                if (FAILED(c->Map(shown.Get(), 0, D3D11_MAP_READ, 0, &display)) ||
                    FAILED(c->Map(before.Get(), 0, D3D11_MAP_READ, 0, &input)) ||
                    FAILED(c->Map(after.Get(), 0, D3D11_MAP_READ, 0, &output))) return false;
                const auto red = [&](const D3D11_MAPPED_SUBRESOURCE& m, UINT x, UINT y) {
                    if (game.padded_hdr) {
                        uint16_t half;
                        std::memcpy(&half, static_cast<const BYTE*>(m.pData) + y * m.RowPitch + 8*x, sizeof(half));
                        return int(std::lround(std::clamp(DirectX::PackedVector::XMConvertHalfToFloat(half), 0.f, 1.f) * 255));
                    }
                    DirectX::PackedVector::XMFLOAT3PK texel;
                    std::memcpy(&texel, static_cast<const BYTE*>(m.pData) + y * m.RowPitch + 4*x, sizeof(texel));
                    DirectX::XMFLOAT3 rgb;
                    DirectX::XMStoreFloat3(&rgb, DirectX::PackedVector::XMLoadFloat3PK(&texel));
                    return int(std::lround(std::clamp(rgb.x, 0.f, 1.f) * 255));
                };
                std::vector<int> current;
                for (UINT y = 60; y < 116; ++y) for (UINT x = 100; x < 196; ++x) {
                    const UINT screen_y = y + (game.padded_hdr ? 2 : 0);
                    const int visible_pixel = static_cast<const BYTE*>(display.pData)[screen_y*display.RowPitch + 4*x];
                    const int in = red(input, x, y), out = red(output, x, y);
                    if (std::abs(in-out) >= 5) {
                        ++s.changed;
                        s.reached += std::abs(out-visible_pixel) <= 2;
                    }
                    if (!s.previous.empty()) {
                        s.movement += std::abs(visible_pixel-s.previous[current.size()]);
                        ++s.comparisons;
                    }
                    s.brightness += visible_pixel;
                    current.push_back(visible_pixel);
                }
                s.previous = std::move(current);
                c->Unmap(after.Get(), 0);
                c->Unmap(before.Get(), 0);
                c->Unmap(shown.Get(), 0);
            }
            return SUCCEEDED(game.swap->Present(0, 0));
        };
        Sample baseline, fixed, dim, pinned, pinned_dim;
        for (int i = 0; i < 70; ++i) if (!record(i, true, i < 42 ? nullptr : &baseline)) return 1;
        na::settings::Get().referee_enabled.store(1);
        na::native11::SetScreenCheck(true);
        if (composition_fault) {
            // The final game shader flashes independently of the amount of
            // sharpening. The real provider must try the real RCAS control,
            // then restore it while continuing to deliver NGX.
            auto& options = na::settings::Get();
            options.referee_every_ms.store(100);
            options.j2_sharpness.store(50);
            const auto end = GetTickCount64() + 90000;
            int i = 0;
            bool excluded = false, placement_rejected = false;
            while (!excluded && GetTickCount64() < end) {
                if (!record(i++, false, nullptr)) return 1;
                Sleep(6);
                for (const auto& line : na::eventlog::Drain()) {
                    if (line.find("native11_route_trial") != std::string::npos) {
                        std::printf("%s", line.c_str());
                        if (line.find("phase=baseline ") != std::string::npos) start_fault = true;
                        if (line.find("phase=rejected ") != std::string::npos &&
                            line.find("did not improve") != std::string::npos) placement_rejected = true;
                    }
                    if (line.find("referee_recovery") != std::string::npos) {
                        std::printf("%s\n", line.c_str());
                        excluded |= line.find("tool=reduce_sharpening phase=restored ruled_out=16") != std::string::npos;
                    }
                }
            }
            const auto tried = evaluator.sharpening_trial_frames;
            Sample restored;
            for (unsigned j = 0; j < 28; ++j) if (!record(i++, false, &restored)) return 1;
            std::printf("composition recovery: tried=%u restored=%d/28 changes=%zu visible=%zu exclusions=%d\n",
                tried, restored.evaluated, restored.changed, restored.reached, int(excluded));
            expect(placement_rejected, "failed lighting placement hands the same fault to the stage-specific selector");
            expect(tried && excluded && evaluator.sharpening_trial_frames == tried && options.j2_sharpness.load() == 50,
                   "actual DX11 trial rejects a compositing flash independent of sharpening and restores user settings");
            expect(restored.evaluated == 28, "real DLAA stays engaged after the unsuccessful correction");
            na::native11::Uninstall();
            return g_failed ? 1 : 0;
        }
        if (compare_routes) {
            na::settings::Get().referee_every_ms.store(100);
            const auto menu_draws = na::overlay::GetStats().frames_rendered;
            const auto until = GetTickCount64() + 18000;
            int i = 0;
            while (GetTickCount64() < until) {
                if (reuse_depth) na::overlay::SetOpen((i / 8) % 2 == 0);
                if (!record(i++, true, nullptr)) return 1;
                Sleep(7);
            }
            if (reuse_depth) {
                na::overlay::SetOpen(false);
                expect(na::overlay::GetStats().frames_rendered > menu_draws,
                       "Home was drawn during the lighting comparison");
            }
            for (int j = 0; j < 28; ++j) if (!record(i++, true, &fixed)) return 1;
            const double old_delta = baseline.movement / baseline.comparisons;
            const double new_delta = fixed.movement / fixed.comparisons;
            const auto stats = na::referee::gpu::GetStats();
            std::printf("controlled native scene: before=%.4f after=%.4f late=%d/28, visible result=%zu/%zu, GPU pairs=%llu comparable=%llu\n",
                        old_delta, new_delta, fixed.correct, fixed.reached, fixed.changed,
                        static_cast<unsigned long long>(stats.trial_pairs), static_cast<unsigned long long>(stats.trial_comparable));
            expect(fixed.correct == 28 && fixed.proven == 28 && fixed.displayed_readers == 28,
                   "the actual provider keeps the improved screen-proved route");
            expect(fixed.changed > 100 && fixed.reached * 100 >= fixed.changed * 98 && new_delta < old_delta * .25,
                   "real NGX reaches the screen and the reflected light stops flickering");
            auto& sharpness = na::settings::Get().j2_sharpness;
            const auto previous_sharpness = sharpness.load();
            sharpness.store(previous_sharpness ? 0 : 1);
            const auto changed_until = GetTickCount64() + 4000;
            while (GetTickCount64() < changed_until) {
                if (!record(i++, true, nullptr)) return 1;
                Sleep(7);
            }
            Sample after_change;
            for (int j = 0; j < 28; ++j) if (!record(i++, true, &after_change)) return 1;
            sharpness.store(previous_sharpness);
            std::printf("after settings change: ordinary delivered=%d/28 later reader=%d/28\n", after_change.evaluated, after_change.correct);
            expect(after_change.evaluated == 28 && after_change.correct == baseline.correct,
                   "settings change restores the ordinary route and preserves DLAA on every test frame");
            na::native11::Uninstall();
            return g_failed ? 1 : 0;
        }
        const auto resets = [] {
            char status[2048]{};
            na::native_d3d11::FormatStatus(status, sizeof(status));
            std::array<unsigned long long, 5> result{};
            const auto* history = std::strstr(status, "history_resets{");
            const auto* measured = std::strstr(status, "measured{reset_first_picture=");
            unsigned last_reason = 0;
            if (!history || !measured ||
                sscanf_s(history, "history_resets{camera_cut=%llu inputs_changed=%llu frame_gap=%llu", &result[0], &result[1], &result[2]) != 3 ||
                sscanf_s(measured, "measured{reset_first_picture=%llu last_no_pair=%u reset_scene_cut=%llu", &result[3], &last_reason, &result[4]) != 3)
                expect(false, "read actual evaluator history counters");
            return result;
        };
        for (int i = 0; i < 322; ++i) if (!record(i, true, nullptr)) return 1;
        const auto before_swaps = resets();
        for (int i = 322; i < 350; ++i) if (!record(i, true, &fixed)) return 1;
        expect(resets() == before_swaps, "actual DLAA and measured-motion histories survive every qualified swap");
        for (int i = 0; i < 56; ++i) if (!record(i, true, i < 28 ? nullptr : &pinned, true)) return 1;
        for (int i = 0; i < 21; ++i) if (!record(i, false, i >= 14 ? &dim : nullptr)) return 1;
        for (int i = 0; i < 21; ++i) if (!record(i, false, i >= 14 ? &pinned_dim : nullptr, true)) return 1;
        double old_delta = baseline.movement / baseline.comparisons, new_delta = fixed.movement / fixed.comparisons;
        std::printf("fixed-owner control reflection delta=%.4f\n", pinned.movement/pinned.comparisons);
        std::printf("reflection swaps: baseline late=%d/28, delta=%.4f; qualified late=%d/28, submissions=%d, proofs=%d, stable histories=%d, delta=%.4f, display matches=%zu/%zu, light on/off=%.2f/%.2f\n",
            baseline.correct, old_delta, fixed.correct, fixed.evaluated, fixed.proven, fixed.fixed_history, new_delta,
            fixed.reached, fixed.changed, fixed.brightness/(28*5376), dim.brightness/(7*5376));
        expect(baseline.correct < 28 && old_delta > 1, "physical selection reproduces visible reflection flicker");
        expect(fixed.correct == 28 && fixed.evaluated == 28 && fixed.proven == 28 && fixed.fixed_history == 28,
               "both fully lit surfaces reach one continuous DLAA history");
        expect(fixed.displayed_readers == 28, "the identical shader drawing into the undisplayed decoy never steers DLAA");
        expect(fixed.changed > 100 && fixed.reached*100 >= fixed.changed*98, "displayed reflection matches real NGX output on differentiated pixels");
        expect(new_delta < 1 && new_delta < old_delta*.25, "stationary reflection stops alternating when the physical buffers swap");
        expect(std::abs(new_delta - pinned.movement/pinned.comparisons) < .25, "swapping and pinned surfaces have the same residual flicker within a quarter of a display level");
        // Measure the response using this model/runtime on a single physical
        // owner. Different NVIDIA runtimes can render the same change with
        // different contrast, even when both deliver the correct result.
        constexpr double pixels = 5376;
        const double pinned_drop = (pinned.brightness / 28 - pinned_dim.brightness / 7) / pixels;
        const double rotating_drop = (fixed.brightness / 28 - dim.brightness / 7) / pixels;
        std::printf("intentional light change: pinned=%.3f rotating=%.3f display levels\n", pinned_drop, rotating_drop);
        expect(pinned_drop >= 8 && std::abs(rotating_drop - pinned_drop) <= .10 * pinned_drop,
               "rotating placement preserves the fixed owner's response when a real light goes out");
        if (reuse_depth) {
            if (!record(0, true, nullptr, false, false, true)) return 1;
            expect(!evaluator.proof, "the lighting reader refuses a depth clear before it consumes the scene");
        }
        if (!record(0, true, nullptr, false, true)) return 1;
        expect(!evaluator.proof, "a clear between the final lighting draw and its reader invalidates the source");
        if (g_failed)
            for (const auto& event : na::eventlog::Drain()) if (event.find("lit_role") != std::string::npos) std::printf("%s", event.c_str());
        return g_failed ? 1 : 0;
    }
#endif
    if (adaptive) {
        // Learn a numbered read on order A, then put a plausible draw at
        // that number before more scene work. B and C both share A's inputs.
        const auto render = [&](int order, bool check_pixels = false, bool extra_read = false, bool different_writer = false) {
            Pump();
            evaluator.proof = 0;
            const auto writes_before = evaluator.writes;
            Scene(game.ctx.Get(), game, 0);
            game.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            game.ctx->VSSetShader(game.vs_post.Get(), nullptr, 0);
            if (order != 0 || extra_read) {
                // Same reader as A, but in this order it receives unfinished colour.
                reader(canvas.rtv.Get(), game.color_srv.Get(), game.ps_tonemap.Get());
            }
            auto* rt = game.color_rtv.Get();
            game.ctx->OMSetRenderTargets(1, &rt, game.dsv.Get());
            game.ctx->VSSetShader(game.vs.Get(), nullptr, 0);
            game.ctx->PSSetShader(different_writer ? game.ps_scene.Get() : variant.Get(), nullptr, 0);
            SetObject(game.ctx.Get(), game, 2);
            game.ctx->Draw(4, 0);
            game.ctx->VSSetShader(game.vs_post.Get(), nullptr, 0);
            if (order == 2) reader(decoy.rtv.Get(), game.color_srv.Get(), game.ps_tonemap.Get());
            auto& target = order == 2 ? third_canvas : order == 1 ? other_canvas : canvas;
            reader(target.rtv.Get(), game.color_srv.Get(), game.ps_tonemap.Get());
            reader(game.back_rtv.Get(), target.srv.Get(), composite.Get());
            ComPtr<ID3D11Texture2D> screen;
            if (FAILED(game.swap->GetBuffer(0, IID_PPV_ARGS(&screen)))) return false;
            bool reached = false;
            if (check_pixels && evaluator.writes == writes_before + 1) {
#if defined(NA_TEST_NATIVE_DLAA)
                na::native11::OwnWorkScope own;
                game.ctx->CopyResource(shown.Get(), screen.Get());
                struct Sample {
                    ID3D11DeviceContext* ctx;
                    ID3D11Texture2D* texture;
                    D3D11_MAPPED_SUBRESOURCE map{};
                    bool valid;
                    Sample(ID3D11DeviceContext* context, ID3D11Texture2D* source)
                        : ctx(context), texture(source), valid(SUCCEEDED(ctx->Map(texture, 0, D3D11_MAP_READ, 0, &map))) {}
                    ~Sample() { if (valid) ctx->Unmap(texture, 0); }
                    const uint8_t* At(UINT x, UINT y) const {
                        return static_cast<const uint8_t*>(map.pData) + y * map.RowPitch + x * 4;
                    }
                } original{game.ctx.Get(), before.Get()}, evaluated{game.ctx.Get(), after.Get()},
                  presented{game.ctx.Get(), shown.Get()};
                unsigned different = 0, matches_output = 0;
                if (original.valid && evaluated.valid && presented.valid) {
                    const auto pixel = [](const uint8_t* bytes) {
                        DirectX::PackedVector::XMFLOAT3PK packed;
                        std::memcpy(&packed, bytes, sizeof(packed));
                        DirectX::XMFLOAT3 rgb;
                        DirectX::XMStoreFloat3(&rgb, DirectX::PackedVector::XMLoadFloat3PK(&packed));
                        const auto channel = [](float linear) {
                            return int(std::lround(std::lround(std::clamp(linear, 0.f, 1.f) * 255.f) * .75f));
                        };
                        return std::array<int, 3>{channel(rgb.x), channel(rgb.y), channel(rgb.z)};
                    };
                    for (UINT y = 0; y < game.h; y += 2) {
                        for (UINT x = 0; x < game.w; x += 2) {
                            const auto input = pixel(original.At(x, y));
                            const auto output = pixel(evaluated.At(x, y));
                            const auto* display = presented.At(x, y);
                            for (int rgb = 0; rgb < 3; ++rgb) {
                                if (std::abs(output[rgb] - input[rgb]) < 5) continue;
                                ++different;
                                matches_output += std::abs(output[rgb] - int(display[rgb])) <= 2;
                            }
                        }
                    }
                }
                reached = different >= 16 && matches_output * 100 >= different * 98;
                if (!reached)
                    std::printf("NGX screen provenance: order=%d, differentiated samples=%u, output matches=%u\n",
                                order, different, matches_output);
#else
                const auto rgba = read(screen.Get());
                reached = std::abs(int(rgba[0]) - 143) <= 2 && std::abs(int(rgba[1]) - 48) <= 2 &&
                          std::abs(int(rgba[2]) - 24) <= 2;
                if (order == 2)
                    reached = reached && std::abs(int(read(decoy.picture.Get())[0]) - 191) > 5;
#endif
            }
            if (FAILED(game.swap->Present(0, 0))) expect(false, "present the adaptive DX11 frame");
            return reached;
        };

        for (int i = 0; i < 12; ++i) render(0, false, missed_order);
#if defined(NA_TEST_NATIVE_DLAA)
        na::settings::Get().referee_every_ms.store(100);
        na::settings::Get().referee_enabled.store(1);
#endif
        na::native11::SetScreenCheck(true);
        render(0, false, missed_order); // advance the production Present path with the referee enabled
        const auto before_change = na::native11::Stats();
        render(missed_order ? 0 : 1);
        const auto baseline = na::native11::Stats();
        if (missed_order)
            expect(baseline.fired == before_change.fired && baseline.contradictions == 0,
                   "the changed order has fewer reads than the old route expects and misses its insertion");
        else expect(baseline.contradictions > 0, "changing the order exposes the old numbered-read contradiction");
        multiple_outputs = true; // the ordinary reader works, but the screen probe cannot qualify MRT
        render(0); // let the original ordinal settle on the new order
        const auto writes_before_search = evaluator.writes;
        int visible_while_searching = 0;
        for (int i = 0; i < 24; ++i) visible_while_searching += render(0, true);
        expect(visible_while_searching == 24 && evaluator.writes == writes_before_search + 24 && na::native11::Stats().screen_confirmed == 0,
               "searching without a replacement preserves every ordinary submission");
        multiple_outputs = false;
        for (int i = 0; i < 64 && na::native11::Stats().screen_confirmed < 2; ++i) render(i & 1);
        const auto learned = na::native11::Stats();
        expect(learned.screen_confirmed == 2, "both recurring readers earn independent screen proofs");
        const auto late_before_alternating = learned.contradictions;
        int alternating = 0;
        bool consistent = true;
        uint64_t proof[2]{};
        for (int i = 0; i < 32; ++i) {
            alternating += render(i & 1, true);
            if (i < 2) proof[i] = evaluator.proof;
            else consistent = consistent && proof[i & 1] == evaluator.proof;
        }
        expect(alternating == 32 && consistent && proof[0] && proof[1] && proof[0] != proof[1],
               "all 32 alternating frames carry the evaluated scene through to the displayed pixels");

        expect(na::native11::Stats().contradictions == late_before_alternating,
               "confirmed placements produce no late-scene contradictions across alternating orders");
        expect(render(1, true, false, true) && evaluator.proof == proof[1],
               "the same unambiguous final reader retains proof when the last object's shader changes");
        for (int i = 0; i < 48 && na::native11::Stats().screen_confirmed < 3; ++i) render(2);
        const auto discovered = na::native11::Stats();
        expect(discovered.screen_confirmed == 3 && discovered.screen_refuted > learned.screen_refuted,
               "an unseen third order rejects its finished-colour decoy and proves the visible reader");
        expect(render(2, true), "the new order presents evaluated pixels");
        expect(render(0, true) && evaluator.proof == proof[0] && render(1, true) && evaluator.proof == proof[1],
               "switching back to previous orders uses known proofs on the first returning frames");

#if defined(NA_TEST_NATIVE_DLAA)
        for (int i = 0; i < 48 && na::referee::gpu::GetStats().screens < 2; ++i) render(i & 1);
        const auto witness = na::referee::gpu::GetStats();
        expect(witness.screens >= 2 && witness.pairs > 0,
               "the real native Present hook captures diagnostic screen and motion samples");
        std::printf("native Present witness: screens=%llu pairs=%llu failed=%llu\n",
                    static_cast<unsigned long long>(witness.screens), static_cast<unsigned long long>(witness.pairs),
                    static_cast<unsigned long long>(witness.failed));
#endif
        multiple_outputs = true;
        render(0);
        render(0); // the first absence releases the ordinary reader
        const auto writes_after_loss = evaluator.writes;
        int visible_after_loss = 0;
        for (int i = 0; i < 24; ++i) visible_after_loss += render(0, true);
        expect(visible_after_loss == 24 && evaluator.writes == writes_after_loss + 24,
               "an absent proven order cannot keep a valid ordinary reader offline");
        multiple_outputs = false;
        for (int i = 0; i < 6; ++i) render(1);
        na::native11::RejectScreenProof(proof[0]);
        expect(render(1, true) && evaluator.proof == proof[1] && !render(0, true) && !evaluator.proof,
               "a lost screen proof retires only its own draw order");
#if !defined(NA_TEST_NATIVE_DLAA)
        bool recovered = false;
        for (int i = 0; i < 176 && !recovered; ++i) {
            const bool reached = render(0, true);
            if (i < 112) expect(!evaluator.proof, "rejected reader stays off during cooldown");
            recovered = reached && evaluator.proof && evaluator.proof != proof[0];
        }
        expect(recovered, "a returning reader can earn a fresh screen proof after cooldown");
#endif
        std::printf("adaptive DX11 screen: %d/32 alternating, %llu confirmed, %llu rejected, %llu paired\n",
                    alternating, (unsigned long long)discovered.screen_confirmed,
                    (unsigned long long)discovered.screen_refuted, (unsigned long long)discovered.screen_pairs);
        na::native11::Uninstall();
        return g_failed ? 1 : 0;
    }

    struct Presented {
        bool processed = false, clean_decoy = false;
    };
    auto frame = [&](bool other, bool bypass = false, bool no_pose = false) {
        Pump();
        Scene(game.ctx.Get(), game, 0, no_pose);
        game.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        game.ctx->VSSetShader(game.vs_post.Get(), nullptr, 0);
        reader(decoy.rtv.Get(), game.color_srv.Get(), game.ps_tonemap.Get());
        const auto decoy_pixel = read(decoy.picture.Get());
        auto& target = other ? other_canvas : canvas;
        reader(target.rtv.Get(), game.color_srv.Get(), game.ps_tonemap.Get());
        if (bypass) {
            const float game_picture[]{.25f, .5f, .75f, 1.f};
            game.ctx->ClearRenderTargetView(other_canvas.rtv.Get(), game_picture);
        }
        reader(game.back_rtv.Get(), bypass ? other_canvas.srv.Get() : target.srv.Get(), composite.Get());
        ComPtr<ID3D11Texture2D> screen;
        if (FAILED(game.swap->GetBuffer(0, IID_PPV_ARGS(&screen)))) {
            expect(false, "read the fixture backbuffer");
            return Presented{};
        }
        const auto shown = read(screen.Get());
        const bool processed = std::abs(int(shown[0]) - 143) <= 2 &&
                               std::abs(int(shown[1]) - 48) <= 2 &&
                               std::abs(int(shown[2]) - 24) <= 2;
        if (FAILED(game.swap->Present(0, 0))) expect(false, "present the fixture frame");
        return Presented{processed, std::abs(int(decoy_pixel[0]) - 191) > 5};
    };

    for (int i = 0; i < 16; ++i) frame(false);
    na::native11::SetScreenCheck(true);
    int ordinary_shown = 0;
    for (int i = 0; i < 48; ++i) ordinary_shown += frame((i & 1) != 0).processed;
    expect(ordinary_shown == 48 && na::native11::Stats().screen_confirmed == 0,
           "the ordinary route reaches the screen on every frame with Referee and alternating readers");

    evaluator.ordinary = false;
    int alternating_shown = 0;
    for (int i = 0; i < 48; ++i) alternating_shown += frame((i & 1) != 0).processed;
    const auto alternating = na::native11::Stats();
    expect(alternating_shown == 0 && alternating.screen_confirmed == 0 && alternating.screen_refuted == 1,
           "an alternating reader cannot take over during fallback discovery");

    na::native11::SetScreenCheck(false);
    frame(false);
    na::native11::SetScreenCheck(true);
    int bypass_shown = 0;
    for (int i = 0; i < 48; ++i) bypass_shown += frame(false, (i & 1) != 0).processed;
    const auto bypassed = na::native11::Stats();
    expect(bypass_shown == 0 && bypassed.screen_confirmed == 0 &&
           bypassed.screen_refuted == alternating.screen_refuted + 1,
           "a stable reader whose output is discarded every other frame cannot take over");

    na::native11::SetScreenCheck(false);
    frame(false);
    na::native11::SetScreenCheck(true);
    uint64_t deliveries = 0;
    for (int i = 0; i < 72; ++i) {
        const auto result = frame(false);
        const auto stats = na::native11::Stats();
        if (stats.screen_delivered > deliveries) {
            expect(stats.screen_delivered == deliveries + 1, "verified reader delivers once");
            expect(result.processed, "presented pixels contain the evaluator output after the game's composite");
            expect(result.clean_decoy, "the decoy stays unprocessed");
            ++deliveries;
        } else {
            expect(!deliveries, "the verified reader stays active when the scene is stable");
        }
    }
    const auto confirmed = na::native11::Stats();
    expect(confirmed.screen_confirmed == 1 && confirmed.screen_refuted == bypassed.screen_refuted + 1 &&
           confirmed.screen_pairs == bypassed.screen_pairs + 16,
           "screen proof rejects the decoy and confirms the visible reader");
    expect(deliveries >= 20, "the verified reader can deliver continuously when the ordinary route cannot");

    evaluator.ordinary = true;
    // The learned reader now disappears on every other frame. After two spaced
    // misses the ordinary route must take over and keep all later frames.
    for (int i = 0; i < 4; ++i) frame((i & 1) != 0);
    const auto fallback = na::native11::Stats();
    expect(fallback.screen_delivered == deliveries + 2, "verified reader is retired on its second spaced miss");
    const auto writes_before = evaluator.writes;
    int restored_shown = 0;
    for (int i = 0; i < 48; ++i) restored_shown += frame((i & 1) != 0).processed;
    const auto final = na::native11::Stats();
    expect(restored_shown == 48 && evaluator.writes == writes_before + 48 &&
           final.screen_delivered == fallback.screen_delivered && final.screen_confirmed == confirmed.screen_confirmed,
           "fallback preserves every presented frame and does not take over again");
    na::native11::SetScreenCheck(false);
    frame(false);
    na::native11::SetScreenCheck(true);
    evaluator.ordinary = false;
    for (int i = 0; i < 40; ++i) frame(false);
    const auto proof = evaluator.proof;
    expect(proof != 0, "the accepted reader tags its actual evaluations");
    const auto before_reject = na::native11::Stats();
    na::native11::RejectScreenProof(proof + 1);
    expect(frame(false).processed && na::native11::Stats().screen_delivered == before_reject.screen_delivered + 1,
           "another reader's result cannot retire the current proof");
    na::native11::RejectScreenProof(proof);
    evaluator.ordinary = true;
    expect(frame(false).processed && evaluator.proof == 0,
           "a visible-delivery rejection returns placement to the ordinary route");

    na::native11::SetScreenCheck(false);
    const auto before_pause = na::native11::Stats();
    na::native11::SetEvaluationPaused(true);
    for (int i = 0; i < 16; ++i) frame(false);
    const auto paused = na::native11::Stats();
    expect(paused.camera_found == before_pause.camera_found && paused.camera_missing == before_pause.camera_missing,
           "the camera search stays completely idle while paused");
    na::native11::SetEvaluationPaused(false);
    int measured_shown = 0;
    for (int i = 0; i < 32; ++i) measured_shown += frame(false, false, true).processed;
    const auto measured = na::native11::Stats();
    const auto searches = measured.camera_missing - paused.camera_missing;
    expect(searches == 8 && measured_shown == 32, "failed optional searches back off while the consumer still delivers");
    for (int i = 0; i < 124; ++i) frame(false);
    expect(na::native11::Stats().camera_found > measured.camera_found,
           "optional search recovers automatically when the camera returns");
    std::printf("native DX11 camera budget: paused searches=0, optional searches=%llu/32, displayed=%d/32\n",
                (unsigned long long)searches, measured_shown);
    std::printf("native DX11 screen check: ordinary=%d/48 alternating=%d/48 bypass=%d/48 cold_start=%llu restored=%d/48\n",
                ordinary_shown, alternating_shown, bypass_shown, (unsigned long long)deliveries, restored_shown);
    na::native11::Uninstall();
    return g_failed ? 1 : 0;
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char** argv)
{
    const bool missed_order = argc > 1 && std::strcmp(argv[1], "--adaptive-screen-miss") == 0;
    const bool adaptive = missed_order || (argc > 1 && (std::strcmp(argv[1], "--adaptive-screen") == 0 ||
                                                       std::strcmp(argv[1], "--adaptive-screen-warp") == 0));
#if defined(NA_TEST_NATIVE_DLAA)
    const bool automatic = argc > 1 && std::strcmp(argv[1], "--automatic") == 0;
    const bool padded_hdr = argc > 1 && std::strcmp(argv[1], "--lit-role-padded") == 0;
    const bool trial_reused_depth = argc > 1 && std::strcmp(argv[1], "--lit-role-trial-reused-depth") == 0;
    const bool reuse_depth = trial_reused_depth || (argc > 1 && std::strcmp(argv[1], "--lit-role-reused-depth") == 0);
    const bool composition_fault = argc > 1 && std::strcmp(argv[1], "--composition-recovery") == 0;
    const bool compare_routes = composition_fault || trial_reused_depth || (argc > 1 && std::strcmp(argv[1], "--lit-role-trial") == 0);
    const bool rotating_lights = compare_routes || padded_hdr || reuse_depth || (argc > 1 && std::strcmp(argv[1], "--lit-role") == 0);
    const bool camera_off_start = argc > 1 && std::strcmp(argv[1], "--camera-off-start") == 0;
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const auto dir = std::filesystem::path(exe).parent_path();
    // The host persists settings across runs. Windows reuses PIDs, so a fresh
    // fixture must also distinguish the run that previously held the same PID.
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const auto ticks = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    const auto scope = L"Native11Activation-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(ticks);
    const auto ini_name = scope + L".ini";
    const auto ini = dir / ini_name;
    const auto status_path = dir / (scope + L".log");
    wchar_t process_log[MAX_PATH]{};
    if (!na::host::ProcessLogPath(process_log, MAX_PATH, status_path.c_str())) return 1;
    const auto set = [&](const wchar_t* section, const wchar_t* key, const wchar_t* value) {
        return WritePrivateProfileStringW(section, key, value, ini.c_str()) != FALSE;
    };
    if (!set(L"host", L"enabled", L"1") || !set(L"host", L"status_log", status_path.c_str()) ||
        !set(L"dx11on12", L"enabled", automatic ? L"1" : L"0") ||
        !set(L"native11", L"enabled", automatic ? L"0" : L"1") ||
        !set(L"host", L"graphics_api", automatic ? L"auto" : L"") ||
        !set(L"overlay", L"enabled", L"1") || !set(L"adapter", L"enabled", L"0") ||
        !set(L"observe", L"producers", automatic ? L"1" : L"0") ||
        !set(L"observe", L"early_install", L"0") ||
        !set(L"present", L"proxy", L"0") || !set(L"settings", L"scope", scope.c_str()) ||
        !set(L"fsr", L"enabled", L"0") || !set(L"xess", L"enabled", L"0") ||
        !set(L"j2", L"enabled", L"1") ||
        !set(L"j2", L"camera", (camera_off_start || adaptive || rotating_lights) ? L"0" : L"2") ||

        !na::host::StartFromIni(GetModuleHandleW(nullptr), ini_name.c_str(), "NATIVE DX11 TEST")) return 1;
    na::host::WaitForObserveInstall();
    const ULONGLONG init_deadline = GetTickCount64() + 20000;
    while (!na::host::GetStatus().init_complete && GetTickCount64() < init_deadline) Sleep(5);
    expect(na::host::GetStatus().init_complete && na::on12::NativeRoute() != automatic && !na::on12::Enabled(),
           "production host starts with the bridge disabled");
    if (g_failed) return 1;
#endif
    Game g;
#if defined(NA_TEST_NATIVE_DLAA)
    g.padded_hdr = padded_hdr;
#endif
    bool warp = false;
    const bool screen_warp = argc > 1 && (std::strcmp(argv[1], "--screen-check-warp") == 0 ||
                                         std::strcmp(argv[1], "--adaptive-screen-warp") == 0);
    const bool depth_warp = argc > 1 && std::strcmp(argv[1], "--depth-reads-warp") == 0;
    const bool depth_debug = argc > 1 && std::strcmp(argv[1], "--depth-reads-debug") == 0;
    const bool depth_reads = depth_warp || depth_debug || (argc > 1 && std::strcmp(argv[1], "--depth-reads") == 0);
    g.shader_depth = depth_reads;
    if (screen_warp || depth_warp || depth_debug || !MakeGame(g, false)) {
        g = Game{};
        g.shader_depth = depth_reads;
        if (!MakeGame(g, true, depth_debug)) {
            std::printf("SKIP: no %s DX11 device\n", depth_debug ? "debug-layer" : "WARP");
            return 77;
        }
        warp = true;
    }
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapter_desc{};
    if (SUCCEEDED(g.device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter))) adapter->GetDesc(&adapter_desc);
    std::printf("device: %ls (%s)\n", adapter_desc.Description, warp ? "WARP" : "hardware");
#if !defined(NA_TEST_NATIVE_DLAA)
    if (adaptive || screen_warp || (argc > 1 && std::strcmp(argv[1], "--screen-check") == 0)) return ScreenCheckFixture(g, adaptive, missed_order);
#endif
#if defined(NA_TEST_NATIVE_DLAA)
    if (warp) return 77;
    ComPtr<ID3D11On12Device> translated;
    expect(FAILED(g.device.As(&translated)), "game device is native DX11");
    if (automatic) {
        expect(na::on12::SwapchainApi(g.swap.Get()) == na::on12::GraphicsApi::Dx11,
               "automatic selection identifies the swap chain's native DX11 device");
        expect(SUCCEEDED(g.swap->Present(0, DXGI_PRESENT_TEST)) && !na::on12::NativeRoute(),
               "a test present leaves the route unselected");
        expect(SUCCEEDED(g.swap->Present(0, 0)) && na::on12::NativeRoute(),
               "the real present selects native DX11");
        expect(!na::on12::AcceptPresentation(na::on12::GraphicsApi::Dx12),
               "a helper chain cannot replace the native device with DX12");
    }
    static char activation[8192]{}; // the whole status line; it grows with every counter added
    na::on12::FormatStatus(activation, sizeof activation);
    if (!std::strstr(activation, "native11_route=active installs=1 install_failures=0") || !std::strstr(activation, "wrapped=0"))
        std::printf("activation status: %s\n", activation);
    expect(std::strstr(activation, "native11_route=active installs=1 install_failures=0") != nullptr &&
           std::strstr(activation, "dx11on12 enabled=0") != nullptr && std::strstr(activation, "wrapped=0") != nullptr,
           "production host installed the native provider exactly once");
    if (g_failed) return 1;
    if (adaptive || rotating_lights) {
        na::j2::SetJitter(true);
        const int result = ScreenCheckFixture(g, adaptive, missed_order, rotating_lights, compare_routes, composition_fault, reuse_depth);
        DeleteFileW(ini.c_str());
        std::fflush(stdout);
        ExitProcess(result);
    }
    na::j2::SetJitter(true);
#endif
    g_seen.game = &g;
#if !defined(NA_TEST_NATIVE_DLAA)
    // A game that makes a device, then a second one and draws on that: the second device's context
    // is first met while the first device is still the one watched. Every phase below would see
    // nothing if that first answer stood.
    {
        ComPtr<ID3D11Device> first;
        ComPtr<ID3D11DeviceContext> first_ctx;
        const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        const HRESULT hr = D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1,
                                             D3D11_SDK_VERSION, &first, nullptr, &first_ctx);
        expect(SUCCEEDED(hr) && na::native11::Install(first.Get(), &Consume, &g_seen), "the provider installs on a first device");
        if (g_failed) return 1;
        ID3D11RenderTargetView* none = nullptr;
        g.ctx->OMSetRenderTargets(1, &none, nullptr); // met as another device's context
        expect(!na::native11::IsGameWork(g.ctx.Get()), "a context of a device that is not the watched one is left alone");
    }
#endif
    const bool installed = na::native11::Install(g.device.Get(), &Consume, &g_seen);
#if !defined(NA_TEST_NATIVE_DLAA)
    expect(na::native11::IsGameWork(g.ctx.Get()), "a context met before its device became the game's is asked again and now counts");
    {
        char moved[4096];
        na::native11::FormatStatus(moved, sizeof moved);
        expect(std::strstr(moved, "game_device{generation=3 contexts_rechecked=1 became_game=1") != nullptr,
               "the status line names the move to a second device");
    }
#endif
    expect(installed, "the provider's hooks install on the game's device");
    if (!installed) return 1;
#if !defined(NA_TEST_NATIVE_DLAA)
    if (depth_reads) return DepthReadFixture(g, depth_debug);
#endif
#if defined(NA_TEST_NATIVE_DLAA)
    if (camera_off_start) {
        // Cold start: no earlier camera-backed frame, no hand-written next jitter, no warm flow pair.
        // Run the production host and real NGX (also via NA_DLAA_COMPANION=1). Camera bytes exist in
        // this synthetic game's rendering, but discovery must never read or publish them.
        g_no_camera_phase = 1;
        na::native11::SetJitterRoute(na::native11::JitterRoute::Viewport);
        unsigned raster_samples = 0;
        for (int frame = 0; frame < 48; ++frame) {
            Pump();
            const auto before = g_seen.calls;
            Scene(g.ctx.Get(), g, frame);
            Post(g.ctx.Get(), g, true);
            g.swap->Present(0, 0);
            if (g_seen.calls > before && g_seen.last.raster.applied &&
                (g_seen.last.raster.jitter_px[0] != 0 || g_seen.last.raster.jitter_px[1] != 0)) ++raster_samples;
        }
        const auto s = na::native11::Stats();
        char wide[8192]{};
        na::native_d3d11::FormatStatus(wide, sizeof wide);
        std::printf("cold camera-off: calls=%llu delivered=%llu raster_samples=%u | %s\n",
                    static_cast<unsigned long long>(g_seen.calls), static_cast<unsigned long long>(evaluations), raster_samples, wide);
        expect(g_seen.calls >= 30 && evaluations >= g_seen.calls - 2,
               "camera-free cold start delivers real DLAA for every acquired frame after at most two seed frames");
        expect(g_seen.without_camera == g_seen.calls && g_seen.camera_leak == 0 &&
               s.camera_found == 0 && s.camera_missing == 0 && s.camera_held == 0,
               "no discovered camera or camera-derived fact was used, even during bootstrap");
        expect(g_seen.wrong_inputs == 0 && g_seen.no_scene == 0 && g_seen.scene_changed == 1 && g_seen.twice == 0,
               "correct scene/depth identity survives bootstrap without repeated resets or duplicate deliveries");
        expect(raster_samples >= 20, "production delivery schedules actual viewport samples without a test-provided jitter");
        expect(std::strstr(wide, "camera_setting=0") && std::strstr(wide, "measured=0}") == nullptr,
               "the production consumer names camera-independent measured motion");
        DeleteFileW(ini.c_str());
        std::printf(g_failed ? "FAIL: %d cold-start check(s) failed\n" : "PASS: camera-free cold start with real NGX\n", g_failed);
        std::fflush(stdout);
        ExitProcess(g_failed ? 1u : 0u);
    }
#endif
    // The production host selects the viewport route. The camera-matrix checks below exercise the
    // matrix route explicitly; the viewport route has its own phases.
    na::native11::SetJitterRoute(na::native11::JitterRoute::Matrix);

    char status[512];
    // ---- phase 1: immediate context ----
    int frame = 0;
    for (; frame < 200 && g_seen.calls < 30; ++frame) {
        Pump();
        na::native11::SetNextJitter(0.25f, -0.25f);
        Scene(g.ctx.Get(), g, frame);
        Post(g.ctx.Get(), g, true);
        g.swap->Present(0, 0);
    }
    na::native11::FormatStatus(status, sizeof status);
    std::printf("phase 1: frames=%d calls=%llu | %s\n", frame, static_cast<unsigned long long>(g_seen.calls), status);
    expect(g_seen.calls >= 30, "the consumer is called every frame once the scene is learned (30 frames)");
    expect(g_seen.twice == 0, "at most one call per frame");
    expect(g_seen.wrong_inputs == 0, "correct HDR scene colour, D24S8 depth, full extent and lifetime ids");
    expect(g_seen.after_tonemap == 0 && g_seen.before_read == g_seen.calls, "every call is before the tonemap read (BeforeRead)");
    expect(g_seen.no_scene == 0, "the scene's 40 draws are done when the consumer is called");
    expect(g_seen.no_camera == 0, "the camera buffer (VS slot 0) and its 512 CPU bytes come with each call");
    expect(g_seen.streams == 1 && g_seen.changed == 1, "one input stream for the whole phase (history reset once)");
    expect(g_seen.jitter_reported >= 10 && na::native11::Stats().jitter_written >= 10, "jitter from SetNextJitter is written and reported");
    {
        float px[4] = {};
        const bool read = ReadCopy(g, px);
        const bool scene_pixel = read && px[3] > 0.99f && (std::fabs(px[0] - 0.25f) < 0.01f || std::fabs(px[0] - 0.1f) < 0.01f);
        std::printf("consumer copy pixel: %.3f %.3f %.3f %.3f\n", px[0], px[1], px[2], px[3]);
        expect(scene_pixel, "the consumer's copy on the game's context holds the drawn scene");
#if defined(NA_TEST_NATIVE_DLAA)
        float result[4]{};
        const bool read_result = ReadCopy(g, result, g.color.Get());
        expect(read_result && std::isfinite(result[0]) && std::fabs(result[0] - px[0]) < 0.06f &&
               std::fabs(result[1] - px[1]) < 0.06f && std::fabs(result[2] - px[2]) < 0.06f,
               "native scene contains the expected RGB after the effects writeback");
#endif
    }

    // The GPU sees the jittered camera: rows 0/1 moved by a sub-pixel amount, rows 2/3 unchanged.
    {
        D3D11_BUFFER_DESC bd{};
        g.cam_cb->GetDesc(&bd);
        bd.Usage = D3D11_USAGE_STAGING;
        bd.BindFlags = 0;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> st;
        g.device->CreateBuffer(&bd, nullptr, &st);
        na::native11::SetNextJitter(0.25f, -0.25f);
        Scene(g.ctx.Get(), g, frame);
        g.ctx->CopyResource(st.Get(), g.cam_cb.Get());
        Post(g.ctx.Get(), g, true);
        g.swap->Present(0, 0);
        D3D11_MAPPED_SUBRESOURCE m{};
        bool rows23 = false, sub_pixel = false;
        if (st && SUCCEEDED(g.ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) {
            std::vector<uint8_t> b(static_cast<uint8_t*>(m.pData), static_cast<uint8_t*>(m.pData) + 512);
            g.ctx->Unmap(st.Get(), 0);
            const auto gpu = na::camera::LoadMatrix(b, 64, na::camera::Layout::RowMajor);
            const auto& want = g.last_written;
            rows23 = true;
            for (int i = 8; i < 16; ++i) rows23 = rows23 && std::fabs(gpu[i] - want[i]) < 1e-4;
            const double dx = (gpu[2] - want[2]) / want[14], dy = (gpu[6] - want[6]) / want[14];
            sub_pixel = std::fabs(dx) <= 2.0 / g.w + 1e-6 && std::fabs(dy) <= 2.0 / g.h + 1e-6 && (std::fabs(dx) > 1e-5 || std::fabs(dy) > 1e-5);
            std::printf("jitter seen by the GPU: %.3f px, %.3f px\n", dx * g.w / 2.0, -dy * g.h / 2.0);
        }
        expect(rows23 && sub_pixel, "the GPU's camera carries a sub-pixel jitter in rows 0/1 only");
        ++frame;
    }

    // ---- no camera (2026-11-16): a frame is handed over without one, and without looking for one ----
    // The scene clears its depth to 1 and draws with the default depth state (LESS, writing), so the
    // depth direction is proven from the depth's own writes. The jitter route is still the matrix
    // one here, which needs a camera: with none looked for, nothing may be written.
    {
        const auto set_use = [&](na::native11::CameraUse use) {
#if defined(NA_TEST_NATIVE_DLAA)
            na::settings::Get().j2_camera.store(use == na::native11::CameraUse::Off ? 0 : use == na::native11::CameraUse::Optional ? 1 : 2);
            na::native_d3d11::SyncCameraUse();
#else
            na::native11::SetCameraUse(use);
#endif
        };
        const auto run = [&](int frames) {
            for (int i = 0; i < frames; ++i, ++frame) {
                Pump();
                na::native11::SetNextJitter(0.25f, -0.25f);
                Scene(g.ctx.Get(), g, frame);
                Post(g.ctx.Get(), g, true);
                g.swap->Present(0, 0);
            }
        };
        static char wide[8192];
        const auto stats0 = na::native11::Stats();
        const auto calls0 = g_seen.calls, streams0 = g_seen.streams, changed0 = g_seen.changed, scene0 = g_seen.scene_changed;
        const auto wrong0 = g_seen.wrong_inputs, no_scene0 = g_seen.no_scene, reversed0 = g_seen.reversed_depth;
        expect(scene0 == 1, "the scene's own identity changed once so far (the first frame), like the input stream");
#if defined(NA_TEST_NATIVE_DLAA)
        const auto evaluations0 = evaluations;
#endif
        g_no_camera_phase = 1;
        set_use(na::native11::CameraUse::Off);
        run(12);
        const auto stats1 = na::native11::Stats();
        na::native11::FormatStatus(wide, sizeof wide);
        std::printf("no camera looked for: calls=%llu without_camera=%llu | %s\n", static_cast<unsigned long long>(g_seen.calls - calls0),
                    static_cast<unsigned long long>(g_seen.without_camera), wide);
        expect(g_seen.calls >= calls0 + 11, "with no camera looked for, every frame is still handed over");
        expect(g_seen.without_camera == g_seen.calls - calls0 && g_seen.camera_leak == 0,
               "none of those frames carries a camera or any camera fact");
        expect(g_seen.wrong_inputs == wrong0 && g_seen.no_scene == no_scene0, "the scene colour, depth and draw count are the same facts as with a camera");
        expect(g_seen.reversed_depth == reversed0 && !g_seen.last.reversed_depth,
               "the depth direction comes from the depth's own writes (cleared to 1, drawn with LESS)");
        expect(stats1.camera_off >= stats0.camera_off + 11 && stats1.camera_found == stats0.camera_found &&
               stats1.camera_missing == stats0.camera_missing && stats1.camera_held == stats0.camera_held,
               "no camera search ran on those frames");
        expect(stats1.jitter_written == stats0.jitter_written && !g_seen.last.raster.applied && !g_seen.last.raster.qualified,
               "nothing is written into a camera that is not looked for, and the frame says its picture is unjittered");
        expect(g_seen.streams == streams0 && g_seen.changed == changed0 && g_seen.scene_changed == scene0,
               "a camera that goes away is not a change of scene: no history reset");
        expect(stats1.depth_unproven == stats0.depth_unproven, "no frame waited for the depth direction");
        expect(std::strstr(wide, "camera_use{mode=off") != nullptr && std::strstr(wide, "depth_direction{near_is_less") != nullptr,
               "the status line names the camera mode and the proven depth direction");
#if defined(NA_TEST_NATIVE_DLAA)
        // End to end on the GPU: DLAA keeps running with motion measured from the picture. (Written
        // 2026-11-16 without an NVIDIA GPU to run it on: first run of this check is still owed.)
        na::native_d3d11::FormatStatus(wide, sizeof wide);
        std::printf("native DLAA with no camera: %s\n", wide);
        expect(evaluations >= evaluations0 + 10, "DLAA keeps delivering with no camera (motion measured from the picture)");
        expect(std::strstr(wide, "camera_setting=0") != nullptr && std::strstr(wide, "measured=0}") == nullptr,
               "the DLAA status counts frames whose motion was measured");
#endif
        // Optional: the camera is looked for again and handed over when found, as when it is required.
        g_no_camera_phase = 2; // the two frames around the switch may go either way
        set_use(na::native11::CameraUse::Optional);
        run(2);
        g_no_camera_phase = 0;
        const auto calls2 = g_seen.calls, no_camera2 = g_seen.no_camera;
        const auto stats2 = na::native11::Stats();
        run(8);
        const auto stats3 = na::native11::Stats();
        na::native11::FormatStatus(wide, sizeof wide);
        std::printf("camera optional: calls=%llu | %s\n", static_cast<unsigned long long>(g_seen.calls - calls2), wide);
        expect(g_seen.calls >= calls2 + 7 && g_seen.no_camera == no_camera2 && g_seen.last.camera_known,
               "optional: a camera that is found is handed over exactly as when it is required");
        expect(stats3.camera_found >= stats2.camera_found + 7 && stats3.camera_absent == stats2.camera_absent,
               "optional: the camera was found on those frames");
        expect(g_seen.streams == streams0 && g_seen.changed == changed0 && g_seen.scene_changed == scene0,
               "the same camera buffer met again is not a change of inputs either");
        expect(std::strstr(wide, "camera_use{mode=optional") != nullptr, "the status line names the optional mode");
        set_use(na::native11::CameraUse::Required); // the phases below test the camera itself
        run(1);
    }

#if !defined(NA_TEST_NATIVE_DLAA)
    // ---- viewport jitter route: the sample moves the game's viewports on scene draws only ----
    {
        na::native11::SetJitterRoute(na::native11::JitterRoute::Viewport);
        const auto reported = g_seen.jitter_reported;
        const auto written = na::native11::Stats().jitter_written;
        const auto viewport_now = [&] {
            UINT n = 1;
            D3D11_VIEWPORT vp{};
            g.ctx->RSGetViewports(&n, &vp);
            return vp;
        };
        bool moved = true, restored = true, depth_only_same = true;
        for (int i = 0; i < 6; ++i) {
            na::native11::SetNextJitter(0.25f, -0.25f); // covers the frame after the next Present
            Scene(g.ctx.Get(), g, frame++);
            const D3D11_VIEWPORT scene_vp = viewport_now(); // the last scene draw's viewport
            // A depth-only pass into the scene depth (a prepass has no colour target) must sit at
            // the same sub-pixel position as the colour passes tested against it.
            g.ctx->OMSetRenderTargets(0, nullptr, g.dsv.Get());
            g.ctx->Draw(3, 0);
            const D3D11_VIEWPORT depth_only_vp = viewport_now();
            depth_only_same = depth_only_same && (i == 0 || std::memcmp(&depth_only_vp, &scene_vp, sizeof scene_vp) == 0);
            Post(g.ctx.Get(), g, true);
            const D3D11_VIEWPORT post_vp = viewport_now();
            g.swap->Present(0, 0);
            if (i == 0) continue; // armed by the first Present
            // Full-target viewport: widened half a pixel each side, then moved by the sample.
            moved = moved && std::fabs(scene_vp.TopLeftX - (-0.5f + 0.25f)) < 1e-4f && std::fabs(scene_vp.TopLeftY - (-0.5f - 0.25f)) < 1e-4f &&
                    std::fabs(scene_vp.Width - float(g.w + 1)) < 1e-4f && std::fabs(scene_vp.Height - float(g.h + 1)) < 1e-4f;
            restored = restored && post_vp.TopLeftX == 0.f && post_vp.TopLeftY == 0.f && post_vp.Width == float(g.w) && post_vp.Height == float(g.h);
            if (i == 1) std::printf("viewport jitter: scene viewport %.3f %.3f %.3f x %.3f, post viewport %.3f %.3f\n", scene_vp.TopLeftX,
                                    scene_vp.TopLeftY, scene_vp.Width, scene_vp.Height, post_vp.TopLeftX, post_vp.TopLeftY);
        }
        na::native11::FormatStatus(status, sizeof status);
        std::printf("viewport jitter: %s\n", status);
        expect(moved, "scene draws are rasterised through viewports moved by the sub-pixel sample");
        expect(depth_only_same, "a depth-only draw into the scene depth carries the same offset as the colour draws");
        expect(restored, "the post pass gets the game's own viewport back");
        expect(g_seen.jitter_reported >= reported + 5, "the consumer is told the jitter the scene draws carried");
        expect(g_seen.last.camera.viewport_expanded, "the consumer receives the actual expanded raster footprint");
        expect(na::native11::Stats().jitter_written == written, "the camera upload is left alone on the viewport route");
        // No request: the next frame's scene is the game's own.
        Scene(g.ctx.Get(), g, frame++);
        Post(g.ctx.Get(), g, true);
        g.swap->Present(0, 0);
        Scene(g.ctx.Get(), g, frame++);
        const D3D11_VIEWPORT plain = viewport_now();
        Post(g.ctx.Get(), g, true);
        g.swap->Present(0, 0);
        expect(plain.TopLeftX == 0.f && plain.TopLeftY == 0.f && plain.Width == float(g.w) && plain.Height == float(g.h),
               "a frame with no jitter request renders through the game's own viewports");
        expect(!g_seen.last.camera.viewport_expanded, "jitter off restores the original reconstruction footprint");
        // Submit a list after Present advances the armed offset. DLAA needs
        // the offset recorded into the executed draws, not the newer request.
        ComPtr<ID3D11DeviceContext> delayed;
        expect(SUCCEEDED(g.device->CreateDeferredContext(0, &delayed)), "create delayed viewport recording context");
        if (delayed) {
            na::native11::SetNextJitter(0.25f, -0.25f);
            Scene(g.ctx.Get(), g, frame++);
            Post(g.ctx.Get(), g, true);
            g.swap->Present(0, 0);
            Scene(delayed.Get(), g, frame);
            ComPtr<ID3D11CommandList> list;
            expect(SUCCEEDED(delayed->FinishCommandList(FALSE, &list)), "finish the delayed jitter list");
            na::native11::SetNextJitter(-0.25f, 0.25f);
            Scene(g.ctx.Get(), g, frame++);
            Post(g.ctx.Get(), g, true);
            g.swap->Present(0, 0);
            if (list) {
                const auto before = g_seen.calls;
                g.ctx->ExecuteCommandList(list.Get(), FALSE);
                Post(g.ctx.Get(), g, true);
                expect(g_seen.calls == before + 1 && g_seen.last.camera.viewport_jitter &&
                       g_seen.last.camera.jitter_applied && g_seen.last.camera.jitter_px[0] == 0.25f &&
                       g_seen.last.camera.jitter_px[1] == -0.25f && g_seen.last.camera.viewport_expanded,
                       "delayed list reports the executed viewport sample after Present advances");
                g.swap->Present(0, 0);
            }
        }
        na::native11::SetJitterRoute(na::native11::JitterRoute::Matrix);
    }
#endif

    // ---- phase 2: scene and post recorded on deferred contexts ----
    const uint64_t calls1 = g_seen.calls, lists1 = g_seen.before_list;
    ComPtr<ID3D11DeviceContext> d_scene, d_post;
    g.device->CreateDeferredContext(0, &d_scene);
    g.device->CreateDeferredContext(0, &d_post);
    expect(d_scene && d_post, "deferred contexts were created");
    for (int i = 0; d_scene && d_post && i < 200 && g_seen.before_list - lists1 < 20; ++i, ++frame) {
        Pump();
        ComPtr<ID3D11CommandList> l_scene, l_post;
        Scene(d_scene.Get(), g, frame);
        if (FAILED(d_scene->FinishCommandList(FALSE, &l_scene))) { expect(false, "finish scene list"); break; }
        Post(d_post.Get(), g, false);
        if (FAILED(d_post->FinishCommandList(FALSE, &l_post))) { expect(false, "finish post list"); break; }
        g.ctx->ExecuteCommandList(l_scene.Get(), FALSE);
        g.ctx->ExecuteCommandList(l_post.Get(), FALSE);
        g.swap->Present(0, 0);
    }
    na::native11::FormatStatus(status, sizeof status);
    std::printf("phase 2: calls=+%llu before_list=+%llu | %s\n", static_cast<unsigned long long>(g_seen.calls - calls1),
                static_cast<unsigned long long>(g_seen.before_list - lists1), status);
    expect(g_seen.before_list - lists1 >= 20, "deferred: the consumer runs before the post-processing command list");
    expect(g_seen.wrong_inputs == 0 && g_seen.no_scene == 0 && g_seen.twice == 0, "deferred: same inputs, scene drawn, once per frame");

    // ---- resize: nothing of the game's held, new targets found, new stream ----
    const uint64_t color_inc = g_seen.color_inc, streams = g_seen.streams, calls2 = g_seen.calls;
    g.ctx->ClearState();
    g.back_rtv.Reset();
    g.color_srv.Reset();
    g.color_rtv.Reset();
    g.dsv.Reset();
    g.color.Reset();
    g.depth.Reset();
    g.ctx->Flush();
    g.w = 256;
    g.h = 144;
    const HRESULT rz = g.swap->ResizeBuffers(0, g.w, g.h, DXGI_FORMAT_UNKNOWN, 0);
    expect(SUCCEEDED(rz), "ResizeBuffers succeeds under the hooks (no back buffer held)");
    expect(SUCCEEDED(rz) && MakeTargets(g), "the game makes its new-size targets");
    for (int i = 0; SUCCEEDED(rz) && i < 200 && g_seen.calls - calls2 < 20; ++i, ++frame) {
        Pump();
        Scene(g.ctx.Get(), g, frame);
        Post(g.ctx.Get(), g, true);
        g.swap->Present(0, 0);
    }
    na::native11::FormatStatus(status, sizeof status);
    std::printf("resize: calls=+%llu | %s\n", static_cast<unsigned long long>(g_seen.calls - calls2), status);
    expect(g_seen.calls - calls2 >= 20 && g_seen.color_w == 256 && g_seen.color_h == 144, "after the resize the new scene colour is handed over");
    expect(g_seen.color_inc != color_inc && g_seen.streams > streams, "the new colour has a new lifetime id and starts a new stream");
    expect(g_seen.wrong_inputs == 0, "no call ever named the wrong colour or depth");
#if defined(NA_TEST_NATIVE_DLAA)
    na::native_d3d11::FormatStatus(status, sizeof status);
    std::printf("%s\n", status);
    expect(evaluations + 2 >= g_seen.calls && evaluations >= 65,
           "real DLAA reaches the native scene across immediate, deferred, and resize cases");
    bool native_status = false;
    for (unsigned i = 0; i < 120 && !native_status; ++i) {
        std::ifstream log{std::filesystem::path(process_log)};
        std::string line;
        while (std::getline(log, line)) {
            const auto delivered_at = line.find("native11_dlaa delivered=");
            if (delivered_at == std::string::npos) continue;
            const auto fps_at = line.find(" fps=");
            if (fps_at != std::string::npos && std::atof(line.c_str() + fps_at + 5) > 0.0 &&
                std::strtoull(line.c_str() + delivered_at + std::strlen("native11_dlaa delivered="), nullptr, 10) > 0) {
                native_status = true;
                break;
            }
        }
        if (!native_status) {
            Scene(g.ctx.Get(), g, frame++);
            Post(g.ctx.Get(), g, true);
            g.swap->Present(0, 0);
            Pump();
        }
    }
    expect(native_status, "status worker records native FPS and DLAA delivery without a DX12 swap chain");
    expect(na::overlay::GetStats().initialized == 1, "Home renderer initialized on the native swap chain");
    SendMessageW(g.wnd, WM_KEYDOWN, VK_HOME, 1);
    for (unsigned i = 0; i < 12; ++i) {
        Scene(g.ctx.Get(), g, frame++);
        Post(g.ctx.Get(), g, true);
        expect(SUCCEEDED(g.swap->Present(0, 0)), "native Present with Home open");
        Pump();
    }
    char menu_status[2048]{};
    na::on12::FormatStatus(menu_status, sizeof menu_status);
    std::printf("%s\n", menu_status);
    const char* draws = std::strstr(menu_status, "menu{draws=");
    expect(na::overlay::GetStats().open && draws && std::strtoull(draws + std::strlen("menu{draws="), nullptr, 10) >= 8 &&
           std::strstr(draws, " failures=0}") != nullptr, "Home is composited repeatedly on native DX11 without failures");
    SendMessageW(g.wnd, WM_KEYDOWN, VK_HOME, 1);
    g.swap->Present(0, 0);
    expect(!na::overlay::GetStats().open, "Home closes on the native swap chain");
    // The earlier fixture only provided VP+inverse. Replace its camera with
    // the observed WWII packet shape (relative VP, repeated origin, no inverse).
    D3D11_BUFFER_DESC camera_desc{};
    g.cam_cb->GetDesc(&camera_desc);
    camera_desc.ByteWidth = 1280;
    expect(SUCCEEDED(g.device->CreateBuffer(&camera_desc, nullptr, g.cam_cb.ReleaseAndGetAddressOf())), "create WWII-shaped camera buffer");
    g.campaign_camera = true;
    const auto before_campaign = evaluations;
    const auto before_jitter = g_seen.jitter_reported;
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const auto captures = std::filesystem::path(temp) / (L"NA-native-camera-capture-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(captures / L"eval");
    std::filesystem::create_directories(captures / L"present");
    na::capture::j2::Request(3, (captures / L"eval").c_str());
    na::capture::RequestPresents(3, (captures / L"present").c_str());
    auto present_campaign = [&]() {
        Scene(g.ctx.Get(), g, frame++);
        Post(g.ctx.Get(), g, true);
        g.swap->Present(0, 0);
        Pump();
    };
    na::j2::SetJitter(true);
    for (int i = 0; i < 22; ++i) present_campaign();
    expect(g_seen.last.camera.buffer == reinterpret_cast<uint64_t>(static_cast<ID3D11Resource*>(g.cam_cb.Get())) &&
           g_seen.last.camera.size == 1280 && g_seen.last.camera.version != 0 && evaluations >= before_campaign + 16,
           "native DLAA accepts the current WWII camera packet with no inverse");
    expect(g_seen.jitter_reported >= before_jitter + 12, "native WWII camera uploads carry the applied jitter");
    na::j2::SetJitter(false);
    present_campaign(); // an already armed phase may still apply to this frame
    const auto jitter_off = g_seen.jitter_reported;
    for (int i = 0; i < 4; ++i) present_campaign();
    expect(g_seen.jitter_reported == jitter_off, "jitter switch stops changing future WWII camera uploads");
    na::j2::SetPaused(true);
    const auto paused = evaluations;
    const auto before_pause = na::native11::Stats();
    for (int i = 0; i < 3; ++i) present_campaign();
    const auto in_pause = na::native11::Stats();
    expect(evaluations == paused, "DLAA switch stops native evaluations");
    expect(in_pause.camera_found == before_pause.camera_found && in_pause.camera_missing == before_pause.camera_missing,
           "DLAA switch stops camera search before the next campaign draw");
    na::j2::SetPaused(false);
    present_campaign();
    expect(evaluations > paused, "DLAA switch resumes native evaluations");
    for (int i = 0; i < 100 && (na::capture::j2::GetStats().written < 3 || na::capture::GetStats().frames_written < 3); ++i) {
        present_campaign();
        Sleep(10);
    }
    const auto ec = na::capture::j2::GetStats();
    const auto pc = na::capture::GetStats();
    expect(ec.written == 3 && ec.without_present == 0 && ec.errors == 0 &&
           std::filesystem::exists(captures / L"eval" / L"00000-present.raw"), "native DLAA capture saves evaluations with their actual presented image");
    expect(pc.frames_written == 3 && pc.errors == 0, "native presented-frame capture writes the backbuffers");
    // A second scene uses a PS-only packet in a D3D11.1 ranged bind above
    // 16 KiB, as seen in the translated campaign's recorded bindings.
    camera_desc.ByteWidth = 32768;
    expect(SUCCEEDED(g.device->CreateBuffer(&camera_desc, nullptr, g.cam_cb.ReleaseAndGetAddressOf())),
           "create camera upload ring");
    g.campaign_pixel_range = true;
    na::j2::SetJitter(true);
    const auto before_pixel = evaluations;
    const auto raster_before_pixel = g_seen.jitter_reported;
    for (int i = 0; i < 22; ++i) present_campaign();
    expect(evaluations >= before_pixel + 16 &&
           g_seen.last.camera.buffer == reinterpret_cast<uint64_t>(static_cast<ID3D11Resource*>(g.cam_cb.Get())) &&
           g_seen.last.camera.vs_slot == -1 && g_seen.last.camera.ps_slot == 0 &&
           g_seen.last.camera.first_constant == 24576 / 16 && g_seen.last.camera.num_constants == 1280 / 16,
           "native DLAA accepts the current WWII pose from the actual PS binding range");
    expect(g_seen.jitter_reported == raster_before_pixel, "PS-only camera is never treated as raster jitter");
    g.campaign_mixed_ring = true;
    const auto before_mixed_ring = evaluations;
    for (int i = 0; i < 22; ++i) present_campaign();
    expect(evaluations == before_mixed_ring + 22 && g_seen.last.camera.first_constant == 24576 / 16,
           "mixed VS/PS ring retains camera range after another range and discard overwrite");
    g.campaign_mixed_ring = false;
    // Battlefield-style large dynamic buffer with a shader-visible range far
    // beyond 64 KiB. Keep the prior small-ring assertions above unchanged.
    if (automatic) {
        camera_desc.ByteWidth = 1024 * 1024;
        expect(SUCCEEDED(g.device->CreateBuffer(&camera_desc, nullptr, g.cam_cb.ReleaseAndGetAddressOf())),
               "create large native camera upload ring");
        g.camera_ring_offset = 53056 * 16;
        g.camera_snapshot_size = 1280 + 2 * 2048;
        const auto before_large = evaluations;
        for (int i = 0; i < 12; ++i) {
            g.camera_ring_offset = 53056 * 16 + (i % 2) * 256;
            present_campaign();
        }
        expect(evaluations >= before_large + 9 &&
               g_seen.last.camera.first_constant == g.camera_ring_offset / 16 &&
               g_seen.last.camera.bytes_first_constant < g_seen.last.camera.first_constant &&
               g_seen.last.camera.size <= 16 * 1024,
               "DLAA receives the current high-offset binding without copying the whole ring");
        // The real run rotates between distant ranges of the same buffer.
        // After both have been observed, no frame may borrow stale bytes or
        // drop out just because the range differs from the previous frame.
        const uint32_t first_range = g.camera_ring_offset;
        const uint32_t second_range = 46000 * 16;
        g.camera_ring_offset = second_range;
        present_campaign();
        present_campaign();
        const auto before_alternating = evaluations;
        for (int i = 0; i < 12; ++i) {
            g.camera_ring_offset = i % 2 ? first_range : second_range;
            present_campaign();
            expect(g_seen.last.camera.first_constant == g.camera_ring_offset / 16,
                   "each alternating frame consumes its own camera range");
        }
        expect(evaluations == before_alternating + 12, "learned disjoint ranges sustain uninterrupted DLAA");
        // An unfamiliar binding must never reuse a captured window.
        const auto before_jump = evaluations;
        g.camera_ring_offset = 982 * 1024;
        present_campaign();
        // The frame is not dropped either: it carries the previous frame's camera, marked held,
        // from the range that camera was read at.
        expect(evaluations == before_jump ||
               (evaluations == before_jump + 1 && g_seen.last.camera.held &&
                g_seen.last.camera.first_constant != g.camera_ring_offset / 16),
               "a disjoint large-buffer binding cannot reuse old camera bytes");
        for (int i = 0; i < 8; ++i) present_campaign();
        expect(evaluations >= before_jump + 6 &&
               g_seen.last.camera.first_constant == g.camera_ring_offset / 16,
               "large-buffer capture follows the newly observed binding");
        // A frame allocator puts the camera wherever this frame's earlier constants ended, so the
        // place follows what is drawn and never has to repeat (places below are from a recorded
        // run, in 16-byte constants). The bound range is read at the draw: every frame gets its
        // own camera, including the first frame at a place never seen before.
        D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
        if (SUCCEEDED(g.device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof options)) &&
            options.MapNoOverwriteOnDynamicConstantBuffer) {
            expect(SUCCEEDED(g.device->CreateBuffer(&camera_desc, nullptr, g.cam_cb.ReleaseAndGetAddressOf())),
                   "create a fresh large frame-allocator buffer");
            g.camera_no_overwrite = true;
            const uint32_t recorded[] = {13392, 34192, 33536, 21952, 15408, 29696, 30160, 21488,
                                         48288, 46704, 48400, 46656, 46080, 44128, 46880, 46832};
            g.camera_ring_offset = recorded[0] * 16;
            for (int i = 0; i < 3; ++i) present_campaign(); // the buffer becomes a camera candidate
            const auto before_moving = evaluations;
            for (const uint32_t place : recorded) {
                g.camera_ring_offset = place * 16;
                present_campaign();
                expect(g_seen.last.camera.first_constant == place && g_seen.last.camera.bytes_first_constant == place &&
                       g_seen.last.camera.size == 1280,
                       "the frame's camera is exactly the range its scene draws bound");
            }
            expect(evaluations == before_moving + std::size(recorded),
                   "a camera whose place moves every frame gets DLAA on every frame");
            g.camera_no_overwrite = false;
        }
        camera_desc.ByteWidth = 32768;
        expect(SUCCEEDED(g.device->CreateBuffer(&camera_desc, nullptr, g.cam_cb.ReleaseAndGetAddressOf())),
               "restore the small camera upload ring");
        g.camera_ring_offset = 24576;
        g.camera_snapshot_size = 32768;
        const auto before_restore = evaluations;
        for (int i = 0; i < 4; ++i) present_campaign();
        expect(evaluations >= before_restore + 2 && g_seen.last.camera.size == 32768 &&
               g_seen.last.camera.bytes_first_constant == 0,
               "the original small-buffer capture resumes after returning to a new buffer");
    }
    {
        // A layered renderer (shape recorded from a real game: DLAA was handed the 8-bit normals
        // layer, the most-drawn target, while the lit picture went to the screen untouched).
        D3D11_TEXTURE2D_DESC layer_desc{};
        g.color->GetDesc(&layer_desc);
        layer_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        expect(SUCCEEDED(g.device->CreateTexture2D(&layer_desc, nullptr, &g.layer)) &&
               SUCCEEDED(g.device->CreateRenderTargetView(g.layer.Get(), nullptr, &g.layer_rtv)) &&
               SUCCEEDED(g.device->CreateShaderResourceView(g.layer.Get(), nullptr, &g.layer_srv)) &&
               SUCCEEDED(g.device->CreateTexture2D(&layer_desc, nullptr, &g.layer2)) &&
               SUCCEEDED(g.device->CreateRenderTargetView(g.layer2.Get(), nullptr, &g.layer2_rtv)),
               "create two material layers");
        g.layered = true;
        for (int i = 0; i < 6; ++i) present_campaign(); // the layers are seen, then their read, then the whole scene
        const auto before_layered = evaluations;
        const auto calls_before_layered = g_seen.calls;
        bool picture = true, whole_scene = true;
        for (int i = 0; i < 14; ++i) {
            const auto calls = g_seen.calls;
            present_campaign();
            if (g_seen.calls == calls) continue; // judged on the frames handed over; their number is checked below
            picture = picture && g_seen.last.color.d3d11_resource == static_cast<ID3D11Resource*>(g.color.Get()) &&
                      g_seen.last.color.format == kSceneFormat;
            whole_scene = whole_scene && g_seen.last.scene_draws == 44;
        }
        char scene_status[4096]; // the scene field closes a line longer than the shared 512-byte buffer
        na::native11::FormatStatus(scene_status, sizeof scene_status);
        std::printf("native layered scene: %s\n", scene_status);
        expect(picture && g_seen.calls >= calls_before_layered + 10 && evaluations >= before_layered + 10,
               "a layered renderer: DLAA is handed the lit picture, never the most-drawn material layer");
        expect(whole_scene && std::strstr(scene_status, "scene{picture=after-layers") != nullptr,
               "the scene is the layers' draws and the picture's draws, and the status says which rule chose");
        g.layered = false;
        for (int i = 0; i < 4; ++i) present_campaign();
        expect(g_seen.last.color.d3d11_resource == static_cast<ID3D11Resource*>(g.color.Get()) && g_seen.last.scene_draws == 40,
               "a scene drawn into one target again keeps its picture");
    }
    {
        // The scene drawn straight onto the screen and never read (a shape seen on a real game:
        // 3,704 frames, no place for DLAA ever learned). Two kinds of HUD after it.
        D3D11_DEPTH_STENCIL_DESC off{};
        off.DepthEnable = FALSE;
        off.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        off.DepthFunc = D3D11_COMPARISON_ALWAYS;
        expect(SUCCEEDED(g.device->CreateDepthStencilState(&off, &g.depth_off)), "create a depth-test-off state");
        for (int mode = 1; mode <= 2; ++mode) {
            g.on_screen = mode;
            for (int i = 0; i < 8; ++i) present_campaign(); // the new picture is chosen, then its HUD is learned
            const auto before_screen = evaluations;
            const auto calls_before_screen = g_seen.calls;
            const auto wrong_before = g_seen.wrong_inputs;
            bool before_hud = true;
            for (int i = 0; i < 14; ++i) {
                const auto calls = g_seen.calls;
                present_campaign();
                if (g_seen.calls == calls) continue;
                before_hud = before_hud && g_seen.last.boundary == na::native11::Boundary::BeforeHud && g_seen.last.scene_draws == 40;
            }
            char screen_status[4096];
            na::native11::FormatStatus(screen_status, sizeof screen_status);
            std::printf("native scene on screen (HUD kind %d): %s\n", mode, screen_status);
            expect(before_hud && g_seen.wrong_inputs == wrong_before && g_seen.calls >= calls_before_screen + 10 &&
                   evaluations >= before_screen + 10,
                   mode == 1 ? "a scene drawn straight onto the screen: DLAA runs on it before a HUD drawn without depth"
                             : "a scene drawn straight onto the screen: DLAA runs on it before a HUD that keeps the scene depth, test off");
            expect(std::strstr(screen_status, "backbuffer=1") != nullptr &&
                   std::strstr(screen_status, mode == 1 ? " learned_hud=1 " : " learned_untested_hud=1 ") != nullptr,
                   "the status names the screen target and which HUD rule placed DLAA");
        }
        g.on_screen = 0;
        for (int i = 0; i < 8; ++i) present_campaign();
        expect(g_seen.last.color.d3d11_resource == static_cast<ID3D11Resource*>(g.color.Get()) &&
               g_seen.last.boundary == na::native11::Boundary::BeforeRead && g_seen.last.scene_draws == 40,
               "a scene drawn into its own target again goes back to the read point");
    }
    na::settings::Get().j2_sharpness.store(60);
    na::settings::Get().j2_preset.store(11);
    present_campaign();
    na::native_d3d11::FormatStatus(status, sizeof status);
    expect(std::strstr(status, "preset=11 sharpness=60 raster_slot=-1") != nullptr,
           "native DLAA submits the selected preset and applies the selected RCAS sharpness");
    na::settings::Get().j2_sharpness.store(0);
    present_campaign();
    na::native_d3d11::FormatStatus(status, sizeof status);
    expect(std::strstr(status, "preset=11 sharpness=0 raster_slot=-1") != nullptr,
           "sharpness at zero bypasses RCAS on the native route");
    const auto effects_before = evaluations;
    const auto flow_before = na::j2::compute_flow::GetStats();
    na::j2::flow::SetEnabled(true);
    na::settings::Get().xegtao_enabled.store(1);
    na::settings::Get().donut_bloom.store(1);
    na::settings::Get().donut_tone.store(1);
    present_campaign(); // starts existing asynchronous shader preparation
    na::xegtao::ScenePass().WaitPrepared(20000); // offline fixture only
    na::donut::ScenePass().WaitPrepared(20000);
    for (int i = 0; i < 20; ++i) present_campaign();
    const auto flow_live = na::j2::compute_flow::GetStats();
    const auto ao_live = na::xegtao::ScenePass().GetStats();
    const auto lighting_live = na::donut::ScenePass().GetStats();
    std::printf("native effects: flow pairs=%llu submitted=%llu busy=%llu unavailable=%llu ao=%llu lighting=%llu\n",
        (unsigned long long)flow_live.submitted_pairs, (unsigned long long)flow_live.submitted,
        (unsigned long long)flow_live.busy, (unsigned long long)flow_live.unavailable,
        (unsigned long long)ao_live.recorded, (unsigned long long)lighting_live.recorded);
    expect(flow_live.submitted >= flow_before.submitted + 20 && flow_live.submitted_pairs >= flow_before.submitted_pairs + 16,
           "native optical-flow switch submits consecutive paired frames with retired recording ownership");
    expect(ao_live.recorded >= 20 && lighting_live.recorded >= 20 && evaluations == effects_before + 21,
           "native XeGTAO and lighting run before real DLAA without missing frames");
    static char route_status[2048]{};
    na::native_d3d11::FormatStatus(route_status, sizeof route_status);
    if (std::strstr(route_status, "dlaa_companion started=1")) {
        // With DLSS in the companion program (NA_DLAA_COMPANION=1), the companion reads the shared
        // scene colour, so the passes' picture has to be copied into it on every frame.
        const char* shaded = std::strstr(route_status, "companion_shaded=");
        std::printf("companion route: %s\n", shaded ? shaded : "(no companion_shaded in the status)");
        expect(shaded && std::strtoull(shaded + std::strlen("companion_shaded="), nullptr, 10) >= 20,
               "ambient shading and bloom reach the picture the 64-bit companion reads");
    }
    na::j2::flow::SetEnabled(false);
    na::settings::Get().xegtao_enabled.store(0);
    na::settings::Get().donut_bloom.store(0);
    na::settings::Get().donut_tone.store(0);
    for (int i = 0; i < 4; ++i) present_campaign();
    expect(na::j2::compute_flow::GetStats().submitted == flow_live.submitted &&
           na::xegtao::ScenePass().GetStats().recorded == ao_live.recorded &&
           na::donut::ScenePass().GetStats().recorded == lighting_live.recorded,
           "native enhancement switches stop their GPU work live");
    // Reproduce the plain VS/PS slot-3 layout recorded in native WWII p30648.
    // The duplicated pose proves motion, but the raster transform is unknown.
    g.campaign_native_pose = true;
    g.campaign_pixel_range = false;
    camera_desc.ByteWidth = 1280;
    expect(SUCCEEDED(g.device->CreateBuffer(&camera_desc, nullptr, g.cam_cb.ReleaseAndGetAddressOf())),
           "create native campaign camera buffer");
    g.ctx->ClearState();
    // Changing buffers invalidates the previous learned range. That first
    // frame lets the provider return to acquisition for the new buffer.
    present_campaign();
    const auto before_native_pose = evaluations;
    const auto jitter_before_native_pose = g_seen.jitter_reported;
    const auto before_pose_stats = na::native11::Stats();
    for (int i = 0; i < 22; ++i) present_campaign();
    na::native_d3d11::FormatStatus(status, sizeof status);
    std::printf("native campaign slot-3: delivered +%llu | %s\n",
                (unsigned long long)(evaluations - before_native_pose), status);
    expect(evaluations == before_native_pose + 22 &&
           (g_seen.last.camera.vs_slot == 3 || g_seen.last.camera.ps_slot == 3),
           "native campaign slot-3 pose delivers all 22 NVIDIA DLAA frames");
    expect(!g_seen.last.camera.raster_jitter_qualified && g_seen.jitter_reported == jitter_before_native_pose,
           "the native campaign pose does not authorize a raster jitter write");
    const auto accepted_pose_stats = na::native11::Stats();
    expect(accepted_pose_stats.pose_native == before_pose_stats.pose_native + 22,
           "native pose counter advances once per accepted scene");
    g.campaign_single_origin = true;
    present_campaign();
    g.campaign_single_origin = false;
    const auto rejected_pose_stats = na::native11::Stats();
    // Native p47656: this form (matrix copy at 192, origin at 928 only) ran 36 min of play.
    expect(rejected_pose_stats.pose_repeat == accepted_pose_stats.pose_repeat &&
           rejected_pose_stats.camera_missing == accepted_pose_stats.camera_missing &&
           rejected_pose_stats.pose_native == accepted_pose_stats.pose_native + 1 &&
           evaluations == before_native_pose + 23,
           "the single-origin form (matrix copy at 192) yields the native pose and delivers DLAA");
    // Native p37752: the game leaves the second origin copy blank for whole stretches (30 s of
    // rejected frames, no DLAA). Both matrices and the first origin still establish the pose.
    g.campaign_blank_witness = true;
    for (int i = 0; i < 4; ++i) present_campaign();
    g.campaign_blank_witness = false;
    const auto blank_pose_stats = na::native11::Stats();
    expect(blank_pose_stats.pose_origin == rejected_pose_stats.pose_origin &&
           blank_pose_stats.camera_missing == rejected_pose_stats.camera_missing &&
           blank_pose_stats.pose_native > rejected_pose_stats.pose_native,
           "a blank second origin copy still yields the native pose");
    // The production route: jitter through the viewports needs no proven camera layout.
    {
        na::native11::SetJitterRoute(na::native11::JitterRoute::Viewport);
        na::j2::SetJitter(true);
        const auto prior_r2 = na::settings::Get().j2_jitter_r2.load();
        const auto prior_preset = na::settings::Get().j2_preset.load();
        na::settings::Get().j2_jitter_r2.store(1);
        na::settings::Get().j2_preset.store(13); // Cort keeps M throughout the new R2 cycle.
        const auto jitter_before_viewport = g_seen.jitter_reported;
        const auto delivered_before_viewport = evaluations;
        bool exact_r2 = true;
        for (int i = 0; i < 34; ++i) {
            present_campaign();
            if (i < 2) continue; // previous request was armed before this switch
            const auto& f = g_seen.last;
            const auto want = na::native11::DlaaJitterSample(uint32_t(f.frame - 1), true);
            exact_r2 = exact_r2 && f.camera.viewport_expanded && f.camera.jitter_applied &&
                f.camera.jitter_px[0] == float(want.x_pixels) && f.camera.jitter_px[1] == float(want.y_pixels);
        }
        na::native11::FormatStatus(status, sizeof status);
        std::printf("native campaign viewport jitter: %s\n", status);
        expect(evaluations == delivered_before_viewport + 34, "DLAA keeps delivering through two complete R2 cycles");
        expect(exact_r2, "actual DX11 draws and DLAA handoff carry the same sixteen-sample R2 sequence and footprint");
        na::native_d3d11::FormatStatus(status, sizeof status);
        expect(std::strstr(status, "preset=13") != nullptr, "NVIDIA DLAA keeps preset M during sixteen-sample R2");
        expect(g_seen.last.camera.jitter_applied && g_seen.jitter_reported >= jitter_before_viewport + 8,
               "the native campaign pose gets real jitter through the viewports");
        const auto jitter_resets = [&] {
            na::native_d3d11::FormatStatus(status, sizeof status);
            const char* count = std::strstr(status, "jitter_switch=");
            unsigned long long resets = 0;
            expect(count && sscanf_s(count, "jitter_switch=%llu", &resets) == 1,
                   "read the native jitter history reset counter");
            return resets;
        };
        const auto before_switch = jitter_resets();
        na::j2::SetJitter(false);
        for (int i = 0; i < 2; ++i) {
            present_campaign(); // an already armed sample may still apply to the first frame
            const auto wanted = before_switch + (g_seen.last.raster.applied ? 0 : 1);
            expect(jitter_resets() == wanted, "switch-off resets DLAA with the first unjittered picture");
        }
        const auto viewport_off = g_seen.jitter_reported;
        for (int i = 0; i < 4; ++i) present_campaign();
        expect(g_seen.jitter_reported == viewport_off, "the jitter switch stops the viewport offsets");
        expect(!g_seen.last.camera.viewport_expanded, "native DLAA returns to the original footprint with jitter off");
        expect(jitter_resets() == before_switch + 1, "jitter-off pictures keep their new history");
        na::j2::SetJitter(true);
        for (int i = 0; i < 5; ++i) {
            present_campaign();
            const auto wanted = before_switch + (g_seen.last.raster.applied ? 2 : 1);
            expect(jitter_resets() == wanted, "switch-on resets DLAA with the first jittered picture");
        }
        expect(jitter_resets() == before_switch + 2, "jitter-on pictures keep their new history");
        na::j2::SetJitter(false);
        na::settings::Get().j2_jitter_r2.store(prior_r2);
        na::settings::Get().j2_preset.store(prior_preset);

        camera_desc.ByteWidth = 160;
        expect(SUCCEEDED(g.device->CreateBuffer(&camera_desc, nullptr, &g.split_world)), "create split world block");
        camera_desc.ByteWidth = 64;
        expect(SUCCEEDED(g.device->CreateBuffer(&camera_desc, nullptr, &g.split_projection)), "create split projection block");
        g.campaign_split_camera = true;
        // Newly bound buffers get acquired after the old source misses.
        present_campaign();
        const auto split_start = evaluations;
        for (int i = 0; i < 8; ++i) present_campaign();
        expect(evaluations == split_start + 8 && g_seen.last.camera.split_pose &&
               g_seen.last.camera.split_pose->origin[0] == -11659.0,
               "split camera keeps DLAA delivering and excludes a late identity-basis object pose");
        g.split_tie = true;
        const auto tied = evaluations;
        present_campaign();
        expect(evaluations == tied, "equally supported contradictory world origins do not deliver a false pose");
        g.split_tie = false;
        present_campaign();
        expect(evaluations == tied + 1, "fresh scene agreement restores split-camera DLAA");
        g.campaign_split_camera = false;
        na::native11::SetJitterRoute(na::native11::JitterRoute::Matrix);
    }
    {
        // Constant buffers taken from a rotating pool (seen on a real game: the camera was found on
        // every other frame only, 7,465 of 14,929, and jitter never reached a delivered frame).
        D3D11_BUFFER_DESC pool_desc{};
        g.cam_cb->GetDesc(&pool_desc);
        g.cam_pool[0] = g.cam_cb;
        expect(SUCCEEDED(g.device->CreateBuffer(&pool_desc, nullptr, &g.cam_pool[1])), "create the second camera buffer of the pool");
        const bool jitter_was = na::j2::Jitter();
        na::native11::SetJitterRoute(na::native11::JitterRoute::Viewport); // the production route
        na::j2::SetJitter(true);
        g.rotate_camera = true;
        for (int i = 0; i < 10; ++i) present_campaign(); // the rotation is seen twice, then every upload is kept
        const auto before_pool = evaluations;
        const auto jitter_before_pool = g_seen.jitter_reported;
        const auto no_camera_before = g_seen.no_camera;
        const auto changed_before_pool = g_seen.changed;
        const auto streams_before_pool = g_seen.streams;
        for (int i = 0; i < 30; ++i) present_campaign();
        char pool_status[4096];
        na::native11::FormatStatus(pool_status, sizeof pool_status);
        std::printf("native rotating camera buffers: %s\n", pool_status);
        expect(evaluations >= before_pool + 28 && g_seen.no_camera == no_camera_before,
               "camera buffers from a rotating pool: the camera is found on every frame, not every other one");
        expect(g_seen.jitter_reported >= jitter_before_pool + 24, "rotating camera buffers: delivered frames carry the applied jitter");
        expect(std::strstr(pool_status, "camera_bytes{keep_all=1 ") != nullptr, "the status says every upload is being kept");
        expect(g_seen.changed == changed_before_pool && g_seen.streams == streams_before_pool,
               "rotating camera buffers: a new buffer object is not a change of inputs, so DLSS keeps its history");
        g.rotate_camera = false;
        g.cam_cb = g.cam_pool[0];
        for (int i = 0; i < 4; ++i) present_campaign();
        na::j2::SetJitter(jitter_was);
        present_campaign();
        na::native11::SetJitterRoute(na::native11::JitterRoute::Matrix);
    }
    // Real shader sampling: each mip is a different colour. Exact LOD 1
    // produces green; a total bias of -1 selects red mip 0. No counters stand
    // in for the pixel result, and the sampler is bound only once.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 64; td.MipLevels = 7; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<uint32_t> pixels[7]; D3D11_SUBRESOURCE_DATA initial[7]{};
    for (UINT mip = 0; mip < 7; ++mip) {
        const UINT extent = 64u >> mip;
        pixels[mip].assign(size_t(extent) * extent, mip == 0 ? 0xff0000ffu : mip == 1 ? 0xff00ff00u : 0xffff0000u);
        initial[mip] = {pixels[mip].data(), extent * 4, 0};
    }
    ComPtr<ID3D11Texture2D> mip_texture;
    ComPtr<ID3D11ShaderResourceView> mip_view;
    ComPtr<ID3D11PixelShader> mip_shader;
    auto mip_code = Compile("ps_mips", "ps_5_0");
    expect(mip_code && SUCCEEDED(g.device->CreateTexture2D(&td, initial, &mip_texture)) &&
           SUCCEEDED(g.device->CreateShaderResourceView(mip_texture.Get(), nullptr, &mip_view)) &&
           SUCCEEDED(g.device->CreatePixelShader(mip_code->GetBufferPointer(), mip_code->GetBufferSize(), nullptr, &mip_shader)),
           "create native material-mip pixel test");
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX; sampler.MaxAnisotropy = 1;
    ComPtr<ID3D11SamplerState> material;
    expect(SUCCEEDED(g.device->CreateSamplerState(&sampler, &material)), "create native material sampler");
    na::observe::SetSamplerMipBias(0.f); na::observe::SetSamplerAnisotropy(0);
    auto* material_raw = material.Get();
    g.ctx->PSSetSamplers(0, 1, &material_raw);
    g.ctx->ClearState();
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    // The synthetic quads use the opposite winding to the default cull state.
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE; raster.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> no_cull;
    g.device->CreateRasterizerState(&raster, &no_cull); g.ctx->RSSetState(no_cull.Get());
    g.ctx->PSSetSamplers(0, 1, &material_raw);
    auto* rt = g.color_rtv.Get(); g.ctx->OMSetRenderTargets(1, &rt, nullptr);
    auto* texture_view = mip_view.Get(); g.ctx->PSSetShaderResources(0, 1, &texture_view);
    g.ctx->VSSetShader(g.vs_post.Get(), nullptr, 0); g.ctx->PSSetShader(mip_shader.Get(), nullptr, 0);
    Viewport(g.ctx.Get(), g.w, g.h);
    float before_bias[4]{}, after_bias[4]{}, restored_bias[4]{};
    g.ctx->Draw(4, 0); ReadCopy(g, before_bias, g.color.Get());
    na::observe::SetSamplerMipBias(-1.f);
    g.ctx->Draw(4, 0); ReadCopy(g, after_bias, g.color.Get());
    na::observe::SetSamplerMipBias(0.f);
    g.ctx->Draw(4, 0); ReadCopy(g, restored_bias, g.color.Get());
    std::printf("native mip pixels: own %.2f/%.2f, -1 %.2f/%.2f, restored %.2f/%.2f\n",
                before_bias[0], before_bias[1], after_bias[0], after_bias[1], restored_bias[0], restored_bias[1]);
    expect(before_bias[1] > .9f && after_bias[0] > .9f && after_bias[1] < .1f && restored_bias[1] > .9f,
           "live native mip bias changes sampled pixels and restores without another game sampler bind");
    na::observe::SetSamplerMipBias(-1.f); na::observe::SetSamplerAnisotropy(16);
    g.ctx->Draw(4, 0);
    ComPtr<ID3D11SamplerState> bound;
    g.ctx->PSGetSamplers(0, 1, &bound); D3D11_SAMPLER_DESC applied{};
    if (bound) bound->GetDesc(&applied);
    expect(bound && applied.Filter == D3D11_FILTER_ANISOTROPIC && applied.MaxAnisotropy == 16 && applied.MipLODBias == -1.f,
           "the actual native draw binding has 16x AF and total -1 mip bias");
    // Deferred state queries must carry the same live policy to recorded draws.
    // Keep state over FinishCommandList so the second draw has no new sampler bind.
    ComPtr<ID3D11DeviceContext> sampler_deferred;
    g.device->CreateDeferredContext(0, &sampler_deferred);
    na::observe::SetSamplerAnisotropy(0);
    if (sampler_deferred) {
        auto* dc = sampler_deferred.Get();
        dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        dc->RSSetState(no_cull.Get()); dc->OMSetRenderTargets(1, &rt, nullptr);
        dc->VSSetShader(g.vs_post.Get(), nullptr, 0); dc->PSSetShader(mip_shader.Get(), nullptr, 0);
        dc->PSSetShaderResources(0, 1, &texture_view); material_raw = material.Get();
        dc->PSSetSamplers(0, 1, &material_raw); Viewport(dc, g.w, g.h);
        float deferred_on[4]{}, deferred_off[4]{};
        dc->Draw(4, 0);
        ComPtr<ID3D11CommandList> recorded;
        expect(SUCCEEDED(dc->FinishCommandList(TRUE, &recorded)), "record native sampler change");
        if (recorded) g.ctx->ExecuteCommandList(recorded.Get(), TRUE);
        ReadCopy(g, deferred_on, g.color.Get());
        na::observe::SetSamplerMipBias(0.f);
        dc->Draw(4, 0); recorded.Reset();
        expect(SUCCEEDED(dc->FinishCommandList(TRUE, &recorded)), "record native sampler restoration");
        if (recorded) g.ctx->ExecuteCommandList(recorded.Get(), TRUE);
        ReadCopy(g, deferred_off, g.color.Get());
        expect(deferred_on[0] > .9f && deferred_off[1] > .9f,
               "deferred draws update and restore mip pixels without rebinding");
    } else expect(false, "create deferred material context");
    na::observe::SetSamplerMipBias(-1.f);
    // A game sampler already at -1 must stay there, rather than receiving -2.
    sampler.MipLODBias = -1.f;
    ComPtr<ID3D11SamplerState> already;
    g.device->CreateSamplerState(&sampler, &already); material_raw = already.Get();
    g.ctx->PSSetSamplers(0, 1, &material_raw);
    bound.Reset(); g.ctx->PSGetSamplers(0, 1, &bound);
    if (bound) bound->GetDesc(&applied);
    expect(bound && applied.MipLODBias == -1.f && na::native11::GetSamplerStats().original_bias == -1.f,
           "native mip total avoids double bias when the game already requests -1");
    // After the frame's picture is handed to DLSS the game's later passes keep its own samplers;
    // the next frame's scene gets the policy again. Neither needs a new sampler bind by the game.
    na::observe::SetSamplerMipBias(-1.f); na::observe::SetSamplerAnisotropy(16);
    material_raw = material.Get();
    g.ctx->PSSetSamplers(0, 1, &material_raw);
    g.ctx->Draw(4, 0);
    const uint64_t own_before = na::native11::GetSamplerStats().after_scene;
    na::native11::SetSceneHanded(true);
    g.ctx->Draw(4, 0);
    bound.Reset(); g.ctx->PSGetSamplers(0, 1, &bound); applied = {};
    if (bound) bound->GetDesc(&applied);
    expect(bound && applied.Filter == D3D11_FILTER_MIN_MAG_MIP_LINEAR && applied.MipLODBias == 0.f &&
           na::native11::GetSamplerStats().after_scene > own_before,
           "draws after the scene is handed to DLSS sample with the game's own sampler");
    na::native11::SetSceneHanded(false);
    g.ctx->Draw(4, 0);
    bound.Reset(); g.ctx->PSGetSamplers(0, 1, &bound); applied = {};
    if (bound) bound->GetDesc(&applied);
    expect(bound && applied.Filter == D3D11_FILTER_ANISOTROPIC && applied.MaxAnisotropy == 16 && applied.MipLODBias == -1.f,
           "the next frame's scene draws get 16x AF and the -1 mip bias back");
    // Material textures: a published replacement is sampled in place of the game's view from the
    // next Present on, with no new bind by the game, and the game still reads back its own view.
    // The game's texture samples green at this LOD; the marker is white on every texel.
    na::observe::SetSamplerMipBias(0.f); na::observe::SetSamplerAnisotropy(0);
    D3D11_TEXTURE2D_DESC marker_desc = td;
    marker_desc.MipLevels = 1;
    std::vector<uint32_t> white(size_t(64) * 64, 0xffffffffu);
    D3D11_SUBRESOURCE_DATA marker_data{white.data(), 64 * 4, 0};
    ComPtr<ID3D11Texture2D> marker;
    ComPtr<ID3D11ShaderResourceView> marker_view;
    expect(SUCCEEDED(g.device->CreateTexture2D(&marker_desc, &marker_data, &marker)) &&
           SUCCEEDED(g.device->CreateShaderResourceView(marker.Get(), nullptr, &marker_view)),
           "create the marker texture");
    g.ctx->PSSetShaderResources(0, 1, &texture_view);
    float tex_own[4]{}, tex_early[4]{}, tex_swapped[4]{}, tex_off[4]{}, tex_on[4]{}, tex_removed[4]{};
    g.ctx->Draw(4, 0); ReadCopy(g, tex_own, g.color.Get());
    const auto textures_before = na::native11::GetTextureStats();
    expect(textures_before.installed && textures_before.candidates > 0,
           "the game's mipmapped colour texture is judged a candidate");
    expect(na::native11::PublishTexture(mip_view.Get(), marker_view.Get(), uint64_t(64) * 64 * 4),
           "publish a replacement for a view the game created");
    g.ctx->Draw(4, 0); ReadCopy(g, tex_early, g.color.Get());
    na::native11::TexturesOnPresent();
    g.ctx->Draw(4, 0); ReadCopy(g, tex_swapped, g.color.Get());
    ComPtr<ID3D11ShaderResourceView> seen;
    g.ctx->PSGetShaderResources(0, 1, &seen);
    na::native11::SetTexturesEnabled(false);
    g.ctx->Draw(4, 0); ReadCopy(g, tex_off, g.color.Get());
    na::native11::SetTexturesEnabled(true);
    g.ctx->Draw(4, 0); ReadCopy(g, tex_on, g.color.Get());
    const auto textures_on = na::native11::GetTextureStats();
    expect(na::native11::RemoveTexture(mip_view.Get()), "withdraw the replacement");
    na::native11::TexturesOnPresent();
    g.ctx->Draw(4, 0); ReadCopy(g, tex_removed, g.color.Get());
    const auto textures_after = na::native11::GetTextureStats();
    std::printf("native texture pixels (r/g/b): own %.2f/%.2f/%.2f, before present %.2f/%.2f/%.2f, replaced %.2f/%.2f/%.2f, "
                "off %.2f/%.2f/%.2f, on %.2f/%.2f/%.2f, withdrawn %.2f/%.2f/%.2f\n",
                tex_own[0], tex_own[1], tex_own[2], tex_early[0], tex_early[1], tex_early[2], tex_swapped[0], tex_swapped[1],
                tex_swapped[2], tex_off[0], tex_off[1], tex_off[2], tex_on[0], tex_on[1], tex_on[2], tex_removed[0],
                tex_removed[1], tex_removed[2]);
    expect(tex_own[1] > .9f && tex_own[0] < .1f && tex_early[1] > .9f && tex_early[0] < .1f,
           "a published replacement is not sampled before the next Present");
    expect(tex_swapped[0] > .9f && tex_swapped[1] > .9f && tex_swapped[2] > .9f,
           "after the Present the draw samples the replacement without a new bind by the game");
    expect(seen.Get() == mip_view.Get() && textures_on.returned_original > textures_before.returned_original,
           "the game reads back its own view while the replacement is bound");
    expect(tex_off[1] > .9f && tex_off[0] < .1f && tex_on[0] > .9f && tex_on[2] > .9f,
           "the switch restores the game's texture and brings the replacement back, live");
    expect(textures_on.replaced == 1 && textures_on.replaced_bytes == uint64_t(64) * 64 * 4,
           "the status counts the visible replacement and its memory");
    expect(tex_removed[1] > .9f && tex_removed[0] < .1f && textures_after.replaced == 0 && textures_after.replaced_bytes == 0,
           "a withdrawn replacement leaves the game's texture and no accounted memory");
    // A second version published over a visible one, then removal, all without a bind by the game
    // (Astra's sequence, COMMS f55ee3e3): pixels and the game-facing view at each step.
    std::vector<uint32_t> blue(size_t(64) * 64, 0xffff0000u);
    D3D11_SUBRESOURCE_DATA blue_data{blue.data(), 64 * 4, 0};
    ComPtr<ID3D11Texture2D> marker_b;
    ComPtr<ID3D11ShaderResourceView> marker_b_view;
    expect(SUCCEEDED(g.device->CreateTexture2D(&marker_desc, &blue_data, &marker_b)) &&
           SUCCEEDED(g.device->CreateShaderResourceView(marker_b.Get(), nullptr, &marker_b_view)),
           "create the second marker texture");
    float ver_a[4]{}, ver_b[4]{}, ver_none[4]{};
    ComPtr<ID3D11ShaderResourceView> seen_a, seen_b, seen_none;
    na::native11::PublishTexture(mip_view.Get(), marker_view.Get(), 1);
    na::native11::TexturesOnPresent();
    g.ctx->Draw(4, 0); ReadCopy(g, ver_a, g.color.Get());
    g.ctx->PSGetShaderResources(0, 1, &seen_a);
    na::native11::PublishTexture(mip_view.Get(), marker_b_view.Get(), 1);
    na::native11::TexturesOnPresent();
    g.ctx->Draw(4, 0); ReadCopy(g, ver_b, g.color.Get());
    g.ctx->PSGetShaderResources(0, 1, &seen_b);
    na::native11::RemoveTexture(mip_view.Get());
    na::native11::TexturesOnPresent();
    g.ctx->PSGetShaderResources(0, 1, &seen_none); // before any draw has repaired the slot
    g.ctx->Draw(4, 0); ReadCopy(g, ver_none, g.color.Get());
    std::printf("native texture versions (r/g/b): A %.2f/%.2f/%.2f, B %.2f/%.2f/%.2f, removed %.2f/%.2f/%.2f\n", ver_a[0],
                ver_a[1], ver_a[2], ver_b[0], ver_b[1], ver_b[2], ver_none[0], ver_none[1], ver_none[2]);
    expect(ver_a[0] > .9f && ver_a[1] > .9f && ver_b[2] > .9f && ver_b[0] < .1f && ver_b[1] < .1f,
           "a newer version replaces a visible one without a new bind by the game");
    expect(ver_none[1] > .9f && ver_none[0] < .1f && ver_none[2] < .1f,
           "removal after two versions restores the game's texture without a new bind");
    expect(seen_a.Get() == mip_view.Get() && seen_b.Get() == mip_view.Get() && seen_none.Get() == mip_view.Get(),
           "the game reads back its own view under each version and after removal");
    seen_a.Reset(); seen_b.Reset(); seen_none.Reset();
    // The game may drop its last reference to a view it has bound: D3D keeps a bound view alive, and
    // so must the swap while a replacement sits in its place.
    ComPtr<ID3D11Texture2D> second;
    ComPtr<ID3D11ShaderResourceView> second_view;
    expect(SUCCEEDED(g.device->CreateTexture2D(&td, initial, &second)) &&
           SUCCEEDED(g.device->CreateShaderResourceView(second.Get(), nullptr, &second_view)),
           "create a second game texture");
    ID3D11ShaderResourceView* second_raw = second_view.Get();
    g.ctx->PSSetShaderResources(0, 1, &second_raw);
    na::native11::PublishTexture(second_raw, marker_view.Get(), 1);
    na::native11::TexturesOnPresent();
    float gone_on[4]{}, gone_off[4]{};
    g.ctx->Draw(4, 0); ReadCopy(g, gone_on, g.color.Get());
    const uint64_t retired_bound = na::native11::GetTextureStats().retired;
    second_view.Reset(); second.Reset(); // the game's last references; the slot still binds it
    ComPtr<ID3D11ShaderResourceView> seen_gone;
    g.ctx->PSGetShaderResources(0, 1, &seen_gone);
    const bool gone_identity = seen_gone.Get() == second_raw;
    seen_gone.Reset();
    na::native11::SetTexturesEnabled(false);
    g.ctx->Draw(4, 0); ReadCopy(g, gone_off, g.color.Get());
    na::native11::SetTexturesEnabled(true);
    g.ctx->Draw(4, 0);
    const uint64_t retired_held = na::native11::GetTextureStats().retired;
    ID3D11ShaderResourceView* nothing = nullptr;
    g.ctx->PSSetShaderResources(0, 1, &nothing); // the real unbind: now the view may die
    g.ctx->Flush(); // D3D11 destroys released objects lazily; a flush completes it
    const uint64_t retired_unbound = na::native11::GetTextureStats().retired;
    std::printf("native texture released-while-bound (r/g/b): replaced %.2f/%.2f/%.2f, off %.2f/%.2f/%.2f, retired %llu -> %llu -> %llu\n",
                gone_on[0], gone_on[1], gone_on[2], gone_off[0], gone_off[1], gone_off[2],
                static_cast<unsigned long long>(retired_bound), static_cast<unsigned long long>(retired_held),
                static_cast<unsigned long long>(retired_unbound));
    expect(gone_on[0] > .9f && gone_on[2] > .9f && gone_identity && gone_off[1] > .9f && gone_off[0] < .1f,
           "a bound view the game has released is still read back and restored while it is bound");
    expect(retired_held == retired_bound && retired_unbound == retired_bound + 1,
           "that view lives exactly until the game's slot lets go of it");
    // A deferred recording made while a replacement is visible samples it when executed.
    g.ctx->PSSetShaderResources(0, 1, &texture_view);
    na::native11::PublishTexture(mip_view.Get(), marker_view.Get(), 1);
    na::native11::TexturesOnPresent();
    ComPtr<ID3D11DeviceContext> texture_deferred;
    g.device->CreateDeferredContext(0, &texture_deferred);
    if (texture_deferred) {
        auto* dc = texture_deferred.Get();
        dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        dc->RSSetState(no_cull.Get()); dc->OMSetRenderTargets(1, &rt, nullptr);
        dc->VSSetShader(g.vs_post.Get(), nullptr, 0); dc->PSSetShader(mip_shader.Get(), nullptr, 0);
        dc->PSSetShaderResources(0, 1, &texture_view); material_raw = material.Get();
        dc->PSSetSamplers(0, 1, &material_raw); Viewport(dc, g.w, g.h);
        ComPtr<ID3D11ShaderResourceView> seen_deferred;
        dc->PSGetShaderResources(0, 1, &seen_deferred);
        dc->Draw(4, 0);
        ComPtr<ID3D11CommandList> recorded;
        float deferred_tex[4]{};
        expect(SUCCEEDED(dc->FinishCommandList(FALSE, &recorded)), "record a draw with a replaced texture");
        if (recorded) g.ctx->ExecuteCommandList(recorded.Get(), TRUE);
        ReadCopy(g, deferred_tex, g.color.Get());
        expect(deferred_tex[0] > .9f && deferred_tex[1] > .9f && deferred_tex[2] > .9f && seen_deferred.Get() == mip_view.Get() &&
               na::native11::GetTextureStats().swapped_deferred > 0,
               "a deferred recording samples the replacement and reads back the game's view");
    } else expect(false, "create deferred texture context");
    na::native11::RemoveTexture(mip_view.Get());
    na::native11::TexturesOnPresent();
    // Source dump: the texture the game binds in its scene is written as stored, under the name the
    // shared identity gives its bytes (the digest below is from tools/material_baseline/texsub.py
    // for this 64x64 RGBA8 chain: red, green, then blue levels).
    {
        wchar_t dump_temp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, dump_temp);
        const std::wstring dump_dir = std::wstring(dump_temp) + L"na-texture-dump-" + std::to_wstring(GetCurrentProcessId());
        const std::wstring dump_file = dump_dir + L"\\2377209502e7d867be939510f289f7a973418330e52ba7181ec5b703ada80598.dds";
        const std::wstring dump_index = dump_dir + L"\\index.jsonl";
        DeleteFileW(dump_file.c_str()); DeleteFileW(dump_index.c_str());
        const auto dump_before = na::native11::GetTextureStats();
        expect(na::native11::RequestTextureDump(dump_dir.c_str()) && !na::native11::RequestTextureDump(dump_dir.c_str()),
               "a source dump starts, and a second request waits for the first");
        g.ctx->PSSetShaderResources(0, 1, &texture_view);
        g.ctx->Draw(4, 0);
        // The game's Present submits the queued copy; here a flush stands in for it. The copy is read
        // back without waiting, and the file is written on the dump's own thread.
        for (int i = 0; i < 1500 && na::native11::GetTextureStats().dump_active; ++i) {
            na::native11::TexturesOnPresent();
            g.ctx->Flush();
            Sleep(2);
        }
        const auto dump_after = na::native11::GetTextureStats();
        WIN32_FILE_ATTRIBUTE_DATA dump_written_file{}, dump_index_file{};
        const bool have_file = GetFileAttributesExW(dump_file.c_str(), GetFileExInfoStandard, &dump_written_file) != 0;
        const bool have_index = GetFileAttributesExW(dump_index.c_str(), GetFileExInfoStandard, &dump_index_file) != 0;
        std::printf("native texture dump: active %d, written %llu, skipped %llu, file %d (%lu bytes), index %d (%lu bytes)\n",
                    dump_after.dump_active ? 1 : 0, static_cast<unsigned long long>(dump_after.dump_written - dump_before.dump_written),
                    static_cast<unsigned long long>(dump_after.dump_failed - dump_before.dump_failed), have_file ? 1 : 0,
                    dump_written_file.nFileSizeLow, have_index ? 1 : 0, dump_index_file.nFileSizeLow);
        expect(!dump_after.dump_active && dump_after.dump_written == dump_before.dump_written + 1 &&
               dump_after.dump_failed == dump_before.dump_failed,
               "the dump finishes with the one scene texture written and none skipped");
        expect(have_file && dump_written_file.nFileSizeLow == 148 + 21844,
               "the file is named by the texture-subresources-v1 digest of the game's bytes and holds every level");
        expect(have_index && dump_index_file.nFileSizeLow > 0, "the dump's index lists it");
        DeleteFileW(dump_file.c_str()); DeleteFileW(dump_index.c_str()); RemoveDirectoryW(dump_dir.c_str());
    }
    // Material cache: a game texture whose creation bytes are a prepared source gets the cache's map,
    // found by digest (no read-back), created on the loader's thread and shown with no bind by the game.
    // The game's texture is BC1 sRGB, every block green; the prepared map is BC7 sRGB, every block white.
    {
        const UINT mat_side = 64, mat_mips = 7;
        std::vector<uint8_t> mat_game, mat_other, mat_prepared;
        std::vector<D3D11_SUBRESOURCE_DATA> mat_levels(mat_mips), mat_other_levels(mat_mips);
        std::vector<size_t> mat_game_at, mat_prepared_at;
        static const uint8_t mat_green[8] = {0xE0, 0x07, 0xE0, 0x07, 0, 0, 0, 0};
        static const uint8_t mat_white[16] = {0xC0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0, 0, 0, 0, 0, 0, 0}; // mode 6, endpoints 255
        for (UINT mat_mip = 0; mat_mip < mat_mips; ++mat_mip) {
            const UINT mat_blocks = ((std::max)(1u, mat_side >> mat_mip) + 3) / 4;
            mat_game_at.push_back(mat_game.size());
            mat_prepared_at.push_back(mat_prepared.size());
            for (UINT mat_b = 0; mat_b < mat_blocks * mat_blocks; ++mat_b) {
                mat_game.insert(mat_game.end(), mat_green, mat_green + 8);
                mat_prepared.insert(mat_prepared.end(), mat_white, mat_white + 16);
            }
        }
        mat_other = mat_game;
        mat_other[0] = 0x1F; mat_other[1] = 0x00; mat_other[2] = 0x1F; mat_other[3] = 0x00; // one block differs: another source
        for (UINT mat_mip = 0; mat_mip < mat_mips; ++mat_mip) {
            const UINT mat_blocks = ((std::max)(1u, mat_side >> mat_mip) + 3) / 4;
            mat_levels[mat_mip] = {mat_game.data() + mat_game_at[mat_mip], mat_blocks * 8, 0};
            mat_other_levels[mat_mip] = {mat_other.data() + mat_game_at[mat_mip], mat_blocks * 8, 0};
        }
        // The identity the preparer would have recorded for the game's texture.
        na::materials::TexSubHeader mat_header;
        mat_header.resource_format = mat_header.view_format = 72;
        mat_header.width = mat_header.height = mat_side;
        mat_header.mip_count = mat_mips;
        na::materials::TextureSubresourcesV1 mat_digest(mat_header);
        for (UINT mat_mip = 0; mat_mip < mat_mips; ++mat_mip)
            mat_digest.AddLevel(0, mat_mip, mat_levels[mat_mip].pSysMem, mat_levels[mat_mip].SysMemPitch);
        const std::string mat_source = mat_digest.Finish();
        // The cache entry: one base_color map, written the way the preparer lays a DDS out.
        std::vector<uint8_t> mat_file(148, 0);
        const auto mat_put = [&](size_t mat_at, uint32_t mat_v) {
            for (int mat_k = 0; mat_k < 4; ++mat_k) mat_file[mat_at + static_cast<size_t>(mat_k)] = static_cast<uint8_t>(mat_v >> (8 * mat_k));
        };
        std::memcpy(mat_file.data(), "DDS ", 4);
        mat_put(4, 124); mat_put(8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000 | 0x20000); mat_put(12, mat_side); mat_put(16, mat_side);
        mat_put(20, 16 * 16 * 16); mat_put(28, mat_mips); mat_put(76, 32); mat_put(80, 4);
        std::memcpy(mat_file.data() + 84, "DX10", 4);
        mat_put(108, 0x1000 | 0x8 | 0x400000); mat_put(128, 99); mat_put(132, 3); mat_put(140, 1); mat_put(144, 1);
        mat_file.insert(mat_file.end(), mat_prepared.begin(), mat_prepared.end());
        na::materials::Sha256 mat_sha;
        mat_sha.Update(mat_file.data(), mat_file.size());
        std::string mat_file_sha;
        for (uint8_t mat_byte : mat_sha.Final()) {
            char mat_hex[3];
            std::snprintf(mat_hex, sizeof(mat_hex), "%02x", mat_byte);
            mat_file_sha += mat_hex;
        }
        const std::string mat_key(64, 'e'); // a fixture's key; the runtime does not re-derive it from a recipe
        std::string mat_manifest =
            "{\"schema_version\":1,\"material_id\":\"fixture_green\",\"entry_key\":\"" + mat_key +
            "\",\"algorithm\":\"fixture\",\"algorithm_version\":\"1\",\"settings\":{},\"seed\":0,\"fixture\":true,\"recipe\":{},"
            "\"maps\":[{\"id\":\"base_color\",\"role\":\"base_color\",\"source\":{\"content_sha256\":\"" + mat_source +
            "\",\"identity_domain\":\"texture-subresources-v1\",\"width\":64,\"height\":64,\"first_mip\":0,\"mip_count\":7,"
            "\"view_format\":72,\"complete_observed_range\":true,\"resource_format\":72,\"first_array_slice\":0,\"array_size\":1},"
            "\"channels\":[\"color_r\",\"color_g\",\"color_b\",\"opacity\"],\"encoding\":\"srgb\",\"address\":\"wrap\","
            "\"dxgi_format\":99,\"width\":64,\"height\":64,\"file\":\"base_color.dds\",\"sha256\":\"" + mat_file_sha + "\",\"mips\":[";
        for (UINT mat_mip = 0; mat_mip < mat_mips; ++mat_mip) {
            const UINT mat_extent = (std::max)(1u, mat_side >> mat_mip), mat_blocks = (mat_extent + 3) / 4;
            char mat_line[192];
            std::snprintf(mat_line, sizeof(mat_line), "%s{\"level\":%u,\"width\":%u,\"height\":%u,\"offset\":%zu,\"row_pitch\":%u,\"byte_size\":%u}",
                          mat_mip ? "," : "", mat_mip, mat_extent, mat_extent, size_t(148) + mat_prepared_at[mat_mip], mat_blocks * 16,
                          mat_blocks * mat_blocks * 16);
            mat_manifest += mat_line;
        }
        mat_manifest += "]}],\"payload_bytes\":" + std::to_string(mat_prepared.size()) + "}";
        wchar_t mat_temp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, mat_temp);
        const std::wstring mat_root = std::wstring(mat_temp) + L"na-material-cache-" + std::to_wstring(GetCurrentProcessId());
        const std::wstring mat_entry = mat_root + L"\\" + std::wstring(mat_key.begin(), mat_key.end());
        CreateDirectoryW(mat_root.c_str(), nullptr);
        CreateDirectoryW(mat_entry.c_str(), nullptr);
        const auto mat_write = [](const std::wstring& mat_path, const void* mat_data, size_t mat_size) {
            FILE* mat_f = nullptr;
            if (_wfopen_s(&mat_f, mat_path.c_str(), L"wb") || !mat_f) return false;
            const bool mat_ok = std::fwrite(mat_data, 1, mat_size, mat_f) == mat_size;
            return std::fclose(mat_f) == 0 && mat_ok;
        };
        expect(mat_write(mat_entry + L"\\base_color.dds", mat_file.data(), mat_file.size()) &&
               mat_write(mat_entry + L"\\manifest.json", mat_manifest.data(), mat_manifest.size()),
               "write the fixture cache entry");
        const size_t mat_read = na::native11::LoadMaterialCache(mat_root.c_str());
        const auto mat_before = na::native11::GetMaterialStats();
        expect(mat_read == 1 && mat_before.entries == 1 && mat_before.findable_maps == 1 && mat_before.refused_maps == 0,
               "the cache entry is read and its map is findable");
        // The game now creates its textures: one the prepared source, one of the same shape with other bytes.
        D3D11_TEXTURE2D_DESC mat_desc{};
        mat_desc.Width = mat_desc.Height = mat_side; mat_desc.MipLevels = mat_mips; mat_desc.ArraySize = 1;
        mat_desc.Format = DXGI_FORMAT_BC1_UNORM_SRGB; mat_desc.SampleDesc.Count = 1;
        mat_desc.Usage = D3D11_USAGE_IMMUTABLE; mat_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> mat_texture, mat_other_texture;
        ComPtr<ID3D11ShaderResourceView> mat_view, mat_other_view;
        expect(SUCCEEDED(g.device->CreateTexture2D(&mat_desc, mat_levels.data(), &mat_texture)) &&
               SUCCEEDED(g.device->CreateShaderResourceView(mat_texture.Get(), nullptr, &mat_view)) &&
               SUCCEEDED(g.device->CreateTexture2D(&mat_desc, mat_other_levels.data(), &mat_other_texture)) &&
               SUCCEEDED(g.device->CreateShaderResourceView(mat_other_texture.Get(), nullptr, &mat_other_view)),
               "the game creates its block-compressed textures");
        ID3D11ShaderResourceView* mat_raw = mat_view.Get();
        g.ctx->PSSetShaderResources(0, 1, &mat_raw);
        float mat_own[4]{}, mat_shown[4]{}, mat_unprepared[4]{};
        g.ctx->Draw(4, 0); ReadCopy(g, mat_own, g.color.Get());
        // The digest, the file read and the texture creation happen on the loader's thread; the
        // entry becomes visible at a Present after they finish.
        // Waits for what the checks below read: both digests done, and the publication visible.
        for (int mat_i = 0; mat_i < 2500 && na::native11::GetMaterialStats().maps_failed == 0 &&
                            (na::native11::GetMaterialStats().textures_hashed < 2 || na::native11::GetTextureStats().replaced == 0); ++mat_i) {
            na::native11::TexturesOnPresent();
            Sleep(2);
        }
        g.ctx->Draw(4, 0); ReadCopy(g, mat_shown, g.color.Get());
        ComPtr<ID3D11ShaderResourceView> mat_seen;
        g.ctx->PSGetShaderResources(0, 1, &mat_seen);
        ID3D11ShaderResourceView* mat_other_raw = mat_other_view.Get();
        g.ctx->PSSetShaderResources(0, 1, &mat_other_raw);
        g.ctx->Draw(4, 0); ReadCopy(g, mat_unprepared, g.color.Get());
        const auto mat_after = na::native11::GetMaterialStats();
        std::printf("native material cache (r/g/b): own %.2f/%.2f/%.2f, shown %.2f/%.2f/%.2f, other texture %.2f/%.2f/%.2f; "
                    "hashed %llu, matched %llu, loaded %llu, failed %llu, shown entries %llu, bytes %llu\n",
                    mat_own[0], mat_own[1], mat_own[2], mat_shown[0], mat_shown[1], mat_shown[2], mat_unprepared[0], mat_unprepared[1],
                    mat_unprepared[2], static_cast<unsigned long long>(mat_after.textures_hashed),
                    static_cast<unsigned long long>(mat_after.views_matched), static_cast<unsigned long long>(mat_after.maps_loaded),
                    static_cast<unsigned long long>(mat_after.maps_failed), static_cast<unsigned long long>(mat_after.entries_shown),
                    static_cast<unsigned long long>(mat_after.loaded_bytes));
        expect(mat_own[0] < .1f && mat_own[1] > .9f && mat_own[2] < .1f, "before the entry is shown the game's own texture is sampled");
        expect(mat_after.textures_hashed == 2 && mat_after.views_matched == 1 && mat_after.maps_loaded == 1 && mat_after.maps_failed == 0 &&
               mat_after.entries_shown == 1 && mat_after.loaded_bytes == mat_file.size(),
               "both textures of the prepared shape are digested from creation data; only the prepared source matches and loads");
        expect(mat_shown[0] > .9f && mat_shown[1] > .9f && mat_shown[2] > .9f && mat_seen.Get() == mat_view.Get(),
               "the prepared map is sampled with no new bind, and the game reads back its own view");
        expect(mat_unprepared[0] < .1f && mat_unprepared[1] > .9f && mat_unprepared[2] < .1f,
               "a texture of the same shape with other bytes keeps its own content");
        na::native11::RemoveTexture(mat_view.Get());
        na::native11::TexturesOnPresent();
        DeleteFileW((mat_entry + L"\\base_color.dds").c_str()); DeleteFileW((mat_entry + L"\\manifest.json").c_str());
        RemoveDirectoryW(mat_entry.c_str()); RemoveDirectoryW(mat_root.c_str());
        na::native11::LoadMaterialCache(mat_root.c_str()); // the folder is gone: the loader is idle again
    }
    na::capture::j2::Shutdown();
    na::capture::Shutdown();
    DeleteFileW(ini.c_str());
#endif

    std::printf(g_failed ? "FAIL: %d check(s) failed\n" : "PASS: native DX11 provider on a synthetic game\n", g_failed);
    std::fflush(stdout);
    // The hooks stay installed for the process lifetime (as in a game); exit directly.
    ExitProcess(g_failed ? 1u : 0u);
}
