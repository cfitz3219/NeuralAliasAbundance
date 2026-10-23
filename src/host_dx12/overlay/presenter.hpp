// Presenter (A59): NeuralAlias owns the game's swap chain so it can show
// more pictures than the game renders (Frame Generation).
//
// How: the game never touches the real back buffers. IDXGISwapChain::GetBuffer
// hands it NeuralAlias-owned textures ("proxies", same size/format), and
// GetCurrentBackBufferIndex reports a rotation that advances by exactly one
// per game Present, whatever the real chain does. At the game's Present the
// finished proxy is copied into a NeuralAlias texture on the game's direct
// queue, Frame Generation records its extra pictures right after it, and a
// present thread copies each picture into the real current back buffer and
// calls the original Present, spaced evenly over the measured frame interval.
// The game's thread is never blocked by the presents (only by back-pressure
// when it runs more than the ring's depth ahead).
//
// Before Frame Generation is first enabled, ordinary frames may present
// inline. After it is enabled, the worker remains the sole presenter for
// this chain, including menus, vetoes and FG-off frames. Menu drawing happens
// on the game's finished proxy before the copy to the owned job texture.
//
// This layer is enabled by the host BEFORE the game creates its swap chain
// (SetEnabled); Route B does not enable it and keeps the old path. Hooks are
// installed by overlay.cpp, which routes GetBuffer / GetCurrentBackBufferIndex /
// ResizeBuffers / Present here and supplies the ORIGINAL entry points.
#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdint>
#include "host_dx12/latency/policy.hpp"

namespace na::present {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using GetBufferFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, REFIID, void**);
using GetIndexFn = UINT(STDMETHODCALLTYPE*)(IDXGISwapChain3*);
using SetMaxLatencyFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain2*, UINT);
using GetLatencyObjectFn = HANDLE(STDMETHODCALLTYPE*)(IDXGISwapChain2*);
using ResizeQueuesFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*,UINT,UINT,UINT,DXGI_FORMAT,UINT,const UINT*,IUnknown* const*);
using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*);
struct Originals {
    PresentFn present = nullptr;
    Present1Fn present1 = nullptr;
    GetBufferFn get_buffer = nullptr;
    GetIndexFn get_index = nullptr;
    SetMaxLatencyFn set_max_latency = nullptr;
    GetLatencyObjectFn get_latency_object = nullptr;
    ResizeQueuesFn resize_queues = nullptr;
    ExecuteFn execute = nullptr;
};
void SetOriginals(const Originals& o);
// A63: the game's own maximum frame latency for `sc` (from its
// SetMaximumFrameLatency call). While pictures are being generated the real
// chain is given room for them (game value + count); with nothing generated
// the game's value stands, so the game never runs further ahead than it
// asked to (session 11: a blanket +5 let the game queue up to 8 frames,
// which starved Neural Rendering's ring and made it skip alternate frames).
void NoteGameLatency(IDXGISwapChain* sc, UINT n);
// A76: the maximum frame latency the presenter has applied to `sc` (0 = none
// / unknown chain). The real chain's value is never lowered below it: the
// overlay forwards a game's SetMaximumFrameLatency(n) as max(n, floor).
UINT LatencyFloor(IDXGISwapChain* sc);

void SetEnabled(bool enabled);
bool Enabled();

// Overlay detours call these first. They return true when the presenter
// handled the call (proxy mode active for `sc`); otherwise the detour
// forwards to the original.
bool HandleGetBuffer(IDXGISwapChain* sc, UINT index, REFIID riid, void** out, HRESULT* hr);
bool HandleGetIndex(IDXGISwapChain* sc, UINT* out);
// Before the original ResizeBuffers: drains the present thread, releases
// proxies and cached real buffers. The game must have released its proxy
// references (it releases its "back buffers" before resizing, as DXGI requires).
void BeforeResize(IDXGISwapChain* sc);
bool Proxied(IDXGISwapChain* sc);

// Called on the finished frame (the proxy the game just presented; PRESENT
// state) for Neural Rendering, motion analysis and capture. Records and
// executes on `queue` as those passes always did.
using FrameFn = void (*)(ID3D12CommandQueue* queue, ID3D12Resource* finished_frame, void* user);
// Composite the menu onto the finished proxy (PRESENT state, restore that
// state). Called on the game thread after FrameFn, before the job's copy;
// never called by the worker, and `user` need only live until Present returns.
using MenuFn = void (*)(IDXGISwapChain* sc, ID3D12CommandQueue* queue, ID3D12Resource* frame, void* user);

