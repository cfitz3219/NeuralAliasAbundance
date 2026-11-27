#pragma once

#include "core/referee_trial.hpp"

namespace na::referee {

constexpr uint8_t normal_jitter = 100, reduced_jitter = 50, quarter_jitter = 25;

// Compare what actually rasterized with the requested phase on this frame.
// DX12 quantizes viewport offsets to 1/65536 of a pixel.
inline uint8_t VerifiedJitterAmount(uint8_t amount, float requested_x, float requested_y, float x, float y)
{
    constexpr float tolerance = 1.f / 32768.f;
    return (amount == normal_jitter || amount == reduced_jitter || amount == quarter_jitter) &&
           std::fabs(requested_x - x) < tolerance && std::fabs(requested_y - y) < tolerance &&
           std::hypot(x, y) > tolerance ? amount : 0;
}

// Compare the scene structure independently of a uniform change in log luminance.
inline bool SameJitterPicture(const std::array<float, FlickerTransition::count>& a,
                              const std::array<float, FlickerTransition::count>& b)
{
    float mean = 0;
    for (unsigned i = 0; i < a.size(); ++i) mean += a[i] - b[i];
    mean /= float(a.size());
    float difference = 0;
    for (unsigned i = 0; i < a.size(); ++i) difference += std::fabs(a[i] - b[i] - mean);
    return difference <= .10f * a.size();
}

// VARIANT: referee/jitter-control
// One window compares the same visible edge at normal and reduced strength.
// A second normal-strength window must reproduce the problem. Never promote
// a smaller jitter based on reduced motion alone: it must preserve detail.
class JitterTrial {
public:
    using Position = RegionalTiles::Position;
    struct Measurement {
        Position at;
        uint32_t layout = 0;
        float shift = 0, reconstructed = 0, detail = 0;
        bool locked = false;
        std::array<float, FlickerTransition::count> image{};
        bool field = false;
        RegionalTiles controls{};
        TileLocations locations{};
        float phase_step = 0;
        struct Witness { unsigned index; float shift, detail; };
        std::vector<Witness> witnesses;
        unsigned regional_checks = 0, regional_improved = 0, regional_worse = 0;
    };
    enum class Phase : uint8_t { Observe, Confirm, Reduce, Recheck, Keep, Cooldown };
    Phase Current() const { return phase_; }
    const Measurement& Reference() const { return reference_; }
    bool NeedsReference() const {
        return phase_ == Phase::Confirm || phase_ == Phase::Reduce || phase_ == Phase::Recheck || phase_ == Phase::Keep;
    }
    // VARIANT: referee/no-strength-reduction (coordinator 2026-11-27): the Referee never weakens the jitter.
    // Halving it (22 times in Cort's 0f63301 Hearts of Iron IV run, 3 minutes of weaker anti-aliasing) fixed
    // nothing; vibrating edges are answered by moving the scene draws left off the jitter onto the sample.
    uint8_t Amount() const { return normal_jitter; }
    // The same displayed edges vibrated twice and no scene mesh was left off the jitter by the dropout rule:
    // the Referee's coverage trial (core/referee_coverage.hpp) tries the classes of draws left still.
    bool ClassTrialWanted() const { return class_trial_; }
    void ClassTrialTaken() { class_trial_ = false; }
    // Scene meshes the host left off the frame's sample on the frames being compared (0: none).
    void NoteUnjittered(uint64_t meshes) { unjittered_meshes_ = meshes; }
    // The answer to vibration when such meshes exist: the host moves them onto the sample.
    bool MoveUnjittered() const { return move_unjittered_; }
    const char* Reason() const { return reason_; }
    float Baseline() const { return std::fabs(reference_.shift); }
    float Candidate() const { return candidate_; }
    const char* Name() const {
        constexpr const char* names[]{"observe", "confirm", "reduce", "recheck", "kept", "restored"};
        return names[static_cast<unsigned>(phase_)];
    }
    void Tick(uint64_t now)
    {
        if (NeedsReference() && now >= deadline_) Reject(now, "comparable raster and screen evidence expired");
        else if (phase_ == Phase::Cooldown && now >= deadline_) phase_ = Phase::Observe;
    }
    void Bypassed(uint64_t) {}
    void Observe(const Measurement& m, uint8_t amount, uint64_t now)
    {
        if (amount != Amount() || !std::isfinite(m.detail) || !std::isfinite(m.shift) || phase_ == Phase::Cooldown) return;
        const float wobble = std::fabs(m.shift);
        if (phase_ == Phase::Observe) {
            if (!m.locked || wobble < .12f || m.detail <= .02f) return;
            if (m.field && std::count_if(m.controls.detail.begin(), m.controls.detail.end(),
                    [](float detail) { return detail >= .02f; }) < 3) return;
            if (rejected_view_ && SameJitterPicture(reference_.image, m.image)) return;
            rejected_view_ = false;
            reference_ = m;
            candidate_ = 0;
            Set(Phase::Confirm, now, "checking the same problem edges again before trying the draws left off the jitter");
            return;
        }
        if (m.field != reference_.field || !SameView(m)) return;
        const bool repeated = m.locked && m.shift * reference_.shift > 0.f &&
            wobble >= .70f * Baseline() && wobble <= 1.45f * Baseline();
        if (phase_ == Phase::Confirm && repeated) {
            // Edges that vibrate while scene meshes were drawn off the jitter are those meshes: they take the
            // frame's sample first. Otherwise the coverage trial tries the classes of draws left still.
            if (unjittered_meshes_) {
                move_unjittered_ = true;
                Reject(now, "the vibrating edges belong to scene meshes drawn off the jitter: those meshes take the frame's sample first (strength unchanged)");
                return;
            }
            class_trial_ = true;
            Reject(now, "the same displayed edges vibrated in a second independent window: the scene draws left off the jitter are tried, class by class (the jitter is never weakened)");
        }
    }

private:
    static bool Improves(float candidate, float baseline) {
        return candidate <= baseline * .70f && baseline - candidate >= .035f;
    }
    bool SameView(const Measurement& m) const {
        if (m.at.x != reference_.at.x || m.at.y != reference_.at.y || m.layout != reference_.layout) return false;
        if (m.field) return SameJitterPicture(reference_.image, m.image);
        float difference = 0;
        for (unsigned i = 0; i < m.image.size(); ++i) difference += std::fabs(m.image[i] - reference_.image[i]);
        return difference <= m.image.size() * .10f;
    }
    void Set(Phase phase, uint64_t now, const char* why) {
        phase_ = phase;
        deadline_ = now + (phase == Phase::Reduce ? 8000 : comparison_ms);
        losses_ = 0;
        reason_ = why;
    }
    void Reject(uint64_t now, const char* why) {
        // A comparison that actually changed this view must not repeat the
        // visible disturbance until the picture or scene identity changes.
        rejected_view_ = phase_ != Phase::Confirm;
        Set(Phase::Cooldown, now, why);
        deadline_ = now + 30000;
    }
    static constexpr uint64_t comparison_ms = 18000;
    Measurement reference_;
    Phase phase_ = Phase::Observe;
    unsigned losses_ = 0;
    uint64_t deadline_ = 0;
    float candidate_ = 0;
    bool rejected_view_ = false;
    uint64_t unjittered_meshes_ = 0;
    bool move_unjittered_ = false;
    bool class_trial_ = false;
    const char* reason_ = "";
};

} // namespace na::referee
