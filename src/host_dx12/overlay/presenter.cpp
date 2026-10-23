#include "host_dx12/overlay/presenter.hpp"

#include <windows.h>
#include <timeapi.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "host_dx12/fg/frame_gen.hpp"
#include "host_dx12/settings.hpp"
#include "host_dx12/latency/nvidia_latency.hpp"
#include "host_dx12/adapters/tlou1/timing.hpp"

#pragma comment(lib, "winmm.lib")

using Microsoft::WRL::ComPtr;

namespace na::present {
namespace {

constexpr uint32_t kSlots = 3;        // real frames in flight through the presenter
constexpr uint32_t kMaxGenerated = 5; // 6x
constexpr uint32_t kPresentsPerSlot = kMaxGenerated + 1;
constexpr uint32_t kQueriesPerSlot = 2 + kPresentsPerSlot;
constexpr uint32_t kJobs = 4;
constexpr uint32_t kChains = 2;

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
};

struct Job {
    uint32_t slot = 0;
    uint32_t count = 0;
    uint64_t engine_frame = 0;
    UINT sync = 0;
    UINT flags = 0;
    LONGLONG arrival = 0;
    float frame_ms = 0;
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
    UINT virtual_index = 0;
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
    bool worker_owner = false; // once FG starts, real presentation stays on this worker
    HANDLE queried_latency_handle = nullptr; // diagnostics: query only, never wait/signal
    bool uav_ok = false;
    uint64_t frame_id = 0;
    LARGE_INTEGER last_arrival{}, last_shown{}, qpc{};
    float frame_ms = 0.f, shown_ms = 0.f;
    float avail_ms = 0.f;               // A73: smoothed time from the game's present to its pictures being ready (present thread only)
    float render_ms = 0.f;              // A74 review: the game's own interval EXCLUDING time it spent blocked inside our Present (Frame Generation's base rate)
    float last_blocked_ms = 0.f;        // time the previous Present spent waiting on our slot ring / job queue
    bool orphaned = false;              // A75: the game let go but our teardown timed out; retried from later presents / Shutdown
    uint32_t hitch_skips = 0;
    float display_hz = 0.f;
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

float refresh_hz_for(HWND hwnd)
{
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!mon || !GetMonitorInfoW(mon, &mi)) return 0.f;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) return 0.f;
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
    if (!c.proxies.empty()) return true;
    if (FAILED(c.sc->GetDesc(&c.desc))) return false;
    if (!c.device && FAILED(c.sc->GetDevice(IID_PPV_ARGS(&c.device)))) return false;
    UINT n = c.desc.BufferCount ? c.desc.BufferCount : 2;
    c.uav_ok = uav_supported(c, c.desc.BufferDesc.Format);
    c.proxies.resize(n);
    for (UINT i = 0; i < n; ++i) {
        if (!create_texture(c, c.desc.BufferDesc.Width, c.desc.BufferDesc.Height, c.desc.BufferDesc.Format, c.uav_ok, true,
                            D3D12_RESOURCE_STATE_PRESENT, c.proxies[i])) {
            c.proxies.clear();
            return false;
        }
        wchar_t name[48];
        swprintf_s(name, L"NeuralAlias proxy back buffer %u", i);
        c.proxies[i]->SetName(name);
    }
    c.virtual_index = 0;
    c.display_hz = refresh_hz_for(c.desc.OutputWindow);
    g_display_hz.store(c.display_hz);
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
    if(!c.queues_bound && g_orig.resize_queues && g_orig.execute) {
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
    for (Slot& s : c.slots) {
        if (!create_texture(c, c.desc.BufferDesc.Width, c.desc.BufferDesc.Height, c.desc.BufferDesc.Format, c.uav_ok, false,
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

void release_size_dependent_locked(Chain& c)
{
    if (!drain_locked(c, 2000) || !wait_fence(c, c.last_signal.load(), 2000) || !wait_presentation(c,c.presentation_last.load(),2000)) return;
    if (!c.proxies.empty()) g_proxied.fetch_sub(1, std::memory_order_relaxed);
    c.proxies.clear();
    for (Slot& s : c.slots) {
        s.real_copy.Reset();
        for (auto& t : s.interp) t.Reset();
        s.free_fence = 0;
        s.gen_fence = 0;
        s.owned = false;
    }
    c.ready = false;
    c.queues_bound=false;
    c.virtual_index = 0;
}

// Copies `tex` into the real current back buffer and presents. `state` is
// the texture's resting state. Runs on whichever thread calls it.
// `no_wait` (A71): present with DXGI_PRESENT_DO_NOT_WAIT; when the chain's
// present queue is full the call returns DXGI_ERROR_WAS_STILL_DRAWING at once
// instead of blocking this thread, and nothing is counted as presented.
HRESULT present_texture(Chain& c, ID3D12CommandQueue* queue, ID3D12Resource* tex, D3D12_RESOURCE_STATES state,
                        ID3D12CommandAllocator* alloc, UINT sync, UINT flags, bool no_wait = false, uint64_t engine_frame = 0,
                        UINT query = UINT_MAX)
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
    D3D12_RESOURCE_BARRIER pre[2] = {transition(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST),
                                     transition(tex, state, D3D12_RESOURCE_STATE_COPY_SOURCE)};
    c.list_present->ResourceBarrier(2, pre);
    c.list_present->CopyResource(bb, tex);
    D3D12_RESOURCE_BARRIER post[2] = {transition(bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT),
                                      transition(tex, D3D12_RESOURCE_STATE_COPY_SOURCE, state)};
    c.list_present->ResourceBarrier(2, post);
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
                if (whole_step_late) { g_generated_dropped.fetch_add(1, std::memory_order_relaxed); continue; }
                HRESULT ghr = present_texture(c, pq, s.interp[i].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, s.alloc_present[i].Get(),
                                              job.sync, job.flags, true, job.engine_frame, job.slot*kQueriesPerSlot+2+i);
                if (SUCCEEDED(ghr) || ghr==DXGI_ERROR_WAS_STILL_DRAWING) s.copied_mask |= 1u<<i;
                if (FAILED(ghr)) g_generated_dropped.fetch_add(1, std::memory_order_relaxed);
                else g_generated_shown.fetch_add(1, std::memory_order_relaxed);
            } else {
                HRESULT rhr=present_texture(c, pq, s.real_copy.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, s.alloc_present[i].Get(),
                                job.sync, job.flags, false, job.engine_frame, job.slot*kQueriesPerSlot+2+i);
                if (SUCCEEDED(rhr)) s.copied_mask |= 1u<<i;
            }
        }
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
    return c && !c->proxies.empty();
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
    std::lock_guard<std::mutex> lock(c->m);
    if (!ensure_proxies_locked(*c)) return false; // fall back to the original buffers
    if (index >= c->proxies.size()) { *hr = DXGI_ERROR_INVALID_CALL; *out = nullptr; return true; }
    *hr = c->proxies[index]->QueryInterface(riid, out);
    return true;
}

bool HandleGetIndex(IDXGISwapChain* sc, UINT* out)
{
    if (!Enabled() || !sc || !out) return false;
    std::lock_guard<std::mutex> lock(g_chains_m);
    Chain* c = find_chain(sc);
    if (!c || c->proxies.empty()) return false;
    *out = c->virtual_index;
    return true;
}

void BeforeResize(IDXGISwapChain* sc)
{
    if (!Enabled() || !sc) return;
    Chain* c = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_chains_m);
        c = find_chain(sc);
    }
    if (!c) return;
    std::lock_guard<std::mutex> lock(c->m);
    release_size_dependent_locked(*c);
    fg::Shutdown(); // the feature is bound to the old size; it is re-created on the next frame
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
    if (a.menu) a.menu(a.sc, a.queue, src, a.user);

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
    bool fg_on = na::settings::Get().fg_enabled.load(std::memory_order_relaxed) != 0;
    if (fg_on) c->worker_owner = true;
    bool want_fg = c->uav_ok && !a.menu_open && fg_on;
    // A73: say WHY nothing is generated right now instead of leaving the last
    // message standing (session 13: "starting" stayed on screen because the
    // menu that showed it also pauses generation).
    if (fg_on && !want_fg) {
        if (!c->uav_ok) fg::NotePaused("Frame Generation is paused: this picture format allows no generated frames.");
        else fg::NotePaused("Frame Generation is paused while this menu is open; it resumes the moment you close it (Home).");
    }
    uint32_t backlog_now;
    {
        std::lock_guard<std::mutex> jl(c->jm);
        backlog_now = c->backlog;
    }
    if (!c->worker_owner && !want_fg && backlog_now == 0) {
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
        HRESULT hr = present_texture(*c, pq, src, D3D12_RESOURCE_STATE_PRESENT, s.alloc_present[0].Get(), a.sync, a.flags, false, engine_frame);
        release_slot(*c, s, pq);
        return hr;
    }

