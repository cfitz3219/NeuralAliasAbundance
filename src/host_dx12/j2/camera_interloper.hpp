// Camera interlopers (Cort 2026-11-06, SE5 main menu: the sweatshirt smears and pulses).
// Run p25748 cut DLSS history on ~19% of frames in a static menu: 5612 cuts with a translation
// of exactly 356.5, and fov cuts of exactly +1.378 then -1.378 on the next frame. Runs of 1-3
// frames, then back. That is a second camera (another view's constants in the same slot), not
// motion: each flip to it and back reset DLSS's history twice.
//
// Generic rule, no game data: a camera that jumped away from the main camera and came back to
// it within a few frames is an interloper. Its offset from the main camera (position, fov,
// direction) is remembered; later candidates with that same offset are not the main camera,
// so the frame keeps the main camera instead of cutting. The first flip still cuts once (it
// cannot be told from a real cut yet). A real cut that stays never comes back, so it is never
// learned; one that returns after the window is treated as a real cut both ways.
#pragma once
#include <array>
#include <cmath>
#include <cstdint>

namespace na::j2 {

struct CameraPose {
    std::array<double, 3> position{}, forward{};
    double fov = 0;
};

class InterloperFilter {
public:
    struct Limits {
        double max_translation = 25.0;   // world units per frame (same as CameraTracker)
        double max_rotation = 0.65;      // radians per frame
        double max_fov_change = 1.0;     // radians per frame
        uint64_t return_window = 8;      // frames within which a return marks the jump an interloper
        uint64_t forget_after = 36000;   // frames without a sighting before an interloper is dropped
    };
    enum class Verdict { First, Continuous, Cut, Interloper };

    InterloperFilter() = default;
    explicit InterloperFilter(const Limits& l) : lim_(l) {}

    // True when `p` matches a learned interloper relative to the current main camera.
    bool IsInterloper(const CameraPose& p) const {
        if (!have_main_) return false;
        for (const auto& k : known_)
            if (k.used && same_offset(offset(p), k.off)) return true;
        return false;
    }
    // The pose chosen for `frame`. Updates the main camera unless it is an interloper.
    Verdict Judge(const CameraPose& p, uint64_t frame) {
        if (!have_main_) { main_ = p; have_main_ = true; last_frame_ = frame; return Verdict::First; }
        expire(frame);
        const double steps = static_cast<double>(frame > last_frame_ ? (frame - last_frame_ < 4 ? frame - last_frame_ : 4) : 1);
        if (!jump(p, main_, steps)) {
            main_ = p; last_frame_ = frame;
            return Verdict::Continuous;
        }
        for (auto& k : known_)
            if (k.used && same_offset(offset(p), k.off)) { k.last_seen = frame; ++rejected_; return Verdict::Interloper; }
        // Back to the pose from before a recent jump: that jump's target was an interloper.
        if (pending_ && frame >= pending_frame_ && frame - pending_frame_ <= lim_.return_window &&
            !jump(p, pending_pre_, steps)) {
            learn(pending_off_, frame);
            pending_ = false;
            main_ = p; last_frame_ = frame;
            return Verdict::Cut;
        }
        pending_ = true; pending_pre_ = main_; pending_off_ = offset(p); pending_frame_ = frame;
        main_ = p; last_frame_ = frame;
        return Verdict::Cut;
    }
    void Reset() { have_main_ = false; pending_ = false; for (auto& k : known_) k = Known{}; }
    uint32_t KnownCount() const { uint32_t n = 0; for (const auto& k : known_) n += k.used ? 1u : 0u; return n; }
    uint64_t Rejected() const { return rejected_; }
    uint64_t Learned() const { return learned_; }

private:
    struct Offset { std::array<double, 3> d{}; double fov = 0, angle = 0; };
    struct Known { bool used = false; Offset off{}; uint64_t last_seen = 0; };
    Offset offset(const CameraPose& p) const {
        Offset o;
        double dot = 0;
        for (size_t j = 0; j < 3; ++j) { o.d[j] = p.position[j] - main_.position[j]; dot += p.forward[j] * main_.forward[j]; }
        o.fov = p.fov - main_.fov;
        o.angle = std::acos(dot < -1 ? -1 : dot > 1 ? 1 : dot);
        return o;
    }
    static double length(const std::array<double, 3>& v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
    // The same displacement from the main camera: within 5% of its length (plus half a unit),
    // the same fov change within 0.02 rad and the same turn within 0.02 rad.
    static bool same_offset(const Offset& a, const Offset& b) {
        std::array<double, 3> e{};
        for (size_t j = 0; j < 3; ++j) e[j] = a.d[j] - b.d[j];
        return length(e) <= 0.5 + 0.05 * length(b.d) && std::fabs(a.fov - b.fov) <= 0.02 && std::fabs(a.angle - b.angle) <= 0.02;
    }
    bool jump(const CameraPose& p, const CameraPose& from, double steps) const {
        double d2 = 0, dot = 0;
        for (size_t j = 0; j < 3; ++j) { const double d = p.position[j] - from.position[j]; d2 += d * d; dot += p.forward[j] * from.forward[j]; }
        const double t = lim_.max_translation * steps;
        const double angle = std::acos(dot < -1 ? -1 : dot > 1 ? 1 : dot);
        return d2 > t * t || angle > (lim_.max_rotation * steps < 3.0 ? lim_.max_rotation * steps : 3.0) || std::fabs(p.fov - from.fov) > lim_.max_fov_change;
    }
    void learn(const Offset& o, uint64_t frame) {
        Known* slot = nullptr;
        for (auto& k : known_) if (!k.used) { slot = &k; break; }
        if (!slot) { slot = &known_[0]; for (auto& k : known_) if (k.last_seen < slot->last_seen) slot = &k; }
        *slot = Known{true, o, frame};
        ++learned_;
    }
    void expire(uint64_t frame) {
        for (auto& k : known_) if (k.used && frame > k.last_seen && frame - k.last_seen > lim_.forget_after) k = Known{};
    }
    Limits lim_;
    bool have_main_ = false, pending_ = false;
    CameraPose main_{}, pending_pre_{};
    Offset pending_off_{};
    uint64_t last_frame_ = 0, pending_frame_ = 0, rejected_ = 0, learned_ = 0;
    std::array<Known, 4> known_{};
};

} // namespace na::j2
