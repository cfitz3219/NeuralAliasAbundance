#include "host_dx12/host_boot/process_log.hpp"
#include "host_dx12/native_present/size_spoof.hpp"
#include "host_dx12/native_present/size_probe.hpp"
#include "host_dx12/native_present/size_controller.hpp"
#include "host_dx12/native_present/input_map.hpp"
#include <intrin.h>
#include <algorithm>
#include <cmath>
#include "host_dx12/adapters/mode_control.hpp"
#include "host_dx12/nr/native_fg.hpp"
#include "host_dx12/sl_boundary/sl_boundary.hpp"
#include "host_dx12/nr/fg_states.hpp"
#include "host_dx12/adapters/tlou1/timing.hpp"
#include "host_dx12/adapters/tlou1/apply.hpp"
#include "host_dx12/adapters/tsushima/apply.hpp"
#include "host_dx12/overlay/overlay.hpp"
#include "host_dx12/overlay/input_handover.hpp"
#include "host_dx12/overlay/present_queues.hpp"
#include "host_dx12/overlay/boundary_fallback.hpp"
#include "host_dx12/overlay/submission_tracking.hpp"
#include "host_dx12/overlay/usage_sampler.hpp"

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
#include "host_dx12/capture/j2_capture.hpp"
#include "host_dx12/dlss/dlss_backend.hpp"
#include "host_dx12/observe/producers.hpp"
#include "host_dx12/upscalers/ffx_intercept.hpp"
#include "host_dx12/upscalers/xess_intercept.hpp"
#include "host_dx12/color/color_space.hpp"
#include "host_dx12/fg/frame_gen.hpp"
#include "host_dx12/fg/preset_profile.hpp"
#include "host_dx12/hooking/variant_registry.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
#include "host_dx12/motion/live_flow.hpp"
#include "host_dx12/ngx_observe/gpu_timing.hpp"
#include "host_dx12/ngx_observe/ngx_observe.hpp"
#include "host_dx12/nr/nr_pass.hpp"
#include "host_dx12/j2/live_dlaa.hpp"
#include "host_dx12/j2/native_hud.hpp"
#include "host_dx12/j2/hud_unblend.hpp"
#include "host_dx12/j2/projection_window.hpp"
#include "host_dx12/scale/live_driver.hpp"
#include "host_dx12/scale/auto_scene.hpp"
#include "host_dx12/scale/settings_read_hook.hpp"
#include "host_dx12/j2/j2_flow.hpp"
#include "host_dx12/j2/compute_flow.hpp"
#include "host_dx12/overlay/menu.hpp"
#include "host_dx12/overlay/na_style.hpp"
#include "host_dx12/overlay/presenter.hpp"
#include "host_dx12/overlay/freeze_watchdog.hpp"
#include "host_dx12/nr/queue_trace.hpp"
#include <ctime>
#include "host_dx12/settings.hpp"
#include "host_dx12/host_boot/session.hpp"
#include "host_dx12/latency/nvidia_latency.hpp"
#include "host_dx12/latency/pcl_stats.hpp"

using Microsoft::WRL::ComPtr;

namespace na::overlay {
namespace {

constexpr uint32_t kSwapReleaseSlot = 2;          // IUnknown::Release on the swap chain (A71: same-window replacement)
constexpr uint32_t kSwapPresentSlot = 8;
constexpr uint32_t kSwapGetBufferSlot = 9;        // IDXGISwapChain::GetBuffer (A59 presenter)
constexpr uint32_t kSwapResizeBuffersSlot = 13;
constexpr uint32_t kSwapResizeTargetSlot = 14;
constexpr uint32_t kSwap1Present1Slot = 22;
constexpr uint32_t kSwapGetDescSlot = 12;   // IDXGISwapChain::GetDesc (render-small size spoof)
constexpr uint32_t kSwap1GetDesc1Slot = 18; // IDXGISwapChain1::GetDesc1
constexpr uint32_t kSwap2SetMaxLatencySlot = 31;  // IDXGISwapChain2::SetMaximumFrameLatency (A62)
constexpr uint32_t kSwap2GetMaxLatencySlot = 32;  // IDXGISwapChain2::GetMaximumFrameLatency (A62)
constexpr uint32_t kSwap2GetLatencyObjectSlot = 33;
constexpr uint32_t kSwap3GetIndexSlot = 36;       // IDXGISwapChain3::GetCurrentBackBufferIndex (A59)
constexpr uint32_t kSwap3SetColorSpace1Slot = 38; // IDXGISwapChain3::SetColorSpace1
constexpr uint32_t kSwap3ResizeBuffers1Slot = 39; // IDXGISwapChain3::ResizeBuffers1 (A59)
constexpr uint32_t kQueueExecuteSlot = 10;
// IDXGIFactory / IDXGIFactory2 swap-chain creation (shared factory vtable): for D3D12
// the "device" argument is the command queue the swap chain presents from.
constexpr uint32_t kFactoryCreateSwapChainSlot = 10;
constexpr uint32_t kFactory2CreateForHwndSlot = 15;
constexpr uint32_t kFactory2CreateForCoreWindowSlot = 16;
constexpr uint32_t kFactory2CreateForCompositionSlot = 24;
constexpr uint32_t kSrvHeapSize = 64;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT,
                                               const DXGI_PRESENT_PARAMETERS*);
using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT,
                                             DXGI_FORMAT, UINT);
using ResizeTargetFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, const DXGI_MODE_DESC*);
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
ResizeTargetFn g_orig_resize_target = nullptr;
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
using QueueSyncFn=HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*,ID3D12Fence*,UINT64);
QueueSyncFn g_trace_signal=nullptr,g_trace_wait=nullptr;
SetColorSpaceFn g_orig_set_color_space = nullptr;

// Internal reads bypass only OUR virtual backbuffer substitution. They must
// still dispatch through the actual COM object's vtable: a Streamline wrapper
// is not a native DXGI object and cannot be passed to the dummy-chain trampoline.
thread_local unsigned t_native_buffer_read=0;
struct NativeBufferRead {
    NativeBufferRead(){++t_native_buffer_read;}
    ~NativeBufferRead(){--t_native_buffer_read;}
};
HRESULT real_get_buffer(IDXGISwapChain* sc, UINT i, REFIID riid, void** out)
{
    NativeBufferRead scope;
    return sc->GetBuffer(i, riid, out);
}
UINT real_index(IDXGISwapChain* sc)
{
    if(!sc)return 0xFFFFFFFFu;
    ComPtr<IDXGISwapChain3> sc3;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) return 0xFFFFFFFFu;
    NativeBufferRead scope;
    return sc3->GetCurrentBackBufferIndex();
}
std::atomic<int> g_color_space{DXGI_COLOR_SPACE_CUSTOM}; // last SetColorSpace1 on the game's chain
std::atomic<uint32_t> g_presented_format{0}; // the proxied chain's buffer format at its last present (status line)
XInputGetStateFn g_orig_xinput[3] = {};
std::atomic<XInputGetStateFn> g_menu_xinput{nullptr};
uint64_t g_pad_retry_after=0; // render thread, under g_state.mutex

std::atomic<ID3D12CommandQueue*> g_direct_queue{nullptr};

// The queue each swap chain was created with. The menu (and anything else that
// writes the back buffer at Present) must use THIS queue: Present executes on it,
// so commands submitted there right before Present are ordered after everything
// the presenter queued (Streamline DLSS Frame Generation writes the generated frame
// and waits on this queue before presenting). g_direct_queue is only "the last
// direct queue that submitted", which in a two-queue game with native Frame
// Generation (Phantom Blade Zero) is often not the presenting queue: the menu then
// wrote the back buffer unsynchronised with FG and the GPU faulted (Cort, 2026-10-29).
struct SwapQueue { void* sc = nullptr; PresentQueues queues; int color_space=DXGI_COLOR_SPACE_CUSTOM; };
std::mutex g_swap_queue_m;
SwapQueue g_swap_queues[8];
uint32_t g_swap_queue_next = 0;
std::atomic<uint32_t> g_swap_queue_hits{0}, g_swap_queue_misses{0}, g_swap_queues_recorded{0}, g_swap_queue_observed{0};
std::atomic<uintptr_t> g_last_present_queue{0};

void note_swapchain_queue(IUnknown* device_or_queue, void* swapchain)
{
    if (!device_or_queue || !swapchain) return;
    ComPtr<ID3D12CommandQueue> q;
    if (FAILED(device_or_queue->QueryInterface(IID_PPV_ARGS(&q))) || !q) return; // D3D11 / other device: not ours
    std::lock_guard<std::mutex> lk(g_swap_queue_m);
    for (auto& e : g_swap_queues)
        if (e.sc == swapchain) { e.queues.Created(q.Get()); e.color_space=DXGI_COLOR_SPACE_CUSTOM; g_swap_queues_recorded.fetch_add(1); return; } // address reused
    SwapQueue& slot = g_swap_queues[g_swap_queue_next++ % 8];
    slot.sc = swapchain;slot.color_space=DXGI_COLOR_SPACE_CUSTOM;
    slot.queues.Created(q.Get());
    g_swap_queues_recorded.fetch_add(1);
}
ComPtr<ID3D12CommandQueue> swapchain_queue(IDXGISwapChain* swapchain)
{
    const UINT index=real_index(swapchain);
    std::lock_guard<std::mutex> lk(g_swap_queue_m);
    for (const auto& e : g_swap_queues) if (e.sc == swapchain) return e.queues.At(index);
    return nullptr;
}

using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using CreateForCoreWindowFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*,
                                                          IDXGIOutput*, IDXGISwapChain1**);
using CreateForCompositionFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*,
                                                           IDXGIOutput*, IDXGISwapChain1**);
CreateSwapChainFn g_orig_create_swapchain = nullptr;
CreateForHwndFn g_orig_create_for_hwnd = nullptr;
CreateForCoreWindowFn g_orig_create_for_core_window = nullptr;
CreateForCompositionFn g_orig_create_for_composition = nullptr;

