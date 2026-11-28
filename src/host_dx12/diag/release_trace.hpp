#pragma once
// Timings of the work that releases our video memory, and of the game thread's waits on the DLSS backend's lock,
// kept for tests and the log (2026-11-27: a single 256 ms frame while memory was freed at 3440x1440 had to be
// found). Each entry: the longest such time since the last Take, and what it was.
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace na::diag::release_trace {
struct Entry { double ms = 0; std::string what; unsigned long long tick = 0; };
inline std::mutex g_m;
inline std::vector<Entry> g_entries; // under g_m; bounded
inline void Note(const char* what, double ms, unsigned long long tick)
{
    std::lock_guard<std::mutex> lock(g_m);
    if (g_entries.size() >= 4096) g_entries.erase(g_entries.begin());
    g_entries.push_back({ms, what ? what : "", tick});
}
inline std::vector<Entry> Take()
{
    std::lock_guard<std::mutex> lock(g_m);
    std::vector<Entry> out;
    out.swap(g_entries);
    return out;
}
} // namespace na::diag::release_trace
