#pragma once
// Event log (Cort 2026-11-06: the log must gather all data we may need). Transitions that
// the 5 s status samples cannot time (DLSS delivered/not, route, HUD method, mip/AF, setting
// changes and who made them, device removal) are queued here from any thread with the
// present frame number and a millisecond timestamp, and drained into the status log by the
// status writer. Header-only so every host library can emit without a new link dependency.
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>
#include <windows.h>

namespace na::eventlog {

inline std::atomic<uint64_t> g_frame{0};       // incremented once per game Present
inline std::atomic<uint64_t> g_dropped{0};     // events lost because the queue was full
inline std::atomic<uint64_t> g_total{0};
inline std::mutex g_mu;
inline std::vector<std::string> g_queue;
constexpr size_t kMaxQueued = 8192;           // ~5 s of per-frame flapping at 1000+ fps

// Who is acting on this thread (panel, mode commit, ini reload...); "code" otherwise.
inline thread_local const char* t_source = nullptr;
struct ScopedSource {
    const char* previous;
    explicit ScopedSource(const char* s) : previous(t_source) { t_source = s; }
    ~ScopedSource() { t_source = previous; }
};
inline const char* Source() { return t_source ? t_source : "code"; }

inline void TickFrame() { g_frame.fetch_add(1, std::memory_order_relaxed); }
inline uint64_t Frame() { return g_frame.load(std::memory_order_relaxed); }

// One line: "event t=<ms since boot> frame=<n> thread=<id> src=<who> <category> <text>".
inline void Emit(const char* category, const char* fmt, ...)
{
    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    char line[1200];
    const int m = snprintf(line, sizeof line, "event t=%llu frame=%llu thread=%lu src=%s %s %s\r\n",
                           static_cast<unsigned long long>(GetTickCount64()),
                           static_cast<unsigned long long>(Frame()), GetCurrentThreadId(), Source(), category, body);
    if (m <= 0) return;
    g_total.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_queue.size() >= kMaxQueued) { g_dropped.fetch_add(1, std::memory_order_relaxed); return; }
    g_queue.emplace_back(line, static_cast<size_t>((std::min)(m, int(sizeof line) - 1)));
}

// Edge detector: emits only when the value changes, so per-frame callers stay cheap.
struct Edge {
    std::atomic<int64_t> last{INT64_MIN};
    // returns true (and the previous value) when v differs from the last value seen
    bool Changed(int64_t v, int64_t* previous = nullptr)
    {
        const int64_t old = last.exchange(v, std::memory_order_relaxed);
        if (previous) *previous = old;
        return old != v;
    }
};

// Status writer only: hands back everything queued since the last drain.
inline std::vector<std::string> Drain()
{
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lk(g_mu);
    out.swap(g_queue);
    return out;
}

} // namespace na::eventlog
