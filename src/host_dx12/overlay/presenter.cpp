#include "host_dx12/overlay/presenter.hpp"
#include "host_dx12/diag/dred.hpp"

#include <windows.h>
#include <timeapi.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

#include "host_dx12/color/color_space.hpp"
#include "host_dx12/fg/frame_gen.hpp"
#include "host_dx12/fg/hud_paste.hpp"
#include "host_dx12/fg/hud_move.hpp"
#include "host_dx12/fg/steady_pass.hpp"
#include "host_dx12/capture/capture_session.hpp"
#include "host_dx12/settings.hpp"
#include "core/live_frame_gen.hpp"
#include "host_dx12/diag/event_log.hpp"
#include "host_dx12/diag/vram_ledger.hpp"

#include <string>

#include <dxgi1_4.h>
#include <thread>
#include "host_dx12/native_present/size_spoof.hpp"
#include "host_dx12/native_present/size_controller.hpp"
#include "host_dx12/latency/nvidia_latency.hpp"
#include "host_dx12/adapters/tlou1/timing.hpp"

#pragma comment(lib, "winmm.lib")

using Microsoft::WRL::ComPtr;

namespace na::present {
namespace {

constexpr uint32_t kSlots = 3;        // real frames in flight through the presenter
constexpr uint32_t kMaxGenerated = 5; // 6x
constexpr uint32_t kPresentsPerSlot = kMaxGenerated + 1;
// Per slot: [0] start, [1] end of the generate list, [2..] each present, then the live path's
// split of the generate list: after the real-frame copy, after the HUD-free copy, after evaluate,
// after the still-area steadying.
constexpr uint32_t kLiveQuery = 2 + kPresentsPerSlot;
constexpr uint32_t kQueriesPerSlot = kLiveQuery + 4;
constexpr uint32_t kJobs = 4;
constexpr uint32_t kChains = 16; // eight proxy/base pairs; GPU objects remain lazy

Originals g_orig;
std::atomic<bool> g_enabled{false};
std::atomic<uint64_t> g_presents_game{0}, g_presents_real{0}, g_generated_shown{0}, g_dropped{0}, g_late{0}, g_vetoed{0};
std::atomic<uint64_t> g_generated_dropped{0}; // A71: generated pictures not shown (a whole step late, or the present queue was full)
std::atomic<float> g_display_hz_override{0.f}; // A71: tests only (SetDisplayHzForTest)
std::atomic<uint32_t> g_chain_releases{0};    // A71: chains torn down because the game released its last reference
std::atomic<uint32_t> g_orphaned{0};          // A75: teardowns that timed out (chain kept alive by our reference; retried later)
thread_local bool t_teardown_release = false; // A75: our own Release of a chain object is in progress on this thread
std::atomic<float> g_base_fps{0.f}, g_shown_fps{0.f}, g_display_hz{0.f}, g_fg_ms{0.f}, g_fg_avg_ms{0.f};
std::atomic<uint32_t> g_thread_mode{0}, g_proxied{0};
std::atomic<DWORD> g_real_present_caller{0};
std::atomic<DWORD> g_present_thread_id{0};
std::atomic<HANDLE> g_test_worker_entered{nullptr}, g_test_worker_resume{nullptr};
char g_reason[112] = "Presenter is idle.";
std::mutex g_reason_m;
std::mutex g_metrics_m;
na::latency::Samples g_intervals, g_present_calls, g_ready_ages, g_real_ages;
na::latency::Samples g_gpu_first_copy, g_gpu_copy_interval;
std::atomic<uint32_t> g_requested_sync{0}, g_requested_flags{0};
// Render-small (SetRenderScale).
std::atomic<float> g_render_scale{1.f};
std::atomic<UpscaleFn> g_upscale{nullptr};
// [present] present_dlaa=1: at full size (DLAA) the presenter still takes the game's frame and
// J2 runs DLAA at Present, exactly like the render-small path but with render == output. For
// comparing Present-time DLAA with in-frame DLAA on the same game (Astra, COMMS cc47232).
std::atomic<bool> g_present_dlaa{false};
std::atomic<void*> g_upscale_user{nullptr};
std::atomic<uint64_t> g_upscaled{0}, g_upscale_failed{0};
std::atomic<uint32_t> g_scaled_w{0}, g_scaled_h{0}, g_native_w{0}, g_native_h{0};
// Route B live Frame Generation (SetLiveFrameGeneration).
std::atomic<bool> g_live{false};
std::atomic<HudFreeFn> g_hudfree_fn{nullptr};
na::fg::HudPaste g_paste; // compiled by the preparation thread, used on the game's thread afterwards
std::atomic<bool> g_paste_ready{false};
std::atomic<uint64_t> g_live_inline{0}, g_live_worker{0}, g_skip_hudfree{0}, g_skip_menu{0}, g_skip_preparing{0};
struct Timing { std::atomic<float> last{0.f}, avg{0.f};
    void add(float ms) { last.store(ms); const float a = avg.load(); avg.store(a > 0.f ? a * 0.95f + ms * 0.05f : ms); } };
Timing g_t_off, g_t_copy, g_t_hudcopy, g_t_eval, g_t_paste, g_t_steady;
// Still-area steadying (Astra): compiled by the preparation thread with the paste; refused with a reason.
na::fg::SteadyPass g_steady;
std::atomic<bool> g_steady_ready{false};
na::fg::HudMove g_move; // hudpaste/moves-with-scene; used on the game's thread after the preparation thread made it
std::atomic<bool> g_move_ready{false};
char g_move_refused[160] = "";
char g_steady_refused[112] = "";
std::atomic<uint64_t> g_steady_frames{0}, g_steady_off{0}, g_steady_changed{0}, g_steady_counted{0};
std::atomic<uint64_t> g_drop_gap{0}, g_drop_reset{0}, g_drop_motion{0}, g_drop_relearn{0}, g_drop_resize{0}, g_drop_refused{0};
std::atomic<uint64_t> g_hud_epoch{0};
std::atomic<uint64_t> g_skip_display{0};
std::atomic<float> g_display_reported{0.f};
char g_display_dynamic[64] = "";
// Presented-picture capture (F8, x2 on).
std::atomic<uint32_t> g_cap_request{0};
char g_cap_text[220] = "";
std::atomic<uint64_t> g_vram_budget_mb{0}, g_vram_usage_mb{0}, g_extra_mb{0};
constexpr ULONGLONG kLiveReleaseAfterMs = 3000; // x2 Off this long: its textures and feature are given back
std::atomic<uint32_t> g_inline_owner{1}, g_live_ready{0}, g_live_preparing{0};
std::mutex g_live_text_m;
char g_prepare_text[112] = "";
char g_hudfree_text[112] = "";
void set_live_text(char* dst, const char* text, size_t n = 112) { std::lock_guard<std::mutex> lock(g_live_text_m); strncpy_s(dst, n, text ? text : "", _TRUNCATE); }
// x2 chosen since / last generated picture recorded (GetTickCount64): "on but waiting" after a second.
std::atomic<uint64_t> g_fg_on_since{0}, g_last_generated_tick{0};
// Every live link logs its first success and its first failure, once each.
void live_first(const char* link, bool ok, const char* detail)
{
    static std::mutex m;
    static std::vector<std::string> seen;
    const std::string key = std::string(ok ? "+" : "-") + link;
    {
        std::lock_guard<std::mutex> lock(m);
        for (const auto& s : seen) if (s == key) return;
        if (seen.size() < 64) seen.push_back(key);
    }
    na::eventlog::Emit("fg_live", "FIRST %s %s%s%s", ok ? "SUCCESS" : "FAILURE", link, detail && *detail ? ": " : "", detail ? detail : "");
}

void set_reason(const char* t)
{
    std::lock_guard<std::mutex> lock(g_reason_m);
    strncpy_s(g_reason, t, _TRUNCATE);
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER br{};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    br.Transition.StateBefore = a;
    br.Transition.StateAfter = b;
    return br;
}

struct Slot {
    // An old GPU fence can be complete while a NEW job still owns this slot.
    // Ownership lasts from admission until the worker publishes its final fence.
    std::atomic<bool> owned{false};
    ComPtr<ID3D12CommandAllocator> alloc_gen;
    ComPtr<ID3D12CommandAllocator> alloc_present[kPresentsPerSlot];
    ComPtr<ID3D12Resource> real_copy;              // NPSR at rest
    ComPtr<ID3D12Resource> interp[kMaxGenerated];  // UAV at rest
    ComPtr<ID3D12Resource> veto;                   // A62: 16-byte UAV buffer the runtime writes (disable interpolation)
    ComPtr<ID3D12Resource> veto_readback;          // A62: its readback copy, read by the present thread
    std::atomic<UINT64> free_fence{0};             // slot reusable once the fence passed this (thread writes, hook reads)
    std::atomic<UINT64> gen_fence{0};              // generate list done (timestamps and veto readable)
    uint32_t copied_mask = 0; // worker writes; read only after ownership and free fence retire
    // Live Frame Generation: this frame's HUD-free picture (NPSR at rest), what the generate list
    // measured (the split timestamps are valid), and whether the Off path's copy was timed.
    ComPtr<ID3D12Resource> hudfree;
    bool live_split = false, hudfree_used = false;
    bool off_timed = false;
    // Still-area steadying ran on this slot's generated picture (its counter page is read once the
    // generate fence passed).
    bool steady_ran = false;
    uint32_t steady_page = 0, steady_pixels = 0;
};

// Presented-picture capture (F8): readback buffers for each captured present, filled on the
// presentation queue right after the copy into the back buffer; written to disk by its own thread
// once its fence passed.
struct CaptureShot {
    ComPtr<ID3D12Resource> buffer;   // READBACK, footprint of the presented texture
    const char* kind = "";           // previous-real / generated / next-real
    uint64_t frame_id = 0, engine_frame = 0;
    HRESULT hr = S_OK;
    bool presented = false, copied = false;
    LONGLONG qpc = 0;
};
struct Capture {
    std::atomic<uint32_t> state{0};  // 0 idle, 1 buffers being made, 2 ready (waits for a job with x2), 3 running
    uint32_t triples = 0, jobs_seen = 0, used = 0;
    std::vector<CaptureShot> shots;
    ComPtr<ID3D12Fence> fence;
    UINT64 fence_value = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT w = 0, h = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::thread maker;
    std::atomic<bool> made{false}, failed{false};
};

struct Job {
    uint32_t slot = 0;
    uint32_t count = 0;
    uint64_t engine_frame = 0;
    UINT sync = 0;
    UINT flags = 0;
    LONGLONG arrival = 0;
    float frame_ms = 0;
    uint64_t frame_id = 0;
    ID3D12CommandQueue* queue = nullptr; // the chain holds a reference
};

struct Chain {
    std::mutex m;                       // hook-side state
    IDXGISwapChain* sc = nullptr;
    ComPtr<IDXGISwapChain3> sc3;
    DXGI_SWAP_CHAIN_DESC desc{};
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;   // the game's direct queue (from Present)
    ComPtr<ID3D12CommandQueue> presentation_queue;
    ComPtr<ID3D12Fence> presentation_fence;
    UINT64 presentation_next=1;
    std::atomic<UINT64> presentation_last{0};
    bool queues_bound=false;
    std::vector<ComPtr<ID3D12Resource>> proxies;
    std::atomic<bool> external_owner{false};
    std::mutex buffers_m; // never held across callbacks or while acquiring m
    std::vector<ComPtr<ID3D12Resource>> resize_proxies;
    UINT resize_index=0;
    bool resize_scaled=false; UINT resize_pw=0, resize_ph=0; // the saved proxies' size (a failed resize restores them)
    std::atomic<UINT> virtual_index{0},proxy_count{0};
    ComPtr<ID3D12GraphicsCommandList> list_gen, list_present;
    Slot slots[kSlots];
    ComPtr<ID3D12Fence> fence;
    UINT64 fence_next = 1;
    std::mutex signal_mutex;
    std::atomic<UINT64> last_signal{0};
    HANDLE fence_event = nullptr;
    ComPtr<ID3D12QueryHeap> queries;
    ComPtr<ID3D12Resource> query_readback;
    bool ready = false;
    bool worker_owner = false; // once FG starts, real presentation stays on this worker (live: until it is off and drained)
    // Live Frame Generation: textures prepared off the game's thread; Off-path copy timing.
    std::atomic<bool> live_ready{false};
    std::thread live_prep;
    ULONGLONG live_retry_after = 0;           // a refused preparation (memory budget) is asked again after this tick
    ULONGLONG live_off_since = 0;             // x2 turned Off at this tick with its textures still held (0: not)
    ComPtr<ID3D12QueryHeap> off_queries;      // 2 per slot: before / after the Off path's copy (presentation queue)
    ComPtr<ID3D12Resource> off_readback;
    HANDLE queried_latency_handle = nullptr; // diagnostics: query only, never wait/signal
    bool uav_ok = false;
    bool scaled = false;  // render-small: proxies are pw x ph, the real chain desc size
    UINT pw = 0, ph = 0;  // proxy size (== desc size when not scaled)
    uint64_t frame_id = 0;
    LARGE_INTEGER last_arrival{}, last_shown{}, qpc{};
    float frame_ms = 0.f, shown_ms = 0.f;
    float avail_ms = 0.f;               // A73: smoothed time from the game's present to its pictures being ready (present thread only)
    float render_ms = 0.f;              // A74 review: the game's own interval EXCLUDING time it spent blocked inside our Present (Frame Generation's base rate)
    float last_blocked_ms = 0.f;        // time the previous Present spent waiting on our slot ring / job queue
    bool orphaned = false;              // A75: the game let go but our teardown timed out; retried from later presents / Shutdown
    uint32_t hitch_skips = 0;
    float display_hz = 0.f;                // x2's ceiling: the display's maximum refresh (else the reported rate)
    float display_reported_hz = 0.f, display_max_hz = 0.f;
    // Still-area steadying: the frame whose HUD-free picture the previous slot holds (UINT64_MAX none),
    // the HUD epoch it was made in, and a resize since (the slot textures were made again).
    uint64_t steady_prev_id = UINT64_MAX, steady_prev_epoch = 0;
    bool steady_resized = false;
    Capture cap;
    std::atomic<UINT> game_latency{0};  // A63: what the game asked for (0 = never asked)
    UINT latency_applied = 0;           // what the real chain currently has from us
    std::atomic<HRESULT> last_hr{S_OK};
    // Present thread.
    std::mutex jm;
    std::condition_variable jcv;
    Job jobs[kJobs];
    uint32_t job_w = 0, job_r = 0;
    uint32_t backlog = 0;               // enqueued and not yet fully presented
    bool stop = false;
    HANDLE thread = nullptr;
};
Chain g_chains[kChains];
std::atomic<ID3D12CommandQueue*> g_private_queues[kChains]{};
std::mutex g_chains_m;

float refresh_hz_for(HWND hwnd, float* max_hz = nullptr)
{
    if (max_hz) *max_hz = 0.f;
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!mon || !GetMonitorInfoW(mon, &mi)) return 0.f;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) return 0.f;
    if (max_hz) { // the highest refresh the display offers at its current size: x2's ceiling
        DWORD best = dm.dmDisplayFrequency;
        DEVMODEW m{};
        m.dmSize = sizeof(m);
        for (DWORD i = 0; EnumDisplaySettingsW(mi.szDevice, i, &m); ++i)
            if (m.dmPelsWidth == dm.dmPelsWidth && m.dmPelsHeight == dm.dmPelsHeight && m.dmDisplayFrequency > best && m.dmDisplayFrequency < 1000)
                best = m.dmDisplayFrequency;
        *max_hz = static_cast<float>(best);
    }
    return static_cast<float>(dm.dmDisplayFrequency);
}

