#pragma once

// VARIANT: referee/coverage-trial (coordinator 2026-11-27)
// The Referee moves scene draws left off the jitter onto the frame's sample, one class at a time, when its own
// measure says too many edges do not move with the jitter (Cort's 0f63301 Hearts of Iron IV run: the share of
// edges not following the jitter was 0.01-0.3 % in quiet stretches and 1-2.8 % exactly while 2 to 20 scene-colour
// draws a frame were left off the jitter, the "fuzzy, noisy, comes and goes" picture).
//
// What is measured, per Referee window (a few seconds of sampled frame pairs):
//   share   the Referee's unjittered-edge share: points whose picture did not shift with the jitter DLSS was told,
//           over the points where the jitter shows (follows + reverse + none), judged windows only;
//   black   the dropout watch's pairs where surfaces dropped to black or came back, and the cells black on both
//           frames of a pair (lasting black), per pair;
//   left    the host's scene-colour draws left off the jitter this window, by class.
// A trial: one class moved onto the sample, one window to settle, then two measured windows. It is kept when the
// share drops to at most 60 % of what it was and below the threshold (or by 0.3 points), with no new black
// (transition or lasting); otherwise the class goes back on the game's grid. Black during the trial takes it back
// at once. Full-screen passes are tried last, only while the measure still fails after every other class. A
// class put back is not tried again for five minutes (a zoom level or map mode may differ by then). Classes the
// host leaves still on purpose are never in the list: draws after the hand-over (not part of what DLSS resolves)
// and draws whose own signature was put back before (the host keeps those per draw signature).
// The jitter's strength is never changed (referee/no-strength-reduction).

#include <array>
#include <cstdint>
#include <cstdio>

namespace na::referee {

enum class LeftClass : uint8_t { OtherTarget, OwnViewport, RuleOff, StationarySource, Deferred, Other, FullScreen, Count };
constexpr unsigned kLeftClasses = unsigned(LeftClass::Count);
inline const char* LeftClassName(unsigned c)
{
    constexpr const char* names[kLeftClasses] = {"other_target", "own_viewport", "rule_off", "stationary_source", "deferred",
                                                 "other", "full_screen"};
    return c < kLeftClasses ? names[c] : "none";
}

struct CoverageWindow {
    bool judged = false;            // the jitter was measured (enough points, vectors not wrong, the picture matches)
    float share = 0;                // unjittered-edge share, 0..1
    float black_events = 0;         // pairs with a black onset or return, per measured pair
    float black_cells = 0;          // cells black on both frames of a pair, per measured pair
    std::array<uint64_t, kLeftClasses> left{}; // scene-colour draws left off the jitter by class, this window
};

class CoverageTrial {
public:
    static constexpr float kThreshold = .006f;   // above 0.6 %: quiet stretches measured 0.01-0.3 %, noisy ones 1-2.8 %
    static constexpr unsigned kHighWindows = 2;  // consecutive windows above it before a trial
    static constexpr unsigned kTrialWindows = 2; // measured windows after the settling one
    static constexpr uint64_t kRetryMs = 5 * 60 * 1000;
    enum class Phase : uint8_t { Watch, Settle, Trial };
    enum class Verdict : uint8_t { Unknown, Move, Still };

    Phase Current() const { return phase_; }
    const char* Name() const { return phase_ == Phase::Watch ? "watch" : phase_ == Phase::Settle ? "settle" : "trial"; }
    // Classes on the sample: kept ones, and the one on trial.
    uint32_t MoveMask() const
    {
        uint32_t m = 0;
        for (unsigned c = 0; c < kLeftClasses; ++c) if (verdict_[c] == Verdict::Move) m |= 1u << c;
        if (trial_ >= 0) m |= 1u << unsigned(trial_);
        return m;
    }
    int TrialClass() const { return trial_; }
    Verdict VerdictOf(unsigned c) const { return c < kLeftClasses ? verdict_[c] : Verdict::Unknown; }
    // The jitter trial saw the same edges vibrate twice: try at the next window even with one window above.
    void Kick() { kicked_ = true; }
    // Hints from earlier launches (core/referee_memory.hpp): classes that only did harm are tried after the rest.
    void SetAvoided(uint32_t mask) { avoided_ = mask; }
    const char* Why() const { return why_; }
    float Baseline() const { return base_share_; }
    float Measured() const { return measured_; }
    // The last decision: class, kept or put back; `decisions` counts them (the host promotes or demotes the draw
    // signatures it moved during the trial on each new decision).
    int LastClass() const { return last_class_; }
    bool LastKept() const { return last_kept_; }
    uint64_t Decisions() const { return decisions_; }

