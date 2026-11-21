#pragma once

#include "core/referee_trial.hpp"

namespace na::referee {

constexpr uint8_t normal_jitter = 100, reduced_jitter = 50;

// Compare what actually rasterized with the requested phase on this frame.
// DX12 quantizes viewport offsets to 1/65536 of a pixel.
inline uint8_t VerifiedJitterAmount(uint8_t amount, float requested_x, float requested_y, float x, float y)
{
    constexpr float tolerance = 1.f / 32768.f;
    return (amount == normal_jitter || amount == reduced_jitter) &&
           std::fabs(requested_x - x) < tolerance && std::fabs(requested_y - y) < tolerance &&
           std::hypot(x, y) > tolerance ? amount : 0;
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
    };
    enum class Phase : uint8_t { Observe, Reduce, Recheck, Keep, Cooldown };
    Phase Current() const { return phase_; }
    const Measurement& Reference() const { return reference_; }
    bool NeedsReference() const { return phase_ == Phase::Reduce || phase_ == Phase::Recheck || phase_ == Phase::Keep; }
    uint8_t Amount() const { return phase_ == Phase::Reduce || phase_ == Phase::Keep ? reduced_jitter : normal_jitter; }
    const char* Reason() const { return reason_; }
    float Baseline() const { return std::fabs(reference_.shift); }
    float Candidate() const { return candidate_; }
    const char* Name() const {
        constexpr const char* names[]{"observe", "reduce", "recheck", "kept", "restored"};
        return names[static_cast<unsigned>(phase_)];
    }
    void Tick(uint64_t now)
    {
        if (NeedsReference() && now >= deadline_) Reject(now, "comparable raster and screen evidence expired");
        else if (phase_ == Phase::Cooldown && now >= deadline_) phase_ = Phase::Observe;
    }
    void Bypassed(uint64_t now)
    {
        if (Amount() == reduced_jitter) Reject(now, "the presented picture bypassed DLAA under reduced jitter");
    }
    void Observe(const Measurement& m, uint8_t amount, uint64_t now)
    {
        if (amount != Amount() || !std::isfinite(m.detail) || !std::isfinite(m.shift) || phase_ == Phase::Cooldown) return;
        const float wobble = std::fabs(m.shift);
        if (phase_ == Phase::Observe) {
            if (!m.locked || wobble < .12f || m.detail <= .02f) return;
            reference_ = m;
            Set(Phase::Reduce, now, wobble > std::fabs(m.reconstructed) + .05f
                ? "displayed edge moves with jitter after reconstruction" : "reconstructed edge moves with raster jitter");
            return;
        }
        if (!SameView(m)) return;
        if (Amount() == reduced_jitter) {
            if (m.detail < reference_.detail * .90f || !Improves(wobble, std::fabs(reference_.shift))) {
                if (++losses_ >= 2) Reject(now, "less jitter lost detail or failed to improve the displayed edge");
                return;
            }
            losses_ = 0;
            if (phase_ == Phase::Keep) { deadline_ = now + comparison_ms; return; }
            candidate_ = wobble;
            Set(Phase::Recheck, now, "displayed edge improved and retained detail");
            return;
        }
        if (m.locked && m.shift * reference_.shift > 0.f &&
            wobble >= .70f * std::fabs(reference_.shift) && wobble <= 1.45f * std::fabs(reference_.shift) &&
            Improves(candidate_, wobble)) {
            Set(Phase::Keep, now, "the same vibration returned at normal strength");
        } else {
            Reject(now, "the original vibration did not repeat");
        }
    }

private:
    static bool Improves(float candidate, float baseline) {
        return candidate <= baseline * .70f && baseline - candidate >= .035f;
    }
    bool SameView(const Measurement& m) const {
        if (m.at.x != reference_.at.x || m.at.y != reference_.at.y || m.layout != reference_.layout) return false;
        float difference = 0;
        for (unsigned i = 0; i < m.image.size(); ++i) difference += std::fabs(m.image[i] - reference_.image[i]);
        return difference <= m.image.size() * .10f;
    }
    void Set(Phase phase, uint64_t now, const char* why) {
        phase_ = phase; deadline_ = now + comparison_ms; losses_ = 0; reason_ = why;
    }
    void Reject(uint64_t now, const char* why) {
        Set(Phase::Cooldown, now, why);
        deadline_ = now + 30000;
    }
    static constexpr uint64_t comparison_ms = 18000;
    Measurement reference_;
    Phase phase_ = Phase::Observe;
    unsigned losses_ = 0;
    uint64_t deadline_ = 0;
    float candidate_ = 0;
    const char* reason_ = "";
};

} // namespace na::referee