Chain* find_chain(IDXGISwapChain* sc)
{
    for (Chain& c : g_chains)
        if (c.sc == sc) return &c;
    return nullptr;
}

bool create_texture(Chain& c, UINT w, UINT h, DXGI_FORMAT fmt, bool uav, bool rt, D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& out)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    if (uav) d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (rt) d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    out.Reset();
    return SUCCEEDED(c.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&out)));
}

bool uav_supported(Chain& c, DXGI_FORMAT fmt)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{};
    fs.Format = fmt;
    if (FAILED(c.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs)))) return false;
    return (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
}

// Proxies: created at the game's first GetBuffer (or after a resize).
bool ensure_proxies_locked(Chain& c)
{
    std::lock_guard<std::mutex> buffers_lock(c.buffers_m);
    if (!c.proxies.empty()) return true;
    if (FAILED(c.sc->GetDesc(&c.desc))) return false;
    if (!c.device && FAILED(c.sc->GetDevice(IID_PPV_ARGS(&c.device)))) return false;
    UINT n = c.desc.BufferCount ? c.desc.BufferCount : 2;
    c.uav_ok = uav_supported(c, c.desc.BufferDesc.Format);
    {
        const UINT W = c.desc.BufferDesc.Width, H = c.desc.BufferDesc.Height;
        // Render-small: the proxies are the size the game is TOLD (size_spoof's advertisement:
        // the delivered request while one is pending, else the committed size), never a size
        // derived from the applied mode, so a game that has not rebuilt keeps buffers that fit
        // what it draws (COMMS a066d76 point 1). Native boot advertises native.
        UINT aw = W, ah = H;
        if (g_upscale.load(std::memory_order_acquire)) {
            const na::native_present::Size adv = na::native_present::AdvertisedSize();
            if (adv.w && adv.h && adv.w <= W && adv.h <= H) { aw = adv.w; ah = adv.h; }
        }
        const bool reduced = aw < W || ah < H;
        const bool full = !reduced && g_present_dlaa.load(std::memory_order_acquire); // DLAA at Present
        c.scaled = (reduced || full) && c.uav_ok && g_upscale.load(std::memory_order_acquire) && W > 1 && H > 1;
        c.pw = c.scaled && reduced ? aw : W;
        c.ph = c.scaled && reduced ? ah : H;
        g_scaled_w.store(c.scaled ? c.pw : 0); g_scaled_h.store(c.scaled ? c.ph : 0);
        g_native_w.store(W); g_native_h.store(H);
        if (reduced && !c.scaled)
            set_reason(!c.uav_ok ? "Render-small is off for this picture format (no shader writes): full size."
                                 : "Render-small is off: no upscaler is attached.");
    }
    c.proxies.resize(n);
    for (UINT i = 0; i < n; ++i) {
        if (!create_texture(c, c.pw, c.ph, c.desc.BufferDesc.Format, c.uav_ok, true,
                            D3D12_RESOURCE_STATE_PRESENT, c.proxies[i])) {
            c.proxy_count.store(0,std::memory_order_release);
            c.proxies.clear();
            return false;
        }
        wchar_t name[48];
        swprintf_s(name, L"NeuralAlias proxy back buffer %u", i);
        c.proxies[i]->SetName(name);
    }
    c.virtual_index = 0;
    if (g_upscale.load(std::memory_order_acquire)) { // render-small: what the game is told follows the proxies it got
        namespace np = na::native_present;
        const np::Size out{c.desc.BufferDesc.Width, c.desc.BufferDesc.Height}, act{c.pw, c.ph};
        const np::SizeState st = np::GetSizeState();
        // A delivered live request completes only when the controller has seen the scene follow
        // (overlay small_follow_mode); here the controller only learns the real allocation.
        // A REAL window resize (the user or the OS changed the window) is the new output itself: it
        // completes here, at the allocation, as before the controller existed.
        const bool ours = st.enabled && st.hwnd == c.desc.OutputWindow && st.request && st.request_delivered;
        if (ours && st.request_window) {
            if (!np::CompleteResize(st.request, true, act)) np::CommitBuffers(c.desc.OutputWindow, out, act);
        } else if (!ours) np::CommitBuffers(c.desc.OutputWindow, out, act);
        np::ctl::Allocated({act.w, act.h}, GetTickCount64());
    }
    // Display (Cort 2026-11-26: dynamic refresh raises the rate in game; the desktop reads 60 Hz). The
    // ceiling is the display's maximum refresh, never the reported desktop mode. Windows does not tell a
    // program whether its dynamic refresh is on, so that is logged as not readable.
    c.display_reported_hz = refresh_hz_for(c.desc.OutputWindow, &c.display_max_hz);
    c.display_hz = c.display_max_hz > 0.f ? c.display_max_hz : c.display_reported_hz;
    g_display_hz.store(c.display_hz);
    g_display_reported.store(c.display_reported_hz);
    set_live_text(g_display_dynamic, "not readable (Windows does not report it to programs)", sizeof g_display_dynamic);
    na::eventlog::Emit("display", "refresh reported %.0f Hz, maximum %.0f Hz (x2's ceiling), dynamic refresh: not readable", c.display_reported_hz, c.display_max_hz);
    c.proxy_count.store(n,std::memory_order_release);
    g_proxied.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// Returns true when the fence has passed `value` (false = timed out).
bool wait_fence(Chain& c, UINT64 value, DWORD timeout_ms)
{
    if (!c.fence || !value) return true;
    if (value == UINT64_MAX || c.fence->GetCompletedValue() == UINT64_MAX) return false;
    if (c.fence->GetCompletedValue() >= value) return true;
    if (FAILED(c.fence->SetEventOnCompletion(value, c.fence_event))) return false;
    WaitForSingleObject(c.fence_event, timeout_ms);
    UINT64 done = c.fence->GetCompletedValue();
    return done != UINT64_MAX && done >= value;
}

bool wait_presentation(Chain& c,UINT64 value,DWORD timeout_ms) {
    if(!value) return true;
    auto f=c.presentation_fence.Get();
    if(!f || value==UINT64_MAX || f->GetCompletedValue()==UINT64_MAX) return false;
    if(f->GetCompletedValue()>=value) return true;
    if(FAILED(f->SetEventOnCompletion(value,c.fence_event))) return false;
    WaitForSingleObject(c.fence_event,timeout_ms);
    return f->GetCompletedValue()!=UINT64_MAX && f->GetCompletedValue()>=value;
}
ID3D12CommandQueue* display_queue(Chain& c) {
    return c.queues_bound ? c.presentation_queue.Get() : c.queue.Get();
}
// Allocation AND queue submission share one order; atomic fetch_add alone can
// still submit value N+1 before N and rewind a D3D12 fence.
UINT64 signal_progress(Chain& c, ID3D12CommandQueue* queue)
{
    std::lock_guard<std::mutex> lock(c.signal_mutex);
    UINT64 value = c.fence_next++;
    HRESULT hr = queue->Signal(c.fence.Get(), value);
    if (FAILED(hr)) { c.last_hr.store(hr); return UINT64_MAX; }
    c.last_signal.store(value, std::memory_order_release);
    return value;
}

void release_slot(Chain& c, Slot& s, ID3D12CommandQueue* queue)
{
    { std::lock_guard<std::mutex> lock(c.signal_mutex);
      const UINT64 value=c.presentation_next++;
      HRESULT hr=queue->Signal(c.presentation_fence.Get(),value);
      if(FAILED(hr)) c.last_hr.store(hr);
      s.free_fence.store(FAILED(hr)?UINT64_MAX:value,std::memory_order_release);
      c.presentation_last.store(FAILED(hr)?UINT64_MAX:value); }
    { std::lock_guard<std::mutex> lock(c.jm); s.owned.store(false, std::memory_order_release); }
    c.jcv.notify_all();
}

DWORD WINAPI present_thread(LPVOID p);

// Everything beyond the proxies: real buffers, lists, per-slot textures,
// fence, thread. Lazily at the first Present, again after a resize.
bool ensure_ready_locked(Chain& c)
{
    if (c.ready) return true;
    if (!ensure_proxies_locked(c)) return false;
    if (!c.sc3 && FAILED(c.sc->QueryInterface(IID_PPV_ARGS(&c.sc3)))) return false;
    if (!c.queried_latency_handle && g_orig.get_latency_object && (c.desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))
        c.queried_latency_handle = g_orig.get_latency_object(c.sc3.Get());
    if (!g_orig.get_buffer || !g_orig.get_index || !g_orig.present) return false;
    if(!c.external_owner && !c.queues_bound && g_orig.resize_queues && g_orig.execute) {
        if(!c.presentation_queue) {
            D3D12_COMMAND_QUEUE_DESC q{}; q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
            if(FAILED(c.device->CreateCommandQueue(&q,IID_PPV_ARGS(&c.presentation_queue)))) return false;
            c.presentation_queue->SetName(L"NeuralAlias presentation");
            g_private_queues[&c-g_chains].store(c.presentation_queue.Get(),std::memory_order_release);
        }
        // Only real buffers change queue ownership. The game holds proxies.
        // Setup/resize runs with no worker jobs or retained real buffers.
        std::vector<UINT> masks(c.desc.BufferCount,1);
        std::vector<IUnknown*> queues(c.desc.BufferCount,c.presentation_queue.Get());
        const HRESULT hr=g_orig.resize_queues(c.sc3.Get(),c.desc.BufferCount,c.desc.BufferDesc.Width,c.desc.BufferDesc.Height,
            c.desc.BufferDesc.Format,c.desc.Flags,masks.data(),queues.data());
        c.queues_bound=SUCCEEDED(hr);
    }
    // A71 (Astra B-007): the real back buffers are NOT retained. DXGI keeps a
    // swap chain alive while any of its buffers is referenced, so a cached
    // buffer made the game's last Release leave the chain in place and the
    // next CreateSwapChainForHwnd on the same window fail (E_ACCESSDENIED).
    // present_texture fetches the current buffer for the duration of one copy.
    // The presenter's only reference on the chain object is `sc3` (exactly
    // one), which HeldReferences() reports so the Release detour can tell
    // "only NeuralAlias is left" apart from "the game still uses it".
    if (!c.fence) {
        if (FAILED(c.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&c.fence)))) return false;
        if(FAILED(c.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&c.presentation_fence)))) return false;
        c.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        for (Slot& s : c.slots) {
            if (FAILED(c.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.alloc_gen)))) return false;
            for (auto& a : s.alloc_present)
                if (FAILED(c.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) return false;
        }
        if (FAILED(c.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, c.slots[0].alloc_gen.Get(), nullptr, IID_PPV_ARGS(&c.list_gen))) ||
            FAILED(c.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, c.slots[0].alloc_present[0].Get(), nullptr, IID_PPV_ARGS(&c.list_present))))
            return false;
        c.list_gen->Close();
        c.list_present->Close();
        D3D12_QUERY_HEAP_DESC qh{};
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qh.Count = kQueriesPerSlot * kSlots;
        if (FAILED(c.device->CreateQueryHeap(&qh, IID_PPV_ARGS(&c.queries)))) return false;
        D3D12_HEAP_PROPERTIES rb{};
        rb.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = sizeof(UINT64) * kQueriesPerSlot * kSlots;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(c.device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&c.query_readback))))
            return false;
        QueryPerformanceFrequency(&c.qpc);
    }
    if (g_live.load(std::memory_order_acquire) && !c.off_queries) {
        // The Off path's own copy, timed on the queue that runs it (failure only loses the figure).
        D3D12_QUERY_HEAP_DESC qh{};
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qh.Count = 2 * kSlots;
        D3D12_HEAP_PROPERTIES rb{};
        rb.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = sizeof(UINT64) * 2 * kSlots;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(c.device->CreateQueryHeap(&qh, IID_PPV_ARGS(&c.off_queries))) ||
            FAILED(c.device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&c.off_readback)))) {
            c.off_queries.Reset();
            c.off_readback.Reset();
        }
    }
    // Live Frame Generation: the per-slot textures exist only once it is switched on (prepared on
    // a background thread, see prepare_live); the Off path presents straight from the proxy.
    if (g_live.load(std::memory_order_acquire)) {
        if (!c.thread) {
            c.stop = false;
            c.thread = CreateThread(nullptr, 0, &present_thread, &c, 0, nullptr);
            if (!c.thread) return false;
        }
        c.ready = true;
        c.last_arrival.QuadPart = 0;
        c.last_shown.QuadPart = 0;
        set_reason(c.uav_ok ? "Presenter is active (Frame Generation Off: frames presented on the game's thread)."
                            : "Presenter is active; this picture format allows no generated frames.");
        return true;
    }
    for (Slot& s : c.slots) {
        // Render-small: the upscaler writes it (UAV) and the menu draws on it (render target).
        if (!create_texture(c, c.desc.BufferDesc.Width, c.desc.BufferDesc.Height, c.desc.BufferDesc.Format, c.uav_ok, c.scaled,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, s.real_copy))
            return false;
        if (!s.veto) { // A62: the runtime's "do not show generated pictures" flag, plus its readback
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = 16;
            bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (FAILED(c.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&s.veto))))
                return false;
            D3D12_HEAP_PROPERTIES rb{};
            rb.Type = D3D12_HEAP_TYPE_READBACK;
            bd.Flags = D3D12_RESOURCE_FLAG_NONE;
            if (FAILED(c.device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s.veto_readback))))
                return false;
        }
        for (auto& t : s.interp) {
            if (!c.uav_ok) { t.Reset(); continue; } // no generation possible on this format; real frames only
            if (!create_texture(c, c.desc.BufferDesc.Width, c.desc.BufferDesc.Height, c.desc.BufferDesc.Format, true, false,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, t))
                return false;
        }
        s.free_fence = 0;
        s.gen_fence = 0;
    }
    if (!c.thread) {
        c.stop = false;
        c.thread = CreateThread(nullptr, 0, &present_thread, &c, 0, nullptr);
        if (!c.thread) return false;
    }
    c.ready = true;
    c.last_arrival.QuadPart = 0;
    c.last_shown.QuadPart = 0;
    set_reason(c.uav_ok ? "Presenter is active." : "Presenter is active; this picture format allows no generated frames.");
    return true;
}

