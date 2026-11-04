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
