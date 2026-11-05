// DX9 plan step D2: a small DX9 "game" runs unchanged through our d3d9.dll proxy.
//
//   d3d9_proxy_test <NeuralAliasD3D9.dll> <work dir> on|off
//
// The proxy is copied into <work dir>/<mode>/ with a NeuralAliasHost.ini, loaded
// by full path, and the game calls its Direct3DCreate9 like any DX9 game would.
//  off: a pure forward. The device is plain system d3d9 (not 9On12).
//  on:  the device runs on D3D9On12 (or, where this Windows has none, on plain
//       d3d9 with the fallback logged); a FULLSCREEN request comes out borderless
//       windowed, at creation and at Reset; the run log is written.
// Both: clear + Present + Reset work, and the picture reads back right.
// Exit 0 = pass, 1 = fail, 77 = skipped (no DX9 device at all here).
#include <windows.h>
#include <d3d9.h>
#if __has_include(<d3d9on12.h>)
#include <d3d12.h>
#include <d3d9on12.h>
#define NA_HAVE_9ON12 1
#endif
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cmath>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "host_dx9/d3d9_proxy.hpp"
#include "providers/camera/matrix_candidates.hpp"

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {

int g_fail = 0;
void Check(bool ok, const char* what)
{
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    std::fflush(stdout);
    if (!ok) ++g_fail;
}

using PFN_Create9 = IDirect3D9*(WINAPI*)(UINT);
using PFN_Status = int(__cdecl*)(na::dx9::ProxyStatus*);

na::dx9::ProxyStatus Status(PFN_Status f)
{
    na::dx9::ProxyStatus s{};
    s.size = sizeof(s);
    if (!f || !f(&s)) s = {};
    return s;
}

// Reads the back buffer's centre pixel (X8R8G8B8).
bool CentrePixel(IDirect3DDevice9* dev, UINT w, UINT h, DWORD* out)
{
    ComPtr<IDirect3DSurface9> bb, sys;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return false;
    if (FAILED(dev->CreateOffscreenPlainSurface(w, h, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr))) return false;
    if (FAILED(dev->GetRenderTargetData(bb.Get(), sys.Get()))) return false;
    D3DLOCKED_RECT lr{};
    if (FAILED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return false;
    *out = static_cast<const DWORD*>(lr.pBits)[(h / 2) * (lr.Pitch / 4) + w / 2] & 0x00FFFFFF;
    sys->UnlockRect();
    return true;
}


// ---- D3: a small DX9 game the hooks must understand.
// Scene: a vertex shader with per-object World x ViewProj in c0..c3 and the
// camera's ViewProj + inverse in c8 / c12, 40 objects with depth test, into
// its own colour texture ("post") or straight into the back buffer. Then (post)
// a tonemap that samples the scene texture into the back buffer, then a HUD
// with depth test off.
using M = na::camera::Matrix;
M Mul(const M& a, const M& b)
{
    M r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) r[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
    return r;
}
void Regs(const M& m, float* out)
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) out[c * 4 + r] = static_cast<float>(m[r * 4 + c]);
}

struct V3 { float x, y, z; DWORD c; };
struct VT { float x, y, z, rhw, u, v; };

