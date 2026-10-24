// Gate 2 R-306: a real-interposer stand-in that LACKS slInit and
// slEvaluateFeature (and every other wrapped export except slSetConstants,
// which always fails with a nonzero 2.x error). Built as
// sl_interposer_real.dll into its own folder together with
// sl_proxy_missing_test.exe, so the proxy resolves THIS one.
#include <windows.h>
#include <cstdint>

extern "C" __declspec(dllexport) uint64_t __cdecl slSetConstants(const void*, uintptr_t, uintptr_t) { return 5; }
extern "C" __declspec(dllexport) int __cdecl SlStubMinMarker() { return 0x51D; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) { if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(h); return TRUE; }