// Render-small (S1 integration, docs/NATIVE-PRESENT-SIZING.md): the REAL chain is always created at
// the window's physical size. A game that already learned the advertised render size and passes it
// back gets the physical size instead (else the presenter would scale an already small chain again).
// The presenter's rounding (presenter.cpp ensure_proxies_locked): round(real * scale), full size
// when that would not be smaller.
na::native_present::Size small_render_for(na::native_present::Size out, double f)
{
    if (!(f < 1.0) || !out.w || !out.h) return out;
    na::native_present::Size r{(std::max)(1u, static_cast<UINT>(std::lround(out.w * f))),
                               (std::max)(1u, static_cast<UINT>(std::lround(out.h * f)))};
    return r.w >= out.w || r.h >= out.h ? out : r;
}
void small_native_dims(HWND w, UINT& width, UINT& height)
{
    if (!na::j2::RenderSmall() || !w) return;
    const auto st = na::native_present::GetSizeState();
    if (st.enabled && st.hwnd == w && width == st.render.w && height == st.render.h) { width = st.output.w; height = st.output.h; }
}
// After creation: attach the size spoof to this window with the presenter's rounding.
void small_attach(HWND w, IDXGISwapChain* sc)
{
    if (!na::j2::RenderSmall() || !w || !sc) return;
    DXGI_SWAP_CHAIN_DESC real{};
    { na::native_present::NativeScope native; if (FAILED(sc->GetDesc(&real))) return; }
    const na::native_present::Size out{real.BufferDesc.Width, real.BufferDesc.Height};
    na::native_present::Attach(w, out, small_render_for(out, static_cast<double>(na::present::RenderScale())));
}
HRESULT STDMETHODCALLTYPE DetourCreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** out)
{
    DXGI_SWAP_CHAIN_DESC copy{};
    if (d && na::j2::RenderSmall()) { copy = *d; small_native_dims(copy.OutputWindow, copy.BufferDesc.Width, copy.BufferDesc.Height); }
    HRESULT hr;
    { na::native_present::NativeScope native; hr = g_orig_create_swapchain(f, dev, d && na::j2::RenderSmall() ? &copy : d, out); }
    if (SUCCEEDED(hr) && out && *out) { note_swapchain_queue(dev, *out); if (d) small_attach(d->OutputWindow, *out); }
    return hr;
}
HRESULT STDMETHODCALLTYPE DetourCreateForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND w, const DXGI_SWAP_CHAIN_DESC1* d,
                                              const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* o, IDXGISwapChain1** out)
{
    DXGI_SWAP_CHAIN_DESC1 copy{};
    if (d && na::j2::RenderSmall()) { copy = *d; small_native_dims(w, copy.Width, copy.Height); }
    HRESULT hr;
    { na::native_present::NativeScope native; hr = g_orig_create_for_hwnd(f, dev, w, d && na::j2::RenderSmall() ? &copy : d, fs, o, out); }
    if (SUCCEEDED(hr) && out && *out) { note_swapchain_queue(dev, *out); small_attach(w, *out); }
    return hr;
}
// Render-small: the game's own descriptor queries see the render size; ours and other modules' stay real.
using GetDescFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, DXGI_SWAP_CHAIN_DESC*);
using GetDesc1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, DXGI_SWAP_CHAIN_DESC1*);
GetDescFn g_orig_get_desc = nullptr;
GetDesc1Fn g_orig_get_desc1 = nullptr;
HRESULT STDMETHODCALLTYPE DetourGetDesc(IDXGISwapChain* sc, DXGI_SWAP_CHAIN_DESC* d)
{
    const void* caller = _ReturnAddress();
    const HRESULT hr = g_orig_get_desc(sc, d);
    if (SUCCEEDED(hr) && d && na::native_present::GameCaller(caller)) na::native_present::RewriteDesc(d);
    return hr;
}
HRESULT STDMETHODCALLTYPE DetourGetDesc1(IDXGISwapChain1* sc, DXGI_SWAP_CHAIN_DESC1* d)
{
    const void* caller = _ReturnAddress();
    const HRESULT hr = g_orig_get_desc1(sc, d);
    if (SUCCEEDED(hr) && d && na::native_present::GameCaller(caller)) {
        HWND w = nullptr;
        { na::native_present::NativeScope native; if (FAILED(sc->GetHwnd(&w))) w = nullptr; }
        if (w) na::native_present::RewriteDesc1(w, d);
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE DetourCreateForCoreWindow(IDXGIFactory2* f, IUnknown* dev, IUnknown* w, const DXGI_SWAP_CHAIN_DESC1* d,
                                                    IDXGIOutput* o, IDXGISwapChain1** out)
{
    const HRESULT hr = g_orig_create_for_core_window(f, dev, w, d, o, out);
    if (SUCCEEDED(hr) && out && *out) note_swapchain_queue(dev, *out);
    return hr;
}
HRESULT STDMETHODCALLTYPE DetourCreateForComposition(IDXGIFactory2* f, IUnknown* dev, const DXGI_SWAP_CHAIN_DESC1* d,
                                                     IDXGIOutput* o, IDXGISwapChain1** out)
{
    const HRESULT hr = g_orig_create_for_composition(f, dev, d, o, out);
    if (SUCCEEDED(hr) && out && *out) note_swapchain_queue(dev, *out);
    return hr;
}
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
std::atomic<bool> g_core_started{false}, g_menu_enabled{false}, g_fail_menu_start{false},g_fail_menu_create{false};
std::atomic<unsigned> g_fail_optional_input{0};
std::atomic<bool> g_fail_cursor_cleanup{false};
std::mutex g_start_m;
bool g_menu_frame_ready=false; // g_state.mutex
std::atomic<uint32_t> g_gamepad_hooked{0};
std::atomic<bool> g_stopping{false};
// Diagnostic counters plus per-chain ownership; no global routing switch.
// Native Presents include DLSS-G's asynchronous generated frames.
std::atomic<uint64_t> g_native_game_presents{0};
std::atomic<bool> g_boundary_frames{false};
std::mutex g_boundary_m;
BoundaryRoutes g_boundary_policy; // g_boundary_m
void* g_native_present_target = nullptr;  // native Present body hooked at Start (diagnostics only)
void* g_native_present1_target = nullptr;
std::atomic<float> g_fps{0.0f}, g_frame_ms{0.0f}, g_mouse_x{0.0f}, g_mouse_y{0.0f};
char g_route_tag[64]{};
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
// Start. Only the overlay's OWN reads pass through (it needs the real cursor):
// t_overlay_input is set while the menu is being drawn. It used to exempt the
// whole render thread, but that is the game's thread too (it calls Present),
// and many games poll input there: Sniper Elite Resistance fired its gun on
// every menu click. Everything the game calls sees a frozen cursor, no buttons
// and no keys.
using PfnGetRawInputData = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using PfnGetRawInputBuffer = UINT(WINAPI*)(PRAWINPUT, PUINT, UINT);
using PfnGetCursorPos = BOOL(WINAPI*)(LPPOINT);
using PfnSetCursorPos = BOOL(WINAPI*)(int, int);
using PfnSetCursor = HCURSOR(WINAPI*)(HCURSOR);
PfnSetCursor g_orig_set_cursor=nullptr;
HCURSOR g_menu_cursor=nullptr;
std::atomic<DWORD> g_cursor_thread{0};
std::atomic<HCURSOR> g_game_cursor{nullptr};
std::atomic<bool> g_cursor_owned{false},g_cursor_message_pending{false};
using PfnGetAsyncKeyState = SHORT(WINAPI*)(int);
using PfnGetKeyboardState = BOOL(WINAPI*)(PBYTE);
using PfnGetKeyState = SHORT(WINAPI*)(int);
PfnGetRawInputData g_orig_get_raw_input_data = nullptr;
PfnGetRawInputBuffer g_orig_get_raw_input_buffer = nullptr;
PfnGetCursorPos g_orig_get_cursor_pos = nullptr;
PfnSetCursorPos g_orig_set_cursor_pos = nullptr;
PfnGetAsyncKeyState g_orig_get_async_key_state = nullptr;
PfnGetKeyboardState g_orig_get_keyboard_state = nullptr;
PfnGetKeyState g_orig_get_key_state = nullptr;
void* g_input_hook_targets[8] = {};
thread_local bool t_overlay_input = false; // the overlay itself is reading input (menu drawing)
std::atomic<uint32_t> g_input_hooks{0};
std::atomic<DWORD> g_render_thread{0};
std::atomic<LONG> g_frozen_x{0}, g_frozen_y{0}; // what the game sees as the cursor while open

inline bool lock_input_for_this_thread()
{
    return g_open.load(std::memory_order_acquire) && g_initialized.load(std::memory_order_acquire) &&
           !t_overlay_input;
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
// Render-small (S2): absolute raw mouse positions follow the game's virtual window; relative deltas never change.
void small_map_raw(RAWINPUT* ri, const void* caller)
{
    if (ri && ri->header.dwType == RIM_TYPEMOUSE) na::native_present::MapRawMouseRead(ri->data.mouse, caller);
}
UINT WINAPI DetourGetRawInputData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT header)
{
    const void* caller = _ReturnAddress();
    UINT r = g_orig_get_raw_input_data(h, cmd, data, size, header);
    if (r != static_cast<UINT>(-1) && data && cmd == RID_INPUT && r >= sizeof(RAWINPUTHEADER)) {
        if (lock_input_for_this_thread()) neutralise_raw(static_cast<RAWINPUT*>(data));
        else small_map_raw(static_cast<RAWINPUT*>(data), caller);
    }
    return r;
}
UINT WINAPI DetourGetRawInputBuffer(PRAWINPUT data, PUINT size, UINT header)
{
    const void* caller = _ReturnAddress();
    UINT r = g_orig_get_raw_input_buffer(data, size, header);
    const bool locked = lock_input_for_this_thread();
    if (r != static_cast<UINT>(-1) && r != 0 && data && (locked || na::j2::RenderSmall())) {
        RAWINPUT* p = data;
        for (UINT i = 0; i < r; ++i) {
            if (locked) neutralise_raw(p);
            else small_map_raw(p, caller);
            // NEXTRAWINPUTBLOCK without the QWORD type the lean headers omit: 8-byte aligned on x64.
            uintptr_t next = reinterpret_cast<uintptr_t>(reinterpret_cast<BYTE*>(p) + p->header.dwSize);
            p = reinterpret_cast<RAWINPUT*>((next + sizeof(uint64_t) - 1) & ~(sizeof(uint64_t) - 1));
        }
    }
    return r;
}
BOOL WINAPI DetourGetCursorPos(LPPOINT p)
{
    const void* caller = _ReturnAddress();
    if (p && lock_input_for_this_thread()) {
        p->x = g_frozen_x.load(std::memory_order_relaxed);
        p->y = g_frozen_y.load(std::memory_order_relaxed);
        na::native_present::MapCursorRead(p, caller); // the frozen point is real; the game reads virtual
        return TRUE;
    }
    const BOOL ok = g_orig_get_cursor_pos(p);
    if (ok && p) na::native_present::MapCursorRead(p, caller);
    return ok;
}
HCURSOR WINAPI DetourSetCursor(HCURSOR cursor)
{
    // Remember the window owner's latest intent, including NULL (game-drawn
    // cursor). Our own writes bypass this detour. A render/worker thread must
    // not replace the window thread's restore handle.
    if(g_cursor_owned.load(std::memory_order_acquire) && !t_overlay_input &&
       GetCurrentThreadId()==g_cursor_thread.load())g_game_cursor.store(cursor);
    return g_orig_set_cursor(lock_input_for_this_thread() && g_menu_cursor ? g_menu_cursor : cursor);
}
BOOL WINAPI DetourSetCursorPos(int x, int y)
{
    if (lock_input_for_this_thread()) return TRUE; // the game may not re-centre the menu cursor
    na::native_present::MapCursorWrite(x, y, _ReturnAddress());
    return g_orig_set_cursor_pos(x, y);
}
// Render-small (S2): the game's cursor clip is in its virtual screen space. Not counted in
// input_hooks (installed only in this mode, on top of the eight).
using PfnClipCursor = BOOL(WINAPI*)(const RECT*);
using PfnGetClipCursor = BOOL(WINAPI*)(LPRECT);
PfnClipCursor g_orig_clip_cursor = nullptr;
PfnGetClipCursor g_orig_get_clip_cursor = nullptr;
void* g_clip_hook_targets[2] = {};
BOOL WINAPI DetourClipCursor(const RECT* r)
{
    RECT physical{};
    if (r && na::native_present::MapClipRect(r, physical, _ReturnAddress())) return g_orig_clip_cursor(&physical);
    return g_orig_clip_cursor(r);
}
BOOL WINAPI DetourGetClipCursor(LPRECT r)
{
    const void* caller = _ReturnAddress();
    const BOOL ok = g_orig_get_clip_cursor(r);
    if (ok && r) na::native_present::MapClipRead(r, caller);
    return ok;
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
SHORT WINAPI DetourGetKeyState(int vk)
{
    // Mouse buttons and keys read through GetKeyState (VK_LBUTTON is common for "fire").
    if (lock_input_for_this_thread()) return 0;
    return g_orig_get_key_state(vk);
}
// DirectInput 8 (keyboard/mouse through IDirectInputDevice8::GetDeviceState /
// GetDeviceData). The device methods live in dinput8.dll and are shared by
// every device, so they are hooked once, found through a keyboard device of
// our own (A and W interfaces). Locked: state reads as all-zero (no keys, no
// buttons, no motion) and buffered events are drained and dropped.
using PfnDI8Create = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using PfnDIGetDeviceState = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPVOID);
using PfnDIGetDeviceData = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, void*, LPDWORD, DWORD);
constexpr int kDiSlots = 2; // [0] W, [1] A
PfnDIGetDeviceState g_orig_di_state[kDiSlots] = {};
PfnDIGetDeviceData g_orig_di_data[kDiSlots] = {};
void* g_di_targets[kDiSlots * 2] = {};
template <int I>
HRESULT STDMETHODCALLTYPE DetourDIGetDeviceState(void* self, DWORD cb, LPVOID data)
{
    const HRESULT r = g_orig_di_state[I](self, cb, data);
    if (SUCCEEDED(r) && data && cb && lock_input_for_this_thread()) memset(data, 0, cb);
    return r;
}
template <int I>
HRESULT STDMETHODCALLTYPE DetourDIGetDeviceData(void* self, DWORD cb, void* events, LPDWORD inout, DWORD flags)
{
    const HRESULT r = g_orig_di_data[I](self, cb, events, inout, flags);
    if (SUCCEEDED(r) && inout && lock_input_for_this_thread()) *inout = 0; // drained, not delivered
    return r;
}
// Startup worker only: optional input hooks must not lose to another installer.
bool install_input_function(void* target,void* detour,void** original) {
    if(!na::hooking::BeginInstallWait("overlay input"))return false;
    if(!na::hooking::PrepareHook(target,detour,original)){
        na::hooking::AbandonInstall();return false;
    }
    return na::hooking::CommitInstall();
}
void install_dinput_hooks()
{
    HMODULE m = LoadLibraryW(L"dinput8.dll"); // pinned: the hooked code must stay loaded
    if (!m) return;
    auto create = reinterpret_cast<PfnDI8Create>(GetProcAddress(m, "DirectInput8Create"));
    if (!create) return;
    static const GUID kIID_DI8W = {0xBF798031, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
    static const GUID kIID_DI8A = {0xBF798030, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
    static const GUID kSysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
    using PfnCreateDevice = HRESULT(STDMETHODCALLTYPE*)(void*, REFGUID, void**, void*);
    const GUID* iids[kDiSlots] = {&kIID_DI8W, &kIID_DI8A};
    void* states[kDiSlots] = {reinterpret_cast<void*>(&DetourDIGetDeviceState<0>), reinterpret_cast<void*>(&DetourDIGetDeviceState<1>)};
    void* datas[kDiSlots] = {reinterpret_cast<void*>(&DetourDIGetDeviceData<0>), reinterpret_cast<void*>(&DetourDIGetDeviceData<1>)};
    for (int i = 0; i < kDiSlots; ++i) {
        void* di = nullptr;
        if (FAILED(create(GetModuleHandleW(nullptr), 0x0800, *iids[i], &di, nullptr)) || !di) continue;
        void** di_vt = *reinterpret_cast<void***>(di);
        void* dev = nullptr;
        if (SUCCEEDED(reinterpret_cast<PfnCreateDevice>(di_vt[3])(di, kSysKeyboard, &dev, nullptr)) && dev) {
            void** vt = *reinterpret_cast<void***>(dev);
            void* st = vt[9], *dt = vt[10]; // GetDeviceState, GetDeviceData
            // The A and W interfaces may share one implementation: hook each function once.
            bool st_done = false, dt_done = false;
            for (void* t : g_di_targets) { st_done |= t == st; dt_done |= t == dt; }
            if (!st_done && install_input_function(st, states[i], reinterpret_cast<void**>(&g_orig_di_state[i]))) g_di_targets[i * 2] = st;
            if (!dt_done && install_input_function(dt, datas[i], reinterpret_cast<void**>(&g_orig_di_data[i]))) g_di_targets[i * 2 + 1] = dt;
            static_cast<IUnknown*>(dev)->Release();
        }
        static_cast<IUnknown*>(di)->Release();
    }
}
void install_input_hooks()
{
    install_dinput_hooks();
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (!u) return;
    struct { const char* name; void* detour; void** original; } specs[8] = {
        {"GetRawInputData", reinterpret_cast<void*>(&DetourGetRawInputData), reinterpret_cast<void**>(&g_orig_get_raw_input_data)},
        {"GetRawInputBuffer", reinterpret_cast<void*>(&DetourGetRawInputBuffer), reinterpret_cast<void**>(&g_orig_get_raw_input_buffer)},
        {"GetCursorPos", reinterpret_cast<void*>(&DetourGetCursorPos), reinterpret_cast<void**>(&g_orig_get_cursor_pos)},
        {"SetCursorPos", reinterpret_cast<void*>(&DetourSetCursorPos), reinterpret_cast<void**>(&g_orig_set_cursor_pos)},
        {"GetAsyncKeyState", reinterpret_cast<void*>(&DetourGetAsyncKeyState), reinterpret_cast<void**>(&g_orig_get_async_key_state)},
        {"GetKeyboardState", reinterpret_cast<void*>(&DetourGetKeyboardState), reinterpret_cast<void**>(&g_orig_get_keyboard_state)},
        {"GetKeyState", reinterpret_cast<void*>(&DetourGetKeyState), reinterpret_cast<void**>(&g_orig_get_key_state)},
        {"SetCursor", reinterpret_cast<void*>(&DetourSetCursor), reinterpret_cast<void**>(&g_orig_set_cursor)},
    };
    uint32_t n = 0;
    for (int i = 0; i < 8; ++i) {
        if(g_input_hook_targets[i]){++n;continue;}
        void* target = reinterpret_cast<void*>(GetProcAddress(u, specs[i].name));
        if(i==7 && g_fail_cursor_cleanup.exchange(false)){
            na::hooking::FailNextCommitEnableForTest(0);na::hooking::FailNextRemoveForTest();
        }
        const bool optional_unavailable=i==7 && g_fail_optional_input.load() && g_fail_optional_input.fetch_sub(1)>0;
        if (target && !optional_unavailable && install_input_function(target, specs[i].detour, specs[i].original)) {
            g_input_hook_targets[i] = target;
            ++n;
        } else {
            // Failed cleanup may retain a callable trampoline. Overwriting it
            // with the patched export can recurse into our own detour.
            if(!*specs[i].original)*specs[i].original=target;
        }
    }
    g_input_hooks.store(n, std::memory_order_release);
    if (na::j2::RenderSmall()) {
        const struct { const char* name; void* detour; void** original; } clip[2] = {
            {"ClipCursor", reinterpret_cast<void*>(&DetourClipCursor), reinterpret_cast<void**>(&g_orig_clip_cursor)},
            {"GetClipCursor", reinterpret_cast<void*>(&DetourGetClipCursor), reinterpret_cast<void**>(&g_orig_get_clip_cursor)},
        };
        for (int i = 0; i < 2; ++i) {
            if (g_clip_hook_targets[i]) continue;
            void* target = reinterpret_cast<void*>(GetProcAddress(u, clip[i].name));
            if (target && install_input_function(target, clip[i].detour, clip[i].original)) g_clip_hook_targets[i] = target;
            else if (!*clip[i].original) *clip[i].original = target;
        }
    }
}
void remove_input_hooks()
{
    for (void*& t : g_clip_hook_targets) {
        if (t) na::hooking::Unhook(t);
        t = nullptr;
    }
    for (void*& t : g_di_targets) {
        if (t) na::hooking::Unhook(t);
        t = nullptr;
    }
    for (void*& t : g_input_hook_targets) {
        if (t) na::hooking::Unhook(t);
        t = nullptr;
    }
    g_input_hooks.store(0, std::memory_order_release);
}
// Originals for our own use (valid after install_input_hooks; plain exports otherwise).
inline BOOL real_get_cursor_pos(LPPOINT p) { return g_orig_get_cursor_pos ? g_orig_get_cursor_pos(p) : GetCursorPos(p); }
inline HCURSOR real_set_cursor(HCURSOR c) { return g_orig_set_cursor ? g_orig_set_cursor(c) : SetCursor(c); }
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
bool g_menu_diagnostics = false;
void menu_note(const char* text) {
    if (!g_menu_diagnostics || !g_status_log[0]) return;
    HANDLE f = CreateFileW(g_status_log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, text, static_cast<DWORD>(strlen(text)), &written, nullptr);
    CloseHandle(f);
}
// Render-small test builds: a fatal exception anywhere in the process leaves a line (code,
// module+offset, thread) and a minidump next to the status log, so a crash that Windows
// does not report still has evidence. At most two dumps per process.
void status_note(const char* text) {
    if (!g_status_log[0]) return;
    HANDLE f = CreateFileW(g_status_log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, text, static_cast<DWORD>(strlen(text)), &written, nullptr);
    CloseHandle(f);
}
LONG CALLBACK crash_capture(EXCEPTION_POINTERS* e) {
    const DWORD code = e && e->ExceptionRecord ? e->ExceptionRecord->ExceptionCode : 0;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW &&
        code != 0xC0000409u /* stack buffer overrun / fail fast */ && code != 0xC0000374u /* heap corruption */ &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    static std::atomic<int> count{0};
    const int n = count.fetch_add(1);
    if (n >= 2) return EXCEPTION_CONTINUE_SEARCH;
    void* at = e->ExceptionRecord->ExceptionAddress;
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(at), &mod) && mod)
        GetModuleFileNameA(mod, name, MAX_PATH);
    const char* base = strrchr(name, '\\');
    char row[512];
    snprintf(row, sizeof(row), "crash code=0x%08lX at=%s+0x%llX thread=%lu info=%llu/%p mode=%u render_scale=%.3f\r\n", code,
             base ? base + 1 : name, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(at) - reinterpret_cast<uintptr_t>(mod)),
             GetCurrentThreadId(), e->ExceptionRecord->NumberParameters ? static_cast<unsigned long long>(e->ExceptionRecord->ExceptionInformation[0]) : 0ull,
             e->ExceptionRecord->NumberParameters > 1 ? reinterpret_cast<void*>(e->ExceptionRecord->ExceptionInformation[1]) : nullptr,
             na::settings::Get().j2_scale_mode.load(), na::present::RenderScale());
    status_note(row);
    wchar_t path[MAX_PATH]{};
    if (swprintf_s(path, L"%s.crash%d.dmp", g_status_log, n) > 0) na::freeze::WriteMiniDump(path, e);
    return EXCEPTION_CONTINUE_SEARCH;
}
int menu_fault(EXCEPTION_POINTERS* exception) {
    na::settings::MarkSessionFailed("crash");
    if (g_menu_diagnostics) {
        char row[180];
        snprintf(row, sizeof(row), "menu_fault code=0x%08lX address=%p thread=%lu\r\n",
                 exception->ExceptionRecord->ExceptionCode, exception->ExceptionRecord->ExceptionAddress, GetCurrentThreadId());
        menu_note(row);
        wchar_t path[MAX_PATH]{};
        if (swprintf_s(path, L"%s.menu.dmp", g_status_log) > 0)
            na::freeze::WriteMiniDump(path, exception);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
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
    HANDLE f = CreateFileW(g_status_log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, line, static_cast<DWORD>((std::min)(n, static_cast<int>(sizeof(line)-1))), &written, nullptr);
    char mode_line[1536]{};
    const int mode_bytes=na::mode::FormatStatus(mode_line,sizeof(mode_line));
    if(mode_bytes>0){WriteFile(f,mode_line,DWORD((std::min)(mode_bytes,int(sizeof(mode_line)-1))),&written,nullptr);WriteFile(f,"\r\n",2,&written,nullptr);}
    {char usage_line[512];const int usage_bytes=na::usage::FormatLine(usage_line,sizeof(usage_line));
     if(usage_bytes>0)WriteFile(f,usage_line,DWORD((std::min)(usage_bytes,int(sizeof(usage_line)-1))),&written,nullptr);}
    {
        char source_line[256];
        const int source_bytes=snprintf(source_line,sizeof(source_line),"overlay_source=%s native_presents=%llu presents=%llu pid=%lu queue_hits=%u buffer_queue=%u queue_misses=%u\r\n",
            g_boundary_frames.load(std::memory_order_acquire)?"streamline":"native",
            static_cast<unsigned long long>(g_native_game_presents.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_presents.load(std::memory_order_relaxed)),GetCurrentProcessId(),
            g_swap_queue_hits.load(),g_swap_queue_observed.load(),g_swap_queue_misses.load());
        if(source_bytes>0)WriteFile(f,source_line,DWORD((std::min)(source_bytes,int(sizeof(source_line)-1))),&written,nullptr);
    }
    {
        char reason[160]{},failure_line[240]{};
        const auto count=na::hooking::InstallWaitFailures(reason,sizeof(reason));
        if(count){const auto bytes=snprintf(failure_line,sizeof(failure_line),"hook_install_wait_failures=%u last=%s\r\n",count,reason);
            WriteFile(f,failure_line,DWORD((std::min)(bytes,int(sizeof(failure_line)-1))),&written,nullptr);}
    }
    char readiness[128]{};
    const auto readiness_bytes=snprintf(readiness,sizeof(readiness),"overlay_ready core=%u menu=%u initialized=%u input_hooks=%u/8\r\n",
        g_core_started.load()?1u:0u,g_menu_enabled.load()?1u:0u,g_initialized.load(),g_input_hooks.load());
    if(readiness_bytes>0)WriteFile(f,readiness,DWORD((std::min)(readiness_bytes,int(sizeof(readiness)-1))),&written,nullptr);
    char sl_boundary_line[1024];
    const int sl_boundary_bytes=na::slb::FormatStatus(sl_boundary_line,sizeof(sl_boundary_line));
    if(sl_boundary_bytes>0){
        WriteFile(f,sl_boundary_line,static_cast<DWORD>((std::min)(sl_boundary_bytes,static_cast<int>(sizeof(sl_boundary_line)-1))),&written,nullptr);
        WriteFile(f,"\r\n",2,&written,nullptr);
    }
    char safe_line[144];const int safe_bytes=snprintf(safe_line,sizeof(safe_line),"safe_start=%u reason=%s renderer_ready=%u shared_writer=%u clean_exit_hook=%u\r\n",na::settings::SafeStart()?1u:0u,na::settings::SafeStartReason(),na::host::SessionReady()?1u:0u,na::settings::CanPersist()?1u:0u,na::host::CleanExitObserved()?1u:0u);
    if(safe_bytes>0)WriteFile(f, safe_line, static_cast<DWORD>((std::min)(safe_bytes, static_cast<int>(sizeof(safe_line)-1))), &written, nullptr);
    char fg_inputs[512];
    const int fg_input_bytes = snprintf(fg_inputs, sizeof(fg_inputs),
        "native_fg_inputs evaluates=%llu available_resources=%llu hudless=%llu distortion=%llu list_type=%u color_format=%u allocation=%ux%u nr_route=%s\r\n",
        static_cast<unsigned long long>(ngx.fg_evaluates), static_cast<unsigned long long>(ngx.fg_with_inputs),
        static_cast<unsigned long long>(ngx.fg_with_hudless), static_cast<unsigned long long>(ngx.fg_with_distortion),
        ngx.fg_list_type, ngx.fg_color_format, ngx.fg_color_w, ngx.fg_color_h,na::nr::native::GetStatus().source==2?"pre_streamline_present":na::nr::native::OwnsRoute()?"pre_native_fg":"present");
    if (fg_input_bytes > 0) WriteFile(f, fg_inputs, static_cast<DWORD>((std::min)(fg_input_bytes, static_cast<int>(sizeof(fg_inputs)-1))), &written, nullptr);
    const int fg_contract_bytes = snprintf(fg_inputs, sizeof(fg_inputs),
        "native_fg_contract frame_id_calls=%llu alpha_calls=%llu ui_calls=%llu last_create_uir=%u\r\n",
        static_cast<unsigned long long>(ngx.fg_with_frame_id), static_cast<unsigned long long>(ngx.fg_with_alpha),
        static_cast<unsigned long long>(ngx.fg_with_ui), ngx.fg_last_create_uir);
    if (fg_contract_bytes > 0) WriteFile(f, fg_inputs, static_cast<DWORD>((std::min)(fg_contract_bytes, static_cast<int>(sizeof(fg_inputs)-1))), &written, nullptr);
    if (na::nr::native::OwnsRoute()) {
        const auto native=na::nr::native::GetStatus();
        const int bytes=snprintf(fg_inputs,sizeof(fg_inputs),
            "nr_pre_fg evaluated=%llu reused=%llu submissions=%llu retired=%llu skipped=%llu gpu_ms=%.3f samples=%u source=%u reason=%s\r\n",
            static_cast<unsigned long long>(native.evaluates),static_cast<unsigned long long>(native.reused),
            static_cast<unsigned long long>(native.submitted),static_cast<unsigned long long>(native.completed),
            static_cast<unsigned long long>(native.skipped),native.avg_ms,native.gpu_samples,native.source,native.reason);
        if(bytes>0)WriteFile(f,fg_inputs,static_cast<DWORD>((std::min)(bytes,static_cast<int>(sizeof(fg_inputs)-1))),&written,nullptr);
        const int state_bytes=snprintf(fg_inputs,sizeof(fg_inputs),
            "nr_pre_fg_delivery frame_id=%llu states_known=%u color_state=0x%x hudless_state=0x%x backbuffer_is_swapchain=%u output_real=%u output_real_is_swapchain=%u identity=%u inner=%u/%u list_type=%u states_ready=%u read_contract_mask=0x%x hudless_replaced=%u\r\n",
            static_cast<unsigned long long>(native.frame_id),native.input_states_known,native.color_state,native.hudless_state,
            native.backbuffer_is_swapchain,native.output_real_present,native.output_real_is_swapchain,native.identity_mode,native.multi_index,native.multi_count,native.list_type,native.input_states_ready,native.state_contract_mask,native.hudless_replaced);
        if(state_bytes>0)WriteFile(f,fg_inputs,static_cast<DWORD>((std::min)(state_bytes,static_cast<int>(sizeof(fg_inputs)-1))),&written,nullptr);
        const char* names[]={"color","hudless","depth","motion"};
        for(unsigned i=0;i<4;++i){
            const int bytes_input=snprintf(fg_inputs,sizeof(fg_inputs),"nr_pre_fg_state input=%s state=0x%x evidence=%s illegal_on_queue=%u\r\n",
                names[i],native.input_states[i],na::nr::states::Name(static_cast<na::nr::states::Evidence>(native.input_evidence[i])),(native.illegal_state_mask>>i)&1u);
            if(bytes_input>0)WriteFile(f,fg_inputs,static_cast<DWORD>((std::min)(bytes_input,static_cast<int>(sizeof(fg_inputs)-1))),&written,nullptr);
        }
    }
    // Worker-thread snapshots only: no GPU waits or readbacks in callbacks.
    const auto fsr = na::ffx::GetStats();
    const auto xess = na::xess::GetStats();
    const auto dlss = na::dlss::GetStats();
    char upscalers[1024];
    int un = snprintf(upscalers, sizeof(upscalers),
        "upscalers fsr_hooks=%u fsr_contexts=%llu fsr_dispatches=%llu fsr_replaced=%llu fsr_fallbacks=%llu fsr_reason=\"%s\" xess_hooks=%u xess_inits=%llu xess_executes=%llu xess_replaced=%llu xess_fallbacks=%llu xess_reason=\"%s\" injector_evals=%llu injector_creates=%llu rejected=%llu ngx_result=0x%X reason=\"%s\"\r\n",
        fsr.hooked_functions, fsr.upscale_contexts, fsr.upscale_dispatches, fsr.replaced, fsr.fallbacks, fsr.reason,
        xess.hooked_functions, xess.contexts_initialized, xess.executes, xess.replaced, xess.fallbacks, xess.reason,
        dlss.evaluates, dlss.features_created, dlss.rejected, static_cast<unsigned>(dlss.last_ngx_result), dlss.reason);
    if (un > 0) WriteFile(f, upscalers, static_cast<DWORD>((std::min)(un, static_cast<int>(sizeof(upscalers) - 1))), &written, nullptr);
    {   // Every DLSS feature creation since the last block (flags: 1 IsHDR, 2 MVLowRes, 4 MVJittered,
        // 8 DepthInverted, 64 AutoExposure), so a changed DLSS state can be tied to a mode change.
        static uint64_t printed = 0;
        const uint64_t first = dlss.create_seq > 8 && printed < dlss.create_seq - 8 ? dlss.create_seq - 8 : printed;
        for (uint64_t s = first + 1; s <= dlss.create_seq; ++s) {
            const auto& c = dlss.creates[(s - 1) % 8];
            if (c.seq != s) continue;
            char crow[256];
            const int cn = snprintf(crow, sizeof(crow), "dlss_create seq=%llu view=0x%llX flags=0x%X hdr=%u autoexp=%u inverted=%u preset=%u quality=%u render=%ux%u out=%ux%u\r\n",
                                   static_cast<unsigned long long>(c.seq), static_cast<unsigned long long>(c.view), c.flags, (c.flags & 1u) ? 1u : 0u,
                                   (c.flags & 64u) ? 1u : 0u, (c.flags & 8u) ? 1u : 0u, c.preset, c.quality, c.rw, c.rh, c.ow, c.oh);
            if (cn > 0) WriteFile(f, crow, static_cast<DWORD>((std::min)(cn, static_cast<int>(sizeof(crow) - 1))), &written, nullptr);
        }
        printed = dlss.create_seq;
    }
    char pacing[256];
    int pn = snprintf(pacing, sizeof(pacing), "present_boundary returned=%llu thread=%lu hr=0x%08lX game_latency_handle=0x%llX queue=0x%llX\r\n",
                      static_cast<unsigned long long>(g_present_returns.load()), g_present_caller.load(),
                      static_cast<unsigned long>(g_present_result.load()),
                      static_cast<unsigned long long>(g_game_latency_handle.load()),
                      static_cast<unsigned long long>(g_present_queue_identity.load()));
    if (pn > 0) WriteFile(f, pacing, static_cast<DWORD>((std::min)(pn, static_cast<int>(sizeof(pacing)-1))), &written, nullptr);
    {
        const auto j2 = na::j2::GetStats();
        uint64_t mb_s = 0, mb_st = 0;
        na::observe::MipBiasCounts(&mb_s, &mb_st);
        uint64_t sel_depth = 0, sel_color = 0;
        na::observe::GetLiveSelection(&sel_depth, &sel_color);
        char jl[1536];
        int jn = snprintf(jl, sizeof(jl),
            "j2 on=%u active=%u depth=%llu color=%llu submissions=%llu frames=%llu no_selection=%llu no_scene_list=%llu no_state=%llu (colour %llu depth %llu) no_camera=%llu busy=%llu dlss_failed=%llu cuts=%llu camera_offset=%llu later_passes=%u duplicates=%llu jitter_writes=%llu cam_learned=%llu cam_scans=%llu inv_delta=%lld proj_delta=%lld proj_writes=%llu rebased=%llu mip_bias=%.2f biased_samplers=%llu biased_static=%llu post_cam=%llu post_depth=%llu other_cam=%llu reversed=%u preset=%u sharpness=%u nis_frames=%llu scene_batches=%u/%u target=%u waited=%llu post_scene=%llu inline=%u/%u cands=%llu inl_wait=%llu inl_rec=%llu inl_frames=%llu inl_refused=%llu shape=sub%u/mips%u/slices%u/msaa%u depth_slices%u/msaa%u size=%ux%u format=%u \"%s\"\r\n",
            na::j2::Enabled() ? 1u : 0u, j2.active ? 1u : 0u, static_cast<unsigned long long>(sel_depth), static_cast<unsigned long long>(sel_color),
            static_cast<unsigned long long>(j2.submissions), static_cast<unsigned long long>(j2.frames), static_cast<unsigned long long>(j2.no_selection),
            static_cast<unsigned long long>(j2.no_scene_list), static_cast<unsigned long long>(j2.no_state), static_cast<unsigned long long>(j2.no_state_color), static_cast<unsigned long long>(j2.no_state_depth), static_cast<unsigned long long>(j2.no_camera),
            static_cast<unsigned long long>(j2.busy), static_cast<unsigned long long>(j2.dlss_failed), static_cast<unsigned long long>(j2.cuts),
            static_cast<unsigned long long>(j2.camera_offset), j2.later_passes, static_cast<unsigned long long>(j2.duplicate_submissions), static_cast<unsigned long long>(j2.jitter_writes), static_cast<unsigned long long>(j2.camera_learned_hits), static_cast<unsigned long long>(j2.camera_scans), static_cast<long long>(j2.camera_inv_delta), static_cast<long long>(j2.camera_proj_delta), static_cast<unsigned long long>(j2.jitter_proj_writes), static_cast<unsigned long long>(j2.jitter_rebased), na::observe::SamplerMipBias(), static_cast<unsigned long long>(mb_s), static_cast<unsigned long long>(mb_st), static_cast<unsigned long long>(j2.post_camera_lists), static_cast<unsigned long long>(j2.post_depth_lists), static_cast<unsigned long long>(j2.other_camera_blocks), j2.depth_reversed ? 1u : 0u, dlss.feature_preset, na::settings::Get().j2_sharpness.load(), static_cast<unsigned long long>(j2.nis_frames), j2.scene_batches, j2.scene_batches_max, j2.scene_batches_target, static_cast<unsigned long long>(j2.waited_batches), static_cast<unsigned long long>(j2.post_scene_batches), j2.inline_mode, j2.inline_target, static_cast<unsigned long long>(j2.inline_candidates), static_cast<unsigned long long>(j2.inline_waited), static_cast<unsigned long long>(j2.inline_recorded), static_cast<unsigned long long>(j2.inline_frames), static_cast<unsigned long long>(j2.inline_refused), j2.color_sub, j2.color_mips, j2.color_slices, j2.color_samples, j2.depth_slices, j2.depth_samples, j2.width, j2.height, j2.color_format, j2.reason);
        if (jn > 0) WriteFile(f, jl, static_cast<DWORD>((std::min)(jn, static_cast<int>(sizeof(jl) - 1))), &written, nullptr);
        if (na::j2::ProjectionWindow()) {
            char wl[1800];
            const int wn = snprintf(wl, sizeof wl,
                "j2_window engaged=%u corner=%ux%u mode=%u frames=%llu missed=%llu engagements=%llu render=%ux%u out=%ux%u writes{vp=%llu inv=%llu proj=%llu prev=%llu} post_lists=%llu scissored=%llu restored=%llu derived{forward=%llu inverse=%llu kinds=\"%s\"} unpacked=%llu repeated=%llu fallbacks=%llu stale_repeats=%llu extra_cameras=%llu stale_samples=%llu blocks_capped=%llu blocks_dropped=%llu blocks_per_frame=%.0f slot_per_frame=%.1f probed_out_per_frame=%.0f derived_skipped=%llu patches=%llu refused=%llu cpu_us_per_frame=%.1f cpu_max_us_call=%llu last_unpacked=\"%s\" note=\"%s\"\r\n",
                j2.window_engaged ? 1u : 0u, j2.window_w, j2.window_h, na::settings::Get().j2_scale_mode.load(),
                (unsigned long long)j2.window_frames, (unsigned long long)j2.window_missed, (unsigned long long)j2.window_engagements,
                j2.render_w, j2.render_h, j2.out_w, j2.out_h,
                (unsigned long long)j2.window_vp_writes, (unsigned long long)j2.window_inv_writes, (unsigned long long)j2.window_proj_writes,
                (unsigned long long)j2.window_prev_writes, (unsigned long long)j2.window_post_lists,
                (unsigned long long)na::j2::ProjectionWindowScissored(), (unsigned long long)na::j2::ProjectionWindowScissorRestored(),
                (unsigned long long)j2.window_derived_forward, (unsigned long long)j2.window_derived_inverse, j2.window_derived_note,
                (unsigned long long)j2.window_unpacked, (unsigned long long)j2.window_repeated, (unsigned long long)j2.window_repeat_fallbacks,
                (unsigned long long)j2.window_repeat_stale, (unsigned long long)j2.window_extra_cameras,
                (unsigned long long)j2.window_stale_samples, (unsigned long long)j2.jitter_blocks_capped, (unsigned long long)j2.jitter_blocks_dropped,
                j2.frames ? double(j2.jitter_blocks_scanned) / double(j2.frames) : 0.0, j2.frames ? double(j2.jitter_slot_blocks) / double(j2.frames) : 0.0,
                j2.frames ? double(j2.jitter_blocks_probed_out) / double(j2.frames) : 0.0,
                (unsigned long long)j2.window_derived_skipped, (unsigned long long)j2.jitter_patches,
                (unsigned long long)j2.jitter_patches_refused,
                j2.frames ? double(j2.jitter_cpu_us) / double(j2.frames) : 0.0, (unsigned long long)j2.jitter_cpu_max_us,
                j2.window_unpacked_note, j2.window_note);
            if (wn > 0) WriteFile(f, wl, DWORD((std::min)(wn, int(sizeof wl - 1))), &written, nullptr);
        }
        if (na::j2::RenderSmall()) { // render-small S1/S2 counters (Astra's formatters)
            char size_text[512] = {}, input_text[512] = {}, size_line[1100];
            na::native_present::FormatSizeStatus(size_text, sizeof size_text);
            na::native_present::FormatInputStatus(input_text, sizeof input_text);
            const int size_n = snprintf(size_line, sizeof size_line, "%s\r\n%s\r\n", size_text, input_text);
            if (size_n > 0) WriteFile(f, size_line, DWORD((std::min)(size_n, int(sizeof size_line - 1))), &written, nullptr);
        }
        if (na::native_present::SizeProbeInstalled()) { // read-only size-query probe ([present] size_probe=1)
            char probe[2400] = {};
            na::native_present::FormatSizeProbe(probe, sizeof probe - 2);
            const size_t probe_n = std::strlen(probe);
            if (probe_n) { probe[probe_n] = '\r'; probe[probe_n + 1] = '\n'; WriteFile(f, probe, DWORD(probe_n + 2), &written, nullptr); }
        }
        { // Hook cost (2026-11-05): where the CPU time inside our hooks goes
            char hl[2400];
            na::observe::FormatHookCost(hl, sizeof hl - 2);
            const size_t hn = std::strlen(hl);
            if (hn) { hl[hn] = '\r'; hl[hn + 1] = '\n'; WriteFile(f, hl, DWORD(hn + 2), &written, nullptr); }
        }
        { // Viewport jitter ([j2] viewport_jitter): real samples through the game's viewports
            const auto vps = na::observe::GetViewportJitterStats();
            char vl[400];
            const int vn = snprintf(vl, sizeof vl,
                "viewport_jitter mode=%d frames=%llu declared_list=%llu declared_submitted=%llu declared_none=%llu other_frame=%llu scene_draws=%llu sets=%llu restores=%llu unknown_viewports=%llu\r\n",
                na::settings::Get().j2_viewport_jitter.load(), (unsigned long long)j2.vpj_frames, (unsigned long long)j2.vpj_from_list,
                (unsigned long long)j2.vpj_from_submitted, (unsigned long long)j2.vpj_none, (unsigned long long)j2.vpj_other_frame,
                (unsigned long long)vps.draws, (unsigned long long)vps.sets, (unsigned long long)vps.restores, (unsigned long long)vps.unknown);
            if (vn > 0) WriteFile(f, vl, DWORD((std::min)(vn, int(sizeof vl - 1))), &written, nullptr);
        }
        { // anisotropic filtering ([j2] anisotropic): level = in use; static = changed in the twins in use
            const auto as = na::observe::GetAnisoStats();
            char al[200];
            const int an = snprintf(al, sizeof al, "anisotropic level=%u setting=%u samplers=%llu static=%llu already=%llu native_surface=%llu native_16=%llu\r\n",
                as.level, na::settings::Get().aniso16.load(), (unsigned long long)as.samplers,
                  (unsigned long long)as.static_samplers, (unsigned long long)as.already,
                  (unsigned long long)as.native_surface,(unsigned long long)as.native_16);
            if (an > 0) WriteFile(f, al, DWORD((std::min)(an, int(sizeof al - 1))), &written, nullptr);
        }
        if (na::j2::RenderSmall()) { // P4-4 render-small: one line per status write
            const auto ps = na::present::GetStats();
            char sl[900];
            const int sn = snprintf(sl, sizeof sl,
                "native_present mode=%u scale=%.3f proxies=%ux%u native=%ux%u scene=%ux%u deferred=%llu dlss=%llu stretched=%llu failed=%llu dlss_refused=%llu sharpened=%llu sharpness=%u hud_nis_frames=%llu hud_nis=%u presenter_upscaled=%llu presenter_kept_old=%llu dlss_in=%ux%u dlss_out=%ux%u jitter_phases=%u mip_bias=%.2f present_source=%llu present_refused=%llu depth_clears=%u dlaa_fresh=%llu jshift=%llu present_why=\"%s\" note=\"%s\"\r\n",
                na::settings::Get().j2_scale_mode.load(), na::present::RenderScale(), ps.scaled_w, ps.scaled_h, ps.native_w, ps.native_h,
                j2.width, j2.height, (unsigned long long)j2.small_deferred, (unsigned long long)j2.small_dlss,
                (unsigned long long)j2.small_stretched, (unsigned long long)j2.small_failed, (unsigned long long)j2.small_dlss_failed,
                (unsigned long long)j2.small_sharpened, na::settings::Get().j2_sharpness.load(),
                (unsigned long long)j2.small_hud, na::settings::Get().j2_hud_nis.load(),
                (unsigned long long)ps.upscaled, (unsigned long long)ps.upscale_failed, j2.small_rw, j2.small_rh, j2.small_ow, j2.small_oh,
                j2.jitter_phases, na::observe::SamplerMipBias(), (unsigned long long)j2.small_present_source,
                (unsigned long long)j2.small_present_refused, j2.depth_clears_last, (unsigned long long)j2.dlaa_fresh_features, (unsigned long long)j2.jitter_shifted, j2.small_present_why, j2.small_note);
            if (sn > 0) WriteFile(f, sl, DWORD((std::min)(sn, int(sizeof sl - 1))), &written, nullptr);
            {
                char wl[320];
                const int wn = snprintf(wl, sizeof wl,
                    "jitter_witness frames=%llu windows=%llu last_window{tested=%llu unshifted=%llu shifted=%llu} unseen_now=%u unseen_count=%llu retests=%llu final_colour{convention=%u epoch=%llu changes=%llu}\r\n",
                    (unsigned long long)j2.witness_frames, (unsigned long long)j2.witness_windows, (unsigned long long)j2.witness_tested,
                    (unsigned long long)j2.witness_unshifted, (unsigned long long)j2.witness_shifted, j2.jitter_unseen ? 1u : 0u,
                    (unsigned long long)j2.jitter_unseen_count, (unsigned long long)j2.jitter_retests,
                    j2.jitter_input_convention, (unsigned long long)j2.jitter_input_epoch, (unsigned long long)j2.jitter_input_changes);
                if (wn > 0) WriteFile(f, wl, DWORD((std::min)(wn, int(sizeof wl - 1))), &written, nullptr);
            }
            {
                char hl[320];
                const int hn = snprintf(hl, sizeof hl,
                    "hud_fallback on=%u frames=%llu applied=%llu learn_only=%llu last{mask_px=%u tested_px=%u cover_permille=%u}\r\n",
                    na::j2::HudMaskEnabled() ? 1u : 0u, (unsigned long long)j2.hud_fb_frames, (unsigned long long)j2.hud_fb_applied,
                    (unsigned long long)j2.hud_fb_learn_only, j2.hud_fb_mask_px, j2.hud_fb_tested_px, j2.hud_fb_cover_permille);
                if (hn > 0) WriteFile(f, hl, DWORD((std::min)(hn, int(sizeof hl - 1))), &written, nullptr);
            }
            const std::string hud = na::j2::native_hud::StatusLine() + " composited=" + std::to_string(j2.small_hud_layer) +
                " chain_fmt=" + std::to_string(g_presented_format.load(std::memory_order_relaxed)) +
                " color_space=" + std::to_string(g_color_space.load(std::memory_order_acquire)) + "\r\n";
            WriteFile(f, hud.data(), DWORD(hud.size()), &written, nullptr);
            if (na::j2::hud_unblend::Configured()) {
                const std::string ub = na::j2::hud_unblend::StatusLine() + " composited=" + std::to_string(j2.small_hud_layer) + "\r\n";
                WriteFile(f, ub.data(), DWORD(ub.size()), &written, nullptr);
            }
            {
                const std::string trace = na::j2::native_hud::TakeTrace();
                if (!trace.empty()) WriteFile(f, trace.data(), DWORD(trace.size()), &written, nullptr);
            }
        }
        {
            const auto automatic=na::scale::GetAutomaticSceneStats();
            if(automatic.enabled){char automatic_line[1400];const int automatic_bytes=snprintf(automatic_line,sizeof automatic_line,
                "layer2 mode=%u active=%u generation=%llu render=%ux%u output=%ux%u members=%u plans=%u operations=%llu redirected=%llu checkpoints=%llu misses=%llu frames=%llu presents=%llu reduced_frames=%llu reduced_pct=%.2f fast_hits=%llu snapshots=%llu failed_plans=%llu poll_builds=%llu group_builds=%llu unknown_ops=%llu binding_failures=%llu native_boundaries=%llu transfer_failures=%llu control_wait_us=%llu control_hold_us=%llu control_max_us=%llu worker_us=%llu reason=\"%s\"\r\n",
                na::settings::Get().j2_scale_mode.load(),automatic.active?1u:0u,(unsigned long long)automatic.generation,
                automatic.render_w,automatic.render_h,automatic.out_w,automatic.out_h,automatic.members,automatic.plans,
                (unsigned long long)automatic.operations,(unsigned long long)automatic.redirected,(unsigned long long)automatic.checkpoints,
                (unsigned long long)automatic.misses,(unsigned long long)automatic.frames,(unsigned long long)automatic.presents,
                (unsigned long long)automatic.redirected_frames,automatic.presents?100.0*automatic.redirected_frames/automatic.presents:0.0,
                (unsigned long long)automatic.fast_hits,(unsigned long long)automatic.snapshots,(unsigned long long)automatic.failed_plans,
                (unsigned long long)automatic.poll_builds,(unsigned long long)automatic.group_builds,(unsigned long long)automatic.unknown_operations,
                (unsigned long long)automatic.binding_failures,(unsigned long long)automatic.native_boundaries,(unsigned long long)automatic.transfer_failures,
                (unsigned long long)automatic.control_wait_us,(unsigned long long)automatic.control_hold_us,(unsigned long long)automatic.control_max_us,
                (unsigned long long)automatic.worker_us,automatic.reason);
                if(automatic_bytes>0)WriteFile(f,automatic_line,DWORD((std::min)(automatic_bytes,int(sizeof automatic_line-1))),&written,nullptr);
                const int heap_bytes=snprintf(automatic_line,sizeof automatic_line,
                    "layer2_heaps shader_heaps=%llu shader_descriptors=%llu shared_tables=%llu rtv_heaps=%llu dsv_heaps=%llu heap_failures=%llu heap_hr=0x%08X failure_kinds=\"%s\"\r\n",
                    (unsigned long long)automatic.shader_heaps,(unsigned long long)automatic.shader_descriptors,(unsigned long long)automatic.shared_tables,
                    (unsigned long long)automatic.rtv_heaps,(unsigned long long)automatic.dsv_heaps,(unsigned long long)automatic.heap_failures,automatic.heap_hr,automatic.failure_kinds);
                if(heap_bytes>0)WriteFile(f,automatic_line,DWORD((std::min)(heap_bytes,int(sizeof automatic_line-1))),&written,nullptr);
            }
        }
        if (na::scale::SliderRouteActive()) {
            // P4-4 slider route: what our mode did to the game's render scale (start-up and live).
            const auto rh = na::scale::GetReadHookStats();
            const auto lv = na::scale::GetLiveStats();
            const auto dr = na::scale::GetDriverStats();
            char sl[1400];
            const int sn = snprintf(sl, sizeof(sl),
                "scale mode=%d fraction=%.4f requested=%.4f baseline_w=%u scene=%ux%u judged=%u changed=%u unchanged=%u state=\"%s\" "
                "read files=%llu patched=%llu restored=%llu unpatched=%llu async=%llu utf16=%llu mapped=%llu adopted=%llu key=%s original=%.6f file=\"%s\" note=\"%s\" "
                "live result=%s tagged=%u tag=%.6f hits=%u/%u candidates=%u trying=%d control=%d writes=%u restores=%u scanned_kb=%llu scan_ms=%llu live_note=\"%s\"\r\n",
                dr.mode, dr.fraction, dr.requested, dr.baseline_w, j2.width, j2.height, dr.judged, dr.changed, dr.unchanged, dr.state,
                static_cast<unsigned long long>(rh.files_opened), static_cast<unsigned long long>(rh.reads_patched),
                static_cast<unsigned long long>(rh.writes_restored), static_cast<unsigned long long>(rh.reads_unpatched),
                static_cast<unsigned long long>(rh.reads_async), static_cast<unsigned long long>(rh.reads_utf16),
                static_cast<unsigned long long>(rh.files_mapped),
                static_cast<unsigned long long>(rh.handles_adopted), rh.key, rh.original, rh.file, rh.note,
                na::scale::LiveResultName(lv.result), lv.tagged ? 1u : 0u, lv.tag, lv.float_hits, lv.double_hits, lv.candidates, lv.trying, lv.control,
                lv.writes, lv.restores, static_cast<unsigned long long>(lv.scanned_kb), static_cast<unsigned long long>(lv.scan_ms), lv.note);
            if (sn > 0) WriteFile(f, sl, static_cast<DWORD>((std::min)(sn, static_cast<int>(sizeof(sl) - 1))), &written, nullptr);
        }
        {
            char cl[320]; // camera choice and loss reasons (Sniper steep-view flicker, 2026-11-01)
            const int cn = snprintf(cl, sizeof(cl),
                "j2_camera contested=%llu alternatives=%llu lost_unread=%llu lost_no_vp=%llu lost_no_inverse=%llu lost_capped=%llu other_cam=%llu jitter_slot_other=%llu inverse_computed=%llu\r\n",
                static_cast<unsigned long long>(j2.camera_contested), static_cast<unsigned long long>(j2.camera_alternatives),
                static_cast<unsigned long long>(j2.camera_lost_unread), static_cast<unsigned long long>(j2.camera_lost_no_vp),
                static_cast<unsigned long long>(j2.camera_lost_no_inverse), static_cast<unsigned long long>(j2.camera_lost_capped),
                static_cast<unsigned long long>(j2.other_camera_blocks), static_cast<unsigned long long>(j2.jitter_slot_other_camera),
                static_cast<unsigned long long>(j2.camera_inverse_computed));
            if (cn > 0) WriteFile(f, cl, static_cast<DWORD>((std::min)(cn, static_cast<int>(sizeof(cl) - 1))), &written, nullptr);
            // Per present: why a frame had no DLAA / why a DLAA frame had no jitter (SE5 flicker, 2026-11-02).
            char ml[384];
            const int mn = snprintf(ml, sizeof(ml),
                "j2_misses no_dlaa{no_camera=%llu no_state=%llu no_candidate=%llu other=%llu} unjittered{no_sample=%llu not_applied=%llu not_held=%llu} camera_held=%llu extrapolated=%llu depth_assumed=%llu camera_from_j1=%llu\r\n",
                static_cast<unsigned long long>(j2.miss_no_camera), static_cast<unsigned long long>(j2.miss_no_state),
                static_cast<unsigned long long>(j2.miss_no_candidate), static_cast<unsigned long long>(j2.miss_other),
                static_cast<unsigned long long>(j2.unjit_no_sample), static_cast<unsigned long long>(j2.unjit_not_applied),
                static_cast<unsigned long long>(j2.unjit_not_held), static_cast<unsigned long long>(j2.camera_held),
                static_cast<unsigned long long>(j2.camera_extrapolated), static_cast<unsigned long long>(j2.depth_state_assumed),
                static_cast<unsigned long long>(j2.camera_from_j1));
            if (mn > 0) WriteFile(f, ml, static_cast<DWORD>((std::min)(mn, static_cast<int>(sizeof(ml) - 1))), &written, nullptr);
            char sources[640];
            const int sn=snprintf(sources,sizeof sources,
                "j2_input_sources evaluated{depth_local=%llu depth_inherited=%llu depth_supplied=%llu depth_submitted=%llu camera_fresh=%llu camera_j1=%llu camera_still=%llu camera_moving=%llu camera_predicted=%llu camera_saved=%llu} last{depth=%u camera=%u age=%llu max_age=%llu}\r\n",
                (unsigned long long)j2.depth_sources[0],(unsigned long long)j2.depth_sources[1],
                (unsigned long long)j2.depth_sources[2],(unsigned long long)j2.depth_sources[3],
                (unsigned long long)j2.camera_sources[0],(unsigned long long)j2.camera_sources[1],
                (unsigned long long)j2.camera_sources[2],(unsigned long long)j2.camera_sources[3],
                (unsigned long long)j2.camera_sources[4],(unsigned long long)j2.camera_sources[5],
                j2.depth_source_last,j2.camera_source_last,(unsigned long long)j2.camera_age_last,(unsigned long long)j2.camera_age_max);
            if(sn>0)WriteFile(f,sources,static_cast<DWORD>((std::min)(sn,static_cast<int>(sizeof(sources)-1))),&written,nullptr);
            if (j2.camera_lost_evidence[0]) {
                WriteFile(f, "j2_camera_lost ", 15, &written, nullptr);
                WriteFile(f, j2.camera_lost_evidence, static_cast<DWORD>(std::strlen(j2.camera_lost_evidence)), &written, nullptr);
                WriteFile(f, "\r\n", 2, &written, nullptr);
            }
        }
        {
            // P4-0b moving-object motion (Opus; one status line, read-only snapshot).
            const auto fs = na::j2::flow::GetStats();
            char fl[640];
            const int fn = snprintf(fl, sizeof(fl),
                "j2flow on=%u available=%u active=%u latched=%u cost=%u size=%ux%u engine=%ux%u grid=%u perf=%u ds=%u pairs=%llu hinted=%llu consumed=%llu encode_only=%llu abandoned=%llu skip_busy=%llu skip_split=%llu skip_msaa=%llu skip_reset=%llu skip_resize=%llu engine_failed=%llu watchdog=%llu px_acc=%llu px_inv=%llu px_small=%llu px_cost=%llu px_off=%llu px_img=%llu px_flk=%llu mean_cost=%.1f ms=%.2f ms_avg=%.2f \"%s\"\r\n",
                fs.enabled, fs.available, fs.active, fs.latched, fs.cost, fs.width, fs.height, fs.engine_w, fs.engine_h, fs.grid, fs.perf, fs.downscale,
                static_cast<unsigned long long>(fs.pairs), static_cast<unsigned long long>(fs.hinted), static_cast<unsigned long long>(fs.consumed), static_cast<unsigned long long>(fs.encode_only),
                static_cast<unsigned long long>(fs.abandoned), static_cast<unsigned long long>(fs.skipped_busy), static_cast<unsigned long long>(fs.skipped_split),
                static_cast<unsigned long long>(fs.skipped_msaa), static_cast<unsigned long long>(fs.skipped_reset), static_cast<unsigned long long>(fs.skipped_resize),
                static_cast<unsigned long long>(fs.engine_failed), static_cast<unsigned long long>(fs.watchdog_trips),
                static_cast<unsigned long long>(fs.px_accepted), static_cast<unsigned long long>(fs.px_invalid), static_cast<unsigned long long>(fs.px_small),
                static_cast<unsigned long long>(fs.px_cost), static_cast<unsigned long long>(fs.px_offscreen), static_cast<unsigned long long>(fs.px_image),
                static_cast<unsigned long long>(fs.px_flicker),
                fs.mean_cost, fs.ms_last, fs.ms_avg, fs.reason);
            if (fn > 0) WriteFile(f, fl, static_cast<DWORD>((std::min)(fn, static_cast<int>(sizeof(fl) - 1))), &written, nullptr);
        }
        char coverage[256];
        if (j2.depth_state_evidence[0]) {
            WriteFile(f, "j2_depth_evidence ", 18, &written, nullptr);
            WriteFile(f, j2.depth_state_evidence, static_cast<DWORD>(std::strlen(j2.depth_state_evidence)), &written, nullptr);
            WriteFile(f, "\r\n", 2, &written, nullptr);
        }
        {   // Why DLSS history restarted (Cort: a blink every ~30 s while standing still).
            char cl2[400];
            const int cn2 = snprintf(cl2, sizeof cl2, "j2_cuts restarts=%llu gap=%llu motion=%llu bridged=%llu now=%llu recent=%s\r\n",
                (unsigned long long)j2.cut_restarts, (unsigned long long)j2.cut_gap, (unsigned long long)j2.cut_motion, (unsigned long long)j2.gap_bridged,
                (unsigned long long)(GetTickCount64() / 100 % 100000000), j2.recent_cuts);
            if (cn2 > 0) WriteFile(f, cl2, static_cast<DWORD>((std::min)(cn2, static_cast<int>(sizeof(cl2) - 1))), &written, nullptr);
            {
                const char* eff = na::settings::EffectiveSummary();
                if (eff && eff[0]) { WriteFile(f, eff, static_cast<DWORD>(std::strlen(eff)), &written, nullptr); WriteFile(f, "\r\n", 2, &written, nullptr); }
            }
            // aca551b cost: standalone-projection search (cached = blocks skipped, solved = solver windows) and J1 CPU per frame.
            const int psn = snprintf(cl2, sizeof cl2, "j2_pscan cached=%llu solved=%llu cpu_us_per_frame=%.1f cpu_max_us=%llu\r\n",
                (unsigned long long)j2.jitter_p_scan_cached, (unsigned long long)j2.jitter_p_scan_solved,
                j2.frames ? double(j2.jitter_cpu_us) / double(j2.frames) : 0.0, (unsigned long long)j2.jitter_cpu_max_us);
            if (psn > 0) WriteFile(f, cl2, static_cast<DWORD>((std::min)(psn, static_cast<int>(sizeof(cl2) - 1))), &written, nullptr);
        }
        {   // Frames whose DLAA ran before the last scene draw (SE5 pan-only flicker): placement + late writer.
            char ll[1000];
            const int ln = snprintf(ll, sizeof ll, "j2_late frames=%llu now=%llu recent=%s\r\n", (unsigned long long)j2.late_frames,
                (unsigned long long)(GetTickCount64() / 100 % 100000000), j2.late_detail);
            if (ln > 0) WriteFile(f, ll, static_cast<DWORD>((std::min)(ln, static_cast<int>(sizeof(ll) - 1))), &written, nullptr);
        }
        {   // Why carried resource states are lost (SE5 depth no_state: ambiguous_queue_order).
            const auto ps = na::observe::GetProducerStats();
            char ol[192];
            const int on = snprintf(ol, sizeof ol, "observe_order executes=%llu ambiguous=%llu nested_unlocked=%llu known_clears=%llu serialized=%llu\r\n",
                (unsigned long long)ps.executes, (unsigned long long)ps.ambiguous_executes, (unsigned long long)ps.nested_unlocked,
                (unsigned long long)ps.known_state_clears, (unsigned long long)ps.serialized_submissions);
            if (on > 0) WriteFile(f, ol, static_cast<DWORD>((std::min)(on, static_cast<int>(sizeof(ol) - 1))), &written, nullptr);
        }
        const DWORD trace_len = static_cast<DWORD>(std::strlen(j2.insertion_frame_trace));
        WriteFile(f, "j2_frame", 8, &written, nullptr);
        WriteFile(f, j2.insertion_frame_trace, trace_len, &written, nullptr);
        WriteFile(f, "\r\n", 2, &written, nullptr);
        const int nc = snprintf(coverage, sizeof coverage, "j2_coverage presents=%llu covered=%llu later_writes=%llu jittered=%llu jitter=(%.4f,%.4f)\r\n",
            (unsigned long long)j2.inline_presents, (unsigned long long)j2.inline_covered,
            (unsigned long long)j2.inline_late_writes, (unsigned long long)j2.inline_jittered,
            j2.inline_jitter_x, j2.inline_jitter_y);
        if (nc > 0) WriteFile(f, coverage, static_cast<DWORD>((std::min)(nc, static_cast<int>(sizeof(coverage)-1))), &written, nullptr);
        {
            static const char* kRoutes[] = {"none", "between", "inline", "interceptor"};
            static const char* kModes[] = {"auto", "forced-between", "forced-inline"};
            const auto& rt = j2.route;
            char rl[640];
            const int nroute = snprintf(rl, sizeof rl,
                "j2_route mode=%s route=%s state=%s matches=%u observed=%u gen=%u changes=%llu scene_frames=%llu executed=%llu placement_consistent=%llu contradictions=%llu unstable=%llu contribution=%s reason=\"%s\" missing=\"%s\" change=\"%s\"\r\n",
                kModes[rt.mode < 3 ? rt.mode : 0], kRoutes[rt.route < 4 ? rt.route : 0], rt.state ? "active" : "observing", rt.matches, rt.observed,
                rt.generation, (unsigned long long)rt.changes, (unsigned long long)rt.scene_frames, (unsigned long long)rt.executed_frames,
                (unsigned long long)rt.placement_consistent, (unsigned long long)rt.contradictions, (unsigned long long)rt.unstable_windows,
                rt.contribution_established ? "established" : "unknown", rt.reason, rt.missing, rt.change);
            if (nroute > 0) WriteFile(f, rl, static_cast<DWORD>((std::min)(nroute, static_cast<int>(sizeof rl - 1))), &written, nullptr);
        }
        if (j2.insertion_last_miss[0]) {
            WriteFile(f, "j2_last_miss ", 13, &written, nullptr);
            WriteFile(f, j2.insertion_last_miss, static_cast<DWORD>(std::strlen(j2.insertion_last_miss)), &written, nullptr);
            WriteFile(f, "\r\n", 2, &written, nullptr);
        }
        {
            const auto& rj = j2.inline_rejects;
            char rl[400];
            const int nrej = snprintf(rl, sizeof rl,
                "j2_inline_rejects no_learned=%llu resource=%llu writer=%llu occurrence=%llu transition=%llu candidate=%llu destination=%llu pipeline=%llu pending=%llu last_frame=%u\r\n",
                (unsigned long long)rj[0], (unsigned long long)rj[1], (unsigned long long)rj[2], (unsigned long long)rj[3],
                (unsigned long long)rj[4], (unsigned long long)rj[5], (unsigned long long)rj[6], (unsigned long long)rj[7],
                (unsigned long long)rj[8], j2.inline_frame_rejects);
            if (nrej > 0) WriteFile(f, rl, static_cast<DWORD>((std::min)(nrej, static_cast<int>(sizeof rl - 1))), &written, nullptr);
            char al[700];
            const int na2 = snprintf(al, sizeof al,
                "j2_profile accepted=%u rejected=%u ambiguous=%u entries=%u matched{class=%llu pair=%llu barrier=%llu legacy=%llu} rejected_skips=%llu barrier_vetoes=%llu barrier_ambiguous=%llu contradictions{class=%llu barrier=%llu} labelled=%llu unlabelled=%llu expired=%llu between{observed=%llu predicted=%llu} note=\"%s\"\r\n",
                j2.profile_accepted, j2.profile_rejected, j2.profile_ambiguous, j2.profile_entries,
                (unsigned long long)j2.class_matched, (unsigned long long)j2.class_pair_matched, (unsigned long long)j2.barrier_matched,
                (unsigned long long)j2.legacy_matched, (unsigned long long)j2.class_rejected_skips, (unsigned long long)j2.barrier_vetoes,
                (unsigned long long)j2.barrier_ambiguous_skips, (unsigned long long)j2.class_contradictions, (unsigned long long)j2.barrier_contradictions,
                (unsigned long long)j2.labelled_frames, (unsigned long long)j2.unlabelled_frames, (unsigned long long)j2.expired_inline,
                (unsigned long long)j2.between_observed, (unsigned long long)j2.between_predicted, j2.class_note);
            if (na2 > 0) WriteFile(f, al, static_cast<DWORD>((std::min)(na2, static_cast<int>(sizeof al - 1))), &written, nullptr);
            const int en=snprintf(al,sizeof al,"j2_entry keys=%u matched=%llu source=actual_operation rejected{new=%llu learning=%llu middle=%llu} rescued_by_boundary=%llu\r\n",j2.entry_keys,(unsigned long long)j2.entry_matched,
                (unsigned long long)j2.entry_unknown,(unsigned long long)j2.entry_learning,(unsigned long long)j2.entry_middle,(unsigned long long)j2.entry_rescued);
            if(en>0)WriteFile(f,al,static_cast<DWORD>(en),&written,nullptr);
            {
                // Burst diagnosis (2026-11-05): every distinct reason given, with counts, and
                // inline placements that fired but could not record DLAA.
                char reasons_line[1200];
                int rn = snprintf(reasons_line, sizeof reasons_line, "j2_reasons inline_record_failed=%llu kinds=%u", (unsigned long long)j2.inline_record_failed, j2.reason_kinds);
                for (uint32_t i = 0; i < j2.reason_kinds && rn > 0 && rn < static_cast<int>(sizeof reasons_line) - 90; ++i) {
                    const int m = snprintf(reasons_line + rn, sizeof reasons_line - static_cast<size_t>(rn), " | %llu:%s", (unsigned long long)j2.reason_counts[i].n, j2.reason_counts[i].text);
                    if (m < 0) break;
                    rn += m;
                }
                if (rn > 0 && rn < static_cast<int>(sizeof reasons_line) - 2) { reasons_line[rn++] = '\r'; reasons_line[rn++] = '\n'; WriteFile(f, reasons_line, static_cast<DWORD>(rn), &written, nullptr); }
            }
            const auto cs = na::capture::j2::GetStats();
            {
                // Four separate outcomes (Astra): frames without DLAA, DLAA placed before more
                // scene work, route changes / history resets, captures the recorder skipped.
                const auto& rt = j2.route;
                char sl[320];
                const int ns2 = snprintf(sl, sizeof sl,
                    "j2_summary scene_frames=%llu missed=%llu placed_before_scene_work=%llu route_changes=%llu history_resets=%llu capture_skipped=%u\r\n",
                    (unsigned long long)rt.scene_frames,
                    (unsigned long long)(rt.scene_frames > rt.executed_frames ? rt.scene_frames - rt.executed_frames : 0),
                    (unsigned long long)rt.contradictions, (unsigned long long)rt.changes, (unsigned long long)j2.history_resets,
                    cs.dropped_busy);
                if (ns2 > 0) WriteFile(f, sl, static_cast<DWORD>((std::min)(ns2, static_cast<int>(sizeof sl - 1))), &written, nullptr);
            }
            if (cs.requested) {
                char cl[300];
                const int nc2 = snprintf(cl, sizeof cl, "j2_capture requested=%u recorded=%u submitted=%u written=%u busy=%u discarded=%u retired=%u no_present=%u errors=%u triggers=%u extra_skipped=%u dir=\"%S\"\r\n",
                    cs.requested, cs.recorded, cs.submitted, cs.written, cs.dropped_busy, cs.discarded, cs.retired, cs.without_present, cs.errors,
                    cs.triggers, cs.extra_skipped,
                    na::capture::j2::Directory());
                if (nc2 > 0) WriteFile(f, cl, static_cast<DWORD>((std::min)(nc2, static_cast<int>(sizeof cl - 1))), &written, nullptr);
            }
        }
        char il[320];
        const int ni = snprintf(il, sizeof il, "j2_insertion traces=%llu changed=%llu list=%llu writes=%u reads=%u final=%u destination=%llu pipeline=%llu explicit_inline=%u\r\n",
            (unsigned long long)j2.insertion_traces, (unsigned long long)j2.insertion_changes,
            (unsigned long long)j2.insertion_list, j2.insertion_writes, j2.insertion_reads, j2.insertion_final,
            (unsigned long long)j2.insertion_destination, (unsigned long long)j2.insertion_pipeline,
            na::settings::Get().j2_inline.load() ? 1u : 0u);
        if (ni > 0) WriteFile(f, il, static_cast<DWORD>((std::min)(ni, static_cast<int>(sizeof il - 1))), &written, nullptr);
        na::observe::NativeBiasStats nbs;
        na::observe::NativeMipBiases(&nbs);
        char nl[200];
        const int nn = snprintf(nl, sizeof nl, "mip_native below0=%llu at0=%llu above0=%llu min=%.2f max=%.2f (game's own bias on surface samplers)\r\n",
                                static_cast<unsigned long long>(nbs.negative), static_cast<unsigned long long>(nbs.zero), static_cast<unsigned long long>(nbs.positive), nbs.min, nbs.max);
        if (nn > 0) WriteFile(f, nl, static_cast<DWORD>((std::min)(nn, static_cast<int>(sizeof(nl)-1))), &written, nullptr);
        // Live texture detail: were twins built, and do recordings use them or fall back?
        const auto ml = na::observe::GetMipLiveStats();
        char ml_line[420];
        // chosen = menu/saved; in_use = what recordings bind (differs while preparing);
        // changed_static = the game's static samplers changed in the twins in use (the proof).
        const int mn = snprintf(ml_line, sizeof ml_line, "mip_live enabled=%u chosen=%.2f/af%u in_use=%.2f/af%u preparing=%u (%llu/%llu) changed_static=%llu game=%.2f twin_rs=%llu twin_pso=%llu untwinned_pso=%llu fallbacks=%llu twin_failures=%llu\r\n",
                                ml.enabled ? 1u : 0u, ml.want_bias, ml.want_aniso, ml.live_bias, ml.live_aniso, ml.preparing ? 1u : 0u,
                                (unsigned long long)ml.prepare_done, (unsigned long long)ml.prepare_total, (unsigned long long)ml.live_static_changed,
                                ml.game_bias, (unsigned long long)ml.twin_root_signatures,
                                (unsigned long long)ml.twin_pipelines, (unsigned long long)ml.untwinned_pipelines, (unsigned long long)ml.fallbacks,
                                (unsigned long long)ml.twin_failures);
        if (mn > 0) WriteFile(f, ml_line, static_cast<DWORD>((std::min)(mn, static_cast<int>(sizeof(ml_line)-1))), &written, nullptr);
    }
    const auto latency = na::latency::GetStats();
    char lm[900];
    int ln = snprintf(lm, sizeof(lm), "latency request=%d available=%u confirmed=%u game_sleep=%u native_n=%llu native_ms=%.2f set=%d query=%d foreground=%u vrr=%u cpl_vsync=%u iflip=%u sync=%u flags=0x%X ready_ms=%.2f ready_p95=%.2f real_age_ms=%.2f real_age_p95=%.2f interval_ms=%.2f interval_p95=%.2f present_ms=%.2f backlog=%u\r\n", na::settings::Get().reflex_mode.load(), latency.available, latency.low_latency, latency.game_sleep, static_cast<unsigned long long>(latency.native_samples), latency.native_render_ms, latency.set_result, latency.query_result, latency.foreground, latency.fullscreen_vrr, latency.vsync_override, latency.independent_flip, pr.requested_sync, pr.requested_flags, pr.ready_age.mean, pr.ready_age.p95, pr.real_age.mean, pr.real_age.p95, pr.submit_interval.mean, pr.submit_interval.p95, pr.present_call.mean, pr.backlog);
    if (ln > 0) WriteFile(f, lm, static_cast<DWORD>((std::min)(ln, static_cast<int>(sizeof(lm)-1))), &written, nullptr);
    ln = snprintf(lm, sizeof(lm), "reflex_observers sleep=%u marker=%u dropped=%llu (incoming calls, not engine-placement qualification)\r\n", latency.sleep_observer, latency.marker_observer, static_cast<unsigned long long>(latency.audit_dropped));
    if (ln > 0) WriteFile(f, lm, static_cast<DWORD>((std::min)(ln, static_cast<int>(sizeof(lm)-1))), &written, nullptr);
    for (uint32_t kind = 0; kind < na::latency::NativeAudit::Slots; ++kind) {
        const auto& c = latency.audit.calls[kind];
        if (!c.count) continue;
        ln = snprintf(lm, sizeof(lm), "reflex_incoming kind=%u count=%llu ok=%llu device=0x%llX frame=%llu thread=%u caller=0x%llX begin_qpc=%llu end_qpc=%llu result=%d\r\n", kind, static_cast<unsigned long long>(c.count), static_cast<unsigned long long>(c.successes), static_cast<unsigned long long>(c.device), static_cast<unsigned long long>(c.frame), c.thread, static_cast<unsigned long long>(c.caller), static_cast<unsigned long long>(c.begin_qpc), static_cast<unsigned long long>(c.end_qpc), c.result);
        if (ln > 0) WriteFile(f, lm, static_cast<DWORD>((std::min)(ln, static_cast<int>(sizeof(lm)-1))), &written, nullptr);
    }
    ln = snprintf(lm, sizeof(lm), "reflex_engine attached=%u available=%u external=%u pcl=%u sleeps=%llu markers=%llu complete=%llu rejected=%llu errors=%llu result=%d\r\n", latency.engine_attached, latency.engine_available, latency.external_owner, latency.pcl_available, static_cast<unsigned long long>(latency.engine_sleep), static_cast<unsigned long long>(latency.engine_markers), static_cast<unsigned long long>(latency.engine_complete), static_cast<unsigned long long>(latency.engine_rejected), static_cast<unsigned long long>(latency.engine_errors), latency.engine_last_result);
    if (ln > 0) WriteFile(f, lm, static_cast<DWORD>((std::min)(ln, static_cast<int>(sizeof(lm)-1))), &written, nullptr);
    const auto timing_trace = na::tlou1::GetTimingTrace();
    ln=snprintf(lm,sizeof(lm),"gpu_present_copy first_ms=%.3f first_p95=%.3f interval_ms=%.3f interval_p95=%.3f samples=%u independent_queue=%u (GPU completion, not scanout)\r\n",pr.gpu_first_copy.mean,pr.gpu_first_copy.p95,pr.gpu_copy_interval.mean,pr.gpu_copy_interval.p95,pr.gpu_first_copy.count,pr.independent_queue);
    if(ln>0) WriteFile(f, lm, static_cast<DWORD>((std::min)(ln, static_cast<int>(sizeof(lm)-1))), &written, nullptr);
    ln = snprintf(lm,sizeof(lm),"engine_present seen=%llu with_frame=%llu game_callers=",timing_trace.seen,timing_trace.with_frame);
    for(unsigned i=0;i<timing_trace.count && ln<700;++i) ln+=snprintf(lm+ln,sizeof(lm)-ln,"%llX,",static_cast<unsigned long long>(timing_trace.callers[i]));
    ln+=snprintf(lm+ln,sizeof(lm)-ln,"\r\n");
    if(ln>0) WriteFile(f, lm, static_cast<DWORD>((std::min)(ln, static_cast<int>(sizeof(lm)-1))), &written, nullptr);
    const auto camera = na::fg::GetCameraStats();
    ln = snprintf(lm, sizeof(lm), "fg_camera recorded=%llu rejected=%llu matched=%llu missed=%llu\r\n", camera.recorded,camera.rejected,camera.matched,camera.missed);
    if (ln > 0) WriteFile(f, lm, static_cast<DWORD>((std::min)(ln, static_cast<int>(sizeof(lm)-1))), &written, nullptr);
    const auto fpreset = na::fg::preset::GetStatus();
    ln = snprintf(lm, sizeof(lm), "fg_driver_preset requested=%u stored=%u result=%d pending=%u restart=%u (profile request; active model unverified)\r\n", fpreset.requested, fpreset.stored, fpreset.result, fpreset.pending ? 1u : 0u, fpreset.restart ? 1u : 0u);
    if (ln > 0) WriteFile(f, lm, static_cast<DWORD>((std::min)(ln, static_cast<int>(sizeof(lm)-1))), &written, nullptr);
    const auto drv = na::fg::preset::GetUpscalerOverrides();
    const auto optc = na::ngx::GetOptimalStats();
    const auto ngxs = na::ngx::GetStats();
    ln = snprintf(lm, sizeof(lm), "dlss_control driver_read=%u sr_override=%u sr_preset=0x%X sr_global=%u rr_override=%u rr_preset=0x%X rr_global=%u preset_setting=%d rr_preset_setting=%d optimal_calls=%llu last_query=%ux%u game_asks_size=%u swaps=cap%llu/allocate%llu/getparams%llu\r\n",
                  drv.available ? 1u : 0u, drv.sr_enabled, drv.sr_preset, drv.sr_global ? 1u : 0u, drv.rr_enabled, drv.rr_preset, drv.rr_global ? 1u : 0u,
                  na::settings::Get().preset.load(), na::settings::Get().rr_preset.load(), static_cast<unsigned long long>(optc.calls), optc.last_user_w, optc.last_user_h,
                  na::ngx::OptimalQueriedFor(ngxs.last_sr_out_w, ngxs.last_sr_out_h) ? 1u : 0u,
                  static_cast<unsigned long long>(optc.swaps_capability), static_cast<unsigned long long>(optc.swaps_allocate),
                  static_cast<unsigned long long>(optc.swaps_getparams));
    if (ln > 0) WriteFile(f, lm, static_cast<DWORD>((std::min)(ln, static_cast<int>(sizeof(lm)-1))), &written, nullptr);
    {
        static uint64_t opt_logged = 0;
        na::ngx::OptimalCall calls[16];
        const uint32_t opt_n = na::ngx::GetOptimalCalls(opt_logged, calls, 16);
        for (uint32_t i = 0; i < opt_n; ++i) {
            const auto& c = calls[i];
            ln = snprintf(lm, sizeof(lm), "dlss_optimal_call seq=%llu rr=%u asked=%ux%u pq=%d nvidia=%ux%u returned=%ux%u overridden=%u\r\n",
                          static_cast<unsigned long long>(c.sequence), c.rr, c.user_w, c.user_h, c.perf_quality,
                          c.native_w, c.native_h, c.out_w, c.out_h, c.overridden);
            if (ln > 0) WriteFile(f, lm, static_cast<DWORD>((std::min)(ln, static_cast<int>(sizeof(lm)-1))), &written, nullptr);
            opt_logged = c.sequence;
        }
    }
    write_menu_progress(f);
    char nr_line[256];
    // Read a bounded receipt ring on the logging worker, never write a file
    // from the menu/input callback. Sequence gaps expose overwritten receipts.
    static uint64_t nr_write_logged=0;
    const auto nr_writes=na::settings::GetNrWrites();
    for(uint32_t i=0;i<nr_writes.count;++i) {
        const auto& change=nr_writes.entries[i];
        if(change.sequence<=nr_write_logged) continue;
        const char* source=change.source==na::settings::NrWriteSource::Overlay ? "overlay" :
            (change.source==na::settings::NrWriteSource::SafeStartup ? "safe_start" :
            (change.source==na::settings::NrWriteSource::UserIni ? "user_ini" : "default_ini"));
        const int bytes=snprintf(nr_line,sizeof(nr_line),
            "nr_setting_write seq=%llu tick=%llu thread=%u before=%u after=%u source=%s input_flags=%u\r\n",
            change.sequence,change.tick_ms,change.thread,change.before,change.after,source,change.input_flags);
        if(bytes>0 && WriteFile(f,nr_line,static_cast<DWORD>(bytes),&written,nullptr)) nr_write_logged=change.sequence;
    }
    int nn = snprintf(nr_line, sizeof(nr_line), "nr_inputs seen=%llu overwritten=%llu missing=%llu allocator_busy=%llu untracked_list=%llu\r\n",
                      static_cast<unsigned long long>(nr.inputs_seen), static_cast<unsigned long long>(nr.input_overwrites),
                      static_cast<unsigned long long>(nr.missing_inputs), static_cast<unsigned long long>(nr.busy_slots),
                      static_cast<unsigned long long>(nr.untracked_lists));
    if (nn > 0) WriteFile(f, nr_line, static_cast<DWORD>((std::min)(nn, static_cast<int>(sizeof(nr_line)-1))), &written, nullptr);
    nn = snprintf(nr_line, sizeof(nr_line), "nr_queue_sync input_submitted=%llu input_completed=%llu gpu_waits=%llu\r\n",
                  static_cast<unsigned long long>(nr.input_submitted), static_cast<unsigned long long>(nr.input_completed),
                  static_cast<unsigned long long>(nr.cross_queue_waits));
    if (nn > 0) WriteFile(f, nr_line, static_cast<DWORD>((std::min)(nn, static_cast<int>(sizeof(nr_line)-1))), &written, nullptr);
    char fg_line[256];
    int fn = snprintf(fg_line, sizeof(fg_line), "fg_inputs recorded=%llu submitted=%llu overwritten=%llu missing=%llu\r\n",
                      static_cast<unsigned long long>(fg.inputs_recorded), static_cast<unsigned long long>(fg.inputs_submitted),
                      static_cast<unsigned long long>(fg.inputs_overwritten), static_cast<unsigned long long>(fg.missing_inputs));
    if (fn > 0) WriteFile(f, fg_line, static_cast<DWORD>((std::min)(fn, static_cast<int>(sizeof(fg_line)-1))), &written, nullptr);
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
    // Native-FG games bypass our optional presenter, whose presents_game
    // remains zero. The host counter advances on both presentation routes.
    const uint64_t host_presents = g_presents.load(std::memory_order_relaxed);
    if (watchdog.Check(host_presents, now)) {
        na::settings::MarkSessionFailed("freeze");
        na::nr::native::TraceGpuProgress();
        na::nr::trace::Dump(f);
        char hl[400];
        int m = snprintf(hl, sizeof(hl), "%02u:%02u:%02u FREEZE detected: the game's presents have not advanced for %llu ms (game_presents=%llu real_presents=%llu fg_active=%u count=%u backlog=%u). Thread stacks follow (innermost first).\r\n",
                         t.wHour, t.wMinute, t.wSecond, static_cast<unsigned long long>(watchdog.StalledMs(now)),
                         static_cast<unsigned long long>(host_presents), static_cast<unsigned long long>(pr.presents_real), fg.active, fg.last_count, pr.backlog);
        if (m > 0) WriteFile(f, hl, static_cast<DWORD>((std::min)(m, static_cast<int>(sizeof(hl)-1))), &written, nullptr);
        static na::freeze::Capture cap; // static: not on this thread's stack
        na::freeze::CaptureAllThreads(&cap);
        m = snprintf(hl, sizeof(hl), "FREEZE thread coverage: captured=%u found=%u capacity=%u\r\n",
                     cap.thread_count, cap.threads_total, na::freeze::kMaxThreads);
        if (m > 0) WriteFile(f, hl, static_cast<DWORD>((std::min)(m, static_cast<int>(sizeof(hl)-1))), &written, nullptr);
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
                if (m > 0) WriteFile(f, dl, static_cast<DWORD>((std::min)(m, static_cast<int>(sizeof(dl)-1))), &written, nullptr);
            }
        }
        // Safe mode for the next launch: Frame Generation off, with the time recorded.
        if (na::settings::Get().fg_enabled.load(std::memory_order_relaxed)) {
            na::settings::Get().fg_enabled.store(0, std::memory_order_release);
            na::settings::Get().fg_frozen_at.store(static_cast<uint32_t>(time(nullptr)), std::memory_order_release);
            bool saved = na::settings::Save();
            m = snprintf(hl, sizeof(hl), "%02u:%02u:%02u FREEZE safe mode: Frame Generation switched off for the next launch (%s).\r\n", t.wHour, t.wMinute, t.wSecond, saved ? "saved" : "save FAILED");
            if (m > 0) WriteFile(f, hl, static_cast<DWORD>((std::min)(m, static_cast<int>(sizeof(hl)-1))), &written, nullptr);
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
        if (len > 0 && len < static_cast<int>(sizeof(row))) WriteFile(f, row, static_cast<DWORD>((std::min)(len, static_cast<int>(sizeof(row)-1))), &written, nullptr);
    }
    {
        auto a=na::tsushima::GetApplyStatus();
        if(a.available) {
            char row[512];
            int len=snprintf(row,sizeof(row),"ghost_apply request=%llu state=%u mode=%u size=%ux%u frames=%llu triggers=%llu thread=%u reason=\"%s\"\r\n",
                static_cast<unsigned long long>(a.request_id),static_cast<unsigned>(a.stage),a.choice.mode,
                a.choice.width,a.choice.height,static_cast<unsigned long long>(a.frame_calls),
                static_cast<unsigned long long>(a.triggers),a.trigger_thread,a.reason);
            if(len>0 && len<static_cast<int>(sizeof(row)))WriteFile(f,row,static_cast<DWORD>(len),&written,nullptr);
        }
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
        if (m > 0) WriteFile(f, cl, static_cast<DWORD>((std::min)(m, static_cast<int>(sizeof(cl)-1))), &written, nullptr);
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
    input::ReleaseBeforeUse mouse_handover[3], pad_handover;
    RECT client{};
};
InputState g_in;

struct FrameCtx {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12Resource> back_buffer;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    UINT64 fence_value = 0;
    ComPtr<ID3D12Fence> completion;
    bool signal_failed = false;
    bool Complete() const {
        if (signal_failed) return false;
        if (!fence_value) return true;
        const auto done = completion ? completion->GetCompletedValue() : UINT64_MAX;
        return done != UINT64_MAX && done >= fence_value;
    }
};

struct State {
    std::mutex mutex;
    IDXGISwapChain* swapchain = nullptr;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> texture_queue;
    ComPtr<ID3D12DescriptorHeap> rtv_heap, srv_heap;
    ComPtr<ID3D12GraphicsCommandList> list;
    HANDLE fence_event = nullptr;
    UINT64 fence_next = 1;
    std::vector<FrameCtx> frames;
    UINT backbuffer_count = 0;
    uint64_t imgui_draws = 0;
    std::vector<UINT64> imgui_slot_fences;
    UINT rtv_stride = 0, srv_stride = 0;
    std::vector<bool> srv_used;
    HWND hwnd = nullptr;
    WNDPROC orig_wndproc = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool imgui_ready = false;
    bool imgui_context_created=false;
    bool buffers_ready = false;
    bool was_open = false;
    LARGE_INTEGER last_present{};
    LARGE_INTEGER qpc_freq{};
    void* dummy_swapchain_identity = nullptr;
};
// Never destroyed: FrameCtx's D3D12 objects would be released by the CRT at exit (c0000005).
State& g_state = *new State;

// A slot is not signaled again until its previous use completes. Independent
// queues therefore cannot advance one another's completion receipts.
bool menu_draws_complete() {
    for (const auto& f : g_state.frames) if (!f.Complete()) return false;
    return true;
}
UINT64 menu_completed_prefix() {
    UINT64 result = g_state.fence_next - 1;
    for (const auto& f : g_state.frames)
        if (!f.Complete() && f.fence_value) result = (std::min)(result, f.fence_value - 1);
    return result;
}

// Diagnostic polling must never wait on the render thread or on the GPU.
void write_menu_progress(HANDLE log)
{
    const auto cf=na::j2::compute_flow::GetStats();char cfline[950];DWORD cfw=0;
    const int cfn=snprintf(cfline,sizeof cfline,"j2compute engine=amd recorded=%llu submitted=%llu pairs=%llu busy=%llu accepted=%llu total=%llu ms=%.3f reason=\"%s\" attempts=%llu paired_submissions=%llu unavailable=%llu history_only=%llu last_frame=%llu submit_age_ms=%llu sample_frame=%llu sample_age_ms=%llu samples=%llu excluded{invalid=%llu small=%llu cost=%llu offscreen=%llu image=%llu flicker=%llu} pool{recordings=%u images=%u} flow_loosen=%u sum{pairs=%llu accepted=%llu invalid=%llu small=%llu cost=%llu offscreen=%llu image=%llu flicker=%llu}\r\n",
        static_cast<unsigned long long>(cf.recorded),static_cast<unsigned long long>(cf.submitted),static_cast<unsigned long long>(cf.pairs),
        static_cast<unsigned long long>(cf.busy),static_cast<unsigned long long>(cf.accepted),static_cast<unsigned long long>(cf.total),cf.ms,cf.reason,
        (unsigned long long)cf.attempts,(unsigned long long)cf.submitted_pairs,(unsigned long long)cf.unavailable,(unsigned long long)cf.waiting_history,
        (unsigned long long)cf.last_submitted_frame,(unsigned long long)cf.submit_age_ms,(unsigned long long)cf.sample_frame,(unsigned long long)cf.sample_age_ms,(unsigned long long)cf.samples,
        (unsigned long long)cf.rejected_invalid,(unsigned long long)cf.rejected_small,(unsigned long long)cf.rejected_cost,(unsigned long long)cf.rejected_offscreen,(unsigned long long)cf.rejected_image,(unsigned long long)cf.rejected_flicker,cf.recording_slots,cf.image_slots,cf.loosen,
        (unsigned long long)cf.sum_pairs,(unsigned long long)cf.sum[0],(unsigned long long)cf.sum[1],(unsigned long long)cf.sum[2],(unsigned long long)cf.sum[3],
        (unsigned long long)cf.sum[4],(unsigned long long)cf.sum[5],(unsigned long long)cf.sum[6]);
    if(cfn>0)WriteFile(log,cfline,(std::min)(DWORD(cfn),DWORD(sizeof(cfline)-1)),&cfw,nullptr);
    {char line[160];const int n=snprintf(line,sizeof line,"j2jitter source=flow convention=%u epoch=%llu changes=%llu\r\n",
        cf.colour_jitter_convention,(unsigned long long)cf.colour_jitter_epoch,(unsigned long long)cf.colour_jitter_changes);
        if(n>0)WriteFile(log,line,DWORD((std::min)(n,int(sizeof(line)-1))),&cfw,nullptr);}
    {char fl[700];DWORD fw=0;const double acc=cf.fit_accepted?1.0/double(cf.fit_accepted):0.0;
        const int fn=snprintf(fl,sizeof fl,"j2camfit recorded=%llu collected=%llu accepted=%llu flow_passed=%llu rejected_jitter=%llu rejected_image=%llu fit_only=%llu reconstruct=%d reconstruct_frames=%llu accepted_e0_mean=%.4f accepted_e_mean=%.4f last{accepted=%d e0=%.4f e0_jitter=%.4f n0=%u e=%.4f n=%u t=[%.5f %.5f %.5f] r=[%.6f %.6f %.6f] image{tested=%u better=%u worse=%u e_fit=%.4f e_cam=%.4f regions=%u good=%u}}\r\n",
            (unsigned long long)cf.fit_recorded,(unsigned long long)cf.fit_collected,(unsigned long long)cf.fit_accepted,
            (unsigned long long)cf.fit_flow_passed,(unsigned long long)cf.fit_rejected_jitter,(unsigned long long)cf.fit_rejected_image,(unsigned long long)cf.fit_only,
            cf.camera_reconstruct?1:0,(unsigned long long)cf.reconstruct_frames,cf.fit_e0_sum*acc,cf.fit_e_sum*acc,
            cf.fit_last_accepted?1:0,cf.fit_e0,cf.fit_e_alt,cf.fit_n0,cf.fit_e,cf.fit_n,cf.fit_theta[0],cf.fit_theta[1],cf.fit_theta[2],cf.fit_theta[3],cf.fit_theta[4],cf.fit_theta[5],
            cf.fit_photo_tested,cf.fit_photo_better,cf.fit_photo_worse,cf.fit_photo_e_fit,cf.fit_photo_e_cam,cf.fit_photo_regions,cf.fit_photo_good);
        if(fn>0)WriteFile(log,fl,(std::min)(DWORD(fn),DWORD(sizeof(fl)-1)),&fw,nullptr);}
    auto ps = na::present::GetStats();
    char p_line[256];
    int pn = snprintf(p_line, sizeof(p_line), "present_gpu submitted=%llu completed=%llu owned=%u device_hr=0x%08lX queried_latency_handle=0x%llX\r\n",
        static_cast<unsigned long long>(ps.gpu_submitted), static_cast<unsigned long long>(ps.gpu_completed), ps.slots_owned,
        static_cast<unsigned long>(ps.device_hr), static_cast<unsigned long long>(ps.queried_latency_handle));
    DWORD pw = 0;
    if (pn > 0) WriteFile(log, p_line, static_cast<DWORD>(pn), &pw, nullptr);
    CURSORINFO ci{};ci.cbSize=sizeof(ci);const bool have_cursor=GetCursorInfo(&ci)!=FALSE;
    pn=snprintf(p_line,sizeof(p_line),"menu_cursor open=%u owned=%u window_thread=%lu pending=%u global_visible=%u global_shape=0x%llX game_shape=0x%llX\r\n",
        g_open.load()?1u:0u,g_cursor_owned.load()?1u:0u,g_cursor_thread.load(),g_cursor_message_pending.load()?1u:0u,
        have_cursor && (ci.flags&CURSOR_SHOWING)?1u:0u,
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(ci.hCursor)),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_game_cursor.load())));
    if(pn>0)WriteFile(log,p_line,static_cast<DWORD>(pn),&pw,nullptr);
    std::unique_lock<std::mutex> lock(g_state.mutex, std::try_to_lock);
    if (!lock.owns_lock() || !g_state.device) return;
    char line[400];
    int n = snprintf(line, sizeof(line), "menu_gpu draws=%llu submitted=%llu completed=%llu device_hr=0x%08lX observed_queue=0x%llX present_queue=0x%llX swapchain_queues=%u hits=%u buffer_queue=%u misses=%u\r\n",
                     static_cast<unsigned long long>(g_frames.load()),
                     static_cast<unsigned long long>(g_menu_draw_marker.load()),
                     static_cast<unsigned long long>(menu_completed_prefix()),
                     static_cast<unsigned long>(g_state.device->GetDeviceRemovedReason()),
                     static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_direct_queue.load())),
                     static_cast<unsigned long long>(g_last_present_queue.load()), g_swap_queues_recorded.load(),
                     g_swap_queue_hits.load(), g_swap_queue_observed.load(), g_swap_queue_misses.load());
    if (n >= static_cast<int>(sizeof(line))) n = static_cast<int>(sizeof(line)) - 1; // never write past a truncated line
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
void request_cursor_sync(HWND hwnd){
    if(hwnd && !g_cursor_message_pending.exchange(true))
        if(!PostMessageW(hwnd,kMsgCursorShow,0,0))g_cursor_message_pending.store(false);
}
// Window thread only. Messages carry no stale open/close command: always
// reconcile with current state, so rapid toggles cannot acquire twice or
// restore a cursor after a subsequent reopen.
void sync_cursor(){
    const bool open=g_open.load(std::memory_order_acquire) && g_initialized.load() &&
        g_menu_enabled.load() && !g_stopping.load();
    if(open){
        if(!g_cursor_owned.load()){
            g_cursor_thread.store(GetCurrentThreadId());g_game_cursor.store(GetCursor());
            g_saved_clip_valid=GetClipCursor(&g_saved_clip)!=FALSE;
            POINT frozen{};
            if(real_get_cursor_pos(&frozen)){g_frozen_x.store(frozen.x);g_frozen_y.store(frozen.y);}
            while(g_cursor_shows<64){++g_cursor_shows;if(ShowCursor(TRUE)>=0)break;}
            g_cursor_owned.store(true,std::memory_order_release);
        }
        ClipCursor(nullptr);
        // ImGui caches its last cursor and may not notice another overlay or
        // the game changed it. Reassert on the INPUT owner, not Present's thread.
        real_set_cursor(g_menu_cursor);
    }else if(g_cursor_owned.exchange(false,std::memory_order_acq_rel)){
        while(g_cursor_shows>0){ShowCursor(FALSE);--g_cursor_shows;}
        real_set_cursor(g_game_cursor.load());
        if(g_saved_clip_valid)ClipCursor(&g_saved_clip);
        g_saved_clip_valid=false;
    }
}
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

