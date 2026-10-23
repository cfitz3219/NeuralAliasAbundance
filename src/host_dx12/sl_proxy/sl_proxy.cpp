#include "host_dx12/fg/camera.hpp"
#include "host_dx12/adapters/tlou1/timing.hpp"
#include "host_dx12/adapters/tlou1/apply.hpp"
#include "host_dx12/adapters/tlou1/timing.hpp"
// Route A: sl.interposer.dll stand-in for Streamline titles.
//
// Every export is a PE forwarder (see sl_proxy_exports.inl) to sl_interposer_real.dll,
// which deployment creates by renaming the game's original sl.interposer.dll.
// Forwarding is resolved lazily by the Windows loader per export, so this
// module adds no behavior of its own yet. Typed observation wrappers for the
// sl* API replace individual forwarders only once pinned Streamline headers
// establish the exact signatures (tracked in OBSERVATION-API.md / B-002).
//
// Activation: DllMain does no real work under the loader lock â€” it only
// spawns an init thread. That thread (running after the loader lock is
// released) looks for NeuralAliasSL.ini NEXT TO THIS DLL and, only when
// [observe] enabled=1, starts the hooking core, the nvngx module watch and
// the record logger. Without the ini (or with enabled=0) the proxy stays a
// pure forwarder with zero hooks â€” the deployment-safe default.
//
// PROCESS_DETACH deliberately does nothing: stopping threads or flushing
// files under loader teardown risks deadlock, and the logger flushes every
// 250 ms during the session anyway (the final partial second may be lost;
// accepted and documented).

#include <windows.h>

#include <cstdint>
#include <cstdlib>

#include "host_dx12/fg/frame_gen.hpp"
#include "host_dx12/fg/preset_profile.hpp"
#include "host_dx12/latency/nvidia_latency.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
#include "host_dx12/ngx_observe/gpu_timing.hpp"
#include "host_dx12/overlay/presenter.hpp"
#include "host_dx12/ngx_observe/ngx_observe.hpp"
#include "host_dx12/nr/nr_pass.hpp"
#include "host_dx12/overlay/na_style.hpp"
#include "host_dx12/overlay/overlay.hpp"
#include "host_dx12/overlay/route_a_panel.hpp"
#include "host_dx12/settings.hpp"
#include "sl_proxy_exports.inl"

namespace {

HMODULE g_self = nullptr;

// Resolves <module dir>\NeuralAliasSL.ini and whether [observe] enabled=1.
// Cheap enough to call from both the init thread and the slInit wrapper.
bool observe_enabled(wchar_t* ini_out, size_t ini_cap, wchar_t* dir_out,
                     size_t dir_cap)
{
    wchar_t dir[MAX_PATH] = {};
    if (!GetModuleFileNameW(g_self, dir, MAX_PATH)) return false;
    for (int i = lstrlenW(dir) - 1; i >= 0; --i) {
        if (dir[i] == L'\\' || dir[i] == L'/') {
            dir[i] = 0;
            break;
        }
    }
    wchar_t ini[MAX_PATH];
    wsprintfW(ini, L"%s\\NeuralAliasSL.ini", dir);
    if (GetFileAttributesW(ini) == INVALID_FILE_ATTRIBUTES) return false;
    if (GetPrivateProfileIntW(L"observe", L"enabled", 0, ini) != 1) return false;
    if (ini_out) lstrcpynW(ini_out, ini, static_cast<int>(ini_cap));
    if (dir_out) lstrcpynW(dir_out, dir, static_cast<int>(dir_cap));
    return true;
}

DWORD WINAPI InitThread(LPVOID)
{
    wchar_t dir[MAX_PATH] = {};
    wchar_t ini[MAX_PATH] = {};
    if (!observe_enabled(ini, MAX_PATH, dir, MAX_PATH)) return 0;

    // Once hooks go in, this module must never unload (detours and threads
    // would dangle): pin it for the process lifetime.
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(&InitThread), &pinned);

