#include "host_dx12/adapters/tlou1/timing.hpp"
#include "host_dx12/adapters/tlou1/apply.hpp"
#include "host_dx12/overlay/overlay.hpp"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <windows.h>
#include <Xinput.h>
#include <wrl/client.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>

#include "host_dx12/capture/frame_capture.hpp"
#include "host_dx12/color/color_space.hpp"
#include "host_dx12/fg/frame_gen.hpp"
#include "host_dx12/fg/preset_profile.hpp"
#include "host_dx12/hooking/variant_registry.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
#include "host_dx12/motion/live_flow.hpp"
#include "host_dx12/ngx_observe/gpu_timing.hpp"
#include "host_dx12/ngx_observe/ngx_observe.hpp"
#include "host_dx12/nr/nr_pass.hpp"
#include "host_dx12/overlay/menu.hpp"
#include "host_dx12/overlay/na_style.hpp"
#include "host_dx12/overlay/presenter.hpp"
#include "host_dx12/overlay/freeze_watchdog.hpp"
#include <ctime>
#include "host_dx12/settings.hpp"
#include "host_dx12/latency/nvidia_latency.hpp"
#include "host_dx12/latency/pcl_stats.hpp"

using Microsoft::WRL::ComPtr;

namespace na::overlay {
namespace {

constexpr uint32_t kSwapReleaseSlot = 2;          // IUnknown::Release on the swap chain (A71: same-window replacement)
constexpr uint32_t kSwapPresentSlot = 8;
constexpr uint32_t kSwapGetBufferSlot = 9;        // IDXGISwapChain::GetBuffer (A59 presenter)
constexpr uint32_t kSwapResizeBuffersSlot = 13;
constexpr uint32_t kSwap1Present1Slot = 22;
constexpr uint32_t kSwap2SetMaxLatencySlot = 31;  // IDXGISwapChain2::SetMaximumFrameLatency (A62)
constexpr uint32_t kSwap2GetMaxLatencySlot = 32;  // IDXGISwapChain2::GetMaximumFrameLatency (A62)
constexpr uint32_t kSwap2GetLatencyObjectSlot = 33;
constexpr uint32_t kSwap3GetIndexSlot = 36;       // IDXGISwapChain3::GetCurrentBackBufferIndex (A59)
constexpr uint32_t kSwap3SetColorSpace1Slot = 38; // IDXGISwapChain3::SetColorSpace1
constexpr uint32_t kSwap3ResizeBuffers1Slot = 39; // IDXGISwapChain3::ResizeBuffers1 (A59)
constexpr uint32_t kQueueExecuteSlot = 10;
constexpr uint32_t kSrvHeapSize = 64;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT,
                                               const DXGI_PRESENT_PARAMETERS*);
using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT,
                                             DXGI_FORMAT, UINT);
using Resize1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT,
                                              const UINT*, IUnknown* const*);
using GetBufferFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, REFIID, void**);
using GetIndexFn = UINT(STDMETHODCALLTYPE*)(IDXGISwapChain3*);
using SetMaxLatencyFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain2*, UINT);
using GetMaxLatencyFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain2*, UINT*);
using GetLatencyObjectFn = HANDLE(STDMETHODCALLTYPE*)(IDXGISwapChain2*);
using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT,
                                           ID3D12CommandList* const*);
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using SetColorSpaceFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE);
using ReleaseFn = ULONG(STDMETHODCALLTYPE*)(IUnknown*);
ReleaseFn g_orig_release = nullptr;          // A71
thread_local bool t_in_our_release = false;  // A71: our own Release calls inside the detour pass straight through
std::atomic<uint32_t> g_chain_gone{0};       // A71: chains observed destroyed (final Release)
PresentFn g_orig_present = nullptr;
Present1Fn g_orig_present1 = nullptr;
ResizeFn g_orig_resize = nullptr;
Resize1Fn g_orig_resize1 = nullptr;
GetBufferFn g_orig_get_buffer = nullptr;
GetIndexFn g_orig_get_index = nullptr;
SetMaxLatencyFn g_orig_set_max_latency = nullptr;
GetMaxLatencyFn g_orig_get_max_latency = nullptr;
GetLatencyObjectFn g_orig_get_latency_object = nullptr;
std::atomic<uintptr_t> g_game_latency_handle{0}, g_present_queue_identity{0};
std::atomic<uint64_t> g_present_returns{0};
std::atomic<DWORD> g_present_caller{0};
std::atomic<HRESULT> g_present_result{S_OK};
std::atomic<UINT> g_game_max_latency{0}; // what the game asked for (A62), reported back to it unchanged
ExecuteFn g_orig_execute = nullptr;
SetColorSpaceFn g_orig_set_color_space = nullptr;

// Our own reads of the game's chain must bypass the presenter's proxies.
HRESULT real_get_buffer(IDXGISwapChain* sc, UINT i, REFIID riid, void** out)
{
    return g_orig_get_buffer ? g_orig_get_buffer(sc, i, riid, out) : sc->GetBuffer(i, riid, out);
}
UINT real_index(IDXGISwapChain* sc)
{
    ComPtr<IDXGISwapChain3> sc3;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3)))) return 0xFFFFFFFFu;
    return g_orig_get_index ? g_orig_get_index(sc3.Get()) : sc3->GetCurrentBackBufferIndex();
}
std::atomic<int> g_color_space{DXGI_COLOR_SPACE_CUSTOM}; // last SetColorSpace1 on the game's chain
XInputGetStateFn g_orig_xinput[3] = {};

std::atomic<ID3D12CommandQueue*> g_direct_queue{nullptr};
// ImGui synchronously waits for font/texture uploads during RenderDrawData.
// They must not sit behind game work that needs this Present to return.
// Published separately so the upload cannot become the observed game queue.
std::atomic<ID3D12CommandQueue*> g_texture_queue{nullptr};
std::atomic<bool> g_open{false};
std::atomic<uint64_t> g_frames{0}, g_presents{0};
std::atomic<uint64_t> g_menu_draw_marker{0};
void write_menu_progress(HANDLE log);
std::atomic<uint32_t> g_retargets{0}; // swap chain re-targets (A31)
std::atomic<uint32_t> g_initialized{0};
std::atomic<uint32_t> g_gamepad_hooked{0};
std::atomic<bool> g_stopping{false};
std::atomic<float> g_fps{0.0f}, g_frame_ms{0.0f}, g_mouse_x{0.0f}, g_mouse_y{0.0f};
const char* g_route_tag = "";
PanelFn g_controls = nullptr;
char g_game_name[96] = {};
// Evidence freshness for the menu: presents since the evaluate counter moved.
uint64_t g_last_evaluates = 0;
std::atomic<uint64_t> g_presents_since_eval{0};
// A44 window placement bookkeeping (render thread only).
bool g_window_reset = false;
bool g_window_dirty = false;

// ---- A44 input lock: while the menu is open the GAME must not see the
// mouse or keyboard, whichever way it reads them. The subclassed WndProc
// already swallows messages on the game's main window, but games commonly
// read raw input on a hidden window or through GetRawInputBuffer, poll
// GetAsyncKeyState/GetKeyboardState, or track GetCursorPos deltas and
// re-centre with SetCursorPos. These user32 entry points are hooked once at
// Start; calls from OUR render thread pass through (the menu needs the real
// cursor), everything else sees a frozen cursor, no buttons and no keys.
using PfnGetRawInputData = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using PfnGetRawInputBuffer = UINT(WINAPI*)(PRAWINPUT, PUINT, UINT);
using PfnGetCursorPos = BOOL(WINAPI*)(LPPOINT);
using PfnSetCursorPos = BOOL(WINAPI*)(int, int);
using PfnGetAsyncKeyState = SHORT(WINAPI*)(int);
using PfnGetKeyboardState = BOOL(WINAPI*)(PBYTE);
PfnGetRawInputData g_orig_get_raw_input_data = nullptr;
PfnGetRawInputBuffer g_orig_get_raw_input_buffer = nullptr;
PfnGetCursorPos g_orig_get_cursor_pos = nullptr;
PfnSetCursorPos g_orig_set_cursor_pos = nullptr;
PfnGetAsyncKeyState g_orig_get_async_key_state = nullptr;
PfnGetKeyboardState g_orig_get_keyboard_state = nullptr;
void* g_input_hook_targets[6] = {};
std::atomic<uint32_t> g_input_hooks{0};
std::atomic<DWORD> g_render_thread{0};
std::atomic<LONG> g_frozen_x{0}, g_frozen_y{0}; // what the game sees as the cursor while open

inline bool lock_input_for_this_thread()
{
    return g_open.load(std::memory_order_acquire) && g_initialized.load(std::memory_order_acquire) &&
           GetCurrentThreadId() != g_render_thread.load(std::memory_order_relaxed);
}
void neutralise_raw(RAWINPUT* ri)
{
    if (!ri) return;
    if (ri->header.dwType == RIM_TYPEMOUSE) {
        ri->data.mouse.lLastX = 0;
        ri->data.mouse.lLastY = 0;
        ri->data.mouse.usButtonFlags = 0;
        ri->data.mouse.usButtonData = 0;
        ri->data.mouse.ulRawButtons = 0;
    } else if (ri->header.dwType == RIM_TYPEKEYBOARD) {
        ri->data.keyboard.Flags |= RI_KEY_BREAK; // every key reads as released
    }
}
UINT WINAPI DetourGetRawInputData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT header)
{
    UINT r = g_orig_get_raw_input_data(h, cmd, data, size, header);
    if (r != static_cast<UINT>(-1) && data && cmd == RID_INPUT && r >= sizeof(RAWINPUTHEADER) && lock_input_for_this_thread())
        neutralise_raw(static_cast<RAWINPUT*>(data));
    return r;
}
UINT WINAPI DetourGetRawInputBuffer(PRAWINPUT data, PUINT size, UINT header)
{
    UINT r = g_orig_get_raw_input_buffer(data, size, header);
    if (r != static_cast<UINT>(-1) && r != 0 && data && lock_input_for_this_thread()) {
        RAWINPUT* p = data;
        for (UINT i = 0; i < r; ++i) {
            neutralise_raw(p);
            // NEXTRAWINPUTBLOCK without the QWORD type the lean headers omit: 8-byte aligned on x64.
            uintptr_t next = reinterpret_cast<uintptr_t>(reinterpret_cast<BYTE*>(p) + p->header.dwSize);
            p = reinterpret_cast<RAWINPUT*>((next + sizeof(uint64_t) - 1) & ~(sizeof(uint64_t) - 1));
        }
    }
    return r;
}
BOOL WINAPI DetourGetCursorPos(LPPOINT p)
{
    if (p && lock_input_for_this_thread()) {
        p->x = g_frozen_x.load(std::memory_order_relaxed);
        p->y = g_frozen_y.load(std::memory_order_relaxed);
        return TRUE;
    }
    return g_orig_get_cursor_pos(p);
}
BOOL WINAPI DetourSetCursorPos(int x, int y)
{
    if (lock_input_for_this_thread()) return TRUE; // the game may not re-centre the menu cursor
    return g_orig_set_cursor_pos(x, y);
}
SHORT WINAPI DetourGetAsyncKeyState(int vk)
{
    if (lock_input_for_this_thread()) return 0;
    return g_orig_get_async_key_state(vk);
}
BOOL WINAPI DetourGetKeyboardState(PBYTE state)
{
    BOOL r = g_orig_get_keyboard_state(state);
    if (r && state && lock_input_for_this_thread()) memset(state, 0, 256);
    return r;
}
void install_input_hooks()
{
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (!u) return;
    struct { const char* name; void* detour; void** original; } specs[6] = {
        {"GetRawInputData", reinterpret_cast<void*>(&DetourGetRawInputData), reinterpret_cast<void**>(&g_orig_get_raw_input_data)},
        {"GetRawInputBuffer", reinterpret_cast<void*>(&DetourGetRawInputBuffer), reinterpret_cast<void**>(&g_orig_get_raw_input_buffer)},
        {"GetCursorPos", reinterpret_cast<void*>(&DetourGetCursorPos), reinterpret_cast<void**>(&g_orig_get_cursor_pos)},
        {"SetCursorPos", reinterpret_cast<void*>(&DetourSetCursorPos), reinterpret_cast<void**>(&g_orig_set_cursor_pos)},
        {"GetAsyncKeyState", reinterpret_cast<void*>(&DetourGetAsyncKeyState), reinterpret_cast<void**>(&g_orig_get_async_key_state)},
        {"GetKeyboardState", reinterpret_cast<void*>(&DetourGetKeyboardState), reinterpret_cast<void**>(&g_orig_get_keyboard_state)},
    };
    uint32_t n = 0;
    for (int i = 0; i < 6; ++i) {
        void* target = reinterpret_cast<void*>(GetProcAddress(u, specs[i].name));
        if (target && na::hooking::HookFunction(target, specs[i].detour, specs[i].original)) {
            g_input_hook_targets[i] = target;
            ++n;
        } else {
            // Fall back to the plain export so our own calls still work.
            *specs[i].original = target;
        }
    }
    g_input_hooks.store(n, std::memory_order_release);
}
void remove_input_hooks()
{
    for (void*& t : g_input_hook_targets) {
        if (t) na::hooking::Unhook(t);
        t = nullptr;
    }
    g_input_hooks.store(0, std::memory_order_release);
}
// Originals for our own use (valid after install_input_hooks; plain exports otherwise).
inline BOOL real_get_cursor_pos(LPPOINT p) { return g_orig_get_cursor_pos ? g_orig_get_cursor_pos(p) : GetCursorPos(p); }
inline SHORT real_get_async_key_state(int vk) { return g_orig_get_async_key_state ? g_orig_get_async_key_state(vk) : GetAsyncKeyState(vk); }
inline UINT real_get_raw_input_data(HRAWINPUT h, UINT cmd, LPVOID d, PUINT s, UINT hs)
{
    return g_orig_get_raw_input_data ? g_orig_get_raw_input_data(h, cmd, d, s, hs) : GetRawInputData(h, cmd, d, s, hs);
}

// Queue-type cache so ExecuteCommandLists never pays GetDesc per submit.
ID3D12CommandQueue* g_known_direct[8] = {};
ID3D12CommandQueue* g_known_other[8] = {};
std::atomic<uint32_t> g_known_direct_n{0}, g_known_other_n{0};

