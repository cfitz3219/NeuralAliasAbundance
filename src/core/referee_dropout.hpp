#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace na::referee {

// VARIANT: referee/sample-locked-dropout
// A surface that goes exactly black on some raster samples and comes back on others, while the
// picture is otherwise the same, is geometry that our jitter left on a different raster grid from
// a layer it tests itself against (Three Kingdoms terrain, 2026-11-21; WWII depth prepass before
// it). The Referee sees it in the DLAA input, asks the host to move the meshes that kept the
// game's own viewport onto the frame's full sample, and keeps that only if the dropouts stop on
// the same samples that produced them. Jitter strength is never reduced for this.

// One pair of consecutive evaluations, counted over the raster field's cells. A cell counts when
// part of its footprint is exactly black (red, green and blue all zero) on one side of the pair and
// none of it on the other. On the capture that found this (p32176, frames 1918..1920) the black
// frame gives 6 such cells of 12,384 and its neighbours none; a weaker black frame gives 2.
struct DropoutPair {
    unsigned gone = 0, back = 0, cells = 0;
    unsigned black = 0; // cells black on BOTH frames: persistent black, which no onset or return shows (Astra, 2026-11-26)
    bool Any() const { return gone || back; }
};
constexpr float dropout_share = .05f; // three of the 49 places of a footprint
// `values` floats per cell; the share of the footprint that is exactly black before and now.
inline DropoutPair CountDropouts(const float* field, unsigned cells, unsigned values, unsigned before_at, unsigned now_at)
{
    DropoutPair pair;
    pair.cells = cells;
    for (unsigned i = 0; i < cells; ++i, field += values) {
        const float before = field[before_at], now = field[now_at];
        if (!(std::isfinite(before) && std::isfinite(now))) continue;
        if (before == 0.f && now >= dropout_share) ++pair.gone;
        else if (now == 0.f && before >= dropout_share) ++pair.back;
        else if (before >= dropout_share && now >= dropout_share) ++pair.black;
    }
    return pair;
}

class DropoutWatch {
public:
    enum class Phase : uint8_t { Watch, Trial, Kept, RuledOut };
    struct Sample { float x = 0, y = 0; };
    static constexpr unsigned kWindow = 96;      // pairs a symptom must show itself within
    static constexpr unsigned kSettle = 4;       // pairs before a change of policy is in the picture
    static constexpr unsigned kChallenges = 4;   // pairs on a sample that dropped out before
    static constexpr unsigned kTrialLeast = 24, kTrialMost = 600;
    static constexpr unsigned kRetryAfter = 4000; // pairs before a ruled-out scene is looked at again
    static constexpr float kSameSample = .08f;   // pixels

    Phase Current() const { return phase_; }
    // What the host is asked to do: meshes that kept the game's viewport take the frame's sample.
    bool MoveStillMeshes() const { return phase_ == Phase::Trial || phase_ == Phase::Kept; }
    const char* Why() const { return why_; }
    unsigned Onsets() const { return gone_; }
    unsigned Returns() const { return back_; }
    unsigned Challenges() const { return challenges_; }
    unsigned TrialPairs() const { return trial_pairs_; }
    unsigned TrialEvents() const { return trial_events_; }
    unsigned Recurred() const { return recurred_; }
    unsigned BlackSamples() const { return black_n_; }
    Sample BlackSample(unsigned i) const { return black_[i]; }

