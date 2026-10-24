// Gate 2 R-302: the shared DLL watcher never calls into the Windows loader
// while holding its own mutex (the loader-notification callback holds the
// loader lock and takes that mutex), so registering watches concurrently
// with DLL loads cannot deadlock.
//   1. deterministic: a probe placed before every loader call the watcher
//      makes asserts the watch mutex is NOT held by the calling thread;
//   2. stress: one thread registers watches (each does GetModuleHandleW
//      lookups) while another loads/unloads an unwatched DLL in a loop, so
//      notification callbacks contend for the mutex. A deadlock would hang
//      until the ctest TIMEOUT fails the test.
// argv[1] = a DLL to load/unload (unwatched); argv[2] = a DLL that is watched.
#include "test_support.hpp"
#include "host_dx12/upscalers/module_watch.hpp"

#include <atomic>
#include <thread>

namespace {
std::atomic<int> g_violations{0}, g_probes{0}, g_installs{0};
HANDLE g_installed = nullptr;
void Probe()
{
    g_probes.fetch_add(1);
    if (na::upscalers::WatchLockHeldByThisThread()) g_violations.fetch_add(1);
}
void Install(HMODULE) { g_installs.fetch_add(1); SetEvent(g_installed); }
// R-308: a module that loads AFTER the already-loaded lookup but before the
// watch would have been registered under the old order.
DWORD g_main = 0;
const char* g_late_path = nullptr;
std::atomic<int> g_main_probes{0}, g_late_installs{0};
std::atomic<HMODULE> g_late_module{nullptr}, g_late_seen{nullptr};
HANDLE g_late_installed = nullptr;
void LateProbe()
{
    Probe();
    if (GetCurrentThreadId() != g_main) return;          // ignore the worker's pins
    if (g_main_probes.fetch_add(1) == 1)                   // after the lookup of the FIRST name
        g_late_module.store(LoadLibraryA(g_late_path));    // ... the watched DLL loads
}
void LateInstall(HMODULE m)
{
    g_late_installs.fetch_add(1);
    g_late_seen.store(m);
    SetEvent(g_late_installed);
}
} // namespace

int main(int argc, char** argv)
{
    REQUIRE(argc > 2);
    g_installed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    REQUIRE(g_installed != nullptr);
    static const wchar_t* const kWatched[] = {L"ffx_stub.dll", nullptr};

    // 1. Deterministic rule check, including the "already loaded" path.
    HMODULE watched = LoadLibraryA(argv[2]);
    REQUIRE(watched != nullptr);
    na::upscalers::SetLoaderCallProbeForTests(&Probe);
    REQUIRE(na::upscalers::AddModuleWatch(kWatched, &Install));
    CHECK(WaitForSingleObject(g_installed, 5000) == WAIT_OBJECT_0); // already-loaded module reached the installer
    CHECK(g_probes.load() >= 2);   // the lookup and the worker's pin
    CHECK(g_violations.load() == 0);

    // 2. Contention: watch registration vs. load notifications.
    std::atomic<bool> stop{false};
    std::thread loader([&] {
        for (int i = 0; i < 300 && !stop.load(); ++i) {
            if (HMODULE m = LoadLibraryA(argv[1])) FreeLibrary(m);
        }
    });
    for (int i = 0; i < 300; ++i) na::upscalers::AddModuleWatch(kWatched, &Install);
    stop.store(true);
    loader.join();
    CHECK(g_violations.load() == 0);

    // 3. R-308: the target loads between its own lookup and the end of
    // registration. The watch is already registered, so the load notification
    // queues it; the installer runs exactly once.
    {
        g_main = GetCurrentThreadId();
        g_late_path = argv[1];
        g_late_installed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        REQUIRE(g_late_installed != nullptr);
        REQUIRE(GetModuleHandleA("sl_interposer_real.dll") == nullptr); // the stress loop unloaded it
        static const wchar_t* const kLate[] = {L"sl_interposer_real.dll", L"na_never_loaded.dll", nullptr};
        na::upscalers::SetLoaderCallProbeForTests(&LateProbe);
        REQUIRE(na::upscalers::AddModuleWatch(kLate, &LateInstall));
        REQUIRE(g_late_module.load() != nullptr);
        CHECK(WaitForSingleObject(g_late_installed, 5000) == WAIT_OBJECT_0);
        Sleep(200); // a duplicate would land here
        CHECK(g_late_installs.load() == 1);
        CHECK(g_late_seen.load() == g_late_module.load());
        na::upscalers::SetLoaderCallProbeForTests(&Probe);
    }
    std::printf("module_watch: %d probes, %d installs, 0 lock violations expected (%d)\n", g_probes.load(),
                g_installs.load(), g_violations.load());
    na::upscalers::SetLoaderCallProbeForTests(nullptr);
    na::upscalers::StopModuleWatches();
    CloseHandle(g_installed);
    return test::finish("module_watch_test");
}
