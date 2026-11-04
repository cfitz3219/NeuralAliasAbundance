#include "host_dx12/native_present/size_spoof.hpp"
#include "host_dx12/native_present/size_probe.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
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
using RectFn = BOOL(WINAPI*)(HWND, LPRECT);
RectFn g_client = nullptr, g_window = nullptr;
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
    if (!ok) { na::hooking::AbandonInstall(); na::hooking::Shutdown(); return false; }
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
    return true;
}
void Detach(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_state.hwnd == hwnd) { g_state = {}; ++g_next; }
}
SizeState GetSizeState() { std::lock_guard<std::mutex> lock(g_mutex); return g_state; }
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
uint64_t RequestRenderSize(Size render) {
    if (!valid(render) || !ResizeMessage()) return 0;
    HWND hwnd = nullptr; uint64_t token = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_state.enabled || g_state.request || same(render, g_state.render)) return 0;
        hwnd = g_state.hwnd; token = ++g_next;
        g_state.requested = render; g_state.request = token; g_state.request_delivered = false;
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
    if (g_state.request && g_state.request_delivered && same(output,g_request_output) && same(render,g_state.requested)) return g_state.request;
    g_state.requested=render; g_state.request=++g_next; g_state.request_delivered=true;
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
        msg = WM_SIZE; wp = IsZoomed(hwnd) ? SIZE_MAXIMIZED : SIZE_RESTORED;
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
    } else return MessageAction::Forward;
    const auto size = advertised(s);
    lp = MAKELPARAM(size.w, size.h); ++g_size_messages;
    return MessageAction::Changed;
}
bool CompleteResize(uint64_t token, bool success, Size actual) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_state.enabled || !token || g_state.request != token) return false;
    const bool accepted = success && valid(actual) && same(actual, g_state.requested);
    if (accepted) { g_state.render = actual; g_state.output = g_request_output; g_state.generation = token; ++g_completed; }
    else ++g_failed;
    g_state.request = 0; g_state.requested = {}; g_state.request_delivered = false; g_request_tick = 0; g_request_output = {};
    return accepted;
}
bool ExpireResize(uint32_t timeout_ms, uint64_t now_ms) {
    if (!now_ms) now_ms = GetTickCount64();
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_state.request || now_ms < g_request_tick || now_ms-g_request_tick < timeout_ms) return false;
    g_state.request = 0; g_state.requested = {}; g_state.request_delivered = false; g_request_tick = 0; g_request_output = {};
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
        "native_size on=%u output=%ux%u render=%ux%u requested=%ux%u generation=%llu pending=%llu queries{client=%llu window=%llu desc=%llu} messages{size=%llu pos=%llu} resize{asked=%llu ok=%llu failed=%llu}",
        s.enabled ? 1u : 0u, s.output.w,s.output.h,s.render.w,s.render.h,s.requested.w,s.requested.h,
        static_cast<unsigned long long>(s.generation),static_cast<unsigned long long>(s.request),
        static_cast<unsigned long long>(c.client_queries),static_cast<unsigned long long>(c.window_queries),
        static_cast<unsigned long long>(c.desc_queries),static_cast<unsigned long long>(c.size_messages),
        static_cast<unsigned long long>(c.position_messages),static_cast<unsigned long long>(c.requests),
        static_cast<unsigned long long>(c.completed),static_cast<unsigned long long>(c.failed));
}
} // namespace na::native_present
