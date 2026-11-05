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
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "host_dx9/d3d9_proxy.hpp"

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
    const auto s2 = Status(status);
    if (on) Check(s2.resets == 1 && s2.borderless == 2, "Reset seen and kept borderless");
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