// Waits until the thread has shown everything queued (caller holds c.m; the
// thread never takes c.m).
bool drain_locked(Chain& c, DWORD timeout_ms)
{
    std::unique_lock<std::mutex> lock(c.jm);
    return c.jcv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return c.backlog == 0; });
}

// Returns false (and keeps everything) when the GPU or the presenter thread
// could not be drained in time: nothing size-dependent may be freed then.
bool release_size_dependent_locked(Chain& c)
{
    if (c.live_prep.joinable()) c.live_prep.join(); // it writes the slots' textures
    if (!drain_locked(c, 2000) || !wait_fence(c, c.last_signal.load(), 2000) || !wait_presentation(c,c.presentation_last.load(),2000)) return false;
    if (g_live.load(std::memory_order_acquire)) { // drained above: the next frames start on the game's thread
        c.live_ready.store(false, std::memory_order_release);
        g_live_ready.store(0);
        c.worker_owner = false;
        g_inline_owner.store(1);
    }
    for (Slot& s : c.slots) { s.hudfree.Reset(); s.steady_ran = false; }
    c.steady_prev_id = UINT64_MAX; // the slots' HUD-free pictures are made again
    c.steady_resized = true;
    if (c.cap.state.load() >= 2) { // F8 capture: the picture size changes, so an unfinished burst is dropped
        c.cap.shots.clear(); c.cap.used = 0; c.cap.state = 0;
        set_live_text(g_cap_text, "Presented-picture capture stopped: the picture size changed.", sizeof g_cap_text);
    }
    if (!c.proxies.empty()) g_proxied.fetch_sub(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> buffers_lock(c.buffers_m);
        c.proxy_count.store(0,std::memory_order_release);
        c.proxies.clear();
    }
    if (c.scaled) { g_scaled_w.store(0); g_scaled_h.store(0); c.scaled = false; }
    for (Slot& s : c.slots) {
        s.real_copy.Reset();
        for (auto& t : s.interp) t.Reset();
        s.free_fence = 0;
        s.gen_fence = 0;
        s.owned = false;
    }
    na::vram::Set("x2_textures", 0, "the picture size changed: x2's textures are made again at the new size");
    c.ready = false;
    c.queues_bound=false;
    c.virtual_index = 0;
    return true;
}

uint32_t bytes_per_pixel(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_UNORM: return 8;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
    default: return 4;
    }
}
bool video_memory(ID3D12Device* d, uint64_t* budget, uint64_t* usage)
{
    ComPtr<IDXGIFactory4> f;
    ComPtr<IDXGIAdapter3> a;
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (!d || FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f))) || FAILED(f->EnumAdapterByLuid(d->GetAdapterLuid(), IID_PPV_ARGS(&a))) ||
        FAILED(a->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
        return false;
    *budget = info.Budget;
    *usage = info.CurrentUsage;
    return true;
}
// Live Frame Generation is switched on: the x2 textures (per slot: the real frame's copy, one
// generated picture, the HUD-free picture) and the HUD paste pass are made on a background thread
// while the Off path stays on screen, so switching never stalls a frame. Refused, with the reason
// in the log and the menu, when the graphics-memory budget cannot hold them.
void prepare_live_locked(Chain& c)
{
    if (c.live_ready.load(std::memory_order_acquire)) return;
    if (c.live_prep.joinable()) return; // already running
    const UINT w = c.desc.BufferDesc.Width, h = c.desc.BufferDesc.Height;
    const DXGI_FORMAT fmt = c.desc.BufferDesc.Format;
    const uint64_t need = uint64_t(w) * h * bytes_per_pixel(fmt) * 3 * kSlots + (1ull << 20) * 8;
    g_extra_mb.store(need >> 20);
    uint64_t budget = 0, usage = 0;
    if (video_memory(c.device.Get(), &budget, &usage)) {
        g_vram_budget_mb.store(budget >> 20);
        g_vram_usage_mb.store(usage >> 20);
        constexpr uint64_t kMargin = 256ull << 20; // what Frame Generation's own feature needs, roughly
        if (usage + need + kMargin > budget) {
            char why[112];
            snprintf(why, sizeof why, "Frame Generation is off: it needs about %llu MB more graphics memory (%llu of %llu MB in use).",
                     static_cast<unsigned long long>((need + kMargin) >> 20), static_cast<unsigned long long>(usage >> 20),
                     static_cast<unsigned long long>(budget >> 20));
            set_live_text(g_prepare_text, why);
            fg::NotePaused(why);
            return;
        }
    }
    set_live_text(g_prepare_text, "x2 preparing: making its graphics memory; the picture stays as it is meanwhile.");
    g_live_preparing.store(1);
    ID3D12Device* device = c.device.Get();
    const bool uav = c.uav_ok;
    c.live_prep = std::thread([&c, device, w, h, fmt, uav] {
        bool ok = uav;
        for (Slot& s : c.slots) {
            if (!ok) break;
            ok = create_texture(c, w, h, fmt, uav, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, s.real_copy) &&
                 create_texture(c, w, h, fmt, true, false, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, s.interp[0]) &&
                 create_texture(c, w, h, fmt, false, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, s.hudfree);
            for (uint32_t i = 1; i < kMaxGenerated; ++i) s.interp[i].Reset(); // this build shows x2 at most
            if (ok && !s.veto) {
                D3D12_HEAP_PROPERTIES hp{};
                hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                D3D12_RESOURCE_DESC bd{};
                bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                bd.Width = 16;
                bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
                bd.SampleDesc.Count = 1;
                bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                ok = SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&s.veto)));
                D3D12_HEAP_PROPERTIES rb{};
                rb.Type = D3D12_HEAP_TYPE_READBACK;
                bd.Flags = D3D12_RESOURCE_FLAG_NONE;
                ok = ok && SUCCEEDED(device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s.veto_readback)));
            }
        }
        if (ok && !g_paste_ready.load(std::memory_order_acquire)) {
            ok = g_paste.Init(device);
            g_paste_ready.store(ok, std::memory_order_release);
        }
        if (ok && !g_steady_ready.load(std::memory_order_acquire)) { // optional: x2 runs without it, with the reason shown
            char why[112] = {};
            const bool steady = g_steady.Init(device, fmt, why, sizeof why);
            set_live_text(g_steady_refused, steady ? "" : why);
            if (!steady) na::eventlog::Emit("fg_steady", "not available: %s", why);
            g_steady_ready.store(steady, std::memory_order_release);
        }
        if (ok && !g_move_ready.load(std::memory_order_acquire)) { // optional: without it the interface is pasted the T2 way
            const bool moved = g_move.Init(device);
            set_live_text(g_move_refused, moved ? "" : "the moving-interface pass did not build");
            if (!moved) na::eventlog::Emit("fg_hud_move", "not available: %s", g_move.Error());
            g_move_ready.store(moved, std::memory_order_release);
        }
        g_live_preparing.store(0);
        if (!ok) {
            set_live_text(g_prepare_text, uav ? "Frame Generation could not make its textures on the graphics card."
                                              : "Frame Generation is not possible with this picture format (no shader writes).");
            return;
        }
        set_live_text(g_prepare_text, "Frame Generation textures are ready.");
        uint64_t bytes = 0;
        for (const Slot& s : c.slots) {
            bytes += na::vram::SizeOf(s.real_copy.Get()) + na::vram::SizeOf(s.hudfree.Get());
            for (const auto& t : s.interp) bytes += na::vram::SizeOf(t.Get());
        }
        g_extra_mb.store(bytes >> 20);
        na::vram::Set("x2_textures", bytes, "x2 is on: per slot the real frame's copy, the generated picture and the HUD-free picture");
        g_live_ready.store(1);
        c.live_ready.store(true, std::memory_order_release);
    });
}

// x2 has been Off for kLiveReleaseAfterMs, the presenter thread is idle and the GPU finished every frame
// that used x2 (the caller checked): its textures and NVIDIA's Frame Generation feature are released.
// Turning x2 on again prepares them anew off the game's thread (prepare_live_locked) while Off stays on
// screen; the menu says "x2 preparing" meanwhile.
void release_live_locked(Chain& c)
{
    c.live_ready.store(false, std::memory_order_release);
    g_live_ready.store(0);
    c.worker_owner = false;
    g_inline_owner.store(1);
    for (Slot& s : c.slots) {
        s.real_copy.Reset();
        for (auto& t : s.interp) t.Reset();
        s.hudfree.Reset();
        s.steady_ran = false;
        s.live_split = false;
    }
    c.steady_prev_id = UINT64_MAX;
    c.steady_resized = true;
    fg::Shutdown(); // the GPU is done with it (checked by the caller); it is made again when x2 is next on
    g_extra_mb.store(0);
    set_live_text(g_prepare_text, "x2 preparing: its graphics memory was given back while x2 was off.");
    na::vram::Set("x2_textures", 0, "x2 has been off for a few seconds: its textures are released");
    na::eventlog::Emit("fg_live", "x2 off for %u s: textures and the Frame Generation feature released (they are made again when x2 is on)",
                       static_cast<unsigned>(kLiveReleaseAfterMs / 1000));
}

// Copies `tex` into the real current back buffer and presents. `state` is
// the texture's resting state. Runs on whichever thread calls it.
// `no_wait` (A71): present with DXGI_PRESENT_DO_NOT_WAIT; when the chain's
// present queue is full the call returns DXGI_ERROR_WAS_STILL_DRAWING at once
// instead of blocking this thread, and nothing is counted as presented.
HRESULT present_texture(Chain& c, ID3D12CommandQueue* queue, ID3D12Resource* tex, D3D12_RESOURCE_STATES state,
                        ID3D12CommandAllocator* alloc, UINT sync, UINT flags, bool no_wait = false, uint64_t engine_frame = 0,
                        UINT query = UINT_MAX, int off_slot = -1, CaptureShot* shot = nullptr)
{
    UINT idx = g_orig.get_index(c.sc3.Get());
    if (idx >= c.desc.BufferCount) return DXGI_ERROR_INVALID_CALL;
    ComPtr<ID3D12Resource> bb_ref; // A71: held for this copy only, never across presents
    if (FAILED(g_orig.get_buffer(c.sc, idx, IID_PPV_ARGS(&bb_ref)))) return DXGI_ERROR_INVALID_CALL;
    ID3D12Resource* bb = bb_ref.Get();
    HRESULT setup_hr = alloc->Reset();
    if (FAILED(setup_hr)) return setup_hr;
    setup_hr = c.list_present->Reset(alloc, nullptr);
    if (FAILED(setup_hr)) return setup_hr;
    // Live Frame Generation Off path: the copy's own GPU time (the cost of presenting through a proxy).
    const bool off_timed = off_slot >= 0 && c.off_queries && c.off_readback;
    if (off_timed) c.list_present->EndQuery(c.off_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, UINT(off_slot) * 2);
    D3D12_RESOURCE_BARRIER pre[2] = {transition(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST),
                                     transition(tex, state, D3D12_RESOURCE_STATE_COPY_SOURCE)};
    c.list_present->ResourceBarrier(2, pre);
    c.list_present->CopyResource(bb, tex);
    if (shot && shot->buffer) { // F8 capture: exactly the picture copied into the back buffer
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = shot->buffer.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = c.cap.fp;
        src.pResource = tex; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        c.list_present->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        shot->copied = true;
    }
    D3D12_RESOURCE_BARRIER post[2] = {transition(bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT),
                                      transition(tex, D3D12_RESOURCE_STATE_COPY_SOURCE, state)};
    c.list_present->ResourceBarrier(2, post);
    if (off_timed) {
        c.list_present->EndQuery(c.off_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, UINT(off_slot) * 2 + 1);
        c.list_present->ResolveQueryData(c.off_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, UINT(off_slot) * 2, 2,
                                         c.off_readback.Get(), sizeof(UINT64) * 2 * UINT(off_slot));
    }
    if (query != UINT_MAX) {
        c.list_present->EndQuery(c.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query);
        c.list_present->ResolveQueryData(c.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query, 1,
            c.query_readback.Get(), sizeof(UINT64)*query);
    }
    setup_hr = c.list_present->Close();
    if (FAILED(setup_hr)) return setup_hr;
    ID3D12CommandList* lists[] = {c.list_present.Get()};
    if(g_orig.execute) g_orig.execute(queue,1,lists); // private queue is not a game input producer
    else queue->ExecuteCommandLists(1, lists);
    UINT pflags = no_wait ? (flags | static_cast<UINT>(DXGI_PRESENT_DO_NOT_WAIT)) : (flags & ~static_cast<UINT>(DXGI_PRESENT_DO_NOT_WAIT));
    g_real_present_caller.store(GetCurrentThreadId());
    LARGE_INTEGER called; QueryPerformanceCounter(&called);
    if (no_wait) na::latency::GeneratedPresent(engine_frame, false);
    else na::latency::EngineMark(engine_frame, na::latency::Phase::Present);
    HRESULT hr = g_orig.present(c.sc, sync, pflags);
    if (no_wait) na::latency::GeneratedPresent(engine_frame, true);
    else na::latency::EngineMark(engine_frame, na::latency::Phase::Presented);
    if (shot) {
        shot->hr = hr;
        shot->presented = SUCCEEDED(hr) && hr != DXGI_ERROR_WAS_STILL_DRAWING;
        QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&shot->qpc));
        if (shot->copied && c.cap.fence) queue->Signal(c.cap.fence.Get(), ++c.cap.fence_value);
    }
    if (no_wait && hr == DXGI_ERROR_WAS_STILL_DRAWING) return hr; // queue full: not shown, not counted
    c.last_hr.store(hr);
    if (FAILED(hr)) return hr;
    g_presents_real.fetch_add(1, std::memory_order_relaxed);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    {
        std::lock_guard<std::mutex> metrics(g_metrics_m);
        const float factor = 1000.f / static_cast<float>(c.qpc.QuadPart);
        g_present_calls.Add(static_cast<float>(now.QuadPart - called.QuadPart) * factor);
        if (SUCCEEDED(hr) && c.last_shown.QuadPart) g_intervals.Add(static_cast<float>(now.QuadPart - c.last_shown.QuadPart) * factor);
    }
    if (c.last_shown.QuadPart && c.qpc.QuadPart) {
        float ms = 1000.f * static_cast<float>(now.QuadPart - c.last_shown.QuadPart) / static_cast<float>(c.qpc.QuadPart);
        c.shown_ms = c.shown_ms > 0 ? c.shown_ms * 0.9f + ms * 0.1f : ms;
        g_shown_fps.store(c.shown_ms > 0 ? 1000.f / c.shown_ms : 0.f);
    }
    c.last_shown = now;
    return hr;
}

