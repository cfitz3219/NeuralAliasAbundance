// Rules that admit a ViewProjection as the main camera when no inverse sits
// beside it. One definition for every graphics API: a provider reports what its
// scene draws bound and calls these, it never carries its own copy.
//
// The inverse-pair rule (matrix_candidates.hpp, C12) stays the first proof. These
// two cover games that upload no inverse at all:
//  - StartWithoutInverse: a ViewProjection that keeps its logical slot and moves
//    like one camera over consecutive scene frames starts the camera.
//  - ContinuesCamera: a ViewProjection that continues the last main camera is
//    that camera for this frame.
// IsCameraRelative says whether an admitted matrix carries the camera position
// (a full camera) or has it removed (camera-relative rendering: the position
// must come from elsewhere, from a witness).
//
// History: both rules were written inside host_dx12/j2/live_dlaa.cpp (2026-11-09,
// for Call of Duty WWII through the DX11 bridge) and only DX12 had them. Moved
// here unchanged on 2026-11-11 so the native DX11 provider uses the same rules
// (Cort: "DX11 should have been exactly the same as DX12").
//
// Portable C++17 (Linux-tested).
#pragma once
#include "providers/camera/camera_tracker.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace na::camera {

// Where a matrix sits, as the shaders see it. Never the physical buffer: upload
// rings rotate the resource and its offset every frame (Astra, p12724).
// DX12: binding = root parameter (and table or not), size = block size.
// DX11: binding = shader stage and slot, range = first constant and count.
struct StartSlot {
    uint64_t binding = 0;
    uint64_t range = 0;
    uint64_t size = 0;        // tells apart ranges that share one binding and in-block offset
    uint64_t byte_offset = 0; // of the matrix inside the block
};
inline bool operator==(const StartSlot& a, const StartSlot& b)
{
    return a.binding == b.binding && a.range == b.range && a.size == b.size && a.byte_offset == b.byte_offset;
}
inline bool operator!=(const StartSlot& a, const StartSlot& b) { return !(a == b); }

// Two consecutive frames of one camera: nearly the same direction, a bounded
// step, the same lens.
inline bool StartContinues(const Matrix& a, const Matrix& b)
{
    VpGeometry u{}, v{};
    if (!DecomposeViewProjection(a, u) || !DecomposeViewProjection(b, v)) return false;
    double dot = 0, d2 = 0;
    for (int j = 0; j < 3; ++j) {
        dot += u.forward[j] * v.forward[j];
        d2 += (u.position[j] - v.position[j]) * (u.position[j] - v.position[j]);
    }
    return dot > 0.9 && d2 <= 2500.0 && std::abs(u.sy - v.sy) <= 0.05 * std::abs(v.sy);
}

// The first main camera, proven by persistence instead of an inverse pair.
// Feed it one candidate per scene frame: the first camera-shaped matrix of the
// best-ranked block. One verdict per frame: a retry of the same frame reuses it
// instead of restarting the streak.
class StartWithoutInverse {
public:
    static constexpr uint32_t kFrames = 4;

    // True when `vp` at `slot` on `frame` completes the streak.
    bool Judge(const StartSlot& slot, const Matrix& vp, uint64_t frame)
    {
        const bool same_slot = valid_ && slot_ == slot;
        if (valid_ && frame_ == frame) // already judged this frame: the verdict belongs to that slot and camera only
            return same_slot && verdict_ && StartContinues(vp_, vp);
        const bool consecutive = valid_ && frame > frame_ && frame - frame_ <= 2; // a menu frame between is tolerated
        if (same_slot && consecutive && StartContinues(vp_, vp)) ++streak_;
        else streak_ = 1;
        valid_ = true;
        slot_ = slot;
        vp_ = vp;
        frame_ = frame;
        verdict_ = streak_ >= kFrames;
        return verdict_;
    }
    uint32_t streak() const { return streak_; }
    void Reset() { *this = {}; }

private:
    bool valid_ = false, verdict_ = false;
    StartSlot slot_{};
    Matrix vp_{};
    uint64_t frame_ = 0;
    uint32_t streak_ = 0;
};

// `m` shows the camera whose geometry is `last` (the last accepted main camera).
// Tighter than StartContinues: this one admits a matrix on its own frame.
inline bool ContinuesCamera(const VpGeometry& last, const Matrix& m)
{
    VpGeometry v{};
    if (!DecomposeViewProjection(m, v)) return false;
    double dot = 0, d2 = 0, len2 = 0;
    for (int j = 0; j < 3; ++j) {
        dot += v.forward[j] * last.forward[j];
        d2 += (v.position[j] - last.position[j]) * (v.position[j] - last.position[j]);
        len2 += last.position[j] * last.position[j];
    }
    return dot > 0.98 && std::abs(v.sy - last.sy) <= 0.05 * std::abs(last.sy) && d2 <= 0.01 * len2 + 100.0;
}

// The matrix has the camera position removed (the game renders relative to the
// camera). It gives direction and lens; the position must be witnessed elsewhere.
inline bool IsCameraRelative(const Matrix& m)
{
    VpGeometry g{};
    return DecomposeViewProjection(m, g) && std::abs(g.position[0]) <= 1e-5 && std::abs(g.position[1]) <= 1e-5 &&
           std::abs(g.position[2]) <= 1e-5;
}

