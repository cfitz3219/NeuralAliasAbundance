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
    std::printf("module_watch: %d probes, %d installs, 0 lock violations expected (%d)\n", g_probes.load(),
                g_installs.load(), g_violations.load());
    na::upscalers::SetLoaderCallProbeForTests(nullptr);
    na::upscalers::StopModuleWatches();
    CloseHandle(g_installed);
    return test::finish("module_watch_test");
}
