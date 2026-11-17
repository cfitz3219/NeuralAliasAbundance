#pragma once
#include <cstdint>
#include <functional>
namespace na::background {
// Bounded, independent jobs only. Long-running consumer loops retain their own owners.
// No render callback waits. Graphics work is excluded from observation on its actual worker.
bool Start();
bool Submit(std::function<void()> job);
// Explicit orderly shutdown / offline tests only. A timeout retains the live pool for retry.
bool Stop(uint32_t timeout_ms);
bool WaitIdle(uint32_t timeout_ms);
// Loader-lock-safe process-exit notification: no allocation, locking, joins or callbacks.
void RequestStop();
struct Stats {uint64_t accepted=0,completed=0,failed=0,rejected=0;uint32_t pending=0,active=0,workers=0,peak_active=0;bool stopping=false;};
Stats GetStats();
}