// ---- A48 host seam: submission scope + retained discovery queue ----
struct ExecuteSubscriber { ExecuteBeginFn begin; ExecuteEndFn end; void* user; };
std::atomic<ExecuteSubscriber*> g_exec_sub{nullptr};
ExecuteSubscriber g_exec_sub_storage{};
std::atomic<uint64_t> g_exec_ticket{0}, g_exec_scoped{0};
struct DiscoverySubscriber { DeviceSeenFn device; QueueSeenFn queue; ListSeenFn list; void* user; };
std::atomic<DiscoverySubscriber*> g_disc_sub{nullptr};
DiscoverySubscriber g_disc_sub_storage{};
enum class SeenKind : uint32_t { Device = 1, Queue = 2, List = 3 };
struct SeenEntry { std::atomic<IUnknown*> object{nullptr}; std::atomic<uint32_t> kind{0}; };
constexpr uint32_t kSeenCapacity = 16;
SeenEntry g_seen[kSeenCapacity];
std::atomic<uint32_t> g_seen_write{0}, g_seen_read{0};
std::atomic<uint32_t> g_seen_queued{0}, g_seen_delivered{0}, g_seen_overflow{0};
// A49: one-shot announcements are latched only when the push SUCCEEDED; a
// device seen before any subscriber (or lost to overflow) is retried on the
// next opportunity (SubscribeDiscovery, every Execute) until delivered.
std::atomic<bool> g_device_announced{false};
std::atomic<uint32_t> g_queue_announced_mask{0}; // bit i = g_known_direct[i] delivered
std::atomic<ID3D12Device*> g_seen_device{nullptr};  // the game's device once the overlay initialised (held by g_state)
std::atomic<uint32_t> g_exec_active{0};          // Execute callbacks in flight (drained before Unsubscribe returns true)
std::atomic<uint32_t> g_unsubscribe_timeouts{0}, g_stop_incomplete{0}; // A50 seam stats

