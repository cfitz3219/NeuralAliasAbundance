#include "host_dx12/native_present/size_probe.hpp"
#include "host_dx12/native_present/size_spoof.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <intrin.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace na::native_present {
namespace {
constexpr uint32_t kApis = static_cast<uint32_t>(SizeApi::Count);
constexpr uint32_t kSites = 8;
constexpr uint64_t kAfterRequestMs = 3000;
const char* const kNames[kApis] = {"client", "window", "monitor_from_window", "monitor_info", "enum_display_settings",
                                   "system_metrics", "output_desc", "output_desc1", "mode_list", "mode_list1"};
struct Site { uintptr_t module = 0, rva = 0; uint64_t calls = 0; };
struct Entry {
    uint64_t calls = 0, after_request = 0, other_sites = 0;
    uint32_t w = 0, h = 0;
    Site sites[kSites];
};
std::mutex g_m;
Entry g_e[kApis];
std::atomic<bool> g_installed{false};
thread_local unsigned t_inside = 0; // our own calls inside a detour are not counted twice

struct Inside { Inside() { ++t_inside; } ~Inside() { --t_inside; } };

using MonitorFromWindowFn = HMONITOR(WINAPI*)(HWND, DWORD);
using GetMonitorInfoWFn = BOOL(WINAPI*)(HMONITOR, LPMONITORINFO);
using GetMonitorInfoAFn = BOOL(WINAPI*)(HMONITOR, LPMONITORINFO);
using EnumDisplaySettingsWFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DEVMODEW*);
using EnumDisplaySettingsAFn = BOOL(WINAPI*)(LPCSTR, DWORD, DEVMODEA*);
using EnumDisplaySettingsExWFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DEVMODEW*, DWORD);
using GetSystemMetricsFn = int(WINAPI*)(int);
using OutputGetDescFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIOutput*, DXGI_OUTPUT_DESC*);
using OutputGetDesc1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGIOutput6*, DXGI_OUTPUT_DESC1*);
using OutputModeListFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIOutput*, DXGI_FORMAT, UINT, UINT*, DXGI_MODE_DESC*);
using OutputModeList1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGIOutput1*, DXGI_FORMAT, UINT, UINT*, DXGI_MODE_DESC1*);
MonitorFromWindowFn o_mfw = nullptr;
GetMonitorInfoWFn o_gmiw = nullptr;
GetMonitorInfoAFn o_gmia = nullptr;
EnumDisplaySettingsWFn o_edsw = nullptr;
EnumDisplaySettingsAFn o_edsa = nullptr;
EnumDisplaySettingsExWFn o_edsexw = nullptr;
GetSystemMetricsFn o_gsm = nullptr;
OutputGetDescFn o_desc = nullptr;
OutputGetDesc1Fn o_desc1 = nullptr;
OutputModeListFn o_modes = nullptr;
OutputModeList1Fn o_modes1 = nullptr;

bool counted(const void* caller) { return !t_inside && GameCaller(caller); }
uint32_t rect_w(const RECT& r) { return r.right > r.left ? static_cast<uint32_t>(r.right - r.left) : 0; }
uint32_t rect_h(const RECT& r) { return r.bottom > r.top ? static_cast<uint32_t>(r.bottom - r.top) : 0; }

