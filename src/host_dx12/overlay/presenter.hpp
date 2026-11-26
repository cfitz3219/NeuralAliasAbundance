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

// P4-4 render-small ("native present", plan COMMS 4e30a94/f0de8b4). With a scale below 1
// the game's proxies are render size (round(real * scale)) while the real chain stays at
// the window's size, and every finished render-size picture goes through `upscale` into the
// native slot texture before the menu, Frame Generation and presentation see it. The scale
// applies when proxies are next created (the first GetBuffer, or after a resize); a chain
// whose format cannot be written by a shader stays unscaled. 1 = off (default).
// `frame` is in PRESENT state and must be returned in it; `out` (render-target and UAV
// capable) arrives in NON_PIXEL_SHADER_RESOURCE and must be left in PRESENT (COMMON) state
// when the call returns true; on false it must be left untouched (the slot keeps its old picture).
using UpscaleFn = bool (*)(ID3D12CommandQueue* queue, ID3D12Resource* frame, uint32_t rw, uint32_t rh,
                           ID3D12Resource* out, uint32_t ow, uint32_t oh, bool hdr, void* user);
void SetRenderScale(float scale, UpscaleFn upscale, void* user);
float RenderScale();
// [present] present_dlaa=1: at scale 1.0 the presenter still hands the frame to the upscaler
// (render == output, DLSS runs as DLAA at Present). Set before the swap chain's first GetBuffer.
void SetPresentDlaa(bool on);
bool PresentDlaa();
// True while the served chain's proxies are render size (J2 then defers DLSS to Present).
bool Scaling();
// The buffer size the game is served for `sc` (render size while scaled). false = this
// chain has no proxies from us.
bool GameBufferSize(IDXGISwapChain* sc, UINT* w, UINT* h);
// A resize while scaled: the game asks for the size it believes; the real chain must
// follow the window instead. Rewrites w/h to 0 (DXGI: the window's client size) when
// scaling is on. Call before the original ResizeBuffers / ResizeBuffers1.
void TranslateResize(IDXGISwapChain* sc, UINT* w, UINT* h);

// Overlay detours call these first. They return true when the presenter
// handled the call (proxy mode active for `sc`); otherwise the detour
// forwards to the original.
bool HandleGetBuffer(IDXGISwapChain* sc, UINT index, REFIID riid, void** out, HRESULT* hr);
bool HandleGetIndex(IDXGISwapChain* sc, UINT* out);
// Before the original ResizeBuffers: drains the present thread, releases
// proxies and cached real buffers. The game must have released its proxy
// references (it releases its "back buffers" before resizing, as DXGI requires).
bool BeforeResize(IDXGISwapChain* sc); // false: keep old images/size; submitted work has not retired
bool Proxied(IDXGISwapChain* sc);
// A known external presenter may not acquire new NeuralAlias buffers. Already
// acquired proxies keep their delivery path until a successful resize.
bool NoteExternalOwner(IDXGISwapChain* sc);
void AfterResize(IDXGISwapChain* sc,HRESULT result);

// Called on the finished frame (the proxy the game just presented; PRESENT
// state) for Neural Rendering, motion analysis and capture. Records and
// executes on `queue` as those passes always did.
using FrameFn = void (*)(ID3D12CommandQueue* queue, ID3D12Resource* finished_frame, void* user);
// Composite the menu onto the finished proxy (PRESENT state, restore that
// state). Called on the game thread after FrameFn, before the job's copy;
// never called by the worker, and `user` need only live until Present returns.
using MenuFn = void (*)(IDXGISwapChain* sc, ID3D12CommandQueue* queue, ID3D12Resource* frame, void* user);