ComPtr<IDirect3DVertexShader9> SceneShader(IDirect3DDevice9* dev)
{
    const char* src =
        "float4x4 wvp : register(c0);\n"
        "struct O { float4 p : POSITION; float4 c : COLOR0; };\n"
        "O main(float3 p : POSITION, float4 c : COLOR0) { O o; o.p = mul(float4(p, 1), wvp); o.c = c; return o; }\n";
    ComPtr<ID3DBlob> code, err;
    if (FAILED(D3DCompile(src, std::strlen(src), "scene_vs", nullptr, nullptr, "main", "vs_2_0", 0, 0, &code, &err))) {
        std::printf("     vs compile: %s\n", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return nullptr;
    }
    ComPtr<IDirect3DVertexShader9> vs;
    dev->CreateVertexShader(static_cast<const DWORD*>(code->GetBufferPointer()), &vs);
    return vs;
}

bool RunGame(IDirect3DDevice9* dev, PFN_Status status, bool post, UINT w, UINT h)
{
    ComPtr<IDirect3DVertexShader9> vs = SceneShader(dev);
    ComPtr<IDirect3DVertexDeclaration9> decl;
    const D3DVERTEXELEMENT9 el[] = {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
                                    {0, 12, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0},
                                    D3DDECL_END()};
    dev->CreateVertexDeclaration(el, &decl);
    if (!vs || !decl) { Check(false, "game: shader + declaration"); return false; }
    ComPtr<IDirect3DTexture9> scene;
    ComPtr<IDirect3DSurface9> scene_rt, depth, back, auto_depth;
    if (post) {
        dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &scene, nullptr);
        if (!scene) dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &scene, nullptr);
        if (scene) scene->GetSurfaceLevel(0, &scene_rt);
        dev->CreateDepthStencilSurface(w, h, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &depth, nullptr);
        if (!scene_rt || !depth) { Check(false, "game: scene target + depth"); return false; }
    }
    dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back);
    dev->GetDepthStencilSurface(&auto_depth);
    if (!post) depth = auto_depth;

    const V3 tri[] = {{-1, -1, 0, 0xff8040ff}, {0, 1, 0, 0xff40ff80}, {1, -1, 0, 0xffff4080}};
    const float sw = static_cast<float>(w), sh = static_cast<float>(h);
    const VT quad[] = {{0, 0, 0, 1, 0, 0}, {sw, 0, 0, 1, 1, 0}, {0, sh, 0, 1, 0, 1}, {sw, sh, 0, 1, 1, 1}};
    const VT hud[] = {{8, 8, 0, 1, 0, 0}, {64, 8, 0, 1, 1, 0}, {8, 24, 0, 1, 0, 1}, {64, 24, 0, 1, 1, 1}};

    for (int frame = 0; frame < 8; ++frame) {
        const double ys = 1.0 / std::tan(0.5), xs = ys * sh / sw, q = 1000.0 / (1000.0 - 0.1);
        const M proj{xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, q, 1, 0, 0, -0.1 * q, 0};
        const double a = 0.02 * frame, c = std::cos(a), s = std::sin(a);
        M world_cam = Mul(M{c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, 0, 1}, M{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.1 * frame, 2, -20, 1});
        M view{}, inv{};
        na::camera::Invert(world_cam, view);
        const M vp = Mul(view, proj);
        na::camera::Invert(vp, inv);

        dev->BeginScene();
        dev->SetRenderTarget(0, post ? scene_rt.Get() : back.Get());
        dev->SetDepthStencilSurface(depth.Get());
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(20, 30, 60), 1.f, 0);
        dev->SetVertexShader(vs.Get());
        dev->SetVertexDeclaration(decl.Get());
        dev->SetTexture(0, nullptr);
        float r[16];
        Regs(vp, r);
        dev->SetVertexShaderConstantF(8, r, 4);
        Regs(inv, r);
        dev->SetVertexShaderConstantF(12, r, 4);
        for (int i = 0; i < 40; ++i) {
            const M world{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, (i % 8) * 2.5 - 9, (i / 8) * 2.0 - 4, (i % 5) * 1.0, 1};
            Regs(Mul(world, vp), r);
            dev->SetVertexShaderConstantF(0, r, 4);
            dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(V3));
        }
        dev->SetVertexShader(nullptr);
        dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        if (post) {
            // Tonemap: sample the scene into the back buffer.
            dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
            dev->SetRenderTarget(0, back.Get());
            dev->SetDepthStencilSurface(nullptr);
            dev->SetTexture(0, scene.Get());
            dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(VT));
            dev->SetTexture(0, nullptr);
        }
        // HUD: depth test off.
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, hud, sizeof(VT));
        dev->EndScene();
        dev->Present(nullptr, nullptr, nullptr, nullptr);
    }
    dev->SetDepthStencilSurface(auto_depth.Get());
    dev->SetRenderTarget(0, back.Get());

    const auto st = Status(status);
    const auto& k = st.hooks;
    std::printf("     %s game: frames %u selected %u colour %ux%u fmt %u depth fmt %u, point %u, fired read %u hud %u present %u, camera %u (source %u, c%u), scene draws %u of %u\n",
                post ? "post" : "back-buffer", k.frames, k.selected, k.color_w, k.color_h, k.color_format, k.depth_format, k.point_kind,
                k.fired_read, k.fired_hud, k.fired_present, k.camera_found, k.camera_source, k.camera_register, k.scene_draws, k.draws);
    Check(k.selected && k.color_w == w && k.color_h == h, post ? "post game: scene colour + depth picked" : "back-buffer game: back buffer + depth picked");
    Check(k.camera_found && k.camera_source == 1 && k.camera_register == 8, "camera found in the shader constants at c8");
    Check(k.scene_draws == 40, "the 40 objects are the scene draws (HUD and tonemap are not)");
    if (post) Check(k.point_kind == 1 && k.fired_read >= 1, "post game: DLSS point before the tonemap's read of the scene");
    else Check(k.point_kind == 2 && k.fired_hud >= 1, "back-buffer game: DLSS point before the HUD (HUD rule)");
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 4) {
        std::printf("usage: d3d9_proxy_test <proxy dll> <work dir> on|off\n");
        return 1;
    }
    const bool on = std::strcmp(argv[3], "on") == 0;
    std::printf("d3d9 proxy test (%s), %d-bit\n", on ? "on" : "off", static_cast<int>(sizeof(void*) * 8));

    // The proxy beside its own ini, as deploy-dx9.ps1 will lay it out.
    const fs::path dir = fs::path(argv[2]) / (on ? "on" : "off");
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path dll = dir / "NeuralAliasD3D9.dll";
    fs::copy_file(argv[1], dll, fs::copy_options::overwrite_existing, ec);
    Check(!ec, "proxy copied beside its ini");
    const fs::path log = dir / "dx9.log";
    {
        std::ofstream ini(dir / "NeuralAliasHost.ini");
        ini << "[dx9]\nenabled=" << (on ? 1 : 0) << "\nlog=" << log.string() << "\n";
    }

    HMODULE proxy = LoadLibraryW(dll.wstring().c_str());
    Check(proxy != nullptr, "proxy loads");
    if (!proxy) return 1;
    const auto create = reinterpret_cast<PFN_Create9>(GetProcAddress(proxy, "Direct3DCreate9"));
    const auto status = reinterpret_cast<PFN_Status>(GetProcAddress(proxy, "NA_D3D9_QueryStatus"));
    Check(create && status, "proxy exports Direct3DCreate9 and its status");
    for (const char* name : {"Direct3DCreate9Ex", "Direct3DCreate9On12", "D3DPERF_BeginEvent", "D3DPERF_EndEvent", "D3DPERF_SetMarker",
                             "D3DPERF_GetStatus", "Direct3DShaderValidatorCreate9"})
        if (!GetProcAddress(proxy, name)) {
            std::printf("     missing export %s\n", name);
            Check(false, "every d3d9 export a game may import is there");
        }
    if (!create) return 1;

    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"na_d3d9_proxy_test";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"dx9 game", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, nullptr, nullptr, wc.hInstance, nullptr);

    ComPtr<IDirect3D9> d3d;
    d3d.Attach(create(D3D_SDK_VERSION));
    if (!d3d) {
        std::printf("SKIP: no Direct3D9 here\n");
        return 77;
    }

    // "on" asks for exclusive fullscreen, as many DX9 games do; the proxy must make it borderless.
    const UINT W = 64, H = 48;
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = on ? FALSE : TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = W;
    pp.BackBufferHeight = H;
    pp.BackBufferCount = 1;
    pp.hDeviceWindow = hwnd;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.FullScreen_RefreshRateInHz = on ? 60 : 0;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    if (on) {
        // Fullscreen needs a real mode; take the desktop's.
        D3DDISPLAYMODE dm{};
        d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &dm);
        pp.BackBufferWidth = dm.Width ? dm.Width : W;
        pp.BackBufferHeight = dm.Height ? dm.Height : H;
        pp.FullScreen_RefreshRateInHz = dm.RefreshRate;
    }
    const D3DPRESENT_PARAMETERS asked = pp;

    ComPtr<IDirect3DDevice9> dev;
    for (DWORD vp : {DWORD(D3DCREATE_HARDWARE_VERTEXPROCESSING), DWORD(D3DCREATE_SOFTWARE_VERTEXPROCESSING)}) {
        pp = asked;
        if (SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, vp, &pp, &dev))) break;
        dev.Reset();
    }
    if (!dev && !on) {
        std::printf("SKIP: no DX9 device here\n");
        return 77;
    }
    Check(dev != nullptr, "the game's CreateDevice succeeds through the proxy");
    if (!dev) return 1;

    const auto s = Status(status);
    std::printf("     status: enabled %u on12 %u our_device %u creates %u fallbacks %u devices %u borderless %u\n", s.enabled, s.on12,
                s.our_device, s.creates, s.fallbacks, s.devices, s.borderless);
    bool is_on12 = false;