// Every message the GAME's window procedure receives, after our own input handling.
// Render-small: a posted live-size request becomes the game's virtual WM_SIZE, and mouse
// coordinates are mapped once into the game's render-size client (docs/NATIVE-PRESENT-SIZING.md).
LRESULT call_game_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (na::j2::RenderSmall()) {
        WINDOWPOS scratch{}; // must outlive the game's call
        // A REAL window resize (not our posted request): the new physical size and its render size
        // go together, with the presenter's exact scale and rounding.
        if (msg == WM_SIZE && wp != SIZE_MINIMIZED && msg != na::native_present::ResizeMessage()) {
            const na::native_present::Size out{LOWORD(lp), HIWORD(lp)};
            if (out.w && out.h) na::native_present::BeginWindowResize(hwnd, out, small_render_for(out, static_cast<double>(na::present::RenderScale())));
        }
        if (na::native_present::PrepareSizeMessage(hwnd, msg, wp, lp, scratch) == na::native_present::MessageAction::Consume) return 0;
        na::native_present::MapGameMouseMessage(hwnd, msg, lp);
        return CallWindowProcW(g_state.orig_wndproc, hwnd, msg, wp, lp);
    }
    return CallWindowProcW(g_state.orig_wndproc, hwnd, msg, wp, lp);
}
LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    na::latency::pcl::Message(msg, wp);
    if (msg == WM_KEYDOWN && wp == VK_HOME && !(lp & (1 << 30))) {
        g_open.store(!g_open.load(std::memory_order_relaxed), std::memory_order_release);
        sync_cursor(); // close restores even if the game stops presenting
        return 0;
    }
    if (msg == WM_KEYDOWN && !(lp & (1 << 30)) && na::j2::Enabled()) {
        const uint32_t key = na::settings::Get().j2_capture_key.load(std::memory_order_relaxed);
        if (key && wp == key && !na::capture::j2::Armed()) // J2 evidence capture, N consecutive frames
            na::capture::j2::RequestTimestamped(na::settings::Get().j2_capture_frames.load());
    }
    if (msg == kMsgCursorShow || msg == kMsgCursorRestore) {
        g_cursor_message_pending.store(false);
        sync_cursor();
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
        return call_game_wndproc(hwnd, msg, wp, lp);

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
        sync_cursor();
        return TRUE;
    default:
        return call_game_wndproc(hwnd, msg, wp, lp);
    }
}

