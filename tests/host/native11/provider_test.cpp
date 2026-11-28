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
#include "core/first_setup.hpp"
#include "host_dx12/native11/camera_pick.hpp"
#include "host_dx12/native11/input_provider.hpp"
#include "host_dx12/native11/viewport_jitter.hpp"
#include "host_dx12/diag/event_log.hpp"
#include "host_dx12/diag/vram_ledger.hpp"
#if defined(NA_TEST_NATIVE_DLAA)
#include "host_dx12/adapters/wwii/scene_camera.hpp"
#include "host_dx12/capture/frame_capture.hpp"
#include "host_dx12/capture/j2_capture.hpp"
#include "host_dx12/capture/referee_gpu.hpp"
#include "core/referee_recovery.hpp"
#include "core/referee_placement.hpp"
#include "core/anisotropic_auto.hpp"
#include "host_dx12/native_d3d11/live_dlaa.hpp"
#include "host_dx12/native11/draw_replay.hpp"
#include "host_dx12/j2/live_dlaa.hpp"
#include "host_dx12/j2/compute_flow.hpp"
#include "host_dx12/j2/j2_flow.hpp"
#include "host_dx12/effects/xegtao.hpp"
#include "host_dx12/effects/donut.hpp"
#include "host_dx12/host_boot/host_boot.hpp"
#include "host_dx12/host_boot/process_log.hpp"
#include "host_dx12/on12/d3d11_on12.hpp"
#include "host_dx12/overlay/overlay.hpp"
#include "host_dx12/overlay/presenter.hpp"
#include "host_dx12/fg/frame_gen.hpp"
#include "core/live_frame_gen.hpp"
#include "host_dx12/settings.hpp"
#include "core/fold_shading.hpp"
#include "host_dx12/dlss/dlss_backend.hpp"
#include "host_dx12/diag/release_trace.hpp"
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
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
// The names fixture's map: a textured map panned by offset.z pixels, with a unit at x = offset.w moving against the pan.
float4 ps_map(float4 p : SV_Position) : SV_Target {
    if (p.x >= offset.w && p.x < offset.w + 64.0 && p.y >= 600.0 && p.y < 640.0) return float4(0.7, 0.08, 0.06, 1.0);
    float x = p.x + offset.z, y = p.y;
    float v = 0.12 + 0.06 * sin(x * 0.21) * cos(y * 0.17) + 0.04 * sin((x + y) * 0.05);
    if (fmod(abs(x * 0.7 + y * 0.3), 40.0) < 1.6) v += 0.1;
    return float4(v * 0.9, v * 1.1, v * 0.8, 1.0);
}
float4 ps_scene_variant(float4 p : SV_Position) : SV_Target { return float4(0.3, 0.6, 0.25, 1.0); }
float4 ps_tonemap(float4 p : SV_Position) : SV_Target { return scene.Load(int3(int2(p.xy), 0)); }
float4 ps_letterbox(float4 p : SV_Position) : SV_Target { return scene.Load(int3(int2(p.xy) - int2(0,2), 0)); }
float4 ps_centered_scene(float4 p : SV_Position) : SV_Target { return scene.Load(int3(int2(p.xy) - int2(44,0), 0)); }
float4 ps_hud(float4 p : SV_Position) : SV_Target { return float4(1, 1, 1, 0.5); }
// The fold fixture: shading that depends on the pixel (a constant colour is not counted as pixel shading
// by the driver's statistics, and a pattern shows where blocks were shaded once).
// Each invocation also counts itself (the driver's own statistics leave these draws out).
RWByteAddressBuffer shading_count : register(u1);
float4 ps_scene_coords(float4 p : SV_Position) : SV_Target { shading_count.InterlockedAdd(0, 1); return float4(frac(p.x / 16.0), frac(p.y / 16.0), 0.5, 1.0); }
float4 ps_hud_coords(float4 p : SV_Position) : SV_Target { shading_count.InterlockedAdd(4, 1); return float4(frac(p.x / 8.0), 1, 1, 0.5); }
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
float4 vs_shader_depth_mesh(uint id : SV_VertexID) : SV_Position { return vs_depth_ramp(id % 3); }
float ps_write_shader_depth(float4 p : SV_Position) : SV_Target { return p.z; }
float4 ps_test_shader_depth(float4 p : SV_Position) : SV_Target {
    float same = abs(test_depth.Load(int3(uint2(p.xy), 0)) - p.z) < 0.0001 ? 1 : 0;
    return float4(same, same, same, 1);
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
// The overlay fixture (Hearts of Iron IV's frame shape, 2026-11-27): thin lines and small labels drawn into
// the scene picture with the depth test off and the scene depth bound, over depth-tested terrain.
cbuffer OverlayCb : register(b2) { float4 overlay_size; }; // the picture's width and height in pixels
uint quad_corner(uint id) { const uint corner = id % 6; return corner < 3 ? corner : corner == 3 ? 2 : corner == 4 ? 1 : 3; }
float4 vs_overlay_lines(uint id : SV_VertexID) : SV_Position {
    const uint line_index = id / 6, c = quad_corner(id);
    const float along = (c & 1) ? 1.0 : 0.0, across = (c & 2) ? 0.5 : -0.5;
    const float angle = 0.30 + 0.093 * line_index;
    const float2 dir = float2(cos(angle), sin(angle));
    const float2 nrm = float2(-dir.y, dir.x) * 1.4 / overlay_size.xy; // 0.7 pixel wide
    const float2 start = float2(-0.95 + 0.11 * line_index, -0.85);
    return float4(start + dir * along * 1.9 + nrm * across * 2.0, 0.5, 1.0);
}
float4 vs_overlay_text(uint id : SV_VertexID) : SV_Position {
    const uint glyph = id / 6, c = quad_corner(id);
    const float2 cell = float2(glyph % 48, glyph / 48);
    const float2 px = float2(64.3, 120.6) + cell * float2(5.0, 7.0) + float2((c & 1) ? 2.0 : 0.0, (c & 2) ? 3.0 : 0.0);
    return float4(px.x / overlay_size.x * 2 - 1, 1 - px.y / overlay_size.y * 2, 0.5, 1.0);
}
// A panel drawn before the hand-over with a scissor smaller than the scene: small blocks, clipped by the scissor.
float4 vs_overlay_panel(uint id : SV_VertexID) : SV_Position {
    const uint block = id / 6, c = quad_corner(id);
    const float2 cell = float2(block % 30, block / 30);
    const float2 px = float2(1060.3, 20.6) + cell * float2(6.0, 6.0) + float2((c & 1) ? 4.0 : 0.0, (c & 2) ? 4.0 : 0.0);
    return float4(px.x / overlay_size.x * 2 - 1, 1 - px.y / overlay_size.y * 2, 0.5, 1.0);
}
float4 ps_overlay_panel(float4 p : SV_Position) : SV_Target { return float4(0.0, 1.0, 0.0, 1.0); }
float4 ps_overlay_line(float4 p : SV_Position) : SV_Target { return float4(1.0, 0.85, 0.2, 1.0); }
float4 ps_overlay_text(float4 p : SV_Position) : SV_Target { return float4(0.95, 0.95, 0.95, 1.0); }
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
    UINT screen_extra_width = 0;
    bool padded_hdr = false;
    bool shader_depth = false;
    DXGI_FORMAT back_format = DXGI_FORMAT_R8G8B8A8_UNORM; // the swap chain's format
    ComPtr<ID3D11RasterizerState> hud_scissor;            // set: the HUD covers only kHudRect
    ID3D11Query* main_stats = nullptr;                    // set: pipeline statistics around the main scene's draws only
    ComPtr<ID3D11PixelShader> ps_scene_main, ps_hud_main;
    // The x2 names fixture (--fg-x2-names*): a map panned `pan` pixels a frame and names drawn after the hand-over
    // that move with it (and the HUD panel that does not).
    ComPtr<ID3D11PixelShader> ps_map;
    ComPtr<ID3D11RasterizerState> names_rs; // no culling, no scissor: the full-screen passes and the names are drawn
    bool names = false;
    float pan = 0;
    int frame_now = 0;  // set: the main scene's and the HUD's shading (fold fixture)
    ComPtr<ID3D11UnorderedAccessView> count_uav;           // set: their invocations, counted (u1)
    ComPtr<ID3D11RasterizerState> scene_rs;                // set: the main scene drawn with no face culling (its quads face away)
    // The interface recorded on a deferred context and run by the immediate one (multithreaded DX11):
    // 1 = the whole interface, 2 = part of it, between the scene and the immediate interface draws.
    int deferred_hud = 0;
    ComPtr<ID3D11DeviceContext> hud_dc;
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
    // The overlay fixture: lines and labels into the scene picture with the depth test off, scene depth bound.
    bool overlays = false;
    static constexpr UINT kOverlayLines = 16, kOverlayGlyphs = 48 * 6, kPanelBlocks = 30 * 12;
    static constexpr LONG kPanelScissor[4] = {1080, 30, 1210, 80}; // left, top, right, bottom: smaller than the panel
    ComPtr<ID3D11VertexShader> vs_lines, vs_text, vs_panel;
    ComPtr<ID3D11PixelShader> ps_line, ps_text, ps_panel;
    ComPtr<ID3D11Buffer> overlay_cb;
    ComPtr<ID3D11DepthStencilState> overlay_depth_off;
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
    sd.BufferDesc.Width = g.w + g.screen_extra_width;
    sd.BufferDesc.Height = g.h + (g.padded_hdr ? 3 : 0);
    sd.BufferDesc.Format = g.back_format;
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


void Scene(ID3D11DeviceContext* c, Game& g, int frame, bool no_pose = false, bool replay_mesh = false)
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
    if (g.names && g.ps_map) { // the names fixture: one depth-tested map draw (the objects after it fail the depth test)
        g.frame_now = frame;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(c->Map(g.obj_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            const float o[4] = {0, 0, g.pan * float(frame), 200.f + 3.f * float(frame % 300)};
            std::memcpy(m.pData, o, 16);
            c->Unmap(g.obj_cb.Get(), 0);
        }
        ID3D11Buffer* ob = g.obj_cb.Get();
        c->PSSetConstantBuffers(1, 1, &ob);
        c->VSSetShader(g.vs_post.Get(), nullptr, 0);
        c->PSSetShader(g.ps_map.Get(), nullptr, 0);
        c->RSSetState(g.names_rs.Get());
        c->Draw(4, 0);
        c->VSSetShader(g.vs.Get(), nullptr, 0);
        c->PSSetShader(g.ps_scene.Get(), nullptr, 0);
    }
    if (g.campaign_split_camera) BindSplitCamera(c, g);
    if (g.ps_scene_main) c->PSSetShader(g.ps_scene_main.Get(), nullptr, 0);
    if (g.scene_rs) c->RSSetState(g.scene_rs.Get());
    if (g.count_uav) {
        ID3D11UnorderedAccessView* u = g.count_uav.Get();
        c->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, nullptr, nullptr, 1, 1, &u, nullptr);
    }
    if (g.main_stats) c->Begin(g.main_stats);
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
        SetObject(c, g, i);
        if (replay_mesh && i == 39) {
            c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            c->DrawInstanced(3, 350, 0, 0); // 1,050 positions span two packed rows.
            c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        } else c->Draw(4, 0);
    }
    if (g.overlays) {
        // Map overlays: depth test off, the scene depth still bound, into the scene picture (Hearts of Iron IV).
        c->OMSetDepthStencilState(g.overlay_depth_off.Get(), 0);
        ID3D11Buffer* ocb = g.overlay_cb.Get();
        c->VSSetConstantBuffers(2, 1, &ocb);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c->VSSetShader(g.vs_lines.Get(), nullptr, 0);
        c->PSSetShader(g.ps_line.Get(), nullptr, 0);
        c->Draw(Game::kOverlayLines * 6, 0);
        c->VSSetShader(g.vs_text.Get(), nullptr, 0);
        c->PSSetShader(g.ps_text.Get(), nullptr, 0);
        c->Draw(Game::kOverlayGlyphs * 6, 0);
        // A panel with the whole scene's viewport and a smaller scissor, still before the hand-over.
        const D3D11_RECT clip{Game::kPanelScissor[0], Game::kPanelScissor[1], Game::kPanelScissor[2], Game::kPanelScissor[3]};
        c->RSSetState(g.hud_scissor.Get());
        c->RSSetScissorRects(1, &clip);
        c->VSSetShader(g.vs_panel.Get(), nullptr, 0);
        c->PSSetShader(g.ps_panel.Get(), nullptr, 0);
        c->Draw(Game::kPanelBlocks * 6, 0);
        c->RSSetState(g.scene_rs ? g.scene_rs.Get() : nullptr);
        c->OMSetDepthStencilState(nullptr, 0);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        c->VSSetShader(g.vs.Get(), nullptr, 0);
        c->PSSetShader(g.ps_scene_main ? g.ps_scene_main.Get() : g.ps_scene.Get(), nullptr, 0);
    }
    if (g.main_stats) c->End(g.main_stats);
    if (g.scene_rs) c->RSSetState(nullptr);
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



