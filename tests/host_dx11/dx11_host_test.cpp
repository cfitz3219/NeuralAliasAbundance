// A small synthetic DX11 "game" under the real DX11 hooks (no game names, no
// special cases). Each frame, like a real game:
//   shadow pass (own depth, a shadow camera upload), main camera upload
//   (ViewProjection + inverse in a dynamic constant buffer), scene draws into
//   an FP16 colour with a D24S8 depth (no shader access), tonemap reading the
//   scene colour into the back buffer, HUD draws, Present.
// Phase 1 records everything on the immediate context; phase 2 records the
// scene and the post pass on deferred contexts (as Dunia-engine games do) and
// executes them on the immediate one.
//
// Checks: the hooks find the device, the scene depth/colour, the camera at its
// byte offset; DLSS runs (helper in copy-test mode, the camera-motion pass
// really runs) before the tonemap read every frame in both phases; jitter is
// written into the game's own camera upload (rows 0/1 only); mip bias is
// applied to new mip-mapped samplers; a resize stops and restarts cleanly.
// Exit 77 (SKIP) only on WARP when it cannot share across DX11/DX12.
//
// Usage: dx11_host_test <na_bridge_helper64.exe> <data folder>
#include "host_dx11/camera_pick.hpp"
#include "host_dx11/dx11_host.hpp"

#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <share.h>
#include <filesystem>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

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
float4 ps_scene(float4 p : SV_Position) : SV_Target { return float4(0.25, 0.5, 0.75, 1.0); }
float4 ps_tonemap(float4 p : SV_Position) : SV_Target { return scene.Load(int3(int2(p.xy), 0)); }
float4 ps_hud(float4 p : SV_Position) : SV_Target { return float4(1, 1, 1, 0.5); }
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
    ComPtr<ID3D11Texture2D> shadow_depth, shadow_rt, depth, color;
    ComPtr<ID3D11DepthStencilView> shadow_dsv, dsv;
    ComPtr<ID3D11RenderTargetView> shadow_rtv, color_rtv, back_rtv;
    ComPtr<ID3D11ShaderResourceView> color_srv;
    ComPtr<ID3D11Buffer> cam_cb, obj_cb;
    ComPtr<ID3D11VertexShader> vs;
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
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
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
    if (!vs || !pss || !pst || !psh) return false;
    g.device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g.vs);
    g.device->CreatePixelShader(pss->GetBufferPointer(), pss->GetBufferSize(), nullptr, &g.ps_scene);
    g.device->CreatePixelShader(pst->GetBufferPointer(), pst->GetBufferSize(), nullptr, &g.ps_tonemap);
    g.device->CreatePixelShader(psh->GetBufferPointer(), psh->GetBufferSize(), nullptr, &g.ps_hud);
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g.device->CreateBlendState(&bd, &g.blend);
    return g.shadow_dsv && g.shadow_rtv && g.cam_cb && g.obj_cb && g.blend && MakeTargets(g);
}