    // Copy the finished frame into our texture; then generate.
    s.alloc_gen->Reset();
    c->list_gen->Reset(s.alloc_gen.Get(), nullptr);
    c->list_gen->EndQuery(c->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, si * kQueriesPerSlot);
    D3D12_RESOURCE_BARRIER pre[2] = {transition(src, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                     transition(s.real_copy.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST)};
    c->list_gen->ResourceBarrier(2, pre);
    c->list_gen->CopyResource(s.real_copy.Get(), src);
    D3D12_RESOURCE_BARRIER post[2] = {transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT),
                                      transition(s.real_copy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
    c->list_gen->ResourceBarrier(2, post);
    uint32_t count = 0;
    if (want_fg) {
        fg::FrameInfo fi;
        fi.engine_frame=engine_frame;
        fi.color = s.real_copy.Get();
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
        count = fg::Generate(c->list_gen.Get(), c->device.Get(), fi, outs, kMaxGenerated);
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
    c->list_gen->Close();
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
    if (!drain_locked(c, timeout_ms)) return false;
    if (!stop_thread(c, timeout_ms)) return false;
    if (!wait_fence(c, c.last_signal.load(), timeout_ms) || !wait_presentation(c,c.presentation_last.load(),timeout_ms)) return false;
    if (!c.proxies.empty()) g_proxied.fetch_sub(1, std::memory_order_relaxed);
    c.proxies.clear();
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