#ifdef NA_HAVE_9ON12
    {
        ComPtr<IDirect3DDevice9On12> on12;
        is_on12 = SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&on12))) && on12;
    }
#endif
    if (on) {
        Check(s.enabled == 1 && s.creates == 1 && s.devices == 1, "proxy is on and saw the game's device");
        if (s.on12) Check(is_on12, "the game's device is a D3D9On12 device");
        else Check(s.fallbacks == 1, "no 9On12 here: plain d3d9, fallback counted");
        Check(s.borderless == 1, "fullscreen request turned into borderless windowed");
        RECT rc{};
        GetWindowRect(hwnd, &rc);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        Check(EqualRect(&rc, &mi.rcMonitor) && !(GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CAPTION), "the window covers its monitor with no frame");
    } else {
        Check(s.enabled == 0 && s.creates == 0 && s.devices == 0, "proxy is off: nothing counted");
        Check(!is_on12, "proxy is off: the device is plain system d3d9");
    }

    // A frame: clear to a colour, Present, read it back.
    auto frame = [&](D3DCOLOR c, const char* what) {
        dev->BeginScene();
        dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, c, 1.f, 0);
        dev->EndScene();
        DWORD px = 0;
        const bool read = CentrePixel(dev.Get(), pp.BackBufferWidth, pp.BackBufferHeight, &px);
        Check(read && px == (c & 0x00FFFFFF), what);
        Check(SUCCEEDED(dev->Present(nullptr, nullptr, nullptr, nullptr)), "Present");
    };
    frame(D3DCOLOR_XRGB(10, 200, 30), "the cleared colour reads back");

    // Reset, as a game does on resize / Alt-Tab (fullscreen again when "on").
    pp = asked;
    pp.BackBufferWidth = on ? asked.BackBufferWidth : W * 2;
    pp.BackBufferHeight = on ? asked.BackBufferHeight : H * 2;
    const HRESULT rr = dev->Reset(&pp);
    Check(SUCCEEDED(rr), "Reset succeeds");
    if (SUCCEEDED(rr)) frame(D3DCOLOR_XRGB(200, 20, 90), "a frame after Reset reads back");
    if (on && SUCCEEDED(rr)) {
        // Two games on the same device: post-processing, then scene-in-back-buffer
        // (a Reset in between, as a game does on a settings change).
        RunGame(dev.Get(), status, true, pp.BackBufferWidth, pp.BackBufferHeight);
        pp = asked;
        Check(SUCCEEDED(dev->Reset(&pp)), "second Reset");
        RunGame(dev.Get(), status, false, pp.BackBufferWidth, pp.BackBufferHeight);
    }
    const auto s2 = Status(status);
    if (on) Check(s2.resets == 2 && s2.borderless == 3, "Resets seen and kept borderless");
    else Check(s2.resets == 0, "proxy is off: Reset not hooked");

    dev.Reset();
    d3d.Reset();
    std::ifstream lf(log.string().substr(0, log.string().size() - 4) + "-" + std::to_string(GetCurrentProcessId()) + ".log");
    std::string text((std::istreambuf_iterator<char>(lf)), std::istreambuf_iterator<char>());
    if (on) {
        Check(text.find("NeuralAlias DX9 proxy") != std::string::npos && text.find("device created") != std::string::npos, "run log written");
        std::printf("---- proxy log\n%s----\n", text.c_str());
    } else {
        Check(text.empty(), "proxy is off: no log");
    }

    DestroyWindow(hwnd);
    std::printf("%s: d3d9 proxy (%s, %d-bit)\n", g_fail ? "FAIL" : "PASS", on ? "on" : "off", static_cast<int>(sizeof(void*) * 8));
    return g_fail ? 1 : 0;
}