    if (!na::hooking::Initialize()) return 0;
    bool sync_hook =
        GetPrivateProfileIntW(L"observe", L"sync_loader_hook", 0, ini) == 1;
    // Takeover settings ([override] preset / resolution_mode / ratio) are
    // read by the NGX layer live; the overlay edits and saves them.
    na::settings::SetIniPath(ini);
    na::settings::Load();
    na::latency::Initialize();
    na::fg::preset::Initialize();
    // Deterministic attach: hook the driver's NGX core before Streamline
    // loads it (closes the Init/GetCapabilityParameters race, A17).
    na::ngx::PreloadAndHookNgxCore();
    na::tlou1::InitializeApply();
    na::tlou1::InitializeTiming();
    if (!na::ngx::StartModuleWatch(sync_hook)) return 0;
    // Live Neural Rendering (A30) and Frame Generation (A59): hand the
    // runtime's original entry points to the passes. Inert until switched on.
    {
        na::nr::Runtime rt;
        if (na::ngx::GetRuntimeEntryPoints(&rt.create_feature, &rt.evaluate_feature, &rt.release_feature,
                                           &rt.allocate_parameters, &rt.destroy_parameters,
                                           &rt.get_capability_parameters)) {
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
    }
    // A59: the presenter owns the swap chain on Route A ([present] proxy=0
    // restores the old path; then Frame Generation is unavailable).
    na::present::SetEnabled(GetPrivateProfileIntW(L"present", L"proxy", 1, ini) == 1);
    // Per-evaluate detail logging is a diagnostics option ([observe] detail=1).
    na::ngx::SetDetailLogging(GetPrivateProfileIntW(L"observe", L"detail", 0, ini) == 1);
    // GPU timestamps around the DLSS evaluate ([observe] gpu_timing=0 to disable).
    na::gputime::SetEnabled(GetPrivateProfileIntW(L"observe", L"gpu_timing", 1, ini) == 1);
    // In-game menu (Home). [overlay] enabled=0 disables it; scale= and
    // font_body=/font_title= tune size and typeface without a rebuild.
    if (GetPrivateProfileIntW(L"overlay", L"enabled", 1, ini) == 1) {
        wchar_t wbuf[64] = {};
        GetPrivateProfileStringW(L"overlay", L"scale", L"1.25", wbuf, 64, ini);
        na::ui::SetScale(static_cast<float>(_wtof(wbuf)));
        char body[260] = {}, title[260] = {};
        char ini_a[MAX_PATH];
        WideCharToMultiByte(CP_ACP, 0, ini, -1, ini_a, MAX_PATH, nullptr, nullptr);
        GetPrivateProfileStringA("overlay", "font_body", "", body, sizeof(body), ini_a);
        GetPrivateProfileStringA("overlay", "font_title", "", title, sizeof(title), ini_a);
        na::ui::SetFontPaths(body, title);
        na::overlay::RegisterRouteAMenu();
        wchar_t status_path[MAX_PATH];
        wsprintfW(status_path, L"%s\\NeuralAliasSL-status.log", dir);
        na::overlay::SetStatusLogPath(status_path); // A55: measurements recorded every 5 s
        na::overlay::Start("ROUTE A", nullptr); // A30: the host draws the pages
    }
    wchar_t log_path[MAX_PATH];
    wchar_t configured[MAX_PATH] = {};
    GetPrivateProfileStringW(L"observe", L"log", L"", configured, MAX_PATH, ini);
    if (configured[0])
        lstrcpynW(log_path, configured, MAX_PATH);
    else
        wsprintfW(log_path, L"%s\\NeuralAliasSL-observe.log", dir);
    na::ngx::StartLogger(log_path);
    return 0;
}

} // namespace

extern "C" __declspec(dllexport) const char* NeuralAliasSLProxyIdentity()
{
    return "NeuralAliasAbundance sl.interposer forwarding proxy 0.3.0";
}

// Typed wrapper for slInit (SL 1.x: bool slInit(const sl::Preferences&, int)).
// The Preferences struct is passed through untouched (its 1.4.7 layout is
// unverified â€” we never read it). Belt-and-braces for the NGX attach: if the
// game reaches slInit before the init thread finished, hook the NGX core
// synchronously here, on the game thread, outside the loader lock.
namespace {
using SlInitFn = bool(__cdecl*)(const void* prefs, int app_id);
SlInitFn resolve_real_slinit()
{
    HMODULE real = GetModuleHandleW(L"sl_interposer_real.dll");
    if (!real) real = LoadLibraryW(L"sl_interposer_real.dll");
    if (!real) return nullptr;
    return reinterpret_cast<SlInitFn>(
        reinterpret_cast<void*>(GetProcAddress(real, "slInit")));
}
} // namespace

