#include "host_dx12/adapters/tlou1/apply.hpp"
#include "host_dx12/host_boot/host_boot.hpp"
#include "host_dx12/host_boot/session.hpp"
#include "host_dx12/j2/live_dlaa.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <mutex>

#include "host_dx12/fg/frame_gen.hpp"
#include "host_dx12/fg/preset_profile.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
#include "host_dx12/host_boot/game_adapter.h"
#include "host_dx12/ngx_observe/gpu_timing.hpp"
#include "host_dx12/ngx_observe/ngx_observe.hpp"
#include "host_dx12/nr/nr_pass.hpp"
#include "host_dx12/overlay/na_style.hpp"
#include "host_dx12/overlay/overlay.hpp"
#include "host_dx12/overlay/presenter.hpp"
#include "host_dx12/overlay/route_a_panel.hpp"
#include "host_dx12/settings.hpp"
#include "host_dx12/upscalers/ffx_intercept.hpp"
#include "host_dx12/upscalers/xess_intercept.hpp"
#include "host_dx12/observe/producers.hpp"

namespace na::host {
namespace {

std::mutex g_mutex;
Status g_status{};
HMODULE g_self = nullptr;
wchar_t g_ini[MAX_PATH] = {};
// G3-03: with [observe] producers=1, the game's first DXGI factory waits
// (bounded) until the init thread has installed the observation producers, so
// the game's device, heaps and resources are created under observation.
HANDLE g_install_event = nullptr;              // manual reset; set once install was attempted
std::atomic<DWORD> g_init_thread{0};
void signal_install_attempted()
{
    if (g_install_event) SetEvent(g_install_event);
}

// Optional worker-side startup diagnostics, including failures before Present.
void boot_log(const char* message)
{
    wchar_t path[MAX_PATH]{};
    GetPrivateProfileStringW(L"host", L"startup_log", L"", path, MAX_PATH, g_ini);
    if (!path[0]) return;
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"a") == 0 && file) {
        std::fprintf(file, "pid=%lu %s\n", GetCurrentProcessId(), message);
        std::fclose(file);
    }
}

void set_error(const char* text)
{
    boot_log(text);
    std::lock_guard<std::mutex> lock(g_mutex);
    strncpy_s(g_status.error, text, _TRUNCATE);
}

