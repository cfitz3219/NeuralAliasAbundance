#include "host_dx12/native_present/size_spoof.hpp"
#include "host_dx12/native_present/size_probe.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
#include "host_dx12/diag/event_log.hpp"
#include <cmath>
#include <cwchar>
#include <intrin.h>
#include <atomic>
#include <array>
#include <cstdio>
#include <limits>
#include <mutex>

namespace na::native_present {
namespace {
std::mutex g_mutex, g_install_mutex;
SizeState g_state;
uint64_t g_next = 0;
uint64_t g_request_tick = 0;
Size g_request_output{};
thread_local unsigned t_native = 0, t_game = 0;
uintptr_t g_module_begin = 0, g_module_end = 0;
struct ModuleRange { uintptr_t begin = 0, end = 0; };
std::array<ModuleRange,16> g_game_modules{};
std::atomic<unsigned> g_game_module_count{0};
std::mutex g_modules_mutex;
bool g_installed = false;
bool g_mode_valid = false, g_windowed = true;
LONG_PTR g_window_style = 0;
HMONITOR g_monitor = nullptr;
using RectFn = BOOL(WINAPI*)(HWND, LPRECT);
RectFn g_client = nullptr, g_window = nullptr;
using SetWindowPosFn = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
using MoveWindowFn = BOOL(WINAPI*)(HWND, int, int, int, int, BOOL);
SetWindowPosFn g_setpos = nullptr;
MoveWindowFn g_move = nullptr;
std::atomic<uint64_t> g_s_monitor{0}, g_s_modes{0}, g_s_metrics{0}, g_s_outputs{0}, g_s_output_modes{0};
std::atomic<uint64_t> g_s_moves{0}, g_s_clamped{0}, g_s_display_change{0}, g_s_resize_target{0};
std::atomic<uint64_t> g_client_queries{0}, g_window_queries{0}, g_desc_queries{0};
std::atomic<uint64_t> g_size_messages{0}, g_position_messages{0};
std::atomic<uint64_t> g_requests{0}, g_completed{0}, g_failed{0};
bool valid(Size s) { return s.w && s.h && s.w <= 65535 && s.h <= 65535; }
bool same(Size a, Size b) { return a.w == b.w && a.h == b.h; }
Size advertised(const SizeState& s) { return s.request && s.request_delivered ? s.requested : s.render; }
bool for_window(const SizeState& s, HWND hwnd) { return s.enabled && hwnd && s.hwnd == hwnd; }
BOOL real_client(HWND hwnd, RECT* r) { return g_client ? g_client(hwnd, r) : GetClientRect(hwnd, r); }
LONG bounded(int64_t v) {
    return static_cast<LONG>((v < (std::numeric_limits<LONG>::min)()) ? (std::numeric_limits<LONG>::min)() :
        (v > (std::numeric_limits<LONG>::max)()) ? (std::numeric_limits<LONG>::max)() : v);
}
BOOL WINAPI client_detour(HWND hwnd, LPRECT r) {
    const BOOL ok = g_client(hwnd, r);
    if (ok && GameCaller(_ReturnAddress())) {
        RewriteClientRect(hwnd, r);
        if (r) NoteSizeQuery(SizeApi::ClientRect, _ReturnAddress(), static_cast<uint32_t>(r->right - r->left), static_cast<uint32_t>(r->bottom - r->top));
    }
    return ok;
}
BOOL WINAPI setpos_detour(HWND hwnd, HWND after, int x, int y, int cx, int cy, UINT flags) {
    if (GameCaller(_ReturnAddress())) RewriteGameWindowPos(hwnd, &x, &y, &cx, &cy, flags);
    return g_setpos(hwnd, after, x, y, cx, cy, flags);
}
BOOL WINAPI move_detour(HWND hwnd, int x, int y, int cx, int cy, BOOL repaint) {
    if (GameCaller(_ReturnAddress())) RewriteGameWindowPos(hwnd, &x, &y, &cx, &cy, 0);
    return g_move(hwnd, x, y, cx, cy, repaint);
}
LONG scaled(LONG v, double s) { return bounded(std::llround(double(v) * s)); }
LONG to_virtual(LONG p, LONG o, double s) { return bounded(int64_t(o) + std::llround((double(p) - o) * s)); }
LONG to_physical(LONG v, LONG o, double s) { return bounded(int64_t(o) + std::llround((double(v) - o) / s)); }
bool same_device(const ScreenMap& m, const wchar_t* device) {
    return device ? std::wcscmp(device, m.device) == 0 : m.primary; // null = the primary display
}
BOOL WINAPI window_detour(HWND hwnd, LPRECT r) {
    const BOOL ok = g_window(hwnd, r);
    if (ok && GameCaller(_ReturnAddress())) {
        RewriteWindowRect(hwnd, r);
        if (r) NoteSizeQuery(SizeApi::WindowRect, _ReturnAddress(), static_cast<uint32_t>(r->right - r->left), static_cast<uint32_t>(r->bottom - r->top));
    }
    return ok;
}
}
NativeScope::NativeScope() { ++t_native; }
NativeScope::~NativeScope() { --t_native; }
GameScopeForTest::GameScopeForTest() { ++t_game; }
GameScopeForTest::~GameScopeForTest() { --t_game; }
bool GameCaller(const void* caller) {
    if (t_native) return false;
    if (t_game) return true;
    const auto p = reinterpret_cast<uintptr_t>(caller);
    if (!p || (p >= g_module_begin && p < g_module_end)) return false;
    const unsigned n = g_game_module_count.load(std::memory_order_acquire);
    for (unsigned i=0;i<n;++i) if(p>=g_game_modules[i].begin && p<g_game_modules[i].end) return true;
    return false;
}
uintptr_t GameModuleBase(const void* caller) {
    const auto p = reinterpret_cast<uintptr_t>(caller);
    const unsigned n = g_game_module_count.load(std::memory_order_acquire);
    for (unsigned i=0;i<n;++i) if(p>=g_game_modules[i].begin && p<g_game_modules[i].end) return g_game_modules[i].begin;
    return 0;
}
bool RegisterGameModule(const void* address) {
    if (!address) return false;
    HMODULE module=nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(address),&module)) return false;
    const auto base=reinterpret_cast<const unsigned char*>(module);
    const auto dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto nt=reinterpret_cast<const IMAGE_NT_HEADERS*>(base+dos->e_lfanew);
    const uintptr_t begin=reinterpret_cast<uintptr_t>(base), end=begin+nt->OptionalHeader.SizeOfImage;
    std::lock_guard<std::mutex> lock(g_modules_mutex);
    const unsigned n=g_game_module_count.load(std::memory_order_relaxed);
    for(unsigned i=0;i<n;++i) if(g_game_modules[i].begin==begin) return true;
    if(n==g_game_modules.size()) return false;
    g_game_modules[n]={begin,end};
    g_game_module_count.store(n+1,std::memory_order_release);
    return true;
}
bool InstallSizeHooks() {
    std::lock_guard<std::mutex> install(g_install_mutex);
    if (g_installed) return true;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&InstallSizeHooks), &module)) return false;
    const auto base = reinterpret_cast<const unsigned char*>(module);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    g_module_begin = reinterpret_cast<uintptr_t>(base);
    g_module_end = g_module_begin + nt->OptionalHeader.SizeOfImage;
    if(!RegisterGameModule(GetModuleHandleW(nullptr))) return false;
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (!u || !na::hooking::Initialize()) return false;
    if (!na::hooking::BeginInstallWait("native-present size")) { na::hooking::Shutdown(); return false; }
    const auto c = reinterpret_cast<void*>(GetProcAddress(u, "GetClientRect"));
    const auto w = reinterpret_cast<void*>(GetProcAddress(u, "GetWindowRect"));
    const bool ok = c && w && na::hooking::PrepareHook(c, reinterpret_cast<void*>(&client_detour), reinterpret_cast<void**>(&g_client)) &&
        na::hooking::PrepareHook(w, reinterpret_cast<void*>(&window_detour), reinterpret_cast<void**>(&g_window));
    const auto sp = reinterpret_cast<void*>(GetProcAddress(u, "SetWindowPos"));
    const auto mv = reinterpret_cast<void*>(GetProcAddress(u, "MoveWindow"));
    const bool moves = ok && sp && mv && na::hooking::PrepareHook(sp, reinterpret_cast<void*>(&setpos_detour), reinterpret_cast<void**>(&g_setpos)) &&
        na::hooking::PrepareHook(mv, reinterpret_cast<void*>(&move_detour), reinterpret_cast<void**>(&g_move));
    if (!ok || !moves) { na::hooking::AbandonInstall(); na::hooking::Shutdown(); return false; }
    if (!na::hooking::CommitInstall()) { na::hooking::Shutdown(); return false; }
    g_installed = true;
    return true;
}
bool Attach(HWND hwnd, Size output, Size render) {
    if (!hwnd || !IsWindow(hwnd) || !valid(output) || !valid(render)) return false;
    DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_state.enabled && g_state.hwnd == hwnd) return same(output, g_state.output) && same(render, g_state.render);
    g_state = {hwnd, output, render, {}, ++g_next, 0, true};
    g_mode_valid = false;
    return true;
}
void Detach(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_state.hwnd == hwnd) { g_state = {}; g_mode_valid = false; ++g_next; }
}
SizeState GetSizeState() { std::lock_guard<std::mutex> lock(g_mutex); return g_state; }
void ObserveWindowMode(HWND hwnd, bool windowed) {
    if (!hwnd || !IsWindow(hwnd)) return;
    const NativeScope native;
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE) & (WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_MAXIMIZE);
    const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!for_window(g_state, hwnd)) return;
    if (!g_mode_valid || g_windowed != windowed || g_window_style != style || g_monitor != monitor) {
        g_windowed = windowed; g_window_style = style; g_monitor = monitor; g_mode_valid = true;
        g_state.mode_generation = ++g_next;
    }
}
bool GetInputGeometry(InputGeometry* out) {
    if (!out) return false;
    const auto s = GetSizeState();
    if (!s.enabled) return false;
    NativeScope native;
    RECT r{}; POINT origin{};
    if (!real_client(s.hwnd, &r) || r.right <= r.left || r.bottom <= r.top || !ClientToScreen(s.hwnd, &origin)) return false;
    r.left += origin.x; r.right += origin.x; r.top += origin.y; r.bottom += origin.y;
    *out = {r, s.render, s.generation};
    return true;
}
bool RewriteClientRect(HWND hwnd, RECT* r) {
    const auto s = GetSizeState();
    if (!r || !for_window(s, hwnd)) return false;
    const auto size = advertised(s);
    *r = {0, 0, static_cast<LONG>(size.w), static_cast<LONG>(size.h)};
    ++g_client_queries; return true;
}
bool RewriteWindowRect(HWND hwnd, RECT* r) {
    const auto s = GetSizeState();
    if (!r || !for_window(s, hwnd)) return false;
    RECT client{};
    if (!real_client(hwnd, &client)) return false;
    const auto size = advertised(s);
    r->right = bounded(int64_t(r->right) - (client.right - client.left) + size.w);
    r->bottom = bounded(int64_t(r->bottom) - (client.bottom - client.top) + size.h);
    ++g_window_queries; return true;
}
bool RewriteDesc(DXGI_SWAP_CHAIN_DESC* d) {
    if (!d) return false;
    const auto s = GetSizeState();
    if (!for_window(s, d->OutputWindow)) return false;
    const auto size = advertised(s);
    d->BufferDesc.Width = size.w; d->BufferDesc.Height = size.h;
    ++g_desc_queries; return true;
}
bool RewriteDesc1(HWND hwnd, DXGI_SWAP_CHAIN_DESC1* d) {
    if (!d) return false;
    const auto s = GetSizeState();
    if (!for_window(s, hwnd)) return false;
    const auto size = advertised(s);
    d->Width = size.w; d->Height = size.h;
    ++g_desc_queries; return true;
}
UINT ResizeMessage() {
    static const UINT id = RegisterWindowMessageW(L"NeuralAlias.RenderSmall.Resize.v1");
    return id;
}
bool RewriteResizeTarget(HWND hwnd, DXGI_MODE_DESC* mode) {
    if (!mode) return false;
    const auto s = GetSizeState();
    if (!for_window(s, hwnd)) return false;
    // A delivered render-only request, or (virtual screen) any mode the size of the screen the
    // game is told about: the real target stays the physical output in every window mode.
    const Size a = advertised(s);
    const bool ours = s.request && s.request_delivered && !s.request_window &&
        mode->Width == s.requested.w && mode->Height == s.requested.h;
    // Committed render size only: during a real window resize the physical output is changing.
    const bool virtual_screen = !s.request && !same(a, s.output) && mode->Width == a.w && mode->Height == a.h;
    if (!ours && !virtual_screen) return false;
    mode->Width = s.output.w; mode->Height = s.output.h;
    ++g_s_resize_target;
    return true;
}
uint64_t RequestRenderSize(Size render, bool force) {
    if (!valid(render) || !ResizeMessage()) return 0;
    HWND hwnd = nullptr; uint64_t token = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_state.enabled || g_state.request || (!force && same(render, g_state.render))) return 0;
        hwnd = g_state.hwnd; token = ++g_next;
        g_state.requested = render; g_state.request = token; g_state.request_delivered = false; g_state.request_window = false;
        g_request_tick = GetTickCount64();
        g_request_output = g_state.output;
    }
    ++g_requests;
    if (PostMessageW(hwnd, ResizeMessage(), static_cast<WPARAM>(token), 0)) return token;
    CompleteResize(token, false, {}); return 0;
}
uint64_t BeginWindowResize(HWND hwnd, Size output, Size render) {
    if (!valid(output) || !valid(render)) return 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!for_window(g_state,hwnd)) return 0;
    if (!g_state.request && same(output,g_state.output) && same(render,g_state.render)) return 0;
    // ResizeTarget/display transitions can notify WM_SIZE without changing
    // the physical extent. Its scale is still the old committed scale; it
    // must not replace our pending logical target with that old render size.
    if (g_state.request && !g_state.request_window && same(output,g_request_output)) return g_state.request;
    if (g_state.request && g_state.request_delivered && same(output,g_request_output) && same(render,g_state.requested)) return g_state.request;
    g_state.requested=render; g_state.request=++g_next; g_state.request_delivered=true; g_state.request_window=true;
    g_request_tick=GetTickCount64(); g_request_output=output; ++g_requests;
    return g_state.request;
}
MessageAction PrepareSizeMessage(HWND hwnd, UINT& msg, WPARAM& wp, LPARAM& lp, WINDOWPOS& scratch) {
    auto s = GetSizeState();
    if (!for_window(s, hwnd)) return MessageAction::Forward;
    if (msg == ResizeMessage()) {
        if (!s.request || static_cast<uint64_t>(wp) != s.request) return MessageAction::Consume;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_state.request != s.request || g_state.hwnd != hwnd) return MessageAction::Consume;
            g_state.request_delivered = true; s.request_delivered = true;
        }
        // This changes the logical render canvas, not the OS window state.
        // SIZE_MAXIMIZED asks some engines to restore their monitor dimensions
        // and ignore our smaller client extent. Real WM_SIZE keeps its wParam.
        msg = WM_SIZE; wp = SIZE_RESTORED;
    } else if (msg == WM_SIZE) {
        if (wp == SIZE_MINIMIZED || !LOWORD(lp) || !HIWORD(lp)) return MessageAction::Forward;
    } else if (msg == WM_WINDOWPOSCHANGED) {
        if (!lp) return MessageAction::Forward;
        scratch = *reinterpret_cast<const WINDOWPOS*>(lp);
        if (scratch.flags & SWP_NOSIZE) return MessageAction::Forward;
        RECT r{};
        if (!real_client(hwnd, &r)) return MessageAction::Forward;
        const auto size = advertised(s);
        scratch.cx = bounded(int64_t(scratch.cx) - (r.right - r.left) + size.w);
        scratch.cy = bounded(int64_t(scratch.cy) - (r.bottom - r.top) + size.h);
        lp = reinterpret_cast<LPARAM>(&scratch); ++g_position_messages;
        return MessageAction::Changed;
    } else if (msg == WM_DISPLAYCHANGE) {
        // lParam = primary display resolution: the virtual screen's, like every other screen size.
        ScreenMap m;
        if (!GetScreenMap(&m) || !m.primary) return MessageAction::Forward;
        lp = MAKELPARAM(static_cast<WORD>(scaled(LOWORD(lp), m.sx)), static_cast<WORD>(scaled(HIWORD(lp), m.sy)));
        ++g_s_display_change;
        return MessageAction::Changed;
    } else return MessageAction::Forward;
    const auto size = advertised(s);
    lp = MAKELPARAM(size.w, size.h); ++g_size_messages;
    return MessageAction::Changed;
}
Size AdvertisedSize() { const auto s = GetSizeState(); return s.enabled ? advertised(s) : Size{}; }
bool CompleteResize(uint64_t token, bool success, Size actual) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_state.enabled || !token || g_state.request != token) return false;
    const bool accepted = success && valid(actual) && same(actual, g_state.requested);
    if (accepted) { g_state.render = actual; g_state.output = g_request_output; g_state.generation = token; ++g_completed; }
    else ++g_failed;
    g_state.request = 0; g_state.requested = {}; g_state.request_delivered = false; g_state.request_window = false; g_request_tick = 0; g_request_output = {};
    return accepted;
}
bool ExpireResize(uint32_t timeout_ms, uint64_t now_ms) {
    if (!now_ms) now_ms = GetTickCount64();
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_state.request || now_ms < g_request_tick || now_ms-g_request_tick < timeout_ms) return false;
    g_state.request = 0; g_state.requested = {}; g_state.request_delivered = false; g_state.request_window = false; g_request_tick = 0; g_request_output = {};
    ++g_failed; return true;
}
bool CommitBuffers(HWND hwnd, Size output, Size actual) {
    if (!valid(output) || !valid(actual)) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!for_window(g_state, hwnd) || g_state.request) return false;
    if (!same(g_state.render, actual) || !same(g_state.output, output)) {
        g_state.render = actual; g_state.output = output; g_state.generation = ++g_next;
    }
    return true;
}
SizeStats GetSizeStats() {
    return {g_client_queries.load(), g_window_queries.load(), g_desc_queries.load(),
        g_size_messages.load(), g_position_messages.load(), g_requests.load(), g_completed.load(), g_failed.load()};
}
uint64_t LastRequestTick() { std::lock_guard<std::mutex> lock(g_mutex); return g_state.request ? g_request_tick : 0; }
void FormatSizeStatus(char* out, uint32_t capacity) {
    if (!out || !capacity) return;
    const auto s = GetSizeState(); const auto c = GetSizeStats();
    std::snprintf(out, capacity,
        "native_size on=%u output=%ux%u render=%ux%u requested=%ux%u generation=%llu pending=%llu queries{client=%llu window=%llu desc=%llu} messages{size=%llu pos=%llu} resize{asked=%llu ok=%llu failed=%llu} mode_generation=%llu",
        s.enabled ? 1u : 0u, s.output.w,s.output.h,s.render.w,s.render.h,s.requested.w,s.requested.h,
        static_cast<unsigned long long>(s.generation),static_cast<unsigned long long>(s.request),
        static_cast<unsigned long long>(c.client_queries),static_cast<unsigned long long>(c.window_queries),
        static_cast<unsigned long long>(c.desc_queries),static_cast<unsigned long long>(c.size_messages),
        static_cast<unsigned long long>(c.position_messages),static_cast<unsigned long long>(c.requests),
        static_cast<unsigned long long>(c.completed),static_cast<unsigned long long>(c.failed),
        static_cast<unsigned long long>(s.mode_generation));
}
bool GetScreenMap(ScreenMap* out) {
    if (!out) return false;
    const auto s = GetSizeState();
    if (!s.enabled || !s.hwnd) return false;
    const Size a = advertised(s);
    if (!valid(a) || !valid(s.output) || same(a, s.output)) return false;
    ScreenMap m;
    m.hwnd = s.hwnd; m.advertised = a; m.output = s.output;
    m.sx = double(a.w) / s.output.w; m.sy = double(a.h) / s.output.h;
    {
        const NativeScope native;
        if (!ClientToScreen(s.hwnd, &m.origin)) return false;
        m.monitor = MonitorFromWindow(s.hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFOEXW mi{}; mi.cbSize = sizeof mi;
        if (!m.monitor || !GetMonitorInfoW(m.monitor, &mi)) return false;
        m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        m.monitor_rect = mi.rcMonitor; m.work_rect = mi.rcWork;
        wcsncpy_s(m.device, _countof(m.device), mi.szDevice, _TRUNCATE);
    }
    // One event per change of the virtual screen (scale, monitor or origin).
    static na::eventlog::Edge edge;
    const int64_t key = (int64_t(a.w) << 48) ^ (int64_t(a.h) << 32) ^ (int64_t(s.output.w) << 16) ^ int64_t(s.output.h) ^
        (int64_t(reinterpret_cast<uintptr_t>(m.monitor)) << 7) ^ (int64_t(m.origin.x) << 20) ^ (int64_t(m.origin.y) << 40);
    if (edge.Changed(key)) {
        const RECT v = MapScreenRect(m, m.monitor_rect);
        na::eventlog::Emit("virtual_screen", "game=%ux%u physical=%ux%u scale=%.4fx%.4f monitor=%ldx%ld->%ldx%ld origin=%ld,%ld primary=%d device=%ls",
            a.w, a.h, s.output.w, s.output.h, m.sx, m.sy, m.monitor_rect.right - m.monitor_rect.left, m.monitor_rect.bottom - m.monitor_rect.top,
            v.right - v.left, v.bottom - v.top, m.origin.x, m.origin.y, m.primary ? 1 : 0, m.device);
    }
    *out = m;
    return true;
}
RECT MapScreenRect(const ScreenMap& m, RECT r) {
    return {to_virtual(r.left, m.origin.x, m.sx), to_virtual(r.top, m.origin.y, m.sy),
            to_virtual(r.right, m.origin.x, m.sx), to_virtual(r.bottom, m.origin.y, m.sy)};
}
bool RewriteMonitorInfo(HMONITOR monitor, MONITORINFO* info) {
    ScreenMap m;
    if (!info || !monitor || !GetScreenMap(&m) || monitor != m.monitor) return false;
    info->rcMonitor = MapScreenRect(m, info->rcMonitor);
    info->rcWork = MapScreenRect(m, info->rcWork);
    ++g_s_monitor; return true;
}
bool RewriteDisplayModeW(const wchar_t* device, DWORD* w, DWORD* h) {
    ScreenMap m;
    if (!w || !h || !GetScreenMap(&m) || !same_device(m, device)) return false;
    *w = static_cast<DWORD>(scaled(static_cast<LONG>(*w), m.sx)); *h = static_cast<DWORD>(scaled(static_cast<LONG>(*h), m.sy));
    ++g_s_modes; return true;
}
bool RewriteDisplayModeA(const char* device, DWORD* w, DWORD* h) {
    if (!device) return RewriteDisplayModeW(nullptr, w, h);
    wchar_t wide[64] = {};
    if (!MultiByteToWideChar(CP_ACP, 0, device, -1, wide, 63)) return false;
    return RewriteDisplayModeW(wide, w, h);
}
bool RewriteScreenMetric(int index, int* value) {
    ScreenMap m;
    if (!value || !GetScreenMap(&m)) return false;
    const bool x = index == SM_CXSCREEN || index == SM_CXFULLSCREEN || index == SM_CXMAXIMIZED || index == SM_CXVIRTUALSCREEN;
    const bool y = index == SM_CYSCREEN || index == SM_CYFULLSCREEN || index == SM_CYMAXIMIZED || index == SM_CYVIRTUALSCREEN;
    if (!x && !y) return false;
    // Primary-display metrics; the virtual-desktop size only when this is the only display.
    if (!m.primary) return false;
    if (index == SM_CXVIRTUALSCREEN || index == SM_CYVIRTUALSCREEN) {
        const NativeScope native;
        if (GetSystemMetrics(SM_CMONITORS) != 1) return false;
    }
    *value = scaled(*value, x ? m.sx : m.sy);
    ++g_s_metrics; return true;
}
bool RewriteOutputRect(HMONITOR monitor, RECT* desktop) {
    ScreenMap m;
    if (!desktop || !monitor || !GetScreenMap(&m) || monitor != m.monitor) return false;
    *desktop = MapScreenRect(m, *desktop);
    ++g_s_outputs; return true;
}
bool RewriteOutputMode(HMONITOR monitor, UINT* w, UINT* h) {
    ScreenMap m;
    if (!w || !h || !monitor || !GetScreenMap(&m) || monitor != m.monitor) return false;
    *w = static_cast<UINT>(scaled(static_cast<LONG>(*w), m.sx)); *h = static_cast<UINT>(scaled(static_cast<LONG>(*h), m.sy));
    ++g_s_output_modes; return true;
}
bool RewriteGameWindowPos(HWND hwnd, int* x, int* y, int* cx, int* cy, UINT flags) {
    ScreenMap m;
    if (!hwnd || !x || !y || !cx || !cy || !GetScreenMap(&m) || hwnd != m.hwnd) return false;
    RECT wr{}, cr{};
    {
        const NativeScope native;
        const BOOL got_window = g_window ? g_window(hwnd, &wr) : GetWindowRect(hwnd, &wr);
        if (!got_window || !real_client(hwnd, &cr)) return false;
    }
    const LONG frame_w = (wr.right - wr.left) - (cr.right - cr.left), frame_h = (wr.bottom - wr.top) - (cr.bottom - cr.top);
    const LONG off_x = m.origin.x - wr.left, off_y = m.origin.y - wr.top;
    const int was_x = *x, was_y = *y, was_cx = *cx, was_cy = *cy;
    bool clamped = false;
    if (!(flags & SWP_NOSIZE)) {
        // The window frame is real in the game's view too; only the client is scaled.
        LONG w = bounded(std::llround(double(*cx - frame_w) / m.sx)), h = bounded(std::llround(double(*cy - frame_h) / m.sy));
        const LONG mw = m.monitor_rect.right - m.monitor_rect.left, mh = m.monitor_rect.bottom - m.monitor_rect.top;
        if (w > mw) { w = mw; clamped = true; }
        if (h > mh) { h = mh; clamped = true; }
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        *cx = frame_w + w; *cy = frame_h + h;
    }
    if (!(flags & SWP_NOMOVE)) {
        *x = to_physical(*x + off_x, m.origin.x, m.sx) - off_x;
        *y = to_physical(*y + off_y, m.origin.y, m.sy) - off_y;
    }
    ++g_s_moves; if (clamped) ++g_s_clamped;
    na::eventlog::Emit("virtual_screen", "window_pos game=%d,%d %dx%d physical=%d,%d %dx%d flags=0x%x clamped=%d",
        was_x, was_y, was_cx, was_cy, *x, *y, *cx, *cy, flags, clamped ? 1 : 0);
    return true;
}
ScreenStats GetScreenStats() {
    return {g_s_monitor.load(), g_s_modes.load(), g_s_metrics.load(), g_s_outputs.load(), g_s_output_modes.load(),
            g_s_moves.load(), g_s_clamped.load(), g_s_display_change.load(), g_s_resize_target.load()};
}
void FormatScreenStatus(char* out, uint32_t capacity) {
    if (!out || !capacity) return;
    const auto c = GetScreenStats();
    ScreenMap m; const bool on = GetScreenMap(&m);
    const RECT v = on ? MapScreenRect(m, m.monitor_rect) : RECT{};
    std::snprintf(out, capacity,
        "virtual_screen on=%u scale=%.4fx%.4f monitor=%ldx%ld->%ldx%ld primary=%u rewrites{monitor=%llu display_modes=%llu metrics=%llu outputs=%llu output_modes=%llu window_moves=%llu clamped=%llu display_change=%llu resize_target=%llu}",
        on ? 1u : 0u, on ? m.sx : 1.0, on ? m.sy : 1.0,
        on ? m.monitor_rect.right - m.monitor_rect.left : 0L, on ? m.monitor_rect.bottom - m.monitor_rect.top : 0L,
        on ? v.right - v.left : 0L, on ? v.bottom - v.top : 0L, on && m.primary ? 1u : 0u,
        static_cast<unsigned long long>(c.monitor), static_cast<unsigned long long>(c.display_modes),
        static_cast<unsigned long long>(c.metrics), static_cast<unsigned long long>(c.outputs),
        static_cast<unsigned long long>(c.output_modes), static_cast<unsigned long long>(c.window_moves),
        static_cast<unsigned long long>(c.clamped), static_cast<unsigned long long>(c.display_change),
        static_cast<unsigned long long>(c.resize_target));
}
} // namespace na::native_present