extern "C" __declspec(dllexport) bool __cdecl slInit(const void* prefs, int app_id)
{
    // Only when observation is enabled by the ini â€” otherwise the proxy must
    // stay a pure forwarder with zero hooks.
    if (observe_enabled(nullptr, 0, nullptr, 0) && na::hooking::Initialize())
        na::ngx::PreloadAndHookNgxCore();
    SlInitFn real = resolve_real_slinit();
    return real ? real(prefs, app_id) : false;
}

// A71: typed wrappers for the two Streamline calls that carry the game's DLSS
// settings (SL 1.x: bool slSetFeatureConstants(Feature, const void* consts,
// uint32_t frameIndex, uint32_t id); bool slGetFeatureSettings(Feature,
// const void* consts, void* settings)). Arguments pass through untouched; the
// call is recorded with its thread and 24 raw return addresses so the game's
// settings-apply path (synchronous, in game code) is visible in the status
// log. Nothing in the constants block is interpreted (layout unverified).
namespace {
using SlSetFeatureConstantsFn = bool(__cdecl*)(uint32_t, const void*, uint32_t, uint32_t);
using SlGetFeatureSettingsFn = bool(__cdecl*)(uint32_t, const void*, void*);
template <typename Fn> Fn resolve_real(const char* name)
{
    HMODULE real = GetModuleHandleW(L"sl_interposer_real.dll");
    if (!real) real = LoadLibraryW(L"sl_interposer_real.dll");
    return real ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(real, name))) : nullptr;
}
} // namespace

namespace {
bool copy_camera(na::fg::Camera& out,const void* in) {
    __try { memcpy(&out,in,sizeof(out)); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}
extern "C" __declspec(dllexport) bool __cdecl slSetConstants(const void* constants, uint32_t frame, uint32_t view) {
    using Fn=bool(__cdecl*)(const void*,uint32_t,uint32_t);
    static Fn real=resolve_real<Fn>("slSetConstants");
    const bool ok=real && real(constants,frame,view);
    if (ok && constants && na::tlou1::TimingAttached()) {
        na::fg::Camera copy{};
        if (!copy_camera(copy,constants)) return ok;
        na::fg::RecordCamera(uint64_t(frame)+1,view,copy);
    }
    return ok;
}
extern "C" __declspec(dllexport) bool __cdecl slEvaluateFeature(void* list,uint32_t feature,uint32_t frame,uint32_t view) {
    using Fn=bool(__cdecl*)(void*,uint32_t,uint32_t,uint32_t);
    static Fn real=resolve_real<Fn>("slEvaluateFeature");
    na::fg::EvaluationScope scope(frame,view);
    return real && real(list,feature,frame,view);
}

extern "C" __declspec(dllexport) bool __cdecl slSetFeatureConstants(uint32_t feature, const void* consts, uint32_t frame_index, uint32_t id)
{
    static SlSetFeatureConstantsFn real = resolve_real<SlSetFeatureConstantsFn>("slSetFeatureConstants");
    if (observe_enabled(nullptr, 0, nullptr, 0)) {
        void* frames[24] = {};
        USHORT n = RtlCaptureStackBackTrace(1, 24, frames, nullptr);
        na::ngx::RecordSlCall(1, feature, consts, frames, n);
    }
    return real ? real(feature, consts, frame_index, id) : false;
}

extern "C" __declspec(dllexport) bool __cdecl slGetFeatureSettings(uint32_t feature, const void* consts, void* settings)
{
    static SlGetFeatureSettingsFn real = resolve_real<SlGetFeatureSettingsFn>("slGetFeatureSettings");
    if (observe_enabled(nullptr, 0, nullptr, 0)) {
        void* frames[24] = {};
        USHORT n = RtlCaptureStackBackTrace(1, 24, frames, nullptr);
        na::ngx::RecordSlCall(2, feature, consts, frames, n);
    }
    return real ? real(feature, consts, settings) : false;
}

// Diagnostic export so tests/tools outside this DLL can read observation
// state (the ngx_observe statics live inside this module).
extern "C" __declspec(dllexport) void NeuralAliasSLProxyStats(
    uint64_t* out_evaluates, uint64_t* out_creates, uint32_t* out_hooked)
{
    na::ngx::Stats s = na::ngx::GetStats();
    if (out_evaluates) *out_evaluates = s.evaluates;
    if (out_creates) *out_creates = s.creates;
    if (out_hooked) *out_hooked = s.hooked_functions;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = instance;
        DisableThreadLibraryCalls(instance);
        HANDLE t = CreateThread(nullptr, 0, &InitThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
