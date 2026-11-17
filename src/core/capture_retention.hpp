#pragma once
// Which capture folders go when the captures folder is over its budget (Cort 2026-11-17: 819 GB of
// captures had filled his drive; "We need a process for deleting old captures regularly").
// The rule, with no game and no folder name in it:
//   - the folder may hold `keep_bytes` at most; 0 turns the rule off;
//   - the oldest folders go first, until the total fits;
//   - a folder of the run in progress is never removed, whatever the total.
// Pure and portable, so the CPU test holds it; the Windows side (capture_session.cpp) measures the
// folders, applies the plan on a background thread and logs every removal with its size.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace na::capture::retention {

struct Entry {
    uint64_t bytes = 0;
    uint64_t created = 0;      // any monotonic time; smaller = older
    bool current_run = false;  // made by the process that is running now
};

struct Plan {
    uint64_t total_before = 0, total_after = 0, removed_bytes = 0, protected_bytes = 0;
    uint32_t removed = 0, protected_count = 0;
    bool over_after = false;   // still over the budget: only the current run's folders are left to blame
};

// remove[i] is set for every entry that goes. `remove` must hold `count` flags.
inline Plan Decide(const Entry* entries, size_t count, uint64_t keep_bytes, bool* remove)
{
    Plan p;
    for (size_t i = 0; i < count; ++i) {
        if (remove) remove[i] = false;
        p.total_before += entries[i].bytes;
        if (entries[i].current_run) { p.protected_bytes += entries[i].bytes; ++p.protected_count; }
    }
    p.total_after = p.total_before;
    if (!keep_bytes || !remove) { p.over_after = keep_bytes && p.total_after > keep_bytes; return p; }
    std::vector<size_t> order;
    order.reserve(count);
    for (size_t i = 0; i < count; ++i)
        if (!entries[i].current_run) order.push_back(i);
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t a, size_t b) { return entries[a].created < entries[b].created; });
    for (size_t k = 0; k < order.size() && p.total_after > keep_bytes; ++k) {
        const size_t i = order[k];
        remove[i] = true;
        p.total_after -= entries[i].bytes;
        p.removed_bytes += entries[i].bytes;
        ++p.removed;
    }
    p.over_after = p.total_after > keep_bytes;
    return p;
}

// A capture is not started when the drive has less than `min_free_bytes` left (0 = no guard).
inline bool DiskTooFull(uint64_t free_bytes, uint64_t min_free_bytes)
{
    return min_free_bytes && free_bytes < min_free_bytes;
}

} // namespace na::capture::retention