    // One same-scene pair. `before` and `now` are the raster samples of its two frames in pixels.
    // same_picture: the view did not change between them (a cut or a fade is not a dropout).
    // Returns true when the phase changed (the caller logs it and republishes the policy).
    bool Observe(const DropoutPair& pair, Sample before, Sample now, bool same_picture)
    {
        ++pairs_;
        const Phase was = phase_;
        // Most of the picture turning black at once is the game's doing.
        const bool event = same_picture && pair.Any() && (pair.gone + pair.back) * 2 <= pair.cells;
        switch (phase_) {
        case Phase::Watch:
            Forget();
            if (event) Note(pair, before, now);
            if (same_picture) { watch_black_ = watch_black_ * .9f + .1f * float(pair.black); }
            if (gone_ >= 3 && back_ >= 2) {
                trial_started_ = pairs_;
                trial_pairs_ = trial_events_ = challenges_ = 0;
                trial_black_sum_ = 0; trial_black_n_ = 0;
                baseline_black_ = watch_black_;
                Set(Phase::Trial, "surfaces drop to black on particular jitter samples: moving the meshes that kept the game's viewport onto the sample");
            }
            break;
        case Phase::Trial:
            if (same_picture && ++trial_pairs_ > kSettle) {
                if (event) ++trial_events_;
                else if (Challenge(now)) ++challenges_;
                trial_black_sum_ += pair.black; ++trial_black_n_;
            }
            if (trial_black_n_ >= kSettle * 2 &&
                float(trial_black_sum_) / float(trial_black_n_) >= baseline_black_ + float(kBlackCells)) {
                RuleOut("surfaces turned black and stayed black after the meshes were moved: put back");
            } else if (trial_events_ >= 3) {
                RuleOut("the dropouts continued with the meshes moved: put back, another cause");
            } else if (!trial_events_ && challenges_ >= kChallenges && trial_pairs_ >= kTrialLeast) {
                Set(Phase::Kept, "no dropout on the samples that produced them: kept");
            } else if (pairs_ - trial_started_ >= kTrialMost) {
                RuleOut("the trial did not verify the fix on the samples that produced the dropouts: put back");
            }
            break;
        case Phase::Kept:
            if (event) {
                if (!recurred_at_ || pairs_ - recurred_at_ > kWindow) {
                    recurred_at_ = pairs_;
                    recurred_ = 0;
                }
                if (++recurred_ >= 3) RuleOut("dropouts returned on this scene: put back");
            }
            break;
        case Phase::RuledOut:
            if (pairs_ >= retry_at_) {
                gone_ = back_ = black_n_ = recurred_ = 0;
                first_event_ = recurred_at_ = 0;
                Set(Phase::Watch, "looking again");
            }
            break;
        }
        return was != phase_;
    }

private:
    void Set(Phase phase, const char* why) { phase_ = phase; why_ = why; }
    void RuleOut(const char* why)
    {
        retry_at_ = pairs_ + kRetryAfter;
        Set(Phase::RuledOut, why);
    }
    void Forget()
    {
        if (first_event_ && pairs_ - first_event_ > kWindow) {
            gone_ = back_ = black_n_ = 0;
            first_event_ = 0;
        }
    }
    void Note(const DropoutPair& pair, Sample before, Sample now)
    {
        if (!first_event_) first_event_ = pairs_;
        if (pair.gone) { ++gone_; Remember(now); }      // black on the frame that just arrived
        if (pair.back) { ++back_; Remember(before); }   // black on the frame before it
    }
    bool Near(Sample a, Sample b) const { return std::fabs(a.x - b.x) <= kSameSample && std::fabs(a.y - b.y) <= kSameSample; }
    void Remember(Sample s)
    {
        for (unsigned i = 0; i < black_n_; ++i)
            if (Near(black_[i], s)) return;
        if (black_n_ < black_.size()) black_[black_n_++] = s;
    }
    bool Challenge(Sample s) const
    {
        for (unsigned i = 0; i < black_n_; ++i)
            if (Near(black_[i], s)) return true;
        return false;
    }

    Phase phase_ = Phase::Watch;
    const char* why_ = "watching";
    uint64_t pairs_ = 0, first_event_ = 0, retry_at_ = 0, trial_started_ = 0, recurred_at_ = 0;
    unsigned gone_ = 0, back_ = 0, recurred_ = 0;
    unsigned trial_pairs_ = 0, trial_events_ = 0, challenges_ = 0;
    std::array<Sample, 8> black_{};
    unsigned black_n_ = 0;
    float watch_black_ = 0, baseline_black_ = 0;
    unsigned trial_black_sum_ = 0, trial_black_n_ = 0;
public:
    static constexpr unsigned kBlackCells = 3; // persistent black cells, on average, that count as a group turned black
};

// VARIANT: jitter/scene-overlays (default) | jitter/depth-tested-only
// The guard on the scene-overlay rule (core/temporal_jitter.hpp SceneOverlayTakesJitter). The overlays
// are on the frame's sample from the start. Should surfaces drop to black on particular samples while
// overlays were moved, the earlier rule is tried (the overlays back on the game's grid): kept when the
// dropouts stop, put back when they continue (then they were not the cause, and the dropout watch above
// tries its own move). Looked at again after kRetryAfter pairs.
class OverlayWatch {
public:
    enum class Phase : uint8_t { On, TrialOff, Off, RuledOut };
    static constexpr unsigned kWindow = DropoutWatch::kWindow, kSettle = DropoutWatch::kSettle;
    static constexpr unsigned kTrialLeast = DropoutWatch::kTrialLeast, kRetryAfter = DropoutWatch::kRetryAfter;