// Producer (any thread, lock-free, bounded): AddRef'd retention until delivery.
bool seen_push(SeenKind kind, IUnknown* object)
{
    if (!object || !g_disc_sub.load(std::memory_order_acquire)) return false;
    for (;;) {
        uint32_t w = g_seen_write.load(std::memory_order_acquire), r = g_seen_read.load(std::memory_order_acquire);
        if (w - r >= kSeenCapacity) { g_seen_overflow.fetch_add(1, std::memory_order_relaxed); return false; }
        if (g_seen_write.compare_exchange_weak(w, w + 1, std::memory_order_acq_rel)) {
            object->AddRef();
            SeenEntry& e = g_seen[w % kSeenCapacity];
            e.kind.store(static_cast<uint32_t>(kind), std::memory_order_relaxed);
            e.object.store(object, std::memory_order_release);
            g_seen_queued.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
}
// Retry pending one-shot announcements (device, known direct queues).
void announce_pending()
{
    if (!g_disc_sub.load(std::memory_order_acquire)) return;
    if (!g_device_announced.load(std::memory_order_acquire)) {
        ID3D12Device* dev = g_seen_device.load(std::memory_order_acquire); // published by ensure_init_locked
        if (dev && seen_push(SeenKind::Device, dev)) g_device_announced.store(true, std::memory_order_release);
    }
    uint32_t nd = g_known_direct_n.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < nd; ++i) {
        uint32_t bit = 1u << i;
        if (g_queue_announced_mask.load(std::memory_order_acquire) & bit) continue;
        if (seen_push(SeenKind::Queue, g_known_direct[i])) g_queue_announced_mask.fetch_or(bit, std::memory_order_acq_rel);
    }
}
// A55 status log (discovery thread only).
wchar_t g_status_log[MAX_PATH] = {};
ULONGLONG g_status_last_tick = 0;
void write_status_line()
{
    if (!g_status_log[0]) return;
    ULONGLONG now = GetTickCount64();
    if (now - g_status_last_tick < 5000) return;
    g_status_last_tick = now;
    na::ngx::Stats ngx = na::ngx::GetStats();
    na::nr::Stats nr = na::nr::GetStats();
    na::motion::Stats mo = na::motion::GetStats();
    na::gputime::Stats gt = na::gputime::GetStats();
    na::fg::Stats fg = na::fg::GetStats();
    na::present::Stats pr = na::present::GetStats();
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[960];
    int n = snprintf(line, sizeof(line),
                     "%02u:%02u:%02u fps=%.1f frame_ms=%.2f dlss_gpu_ms=%.2f evaluates=%llu creates=%llu menu=%u | nr on=%u active=%u ms=%.2f evals=%llu skipped=%llu \"%s\" | motion on=%u active=%u frames=%llu skipped=%llu engine_ms=%.2f avg=%.2f vs_game_mv: err_px=%.3f within_1px=%.1f%% motion_px=%.2f cmp=%llu \"%s\" | fg on=%u active=%u mode=%s max=%ux count=%u supported=%u generated=%llu real=%llu skipped=%llu target=%.0f \"%s\" | presenter proxied=%u thread=%u base_fps=%.1f shown_fps=%.1f display_hz=%.0f fg_gpu_ms=%.2f avg=%.2f real_presents=%llu game_presents=%llu late=%llu dropped=%llu vetoed=%llu gen_dropped=%llu chain_releases=%u chain_flags=0x%X orphaned=%u\r\n",
                     t.wHour, t.wMinute, t.wSecond, g_fps.load(), g_frame_ms.load(), gt.avg_ms,
                     static_cast<unsigned long long>(ngx.evaluates), static_cast<unsigned long long>(ngx.creates), g_open.load() ? 1u : 0u,
                     na::settings::Get().nr_enabled.load(), nr.active, nr.avg_ms, static_cast<unsigned long long>(nr.evaluates),
                     static_cast<unsigned long long>(nr.skipped), nr.reason,
                     mo.enabled, mo.active, static_cast<unsigned long long>(mo.frames), static_cast<unsigned long long>(mo.skipped), mo.flow_ms, mo.avg_ms,
                     mo.ref_mean_err_px, mo.ref_within_1px_pct, mo.ref_mean_mag_px, static_cast<unsigned long long>(mo.ref_frames), mo.reason,
                     fg.enabled, fg.active, fg.mode ? "dynamic" : "fixed", fg.multiplier, fg.last_count, fg.max_supported,
                     static_cast<unsigned long long>(fg.generated), static_cast<unsigned long long>(fg.real_frames),
                     static_cast<unsigned long long>(fg.skipped), fg.target_fps, fg.reason,
                     pr.proxied, pr.thread_mode, pr.base_fps, pr.shown_fps, pr.display_hz, pr.fg_ms, pr.fg_avg_ms,
                     static_cast<unsigned long long>(pr.presents_real), static_cast<unsigned long long>(pr.presents_game),
                     static_cast<unsigned long long>(pr.late), static_cast<unsigned long long>(pr.dropped),
                     static_cast<unsigned long long>(pr.vetoed), static_cast<unsigned long long>(pr.generated_dropped), pr.chain_releases, pr.chain_flags, pr.orphaned);
    if (n <= 0) return;
    HANDLE f = CreateFileW(g_status_log, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, line, static_cast<DWORD>(n), &written, nullptr);
    char pacing[256];
    int pn = snprintf(pacing, sizeof(pacing), "present_boundary returned=%llu thread=%lu hr=0x%08lX game_latency_handle=0x%llX queue=0x%llX\r\n",
                      static_cast<unsigned long long>(g_present_returns.load()), g_present_caller.load(),
                      static_cast<unsigned long>(g_present_result.load()),
                      static_cast<unsigned long long>(g_game_latency_handle.load()),
                      static_cast<unsigned long long>(g_present_queue_identity.load()));
    if (pn > 0) WriteFile(f, pacing, static_cast<DWORD>(pn), &written, nullptr);
    const auto latency = na::latency::GetStats();
    char lm[900];
    int ln = snprintf(lm, sizeof(lm), "latency request=%d available=%u confirmed=%u game_sleep=%u native_n=%llu native_ms=%.2f set=%d query=%d foreground=%u vrr=%u cpl_vsync=%u iflip=%u sync=%u flags=0x%X ready_ms=%.2f ready_p95=%.2f real_age_ms=%.2f real_age_p95=%.2f interval_ms=%.2f interval_p95=%.2f present_ms=%.2f backlog=%u\r\n", na::settings::Get().reflex_mode.load(), latency.available, latency.low_latency, latency.game_sleep, static_cast<unsigned long long>(latency.native_samples), latency.native_render_ms, latency.set_result, latency.query_result, latency.foreground, latency.fullscreen_vrr, latency.vsync_override, latency.independent_flip, pr.requested_sync, pr.requested_flags, pr.ready_age.mean, pr.ready_age.p95, pr.real_age.mean, pr.real_age.p95, pr.submit_interval.mean, pr.submit_interval.p95, pr.present_call.mean, pr.backlog);
    if (ln > 0) WriteFile(f, lm, static_cast<DWORD>(ln), &written, nullptr);
    ln = snprintf(lm, sizeof(lm), "reflex_observers sleep=%u marker=%u dropped=%llu (incoming calls, not engine-placement qualification)\r\n", latency.sleep_observer, latency.marker_observer, static_cast<unsigned long long>(latency.audit_dropped));
    if (ln > 0) WriteFile(f, lm, static_cast<DWORD>(ln), &written, nullptr);
    for (uint32_t kind = 0; kind < na::latency::NativeAudit::Slots; ++kind) {
        const auto& c = latency.audit.calls[kind];
        if (!c.count) continue;
        ln = snprintf(lm, sizeof(lm), "reflex_incoming kind=%u count=%llu ok=%llu device=0x%llX frame=%llu thread=%u caller=0x%llX begin_qpc=%llu end_qpc=%llu result=%d\r\n", kind, static_cast<unsigned long long>(c.count), static_cast<unsigned long long>(c.successes), static_cast<unsigned long long>(c.device), static_cast<unsigned long long>(c.frame), c.thread, static_cast<unsigned long long>(c.caller), static_cast<unsigned long long>(c.begin_qpc), static_cast<unsigned long long>(c.end_qpc), c.result);
        if (ln > 0) WriteFile(f, lm, static_cast<DWORD>(ln), &written, nullptr);
    }
    ln = snprintf(lm, sizeof(lm), "reflex_engine attached=%u available=%u external=%u pcl=%u sleeps=%llu markers=%llu complete=%llu rejected=%llu errors=%llu result=%d\r\n", latency.engine_attached, latency.engine_available, latency.external_owner, latency.pcl_available, static_cast<unsigned long long>(latency.engine_sleep), static_cast<unsigned long long>(latency.engine_markers), static_cast<unsigned long long>(latency.engine_complete), static_cast<unsigned long long>(latency.engine_rejected), static_cast<unsigned long long>(latency.engine_errors), latency.engine_last_result);
    if (ln > 0) WriteFile(f, lm, static_cast<DWORD>(ln), &written, nullptr);
    const auto timing_trace = na::tlou1::GetTimingTrace();
    ln=snprintf(lm,sizeof(lm),"gpu_present_copy first_ms=%.3f first_p95=%.3f interval_ms=%.3f interval_p95=%.3f samples=%u independent_queue=%u (GPU completion, not scanout)\r\n",pr.gpu_first_copy.mean,pr.gpu_first_copy.p95,pr.gpu_copy_interval.mean,pr.gpu_copy_interval.p95,pr.gpu_first_copy.count,pr.independent_queue);
    if(ln>0) WriteFile(f,lm,static_cast<DWORD>(ln),&written,nullptr);
    ln = snprintf(lm,sizeof(lm),"engine_present seen=%llu with_frame=%llu game_callers=",timing_trace.seen,timing_trace.with_frame);
    for(unsigned i=0;i<timing_trace.count && ln<700;++i) ln+=snprintf(lm+ln,sizeof(lm)-ln,"%llX,",static_cast<unsigned long long>(timing_trace.callers[i]));
    ln+=snprintf(lm+ln,sizeof(lm)-ln,"\r\n");
    if(ln>0) WriteFile(f,lm,static_cast<DWORD>(ln),&written,nullptr);
    const auto camera = na::fg::GetCameraStats();
    ln = snprintf(lm, sizeof(lm), "fg_camera recorded=%llu rejected=%llu matched=%llu missed=%llu\r\n", camera.recorded,camera.rejected,camera.matched,camera.missed);
    if (ln > 0) WriteFile(f,lm,static_cast<DWORD>(ln),&written,nullptr);
    const auto fpreset = na::fg::preset::GetStatus();
    ln = snprintf(lm, sizeof(lm), "fg_driver_preset requested=%u stored=%u result=%d pending=%u restart=%u (profile request; active model unverified)\r\n", fpreset.requested, fpreset.stored, fpreset.result, fpreset.pending ? 1u : 0u, fpreset.restart ? 1u : 0u);
    if (ln > 0) WriteFile(f, lm, static_cast<DWORD>(ln), &written, nullptr);
    write_menu_progress(f);
    char nr_line[256];
    int nn = snprintf(nr_line, sizeof(nr_line), "nr_inputs seen=%llu overwritten=%llu missing=%llu allocator_busy=%llu\r\n",
                      static_cast<unsigned long long>(nr.inputs_seen), static_cast<unsigned long long>(nr.input_overwrites),
                      static_cast<unsigned long long>(nr.missing_inputs), static_cast<unsigned long long>(nr.busy_slots));
    if (nn > 0) WriteFile(f, nr_line, static_cast<DWORD>(nn), &written, nullptr);
    char fg_line[256];
    int fn = snprintf(fg_line, sizeof(fg_line), "fg_inputs recorded=%llu submitted=%llu overwritten=%llu missing=%llu\r\n",
                      static_cast<unsigned long long>(fg.inputs_recorded), static_cast<unsigned long long>(fg.inputs_submitted),
                      static_cast<unsigned long long>(fg.inputs_overwritten), static_cast<unsigned long long>(fg.missing_inputs));
    if (fn > 0) WriteFile(f, fg_line, static_cast<DWORD>(fn), &written, nullptr);
    // A66 (R3): one line per game-driven DLSS creation since the last write:
    // sequence, thread, result, sizes, the Apply request it happened under
    // (ctx=apply#N, or ctx=game for a baseline / game-menu creation), and the
    // caller chain with each frame labelled game (the executable) or module.
    static uint64_t last_create_seq = 0;
    na::ngx::CreateRecord recs[16];
    uint32_t count = na::ngx::GetCreateRecords(last_create_seq, recs, 16);
    for (uint32_t i = 0; i < count; ++i) {
        const na::ngx::CreateRecord& r = recs[i];
        last_create_seq = r.seq;
        char cl[2400];
        int m = snprintf(cl, sizeof(cl), "%02u:%02u:%02u dlss_create seq=%llu thread=%u feature=%u result=%d in=%ux%u out=%ux%u ctx=%s%llu callers=",
                         t.wHour, t.wMinute, t.wSecond, static_cast<unsigned long long>(r.seq), r.thread_id, r.feature_id, r.result,
                         r.in_w, r.in_h, r.out_w, r.out_h, r.confirm_id ? "apply#" : "game", static_cast<unsigned long long>(r.confirm_id));
        if (m <= 0) continue;
        for (uint32_t k = 0; k < r.frame_count; ++k) {
            const na::ngx::CreateFrame& cf = r.frames[k];
            char fr[96];
            if (cf.game == 2) snprintf(fr, sizeof(fr), "%s?0x%llX", k ? " > " : "", static_cast<unsigned long long>(cf.address));
            else snprintf(fr, sizeof(fr), "%s%s%s+0x%llX", k ? " > " : "", cf.game ? "[game]" : "", cf.module, static_cast<unsigned long long>(cf.rva));
            strncat_s(cl, fr, _TRUNCATE);
        }
        // ctx=game0 reads oddly; drop the trailing 0 for baseline creations.
        char* ctx0 = strstr(cl, "ctx=game0");
        if (ctx0) memmove(ctx0 + 8, ctx0 + 9, strlen(ctx0 + 9) + 1);
        strncat_s(cl, "\r\n", _TRUNCATE);
        WriteFile(f, cl, static_cast<DWORD>(strlen(cl)), &written, nullptr);
    }
    // A69: freeze watchdog. The game's presents stopped for good in session
    // 12 while this thread kept writing; make the next such freeze describe
    // itself: every thread's stack, a minidump, and Frame Generation off for
    // the next launch.
    static na::freeze::Watchdog watchdog(10000);
    if (watchdog.Check(pr.presents_game, now)) {
        char hl[400];
        int m = snprintf(hl, sizeof(hl), "%02u:%02u:%02u FREEZE detected: the game's presents have not advanced for %llu ms (game_presents=%llu real_presents=%llu fg_active=%u count=%u backlog=%u). Thread stacks follow (innermost first).\r\n",
                         t.wHour, t.wMinute, t.wSecond, static_cast<unsigned long long>(watchdog.StalledMs(now)),
                         static_cast<unsigned long long>(pr.presents_game), static_cast<unsigned long long>(pr.presents_real), fg.active, fg.last_count, pr.backlog);
        if (m > 0) WriteFile(f, hl, static_cast<DWORD>(m), &written, nullptr);
        static na::freeze::Capture cap; // static: not on this thread's stack
        na::freeze::CaptureAllThreads(&cap);
        m = snprintf(hl, sizeof(hl), "FREEZE thread coverage: captured=%u found=%u capacity=%u\r\n",
                     cap.thread_count, cap.threads_total, na::freeze::kMaxThreads);
        if (m > 0) WriteFile(f, hl, static_cast<DWORD>(m), &written, nullptr);
        for (uint32_t i = 0; i < cap.thread_count; ++i) {
            char sl[2400];
            uint32_t len = na::freeze::FormatStack(cap.threads[i], sl, sizeof(sl) - 4);
            if (!len) continue;
            memcpy(sl + len, "\r\n", 3);
            WriteFile(f, sl, static_cast<DWORD>(len + 2), &written, nullptr);
        }
        // Minidump beside the user settings (%LOCALAPPDATA%\NeuralAlias).
        wchar_t dump[MAX_PATH] = {};
        const wchar_t* ini = na::settings::UserIniPath();
        if (ini && *ini) {
            wcsncpy_s(dump, ini, _TRUNCATE);
            wchar_t* slash = wcsrchr(dump, L'\\');
            if (slash) {
                swprintf_s(slash + 1, MAX_PATH - (slash + 1 - dump), L"freeze-%04u%02u%02u-%02u%02u%02u.dmp", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
                bool ok = na::freeze::WriteMiniDump(dump);
                char dl[MAX_PATH + 64];
                char dump_a[MAX_PATH];
                WideCharToMultiByte(CP_ACP, 0, dump, -1, dump_a, sizeof(dump_a), nullptr, nullptr);
                m = snprintf(dl, sizeof(dl), "%02u:%02u:%02u FREEZE minidump %s: %s\r\n", t.wHour, t.wMinute, t.wSecond, ok ? "written" : "FAILED", dump_a);
                if (m > 0) WriteFile(f, dl, static_cast<DWORD>(m), &written, nullptr);
            }
        }
        // Safe mode for the next launch: Frame Generation off, with the time recorded.
        if (na::settings::Get().fg_enabled.load(std::memory_order_relaxed)) {
            na::settings::Get().fg_enabled.store(0, std::memory_order_release);
            na::settings::Get().fg_frozen_at.store(static_cast<uint32_t>(time(nullptr)), std::memory_order_release);
            bool saved = na::settings::Save();
            m = snprintf(hl, sizeof(hl), "%02u:%02u:%02u FREEZE safe mode: Frame Generation switched off for the next launch (%s).\r\n", t.wHour, t.wMinute, t.wSecond, saved ? "saved" : "save FAILED");
            if (m > 0) WriteFile(f, hl, static_cast<DWORD>(m), &written, nullptr);
        }
    }
    // A71: the game's Streamline settings calls (slSetFeatureConstants = set_constants,
    // slGetFeatureSettings = get_settings), one line each: the thread and the
    // caller chain of the game's own settings-apply path. `raw` is the first
    // three dwords of the game's constants block, uninterpreted.
    static uint64_t last_sl_seq = 0;
    na::ngx::SlCallRecord sl[32];
    uint32_t sl_count = na::ngx::GetSlCallRecords(last_sl_seq, sl, 32);
    for (uint32_t i = 0; i < sl_count; ++i) {
        const na::ngx::SlCallRecord& r = sl[i];
        last_sl_seq = r.seq;
        char cl[2400];
        int m = snprintf(cl, sizeof(cl), "%02u:%02u:%02u sl_call seq=%llu kind=%s feature=%u thread=%u raw=[0x%X,0x%X,0x%X] callers=",
                         t.wHour, t.wMinute, t.wSecond, static_cast<unsigned long long>(r.seq), r.kind == 1 ? "set_constants" : (r.kind == 2 ? "get_settings" : "?"),
                         r.feature, r.thread_id, r.raw[0], r.raw[1], r.raw[2]);
        if (m <= 0) continue;
        for (uint32_t k = 0; k < r.frame_count; ++k) {
            const na::ngx::CreateFrame& cf = r.frames[k];
            char fr[96];
            if (cf.game == 2) snprintf(fr, sizeof(fr), "%s?0x%llX", k ? " > " : "", static_cast<unsigned long long>(cf.address));
            else snprintf(fr, sizeof(fr), "%s%s%s+0x%llX", k ? " > " : "", cf.game ? "[game]" : "", cf.module, static_cast<unsigned long long>(cf.rva));
            strncat_s(cl, fr, _TRUNCATE);
        }
        strncat_s(cl, "\r\n", _TRUNCATE);
        WriteFile(f, cl, static_cast<DWORD>(strlen(cl)), &written, nullptr);
    }
    {
        auto a = na::tlou1::GetApplyStatus();
        char row[512];
        int len = snprintf(row, sizeof(row), "%02u:%02u:%02u tlou_apply available=%u request=%llu state=%u mode=%u preset=%d size=%ux%u frames=%llu predicates=%llu triggers=%llu releases=%llu release_failures=%llu thread=%u reason=\"%s\"\r\n",
                           t.wHour, t.wMinute, t.wSecond, a.available ? 1u : 0u,
                           static_cast<unsigned long long>(a.request_id), static_cast<unsigned>(a.stage),
                           a.choice.mode, a.choice.preset, a.choice.width, a.choice.height,
                           static_cast<unsigned long long>(a.frame_calls), static_cast<unsigned long long>(a.predicate_calls),
                           static_cast<unsigned long long>(a.triggers), static_cast<unsigned long long>(a.resource_releases),
                           static_cast<unsigned long long>(a.resource_release_failures), a.trigger_thread, a.reason);
        if (len > 0 && len < static_cast<int>(sizeof(row))) WriteFile(f, row, static_cast<DWORD>(len), &written, nullptr);
    }
    // A66 (R1): the size-confirmation monitor, one line whenever its state changes.
    static uint64_t last_confirm_id = 0;
    static uint32_t last_confirm_state = 0;
    na::ngx::SizeConfirmation sc = na::ngx::GetSizeConfirmation();
    if (sc.id && (sc.id != last_confirm_id || sc.state != last_confirm_state)) {
        last_confirm_id = sc.id;
        last_confirm_state = sc.state;
        char cl[400];
        int m = snprintf(cl, sizeof(cl), "%02u:%02u:%02u size_confirm apply#%llu state=%u reason=%u requested=%ux%u created=%ux%u last=%ux%u streak=%u/%u evals=%llu matching=%llu mismatched=%llu incoherent=%llu failed=%llu resets=%llu unknown_handle=%llu other_feature=%llu before_restore=%llu creations=%llu\r\n",
                         t.wHour, t.wMinute, t.wSecond, static_cast<unsigned long long>(sc.id), sc.state, sc.reason, sc.req_w, sc.req_h, sc.created_w, sc.created_h,
                         sc.last_w, sc.last_h, sc.streak, sc.need, static_cast<unsigned long long>(sc.evals), static_cast<unsigned long long>(sc.matching),
                         static_cast<unsigned long long>(sc.mismatched), static_cast<unsigned long long>(sc.incoherent), static_cast<unsigned long long>(sc.failed),
                         static_cast<unsigned long long>(sc.resets), static_cast<unsigned long long>(sc.unknown_handle), static_cast<unsigned long long>(sc.other_feature),
                         static_cast<unsigned long long>(sc.before_restore), static_cast<unsigned long long>(sc.creations));
        if (m > 0) WriteFile(f, cl, static_cast<DWORD>(m), &written, nullptr);
    }
    CloseHandle(f);
}

// Consumer (discovery thread only): deliver and release.
void seen_drain(void*)
{
    write_status_line();
    for (;;) {
        uint32_t r = g_seen_read.load(std::memory_order_acquire);
        if (r == g_seen_write.load(std::memory_order_acquire)) return;
        SeenEntry& e = g_seen[r % kSeenCapacity];
        IUnknown* obj = e.object.load(std::memory_order_acquire);
        if (!obj) { // claimed, not yet stored: wait briefly, never for ever
            bool ready = false;
            for (int spins = 0; spins < 1000 && !ready; ++spins) { Sleep(0); ready = e.object.load(std::memory_order_acquire) != nullptr; }
            if (!ready) return;
            continue;
        }
        uint32_t kind = e.kind.load(std::memory_order_relaxed);
        e.object.store(nullptr, std::memory_order_release);
        g_seen_read.store(r + 1, std::memory_order_release);
        if (DiscoverySubscriber* s = g_disc_sub.load(std::memory_order_acquire)) {
            if (kind == static_cast<uint32_t>(SeenKind::Device) && s->device) s->device(static_cast<ID3D12Device*>(obj), s->user);
            else if (kind == static_cast<uint32_t>(SeenKind::Queue) && s->queue) s->queue(static_cast<ID3D12CommandQueue*>(obj), s->user);
            else if (kind == static_cast<uint32_t>(SeenKind::List) && s->list) s->list(static_cast<ID3D12CommandList*>(obj), s->user);
        }
        obj->Release();
        g_seen_delivered.fetch_add(1, std::memory_order_relaxed);
    }
}

// ---- Input state written by WndProc / XInput detour, read by render thread
struct InputState {
    std::mutex m;
    float mouse_x = 0, mouse_y = 0;
    bool have_mouse = false;
    bool raw_mouse_seen = false;
    bool buttons[5] = {};
    float wheel = 0;
    struct Key { int vk; bool down; };
    std::vector<Key> keys;
    XINPUT_STATE pad{};
    bool pad_valid = false;
    RECT client{};
};
InputState g_in;

struct FrameCtx {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12Resource> back_buffer;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    UINT64 fence_value = 0;
};

struct State {
    std::mutex mutex;
    IDXGISwapChain* swapchain = nullptr;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> texture_queue;
    ComPtr<ID3D12DescriptorHeap> rtv_heap, srv_heap;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;
    UINT64 fence_next = 1;
    std::vector<FrameCtx> frames;
    UINT rtv_stride = 0, srv_stride = 0;
    std::vector<bool> srv_used;
    HWND hwnd = nullptr;
    WNDPROC orig_wndproc = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool imgui_ready = false;
    bool buffers_ready = false;
    bool was_open = false;
    LARGE_INTEGER last_present{};
    LARGE_INTEGER qpc_freq{};
    void* dummy_swapchain_identity = nullptr;
};
State g_state;

// Diagnostic polling must never wait on the render thread or on the GPU.
void write_menu_progress(HANDLE log)
{
    auto ps = na::present::GetStats();
    char p_line[256];
    int pn = snprintf(p_line, sizeof(p_line), "present_gpu submitted=%llu completed=%llu owned=%u device_hr=0x%08lX queried_latency_handle=0x%llX\r\n",
        static_cast<unsigned long long>(ps.gpu_submitted), static_cast<unsigned long long>(ps.gpu_completed), ps.slots_owned,
        static_cast<unsigned long>(ps.device_hr), static_cast<unsigned long long>(ps.queried_latency_handle));
    DWORD pw = 0;
    if (pn > 0) WriteFile(log, p_line, static_cast<DWORD>(pn), &pw, nullptr);
    std::unique_lock<std::mutex> lock(g_state.mutex, std::try_to_lock);
    if (!lock.owns_lock() || !g_state.fence || !g_state.device) return;
    char line[256];
    int n = snprintf(line, sizeof(line), "menu_gpu draws=%llu submitted=%llu completed=%llu device_hr=0x%08lX observed_queue=0x%llX\r\n",
                     static_cast<unsigned long long>(g_frames.load()),
                     static_cast<unsigned long long>(g_menu_draw_marker.load()),
                     static_cast<unsigned long long>(g_state.fence->GetCompletedValue()),
                     static_cast<unsigned long>(g_state.device->GetDeviceRemovedReason()),
                     static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_direct_queue.load())));
    DWORD written = 0;
    if (n > 0) WriteFile(log, line, static_cast<DWORD>(n), &written, nullptr);
    auto nr_progress = na::nr::GetStats();
    n = snprintf(line, sizeof(line), "frame_progress nr_submitted=%llu nr_completed=%llu direct_queues=%u present_thread=%u\r\n",
        static_cast<unsigned long long>(nr_progress.gpu_submitted), static_cast<unsigned long long>(nr_progress.gpu_completed),
        g_known_direct_n.load(), ps.real_present_thread);
    if (n > 0) WriteFile(log, line, static_cast<DWORD>(n), &written, nullptr);
}

void SrvAlloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
              D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
{
    for (uint32_t i = 0; i < kSrvHeapSize; ++i) {
        if (!g_state.srv_used[i]) {
            g_state.srv_used[i] = true;
            cpu->ptr = g_state.srv_heap->GetCPUDescriptorHandleForHeapStart().ptr +
                       static_cast<SIZE_T>(i) * g_state.srv_stride;
            gpu->ptr = g_state.srv_heap->GetGPUDescriptorHandleForHeapStart().ptr +
                       static_cast<UINT64>(i) * g_state.srv_stride;
            return;
        }
    }
    cpu->ptr = 0;
    gpu->ptr = 0;
}
void SrvFree(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
             D3D12_GPU_DESCRIPTOR_HANDLE)
{
    SIZE_T base = g_state.srv_heap->GetCPUDescriptorHandleForHeapStart().ptr;
    if (cpu.ptr < base) return;
    SIZE_T i = (cpu.ptr - base) / g_state.srv_stride;
    if (i < kSrvHeapSize) g_state.srv_used[i] = false;
}

