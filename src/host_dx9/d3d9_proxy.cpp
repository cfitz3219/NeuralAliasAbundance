// d3d9.dll proxy (DX9 plan step D2, docs/DX9-PLAN.md). Owner: Claude.
//
// Off unless NeuralAliasHost.ini beside this DLL has [dx9] enabled=1; then every
// export is a plain forward to the system d3d9.dll and nothing else happens.
//
// On: Direct3DCreate9(Ex) goes through D3D9On12 (Windows' own DX9-on-DX12
// layer) on OUR D3D12 device and queue, so later steps can hand the picture and
// depth to the 64-bit DLSS helper. If 9On12 refuses our device it runs on its
// own; if 9On12 is missing or fails, the game gets plain system d3d9 (logged).
// Fullscreen requests become borderless windowed (9On12 has no exclusive
// fullscreen), at device creation and at every Reset.
//
// Knows nothing about any game. Builds at 32 and 64 bits.
#include <windows.h>
#include <d3d9.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#if __has_include(<d3d9on12.h>)
#include <d3d9on12.h>
#define NA_HAVE_9ON12 1
#endif
#include <MinHook.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>

#include "host_dx9/d3d9_proxy.hpp"

using Microsoft::WRL::ComPtr;

namespace {

// ------------------------------------------------------------------ config + log
struct Config {
    bool enabled = false;
    bool borderless = true;
    bool own_device = true; // try 9On12 on our D3D12 device first
    std::wstring log_path;
};

HMODULE SelfModule()
{
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&SelfModule), &m);
    return m;
}

std::wstring SelfDir()
{
    wchar_t p[MAX_PATH]{};
    GetModuleFileNameW(SelfModule(), p, MAX_PATH);
    std::wstring s = p;
    const auto slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : s.substr(0, slash);
}

std::once_flag g_cfg_once;
Config g_cfg;
std::mutex g_log_mu;
FILE* g_log = nullptr;
na::dx9::ProxyStatus g_status{};
std::atomic<uint32_t> g_creates{0}, g_fallbacks{0}, g_devices{0}, g_borderless{0}, g_resets{0};
std::atomic<uint32_t> g_on12{0}, g_our_device{0};

void Log(const char* fmt, ...)
{
    std::lock_guard<std::mutex> l(g_log_mu);
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(g_log, "%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a;
    va_start(a, fmt);
    std::vfprintf(g_log, fmt, a);
    va_end(a);
    std::fputc('\n', g_log);
    std::fflush(g_log);
}

void OpenLog()
{
    std::wstring path = g_cfg.log_path;
    if (path.empty()) {
        wchar_t base[MAX_PATH]{};
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
        if (!n || n >= MAX_PATH) return;
        const std::wstring dir = std::wstring(base) + L"\\NeuralAlias\\dx9";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        path = dir + L"\\NeuralAliasDX9.log";
    }
    const auto dot = path.find_last_of(L'.');
    path.insert(dot == std::wstring::npos ? path.size() : dot, L"-" + std::to_wstring(GetCurrentProcessId()));
    std::lock_guard<std::mutex> l(g_log_mu);
    g_log = _wfsopen(path.c_str(), L"w", _SH_DENYWR); // readable while the game runs
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, g_status.log_path, MAX_PATH, nullptr, nullptr);
}

const Config& Cfg()
{
    std::call_once(g_cfg_once, [] {
        const std::wstring ini = SelfDir() + L"\\NeuralAliasHost.ini";
        g_cfg.enabled = GetPrivateProfileIntW(L"dx9", L"enabled", 0, ini.c_str()) == 1;
        if (!g_cfg.enabled) return;
        g_cfg.borderless = GetPrivateProfileIntW(L"dx9", L"borderless", 1, ini.c_str()) == 1;
        g_cfg.own_device = GetPrivateProfileIntW(L"dx9", L"own_device", 1, ini.c_str()) == 1;
        wchar_t b[MAX_PATH]{};
        GetPrivateProfileStringW(L"dx9", L"log", L"", b, MAX_PATH, ini.c_str());
        g_cfg.log_path = b;
        OpenLog();
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        char exe8[MAX_PATH * 3]{};
        WideCharToMultiByte(CP_UTF8, 0, exe, -1, exe8, sizeof(exe8), nullptr, nullptr);
        Log("NeuralAlias DX9 proxy (%d-bit) in %s: borderless=%d own_device=%d", static_cast<int>(sizeof(void*) * 8), exe8,
            g_cfg.borderless ? 1 : 0, g_cfg.own_device ? 1 : 0);
    });
    return g_cfg;
}