void wait_until(const Chain& c, LONGLONG target)
{
    for (;;) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (now.QuadPart >= target) return;
        double remaining_ms = 1000.0 * static_cast<double>(target - now.QuadPart) / static_cast<double>(c.qpc.QuadPart);
        if (remaining_ms > 2.5) Sleep(static_cast<DWORD>(remaining_ms - 1.5));
        else if (remaining_ms > 0.2) Sleep(0);
        else YieldProcessor();
    }
}

// F8 capture, present thread: the next shot to fill (null = not capturing this present). The burst is
// one real frame, then `triples` jobs each giving a generated picture and its real frame.
CaptureShot* capture_shot(Chain& c, const char* kind, uint64_t frame_id, uint64_t engine_frame)
{
    if (c.cap.state != 3 || c.cap.used >= c.cap.shots.size()) return nullptr;
    CaptureShot& s = c.cap.shots[c.cap.used++];
    s.kind = kind; s.frame_id = frame_id; s.engine_frame = engine_frame;
    return &s;
}
void capture_write(Chain& c)
{
    // Hand the buffers to a writer thread (it waits for the capture fence, then writes and releases).
    std::vector<CaptureShot> shots;
    shots.swap(c.cap.shots);
    shots.resize(c.cap.used);
    ComPtr<ID3D12Fence> fence = c.cap.fence;
    const UINT64 last = c.cap.fence_value;
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = c.cap.fp;
    const UINT w = c.cap.w, h = c.cap.h;
    const DXGI_FORMAT format = c.cap.format;
    const double qpf = static_cast<double>(c.qpc.QuadPart);
    if (c.cap.maker.joinable()) c.cap.maker.join(); // finished (it set `made`)
    c.cap.used = 0;
    c.cap.state = 0;
    std::thread([shots = std::move(shots), fence, last, fp, w, h, format, qpf]() mutable {
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (fence && fence->GetCompletedValue() < last && ev) { fence->SetEventOnCompletion(last, ev); WaitForSingleObject(ev, 5000); }
        if (ev) CloseHandle(ev);
        if (!fence || fence->GetCompletedValue() < last) { set_live_text(g_cap_text, "Presented-picture capture failed: the graphics card did not finish the copies.", sizeof g_cap_text); return; }
        wchar_t dir[MAX_PATH];
        if (!na::capture::MakeDir(L"fgout", dir, MAX_PATH, true)) { set_live_text(g_cap_text, "Presented-picture capture failed: the captures folder could not be made (disk nearly full?).", sizeof g_cap_text); return; }
        na::capture::TagDir(dir, "fgout");
        const uint32_t bpp = format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u : 4u;
        uint64_t bytes = 0;
        std::string json = "{\n  \"width\": " + std::to_string(w) + ", \"height\": " + std::to_string(h) + ", \"format\": " + std::to_string(static_cast<unsigned>(format)) +
                           ", \"bytes_per_pixel\": " + std::to_string(bpp) + ", \"layout\": \"tight rows, top to bottom\",\n  \"pictures\": [\n";
        for (size_t i = 0; i < shots.size(); ++i) {
            CaptureShot& s = shots[i];
            char name[64] = "";
            if (s.copied && s.buffer) {
                snprintf(name, sizeof name, "%02zu-%s.raw", i, s.kind);
                wchar_t path[MAX_PATH], wname[64];
                MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 64);
                swprintf_s(path, L"%s\\%s", dir, wname);
                void* m = nullptr;
                D3D12_RANGE rr{0, SIZE_T(fp.Footprint.RowPitch) * h};
                HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (f != INVALID_HANDLE_VALUE && SUCCEEDED(s.buffer->Map(0, &rr, &m))) {
                    for (UINT y = 0; y < h; ++y) {
                        DWORD wr = 0;
                        WriteFile(f, static_cast<const uint8_t*>(m) + fp.Offset + size_t(y) * fp.Footprint.RowPitch, w * bpp, &wr, nullptr);
                        bytes += wr;
                    }
                    D3D12_RANGE none{0, 0};
                    s.buffer->Unmap(0, &none);
                } else name[0] = 0;
                if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
            }
            s.buffer.Reset(); // release as we go
            char line[400];
            snprintf(line, sizeof line, "    {\"index\": %zu, \"kind\": \"%s\", \"frame_id\": %llu, \"engine_frame\": %llu, \"presented\": %s, \"present_result\": \"0x%08X\", \"time_ms\": %.3f, \"file\": \"%s\"}%s\n",
                     i, s.kind, static_cast<unsigned long long>(s.frame_id), static_cast<unsigned long long>(s.engine_frame), s.presented ? "true" : "false",
                     static_cast<unsigned>(s.hr), qpf > 0 ? 1000.0 * static_cast<double>(s.qpc) / qpf : 0.0, name, i + 1 < shots.size() ? "," : "");
            json += line;
        }
        json += "  ]\n}\n";
        wchar_t jp[MAX_PATH];
        swprintf_s(jp, L"%s\\presented.json", dir);
        HANDLE f = CreateFileW(jp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) { DWORD wr = 0; WriteFile(f, json.data(), static_cast<DWORD>(json.size()), &wr, nullptr); CloseHandle(f); }
        char dir8[MAX_PATH] = {};
        WideCharToMultiByte(CP_UTF8, 0, dir, -1, dir8, sizeof dir8, nullptr, nullptr);
        char text[220];
        snprintf(text, sizeof text, "Presented-picture capture saved: %zu pictures, %.0f MB, %s", shots.size(), double(bytes) / (1024.0 * 1024.0), dir8);
        set_live_text(g_cap_text, text, sizeof g_cap_text);
        na::eventlog::Emit("fg_capture", "%s", text);
    }).detach();
}

DWORD WINAPI present_thread(LPVOID p)
{
    Chain& c = *static_cast<Chain*>(p);
    g_present_thread_id.store(GetCurrentThreadId());
    timeBeginPeriod(1);
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(c.jm);
            c.jcv.wait(lock, [&] { return c.stop || c.job_r != c.job_w; });
            if (c.stop && c.job_r == c.job_w) break;
            job = c.jobs[c.job_r % kJobs];
        }
        Slot& s = c.slots[job.slot];
        auto pq=display_queue(c);
        if (c.cap.state == 2 && c.cap.made.load(std::memory_order_acquire) && job.count) { c.cap.state = 3; c.cap.jobs_seen = 0; }
        if (c.cap.state == 1 && c.cap.failed.load(std::memory_order_acquire)) { if (c.cap.maker.joinable()) c.cap.maker.join(); c.cap.shots.clear(); c.cap.state = 0; }
        if (c.cap.state == 2 && (!g_live.load(std::memory_order_relaxed) || c.cap.w != c.desc.BufferDesc.Width || c.cap.h != c.desc.BufferDesc.Height)) {
            c.cap.shots.clear(); c.cap.state = 0; // x2 went away or the picture size changed while the buffers were made
            set_live_text(g_cap_text, "Presented-picture capture stopped before it began (x2 off or the picture size changed).", sizeof g_cap_text);
        }
        if (c.cap.state == 1 && c.cap.made.load(std::memory_order_acquire)) c.cap.state = 2;
        // Rendering does not wait for presentation. Separate fences prevent
        // an earlier value on one queue falsely completing work on the other.
        if(c.queues_bound && FAILED(pq->Wait(c.fence.Get(),s.gen_fence.load()))) {
            c.last_hr.store(DXGI_ERROR_DEVICE_REMOVED); return 0;
        }
        if (HANDLE resume = g_test_worker_resume.load(std::memory_order_acquire)) {
            if (HANDLE entered = g_test_worker_entered.load()) SetEvent(entered);
            WaitForSingleObject(resume, 5000);
        }
        double ticks_per_ms = static_cast<double>(c.qpc.QuadPart) / 1000.0;
        // A62: the runtime may veto the generated pictures for this frame (its
        // full-screen-menu detection). The flag is readable once the generate
        // list finished; the first present cannot precede that anyway.
        if (job.count) {
            UINT64 gf = s.gen_fence.load(std::memory_order_acquire);
            // A timeout is not GPU completion. Keep this job and its textures
            // owned; retry without reading a readback buffer still being written.
            if (gf == UINT64_MAX || c.fence->GetCompletedValue() == UINT64_MAX) {
                c.last_hr.store(DXGI_ERROR_DEVICE_REMOVED);
                return 0;
            }
            if (gf && c.fence->GetCompletedValue() < gf) {
                HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (ev) { c.fence->SetEventOnCompletion(gf, ev); WaitForSingleObject(ev, 100); CloseHandle(ev); }
                if (c.fence->GetCompletedValue() < gf) continue;
            }
            void* m = nullptr;
            D3D12_RANGE rr{0, 4};
            if (s.veto_readback && SUCCEEDED(s.veto_readback->Map(0, &rr, &m))) {
                uint8_t flag = *static_cast<const uint8_t*>(m);
                D3D12_RANGE none{0, 0};
                s.veto_readback->Unmap(0, &none);
                if (flag) { job.count = 0; g_vetoed.fetch_add(1, std::memory_order_relaxed); }
            }
        }
        // A73: the schedule starts when the pictures EXIST, not when the game
        // presented. Generating takes the GPU a few milliseconds after the
        // game's present (6 ms at 3440x1440 for five pictures); anchoring at
        // the present made the first steps "late" before they could possibly
        // be shown and A71's late-drop rule then threw most pictures away
        // (session 13: 4,588 of 5,545 dropped). The whole sequence is shifted
        // by the smoothed generation time (steady, so the cadence stays even),
        // and never starts before the generate fence has actually passed.
        // This is what NVIDIA's own pacer does: real and generated frames are
        // presented after generation, spaced across the frame interval.
        LARGE_INTEGER ready;
        QueryPerformanceCounter(&ready);
        float lag_ms = static_cast<float>(1000.0 * static_cast<double>(ready.QuadPart - job.arrival) / static_cast<double>(c.qpc.QuadPart));
        { std::lock_guard<std::mutex> metrics(g_metrics_m);
          if (job.count) g_ready_ages.Add(lag_ms); else g_ready_ages = {}; }
        // A75 review: only a job picked up promptly measures generation time;
        // one that waited behind the previous job's presents measures queue
        // delay and would ratchet the anchor after any stall. Capped at one
        // frame interval.
        if (job.count && lag_ms >= 0.f && lag_ms <= job.frame_ms) {
            c.avail_ms = c.avail_ms > 0.f ? c.avail_ms * 0.9f + lag_ms * 0.1f : lag_ms;
        }
        if (c.avail_ms > job.frame_ms) c.avail_ms = job.frame_ms;
        LONGLONG anchor = job.arrival + static_cast<LONGLONG>(static_cast<double>(job.count ? c.avail_ms : 0.f) * ticks_per_ms);
        if (anchor < ready.QuadPart) anchor = ready.QuadPart;
        uint32_t steps = job.count + 1;
        double interval_ms = static_cast<double>(job.frame_ms) / static_cast<double>(steps);
        for (uint32_t i = 0; i < steps; ++i) {
            LONGLONG target = anchor + static_cast<LONGLONG>(interval_ms * static_cast<double>(i) * ticks_per_ms);
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            bool late = now.QuadPart > target + static_cast<LONGLONG>(2.0 * ticks_per_ms);
            if (late) g_late.fetch_add(1, std::memory_order_relaxed);
            else wait_until(c, target);
            if (i < job.count) {
                // A71: a generated picture that is already a whole step late is
                // not shown (it would only push the real frame later still), and
                // one that finds the chain's present queue full is not waited
                // for: this thread must never fall behind the game or block it.
                // (floor of 4 ms: scheduler jitter alone must not drop pictures when steps are tiny)
                double drop_after_ms = interval_ms > 4.0 ? interval_ms : 4.0;
                bool whole_step_late = now.QuadPart > target + static_cast<LONGLONG>(drop_after_ms * ticks_per_ms);
                if (whole_step_late) {
                    g_generated_dropped.fetch_add(1, std::memory_order_relaxed);
                    if (c.cap.jobs_seen) if (CaptureShot* shot = capture_shot(c, "generated", job.frame_id, job.engine_frame)) { shot->buffer.Reset(); shot->hr = DXGI_ERROR_WAS_STILL_DRAWING; }
                    continue;
                }
                CaptureShot* shot = c.cap.jobs_seen ? capture_shot(c, "generated", job.frame_id, job.engine_frame) : nullptr;
                HRESULT ghr = present_texture(c, pq, s.interp[i].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, s.alloc_present[i].Get(),
                                              job.sync, job.flags, true, job.engine_frame, job.slot*kQueriesPerSlot+2+i, -1, shot);
                if (SUCCEEDED(ghr) || ghr==DXGI_ERROR_WAS_STILL_DRAWING) s.copied_mask |= 1u<<i;
                if (FAILED(ghr)) { g_generated_dropped.fetch_add(1, std::memory_order_relaxed); if (g_live.load(std::memory_order_relaxed)) { char d[64]; snprintf(d, sizeof d, "Present returned 0x%08X", static_cast<unsigned>(ghr)); live_first("generated picture presented", false, d); } }
                else { g_generated_shown.fetch_add(1, std::memory_order_relaxed); if (g_live.load(std::memory_order_relaxed)) live_first("generated picture presented", true, nullptr); }
            } else {
                CaptureShot* shot = capture_shot(c, c.cap.jobs_seen ? "next-real" : "previous-real", job.frame_id, job.engine_frame);
                HRESULT rhr=present_texture(c, pq, s.real_copy.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, s.alloc_present[i].Get(),
                                job.sync, job.flags, false, job.engine_frame, job.slot*kQueriesPerSlot+2+i, -1, shot);
                if (SUCCEEDED(rhr)) s.copied_mask |= 1u<<i;
            }
        }
        if (c.cap.state == 3 && ++c.cap.jobs_seen > c.cap.triples) capture_write(c);
        LARGE_INTEGER finished; QueryPerformanceCounter(&finished);
        { std::lock_guard<std::mutex> metrics(g_metrics_m); g_real_ages.Add(static_cast<float>(1000.0 * static_cast<double>(finished.QuadPart - job.arrival) / static_cast<double>(c.qpc.QuadPart))); }
        release_slot(c, s, pq);
        {
            std::lock_guard<std::mutex> lock(c.jm);
            ++c.job_r;
            --c.backlog;
        }
        c.jcv.notify_all();
    }
    timeEndPeriod(1);
    return 0;
}

