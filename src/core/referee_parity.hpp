#pragma once
#include <array>
#include <cmath>
#include <cstdint>

namespace na::referee {

// VARIANT: referee/parity-role-swap
// Picture signature of a scene that keeps two identical targets and swaps
// their roles every frame while DLAA follows one physical target. DLAA then
// gets the lit picture on one frame and the unlit one on the next; the game's
// later pass adds the missing light on top. On screen the same area flips
// brightness with frame parity, in the direction opposite to DLAA's input.
// Shadow of War 18dea9e: 61 of 61 traced flashes, odd frames brighter on
// screen, input about 0.3 stops the other way, screen about 0.6 stops.
// Saved 07a2004 captures: ordinary route 7.7 % whole-screen parity swing,
// role-following route under 0.4 %.
//
// Observe each traced adjacent pair (input / DLAA output / screen stops in
// the region that changed most on screen). Recognized after `needed`
// consecutive pairs agree on the parity phase with input opposing screen.
// Adapters answer it by following the role (referee_trial.hpp,
// referee/proved-roles-kept); a recognized signature that keeps growing on
// the kept route means the role route did not remove it.
class ParityRoleSwap {
public:
    static constexpr unsigned needed = 6;
    static constexpr float visible = .12f, upstream = .05f;

    // True on the pair that first recognizes the signature since the last
    // Reset() or since it lapsed.
    bool Observe(uint64_t first, uint64_t second, const std::array<float, 3>& stops)
    {
        if (second <= first || second - first != 1 || second <= last_pair_ ||
            !std::isfinite(stops[0]) || !std::isfinite(stops[2])) return false;
        last_pair_ = second;
        if (std::fabs(stops[2]) < visible) return false;
        const int parity = (second & 1u) ? 1 : -1;
        const int phase = (stops[2] > 0.f ? 1 : -1) * parity;
        const bool opposed = std::fabs(stops[0]) >= upstream && stops[0] * stops[2] < 0.f;
        if (recognized_phase_) {
            if (opposed && phase == recognized_phase_) lapsed_ = 0;
            else if (++lapsed_ >= needed) recognized_phase_ = 0;
        }
        if (!opposed || (phase_ && phase != phase_)) {
            ++contradicted_;
            run_ = 0;
            parity_counts_ = {};
            phase_ = opposed ? phase : 0;
            if (!opposed) return false;
        }
        phase_ = phase;
        ++matched_;
        // Sparse measurements can land on odd frames only. Multiple rises
        // then say nothing about the return transition on even frames.
        auto& count = parity_counts_[second & 1u];
        if (count < 2) ++count;
        if (run_ < needed) ++run_;
        if (run_ >= needed && parity_counts_[0] >= 2 && parity_counts_[1] >= 2 && recognized_phase_ != phase) {
            recognized_phase_ = phase;
            lapsed_ = 0;
            ++recognitions_;
            return true;
        }
        return false;
    }
    void Reset()
    {
        const auto matched = matched_, contradicted = contradicted_, recognitions = recognitions_;
        *this = {};
        matched_ = matched;
        contradicted_ = contradicted;
        recognitions_ = recognitions;
    }
    bool Recognized() const { return recognized_phase_ != 0; }
    // +1: odd frames brighter on screen; -1: even frames brighter.
    int Phase() const { return recognized_phase_ ? recognized_phase_ : phase_; }
    uint64_t Matched() const { return matched_; }
    uint64_t Contradicted() const { return contradicted_; }
    uint64_t Recognitions() const { return recognitions_; }

private:
    int phase_ = 0;
    unsigned run_ = 0, lapsed_ = 0;
    std::array<unsigned, 2> parity_counts_{};
    uint64_t last_pair_ = 0;
    int recognized_phase_ = 0;
    uint64_t matched_ = 0, contradicted_ = 0, recognitions_ = 0;
};

} // namespace na::referee
