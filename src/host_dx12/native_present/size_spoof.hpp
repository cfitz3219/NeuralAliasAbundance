// Render-small/native-present, S1 (Astra). Game-facing geometry only.
// Desktop modes and the real HWND are never resized by these helpers.
#pragma once
#include <windows.h>
#include <dxgi1_2.h>
#include <cstdint>

namespace na::native_present {
struct Size { UINT w = 0, h = 0; };
struct SizeState {
    HWND hwnd = nullptr;
    Size output{}, render{}, requested{};
    uint64_t generation = 0, request = 0;
    bool enabled = false, request_delivered = false;
    bool request_window = false; // the pending request is a REAL window resize (BeginWindowResize), not our ask
};
struct InputGeometry {
    RECT physical_client{}; // SCREEN coordinates, using real user32 calls
    Size render{};         // committed proxy extent, NOT an uncommitted request
    uint64_t generation = 0;
};

// Use around original DXGI creation/resize/descriptor calls and our overlay's
// user32 work, never around a callback into the game. Nestable/thread-local.
struct NativeScope {
    NativeScope();
    ~NativeScope();
    NativeScope(const NativeScope&) = delete;
    NativeScope& operator=(const NativeScope&) = delete;
};
// A standalone test EXE contains both the helper and simulated game. This
// scope lets that fixture exercise the same detours as a different game module.
struct GameScopeForTest {
    GameScopeForTest();
    ~GameScopeForTest();
    GameScopeForTest(const GameScopeForTest&) = delete;
    GameScopeForTest& operator=(const GameScopeForTest&) = delete;
};
bool GameCaller(const void* return_address);
// Main EXE is registered automatically. If the engine's window/render code is
// in a DLL, register an observed engine code address on the boot/watch worker.
// Unknown injected overlays and system DLLs keep physical geometry/input.
bool RegisterGameModule(const void* engine_code_address);

// Install on the boot/watch worker only (not Present, WndProc or DllMain).
// Uses the existing transactional hook owner. Hooks persist until the host's
// quiescent shutdown; disabling/detaching makes them transparent immediately.
bool InstallSizeHooks();
bool Attach(HWND hwnd, Size output, Size render);
void Detach(HWND hwnd);
SizeState GetSizeState();
bool GetInputGeometry(InputGeometry* out);

// Caller-filter BEFORE invoking these explicit transforms from DXGI detours.
// Preserve every non-size field. GetFullscreenDesc has no size fields.
bool RewriteClientRect(HWND hwnd, RECT* rect);
bool RewriteWindowRect(HWND hwnd, RECT* rect);
bool RewriteDesc(DXGI_SWAP_CHAIN_DESC* desc);
bool RewriteDesc1(HWND hwnd, DXGI_SWAP_CHAIN_DESC1* desc);

// Posts one registered message to the WINDOW thread. PrepareSizeMessage turns
// it into a virtual WM_SIZE immediately before forwarding to the GAME WndProc.
// Returns a token, or 0 for failure/no change/pending request. No callback or
// physical SetWindowPos occurs under a helper lock (or at all here).
// force: ask even when `render` equals the committed size (the render-small controller's
// restore/supersede: the game may already hold buffers of the abandoned request's size).
uint64_t RequestRenderSize(Size render, bool force = false);
// The size the game is currently told (requested while a delivered request is pending).
Size AdvertisedSize();
// Already on the window thread for a REAL WM_SIZE: caller computes render
// with the presenter's exact scale/rounding and supplies the new physical
// output. Supersedes an older pending request, without posting another message.
// Call before PrepareSizeMessage, never for our registered synthetic message.
uint64_t BeginWindowResize(HWND hwnd, Size output, Size render);
UINT ResizeMessage();
enum class MessageAction { Forward, Changed, Consume };
MessageAction PrepareSizeMessage(HWND hwnd, UINT& msg, WPARAM& wp,
                                 LPARAM& lp, WINDOWPOS& scratch);
// Called after presenter has actually allocated the new proxies. A failed
// resize rolls advertised geometry back, retains old input mapping, and may be
// retried. Token matching prevents a late completion changing a newer request.
bool CompleteResize(uint64_t token, bool success, Size actual_render);
// Call from the normal Present/status tick. Resolves an ignored WM_SIZE as a
// failed request, without changing route, touching the window or repeating old
// frames. No timer thread is created. now_ms=0 uses GetTickCount64.
bool ExpireResize(uint32_t timeout_ms = 5000, uint64_t now_ms = 0);
// Initial allocation or a natural game resize, with no pending live request.
bool CommitBuffers(HWND hwnd, Size output, Size actual_render);

struct SizeStats {
    uint64_t client_queries = 0, window_queries = 0, desc_queries = 0;
    uint64_t size_messages = 0, position_messages = 0;
    uint64_t requests = 0, completed = 0, failed = 0;
};
SizeStats GetSizeStats();
void FormatSizeStatus(char* out, uint32_t capacity);
// Tick of the pending live request (0 when none); the size probe's after_req window.
uint64_t LastRequestTick();
} // namespace na::native_present