// ---- WndProc: records input only; never calls ImGui. ----
void record_raw_input(HRAWINPUT h)
{
    alignas(8) BYTE buf[sizeof(RAWINPUT) + 64];
    UINT size = sizeof(buf);
    if (real_get_raw_input_data(h, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1))
        return;
    const RAWINPUT* ri = reinterpret_cast<const RAWINPUT*>(buf);
    if (ri->header.dwType != RIM_TYPEMOUSE) return;
    const RAWMOUSE& m = ri->data.mouse;
    std::lock_guard<std::mutex> lock(g_in.m);
    g_in.raw_mouse_seen = true;
    if (m.usFlags & MOUSE_MOVE_ABSOLUTE) {
        // Absolute: 0..65535 over the virtual screen; map into client space.
        int sw = GetSystemMetrics(SM_CXVIRTUALSCREEN), sh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        POINT p{static_cast<LONG>(m.lLastX * sw / 65535.0f), static_cast<LONG>(m.lLastY * sh / 65535.0f)};
        ScreenToClient(g_state.hwnd, &p);
        g_in.mouse_x = static_cast<float>(p.x);
        g_in.mouse_y = static_cast<float>(p.y);
    } else {
        g_in.mouse_x += static_cast<float>(m.lLastX);
        g_in.mouse_y += static_cast<float>(m.lLastY);
    }
    float w = static_cast<float>(g_in.client.right - g_in.client.left);
    float hgt = static_cast<float>(g_in.client.bottom - g_in.client.top);
    if (g_in.mouse_x < 0) g_in.mouse_x = 0;
    if (g_in.mouse_y < 0) g_in.mouse_y = 0;
    if (w > 0 && g_in.mouse_x > w - 1) g_in.mouse_x = w - 1;
    if (hgt > 0 && g_in.mouse_y > hgt - 1) g_in.mouse_y = hgt - 1;
    g_in.have_mouse = true;
    USHORT f = m.usButtonFlags;
    if (f & RI_MOUSE_LEFT_BUTTON_DOWN) g_in.buttons[0] = true;
    if (f & RI_MOUSE_LEFT_BUTTON_UP) g_in.buttons[0] = false;
    if (f & RI_MOUSE_RIGHT_BUTTON_DOWN) g_in.buttons[1] = true;
    if (f & RI_MOUSE_RIGHT_BUTTON_UP) g_in.buttons[1] = false;
    if (f & RI_MOUSE_MIDDLE_BUTTON_DOWN) g_in.buttons[2] = true;
    if (f & RI_MOUSE_MIDDLE_BUTTON_UP) g_in.buttons[2] = false;
    if (f & RI_MOUSE_WHEEL) g_in.wheel += static_cast<SHORT>(m.usButtonData) / 120.0f;
}

// A42 cursor model: while the menu is open the REAL Windows cursor is the
// menu cursor. Its position is read every frame (GetCursorPos), buttons via
// GetAsyncKeyState, so nothing depends on which window the game routes
// mouse messages to. The cursor is forced visible and unclipped on the
// window thread (these messages), and restored on close. Raw input and
// legacy mouse messages are still swallowed so the game gets nothing.
constexpr UINT kMsgCursorShow = WM_APP + 31;
constexpr UINT kMsgCursorRestore = WM_APP + 32;
constexpr UINT kMsgNudge = WM_APP + 33;        // A63: shrink the game window by one pixel
constexpr UINT_PTR kNudgeTimer = 0x4E41;       // A64: restore timer id on the game's window ("NA")
constexpr UINT kNudgeRestoreMs = 120;          // A64: restore after this, by timer, not by frames
constexpr uint32_t kNudgeRestoreRetries = 8;   // A64: restore attempts before giving up
int g_cursor_shows = 0;      // ShowCursor(TRUE) calls we made (window thread)
RECT g_saved_clip{};
bool g_saved_clip_valid = false;
// A63/A64 picture-rebuild nudge: state machine, window thread only except the
// request counter and the readable snapshot.
std::atomic<uint32_t> g_nudge_requests{0}, g_nudge_pending{0};
std::atomic<uint32_t> g_nudge_state{0}, g_nudge_attempts{0}, g_nudge_failures{0}, g_nudge_retries{0};
std::atomic<uint64_t> g_nudge_restored_tick{0}, g_nudge_started_tick{0}, g_nudge_confirm_id{0};
std::atomic<uint32_t> g_nudge_req_w{0}, g_nudge_req_h{0}; // A66: the size the NEXT request asks for (written by RequestPictureRebuild)
uint32_t g_nudge_frozen_w = 0, g_nudge_frozen_h = 0;      // A67: the admitted request's size, frozen at admission (present thread writes, window thread reads after the post)
std::atomic<uint32_t> g_nudge_rejected{0};                // A67: requests refused because one was in flight
std::mutex g_nudge_reason_m;
char g_nudge_reason[96] = {};
RECT g_nudge_rect{};
uint32_t g_nudge_restore_tries = 0;
constexpr uint64_t kNudgeDeadlineMs = 5000;      // A66: nudge must have restored by then, else it fails on its own
constexpr uint32_t kConfirmNeed = 30;            // A66: consecutive matching evaluations
constexpr uint32_t kConfirmCreateMs = 2500;      // A66: creation must follow the restoration within this
constexpr uint32_t kConfirmStreakMs = 6000;      // A66: the streak must complete within this after the creation
constexpr uint32_t kConfirmOverallMs = 15000;    // A67: the whole request, replacements included, ends by then

void nudge_fail(const char* why)
{
    {
        std::lock_guard<std::mutex> lock(g_nudge_reason_m);
        strncpy_s(g_nudge_reason, why, _TRUNCATE);
    }
    g_nudge_state.store(4, std::memory_order_release);
    g_nudge_failures.fetch_add(1, std::memory_order_relaxed);
    g_nudge_pending.store(0, std::memory_order_release);
    na::ngx::CancelSizeConfirmation(); // A67: a failed nudge withdraws its confirmation request
}
// A66: the nudge's own deadline, checked from every reader and every present,
// so a request never stays pending because nobody looked at it.
void nudge_tick()
{
    uint32_t st = g_nudge_state.load(std::memory_order_acquire);
    if ((st == 1 || st == 2) && GetTickCount64() - g_nudge_started_tick.load(std::memory_order_acquire) > kNudgeDeadlineMs) {
        if (HWND h = g_state.hwnd) KillTimer(h, kNudgeTimer); // harmless from another thread if the timer is gone
        nudge_fail("the window nudge did not complete within 5 seconds");
    }
}
// Window thread: put the window back; retried by the timer until it works or the retries run out.
void nudge_restore(HWND hwnd)
{
    int w = g_nudge_rect.right - g_nudge_rect.left, h = g_nudge_rect.bottom - g_nudge_rect.top;
    if (SetWindowPos(hwnd, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER)) {
        KillTimer(hwnd, kNudgeTimer);
        g_nudge_restored_tick.store(GetTickCount64(), std::memory_order_release);
        // A67: the request was armed at admission (before the window moved);
        // this is the restoration boundary after which evaluations count.
        na::ngx::MarkSizeConfirmationRestored(g_nudge_confirm_id.load(std::memory_order_acquire));
        g_nudge_state.store(3, std::memory_order_release);
        g_nudge_pending.store(0, std::memory_order_release);
        return;
    }
    ++g_nudge_restore_tries;
    g_nudge_retries.store(g_nudge_restore_tries, std::memory_order_relaxed);
    if (g_nudge_restore_tries >= kNudgeRestoreRetries) {
        KillTimer(hwnd, kNudgeTimer);
        char buf[96];
        snprintf(buf, sizeof(buf), "the window refused to return to %dx%d (error %lu)", w, h, GetLastError());
        nudge_fail(buf);
    } // else the timer fires again
}

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    na::latency::pcl::Message(msg, wp);
    if (msg == WM_KEYDOWN && wp == VK_HOME && !(lp & (1 << 30))) {
        g_open.store(!g_open.load(std::memory_order_relaxed), std::memory_order_release);
        return 0;
    }
    if (msg == kMsgCursorShow) {
        GetClipCursor(&g_saved_clip);
        g_saved_clip_valid = true;
        ClipCursor(nullptr);
        POINT frozen{};
        if (real_get_cursor_pos(&frozen)) { // what the game keeps seeing while the menu is open (A44)
            g_frozen_x.store(frozen.x, std::memory_order_relaxed);
            g_frozen_y.store(frozen.y, std::memory_order_relaxed);
        }
        while (ShowCursor(TRUE) < 0 && g_cursor_shows < 64) ++g_cursor_shows;
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return 0;
    }
    if (msg == kMsgCursorRestore) {
        while (g_cursor_shows > 0) { ShowCursor(FALSE); --g_cursor_shows; }
        if (g_saved_clip_valid) ClipCursor(&g_saved_clip);
        g_saved_clip_valid = false;
        return 0;
    }
    if (msg == kMsgNudge) { // A63/A64: one pixel smaller; the game sees WM_SIZE and (hopefully) rebuilds
        g_nudge_attempts.fetch_add(1, std::memory_order_relaxed);
        g_nudge_restore_tries = 0;
        if (!GetWindowRect(hwnd, &g_nudge_rect)) { nudge_fail("the game window's size could not be read"); return 0; }
        int w = g_nudge_rect.right - g_nudge_rect.left, h = g_nudge_rect.bottom - g_nudge_rect.top;
        if (w <= 64 || h <= 64) { nudge_fail("the game window is too small to nudge"); return 0; }
        if (!SetWindowPos(hwnd, nullptr, 0, 0, w - 1, h - 1, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER)) {
            char buf[96];
            snprintf(buf, sizeof(buf), "the game window refused the resize (error %lu)", GetLastError());
            nudge_fail(buf);
            return 0;
        }
        g_nudge_state.store(2, std::memory_order_release);
        // Restoration by timer: a message, delivered as long as the game pumps
        // its window at all, whether or not it presents frames meanwhile.
        if (!SetTimer(hwnd, kNudgeTimer, kNudgeRestoreMs, nullptr)) {
            nudge_restore(hwnd); // no timer available: put it back at once (the game still saw the size change)
            if (g_nudge_state.load(std::memory_order_acquire) == 2) nudge_fail("no restore timer and the window refused to return");
        }
        return 0;
    }
    if (msg == WM_TIMER && wp == kNudgeTimer) {
        if (g_nudge_state.load(std::memory_order_acquire) == 2) nudge_restore(hwnd);
        else KillTimer(hwnd, kNudgeTimer);
        return 0;
    }
    if (!g_open.load(std::memory_order_acquire) || !g_state.imgui_ready)
        return CallWindowProcW(g_state.orig_wndproc, hwnd, msg, wp, lp);

    switch (msg) {
    case WM_INPUT:
        record_raw_input(reinterpret_cast<HRAWINPUT>(lp));
        return DefWindowProcW(hwnd, msg, wp, lp); // required cleanup; game not called
    case WM_MOUSEMOVE: {
        std::lock_guard<std::mutex> lock(g_in.m);
        if (!g_in.raw_mouse_seen) {
            g_in.mouse_x = static_cast<float>(static_cast<short>(LOWORD(lp)));
            g_in.mouse_y = static_cast<float>(static_cast<short>(HIWORD(lp)));
            g_in.have_mouse = true;
        }
        return 0;
    }
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK: {
        std::lock_guard<std::mutex> lock(g_in.m);
        if (!g_in.raw_mouse_seen) {
            int b = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP || msg == WM_LBUTTONDBLCLK) ? 0
                    : (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP || msg == WM_RBUTTONDBLCLK) ? 1 : 2;
            bool down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN ||
                        msg == WM_LBUTTONDBLCLK || msg == WM_RBUTTONDBLCLK || msg == WM_MBUTTONDBLCLK;
            g_in.buttons[b] = down;
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        std::lock_guard<std::mutex> lock(g_in.m);
        if (!g_in.raw_mouse_seen) g_in.wheel += GET_WHEEL_DELTA_WPARAM(wp) / 120.0f;
        return 0;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP: {
        std::lock_guard<std::mutex> lock(g_in.m);
        if (g_in.keys.size() < 64)
            g_in.keys.push_back({static_cast<int>(wp), msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN});
        return 0;
    }
    case WM_CHAR: case WM_SYSCHAR: case WM_MOUSEHWHEEL: case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_ARROW)); // the real cursor IS the menu cursor (A42)
        return TRUE;
    default:
        return CallWindowProcW(g_state.orig_wndproc, hwnd, msg, wp, lp);
    }
}

// ---- XInput detour: the game sees a neutral pad while the menu is open;
// the menu reads the real state from the snapshot. ----
DWORD WINAPI xinput_common(int slot, DWORD idx, XINPUT_STATE* st)
{
    DWORD r = g_orig_xinput[slot](idx, st);
    if (r == ERROR_SUCCESS && st && idx == 0) {
        {
            std::lock_guard<std::mutex> lock(g_in.m);
            g_in.pad = *st;
            g_in.pad_valid = true;
        }
        if (g_open.load(std::memory_order_acquire)) {
            XINPUT_GAMEPAD neutral{};
            st->Gamepad = neutral;
        }
    }
    return r;
}
DWORD WINAPI XInputDetour0(DWORD i, XINPUT_STATE* s) { return xinput_common(0, i, s); }
DWORD WINAPI XInputDetour1(DWORD i, XINPUT_STATE* s) { return xinput_common(1, i, s); }
DWORD WINAPI XInputDetour2(DWORD i, XINPUT_STATE* s) { return xinput_common(2, i, s); }

ImGuiKey vk_to_key(int vk)
{
    switch (vk) {
    case VK_TAB: return ImGuiKey_Tab;
    case VK_LEFT: return ImGuiKey_LeftArrow;
    case VK_RIGHT: return ImGuiKey_RightArrow;
    case VK_UP: return ImGuiKey_UpArrow;
    case VK_DOWN: return ImGuiKey_DownArrow;
    case VK_PRIOR: return ImGuiKey_PageUp;
    case VK_NEXT: return ImGuiKey_PageDown;
    case VK_END: return ImGuiKey_End;
    case VK_BACK: return ImGuiKey_Backspace;
    case VK_SPACE: return ImGuiKey_Space;
    case VK_RETURN: return ImGuiKey_Enter;
    case VK_ESCAPE: return ImGuiKey_Escape;
    default: return ImGuiKey_None;
    }
}