// The monitor an output belongs to, read with the original GetDesc (no rewrite, not counted).
HMONITOR output_monitor(IDXGIOutput* o) {
    DXGI_OUTPUT_DESC d{};
    Inside in; NativeScope native;
    const HRESULT hr = o_desc ? o_desc(o, &d) : o->GetDesc(&d);
    return SUCCEEDED(hr) ? d.Monitor : nullptr;
}
HMONITOR WINAPI mfw_detour(HWND w, DWORD f) {
    const void* c = _ReturnAddress();
    HMONITOR m; { Inside in; m = o_mfw(w, f); }
    if (counted(c)) NoteSizeQuery(SizeApi::MonitorFromWindowCall, c, 0, 0);
    return m;
}
BOOL WINAPI gmiw_detour(HMONITOR m, LPMONITORINFO i) {
    const void* c = _ReturnAddress();
    BOOL ok; { Inside in; ok = o_gmiw(m, i); }
    if (ok && i && counted(c)) { RewriteMonitorInfo(m, i); NoteSizeQuery(SizeApi::MonitorInfo, c, rect_w(i->rcMonitor), rect_h(i->rcMonitor)); }
    return ok;
}
BOOL WINAPI gmia_detour(HMONITOR m, LPMONITORINFO i) {
    const void* c = _ReturnAddress();
    BOOL ok; { Inside in; ok = o_gmia(m, i); }
    if (ok && i && counted(c)) { RewriteMonitorInfo(m, i); NoteSizeQuery(SizeApi::MonitorInfo, c, rect_w(i->rcMonitor), rect_h(i->rcMonitor)); }
    return ok;
}
BOOL WINAPI edsw_detour(LPCWSTR d, DWORD n, DEVMODEW* dm) {
    const void* c = _ReturnAddress();
    BOOL ok; { Inside in; ok = o_edsw(d, n, dm); }
    if (ok && dm && counted(c)) RewriteDisplayModeW(d, &dm->dmPelsWidth, &dm->dmPelsHeight);
    if (ok && dm && counted(c)) NoteSizeQuery(SizeApi::DisplaySettings, c, dm->dmPelsWidth, dm->dmPelsHeight);
    return ok;
}
BOOL WINAPI edsa_detour(LPCSTR d, DWORD n, DEVMODEA* dm) {
    const void* c = _ReturnAddress();
    BOOL ok; { Inside in; ok = o_edsa(d, n, dm); }
    if (ok && dm && counted(c)) RewriteDisplayModeA(d, &dm->dmPelsWidth, &dm->dmPelsHeight);
    if (ok && dm && counted(c)) NoteSizeQuery(SizeApi::DisplaySettings, c, dm->dmPelsWidth, dm->dmPelsHeight);
    return ok;
}
BOOL WINAPI edsexw_detour(LPCWSTR d, DWORD n, DEVMODEW* dm, DWORD f) {
    const void* c = _ReturnAddress();
    BOOL ok; { Inside in; ok = o_edsexw(d, n, dm, f); }
    if (ok && dm && counted(c)) RewriteDisplayModeW(d, &dm->dmPelsWidth, &dm->dmPelsHeight);
    if (ok && dm && counted(c)) NoteSizeQuery(SizeApi::DisplaySettings, c, dm->dmPelsWidth, dm->dmPelsHeight);
    return ok;
}
int WINAPI gsm_detour(int index) {
    const void* c = _ReturnAddress();
    int v = o_gsm(index);
    // Only the screen-size metrics; every other index passes straight through.
    if (index == SM_CXSCREEN || index == SM_CYSCREEN || index == SM_CXFULLSCREEN || index == SM_CYFULLSCREEN ||
        index == SM_CXVIRTUALSCREEN || index == SM_CYVIRTUALSCREEN) {
        if (counted(c)) {
            RewriteScreenMetric(index, &v);
            const bool x = index == SM_CXSCREEN || index == SM_CXFULLSCREEN || index == SM_CXVIRTUALSCREEN;
            NoteSizeQuery(SizeApi::SystemMetrics, c, x ? static_cast<uint32_t>(v) : 0, x ? 0 : static_cast<uint32_t>(v));
        }
    }
    return v;
}
HRESULT STDMETHODCALLTYPE desc_detour(IDXGIOutput* o, DXGI_OUTPUT_DESC* d) {
    const void* c = _ReturnAddress();
    HRESULT hr; { Inside in; hr = o_desc(o, d); }
    if (SUCCEEDED(hr) && d && counted(c)) RewriteOutputRect(d->Monitor, &d->DesktopCoordinates);
    if (SUCCEEDED(hr) && d && counted(c)) NoteSizeQuery(SizeApi::OutputGetDesc, c, rect_w(d->DesktopCoordinates), rect_h(d->DesktopCoordinates));
    return hr;
}
HRESULT STDMETHODCALLTYPE desc1_detour(IDXGIOutput6* o, DXGI_OUTPUT_DESC1* d) {
    const void* c = _ReturnAddress();
    HRESULT hr; { Inside in; hr = o_desc1(o, d); }
    if (SUCCEEDED(hr) && d && counted(c)) RewriteOutputRect(d->Monitor, &d->DesktopCoordinates);
    if (SUCCEEDED(hr) && d && counted(c)) NoteSizeQuery(SizeApi::OutputGetDesc1, c, rect_w(d->DesktopCoordinates), rect_h(d->DesktopCoordinates));
    return hr;
}
// Mode lists: the largest mode returned (the native candidate a game would pick).
HRESULT STDMETHODCALLTYPE modes_detour(IDXGIOutput* o, DXGI_FORMAT f, UINT fl, UINT* n, DXGI_MODE_DESC* m) {
    const void* c = _ReturnAddress();
    HRESULT hr; { Inside in; hr = o_modes(o, f, fl, n, m); }
    if (SUCCEEDED(hr) && n && counted(c)) {
        if (m && *n) { const HMONITOR mon = output_monitor(o); for (UINT i = 0; i < *n; ++i) RewriteOutputMode(mon, &m[i].Width, &m[i].Height); }
        uint32_t w = 0, h = 0;
        if (m) for (UINT i = 0; i < *n; ++i) if (m[i].Width * m[i].Height > w * h) { w = m[i].Width; h = m[i].Height; }
        NoteSizeQuery(SizeApi::OutputModeList, c, w, h);
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE modes1_detour(IDXGIOutput1* o, DXGI_FORMAT f, UINT fl, UINT* n, DXGI_MODE_DESC1* m) {
    const void* c = _ReturnAddress();
    HRESULT hr; { Inside in; hr = o_modes1(o, f, fl, n, m); }
    if (SUCCEEDED(hr) && n && counted(c)) {
        if (m && *n) { const HMONITOR mon = output_monitor(o); for (UINT i = 0; i < *n; ++i) RewriteOutputMode(mon, &m[i].Width, &m[i].Height); }
        uint32_t w = 0, h = 0;
        if (m) for (UINT i = 0; i < *n; ++i) if (m[i].Width * m[i].Height > w * h) { w = m[i].Width; h = m[i].Height; }
        NoteSizeQuery(SizeApi::OutputModeList1, c, w, h);
    }
    return hr;
}
const char* module_name(uintptr_t base, char* out, size_t n) {
    char path[MAX_PATH] = {};
    if (!base || !GetModuleFileNameA(reinterpret_cast<HMODULE>(base), path, MAX_PATH)) { std::snprintf(out, n, "?"); return out; }
    const char* s = std::strrchr(path, '\\');
    std::snprintf(out, n, "%s", s ? s + 1 : path);
    return out;
}
// IDXGIOutput vtable slots (IUnknown 3 + IDXGIObject 4): GetDesc 7, GetDisplayModeList 8;
// IDXGIOutput1::GetDisplayModeList1 19; IDXGIOutput6::GetDesc1 27.
constexpr uint32_t kDescSlot = 7, kModesSlot = 8, kModes1Slot = 19, kDesc1Slot = 27;
} // namespace

void NoteSizeQuery(SizeApi api, const void* caller, uint32_t w, uint32_t h)
{
    const uint32_t a = static_cast<uint32_t>(api);
    if (a >= kApis) return;
    uintptr_t base = GameModuleBase(caller); // registered game modules: no loader lock per call
    if (!base) {
        HMODULE m = nullptr;
        if (caller && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                         static_cast<LPCWSTR>(caller), &m)) base = reinterpret_cast<uintptr_t>(m);
    }
    const uintptr_t rva = reinterpret_cast<uintptr_t>(caller) - base;
    const uint64_t req = LastRequestTick();
    const bool after = req && GetTickCount64() - req <= kAfterRequestMs;
    std::lock_guard<std::mutex> lock(g_m);
    Entry& e = g_e[a];
    ++e.calls;
    if (after) ++e.after_request;
    if (w) e.w = w;
    if (h) e.h = h;
    for (Site& s : e.sites) {
        if (s.calls && s.module == base && s.rva == rva) { ++s.calls; return; }
        if (!s.calls) { s.module = base; s.rva = rva; s.calls = 1; return; }
    }
    ++e.other_sites;
}

