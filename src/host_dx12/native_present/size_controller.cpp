#include "host_dx12/native_present/size_controller.hpp"
#include <mutex>

namespace na::native_present::ctl {
namespace {
std::mutex g_m;
Status g_s;
uint64_t g_since = 0;          // ms: the Ask (or the allocation) the timeout counts from
bool g_sample_this_present = false;
Extent g_previous{};           // the applied size before this change (restore target)
float g_previous_scale = 1.f;
bool valid(Extent e) { return e.w && e.h; }
}

void Reset(Extent output, Extent buffers, float applied_scale)
{
    std::lock_guard<std::mutex> lock(g_m);
    const Status old = g_s;
    g_s = Status{};
    g_s.output = output; g_s.applied = buffers; g_s.buffers = buffers; g_s.target = buffers;
    g_s.applied_scale = g_s.target_scale = applied_scale;
    g_s.commits = old.commits; g_s.ignored = old.ignored; g_s.restores = old.restores; g_s.superseded = old.superseded;
    g_s.alloc_generation = old.alloc_generation + 1;
    g_sample_this_present = false;
}

Action Choose(Extent target, float scale, uint64_t now_ms)
{
    std::lock_guard<std::mutex> lock(g_m);
    if (!valid(target) || !valid(g_s.output)) return {};
    if (g_s.phase != Phase::Idle && Same(target, g_s.target)) return {};       // already on its way
    if (g_s.phase == Phase::Idle && Same(target, g_s.applied)) {               // already applied
        g_s.target = target; g_s.target_scale = scale; g_s.applied_scale = scale;
        return {};
    }
    if (g_s.phase != Phase::Idle) { ++g_s.superseded; g_s.last = Outcome::Superseded; }
    else { g_previous = g_s.applied; g_previous_scale = g_s.applied_scale; }
    g_s.target = target; g_s.target_scale = scale;
    g_s.phase = Phase::Asked; g_s.token = 0; g_s.matches = 0; g_s.contradictions = 0;
    g_since = now_ms;
    // Buffers already at the target (e.g. a cancelled change the game had not acted on):
    // only the scene confirmation is needed.
    if (Same(g_s.buffers, target)) g_s.phase = Phase::Allocated;
    return {ActionKind::Ask, target, scale};
}

void Asked(uint64_t token, uint64_t now_ms)
{
    std::lock_guard<std::mutex> lock(g_m);
    if (g_s.phase == Phase::Idle) return;
    if (!token && g_s.phase == Phase::Asked) { g_s.phase = Phase::Idle; g_s.target = g_s.applied; g_s.target_scale = g_s.applied_scale; return; }
    g_s.token = token; g_since = now_ms;
}

void Allocated(Extent buffers, uint64_t now_ms)
{
    std::lock_guard<std::mutex> lock(g_m);
    g_s.buffers = buffers; // the real allocation, immediately (Astra a066d76 point 1)
    ++g_s.alloc_generation;
    g_s.matches = 0; g_s.contradictions = 0;
    if ((g_s.phase == Phase::Asked || g_s.phase == Phase::Restoring) && Same(buffers, g_s.target)) { g_s.phase = Phase::Allocated; g_since = now_ms; }
    else if (g_s.phase == Phase::Idle && !Same(buffers, g_s.applied)) {
        // A natural game/window resize: treat as the new applied layout once the scene confirms.
        g_previous = g_s.applied; g_previous_scale = g_s.applied_scale;
        g_s.target = buffers; g_s.phase = Phase::Allocated; g_since = now_ms;
    }
}

void Observe(Extent scene)
{
    std::lock_guard<std::mutex> lock(g_m);
    if (g_sample_this_present) return; // one sample per present
    if (!valid(scene)) return;         // UNKNOWN: neither a match nor a contradiction
    g_sample_this_present = true;
    if (g_s.phase != Phase::Allocated) return;
    if (Same(scene, g_s.target)) { ++g_s.matches; g_s.contradictions = 0; }
    else { g_s.matches = 0; ++g_s.contradictions; } // a known different size resets the count
}

Action Tick(uint64_t now_ms)
{
    std::lock_guard<std::mutex> lock(g_m);
    g_sample_this_present = false;
    if (g_s.phase == Phase::Idle) return {};
    if (g_s.phase == Phase::Allocated && g_s.matches >= kMatches) {
        g_s.applied = g_s.target; g_s.applied_scale = g_s.target_scale;
        g_s.phase = Phase::Idle; g_s.token = 0; ++g_s.commits; g_s.last = Outcome::Committed;
        return {ActionKind::Commit, g_s.applied, g_s.applied_scale};
    }
    const bool timed_out = now_ms >= g_since && now_ms - g_since >= kTimeoutMs;
    if (g_s.phase == Phase::Asked && timed_out) {
        // The game never rebuilt: nothing to undo but the advertisement.
        g_s.phase = Phase::Idle; g_s.token = 0; ++g_s.ignored; g_s.last = Outcome::Ignored;
        g_s.target = g_s.applied; g_s.target_scale = g_s.applied_scale;
        return {ActionKind::Ignored, g_s.applied, g_s.applied_scale};
    }
    if (g_s.phase == Phase::Allocated && (timed_out || g_s.contradictions >= kContradict)) {
        // The buffers changed but the scene did not follow: ask for the previous layout back.
        if (Same(g_s.buffers, g_previous)) { // nothing to rebuild (a natural resize that did not settle)
            g_s.phase = Phase::Idle; g_s.target = g_s.applied; g_s.last = Outcome::Contradicted; ++g_s.restores;
            return {ActionKind::Ignored, g_s.applied, g_s.applied_scale};
        }
        g_s.last = g_s.contradictions >= kContradict ? Outcome::Contradicted : Outcome::Restored;
        ++g_s.restores;
        g_s.target = g_previous; g_s.target_scale = g_previous_scale;
        g_s.applied = g_previous; g_s.applied_scale = g_previous_scale;
        g_s.phase = Phase::Restoring; g_s.token = 0; g_s.matches = 0; g_s.contradictions = 0; g_since = now_ms;
        return {ActionKind::Restore, g_previous, g_previous_scale};
    }
    if (g_s.phase == Phase::Restoring && timed_out) {
        // The game did not answer the restore either: stop asking (no retry storm).
        g_s.phase = Phase::Idle; g_s.token = 0; g_s.last = Outcome::Ignored; ++g_s.ignored;
        return {ActionKind::Ignored, g_s.applied, g_s.applied_scale};
    }
    return {};
}

Status Get() { std::lock_guard<std::mutex> lock(g_m); return g_s; }

Extent AllocationSize()
{
    std::lock_guard<std::mutex> lock(g_m);
    return g_s.phase != Phase::Idle ? g_s.target : g_s.applied;
}
} // namespace na::native_present::ctl