// Render thread: apply recorded input to ImGui before NewFrame.
void apply_input(bool just_opened)
{
    ImGuiIO& io = ImGui::GetIO();
    std::lock_guard<std::mutex> lock(g_in.m);
    GetClientRect(g_state.hwnd, &g_in.client);
    if (just_opened) {
        g_in.wheel = 0;
        g_in.keys.clear();
    }
    // A42: the real cursor's position and buttons, independent of message routing.
    POINT cp{};
    if (real_get_cursor_pos(&cp) && ScreenToClient(g_state.hwnd, &cp)) {
        float w = static_cast<float>(g_in.client.right - g_in.client.left);
        float hgt = static_cast<float>(g_in.client.bottom - g_in.client.top);
        g_in.mouse_x = static_cast<float>(cp.x);
        g_in.mouse_y = static_cast<float>(cp.y);
        if (g_in.mouse_x < 0) g_in.mouse_x = 0;
        if (g_in.mouse_y < 0) g_in.mouse_y = 0;
        if (w > 0 && g_in.mouse_x > w - 1) g_in.mouse_x = w - 1;
        if (hgt > 0 && g_in.mouse_y > hgt - 1) g_in.mouse_y = hgt - 1;
        g_in.have_mouse = true;
    }
    if (g_in.have_mouse) {
        io.AddMousePosEvent(g_in.mouse_x, g_in.mouse_y);
        g_mouse_x.store(g_in.mouse_x);
        g_mouse_y.store(g_in.mouse_y);
    }
    const int vks[3] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON};
    for (int b = 0; b < 3; ++b) {
        bool down = (real_get_async_key_state(vks[b]) & 0x8000) != 0;
        if (just_opened) down = false; // the click that opened nothing must not select
        io.AddMouseButtonEvent(b, down);
    }
    if (g_in.wheel != 0.0f) {
        io.AddMouseWheelEvent(0.0f, g_in.wheel);
        g_in.wheel = 0.0f;
    }
    for (const InputState::Key& k : g_in.keys) {
        ImGuiKey key = vk_to_key(k.vk);
        if (key != ImGuiKey_None) io.AddKeyEvent(key, k.down);
    }
    g_in.keys.clear();
    if (g_in.pad_valid) {
        const XINPUT_GAMEPAD& g = g_in.pad.Gamepad;
        auto btn = [&](ImGuiKey key, WORD mask) { io.AddKeyEvent(key, (g.wButtons & mask) != 0); };
        btn(ImGuiKey_GamepadDpadUp, XINPUT_GAMEPAD_DPAD_UP);
        btn(ImGuiKey_GamepadDpadDown, XINPUT_GAMEPAD_DPAD_DOWN);
        btn(ImGuiKey_GamepadDpadLeft, XINPUT_GAMEPAD_DPAD_LEFT);
        btn(ImGuiKey_GamepadDpadRight, XINPUT_GAMEPAD_DPAD_RIGHT);
        btn(ImGuiKey_GamepadFaceDown, XINPUT_GAMEPAD_A);
        btn(ImGuiKey_GamepadFaceRight, XINPUT_GAMEPAD_B);
        btn(ImGuiKey_GamepadFaceLeft, XINPUT_GAMEPAD_X);
        btn(ImGuiKey_GamepadFaceUp, XINPUT_GAMEPAD_Y);
        btn(ImGuiKey_GamepadStart, XINPUT_GAMEPAD_START);
        btn(ImGuiKey_GamepadBack, XINPUT_GAMEPAD_BACK);
        btn(ImGuiKey_GamepadL1, XINPUT_GAMEPAD_LEFT_SHOULDER);
        btn(ImGuiKey_GamepadR1, XINPUT_GAMEPAD_RIGHT_SHOULDER);
        auto axis = [&](ImGuiKey key, SHORT v, SHORT lo, SHORT hi) {
            float t = (static_cast<float>(v) - lo) / static_cast<float>(hi - lo);
            if (t < 0) t = 0; if (t > 1) t = 1;
            io.AddKeyAnalogEvent(key, t > 0.1f, t);
        };
        axis(ImGuiKey_GamepadLStickLeft, g.sThumbLX, -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, -32768);
        axis(ImGuiKey_GamepadLStickRight, g.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, 32767);
        axis(ImGuiKey_GamepadLStickUp, g.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, 32767);
        axis(ImGuiKey_GamepadLStickDown, g.sThumbLY, -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, -32768);
    }
}

void release_buffers_locked()
{
    for (FrameCtx& f : g_state.frames) f.back_buffer.Reset();
    g_state.buffers_ready = false;
}

bool ensure_buffers_locked(IDXGISwapChain* sc)
{
    if (g_state.buffers_ready) return true;
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(sc->GetDesc(&desc))) return false;
    UINT count = desc.BufferCount;
    if (g_state.frames.size() != count) {
        g_state.frames.clear();
        g_state.frames.resize(count);
        D3D12_DESCRIPTOR_HEAP_DESC rh{};
        rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rh.NumDescriptors = count;
        if (FAILED(g_state.device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&g_state.rtv_heap))))
            return false;
        for (UINT i = 0; i < count; ++i) {
            if (FAILED(g_state.device->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_state.frames[i].allocator))))
                return false;
            g_state.frames[i].rtv.ptr =
                g_state.rtv_heap->GetCPUDescriptorHandleForHeapStart().ptr +
                static_cast<SIZE_T>(i) * g_state.rtv_stride;
        }
    }
    g_state.format = desc.BufferDesc.Format;
    // A71 (Astra B-007): back buffers are NOT retained here. A cached buffer
    // reference keeps the swap chain alive past the game's last Release and
    // makes its replacement chain for the same window fail (E_ACCESSDENIED).
    // acquire_back_buffer_locked fetches the current buffer for one frame.
    g_state.buffers_ready = true;
    return true;
}

// The current real back buffer for THIS frame only: fetched, its RTV
// (re)written, used, and released by the caller before the Present returns.
bool acquire_back_buffer_locked(IDXGISwapChain* sc, UINT idx, ComPtr<ID3D12Resource>& out)
{
    if (idx >= g_state.frames.size()) return false;
    if (FAILED(real_get_buffer(sc, idx, IID_PPV_ARGS(&out)))) return false;
    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = g_state.format;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    g_state.device->CreateRenderTargetView(out.Get(), &rtv, g_state.frames[idx].rtv);
    return true;
}

bool ensure_init_locked(IDXGISwapChain* sc)
{
    if (g_state.imgui_ready) {
        // A78: a replacement chain for the same window (on_chain_gone cleared
        // the slot) must be adopted here; the early return never did, so
        // resizes and later chain deaths went unnoticed and the buffer
        // bookkeeping (format, heap, frame count) stayed stale (Astra's
        // lifecycle matrix: menu open across replacement removed the device).
        const DXGI_FORMAT before = g_state.format;
        const size_t frames_before = g_state.frames.size();
        if (!ensure_buffers_locked(sc)) return false;
        g_state.swapchain = sc;
        if (before != g_state.format || frames_before != g_state.frames.size()) {
            // The DX12 backend's pipeline is built for one render-target
            // format and frame count: re-create it for the new chain. The
            // ImGui context, fonts and the Win32 backend stay; device objects
            // are rebuilt right away (eager, as at first init).
            ImGui_ImplDX12_Shutdown();
            ImGui_ImplDX12_InitInfo info{};
            info.Device = g_state.device.Get();
            info.CommandQueue = g_state.texture_queue.Get();
            info.NumFramesInFlight = static_cast<int>(g_state.frames.size());
            info.RTVFormat = g_state.format;
            info.DSVFormat = DXGI_FORMAT_UNKNOWN;
            info.SrvDescriptorHeap = g_state.srv_heap.Get();
            info.SrvDescriptorAllocFn = &SrvAlloc;
            info.SrvDescriptorFreeFn = &SrvFree;
            if (!ImGui_ImplDX12_Init(&info) || !ImGui_ImplDX12_CreateDeviceObjects()) { g_state.imgui_ready = false; return false; }
        }
        return true;
    }
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&g_state.device)))) return false;
    na::gputime::SetDevice(g_state.device.Get());
    g_state.rtv_stride = g_state.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    g_state.srv_stride = g_state.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_DESCRIPTOR_HEAP_DESC sh{};
    sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    sh.NumDescriptors = kSrvHeapSize;
    sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_state.device->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&g_state.srv_heap)))) return false;
    g_state.srv_used.assign(kSrvHeapSize, false);
    if (FAILED(g_state.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_state.fence)))) return false;
    g_state.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ensure_buffers_locked(sc)) return false;
    if (FAILED(g_state.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 g_state.frames[0].allocator.Get(), nullptr,
                                                 IID_PPV_ARGS(&g_state.list))))
        return false;
    g_state.list->Close();

    D3D12_COMMAND_QUEUE_DESC upload_desc{};
    upload_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g_state.device->CreateCommandQueue(&upload_desc, IID_PPV_ARGS(&g_state.texture_queue)))) return false;
    g_state.texture_queue->SetName(L"NeuralAlias ImGui texture uploads");
    g_texture_queue.store(g_state.texture_queue.Get(), std::memory_order_release);

    DXGI_SWAP_CHAIN_DESC desc{};
    sc->GetDesc(&desc);
    g_state.hwnd = desc.OutputWindow;
    {
        // Game name for the title bar: the window title, trimmed of symbols
        // ImGui's default glyph range cannot draw (e.g. the trademark sign).
        wchar_t wt[96] = {};
        GetWindowTextW(g_state.hwnd, wt, 96);
        char* o = g_game_name;
        for (const wchar_t* c = wt; *c && o < g_game_name + sizeof(g_game_name) - 1; ++c)
            if (*c >= 0x20 && *c < 0x7F) *o++ = static_cast<char>(*c);
        *o = 0;
        while (o > g_game_name && o[-1] == ' ') *--o = 0;
    }
    g_state.orig_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        g_state.hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&OverlayWndProc)));
    QueryPerformanceFrequency(&g_state.qpc_freq);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    na::ui::SetBoldText(na::settings::Get().text_bold.load() != 0);
    na::ui::ApplyStyle();
    na::ui::LoadFonts();
    ImGui_ImplWin32_Init(g_state.hwnd);
    ImGui_ImplDX12_InitInfo info;
    info.Device = g_state.device.Get();
    // Texture uploads only. The actual menu draw stays on the game's queue,
    // ordered after the presenter copy. ImGui completes its upload before
    // returning, so the later draw may safely read the texture on that queue.
    info.CommandQueue = g_state.texture_queue.Get();
    info.NumFramesInFlight = static_cast<int>(g_state.frames.size());
    info.RTVFormat = g_state.format;
    info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    info.SrvDescriptorHeap = g_state.srv_heap.Get();
    info.SrvDescriptorAllocFn = &SrvAlloc;
    info.SrvDescriptorFreeFn = &SrvFree;
    if (!ImGui_ImplDX12_Init(&info)) return false;
    // Compile/create the menu pipelines before this chain can start generating
    // frames. Do not first initialize the renderer during the transition from
    // the presentation worker to an inline Home frame.
    if (!ImGui_ImplDX12_CreateDeviceObjects()) return false;
    g_state.imgui_ready = true;
    g_state.swapchain = sc;
    g_initialized.store(1, std::memory_order_release);
    g_seen_device.store(g_state.device.Get(), std::memory_order_release);
    if (seen_push(SeenKind::Device, g_state.device.Get())) g_device_announced.store(true, std::memory_order_release); // A49: latch on success only
    return true;
}

// ---- Host-owned pages ----
void draw_page_intro(na::menu::Page p)
{
    using namespace na::ui;
    switch (p) {
    case na::menu::Page::Picture:
        Eyebrow("IMAGE QUALITY");
        Heading("A clearer picture.");
        Lead("Choose the balance between fine detail and smoother performance.");
        break;
    case na::menu::Page::Enhancements:
        Eyebrow("ADDITIONAL FEATURES");
        Heading("More possibilities.");
        Lead("Availability depends on the game and the installed version.");
        break;
    case na::menu::Page::Preferences:
        Eyebrow("MAKE IT YOURS");
        Heading("Quiet by design.");
        Lead("Keep the game in view and the controls easy to read.");
        break;
    default:
        Eyebrow("WHEN YOU NEED IT");
        Heading("Details & support.");
        Lead("Picture status and useful information in one place.");
        break;
    }
    ImGui::Dummy(ImVec2(0, S(4)));
}

void draw_preferences()
{
    using namespace na::ui;
    na::settings::Settings& st = na::settings::Get();
    BeginGroupBox("prefs");
    static const char* kWeights[] = {"Standard", "Bold"};
    int weight = st.text_bold.load() ? 1 : 0;
    BeginRow("Text weight", "Bold sets all menu text in the heavier face.", DropdownWidth());
    if (Combo("textweight", &weight, kWeights, 2, DropdownWidth())) {
        st.text_bold.store(weight ? 1u : 0u);
        SetBoldText(weight != 0);
        na::settings::Save();
    }
    EndRow();
    RowDivider();
    BeginRow("Window", "Drag the title bar to move the menu; drag its edges or corner to resize it. The size is remembered.",
             ButtonWidth("Reset"));
    if (Button("Reset")) {
        st.win_x.store(0); st.win_y.store(0); st.win_w.store(0); st.win_h.store(0);
        g_window_reset = true;
        na::settings::Save();
    }
    EndRow();
    RowDivider();
    BeginRow("Always-on information", "No floating window or performance badge appears over your game.", S(60));
    ValueText("Off");
    EndRow();
    RowDivider();
    BeginRow("Menu shortcut", "The game keeps running while this menu is open.", S(80));
    ValueText("Home");
    EndRow();
    EndGroupBox();
    ImGui::Dummy(ImVec2(0, S(8)));
    Hint("The menu takes mouse, keyboard and controller input while it is open. The game receives none until you close it.");
}

void draw_details_host_rows()
{
    using namespace na::ui;
    char buf[160];
    na::gputime::Stats gt = na::gputime::GetStats();
    BeginGroupBox("perf");
    if (g_frame_ms.load() > 0.0f)
        snprintf(buf, sizeof(buf), "Frames shown: %.0f per second (%.1f ms between frames).",
                 g_fps.load(), g_frame_ms.load());
    else
        snprintf(buf, sizeof(buf), "Frames shown: not measured yet.");
    BeginRow("Performance", buf, S(10));
    EndRow();
    if (gt.samples)
        snprintf(buf, sizeof(buf), "DLSS work on the graphics card: %.1f ms per frame (average %.1f ms). This is not input delay.",
                 gt.last_ms, gt.avg_ms);
    else
        snprintf(buf, sizeof(buf), "DLSS work on the graphics card: not measured (no number means no measurement, not zero cost).");
    Hint(buf);
    EndGroupBox();
}