// ------------------------------------------------------------------ system d3d9
HMODULE SystemD3D9()
{
    static HMODULE module = [] {
        wchar_t dir[MAX_PATH + 16]{};
        const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
        if (!n || n >= MAX_PATH || wcscat_s(dir, L"\\d3d9.dll") != 0) return static_cast<HMODULE>(nullptr);
        return LoadLibraryExW(dir, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    }();
    return module;
}

template <class F>
F Sys(const char* name)
{
    const HMODULE m = SystemD3D9();
    return m ? reinterpret_cast<F>(GetProcAddress(m, name)) : nullptr;
}

// ------------------------------------------------------------------ borderless
// Turns one fullscreen request into borderless windowed on the window's monitor.
// Returns true when it changed something.
bool MakeBorderless(D3DPRESENT_PARAMETERS* pp, HWND focus)
{
    if (!pp || pp->Windowed || !Cfg().borderless) return false;
    HWND w = pp->hDeviceWindow ? pp->hDeviceWindow : focus;
    pp->Windowed = TRUE;
    pp->FullScreen_RefreshRateInHz = 0;
    if (pp->PresentationInterval == D3DPRESENT_INTERVAL_DEFAULT) pp->PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    if (w && IsWindow(w)) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            const LONG_PTR style = GetWindowLongPtrW(w, GWL_STYLE);
            const LONG_PTR ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
            SetWindowLongPtrW(w, GWL_STYLE, (style & ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW)) | WS_POPUP);
            SetWindowLongPtrW(w, GWL_EXSTYLE, ex & ~static_cast<LONG_PTR>(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE));
            const RECT& r = mi.rcMonitor;
            SetWindowPos(w, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
        }
    }
    g_borderless.fetch_add(1, std::memory_order_relaxed);
    Log("fullscreen %ux%u requested: running borderless windowed instead", pp->BackBufferWidth, pp->BackBufferHeight);
    return true;
}

// ------------------------------------------------------------------ vtable hooks
// Every IDirect3D9 (and device) of one implementation shares one vtable, so a
// method is hooked once per implementation (9On12, and system d3d9 on fallback).
// The detour finds its original by the object's own vtable entry.
struct HookSlot {
    std::atomic<void*> target{nullptr};
    std::atomic<void*> original{nullptr};
};
constexpr int kMaxImpl = 4;
std::mutex g_hook_mu;

struct Method {
    Method(int i, void* d) : index(i), detour(d) {}
    int index;
    void* detour;
    HookSlot slots[kMaxImpl];
};

void* OriginalFor(Method& m, void* self)
{
    void* target = (*reinterpret_cast<void***>(self))[m.index];
    for (auto& s : m.slots)
        if (s.target.load(std::memory_order_acquire) == target) return s.original.load(std::memory_order_relaxed);
    return nullptr;
}

void HookMethod(Method& m, void* object)
{
    if (!object) return;
    void* target = (*reinterpret_cast<void***>(object))[m.index];
    std::lock_guard<std::mutex> l(g_hook_mu);
    static bool mh = MH_Initialize() == MH_OK;
    if (!mh) return;
    for (auto& s : m.slots)
        if (s.target.load() == target) return; // already hooked
    for (auto& s : m.slots) {
        if (s.target.load()) continue;
        void* orig = nullptr;
        if (MH_CreateHook(target, m.detour, &orig) != MH_OK) {
            Log("hook of vtable slot %d failed", m.index);
            return;
        }
        // Published before the hook goes live, so the detour always finds its original.
        s.original.store(orig, std::memory_order_relaxed);
        s.target.store(target, std::memory_order_release);
        if (MH_EnableHook(target) != MH_OK) Log("enabling the hook of vtable slot %d failed", m.index);
        return;
    }
}

// IDirect3D9 slot 16, IDirect3D9Ex slot 20, IDirect3DDevice9 slot 16, IDirect3DDevice9Ex slot 132.
HRESULT WINAPI CreateDevice_h(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
HRESULT WINAPI CreateDeviceEx_h(IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*, IDirect3DDevice9Ex**);
HRESULT WINAPI Reset_h(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
HRESULT WINAPI ResetEx_h(IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);

Method g_create_device{16, reinterpret_cast<void*>(&CreateDevice_h)};
Method g_create_device_ex{20, reinterpret_cast<void*>(&CreateDeviceEx_h)};
Method g_reset{16, reinterpret_cast<void*>(&Reset_h)};
Method g_reset_ex{132, reinterpret_cast<void*>(&ResetEx_h)};

void HookDevice(IDirect3DDevice9* dev)
{
    HookMethod(g_reset, dev);
    ComPtr<IDirect3DDevice9Ex> ex;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&ex))) && ex) HookMethod(g_reset_ex, ex.Get());
}

