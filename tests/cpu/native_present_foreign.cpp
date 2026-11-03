// A separate module models a third-party overlay or an explicitly registered
// engine DLL. Post-call work prevents a tail call hiding its caller identity.
#include <windows.h>
volatile LONG g_fixture_queries = 0;
extern "C" __declspec(dllexport) __declspec(noinline) BOOL WINAPI FixtureClientRect(HWND hwnd, RECT* r) {
    const BOOL ok=GetClientRect(hwnd,r);
    InterlockedIncrement(&g_fixture_queries);
    return ok;
}