// ---- XInput detour: the game sees a neutral pad while the menu is open;
// the menu polls the original independently (the game may stop polling). ----
DWORD WINAPI xinput_common(int slot, DWORD idx, XINPUT_STATE* st)
{
    DWORD r = g_orig_xinput[slot](idx, st);
    if (r == ERROR_SUCCESS && st && idx == 0) {
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
    XINPUT_GAMEPAD fresh_pad{};
    const auto now=GetTickCount64();
    if (now>=g_pad_retry_after) {
        bool connected=false;
        fresh_pad=input::FreshPad(g_menu_xinput.load(std::memory_order_acquire),&connected);
        // Disconnected XInput polling can be expensive. Keep sending releases
        // between probes; a connected controller is sampled every menu frame.
        g_pad_retry_after=connected ? 0 : now+1000;
    }
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
        down = g_in.mouse_handover[b].Update(down, just_opened);
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
    {
        if (!g_in.pad_handover.Update(input::Navigating(fresh_pad), just_opened)) fresh_pad = {};
        const XINPUT_GAMEPAD& g = fresh_pad;
        // Always send up/zero events on disconnect or failed polling. Skipping
        // these events leaves ImGui's previous buttons/axes held indefinitely.
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
    g_state.backbuffer_count = desc.BufferCount;
    // Mark identities without retaining swapchain buffers across resize.
    for(UINT i=0;i<desc.BufferCount;++i){ComPtr<ID3D12Resource> buffer;
        if(SUCCEEDED(real_get_buffer(sc,i,IID_PPV_ARGS(&buffer))))na::nr::native::MarkSwapchainBuffer(buffer.Get());}
    // Draw-resource lifetime follows submitted overlay draws, not the native
    // FG swapchain's image index. Allow a short GPU backlog without dropping
    // the menu, while keeping every allocator/RTV/ImGui slot fenced together.
    UINT count = (std::max)(8u, desc.BufferCount);
    if ((g_state.frames.size() != count || g_state.format != desc.BufferDesc.Format) &&
        !menu_draws_complete()) return false;
    if (g_state.frames.size() != count) {
        std::vector<FrameCtx> frames(count);
        ComPtr<ID3D12DescriptorHeap> rtv_heap;
        D3D12_DESCRIPTOR_HEAP_DESC rh{};
        rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rh.NumDescriptors = count;
        if (FAILED(g_state.device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&rtv_heap))))
            return false;
        for (UINT i = 0; i < count; ++i) {
            if (FAILED(g_state.device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                    IID_PPV_ARGS(&frames[i].completion)))) return false;
            if (FAILED(g_state.device->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frames[i].allocator))))
                return false;
            frames[i].rtv.ptr =
                rtv_heap->GetCPUDescriptorHandleForHeapStart().ptr +
                static_cast<SIZE_T>(i) * g_state.rtv_stride;
        }
        g_state.frames=std::move(frames);
        g_state.rtv_heap=std::move(rtv_heap);
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
bool acquire_back_buffer_locked(IDXGISwapChain* sc, UINT idx, D3D12_CPU_DESCRIPTOR_HANDLE target_rtv, ComPtr<ID3D12Resource>& out)
{
    if (idx >= g_state.backbuffer_count) return false;
    if (FAILED(real_get_buffer(sc, idx, IID_PPV_ARGS(&out)))) return false;
    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = g_state.format;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    g_state.device->CreateRenderTargetView(out.Get(), &rtv, target_rtv);
    return true;
}