// Route B live Frame Generation (2026-11-26). The chain is proxied from its creation (SetEnabled
// before the game creates it) so Frame Generation can be switched on and off with no restart.
// While it is Off the game's own thread presents each frame inline (one copy from the proxy, no
// present thread); its GPU cost is measured every frame. Turning it on prepares the extra
// textures on a background thread while Off stays on screen; the presenter thread takes over
// once they exist, and hands back after the last queued picture when it is turned off again.
// Frame Generation gets the HUD-free picture (HudFreeFn) as its back buffer, and the newer real
// frame's HUD is pasted onto each generated picture (T2). Without a HUD-free picture for a frame,
// that frame shows no generated picture (never a warped HUD).
void SetLiveFrameGeneration(bool on);
bool LiveFrameGeneration();
// The HUD-free picture of the frame now being presented: back-buffer size and format; null with `why`
// (plain words) when there is none for this frame. `state` receives its resting state (DX12: the game's
// recording leaves it in RENDER_TARGET; DX11 proxy: a shared image, COMMON).
using HudFreeFn = ID3D12Resource* (*)(const char** why, D3D12_RESOURCE_STATES* state);
void SetHudFreeProvider(HudFreeFn fn);
// The HUD sheet was learned again (live DLAA): the still-area steadying drops its pair of frames.
void NoteHudRelearn();
// F8 with x2 on: the pictures actually presented next are saved, `triples` generated pictures with the
// real frames before and after each (previous-real / generated / next-real), with frame ids and present
// results, to LocalAppData\NeuralAlias\captures\fgout-<time>-<exe>-p<pid>. The pictures are read
// back only during the burst. false + why (plain words) when it cannot start now.
bool RequestOutputCapture(uint32_t triples, char* why, size_t why_size);
struct LiveStats {
    uint32_t live = 0, ready = 0, preparing = 0, inline_owner = 1;
    uint64_t frames_inline = 0, frames_worker = 0;
    uint64_t skipped_no_hudfree = 0, skipped_menu = 0, skipped_preparing = 0, skipped_veto = 0;
    // GPU milliseconds, last and smoothed: Off path copy; x2 path: real-frame copy, HUD-free copy,
    // Frame Generation evaluate, HUD paste.
    float off_ms = 0, off_avg = 0, copy_ms = 0, copy_avg = 0, hudcopy_ms = 0, hudcopy_avg = 0;
    float eval_ms = 0, eval_avg = 0, paste_ms = 0, paste_avg = 0;
    uint64_t vram_budget_mb = 0, vram_usage_mb = 0, extra_mb = 0;
    char prepare[112] = "";
    char hudfree_why[112] = "";
    // Still-area steadying (fg/steady_pass.hpp, Astra): the setting, frames corrected, the share of the
    // picture it changed on those frames (percent, counted on the GPU), its GPU time, frames with x2 and
    // the setting Off, and why a frame pair was not used (the unmodified picture was shown then).
    uint32_t steady_on = 1, steady_ready = 0;
    uint64_t steady_frames = 0, steady_off_frames = 0;
    float steady_share = 0, steady_ms = 0, steady_avg = 0;
    uint64_t steady_drop_gap = 0, steady_drop_reset = 0, steady_drop_motion = 0, steady_drop_relearn = 0, steady_drop_resize = 0, steady_drop_refused = 0;
    char steady_why[112] = "";
    // Display: the refresh rate Windows reports now, the display's maximum (x2's ceiling), whether
    // dynamic refresh is on, and frames shown without x2 because even the maximum is below the real rate.
    float display_reported_hz = 0, display_max_hz = 0;
    char display_dynamic[64] = "";
    uint64_t skipped_display = 0;
    // Presented-picture capture (F8 with x2 on): state in plain words.
    char capture[220] = "";
    // x2 chosen and no generated picture for more than a second: how long, and why in plain words.
    uint32_t waiting_ms = 0;
    char waiting[200] = "";
};
LiveStats GetLiveStats();

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
    uint32_t scaled_w, scaled_h, native_w, native_h; // render-small: proxy and real sizes (0 = not scaled)
    uint64_t upscaled, upscale_failed;               // render-small: pictures upscaled / left with the old picture
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