DWORD WINAPI InitThread(LPVOID)
{
    g_init_thread.store(GetCurrentThreadId());
    struct Release { ~Release() { signal_install_attempted(); } } release; // every exit path
    boot_log("host init thread entered");
    // Once hooks go in, this module must never unload.
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(&InitThread), &pinned);
    if (!na::hooking::Initialize()) { set_error("hooking core failed to initialise"); return 0; }
    na::settings::SetIniPath(g_ini);
    na::settings::Load();
    na::host::StartSession();
    // J2 DLAA reconstructs texture detail over frames: the game's mip-mapped
    // samplers sample one mip sharper (temporal-upscaler rule log2(render/display) - 1
    // = -1 at native). Configure before installing producer hooks, after the
    // previous-session marker is read. [j2] mip_bias overrides; 0 turns it off.
    if (GetPrivateProfileIntW(L"j2", L"enabled", 0, g_ini) == 1 && GetPrivateProfileIntW(L"observe", L"producers", 0, g_ini) == 1) {
        wchar_t mb[32] = {};
        GetPrivateProfileStringW(L"j2", L"mip_bias", L"-1.0", mb, 32, g_ini);
        na::observe::SetSamplerMipBias(na::settings::SafeStart()?0.f:static_cast<float>(_wtof(mb)));
        // Live switching from the menu (no restart): twin root signatures/pipelines are
        // built at creation for the other levels. [j2] mip_bias_live=0 turns it off.
        na::observe::SetSamplerMipBiasLiveEnabled(GetPrivateProfileIntW(L"j2", L"mip_bias_live", 1, g_ini) != 0);
    }

    if (GetPrivateProfileIntW(L"overlay", L"enabled", 1, g_ini) == 1) {
        wchar_t wbuf[64] = {};
        GetPrivateProfileStringW(L"overlay", L"scale", L"1.25", wbuf, 64, g_ini);
        na::ui::SetScale(static_cast<float>(_wtof(wbuf)));
        char body[260] = {}, title[260] = {}, ini_a[MAX_PATH];
        WideCharToMultiByte(CP_ACP, 0, g_ini, -1, ini_a, MAX_PATH, nullptr, nullptr);
        GetPrivateProfileStringA("overlay", "font_body", "", body, sizeof(body), ini_a);
        GetPrivateProfileStringA("overlay", "font_title", "", title, sizeof(title), ini_a);
        na::ui::SetFontPaths(body, title);
    }
    // A61: Route A services for a DLSS game WITHOUT Streamline (the game loads
    // nvngx_dlss.dll itself and talks to the driver's NGX core directly). The
    // same steps the sl.interposer proxy takes, in the same order: hook the
    // NGX core before the game reaches it, watch for the core module, wire
    // the runtimes for Neural Rendering and Frame Generation, register the
    // Route A pages, enable the presenter. Off unless [route_a] enabled=1.
    bool route_a = GetPrivateProfileIntW(L"route_a", L"enabled", 0, g_ini) == 1;
    bool route_a_started = false;
    if (route_a) {
        na::fg::preset::Initialize();
        na::fg::preset::ReadUpscalerOverrides(); // read-only: does the NVIDIA app force a DLSS preset?
        na::ngx::PreloadAndHookNgxCore();
        na::tlou1::InitializeApply();
        bool sync_hook = GetPrivateProfileIntW(L"observe", L"sync_loader_hook", 0, g_ini) == 1;
        route_a_started = na::ngx::StartModuleWatch(sync_hook);
        na::nr::Runtime rt;
        if (na::ngx::GetRuntimeEntryPoints(&rt.create_feature, &rt.evaluate_feature, &rt.release_feature,
                                           &rt.allocate_parameters, &rt.destroy_parameters, &rt.get_capability_parameters)) {
            na::nr::SetRuntime(rt);
            na::fg::Runtime fr;
            fr.create_feature = rt.create_feature;
            fr.evaluate_feature = rt.evaluate_feature;
            fr.release_feature = rt.release_feature;
            fr.allocate_parameters = rt.allocate_parameters;
            fr.destroy_parameters = rt.destroy_parameters;
            fr.get_capability_parameters = rt.get_capability_parameters;
            na::fg::SetRuntime(fr);
        }
        na::ngx::SetDetailLogging(GetPrivateProfileIntW(L"observe", L"detail", 0, g_ini) == 1);
        na::gputime::SetEnabled(GetPrivateProfileIntW(L"observe", L"gpu_timing", 1, g_ini) == 1);
        na::overlay::RegisterRouteAMenu();
        na::present::SetEnabled(GetPrivateProfileIntW(L"present", L"proxy", 1, g_ini) == 1);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_status.route_a_enabled = 1;
            g_status.route_a_started = route_a_started ? 1u : 0u;
            if (!route_a_started) strncpy_s(g_status.error, "NGX module watch failed to start", _TRUNCATE);
        }
    }
    // C7a: FSR 3.1+ (FidelityFX API) interception. Observing is on by default
    // (read-only: records what the game gives FSR); replacing FSR's output with
    // DLSS is opt-in via [fsr] replace=1. Off entirely with [fsr] enabled=0.
    if (GetPrivateProfileIntW(L"fsr", L"enabled", 1, g_ini) == 1) {
        na::ffx::SetMode(!na::settings::SafeStart() && GetPrivateProfileIntW(L"fsr", L"replace", 0, g_ini) == 1 ? na::ffx::Mode::ReplaceWithDlss
                                                                                  : na::ffx::Mode::Observe);
        na::ffx::StartModuleWatch();
    }
    // C7b: XeSS interception, same policy ([xess] enabled / replace).
    if (GetPrivateProfileIntW(L"xess", L"enabled", 1, g_ini) == 1) {
        na::xess::SetMode(!na::settings::SafeStart() && GetPrivateProfileIntW(L"xess", L"replace", 0, g_ini) == 1 ? na::xess::Mode::ReplaceWithDlss
                                                                                    : na::xess::Mode::Observe);
        na::xess::StartModuleWatch();
    }
    // Adapter phase 1: menu sections before the overlay starts.
    bool adapter_enabled = GetPrivateProfileIntW(L"adapter", L"enabled", 1, g_ini) == 1;
    bool prepared = adapter_enabled && na_adapter_prepare() != 0;
    boot_log(prepared ? "adapter prepared" : "adapter not prepared");
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_status.adapter_prepared = prepared ? 1u : 0u;
        strncpy_s(g_status.adapter_name, na_adapter_name(), _TRUNCATE);
    }
    char tag[32];
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        strncpy_s(tag, g_status.tag, _TRUNCATE);
    }
    {
        wchar_t status_path[MAX_PATH];
        lstrcpynW(status_path, g_ini, MAX_PATH);
        for (int i = lstrlenW(status_path) - 1; i >= 0; --i)
            if (status_path[i] == L'\\' || status_path[i] == L'/') { status_path[i] = 0; break; }
        lstrcatW(status_path, L"\\NeuralAliasHost-status.log");
        // The game folder may not be writable (Program Files): [host] status_log overrides.
        wchar_t custom[MAX_PATH] = {};
        if (GetPrivateProfileStringW(L"host", L"status_log", L"", custom, MAX_PATH, g_ini) > 0) lstrcpynW(status_path, custom, MAX_PATH);
        na::overlay::SetStatusLogPath(status_path); // A55
        na::overlay::SetMenuDiagnostics(GetPrivateProfileIntW(L"host", L"menu_diagnostics", 0, g_ini) == 1);
    }
    // C8: discovery observation producers (H1/H2). Off by default until a
    // consumer (A5 classifier) runs; [observe] producers=1 turns them on.
    na::observe::RequestInstall(GetPrivateProfileIntW(L"observe", L"producers", 0, g_ini) == 1);
    // J2 live DLAA (needs the producers and the discovery adapter): off unless [j2] enabled=1.
    na::j2::SetEnabled(GetPrivateProfileIntW(L"j2", L"enabled", 0, g_ini) == 1 &&
                       GetPrivateProfileIntW(L"observe", L"producers", 0, g_ini) == 1);
    if (na::j2::Configured()) na::overlay::RegisterLiveDlaaMenu();
    if(na::settings::SafeStart())na::j2::SetPaused(true);
    bool started = na::overlay::Start(tag, nullptr);
    signal_install_attempted(); // producers were installed (or could not be) inside Start
    boot_log(started ? "overlay started" : "overlay start failed");
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_status.host_started = started ? 1u : 0u;
    }
    if (!started) { set_error("overlay/host start failed"); return 0; }
    if (route_a) { // A61: the DLSS call log beside the ini, as the sl proxy writes it
        wchar_t log_path[MAX_PATH], configured[MAX_PATH] = {};
        GetPrivateProfileStringW(L"observe", L"log", L"", configured, MAX_PATH, g_ini);
        if (configured[0]) {
            lstrcpynW(log_path, configured, MAX_PATH);
        } else {
            lstrcpynW(log_path, g_ini, MAX_PATH);
            for (int i = lstrlenW(log_path) - 1; i >= 0; --i)
                if (log_path[i] == L'\\' || log_path[i] == L'/') { log_path[i] = 0; break; }
            lstrcatW(log_path, L"\\NeuralAliasHost-observe.log");
        }
        na::ngx::StartLogger(log_path);
    }
    // Adapter phase 2: subscriptions and registry after the host owns Execute/discovery.
    if (prepared) {
        bool ok = na_adapter_start() != 0;
        boot_log(ok ? "adapter started" : "adapter start failed");
        std::lock_guard<std::mutex> lock(g_mutex);
        g_status.adapter_started = ok ? 1u : 0u;
        if (!ok) strncpy_s(g_status.error, "adapter start failed; host runs without it", _TRUNCATE);
    }
    return 0;
}

} // namespace