constexpr LONG kHudRect[4] = {40, 30, 200, 90}; // left, top, right, bottom (the live x2 fixture's HUD panel)
constexpr float kNameW = 48.f, kNameH = 12.f;
float NameY(int row) { return 150.f + 150.f * float(row) + (row == 3 ? 12.f : 0.f); } // row 3 lies over the unit's path
float NameX(const Game& g, int row, int i) { return 100.f + 160.f * float(i) + 37.f * float(row) - g.pan * float(g.frame_now); }
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
    if (g.names) c->RSSetState(g.names_rs.Get()); // the tonemap and the names (the HUD panel sets its own state)
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
    if (g.deferred_hud && g.hud_dc && immediate) {
        ID3D11DeviceContext* d = g.hud_dc.Get();
        d->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        d->VSSetShader(g.vs_post.Get(), nullptr, 0);
        d->OMSetRenderTargets(1, &bb, nullptr);
        d->OMSetBlendState(g.blend.Get(), bf, 0xffffffffu);
        d->PSSetShader(g.ps_hud.Get(), nullptr, 0);
        Viewport(d, g.w, g.h);
        if (g.hud_scissor) {
            d->RSSetState(g.hud_scissor.Get());
            const D3D11_RECT r{kHudRect[0], kHudRect[1], kHudRect[2], kHudRect[3]};
            d->RSSetScissorRects(1, &r);
        }
        for (int i = 0; i < 3; ++i) d->Draw(4, 0);
        ComPtr<ID3D11CommandList> list;
        if (SUCCEEDED(d->FinishCommandList(FALSE, &list))) c->ExecuteCommandList(list.Get(), TRUE);
        if (g.deferred_hud == 1) return;
    }
    c->OMSetBlendState(g.blend.Get(), bf, 0xffffffffu);
    c->PSSetShader(g.ps_hud_main ? g.ps_hud_main.Get() : g.ps_hud.Get(), nullptr, 0);
    if (g.count_uav) {
        ID3D11UnorderedAccessView* u = g.count_uav.Get();
        c->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, nullptr, nullptr, 1, 1, &u, nullptr);
    }
    if (g.hud_scissor) { // a HUD panel, not the whole screen (live x2 fixture)
        c->RSSetState(g.hud_scissor.Get());
        const D3D11_RECT r{kHudRect[0], kHudRect[1], kHudRect[2], kHudRect[3]};
        c->RSSetScissorRects(1, &r);
    }
    for (int i = 0; i < 6; ++i) c->Draw(4, 0);
    if (g.hud_scissor) c->RSSetState(nullptr);
    if (g.names) { // map names: interface drawn after the hand-over, anchored to the panning map
        c->RSSetState(g.names_rs.Get());
        c->PSSetShader(g.ps_hud.Get(), nullptr, 0);
        for (int row = 0; row < 4; ++row)
            for (int i = 0; i < 80; ++i) {
                const float x = NameX(g, row, i), y = NameY(row);
                if (x < -60.f || x > float(g.w) + 10.f) continue;
                const D3D11_VIEWPORT vp{x, y, kNameW, kNameH, 0.f, 1.f};
                c->RSSetViewports(1, &vp);
                c->Draw(4, 0);
            }
        Viewport(c, g.w, g.h);
    }
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
void ShaderDepthLayerFixture(Game& g, int& frame, ID3D11RasterizerState* raster)
{
    const auto setup = [](bool ok, const char* what) { expect(ok, what); return ok; };
    // The game's geometry uses an R32 render target as its depth texture. A
    // direct texel comparison produces black when the grids are split.
    const auto vs_code = Compile("vs_shader_depth_mesh", "vs_5_0");
    const auto producer_code = Compile("ps_write_shader_depth", "ps_5_0");
    const auto consumer_code = Compile("ps_test_shader_depth", "ps_5_0");
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> producer, consumer;
    if (!setup(vs_code && producer_code && consumer_code &&
        SUCCEEDED(g.device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs)) &&
        SUCCEEDED(g.device->CreatePixelShader(producer_code->GetBufferPointer(), producer_code->GetBufferSize(), nullptr, &producer)) &&
        SUCCEEDED(g.device->CreatePixelShader(consumer_code->GetBufferPointer(), consumer_code->GetBufferSize(), nullptr, &consumer)),
        "create shader-depth geometry")) return;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = g.w; desc.Height = g.h;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> depth_layer, unproved_copy, colours[2];
    ComPtr<ID3D11RenderTargetView> depth_rtv, colour_rtv[2];
    ComPtr<ID3D11ShaderResourceView> depth_srv, copy_srv;
    if (!setup(SUCCEEDED(g.device->CreateTexture2D(&desc, nullptr, &depth_layer)) &&
        SUCCEEDED(g.device->CreateTexture2D(&desc, nullptr, &unproved_copy)) &&
        SUCCEEDED(g.device->CreateRenderTargetView(depth_layer.Get(), nullptr, &depth_rtv)) &&
        SUCCEEDED(g.device->CreateShaderResourceView(depth_layer.Get(), nullptr, &depth_srv)) &&
        SUCCEEDED(g.device->CreateShaderResourceView(unproved_copy.Get(), nullptr, &copy_srv)),
        "create original and unproved depth-colour surfaces")) return;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    for (unsigned i = 0; i < 2; ++i)
        if (!setup(SUCCEEDED(g.device->CreateTexture2D(&desc, nullptr, &colours[i])) &&
            SUCCEEDED(g.device->CreateRenderTargetView(colours[i].Get(), nullptr, &colour_rtv[i])),
            "create material buffers")) return;
    const auto render = [&](float dx, float dy, bool copy, UINT vertices, bool selected = false, bool stationary_source = false) {
        na::native11::SetNextJitter(dx, dy);
        g.swap->Present(0, 0);
        auto* c = g.ctx.Get();
        Scene(c, g, frame++);
        c->ClearDepthStencilView(g.dsv.Get(), D3D11_CLEAR_DEPTH, 1.f, 0);
        auto* target = depth_rtv.Get();
        c->OMSetDepthStencilState(stationary_source ? g.depth_off.Get() : nullptr, 0);
        c->OMSetRenderTargets(1, &target, stationary_source ? nullptr : g.dsv.Get());
        Viewport(c, g.w, g.h);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c->RSSetState(raster);
        c->VSSetShader(vs.Get(), nullptr, 0);
        c->PSSetShader(producer.Get(), nullptr, 0);
        c->Draw(9, 0);
        ID3D11RenderTargetView* materials[]{selected ? g.color_rtv.Get() : colour_rtv[0].Get(), colour_rtv[1].Get()};
        c->OMSetRenderTargets(selected ? 1 : 2, materials, g.dsv.Get());
        if (copy) c->CopyResource(unproved_copy.Get(), depth_layer.Get());
        auto* source = copy ? copy_srv.Get() : depth_srv.Get();
        c->PSSetShaderResources(1, 1, &source);
        c->PSSetShader(consumer.Get(), nullptr, 0);
        c->OMSetDepthStencilState(g.depth_off.Get(), 0);
        c->Draw(vertices, 0);
        D3D11_VIEWPORT vp{};
        UINT count = 1;
        c->RSGetViewports(&count, &vp);
        const bool same_grid = count == 1 && std::fabs(vp.TopLeftX - (dx - .5f)) < 1.e-6f &&
            std::fabs(vp.TopLeftY - (dy - .5f)) < 1.e-6f && vp.Width == float(g.w) + 1;
        source = nullptr;
        c->PSSetShaderResources(1, 1, &source);
        c->OMSetRenderTargets(0, nullptr, nullptr);
        float pixel[4]{};
        const bool read = ReadCopy(g, pixel, selected ? g.color.Get() : colours[0].Get());
        c->OMSetDepthStencilState(nullptr, 0);
        Post(c, g, true);
        return std::pair{read ? pixel[0] : -1.f, same_grid};
    };
    const auto unrelated = render(.323165f, -.452396f, true, 9);
    const auto fullscreen = render(.323165f, -.452396f, false, 3);
    const auto negative = render(.323165f, -.452396f, false, 9);
    const auto positive = render(-.470734f, .419042f, false, 9);
    std::printf("shader-depth material pixels: unproved %.1f, fullscreen %.1f, matched %.1f/%.1f\n",
                unrelated.first, fullscreen.first, negative.first, positive.first);
    expect(unrelated.first == 0 && !unrelated.second, "an unrelated depth texture retains the original viewport");
    expect(fullscreen.first == 0 && !fullscreen.second, "a fullscreen reader retains the original viewport");
    expect(negative.first == 1 && negative.second && positive.first == 1 && positive.second,
           "full-strength offsets preserve material pixels when shader and depth share the raster");
    for (const bool enabled : {false, true}) {
        na::native11::ForceSceneOverlayJitterForTests(enabled ? 1 : 0);
        na::native11::SetSceneOverlayJitter(enabled, "source raster comparison");
        const auto a = render(.323165f, -.452396f, true, 9, true, false);
        const auto b = render(.323165f, -.452396f, true, 9, true, true);
        const auto c = render(-.470734f, .419042f, true, 9, true, true);
        std::printf("selected scene mesh, overlay=%d: sampled-source pixel=%.1f moved=%d; stationary-source pixels=%.1f/%.1f moved=%d/%d\n",
                    int(enabled), a.first, int(a.second), b.first, c.first, int(b.second), int(c.second));
        // Astra's reproducer (2026-11-26). VARIANT: jitter/stationary-source-readers: a reader of a layer drawn on
        // the game's grid (and of its exact copy) stays on that grid whatever the overlay rule says; a reader of a
        // layer drawn on the sample's grid moves with the overlay rule.
        expect(a.first == float(enabled) && b.first == 1.f && c.first == 1.f,
               "a reader of a stationary layer keeps its grid; a reader of a sampled layer moves with the overlays");
    }
    na::native11::ForceSceneOverlayJitterForTests(-1);
}

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
    if (!debug_layer) ShaderDepthLayerFixture(g, frame, raster.Get());
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

// A game sends a native-sized picture to the middle of a wider swap chain.
// The same source can feed an invisible target first; that read cannot authorize DLAA.
int CenteredSceneFixture(Game& game)
{
    D3D11_TEXTURE2D_DESC d{};
    game.color->GetDesc(&d);
    ComPtr<ID3D11Texture2D> decoy;
    ComPtr<ID3D11RenderTargetView> decoy_rtv;
    if (FAILED(game.device->CreateTexture2D(&d, nullptr, &decoy)) ||
        FAILED(game.device->CreateRenderTargetView(decoy.Get(), nullptr, &decoy_rtv))) return 1;
    const auto ps_code = Compile("ps_centered_scene", "ps_5_0");
    ComPtr<ID3D11PixelShader> ps;
    if (!ps_code || FAILED(game.device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &ps))) return 1;

    struct Consumer {
        Game& game;
        unsigned accepted = 0;
    } consumer{game};
    const auto evaluate = [](void* opaque, void* context, const na::native11::SceneFrame& f) {
        auto& state = *static_cast<Consumer*>(opaque);
        const auto& g = state.game;
        expect(f.screen_proof && f.color.d3d11_resource == g.color.Get() && f.depth.d3d11_resource == g.depth.Get() &&
               f.color.active_w == g.w && f.color.active_h == g.h && !f.color.active_x && !f.color.active_y,
               "the evaluator receives the exact native scene rectangle with a screen receipt");
        auto* ctx = static_cast<ID3D11DeviceContext*>(context);
        ComPtr<ID3D11RenderTargetView> target;
        ctx->OMGetRenderTargets(1, &target, nullptr);
        expect(target.Get() == g.back_rtv.Get(), "only the visible reader can enter the evaluator");
        const float replacement[]{.75f, .25f, .125f, 1.f};
        ctx->ClearRenderTargetView(g.color_rtv.Get(), replacement);
        ++state.accepted;
        return true;
    };
    if (!na::native11::Install(game.device.Get(), evaluate, &consumer)) return 1;
    na::native11::SetScreenCheck(true);
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> visible_quads;
    if (FAILED(game.device->CreateRasterizerState(&raster, &visible_quads))) return 1;
    game.ctx->RSSetState(visible_quads.Get());
    D3D11_TEXTURE2D_DESC pixel_desc{};
    pixel_desc.Width = pixel_desc.Height = pixel_desc.MipLevels = pixel_desc.ArraySize = 1;
    pixel_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    pixel_desc.SampleDesc.Count = 1;
    pixel_desc.Usage = D3D11_USAGE_STAGING;
    pixel_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> pixel;
    if (FAILED(game.device->CreateTexture2D(&pixel_desc, nullptr, &pixel))) return 1;

    auto frame = [&](bool show) {
        Pump();
        auto* ctx = game.ctx.Get();
        Scene(ctx, game, 0);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ctx->VSSetShader(game.vs_post.Get(), nullptr, 0);
        auto* rt = decoy_rtv.Get();
        ctx->OMSetRenderTargets(1, &rt, nullptr);
        auto* view = game.color_srv.Get();
        ctx->PSSetShaderResources(0, 1, &view);
        ctx->PSSetShader(game.ps_tonemap.Get(), nullptr, 0);
        ctx->Draw(4, 0);

        rt = game.back_rtv.Get();
        ctx->OMSetRenderTargets(1, &rt, nullptr);
        constexpr float black[]{0.f, 0.f, 0.f, 1.f};
        ctx->ClearRenderTargetView(rt, black);
        if (show) {
            const D3D11_VIEWPORT vp{44.f, 0.f, float(game.w), float(game.h), 0.f, 1.f};
            ctx->RSSetViewports(1, &vp);
            ctx->PSSetShaderResources(0, 1, &view);
            ctx->PSSetShader(ps.Get(), nullptr, 0);
            ctx->Draw(4, 0);
        }
        view = nullptr;
        ctx->PSSetShaderResources(0, 1, &view);
        bool displayed = false;
        if (show && consumer.accepted) {
            na::native11::OwnWorkScope own;
            ComPtr<ID3D11Texture2D> screen;
            if (SUCCEEDED(game.swap->GetBuffer(0, IID_PPV_ARGS(&screen)))) {
                const auto read_red = [&](UINT x) {
                    const D3D11_BOX box{x, 2, 0, x + 1, 3, 1};
                    ctx->CopySubresourceRegion(pixel.Get(), 0, 0, 0, 0, screen.Get(), 0, &box);
                    D3D11_MAPPED_SUBRESOURCE map{};
                    if (FAILED(ctx->Map(pixel.Get(), 0, D3D11_MAP_READ, 0, &map))) return -1;
                    const int red = *static_cast<const uint8_t*>(map.pData);
                    ctx->Unmap(pixel.Get(), 0);
                    return red;
                };
                displayed = std::abs(read_red(46) - 191) <= 2 && read_red(2) == 0 && read_red(game.w + 50) == 0;
            }
        }
        expect(SUCCEEDED(game.swap->Present(0, 0)), "present the centered scene");
        return displayed;
    };

    for (unsigned i = 0; i < 42; ++i) frame(false);
    const auto invisible = na::native11::Stats();
    expect(!consumer.accepted && invisible.screen_refuted >= 1 && !invisible.screen_confirmed,
           "an otherwise identical offscreen reader fails the centered screen challenge");
    unsigned visible = 0;
    for (unsigned i = 0; i < 46; ++i) visible += frame(true);
    const auto shown = na::native11::Stats();
    expect(shown.screen_confirmed == 1 && consumer.accepted >= 16 && visible == consumer.accepted,
           "the centered reader carries every accepted result to the display with the side bars intact");
    std::printf("centered scene: decoy rejected=%llu, screen confirmed=%llu, displayed=%u\n",
                (unsigned long long)shown.screen_refuted, (unsigned long long)shown.screen_confirmed, visible);
    na::native11::Uninstall();
    return g_failed ? 1 : 0;
}

