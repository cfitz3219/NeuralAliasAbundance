// Gate 2 R-306: when the real Streamline export a wrapper needs is missing,
// neither a 1.x caller (bool) nor a 2.x caller (sl::Result, 0 = ok) may see
// success for a call that never ran; a real nonzero 2.x error passes through.
// argv[1] = path of the proxy (sl.interposer.dll). The minimal stand-in
// sl_interposer_real.dll sits next to this executable.
#include "test_support.hpp"

int main(int argc, char** argv)
{
    REQUIRE(argc > 1);
    HMODULE proxy = LoadLibraryA(argv[1]);
    REQUIRE(proxy != nullptr);
    // 1.x view of slInit / slEvaluateFeature: bool -> must read false.
    auto init1 = reinterpret_cast<bool(__cdecl*)(const void*, int)>(GetProcAddress(proxy, "slInit"));
    auto eval1 = reinterpret_cast<bool(__cdecl*)(void*, uint32_t, uint32_t, uint32_t)>(GetProcAddress(proxy, "slEvaluateFeature"));
    // 2.x view: sl::Result -> must be nonzero (an error).
    auto init2 = reinterpret_cast<uint32_t(__cdecl*)(const void*, uint64_t)>(GetProcAddress(proxy, "slInit"));
    auto eval2 = reinterpret_cast<uint32_t(__cdecl*)(uint32_t, const void*, const void**, uint32_t, void*)>(GetProcAddress(proxy, "slEvaluateFeature"));
    auto setc2 = reinterpret_cast<uint32_t(__cdecl*)(const void*, const void*, const void*)>(GetProcAddress(proxy, "slSetConstants"));
    auto getfs1 = reinterpret_cast<bool(__cdecl*)(uint32_t, const void*, void*)>(GetProcAddress(proxy, "slGetFeatureSettings"));
    REQUIRE(init1 && eval1 && init2 && eval2 && setc2 && getfs1);
    int prefs = 0;
    CHECK(init1(&prefs, 1) == false);
    HMODULE real = GetModuleHandleW(L"sl_interposer_real.dll");
    REQUIRE(real != nullptr && GetProcAddress(real, "SlStubMinMarker") != nullptr); // the minimal stand-in was loaded
    CHECK(init2(&prefs, 0x0002000E0010C0DEull) != 0);
    CHECK(eval1(nullptr, 1, 2, 0) == false);
    CHECK(eval2(1, nullptr, nullptr, 0, nullptr) != 0);
    uint32_t settings = 0;
    CHECK(getfs1(0, &prefs, &settings) == false && settings == 0);
    // A real 2.x error from the runtime is passed back unchanged.
    int consts[8] = {};
    CHECK(setc2(consts, nullptr, nullptr) == 5);
    FreeLibrary(proxy);
    return test::finish("sl_proxy_missing_test");
}
