// In-game DX12 overlay (Dear ImGui) for NeuralAlias, toggled with Home.
//
// Attach strategy: a hidden dummy window + device + swapchain are created
// only to read the shared IDXGISwapChain/ID3D12CommandQueue vtables; hooking
// Present/Present1/ResizeBuffers/ExecuteCommandLists on those vtables makes
// the GAME's swapchain and queues route through us. On the game's first
// Present we lazily create the D3D objects and ImGui backends against the
// game's device and its verified creation/resize DIRECT queue (or submitted
// per-buffer evidence on late native attachment). Streamline chains prefer
// their game-facing Present; associated generated Presents do not draw again.
//
// Input model (A20/A42/A44, ReShade-like): while the menu is open the game
// receives NO input. The subclassed WndProc consumes raw input (WM_INPUT),
// legacy mouse/keyboard messages and WM_SETCURSOR; the WndProc never touches
// ImGui â€” it records into a mutex-guarded input state that the render
// thread applies before each ImGui frame. The REAL Windows cursor is the
// menu cursor (A42: forced visible and unclipped while open). A44: the
// user32 entry points a game may read input through directly
// (GetRawInputData/Buffer, GetCursorPos/SetCursorPos, GetAsyncKeyState,
// GetKeyboardState) are hooked so every thread but the overlay's own sees a
// frozen cursor, no buttons and no keys. Controller input uses an XInputGetState hook
// with independent live menu polling (never a stale game-poll snapshot):
// the game sees a neutral pad while the menu is open; the menu navigates
// with d-pad/left stick, A = select, B = back. Closing with Home restores
// everything. The game itself keeps running (we cannot pause its
// simulation); it simply gets no input.
#pragma once

#include <cstdint>

struct IDXGISwapChain;
struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12CommandList;
struct ID3D12Resource;

