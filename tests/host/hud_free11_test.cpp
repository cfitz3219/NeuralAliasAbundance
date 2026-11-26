// The HUD-free picture on the native DX11 route (native11/hud_free), through the real hooks: a DX11
// "game" on its own device, presented through the DX11 proxy chain, draws a scene and then an interface
// in each of the two shapes; the program learns the shape from one frame by itself.
//  - T3, interface drawn straight onto the shown picture: an opaque scene pass into the back buffer,
//    then blended interface rectangles on it.
//  - T2, separate HUD sheet: the scene in its own texture, interface rectangles blended into a sheet,
//    then ONE final draw composites scene + sheet into the back buffer.
// For each: the HUD-free picture equals the scene exactly (bytes), and the paste rule
// (core/live_frame_gen.hpp PasteHud, the CPU reference of the GPU paste) restores every interface pixel
// of the real frame exactly onto another picture.
#include "test_support.hpp"
#include "host_dx12/host_boot/host_boot.hpp"
#include "host_dx12/host_boot/process_log.hpp"
#include "host_dx12/host_boot/session.hpp"
#include "host_dx12/overlay/overlay.hpp"
#include "host_dx12/on12/d3d11_on12.hpp"
#include "host_dx12/native11/hud_free.hpp"
#include "core/live_frame_gen.hpp"

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")

using test::ComPtr;

namespace {
constexpr UINT W = 640, H = 360;
constexpr char kShaders[] = R"(
cbuffer K : register(b0) { float4 rect; float4 color; uint mode; };
Texture2D scene : register(t0);
Texture2D sheet : register(t1);
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 c = float2(id & 1, id >> 1);
    return float4(lerp(rect.xy, rect.zw, c), 0, 1);
}
float4 ps(float4 p : SV_Position) : SV_Target {
    if (mode == 1) return float4(frac(p.x / 37.0), frac(p.y / 23.0), 0.25 + 0.5 * frac((p.x + p.y) / 91.0), 1); // the scene
    if (mode == 2) { // the final composite: scene under the premultiplied sheet
        float4 s = sheet.Load(int3(p.xy, 0));
        return float4(scene.Load(int3(p.xy, 0)).rgb * (1 - s.a) + s.rgb, 1);
    }
    return color; // interface
}
)";

struct Gpu {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11BlendState> blend, premul;
    ComPtr<ID3D11RasterizerState> rs;
};
struct K { float rect[4]; float color[4]; UINT mode; UINT pad[3]; };

void quad(Gpu& g, float x0, float y0, float x1, float y1, const float color[4], UINT mode)
{
    K k{{x0, y0, x1, y1}, {color[0], color[1], color[2], color[3]}, mode, {}};
    g.ctx->UpdateSubresource(g.cb.Get(), 0, nullptr, &k, 0, 0);
    g.ctx->Draw(4, 0);
}
void pipeline(Gpu& g, ID3D11RenderTargetView* rtv)
{
    g.ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp{0, 0, float(W), float(H), 0, 1};
    g.ctx->RSSetViewports(1, &vp);
    g.ctx->RSSetState(g.rs.Get());
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    g.ctx->VSSetShader(g.vs.Get(), nullptr, 0);
    g.ctx->PSSetShader(g.ps.Get(), nullptr, 0);
    ID3D11Buffer* b = g.cb.Get();
    g.ctx->VSSetConstantBuffers(0, 1, &b);
    g.ctx->PSSetConstantBuffers(0, 1, &b);
}
const float kHud[][4] = {{0.9f, 0.8f, 0.1f, 0.6f}, {0.1f, 0.9f, 0.3f, 0.5f}, {1, 1, 1, 0.8f}, {0.8f, 0.2f, 0.2f, 0.7f}, {0.2f, 0.4f, 0.9f, 0.6f}, {1, 0.5f, 0, 0.9f}};
// The interface: six rectangles (alpha-blended, no depth).
void interface_draws(Gpu& g, ID3D11BlendState* bs)
{
    g.ctx->OMSetBlendState(bs, nullptr, 0xFFFFFFFF);
    for (int i = 0; i < 6; ++i) {
        const float x = -0.9f + 0.3f * i;
        float c[4] = {kHud[i][0], kHud[i][1], kHud[i][2], kHud[i][3]};
        if (bs == g.premul.Get()) { for (int k = 0; k < 3; ++k) c[k] *= c[3]; } // premultiplied into the sheet
        quad(g, x, 0.6f - 0.1f * i, x + 0.25f, 0.85f - 0.1f * i, c, 0);
    }
    g.ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
}

