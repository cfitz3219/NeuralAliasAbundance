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
            if (gone_ >= 3 && back_ >= 2) {
                trial_pairs_ = trial_events_ = challenges_ = 0;
                Set(Phase::Trial, "surfaces drop to black on particular jitter samples: moving the meshes that kept the game's viewport onto the sample");
            }
            break;
        case Phase::Trial:
            if (!same_picture) break; // not evidence either way
            if (++trial_pairs_ <= kSettle) break;
            if (event) ++trial_events_;
            else if (Challenge(before) || Challenge(now)) ++challenges_;
            if (trial_events_ >= 3) {
                retry_at_ = pairs_ + kRetryAfter;
                Set(Phase::RuledOut, "the dropouts continued with the meshes moved: put back, another cause");
            } else if (!trial_events_ && ((challenges_ >= kChallenges && trial_pairs_ >= kTrialLeast) || trial_pairs_ >= kTrialMost)) {
                Set(Phase::Kept, challenges_ >= kChallenges ? "no dropout on the samples that produced them: kept"
                                                            : "no dropout for the whole trial: kept");
            } else if (trial_pairs_ >= kTrialMost) {
                retry_at_ = pairs_ + kRetryAfter;
                Set(Phase::RuledOut, "dropouts still seen during the trial: put back");
            }
            break;
        case Phase::Kept:
            if (event) ++recurred_; // reported, not acted on: the move was proved on this scene
            break;
        case Phase::RuledOut:
            if (pairs_ >= retry_at_) {
                gone_ = back_ = black_n_ = 0;
                first_event_ = 0;
                Set(Phase::Watch, "looking again");
            }
            break;
        }
        return was != phase_;
    }

private:
    void Set(Phase phase, const char* why) { phase_ = phase; why_ = why; }
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
    uint64_t pairs_ = 0, first_event_ = 0, retry_at_ = 0;
    unsigned gone_ = 0, back_ = 0, recurred_ = 0;
    unsigned trial_pairs_ = 0, trial_events_ = 0, challenges_ = 0;
    std::array<Sample, 8> black_{};
    unsigned black_n_ = 0;
};

} // namespace na::referee