void read_timing_locked(Chain& c, Slot& s, uint32_t slot_index)
{
    UINT64 gf = s.gen_fence.load(std::memory_order_acquire);
    if (!gf || c.fence->GetCompletedValue() < gf) return;
    UINT64 freq = 0;
    if (!c.queue || FAILED(c.queue->GetTimestampFrequency(&freq)) || !freq) return;
    void* m = nullptr;
    const SIZE_T offset=static_cast<SIZE_T>(slot_index)*kQueriesPerSlot*sizeof(UINT64);
    D3D12_RANGE rr{offset, offset+kQueriesPerSlot*sizeof(UINT64)};
    if (FAILED(c.query_readback->Map(0, &rr, &m))) return;
    UINT64 st[kQueriesPerSlot]{};
    std::memcpy(st, static_cast<uint8_t*>(m) + offset, sizeof(st));
    D3D12_RANGE none{0, 0};
    c.query_readback->Unmap(0, &none);
    if (st[1] > st[0] && st[0]) {
        float ms = 1000.f * static_cast<float>(st[1] - st[0]) / static_cast<float>(freq);
        g_fg_ms.store(ms);
        float avg = g_fg_avg_ms.load();
        g_fg_avg_ms.store(avg > 0 ? avg * 0.9f + ms * 0.1f : ms);
    }
    if (s.live_split) { // live x2: real-frame copy | HUD-free copy | evaluate | steadying | HUD paste
        const UINT64 a = st[0], b = st[kLiveQuery], h = st[kLiveQuery + 1], e = st[kLiveQuery + 2], d = st[kLiveQuery + 3], z = st[1];
        const auto ms = [&](UINT64 from, UINT64 to) { return to >= from && from ? 1000.f * float(to - from) / float(freq) : -1.f; };
        if (const float v = ms(a, b); v >= 0.f) g_t_copy.add(v);
        if (s.hudfree_used) {
            if (const float v = ms(b, h); v >= 0.f) g_t_hudcopy.add(v);
            if (const float v = ms(h, e); v >= 0.f) g_t_eval.add(v);
            if (const float v = ms(d, z); v >= 0.f) g_t_paste.add(v);
        }
        if (s.steady_ran) {
            if (const float v = ms(e, d); v >= 0.f) g_t_steady.add(v);
            uint32_t changed = 0;
            if (g_steady.ReadChanged(s.steady_page, &changed)) {
                g_steady_changed.fetch_add(changed, std::memory_order_relaxed);
                g_steady_counted.fetch_add(s.steady_pixels, std::memory_order_relaxed);
            }
            s.steady_ran = false;
        }
        s.live_split = false;
    }
    // GPU timestamps are independent of CPU Present cadence. No diagnostic
    // waits: the caller already retired this slot before allocator reuse.
    UINT64 previous=st[1]; bool first=true;
    UINT64 pfreq=0,ggpu=0,gcpu=0,pgpu=0,pcpu=0;
    auto pq=display_queue(c);
    const bool calibrated=pq && SUCCEEDED(pq->GetTimestampFrequency(&pfreq)) && pfreq &&
        SUCCEEDED(c.queue->GetClockCalibration(&ggpu,&gcpu)) && SUCCEEDED(pq->GetClockCalibration(&pgpu,&pcpu));
    std::lock_guard<std::mutex> metrics(g_metrics_m);
    for(UINT i=0;i<kPresentsPerSlot;++i) if(s.copied_mask & (1u<<i)) {
        const auto end=st[2+i];
        if(previous && calibrated) {
            if(first) {
                const double generated=double(gcpu)/c.qpc.QuadPart+(double(previous)-double(ggpu))/freq;
                const double copied=double(pcpu)/c.qpc.QuadPart+(double(end)-double(pgpu))/pfreq;
                g_gpu_first_copy.Add(float(1000.0*(copied-generated)));
            } else if(end>=previous) g_gpu_copy_interval.Add(float(1000.0*double(end-previous)/pfreq));
        }
        previous=end; first=false;
    }
    s.copied_mask=0;
    s.gen_fence.store(0, std::memory_order_release);
}

bool stop_thread(Chain& c, uint32_t timeout_ms)
{
    if (!c.thread) return true;
    {
        std::lock_guard<std::mutex> lock(c.jm);
        c.stop = true;
    }
    c.jcv.notify_all();
    if (WaitForSingleObject(c.thread, timeout_ms) != WAIT_OBJECT_0) return false;
    CloseHandle(c.thread);
    c.thread = nullptr;
    return true;
}

} // namespace

void SetOriginals(const Originals& o) { g_orig = o; }
bool OwnsQueue(ID3D12CommandQueue* q) {
    if(!q) return false;
    for(auto& p:g_private_queues) if(p.load(std::memory_order_acquire)==q) return true;
    return false;
}
void SetEnabled(bool enabled) { g_enabled.store(enabled, std::memory_order_release); }
void SetLiveFrameGeneration(bool on) { g_live.store(on, std::memory_order_release); }
bool LiveFrameGeneration() { return g_live.load(std::memory_order_acquire); }
void SetHudFreeProvider(HudFreeFn fn) { g_hudfree_fn.store(fn, std::memory_order_release); }
void NoteHudRelearn() { g_hud_epoch.fetch_add(1, std::memory_order_relaxed); }
bool RequestOutputCapture(uint32_t triples, char* why, size_t n)
{
    auto say = [&](const char* t) { if (why && n) strncpy_s(why, n, t, _TRUNCATE); set_live_text(g_cap_text, t, sizeof g_cap_text); return false; };
    if (!g_live.load(std::memory_order_acquire)) return say("Presented-picture capture needs live Frame Generation.");
    if (!na::settings::Get().fg_enabled.load(std::memory_order_relaxed)) return say("Presented-picture capture: switch x2 on first (it saves what x2 presents).");
    if (triples < 1) triples = 1;
    if (triples > 16) triples = 16;
    g_cap_request.store(triples, std::memory_order_release);
    if (why && n) why[0] = 0;
    return true;
}
LiveStats GetLiveStats()
{
    LiveStats s;
    s.live = g_live.load() ? 1u : 0u;
    s.ready = g_live_ready.load();
    s.preparing = g_live_preparing.load();
    s.inline_owner = g_inline_owner.load();
    s.frames_inline = g_live_inline.load();
    s.frames_worker = g_live_worker.load();
    s.skipped_no_hudfree = g_skip_hudfree.load();
    s.skipped_menu = g_skip_menu.load();
    s.skipped_preparing = g_skip_preparing.load();
    s.skipped_veto = g_vetoed.load();
    s.off_ms = g_t_off.last.load(); s.off_avg = g_t_off.avg.load();
    s.copy_ms = g_t_copy.last.load(); s.copy_avg = g_t_copy.avg.load();
    s.hudcopy_ms = g_t_hudcopy.last.load(); s.hudcopy_avg = g_t_hudcopy.avg.load();
    s.eval_ms = g_t_eval.last.load(); s.eval_avg = g_t_eval.avg.load();
    s.paste_ms = g_t_paste.last.load(); s.paste_avg = g_t_paste.avg.load();
    s.steady_on = na::settings::Get().fg_steady.load(std::memory_order_relaxed) ? 1u : 0u;
    s.steady_ready = g_steady_ready.load() ? 1u : 0u;
    s.steady_frames = g_steady_frames.load(); s.steady_off_frames = g_steady_off.load();
    { const uint64_t counted = g_steady_counted.load(); s.steady_share = counted ? 100.f * float(double(g_steady_changed.load()) / double(counted)) : 0.f; }
    s.steady_ms = g_t_steady.last.load(); s.steady_avg = g_t_steady.avg.load();
    {
        const auto ms = g_move.Read();
        s.move_on = na::settings::Get().fg_hud_move.load(std::memory_order_relaxed) ? 1u : 0u;
        s.move_ready = g_move_ready.load() ? 1u : 0u;
        s.move_frames = ms.frames; s.move_pictures = ms.pictures;
        s.move_tiles_moving = ms.tiles_moving; s.move_tiles_fixed = ms.tiles_fixed;
        s.move_pixels_moving = ms.pixels_moving; s.move_pixels_fixed = ms.pixels_fixed;
        s.move_shift_px = ms.shift_n ? float(ms.shift_px_sum / double(ms.shift_n)) : 0.f;
        s.move_fb_pair = ms.fallback_pair; s.move_fb_reset = ms.fallback_reset; s.move_fb_motion = ms.fallback_motion;
        s.move_fb_off = ms.fallback_off; s.move_fb_size = ms.fallback_size;
    }
    s.steady_drop_gap = g_drop_gap.load(); s.steady_drop_reset = g_drop_reset.load(); s.steady_drop_motion = g_drop_motion.load();
    s.steady_drop_relearn = g_drop_relearn.load(); s.steady_drop_resize = g_drop_resize.load(); s.steady_drop_refused = g_drop_refused.load();
    s.display_reported_hz = g_display_reported.load(); s.display_max_hz = g_display_hz.load();
    s.skipped_display = g_skip_display.load();
    s.vram_budget_mb = g_vram_budget_mb.load();
    s.vram_usage_mb = g_vram_usage_mb.load();
    s.extra_mb = g_extra_mb.load();
    const fg::Stats fs = fg::GetStats(); // before our text lock: fg's own lock is never taken under it
    {
        std::lock_guard<std::mutex> lock(g_live_text_m);
        strncpy_s(s.prepare, g_prepare_text, _TRUNCATE);
        strncpy_s(s.hudfree_why, g_hudfree_text, _TRUNCATE);
        strncpy_s(s.steady_why, g_steady_refused, _TRUNCATE);
        strncpy_s(s.display_dynamic, g_display_dynamic, _TRUNCATE);
        strncpy_s(s.capture, g_cap_text, _TRUNCATE);
    }
    // Never a silent "nothing": x2 chosen, and no generated picture for more than a second.
    const uint64_t since = g_fg_on_since.load(), last = g_last_generated_tick.load(), now = GetTickCount64();
    if (s.live && since) {
        const uint64_t from = last > since ? last : since;
        if (now > from + 1000) {
            s.waiting_ms = static_cast<uint32_t>(now - from);
            if (!s.ready) snprintf(s.waiting, sizeof s.waiting, "x2 is on but waiting: %s", s.prepare[0] ? s.prepare : "the extra textures are not ready yet");
            else if (s.hudfree_why[0]) snprintf(s.waiting, sizeof s.waiting, "x2 is on but waiting: the picture without the HUD could not be made (%s)", s.hudfree_why);
            else snprintf(s.waiting, sizeof s.waiting, "x2 is on but waiting: %s", fs.reason);
        }
    }
    return s;
}
void SetRenderScale(float scale, UpscaleFn upscale, void* user)
{
    g_upscale_user.store(user, std::memory_order_release);
    g_upscale.store(upscale, std::memory_order_release);
    g_render_scale.store(scale > 0.f && scale < 1.f ? scale : 1.f, std::memory_order_release);
}
void SetPresentDlaa(bool on) { g_present_dlaa.store(on, std::memory_order_release); }
bool PresentDlaa() { return g_present_dlaa.load(std::memory_order_acquire); }
float RenderScale() { return g_render_scale.load(std::memory_order_acquire); }
bool Scaling() { return g_scaled_w.load(std::memory_order_acquire) != 0; }
bool GameBufferSize(IDXGISwapChain* sc, UINT* w, UINT* h)
{
    if (!Enabled() || !sc) return false;
    std::lock_guard<std::mutex> lock(g_chains_m);
    Chain* c = find_chain(sc);
    if (!c || !c->proxy_count.load(std::memory_order_acquire)) return false;
    if (w) *w = c->pw;
    if (h) *h = c->ph;
    return true;
}
void TranslateResize(IDXGISwapChain* sc, UINT* w, UINT* h)
{
    if (!Enabled() || !sc || !w || !h) return;
    // Render-small: the real chain always keeps the window's physical size, whatever size the
    // game believes (also on the way back to native, where the game may pass a stale small size).
    if (!g_upscale.load(std::memory_order_acquire)) return;
    {
        std::lock_guard<std::mutex> lock(g_chains_m);
        Chain* c = find_chain(sc);
        if (c && c->external_owner.load(std::memory_order_acquire)) return;
    }
    *w = 0; // DXGI: the window's client size, whatever size the game believes it has
    *h = 0;
}
void NoteGameLatency(IDXGISwapChain* sc, UINT n)
{
    if (!sc) return;
    std::lock_guard<std::mutex> lock(g_chains_m);
    Chain* c = find_chain(sc);
    if (!c) { // the game usually sets this right after creating the chain, before any GetBuffer
        for (Chain& k : g_chains) if (!k.sc) { c = &k; c->sc = sc; break; }
        if (!c) return;
    }
    c->game_latency.store(n, std::memory_order_relaxed);
    // A76: the overlay applied max(n, LatencyFloor) (never lower than ours).
    if (n > c->latency_applied) c->latency_applied = n;
}

UINT LatencyFloor(IDXGISwapChain* sc)
{
    if (!sc) return 0;
    std::lock_guard<std::mutex> lock(g_chains_m);
    Chain* c = find_chain(sc);
    return c ? c->latency_applied : 0;
}

namespace {
// A63: real chain latency = game's value + pictures generated per frame.
// A76 (session 14 freeze on Home, and very likely freeze 2 on Alt+Z): the
// value is only ever RAISED, never lowered. A game that paces itself with the
// frame-latency waitable object (TLOU: chain_flags 0x840) waits on it before
// every frame; lowering the maximum while the extra permits handed out for the
// raise are outstanding left the game waiting for a signal that never came
// (game thread in WaitForSingleObject, presents stopped the moment Frame
// Generation paused for the menu). Staying at the high-water mark costs, at
// most, a few frames of extra queue while nothing is generated.
void apply_latency_locked(Chain& c, uint32_t count)
{
    UINT game_n = c.game_latency.load(std::memory_order_relaxed);
    if (!game_n || !g_orig.set_max_latency) return;
    UINT desired = game_n + count;
    if (desired > 16) desired = 16;
    if (desired <= c.latency_applied) return;
    ComPtr<IDXGISwapChain2> sc2;
    if (SUCCEEDED(c.sc->QueryInterface(IID_PPV_ARGS(&sc2))) && SUCCEEDED(g_orig.set_max_latency(sc2.Get(), desired)))
        c.latency_applied = desired;
}
} // namespace
bool Enabled() { return g_enabled.load(std::memory_order_acquire); }
bool IsPresentThread() { return GetCurrentThreadId() == g_present_thread_id.load(std::memory_order_relaxed); }

bool Proxied(IDXGISwapChain* sc)
{
    if (!Enabled() || !sc) return false;
    std::lock_guard<std::mutex> lock(g_chains_m);
    Chain* c = find_chain(sc);
    return c && c->proxy_count.load(std::memory_order_acquire)!=0;
}

