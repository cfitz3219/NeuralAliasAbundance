#pragma once

#include "core/referee_trial.hpp"

namespace na::j2 {

// The Referee may try a separate-list handoff using the existing DX12 reader
// checks. It never asks a game recording to change its recorded commands.
class PlacementTrial {
public:
    using Path = referee::shadow::Path;
    using Context = referee::shadow::Context;
    using Sample = referee::TrialSample;
    using Phase = referee::RouteTrial::Phase;

    bool OwnsComparison() const { return trial_.Active() || trial_.UsesCandidate(); }
    bool UsesCandidate() const { return trial_.UsesCandidate(); }
    const referee::RouteTrial& Comparison() const { return trial_; }
    uint64_t Ticket(uint64_t frame) const { return OwnsComparison() ? trial_.Ticket(frame) : 0; }
    bool Matches(uint64_t color, uint64_t depth, uint64_t generation, uint64_t selection) const
    {
        return OwnsComparison() && scope_.color == color && scope_.depth == depth &&
            scope_.generation == generation && selection_ == selection;
    }
    void Invalidate(uint64_t now)
    {
        trial_ = {};
        next_attempt_ = now + 8000;
    }
    bool Begin(const Sample& witness, uint64_t frame, uint64_t now, uint64_t selection)
    {
        const auto& scene = witness.context;
        if (OwnsComparison() || now < next_attempt_ || !selection || !scene.color || !scene.depth ||
            !witness.comparable || !witness.tiles.Mixed() || witness.alternate.path != Path::Between ||
            scene.placement.path != Path::Inline ||
            !Fresh(witness, frame, now)) return false;
        if (trial_.RuledOut() && selection_ == selection && scope_.SameScene(scene) &&
            scope_.placement == scene.placement && scope_.generation == scene.generation &&
            scope_.trial_configuration == scene.trial_configuration) return false;
        scope_ = scene;
        selection_ = selection;
        // A separate namespace keeps reader receipts distinct from stage tools.
        trial_.Start((uint64_t{1} << 58) | ++serial_, frame + 1, now);
        return true;
    }
    void Tick(uint64_t now, bool delivered_candidate)
    {
        if (!OwnsComparison()) return;
        trial_.Tick(now, delivered_candidate);
        if (trial_.Current() == Phase::Rejected) next_attempt_ = now + 8000;
    }
    void Add(uint64_t ticket, const Sample& sample, uint64_t frame, uint64_t now)
    {
        if (!OwnsComparison() || !Fresh(sample, frame, now) || !scope_.SameScene(sample.context) ||
            sample.context.generation != scope_.generation) return;
        if (UsesCandidate() ? sample.context.placement.path != Path::Between :
            !(sample.context.placement == scope_.placement)) return;
        if (trial_.Current() == Phase::Baseline &&
            (sample.alternate.path != Path::Between || !sample.tiles.Mixed())) return;
        trial_.Add(ticket, sample, frame + 1);
        if (trial_.Current() == Phase::Rejected) next_attempt_ = now + 8000;
    }

private:
    static bool Fresh(const Sample& s, uint64_t frame, uint64_t now)
    {
        return s.ordinal && s.ordinal <= frame && frame - s.ordinal <= 240 &&
            s.observed_at && now >= s.observed_at && now - s.observed_at <= 4000;
    }
    referee::RouteTrial trial_;
    Context scope_;
    uint64_t selection_ = 0, next_attempt_ = 0, serial_ = 0;
};
} // namespace na::j2