    // One Referee window. Returns true when the phase or the moved classes changed (the caller logs and publishes).
    bool Observe(const CoverageWindow& w, uint64_t now_ms)
    {
        for (unsigned c = 0; c < kLeftClasses; ++c)
            if (verdict_[c] == Verdict::Still && now_ms >= still_until_[c]) verdict_[c] = Verdict::Unknown;
        switch (phase_) {
        case Phase::Watch: {
            if (!w.judged) return false;
            const bool high = w.share >= kThreshold;
            if (!high) { high_ = 0; high_sum_ = 0; black_ev_sum_ = black_cells_sum_ = 0; kicked_ = false; return false; }
            ++high_;
            high_sum_ += w.share;
            black_ev_sum_ += w.black_events;
            black_cells_sum_ += w.black_cells;
            if (high_ < kHighWindows && !(kicked_ && high_ >= 1)) return false;
            const int c = Next(w);
            const float base = high_sum_ / float(high_);
            if (c < 0) {
                std::snprintf(why_buf_, sizeof why_buf_, "unjittered-edge share %.2f %% above %.2f %%, but no class of draws left off the "
                              "jitter is waiting for a trial (all tried, put back for now, or none left this window)",
                              base * 100.f, kThreshold * 100.f);
                why_ = why_buf_;
                high_ = 0; high_sum_ = 0; black_ev_sum_ = black_cells_sum_ = 0; kicked_ = false;
                const bool changed = !said_none_;
                said_none_ = true;
                return changed;
            }
            said_none_ = false;
            trial_ = c;
            base_share_ = base;
            base_black_events_ = black_ev_sum_ / float(high_);
            base_black_cells_ = black_cells_sum_ / float(high_);
            sum_share_ = sum_black_events_ = sum_black_cells_ = 0;
            measured_n_ = 0;
            measured_ = 0;
            kicked_ = false;
            high_ = 0; high_sum_ = 0; black_ev_sum_ = black_cells_sum_ = 0;
            std::snprintf(why_buf_, sizeof why_buf_, "unjittered-edge share %.2f %% above %.2f %% with %llu draws a window left off the jitter "
                          "as %s: trying them on the sample", base_share_ * 100.f, kThreshold * 100.f,
                          static_cast<unsigned long long>(w.left[unsigned(c)]), LeftClassName(unsigned(c)));
            why_ = why_buf_;
            phase_ = Phase::Settle;
            return true;
        }
        case Phase::Settle:
            if (Blacked(w)) return TakeBack(now_ms, w, "surfaces dropped to black or stayed black while settling");
            phase_ = Phase::Trial;
            return true;
        case Phase::Trial: {
            if (Blacked(w)) return TakeBack(now_ms, w, "surfaces dropped to black or stayed black with the class moved");
            if (!w.judged) return false;
            sum_share_ += w.share;
            sum_black_events_ += w.black_events;
            sum_black_cells_ += w.black_cells;
            if (++measured_n_ < kTrialWindows) return false;
            measured_ = sum_share_ / float(measured_n_);
            const bool improved = measured_ <= .6f * base_share_ && (measured_ < kThreshold || base_share_ - measured_ >= .003f);
            if (!improved) return TakeBack(now_ms, w, "the unjittered-edge share did not drop enough");
            const unsigned c = unsigned(trial_);
            verdict_[c] = Verdict::Move;
            std::snprintf(why_buf_, sizeof why_buf_, "%s kept on the sample: unjittered-edge share %.2f %% -> %.2f %%, no new black "
                          "(onsets/returns %.2f -> %.2f a pair, lasting black cells %.2f -> %.2f)", LeftClassName(c),
                          base_share_ * 100.f, measured_ * 100.f, base_black_events_, sum_black_events_ / float(measured_n_),
                          base_black_cells_, sum_black_cells_ / float(measured_n_));
            why_ = why_buf_;
            Decide(int(c), true);
            return true;
        }
        }
        return false;
    }

private:
    bool Blacked(const CoverageWindow& w) const
    {
        return w.black_events >= base_black_events_ + .5f || w.black_cells >= base_black_cells_ + 3.f;
    }
    bool TakeBack(uint64_t now_ms, const CoverageWindow& w, const char* what)
    {
        const unsigned c = unsigned(trial_);
        verdict_[c] = Verdict::Still;
        still_until_[c] = now_ms + kRetryMs;
        std::snprintf(why_buf_, sizeof why_buf_, "%s put back on the game's grid: %s (share %.2f %% -> %.2f %%, onsets/returns %.2f -> %.2f a "
                      "pair, lasting black cells %.2f -> %.2f)", LeftClassName(c), what, base_share_ * 100.f,
                      measured_n_ ? sum_share_ / float(measured_n_) * 100.f : w.share * 100.f, base_black_events_, w.black_events,
                      base_black_cells_, w.black_cells);
        why_ = why_buf_;
        measured_ = measured_n_ ? sum_share_ / float(measured_n_) : w.share;
        Decide(int(c), false);
        return true;
    }
    void Decide(int c, bool kept)
    {
        last_class_ = c;
        last_kept_ = kept;
        ++decisions_;
        trial_ = -1;
        phase_ = Phase::Watch;
    }
    int Next(const CoverageWindow& w) const
    {
        const auto ready = [&](unsigned c) { return verdict_[c] == Verdict::Unknown && w.left[c] > 0; };
        for (int pass = 0; pass < 2; ++pass)
            for (unsigned c = 0; c < kLeftClasses; ++c) {
                if (c == unsigned(LeftClass::FullScreen)) continue;
                const bool avoided = (avoided_ >> c) & 1u;
                if ((pass == 0) == avoided) continue;
                if (ready(c)) return int(c);
            }
        // Full-screen passes last, only when nothing else is waiting.
        return ready(unsigned(LeftClass::FullScreen)) ? int(LeftClass::FullScreen) : -1;
    }

    Phase phase_ = Phase::Watch;
    std::array<Verdict, kLeftClasses> verdict_{};
    std::array<uint64_t, kLeftClasses> still_until_{};
    int trial_ = -1, last_class_ = -1;
    bool last_kept_ = false, kicked_ = false, said_none_ = false;
    uint64_t decisions_ = 0;
    uint32_t avoided_ = 0;
    unsigned high_ = 0, measured_n_ = 0;
    float high_sum_ = 0, black_ev_sum_ = 0, black_cells_sum_ = 0;
    float base_share_ = 0, base_black_events_ = 0, base_black_cells_ = 0;
    float sum_share_ = 0, sum_black_events_ = 0, sum_black_cells_ = 0, measured_ = 0;
    const char* why_ = "";
    char why_buf_[320] = "";
};

} // namespace na::referee