int ScreenCheckFixture(Game& game, bool adaptive, bool missed_order = false, [[maybe_unused]] bool rotating_lights = false,
                       [[maybe_unused]] bool compare_routes = false, [[maybe_unused]] bool composition_fault = false,
                       [[maybe_unused]] bool reuse_depth = false, [[maybe_unused]] bool disagreeing_scene = false,
                       [[maybe_unused]] bool parity_roles = false)
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
        if (!sample.comparable)
            std::printf("noncomparable picture: edge agreement %.3f, quiet regions %u\n",
                        sample.screen_structure, sample.quiet_regions);
        return true;
    };
    static bool checked_decoy = false;
    checked_decoy = false;
    na::native11::SetParitySignal([](const void* swap, const na::referee::PlacementScope& scope) {
        const bool seen = na::referee::gpu::ParityRoleSwapSeen(swap, scope);
        if (seen && !checked_decoy) {
            // Same presentation, depth and generation as the actual pair,
            // but two different texture incarnations.
            auto decoy = scope;
            decoy.color = UINT64_MAX;
            decoy.peer_color = UINT64_MAX - 1;
            expect(!na::referee::gpu::ParityRoleSwapSeen(swap, decoy),
                   "an unrelated lighting pair sharing the same depth cannot borrow the screen signature");
            checked_decoy = true;
        }
        return seen;
    });
    if (!na::native11::Install(game.device.Get(), consume, &evaluator, compare_routes,
            composition_fault || disagreeing_scene ? recovery_evidence : compare_routes ? &na::referee::gpu::TakeTrialSample : nullptr)) return 1;
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
        const bool staged_flash = compare_routes && !disagreeing_scene;
        auto blob = Compile(staged_flash ? "ps_trial_light" : "ps_reflection", "ps_5_0");
        auto vertex = Compile("vs_reflection", "vs_5_0");
        ComPtr<ID3D11VertexShader> lit_vs;
        ComPtr<ID3D11PixelShader> reflection;
        if (!blob || FAILED(game.device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &reflection)) ||
            !vertex || FAILED(game.device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &lit_vs))) return 1;
        ComPtr<ID3D11PixelShader> trial_surface, trial_screen;
        ComPtr<ID3D11Buffer> trial_flash;
        if (staged_flash) {
            auto surface = Compile("ps_trial_surface", "ps_5_0");
            if (!surface || FAILED(game.device->CreatePixelShader(surface->GetBufferPointer(),
                surface->GetBufferSize(), nullptr, &trial_surface))) return 1;
            auto screen_shader = Compile("ps_trial_screen", "ps_5_0");
            const D3D11_BUFFER_DESC flags{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
            if (!screen_shader ||
                FAILED(game.device->CreatePixelShader(screen_shader->GetBufferPointer(), screen_shader->GetBufferSize(), nullptr, &trial_screen)) ||
                FAILED(game.device->CreateBuffer(&flags, nullptr, &trial_flash))) return 1;
        }
        na::native11::SetCameraUse(na::native11::CameraUse::Off);
        ComPtr<ID3D11Texture2D> back;
        if (FAILED(game.swap->GetBuffer(0, IID_PPV_ARGS(&back)))) return 1;
        struct Sample {
            int correct = 0, evaluated = 0, proven = 0, fixed_history = 0, displayed_readers = 0;
            size_t changed = 0, reached = 0, comparisons = 0, flash_pairs = 0;
            double movement = 0, brightness = 0, flash_delta = 0;
            uint64_t scene = 0, camera = 0;
            std::vector<int> previous;
        };
        bool alternate_every_frame = false;
        bool start_fault = !composition_fault;
        auto record = [&](int i, bool light_on, Sample* sample, bool pinned = false, bool overwrite = false,
                          bool clear_before_reader = false) {
            Pump();
            const auto start = evaluator.writes;
            evaluator.received = nullptr;
            evaluator.proof = 0;
            // Only the parity fixture swaps roles on every frame.
            const bool swap_roles = !pinned && (alternate_every_frame ? i % 2 != 0 : (i % 7 == 2 || i % 7 == 4 || i % 7 == 5));
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
            c->PSSetShader(staged_flash ? trial_surface.Get() : game.ps_scene.Get(), nullptr, 0);
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
            } else if (staged_flash) {
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
                        const auto change = std::abs(visible_pixel-s.previous[current.size()]);
                        s.movement += change;
                        ++s.comparisons;
                        if (x >= 120 && x < 160 && y >= 68 && y < 90) {
                            s.flash_delta += change;
                            ++s.flash_pairs;
                        }
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
        if (parity_roles) {
            // Establish an ordinary physical owner before the scene begins
            // swapping roles every frame. This is the incumbent under test.
            for (int i = 0; i < 70; ++i) if (!record(i, true, nullptr)) return 1;
            alternate_every_frame = true;
        }
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
            auto until = GetTickCount64() + 18000;
            bool kept_by_roles = false;
            int i = 0;
            while (GetTickCount64() < until) {
                if (reuse_depth) na::overlay::SetOpen((i / 8) % 2 == 0);
                if (!record(i++, true, nullptr)) return 1;
                if (parity_roles && !kept_by_roles) {
                    for (const auto& line : na::eventlog::Drain()) {
                        if (line.find("referee_roles") == std::string::npos || line.find(" kept=1") == std::string::npos) continue;
                        std::printf("%s\n", line.c_str());
                        kept_by_roles = true;
                        until = GetTickCount64() + na::referee::RouteTrial::maximum_ms + 1000;
                    }
                }
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
            if (disagreeing_scene) {
                // Both readers may qualify while successive pictures from
                // the ordinary route have no comparable structure. Keep
                // delivering DLAA on the incumbent in that case.
                expect(fixed.evaluated == 28 && fixed.correct == baseline.correct && new_delta >= old_delta * .80 &&
                    stats.trial_pairs > stats.trial_comparable,
                    "alternating pictures without shared structure cannot authorize a replacement");
                na::native11::Uninstall();
                return g_failed ? 1 : 0;
            }
            if (parity_roles)
                expect(kept_by_roles && checked_decoy && stats.parity_recognitions > 0,
                       "measured parity flicker keeps the proved readers past the former timeout");
            expect(fixed.correct == 28 && fixed.proven == 28 && fixed.displayed_readers == 28,
                   "the actual provider keeps the improved screen-proved route");
            expect(fixed.changed > 100 && fixed.reached * 100 >= fixed.changed * 98 && new_delta < old_delta * .25,
                   "real NGX reaches the screen and the reflected light stops flickering");
            const double flash_before = baseline.flash_delta / baseline.flash_pairs;
            const double flash_after = fixed.flash_delta / fixed.flash_pairs;
            std::printf("known reflection footprint: before=%.4f after=%.4f\n", flash_before, flash_after);
            expect(flash_before > 10 && flash_after <= flash_before * .10,
                   "the automatic replacement cuts displayed variation by at least ninety percent where the light flashes");
            if (parity_roles) {
                na::native11::Uninstall();
                return g_failed ? 1 : 0;
            }
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
    // Live x2 in the automatic-API mode (the graphics API found at the first present, the DX11 proxy from
    // chain creation): 1 = interface on the shown picture; 2 = the interface from a deferred context only;
    // 3 = part of it from a deferred context between the scene and the immediate interface draws;
    // 4 = a chain made before NeuralAlias could take it over.
    const char* arg1 = argc > 1 ? argv[1] : "";
    const int fg_auto = !std::strcmp(arg1, "--fg-x2-auto") ? 1 : !std::strcmp(arg1, "--fg-x2-auto-deferred-hud") ? 2 :
                        !std::strcmp(arg1, "--fg-x2-auto-deferred-hud-mixed") ? 3 : !std::strcmp(arg1, "--fg-x2-auto-late-chain") ? 4 : 0;
    const bool automatic = fg_auto != 0 || (argc > 1 && std::strcmp(argv[1], "--automatic") == 0);
    const bool padded_hdr = argc > 1 && std::strcmp(argv[1], "--lit-role-padded") == 0;
    const bool disagreeing_scene = argc > 1 && std::strcmp(argv[1], "--lit-role-trial-disagreeing-scene") == 0;
    const bool trial_reused_depth = argc > 1 && std::strcmp(argv[1], "--lit-role-trial-reused-depth") == 0;
    const bool reuse_depth = trial_reused_depth || disagreeing_scene || (argc > 1 && std::strcmp(argv[1], "--lit-role-reused-depth") == 0);
    const bool composition_fault = argc > 1 && std::strcmp(argv[1], "--composition-recovery") == 0;
    const bool parity_roles = argc > 1 && std::strcmp(argv[1], "--lit-role-parity") == 0;
    const bool compare_routes = composition_fault || trial_reused_depth || disagreeing_scene || parity_roles ||
        (argc > 1 && std::strcmp(argv[1], "--lit-role-trial") == 0);
    const bool rotating_lights = compare_routes || padded_hdr || reuse_depth || (argc > 1 && std::strcmp(argv[1], "--lit-role") == 0);
    const bool replay_handoff = !std::strcmp(arg1, "--replay-handoff");
    // First-time set-up beside the game (core/first_setup.hpp): the product's order. Every other case
    // counts delivered frames from its first call and asks for set-up inside that call.
    const bool first_setup = !std::strcmp(arg1, "--first-setup");
    na::first_setup::InlineForTests().store(!first_setup);
    const bool camera_off_start = replay_handoff || first_setup || !std::strcmp(arg1, "--camera-off-start");
    // One presenter for both APIs, stage 3: live x2 on a DX11 game (proxy chain, native DLAA inputs,
    // HUD-free picture, presenter), 8-bit SDR or FP16 back buffer.
    const bool fg_x2_hdr = argc > 1 && std::strcmp(argv[1], "--fg-x2-fp16") == 0;
    // x2 together with a DLSS level (the fold): Frame Generation gets the level's folded depth and motion.
    const uint32_t fg_fold = !std::strcmp(arg1, "--fg-x2-quality") ? 1u : !std::strcmp(arg1, "--fg-x2-performance") ? 2u : 0u;
    // The names fixture (VARIANT hudpaste/moves-with-scene): map names drawn after the hand-over that move with a
    // panning map, under x2. --fg-x2-names[-slow|-fast][-t2]: pan 5 px a frame (1.5 slow, 16 fast); -t2 pastes the
    // interface the T2 way (hudpaste/screen-fixed) for the before figures.
    const char* names_arg = std::strncmp(arg1, "--fg-x2-names", 13) == 0 ? arg1 + 13 : nullptr;
    const bool fg_names = names_arg != nullptr;
    const bool names_t2 = fg_names && std::strstr(names_arg, "-t2") != nullptr;
    const float names_pan = !fg_names ? 0.f : std::strstr(names_arg, "-slow") ? 1.5f : std::strstr(names_arg, "-fast") ? 16.f : 5.f;
    const bool fg_x2 = fg_x2_hdr || fg_auto != 0 || fg_fold != 0 || fg_names || (argc > 1 && std::strcmp(argv[1], "--fg-x2") == 0);
    const bool proxy_env_off = GetEnvironmentVariableW(L"NA_TEST_PROXY_OFF", nullptr, 0) != 0;
    // DLSS Quality / Performance on the native DX11 route (the fold: coarse shading through NVAPI, the
    // shared fold pass, the waiting period with DLAA on screen).
    const uint32_t fold_level = !std::strcmp(arg1, "--fold-quality") || !std::strcmp(arg1, "--fold-quality-measured") ? 1u
                              : !std::strcmp(arg1, "--fold-performance") || !std::strcmp(arg1, "--fold-performance-measured") ? 2u : 0u;
    const bool fold_measured = fold_level && std::strstr(arg1, "-measured") != nullptr; // no camera: motion measured, folded flow
    // The overlay fixture: --overlay-jitter (DLAA), --overlay-jitter-quality, --overlay-jitter-performance.
    const int overlay_level = !std::strcmp(arg1, "--overlay-jitter") ? 0 : !std::strcmp(arg1, "--overlay-jitter-quality") ? 1
                            : !std::strcmp(arg1, "--overlay-jitter-performance") ? 2 : -1;
    wchar_t fg_folder[MAX_PATH]{};
    if (fg_x2 && !GetEnvironmentVariableW(L"NA_FG_SEARCH", fg_folder, MAX_PATH)) {
        std::printf("SKIP: NA_FG_SEARCH (the folder with nvngx_dlssg.dll) is not set\n");
        return 77;
    }
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
        !set(L"dx11on12", L"enabled", (automatic && !fg_auto) ? L"1" : L"0") ||
        !set(L"native11", L"enabled", automatic ? L"0" : L"1") ||
        !set(L"host", L"graphics_api", automatic ? L"auto" : L"") ||
        !set(L"overlay", L"enabled", L"1") || !set(L"adapter", L"enabled", L"0") ||
        !set(L"observe", L"producers", (automatic && !fg_auto) ? L"1" : L"0") ||
        !set(L"observe", L"early_install", L"0") ||
        !set(L"present", L"proxy", L"0") || !set(L"settings", L"scope", scope.c_str()) ||
        !set(L"fsr", L"enabled", L"0") || !set(L"xess", L"enabled", L"0") ||
        !set(L"j2", L"enabled", L"1") ||
        !set(L"referee", L"enabled", L"0") || // legacy disabled package
        !set(L"j2", L"camera", (camera_off_start || fg_x2 || adaptive || rotating_lights || fold_measured || overlay_level >= 0) ? L"0" : L"2") ||
        (fg_x2 && (!set(L"fg", L"live", L"1") || !set(L"fg", L"search_path", fg_folder))) ||
        (GetEnvironmentVariableW(L"NA_TEST_PROXY_OFF", nullptr, 0) && !set(L"present", L"dx11_proxy", L"0")) ||

        !na::host::StartFromIni(GetModuleHandleW(nullptr), ini_name.c_str(), "NATIVE DX11 TEST")) return 1;
    na::host::WaitForObserveInstall();
    const ULONGLONG init_deadline = GetTickCount64() + 20000;
    while (!na::host::GetStatus().init_complete && GetTickCount64() < init_deadline) Sleep(5);
    expect(na::host::GetStatus().init_complete && na::on12::NativeRoute() != automatic && !na::on12::Enabled(),
           "production host starts with the bridge disabled");
    expect(na::settings::Get().referee_enabled.load() == unsigned(na::referee::gpu::Supported()),
           "added DLAA automatically starts the supported Referee despite a legacy Off setting");
    if (g_failed) return 1;
#endif
    Game g;
#if defined(NA_TEST_NATIVE_DLAA)
    g.padded_hdr = padded_hdr;
    if (fg_x2_hdr) g.back_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (fold_measured || overlay_level >= 0 || fg_names) { g.w = 1280; g.h = 720; } // the folded flow needs a working size the optical flow accepts
    {   // NA_TEST_FOLD_SIZE=WxH: the fold fixture at another picture size (e.g. 3440x1440 for the memory figure).
        wchar_t sz[32]{};
        unsigned fw = 0, fh = 0;
        if ((fold_level || fg_x2) && GetEnvironmentVariableW(L"NA_TEST_FOLD_SIZE", sz, 32) && swscanf_s(sz, L"%ux%u", &fw, &fh) == 2 && fw && fh) { g.w = fw; g.h = fh; }
    }
#endif
    const bool centered_scene = argc > 1 && std::strcmp(argv[1], "--centered-scene") == 0;
    if (centered_scene) g.screen_extra_width = 88;
    bool warp = false;
    const bool screen_warp = argc > 1 && (std::strcmp(argv[1], "--screen-check-warp") == 0 ||
                                         std::strcmp(argv[1], "--adaptive-screen-warp") == 0);
    const bool depth_warp = argc > 1 && std::strcmp(argv[1], "--depth-reads-warp") == 0;
    const bool depth_debug = argc > 1 && std::strcmp(argv[1], "--depth-reads-debug") == 0;
    const bool depth_reads = depth_warp || depth_debug || (argc > 1 && std::strcmp(argv[1], "--depth-reads") == 0);
    g.shader_depth = depth_reads;
#if defined(NA_TEST_NATIVE_DLAA)
    if (fg_auto == 4) na::on12::SetNativeProxyForTests(false); // the chain is made as if NeuralAlias had not seen it
#endif
    if (screen_warp || depth_warp || depth_debug || !MakeGame(g, false)) {
        g = Game{};
        if (centered_scene) g.screen_extra_width = 88;
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
    if (centered_scene) return CenteredSceneFixture(g);
    if (adaptive || screen_warp || (argc > 1 && std::strcmp(argv[1], "--screen-check") == 0)) return ScreenCheckFixture(g, adaptive, missed_order);
#endif
#if defined(NA_TEST_NATIVE_DLAA)
    if (warp) return 77;
    D3D11_SAMPLER_DESC early_desc{};
    early_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    early_desc.AddressU = early_desc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    early_desc.AddressW = D3D11_TEXTURE_ADDRESS_MIRROR;
    early_desc.MaxLOD = D3D11_FLOAT32_MAX;
    early_desc.MaxAnisotropy = 1;
    ComPtr<ID3D11SamplerState> early_original, early_already_biased;
    {
        // In automatic mode this really runs before the first Present installs
        // hooks. In the explicit route, keep the same states out of the ledger.
        na::native11::OwnWorkScope own_work;
        expect(SUCCEEDED(g.device->CreateSamplerState(&early_desc, &early_original)), "create startup sampler");
        early_desc.MipLODBias = -1.f;
        expect(SUCCEEDED(g.device->CreateSamplerState(&early_desc, &early_already_biased)),
               "create startup sampler already at the policy's target");
    }
    ComPtr<ID3D11On12Device> translated;
    expect(FAILED(g.device.As(&translated)), "game device is native DX11");
    if (fg_auto == 4) na::on12::SetNativeProxyForTests(true);
    if (fg_auto && !proxy_env_off) {
        // Order of events: the chain is made through the hooked DXGI factory, so the proxy exists from its
        // creation; the graphics API is chosen, and the DX11 hooks installed, only at the first present.
        static char before[8192]{};
        na::on12::FormatStatus(before, sizeof before);
        expect(!na::on12::NativeRoute() && std::strstr(before, "native11_route=active") == nullptr,
               "automatic mode: no graphics API chosen and no DX11 hooks before the first present");
        expect(na::on12::NativeProxy(g.swap.Get()) == (fg_auto != 4),
               fg_auto == 4 ? "a chain made before the takeover stays the game's own"
                            : "the chain is presented through our D3D12 queue from its creation, before the first present");
    }
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
        const int result = ScreenCheckFixture(g, adaptive, missed_order, rotating_lights, compare_routes,
                                               composition_fault, reuse_depth, disagreeing_scene, parity_roles);
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
    if (fg_x2) {
        // The game's chain is a DX11 proxy served by the presenter (live Frame Generation package).
        if (fg_auto != 4 && !proxy_env_off)
            expect(na::on12::NativeProxyOn() && na::on12::NativeProxy(g.swap.Get()), "the game's DX11 chain is presented through our D3D12 queue");
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE; rd.ScissorEnable = TRUE;
        expect(SUCCEEDED(g.device->CreateRasterizerState(&rd, &g.hud_scissor)), "HUD panel state");
        na::native11::SetJitterRoute(na::native11::JitterRoute::Viewport);
        auto& st = na::settings::Get();
        st.fg_mode.store(0); st.fg_multiplier.store(2); st.fg_enabled.store(GetEnvironmentVariableW(L"NA_TEST_FG_OFF", nullptr, 0) ? 0u : 1u); // x2, as the menu sets it
        if (GetEnvironmentVariableW(L"NA_TEST_DLAA_HASH", nullptr, 0)) {
            // DLAA alone (x2 Off): a fixed run, then a hash of the scene picture after DLAA's write-back.
            // Run once with the proxy (DLAA on the presentation queue) and once without (its own queue).
            st.fg_enabled.store(0);
            for (int frame = 0; frame < 60; ++frame) { Pump(); Scene(g.ctx.Get(), g, frame); Post(g.ctx.Get(), g, true); g.swap->Present(0, 0); }
            D3D11_TEXTURE2D_DESC d{};
            g.color->GetDesc(&d);
            d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
            ComPtr<ID3D11Texture2D> stg;
            uint64_t h = 1469598103934665603ull;
            if (SUCCEEDED(g.device->CreateTexture2D(&d, nullptr, &stg))) {
                g.ctx->CopyResource(stg.Get(), g.color.Get());
                D3D11_MAPPED_SUBRESOURCE m{};
                if (SUCCEEDED(g.ctx->Map(stg.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                    for (UINT y = 0; y < d.Height; ++y) {
                        const auto* row = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
                        for (UINT i = 0; i < d.Width * 4; ++i) { h ^= row[i]; h *= 1099511628211ull; }
                    }
                    g.ctx->Unmap(stg.Get(), 0);
                }
            }
            std::printf("DLAA picture hash after 60 frames (%s): %016llx, delivered %llu\n", na::on12::NativeProxy(g.swap.Get()) ? "proxy, presentation queue" : "no proxy, own queue",
                        static_cast<unsigned long long>(h), static_cast<unsigned long long>(evaluations));
            DeleteFileW(ini.c_str());
            std::fflush(stdout);
            ExitProcess(0);
        }
        if (fg_auto >= 2) {
            // x2 is on but cannot run with this frame: it must wait with a plain reason, present the game's
            // own frames unchanged, and never stall or remove the device.
            if (fg_auto == 2 || fg_auto == 3) {
                g.deferred_hud = fg_auto == 2 ? 1 : 2;
                expect(SUCCEEDED(g.device->CreateDeferredContext(0, &g.hud_dc)), "a deferred context for the interface");
            }
            for (int frame = 0; frame < 240; ++frame) { Pump(); Scene(g.ctx.Get(), g, frame); Post(g.ctx.Get(), g, true); g.swap->Present(0, 0); }
            const auto pr = na::present::GetStats();
            const auto fs = na::fg::GetStats();
            const auto hud = na::j2::GetLiveHudStatsAnyApi();
            char reason[160] = {};
            unsigned long long refused = 0;
            na::on12::ProxyReason(reason, sizeof reason, &refused);
            std::printf("x2 on, cannot run (%s): shown %.1f from %.1f real, generated shown %llu, fg \"%s\"\n  HUD learned=%u verdict: %s\n  last skip: %s\n  proxy: refused=%llu \"%s\"\n",
                        fg_auto == 2 ? "interface from a deferred context" : fg_auto == 3 ? "part of the interface from a deferred context" : "chain made before the takeover",
                        pr.shown_fps, pr.base_fps, static_cast<unsigned long long>(pr.generated_shown), fs.reason, hud.learned, hud.verdict, hud.last_skip, refused, reason);
            expect(pr.generated_shown == 0, "no generated picture is shown");
            if (fg_auto == 4) {
                expect(!na::on12::NativeProxy(g.swap.Get()) && refused >= 1 && std::strstr(reason, "made before NeuralAlias could take it over") != nullptr,
                       "the menu row's reason says the chain was made before the takeover");
            } else {
                expect(!hud.learned && std::strstr(hud.verdict, "deferred command lists") != nullptr,
                       "the HUD verdict names the deferred command lists, in plain words");
                expect(hud.learn_attempts >= 1, "the frame was looked at");
            }
            expect(pr.device_hr == S_OK, "no device removal");
            DeleteFileW(ini.c_str());
            std::printf(g_failed ? "FAIL: %d check(s) failed\n" : "PASS: x2 waits with the plain reason\n", g_failed);
            std::fflush(stdout);
            ExitProcess(g_failed ? 1u : 0u);
        }
        if (fg_fold) st.j2_fold_mode.store(fg_fold); // the level is asked for from the first frame; x2 runs beside it
        if (fg_names) {
            const auto code = Compile("ps_map", "ps_5_0");
            expect(code && SUCCEEDED(g.device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g.ps_map)), "the map shader");
            D3D11_RASTERIZER_DESC nr{};
            nr.FillMode = D3D11_FILL_SOLID; nr.CullMode = D3D11_CULL_NONE; nr.DepthClipEnable = TRUE;
            expect(SUCCEEDED(g.device->CreateRasterizerState(&nr, &g.names_rs)), "the names fixture's rasterizer state");
            g.names = true;
            g.pan = names_pan;
            st.fg_hud_move.store(names_t2 ? 0u : 1u);
        }
        const char* fold_name = fg_fold == 2 ? "Performance" : "Quality";
        const auto fold_on_screen = [&] {
            char fs11[512]{};
            na::native_d3d11::FoldStatus(fs11, sizeof fs11);
            return std::string(fs11).find(std::string("On screen: ") + fold_name) != std::string::npos;
        };
        bool asked = false;
        static std::atomic<ULONGLONG> last_frame_at{GetTickCount64()};
        std::thread([] { // a frame that never returns: say where everything stood, then stop
            for (;;) {
                Sleep(1000);
                if (GetTickCount64() - last_frame_at.load() < 8000) continue;
                std::printf("HANG: a frame did not return within 8 s\n");
                std::fflush(stdout);
                ExitProcess(3);
            }
        }).detach();
        for (int frame = 0; frame < 400; ++frame) {
            last_frame_at.store(GetTickCount64());
            const ULONGLONG t0 = GetTickCount64();
            Pump();
            Scene(g.ctx.Get(), g, frame);
            Post(g.ctx.Get(), g, true);
            if (fg_names && (frame == 3 || frame == 20)) { // what the game's own picture holds before Present (map, names)
                ComPtr<ID3D11Texture2D> bb, stg;
                if (SUCCEEDED(g.swap->GetBuffer(0, IID_PPV_ARGS(&bb)))) {
                    D3D11_TEXTURE2D_DESC d{};
                    bb->GetDesc(&d);
                    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
                    if (SUCCEEDED(g.device->CreateTexture2D(&d, nullptr, &stg))) {
                        g.ctx->CopyResource(stg.Get(), bb.Get());
                        D3D11_MAPPED_SUBRESOURCE m{};
                        if (SUCCEEDED(g.ctx->Map(stg.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                            const auto px = [&](int x, int y) { const auto* p = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch + size_t(x) * 4; return std::array<int, 3>{p[0], p[1], p[2]}; };
                            const float nx = NameX(g, 0, 1);
                            const auto a = px(600, 400), b = px(int(nx) + 20, 156), c = px(100, 60);
                            std::printf("  frame %d back buffer: map (600,400) %d,%d,%d; name at x %.1f: %d,%d,%d; HUD panel %d,%d,%d\n", frame, a[0], a[1], a[2], nx,
                                        b[0], b[1], b[2], c[0], c[1], c[2]);
                            g.ctx->Unmap(stg.Get(), 0);
                        }
                    }
                }
            }
            g.swap->Present(0, 0);
            if (GetTickCount64() - t0 > 200) std::printf("  frame %d took %llu ms\n", frame, GetTickCount64() - t0);
            const auto pr = na::present::GetStats();
            if (!asked && pr.generated_shown >= 20 && (!fg_fold || fold_on_screen())) { // x2 runs: save the presented real / generated / real pictures
                char why[160]{};
                asked = na::present::RequestOutputCapture(3, why, sizeof why);
                expect(asked, "the presented-picture capture starts");
            }
            const auto lv = na::present::GetLiveStats();
            if (asked && std::strstr(lv.capture, "saved:")) break;
        }
        Sleep(500); // the capture writer finishes on its own thread
        const auto pr = na::present::GetStats();
        const auto fs = na::fg::GetStats();
        const auto lv = na::present::GetLiveStats();
        const auto hud = na::j2::GetLiveHudStatsAnyApi();
        std::printf("x2 on DX11 (%s): real fps %.1f shown %.1f, generated shown %llu, evaluates %llu, fg \"%s\"\n  HUD: %s\n  steady %llu frames, %.2f %%, dropped gap=%llu reset=%llu\n  capture: %s\n  ms: copy %.3f hudcopy %.3f eval %.3f steady %.3f paste %.3f off %.3f\n",
                    fg_x2_hdr ? "FP16" : "8-bit SDR", pr.base_fps, pr.shown_fps, static_cast<unsigned long long>(pr.generated_shown),
                    static_cast<unsigned long long>(fs.evaluates), fs.reason, hud.method, static_cast<unsigned long long>(lv.steady_frames), lv.steady_share,
                    static_cast<unsigned long long>(lv.steady_drop_gap), static_cast<unsigned long long>(lv.steady_drop_reset), lv.capture,
                    lv.copy_avg, lv.hudcopy_avg, lv.eval_avg, lv.steady_avg, lv.paste_avg, lv.off_avg);
        expect(pr.generated_shown >= 20, "generated pictures are presented between the game's own frames");
        expect(hud.learned && std::strstr(hud.method, "hudfree/") != nullptr, "the HUD-free picture's variant was learned from the frame's shape");
        // The presented burst: in every generated picture the HUD panel (inside, one pixel in) equals the next real frame's.
        const char* at = std::strstr(lv.capture, "C:\\");
        expect(at != nullptr, "the capture folder is named");
        if (at) {
            const std::filesystem::path cap{std::string(at)};
            const uint32_t bpp = fg_x2_hdr ? 8u : 4u;
            auto read = [&](const std::string& name) {
                std::vector<uint8_t> b;
                FILE* f = nullptr;
                if (_wfopen_s(&f, (cap / name).c_str(), L"rb") == 0 && f) { std::fseek(f, 0, SEEK_END); b.resize(size_t(std::ftell(f))); std::fseek(f, 0, SEEK_SET); std::fread(b.data(), 1, b.size(), f); std::fclose(f); }
                return b;
            };
            std::string json;
            { auto j = read("presented.json"); json.assign(j.begin(), j.end()); }
            std::vector<std::string> files, kinds;
            for (size_t p = 0; (p = json.find("\"kind\": \"", p)) != std::string::npos;) {
                p += 9; const size_t e = json.find('"', p); kinds.push_back(json.substr(p, e - p));
                const size_t fp = json.find("\"file\": \"", e) + 9; files.push_back(json.substr(fp, json.find('"', fp) - fp));
                p = e;
            }
            const UINT W = g.w, H = g.h;
            uint32_t checked = 0, wrong = 0;
            bool raw_logged = false;
            for (size_t i = 0; i + 1 < kinds.size(); ++i) {
                if (kinds[i] != "generated" || kinds[i + 1] != "next-real" || files[i].empty() || files[i + 1].empty()) continue;
                const auto gen = read(files[i]), real = read(files[i + 1]);
                if (gen.size() < size_t(W) * H * bpp || real.size() != gen.size()) continue;
                const UINT stride = UINT(gen.size() / (size_t(H) * bpp)); // capture width
                for (LONG y = kHudRect[1] + 1; y < kHudRect[3] - 1; ++y)
                    for (LONG x = kHudRect[0] + 1; x < kHudRect[2] - 1; ++x) {
                        const size_t o = (size_t(y) * stride + size_t(x)) * bpp;
                        if (bpp == 4) { wrong += std::memcmp(&gen[o], &real[o], bpp) != 0; continue; }
                        // FP16 (scRGB): exact in what the screen shows (the paste rule's display terms; values
                        // above 1.0 clip to white there, where the real and HUD-free pictures cannot differ).
                        bool differs = false;
                        for (int ch = 0; ch < 3; ++ch) {
                            uint16_t hg = 0, hr = 0;
                            std::memcpy(&hg, &gen[o + size_t(ch) * 2], 2); std::memcpy(&hr, &real[o + size_t(ch) * 2], 2);
                            const float vg = na::fglive::DisplayValue(DirectX::PackedVector::XMConvertHalfToFloat(hg));
                            const float vr = na::fglive::DisplayValue(DirectX::PackedVector::XMConvertHalfToFloat(hr));
                            differs = differs || std::fabs(vg - vr) > 0.5f / 255.f;
                        }
                        if (differs && !raw_logged) {
                            raw_logged = true;
                            uint16_t hg = 0, hr = 0;
                            std::memcpy(&hg, &gen[o], 2); std::memcpy(&hr, &real[o], 2);
                            std::printf("  first differing HUD pixel (%ld,%ld): generated R %.4f, next real R %.4f\n", x, y,
                                        DirectX::PackedVector::XMConvertHalfToFloat(hg), DirectX::PackedVector::XMConvertHalfToFloat(hr));
                        }
                        wrong += differs;
                    }
                ++checked;
            }
            std::printf("  presented burst: %zu pictures, %u generated pictures checked, HUD pixels differing from the next real frame: %u\n", kinds.size(), checked, wrong);
            if (checked < 2) std::printf("  presented.json:\n%s\n", json.c_str()); // why no pair could be checked
            expect(checked >= 2 && wrong == 0, "the interface is exact in generated pictures (the newer real frame's own HUD)");
            if (fg_names && bpp == 4) {
                // Each name's centre (threshold crossings, to a fraction of a pixel) in the previous real, generated and
                // next real pictures; the true place on the generated picture is halfway between the two real ones.
                const auto centres = [&](const std::vector<uint8_t>& pic, int row) {
                    std::vector<float> out;
                    const UINT stride = UINT(pic.size() / (size_t(H) * 4));
                    const int y = int(NameY(row) + kNameH * .5f);
                    const auto lum = [&](int x) { const uint8_t* p = &pic[(size_t(y) * stride + size_t(x)) * 4]; return (p[0] + p[1] + p[2]) / (3.f * 255.f); };
                    const float thr = .52f;
                    for (int x = 1; x + 1 < int(W);) {
                        if (lum(x) <= thr || lum(x - 1) > thr) { ++x; continue; }
                        int e = x;
                        while (e + 1 < int(W) && lum(e + 1) > thr) ++e;
                        if (e - x >= 30 && x > 2 && e < int(W) - 3) {
                            const float l0 = lum(x - 1), l1 = lum(x), r0 = lum(e), r1 = lum(e + 1);
                            const float left = float(x - 1) + (l1 != l0 ? (thr - l0) / (l1 - l0) : 1.f);
                            const float right = float(e) + (r1 != r0 ? (r0 - thr) / (r0 - r1) : 0.f);
                            out.push_back(.5f * (left + right));
                        }
                        x = e + 1;
                    }
                    return out;
                };
                float worst = 0, sum = 0, worst_unit = 0;
                int n = 0, n_unit = 0;
                for (size_t i = 1; i + 1 < kinds.size(); ++i) {
                    if (kinds[i] != "generated" || kinds[i + 1] != "next-real" || kinds[i - 1].find("real") == std::string::npos) continue;
                    const auto prev = read(files[i - 1]), gen = read(files[i]), next = read(files[i + 1]);
                    if (prev.size() != gen.size() || next.size() != gen.size() || gen.empty()) continue;
                    for (int row = 0; row < 4; ++row) {
                        const auto cp = centres(prev, row), cg = centres(gen, row), cn = centres(next, row);
                        for (const float x : cg) {
                            float bp = 1e9f, bn = 1e9f;
                            for (const float p : cp) if (std::fabs(p - x) < std::fabs(bp - x)) bp = p;
                            for (const float q : cn) if (std::fabs(q - x) < std::fabs(bn - x)) bn = q;
                            if (std::fabs(bp - bn) > 2.f * g.pan + 4.f || std::fabs(bp - x) > g.pan + 4.f) continue; // not the same name
                            if (std::fabs((bp - bn) - g.pan) > 1.f) continue; // the two real pictures are not consecutive frames (one was not shown)
                            if (x > kHudRect[0] - 60 && x < kHudRect[2] + 60 && row == 0) continue;
                            const float err = std::fabs(x - .5f * (bp + bn));
                            if (err > .6f) std::printf("    name row %d at x %.2f: previous %.2f, next %.2f, error %.2f px (picture %zu)\n", row, x, bp, bn, err, i);
                            if (row == 3) { worst_unit = (std::max)(worst_unit, err); ++n_unit; }
                            worst = (std::max)(worst, err);
                            sum += err;
                            ++n;
                        }
                    }
                }
                const auto ls = na::present::GetLiveStats();
                std::printf("  names (%s, pan %.1f px a frame): %d measured, mean error %.2f px, worst %.2f px (row over the moving unit: %d, worst %.2f px); "
                            "hud_move frames %llu, pictures %llu, tiles moving %llu fixed %llu, mean shift %.2f px, pasted T2 pair=%llu reset=%llu motion=%llu off=%llu size=%llu\n",
                            names_t2 ? "hudpaste/screen-fixed, before" : "hudpaste/moves-with-scene, after", g.pan, n, n ? sum / n : 0.f, worst, n_unit, worst_unit,
                            static_cast<unsigned long long>(ls.move_frames), static_cast<unsigned long long>(ls.move_pictures),
                            static_cast<unsigned long long>(ls.move_tiles_moving), static_cast<unsigned long long>(ls.move_tiles_fixed), ls.move_shift_px,
                            static_cast<unsigned long long>(ls.move_fb_pair), static_cast<unsigned long long>(ls.move_fb_reset),
                            static_cast<unsigned long long>(ls.move_fb_motion), static_cast<unsigned long long>(ls.move_fb_off),
                            static_cast<unsigned long long>(ls.move_fb_size));
                expect(n >= 4, "names were measured on generated pictures");
                if (names_t2) expect(n && sum / n > .5f * g.pan - .6f, "pasted the T2 way, the names sit about half a real-frame step away");
                else expect(n && worst < .6f, "names that move with the map are placed at the generated picture's time (within 0.6 px)");
            }
            if (fg_names && GetEnvironmentVariableW(L"NA_TEST_KEEP_CAPTURE", nullptr, 0)) {
                std::printf("  capture kept: %s\n", cap.string().c_str());
            } else
            for (const auto& e : std::filesystem::directory_iterator(cap)) { std::error_code ec; std::filesystem::remove(e.path(), ec); }
            std::error_code ec; std::filesystem::remove(cap, ec);
        }
        expect(pr.device_hr == S_OK, "no device removal");
        if (fg_fold) {
            char fs11[512]{};
            na::native_d3d11::FoldStatus(fs11, sizeof fs11);
            const auto cs = na::native11::GetCoarseShadingStats();
            std::printf("  with %s: menu \"%s\"; coarse frames %llu, draws %llu, before lists %llu, failures %llu\n", fold_name, fs11,
                        static_cast<unsigned long long>(cs.frames), static_cast<unsigned long long>(cs.draws),
                        static_cast<unsigned long long>(cs.before_lists), static_cast<unsigned long long>(cs.failures));
            expect(fold_on_screen(), "the DLSS level is on screen while x2 runs");
            expect(cs.frames > 0 && cs.failures == 0, "the scene was shaded in blocks with no failure");
        }
        {   // Video memory with x2 on, then a few seconds after x2 Off (its textures and feature given back),
            // then x2 on again (made again off the frame while the game's own frames keep showing).
            const auto vram = [&](const char* when) {
                static char line[1536];
                na::vram::FormatLine(line, sizeof line);
                double process_mb = 0;
                ComPtr<IDXGIDevice> dd;
                ComPtr<IDXGIAdapter> da;
                ComPtr<IDXGIAdapter3> a3;
                DXGI_QUERY_VIDEO_MEMORY_INFO mi{};
                if (SUCCEEDED(g.device.As(&dd)) && SUCCEEDED(dd->GetAdapter(&da)) && SUCCEEDED(da.As(&a3)) &&
                    SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mi)))
                    process_mb = double(mi.CurrentUsage) / 1048576.0;
                const auto ds = na::dlss::GetStats();
                std::printf("  vram (%s): whole process %.1f MB | ours: %s | DLSS features held %llu\n", when, process_mb, line,
                            static_cast<unsigned long long>(ds.features_created - ds.features_released));
            };
            LARGE_INTEGER hz{};
            QueryPerformanceFrequency(&hz);
            const auto timed_frame = [&](int frame) {
                LARGE_INTEGER t0{}, t1{};
                QueryPerformanceCounter(&t0);
                last_frame_at.store(GetTickCount64());
                Pump(); Scene(g.ctx.Get(), g, frame); Post(g.ctx.Get(), g, true); g.swap->Present(0, 0);
                QueryPerformanceCounter(&t1);
                return double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(hz.QuadPart);
            };
            int frame = 1000;
            double on_sum = 0;
            for (int k = 0; k < 60; ++k) on_sum += timed_frame(frame++);
            vram(fg_fold ? (fg_fold == 2 ? "Performance + x2" : "Quality + x2") : "DLAA + x2");
            st.fg_enabled.store(0);
            double off_max = 0;
            const ULONGLONG until = GetTickCount64() + 4500;
            while (GetTickCount64() < until) off_max = (std::max)(off_max, timed_frame(frame++));
            vram("x2 then Off, 4.5 s later");
            expect(na::vram::Get("x2_textures") == 0 && na::vram::Get("fg_feature") == 0, "x2 Off: its textures and feature are released");
            st.fg_enabled.store(1);
            const auto gen_before = na::present::GetStats().generated_shown;
            double back_max = 0;
            int back_at = -1;
            for (int k = 0; k < 400 && back_at < 0; ++k) {
                back_max = (std::max)(back_max, timed_frame(frame++));
                if (na::present::GetStats().generated_shown > gen_before + 4) back_at = k;
            }
            vram("x2 on again");
            std::printf("  x2 Off then On: steady frame %.1f ms; longest frame while x2's memory was released %.1f ms; x2 generating again after %d frames, longest frame meanwhile %.1f ms\n",
                        on_sum / 60, off_max, back_at, back_max);
            expect(back_at >= 0, "x2 generates again after being turned back on");
        }
        DeleteFileW(ini.c_str());
        std::printf(g_failed ? "FAIL: %d live x2 check(s) failed\n" : "PASS: live x2 on a DX11 game\n", g_failed);
        std::fflush(stdout);
        ExitProcess(g_failed ? 1u : 0u);
    }
    if (overlay_level >= 0) {
        // Hearts of Iron IV's frame shape (2026-11-27): depth-tested terrain, then thin lines and labels drawn into
        // the scene picture with the depth test off and the scene depth bound, then the interface after the
        // hand-over. Before (jitter/depth-tested-only) and after (jitter/scene-overlays) on a still view:
        // the share of the picture's edges that never move with the jitter (the HoI4 frames showed 11-14 %),
        // on the lines and labels and everywhere; the interface must stay exactly on the game's grid.
        na::native11::SetJitterRoute(na::native11::JitterRoute::Viewport);
        na::j2::SetJitter(true);
        const char* level_name = overlay_level == 2 ? "Performance" : overlay_level == 1 ? "Quality" : "DLAA";
        {
            const auto lines_code = Compile("vs_overlay_lines", "vs_5_0"), text_code = Compile("vs_overlay_text", "vs_5_0");
            const auto line_ps = Compile("ps_overlay_line", "ps_5_0"), text_ps = Compile("ps_overlay_text", "ps_5_0");
            const auto panel_code = Compile("vs_overlay_panel", "vs_5_0"), panel_ps = Compile("ps_overlay_panel", "ps_5_0");
            expect(panel_code && panel_ps &&
                   SUCCEEDED(g.device->CreateVertexShader(panel_code->GetBufferPointer(), panel_code->GetBufferSize(), nullptr, &g.vs_panel)) &&
                   SUCCEEDED(g.device->CreatePixelShader(panel_ps->GetBufferPointer(), panel_ps->GetBufferSize(), nullptr, &g.ps_panel)),
                   "scissored panel shaders");
            D3D11_DEPTH_STENCIL_DESC dd{};
            dd.DepthEnable = FALSE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            const float size[4] = {float(g.w), float(g.h), 0, 0};
            D3D11_SUBRESOURCE_DATA init{size, 0, 0};
            D3D11_RASTERIZER_DESC rd{};
            rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE; rd.ScissorEnable = TRUE;
            expect(lines_code && text_code && line_ps && text_ps &&
                   SUCCEEDED(g.device->CreateVertexShader(lines_code->GetBufferPointer(), lines_code->GetBufferSize(), nullptr, &g.vs_lines)) &&
                   SUCCEEDED(g.device->CreateVertexShader(text_code->GetBufferPointer(), text_code->GetBufferSize(), nullptr, &g.vs_text)) &&
                   SUCCEEDED(g.device->CreatePixelShader(line_ps->GetBufferPointer(), line_ps->GetBufferSize(), nullptr, &g.ps_line)) &&
                   SUCCEEDED(g.device->CreatePixelShader(text_ps->GetBufferPointer(), text_ps->GetBufferSize(), nullptr, &g.ps_text)) &&
                   SUCCEEDED(g.device->CreateDepthStencilState(&dd, &g.overlay_depth_off)) &&
                   SUCCEEDED(g.device->CreateBuffer(&bd, &init, &g.overlay_cb)) &&
                   SUCCEEDED(g.device->CreateRasterizerState(&rd, &g.hud_scissor)), "overlay fixture shaders and states");
            D3D11_RASTERIZER_DESC sr{};
            sr.FillMode = D3D11_FILL_SOLID; sr.CullMode = D3D11_CULL_NONE; sr.DepthClipEnable = TRUE;
            expect(SUCCEEDED(g.device->CreateRasterizerState(&sr, &g.scene_rs)), "scene state with no culling (the quads face away)");
            g.overlays = true;
        }
        auto& st = na::settings::Get();
        st.j2_fold_mode.store(static_cast<uint32_t>(overlay_level));
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        const auto root = std::filesystem::path(temp) / (L"NA-overlay-jitter-" + std::to_wstring(GetCurrentProcessId()));
        int frame = 0;
        const auto run = [&](int n) { for (int i = 0; i < n; ++i, ++frame) { Pump(); Scene(g.ctx.Get(), g, frame); Post(g.ctx.Get(), g, true); g.swap->Present(0, 0); } };
        // Distance from a pixel centre to the nearest overlay line, and whether it is a label pixel (as the shaders draw them).
        const auto near_line = [&](int x, int y) {
            const float W = float(g.w), H = float(g.h);
            for (UINT l = 0; l < Game::kOverlayLines; ++l) {
                const float angle = 0.30f + 0.093f * float(l);
                const float dx = std::cos(angle), dy = std::sin(angle);
                const float sx = (-0.95f + 0.11f * float(l) + 1.f) * .5f * W, sy = (1.f - (-0.85f + 1.f) * .5f) * H; // NDC -> pixels
                const float vx = dx * .5f * W, vy = -dy * .5f * H; // the direction in pixels (y down)
                const float len = std::sqrt(vx * vx + vy * vy);
                const float px = float(x) + .5f - sx, py = float(y) + .5f - sy;
                const float t = (px * vx + py * vy) / len;
                if (t < -2.f || t > 1.9f * len + 2.f) continue;
                if (std::fabs((px * vy - py * vx) / len) < 2.5f) return true;
            }
            return false;
        };
        const auto near_text = [&](int x, int y) { return x >= 60 && x < 64 + 48 * 5 + 4 && y >= 116 && y < 121 + 6 * 7 + 4; };
        const auto in_panel = [&](int x, int y) {
            return x >= Game::kPanelScissor[0] + 1 && x < Game::kPanelScissor[2] - 1 && y >= Game::kPanelScissor[1] + 1 && y < Game::kPanelScissor[3] - 1;
        };
        struct Measure { uint64_t edges = 0, still = 0, line_edges = 0, line_still = 0, text_edges = 0, text_still = 0, other_edges = 0, other_still = 0;
                         uint64_t panel_edges = 0, panel_still = 0, panel_outside = 0; int pairs = 0; bool hud_same = true; };
        const auto measure = [&](const std::filesystem::path& cap) {
            Measure m;
            // Each frame's scene picture before DLSS (the folded levels' full-size input is "source_color").
            const char* plane = overlay_level ? "source_color" : "color";
            std::vector<std::vector<uint8_t>> frames;
            std::vector<uint64_t> ordinals;
            std::vector<std::vector<uint8_t>> presents;
            uint32_t pitch = 0, bpp = 0, ppitch = 0;
            for (int i = 0; i < 64; ++i) {
                wchar_t name[32];
                swprintf_s(name, L"%05d-meta.json", i);
                FILE* f = nullptr;
                if (_wfopen_s(&f, (cap / name).c_str(), L"rb") || !f) break;
                std::string json;
                char buf[4096];
                for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) json.append(buf, n);
                std::fclose(f);
                const auto field = [&](const char* plane_name, const char* key) -> uint32_t {
                    const size_t at = json.find(std::string("\"name\": \"") + plane_name + "\"");
                    if (at == std::string::npos) return 0;
                    const size_t k = json.find(std::string("\"") + key + "\": ", at);
                    return k == std::string::npos ? 0 : uint32_t(std::strtoul(json.c_str() + k + std::strlen(key) + 4, nullptr, 10));
                };
                pitch = field(plane, "row_pitch");
                bpp = pitch && g.w ? pitch / g.w : 0;
                ppitch = field("present", "row_pitch");
                const size_t o = json.find("\"present_ordinal\": ");
                ordinals.push_back(o == std::string::npos ? 0 : std::strtoull(json.c_str() + o + 20, nullptr, 10));
                const auto read = [&](const char* which, uint32_t p) {
                    std::vector<uint8_t> b;
                    wchar_t file[64];
                    swprintf_s(file, L"%05d-%hs.raw", i, which);
                    FILE* r = nullptr;
                    if (!_wfopen_s(&r, (cap / file).c_str(), L"rb") && r) { b.resize(size_t(p) * g.h); b.resize(std::fread(b.data(), 1, b.size(), r)); std::fclose(r); }
                    return b;
                };
                frames.push_back(read(plane, pitch));
                presents.push_back(read("present", ppitch));
            }
            if (frames.size() < 2 || !bpp) return m;
            const auto& f0 = frames[0];
            const auto px = [&](const std::vector<uint8_t>& f, int x, int y) { return f.data() + size_t(y) * pitch + size_t(x) * bpp; };
            std::vector<uint8_t> same(size_t(g.w) * g.h, 1), edge(size_t(g.w) * g.h, 0);
            for (int y = 1; y + 1 < int(g.h); ++y)
                for (int x = 1; x + 1 < int(g.w); ++x)
                    edge[size_t(y) * g.w + x] = uint8_t((std::memcmp(px(f0, x, y), px(f0, x + 1, y), bpp) ? 1 : 0) | (std::memcmp(px(f0, x, y), px(f0, x, y + 1), bpp) ? 2 : 0));
            for (size_t i = 0; i + 1 < frames.size(); ++i) {
                if (ordinals[i + 1] != ordinals[i] + 1 || frames[i].size() < size_t(pitch) * g.h || frames[i + 1].size() < size_t(pitch) * g.h) continue;
                ++m.pairs;
                for (int y = 0; y < int(g.h); ++y)
                    for (int x = 0; x < int(g.w); ++x)
                        if (std::memcmp(px(frames[i], x, y), px(frames[i + 1], x, y), bpp)) same[size_t(y) * g.w + x] = 0;
                // The interface (after the hand-over) is on the game's grid: its panel is the same in every frame.
                if (presents[i].size() == presents[i + 1].size() && ppitch)
                    for (int y = kHudRect[1] + 2; y < kHudRect[3] - 2; ++y)
                        if (std::memcmp(presents[i].data() + size_t(y) * ppitch + size_t(kHudRect[0] + 2) * 4,
                                        presents[i + 1].data() + size_t(y) * ppitch + size_t(kHudRect[0] + 2) * 4, size_t(kHudRect[2] - kHudRect[0] - 4) * 4))
                            m.hud_same = false;
            }
            // The panel's own colour (pure green) never lands outside its scissor, in any frame (R11G11B10: red and blue
            // zero, green set; other formats: the same test on the first and third channels).
            for (const auto& f : frames)
                for (int y = 0; y < int(g.h) && f.size() >= size_t(pitch) * g.h; ++y)
                    for (int x = 1040; x < int(g.w); ++x) {
                        if (x >= Game::kPanelScissor[0] && x < Game::kPanelScissor[2] && y >= Game::kPanelScissor[1] && y < Game::kPanelScissor[3]) continue;
                        uint32_t v = 0;
                        std::memcpy(&v, px(f, x, y), 4);
                        const bool green = bpp == 4 ? ((v & 0x7FF) == 0 && (v >> 22) == 0 && ((v >> 11) & 0x7FF) != 0) : false;
                        m.panel_outside += green ? 1 : 0;
                    }
            for (int y = 1; y + 1 < int(g.h); ++y)
                for (int x = 1; x + 1 < int(g.w); ++x) {
                    const size_t i = size_t(y) * g.w + x;
                    if (!edge[i] || (x >= kHudRect[0] && x < kHudRect[2] && y >= kHudRect[1] && y < kHudRect[3])) continue;
                    // An edge stays still when nothing within one pixel of it changes in any frame pair. (A single pixel is
                    // not enough: the eight jitter phases span 0.81 pixel, so an edge whose position falls in the rest of
                    // the pixel never flips its own pixel, while the pixel beside it does.)
                    bool s = true;
                    for (int dy = -1; dy <= 1 && s; ++dy)
                        for (int dx = -1; dx <= 1 && s; ++dx) s = same[i + size_t(ptrdiff_t(dy) * ptrdiff_t(g.w)) + size_t(ptrdiff_t(dx))] != 0;
                    ++m.edges; m.still += s;
                    if (in_panel(x, y)) { ++m.panel_edges; m.panel_still += s; }
                    else if (near_text(x, y)) { ++m.text_edges; m.text_still += s; }
                    else if (near_line(x, y)) { ++m.line_edges; m.line_still += s; }
                    else { ++m.other_edges; m.other_still += s; }
                }
            return m;
        };
        const auto pct = [](uint64_t a, uint64_t b) { return b ? 100.0 * double(a) / double(b) : 0.0; };
        Measure result[2];
        for (int variant = 0; variant < 2; ++variant) {
            const bool after = variant == 1;
            na::native11::ForceSceneOverlayJitterForTests(after ? 1 : 0);
            run(180); // DLAA starts; a level is prepared and shown
            const auto cap = root / (after ? L"after" : L"before");
            std::filesystem::create_directories(cap);
            na::capture::j2::Request(16, cap.c_str());
            run(60);
            Sleep(1500); // the capture writer finishes on its own thread
            result[variant] = measure(cap);
            const auto& m = result[variant];
            std::printf("%s, %s (%s): %d frame pairs; edges never moved by the jitter: all %.2f %% (%llu of %llu), lines %.2f %% (%llu of %llu), "
                        "labels %.2f %% (%llu of %llu), scissored panel %.2f %% (%llu of %llu; its colour outside the scissor %llu pixels), terrain %.2f %%; "
                        "interface identical in every frame: %s; this frame's overlay meshes moved %u, "
                        "scene meshes left on the game's grid %u\n  frames: %ls\n",
                        level_name, after ? "after" : "before", after ? "jitter/scene-overlays" : "jitter/depth-tested-only", m.pairs,
                        pct(m.still, m.edges), (unsigned long long)m.still, (unsigned long long)m.edges,
                        pct(m.line_still, m.line_edges), (unsigned long long)m.line_still, (unsigned long long)m.line_edges,
                        pct(m.text_still, m.text_edges), (unsigned long long)m.text_still, (unsigned long long)m.text_edges,
                        pct(m.panel_still, m.panel_edges), (unsigned long long)m.panel_still, (unsigned long long)m.panel_edges,
                        (unsigned long long)m.panel_outside,
                        pct(m.other_still, m.other_edges), m.hud_same ? "yes" : "NO",
                        na::native11::OverlayMeshesMoved(), na::native11::UnjitteredSceneMeshes(), cap.c_str());
        }
        na::native11::ForceSceneOverlayJitterForTests(-1);
        expect(result[0].pairs >= 4 && result[1].pairs >= 4, "consecutive frame pairs were captured before and after");
        expect(pct(result[0].line_still, result[0].line_edges) > 50.0, "before: the lines' edges stay on the game's grid (the HoI4 symptom reproduced)");
        expect(pct(result[1].still, result[1].edges) < 1.0, "after: under 1 % of the picture's edges stay still while the jitter moves");
        expect(pct(result[1].line_still, result[1].line_edges) < 1.0 && pct(result[1].text_still, result[1].text_edges) < 1.0,
               "after: the lines and labels move with the jitter");
        expect(result[0].hud_same && result[1].hud_same, "the interface after the hand-over stays exactly on the game's grid");
        expect(pct(result[1].panel_still, result[1].panel_edges) < 1.0, "after: a scissored panel before the hand-over moves with the jitter");
        expect(result[0].panel_outside == 0 && result[1].panel_outside == 0, "the scissored panel stays inside its scissor (the scissor is the game's)");
        DeleteFileW(ini.c_str());
        std::printf(g_failed ? "FAIL: %d overlay check(s) failed\n" : "PASS: overlays drawn without the depth test take the jitter\n", g_failed);
        std::fflush(stdout);
        ExitProcess(g_failed ? 1u : 0u);
    }
    if (fold_level) {
        // The game draws at full size; from the level's first shown frame on, its main scene draws are shaded
        // in blocks (pixel-shader invocations: one half / one quarter), the shadow pass, the tone map and the
        // HUD are not, DLSS rebuilds the full picture from the kept pixels, and DLAA stays on screen until
        // the level has proven itself (no frozen frame). Then back to DLAA.
        na::native11::SetJitterRoute(na::native11::JitterRoute::Viewport);
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE; rd.ScissorEnable = TRUE;
        expect(SUCCEEDED(g.device->CreateRasterizerState(&rd, &g.hud_scissor)), "HUD panel state");
        rd.ScissorEnable = FALSE;
        expect(SUCCEEDED(g.device->CreateRasterizerState(&rd, &g.scene_rs)), "scene state with no culling");
        ComPtr<ID3D11Query> main_q, post_q;
        const D3D11_QUERY_DESC qd{D3D11_QUERY_PIPELINE_STATISTICS, 0};
        expect(SUCCEEDED(g.device->CreateQuery(&qd, &main_q)) && SUCCEEDED(g.device->CreateQuery(&qd, &post_q)), "pipeline statistics queries");
        g.main_stats = main_q.Get();
        {
            auto ps1 = Compile("ps_scene_coords", "ps_5_0"), ps2 = Compile("ps_hud_coords", "ps_5_0");
            expect(ps1 && ps2 && SUCCEEDED(g.device->CreatePixelShader(ps1->GetBufferPointer(), ps1->GetBufferSize(), nullptr, &g.ps_scene_main)) &&
                   SUCCEEDED(g.device->CreatePixelShader(ps2->GetBufferPointer(), ps2->GetBufferSize(), nullptr, &g.ps_hud_main)), "pixel-dependent shading for the fixture");
        }
        ComPtr<ID3D11Buffer> count_buf, count_stg;
        {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = 16; bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS; bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = DXGI_FORMAT_R32_TYPELESS; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; ud.Buffer.NumElements = 4; ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
            D3D11_BUFFER_DESC sd2 = bd;
            sd2.BindFlags = 0; sd2.MiscFlags = 0; sd2.Usage = D3D11_USAGE_STAGING; sd2.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            expect(SUCCEEDED(g.device->CreateBuffer(&bd, nullptr, &count_buf)) && SUCCEEDED(g.device->CreateUnorderedAccessView(count_buf.Get(), &ud, &g.count_uav)) &&
                   SUCCEEDED(g.device->CreateBuffer(&sd2, nullptr, &count_stg)), "the shading counter");
        }
        D3D11_TEXTURE2D_DESC cd{};
        g.color->GetDesc(&cd);
        cd.Usage = D3D11_USAGE_STAGING; cd.BindFlags = 0; cd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; cd.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> stg;
        expect(SUCCEEDED(g.device->CreateTexture2D(&cd, nullptr, &stg)), "picture read-back");
        std::vector<float> prev, cur;
        // The picture the game reads after DLSS (the scene colour, written back): RGB as floats.
        const auto read_picture = [&](std::vector<float>& out) {
            out.assign(size_t(g.w) * g.h * 3, 0.f);
            g.ctx->CopyResource(stg.Get(), g.color.Get());
            D3D11_MAPPED_SUBRESOURCE m{};
            if (FAILED(g.ctx->Map(stg.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
            bool finite = true;
            for (UINT y = 0; y < g.h; ++y) {
                const auto* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch);
                for (UINT x = 0; x < g.w; ++x) {
                    DirectX::PackedVector::XMFLOAT3PK pk(row[x]);
                    DirectX::XMFLOAT3 f;
                    DirectX::XMStoreFloat3(&f, DirectX::PackedVector::XMLoadFloat3PK(&pk));
                    float* o = &out[(size_t(y) * g.w + x) * 3];
                    o[0] = f.x; o[1] = f.y; o[2] = f.z;
                    finite = finite && std::isfinite(f.x) && std::isfinite(f.y) && std::isfinite(f.z) && f.x < 64.f && f.y < 64.f && f.z < 64.f;
                }
            }
            g.ctx->Unmap(stg.Get(), 0);
            return finite;
        };
        const auto diff = [&](const std::vector<float>& a, const std::vector<float>& b) {
            double s = 0;
            for (size_t i = 0; i < a.size() && i < b.size(); ++i) s += std::fabs(double(a[i]) - double(b[i]));
            return a.empty() ? 0.0 : s / double(a.size());
        };
        struct Sample { UINT64 scene = 0, post = 0; double ms = 0, change = 0; bool finite = true; };
        LARGE_INTEGER hz{};
        QueryPerformanceFrequency(&hz);
        const auto run = [&](int frame) {
            Sample s;
            LARGE_INTEGER t0{}, t1{};
            QueryPerformanceCounter(&t0);
            Pump();
            const UINT zero[4] = {};
            g.ctx->ClearUnorderedAccessViewUint(g.count_uav.Get(), zero);
            Scene(g.ctx.Get(), g, frame);
            g.ctx->Begin(post_q.Get());
            Post(g.ctx.Get(), g, true);
            g.ctx->End(post_q.Get());
            g.ctx->CopyResource(count_stg.Get(), count_buf.Get());
            g.swap->Present(0, 0);
            QueryPerformanceCounter(&t1);
            s.ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(hz.QuadPart);
            D3D11_QUERY_DATA_PIPELINE_STATISTICS a{}, b{};
            while (g.ctx->GetData(main_q.Get(), &a, sizeof a, 0) == S_FALSE) Sleep(0);
            while (g.ctx->GetData(post_q.Get(), &b, sizeof b, 0) == S_FALSE) Sleep(0);
            s.scene = a.PSInvocations; s.post = b.PSInvocations;
            {
                D3D11_MAPPED_SUBRESOURCE cm{};
                if (SUCCEEDED(g.ctx->Map(count_stg.Get(), 0, D3D11_MAP_READ, 0, &cm))) {
                    const auto* v = static_cast<const uint32_t*>(cm.pData);
                    s.scene = v[0]; s.post = v[1]; // the main scene's and the HUD's own shading, counted by the shaders
                    g.ctx->Unmap(count_stg.Get(), 0);
                }
            }
            s.finite = read_picture(cur);
            s.change = prev.empty() ? 0.0 : diff(cur, prev);
            prev.swap(cur);
            return s;
        };
        const auto shown_now = [] {
            char status[512]{};
            na::native_d3d11::FoldStatus(status, sizeof status);
            return std::string(status);
        };
        auto& st = na::settings::Get();
        // Our own video memory (diag/vram_ledger.hpp) and the DLSS features held, at each step.
        const auto features_held = [] { const auto ds = na::dlss::GetStats(); return ds.features_created - ds.features_released; };
        const auto print_ours = [&](const char* when) {
            static char line[1536];
            na::vram::FormatLine(line, sizeof line);
            const auto cf = na::j2::compute_flow::GetStats();
            double process_mb = 0;
            ComPtr<IDXGIDevice> dd;
            ComPtr<IDXGIAdapter> da;
            ComPtr<IDXGIAdapter3> a3;
            DXGI_QUERY_VIDEO_MEMORY_INFO mi{};
            if (SUCCEEDED(g.device.As(&dd)) && SUCCEEDED(dd->GetAdapter(&da)) && SUCCEEDED(da.As(&a3)) &&
                SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mi)))
                process_mb = double(mi.CurrentUsage) / 1048576.0;
            std::printf("  vram (%s): whole process %.1f MB | ours: %s | DLSS features held %llu | kept motion pictures: %u made, at most %u in use at once\n",
                        when, process_mb, line, static_cast<unsigned long long>(features_held()), cf.image_slots, cf.images_busy_peak);
        };
        const char* level_name = fold_level == 2 ? "Performance" : "Quality";
        int frame = 0;
        // A. DLAA.
        st.j2_fold_mode.store(0);
        double dlaa_ms_sum = 0, dlaa_change = 0;
        UINT64 dlaa_scene = 0, dlaa_post = 0;
        int dlaa_n = 0;
        double start_max_ms = 0; // the first frames: DLAA starts, and the levels' DLSS is made with it
        for (; frame < 60; ++frame) {
            const Sample s = run(frame);
            if (frame < 10) start_max_ms = (std::max)(start_max_ms, s.ms);
            if (frame >= 30) { dlaa_ms_sum += s.ms; dlaa_scene += s.scene; dlaa_post += s.post; dlaa_change += s.change; ++dlaa_n; }
        }
        const double dlaa_ms = dlaa_ms_sum / dlaa_n;
        {   // Video memory this process holds after DLAA has run (with or without the levels' DLSS made beside it).
            ComPtr<IDXGIDevice> dd;
            ComPtr<IDXGIAdapter> da;
            ComPtr<IDXGIAdapter3> a3;
            DXGI_QUERY_VIDEO_MEMORY_INFO mi{};
            if (SUCCEEDED(g.device.As(&dd)) && SUCCEEDED(dd->GetAdapter(&da)) && SUCCEEDED(da.As(&a3)) &&
                SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mi)))
                std::printf("  video memory held on DLAA at %ux%u (whole process): %.1f MB\n", g.w, g.h, double(mi.CurrentUsage) / 1048576.0);
        }
        print_ours("DLAA");
        expect(features_held() == 1, "on DLAA exactly one DLSS feature is held");
        // B. The level asked for: DLAA stays on screen while it is readied, then the switch.
        st.j2_fold_mode.store(fold_level);
        const int asked_at = frame;
        int switched_at = -1;
        double switch_max_ms = 0;
        bool finite = true;
        for (; frame < asked_at + 200 && switched_at < 0; ++frame) {
            const Sample s = run(frame);
            if (s.ms > dlaa_ms + 3.0) std::printf("  frame %d (waiting period): %.1f ms\n", frame, s.ms);
            switch_max_ms = (std::max)(switch_max_ms, s.ms);
            finite = finite && s.finite;
            if (shown_now().find(std::string("On screen: ") + level_name) != std::string::npos) switched_at = frame;
        }
        const std::string status_after = shown_now();
        const auto made_after_b = na::dlss::GetPrepareStats(); // the level's feature: this session's first of its kind
        // C. The level on screen.
        double fold_ms_sum = 0, fold_change = 0, switch_change = 0;
        UINT64 fold_scene = 0, fold_post = 0;
        int fold_n = 0;
        for (int k = 0; k < 90 && switched_at >= 0; ++k, ++frame) {
            const Sample s = run(frame);
            finite = finite && s.finite;
            if (k < 3) switch_change = (std::max)(switch_change, s.change);
            if (k >= 30) { fold_ms_sum += s.ms; fold_scene += s.scene; fold_post += s.post; fold_change += s.change; ++fold_n; }
        }
        print_ours(level_name);
        expect(features_held() == 1, "on a level exactly one DLSS feature is held");
        static char line11[8192]{};
        na::native_d3d11::FormatStatus(line11, sizeof line11);
        const char* gpu = std::strstr(line11, "gpu_ms{");
        const char* f11 = std::strstr(line11, "fold11{");
        // E. The other level, straight from this one.
        const uint32_t other_level = fold_level == 1 ? 2u : 1u;
        const char* other_name = other_level == 2 ? "Performance" : "Quality";
        st.j2_fold_mode.store(other_level);
        double other_max_ms = 0;
        int other_at = -1;
        for (int k = 0; k < 200 && other_at < 0; ++k, ++frame) {
            const Sample s = run(frame);
            other_max_ms = (std::max)(other_max_ms, s.ms);
            if (s.ms > dlaa_ms + 3.0) std::printf("  to %s, frame +%d: %.1f ms\n", other_name, k, s.ms);
            if (shown_now().find(std::string("On screen: ") + other_name) != std::string::npos) other_at = k;
        }
        if (other_at < 0) std::printf("  %s never came on screen: %s (DLSS: %s, NGX result 0x%08x)\n", other_name, shown_now().c_str(), na::dlss::GetStats().reason,
                                      static_cast<unsigned>(na::dlss::GetStats().last_ngx_result));
        for (int k = 0; k < 20; ++k, ++frame) other_max_ms = (std::max)(other_max_ms, run(frame).ms);
        const auto made_after_e = na::dlss::GetPrepareStats();
        std::printf("  first creations (made off the frame): %s %.1f ms, %s %.1f ms\n", level_name, made_after_b.made ? made_after_b.last_ms : -1.0, other_name,
                    made_after_e.made > made_after_b.made ? made_after_e.last_ms : -1.0);
        print_ours(other_level == 2 ? "Quality then Performance" : "Performance then Quality");
        // D. Back to DLAA.
        st.j2_fold_mode.store(0);
        UINT64 back_scene = 0;
        int back_n = 0;
        double back_max_ms = 0;
        for (int k = 0; k < 40; ++k, ++frame) {
            const Sample s = run(frame);
            if (k < 20) back_max_ms = (std::max)(back_max_ms, s.ms);
            if (k < 20 && s.ms > dlaa_ms + 3.0) std::printf("  back to DLAA, frame +%d: %.1f ms\n", k, s.ms);
            if (k >= 20) { back_scene += s.scene; ++back_n; }
        }
        {   // A few seconds on DLAA: what the levels held is given back (planes, folded motion context).
            const ULONGLONG until = GetTickCount64() + 6500;
            double later_max_ms = 0;
            const ULONGLONG from = GetTickCount64();
            while (GetTickCount64() < until) {
                const double ms = run(frame).ms;
                if (ms > dlaa_ms + 20.0) std::printf("  slow frame %.1f ms at %.2f s after DLAA came back\n", ms, (GetTickCount64() - from) / 1000.0);
                later_max_ms = (std::max)(later_max_ms, ms);
                ++frame;
            }
            print_ours("back on DLAA, 6.5 s later");
            expect(shown_now().find("On screen: DLAA") != std::string::npos, "DLAA is on screen again (its feature made off the frame)");
            std::printf("  longest frame in those 6.5 s: %.1f ms\n", later_max_ms);
            expect(features_held() == 1, "back on DLAA exactly one DLSS feature is held");
            char planes_q[48], planes_p[48];
            std::snprintf(planes_q, sizeof planes_q, "fold_planes_dx11_Quality");
            std::snprintf(planes_p, sizeof planes_p, "fold_planes_dx11_Performance");
            expect(na::vram::Get(planes_q) == 0 && na::vram::Get(planes_p) == 0, "back on DLAA the levels' folded planes are released");
            expect(later_max_ms < dlaa_ms + 20.0, "no slow frame while the levels' memory is released");
        }
        {   // NA_TEST_FOLD_CYCLES=N: N more switch-and-free cycles (level, the other level, back to DLAA and 6.5 s for the
            // memory to be given back); every frame more than 20 ms over steady is printed with what was being released.
            wchar_t cv[16]{};
            const int cycles = GetEnvironmentVariableW(L"NA_TEST_FOLD_CYCLES", cv, 16) ? _wtoi(cv) : 0;
            double worst = 0;
            int slow = 0;
            for (int cycle = 0; cycle < cycles; ++cycle) {
                const auto phase = [&](uint32_t mode, const char* name, ULONGLONG ms_after) {
                    st.j2_fold_mode.store(mode);
                    const ULONGLONG start = GetTickCount64();
                    ULONGLONG until = start + 4000;
                    bool on = false;
                    while (GetTickCount64() < until) {
                        const ULONGLONG at = GetTickCount64();
                        const double ms = run(frame).ms;
                        ++frame;
                        worst = (std::max)(worst, ms - dlaa_ms);
                        if (ms > dlaa_ms + 20.0) {
                            ++slow;
                            std::printf("  cycle %d, %s: slow frame %.1f ms (+%.1f) at tick %llu, %.2f s into the phase\n", cycle, name, ms, ms - dlaa_ms,
                                        static_cast<unsigned long long>(at), (at - start) / 1000.0);
                        }
                        if (!on && (mode == 0 ? shown_now().find("On screen: DLAA") != std::string::npos
                                              : shown_now().find(std::string("On screen: ") + (mode == 2 ? "Performance" : "Quality")) != std::string::npos)) {
                            on = true;
                            until = GetTickCount64() + ms_after;
                        }
                    }
                    for (const auto& e : na::diag::release_trace::Take())
                        if (e.ms > 4.0) std::printf("    %s %.1f ms at tick %llu\n", e.what.c_str(), e.ms, e.tick);
                };
                phase(fold_level, level_name, 700);
                phase(fold_level == 1 ? 2u : 1u, fold_level == 1 ? "Performance" : "Quality", 700);
                phase(0, "DLAA and memory given back", 6500);
            }
            if (cycles) {
                std::printf("  %d switch-and-free cycles: worst frame +%.1f ms over steady (%.1f ms), %d frames more than 20 ms over\n", cycles, worst, dlaa_ms, slow);
                expect(slow == 0, "no frame more than 20 ms over steady in the switch-and-free cycles");
            }
        }
        std::printf("  switches (longest frame, steady DLAA frame %.1f ms): first frames %.1f ms (DLAA made); DLAA -> %s %.1f ms; %s -> %s %.1f ms (on screen after %d frames); -> DLAA %.1f ms\n",
                    dlaa_ms, start_max_ms, level_name, switch_max_ms, level_name, other_name, other_max_ms, other_at, back_max_ms);
        {
            const auto ps = na::dlss::GetPrepareStats();
            std::printf("  DLSS: levels warmed at start %s (Quality %.1f ms, Performance %.1f ms, released %.1f ms, %.1f ms in all, first frames waited %.1f ms); "
                        "features made off the frame %llu, last %.1f ms, longest %.1f ms; released off the frame %llu, longest release %.1f ms, "
                        "size report after a release longest %.1f ms\n", ps.warmed ? "yes" : "NO", ps.warm_quality_ms, ps.warm_performance_ms,
                        ps.warm_release_ms, ps.warm_total_ms, ps.warm_waited_ms, static_cast<unsigned long long>(ps.made), ps.last_ms, ps.longest_ms,
                        static_cast<unsigned long long>(ps.released_off_frame), ps.release_longest_ms, ps.size_query_longest_ms);
        }
        // One DLSS feature at a time (coordinator 2026-11-27): the new feature is made off the frame, and the switch frame waits
        // only for what is left of NVIDIA's work. The bar is the coordinator's: no switch more than 20 ms over a steady frame.
        const double margin = 20.0;
        const auto cs = na::native11::GetCoarseShadingStats();
        const double r_scene = fold_n ? double(fold_scene) / fold_n / (double(dlaa_scene) / dlaa_n) : 0;
        const double r_post = fold_n ? double(fold_post) / fold_n / (double(dlaa_post) / dlaa_n) : 0;
        const double r_back = back_n ? double(back_scene) / back_n / (double(dlaa_scene) / dlaa_n) : 0;
        std::printf("fold %s on the DX11 test game (%ux%u):\n"
                    "  pixel-shader invocations per frame: main scene DLAA %.0f -> %s %.0f (x%.3f), HUD %.0f -> %.0f (x%.3f), back on DLAA x%.3f\n"
                    "  waiting period: asked at frame %d, on screen at frame %d (%d frames); frame time DLAA %.1f ms, slowest while readied %.1f ms, on the level %.1f ms\n"
                    "  picture change between frames (mean |d| RGB): DLAA %.5f, at the switch %.5f, on the level %.5f; finite %d\n"
                    "  menu: %s\n  %.*s\n  %.*s\n  coarse: draws %llu sets %llu restores %llu before_lists %llu failures %llu frames %llu\n",
                    level_name, g.w, g.h, double(dlaa_scene) / dlaa_n, level_name, fold_n ? double(fold_scene) / fold_n : 0.0, r_scene,
                    double(dlaa_post) / dlaa_n, fold_n ? double(fold_post) / fold_n : 0.0, r_post, r_back,
                    asked_at, switched_at, switched_at - asked_at, dlaa_ms, switch_max_ms, fold_n ? fold_ms_sum / fold_n : 0.0,
                    dlaa_change / dlaa_n, switch_change, fold_n ? fold_change / fold_n : 0.0, finite ? 1 : 0, status_after.c_str(),
                    gpu ? int(std::strcspn(gpu, "}") + 1) : 0, gpu ? gpu : "", f11 ? int(std::strcspn(f11, "}") + 1) : 0, f11 ? f11 : "",
                    static_cast<unsigned long long>(cs.draws), static_cast<unsigned long long>(cs.sets), static_cast<unsigned long long>(cs.restores),
                    static_cast<unsigned long long>(cs.before_lists), static_cast<unsigned long long>(cs.failures), static_cast<unsigned long long>(cs.frames));
        expect(switched_at > asked_at, "DLAA stays on screen until the level's DLSS feature is made (at least one frame)");
        expect(switch_max_ms < dlaa_ms + margin, "no switch frame more than 20 ms over a steady frame, DLAA to the level");
        expect(other_at >= 0 && other_max_ms < dlaa_ms + margin, "no slow frame from one level to the other");
        expect(back_max_ms < dlaa_ms + margin, "no slow frame back to DLAA");
        const bool quarter = fold_level == 2;
        expect(quarter ? (r_scene > 0.22 && r_scene < 0.34) : (r_scene > 0.47 && r_scene < 0.60),
               quarter ? "the main scene's draws run about a quarter of the pixel shading" : "the main scene's draws run about half of the pixel shading");
        expect(r_post > 0.999 && r_post < 1.001, "the HUD keeps full-rate shading");
        expect(r_back > 0.999 && r_back < 1.001, "back on DLAA the scene is shaded per pixel again");
        expect(finite && fold_n && fold_change / fold_n < 3.0 * (dlaa_change / dlaa_n) + 0.002 && switch_change < 4.0 * (dlaa_change / dlaa_n) + 0.004,
               "DLSS's folded picture is sane: it changes from frame to frame like DLAA's, with no jump at the switch");
        expect(gpu && std::strstr(gpu, "samples=0") == nullptr, "per-stage GPU ms are logged on the DX11 route");
        expect(cs.failures == 0 && cs.draws > 0, "coarse shading was set with no failure");
        if (fold_measured) {
            const char* ff = f11 ? std::strstr(f11, "folded_flow=") : nullptr;
            const char* sm = f11 ? std::strstr(f11, "sampled_motion=") : nullptr;
            { const auto fs = na::j2::compute_flow::GetStats(); std::printf("  folded working set: prepared %llu, failed %llu; kept motion pictures %u, at most %u in use at once\n", (unsigned long long)fs.prepared, (unsigned long long)fs.prepare_failed, fs.image_slots, fs.images_busy_peak); }
            expect(sm && std::strtoull(sm + 15, nullptr, 10) >= 30, "with no camera the motion is sampled: only the kept pixels are decided");
            expect(ff && std::strtoull(ff + 12, nullptr, 10) >= 30, "with no camera the motion search runs on the folded picture");
        }
        DeleteFileW(ini.c_str());
        if (g_failed) std::printf("FAIL: %d fold check(s) failed\n", g_failed);
        else std::printf("PASS: DLSS %s on the native DX11 route\n", level_name);
        std::fflush(stdout);
        ExitProcess(g_failed ? 1u : 0u);
    }
    if (camera_off_start) {
        // Cold start: no earlier camera-backed frame, no hand-written next jitter, no warm flow pair.
        // Run the production host and real NGX (also via NA_DLAA_COMPANION=1). Camera bytes exist in
        // this synthetic game's rendering, but discovery must never read or publish them.
        g_no_camera_phase = 1;
        na::native11::SetJitterRoute(na::native11::JitterRoute::Viewport);
        unsigned raster_samples = 0;
        if (replay_handoff) {
            na::settings::Get().j2_draw_replay.store(1);
            na::settings::Get().j2_replay_budget.store(0);
            na::settings::Get().j2_replay_movers.store(0);
        }
        if (first_setup) {
            // Cort's Crusader Kings III run of 2026-11-27: DLAA's first start did seconds of set-up on the
            // game's drawing thread and the game stopped drawing. Here the "game" keeps drawing frames
            // from a cold start; no single DLAA call may hold its thread, and DLAA must then arrive.
            LARGE_INTEGER hz, start, t0, t1;
            QueryPerformanceFrequency(&hz); QueryPerformanceCounter(&start);
            const auto ms_since = [&](const LARGE_INTEGER& from) {
                LARGE_INTEGER now; QueryPerformanceCounter(&now);
                return double(now.QuadPart - from.QuadPart) * 1000.0 / double(hz.QuadPart);
            };
            double longest_frame_ms = 0, first_delivery_ms = -1;
            int frames = 0, frames_before = 0;
            const auto delivered0 = evaluations;
            while (ms_since(start) < 120000.0 && evaluations < delivered0 + 30) {
                Pump();
                QueryPerformanceCounter(&t0);
                Scene(g.ctx.Get(), g, frames, false, false);
                Post(g.ctx.Get(), g, true);
                g.swap->Present(0, 0);
                QueryPerformanceCounter(&t1);
                longest_frame_ms = (std::max)(longest_frame_ms, double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(hz.QuadPart));
                ++frames;
                if (evaluations == delivered0) ++frames_before;
                else if (first_delivery_ms < 0) first_delivery_ms = ms_since(start);
            }
            char wide[8192]{};
            na::native_d3d11::FormatStatus(wide, sizeof wide);
            const unsigned long long longest_call = na::native_d3d11::LongestCallMs();
            std::printf("first set-up beside the game: frames drawn=%d, frames before DLAA=%d, DLAA first delivered after %.0f ms, "
                        "delivered=%llu, longest wait of the game's thread in one DLAA call=%llu ms, longest whole frame=%.0f ms | %s\n",
                        frames, frames_before, first_delivery_ms, static_cast<unsigned long long>(evaluations - delivered0), longest_call,
                        longest_frame_ms, wide);
            expect(evaluations >= delivered0 + 30, "DLAA arrives after first-time set-up and keeps delivering");
            expect(frames_before >= 1, "the game drew frames of its own while set-up ran (set-up was not inside the first call)");
            expect(longest_call < 500, "no DLAA call held the game's thread for half a second (Windows marks a game not responding at 5 s)");
            expect(longest_frame_ms < 1000.0, "no whole frame took a second during or after first-time set-up");
            if (g_failed) std::printf("FAIL: %d first-set-up check(s) failed\n", g_failed);
            else std::printf("PASS: first-time set-up runs beside the game\n");
            std::fflush(stdout);
            ExitProcess(g_failed ? 1u : 0u);
        }
        for (int frame = 0; frame < 48; ++frame) {
            Pump();
            const auto before = g_seen.calls;
            Scene(g.ctx.Get(), g, frame, false, replay_handoff);
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
        if (replay_handoff) {
            const auto replay = na::native11::draw_replay::GetStats();
            constexpr char key[] = "replay_delivered=";
            const char* field = std::strstr(wide, key);
            const auto replay_frames = field ? std::strtoull(field + sizeof(key) - 1, nullptr, 10) : 0ull;
            std::printf("DX11 replay handoff: replayed=%llu packed=%llu pack_failed=%llu delivered_with_replay=%llu gates=%s\n",
                        (unsigned long long)replay.replayed, (unsigned long long)replay.packed,
                        (unsigned long long)replay.pack_failed, replay_frames, replay.gates);
            expect(replay.packed >= 20 && !replay.pack_failed && replay_frames >= 20,
                   "GPU-packed positions reach the native DLSS motion path across the slot ring");
        }
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
    auto& effect_options = na::settings::Get();
    const auto prior_camera = effect_options.j2_camera.exchange(0);
    effect_options.xegtao_enabled.store(1);
    effect_options.donut_bloom.store(1);
    for (int i = 0; i < 3; ++i) present_campaign();
    const auto bloom_with_no_camera = na::donut::ScenePass().GetStats().recorded;
    const auto ao_with_no_camera = na::xegtao::ScenePass().GetStats().recorded;
    const auto delivered_with_no_camera = evaluations;
    for (int i = 0; i < 4; ++i) present_campaign();
    const auto ambient_status = na::xegtao::ScenePass().GetStats();
    char measured_ao[4096]{};
    na::native_d3d11::FormatStatus(measured_ao, sizeof measured_ao);
    expect(na::donut::ScenePass().GetStats().recorded == bloom_with_no_camera + 4 &&
           evaluations == delivered_with_no_camera + 4 && ambient_status.recorded == ao_with_no_camera + 4 &&
           g_seen.last.camera_known && std::strstr(measured_ao, "motion measured, no camera"),
           "XeGTAO finds native projection while DLAA keeps using picture-measured motion");
    effect_options.xegtao_enabled.store(0);
    effect_options.donut_bloom.store(0);
    effect_options.j2_camera.store(prior_camera);
    auto& draw_replay = na::settings::Get().j2_draw_replay;
    draw_replay.store(1);
    present_campaign();
    expect(na::native11::draw_replay::Active(), "native Present enables requested draw replay");
    na::j2::SetPaused(true);
    expect(SUCCEEDED(g.swap->Present(0, 0)) && !na::native11::draw_replay::Active(),
           "a Present without a DLAA evaluation disables paused draw replay");
    draw_replay.store(0);
    na::j2::SetPaused(false);
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
        na::settings::Get().j2_debug_jitter.store(5); // double, both axes
        for (int i = 0; i < 3; ++i) present_campaign();
        expect(jitter_resets() == before_switch + 3, "double-offset diagnostic changes delivered history once");
        na::settings::Get().j2_debug_jitter.store(0);
        for (int i = 0; i < 3; ++i) present_campaign();
        expect(jitter_resets() == before_switch + 4 && g_seen.last.raster.applied,
               "normal jitter restores delivered history once and keeps raster jitter on");
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
    const auto get_applied_sampler = [&] {
        na::native11::OwnWorkScope own_work;
        bound.Reset();
        g.ctx->PSGetSamplers(0, 1, &bound);
    };
    get_applied_sampler();
    D3D11_SAMPLER_DESC applied{};
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
        std::printf("native deferred mip pixels: -1 %.2f/%.2f, own %.2f/%.2f\n",
                    deferred_on[0], deferred_on[1], deferred_off[0], deferred_off[1]);
        expect(deferred_on[0] > .9f && deferred_off[1] > .9f,
               "deferred draws update and restore mip pixels without rebinding");
    } else expect(false, "create deferred material context");
    na::observe::SetSamplerMipBias(-1.f);
    // A game sampler already at -1 must stay there, rather than receiving -2.
    sampler.MipLODBias = -1.f;
    ComPtr<ID3D11SamplerState> already;
    g.device->CreateSamplerState(&sampler, &already); material_raw = already.Get();
    g.ctx->PSSetSamplers(0, 1, &material_raw);
    get_applied_sampler();
    if (bound) bound->GetDesc(&applied);
    expect(bound && applied.MipLODBias == -1.f && na::native11::GetSamplerStats().original_bias == -1.f,
           "native mip total avoids double bias when the game already requests -1");
    // The startup sampler was never logged at creation. Check the result of a real draw.
    {
        const auto before = na::native11::GetSamplerStats();
        na::observe::SetSamplerMipBias(-1.f); na::observe::SetSamplerAnisotropy(0);
        auto* unseen_raw = early_original.Get();
        g.ctx->PSSetSamplers(0, 1, &unseen_raw);
        float early_on[4]{}, early_off[4]{};
        g.ctx->Draw(4, 0); ReadCopy(g, early_on, g.color.Get());
        get_applied_sampler(); applied = {};
        if (bound) bound->GetDesc(&applied);
        ComPtr<ID3D11SamplerState> read_by_game;
        g.ctx->PSGetSamplers(0, 1, &read_by_game);
        expect(bound.Get() == early_already_biased.Get() && read_by_game.Get() == early_original.Get(),
               "an interned replacement is reported to the game as this slot's original");
        na::observe::SetSamplerMipBias(0.f);
        g.ctx->Draw(4, 0); ReadCopy(g, early_off, g.color.Get());
        const auto after = na::native11::GetSamplerStats();
        std::printf("native mip pixels, sampler made before the hooks: -1 %.2f/%.2f, own %.2f/%.2f adopted=%llu\n",
                    early_on[0], early_on[1], early_off[0], early_off[1],
                    static_cast<unsigned long long>(after.adopted - before.adopted));
        expect(early_original && after.adopted == before.adopted + 1 && after.eligible_bias == before.eligible_bias + 1 &&
               after.unknown == before.unknown && bound && applied.MipLODBias == -1.f &&
               early_on[0] > .9f && early_on[1] < .1f && early_off[1] > .9f,
               "a sampler created before the hooks gets the mip bias at its first bind and restores live");
        auto* game_already_biased = early_already_biased.Get();
        g.ctx->PSSetSamplers(0, 1, &game_already_biased);
        float early_game_bias[4]{};
        g.ctx->Draw(4, 0); ReadCopy(g, early_game_bias, g.color.Get());
        get_applied_sampler();
        expect(bound.Get() == game_already_biased && early_game_bias[0] > .9f &&
               na::native11::GetSamplerStats().adopted == after.adopted + 1,
               "a pre-hook game sampler shared with an existing replacement keeps its own bias");
        na::observe::SetSamplerMipBias(-1.f);
    }
    // After the frame's picture is handed to DLSS the game's later passes keep its own samplers;
    // the next frame's scene gets the policy again. Neither needs a new sampler bind by the game.
    na::observe::SetSamplerMipBias(-1.f); na::observe::SetSamplerAnisotropy(16);
    material_raw = material.Get();
    g.ctx->PSSetSamplers(0, 1, &material_raw);
    g.ctx->Draw(4, 0);
    const uint64_t own_before = na::native11::GetSamplerStats().after_scene;
    na::native11::SetSceneHanded(true);
    g.ctx->Draw(4, 0);
    get_applied_sampler(); applied = {};
    if (bound) bound->GetDesc(&applied);
    expect(bound && applied.Filter == D3D11_FILTER_MIN_MAG_MIP_LINEAR && applied.MipLODBias == 0.f &&
           na::native11::GetSamplerStats().after_scene > own_before,
           "draws after the scene is handed to DLSS sample with the game's own sampler");
    na::native11::SetSceneHanded(false);
    g.ctx->Draw(4, 0);
    get_applied_sampler(); applied = {};
    if (bound) bound->GetDesc(&applied);
    expect(bound && applied.Filter == D3D11_FILTER_ANISOTROPIC && applied.MaxAnisotropy == 16 && applied.MipLODBias == -1.f,
           "the next frame's scene draws get 16x AF and the -1 mip bias back");
    // Automatic AF (no menu control, core/anisotropic_auto.hpp): the game samplers seen so far are
    // tiling trilinear, so the shared detector says the game lacks AF and the host raises them to
    // 16x when it follows the detector. When the game then creates its own 16x world samplers, the
    // decision flips live to the game's own filtering, with no new sampler bind by the game.
    {
        na::observe::SetSamplerMipBias(0.f);
        na::observe::SetSamplerAfFollow(2);
        expect(na::af::Global().Get().state == na::af::Detector::State::GameLacksAf &&
               na::observe::SamplerAfActionWanted() == na::af::kAuto16,
               "automatic AF finds no 8x/16x world samplers and raises them");
        material_raw = material.Get();
        g.ctx->PSSetSamplers(0, 1, &material_raw);
        g.ctx->Draw(4, 0);
        get_applied_sampler(); applied = {};
        if (bound) bound->GetDesc(&applied);
        expect(bound && applied.Filter == D3D11_FILTER_ANISOTROPIC && applied.MaxAnisotropy == 16,
               "automatic AF binds 16x on the tiling material sampler");
        // D3D11 interns identical samplers. The game now requests exactly the
        // descriptor targeted by our existing substitution, while it is bound.
        D3D11_SAMPLER_DESC own16 = applied;
        ComPtr<ID3D11SamplerState> shared16;
        expect(SUCCEEDED(g.device->CreateSamplerState(&own16, &shared16)) && shared16.Get() == bound.Get(),
               "an explicit game sampler can share the bound automatic AF replacement");
        std::vector<ComPtr<ID3D11SamplerState>> game16;
        for (int i = 1; i < 64 && na::observe::SamplerAfActionWanted() != na::af::kKeep; ++i) {
            own16.MipLODBias = -0.125f * float(i); // distinct descriptions: each counts once
            game16.emplace_back();
            g.device->CreateSamplerState(&own16, &game16.back());
        }
        expect(na::observe::SamplerAfActionWanted() == na::af::kKeep &&
               na::af::Global().Get().state == na::af::Detector::State::GameHasAf,
               "the game's own 8x/16x world samplers switch our filtering off, live");
        g.ctx->Draw(4, 0);
        get_applied_sampler(); applied = {};
        if (bound) bound->GetDesc(&applied);
        D3D11_SAMPLER_DESC original{};
        if (material) material->GetDesc(&original);
        std::printf("native AF restored: same object=%d filter=%u/%u max_aniso=%u/%u\n",
                    int(bound.Get() == material.Get()), unsigned(applied.Filter), unsigned(original.Filter),
                    applied.MaxAnisotropy, original.MaxAnisotropy);
        expect(bound && bound.Get() == material.Get(), "after the switch the draw binds the exact original game sampler");
        auto* game_owned16 = shared16.Get();
        g.ctx->PSSetSamplers(0, 1, &game_owned16);
        g.ctx->Draw(4, 0);
        get_applied_sampler();
        expect(bound.Get() == shared16.Get(),
               "a new explicit game bind keeps the same 16x sampler");
        g.ctx->PSSetSamplers(0, 1, &material_raw);
        na::observe::SetSamplerAfFollow(0);
    }
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