void Created(IDirect3DDevice9* dev, const D3DPRESENT_PARAMETERS* pp, DWORD flags)
{
    g_devices.fetch_add(1, std::memory_order_relaxed);
    Log("device created: %ux%u fmt %d, depth %d (auto %d), msaa %d, windowed %d, flags 0x%lx", pp ? pp->BackBufferWidth : 0,
        pp ? pp->BackBufferHeight : 0, pp ? static_cast<int>(pp->BackBufferFormat) : 0, pp ? static_cast<int>(pp->AutoDepthStencilFormat) : 0,
        pp ? pp->EnableAutoDepthStencil : 0, pp ? static_cast<int>(pp->MultiSampleType) : 0, pp ? pp->Windowed : 0, static_cast<unsigned long>(flags));
    HookDevice(dev);
}

HRESULT WINAPI CreateDevice_h(IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags, D3DPRESENT_PARAMETERS* pp,
                              IDirect3DDevice9** out)
{
    using F = HRESULT(WINAPI*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
    const auto orig = reinterpret_cast<F>(OriginalFor(g_create_device, self));
    if (!orig) return D3DERR_INVALIDCALL;
    MakeBorderless(pp, focus);
    const HRESULT hr = orig(self, adapter, type, focus, flags, pp, out);
    if (SUCCEEDED(hr) && out && *out) Created(*out, pp, flags);
    else Log("CreateDevice failed: 0x%08lx", static_cast<unsigned long>(hr));
    return hr;
}

HRESULT WINAPI CreateDeviceEx_h(IDirect3D9Ex* self, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags, D3DPRESENT_PARAMETERS* pp,
                                D3DDISPLAYMODEEX* mode, IDirect3DDevice9Ex** out)
{
    using F = HRESULT(WINAPI*)(IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*, IDirect3DDevice9Ex**);
    const auto orig = reinterpret_cast<F>(OriginalFor(g_create_device_ex, self));
    if (!orig) return D3DERR_INVALIDCALL;
    if (MakeBorderless(pp, focus)) mode = nullptr; // windowed: no display mode
    const HRESULT hr = orig(self, adapter, type, focus, flags, pp, mode, out);
    if (SUCCEEDED(hr) && out && *out) Created(*out, pp, flags);
    else Log("CreateDeviceEx failed: 0x%08lx", static_cast<unsigned long>(hr));
    return hr;
}

HRESULT WINAPI Reset_h(IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* pp)
{
    using F = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
    const auto orig = reinterpret_cast<F>(OriginalFor(g_reset, self));
    if (!orig) return D3DERR_INVALIDCALL;
    g_resets.fetch_add(1, std::memory_order_relaxed);
    D3DDEVICE_CREATION_PARAMETERS cp{};
    self->GetCreationParameters(&cp);
    MakeBorderless(pp, cp.hFocusWindow);
    const HRESULT hr = orig(self, pp);
    Log("Reset %ux%u: 0x%08lx", pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0, static_cast<unsigned long>(hr));
    return hr;
}

HRESULT WINAPI ResetEx_h(IDirect3DDevice9Ex* self, D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* mode)
{
    using F = HRESULT(WINAPI*)(IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);
    const auto orig = reinterpret_cast<F>(OriginalFor(g_reset_ex, self));
    if (!orig) return D3DERR_INVALIDCALL;
    g_resets.fetch_add(1, std::memory_order_relaxed);
    D3DDEVICE_CREATION_PARAMETERS cp{};
    self->GetCreationParameters(&cp);
    if (MakeBorderless(pp, cp.hFocusWindow)) mode = nullptr;
    const HRESULT hr = orig(self, pp, mode);
    Log("ResetEx %ux%u: 0x%08lx", pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0, static_cast<unsigned long>(hr));
    return hr;
}

// ------------------------------------------------------------------ D3D12 side
// One D3D12 device + queue for the process: 9On12 runs on it, and the
// transport (D4) uses the same queue to copy the picture out.
struct Our12 {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
};
std::mutex g_d12_mu;
Our12 g_d12;

bool MakeOur12()
{
    std::lock_guard<std::mutex> l(g_d12_mu);
    if (g_d12.device) return true;
    ComPtr<ID3D12Device> dev;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) {
        Log("no D3D12 device on the default adapter");
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> q;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)))) {
        Log("no D3D12 queue");
        return false;
    }
    g_d12.device = dev;
    g_d12.queue = q;
    return true;
}