// Only called before any menu draw, or after all old draws retired. Core
// device/queue ownership survives a failed menu allocation/backend startup.
void discard_menu_init_locked()
{
    if(g_state.imgui_context_created){
        auto& io=ImGui::GetIO();
        if(io.BackendRendererUserData)ImGui_ImplDX12_Shutdown();
        if(io.BackendPlatformUserData)ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();g_state.imgui_context_created=false;
    }
    if(g_state.hwnd && g_state.orig_wndproc)
        SetWindowLongPtrW(g_state.hwnd,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(g_state.orig_wndproc));
    g_state.orig_wndproc=nullptr;g_state.hwnd=nullptr;g_state.swapchain=nullptr;
    g_state.imgui_ready=false;g_state.buffers_ready=false;
    g_state.frames.clear();g_state.imgui_slot_fences.clear();g_state.list.Reset();
    g_state.rtv_heap.Reset();g_state.srv_heap.Reset();
    if(g_state.fence_event)CloseHandle(g_state.fence_event);
    g_state.fence_event=nullptr;g_texture_queue.store(nullptr,std::memory_order_release);g_state.texture_queue.Reset();
    g_initialized.store(0,std::memory_order_release);
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
            if(!menu_draws_complete())return false; // retry next frame; never wait in Present
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
            if (!ImGui_ImplDX12_Init(&info) || !ImGui_ImplDX12_CreateDeviceObjects()) { discard_menu_init_locked(); return false; }
            g_state.imgui_draws = 0;
            g_state.imgui_slot_fences.assign(g_state.frames.size(), 0);
        }
        return true;
    }
    struct MenuAttempt {
        bool committed=false;
        ~MenuAttempt(){if(!committed)discard_menu_init_locked();}
    } attempt;
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
    ImGui::CreateContext();g_state.imgui_context_created=true;
    if(g_fail_menu_create.exchange(false))return false;
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    // Gameplay Space/Enter must not activate the last mouse-focused switch.
    // Keep controller navigation; text fields still receive keyboard input.
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    // OS cursor ownership belongs to the window thread. In particular the
    // software fallback must not call SetCursor(NULL) from Present and leave
    // that invisible shape behind when the menu stops rendering.
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    na::ui::SetBoldText(na::settings::Get().text_bold.load() != 0);
    na::ui::ApplyStyle();
    na::ui::LoadFonts();
    if(!ImGui_ImplWin32_Init(g_state.hwnd))return false;
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
    g_state.imgui_draws = 0;
    g_state.imgui_slot_fences.assign(g_state.frames.size(), 0);
    // Compile/create the menu pipelines before this chain can start generating
    // frames. Do not first initialize the renderer during the transition from
    // the presentation worker to an inline Home frame.
    if (!ImGui_ImplDX12_CreateDeviceObjects()) return false;
    attempt.committed=true;
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
    if (just_opened) na::fg::preset::RequestUpscalerOverridesRefresh(); // NVIDIA app changes made while playing
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
    if(na::settings::SafeStart()) {
        na::ui::StatusLine(2,"The last session didn't end normally, so NeuralAlias started off. Your settings are saved; turn features back on when ready.");
        const auto profile=na::fg::preset::GetStatus();
        if(profile.available && profile.result!=0)na::ui::StatusLine(2,"The driver preset could not be restored. Check Frame Generation details.");
    }
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
    else if (save == 3) right = "Saving settings...";
    float rw = ImGui::CalcTextSize(right).x;
    ImGui::SameLine(w - pad_x - rw);
    ImGui::TextColored(save == 2 ? kWarn : kTextDim, "%s", right);
    PopFont();
    ImGui::End();
    if (close) {g_open.store(false, std::memory_order_release);request_cursor_sync(g_state.hwnd);}
}