void UploadCamera(ID3D11DeviceContext* c, Game& g, const na::camera::Matrix& vp, bool remember)
{
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(c->Map(g.cam_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    std::vector<uint8_t> bytes(512, 0);
    const float pad[4] = {1.f, 2.f, 3.f, 4.f};
    std::memcpy(bytes.data(), pad, 16);
    na::dx11::StoreMatrix(bytes.data(), bytes.size(), 64, na::camera::Layout::RowMajor, vp);
    na::camera::Matrix inv{};
    na::camera::Invert(vp, inv);
    na::dx11::StoreMatrix(bytes.data(), bytes.size(), 192, na::camera::Layout::RowMajor, inv);
    std::memcpy(m.pData, bytes.data(), bytes.size());
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

void Viewport(ID3D11DeviceContext* c, UINT w, UINT h)
{
    const D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    c->RSSetViewports(1, &vp);
}

void Scene(ID3D11DeviceContext* c, Game& g, int frame)
{
    ID3D11Buffer* cbs[2] = {g.cam_cb.Get(), g.obj_cb.Get()};
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    c->VSSetShader(g.vs.Get(), nullptr, 0);
    c->VSSetConstantBuffers(0, 2, cbs);
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
    ID3D11RenderTargetView* rt = g.color_rtv.Get();
    c->OMSetRenderTargets(1, &rt, g.dsv.Get());
    const float clear[4] = {0.1f, 0.2f, 0.3f, 1.f};
    c->ClearRenderTargetView(g.color_rtv.Get(), clear);
    c->ClearDepthStencilView(g.dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.f, 0);
    Viewport(c, g.w, g.h);
    for (int i = 0; i < 40; ++i) { SetObject(c, g, i); c->Draw(4, 0); }
}

void Post(ID3D11DeviceContext* c, Game& g)
{
    // Tonemap: reads the scene colour (the DLSS point is just before this bind).
    ID3D11RenderTargetView* bb = g.back_rtv.Get();
    c->OMSetRenderTargets(1, &bb, nullptr);
    ID3D11ShaderResourceView* srv = g.color_srv.Get();
    c->PSSetShaderResources(0, 1, &srv);
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
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) { std::printf("usage: dx11_host_test <helper exe> <data folder>\n"); return 2; }
    const std::string helper_a = argv[1], data_a = argv[2];
    const std::wstring helper(helper_a.begin(), helper_a.end()), data(data_a.begin(), data_a.end());
    std::error_code ec;
    std::filesystem::create_directories(data, ec);

    na::dx11::HostConfig cfg;
    cfg.copy_test = true;
    cfg.jitter = true;
    cfg.helper_path = helper;
    cfg.data_path = data;
    cfg.log_path = data + L"\\NeuralAliasDX11-test.log";
    const bool installed = na::dx11::Install(cfg);
    expect(installed, "the DX11 hooks install");
    if (!installed) { std::printf("reason: %s\n", na::dx11::GetStats().reason); return 1; }

    Game g;
    bool warp = false;
    if (!MakeGame(g, false)) {
        g = Game{};
        if (!MakeGame(g, true)) { std::printf("SKIP: no DX11 device\n"); return 77; }
        warp = true;
    }
    std::printf("device: %s\n", warp ? "WARP" : "hardware");

    // Mip bias on a new mip-mapped sampler (jitter is on).
    {
        D3D11_SAMPLER_DESC s{};
        s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        s.MaxLOD = D3D11_FLOAT32_MAX;
        ComPtr<ID3D11SamplerState> st;
        g.device->CreateSamplerState(&s, &st);
        D3D11_SAMPLER_DESC got{};
        if (st) st->GetDesc(&got);
        // The first sampler is created before the game's first Present, still under the hook.
        expect(st && std::fabs(got.MipLODBias - (-1.f)) < 1e-6f, "a new mip-mapped sampler gets the -1 mip bias");
    }

    // ---- phase 1: immediate context ----
    int frame = 0;
    const DWORD t0 = GetTickCount();
    for (; frame < 2000 && GetTickCount() - t0 < 60000; ++frame) {
        Pump();
        Scene(g.ctx.Get(), g, frame);
        Post(g.ctx.Get(), g);
        g.swap->Present(0, 0);
        if (na::dx11::GetStats().delivered >= 20) { ++frame; break; }
        Sleep(5);
    }
    na::dx11::HostStats s = na::dx11::GetStats();
    std::printf("phase 1: frames=%d presents=%llu fired=%llu delivered=%llu repeated=%llu not_ready=%llu failed=%llu no_camera=%llu refused=%llu "
                "jitter_writes=%llu helper_motion=%u reason='%s'\n",
                frame, static_cast<unsigned long long>(s.presents), static_cast<unsigned long long>(s.fired),
                static_cast<unsigned long long>(s.delivered), static_cast<unsigned long long>(s.repeated),
                static_cast<unsigned long long>(s.not_ready), static_cast<unsigned long long>(s.failed),
                static_cast<unsigned long long>(s.no_camera), static_cast<unsigned long long>(s.refused),
                static_cast<unsigned long long>(s.jitter_writes), s.helper_motion_frames, s.reason);
    if (s.failed && warp && std::strstr(s.reason, "share")) { std::printf("SKIP: WARP cannot share across DX11/DX12 here\n"); return 77; }
    expect(s.device_found, "the game's device is found at its first Present");
    expect(s.selected && s.color_w == g.w && s.color_h == g.h && s.color_format == DXGI_FORMAT_R16G16B16A16_FLOAT,
           "the scene colour (FP16, screen size) is selected, not the shadow map or back buffer");
    expect(s.depth_format == DXGI_FORMAT_D24_UNORM_S8_UINT, "the scene depth (D24S8, no shader access) is selected");
    expect(s.camera && s.camera_offset == 64, "the camera ViewProjection is found at byte 64 (with its inverse)");
    expect(!s.depth_reversed, "standard depth is recognised");
    expect(s.delivered >= 20, "DLSS (copy test) ran before the tonemap read, 20 frames");
    expect(s.last_draws == 67 && s.last_scene_draws == 40, "immediate: every draw is counted exactly once");
    expect(s.helper_motion_frames >= 10, "the helper built camera motion with the shared motion arbiter");
    expect(s.jitter_writes >= 10, "jitter was written into the game's own camera upload");

    // The GPU sees the jittered camera: rows 0/1 moved by a sub-pixel amount, rows 2/3 unchanged.
    {
        D3D11_BUFFER_DESC bd{};
        g.cam_cb->GetDesc(&bd);
        bd.Usage = D3D11_USAGE_STAGING;
        bd.BindFlags = 0;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> st;
        g.device->CreateBuffer(&bd, nullptr, &st);
        Scene(g.ctx.Get(), g, frame);
        g.ctx->CopyResource(st.Get(), g.cam_cb.Get());
        Post(g.ctx.Get(), g);
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
            // row0 = want_row0 + dx * row3 (x = jitter * 2 / width): recover dx from column 2 (row3 = (0,0,1,0) shape).
            const double dx = (gpu[2] - want[2]) / want[14], dy = (gpu[6] - want[6]) / want[14];
            sub_pixel = std::fabs(dx) <= 2.0 / g.w + 1e-6 && std::fabs(dy) <= 2.0 / g.h + 1e-6 && (std::fabs(dx) > 1e-5 || std::fabs(dy) > 1e-5);
            std::printf("jitter seen by the GPU: %.3f px, %.3f px\n", dx * g.w / 2.0, -dy * g.h / 2.0);
        }
        expect(rows23 && sub_pixel, "the GPU's camera carries a sub-pixel jitter in rows 0/1 only");
        ++frame;
    }

    // ---- phase 2: scene and post recorded on deferred contexts ----
    const uint64_t fired1 = na::dx11::GetStats().delivered;
    ComPtr<ID3D11DeviceContext> d_scene, d_post;
    g.device->CreateDeferredContext(0, &d_scene);
    g.device->CreateDeferredContext(0, &d_post);
    expect(d_scene && d_post, "deferred contexts were created");
    // At least 25 frames; then until 15 results came back (the helper's pace varies on CI), 30 s at most.
    const DWORD t2 = GetTickCount();
    for (int i = 0; d_scene && d_post && GetTickCount() - t2 < 30000; ++i, ++frame) {
        if (i >= 25 && na::dx11::GetStats().delivered - fired1 >= 15) break;
        Pump();
        ComPtr<ID3D11CommandList> l_scene, l_post;
        Scene(d_scene.Get(), g, frame);
        d_scene->FinishCommandList(FALSE, &l_scene);
        Post(d_post.Get(), g);
        d_post->FinishCommandList(FALSE, &l_post);
        g.ctx->ExecuteCommandList(l_scene.Get(), FALSE);
        g.ctx->ExecuteCommandList(l_post.Get(), FALSE);
        g.swap->Present(0, 0);
        Sleep(5);
    }
    s = na::dx11::GetStats();
    std::printf("phase 2: delivered=%llu (+%llu) lists=%llu/%llu reason='%s'\n", static_cast<unsigned long long>(s.delivered),
                static_cast<unsigned long long>(s.delivered - fired1), static_cast<unsigned long long>(s.lists_executed),
                static_cast<unsigned long long>(s.lists_recorded), s.reason);
    expect(s.lists_executed >= 40, "command lists from deferred contexts were replayed into the frame model");
    // 20 shadow + 40 scene + 1 tonemap + 6 HUD, once each (not again from d3d11's own list replay).
    std::printf("phase 2 last frame: draws=%u scene=%u\n", s.last_draws, s.last_scene_draws);
    expect(s.last_draws == 67 && s.last_scene_draws == 40, "deferred: every draw is counted exactly once");
    expect(s.delivered - fired1 >= 15, "deferred: DLSS ran before the post-processing command list");

    // ---- resize: bridge stops, ResizeBuffers succeeds (no held back buffer), restarts ----
    g.back_rtv.Reset();
    g.color_srv.Reset();
    g.color_rtv.Reset();
    g.dsv.Reset();
    g.color.Reset();
    g.depth.Reset();
    g.w = 256;
    g.h = 144;
    const HRESULT rz = g.swap->ResizeBuffers(0, g.w, g.h, DXGI_FORMAT_UNKNOWN, 0);
    expect(SUCCEEDED(rz), "ResizeBuffers succeeds under the hooks (nothing of the game's is held)");
    expect(SUCCEEDED(rz) && MakeTargets(g), "the game makes its new-size targets");
    const uint64_t before = na::dx11::GetStats().delivered;
    // The helper restarts at the new size; its start-up time varies on CI, so wait on time (30 s), not a frame count.
    const DWORD t3 = GetTickCount();
    for (; SUCCEEDED(rz) && GetTickCount() - t3 < 30000; ++frame) {
        Pump();
        Scene(g.ctx.Get(), g, frame);
        Post(g.ctx.Get(), g);
        g.swap->Present(0, 0);
        if (na::dx11::GetStats().delivered >= before + 10) break;
        Sleep(5);
    }
    s = na::dx11::GetStats();
    expect(s.color_w == 256 && s.color_h == 144 && s.delivered >= before + 10, "after the resize, the new scene colour is found and DLSS runs again");

    std::printf("log: %s\n", s.log_path);
    bool log_selection = false, log_camera = false, log_bridge = false;
    if (FILE* f = _fsopen(s.log_path, "r", _SH_DENYNO)) {
        char line[512];
        int n = 0;
        while (std::fgets(line, sizeof(line), f)) {
            if (n++ < 40) std::printf("  | %s", line);
            log_selection = log_selection || std::strstr(line, "selection: scene depth") != nullptr;
            log_camera = log_camera || std::strstr(line, "camera: found") != nullptr;
            log_bridge = log_bridge || std::strstr(line, "bridge: started") != nullptr;
        }
        std::fclose(f);
    }
    expect(log_selection && log_camera && log_bridge, "the run's log records the selection, the camera and the bridge start");
    std::printf(g_failed ? "FAIL: %d check(s) failed\n" : "PASS: DX11 hooks on a synthetic game\n", g_failed);
    std::fflush(stdout);
    // The hooks stay installed for the process lifetime (as in a game); exit directly.
    ExitProcess(g_failed ? 1u : 0u);
}
