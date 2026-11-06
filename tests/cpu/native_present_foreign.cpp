// A separate module models a third-party overlay or an explicitly registered
// engine DLL. Post-call work prevents a tail call hiding its caller identity.
#include <windows.h>
#include <dxgi.h>
volatile LONG g_fixture_queries = 0;
extern "C" __declspec(dllexport) __declspec(noinline) BOOL WINAPI FixtureClientRect(HWND hwnd, RECT* r) {
    const BOOL ok=GetClientRect(hwnd,r);
    InterlockedIncrement(&g_fixture_queries);
    return ok;
}
extern "C" __declspec(dllexport) __declspec(noinline) HRESULT WINAPI FixtureResizeTarget(IDXGISwapChain* sc, const DXGI_MODE_DESC* mode) {
    const HRESULT result=sc->ResizeTarget(mode);
    InterlockedIncrement(&g_fixture_queries);
    return result;
}
// A borderless/fullscreen engine: sizes from the monitor of its window, and fits its window to it.
extern "C" __declspec(dllexport) __declspec(noinline) BOOL WINAPI FixtureMonitorRect(HWND hwnd, RECT* r) {
    MONITORINFO mi{}; mi.cbSize = sizeof mi;
    const BOOL ok = GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    if (ok && r) *r = mi.rcMonitor;
    InterlockedIncrement(&g_fixture_queries);
    return ok;
}
extern "C" __declspec(dllexport) __declspec(noinline) BOOL WINAPI FixtureSetWindowPos(HWND hwnd, int x, int y, int cx, int cy) {
    const BOOL ok = SetWindowPos(hwnd, nullptr, x, y, cx, cy, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    InterlockedIncrement(&g_fixture_queries);
    return ok;
}
