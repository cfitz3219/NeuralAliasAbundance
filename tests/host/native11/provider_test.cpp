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
#if defined(NA_TEST_NATIVE_DLAA)
#include "host_dx12/adapters/wwii/scene_camera.hpp"
#include "host_dx12/capture/frame_capture.hpp"
#include "host_dx12/capture/j2_capture.hpp"
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
#include "host_dx12/observe/producers.hpp"
#include <DirectXPackedVector.h>
#include <d3d11on12.h>
#include <filesystem>
#include <fstream>
#endif

#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
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
float4 ps_scene(float4 p : SV_Position) : SV_Target { return float4(0.25, 0.5, 0.75, 1.0); }
float4 ps_tonemap(float4 p : SV_Position) : SV_Target { return scene.Load(int3(int2(p.xy), 0)); }
float4 ps_hud(float4 p : SV_Position) : SV_Target { return float4(1, 1, 1, 0.5); }
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
    if (FAILED(g.device->CreateTexture2D(&d, nullptr, &g.depth)) || FAILED(g.device->CreateDepthStencilView(g.depth.Get(), nullptr, &g.dsv))) return false;
    d.Format = kSceneFormat;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g.device->CreateTexture2D(&d, nullptr, &g.color)) || FAILED(g.device->CreateRenderTargetView(g.color.Get(), nullptr, &g.color_rtv)) ||
        FAILED(g.device->CreateShaderResourceView(g.color.Get(), nullptr, &g.color_srv)))
        return false;
    return true;
}

bool MakeGame(Game& g, bool warp)
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
    sd.BufferDesc.Height = g.h;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g.wnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &sd,
                                             &g.swap, &g.device, nullptr, &g.ctx)))
        return false;
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


void Scene(ID3D11DeviceContext* c, Game& g, int frame)
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
    UploadCamera(c, g, MainVp(frame), true);
    ID3D11RenderTargetView* rt = g.on_screen ? g.back_rtv.Get() : g.color_rtv.Get();
    ID3D11RenderTargetView* layers[2] = {g.layer_rtv.Get(), g.layer2_rtv.Get()};
    if (g.layered) c->OMSetRenderTargets(2, layers, g.dsv.Get());
    else c->OMSetRenderTargets(1, &rt, g.dsv.Get());
    const float clear[4] = {0.1f, 0.2f, 0.3f, 1.f};
    c->ClearRenderTargetView(rt, clear);
    c->ClearDepthStencilView(g.dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.f, 0);
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
    uint32_t color_w = 0, color_h = 0;
    uint64_t color_inc = 0, depth_inc = 0;
    na::native11::SceneFrame last{};
};
Seen g_seen;

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
    if (!f.camera.bytes || (g.campaign_pixel_range && g.camera_ring_offset >= 65536 ?
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

int main([[maybe_unused]] int argc, [[maybe_unused]] char** argv)
{
#if defined(NA_TEST_NATIVE_DLAA)
    const bool automatic = argc > 1 && std::strcmp(argv[1], "--automatic") == 0;
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const auto dir = std::filesystem::path(exe).parent_path();
    const auto scope = L"Native11Activation-" + std::to_wstring(GetCurrentProcessId());
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
        !na::host::StartFromIni(GetModuleHandleW(nullptr), ini_name.c_str(), "NATIVE DX11 TEST")) return 1;
    na::host::WaitForObserveInstall();
    const ULONGLONG init_deadline = GetTickCount64() + 20000;
    while (!na::host::GetStatus().init_complete && GetTickCount64() < init_deadline) Sleep(5);
    expect(na::host::GetStatus().init_complete && na::on12::NativeRoute() != automatic && !na::on12::Enabled(),
           "production host starts with the bridge disabled");
    if (g_failed) return 1;
#endif
    Game g;
    bool warp = false;
    if (!MakeGame(g, false)) {
        g = Game{};
        if (!MakeGame(g, true)) { std::printf("SKIP: no DX11 device\n"); return 77; }
        warp = true;
    }
    std::printf("device: %s\n", warp ? "WARP" : "hardware");
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
    char activation[2048]{};
    na::on12::FormatStatus(activation, sizeof activation);
    expect(std::strstr(activation, "native11_route=active installs=1 install_failures=0") != nullptr &&
           std::strstr(activation, "dx11on12 enabled=0") != nullptr && std::strstr(activation, "wrapped=0") != nullptr,
           "production host installed the native provider exactly once");
    if (g_failed) return 1;
    na::j2::SetJitter(true);
#endif
    g_seen.game = &g;
    const bool installed = na::native11::Install(g.device.Get(), &Consume, &g_seen);
    expect(installed, "the provider's hooks install on the game's device");
    if (!installed) return 1;
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
                       g_seen.last.camera.jitter_px[1] == -0.25f,
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
    for (int i = 0; i < 3; ++i) present_campaign();
    expect(evaluations == paused, "DLAA switch stops native evaluations");
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
        const auto jitter_before_viewport = g_seen.jitter_reported;
        const auto delivered_before_viewport = evaluations;
        for (int i = 0; i < 12; ++i) present_campaign();
        na::native11::FormatStatus(status, sizeof status);
        std::printf("native campaign viewport jitter: %s\n", status);
        expect(evaluations == delivered_before_viewport + 12, "DLAA keeps delivering every frame with viewport jitter on");
        expect(g_seen.last.camera.jitter_applied && g_seen.jitter_reported >= jitter_before_viewport + 8,
               "the native campaign pose gets real jitter through the viewports");
        na::j2::SetJitter(false);
        present_campaign(); // an already armed sample may still apply to this frame
        const auto viewport_off = g_seen.jitter_reported;
        for (int i = 0; i < 4; ++i) present_campaign();
        expect(g_seen.jitter_reported == viewport_off, "the jitter switch stops the viewport offsets");

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
    na::capture::j2::Shutdown();
    na::capture::Shutdown();
    DeleteFileW(ini.c_str());
#endif

    std::printf(g_failed ? "FAIL: %d check(s) failed\n" : "PASS: native DX11 provider on a synthetic game\n", g_failed);
    std::fflush(stdout);
    // The hooks stay installed for the process lifetime (as in a game); exit directly.
    ExitProcess(g_failed ? 1u : 0u);
}