bool NoteExternalOwner(IDXGISwapChain* sc) {
    if(!sc)return false;Chain* c=nullptr;
    {std::lock_guard<std::mutex> lock(g_chains_m);
        c=find_chain(sc);if(!c)for(auto& slot:g_chains)if(!slot.sc){c=&slot;c->sc=sc;break;}}
    if(!c)return false;
    std::lock_guard<std::mutex> lock(c->m);c->external_owner=true;
    return c->proxies.empty() && c->resize_proxies.empty();
}
void AfterResize(IDXGISwapChain* sc,HRESULT result) {
    Chain* c=nullptr;{std::lock_guard<std::mutex> lock(g_chains_m);c=find_chain(sc);}
    if(!c)return;std::lock_guard<std::mutex> lock(c->m);
    if(FAILED(result) && g_upscale.load(std::memory_order_acquire)){ // render-small: a pending live size did not happen
        const auto st=na::native_present::GetSizeState();
        if(st.request && st.request_delivered)na::native_present::CompleteResize(st.request,false,{});
    }
    if(FAILED(result) && c->proxies.empty() && !c->resize_proxies.empty()){
        std::lock_guard<std::mutex> buffers_lock(c->buffers_m);
        c->proxies=std::move(c->resize_proxies);c->virtual_index=c->resize_index;c->proxy_count.store(static_cast<UINT>(c->proxies.size()),std::memory_order_release);
        c->scaled=c->resize_scaled;c->pw=c->resize_pw;c->ph=c->resize_ph;
        if(c->scaled){g_scaled_w.store(c->pw);g_scaled_h.store(c->ph);}
        g_proxied.fetch_add(1,std::memory_order_relaxed);
    }
    c->resize_proxies.clear();
}

bool HandleGetBuffer(IDXGISwapChain* sc, UINT index, REFIID riid, void** out, HRESULT* hr)
{
    if (!Enabled() || !sc || !out) return false;
    Chain* c = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_chains_m);
        c = find_chain(sc);
        if (!c) {
            for (Chain& k : g_chains) if (!k.sc) { c = &k; break; }
            if (!c) return false; // more chains than we track: that one keeps the old path
            c->sc = sc;
        }
    }
    // SL's consumer may hold its own mutex while asking for this buffer.
    // Already-acquired images never acquire the presenter state mutex: Present
    // calls the NR consumer while holding that mutex. This avoids that inversion.
    auto acquired=[&](){
        if(c->proxies.empty())return false;
        if(index>=c->proxies.size()){*hr=DXGI_ERROR_INVALID_CALL;*out=nullptr;return true;}
        *hr=c->proxies[index]->QueryInterface(riid,out);return true;
    };
    {
        std::lock_guard<std::mutex> buffers_lock(c->buffers_m);
        if(acquired())return true;
        if(c->external_owner.load(std::memory_order_acquire))return false;
    }
    std::lock_guard<std::mutex> lock(c->m);
    if(c->external_owner.load(std::memory_order_acquire))return false;
    if(!ensure_proxies_locked(*c))return false;
    std::lock_guard<std::mutex> buffers_lock(c->buffers_m);
    return acquired();
}

bool HandleGetIndex(IDXGISwapChain* sc, UINT* out)
{
    if (!Enabled() || !sc || !out) return false;
    std::lock_guard<std::mutex> lock(g_chains_m);
    Chain* c = find_chain(sc);
    if (!c || !c->proxy_count.load(std::memory_order_acquire)) return false;
    *out = c->virtual_index.load(std::memory_order_relaxed);
    return true;
}

bool BeforeResize(IDXGISwapChain* sc)
{
    if (!Enabled() || !sc) return true;
    Chain* c = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_chains_m);
        c = find_chain(sc);
    }
    if (!c) return true;
    std::lock_guard<std::mutex> lock(c->m);
    // The feature is bound to the old size and is re-created on the next frame,
    // but only once draining succeeded: on a timeout its GPU work may still be
    // running, so FG keeps its feature and copies (ASTRA-P4-3).
    // Preserve only our own textures while DXGI decides. Native buffers and
    // pending GPU work still use the existing drain/retirement path.
    auto saved=c->proxies;const auto index=c->virtual_index.load();
    const bool was_scaled=c->scaled;const UINT spw=c->pw,sph=c->ph;
    if (release_size_dependent_locked(*c)) {
        c->resize_proxies=std::move(saved);c->resize_index=index;fg::Shutdown();
        c->resize_scaled=was_scaled;c->resize_pw=spw;c->resize_ph=sph;return true;
    }
    return false; // DXGI must not resize underneath submitted presenter work
}

