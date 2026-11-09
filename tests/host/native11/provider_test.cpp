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
#include "host_dx12/native_d3d11/live_dlaa.hpp"
#include "host_dx12/j2/live_dlaa.hpp"
#include <DirectXPackedVector.h>
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
    if (FAILED(c->Map(g.cam_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    std::vector<uint8_t> bytes(512, 0);
    const float pad[4] = {1.f, 2.f, 3.f, 4.f};
    std::memcpy(bytes.data(), pad, 16);
    na::native11::StoreMatrix(bytes.data(), bytes.size(), 64, na::camera::Layout::RowMajor, vp);
    na::camera::Matrix inv{};
    na::camera::Invert(vp, inv);
    na::native11::StoreMatrix(bytes.data(), bytes.size(), 192, na::camera::Layout::RowMajor, inv);
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

std::atomic<bool> g_tonemap_bound{false}; // the immediate context's tonemap read was bound this frame

void Viewport(ID3D11DeviceContext* c, UINT w, UINT h)
{
    const D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    c->RSSetViewports(1, &vp);
}


void Scene(ID3D11DeviceContext* c, Game& g, int frame)
{
    g_tonemap_bound = false;
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



void Post(ID3D11DeviceContext* c, Game& g, bool immediate)
{
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    c->VSSetShader(g.vs_post.Get(), nullptr, 0);
    // Tonemap: reads the scene colour (the insertion point is just before this bind).
    ID3D11RenderTargetView* bb = g.back_rtv.Get();
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
    const bool inputs = f.color.resource == reinterpret_cast<uint64_t>(static_cast<ID3D11Resource*>(g.color.Get())) &&
                        f.depth.resource == reinterpret_cast<uint64_t>(static_cast<ID3D11Resource*>(g.depth.Get())) &&
                        f.color.format == kSceneFormat && f.depth.format == DXGI_FORMAT_D24_UNORM_S8_UINT &&
                        f.color.width == g.w && f.color.height == g.h && f.color.active_x == 0 && f.color.active_y == 0 &&
                        f.color.active_w == g.w && f.color.active_h == g.h && f.color.incarnation && f.depth.incarnation &&
                        f.color.d3d11_resource == static_cast<ID3D11Resource*>(g.color.Get());
    if (!inputs) ++s.wrong_inputs;
    if (g_tonemap_bound && f.boundary == na::native11::Boundary::BeforeRead) ++s.after_tonemap;
    if (f.scene_draws != 40) ++s.no_scene;
    if (!f.camera.bytes || f.camera.size != 512 || f.camera.buffer != reinterpret_cast<uint64_t>(static_cast<ID3D11Resource*>(g.cam_cb.Get())) ||
        f.camera.vs_slot != 0) {
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
    if (s.copy) c->CopyResource(s.copy.Get(), static_cast<ID3D11Resource*>(f.color.d3d11_resource));
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

int main()
{
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
    na::j2::SetEnabled(true);
    na::j2::SetJitter(true);
#endif
    g_seen.game = &g;
    const bool installed = na::native11::Install(g.device.Get(), &Consume, &g_seen);
    expect(installed, "the provider's hooks install on the game's device");
    if (!installed) return 1;

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
#endif

    std::printf(g_failed ? "FAIL: %d check(s) failed\n" : "PASS: native DX11 provider on a synthetic game\n", g_failed);
    std::fflush(stdout);
    // The hooks stay installed for the process lifetime (as in a game); exit directly.
    ExitProcess(g_failed ? 1u : 0u);
}