#ifdef NA_HAVE_9ON12
using PFN_Create9On12 = IDirect3D9*(WINAPI*)(UINT, D3D9ON12_ARGS*, UINT);
using PFN_Create9On12Ex = HRESULT(WINAPI*)(UINT, D3D9ON12_ARGS*, UINT, IDirect3D9Ex**);

// Fills args for 9On12: on our device and queue when wanted and possible.
bool FillArgs(D3D9ON12_ARGS* args, bool ours)
{
    *args = {};
    args->Enable9On12 = TRUE;
    if (!ours) return false;
    if (!MakeOur12()) return false;
    std::lock_guard<std::mutex> l(g_d12_mu);
    args->pD3D12Device = g_d12.device.Get();
    args->ppD3D12Queues[0] = g_d12.queue.Get();
    args->NumQueues = 1;
    return true;
}
#endif

void Fallback(const char* why)
{
    g_fallbacks.fetch_add(1, std::memory_order_relaxed);
    g_on12.store(0);
    Log("D3D9On12 not used (%s): plain system d3d9", why);
}

} // namespace

// ====================================================================== exports
extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT sdk)
{
    using F = IDirect3D9*(WINAPI*)(UINT);
    const auto sys = Sys<F>("Direct3DCreate9");
    if (!Cfg().enabled) return sys ? sys(sdk) : nullptr;
    g_creates.fetch_add(1, std::memory_order_relaxed);
    IDirect3D9* d3d = nullptr;
#ifdef NA_HAVE_9ON12
    if (const auto on12 = Sys<PFN_Create9On12>("Direct3DCreate9On12")) {
        for (int attempt = Cfg().own_device ? 0 : 1; attempt < 2 && !d3d; ++attempt) {
            D3D9ON12_ARGS args;
            const bool ours = FillArgs(&args, attempt == 0);
            if (attempt == 0 && !ours) continue;
            d3d = on12(sdk, &args, 1);
            if (d3d) {
                g_on12.store(1);
                g_our_device.store(ours ? 1u : 0u);
                Log("Direct3DCreate9(%u) -> D3D9On12 on %s D3D12 device", sdk, ours ? "OUR" : "its own");
            } else if (ours) {
                Log("D3D9On12 refused our D3D12 device; letting it make its own");
            }
        }
        if (!d3d) Fallback("Direct3DCreate9On12 failed");
    } else {
        Fallback("this Windows has no Direct3DCreate9On12");
    }
#else
    Fallback("built without d3d9on12.h");
#endif
    if (!d3d && sys) d3d = sys(sdk);
    if (d3d) HookMethod(g_create_device, d3d);
    return d3d;
}

extern "C" HRESULT WINAPI Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex** out)
{
    using F = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
    const auto sys = Sys<F>("Direct3DCreate9Ex");
    if (!Cfg().enabled) return sys ? sys(sdk, out) : D3DERR_NOTAVAILABLE;
    if (!out) return D3DERR_INVALIDCALL;
    *out = nullptr;
    g_creates.fetch_add(1, std::memory_order_relaxed);
    HRESULT hr = D3DERR_NOTAVAILABLE;
#ifdef NA_HAVE_9ON12
    if (const auto on12 = Sys<PFN_Create9On12Ex>("Direct3DCreate9On12Ex")) {
        for (int attempt = Cfg().own_device ? 0 : 1; attempt < 2 && !*out; ++attempt) {
            D3D9ON12_ARGS args;
            const bool ours = FillArgs(&args, attempt == 0);
            if (attempt == 0 && !ours) continue;
            hr = on12(sdk, &args, 1, out);
            if (SUCCEEDED(hr) && *out) {
                g_on12.store(1);
                g_our_device.store(ours ? 1u : 0u);
                Log("Direct3DCreate9Ex(%u) -> D3D9On12 on %s D3D12 device", sdk, ours ? "OUR" : "its own");
            } else {
                *out = nullptr;
                if (ours) Log("D3D9On12 refused our D3D12 device (0x%08lx); letting it make its own", static_cast<unsigned long>(hr));
            }
        }
        if (!*out) Fallback("Direct3DCreate9On12Ex failed");
    } else {
        Fallback("this Windows has no Direct3DCreate9On12Ex");
    }
#else
    Fallback("built without d3d9on12.h");
#endif
    if (!*out) hr = sys ? sys(sdk, out) : D3DERR_NOTAVAILABLE;
    if (SUCCEEDED(hr) && *out) {
        HookMethod(g_create_device, *out);
        HookMethod(g_create_device_ex, *out);
    }
    return hr;
}