bool InstallSizeProbe()
{
    if (g_installed.load()) return true;
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (!u || !na::hooking::Initialize()) return false;
    RegisterGameModule(GetModuleHandleW(nullptr)); // the game's EXE (also done by InstallSizeHooks)
    // A DXGI output only to read its vtable (shared by the game's outputs of this class).
    Microsoft::WRL::ComPtr<IDXGIFactory1> f;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> ad;
    Microsoft::WRL::ComPtr<IDXGIOutput> out;
    Microsoft::WRL::ComPtr<IDXGIOutput1> out1;
    Microsoft::WRL::ComPtr<IDXGIOutput6> out6;
    {
        NativeScope native;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f))) && SUCCEEDED(f->EnumAdapters1(0, &ad)) && SUCCEEDED(ad->EnumOutputs(0, &out))) {
            out.As(&out1);
            out.As(&out6);
        }
    }
    if (!na::hooking::BeginInstallWait("native-present size probe")) return false;
    auto user = [&](const char* name, void* detour, void** orig) {
        void* t = reinterpret_cast<void*>(GetProcAddress(u, name));
        return t && na::hooking::PrepareHook(t, detour, orig);
    };
    bool ok = user("MonitorFromWindow", reinterpret_cast<void*>(&mfw_detour), reinterpret_cast<void**>(&o_mfw)) &&
              user("GetMonitorInfoW", reinterpret_cast<void*>(&gmiw_detour), reinterpret_cast<void**>(&o_gmiw)) &&
              user("GetMonitorInfoA", reinterpret_cast<void*>(&gmia_detour), reinterpret_cast<void**>(&o_gmia)) &&
              user("EnumDisplaySettingsW", reinterpret_cast<void*>(&edsw_detour), reinterpret_cast<void**>(&o_edsw)) &&
              user("EnumDisplaySettingsA", reinterpret_cast<void*>(&edsa_detour), reinterpret_cast<void**>(&o_edsa)) &&
              user("EnumDisplaySettingsExW", reinterpret_cast<void*>(&edsexw_detour), reinterpret_cast<void**>(&o_edsexw)) &&
              user("GetSystemMetrics", reinterpret_cast<void*>(&gsm_detour), reinterpret_cast<void**>(&o_gsm));
    // DXGI is optional: the user32 probes stay useful without it.
    if (ok && out) {
        if (!na::hooking::PrepareVTableSlot(out.Get(), kDescSlot, reinterpret_cast<void*>(&desc_detour), reinterpret_cast<void**>(&o_desc)) ||
            !na::hooking::PrepareVTableSlot(out.Get(), kModesSlot, reinterpret_cast<void*>(&modes_detour), reinterpret_cast<void**>(&o_modes)))
            ok = false;
        if (ok && out1 && !na::hooking::PrepareVTableSlot(out1.Get(), kModes1Slot, reinterpret_cast<void*>(&modes1_detour), reinterpret_cast<void**>(&o_modes1)))
            ok = false;
        if (ok && out6 && !na::hooking::PrepareVTableSlot(out6.Get(), kDesc1Slot, reinterpret_cast<void*>(&desc1_detour), reinterpret_cast<void**>(&o_desc1)))
            ok = false;
    }
    if (!ok) { na::hooking::AbandonInstall(); return false; }
    if (!na::hooking::CommitInstall()) return false;
    g_installed.store(true);
    return true;
}
bool SizeProbeInstalled() { return g_installed.load(); }

