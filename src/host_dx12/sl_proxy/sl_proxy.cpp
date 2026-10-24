#include "host_dx12/fg/camera.hpp"
#include "host_dx12/adapters/tlou1/apply.hpp"
#include "host_dx12/adapters/tlou1/timing.hpp"
#include "host_dx12/sl_proxy/sl_abi.hpp"
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

#include <atomic>
#include <cstdint>
#include <cstring>
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
#include "host_dx12/upscalers/ffx_intercept.hpp"
#include "host_dx12/upscalers/xess_intercept.hpp"
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
    // Gate 2: the Streamline entry point also hosts non-native upscalers.
    // Observe by default; replacement remains an explicit per-provider opt-in.
    if (GetPrivateProfileIntW(L"fsr", L"enabled", 1, ini) == 1) {
        na::ffx::SetMode(GetPrivateProfileIntW(L"fsr", L"replace", 0, ini) == 1
            ? na::ffx::Mode::ReplaceWithDlss : na::ffx::Mode::Observe);
        na::ffx::StartModuleWatch();
    }
    if (GetPrivateProfileIntW(L"xess", L"enabled", 1, ini) == 1) {
        na::xess::SetMode(GetPrivateProfileIntW(L"xess", L"replace", 0, ini) == 1
            ? na::xess::Mode::ReplaceWithDlss : na::xess::Mode::Observe);
        na::xess::StartModuleWatch();
    }
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