// A game that asks for 9On12 itself gets it unchanged.
#ifdef NA_HAVE_9ON12
using NaArgs9On12 = D3D9ON12_ARGS;
#else
using NaArgs9On12 = void;
#endif
extern "C" IDirect3D9* WINAPI Direct3DCreate9On12(UINT sdk, NaArgs9On12* args, UINT n)
{
    using F = IDirect3D9*(WINAPI*)(UINT, NaArgs9On12*, UINT);
    const auto f = Sys<F>("Direct3DCreate9On12");
    return f ? f(sdk, args, n) : nullptr;
}

extern "C" HRESULT WINAPI Direct3DCreate9On12Ex(UINT sdk, NaArgs9On12* args, UINT n, IDirect3D9Ex** out)
{
    using F = HRESULT(WINAPI*)(UINT, NaArgs9On12*, UINT, IDirect3D9Ex**);
    const auto f = Sys<F>("Direct3DCreate9On12Ex");
    return f ? f(sdk, args, n, out) : D3DERR_NOTAVAILABLE;
}

extern "C" void* WINAPI Direct3DShaderValidatorCreate9()
{
    using F = void*(WINAPI*)();
    const auto f = Sys<F>("Direct3DShaderValidatorCreate9");
    return f ? f() : nullptr;
}

extern "C" void WINAPI Direct3D9EnableMaximizedWindowedModeShim(UINT v)
{
    using F = void(WINAPI*)(UINT);
    if (const auto f = Sys<F>("Direct3D9EnableMaximizedWindowedModeShim")) f(v);
}

extern "C" int WINAPI D3DPERF_BeginEvent(D3DCOLOR c, LPCWSTR name)
{
    using F = int(WINAPI*)(D3DCOLOR, LPCWSTR);
    const auto f = Sys<F>("D3DPERF_BeginEvent");
    return f ? f(c, name) : -1;
}

extern "C" int WINAPI D3DPERF_EndEvent()
{
    using F = int(WINAPI*)();
    const auto f = Sys<F>("D3DPERF_EndEvent");
    return f ? f() : -1;
}

extern "C" DWORD WINAPI D3DPERF_GetStatus()
{
    using F = DWORD(WINAPI*)();
    const auto f = Sys<F>("D3DPERF_GetStatus");
    return f ? f() : 0;
}

extern "C" BOOL WINAPI D3DPERF_QueryRepeatFrame()
{
    using F = BOOL(WINAPI*)();
    const auto f = Sys<F>("D3DPERF_QueryRepeatFrame");
    return f ? f() : FALSE;
}

extern "C" void WINAPI D3DPERF_SetMarker(D3DCOLOR c, LPCWSTR name)
{
    using F = void(WINAPI*)(D3DCOLOR, LPCWSTR);
    if (const auto f = Sys<F>("D3DPERF_SetMarker")) f(c, name);
}

extern "C" void WINAPI D3DPERF_SetOptions(DWORD o)
{
    using F = void(WINAPI*)(DWORD);
    if (const auto f = Sys<F>("D3DPERF_SetOptions")) f(o);
}

extern "C" void WINAPI D3DPERF_SetRegion(D3DCOLOR c, LPCWSTR name)
{
    using F = void(WINAPI*)(D3DCOLOR, LPCWSTR);
    if (const auto f = Sys<F>("D3DPERF_SetRegion")) f(c, name);
}

extern "C" void WINAPI DebugSetMute()
{
    using F = void(WINAPI*)();
    if (const auto f = Sys<F>("DebugSetMute")) f();
}

extern "C" int WINAPI DebugSetLevel()
{
    using F = int(WINAPI*)();
    const auto f = Sys<F>("DebugSetLevel");
    return f ? f() : 0;
}

extern "C" int __cdecl NA_D3D9_QueryStatus(na::dx9::ProxyStatus* s)
{
    if (!s || s->size != sizeof(*s)) return 0;
    const Config& c = Cfg();
    na::dx9::ProxyStatus out = g_status;
    out.size = sizeof(out);
    out.enabled = c.enabled ? 1u : 0u;
    out.on12 = g_on12.load();
    out.our_device = g_our_device.load();
    out.creates = g_creates.load();
    out.fallbacks = g_fallbacks.load();
    out.devices = g_devices.load();
    out.borderless = g_borderless.load();
    out.resets = g_resets.load();
    *s = out;
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    // Everything else happens on the first export call, never inside DllMain.
    return TRUE;
}
