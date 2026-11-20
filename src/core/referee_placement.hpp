#pragma once

#include "core/referee_trial.hpp"

namespace na::referee {

// Adapters certify a runnable reader and identify its scene lifetime. Color
// identifies the logical scene when the adapter has proved rotating owners.
class PlacementTrial {
public:
    struct Scope {
        uint64_t color = 0, depth = 0, generation = 0, selection = 0, reader = 0;
        explicit operator bool() const { return color && depth && generation; }
        bool operator==(const Scope& other) const {
            return color == other.color && depth == other.depth &&
                   generation == other.generation && selection == other.selection && reader == other.reader;
        }
    };
    using Phase = RouteTrial::Phase;

    explicit PlacementTrial(uint64_t domain = uint64_t{1} << 58) : domain_(domain) {}
    bool OwnsComparison() const { return trial_.Active() || trial_.UsesCandidate(); }
    bool UsesCandidate() const { return trial_.UsesCandidate(); }
    const RouteTrial& Comparison() const { return trial_; }
    uint64_t Ticket(uint64_t frame) const { return OwnsComparison() ? trial_.Ticket(frame) : 0; }
    bool Matches(Scope scope) const { return OwnsComparison() && scope == scope_; }

    void Invalidate(uint64_t now)
    {
        trial_ = {};
        scope_ = {};
        next_attempt_ = now + 8000;
    }

    // An independently screen-proved alternative can begin measuring the
    // ordinary picture immediately. Baseline evidence still decides whether
    // any replacement is tried.
    bool BeginVerified(Scope offer, uint64_t frame, uint64_t now)
    {
        if (!offer || OwnsComparison() || now < next_attempt_ ||
            (offer == scope_ && !trial_.CanStart(now))) return false;
        if (!(offer == scope_)) trial_ = {};
        configuration_ = 0;
        incumbent_ = {};
        Start(offer, frame, now);
        return true;
    }

    bool Begin(Scope offer, const TrialSample& witness, uint64_t frame, uint64_t now)
    {
        if (!offer || OwnsComparison() || now < next_attempt_ || !Fresh(witness, frame, now) ||
            !witness.comparable || !HasSymptom(witness)) return false;
        const bool same = offer == scope_ && witness.configuration == configuration_ &&
            witness.layout == layout_ && witness.width == width_ && witness.height == height_;
        if (same && !trial_.CanStart(now)) return false;
        if (!same) trial_ = {};
        Remember(witness);
        incumbent_ = witness.context.placement;
        Start(offer, frame, now);
        return true;
    }

    void Tick(uint64_t now, bool delivered_candidate)
    {
        if (!OwnsComparison()) return;
        trial_.Tick(now, delivered_candidate);
        if (trial_.Current() == Phase::Rejected) next_attempt_ = now + 8000;
    }

    void Add(Scope scope, uint64_t ticket, const TrialSample& sample, uint64_t frame, uint64_t now)
    {
        if (!Matches(scope) || !Fresh(sample, frame, now)) return;
        if (!UsesCandidate() && sample.comparable && ticket == trial_.Ticket(sample.ordinal)) {
            if (incumbent_.path == shadow::Path::None) incumbent_ = sample.context.placement;
            else if (!(incumbent_ == sample.context.placement)) return;
        }
        trial_.Add(ticket, sample, frame + 1);
        if (!configuration_ && sample.comparable && ticket == trial_.Ticket(sample.ordinal)) Remember(sample);
        if (trial_.Current() == Phase::Rejected) next_attempt_ = now + 8000;
    }
    static bool ShowsFlicker(const TrialSample& sample)
    {
        return sample.comparable && sample.delivered && std::any_of(sample.change.begin(), sample.change.end(),
            [](const auto& region) { return TrialMagnitude(region) >= RouteTrial::minimum_flicker; });
    }

private:
    void Start(Scope offer, uint64_t frame, uint64_t now)
    {
        scope_ = offer;
        trial_.Start(domain_ | ++serial_, frame + 1, now);
    }
    void Remember(const TrialSample& sample)
    {
        configuration_ = sample.configuration;
        layout_ = sample.layout;
        width_ = sample.width;
        height_ = sample.height;
    }
    static bool Fresh(const TrialSample& sample, uint64_t frame, uint64_t now)
    {
        return sample.ordinal && sample.ordinal <= frame && frame - sample.ordinal <= 240 &&
            sample.observed_at && now >= sample.observed_at && now - sample.observed_at <= 4000;
    }
    static bool HasSymptom(const TrialSample& sample)
    {
        if (sample.tiles.Mixed()) return true;
        return ShowsFlicker(sample);
    }

    RouteTrial trial_;
    Scope scope_;
    shadow::Placement incumbent_;
    uint64_t domain_, next_attempt_ = 0, serial_ = 0, configuration_ = 0;
    uint32_t layout_ = 0, width_ = 0, height_ = 0;
};

} // namespace na::referee
