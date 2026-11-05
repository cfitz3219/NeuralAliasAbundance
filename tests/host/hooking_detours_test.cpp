// Opt-in Detours engine (hooking/vtable_hook.hpp, [hooking] engine=detours): the same
// contract as MinHook for direct hooks, the install transaction and Unhook, in a process
// whose hooking session uses Detours from the start.
#include "test_support.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"

#include <cstdio>

namespace {
__declspec(noinline) int Twice(int v) { volatile int x = v; return x * 2 + 0; }
__declspec(noinline) int Thrice(int v) { volatile int x = v; return x * 3 + 0; }
using Fn = int (*)(int);
Fn g_twice = nullptr, g_thrice = nullptr;
int g_calls = 0;
int DetourTwice(int v) { ++g_calls; return g_twice(v) + 1; }
int DetourThrice(int v) { ++g_calls; return g_thrice(v) + 100; }
}

int main()
{
    namespace h = na::hooking;
    CHECK(h::SetEngine(h::Engine::Detours));
    REQUIRE(h::Initialize());
    CHECK(h::CurrentEngine() == h::Engine::Detours);
    CHECK(!h::SetEngine(h::Engine::MinHook)); // fixed once the session started
    std::printf("engine: %s\n", h::EngineName());
    Fn volatile twice = &Twice, thrice = &Thrice;

    // Direct hook: the original is published before the jump lands and stays callable.
    REQUIRE(h::HookFunction(reinterpret_cast<void*>(&Twice), reinterpret_cast<void*>(&DetourTwice), reinterpret_cast<void**>(&g_twice)));
    CHECK(g_twice && g_twice != &Twice);
    CHECK(twice(5) == 11);
    CHECK(g_calls == 1);
    CHECK(h::OriginalFor(reinterpret_cast<void*>(&Twice)) == reinterpret_cast<void*>(g_twice));

    // Transaction: prepared hooks publish a callable original (the target itself, nothing
    // patched yet), and the commit attaches.
    REQUIRE(h::BeginInstall());
    REQUIRE(h::PrepareHook(reinterpret_cast<void*>(&Thrice), reinterpret_cast<void*>(&DetourThrice), reinterpret_cast<void**>(&g_thrice)));
    CHECK(g_thrice && g_thrice(2) == 6);
    CHECK(thrice(2) == 6); // not yet hooked
    REQUIRE(h::CommitInstall());
    CHECK(thrice(2) == 106);
    CHECK(g_calls == 2);

    // Unhook: the target runs as before and the published original points back at it.
    CHECK(h::Unhook(reinterpret_cast<void*>(&Twice)));
    CHECK(twice(5) == 10);
    CHECK(g_twice == &Twice);
    h::Shutdown();
    CHECK(thrice(2) == 6); // Shutdown detached the rest
    CHECK(h::InstalledCount() == 0);
    return test::finish("hooking_detours_test");
}
