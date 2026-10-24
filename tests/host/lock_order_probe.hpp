#pragma once
#include "test_support.hpp"
#include "fake_ngx_runtime.hpp"
#include "host_dx12/dlss/dlss_backend.hpp"
#include <thread>
namespace lock_probe {
inline HANDLE entered = nullptr, release_reached = nullptr;
inline void (*read_stats)() = nullptr;
inline void Wait(HANDLE h) {
    if (WaitForSingleObject(h, 10000) != WAIT_OBJECT_0) {
        std::printf("FATAL lock-order synchronization timed out\n");
        std::fflush(stdout); std::exit(1);
    }
}
template<class Eval, class Destroy>
void Run(Eval eval, Destroy destroy, void (*stats)()) {
    entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    release_reached = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!entered || !release_reached) std::exit(1);
    read_stats = stats;
    na::dlss::SetReleaseViewHookForTests([] { SetEvent(release_reached); });
    fake::eval_hook = [] {
        SetEvent(entered);
        Wait(release_reached); // destroy has reached the opposing backend lock
        read_stats();         // must not wait on an interceptor lock held there
    };
    std::thread a(eval);
    std::thread b([&] { Wait(entered); destroy(); });
    a.join(); b.join(); // CTest timeout is the regression's deadlock guard
    fake::eval_hook = nullptr;
    na::dlss::SetReleaseViewHookForTests(nullptr);
    CloseHandle(entered); CloseHandle(release_reached);
}
}