HRESULT Present(const PresentArgs& a)
{
    // A62: a "test" present asks about occlusion only; nothing is shown.
    if (a.flags & DXGI_PRESENT_TEST) return g_orig.present ? g_orig.present(a.sc, a.sync, a.flags) : E_FAIL;
    Chain* c = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_chains_m);
        c = find_chain(a.sc);
    }
    if (!c || !a.queue) return g_orig.present ? g_orig.present(a.sc, a.sync, a.flags) : E_FAIL;
    // Driver control/query must not run under the presenter chain lock.
    ComPtr<ID3D12Device> latency_device;
    DXGI_SWAP_CHAIN_DESC latency_desc{};
    if (SUCCEEDED(a.queue->GetDevice(IID_PPV_ARGS(&latency_device))) && SUCCEEDED(a.sc->GetDesc(&latency_desc)))
        na::latency::Update(latency_device.Get(), latency_desc.OutputWindow);
    std::lock_guard<std::mutex> lock(c->m);
    g_presents_game.fetch_add(1, std::memory_order_relaxed);
    if (!c->queue) c->queue = a.queue;
    if (!ensure_ready_locked(*c)) {
        set_reason("Presenter could not set up on the graphics card; frames pass through untouched.");
        // Without our resources the game's picture is in a proxy nobody presents.
        // Best effort: nothing else we can do than forward (screen stays black
        // until the next resize succeeds).
        return g_orig.present(a.sc, a.sync, a.flags);
    }
    const auto engine_frame = na::tlou1::TimingRenderFrame();
    g_requested_sync.store(a.sync); g_requested_flags.store(a.flags);
    // Timing of the game's presents.
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (c->last_arrival.QuadPart) {
        float ms = 1000.f * static_cast<float>(now.QuadPart - c->last_arrival.QuadPart) / static_cast<float>(c->qpc.QuadPart);
        // A single hitch (loading, alt-tab, a pause) must not inflate the
        // interval that pacing and the dynamic count rely on: ignore up to
        // two consecutive intervals over three times the running value.
        bool hitch = c->frame_ms > 0.f && ms > 3.f * c->frame_ms && c->hitch_skips < 2;
        if (hitch) ++c->hitch_skips;
        else if (ms > 0.f && ms < 500.f) {
            c->frame_ms = c->frame_ms > 0 ? c->frame_ms * 0.8f + ms * 0.2f : ms; // the real cadence (pacing)
            // A74 review: Frame Generation's base rate must be the game's OWN
            // rendering time. With V-Sync the presents of a high multiplier
            // throttle the game through our slot ring; that blocked time sat
            // inside the measured interval, so Dynamic saw a slow game, kept
            // the multiplier, which kept the game slow: a one-way ratchet.
            float render = ms - c->last_blocked_ms;
            if (render < 0.5f) render = 0.5f;
            c->render_ms = c->render_ms > 0 ? c->render_ms * 0.8f + render * 0.2f : render;
            c->hitch_skips = 0;
        }
    }
    c->last_blocked_ms = 0.f;
    c->last_arrival = now;
    g_base_fps.store(c->frame_ms > 0 ? 1000.f / c->frame_ms : 0.f);

    ID3D12Resource* src = c->proxies[c->virtual_index].Get();
    if (a.frame) a.frame(a.queue, src, a.user); // NR / motion / capture on the finished frame
    // The menu belongs to this game's frame. Composite before copying to the
    // owned presentation slot; the worker never touches ImGui or its lifetime.
    // Render-small: the menu draws on the native picture after the upscale instead.
    if (a.menu && !c->scaled) a.menu(a.sc, a.queue, src, a.user);

    // Slot for this frame: wait (back-pressure) until the thread finished
    // showing the frame that used it kSlots frames ago.
    uint32_t si = static_cast<uint32_t>(c->frame_id % kSlots);
    Slot& s = c->slots[si];
    {
        LARGE_INTEGER b0, b1;
        QueryPerformanceCounter(&b0);
        bool slot_free;
        {
            std::unique_lock<std::mutex> jl(c->jm);
            slot_free = c->jcv.wait_for(jl, std::chrono::milliseconds(1000), [&] { return !s.owned.load(std::memory_order_acquire); });
        }
        if (slot_free) slot_free = wait_presentation(*c, s.free_fence.load(std::memory_order_acquire), 1000);
        QueryPerformanceCounter(&b1);
        c->last_blocked_ms += 1000.f * static_cast<float>(b1.QuadPart - b0.QuadPart) / static_cast<float>(c->qpc.QuadPart);
        if (!slot_free) {
            // A75 review: the present thread is still showing from this slot
            // (a blocking Present of an occluded window, or a GPU stall). Do
            // not touch the slot's allocator or textures: forward the game's
            // present untouched (its picture stays in the proxy this once).
            g_dropped.fetch_add(1, std::memory_order_relaxed);
            return DXGI_ERROR_WAS_STILL_DRAWING; // never race the worker's real Present
        }
    }
    s.owned.store(true, std::memory_order_release);
    read_timing_locked(*c, s, si);
    const bool live = g_live.load(std::memory_order_acquire);
    if (live && s.off_timed) { // the Off path's copy that last used this slot has completed (free fence above)
        s.off_timed = false;
        UINT64 st[2]{}, freq = 0;
        void* m = nullptr;
        const SIZE_T off = SIZE_T(si) * 2 * sizeof(UINT64);
        D3D12_RANGE rr{off, off + 2 * sizeof(UINT64)};
        auto pq = display_queue(*c);
        if (pq && SUCCEEDED(pq->GetTimestampFrequency(&freq)) && freq && SUCCEEDED(c->off_readback->Map(0, &rr, &m))) {
            std::memcpy(st, static_cast<uint8_t*>(m) + off, sizeof(st));
            D3D12_RANGE none{0, 0};
            c->off_readback->Unmap(0, &none);
            if (st[0] && st[1] >= st[0]) g_t_off.add(1000.f * float(st[1] - st[0]) / float(freq));
        }
    }
    bool fg_on = na::settings::Get().fg_enabled.load(std::memory_order_relaxed) != 0;
    if (live) { // "x2 is on but waiting" is measured from the moment x2 was chosen
        if (!fg_on) g_fg_on_since.store(0, std::memory_order_relaxed);
        else if (!g_fg_on_since.load(std::memory_order_relaxed)) g_fg_on_since.store(GetTickCount64(), std::memory_order_relaxed);
    }
    uint32_t backlog_now;
    {
        std::lock_guard<std::mutex> jl(c->jm);
        backlog_now = c->backlog;
    }
    if (live) {
        // Route B: x2 needs its textures first; they are made off this thread while Off stays on screen.
        if (c->live_prep.joinable() && c->live_ready.load(std::memory_order_acquire)) c->live_prep.join();
        if (fg_on && !c->live_ready.load(std::memory_order_acquire)) {
            const ULONGLONG now_ms = GetTickCount64();
            if (!c->live_prep.joinable() && now_ms >= c->live_retry_after) {
                prepare_live_locked(*c);
                if (!c->live_prep.joinable()) c->live_retry_after = now_ms + 5000; // refused (memory): ask again in 5 s
            }
            char why[112];
            { std::lock_guard<std::mutex> text_lock(g_live_text_m); strncpy_s(why, g_prepare_text, _TRUNCATE); }
            fg::NotePaused(why[0] ? why : "Preparing Frame Generation.");
            g_skip_preparing.fetch_add(1, std::memory_order_relaxed);
        }
        const bool ready = c->live_ready.load(std::memory_order_acquire);
        const auto owner = na::fglive::NextOwner(fg_on && ready, c->worker_owner ? na::fglive::Owner::PresenterThread : na::fglive::Owner::GameThread, backlog_now);
        c->worker_owner = owner == na::fglive::Owner::PresenterThread;
        g_inline_owner.store(c->worker_owner ? 0u : 1u);
        // x2 Off: its textures and NVIDIA's feature are given back a few seconds later (switching back and forth
        // quickly does not make them again each time), once the presenter thread is idle and the GPU has finished
        // every frame that used them. Nothing waits here: if anything is still running, the next frame looks again.
        if (fg_on || c->worker_owner || !ready) c->live_off_since = 0;
        else {
            const ULONGLONG now_ms = GetTickCount64();
            if (!c->live_off_since) c->live_off_since = now_ms;
            else if (now_ms - c->live_off_since >= kLiveReleaseAfterMs && backlog_now == 0 && !c->live_prep.joinable() &&
                     wait_fence(*c, c->last_signal.load(), 0) && wait_presentation(*c, c->presentation_last.load(), 0)) {
                release_live_locked(*c);
                c->live_off_since = 0;
            }
        }
        if (!ready) fg_on = false; // nothing to generate with yet
    } else if (fg_on) {
        c->worker_owner = true;
    }
    if (live && fg_on && c->display_hz > 0.f && c->render_ms > 0.f && 1000.f / c->render_ms > c->display_hz * 1.02f) {
        // Even the display's maximum refresh is below the game's own rate: x2 could show nothing extra.
        char why[112];
        snprintf(why, sizeof why, "x2 waits: the game already renders %.0f fps, above the display's maximum of %.0f Hz.", 1000.f / c->render_ms, c->display_hz);
        fg::NotePaused(why);
        g_skip_display.fetch_add(1, std::memory_order_relaxed);
        fg_on = false;
    }
    bool want_fg = !c->external_owner && c->uav_ok && !a.menu_open && fg_on;
    // A73: say WHY nothing is generated right now instead of leaving the last
    // message standing (session 13: "starting" stayed on screen because the
    // menu that showed it also pauses generation).
    if (fg_on && !want_fg) {
        if (!c->uav_ok) fg::NotePaused("Frame Generation is paused: this picture format allows no generated frames.");
        else fg::NotePaused("Frame Generation is paused while this menu is open; it resumes the moment you close it (Home).");
        if (live && a.menu_open) g_skip_menu.fetch_add(1, std::memory_order_relaxed);
    }
    if (!c->worker_owner && !want_fg && backlog_now == 0 && !c->scaled) {
        if (live) g_live_inline.fetch_add(1, std::memory_order_relaxed);
        // A62 fast path (nothing to generate, thread idle): present straight
        // from the game's proxy, one copy, no intermediate texture: the old
        // path's cost plus that single copy.
        apply_latency_locked(*c, 0);
        g_thread_mode.store(0, std::memory_order_relaxed);
        c->frame_id++;
        c->virtual_index = (c->virtual_index + 1) % static_cast<UINT>(c->proxies.size());
        na::latency::EngineMark(engine_frame, na::latency::Phase::RenderEnd);
        const UINT64 render_done=signal_progress(*c,a.queue);
        auto pq=display_queue(*c);
        if(render_done==UINT64_MAX) return DXGI_ERROR_DEVICE_REMOVED;
        if(c->queues_bound) { const HRESULT whr=pq->Wait(c->fence.Get(),render_done); if(FAILED(whr)) return whr; }
        HRESULT hr = present_texture(*c, pq, src, D3D12_RESOURCE_STATE_PRESENT, s.alloc_present[0].Get(), a.sync, a.flags, false, engine_frame,
                                     UINT_MAX, live && c->off_queries ? static_cast<int>(si) : -1);
        if (live && c->off_queries) s.off_timed = true;
        release_slot(*c, s, pq);
        return hr;
    }
    if (live) g_live_worker.fetch_add(1, std::memory_order_relaxed);
    if (const uint32_t want = g_cap_request.exchange(0, std::memory_order_acq_rel)) {
        // F8 with x2 on: readback buffers for the burst, made off this thread (one per present).
        if (c->cap.state != 0 || c->cap.maker.joinable()) set_live_text(g_cap_text, "Presented-picture capture is already running.", sizeof g_cap_text);
        else {
            c->cap.triples = want;
            c->cap.used = 0;
            c->cap.w = c->desc.BufferDesc.Width; c->cap.h = c->desc.BufferDesc.Height; c->cap.format = c->desc.BufferDesc.Format;
            D3D12_RESOURCE_DESC td{};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = c->cap.w; td.Height = c->cap.h; td.DepthOrArraySize = 1; td.MipLevels = 1;
            td.Format = c->cap.format; td.SampleDesc.Count = 1;
            UINT64 total = 0;
            c->device->GetCopyableFootprints(&td, 0, 1, 0, &c->cap.fp, nullptr, nullptr, &total);
            c->cap.shots.assign(size_t(want) * 2 + 1, CaptureShot{});
            c->cap.made.store(false); c->cap.failed.store(false);
            if (!c->cap.fence && FAILED(c->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&c->cap.fence)))) c->cap.fence.Reset();
            char text[220];
            snprintf(text, sizeof text, "Presented-picture capture: preparing %u pictures (%.0f MB of memory, the same on disk).", want * 2 + 1,
                     double(total) * double(want * 2 + 1) / (1024.0 * 1024.0));
            set_live_text(g_cap_text, text, sizeof g_cap_text);
            na::eventlog::Emit("fg_capture", "%s", text);
            c->cap.state = 1;
            ID3D12Device* device = c->device.Get();
            Capture* cap = &c->cap;
            cap->maker = std::thread([cap, device, total] {
                D3D12_HEAP_PROPERTIES hp{};
                hp.Type = D3D12_HEAP_TYPE_READBACK;
                D3D12_RESOURCE_DESC bd{};
                bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
                bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                bool ok = cap->fence != nullptr;
                for (auto& shot : cap->shots)
                    if (ok) ok = SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&shot.buffer)));
                if (!ok) {
                    for (auto& shot : cap->shots) shot.buffer.Reset();
                    set_live_text(g_cap_text, "Presented-picture capture failed: not enough memory for the readback buffers.", sizeof g_cap_text);
                    cap->failed.store(true, std::memory_order_release);
                    return;
                }
                set_live_text(g_cap_text, "Presented-picture capture: waiting for the next frame with a generated picture.", sizeof g_cap_text);
                cap->made.store(true, std::memory_order_release);
            });
        }
    }

    // Render-small: the upscaler turns the render-size picture into the native slot texture on
    // the game's queue (its own list, executed before ours), then the menu draws on it.
    bool upscaled = false;
    if (c->scaled) {
        const UpscaleFn fn = g_upscale.load(std::memory_order_acquire);
        upscaled = fn && fn(a.queue, src, c->pw, c->ph, s.real_copy.Get(), c->desc.BufferDesc.Width, c->desc.BufferDesc.Height,
                            a.hdr, g_upscale_user.load(std::memory_order_acquire));
        if (upscaled) {
            g_upscaled.fetch_add(1, std::memory_order_relaxed);
            if (a.menu) a.menu(a.sc, a.queue, s.real_copy.Get(), a.user);
        } else {
            g_upscale_failed.fetch_add(1, std::memory_order_relaxed); // the slot shows its previous picture
        }
    }
    // Copy the finished frame into our texture; then generate.
    s.alloc_gen->Reset();
    c->list_gen->Reset(s.alloc_gen.Get(), nullptr);
    c->list_gen->EndQuery(c->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, si * kQueriesPerSlot);
    if (c->scaled) {
        if (upscaled) {
            D3D12_RESOURCE_BARRIER back = transition(s.real_copy.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            c->list_gen->ResourceBarrier(1, &back);
        }
    } else {
        D3D12_RESOURCE_BARRIER pre[2] = {transition(src, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                         transition(s.real_copy.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST)};
        c->list_gen->ResourceBarrier(2, pre);
        c->list_gen->CopyResource(s.real_copy.Get(), src);
        D3D12_RESOURCE_BARRIER post[2] = {transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT),
                                          transition(s.real_copy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
        c->list_gen->ResourceBarrier(2, post);
    }
    // Live x2 (T2): Frame Generation is given this frame's HUD-free picture as its back buffer.
    // Without one there is nothing safe to generate from: this frame shows the real picture only.
    ID3D12Resource* gen_color = s.real_copy.Get();
    s.live_split = live;
    s.hudfree_used = false;
    if (live) {
        c->list_gen->EndQuery(c->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, si * kQueriesPerSlot + kLiveQuery);
        const char* why = nullptr;
        const HudFreeFn provider = g_hudfree_fn.load(std::memory_order_acquire);
        D3D12_RESOURCE_STATES hf_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        ID3D12Resource* hf = provider ? provider(&why, &hf_state) : nullptr;
        if (!want_fg) set_live_text(g_hudfree_text, ""); // a menu or an Off frame: the HUD-free reason no longer applies
        if (want_fg) {
            if (hf && s.hudfree) {
                D3D12_RESOURCE_BARRIER pre[2] = {transition(hf, hf_state, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                                 transition(s.hudfree.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST)};
                c->list_gen->ResourceBarrier(2, pre);
                c->list_gen->CopyResource(s.hudfree.Get(), hf);
                D3D12_RESOURCE_BARRIER post[2] = {transition(hf, D3D12_RESOURCE_STATE_COPY_SOURCE, hf_state),
                                                  transition(s.hudfree.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
                c->list_gen->ResourceBarrier(2, post);
                gen_color = s.hudfree.Get();
                live_first("HUD-free picture used as Frame Generation's input", true, nullptr);
                s.hudfree_used = true;
                set_live_text(g_hudfree_text, "");
            } else {
                char text[112];
                snprintf(text, sizeof text, "No generated picture this frame: %s", why ? why : "the HUD-free picture is not ready.");
                set_live_text(g_hudfree_text, why ? why : "the picture without the HUD could not be made");
                fg::NotePaused(text);
                live_first("HUD-free picture used as Frame Generation's input", false, text);
                g_skip_hudfree.fetch_add(1, std::memory_order_relaxed);
                want_fg = false;
            }
        }
        c->list_gen->EndQuery(c->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, si * kQueriesPerSlot + kLiveQuery + 1);
    }
    uint32_t count = 0;
    if (want_fg) {
        fg::FrameInfo fi;
        fi.engine_frame=engine_frame;
        fi.color = gen_color;
        fi.format = c->desc.BufferDesc.Format;
        fi.width = c->desc.BufferDesc.Width;
        fi.height = c->desc.BufferDesc.Height;
        fi.hdr = a.hdr;
        fi.base_frame_ms = c->render_ms > 0.f ? c->render_ms : c->frame_ms; // A74 review: the game's own time, not our blocking
        float hz_override = g_display_hz_override.load(std::memory_order_relaxed);
        fi.display_hz = hz_override > 0.f ? hz_override : c->display_hz;
        fi.frame_id = c->frame_id;
        fi.queue = a.queue;
        fi.disable_flag = s.veto.Get();
        ID3D12Resource* outs[kMaxGenerated];
        for (uint32_t i = 0; i < kMaxGenerated; ++i) outs[i] = s.interp[i].Get();
        // Clear the veto (a runtime that never writes it must not veto by accident), then generate.
        {
            D3D12_RESOURCE_BARRIER b0 = transition(s.veto.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            c->list_gen->ResourceBarrier(1, &b0);
            // One 32-bit zero written by the command list itself (no upload heap needed).
            D3D12_WRITEBUFFERIMMEDIATE_PARAMETER wp{s.veto->GetGPUVirtualAddress(), 0u};
            D3D12_WRITEBUFFERIMMEDIATE_MODE wm = D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT;
            ComPtr<ID3D12GraphicsCommandList2> l2;
            if (SUCCEEDED(c->list_gen.As(&l2))) l2->WriteBufferImmediate(1, &wp, &wm);
            D3D12_RESOURCE_BARRIER b2 = transition(s.veto.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            c->list_gen->ResourceBarrier(1, &b2);
        }
        const uint32_t capacity = live ? na::fglive::kMaxGeneratedThisBuild : kMaxGenerated;
        count = c->external_owner ? 0 : fg::Generate(c->list_gen.Get(), c->device.Get(), fi, outs, capacity);
        if (live) {
            c->list_gen->EndQuery(c->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, si * kQueriesPerSlot + kLiveQuery + 2);
            // Still-area steadying (Astra): on the generated picture, before the HUD paste, from the two
            // consecutive real HUD-free frames. Only a valid pair: anything else shows the unmodified picture.
            s.steady_ran = false;
            const uint32_t picture = na::color::ShaderMode(na::color::ForStoredPicture(fi.format, fi.hdr)); // SDR / PQ / linear FP16
            if (count) {
                const fg::UsedMotion um = fg::LastUsedMotion();
                const uint64_t prev_id = c->steady_prev_id, epoch = g_hud_epoch.load(std::memory_order_relaxed);
                Slot& older = c->slots[(si + kSlots - 1) % kSlots];
                const char* drop = nullptr;
                if (!na::settings::Get().fg_steady.load(std::memory_order_relaxed)) { g_steady_off.fetch_add(1, std::memory_order_relaxed); }
                else if (!g_steady_ready.load(std::memory_order_acquire)) { g_drop_refused.fetch_add(1, std::memory_order_relaxed); drop = "refused"; }
                else if (c->steady_resized) { g_drop_resize.fetch_add(1, std::memory_order_relaxed); drop = "resize"; }
                else if (prev_id == UINT64_MAX || prev_id + 1 != c->frame_id || !older.hudfree) { g_drop_gap.fetch_add(1, std::memory_order_relaxed); drop = "gap"; }
                else if (c->steady_prev_epoch != epoch) { g_drop_relearn.fetch_add(1, std::memory_order_relaxed); drop = "HUD relearned"; }
                else if (um.reset || um.frame_id != c->frame_id) { g_drop_reset.fetch_add(1, std::memory_order_relaxed); drop = "history reset"; }
                else if (!um.filled) { g_drop_motion.fetch_add(1, std::memory_order_relaxed); drop = "no motion guide"; }
                else {
                    const uint32_t page = static_cast<uint32_t>(c->frame_id % na::fg::SteadyPass::kPages);
                    if (g_steady.Record(c->list_gen.Get(), outs[0], older.hudfree.Get(), s.hudfree.Get(), um.filled, fi.width, fi.height,
                                        um.mv_scale_x, um.mv_scale_y, page, picture)) {
                        s.steady_ran = true;
                        s.steady_page = page;
                        s.steady_pixels = fi.width * fi.height;
                        g_steady_frames.fetch_add(1, std::memory_order_relaxed);
                        live_first("still-area steadying applied", true, nullptr);
                    }
                }
                if (drop) live_first("still-area steadying used a frame pair", false, drop);
            }
            c->list_gen->EndQuery(c->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, si * kQueriesPerSlot + kLiveQuery + 3);
            // VARIANT: hudpaste/moves-with-scene (default) | hudpaste/screen-fixed
            // Interface that moves with the scene (map names drawn after the hand-over) is placed at each
            // generated picture's time; screen-fixed interface is pasted the T2 way, exactly, on top. Without a
            // continuous pair of real frames (or with the variant off) everything is pasted the T2 way.
            bool moving_paste = false;
            if (count) {
                const fg::UsedMotion um = fg::LastUsedMotion();
                Slot& older = c->slots[(si + kSlots - 1) % kSlots];
                const char* fallback = nullptr;
                if (!na::settings::Get().fg_hud_move.load(std::memory_order_relaxed)) fallback = "off";
                else if (!g_move_ready.load(std::memory_order_acquire)) fallback = "off";
                else if (c->steady_resized) fallback = "size";
                else if (c->steady_prev_id == UINT64_MAX || c->steady_prev_id + 1 != c->frame_id || !older.hudfree || !older.real_copy ||
                         c->steady_prev_epoch != g_hud_epoch.load(std::memory_order_relaxed)) fallback = "pair";
                else if (um.reset || um.frame_id != c->frame_id) fallback = "reset";
                else if (!um.filled) fallback = "motion";
                if (!fallback && g_move.Classify(c->list_gen.Get(), s.real_copy.Get(), s.hudfree.Get(), older.real_copy.Get(), older.hudfree.Get(),
                                                 um.filled, um.mv_scale_x, um.mv_scale_y, fi.width, fi.height, fi.format, picture, c->frame_id)) {
                    moving_paste = true;
                    for (uint32_t i = 0; i < count && moving_paste; ++i)
                        moving_paste = g_move.Paste(c->list_gen.Get(), outs[i], s.real_copy.Get(), s.hudfree.Get(), um.filled, um.mv_scale_x,
                                                    um.mv_scale_y, float(i + 1) / float(count + 1), fi.width, fi.height, fi.format, picture);
                    if (!moving_paste) { count = 0; live_first("HUD pasted onto the generated picture", false, "the moving-interface paste failed; the picture is not shown"); }
                    else live_first("interface that moves with the scene placed at the generated picture's time", true, nullptr);
                } else {
                    g_move.Forget();
                    g_move.NoteFallback(fallback ? fallback : "size");
                }
            }
            // T2: the newer real frame's HUD, exactly, through its own HUD mask.
            // A generated picture whose HUD could not be pasted is never shown (it would carry a warped HUD).
            for (uint32_t i = 0; i < count && !moving_paste; ++i)
                if (!g_paste.Record(c->list_gen.Get(), outs[i], s.real_copy.Get(), s.hudfree.Get(), fi.width, fi.height, fi.format,
                                    static_cast<uint32_t>((c->frame_id * kMaxGenerated + i) % na::fg::HudPaste::kPages), picture)) {
                    live_first("HUD pasted onto the generated picture", false, "the paste pass is not ready; the picture is not shown");
                    count = 0;
                    break;
                }
            if (count) {
                live_first("HUD pasted onto the generated picture", true, nullptr);
                g_last_generated_tick.store(GetTickCount64(), std::memory_order_relaxed);
            }
        }
        if (count) { // copy the runtime's answer out for the present thread
            D3D12_RESOURCE_BARRIER b3 = transition(s.veto.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            c->list_gen->ResourceBarrier(1, &b3);
            c->list_gen->CopyBufferRegion(s.veto_readback.Get(), 0, s.veto.Get(), 0, 16);
            D3D12_RESOURCE_BARRIER b4 = transition(s.veto.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            c->list_gen->ResourceBarrier(1, &b4);
        }
    }
    c->list_gen->EndQuery(c->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, si * kQueriesPerSlot + 1);
    c->list_gen->ResolveQueryData(c->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, si * kQueriesPerSlot, 2, c->query_readback.Get(), si * kQueriesPerSlot*sizeof(UINT64));
    if (live) {
        if (!count) { // nothing was evaluated: the split marks still need values to resolve
            c->list_gen->EndQuery(c->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, si * kQueriesPerSlot + kLiveQuery + 2);
            c->list_gen->EndQuery(c->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, si * kQueriesPerSlot + kLiveQuery + 3);
            s.hudfree_used = false;
            s.steady_ran = false;
        }
        c->list_gen->ResolveQueryData(c->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, si * kQueriesPerSlot + kLiveQuery, 4, c->query_readback.Get(),
                                      (si * kQueriesPerSlot + kLiveQuery) * sizeof(UINT64));
    }
    c->list_gen->Close();
    if (live) { // this slot's HUD-free picture is the next frame's older one (only when it was made this frame)
        c->steady_prev_id = s.hudfree_used ? c->frame_id : UINT64_MAX;
        c->steady_prev_epoch = g_hud_epoch.load(std::memory_order_relaxed);
        if (s.hudfree_used) c->steady_resized = false;
    }
    apply_latency_locked(*c, count);
    ID3D12CommandList* lists[] = {c->list_gen.Get()};
    a.queue->ExecuteCommandLists(1, lists);
    UINT64 gen_value = signal_progress(*c, a.queue);
    s.gen_fence.store(gen_value, std::memory_order_release);
    c->frame_id++;
    c->virtual_index = (c->virtual_index + 1) % static_cast<UINT>(c->proxies.size());

    // Keep one owner even for menu frames, a runtime veto, or FG being turned
    // off. Those are zero-interpolation jobs, not a thread handoff in DXGI.
    g_thread_mode.store(1, std::memory_order_relaxed);
    Job job;
    na::latency::EngineMark(engine_frame, na::latency::Phase::RenderEnd);
    job.engine_frame = engine_frame;
    job.slot = si;
    job.count = count;
    job.sync = a.sync;
    job.flags = a.flags;
    job.arrival = now.QuadPart;
    job.frame_ms = c->frame_ms > 0 ? c->frame_ms : 16.7f;
    job.queue = a.queue;
    job.frame_id = c->frame_id - 1; // incremented above
    {
        std::unique_lock<std::mutex> jl(c->jm);
        LARGE_INTEGER b0, b1;
        QueryPerformanceCounter(&b0);
        c->jcv.wait_for(jl, std::chrono::milliseconds(1000), [&] { return c->job_w - c->job_r < kJobs; });
        QueryPerformanceCounter(&b1);
        c->last_blocked_ms += 1000.f * static_cast<float>(b1.QuadPart - b0.QuadPart) / static_cast<float>(c->qpc.QuadPart);
        if (c->job_w - c->job_r >= kJobs) {
            g_dropped.fetch_add(1, std::memory_order_relaxed);
            jl.unlock();
            auto pq=display_queue(*c);
            if(c->queues_bound) pq->Wait(c->fence.Get(),gen_value);
            release_slot(*c, s, pq);
            return DXGI_ERROR_WAS_STILL_DRAWING;
        }
        c->jobs[c->job_w % kJobs] = job;
        ++c->job_w;
        ++c->backlog;
    }
    c->jcv.notify_all();
    return c->last_hr.load();
}

Stats GetStats()
{
    Stats s{};
    s.enabled = Enabled() ? 1u : 0u;
    s.proxied = g_proxied.load(std::memory_order_relaxed);
    s.thread_mode = g_thread_mode.load(std::memory_order_relaxed);
    s.real_present_thread = g_real_present_caller.load();
    uint32_t backlog = 0;
    for (Chain& c : g_chains) {
        std::lock_guard<std::mutex> lock(c.jm);
        backlog += c.backlog;
    }
    s.backlog = backlog;
    s.presents_game = g_presents_game.load(std::memory_order_relaxed);
    s.presents_real = g_presents_real.load(std::memory_order_relaxed);
    s.generated_shown = g_generated_shown.load(std::memory_order_relaxed);
    s.dropped = g_dropped.load(std::memory_order_relaxed);
    s.late = g_late.load(std::memory_order_relaxed);
    s.vetoed = g_vetoed.load(std::memory_order_relaxed);
    s.generated_dropped = g_generated_dropped.load(std::memory_order_relaxed);
    s.chain_releases = g_chain_releases.load(std::memory_order_relaxed);
    s.orphaned = g_orphaned.load(std::memory_order_relaxed);
    for (Chain& c : g_chains) // A74: diagnostics only (no lock): which kind of chain the game made
        if (c.sc && c.ready) { s.chain_flags = c.desc.Flags; break; }
    for (Chain& c : g_chains) {
        std::unique_lock<std::mutex> lock(c.m, std::try_to_lock);
        if (!lock.owns_lock() || !c.ready) continue;
        s.gpu_submitted = c.last_signal.load();
        s.gpu_completed = c.fence ? c.fence->GetCompletedValue() : 0;
        s.queried_latency_handle = reinterpret_cast<uintptr_t>(c.queried_latency_handle);
        s.independent_queue=c.queues_bound;
        s.device_hr = c.device ? c.device->GetDeviceRemovedReason() : S_OK;
        if (s.device_hr != S_OK) na::diag::ReportIfRemoved(c.device.Get());
        for (Slot& slot : c.slots) if (slot.owned.load()) ++s.slots_owned;
        break;
    }
    { std::lock_guard<std::mutex> metrics(g_metrics_m);
      s.submit_interval = g_intervals.Read(); s.present_call = g_present_calls.Read();
      s.ready_age = g_ready_ages.Read(); s.real_age = g_real_ages.Read();
      s.gpu_first_copy=g_gpu_first_copy.Read(); s.gpu_copy_interval=g_gpu_copy_interval.Read(); }
    s.requested_sync = g_requested_sync.load(); s.requested_flags = g_requested_flags.load();
    s.base_fps = g_base_fps.load();
    s.shown_fps = g_shown_fps.load();
    s.display_hz = g_display_hz.load();
    s.fg_ms = g_fg_ms.load();
    s.fg_avg_ms = g_fg_avg_ms.load();
    s.scaled_w = g_scaled_w.load(); s.scaled_h = g_scaled_h.load();
    s.native_w = g_native_w.load(); s.native_h = g_native_h.load();
    s.upscaled = g_upscaled.load(); s.upscale_failed = g_upscale_failed.load();
    {
        std::lock_guard<std::mutex> lock(g_reason_m);
        strncpy_s(s.reason, g_reason, _TRUNCATE);
    }
    return s;
}

namespace {
// Releases everything of one chain (caller holds c.m). false = the present
// thread did not stop within the timeout (state retained; call again).
bool teardown_chain_locked(Chain& c, uint32_t timeout_ms)
{
    if (c.live_prep.joinable()) c.live_prep.join(); // it writes the slots' textures
    if (!drain_locked(c, timeout_ms)) return false;
    if (!stop_thread(c, timeout_ms)) return false;
    if (c.cap.maker.joinable()) c.cap.maker.join(); // F8 capture: an unfinished burst is dropped
    c.cap.shots.clear();
    c.cap.used = 0;
    c.cap.state = 0;
    if (!wait_fence(c, c.last_signal.load(), timeout_ms) || !wait_presentation(c,c.presentation_last.load(),timeout_ms)) return false;
    if (!c.proxies.empty()) g_proxied.fetch_sub(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> buffers_lock(c.buffers_m);
        c.proxy_count.store(0,std::memory_order_release);
        c.proxies.clear();
    }
    if (c.scaled) { g_scaled_w.store(0); g_scaled_h.store(0); c.scaled = false; }
    c.resize_proxies.clear();c.external_owner=false;
    c.uav_ok = false;
    for (Slot& s : c.slots) {
        s.real_copy.Reset();
        for (auto& t : s.interp) t.Reset();
        s.veto.Reset();
        s.veto_readback.Reset();
        s.alloc_gen.Reset();
        for (auto& a : s.alloc_present) a.Reset();
        s.free_fence = 0;
        s.gen_fence = 0;
        s.owned = false;
    }
    c.list_gen.Reset();
    c.list_present.Reset();
    c.queries.Reset();
    c.query_readback.Reset();
    c.off_queries.Reset();
    c.off_readback.Reset();
    c.live_ready.store(false);
    c.worker_owner = false;
    for (Slot& s : c.slots) { s.hudfree.Reset(); s.off_timed = false; s.live_split = false; }
    c.fence.Reset();
    c.presentation_fence.Reset(); c.presentation_queue.Reset();
    g_private_queues[&c-g_chains].store(nullptr,std::memory_order_release);
    c.presentation_next=1; c.presentation_last=0; c.queues_bound=false;
    if (c.fence_event) CloseHandle(c.fence_event);
    c.fence_event = nullptr;
    c.fence_next = 1;
    c.last_signal = 0;
    c.queue.Reset();
    // A75 review: our own Release of the chain must not come back into this
    // module. The slot is unregistered FIRST (find_chain can no longer match)
    // and the thread-local guard tells the overlay's Release detour to pass
    // straight through, whatever the count does; relocking c.m here would be
    // a deadlock (Shutdown with sc3 as the last reference).
    {
        std::lock_guard<std::mutex> gl(g_chains_m);
        c.sc = nullptr;
    }
    t_teardown_release = true;
    c.sc3.Reset(); // our one reference on the chain object
    t_teardown_release = false;
    c.device.Reset();
    c.queried_latency_handle = nullptr; // borrowed native identity, no waits or signals
    c.ready = false;
    c.queues_bound=false;
    c.virtual_index = 0;
    c.frame_id = 0;
    c.worker_owner = false;
    c.game_latency = 0;
    c.latency_applied = 0;
    c.frame_ms = c.shown_ms = 0.f;
    c.last_arrival.QuadPart = c.last_shown.QuadPart = 0;
    c.job_w = c.job_r = c.backlog = 0;
    {
        std::lock_guard<std::mutex> gl(g_chains_m);
        c.sc = nullptr;
    }
    return true;
}
} // namespace

// A71 (Astra B-007): how many references THIS module holds on the chain
// object itself: one (`sc3`) once the presenter is set up on it, else none.
// No back buffer is retained (see ensure_ready_locked), so when the game's
// Release leaves exactly this many, only NeuralAlias keeps the chain alive.
uint32_t HeldReferences(IDXGISwapChain* sc)
{
    if (!sc) return 0;
    std::lock_guard<std::mutex> lock(g_chains_m);
    Chain* c = find_chain(sc);
    return c && c->sc3 ? 1u : 0u;
}

// The game let go of `sc` (its Release returned HeldReferences(sc)): drop our
// reference(s) so the object is destroyed now, before the game creates a
// replacement chain for the same window. Runs on the game's thread inside
// its Release; the present thread is stopped first (it may be inside a
// Present on this chain), bounded by `timeout_ms`.
bool ReleaseChain(IDXGISwapChain* sc, uint32_t timeout_ms)
{
    if (!sc) return false;
    Chain* c = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_chains_m);
        c = find_chain(sc);
    }
    if (!c) return false;
    std::lock_guard<std::mutex> lock(c->m);
    if (c->sc != sc) return false;
    bool ok = teardown_chain_locked(*c, timeout_ms);
    if (ok) g_chain_releases.fetch_add(1, std::memory_order_relaxed);
    else if (!c->orphaned) { c->orphaned = true; g_orphaned.fetch_add(1, std::memory_order_relaxed); } // A75: retried later
    return ok;
}

bool InTeardownRelease() { return t_teardown_release; }

// A75 review: a chain whose teardown timed out (present thread stuck in a
// blocking Present of an occluded window, say) stayed alive through our
// reference for ever and the game's next same-window chain would be refused
// again. Retried from every later present of any chain and from Shutdown.
void RetryOrphans()
{
    for (Chain& c : g_chains) {
        if (!c.orphaned || !c.sc) continue;
        std::unique_lock<std::mutex> lock(c.m, std::try_to_lock);
        if (!lock.owns_lock()) continue;
        if (teardown_chain_locked(c, 100)) {
            c.orphaned = false;
            g_chain_releases.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// A chain that was destroyed without us holding anything (never set up, or
// NoteGameLatency saw it first): forget the slot.
void ForgetChain(IDXGISwapChain* sc)
{
    if (!sc) return;
    Chain* c = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_chains_m);
        c = find_chain(sc);
    }
    if (!c) return;
    std::lock_guard<std::mutex> lock(c->m);
    if (c->sc != sc || c->sc3) return; // set up chains go through ReleaseChain
    teardown_chain_locked(*c, 1000);
}

void SetDisplayHzForTest(float hz) { g_display_hz_override.store(hz, std::memory_order_relaxed); }
void SetWorkerGateForTest(HANDLE entered, HANDLE resume)
{
    g_test_worker_entered.store(entered);
    g_test_worker_resume.store(resume, std::memory_order_release);
}

bool Shutdown(uint32_t timeout_ms)
{
    bool ok = true;
    for (Chain& c : g_chains) {
        if (!c.sc) continue;
        std::lock_guard<std::mutex> lock(c.m);
        if (!teardown_chain_locked(c, timeout_ms)) { ok = false; continue; }
    }
    if (ok) {
        fg::Shutdown();
        g_proxied.store(0, std::memory_order_relaxed);
        g_thread_mode.store(0, std::memory_order_relaxed);
    }
    return ok;
}


} // namespace na::present