void render_locked(IDXGISwapChain* sc, ID3D12CommandQueue* queue, bool just_opened, ID3D12Resource* frame = nullptr)
{
    if (just_opened) menu_note("menu_open: render entry\r\n");
    g_render_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    // Only the overlay's own input reads (apply_input, the ImGui Win32 backend)
    // pass the input lock; the game's reads on this same thread do not.
    struct OverlayInputScope {
        bool prev = t_overlay_input;
        OverlayInputScope() { t_overlay_input = true; }
        ~OverlayInputScope() { t_overlay_input = prev; }
    } overlay_input_scope;
    UINT idx = frame ? 0 : real_index(sc);
    if (!frame && idx >= g_state.backbuffer_count) return;
    if (g_state.imgui_slot_fences.empty()) return;
    const size_t imgui_slot = g_state.imgui_draws % g_state.imgui_slot_fences.size();
    FrameCtx& f = g_state.frames[imgui_slot];
    // Never wait for game-queue work from inside its Present callback. A
    // driver/engine may release that work only after the callback returns.
    // If this allocator is still in use, skip this overlay draw and retry.
    if (!f.Complete()) return;
    ComPtr<ID3D12Resource> back_buffer; // A71: this frame only
    if (frame) {
        back_buffer = frame;
        D3D12_RENDER_TARGET_VIEW_DESC rtv{};
        rtv.Format = g_state.format;
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        g_state.device->CreateRenderTargetView(frame, &rtv, f.rtv);
    } else if (!acquire_back_buffer_locked(sc, idx, f.rtv, back_buffer)) return;

    apply_input(just_opened);
    if (just_opened) menu_note("menu_open: input applied\r\n");
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
    if (just_opened) menu_note("menu_open: draw data built\r\n");

    ImDrawData* draw = ImGui::GetDrawData();
    if (draw->DisplaySize.x <= 0 || draw->DisplaySize.y <= 0) return;
    // New glyphs can update an EXISTING font atlas. The private upload queue
    // must not write it while an older game-queue menu draw is sampling it.
    // A completion check (not a wait) protects all atlas updates/destruction.
    // Leave the texture request pending when busy; the next frame retries.
    if (draw->Textures) {
        for (ImTextureData* texture : *draw->Textures) {
            if (texture->Status != ImTextureStatus_OK &&
                !menu_draws_complete()) return;
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
    ++g_state.imgui_draws; // backend advances once per RenderDrawData, even if submission fails
    if (just_opened) menu_note("menu_open: GPU draw recorded\r\n");
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_state.list->ResourceBarrier(1, &b);
    if (FAILED(g_state.list->Close())) return;
    ID3D12CommandList* lists[] = {g_state.list.Get()};
    g_orig_execute(queue, 1, lists);
    f.fence_value = g_state.fence_next++;
    g_state.imgui_slot_fences[imgui_slot] = f.fence_value;
    f.signal_failed = FAILED(queue->Signal(f.completion.Get(), f.fence_value));
    g_menu_draw_marker.store(f.fence_value);
    g_frames.fetch_add(1, std::memory_order_relaxed);
    if (just_opened) menu_note("menu_open: GPU draw submitted\r\n");
}

void render_with_fault_receipt(IDXGISwapChain* sc, ID3D12CommandQueue* queue, bool just_opened, ID3D12Resource* frame) {
    __try { render_locked(sc, queue, just_opened, frame); }
    __except (menu_fault(GetExceptionInformation())) {} // filter always continues the original exception
}

// The finished frame (real back buffer on the old path, the game's proxy on
// the presenter path): Neural Rendering, motion analysis, capture.
void process_frame(ID3D12CommandQueue* queue, ID3D12Resource* frame)
{
    if (!frame) return;
    DXGI_COLOR_SPACE_TYPE cs = static_cast<DXGI_COLOR_SPACE_TYPE>(g_color_space.load(std::memory_order_acquire));
    na::nr::OnPresent(queue, frame, cs);      // live Neural Rendering (before capture and the menu)
    na::motion::OnPresent(queue, frame, cs);  // A52: live optical flow (after NR, before capture)
    if (na::capture::Pending()) {
        // What J2 did on this present (SE5 flashing, 2026-11-02): DLAA frames since the last
        // present, its jitter, and the misses, so a flashing present can be matched to a cause.
        static uint64_t pf = 0, pnc = 0, pns = 0, pcut = 0, pmc = 0, pheld = 0;
        const auto js = na::j2::GetStats();
        char note[240];
        snprintf(note, sizeof note, "j2_frames=%llu jitter=%.3f,%.3f no_camera=%llu no_state=%llu cuts=%llu miss_no_candidate=%llu camera_held=%llu active=%u",
                 static_cast<unsigned long long>(js.frames - pf), js.inline_jitter_x, js.inline_jitter_y,
                 static_cast<unsigned long long>(js.no_camera - pnc), static_cast<unsigned long long>(js.no_state - pns),
                 static_cast<unsigned long long>(js.cuts - pcut), static_cast<unsigned long long>(js.miss_no_candidate - pmc),
                 static_cast<unsigned long long>(js.camera_held - pheld), js.active ? 1u : 0u);
        pf = js.frames; pnc = js.no_camera; pns = js.no_state; pcut = js.cuts; pmc = js.miss_no_candidate; pheld = js.camera_held;
        na::capture::NotePresent(note);
        na::capture::OnPresent(queue, frame); // phase-1 capture
    }
    {
        // J2 evidence capture: the presented image joins this frame's evaluation (if one
        // was captured). Executed unhooked so J2 never observes our copy as game work.
        static bool auto_armed = false;
        const uint32_t after = na::settings::Get().j2_capture_after_s.load(std::memory_order_relaxed);
        if (!auto_armed && after && na::j2::Enabled()) {
            auto_armed = true;
            na::capture::j2::RequestTimestamped(na::settings::Get().j2_capture_frames.load(), after * 1000u);
        }
        {
            static bool auto_set = false;
            if (!auto_set) {
                auto_set = true;
                const uint32_t n = na::settings::Get().j2_capture_auto.load(std::memory_order_relaxed);
                na::capture::j2::SetAutoTrigger(n != 0, n, na::settings::Get().j2_capture_cooldown_s.load(std::memory_order_relaxed) * 1000u);
            }
        }
        na::capture::j2::OnPresent(queue, frame, g_orig_execute);
    }
}

// Common prelude of a game Present: bookkeeping, lazy init, timing. Returns
// false when nothing further should happen (stopping, no queue yet, init
// failed). Takes g_state.mutex on success (caller releases via the guard it
// passes in).
bool present_prelude(IDXGISwapChain* sc, ID3D12CommandQueue*& queue,ComPtr<ID3D12CommandQueue>& retained_queue,
                     ID3D12CommandQueue* queue_hint = nullptr)
{
    if (g_stopping.load(std::memory_order_acquire)) return false;
    if (sc == g_state.dummy_swapchain_identity) return false;
    na::host::RendererObserved();
    g_presents.fetch_add(1, std::memory_order_relaxed);
    // Use the recorded per-buffer queue or recover it from submitted writes.
    retained_queue = queue_hint ? ComPtr<ID3D12CommandQueue>(queue_hint) : swapchain_queue(sc);
    // Boundary frames: the game-facing proxy is not a key in g_swap_queues;
    // sl_boundary supplies the proxy's exact creation/ResizeBuffers1 queue.
    if (retained_queue) g_swap_queue_hits.fetch_add(1, std::memory_order_relaxed);
    else {
        // GetDevice returns the device, not a supported queue-recovery API.
        // Native FG can present on a different queue than the game's last
        // Execute. Writing its real backbuffer on that last queue removes the
        // device with ACCESS_DENIED. Recover per BUFFER from actual submissions.
        DXGI_SWAP_CHAIN_DESC desc{};
        const UINT current=real_index(sc);
        if(SUCCEEDED(sc->GetDesc(&desc)))for(UINT i=0;i<desc.BufferCount;++i){
            ComPtr<ID3D12Resource> buffer;
            if(FAILED(real_get_buffer(sc,i,IID_PPV_ARGS(&buffer))))continue;
            ComPtr<ID3D12CommandQueue> observed;
            observed.Attach(na::nr::states::WatchPresentBuffer(buffer.Get()));
            if(i==current)retained_queue=observed;
        }
        if(retained_queue)g_swap_queue_observed.fetch_add(1,std::memory_order_relaxed);
        else g_swap_queue_misses.fetch_add(1,std::memory_order_relaxed);
    }
    queue=retained_queue.Get();
    if (!queue) return false; // next submitted frame supplies evidence; never guess
    g_last_present_queue.store(reinterpret_cast<uintptr_t>(queue), std::memory_order_relaxed);
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
        if (!menu_draws_complete())
            return false; // our last draw on the old chain is still in flight; try next present
        release_buffers_locked();
        g_state.frames.clear();
        g_state.rtv_heap.Reset();
        if (g_state.imgui_ready) ImGui_ImplDX12_InvalidateDeviceObjects();
        g_state.swapchain = sc;
        g_state.last_present.QuadPart = 0;
        g_retargets.fetch_add(1, std::memory_order_relaxed);
    }
    if(!g_state.device){
        if(SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&g_state.device)))){
            na::gputime::SetDevice(g_state.device.Get());
            g_seen_device.store(g_state.device.Get(),std::memory_order_release);
            if(seen_push(SeenKind::Device,g_state.device.Get()))g_device_announced.store(true,std::memory_order_release);
        }
    }
    // ImGui allocation may fail or its buffers may still be retiring. Core
    // frame work continues, but no draw may use a partially prepared menu.
    g_menu_frame_ready=g_menu_enabled.load(std::memory_order_acquire) && ensure_init_locked(sc);
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
    if (!g_menu_enabled.load(std::memory_order_acquire) || !g_state.imgui_ready || !g_menu_frame_ready) return;
    bool just_opened = open && !g_state.was_open;
    bool just_closed = !open && g_state.was_open;
    g_state.was_open = open;
    if(open || just_closed)request_cursor_sync(g_state.hwnd);
    if (!open) return;
    render_with_fault_receipt(sc, queue, just_opened, frame);
}

