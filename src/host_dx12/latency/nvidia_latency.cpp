#include "host_dx12/latency/nvidia_latency.hpp"
#include "host_dx12/latency/policy.hpp"
#include "host_dx12/latency/pcl_stats.hpp"
#include "host_dx12/settings.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
#include "host_dx12/adapters/tlou1/timing.hpp"
#include <wrl/client.h>
#include <dxgi.h>
#include <nvapi.h>
#include <mutex>
#include <atomic>
#include <intrin.h>

namespace na::latency {
namespace {
using Query = void* (__cdecl*)(unsigned);
decltype(&NvAPI_D3D_SetSleepMode) set_mode = nullptr;
decltype(&NvAPI_D3D_GetSleepStatus) get_status = nullptr;
decltype(&NvAPI_D3D_GetLatency) get_latency = nullptr;
decltype(&NvAPI_D3D_Sleep) native_sleep = nullptr;
decltype(&NvAPI_D3D_SetLatencyMarker) native_marker = nullptr;
std::atomic<bool> sleep_observed{false}, marker_observed{false};
std::mutex audit_lock;
NativeAudit audit;
std::atomic<uint64_t> audit_dropped{0};
std::atomic<bool> ready{false};
std::mutex control, stats_lock;
Stats stats;
IUnknown* device_identity = nullptr; // identity only; never dereferenced outside Update/caller
NV_SET_SLEEP_MODE_PARAMS game_params{};
bool have_game_params = false;
int applied = -2;
ULONGLONG last_poll = 0;
uint64_t last_native_frame = 0;
std::atomic<uint64_t> game_calls{0};
std::atomic<bool> external_owner{false};
std::mutex engine_mutex;
Microsoft::WRL::ComPtr<ID3D12Device> engine_device;
EngineFrames engine_frames;
uint64_t engine_sleep_count = 0, engine_marker_count = 0, engine_errors = 0;
uint64_t last_sleep_frame = 0;
int32_t engine_result = -1;

NativeCall BeginCall(IUnknown* device, void* caller) {
    LARGE_INTEGER qpc{}; QueryPerformanceCounter(&qpc);
    NativeCall c;
    c.device = reinterpret_cast<uintptr_t>(device);
    c.caller = reinterpret_cast<uintptr_t>(caller);
    c.thread = GetCurrentThreadId(); c.begin_qpc = qpc.QuadPart;
    return c;
}
void EndCall(uint32_t kind, NativeCall c, NvAPI_Status result) {
    LARGE_INTEGER qpc{}; QueryPerformanceCounter(&qpc);
    c.end_qpc = qpc.QuadPart; c.result = result;
    std::unique_lock<std::mutex> lock(audit_lock, std::try_to_lock);
    if (lock.owns_lock()) audit.Record(kind, c);
    else audit_dropped.fetch_add(1, std::memory_order_relaxed);
}
NvAPI_Status __cdecl ObserveSleep(IUnknown* device) {
    external_owner.store(true, std::memory_order_release);
    auto c = BeginCall(device, _ReturnAddress());
    const auto result = native_sleep(device);
    EndCall(NativeAudit::Sleep, c, result);
    return result;
}
NvAPI_Status __cdecl ObserveMarker(IUnknown* device, NV_LATENCY_MARKER_PARAMS* p) {
    external_owner.store(true, std::memory_order_release);
    auto c = BeginCall(device, _ReturnAddress());
    uint32_t kind = NativeAudit::Unknown;
    if (p && p->version == NV_LATENCY_MARKER_PARAMS_VER) {
        c.frame = p->frameID;
        const auto type = static_cast<uint32_t>(p->markerType);
        if (type < NativeAudit::Sleep) kind = type;
    }
    const auto result = native_marker(device, p); // identical arguments, exactly once
    EndCall(kind, c, result);
    return result;
}

NvAPI_Status __cdecl SetMode(IUnknown* dev, NV_SET_SLEEP_MODE_PARAMS* params) {
    if (!params || params->version != NV_SET_SLEEP_MODE_PARAMS_VER) return set_mode(dev, params);
    std::lock_guard<std::mutex> lock(control);
    game_calls.fetch_add(1);
    if (device_identity != dev) { device_identity = dev; applied = -2; last_native_frame = 0; }
    game_params = *params; have_game_params = true;
    const int requested = settings::Get().reflex_mode.load();
    const bool effects_ready=settings::EffectsReady();
    auto selected = effects_ready?ModeRequest(*params, requested):*params;
    const auto result = set_mode(dev, &selected);
    applied = effects_ready && result == NVAPI_OK ? requested : -2;
    { std::lock_guard<std::mutex> out(stats_lock); stats.set_result = result; }
    return result;
}
}
void Initialize() {
    // Never load a game-directory nvapi proxy or alter system-wide profiles.
    HMODULE module = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return;
    auto query = reinterpret_cast<Query>(reinterpret_cast<void*>(GetProcAddress(module, "nvapi_QueryInterface")));
    if (!query) return;
    auto init = reinterpret_cast<decltype(&NvAPI_Initialize)>(query(0x0150e828));
    if (!init || init() != NVAPI_OK) return;
    auto target = query(0xac1ca9e0);
    get_status = reinterpret_cast<decltype(get_status)>(query(0xaef96ca1));
    get_latency = reinterpret_cast<decltype(get_latency)>(query(0x1a587f9c));
    if (!target || !get_status) return;
    if (!hooking::HookFunction(target, reinterpret_cast<void*>(&SetMode), reinterpret_cast<void**>(&set_mode))) return;
    if (auto sleep_target = query(0x852cd1d2))
        sleep_observed.store(hooking::HookFunction(sleep_target, reinterpret_cast<void*>(&ObserveSleep), reinterpret_cast<void**>(&native_sleep)));
    if (auto marker_target = query(0xd9984c05))
        marker_observed.store(hooking::HookFunction(marker_target, reinterpret_cast<void*>(&ObserveMarker), reinterpret_cast<void**>(&native_marker)));
    // The module and hooking session intentionally live until process exit.
    ready.store(true, std::memory_order_release);
}
void Update(ID3D12Device* device, HWND window) {
    if (!settings::EffectsReady() || !device || !ready.load(std::memory_order_acquire)) return;
    std::unique_lock<std::mutex> lock(control, std::try_to_lock);
    if (!lock.owns_lock()) return;
    const auto now = GetTickCount64();
    const int requested = settings::Get().reflex_mode.load();
    if (device_identity != device) {
        device_identity = device; applied = -2; have_game_params = false; last_poll = 0; last_native_frame = 0;
    }
    Stats s;
    { std::lock_guard<std::mutex> read(stats_lock); s = stats; }
    s.available = 1;
    if (requested != applied) {
        if (requested < 0 && !have_game_params) applied = requested;
        else {
            NV_SET_SLEEP_MODE_PARAMS p = have_game_params ? game_params : NV_SET_SLEEP_MODE_PARAMS{};
            p.version = NV_SET_SLEEP_MODE_PARAMS_VER;
            p = ModeRequest(p, requested);
            s.set_result = set_mode(device, &p);
            // Retry a failure at the next settings change/device, not every frame.
            applied = requested;
        }
        last_poll = 0;
    }
    if (!last_poll || now - last_poll >= 1000) {
        last_poll = now;
        NV_GET_SLEEP_STATUS_PARAMS p{}; p.version = NV_GET_SLEEP_STATUS_PARAMS_VER;
        s.query_result = get_status(device, &p); s.status_valid = s.query_result == NVAPI_OK;
        s.foreground = window && GetForegroundWindow() == window;
        s.low_latency = s.status_valid && p.bLowLatencyMode;
        s.game_sleep = s.status_valid && p.bUseGameSleep;
        s.fullscreen_vrr = s.status_valid && s.foreground && p.bFsVrr;
        s.vsync_override = s.status_valid && p.bCplVsyncOn;
        s.independent_flip = s.status_valid && p.bFullscreenIFlip;
        s.native_samples = 0; s.native_render_ms = 0;
        if (get_latency) {
            NV_LATENCY_RESULT_PARAMS result{}; result.version = NV_LATENCY_RESULT_PARAMS_VER;
            if (get_latency(device, &result) == NVAPI_OK) {
                uint64_t newest = last_native_frame;
                for (const auto& r : result.frameReport) {
                    float ms = 0;
                    if (r.frameID > last_native_frame && NativeDuration(r.simStartTime, r.gpuRenderEndTime, ms)) {
                        ++s.native_samples; s.native_render_ms += ms; newest = (std::max)(newest, r.frameID);
                    }
                }
                last_native_frame = newest;
                if (s.native_samples) s.native_render_ms /= static_cast<float>(s.native_samples);
            }
        }
    }
    s.game_mode_calls = game_calls.load();
    if (tlou1::TimingAttached()) {
        std::lock_guard<std::mutex> engine(engine_mutex);
        if (engine_device.Get() != device) { engine_device = device; engine_frames = {}; last_sleep_frame = 0; }
    }
    { std::lock_guard<std::mutex> out(stats_lock); stats = s; }
}
Stats GetStats() {
    Stats s;
    { std::lock_guard<std::mutex> lock(stats_lock); s = stats; }
    { std::lock_guard<std::mutex> lock(audit_lock); s.audit = audit; }
    s.sleep_observer = sleep_observed.load(); s.marker_observer = marker_observed.load();
    s.audit_dropped = audit_dropped.load(std::memory_order_relaxed);
    s.pcl_available = pcl::Available(); s.engine_attached = tlou1::TimingAttached(); s.external_owner = external_owner.load();
    { std::lock_guard<std::mutex> lock(engine_mutex);
      s.engine_available = engine_device && native_sleep && native_marker && !s.external_owner;
      s.engine_sleep = engine_sleep_count; s.engine_markers = engine_marker_count;
      s.engine_complete = engine_frames.complete; s.engine_rejected = engine_frames.rejected;
      s.engine_errors = engine_errors; s.engine_last_result = engine_result; }
    return s;
}
void EngineBegin(uint64_t frame) {
    if (!frame || !ready.load(std::memory_order_acquire) || external_owner.load() || !native_sleep || !native_marker) return;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    { std::lock_guard<std::mutex> lock(engine_mutex);
      if (!engine_device || frame <= last_sleep_frame) return;
      last_sleep_frame = frame; device = engine_device; }
    if (!device) return;
    // The driver may wait here. Never hold a lock required by the presentation
    // worker: it must remain able to finish the preceding frame.
    const auto result = native_sleep(device.Get()); // required even with mode Off
    { std::lock_guard<std::mutex> lock(engine_mutex);
      ++engine_sleep_count; engine_result = result; if (result != NVAPI_OK) ++engine_errors; }
    EngineMark(frame, Phase::Simulation);
}
void EngineMark(uint64_t frame, Phase phase) {
    if (!frame || !ready.load(std::memory_order_acquire) || external_owner.load() || !native_marker) return;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    { std::lock_guard<std::mutex> lock(engine_mutex);
      if (!engine_device || !engine_frames.Accept(frame, phase)) return;
      device = engine_device; }
    constexpr NV_LATENCY_MARKER_TYPE types[] = {SIMULATION_START, INPUT_SAMPLE, SIMULATION_END, RENDERSUBMIT_START, RENDERSUBMIT_END, PRESENT_START, PRESENT_END};
    NV_LATENCY_MARKER_PARAMS p{}; p.version = NV_LATENCY_MARKER_PARAMS_VER;
    p.frameID = frame; p.markerType = types[static_cast<unsigned>(phase)];
    pcl::Mark(static_cast<uint32_t>(p.markerType), frame);
    if (phase == Phase::Input && pcl::Sample(frame)) {
        NV_LATENCY_MARKER_PARAMS flash = p; flash.markerType = TRIGGER_FLASH;
        native_marker(device.Get(), &flash);
    }
    const auto result = native_marker(device.Get(), &p);
    { std::lock_guard<std::mutex> lock(engine_mutex);
      ++engine_marker_count; engine_result = result; if (result != NVAPI_OK) ++engine_errors; }
}
void GeneratedPresent(uint64_t frame, bool end) {
    if (!frame || !ready.load(std::memory_order_acquire) || external_owner.load() || !native_marker) return;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    { std::lock_guard<std::mutex> lock(engine_mutex); device = engine_device; }
    if (!device) return;
    NV_LATENCY_MARKER_PARAMS p{}; p.version = NV_LATENCY_MARKER_PARAMS_VER; p.frameID = frame;
    p.markerType = end ? OUT_OF_BAND_PRESENT_END : OUT_OF_BAND_PRESENT_START;
    pcl::Mark(static_cast<uint32_t>(p.markerType), frame);
    const auto result = native_marker(device.Get(), &p);
    { std::lock_guard<std::mutex> lock(engine_mutex); engine_result = result; if (result != NVAPI_OK) ++engine_errors; }
}
}