// A main camera fills the scene picture: its aspect is the picture's.
inline bool MatchesSceneAspect(const Matrix& m, uint32_t width, uint32_t height, double tolerance = 0.02)
{
    VpGeometry g{};
    return width && height && DecomposeViewProjection(m, g) && g.sx > 0 &&
           std::abs(g.sy / g.sx - double(width) / height) <= tolerance;
}

// ---------------------------------------------------------------------------
// The two rules applied to one frame's constant blocks, for a provider that has
// found no camera on this frame by a stronger proof.
//
// A game shows what kind of camera it has, and the strongest kind it has shown
// decides which rules stay open:
//  - it uploads inverse pairs: the camera is always found by them. A lone matrix
//    may continue that camera for a frame, but never start a new one (a shadow or
//    reflection camera would qualify during a short loss).
//  - it renders relative to the camera and witnesses the position elsewhere
//    (the relative-camera vote): the camera is that pair. A frame without the witness
//    is a miss with that reason, never a camera at the wrong place, so both rules
//    are closed.
//  - neither: both rules are open.
//
// `blocks` are the ranges the scene's draws bound, best-ranked first (most scene
// draws). Candidates are ViewProjections with no inverse beside them whose
// aspect is the scene picture's. In order:
//  1. Continued: a candidate that continues the last main camera.
//  2. Started: the first candidate of the frame completes its streak.
struct AdmissionBlock {
    StartSlot slot{};             // byte_offset is filled per candidate
    const uint8_t* bytes = nullptr;
    size_t size = 0;
};
enum class Admitted : uint8_t { None, Started, Continued };
// Why nothing was admitted. The status log prints it.
enum class NotAdmitted : uint8_t {
    Admitted,
    PositionGame, // closed: this game witnesses the camera position, and this frame had no witness
    NoCandidate,  // no camera-shaped matrix without an inverse has the scene's aspect
    InverseGame,  // candidates exist, none continues the camera, and the start rule is closed
    Waiting,      // the start candidate has not persisted long enough yet (see `streak`)
};
struct Admission {
    Admitted how = Admitted::None;
    NotAdmitted why = NotAdmitted::NoCandidate;
    size_t block = 0;        // index into `blocks`
    Candidate candidate{};
    bool relative = false;   // the admitted matrix has the camera position removed
    uint32_t candidates = 0; // camera-shaped matrices without an inverse, with the scene's aspect
    uint32_t streak = 0;     // frames the start candidate has persisted (kFrames admits)
};

class NoInverseCamera {
public:
    static constexpr size_t kMaxBlocks = 12;

    // Every accepted main camera, whichever rule found it.
    void Remember(const Matrix& main_camera, bool by_inverse_pair)
    {
        VpGeometry g{};
        if (DecomposeViewProjection(main_camera, g)) { last_ = g; have_last_ = true; }
        inverse_pairs_ = inverse_pairs_ || by_inverse_pair;
    }
    // The game showed where the camera is by other means (the relative-camera vote).
    void PositionWitnessed() { positions_ = true; }

    Admission Admit(const std::vector<AdmissionBlock>& blocks, uint32_t width, uint32_t height, uint64_t frame)
    {
        Admission r;
        if (positions_) { r.why = NotAdmitted::PositionGame; return r; }
        struct Found { size_t block; Candidate c; };
        std::vector<Found> found;
        for (size_t i = 0; i < blocks.size() && i < kMaxBlocks; ++i) {
            const auto& b = blocks[i];
            const size_t usable = b.size & ~size_t(3);
            if (!b.bytes || usable < 64) continue;
            Snapshot s{};
            s.session = s.epoch = 1;
            s.submission = s.content_version = s.resource = frame + 1; // read at the game's own upload: scope known
            s.complete = s.lifetime_valid = true;
            s.sync = Sync::AppSyncUnknown;
            s.bytes.assign(b.bytes, b.bytes + usable);
            for (const auto& c : FindMatrices(s).candidates)
                if (c.kind == Kind::ViewProjection && !c.has_inverse_pair && MatchesSceneAspect(c.matrix, width, height))
                    found.push_back({i, c});
        }
        r.candidates = static_cast<uint32_t>(found.size());
        const auto admit = [&](const Found& f, Admitted how) {
            r.how = how;
            r.why = NotAdmitted::Admitted;
            r.block = f.block;
            r.candidate = f.c;
            r.relative = IsCameraRelative(f.c.matrix);
            return r;
        };
        if (found.empty()) return r;
        if (have_last_)
            for (const auto& f : found)
                if (ContinuesCamera(last_, f.c.matrix)) return admit(f, Admitted::Continued);
        if (inverse_pairs_) { r.why = NotAdmitted::InverseGame; return r; }
        const Found& f = found.front(); // the best-ranked block's first candidate, once per frame
        StartSlot slot = blocks[f.block].slot;
        slot.byte_offset = f.c.byte_offset;
        const bool started = start_.Judge(slot, f.c.matrix, frame);
        r.streak = start_.streak();
        if (started) return admit(f, Admitted::Started);
        r.why = NotAdmitted::Waiting;
        return r;
    }

private:
    StartWithoutInverse start_;
    VpGeometry last_{};
    bool have_last_ = false, inverse_pairs_ = false, positions_ = false;
};

} // namespace na::camera