// Old path (no presenter): everything on the real back buffer, then the
// original Present by the caller.
void on_present(IDXGISwapChain* sc, ID3D12CommandQueue* queue_hint = nullptr)
{
    std::lock_guard<std::mutex> lock(g_state.mutex);
    ID3D12CommandQueue* queue = nullptr;
    ComPtr<ID3D12CommandQueue> retained_queue;
    if (!present_prelude(sc, queue,retained_queue,queue_hint)) return;
    na::j2::OnPresent(queue); // one game-frame boundary, never a TEST Present
    na::scale::AutomaticScenePresent();
    if (na::scale::SliderRouteActive()) na::scale::TickLive(na::j2::GetStats().width); // P4-4: confirm a live render-scale change
    UINT idx = real_index(sc);
    { // frame processing uses the actual buffer, independently of menu allocation
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

// Render-small mode changes (COMMS 4bd474c I1.2, Astra a066d76): one change at a time through the
// size controller. A choice only ASKS the game (virtual WM_SIZE); the mode, mip bias and status
// apply when the game's scene is seen at the new size (3 fresh samples after its buffers were
// rebuilt). A game that ignores the request keeps native DLAA and the whole picture; one that
// rebuilt but whose scene did not follow is asked back. No retry loop: a new choice re-asks.
// Native DLAA (scale_mode 0, the "Off" of upscaling) is the same path with target = output.
uint64_t g_small_token = 0;      // size_spoof token of the controller's current ask
bool g_small_scene_seen = false; // the first scene sample: asking before it would race the game's start
int g_small_mode_now = 0;         // the user's mode this present
uint64_t g_small_ask_after = 0;   // ms: no new ask before this (an ask the size layer refused backs off)
int g_small_refused_mode = -1;    // a mode the game did not follow (for g_small_refused_output)
na::native_present::Size g_small_refused_output{};
uint64_t g_small_display_generation = 0;
bool g_small_revalidate_display = false;
void small_note_line(const char* what, const na::native_present::ctl::Status& c)
{
    char row[256];
    snprintf(row, sizeof(row), "mode_change %s applied=%ux%u (%.3f) target=%ux%u buffers=%ux%u output=%ux%u matches=%u contradictions=%u\r\n", what,
             c.applied.w, c.applied.h, static_cast<double>(c.applied_scale), c.target.w, c.target.h, c.buffers.w, c.buffers.h,
             c.output.w, c.output.h, c.matches, c.contradictions);
    status_note(row);
}
void small_follow_mode()
{
    namespace np = na::native_present;
    namespace ctl = na::native_present::ctl;
    static bool crash_capture_on = false;
    if (!crash_capture_on) { crash_capture_on = true; AddVectoredExceptionHandler(0, &crash_capture); }
    const np::SizeState st = np::GetSizeState();
    if (!st.enabled) return;
    if (st.mode_generation != g_small_display_generation) {
        // An ignored request in one display mode says nothing about another.
        // A same-size mode transition may also have rebuilt the native scene.
        g_small_revalidate_display = g_small_display_generation != 0;
        g_small_display_generation = st.mode_generation;
        g_small_refused_mode = -1;
    }
    const uint64_t now = GetTickCount64();
    g_small_mode_now = (std::min)(static_cast<int>(na::settings::Get().j2_scale_mode.load()), 3);
    {
        const ctl::Status c = ctl::Get();
        if (!ctl::Same(c.output, {st.output.w, st.output.h})) {
            // First attach or a real output change (window resized by the user): start from what the game has.
            ctl::Reset({st.output.w, st.output.h}, {st.render.w, st.render.h}, na::present::RenderScale());
        }
    }
    // One scene sample per present: J2's selected scene colour for this frame (0x0 = UNKNOWN).
    // Fresh only: written in this frame or the previous one (OnPresent may already have advanced).
    const auto js = na::j2::GetStats();
    const bool fresh = js.seen_scene_frame && js.frame_now >= js.seen_scene_frame && js.frame_now - js.seen_scene_frame <= 1;
    const ctl::Extent scene = fresh ? ctl::Extent{js.seen_scene_w, js.seen_scene_h} : ctl::Extent{};
    if (scene.w && scene.h) g_small_scene_seen = true;
    ctl::Observe(scene);
    auto carry_out = [&](const ctl::Action& a) {
        const ctl::Status c = ctl::Get();
        switch (a.kind) {
        case ctl::ActionKind::Ask:
        case ctl::ActionKind::Restore: {
            if (g_small_token) np::CompleteResize(g_small_token, false, {}); // supersede the old ask's advertisement
            g_small_token = np::RequestRenderSize({a.size.w, a.size.h}, true);
            ctl::Asked(g_small_token, now);
            if (!g_small_token) { // refused by the size layer: say so once, try again in a second
                g_small_ask_after = now + 1000;
                small_note_line("ask_refused", ctl::Get());
                break;
            }
            if (a.kind == ctl::ActionKind::Restore) { g_small_refused_mode = g_small_mode_now; g_small_refused_output = st.output; }
            small_note_line(a.kind == ctl::ActionKind::Ask ? "ask" : "restore", ctl::Get());
            break;
        }
        case ctl::ActionKind::Commit: {
            if (g_small_token) np::CompleteResize(g_small_token, true, {a.size.w, a.size.h});
            g_small_token = 0;
            na::present::SetRenderScale(a.scale, &na::j2::PresentUpscale, nullptr);
            // Texture detail follows the applied mode: the menu bias plus log2(render scale).
            na::observe::SetSamplerMipBiasLive(na::j2::SmallMipBias(static_cast<float>(na::settings::Get().j2_mip_bias_x100.load()) / 100.f, a.scale));
            small_note_line(a.scale < 1.f ? "applied" : "applied_native_dlaa", c);
            break;
        }
        case ctl::ActionKind::Ignored: {
            if (g_small_token) np::CompleteResize(g_small_token, false, {});
            g_small_token = 0;
            g_small_refused_mode = g_small_mode_now; g_small_refused_output = st.output;
            small_note_line("not_followed", c);
            break;
        }
        default: break;
        }
    };
    const int mode = (std::min)(static_cast<int>(na::settings::Get().j2_scale_mode.load()), 3); // no Ultra Performance here
    carry_out(ctl::Tick(now));
    if (!g_small_scene_seen) return;
    {
        // Another request (a real window resize) is pending: never ask over it. One that never
        // reached an allocation expires, so it cannot hold the changeover forever.
        const np::SizeState now_st = np::GetSizeState();
        if (now_st.request && now_st.request != g_small_token) {
            if (np::ExpireResize(5000, now)) status_note("mode_change window_resize_expired\r\n");
            return;
        }
    }
    if (now < g_small_ask_after) return;
    const float f = mode > 0 ? static_cast<float>(na::scale::FractionForMode(mode)) : 1.f;
    const np::Size want = small_render_for(st.output, static_cast<double>(f));
    // A choice the game did not follow is not asked again for this output until the user changes it.
    if (g_small_refused_mode >= 0 && (g_small_refused_mode != mode || g_small_refused_output.w != st.output.w ||
                                      g_small_refused_output.h != st.output.h)) g_small_refused_mode = -1;
    if (g_small_refused_mode == mode && !g_small_revalidate_display) return;
    const auto change = ctl::Choose({want.w, want.h}, f, now, g_small_revalidate_display);
    if (change.kind == ctl::ActionKind::Ask && g_small_revalidate_display) {
        g_small_refused_mode = -1; // the old mode's timeout may have completed during Tick above
        g_small_revalidate_display = false;
    }
    carry_out(change);
}
HRESULT presenter_present(IDXGISwapChain* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp)
{
    std::lock_guard<std::mutex> lock(g_state.mutex);
    ID3D12CommandQueue* queue = nullptr;
    ComPtr<ID3D12CommandQueue> retained_queue;
    if (!present_prelude(sc, queue,retained_queue)) {
        // No queue or not initialised yet: the frame sits in a proxy; forward so
        // the chain keeps turning (a black frame at most, at start-up).
        return g_orig_present(sc, sync, flags);
    }
    na::present::PresentArgs a;
    DXGI_SWAP_CHAIN_DESC picture_desc{};
    const HRESULT picture_desc_hr = sc->GetDesc(&picture_desc);
    if (na::j2::RenderSmall() && SUCCEEDED(picture_desc_hr))
        na::native_present::ObserveWindowMode(picture_desc.OutputWindow, picture_desc.Windowed != FALSE);
    na::j2::OnPresent(queue); // game call only; the presenter's generated frames bypass this
    if (na::j2::RenderSmall()) small_follow_mode();
    if (na::scale::SliderRouteActive()) na::scale::TickLive(na::j2::GetStats().width); // P4-4: confirm a live render-scale change
    g_present_caller.store(GetCurrentThreadId());
    g_present_queue_identity.store(reinterpret_cast<uintptr_t>(queue));
    a.sc = sc;
    a.sync = sync;
    a.flags = flags;
    a.pp = pp;
    a.queue = queue;
    g_presented_format.store(uint32_t(picture_desc.BufferDesc.Format), std::memory_order_relaxed);
    a.hdr = na::color::IsHdr(na::color::Classify(picture_desc.BufferDesc.Format, static_cast<DXGI_COLOR_SPACE_TYPE>(g_color_space.load(std::memory_order_acquire)))); // A60
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

bool boundary_owns_native(IDXGISwapChain* chain) {
    std::lock_guard<std::mutex> lock(g_boundary_m);
    return g_boundary_policy.Suppresses(reinterpret_cast<uintptr_t>(chain));
}
HRESULT STDMETHODCALLTYPE DetourPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    if (na::present::IsPresentThread()) return g_orig_present(sc, sync, flags); // never re-entered by design; safety
    if (!(flags & DXGI_PRESENT_TEST)) g_native_game_presents.fetch_add(1, std::memory_order_acq_rel);
    na::dlss::ReleaseIfDeviceRemoved(); // G2-004: never pin a removed device across the game's recovery
    if (!(flags & DXGI_PRESENT_TEST) && !boundary_owns_native(sc) && na::present::Proxied(sc)) {
        // Discovery must see the game's Present here too: without it, its frame never ends (scores add up
        // from boot) and the swapchain size stays unknown, so after a render-size change a shadow map won
        // the depth pick and the scene was never found again (Anno, 2026-11-03).
        const auto observed = na::observe::PresentBegin(sc);
        const HRESULT result = presenter_present(sc, sync, flags, nullptr);
        na::observe::PresentEnd(observed, sync, flags, result);
        return result;
    }
    const auto observed = na::observe::PresentBegin(sc);
    na::nr::trace::Record("present_begin",sc);
    if (!(flags & DXGI_PRESENT_TEST) && !boundary_owns_native(sc)) on_present(sc);
    const HRESULT result = g_orig_present(sc, sync, flags);
    na::nr::trace::Record("present_end",sc,nullptr,0,0,result);
    na::observe::PresentEnd(observed,sync,flags,result);
    return result;
}
HRESULT STDMETHODCALLTYPE DetourPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                         const DXGI_PRESENT_PARAMETERS* pp)
{
    if (na::present::IsPresentThread()) return g_orig_present1(sc, sync, flags, pp);
    if (!(flags & DXGI_PRESENT_TEST)) g_native_game_presents.fetch_add(1, std::memory_order_acq_rel);
    na::dlss::ReleaseIfDeviceRemoved(); // G2-004: never pin a removed device across the game's recovery
    if (!(flags & DXGI_PRESENT_TEST) && !boundary_owns_native(sc) && na::present::Proxied(sc)) {
        // Discovery must see the game's Present here too: without it, its frame never ends (scores add up
        // from boot) and the swapchain size stays unknown, so after a render-size change a shadow map won
        // the depth pick and the scene was never found again (Anno, 2026-11-03).
        const auto observed = na::observe::PresentBegin(sc);
        const HRESULT result = presenter_present(sc, sync, flags, pp);
        na::observe::PresentEnd(observed, sync, flags, result);
        return result;
    }
    const auto observed = na::observe::PresentBegin(sc);
    na::nr::trace::Record("present1_begin",sc);
    if (!(flags & DXGI_PRESENT_TEST) && !boundary_owns_native(sc)) on_present(sc);
    const HRESULT result = g_orig_present1(sc, sync, flags, pp);
    na::nr::trace::Record("present1_end",sc,nullptr,0,0,result);
    na::observe::PresentEnd(observed,sync,flags,result);
    return result;
}
HRESULT STDMETHODCALLTYPE DetourGetBuffer(IDXGISwapChain* sc, UINT index, REFIID riid, void** out)
{
    HRESULT hr = S_OK;
    if (!t_native_buffer_read && sc != g_state.dummy_swapchain_identity && na::present::HandleGetBuffer(sc, index, riid, out, &hr)) return hr;
    return g_orig_get_buffer(sc, index, riid, out);
}
UINT STDMETHODCALLTYPE DetourGetCurrentBackBufferIndex(IDXGISwapChain3* sc)
{
    UINT idx = 0;
    if (!t_native_buffer_read && na::present::HandleGetIndex(sc, &idx)) return idx;
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
    if(SUCCEEDED(hr)) {std::lock_guard<std::mutex> lock(g_swap_queue_m);
        for(auto& binding:g_swap_queues)if(binding.sc==sc)binding.color_space=cs;
    }
    return hr;
}
// A71 (Astra B-007): a chain the game let go of. Forget it at once so the
// next Present (from a replacement chain on the same window) initialises
// immediately instead of after the 2 s silence rule.
void on_chain_gone(IDXGISwapChain* sc)
{
    g_chain_gone.fetch_add(1, std::memory_order_relaxed);
    {std::lock_guard<std::mutex> lock(g_boundary_m);g_boundary_policy.Forget(reinterpret_cast<uintptr_t>(sc));}
    na::present::ForgetChain(sc);
    {std::lock_guard<std::mutex> lock(g_swap_queue_m);for(auto& binding:g_swap_queues)if(binding.sc==sc)binding={};}
    std::lock_guard<std::mutex> lock(g_state.mutex);
    if (g_state.swapchain != sc) return;
    release_buffers_locked();
    // A chain's destruction is not completion of injected draw submissions.
    // Keep their slot receipts and renderer objects until completion; the next
    // chain can reuse them or rebuild them through ensure_buffers_locked.
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
void retire_nr_backbuffers(IDXGISwapChain* sc) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if(FAILED(sc->GetDesc(&desc)))return;
    for(UINT i=0;i<desc.BufferCount;++i){ComPtr<ID3D12Resource> buffer;
        if(SUCCEEDED(real_get_buffer(sc,i,IID_PPV_ARGS(&buffer))))na::nr::native::RetireBackbuffer(buffer.Get());}
}
HRESULT STDMETHODCALLTYPE DetourResizeTarget(IDXGISwapChain* sc, const DXGI_MODE_DESC* requested)
{
    if (requested && na::native_present::GameCaller(_ReturnAddress())) {
        DXGI_SWAP_CHAIN_DESC desc{};
        HRESULT hr;
        { const na::native_present::NativeScope native;
          hr = sc->GetDesc(&desc); }
        DXGI_MODE_DESC physical = *requested;
        if (SUCCEEDED(hr) && na::native_present::RewriteResizeTarget(desc.OutputWindow, &physical))
            return g_orig_resize_target(sc, &physical);
    }
    return g_orig_resize_target(sc, requested);
}
HRESULT STDMETHODCALLTYPE DetourResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w,
                                              UINT h, DXGI_FORMAT fmt, UINT flags)
{
    // Reset may have retired the native FG recording without another Present.
    // Drop its completed image references before DXGI checks ResizeBuffers.
    na::nr::native::Maintain();
    retire_nr_backbuffers(sc);
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        if (g_state.swapchain == sc) release_buffers_locked();
    }
    if(!na::present::BeforeResize(sc))return DXGI_ERROR_INVALID_CALL; // outstanding buffer use; size unchanged
    na::present::TranslateResize(sc,&w,&h); // render-small: the real chain follows the window
    const auto result=g_orig_resize(sc,count,w,h,fmt,flags);
    na::present::AfterResize(sc,result);return result;
}
HRESULT STDMETHODCALLTYPE DetourResizeBuffers1(IDXGISwapChain3* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags,
                                               const UINT* node_masks, IUnknown* const* queues)
{
    na::nr::native::Maintain();
    retire_nr_backbuffers(sc);
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        if (g_state.swapchain == sc) release_buffers_locked();
    }
    if(!na::present::BeforeResize(sc))return DXGI_ERROR_INVALID_CALL; // outstanding buffer use; size unchanged
    na::present::TranslateResize(sc,&w,&h); // render-small: the real chain follows the window
    const auto result=g_orig_resize1(sc, count, w, h, fmt, flags, node_masks, queues);
    na::present::AfterResize(sc,result);
    if(SUCCEEDED(result) && queues){
        UINT actual=count;
        if(!actual){DXGI_SWAP_CHAIN_DESC1 desc{};if(SUCCEEDED(sc->GetDesc1(&desc)))actual=desc.BufferCount;}
        std::lock_guard<std::mutex> lock(g_swap_queue_m);
        SwapQueue* slot=nullptr;
        for(auto& binding:g_swap_queues)if(binding.sc==sc){slot=&binding;break;}
        // A late attachment may first learn the queue from ResizeBuffers1.
        if(!slot){slot=&g_swap_queues[g_swap_queue_next++%8];*slot={};slot->sc=sc;}
        if(slot->queues.Resized(result,actual,queues))g_swap_queues_recorded.fetch_add(1);
        else slot->queues={}; // never retain stale per-buffer ownership
    }
    return result;
}
void STDMETHODCALLTYPE ForwardWithFgInputs(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists)
{
    na::fg::Execute(q, n, lists, g_orig_execute);
}
HRESULT STDMETHODCALLTYPE TraceSignal(ID3D12CommandQueue* q,ID3D12Fence* fence,UINT64 value) {
    void* callers[4]{};
    if(na::settings::Get().nr_enabled.load(std::memory_order_relaxed))CaptureStackBackTrace(1,4,callers,nullptr);
    na::nr::trace::Record("signal_begin",q,fence,value,0,0,callers);
    const auto hr=g_trace_signal(q,fence,value);
    na::nr::trace::Record("signal_end",q,fence,value,0,hr);return hr;
}
HRESULT STDMETHODCALLTYPE TraceWait(ID3D12CommandQueue* q,ID3D12Fence* fence,UINT64 value) {
    const auto completed=fence?fence->GetCompletedValue():0;
    void* callers[4]{};
    if(na::settings::Get().nr_enabled.load(std::memory_order_relaxed))CaptureStackBackTrace(1,4,callers,nullptr);
    na::nr::trace::Record("wait_begin",q,fence,value,completed,0,callers);
    const auto hr=g_trace_wait(q,fence,value);
    na::nr::trace::Record("wait_end",q,fence,value,completed,hr);return hr;
}
// J2: live DLAA may insert its list after the game's scene list, then the
// batch continues through Neural Rendering / Frame Generation as before.
void STDMETHODCALLTYPE ForwardThroughNr(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists)
{
    na::nr::Execute(q, n, lists, ForwardWithFgInputs);
}
void STDMETHODCALLTYPE DetourExecute(ID3D12CommandQueue* q, UINT n,
                                     ID3D12CommandList* const* lists)
{
    struct TraceExecute { ID3D12CommandQueue* q; ~TraceExecute(){na::nr::trace::Record("execute_end",q);} } trace_execute{q};
    na::nr::trace::Record("execute_begin",q,n && lists?lists[0]:nullptr,n);
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
            const auto coverage = na::observe::ProducersActive()
                ? na::hooking::variants::NoteObject(Kind::CommandList, lists[i])
                : na::hooking::variants::CoverageOf(Kind::CommandList, lists[i]);
            if (coverage != na::hooking::variants::Coverage::Complete) {
                ++uncovered;
                if (na::observe::ProducersActive()) seen_push(SeenKind::List, lists[i]);
            }
        }
    }
    g_exec_active.fetch_add(1, std::memory_order_acq_rel);
    // C8: H1/H2 observation of the game's submissions ([observe] producers=1).
    const na::observe::ExecuteToken obs_token =
        na::observe::ProducersActive() ? na::observe::ExecuteBegin(q, n, lists) : na::observe::ExecuteToken{};
    na::scale::AutomaticSceneSubmission automatic_scene(q,n,lists);
    ExecuteSubscriber* sub = g_exec_sub.load(std::memory_order_acquire);
    if (sub) {
        ExecuteContext ctx{q, n, lists, g_exec_ticket.fetch_add(1, std::memory_order_relaxed) + 1, uncovered, sub->user};
        if (sub->begin) sub->begin(&ctx);
        na::overlay::BeforeGameSubmission(q,n,lists);
        na::j2::Execute(q, n, lists, ForwardThroughNr);
        na::overlay::AfterGameSubmission(q,n,lists);
        if (sub->end) sub->end(&ctx);
        g_exec_scoped.fetch_add(1, std::memory_order_relaxed);
    } else {
        na::overlay::BeforeGameSubmission(q,n,lists);
        na::j2::Execute(q, n, lists, ForwardThroughNr);
        na::overlay::AfterGameSubmission(q,n,lists);
    }
    na::nr::states::Submitted(q,n,lists);
    // Same queue-serialization scope, after actual game work and before the
    // application can signal completion/recycle its HUD buffers or tables.
    na::j2::native_hud::AfterSubmission(q,n,lists);
    if (obs_token.queue_state) na::observe::ExecuteEnd(obs_token, q, n, lists);
    g_exec_active.fetch_sub(1, std::memory_order_acq_rel);
}

} // namespace