// C7b: ABI-neutral wrappers. Streamline 1.x and 2.x share these export names
// but not their signatures (2.x passes FrameToken& / ViewportHandle& /
// uint64_t sdkVersion where 1.x passed 32-bit values, and returns sl::Result,
// 0 = ok, where 1.x returned bool). Every wrapper therefore takes and forwards
// five full-width integer arguments and returns the full RAX value, which is
// exact for both on x64 (integer/pointer args in RCX, RDX, R8, R9, then the
// stack; a fifth slot a 4-argument caller did not pass is caller stack memory
// that the real function ignores). Arguments are interpreted only where the
// ABI can be proven from the data itself (sl_abi.hpp).
namespace {
using Fwd5 = uint64_t(__cdecl*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
Fwd5 resolve_fwd(const char* name)
{
    HMODULE real = GetModuleHandleW(L"sl_interposer_real.dll");
    if (!real) real = LoadLibraryW(L"sl_interposer_real.dll");
    return real ? reinterpret_cast<Fwd5>(reinterpret_cast<void*>(GetProcAddress(real, name))) : nullptr;
}
// Resolution is retried until it succeeds (a failed lookup is never cached).
Fwd5 cached_fwd(std::atomic<Fwd5>& slot, const char* name)
{
    Fwd5 f = slot.load(std::memory_order_acquire);
    if (!f && (f = resolve_fwd(name)) != nullptr) slot.store(f, std::memory_order_release);
    return f;
}
// Gate 2 R-306: what a wrapper returns when the real Streamline function is
// missing (no real interposer, or no such export), so nothing ran. It must be
// a FAILURE for both ABIs without knowing which one the caller uses:
//   SL 1.x returns bool  -> the caller tests AL:  0x100 has AL == 0  -> false.
//   SL 2.x returns sl::Result -> the caller tests EAX against eOk (0):
//                           0x100 != 0 -> an error (no defined Result uses 256).
constexpr uint64_t kNotExecuted = 0x100;

// Guarded copies (no C++ objects in these frames: SEH only).
size_t guarded_copy(void* dst, const void* src, size_t n)
{
    __try {
        memcpy(dst, src, n);
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
// SL 2.x FrameToken: virtual operator uint32_t() is its first vtable entry.
bool frame_token_value(const void* token, uint32_t* out)
{
    uint8_t head[8 + na::sl::kHeaderSize] = {};
    if (!token || guarded_copy(head, token, sizeof(head)) != sizeof(head)) return false;
    if (!na::sl::HasType(head + 8, na::sl::kHeaderSize, na::sl::kFrameTokenGuid)) return false;
    __try {
        using Op = uint32_t(__cdecl*)(const void*);
        *out = (*reinterpret_cast<Op const* const*>(token))[0](token);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::atomic<uint32_t> g_sl_layout{0};     // na::sl::Layout last seen in slSetConstants
std::atomic<uint64_t> g_sl_camera_seen{0}; // camera blocks decoded (any layout)
} // namespace

extern "C" __declspec(dllexport) uint64_t __cdecl slInit(uintptr_t prefs, uintptr_t app_or_sdk_version, uintptr_t c, uintptr_t d, uintptr_t e)
{
    // Only when observation is enabled by the ini; otherwise the proxy must
    // stay a pure forwarder with zero hooks. Belt-and-braces for the NGX attach:
    // hook the NGX core here, on the game thread, outside the loader lock.
    if (observe_enabled(nullptr, 0, nullptr, 0) && na::hooking::Initialize())
        na::ngx::PreloadAndHookNgxCore();
    static std::atomic<Fwd5> real_slot{nullptr};
    const Fwd5 real = cached_fwd(real_slot, "slInit");
    return real ? real(prefs, app_or_sdk_version, c, d, e) : kNotExecuted;
}

extern "C" __declspec(dllexport) uint64_t __cdecl slSetConstants(uintptr_t constants, uintptr_t frame, uintptr_t view, uintptr_t d, uintptr_t e)
{
    static std::atomic<Fwd5> real_slot{nullptr};
    const Fwd5 real = cached_fwd(real_slot, "slSetConstants");
    if (!real) return kNotExecuted;
    const uint64_t ret = real(constants, frame, view, d, e);
    if (!constants) return ret;
    // Which ABI? Only 2.x Constants carry the Constants type GUID.
    uint8_t buf[na::sl::kV2ConstantsSize + 1] = {};
    size_t got = guarded_copy(buf, reinterpret_cast<const void*>(constants), na::sl::kHeaderSize);
    if (got && na::sl::HasType(buf, got, na::sl::kConstantsGuid))
        got = guarded_copy(buf, reinterpret_cast<const void*>(constants), na::sl::kV2ConstantsSize);
    else
        got = guarded_copy(buf, reinterpret_cast<const void*>(constants), na::sl::kV1ConstantsSize);
    na::fg::Camera cam{};
    const na::sl::Layout layout = na::sl::DecodeConstants(buf, got, cam);
    if (layout == na::sl::Layout::Unknown) return ret;
    uint32_t frame_index = 0, viewport = 0;
    bool ok = false;
    if (layout == na::sl::Layout::V1) {
        ok = (ret & 0xFFu) != 0; // bool
        frame_index = static_cast<uint32_t>(frame);
        viewport = static_cast<uint32_t>(view);
    } else {
        ok = static_cast<uint32_t>(ret) == 0; // sl::Result::eOk
        uint8_t vh[na::sl::kHeaderSize + 4] = {};
        const size_t vgot = view ? guarded_copy(vh, reinterpret_cast<const void*>(view), sizeof(vh)) : 0;
        ok = ok && frame_token_value(reinterpret_cast<const void*>(frame), &frame_index) &&
             na::sl::ViewportValue(vh, vgot, &viewport);
    }
    if (!ok) return ret;
    g_sl_layout.store(static_cast<uint32_t>(layout), std::memory_order_relaxed);
    g_sl_camera_seen.fetch_add(1, std::memory_order_relaxed);
    na::fg::RecordCamera(uint64_t(frame_index) + 1, viewport, cam); // validated inside (ValidCamera)
    return ret;
}

extern "C" __declspec(dllexport) uint64_t __cdecl slEvaluateFeature(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e)
{
    static std::atomic<Fwd5> real_slot{nullptr};
    const Fwd5 real = cached_fwd(real_slot, "slEvaluateFeature");
    if (!real) return kNotExecuted;
    // 1.x: (void* list, Feature, uint32 frame, uint32 viewport): the explicit
    // frame/viewport scope lets the NGX evaluate find this frame's camera.
    // 2.x: (Feature, const FrameToken&, inputs**, uint32, CommandBuffer*): the
    // viewport travels inside `inputs`; no scope (FG then waits for a camera
    // it can match, which is the safe behaviour).
    const uint32_t layout = g_sl_layout.load(std::memory_order_relaxed);
    uint32_t token_frame = 0;
    const bool v1 = layout == static_cast<uint32_t>(na::sl::Layout::V1) ||
                    (layout == 0 && (c >> 32) == 0 && (d >> 32) == 0 && !frame_token_value(reinterpret_cast<const void*>(b), &token_frame));
    if (v1) {
        na::fg::EvaluationScope scope(static_cast<uint32_t>(c), static_cast<uint32_t>(d));
        return real(a, b, c, d, e);
    }
    return real(a, b, c, d, e);
}

extern "C" __declspec(dllexport) uint64_t __cdecl slSetFeatureConstants(uintptr_t feature, uintptr_t consts, uintptr_t c, uintptr_t d, uintptr_t e)
{
    static std::atomic<Fwd5> real_slot{nullptr};
    const Fwd5 real = cached_fwd(real_slot, "slSetFeatureConstants");
    if (observe_enabled(nullptr, 0, nullptr, 0)) {
        void* frames[24] = {};
        USHORT n = RtlCaptureStackBackTrace(1, 24, frames, nullptr);
        na::ngx::RecordSlCall(1, static_cast<uint32_t>(feature), reinterpret_cast<const void*>(consts), frames, n);
    }
    return real ? real(feature, consts, c, d, e) : kNotExecuted;
}

extern "C" __declspec(dllexport) uint64_t __cdecl slGetFeatureSettings(uintptr_t feature, uintptr_t consts, uintptr_t settings, uintptr_t d, uintptr_t e)
{
    static std::atomic<Fwd5> real_slot{nullptr};
    const Fwd5 real = cached_fwd(real_slot, "slGetFeatureSettings");
    if (observe_enabled(nullptr, 0, nullptr, 0)) {
        void* frames[24] = {};
        USHORT n = RtlCaptureStackBackTrace(1, 24, frames, nullptr);
        na::ngx::RecordSlCall(2, static_cast<uint32_t>(feature), reinterpret_cast<const void*>(consts), frames, n);
    }
    return real ? real(feature, consts, settings, d, e) : kNotExecuted;
}

// Gate 2 R-307 diagnostics: the camera recorded for (engine frame + 1, viewport),
// copied into `out` (sizeof(na::fg::Camera) bytes). False when none was stored.
extern "C" __declspec(dllexport) bool NeuralAliasSLProxyFindCamera(uint64_t frame_plus_one, uint32_t viewport, void* out)
{
    na::fg::Camera c{};
    if (!out || !na::fg::FindCamera(frame_plus_one, viewport, c)) return false;
    memcpy(out, &c, sizeof(c));
    return true;
}

// C7b diagnostics: which Streamline Constants ABI this game uses (0 unknown,
// 1 = 1.x, 2 = 2.x) and how many camera blocks were decoded.
extern "C" __declspec(dllexport) uint32_t NeuralAliasSLProxyLayout(uint64_t* out_camera_blocks)
{
    if (out_camera_blocks) *out_camera_blocks = g_sl_camera_seen.load(std::memory_order_relaxed);
    return g_sl_layout.load(std::memory_order_relaxed);
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
