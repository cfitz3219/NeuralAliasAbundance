// D1: MinHook hooks a function and a COM-style vtable call at the build's
// pointer size (32-bit in win32/). The DX9 hooks rely on both.
#include <windows.h>
#include <MinHook.h>

#include <cstdio>

namespace {

int g_fail = 0;
void Check(bool ok, const char* what)
{
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

// Not inlined, with a body long enough for a 5-byte jump.
__declspec(noinline) int __stdcall Target(int a, int b)
{
    volatile int x = a * 3;
    x += b * 7;
    return x;
}
using PFN_Target = int(__stdcall*)(int, int);
PFN_Target g_orig = nullptr;
int g_calls = 0;
int __stdcall Detour(int a, int b)
{
    ++g_calls;
    return g_orig(a, b) + 1000;
}

// A COM-like object: the DX9 device's methods are __stdcall through a vtable.
struct IThing {
    virtual int __stdcall Get(int v) = 0;
};
struct Thing : IThing {
    __declspec(noinline) int __stdcall Get(int v) override
    {
        volatile int r = v + 1;
        return r;
    }
};
using PFN_Get = int(__stdcall*)(IThing*, int);
PFN_Get g_get_orig = nullptr;
int __stdcall GetDetour(IThing* self, int v) { return g_get_orig(self, v) * 10; }

} // namespace

int main()
{
    std::printf("MinHook test, %d-bit\n", static_cast<int>(sizeof(void*) * 8));
    Check(MH_Initialize() == MH_OK, "MH_Initialize");

    volatile PFN_Target target = &Target;
    Check(target(2, 3) == 27, "plain call before hook");
    Check(MH_CreateHook(reinterpret_cast<LPVOID>(&Target), reinterpret_cast<LPVOID>(&Detour), reinterpret_cast<LPVOID*>(&g_orig)) == MH_OK,
          "create function hook");
    Check(MH_EnableHook(reinterpret_cast<LPVOID>(&Target)) == MH_OK, "enable function hook");
    Check(target(2, 3) == 1027 && g_calls == 1, "hooked call goes through detour and original");
    Check(MH_DisableHook(reinterpret_cast<LPVOID>(&Target)) == MH_OK, "disable function hook");
    Check(target(2, 3) == 27, "plain call after unhook");

    Thing thing;
    IThing* volatile obj = &thing; // volatile: no devirtualising
    void** vtbl = *reinterpret_cast<void***>(obj);
    Check(MH_CreateHook(vtbl[0], reinterpret_cast<LPVOID>(&GetDetour), reinterpret_cast<LPVOID*>(&g_get_orig)) == MH_OK,
          "create vtable method hook");
    Check(MH_EnableHook(vtbl[0]) == MH_OK, "enable vtable method hook");
    Check(obj->Get(4) == 50, "virtual call goes through detour");
    MH_DisableHook(vtbl[0]);
    Check(obj->Get(4) == 5, "virtual call after unhook");

    MH_Uninitialize();
    std::printf("%s: MinHook (%d-bit)\n", g_fail ? "FAIL" : "PASS", static_cast<int>(sizeof(void*) * 8));
    return g_fail ? 1 : 0;
}
