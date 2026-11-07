// Scene depth publication from accumulated evidence (2026-11-04).
//
// The live J2 pick used to be replaced by whatever ONE frame's ranking said at
// each 250 ms publication. In Sniper Elite 5 one frame in ~9 lacked the scene
// pass entirely (only shadow maps ranked), so the pick became 0 for a whole
// publication (~11 frames) and DLSS stretched and restarted. Here the depth
// role is decided from the top depth of every completed frame in a recent
// window, never from one snapshot:
//  - a retired resource is dropped at once (resize / destruction);
//  - no pick yet: adopt the window's majority leader;
//  - another depth replaces the pick only when it leads the window clearly AND
//    the pick has (almost) vanished from it, for two publications in a row
//    (one frame where a shadow or reflection pass wins is not a contradiction);
//  - a window with no scene evidence at all clears the pick only after it
//    persists for kAbsentPublications.
// Pure logic, no host types: tests/cpu/scene_pick_test.cpp.
#pragma once
#include <cstdint>
#include <cstddef>
#include <map>

namespace na::discovery {

struct DepthWindow {
    static constexpr size_t kFrames = 16;
    uint64_t top[kFrames]{}; // top eligible depth of each completed frame (0 = none)
    size_t count = 0, next = 0;
    void Push(uint64_t depth) {
        top[next] = depth;
        next = (next + 1) % kFrames;
        if (count < kFrames) ++count;
    }
    size_t Count(uint64_t depth) const {
        size_t n = 0;
        for (size_t i = 0; i < count; ++i) n += top[i] == depth ? 1u : 0u;
        return n;
    }
    // Most frequent nonzero entry; ties go to the most recent.
    uint64_t Leader(size_t* n_out, uint64_t excluded=0) const {
        std::map<uint64_t, size_t> n;
        for (size_t i = 0; i < count; ++i) if (top[i] && top[i] != excluded) ++n[top[i]];
        uint64_t best = 0; size_t best_n = 0;
        for (size_t k = 0; k < count; ++k) {
            const size_t i = (next + kFrames - 1 - k) % kFrames; // newest first
            if (!top[i] || top[i] == excluded) continue;
            const size_t c = n[top[i]];
            if (c > best_n) { best = top[i]; best_n = c; }
        }
        if (n_out) *n_out = best_n;
        return best;
    }
};

enum class PickReason : uint8_t { kept, first, replaced, retired, absent, none };
inline const char* PickReasonName(PickReason r) {
    switch (r) {
    case PickReason::kept: return "kept";
    case PickReason::first: return "first";
    case PickReason::replaced: return "replaced";
    case PickReason::retired: return "retired";
    case PickReason::absent: return "absent";
    default: return "none";
    }
}

struct DepthPick {
    static constexpr unsigned kReplaceAfter = 2;      // publications a clear challenger must hold
    static constexpr unsigned kAbsentPublications = 8; // ~2 s with no scene evidence before clearing
    uint64_t depth = 0;
    uint64_t challenger = 0;
    unsigned challenger_streak = 0, absent_streak = 0;
    // Diagnostics of the last decision.
    uint64_t leader = 0;
    size_t leader_n = 0, current_n = 0, frames = 0;
    PickReason last = PickReason::none;

    PickReason Publish(const DepthWindow& w, bool current_alive) {
        frames = w.count;
        leader = w.Leader(&leader_n);
        current_n = depth ? w.Count(depth) : 0;
        if (depth && !current_alive) {
            const uint64_t retired = depth;
            depth = 0;
            challenger = 0; challenger_streak = absent_streak = 0;
            // Old votes cannot keep a destroyed texture selected or delay
            // an established replacement. Empty observations still count.
            leader = w.Leader(&leader_n, retired);
            const bool majority = leader_n >= 2 && leader_n * 2 >= frames;
            const bool recent_replacement = leader_n >= 4 && leader_n * 2 >= frames - current_n &&
                w.top[(w.next + DepthWindow::kFrames - 1) % DepthWindow::kFrames] == leader;
            if (leader && (majority || recent_replacement)) depth = leader;
            return last = PickReason::retired;
        }
        if (!depth) {
            challenger = 0; challenger_streak = absent_streak = 0;
            if (leader && leader_n * 2 >= frames && leader_n >= 2) { depth = leader; return last = PickReason::first; }
            return last = PickReason::none;
        }
        if (leader && leader != depth && leader_n * 4 >= frames * 3 && current_n * 8 <= frames) {
            absent_streak = 0;
            if (leader == challenger) ++challenger_streak; else { challenger = leader; challenger_streak = 1; }
            if (challenger_streak >= kReplaceAfter) {
                depth = leader;
                challenger = 0; challenger_streak = 0;
                return last = PickReason::replaced;
            }
            return last = PickReason::kept;
        }
        challenger = 0; challenger_streak = 0;
        if (current_n == 0 && leader_n == 0) {
            if (++absent_streak >= kAbsentPublications) {
                depth = 0;
                absent_streak = 0;
                return last = PickReason::absent;
            }
            return last = PickReason::kept;
        }
        absent_streak = 0;
        return last = PickReason::kept;
    }
};

} // namespace na::discovery
