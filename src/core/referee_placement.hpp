#pragma once

#include "core/referee_trial.hpp"

namespace na::referee {

// Adapters certify a runnable reader and identify its scene lifetime. Color
// identifies the logical scene when the adapter has proved rotating owners.
struct PlacementScope {
    uint64_t color = 0, depth = 0, generation = 0, selection = 0, reader = 0, peer_color = 0;
    explicit operator bool() const { return color && depth && generation; }
    bool operator==(const PlacementScope& other) const {
        return color == other.color && depth == other.depth && generation == other.generation &&
               selection == other.selection && reader == other.reader && peer_color == other.peer_color;
    }
    bool OwnsPicture(const shadow::Context& c) const {
        if (!*this || !c.color || c.depth != depth) return false;
        // Native11's ordinary scene-stream generation and the verified
        // lighting-pair generation are independent counters. Its two
        // texture incarnation IDs and depth incarnation identify the owners.
        if (peer_color) return c.placement.path == shadow::Path::Native11 &&
                               (c.color == color || c.color == peer_color);
        return (c.placement.path == shadow::Path::Inline || c.placement.path == shadow::Path::Between) &&
               c.color == color && c.generation == generation;
    }
};

class PlacementTrial {
public:
    using Scope = PlacementScope;
    using Phase = RouteTrial::Phase;

    explicit PlacementTrial(uint64_t domain = uint64_t{1} << 58) : domain_(domain) {}
    bool OwnsComparison() const { return trial_.Active() || trial_.UsesCandidate(); }
    bool UsesCandidate() const { return trial_.UsesCandidate(); }
    bool AllowsStageRecovery() const { return trial_.ResidualBaseline() > 0.f; }
    uint32_t RecoveryRegion() const { return trial_.ResidualRegion(); }
    float RecoveryBaseline() const { return trial_.ResidualBaseline(); }
    float RecoveryFloor() const { return RecoveryBaseline() * RouteTrial::residual_fraction; }
    const RouteTrial& Comparison() const { return trial_; }
    uint64_t Ticket(uint64_t frame) const { return OwnsComparison() ? trial_.Ticket(frame) : 0; }
    bool Matches(Scope scope) const { return OwnsComparison() && scope == scope_; }

    void Invalidate(uint64_t now)
    {
        trial_ = {};
        scope_ = {};
        next_attempt_ = now + 8000;
    }

    void ClearPresentation()
    {
        if (!scope_ && !observed_count_ && !damage_count_ && !next_attempt_) return;
        trial_ = {};
        scope_ = {};
        observed_count_ = 0;
        damage_count_ = next_damage_ = 0;
        next_attempt_ = 0;
        // Keep serial_ monotonic so outstanding GPU receipts cannot match a new trial.
    }

    // An independently screen-proved alternative can begin measuring the
    // ordinary picture immediately. Baseline evidence still decides whether
    // any replacement is tried.
    bool BeginVerified(Scope offer, uint64_t frame, uint64_t now)
    {
        if (!offer || OwnsComparison() || Damaged(offer) || now < next_attempt_ ||
            (offer == scope_ && !trial_.CanStart(now))) return false;
        if (!(offer == scope_)) trial_ = {};
        configuration_ = 0;
        incumbent_ = {};
        independently_verified_ = true;
        Start(offer, frame, now);
        return true;
    }