struct PresentArgs {
    IDXGISwapChain* sc = nullptr;
    UINT sync = 0;
    UINT flags = 0;
    const DXGI_PRESENT_PARAMETERS* pp = nullptr; // Present1 only
    ID3D12CommandQueue* queue = nullptr;         // the game's direct queue
    bool hdr = false;                            // colour buffer is HDR (for Frame Generation)
    bool menu_open = false;
    FrameFn frame = nullptr;
    MenuFn menu = nullptr;
    void* user = nullptr;
};
// The game's Present in proxy mode (caller checked Proxied(sc)).
HRESULT Present(const PresentArgs& a);

struct Stats {
    uint32_t enabled;
    uint32_t proxied;          // chains currently served through proxies
    uint32_t thread_mode;      // 1 = the present thread is showing frames right now
    uint32_t real_present_thread;
    uint32_t backlog;          // jobs queued for the present thread
    uint64_t presents_game;    // Presents the game made
    uint64_t presents_real;    // Presents made on the real chain (real + generated)
    uint64_t generated_shown;  // generated pictures shown
    uint64_t dropped;          // real frames shown without generation because the ring was busy
    uint64_t late;             // presents that missed their pacing slot by more than 2 ms
    uint64_t vetoed;           // A62: frames whose generated pictures the runtime asked us not to show (menu detection)
    uint64_t generated_dropped; // A71: generated pictures not shown (a whole pacing step late, or the present queue was full)
    uint32_t chain_releases;   // A71: chains torn down because the game released its last reference
    uint32_t chain_flags;      // A74: DXGI_SWAP_CHAIN_FLAG_* of the served chain (0x40 = frame-latency waitable object)
    uint32_t orphaned;         // A75: teardowns that timed out and were left for retry
    uint64_t gpu_submitted, gpu_completed, queried_latency_handle;
    uint32_t slots_owned;
    uint32_t independent_queue;
    HRESULT device_hr;
    na::latency::Distribution submit_interval, present_call, ready_age, real_age;
    na::latency::Distribution gpu_first_copy, gpu_copy_interval;
    uint32_t requested_sync = 0, requested_flags = 0;
    float base_fps;            // game's own rate
    float shown_fps;           // rate on the real chain
    float display_hz;          // refresh rate of the display the game is on (0 unknown)
    float fg_ms, fg_avg_ms;    // GPU time of the copy + generation work per real frame
    char reason[112];
};
Stats GetStats();

bool IsPresentThread();
bool OwnsQueue(ID3D12CommandQueue* queue);
// Stops the thread(s) and releases everything. false = a thread did not
// exit within the timeout (state retained; call again).
bool Shutdown(uint32_t timeout_ms = 5000);

// A71 (Astra B-007, same-window swap-chain replacement): the presenter never
// retains the real back buffers and holds exactly one reference on a chain
// it is set up on (`HeldReferences` = 1; 0 otherwise). The overlay's Release
// detour compares the count the game's Release left with this value and, when
// they match, calls `ReleaseChain` so the object is destroyed right then, on
// the game's thread, before it creates a replacement for the same window.
// `ForgetChain` clears a slot for a chain that died while we held nothing.
uint32_t HeldReferences(IDXGISwapChain* sc);
bool ReleaseChain(IDXGISwapChain* sc, uint32_t timeout_ms = 2000);
void ForgetChain(IDXGISwapChain* sc);
// A75: true on the thread that is releasing a chain object from inside the
// presenter's own teardown (the Release detour must pass straight through).
bool InTeardownRelease();
// A75: retries the teardown of chains whose teardown timed out earlier
// (called from every present of any chain, and from Shutdown).
void RetryOrphans();

// A71, tests only: the refresh rate handed to Frame Generation as the
// ceiling for generated pictures (0 = the display's real rate). Pacing tests
// that need 3x-6x at a 50-100 fps base set a high value; the display-cap test
// uses the real one.
void SetDisplayHzForTest(float hz);
// Test-only gate at worker job acquisition. Null disables it. Handles remain
// owned by the fixture and must outlive every admitted job.
void SetWorkerGateForTest(HANDLE entered, HANDLE resume);

} // namespace na::present