namespace {
// ------------------------------------------------ Streamline boundary frames
// Game-facing Streamline presentation is preferred when creation/resize gives
// us its exact queue. Native generated Presents never draw the same menu again.
void append_status_text(const char* text, int n)
{
    if (!g_status_log[0] || n <= 0) return;
    HANDLE f = CreateFileW(g_status_log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, text, static_cast<DWORD>(n), &written, nullptr);
    CloseHandle(f);
}
const char* module_of(const void* address, char* out, size_t cap)
{
    if (!out || !cap) return "";
    out[0] = 0;
    HMODULE m = nullptr;
    if (!address || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                        static_cast<LPCSTR>(address), &m) || !m) {
        snprintf(out, cap, "?");
        return out;
    }
    char path[MAX_PATH] = {};
    if (!GetModuleFileNameA(m, path, MAX_PATH)) { snprintf(out, cap, "?"); return out; }
    const char* base = strrchr(path, '\\');
    snprintf(out, cap, "%s+0x%llX", base ? base + 1 : path,
             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(m)));
    return out;
}
const void* present_slot_of(IDXGISwapChain* sc)
{
    if (!sc) return nullptr;
    void* const* vt = *reinterpret_cast<void* const* const*>(sc);
    return vt ? vt[kSwapPresentSlot] : nullptr;
}
// Once, when the boundary takes over: where each chain's Present slot points,
// so the next log shows why the native detour was bypassed.
void note_boundary_engaged(IDXGISwapChain* game_facing, IDXGISwapChain* base)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    char proxy_mod[160], base_mod[160], hooked_mod[160], hooked1_mod[160];
    const void* proxy_present = present_slot_of(game_facing);
    const void* base_present = present_slot_of(base);
    char line[1024];
    const int n = snprintf(line, sizeof(line),
        "%02u:%02u:%02u overlay_source=streamline: qualified game-facing queue; associated native draws suppressed "
        "(native_presents=%llu). proxy=%p present=%s base=%p present=%s hooked_present=%s hooked_present1=%s base_slot_is_hooked=%u\r\n",
        t.wHour, t.wMinute, t.wSecond,
        static_cast<unsigned long long>(g_native_game_presents.load(std::memory_order_acquire)),
        static_cast<void*>(game_facing), module_of(proxy_present, proxy_mod, sizeof(proxy_mod)),
        static_cast<void*>(base), base ? module_of(base_present, base_mod, sizeof(base_mod)) : "none",
        module_of(g_native_present_target, hooked_mod, sizeof(hooked_mod)),
        module_of(g_native_present1_target, hooked1_mod, sizeof(hooked1_mod)),
        base_present && base_present == g_native_present_target ? 1u : 0u);
    append_status_text(line, (std::min)(n, static_cast<int>(sizeof(line)) - 1));
}
void boundary_present(IDXGISwapChain* game_facing, IDXGISwapChain* base, ID3D12CommandQueue* queue)
{
    if(!game_facing || !base || game_facing==base || g_stopping.load(std::memory_order_acquire))return;
    {
        std::lock_guard<std::mutex> lock(g_boundary_m);
        if(g_boundary_policy.OwnsProxy(reinterpret_cast<uintptr_t>(game_facing)))
            g_boundary_policy.Select(reinterpret_cast<uintptr_t>(game_facing),reinterpret_cast<uintptr_t>(base));
    }
    // No route-table lock is held while entering the presenter or COM.
    // An acquired NeuralAlias buffer keeps its delivery until resize.
    const bool proxy_external=na::present::NoteExternalOwner(game_facing);
    const bool base_external=na::present::NoteExternalOwner(base);
    if(!proxy_external || !base_external)return;
    // Only the game's creation/ResizeBuffers1 queue, never the base chain's
    // private DLSS-G queue. Keep the owned reference through submission.
    ComPtr<ID3D12CommandQueue> hint(queue);
    if(!hint)hint=swapchain_queue(game_facing);
    if(!hint || hint->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)return;
    ComPtr<ID3D12Resource> buffer;ComPtr<ID3D12Device> buffer_device,queue_device;
    if(FAILED(real_get_buffer(game_facing,real_index(game_facing),IID_PPV_ARGS(&buffer))) ||
       FAILED(buffer->GetDevice(IID_PPV_ARGS(&buffer_device))) ||
       FAILED(hint->GetDevice(IID_PPV_ARGS(&queue_device))) ||
       buffer_device.Get()!=queue_device.Get() || FAILED(queue_device->GetDeviceRemovedReason()))return;
    bool first=false;
    {
        std::lock_guard<std::mutex> lock(g_boundary_m);
        first=!g_boundary_policy.OwnsProxy(reinterpret_cast<uintptr_t>(game_facing));
        if(!g_boundary_policy.Select(reinterpret_cast<uintptr_t>(game_facing),reinterpret_cast<uintptr_t>(base)))return;
    }
    g_boundary_frames.store(true,std::memory_order_release);
    if(first)note_boundary_engaged(game_facing,base);
    // If the native path attached before SL was discovered, transfer this
    // chain immediately once its submitted draws retire, without a 2s blackout.
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        if(g_state.swapchain==base && menu_draws_complete()){
            release_buffers_locked();g_state.swapchain=nullptr;g_state.last_present.QuadPart=0;
        }
    }
    on_present(game_facing,hint.Get());
}
void boundary_before_resize(IDXGISwapChain* game_facing)
{
    if(!game_facing)return;
    na::nr::native::Maintain();
    retire_nr_backbuffers(game_facing);
    std::lock_guard<std::mutex> lock(g_state.mutex);
    if(g_state.swapchain==game_facing)release_buffers_locked();
}
void boundary_released(IDXGISwapChain* game_facing)
{
    if(game_facing)on_chain_gone(game_facing);
}
void boundary_seen(IDXGISwapChain* proxy,IDXGISwapChain* base) {
    na::present::NoteExternalOwner(proxy);na::present::NoteExternalOwner(base);
}
na::slb::OverlayHooks make_boundary_hooks()
{
    na::slb::OverlayHooks h;
    h.chain_seen=&boundary_seen;
    h.present = &boundary_present;
    h.before_resize = &boundary_before_resize;
    h.released = &boundary_released;
    return h;
}
const na::slb::OverlayHooks g_boundary_hooks = make_boundary_hooks();
} // namespace

bool StartCore(const char* route_tag, PanelFn controls)
{
    std::lock_guard<std::mutex> start_lock(g_start_m);
    if(g_core_started.load(std::memory_order_acquire))return true;
    QueryPerformanceFrequency(&g_state.qpc_freq);
    // Start is called with a bootstrap-local buffer; own the overlay label.
    snprintf(g_route_tag, sizeof(g_route_tag), "%s", route_tag ? route_tag : "");
    g_controls = controls;
    g_stopping.store(false);
    g_boundary_frames.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(g_boundary_m);
        g_boundary_policy.Reset();
    }

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
            {
                void* const* vt = *reinterpret_cast<void* const* const*>(sc.Get());
                g_native_present_target = vt[kSwapPresentSlot];
                g_native_present1_target = vt[kSwap1Present1Slot];
            }
            // Other startup workers (Streamline/NGX/engine adapters) also
            // install hooks. Immediate HookVTableSlot fails while ANY install
            // transaction is open. Previously that left just the first hooks
            // enabled and skipped the queue tracker. Own one coherent batch.
            bool transaction=na::hooking::BeginInstallWait("overlay core");
            auto prepare_start_hook=[](void* object,uint32_t slot,void* detour,void** original){
                const bool prepared=na::hooking::PrepareVTableSlot(object,slot,detour,original);
                if(!prepared){
                    char reason[160]{},target[160]{},line[480]{};
                    na::hooking::LastError(reason,sizeof(reason));
                    const void* address=object?(*static_cast<void***>(object))[slot]:nullptr;
                    const int n=snprintf(line,sizeof(line),"overlay_start pid=%lu slot=%u target=%s failed: %s\r\n",
                        GetCurrentProcessId(),slot,module_of(address,target,sizeof(target)),reason);
                    append_status_text(line,(std::min)(n,int(sizeof(line)-1)));
                }
                return prepared;
            };
            ok = transaction && prepare_start_hook(sc.Get(), kSwapReleaseSlot,
                                             reinterpret_cast<void*>(&DetourRelease),
                                             reinterpret_cast<void**>(&g_orig_release)) &&
                 prepare_start_hook(sc.Get(), kSwapPresentSlot,
                                             reinterpret_cast<void*>(&DetourPresent),
                                             reinterpret_cast<void**>(&g_orig_present)) &&
                 prepare_start_hook(sc.Get(), kSwap1Present1Slot,
                                             reinterpret_cast<void*>(&DetourPresent1),
                                             reinterpret_cast<void**>(&g_orig_present1)) &&
                 prepare_start_hook(sc.Get(), kSwapResizeBuffersSlot,
                                             reinterpret_cast<void*>(&DetourResizeBuffers),
                                             reinterpret_cast<void**>(&g_orig_resize)) &&
                 prepare_start_hook(queue.Get(), kQueueExecuteSlot,
                                             reinterpret_cast<void*>(&DetourExecute),
                                             reinterpret_cast<void**>(&g_orig_execute));
            // Only render-small needs to separate logical ResizeTarget from
            // physical output. Native-DLSS routes retain their existing hooks.
            if (ok && na::j2::RenderSmall())
                ok = prepare_start_hook(sc.Get(), kSwapResizeTargetSlot,
                                        reinterpret_cast<void*>(&DetourResizeTarget),
                                        reinterpret_cast<void**>(&g_orig_resize_target));
            // A59 presenter: GetBuffer / GetCurrentBackBufferIndex / ResizeBuffers1.
            // The full observer owns these slots when requested. This narrow
            // trace is for Route A freezes with that observer disabled.
            if(ok && !na::observe::InstallRequested()) {
                const bool signal=prepare_start_hook(queue.Get(),14,reinterpret_cast<void*>(&TraceSignal),reinterpret_cast<void**>(&g_trace_signal));
                const bool wait=prepare_start_hook(queue.Get(),15,reinterpret_cast<void*>(&TraceWait),reinterpret_cast<void**>(&g_trace_wait));
                na::nr::trace::sync_hooks.store((signal?1u:0u)|(wait?2u:0u));
            }
            ComPtr<IDXGISwapChain3> sc3;
            if (ok && SUCCEEDED(sc.As(&sc3))) {
                ok = prepare_start_hook(sc.Get(), kSwapGetBufferSlot,
                                                 reinterpret_cast<void*>(&DetourGetBuffer),
                                                 reinterpret_cast<void**>(&g_orig_get_buffer)) &&
                     prepare_start_hook(sc3.Get(), kSwap3GetIndexSlot,
                                                 reinterpret_cast<void*>(&DetourGetCurrentBackBufferIndex),
                                                 reinterpret_cast<void**>(&g_orig_get_index)) &&
                     prepare_start_hook(sc3.Get(), kSwap3ResizeBuffers1Slot,
                                                 reinterpret_cast<void*>(&DetourResizeBuffers1),
                                                 reinterpret_cast<void**>(&g_orig_resize1)) &&
                     prepare_start_hook(sc3.Get(), kSwap2SetMaxLatencySlot,
                                                 reinterpret_cast<void*>(&DetourSetMaximumFrameLatency),
                                                 reinterpret_cast<void**>(&g_orig_set_max_latency)) &&
                     prepare_start_hook(sc3.Get(), kSwap2GetMaxLatencySlot,
                                                 reinterpret_cast<void*>(&DetourGetMaximumFrameLatency),
                                                 reinterpret_cast<void**>(&g_orig_get_max_latency)) &&
                     prepare_start_hook(sc3.Get(), kSwap2GetLatencyObjectSlot,
                                                 reinterpret_cast<void*>(&DetourGetFrameLatencyWaitableObject),
                                                 reinterpret_cast<void**>(&g_orig_get_latency_object));
                // Colour space (HDR10 detection for Neural Rendering); optional.
                prepare_start_hook(sc3.Get(), kSwap3SetColorSpace1Slot,
                                            reinterpret_cast<void*>(&DetourSetColorSpace1),
                                            reinterpret_cast<void**>(&g_orig_set_color_space));
            }
            if (ok) {
                // Swap-chain creation on the shared factory vtable (after our own dummy
                // chain exists, so it is never recorded). Late attachments recover
                // per-buffer queues from submitted writes instead.
                if (na::j2::RenderSmall()) { // render-small size spoof on the game's descriptor queries
                    prepare_start_hook(sc.Get(), kSwapGetDescSlot, reinterpret_cast<void*>(&DetourGetDesc),
                                       reinterpret_cast<void**>(&g_orig_get_desc));
                    prepare_start_hook(sc.Get(), kSwap1GetDesc1Slot, reinterpret_cast<void*>(&DetourGetDesc1),
                                       reinterpret_cast<void**>(&g_orig_get_desc1));
                }
                prepare_start_hook(factory.Get(), kFactoryCreateSwapChainSlot,
                                            reinterpret_cast<void*>(&DetourCreateSwapChain),
                                            reinterpret_cast<void**>(&g_orig_create_swapchain));
                prepare_start_hook(factory.Get(), kFactory2CreateForHwndSlot,
                                            reinterpret_cast<void*>(&DetourCreateForHwnd),
                                            reinterpret_cast<void**>(&g_orig_create_for_hwnd));
                prepare_start_hook(factory.Get(), kFactory2CreateForCoreWindowSlot,
                                            reinterpret_cast<void*>(&DetourCreateForCoreWindow),
                                            reinterpret_cast<void**>(&g_orig_create_for_core_window));
                prepare_start_hook(factory.Get(), kFactory2CreateForCompositionSlot,
                                            reinterpret_cast<void*>(&DetourCreateForComposition),
                                            reinterpret_cast<void**>(&g_orig_create_for_composition));
            }
            if(transaction){
                if(ok)ok=na::hooking::CommitInstall();else na::hooking::AbandonInstall();
            }
            if(!ok){
                char reason[160]{},line[240]{};na::hooking::LastError(reason,sizeof(reason));
                const int n=snprintf(line,sizeof(line),"overlay_start pid=%lu core_failed: %s\r\n",GetCurrentProcessId(),reason);
                append_status_text(line,(std::min)(n,int(sizeof(line)-1)));
            }
            if (ok && na::observe::InstallRequested()) // C8: observation producers on the same throwaway objects
            {
                na::observe::InstallProducers(dev.Get(), queue.Get(), /*hook_queue_execute=*/false);
                na::observe::SetExecuteForwarder(g_orig_execute); // C9b: host sample copies bypass this detour
            }
            if (ok && na::nr::states::InstallEnhancedInvalidation()) na::observe::InstallListResetTracking(dev.Get(), na::nr::OnCommandListReset,na::nr::states::Observe);
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

    if (ok) {
        ok=na::hooking::variants::StartDiscoveryThread(&seen_drain, nullptr);
        if(ok)na::slb::SetOverlayHooks(&g_boundary_hooks);
    }
    g_core_started.store(ok,std::memory_order_release);
    return ok;
}

bool StartMenu()
{
    std::lock_guard<std::mutex> start_lock(g_start_m);
    if(!g_core_started.load(std::memory_order_acquire))return false;
    if(g_menu_enabled.load(std::memory_order_acquire) && g_input_hooks.load()==8)return true;
    if(g_fail_menu_start.exchange(false))return false;
    g_menu_cursor=LoadCursorW(nullptr,IDC_ARROW);
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
        if (target && install_input_function(target, detours[i],
                                                reinterpret_cast<void**>(&g_orig_xinput[i]))) {
            ++hooked;
            if (!g_menu_xinput.load(std::memory_order_relaxed)) g_menu_xinput.store(g_orig_xinput[i],std::memory_order_release);
        }
    }
    g_gamepad_hooked.store(hooked);
    install_input_hooks(); // A44: the game sees no mouse/keyboard while the menu is open
    const bool ready=g_input_hook_targets[0] && g_input_hook_targets[2]; // essentials; cursor/controller hooks may degrade
    g_menu_enabled.store(ready,std::memory_order_release);
    return ready;
}
bool Start(const char* route_tag, PanelFn controls)
{
    return StartCore(route_tag,controls) && StartMenu();
}
void FailNextMenuStartForTests(){g_fail_menu_start.store(true);}
void FailNextMenuCreateForTests(){g_fail_menu_create.store(true);}
void FailOptionalInputForTests(unsigned attempts){g_fail_optional_input.store(attempts);}
void FailCursorCleanupForTests(){g_fail_cursor_cleanup.store(true);}
void* CursorOriginalForTests(){return reinterpret_cast<void*>(g_orig_set_cursor);}

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
    na::slb::SetOverlayHooks(nullptr);
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
    // Restore before removing the subclass; queued messages cannot perform
    // cleanup once the original WndProc has been reinstated.
    if(g_cursor_owned.load()){
        if(GetCurrentThreadId()==g_cursor_thread.load())sync_cursor();
        else {DWORD_PTR result=0;SendMessageTimeoutW(g_state.hwnd,kMsgCursorRestore,0,0,SMTO_ABORTIFHUNG,200,&result);}
        if(g_cursor_owned.load()){g_stop_incomplete.store(1);return false;}
    }
    remove_input_hooks();
    std::lock_guard<std::mutex> lock(g_state.mutex);
    if (g_state.imgui_ready) {
        for (const auto& f : g_state.frames) if (!f.Complete()) {
            if (f.signal_failed || !f.completion || !g_state.fence_event ||
                FAILED(f.completion->SetEventOnCompletion(f.fence_value, g_state.fence_event)) ||
                WaitForSingleObject(g_state.fence_event, 2000) != WAIT_OBJECT_0 || !f.Complete()) {
                g_stop_incomplete.store(1, std::memory_order_release);
                return false; // keep GPU-owned resources alive; shutdown may retry
            }
        }
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        if (g_state.hwnd && g_state.orig_wndproc)
            SetWindowLongPtrW(g_state.hwnd, GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(g_state.orig_wndproc));
        g_state.imgui_ready = false;g_state.imgui_context_created=false;
        g_state.orig_wndproc=nullptr;
    }
    g_state.frames.clear();
    g_state.list.Reset();
    g_state.rtv_heap.Reset();
    g_state.srv_heap.Reset();
    if (g_state.fence_event) CloseHandle(g_state.fence_event);
    g_state.fence_event = nullptr;
    g_texture_queue.store(nullptr, std::memory_order_release);
    g_state.texture_queue.Reset();
    g_seen_device.store(nullptr,std::memory_order_release);
    g_state.device.Reset();
    g_state.swapchain = nullptr;
    g_state.buffers_ready = false;
    g_state.was_open = false;
    g_menu_xinput.store(nullptr,std::memory_order_release);
    g_pad_retry_after=0;
    g_initialized.store(0, std::memory_order_release);
    g_menu_enabled.store(false,std::memory_order_release);
    g_core_started.store(false,std::memory_order_release);
    g_stopping.store(false, std::memory_order_release); // a later Start may run again
    return true;
}

Stats GetStats()
{
    Stats s{};
    s.frames_rendered = g_frames.load();
    s.presents_seen = g_presents.load();
    s.initialized = g_initialized.load();
    s.core_ready=g_core_started.load();s.menu_enabled=g_menu_enabled.load();
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
    s.present_queue = g_last_present_queue.load(std::memory_order_relaxed);
    return s;
}

void SetOpen(bool open) { g_open.store(open, std::memory_order_release); }

bool RequestPictureRebuild(uint32_t render_w, uint32_t render_h)
{
    // Render-small: the game-visible window is virtual; its live resize is the posted WM_SIZE instead.
    if (na::j2::RenderSmall()) return false;
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
    na::host::ProcessLogPath(g_status_log, MAX_PATH, path);
    g_status_last_tick = 0;
}

void SetMenuDiagnostics(bool enabled) { g_menu_diagnostics = enabled; }
void DrawMenuForTests(ID3D12CommandQueue* queue,ID3D12Resource* frame) {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    if (!queue || !frame || !g_state.imgui_ready || !g_open.load()) return;
    render_locked(g_state.swapchain,queue,false,frame);
}

int SwapChainColorSpace() { return g_color_space.load(std::memory_order_acquire); }
void BoundaryPresentForTests(IDXGISwapChain* proxy,IDXGISwapChain* base,ID3D12CommandQueue* queue){boundary_present(proxy,base,queue);}
void BoundarySeenForTests(IDXGISwapChain* proxy,IDXGISwapChain* base){boundary_seen(proxy,base);}
void BoundaryResizeForTests(IDXGISwapChain* proxy){boundary_before_resize(proxy);}
void BoundaryReleaseForTests(IDXGISwapChain* proxy){boundary_released(proxy);}
long ReadNativeBufferForTests(IDXGISwapChain* chain, unsigned index, REFIID iid, void** out){return real_get_buffer(chain,index,iid,out);}
ID3D12CommandQueue* AcquirePresentQueue(IDXGISwapChain* chain){return swapchain_queue(chain).Detach();}
int SwapChainColorSpace(IDXGISwapChain* chain){
    std::lock_guard<std::mutex> lock(g_swap_queue_m);
    for(const auto& binding:g_swap_queues)if(binding.sc==chain)return binding.color_space;
    return DXGI_COLOR_SPACE_CUSTOM;
}

} // namespace na::overlay