    // VARIANT: referee/proved-roles-kept (referee_trial.hpp). Admission
    // needs both rotating roles proved on the screen (`offer`) and the
    // parity signature recognized on the ordinary route (`parity_seen`,
    // referee_parity.hpp). Without the signature the timed trial
    // (BeginVerified) decides as before. A timed trial already running on
    // the same scope is upgraded. The incumbent's last comparable samples go
    // with the keep as its damage reference. The 8-second wait after an
    // invalidation is for timed trials. A keep that ended on lost delivery
    // waits for its retry; one that ended on proved damage is not retried
    // for the same scope.
    bool KeepProvedRoles(Scope offer, bool parity_seen, uint64_t frame, uint64_t now)
    {
        if (!offer || !parity_seen || Damaged(offer)) return false;
        if (OwnsComparison()) {
            if (!(offer == scope_)) return false;
            if (trial_.Structural()) return true;
        } else if (offer == scope_ && trial_.WasStructural() &&
                   (trial_.RuledOut() || !trial_.CanStart(now))) return false;
        trial_ = {};
        configuration_ = 0;
        incumbent_ = {};
        independently_verified_ = true;
        scope_ = offer;
        const auto fresh = [&](const TrialSample& s) { return s.observed_at && now >= s.observed_at && now - s.observed_at <= 20000; };
        std::array<const TrialSample*, RouteTrial::incumbent_samples> evidence{};
        unsigned count = 0;
        for (unsigned i = 0; i < observed_count_; ++i)
            if (fresh(observed_[i]) && offer.OwnsPicture(observed_[i].context)) evidence[count++] = &observed_[i];
        trial_.KeepProvedRoles(domain_ | ++serial_, frame + 1, now, evidence);
        return true;
    }
    bool KeptByStructure() const { return trial_.Structural(); }
    unsigned IncumbentSamples() const { return trial_.IncumbentSamples(); }
    // Comparable, delivered pictures of the ordinary route, retained as the
    // damage reference for a later structural keep.
    void ObserveIncumbent(const TrialSample& sample)
    {
        if (UsesCandidate() || sample.applied || !sample.comparable || !sample.delivered || sample.bypassed) return;
        if (observed_count_ && (observed_[0].width != sample.width || observed_[0].height != sample.height ||
                                observed_[0].layout != sample.layout || observed_[0].configuration != sample.configuration))
            observed_count_ = 0;
        if (observed_count_ == observed_.size()) {
            std::move(observed_.begin() + 1, observed_.end(), observed_.begin());
            --observed_count_;
        }
        observed_[observed_count_++] = sample;
    }

    bool Begin(Scope offer, const TrialSample& witness, uint64_t frame, uint64_t now)
    {
        if (!offer || OwnsComparison() || Damaged(offer) || now < next_attempt_ || !Fresh(witness, frame, now) || witness.broad_light ||
            !witness.comparable || !HasSymptom(witness)) return false;
        const bool same = offer == scope_ && witness.configuration == configuration_ &&
            witness.layout == layout_ && witness.width == width_ && witness.height == height_;
        if (same && !trial_.CanStart(now)) return false;
        if (!same) trial_ = {};
        Remember(witness);
        incumbent_ = witness.context.placement;
        independently_verified_ = false;
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
        if (!Matches(scope) || !Fresh(sample, frame, now) || (sample.broad_light && !independently_verified_)) return;
        if (!UsesCandidate() && sample.comparable && ticket == trial_.Ticket(sample.ordinal)) {
            if (incumbent_.path == shadow::Path::None) incumbent_ = sample.context.placement;
            else if (!(incumbent_ == sample.context.placement)) return;
        }
        trial_.Add(ticket, sample, frame + 1);
        if (trial_.WasStructural() && trial_.RuledOut() && !Damaged(scope)) {
            damaged_[next_damage_] = scope;
            next_damage_ = (next_damage_ + 1) % damaged_.size();
            if (damage_count_ < damaged_.size()) ++damage_count_;
        }
        if (!configuration_ && sample.comparable && ticket == trial_.Ticket(sample.ordinal)) Remember(sample);
        if (trial_.Current() == Phase::Rejected) next_attempt_ = now + 8000;
    }
    static bool ShowsFlicker(const TrialSample& sample)
    {
        return sample.comparable && sample.delivered && std::any_of(sample.change.begin(), sample.change.end(),
            [](const auto& region) { return TrialMagnitude(region) >= RouteTrial::minimum_flicker; });
    }

private:
    bool Damaged(const Scope& scope) const {
        return std::find(damaged_.begin(), damaged_.begin() + damage_count_, scope) != damaged_.begin() + damage_count_;
    }
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
    std::array<Scope, 8> damaged_{};
    size_t damage_count_ = 0, next_damage_ = 0;
    shadow::Placement incumbent_;
    uint64_t domain_, next_attempt_ = 0, serial_ = 0, configuration_ = 0;
    uint32_t layout_ = 0, width_ = 0, height_ = 0;
    bool independently_verified_ = false;
    std::array<TrialSample, RouteTrial::incumbent_samples> observed_{};
    unsigned observed_count_ = 0;
};

} // namespace na::referee
