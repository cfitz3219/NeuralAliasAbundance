// Render-small changeover controller (COMMS 4bd474c I1.2/I1.3, Astra's approval a066d76).
// Pure state, no Windows calls: the caller (overlay, presenter, J2) feeds events and carries out
// the returned action through size_spoof / presenter. One pending change at a time:
//
//   Choose(target)  -> Ask(target)            game is sent the virtual resize (size_spoof token)
//   Allocated(size) <- presenter proxies       the game rebuilt; buffers have their real size now
//   Observe(scene)  <- J2 selected scene size  per present, at most one sample; UNKNOWN is skipped
//   3 fresh matches after the allocation        -> Commit (applied size/mode, mip bias, status)
//   known contradiction (kContradict in a row) -> Restore (ask for the last applied size again)
//   timeout without allocation                 -> Ignored (advertisement rolls back; nothing rebuilt)
//   timeout after allocation, unconfirmed      -> Restore
// A newer Choose supersedes the pending one; a late event for an old token is ignored.
#pragma once
#include <cstdint>

namespace na::native_present::ctl {
struct Extent { uint32_t w = 0, h = 0; };
inline bool Same(Extent a, Extent b) { return a.w == b.w && a.h == b.h; }
enum class Phase : uint32_t { Idle, Asked, Allocated, Restoring };
enum class Outcome : uint32_t { None, Committed, Ignored, Restored, Superseded, Contradicted };
enum class ActionKind : uint32_t { None, Ask, Commit, Ignored, Restore };
struct Action {
    ActionKind kind = ActionKind::None;
    Extent size{};      // Ask/Restore: the size to ask for; Commit: the applied size
    float scale = 1.f;  // Commit: the applied mode's scale (1 = native DLAA)
};
struct Status {
    Extent output{}, applied{}, buffers{}, target{};
    float applied_scale = 1.f, target_scale = 1.f;
    Phase phase = Phase::Idle;
    Outcome last = Outcome::None;
    uint64_t token = 0, alloc_generation = 0;
    uint32_t matches = 0, contradictions = 0;
    uint64_t commits = 0, ignored = 0, restores = 0, superseded = 0;
};

constexpr uint32_t kMatches = 3;        // fresh matching scene samples after the allocation
constexpr uint32_t kContradict = 30;    // known different samples in a row after the allocation
constexpr uint64_t kTimeoutMs = 5000;

// Boot / output recreation: native buffers, nothing pending. applied = buffers.
void Reset(Extent output, Extent buffers, float applied_scale);
// The user's mode (or native DLAA: scale 1, target = output). Returns Ask when the game must be
// asked; None when it is already applied/pending at that size. Supersedes a pending change.
Action Choose(Extent target, float scale, uint64_t now_ms);
// The size_spoof token for the Ask just carried out (0 = posting failed: the change is dropped).
void Asked(uint64_t token, uint64_t now_ms);
// The presenter allocated the game's buffers at this size (inside the game's ResizeBuffers/GetBuffer).
void Allocated(Extent buffers, uint64_t now_ms);
// One scene sample per present: the J2-selected scene colour size, or {0,0} for UNKNOWN.
void Observe(Extent scene);
// Once per present, after Observe: commit / timeout decisions.
Action Tick(uint64_t now_ms);
Status Get();
// The buffers the presenter should create for the game now (applied unless a delivered change).
Extent AllocationSize();
} // namespace na::native_present::ctl