    Phase Current() const { return phase_; }
    bool OverlaysOn() const { return phase_ == Phase::On || phase_ == Phase::RuledOut; }
    // The dropout watch looks only while this one is not trying its own answer.
    bool Busy() const { return phase_ == Phase::TrialOff; }
    const char* Why() const { return why_; }
    const char* Name() const
    {
        constexpr const char* names[]{"on", "trial_off", "off", "ruled_out"};
        return names[static_cast<unsigned>(phase_)];
    }
    // One same-scene pair; `overlays_moved`: overlay meshes took the sample on these frames.
    // Returns true when the phase changed (the caller logs it and republishes the policy).
    bool Observe(const DropoutPair& pair, bool same_picture, bool overlays_moved)
    {
        ++pairs_;
        const Phase was = phase_;
        const bool event = same_picture && pair.Any() && (pair.gone + pair.back) * 2 <= pair.cells;
        switch (phase_) {
        case Phase::On:
            if (first_ && pairs_ - first_ > kWindow) gone_ = back_ = 0, first_ = 0;
            if (event && overlays_moved) {
                if (!first_) first_ = pairs_;
                gone_ += pair.gone ? 1 : 0;
                back_ += pair.back ? 1 : 0;
            }
            // Persistent black while overlays are moved: an exact reader displaced off its layer can stay black on
            // every sample, so no onset or return ever shows (Astra, 2026-11-26). Tried once a scene, briefly.
            if (same_picture && overlays_moved) { black_sum_ += pair.black; ++black_n_; }
            if (gone_ >= 3 && back_ >= 2) {
                trial_pairs_ = trial_events_ = 0;
                trial_black_sum_ = trial_black_n_ = 0;
                by_black_ = false;
                Set(Phase::TrialOff, "surfaces drop to black on particular samples with the overlays moved: trying them on the game's grid");
            } else if (!probed_ && black_n_ >= kTrialLeast && float(black_sum_) / float(black_n_) >= float(DropoutWatch::kBlackCells)) {
                probed_ = true;
                baseline_black_ = float(black_sum_) / float(black_n_);
                trial_pairs_ = trial_events_ = 0;
                trial_black_sum_ = trial_black_n_ = 0;
                by_black_ = true;
                Set(Phase::TrialOff, "parts of the picture stay black with the overlays moved: trying them on the game's grid");
            }
            break;
        case Phase::TrialOff:
            if (same_picture && ++trial_pairs_ > kSettle) {
                if (event) ++trial_events_;
                trial_black_sum_ += pair.black; ++trial_black_n_;
            }
            if (by_black_ && trial_black_n_ >= kTrialLeast) {
                const float off = float(trial_black_sum_) / float(trial_black_n_);
                retry_at_ = pairs_ + kRetryAfter;
                if (off <= .5f * baseline_black_) Set(Phase::Off, "the black parts came back with the overlays on the game's grid: the earlier rule is kept for this scene");
                else Set(Phase::RuledOut, "the black parts stayed without the move: overlays back on the sample (not the cause)");
            } else if (by_black_) {
            } else if (trial_events_ >= 3) {
                retry_at_ = pairs_ + kRetryAfter;
                Set(Phase::RuledOut, "the dropouts continued with the overlays on the game's grid: overlays back on the sample (not the cause)");
            } else if (!trial_events_ && trial_pairs_ >= kTrialLeast) {
                Set(Phase::Off, "no dropout with the overlays on the game's grid: the earlier rule is kept for this scene");
                retry_at_ = pairs_ + kRetryAfter;
            }
            break;
        case Phase::Off:
        case Phase::RuledOut:
            if (pairs_ >= retry_at_) {
                gone_ = back_ = 0;
                first_ = 0;
                Set(Phase::On, "looking again: overlays on the sample");
                probed_ = false; black_sum_ = black_n_ = 0;
            }
            break;
        }
        return was != phase_;
    }

private:
    void Set(Phase p, const char* why) { phase_ = p; why_ = why; }
    Phase phase_ = Phase::On;
    const char* why_ = "overlays on the frame's sample (scene content drawn without the depth test)";
    uint64_t pairs_ = 0, first_ = 0, retry_at_ = 0;
    unsigned gone_ = 0, back_ = 0, trial_pairs_ = 0, trial_events_ = 0;
    unsigned black_sum_ = 0, black_n_ = 0, trial_black_sum_ = 0, trial_black_n_ = 0;
    float baseline_black_ = 0;
    bool probed_ = false, by_black_ = false;
};

} // namespace na::referee