std::vector<uint8_t> read(Gpu& g, ID3D11Texture2D* t)
{
    D3D11_TEXTURE2D_DESC d{};
    t->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> s;
    if (FAILED(g.dev->CreateTexture2D(&d, nullptr, &s))) return {};
    g.ctx->CopyResource(s.Get(), t);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g.ctx->Map(s.Get(), 0, D3D11_MAP_READ, 0, &m))) return {};
    std::vector<uint8_t> out(size_t(d.Width) * d.Height * 4);
    for (UINT y = 0; y < d.Height; ++y) std::memcpy(out.data() + size_t(y) * d.Width * 4, static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch, d.Width * 4);
    g.ctx->Unmap(s.Get(), 0);
    return out;
}
std::vector<float> floats(const std::vector<uint8_t>& rgba)
{
    std::vector<float> f(rgba.size() / 4 * 3);
    for (size_t i = 0; i < rgba.size() / 4; ++i) for (int c = 0; c < 3; ++c) f[i * 3 + c] = rgba[i * 4 + c] / 255.f;
    return f;
}

// One shape: frames until the HUD-free picture comes, then the checks.
int run_shape(Gpu& g, IDXGISwapChain* chain, bool sheet_shape, const char* label)
{
    na::native11::hud_free::Reset();
    na::native11::hud_free::SetWanted(true);
    // The scene alone, as the reference.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc = {1, 0};
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> scene, sheet;
    ComPtr<ID3D11RenderTargetView> scene_rtv, sheet_rtv;
    ComPtr<ID3D11ShaderResourceView> scene_srv, sheet_srv;
    REQUIRE(SUCCEEDED(g.dev->CreateTexture2D(&td, nullptr, &scene)) && SUCCEEDED(g.dev->CreateTexture2D(&td, nullptr, &sheet)));
    REQUIRE(SUCCEEDED(g.dev->CreateRenderTargetView(scene.Get(), nullptr, &scene_rtv)) && SUCCEEDED(g.dev->CreateRenderTargetView(sheet.Get(), nullptr, &sheet_rtv)));
    REQUIRE(SUCCEEDED(g.dev->CreateShaderResourceView(scene.Get(), nullptr, &scene_srv)) && SUCCEEDED(g.dev->CreateShaderResourceView(sheet.Get(), nullptr, &sheet_srv)));
    const float none[4] = {0, 0, 0, 0};
    std::vector<uint8_t> expected, real, hudfree;
    bool got = false;
    for (int frame = 0; frame < 8 && !got; ++frame) {
        ComPtr<ID3D11Texture2D> back;
        REQUIRE(SUCCEEDED(chain->GetBuffer(0, IID_PPV_ARGS(&back))));
        ComPtr<ID3D11RenderTargetView> back_rtv;
        REQUIRE(SUCCEEDED(g.dev->CreateRenderTargetView(back.Get(), nullptr, &back_rtv)));
        if (sheet_shape) {
            pipeline(g, scene_rtv.Get());
            quad(g, -1, -1, 1, 1, none, 1);                       // scene
            g.ctx->ClearRenderTargetView(sheet_rtv.Get(), none);
            pipeline(g, sheet_rtv.Get());
            interface_draws(g, g.premul.Get());                    // HUD into its sheet
            pipeline(g, back_rtv.Get());
            ID3D11ShaderResourceView* srvs[] = {scene_srv.Get(), sheet_srv.Get()};
            g.ctx->PSSetShaderResources(0, 2, srvs);
            quad(g, -1, -1, 1, 1, none, 2);                       // the one final draw
            ID3D11ShaderResourceView* nulls[2] = {};
            g.ctx->PSSetShaderResources(0, 2, nulls);
        } else {
            pipeline(g, back_rtv.Get());
            quad(g, -1, -1, 1, 1, none, 1);                       // the scene straight onto the shown picture
            interface_draws(g, g.blend.Get());                     // the interface on it
        }
        real = read(g, back.Get());
        back_rtv.Reset(); back.Reset();
        g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
        REQUIRE(SUCCEEDED(chain->Present(0, 0)));
        const char* why = nullptr;
        if (ID3D11Texture2D* p = na::native11::hud_free::Picture(&why)) { hudfree = read(g, p); got = true; }
        else std::printf("  %s frame %d: %s\n", label, frame, why ? why : "?");
    }
    // The scene alone, drawn into the shown picture's format, is the reference.
    pipeline(g, scene_rtv.Get());
    quad(g, -1, -1, 1, 1, none, 1);
    g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    expected = read(g, scene.Get());
    const auto st = na::native11::hud_free::GetStats();
    std::printf("%s: method \"%s\", pictures %llu, verdict: %s\n", label, st.method, static_cast<unsigned long long>(st.pictures), st.verdict);
    CHECK(got);
    if (!got) return 0;
    CHECK(st.variant == static_cast<uint32_t>(sheet_shape ? na::fglive::HudVariant::SheetRedraw : na::fglive::HudVariant::Snapshot));
    size_t differ = 0;
    for (size_t i = 0; i < (std::min)(expected.size(), hudfree.size()); ++i) differ += (i % 4 != 3) && expected[i] != hudfree[i];
    CHECK(hudfree.size() == expected.size());
    CHECK(differ == 0);
    // The paste: a different picture gets the real frame's interface back, exactly.
    auto fr = floats(real), fh = floats(hudfree);
    std::vector<float> gen(fr.size(), 0.33f);
    na::fglive::PasteHud(fr.data(), fh.data(), gen.data(), W, H, na::fglive::kPictureSdr);
    size_t hud = 0, wrong = 0;
    for (size_t p = 0; p < size_t(W) * H; ++p) {
        if (!na::fglive::HudPixel(&fr[p * 3], &fh[p * 3], na::fglive::kPictureSdr)) continue;
        ++hud;
        for (int c = 0; c < 3; ++c) wrong += gen[p * 3 + c] != fr[p * 3 + c];
    }
    std::printf("  HUD-free == scene: %zu differing bytes; interface pixels %zu, wrong after the paste %zu\n", differ, hud, wrong);
    CHECK(hud > 1000 && wrong == 0);
    na::native11::hud_free::SetWanted(false);
    return 0;
}
} // namespace

