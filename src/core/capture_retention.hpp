#pragma once
// Which capture folders go (Cort 2026-11-17, after 819 GB of captures had filled his drive: "every 5
// builds we delete the last 5 builds ... starting at 10 build captures").
// The rule, with no game and no folder name in it:
//   - capture folders are grouped by the build that made them;
//   - when folders of `trigger_builds` different builds are present, only the newest `keep_builds`
//     builds keep their folders and every older build's folders go (10 and 5: it happens again
//     every five builds); a trigger of 0 turns the rule off;
//   - a build is as new as its newest folder;
//   - a folder of the run in progress is never removed, and neither is any folder of its build.
// Pure and portable, so the CPU test holds it; the Windows side (capture_session.cpp) reads each
// folder's build, applies the plan on a background thread and logs every removal with its size.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace na::capture::retention {

struct Entry {
    uint64_t bytes = 0;
    uint64_t created = 0;      // any monotonic time; smaller = older
    uint32_t build = 0;        // the caller's number for the build that made it (one number for "not recorded")
    bool current_run = false;  // made by the process that is running now
};

struct Plan {
    uint64_t total_before = 0, total_after = 0, removed_bytes = 0;
    uint32_t builds_before = 0, builds_after = 0, removed = 0, removed_builds = 0;
    bool triggered = false;
};

// remove[i] is set for every entry that goes. `remove` must hold `count` flags.
inline Plan Decide(const Entry* entries, size_t count, uint32_t trigger_builds, uint32_t keep_builds, bool* remove)
{
    Plan p;
    struct Build { uint32_t id; uint64_t newest; bool current; };
    std::vector<Build> builds;
    for (size_t i = 0; i < count; ++i) {
        if (remove) remove[i] = false;
        p.total_before += entries[i].bytes;
        auto it = std::find_if(builds.begin(), builds.end(), [&](const Build& b) { return b.id == entries[i].build; });
        if (it == builds.end()) builds.push_back({entries[i].build, entries[i].created, entries[i].current_run});
        else { it->newest = (std::max)(it->newest, entries[i].created); it->current = it->current || entries[i].current_run; }
    }
    p.total_after = p.total_before;
    p.builds_before = p.builds_after = static_cast<uint32_t>(builds.size());
    if (!trigger_builds || !remove || builds.size() < trigger_builds) return p;
    p.triggered = true;
    // Newest first; the build of the run in progress counts as the newest of all.
    std::stable_sort(builds.begin(), builds.end(), [](const Build& a, const Build& b) {
        if (a.current != b.current) return a.current;
        return a.newest > b.newest;
    });
    for (size_t k = 0; k < builds.size(); ++k) {
        if (k < keep_builds || builds[k].current) continue;
        ++p.removed_builds;
        for (size_t i = 0; i < count; ++i) {
            if (entries[i].build != builds[k].id || entries[i].current_run) continue;
            remove[i] = true;
            p.total_after -= entries[i].bytes;
            p.removed_bytes += entries[i].bytes;
            ++p.removed;
        }
    }
    p.builds_after = p.builds_before - p.removed_builds;
    return p;
}

// A capture is not started when the drive has less than `min_free_bytes` left (0 = no guard).
inline bool DiskTooFull(uint64_t free_bytes, uint64_t min_free_bytes)
{
    return min_free_bytes && free_bytes < min_free_bytes;
}

} // namespace na::capture::retention