void FormatSizeProbe(char* out, uint32_t capacity)
{
    if (!out || !capacity) return;
    out[0] = 0;
    if (!g_installed.load()) return;
    std::lock_guard<std::mutex> lock(g_m);
    size_t n = static_cast<size_t>(std::snprintf(out, capacity, "size_probe"));
    for (uint32_t a = 0; a < kApis && n < capacity; ++a) {
        const Entry& e = g_e[a];
        if (!e.calls) continue;
        int k = std::snprintf(out + n, capacity - n, " %s{n=%llu after_req=%llu last=%ux%u", kNames[a], static_cast<unsigned long long>(e.calls),
                              static_cast<unsigned long long>(e.after_request), e.w, e.h);
        if (k < 0) return;
        n += static_cast<size_t>(k);
        for (const Site& s : e.sites) {
            if (!s.calls || n >= capacity) continue;
            char mod[64];
            k = std::snprintf(out + n, capacity - n, " %s+0x%llx:%llu", module_name(s.module, mod, sizeof mod), static_cast<unsigned long long>(s.rva),
                              static_cast<unsigned long long>(s.calls));
            if (k < 0) return;
            n += static_cast<size_t>(k);
        }
        if (n < capacity) {
            k = std::snprintf(out + n, capacity - n, e.other_sites ? " other=%llu}" : "}", static_cast<unsigned long long>(e.other_sites));
            if (k < 0) return;
            n += static_cast<size_t>(k);
        }
    }
}
void ResetSizeProbeForTest()
{
    std::lock_guard<std::mutex> lock(g_m);
    for (Entry& e : g_e) e = Entry{};
}
} // namespace na::native_present