void draw_details_technical()
{
    using namespace na::ui;
    if (!BeginDetails("Technical details")) return;
    char buf[200];
    na::ngx::Stats ngx = na::ngx::GetStats();
    snprintf(buf, sizeof(buf), "Route: %s", g_route_tag[0] ? g_route_tag : "A");
    Hint(buf);
    snprintf(buf, sizeof(buf), "Overlay frames drawn %llu, presents seen %llu, controller modules hooked %u.",
             static_cast<unsigned long long>(g_frames.load()),
             static_cast<unsigned long long>(g_presents.load()), g_gamepad_hooked.load());
    Hint(buf);
    snprintf(buf, sizeof(buf), "DLSS module attached %u, connected at startup %u, features created %llu, passes run %llu.",
             ngx.module_attached, ngx.core_preloaded, static_cast<unsigned long long>(ngx.creates),
             static_cast<unsigned long long>(ngx.evaluates));
    Hint(buf);
    na::present::Stats pr = na::present::GetStats(); // A62: the screen-output path in plain words
    if (pr.enabled && pr.proxied)
        snprintf(buf, sizeof(buf), "Screen output: NeuralAlias-owned. Shown %.0f/s from %.0f/s rendered (display %.0f Hz); generated shown %llu, runtime vetoes %llu, pacing misses %llu, dropped %llu.",
                 pr.shown_fps, pr.base_fps, pr.display_hz, static_cast<unsigned long long>(pr.generated_shown),
                 static_cast<unsigned long long>(pr.vetoed), static_cast<unsigned long long>(pr.late), static_cast<unsigned long long>(pr.dropped));
    else
        snprintf(buf, sizeof(buf), "Screen output: the game's own (Frame Generation unavailable on this path).");
    Hint(buf);
    Hint("Frame presentation rate and completed GPU work are different measurements; neither is input latency.");
}