namespace na::overlay {

using PanelFn = void (*)();

// A30: the host draws the whole menu (title, navigation, four pages,
// footer) from the sections registered with na::menu. `route_tag` (e.g.
// "Â· ROUTE B") is shown only on the Details page. `controls`, when not null,
// is a legacy full-body PanelFn drawn instead of the pages (kept for
// compatibility; both routes now pass nullptr).
bool Start(const char* route_tag, PanelFn controls);
// A51: returns false, with SeamStats.stop_incomplete set and NOTHING torn
// down, when an Execute callback or the discovery worker/producers could not
// be proven quiescent within the timeout; call again later. true = fully stopped.
bool Stop();

// Presents since the DLSS evaluate counter last changed (0 when no evaluate
// has ever been seen). The menu uses it to say "connection interrupted"
// rather than pretending a stale confirmation is current.
uint64_t PresentsSinceLastEvaluate();

struct Stats {
    uint64_t frames_rendered;
    uint64_t presents_seen;
    uint32_t initialized;
    uint32_t open;
    uint32_t have_queue;
    uint32_t gamepad_hooked;    // number of XInput modules hooked
    float fps;                  // smoothed, from Present cadence
    float frame_ms;             // smoothed CPU frame interval
    float mouse_x, mouse_y;     // menu cursor (client px) last applied
    uint32_t input_hooks;       // A44: user32 input entry points hooked (6 when complete)
    uint32_t retargets;         // A31: swap chain re-targets after 2 s of silence
    uint32_t chain_gone;        // A71: chains observed destroyed at the game's final Release (immediate re-target)
    uintptr_t present_queue;   // queue selected for the current DXGI buffer
};
Stats GetStats();

void SetOpen(bool open);
// Opt-in local fault receipt for user-owned game testing; never handles a fault.
void SetMenuDiagnostics(bool enabled);

// A63 experiment (Astra's request): ask the game to rebuild its picture
// pipeline by nudging its window size by one pixel and back a few frames
// later. Engines re-create their render targets on a size change and most
// re-query DLSS's optimal settings then, at which point NeuralAlias's chosen
// picture mode takes effect. Whether THIS game does is read from the DLSS
// creation log afterwards (ngx::Stats::sr_creates / last_sr_in_w); the menu
// reports it either way.
// A66/A67: the request carries the render size it is meant to produce. At
// admission (next present) the overlay freezes that size, arms
// ngx::ArmSizeConfirmation BEFORE the window is touched (a creation the game
// performs from inside the resize is thereby observed), and marks the
// restoration boundary when the window is back; the menu only displays the
// result. A nudge that has not restored within 5 seconds fails on its own,
// whether or not any page polls. Returns false, and changes nothing, while a
// nudge is still in flight (a second Apply between shrink and restore is
// rejected, not merged); once the window is back a new request supersedes
// the previous confirmation.
bool RequestPictureRebuild(uint32_t render_w, uint32_t render_h);
// A64 (Astra's A63 review): every step is checked and the restoration runs
// on a window timer (a message, not a frame), so a game that stops
// presenting while resized is still put back; a failed step ends in `Failed`
// with a plain reason, never in a pending state.
struct PictureRebuild {
    uint32_t state;         // 0 idle, 1 nudging (message posted), 2 nudged (window smaller, restore timer armed), 3 restored, 4 failed
    uint32_t attempts;      // nudges started
    uint32_t failures;      // nudges that ended in `failed`
    uint32_t restore_retries; // restore attempts beyond the first, this nudge
    uint64_t restored_tick; // GetTickCount64 when the window was last put back (0 = never)
    uint64_t confirm_id;    // A66: the size-confirmation request armed at restoration (0 = none)
    char reason[96];        // why the last one failed (empty otherwise)
};
PictureRebuild GetPictureRebuild();

// ---- A48 host seam for game adapters (COLLABORATION.md B-005 Â§2, amended) ----
// Submission scope: the host's ExecuteCommandLists detour calls `begin`
// before forwarding and `end` after the ORIGINAL returned, with the same
// context (exact queue, list array, a monotonic ticket, and how many of the
// lists were not yet covered by the variant registry at submission). A
// receipt may only be published from `end`; the return is submission
// evidence, not GPU completion. Called on the game thread: no waits.
// Single subscriber; Unsubscribe requires quiescence (tests only).
struct ExecuteContext {
    ID3D12CommandQueue* queue;
    unsigned int count;
    ID3D12CommandList* const* lists;
    uint64_t ticket;
    uint32_t lists_uncovered;   // lists whose current implementation is not (yet) fully hooked
    void* user;
};
using ExecuteBeginFn = void (*)(ExecuteContext*);
using ExecuteEndFn = void (*)(ExecuteContext*);
bool SubscribeExecute(ExecuteBeginFn begin, ExecuteEndFn end, void* user);
// A50: closes admission, then waits (bounded, calling control thread) for
// callbacks in flight. false = a callback outlasted the timeout: storage is
// retained, do not destroy adapter state or re-subscribe; call again later.
bool UnsubscribeExecute(uint32_t timeout_ms = 5000);

// Discovery deliveries on the registry's discovery thread (never a render
// thread): the game's device (once), each direct queue the first time it
// submits, and each list that was submitted while its implementation was
// unknown. Objects are AddRef'd when queued and Released after the callback
// returns; the queue is bounded (16) and overflow is counted, never blocked.
// A49: the one-shot device/queue announcements are latched only on a
// successful push and retried (at SubscribeDiscovery and every Execute)
// until delivered, so a late subscription or an overflow loses nothing.
// Every submitted list's identity is in the Execute scope's list array;
// ListSeen is only the retained delivery of lists that were uncovered.
using DeviceSeenFn = void (*)(ID3D12Device*, void*);
using QueueSeenFn = void (*)(ID3D12CommandQueue*, void*);
using ListSeenFn = void (*)(ID3D12CommandList*, void*);
bool SubscribeDiscovery(DeviceSeenFn device, QueueSeenFn queue, ListSeenFn list, void* user);
void UnsubscribeDiscovery();
struct SeamStats {
    uint64_t executes_scoped;     // begin/end pairs delivered
    uint32_t discovery_queued, discovery_delivered, discovery_overflow;
    uint32_t execute_active;      // callbacks in flight right now
    uint32_t unsubscribe_timeouts; // UnsubscribeExecute calls that returned false
    uint32_t stop_incomplete;     // 0/1 the last Stop could not prove quiescence (state retained)
};
SeamStats GetSeamStats();

// A55: periodic status log (one line every ~5 s, written from the discovery
// thread, never a render thread): presentation rate, DLSS/NR/motion state
// and timings, so measurements are recorded without anyone reading the menu.
void SetStatusLogPath(const wchar_t* path);

// Tests / hosts: the current page index (0 Picture .. 3 Details).
int CurrentPageIndex();
// Last DXGI_COLOR_SPACE_TYPE the game set on its swap chain (DXGI_COLOR_SPACE_CUSTOM = never).
int SwapChainColorSpace();
// Exact creation/ResizeBuffers1 binding. Returned queue is AddRef-owned.
ID3D12CommandQueue* AcquirePresentQueue(IDXGISwapChain* chain);
int SwapChainColorSpace(IDXGISwapChain* chain);
// Test seam: render the existing menu into a caller-owned PRESENT-state
// texture using the normal draw path, without DXGI Present pacing. Requires
// an initialized/open overlay; caller owns queue synchronization and texture.
void DrawMenuForTests(ID3D12CommandQueue* queue,ID3D12Resource* frame);

} // namespace na::overlay