int main()
{
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const auto dir = std::filesystem::path(exe).parent_path();
    const auto suffix = std::to_wstring(GetCurrentProcessId());
    const auto name = L"hud-free11-" + suffix + L".ini";
    const auto ini = dir / name;
    const auto scope = L"HudFree11-" + suffix;
    const auto startup = (dir / (L"hud-free11-" + suffix + L".log")).wstring();
    auto set = [&](const wchar_t* section, const wchar_t* key, const wchar_t* value) { CHECK(WritePrivateProfileStringW(section, key, value, ini.c_str()) != 0); };
    set(L"host", L"enabled", L"1"); set(L"host", L"startup_log", startup.c_str()); set(L"host", L"graphics_api", L"auto");
    set(L"route_a", L"enabled", L"0"); set(L"overlay", L"enabled", L"1");
    set(L"observe", L"producers", L"0"); set(L"observe", L"early_install", L"1");
    set(L"adapter", L"enabled", L"0"); set(L"fsr", L"enabled", L"0"); set(L"xess", L"enabled", L"0");
    set(L"unreal", L"probe", L"0"); set(L"unreal", L"screen_percentage", L"0");
    set(L"settings", L"scope", scope.c_str()); set(L"present", L"proxy", L"0"); set(L"present", L"dx11_proxy", L"1");
    set(L"dlss", L"enabled", L"0"); set(L"j2", L"enabled", L"0");
    REQUIRE(na::host::StartFromIni(GetModuleHandleW(nullptr), name.c_str(), "HUD FREE 11 TEST"));
    na::host::WaitForObserveInstall();
    for (unsigned i = 0; i < 2000 && !na::host::GetStatus().init_complete; ++i) Sleep(5);
    REQUIRE(na::host::GetStatus().init_complete == 1);

    Gpu g;
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &g.dev, nullptr, &g.ctx)))
        return test::SkipNoDevice("hud_free11_test");
    ComPtr<ID3DBlob> vsb, psb, err;
    REQUIRE(SUCCEEDED(D3DCompile(kShaders, sizeof kShaders - 1, "hud", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsb, &err)));
    REQUIRE(SUCCEEDED(D3DCompile(kShaders, sizeof kShaders - 1, "hud", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psb, &err)));
    REQUIRE(SUCCEEDED(g.dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g.vs)));
    REQUIRE(SUCCEEDED(g.dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g.ps)));
    D3D11_BUFFER_DESC bd{sizeof(K), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    REQUIRE(SUCCEEDED(g.dev->CreateBuffer(&bd, nullptr, &g.cb)));
    D3D11_BLEND_DESC b{};
    b.RenderTarget[0] = {TRUE, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL};
    REQUIRE(SUCCEEDED(g.dev->CreateBlendState(&b, &g.blend)));
    b.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; // premultiplied, into the sheet
    REQUIRE(SUCCEEDED(g.dev->CreateBlendState(&b, &g.premul)));
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    REQUIRE(SUCCEEDED(g.dev->CreateRasterizerState(&rd, &g.rs)));

    HWND window = test::CreateTestWindow(L"HUD-free DX11 test", W, H);
    REQUIRE(window);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    ComPtr<IDXGIFactory2> factory;
    REQUIRE(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))));
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = W; sd.Height = H; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc = {1, 0};
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> chain;
    REQUIRE(SUCCEEDED(factory->CreateSwapChainForHwnd(g.dev.Get(), window, &sd, nullptr, nullptr, &chain)));
    REQUIRE(na::on12::NativeProxy(chain.Get()));
    REQUIRE(SUCCEEDED(chain->Present(0, 0))); // the native route installs its provider at the first present
    { ComPtr<ID3D11Device> d; CHECK(SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&d))) && d.Get() == g.dev.Get()); } // the game asks its proxied chain for its own device
    REQUIRE(run_shape(g, chain.Get(), false, "T3 interface on the shown picture") == 0);
    REQUIRE(run_shape(g, chain.Get(), true, "T2 separate HUD sheet + one final draw") == 0);

    na::host::StopSessionForTests();
    REQUIRE(na::overlay::Stop());
    DestroyWindow(window);
    DeleteFileW(ini.c_str());
    wchar_t log[MAX_PATH]{};
    na::host::ProcessLogPath(log, MAX_PATH, startup.c_str());
    DeleteFileW(log);
    return test::finish("hud_free11_test");
}