void draw_panel(bool just_opened)
{
    using namespace na::ui;
    using na::menu::Page;
    ImGuiIO& io = ImGui::GetIO();
    na::settings::Settings& st = na::settings::Get();
    SetDisplayHeight(io.DisplaySize.y);
    // A44: movable and resizable window. Default: centred at the base size,
    // never larger than the screen minus a margin. A remembered placement
    // ([overlay] win_*) is used when it still fits; the window is kept on
    // screen and above the minimum size, and the layout reflows from its
    // ACTUAL size every frame (narrow -> navigation on top, text wraps).
    float margin = S(30);
    float max_w = io.DisplaySize.x - 2 * margin, max_h = io.DisplaySize.y - 2 * margin;
    float min_w = MinWindowWidth(), min_h = MinWindowHeight();
    if (min_w > max_w) min_w = max_w;
    if (min_h > max_h) min_h = max_h;
    float w = WindowWidth(), h = WindowHeight();
    if (w > max_w) w = max_w;
    if (h > max_h) h = max_h;
    bool remembered = st.win_w.load() >= static_cast<int32_t>(min_w) && st.win_h.load() >= static_cast<int32_t>(min_h);
    if (remembered) {
        w = static_cast<float>(st.win_w.load());
        h = static_cast<float>(st.win_h.load());
        if (w > max_w) w = max_w;
        if (h > max_h) h = max_h;
    }
    ImVec2 pos((io.DisplaySize.x - w) * 0.5f, (io.DisplaySize.y - h) * 0.5f);
    if (remembered) pos = ImVec2(static_cast<float>(st.win_x.load()), static_cast<float>(st.win_y.load()));
    if (pos.x + w > io.DisplaySize.x - margin) pos.x = io.DisplaySize.x - margin - w;
    if (pos.y + h > io.DisplaySize.y - margin) pos.y = io.DisplaySize.y - margin - h;
    if (pos.x < margin) pos.x = margin;
    if (pos.y < margin) pos.y = margin;
    ImGuiCond placement = (just_opened || g_window_reset) ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
    g_window_reset = false;
    ImGui::SetNextWindowPos(pos, placement);
    ImGui::SetNextWindowSize(ImVec2(w, h), placement);
    ImGui::SetNextWindowSizeConstraints(ImVec2(min_w, min_h), ImVec2(max_w, max_h));
    if (just_opened) ImGui::SetNextWindowFocus();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##neuralalias", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    // Actual placement after the user's drag/resize; keep it on screen and remember it.
    w = ImGui::GetWindowWidth();
    h = ImGui::GetWindowHeight();
    {
        ImVec2 p = ImGui::GetWindowPos();
        ImVec2 clamped = p;
        if (clamped.x + w > io.DisplaySize.x) clamped.x = io.DisplaySize.x - w;
        if (clamped.y + h > io.DisplaySize.y) clamped.y = io.DisplaySize.y - h;
        if (clamped.x < 0) clamped.x = 0;
        if (clamped.y < 0) clamped.y = 0;
        if (clamped.x != p.x || clamped.y != p.y) ImGui::SetWindowPos(clamped);
        bool changed = static_cast<int32_t>(clamped.x) != st.win_x.load() || static_cast<int32_t>(clamped.y) != st.win_y.load() ||
                       static_cast<int32_t>(w) != st.win_w.load() || static_cast<int32_t>(h) != st.win_h.load();
        if (changed) {
            st.win_x.store(static_cast<int32_t>(clamped.x));
            st.win_y.store(static_cast<int32_t>(clamped.y));
            st.win_w.store(static_cast<int32_t>(w));
            st.win_h.store(static_cast<int32_t>(h));
            g_window_dirty = true;
        }
        if (g_window_dirty && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) { // save once the drag ends
            na::settings::Save();
            g_window_dirty = false;
        }
    }
    bool narrow = w < S(kBaseNarrowWidth);
    bool close = false;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) close = true;

    // Title bar.
    float pad_x = S(30), pad_y = S(16);
    ImGui::SetCursorPos(ImVec2(pad_x, pad_y));
    ImGui::BeginGroup();
    if (TitleBar(g_game_name)) close = true;
    ImGui::EndGroup();
    float title_h = ImGui::GetCursorPosY() + pad_y;
    ImGui::SetCursorPos(ImVec2(0, title_h));
    ImGui::Separator();

    // Footer metrics.
    PushHelp();
    float foot_h = ImGui::GetTextLineHeight() + S(30);
    PopFont();
    float body_top = ImGui::GetCursorPosY();
    float body_h = h - body_top - foot_h;

    Page page = na::menu::CurrentPage();
    auto nav = [&](float nav_w, bool horizontal) {
        float used = 0, avail = ImGui::GetContentRegionAvail().x;
        for (int i = 0; i < na::menu::kPageCount; ++i) {
            Page p = static_cast<Page>(i + 1);
            float bw = horizontal ? NavButtonWidth(na::menu::PageName(p)) : nav_w;
            if (horizontal && i) {
                if (used + S(8) + bw <= avail) {
                    ImGui::SameLine(0.0f, S(8));
                    used += S(8);
                } else {
                    used = 0; // wrap to the next line
                }
            }
            if (NavButton(na::menu::PageName(p), p == page, bw)) {
                page = p;
                na::menu::SetCurrentPage(p);
            }
            used += bw;
        }
    };

    float content_x = 0;
    if (!narrow) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgNav);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(26)));
        ImGui::BeginChild("##nav", ImVec2(NavWidth(), body_h), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar);
        nav(NavWidth() - S(28), false);
        // Nav note at the bottom.
        PushHelp();
        float note_y = ImGui::GetWindowHeight() - ImGui::GetTextLineHeight() - S(30);
        if (note_y > ImGui::GetCursorPosY()) ImGui::SetCursorPosY(note_y);
        ImGui::SetCursorPosX(S(30));
        ImGui::TextColored(kTextDim, "Your game. Refined.");
        PopFont();
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, 0.0f);
        content_x = NavWidth();
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(p, ImVec2(p.x, p.y + body_h), ImGui::ColorConvertFloat4ToU32(kLine));
    }

    // Content (scrolls; title, navigation and footer stay put).
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(36), S(20)));
    ImGui::BeginChild("##content", ImVec2(w - content_x, body_h), ImGuiChildFlags_AlwaysUseWindowPadding);
    if (narrow) {
        nav(0, true);
        ImGui::Dummy(ImVec2(0, S(8)));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, S(8)));
    }
    if (g_controls) {
        g_controls(); // legacy full-body panel
    } else {
        draw_page_intro(page);
        switch (page) {
        case Page::Picture:
            if (na::menu::PageHasContent(Page::Picture))
                na::menu::DrawPage(Page::Picture, true);
            else
                Hint("This game's picture data still needs to be checked. Picture controls are unavailable.");
            break;
        case Page::Enhancements:
            na::menu::DrawPage(Page::Enhancements, true);
            ImGui::Dummy(ImVec2(0, S(8)));
            Hint("New features appear here when they are ready. Unavailable features do not change your picture.");
            break;
        case Page::Preferences:
            draw_preferences();
            na::menu::DrawPage(Page::Preferences, true);
            break;
        default:
            draw_details_host_rows();
            ImGui::Dummy(ImVec2(0, S(4)));
            na::menu::DrawPage(Page::Details, true);
            draw_details_technical();
            break;
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    // Footer.
    ImGui::SetCursorPos(ImVec2(0, h - foot_h));
    ImGui::Separator();
    ImGui::SetCursorPos(ImVec2(pad_x, h - foot_h + S(14)));
    PushHelp();
    ImGui::TextColored(kTextDim, "Home closes the menu   Â·   the game continues running");
    const char* right = "";
    uint32_t save = na::settings::LastSaveResult();
    if (save == 2) right = "Your choice is active for this session but could not be saved.";
    else if (save == 1) right = "Settings saved";
    float rw = ImGui::CalcTextSize(right).x;
    ImGui::SameLine(w - pad_x - rw);
    ImGui::TextColored(save == 2 ? kWarn : kTextDim, "%s", right);
    PopFont();
    ImGui::End();
    if (close) g_open.store(false, std::memory_order_release);
}

void render_locked(IDXGISwapChain* sc, ID3D12CommandQueue* queue, bool just_opened, ID3D12Resource* frame = nullptr)
{
    g_render_thread.store(GetCurrentThreadId(), std::memory_order_relaxed); // input hooks pass this thread through
    UINT idx = frame ? static_cast<UINT>(g_frames.load() % g_state.frames.size()) : real_index(sc);
    if (idx >= g_state.frames.size()) return;
    FrameCtx& f = g_state.frames[idx];
    // Never wait for game-queue work from inside its Present callback. A
    // driver/engine may release that work only after the callback returns.
    // If this allocator is still in use, skip this overlay draw and retry.
    if (g_state.fence->GetCompletedValue() == UINT64_MAX ||
        g_state.fence->GetCompletedValue() < f.fence_value) return;
    ComPtr<ID3D12Resource> back_buffer; // A71: this frame only
    if (frame) {
        back_buffer = frame;
        D3D12_RENDER_TARGET_VIEW_DESC rtv{};
        rtv.Format = g_state.format;
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        g_state.device->CreateRenderTargetView(frame, &rtv, f.rtv);
    } else if (!acquire_back_buffer_locked(sc, idx, back_buffer)) return;

    apply_input(just_opened);
    // A42: the hardware cursor is the menu cursor. A43 safety nets, checked
    // every frame: if the game re-hid it, draw ImGui's software cursor at the
    // same position instead; if the game re-clipped it to a point (mouse-look
    // lock), unclip again so it can reach the menu.
    {
        CURSORINFO ci{};
        ci.cbSize = sizeof(ci);
        bool showing = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) != 0;
        ImGui::GetIO().MouseDrawCursor = !showing;
        RECT clip{};
        if (GetClipCursor(&clip) && (clip.right - clip.left) < 8 && (clip.bottom - clip.top) < 8)
            ClipCursor(nullptr);
    }
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    draw_panel(just_opened);
    ImGui::Render();

    ImDrawData* draw = ImGui::GetDrawData();
    // New glyphs can update an EXISTING font atlas. The private upload queue
    // must not write it while an older game-queue menu draw is sampling it.
    // A completion check (not a wait) protects all atlas updates/destruction.
    // Leave the texture request pending when busy; the next frame retries.
    if (draw->Textures) {
        for (ImTextureData* texture : *draw->Textures) {
            if (texture->Status != ImTextureStatus_OK &&
                g_state.fence->GetCompletedValue() < g_state.fence_next - 1) return;
        }
    }
    if (FAILED(f.allocator->Reset()) || FAILED(g_state.list->Reset(f.allocator.Get(), nullptr))) return;

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = back_buffer.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_state.list->ResourceBarrier(1, &b);
    g_state.list->OMSetRenderTargets(1, &f.rtv, FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[] = {g_state.srv_heap.Get()};
    g_state.list->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(draw, g_state.list.Get());
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_state.list->ResourceBarrier(1, &b);
    if (FAILED(g_state.list->Close())) return;
    ID3D12CommandList* lists[] = {g_state.list.Get()};
    g_orig_execute(queue, 1, lists);
    f.fence_value = g_state.fence_next++;
    queue->Signal(g_state.fence.Get(), f.fence_value);
    g_menu_draw_marker.store(f.fence_value);
    g_frames.fetch_add(1, std::memory_order_relaxed);
}

// The finished frame (real back buffer on the old path, the game's proxy on
// the presenter path): Neural Rendering, motion analysis, capture.
void process_frame(ID3D12CommandQueue* queue, ID3D12Resource* frame)
{
    if (!frame) return;
    DXGI_COLOR_SPACE_TYPE cs = static_cast<DXGI_COLOR_SPACE_TYPE>(g_color_space.load(std::memory_order_acquire));
    na::nr::OnPresent(queue, frame, cs);      // live Neural Rendering (before capture and the menu)
    na::motion::OnPresent(queue, frame, cs);  // A52: live optical flow (after NR, before capture)
    if (na::capture::Pending()) na::capture::OnPresent(queue, frame); // phase-1 capture
}

// Common prelude of a game Present: bookkeeping, lazy init, timing. Returns
// false when nothing further should happen (stopping, no queue yet, init
// failed). Takes g_state.mutex on success (caller releases via the guard it
// passes in).
bool present_prelude(IDXGISwapChain* sc, ID3D12CommandQueue*& queue)
{
    if (g_stopping.load(std::memory_order_acquire)) return false;
    if (sc == g_state.dummy_swapchain_identity) return false;
    g_presents.fetch_add(1, std::memory_order_relaxed);
    queue = g_direct_queue.load(std::memory_order_acquire);
    if (!queue) return false;
    if (g_state.swapchain && g_state.swapchain != sc) {
        // A31 robustness: a game that recreates its swap chain (HDR toggle,
        // display-mode change) presents from a NEW object. If the one we
        // attached to has been silent for 2 s, re-target: release our
        // per-chain objects and initialise lazily on the new chain.
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (!g_state.last_present.QuadPart || !g_state.qpc_freq.QuadPart ||
            (now.QuadPart - g_state.last_present.QuadPart) < 2 * g_state.qpc_freq.QuadPart)
            return false;
        if (g_state.fence && g_state.fence_next > 1 && g_state.fence->GetCompletedValue() < g_state.fence_next - 1)
            return false; // our last draw on the old chain is still in flight; try next present
        release_buffers_locked();
        g_state.frames.clear();
        g_state.rtv_heap.Reset();
        if (g_state.imgui_ready) ImGui_ImplDX12_InvalidateDeviceObjects();
        g_state.swapchain = sc;
        g_state.last_present.QuadPart = 0;
        g_retargets.fetch_add(1, std::memory_order_relaxed);
    }
    if (!ensure_init_locked(sc)) return false;
    // Frame timing (smoothed) for the footer readout.
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_state.last_present.QuadPart && g_state.qpc_freq.QuadPart) {
        float ms = 1000.0f * static_cast<float>(now.QuadPart - g_state.last_present.QuadPart) /
                   static_cast<float>(g_state.qpc_freq.QuadPart);
        float prev = g_frame_ms.load();
        float sm = prev > 0 ? prev * 0.9f + ms * 0.1f : ms;
        g_frame_ms.store(sm);
        g_fps.store(sm > 0 ? 1000.0f / sm : 0.0f);
    }
    g_state.last_present = now;
    na::gputime::Poll(queue);
    // A63/A64 picture-rebuild nudge: started on the window thread; restored by
    // its timer. A post that cannot be delivered is a checked failure.
    na::present::RetryOrphans(); // A75: chains whose teardown timed out earlier
    if (g_state.hwnd) {
        nudge_tick();
        uint32_t want = g_nudge_requests.exchange(0, std::memory_order_acq_rel);
        if (want && !g_nudge_pending.exchange(1, std::memory_order_acq_rel)) {
            // A67: admission freezes the size and arms the confirmation BEFORE
            // the window is touched, so a creation the game performs from
            // inside the resize belongs to this request. A new request
            // supersedes any earlier confirmation (RequestPictureRebuild
            // refuses while a nudge is in flight, so nothing is merged).
            g_nudge_frozen_w = g_nudge_req_w.load(std::memory_order_acquire);
            g_nudge_frozen_h = g_nudge_req_h.load(std::memory_order_acquire);
            uint64_t id = (g_nudge_frozen_w && g_nudge_frozen_h)
                              ? na::ngx::ArmSizeConfirmation(g_nudge_frozen_w, g_nudge_frozen_h, kConfirmNeed, kConfirmCreateMs, kConfirmStreakMs, kConfirmOverallMs)
                              : 0;
            if (!id) na::ngx::CancelSizeConfirmation();
            g_nudge_confirm_id.store(id, std::memory_order_release);
            g_nudge_started_tick.store(GetTickCount64(), std::memory_order_release);
            g_nudge_state.store(1, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(g_nudge_reason_m);
                g_nudge_reason[0] = 0;
            }
            if (!PostMessageW(g_state.hwnd, kMsgNudge, 0, 0)) nudge_fail("the game window did not accept the request");
        }
    }
    // Evidence freshness: how many presents since DLSS last ran.
    {
        uint64_t ev = na::ngx::GetStats().evaluates;
        if (ev != g_last_evaluates) {
            g_last_evaluates = ev;
            g_presents_since_eval.store(0, std::memory_order_relaxed);
        } else if (ev) {
            g_presents_since_eval.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return true;
}

// Menu step (both paths): cursor show/restore messages, then the draw.
void menu_step(IDXGISwapChain* sc, ID3D12CommandQueue* queue, bool open, ID3D12Resource* frame = nullptr)
{
    bool just_opened = open && !g_state.was_open;
    bool just_closed = !open && g_state.was_open;
    g_state.was_open = open;
    if (just_opened && g_state.hwnd) PostMessageW(g_state.hwnd, kMsgCursorShow, 0, 0);
    if (just_closed && g_state.hwnd) PostMessageW(g_state.hwnd, kMsgCursorRestore, 0, 0);
    if (!open) return;
    render_locked(sc, queue, just_opened, frame);
}

// Old path (no presenter): everything on the real back buffer, then the
// original Present by the caller.
void on_present(IDXGISwapChain* sc)
{
    std::lock_guard<std::mutex> lock(g_state.mutex);
    ID3D12CommandQueue* queue = nullptr;
    if (!present_prelude(sc, queue)) return;
    UINT idx = real_index(sc);
    if (idx < g_state.frames.size()) {
        ComPtr<ID3D12Resource> bb; // A71: this frame only
        if (SUCCEEDED(real_get_buffer(sc, idx, IID_PPV_ARGS(&bb)))) process_frame(queue, bb.Get());
    }
    menu_step(sc, queue, g_open.load(std::memory_order_acquire));
}

// A59 presenter path: the game drew into its proxy; the presenter copies,
// generates and presents. Composite the menu on the proxy, after NR and
// before copying to the worker's owned frame. No real-buffer access here.
void presenter_frame_cb(ID3D12CommandQueue* queue, ID3D12Resource* frame, void*) { process_frame(queue, frame); }
void presenter_menu_cb(IDXGISwapChain* sc, ID3D12CommandQueue* queue, ID3D12Resource* frame, void* user)
{
    menu_step(sc, queue, *static_cast<const bool*>(user), frame);
}

HRESULT presenter_present(IDXGISwapChain* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp)
{
    std::lock_guard<std::mutex> lock(g_state.mutex);
    ID3D12CommandQueue* queue = nullptr;
    if (!present_prelude(sc, queue)) {
        // No queue or not initialised yet: the frame sits in a proxy; forward so
        // the chain keeps turning (a black frame at most, at start-up).
        return g_orig_present(sc, sync, flags);
    }
    na::present::PresentArgs a;
    g_present_caller.store(GetCurrentThreadId());
    g_present_queue_identity.store(reinterpret_cast<uintptr_t>(queue));
    a.sc = sc;
    a.sync = sync;
    a.flags = flags;
    a.pp = pp;
    a.queue = queue;
    a.hdr = na::color::IsHdr(na::color::Classify(g_state.format, static_cast<DXGI_COLOR_SPACE_TYPE>(g_color_space.load(std::memory_order_acquire)))); // A60
    bool open = g_open.load(std::memory_order_acquire);
    a.menu_open = open || g_state.was_open; // closing frame still processes cursor restoration
    a.frame = &presenter_frame_cb;
    a.menu = &presenter_menu_cb;
    // MenuFn runs on this recording thread before the owned frame copy. A
    // Home message after this snapshot is handled next frame.
    a.user = &open;
    HRESULT hr = na::present::Present(a);
    g_present_result.store(hr);
    g_present_returns.fetch_add(1);
    return hr;
}

HANDLE STDMETHODCALLTYPE DetourGetFrameLatencyWaitableObject(IDXGISwapChain2* sc)
{
    HANDLE handle = g_orig_get_latency_object(sc);
    g_game_latency_handle.store(reinterpret_cast<uintptr_t>(handle));
    return handle; // observe exactly what the game receives; never signal/replace/close it
}

HRESULT STDMETHODCALLTYPE DetourPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    if (na::present::IsPresentThread()) return g_orig_present(sc, sync, flags); // never re-entered by design; safety
    if (flags & DXGI_PRESENT_TEST) return g_orig_present(sc, sync, flags);      // A62: occlusion query, nothing is shown
    if (na::present::Proxied(sc)) return presenter_present(sc, sync, flags, nullptr);
    on_present(sc);
    return g_orig_present(sc, sync, flags);
}
HRESULT STDMETHODCALLTYPE DetourPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                         const DXGI_PRESENT_PARAMETERS* pp)
{
    if (na::present::IsPresentThread()) return g_orig_present1(sc, sync, flags, pp);
    if (flags & DXGI_PRESENT_TEST) return g_orig_present1(sc, sync, flags, pp);
    if (na::present::Proxied(sc)) return presenter_present(sc, sync, flags, pp);
    on_present(sc);
    return g_orig_present1(sc, sync, flags, pp);
}
HRESULT STDMETHODCALLTYPE DetourGetBuffer(IDXGISwapChain* sc, UINT index, REFIID riid, void** out)
{
    HRESULT hr = S_OK;
    if (sc != g_state.dummy_swapchain_identity && na::present::HandleGetBuffer(sc, index, riid, out, &hr)) return hr;
    return g_orig_get_buffer(sc, index, riid, out);
}
UINT STDMETHODCALLTYPE DetourGetCurrentBackBufferIndex(IDXGISwapChain3* sc)
{
    UINT idx = 0;
    if (na::present::HandleGetIndex(sc, &idx)) return idx;
    return g_orig_get_index(sc);
}
// A62: a game that paces itself with the frame-latency waitable object would
// be starved by the presenter's extra presents (each consumes a slot). With
// the presenter on, the real chain gets room for the generated pictures; the
// game is told the value it asked for.
HRESULT STDMETHODCALLTYPE DetourSetMaximumFrameLatency(IDXGISwapChain2* sc, UINT n)
{
    // A63: the game's value is applied unchanged; the presenter raises it only
    // while generating (game value + count) and lowers it back afterwards.
    // A76: never lower the real chain below what the presenter already
    // applied (lowering starved the game's waitable object: session 14 freeze).
    UINT apply = n;
    if (sc != g_state.dummy_swapchain_identity && na::present::Enabled()) {
        UINT floor_n = na::present::LatencyFloor(sc);
        if (floor_n > apply) apply = floor_n;
    }
    HRESULT hr = g_orig_set_max_latency(sc, apply);
    if (SUCCEEDED(hr) && sc != g_state.dummy_swapchain_identity && na::present::Enabled()) {
        g_game_max_latency.store(n, std::memory_order_relaxed);
        na::present::NoteGameLatency(sc, n);
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE DetourGetMaximumFrameLatency(IDXGISwapChain2* sc, UINT* out)
{
    HRESULT hr = g_orig_get_max_latency(sc, out);
    UINT asked = g_game_max_latency.load(std::memory_order_relaxed);
    if (SUCCEEDED(hr) && out && asked && sc != g_state.dummy_swapchain_identity && na::present::Enabled()) *out = asked;
    return hr;
}
HRESULT STDMETHODCALLTYPE DetourSetColorSpace1(IDXGISwapChain3* sc, DXGI_COLOR_SPACE_TYPE cs)
{
    HRESULT hr = g_orig_set_color_space(sc, cs);
    if (SUCCEEDED(hr) && sc != g_state.dummy_swapchain_identity) g_color_space.store(static_cast<int>(cs), std::memory_order_release);
    return hr;
}
// A71 (Astra B-007): a chain the game let go of. Forget it at once so the
// next Present (from a replacement chain on the same window) initialises
// immediately instead of after the 2 s silence rule.
void on_chain_gone(IDXGISwapChain* sc)
{
    g_chain_gone.fetch_add(1, std::memory_order_relaxed);
    na::present::ForgetChain(sc);
    std::lock_guard<std::mutex> lock(g_state.mutex);
    if (g_state.swapchain != sc) return;
    release_buffers_locked();
    g_state.frames.clear();
    g_state.rtv_heap.Reset();
    if (g_state.imgui_ready) ImGui_ImplDX12_InvalidateDeviceObjects();
    g_state.swapchain = nullptr;
    g_state.last_present.QuadPart = 0;
}
// IUnknown::Release on the game's swap chain. The count the ORIGINAL returns
// tells who is left: 0 = destroyed (we held nothing); HeldReferences(sc) = only
// the presenter's own reference remains, so it is dropped here and now, on
// the game's thread, and the game sees 0 as it should. Our own Release calls
// made inside that teardown carry t_in_our_release and pass straight through.
ULONG STDMETHODCALLTYPE DetourRelease(IUnknown* obj)
{
    if (t_in_our_release || na::present::InTeardownRelease() || !obj || obj == g_state.dummy_swapchain_identity) return g_orig_release(obj);
    auto* sc = static_cast<IDXGISwapChain*>(obj);
    ULONG n = g_orig_release(obj);
    if (n == 0) {
        on_chain_gone(sc);
        return 0;
    }
    uint32_t held = na::present::HeldReferences(sc);
    if (held && n == held) {
        t_in_our_release = true;
        bool released = na::present::ReleaseChain(sc);
        t_in_our_release = false;
        if (released) {
            on_chain_gone(sc);
            return 0;
        }
    }
    return n;
}
HRESULT STDMETHODCALLTYPE DetourResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w,
                                              UINT h, DXGI_FORMAT fmt, UINT flags)
{
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        if (g_state.swapchain == sc) release_buffers_locked();
    }
    na::present::BeforeResize(sc); // A59: drains generated presents, drops proxies and cached real buffers
    return g_orig_resize(sc, count, w, h, fmt, flags);
}
HRESULT STDMETHODCALLTYPE DetourResizeBuffers1(IDXGISwapChain3* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags,
                                               const UINT* node_masks, IUnknown* const* queues)
{
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        if (g_state.swapchain == sc) release_buffers_locked();
    }
    na::present::BeforeResize(sc);
    return g_orig_resize1(sc, count, w, h, fmt, flags, node_masks, queues);
}
void STDMETHODCALLTYPE ForwardWithFgInputs(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists)
{
    na::fg::Execute(q, n, lists, g_orig_execute);
}
void STDMETHODCALLTYPE DetourExecute(ID3D12CommandQueue* q, UINT n,
                                     ID3D12CommandList* const* lists)
{
    if(na::present::OwnsQueue(q)) { g_orig_execute(q,n,lists); return; }
    if (q == g_texture_queue.load(std::memory_order_acquire)) {
        na::nr::Execute(q, n, lists, ForwardWithFgInputs);
        return;
    }
    // Classify each queue pointer once; the hot path is a tiny linear scan.
    uint32_t nd = g_known_direct_n.load(std::memory_order_acquire);
    bool direct = false, known = false;
    for (uint32_t i = 0; i < nd; ++i)
        if (g_known_direct[i] == q) { direct = known = true; break; }
    if (!known) {
        uint32_t no = g_known_other_n.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < no; ++i)
            if (g_known_other[i] == q) { known = true; break; }
    }
    if (!known) {
        direct = q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (direct) {
            uint32_t i = g_known_direct_n.load();
            if (i < 8) { g_known_direct[i] = q; g_known_direct_n.store(i + 1); }
        } else {
            uint32_t i = g_known_other_n.load();
            if (i < 8) { g_known_other[i] = q; g_known_other_n.store(i + 1); }
        }
    }
    if (direct) g_direct_queue.store(q, std::memory_order_release);
    announce_pending(); // A49: device / first-seen direct queues, retried until delivered
    // A48 seam: variant coverage per list (lock-free), then the paired scope
    // around the ACTUAL forward; a receipt may only be published from `end`.
    using na::hooking::variants::Kind;
    uint32_t uncovered = 0;
    if (na::hooking::variants::Active(Kind::CommandList)) { // inert until an adapter registers slots
        for (UINT i = 0; i < n; ++i) {
            if (!lists[i]) continue;
            if (na::hooking::variants::NoteObject(Kind::CommandList, lists[i]) != na::hooking::variants::Coverage::Complete) {
                ++uncovered;
                seen_push(SeenKind::List, lists[i]);
            }
        }
    }
    g_exec_active.fetch_add(1, std::memory_order_acq_rel);
    ExecuteSubscriber* sub = g_exec_sub.load(std::memory_order_acquire);
    if (sub) {
        ExecuteContext ctx{q, n, lists, g_exec_ticket.fetch_add(1, std::memory_order_relaxed) + 1, uncovered, sub->user};
        if (sub->begin) sub->begin(&ctx);
        na::nr::Execute(q, n, lists, ForwardWithFgInputs);
        na::gputime::OnExecuted(q, n, lists); // R-202: completion fence on the queue that ran the lists
        if (sub->end) sub->end(&ctx);
        g_exec_scoped.fetch_add(1, std::memory_order_relaxed);
    } else {
        na::nr::Execute(q, n, lists, ForwardWithFgInputs);
        na::gputime::OnExecuted(q, n, lists); // R-202: completion fence on the queue that ran the lists
    }
    g_exec_active.fetch_sub(1, std::memory_order_acq_rel);
}

} // namespace