bool StartFromIni(HMODULE self, const wchar_t* ini_name, const char* default_tag)
{
    g_self = self;
    wchar_t dir[MAX_PATH] = {};
    if (!GetModuleFileNameW(self, dir, MAX_PATH)) return false;
    for (int i = lstrlenW(dir) - 1; i >= 0; --i)
        if (dir[i] == L'\\' || dir[i] == L'/') { dir[i] = 0; break; }
    wsprintfW(g_ini, L"%s\\%s", dir, ini_name);
    if (GetFileAttributesW(g_ini) == INVALID_FILE_ATTRIBUTES) return false;
    if (GetPrivateProfileIntW(L"host", L"enabled", 0, g_ini) != 1) return false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_status.host_enabled = 1;
        char tag_a[32] = {};
        wchar_t tag_w[32] = {};
        GetPrivateProfileStringW(L"host", L"tag", L"", tag_w, 32, g_ini);
        if (tag_w[0]) WideCharToMultiByte(CP_ACP, 0, tag_w, -1, tag_a, 32, nullptr, nullptr);
        strncpy_s(g_status.tag, tag_a[0] ? tag_a : (default_tag ? default_tag : "HOST"), _TRUNCATE);
    }
    if (GetPrivateProfileIntW(L"observe", L"producers", 0, g_ini) == 1 &&
        GetPrivateProfileIntW(L"observe", L"early_install", 1, g_ini) == 1)
        g_install_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE t = CreateThread(nullptr, 0, &InitThread, nullptr, 0, nullptr);
    if (!t) {
        signal_install_attempted();
        return false;
    }
    CloseHandle(t);
    return true;
}

void WaitForObserveInstall()
{
    if (!g_install_event) return;
    // The init thread's own setup creates factories through this proxy.
    const DWORD init = g_init_thread.load();
    if (init && init == GetCurrentThreadId()) return;
    // Bounded: if the init thread cannot progress (e.g. the caller holds the
    // loader lock), the game continues and early objects stay PREEXISTING.
    WaitForSingleObject(g_install_event, 3000);
}

Status GetStatus()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_status;
}

} // namespace na::host
