#include "host_dx12/sl_boundary/sl_dlss_mode.hpp"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>

#include "host_dx12/hooking/vtable_hook.hpp"
#include "host_dx12/sl_boundary/sl_tags.hpp"

// See sl_dlss_mode.hpp. Layouts from the public Streamline header
// include/sl_dlss.h (MIT, NVIDIA-RTX/Streamline main 625db07):
//   sl::DLSSOptions (kStructVersion3): BaseStructure (32), mode @32,
//     outputWidth @36, outputHeight @40, sharpness @44, preExposure @48,
//     exposureScale @52, 3 x Boolean @56..58, six DLSSPreset @60..80
//     (dlaa, quality, balanced, performance, ultraPerformance, ultraQuality),
//     useAutoExposure @84 (v2), alphaUpscalingEnabled @85 (v3).
//   sl::DLSSOptimalSettings (kStructVersion1): optimalRenderWidth @32,
//     optimalRenderHeight @36, optimalSharpness @40, renderWidthMin @44,
//     renderHeightMin @48, renderWidthMax @52, renderHeightMax @56.
// Copies are made with the exact size of the version the game passed; a
// version newer than these is passed through untouched.

namespace na::slb::dlss {
namespace {

using SlResult = int32_t;
using PfnOptimal = SlResult(__cdecl*)(const void* options, void* settings);
using PfnSetOptions = SlResult(__cdecl*)(const void* viewport, const void* options);

constexpr uint32_t kFeatureDlss = 0; // sl::kFeatureDLSS
// {6AC826E4-4C61-4101-A92D-638D421057B8} sl::DLSSOptions
constexpr uint8_t kOptionsGuid[16] = {0xe4, 0x26, 0xc8, 0x6a, 0x61, 0x4c, 0x01, 0x41, 0xa9, 0x2d, 0x63, 0x8d, 0x42, 0x10, 0x57, 0xb8};
// {EF1D0957-FD58-4DF7-B504-8B69D8AA6B76} sl::DLSSOptimalSettings
constexpr uint8_t kOptimalGuid[16] = {0x57, 0x09, 0x1d, 0xef, 0x58, 0xfd, 0xf7, 0x4d, 0xb5, 0x04, 0x8b, 0x69, 0xd8, 0xaa, 0x6b, 0x76};
constexpr size_t kOptionsSize[4] = {0, 84, 88, 88}; // by struct version 1..3
constexpr size_t kOptimalSize = 60;                  // version 1
constexpr size_t kPresetFirst = 60, kPresetCount = 6;

PfnOptimal g_orig_optimal = nullptr;
PfnSetOptions g_orig_set = nullptr;
std::atomic<bool> g_optimal_requested{false}, g_set_requested{false}, g_lookup_done{false};
std::atomic<uint32_t> g_optimal_hooked{0}, g_set_hooked{0};
std::atomic<uint64_t> g_optimal_calls{0}, g_set_calls{0};

std::mutex g_m;
ModeRequest g_request;
ModeState g_state; // counters and hook flags are filled in GetModeState

size_t guarded_copy(void* dst, const void* src, size_t n)
{
    __try {
        memcpy(dst, src, n);
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool guarded_write(void* dst, const void* src, size_t n)
{
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Copies the game's DLSSOptions with the exact size of its version. Returns
// the size copied, 0 if unrecognised (wrong type, unknown version, unreadable).
size_t copy_options(const void* options, uint8_t (&out)[96])
{
    if (!options || guarded_copy(out, options, abi::kHeaderSize) != abi::kHeaderSize) return 0;
    if (!abi::HasGuid(out, abi::kHeaderSize, kOptionsGuid)) return 0;
    const uint64_t version = abi::Read<uint64_t>(out, 24);
    if (version < 1 || version > 3) return 0;
    const size_t n = kOptionsSize[version];
    return guarded_copy(out, options, n) == n ? n : 0;
}

void set_reason_locked(const char* text) { strncpy_s(g_state.reason, text, _TRUNCATE); }

SlResult __cdecl DetourGetOptimalSettings(const void* options, void* settings)
{
    g_optimal_calls.fetch_add(1, std::memory_order_relaxed);
    uint8_t copy[96] = {};
    const size_t n = copy_options(options, copy);
    ModeRequest req;
    {
        std::lock_guard<std::mutex> lock(g_m);
        req = g_request;
        const uint64_t now = GetTickCount64();
        if (!g_state.first_optimal_tick) g_state.first_optimal_tick = now;
        g_state.last_optimal_tick = now;
        ++g_state.optimal_calls_this_generation;
    }
    if (!n) {
        std::lock_guard<std::mutex> lock(g_m);
        set_reason_locked("The game's DLSS options were not recognised; passed through unchanged.");
        return g_orig_optimal(options, settings);
    }
    const uint32_t game_mode = abi::Read<uint32_t>(copy, 32);
    const bool rewrite = req.own_mode && game_mode != kSlOff && req.sl_mode != kSlOff;
    if (rewrite) memcpy(copy + 32, &req.sl_mode, 4);
    const SlResult r = rewrite ? g_orig_optimal(copy, settings) : g_orig_optimal(options, settings);
    uint8_t s[kOptimalSize] = {};
    bool have = false;
    if (r == 0 && settings && guarded_copy(s, settings, kOptimalSize) == kOptimalSize && abi::HasGuid(s, kOptimalSize, kOptimalGuid)) {
        have = true;
        if (rewrite && req.pin_bounds) {
            // Dynamic resolution reads these bounds: pin them to the optimal size.
            const uint32_t w = abi::Read<uint32_t>(s, 32), h = abi::Read<uint32_t>(s, 36);
            if (w && h) {
                const uint32_t bounds[4] = {w, h, w, h};
                if (guarded_write(static_cast<uint8_t*>(settings) + 44, bounds, sizeof(bounds))) memcpy(s + 44, bounds, sizeof(bounds));
            }
        }
    }
    std::lock_guard<std::mutex> lock(g_m);
    g_state.game_mode = game_mode;
    g_state.sent_mode = rewrite ? req.sl_mode : game_mode;
    g_state.out_w = abi::Read<uint32_t>(copy, 36);
    g_state.out_h = abi::Read<uint32_t>(copy, 40);
    g_state.last_result = r;
    if (have) {
        g_state.optimal_w = abi::Read<uint32_t>(s, 32);
        g_state.optimal_h = abi::Read<uint32_t>(s, 36);
        g_state.min_w = abi::Read<uint32_t>(s, 44);
        g_state.min_h = abi::Read<uint32_t>(s, 48);
        g_state.max_w = abi::Read<uint32_t>(s, 52);
        g_state.max_h = abi::Read<uint32_t>(s, 56);
    }
    if (rewrite && r == 0) {
        ++g_state.rewritten_this_generation;
        g_state.size_followed = true;
        set_reason_locked("The game asked Streamline for its render size; it received the selected mode's size.");
    } else if (!rewrite) {
        set_reason_locked(game_mode == kSlOff ? "The game has DLSS off in this call; left unchanged." : "The game's own DLSS mode is in use.");
    } else {
        set_reason_locked("Streamline refused the selected mode; the game's answer was not changed.");
    }
    return r;
}

SlResult __cdecl DetourSetOptions(const void* viewport, const void* options)
{
    g_set_calls.fetch_add(1, std::memory_order_relaxed);
    uint8_t copy[96] = {};
    const size_t n = copy_options(options, copy);
    if (!n) return g_orig_set(viewport, options);
    ModeRequest req;
    bool followed;
    {
        std::lock_guard<std::mutex> lock(g_m);
        req = g_request;
        followed = g_state.size_followed;
    }
    const uint32_t game_mode = abi::Read<uint32_t>(copy, 32);
    bool changed = false;
    if (req.own_preset && n >= kPresetFirst + 4 * kPresetCount)
        for (size_t i = 0; i < kPresetCount; ++i) {
            memcpy(copy + kPresetFirst + 4 * i, &req.preset, 4);
            changed = true;
        }
    // The label follows the size: only once the game took our size.
    const bool mode = req.own_mode && followed && game_mode != kSlOff && req.sl_mode != kSlOff && game_mode != req.sl_mode;
    if (mode) {
        memcpy(copy + 32, &req.sl_mode, 4);
        changed = true;
    }
    const SlResult r = changed ? g_orig_set(viewport, copy) : g_orig_set(viewport, options);
    std::lock_guard<std::mutex> lock(g_m);
    g_state.set_game_mode = game_mode;
    g_state.set_sent_mode = mode ? req.sl_mode : game_mode;
    return r;
}

struct Install { void* target; bool optimal; };

DWORD WINAPI install_thread(LPVOID p)
{
    auto* job = static_cast<Install*>(p);
    for (int i = 0; i < 100; ++i) {
        if (na::hooking::BeginInstall()) {
            const bool ok = job->optimal
                ? na::hooking::PrepareHook(job->target, reinterpret_cast<void*>(&DetourGetOptimalSettings), reinterpret_cast<void**>(&g_orig_optimal))
                : na::hooking::PrepareHook(job->target, reinterpret_cast<void*>(&DetourSetOptions), reinterpret_cast<void**>(&g_orig_set));
            if (!ok) na::hooking::AbandonInstall();
            else if (na::hooking::CommitInstall()) (job->optimal ? g_optimal_hooked : g_set_hooked).store(1, std::memory_order_release);
            break;
        }
        Sleep(10); // another module's install transaction is open
    }
    delete job;
    return 0;
}

void request_install(void* target, bool optimal)
{
    if (!target) return;
    std::atomic<bool>& flag = optimal ? g_optimal_requested : g_set_requested;
    if (flag.exchange(true, std::memory_order_acq_rel)) return;
    if (HANDLE t = CreateThread(nullptr, 0, &install_thread, new Install{target, optimal}, 0, nullptr)) CloseHandle(t);
}

} // namespace

uint32_t SlModeForDlssMode(uint32_t dlss_mode)
{
    // settings::DlssMode: DLAA 0, Quality 1, Balanced 2, Performance 3, Ultra Performance 4.
    static const uint32_t kMap[] = {kSlDlaa, kSlMaxQuality, kSlBalanced, kSlMaxPerformance, kSlUltraPerformance};
    return dlss_mode < 5 ? kMap[dlss_mode] : kSlOff;
}

void SetModeRequest(const ModeRequest& request)
{
    std::lock_guard<std::mutex> lock(g_m);
    g_request = request;
    ++g_state.generation;
    g_state.optimal_calls_this_generation = 0;
    g_state.rewritten_this_generation = 0;
    g_state.size_followed = false;
}

ModeState GetModeState()
{
    ModeState out;
    {
        std::lock_guard<std::mutex> lock(g_m);
        out = g_state;
    }
    out.optimal_hooked = g_optimal_hooked.load(std::memory_order_acquire);
    out.set_hooked = g_set_hooked.load(std::memory_order_acquire);
    out.optimal_calls = g_optimal_calls.load(std::memory_order_relaxed);
    out.set_calls = g_set_calls.load(std::memory_order_relaxed);
    return out;
}

void OnFeatureFunction(const char* name, void* function)
{
    if (!name || !function) return;
    if (strcmp(name, "slDLSSGetOptimalSettings") == 0) request_install(function, true);
    else if (strcmp(name, "slDLSSSetOptions") == 0) request_install(function, false);
}

void LookupOnce(LookupFn lookup)
{
    if (!lookup || g_lookup_done.exchange(true, std::memory_order_acq_rel)) return;
    void* fn = nullptr;
    if (!g_optimal_requested.load(std::memory_order_acquire) && lookup(kFeatureDlss, "slDLSSGetOptimalSettings", &fn) == 0 && fn)
        request_install(fn, true);
    fn = nullptr;
    if (!g_set_requested.load(std::memory_order_acquire) && lookup(kFeatureDlss, "slDLSSSetOptions", &fn) == 0 && fn)
        request_install(fn, false);
}

bool WaitInstalledForTests(uint32_t timeout_ms)
{
    const ULONGLONG end = GetTickCount64() + timeout_ms;
    while (GetTickCount64() < end) {
        if (g_optimal_hooked.load() && g_set_hooked.load()) return true;
        Sleep(5);
    }
    return false;
}

void ResetForTests()
{
    std::lock_guard<std::mutex> lock(g_m);
    g_request = ModeRequest{};
    g_state = ModeState{};
}

} // namespace na::slb::dlss