bool Start(const char* route_tag, PanelFn controls)
{
    g_route_tag = route_tag ? route_tag : "";
    g_controls = controls;
    g_stopping.store(false);

    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"NeuralAliasOverlayDummy";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 64, 64,
                              nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return false;
    bool ok = false;
    {
        ComPtr<ID3D12Device> dev;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<IDXGIFactory2> factory;
        ComPtr<IDXGISwapChain1> sc;
        D3D12_COMMAND_QUEUE_DESC qd{};
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.BufferCount = 2;
        sd.Width = 64;
        sd.Height = 64;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        // A76: tests set NEURALALIAS_TEST_WARP=1 so this vtable-reading device
        // is created on WARP (software renderer), never on the graphics card.
        // In a game the variable is absent and the default adapter is used.
        ComPtr<IDXGIAdapter> dummy_adapter;
        const bool want_warp = GetEnvironmentVariableW(L"NEURALALIAS_TEST_WARP", nullptr, 0) != 0;
        if (want_warp) {
            ComPtr<IDXGIFactory4> f4;
            if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f4)))) f4->EnumWarpAdapter(IID_PPV_ARGS(&dummy_adapter));
        }
        if ((!want_warp || dummy_adapter) &&
            SUCCEEDED(D3D12CreateDevice(dummy_adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))) &&
            SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) &&
            SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &sd, nullptr, nullptr, &sc))) {
            g_state.dummy_swapchain_identity = sc.Get();
            ok = na::hooking::HookVTableSlot(sc.Get(), kSwapReleaseSlot,
                                             reinterpret_cast<void*>(&DetourRelease),
                                             reinterpret_cast<void**>(&g_orig_release)) &&
                 na::hooking::HookVTableSlot(sc.Get(), kSwapPresentSlot,
                                             reinterpret_cast<void*>(&DetourPresent),
                                             reinterpret_cast<void**>(&g_orig_present)) &&
                 na::hooking::HookVTableSlot(sc.Get(), kSwap1Present1Slot,
                                             reinterpret_cast<void*>(&DetourPresent1),
                                             reinterpret_cast<void**>(&g_orig_present1)) &&
                 na::hooking::HookVTableSlot(sc.Get(), kSwapResizeBuffersSlot,
                                             reinterpret_cast<void*>(&DetourResizeBuffers),
                                             reinterpret_cast<void**>(&g_orig_resize)) &&
                 na::hooking::HookVTableSlot(queue.Get(), kQueueExecuteSlot,
                                             reinterpret_cast<void*>(&DetourExecute),
                                             reinterpret_cast<void**>(&g_orig_execute));
            // A59 presenter: GetBuffer / GetCurrentBackBufferIndex / ResizeBuffers1.
            ComPtr<IDXGISwapChain3> sc3;
            if (ok && SUCCEEDED(sc.As(&sc3))) {
                ok = na::hooking::HookVTableSlot(sc.Get(), kSwapGetBufferSlot,
                                                 reinterpret_cast<void*>(&DetourGetBuffer),
                                                 reinterpret_cast<void**>(&g_orig_get_buffer)) &&
                     na::hooking::HookVTableSlot(sc3.Get(), kSwap3GetIndexSlot,
                                                 reinterpret_cast<void*>(&DetourGetCurrentBackBufferIndex),
                                                 reinterpret_cast<void**>(&g_orig_get_index)) &&
                     na::hooking::HookVTableSlot(sc3.Get(), kSwap3ResizeBuffers1Slot,
                                                 reinterpret_cast<void*>(&DetourResizeBuffers1),
                                                 reinterpret_cast<void**>(&g_orig_resize1)) &&
                     na::hooking::HookVTableSlot(sc3.Get(), kSwap2SetMaxLatencySlot,
                                                 reinterpret_cast<void*>(&DetourSetMaximumFrameLatency),
                                                 reinterpret_cast<void**>(&g_orig_set_max_latency)) &&
                     na::hooking::HookVTableSlot(sc3.Get(), kSwap2GetMaxLatencySlot,
                                                 reinterpret_cast<void*>(&DetourGetMaximumFrameLatency),
                                                 reinterpret_cast<void**>(&g_orig_get_max_latency)) &&
                     na::hooking::HookVTableSlot(sc3.Get(), kSwap2GetLatencyObjectSlot,
                                                 reinterpret_cast<void*>(&DetourGetFrameLatencyWaitableObject),
                                                 reinterpret_cast<void**>(&g_orig_get_latency_object));
                // Colour space (HDR10 detection for Neural Rendering); optional.
                na::hooking::HookVTableSlot(sc3.Get(), kSwap3SetColorSpace1Slot,
                                            reinterpret_cast<void*>(&DetourSetColorSpace1),
                                            reinterpret_cast<void**>(&g_orig_set_color_space));
            }
            if (ok) {
                na::present::Originals o;
                o.present = g_orig_present;
                o.present1 = g_orig_present1;
                o.get_buffer = g_orig_get_buffer;
                o.get_index = g_orig_get_index;
                o.set_max_latency = g_orig_set_max_latency;
                o.get_latency_object = g_orig_get_latency_object;
                o.resize_queues = g_orig_resize1;
                o.execute = g_orig_execute;
                na::present::SetOriginals(o);
            }
        }
    }
    DestroyWindow(hwnd);
    g_state.dummy_swapchain_identity = nullptr;

    // Controller: hook whichever XInput modules the game already loaded.
    const wchar_t* mods[3] = {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
    void* detours[3] = {reinterpret_cast<void*>(&XInputDetour0),
                        reinterpret_cast<void*>(&XInputDetour1),
                        reinterpret_cast<void*>(&XInputDetour2)};
    uint32_t hooked = 0;
    for (int i = 0; i < 3; ++i) {
        HMODULE m = GetModuleHandleW(mods[i]);
        if (!m) continue;
        void* target = reinterpret_cast<void*>(GetProcAddress(m, "XInputGetState"));
        if (target && na::hooking::HookFunction(target, detours[i],
                                                reinterpret_cast<void**>(&g_orig_xinput[i])))
            ++hooked;
    }
    g_gamepad_hooked.store(hooked);
    install_input_hooks(); // A44: the game sees no mouse/keyboard while the menu is open
    if (!na::hooking::variants::StartDiscoveryThread(&seen_drain, nullptr)) return false; // A49: propagated
    return ok;
}

bool SubscribeExecute(ExecuteBeginFn begin, ExecuteEndFn end, void* user)
{
    if (g_exec_sub.load(std::memory_order_acquire)) return false;
    if (g_exec_active.load(std::memory_order_acquire)) return false; // A50: storage still owned by an in-flight callback
    g_exec_sub_storage = {begin, end, user};
    g_exec_sub.store(&g_exec_sub_storage, std::memory_order_release);
    return true;
}
// A49/A50: closes admission, then waits (bounded, on the calling control
// thread) for callbacks already in flight. Returns false and RETAINS the
// subscriber storage when a callback outlasts the timeout: the caller must
// not destroy adapter state or re-subscribe until a later call returns true.
bool UnsubscribeExecute(uint32_t timeout_ms)
{
    g_exec_sub.store(nullptr, std::memory_order_release);
    ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (g_exec_active.load(std::memory_order_acquire)) {
        if (GetTickCount64() >= deadline) { g_unsubscribe_timeouts.fetch_add(1, std::memory_order_relaxed); return false; }
        Sleep(1);
    }
    return true;
}

bool SubscribeDiscovery(DeviceSeenFn device, QueueSeenFn queue, ListSeenFn list, void* user)
{
    if (g_disc_sub.load(std::memory_order_acquire)) return false;
    g_disc_sub_storage = {device, queue, list, user};
    g_disc_sub.store(&g_disc_sub_storage, std::memory_order_release);
    announce_pending(); // a device/queue seen before the subscription is announced now
    return true;
}
void UnsubscribeDiscovery()
{
    g_disc_sub.store(nullptr, std::memory_order_release);
    seen_drain(nullptr); // release anything still retained (quiescent: tests only)
    g_device_announced.store(false, std::memory_order_release);
    g_queue_announced_mask.store(0, std::memory_order_release);
}
SeamStats GetSeamStats()
{
    SeamStats s{};
    s.executes_scoped = g_exec_scoped.load(std::memory_order_relaxed);
    s.discovery_queued = g_seen_queued.load(std::memory_order_relaxed);
    s.discovery_delivered = g_seen_delivered.load(std::memory_order_relaxed);
    s.discovery_overflow = g_seen_overflow.load(std::memory_order_relaxed);
    s.execute_active = g_exec_active.load(std::memory_order_acquire);
    s.unsubscribe_timeouts = g_unsubscribe_timeouts.load(std::memory_order_relaxed);
    s.stop_incomplete = g_stop_incomplete.load(std::memory_order_acquire);
    return s;
}

bool Stop()
{
    g_stopping.store(true, std::memory_order_release);
    g_open.store(false, std::memory_order_release);
    // A50/A51: quiescence first. A failed callback drain or discovery stop
    // is recorded (SeamStats.stop_incomplete) and Stop RETURNS false before
    // any teardown (hooks, ImGui, resources stay); a later Stop retries and
    // clears the flag on success.
    if (!UnsubscribeExecute(5000)) {
        g_stop_incomplete.store(1, std::memory_order_release);
        return false;
    }
    if (!na::hooking::variants::StopDiscoveryThread(5000)) {
        g_disc_sub.store(nullptr, std::memory_order_release); // stop deliveries, keep retained objects
        g_stop_incomplete.store(1, std::memory_order_release);
        return false;
    }
    UnsubscribeDiscovery(); // only once the worker has provably exited
    if (!na::present::Shutdown(5000)) { // A59: the present thread must be gone before resources go
        g_stop_incomplete.store(1, std::memory_order_release);
        return false;
    }
    g_stop_incomplete.store(0, std::memory_order_release);
    remove_input_hooks();
    std::lock_guard<std::mutex> lock(g_state.mutex);
    if (g_state.imgui_ready) {
        if (g_state.fence && g_state.fence_next > 1) {
            UINT64 last = g_state.fence_next - 1;
            if (g_state.fence->GetCompletedValue() < last) {
                g_state.fence->SetEventOnCompletion(last, g_state.fence_event);
                WaitForSingleObject(g_state.fence_event, 2000);
            }
        }
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        if (g_state.hwnd && g_state.orig_wndproc)
            SetWindowLongPtrW(g_state.hwnd, GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(g_state.orig_wndproc));
        g_state.imgui_ready = false;
    }
    g_state.frames.clear();
    g_state.list.Reset();
    g_state.rtv_heap.Reset();
    g_state.srv_heap.Reset();
    g_state.fence.Reset();
    if (g_state.fence_event) CloseHandle(g_state.fence_event);
    g_state.fence_event = nullptr;
    g_texture_queue.store(nullptr, std::memory_order_release);
    g_state.texture_queue.Reset();
    g_state.device.Reset();
    g_state.swapchain = nullptr;
    g_state.buffers_ready = false;
    g_state.was_open = false;
    g_initialized.store(0, std::memory_order_release);
    g_stopping.store(false, std::memory_order_release); // a later Start may run again
    return true;
}

Stats GetStats()
{
    Stats s{};
    s.frames_rendered = g_frames.load();
    s.presents_seen = g_presents.load();
    s.initialized = g_initialized.load();
    s.open = g_open.load() ? 1u : 0u;
    s.have_queue = g_direct_queue.load() ? 1u : 0u;
    s.gamepad_hooked = g_gamepad_hooked.load();
    s.fps = g_fps.load();
    s.frame_ms = g_frame_ms.load();
    s.mouse_x = g_mouse_x.load();
    s.mouse_y = g_mouse_y.load();
    s.input_hooks = g_input_hooks.load(std::memory_order_acquire);
    s.retargets = g_retargets.load(std::memory_order_relaxed);
    s.chain_gone = g_chain_gone.load(std::memory_order_relaxed);
    return s;
}

void SetOpen(bool open) { g_open.store(open, std::memory_order_release); }

bool RequestPictureRebuild(uint32_t render_w, uint32_t render_h)
{
    nudge_tick();
    // A67: refuse while a nudge is in flight (posted or window shrunk) or
    // while an earlier request has not been admitted yet; nothing about the
    // running request is touched.
    uint32_t st = g_nudge_state.load(std::memory_order_acquire);
    if (st == 1 || st == 2 || g_nudge_pending.load(std::memory_order_acquire) || g_nudge_requests.load(std::memory_order_acquire)) {
        g_nudge_rejected.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    g_nudge_req_w.store(render_w, std::memory_order_release);
    g_nudge_req_h.store(render_h, std::memory_order_release);
    g_nudge_requests.fetch_add(1, std::memory_order_acq_rel);
    return true;
}
PictureRebuild GetPictureRebuild()
{
    nudge_tick();
    PictureRebuild r{};
    r.state = g_nudge_state.load(std::memory_order_acquire);
    r.attempts = g_nudge_attempts.load(std::memory_order_relaxed);
    r.failures = g_nudge_failures.load(std::memory_order_relaxed);
    r.restore_retries = g_nudge_retries.load(std::memory_order_relaxed);
    r.restored_tick = g_nudge_restored_tick.load(std::memory_order_acquire);
    r.confirm_id = g_nudge_confirm_id.load(std::memory_order_acquire);
    std::lock_guard<std::mutex> lock(g_nudge_reason_m);
    strncpy_s(r.reason, g_nudge_reason, _TRUNCATE);
    return r;
}

uint64_t PresentsSinceLastEvaluate() { return g_presents_since_eval.load(std::memory_order_relaxed); }

int CurrentPageIndex() { return static_cast<int>(na::menu::CurrentPage()) - 1; }

void SetStatusLogPath(const wchar_t* path)
{
    if (path) lstrcpynW(g_status_log, path, MAX_PATH);
    else g_status_log[0] = 0;
    g_status_last_tick = 0;
}

int SwapChainColorSpace() { return g_color_space.load(std::memory_order_acquire); }

} // namespace na::overlay
