#pragma once

#include "core/referee_trace.hpp"

namespace na::referee {

struct TrialSample {
    static constexpr unsigned subdivisions = 4;
    using Change = std::array<float, subdivisions * subdivisions>;
    uint64_t ordinal = 0;
    uint64_t configuration = 0;
    uint32_t layout = 0, width = 0, height = 0;
    std::array<Change, FlickerTransition::count> change{};
    std::array<float, FlickerTransition::count> image{};
    bool delivered = false, bypassed = false;
    bool comparable = true;
};

inline float TrialMagnitude(const TrialSample::Change& region)
{
    float total = 0;
    for (float value : region) total += std::fabs(value);
    return total / region.size();
}

inline int TrialDirection(const TrialSample::Change& a, const TrialSample::Change& b)
{
    float power_a = 0, power_b = 0, dot = 0;
    for (unsigned i = 0; i < a.size(); ++i) {
        power_a += a[i] * a[i];
        power_b += b[i] * b[i];
        dot += a[i] * b[i];
    }
    if (!(power_a > 0 && power_b > 0 && dot * dot >= .64f * power_a * power_b)) return 0;
    return dot > 0 ? 1 : -1;
}

inline bool MeasureTrialScreen(const float* before, const float* after, TrialSample& sample)
{
    using Region = FlickerTransition;
    if (!(EdgeCorrelation(before, after, kThumbW, kThumbH) >= .90f)) return false;
    constexpr unsigned n = TrialSample::subdivisions;
    constexpr unsigned cols = Region::columns * n, rows = Region::rows * n;
    static_assert(kThumbW % cols == 0 && kThumbH >= rows);
    sample.change = {};
    sample.image = {};
    for (uint32_t y = 0; y < kThumbH; ++y)
        for (uint32_t x = 0; x < kThumbW; ++x) {
            const uint32_t i = y * kThumbW + x;
            if (!std::isfinite(before[i]) || !std::isfinite(after[i])) return false;
            const unsigned cx = x * cols / kThumbW, cy = y * rows / kThumbH;
            const unsigned region = (cy / n) * Region::columns + cx / n;
            sample.change[region][(cy % n) * n + cx % n] += after[i] - before[i];
            sample.image[region] += (after[i] + before[i]) * .5f;
        }
    for (unsigned cy = 0; cy < rows; ++cy) {
        const unsigned y0 = (cy * kThumbH + rows - 1) / rows;
        const unsigned y1 = ((cy + 1) * kThumbH + rows - 1) / rows;
        const float normalize = 1.f / ((kThumbW / cols) * (y1 - y0));
        for (unsigned cx = 0; cx < cols; ++cx) {
            const unsigned region = (cy / n) * Region::columns + cx / n;
            sample.change[region][(cy % n) * n + cx % n] *= normalize;
        }
    }
    for (auto& value : sample.image) value *= float(Region::count) / kThumbFloats;
    return std::count_if(sample.change.begin(), sample.change.end(), [](const auto& region) { return TrialMagnitude(region) < .055f; }) >=
        Region::count * 3 / 4;
}

// VARIANT: referee/controlled-route-trial
// API adapters provide a proved candidate and the ability to restore the
// incumbent. Three runs share a screen region and a registered scene.
class RouteTrial {
public:
    enum class Phase : uint8_t { Idle, Baseline, Candidate, ConfirmBaseline, Kept, Rejected };
    static constexpr unsigned samples = 4;
    static constexpr uint64_t warmup_frames = 16, maximum_ms = 18000;

    void Start(uint64_t id, uint64_t frame, uint64_t now)
    {
        const auto delay = retry_delay_;
        *this = {};
        retry_delay_ = delay;
        now_ = now;
        id_ = id;
        expires_ = now + maximum_ms;
        Next(Phase::Baseline, frame);
    }
    Phase Current() const { return phase_; }
    const char* Name() const
    {
        constexpr const char* names[] = {"idle", "baseline", "replacement", "baseline_recheck", "kept", "rejected"};
        return names[static_cast<unsigned>(phase_)];
    }
    bool Active() const { return phase_ >= Phase::Baseline && phase_ <= Phase::ConfirmBaseline; }
    bool UsesCandidate() const { return phase_ == Phase::Candidate || phase_ == Phase::Kept; }
    bool CanStart(uint64_t now) const
    {
        return phase_ == Phase::Idle || (phase_ == Phase::Rejected && retry_at_ && now >= retry_at_);
    }
    static bool WatchTicket(uint64_t ticket)
    {
        const auto phase = static_cast<Phase>(ticket & 7u);
        return phase == Phase::Kept || phase == Phase::Rejected;
    }
    uint64_t Ticket(uint64_t frame) const
    {
        const bool watch = phase_ == Phase::Kept || (phase_ == Phase::Rejected && !retry_at_ && baseline_);
        return (Active() || watch) && frame >= measured_from_ ? (id_ << 3) | static_cast<uint8_t>(phase_) : 0;
    }
    void Tick(uint64_t now, bool available)
    {
        now_ = now;
        if (UsesCandidate() && !available) Retry("replacement lost its screen route");
        if (Active() && now >= expires_) Retry("timed out without comparable presented evidence");
    }
    void Add(uint64_t ticket, const TrialSample& sample, uint64_t next_frame)
    {
        if (!ticket || ticket != Ticket(sample.ordinal) || sample.ordinal <= last_ordinal_) return;
        last_ordinal_ = sample.ordinal;
        if (baseline_ && !SameSetup(anchor_, sample)) {
            Retry("the display layout or evaluation settings changed");
            return;
        }
        if (phase_ == Phase::Rejected) return;
        if (UsesCandidate() && sample.bypassed) {
            Reject("replacement lost visible delivery");
            return;
        }
        if (!sample.comparable || (UsesCandidate() && !sample.delivered)) {
            watch_same_ = watch_opposite_ = 0;
            return;
        }
        const auto finite = [](float v) { return std::isfinite(v); };
        if (!std::all_of(sample.change.begin(), sample.change.end(), [&](const auto& region) {
                return std::all_of(region.begin(), region.end(), finite);
            }) ||
            !std::all_of(sample.image.begin(), sample.image.end(), finite)) return;
        if (phase_ != Phase::Baseline && !SameView(anchor_, sample)) {
            watch_same_ = watch_opposite_ = 0;
            return;
        }
        if (phase_ == Phase::Kept) {
            // Keep ordinary sampling cadence after a win. Two reversals in each
            // direction at the original severity reopen discovery. A single
            // highlight or a different view cannot displace a working route.
            if (now_ - watch_at_ > maximum_ms) watch_same_ = watch_opposite_ = 0;
            watch_at_ = now_;
            const auto& change = sample.change[region_];
            const bool strong = !ImprovesWith(TrialMagnitude(change), (std::min)(baseline_, confirmation_));
            const int direction = strong ? TrialDirection(anchor_.change[region_], change) : 0;
            watch_same_ = uint8_t((watch_same_ << 1) | (direction > 0));
            watch_opposite_ = uint8_t((watch_opposite_ << 1) | (direction < 0));
            if ((watch_same_ & (watch_same_ - 1)) && (watch_opposite_ & (watch_opposite_ - 1)))
                Retry("the measured flicker returned on the replacement");
            return;
        }
        window_[count_++] = sample;
        if (count_ < samples) return;
        if (phase_ == Phase::Baseline || phase_ == Phase::ConfirmBaseline) {
            float strongest = 0;
            const TrialSample* pattern = nullptr;
            if (phase_ == Phase::Baseline) {
                for (uint32_t i = 0; i < FlickerTransition::count; ++i) {
                    const TrialSample* reference = nullptr;
                    const float amplitude = Repeated(i, nullptr, reference);
                    if (amplitude > strongest) {
                        strongest = amplitude;
                        region_ = i;
                        pattern = reference;
                    }
                }
                if (pattern) anchor_ = *pattern;
            } else {
                strongest = Repeated(region_, &anchor_, pattern);
            }
            bool comparable = strongest >= .12f;
            for (unsigned i = 0; comparable && i < count_; ++i) comparable = SameView(anchor_, window_[i]);
            if (!comparable) {
                if (count_ == window_.size()) {
                    for (unsigned i = 1; i < count_; ++i) window_[i - 1] = window_[i];
                    --count_;
                }
                return;
            }
            if (phase_ == Phase::Baseline) {
                baseline_ = strongest;
                Next(Phase::Candidate, next_frame);
            } else {
                confirmation_ = strongest;
                if (!Improves(confirmation_) || confirmation_ < baseline_ * .67f || confirmation_ > baseline_ * 1.5f) {
                    Reject("the original flicker did not return consistently");
                    return;
                }
                Next(Phase::Kept, next_frame);
                retry_delay_ = 8000;
                reason_ = "the replacement reduced flicker against both ordinary-route measurements";
            }
        } else if (phase_ == Phase::Candidate) {
            replacement_ = 0.f;
            for (unsigned i = 0; i < count_; ++i)
                for (const auto& change : window_[i].change) replacement_ = (std::max)(replacement_, TrialMagnitude(change));
            if (!Improves(baseline_)) { Reject("the replacement did not improve the presented picture"); return; }
            Next(Phase::ConfirmBaseline, next_frame);
        }
    }
    const char* Reason() const { return reason_; }
    float Baseline() const { return baseline_; }
    float Candidate() const { return replacement_; }
    float Confirmation() const { return confirmation_; }
    uint32_t Region() const { return region_; }

private:
    void Next(Phase phase, uint64_t frame)
    {
        phase_ = phase;
        count_ = 0;
        measured_from_ = frame + warmup_frames;
    }
    void Reject(const char* reason) { phase_ = Phase::Rejected; reason_ = reason; }
    void Retry(const char* reason)
    {
        Reject(reason);
        retry_at_ = now_ + retry_delay_;
        retry_delay_ = (std::min)(retry_delay_ * 2, uint64_t{60000});
    }
    static bool ImprovesWith(float replacement, float ordinary)
    {
        return replacement <= ordinary * .70f && ordinary - replacement >= .04f;
    }
    bool Improves(float value) const { return ImprovesWith(replacement_, value); }
    float Repeated(uint32_t region, const TrialSample* prior_pattern, const TrialSample*& pattern) const
    {
        // Two samples must move the same pixels one way, and two the
        // opposite way. Quiet intervening pairs need not be transitions.
        std::array<float, incumbent_window> magnitude{};
        const auto end = magnitude.begin() + count_;
        for (unsigned i = 0; i < count_; ++i) magnitude[i] = TrialMagnitude(window_[i].change[region]);
        pattern = &window_[std::max_element(magnitude.begin(), end) - magnitude.begin()];
        const auto& reference = (prior_pattern ? *prior_pattern : *pattern).change[region];
        std::array<float, 2> same{}, opposite{};
        for (unsigned i = 0; i < count_; ++i) {
            const auto& change = window_[i].change[region];
            const int direction = TrialDirection(reference, change);
            if (!direction) continue;
            auto& best = direction > 0 ? same : opposite;
            if (magnitude[i] > best[0]) { best[1] = best[0]; best[0] = magnitude[i]; }
            else best[1] = (std::max)(best[1], magnitude[i]);
        }
        return (std::min)(same[1], opposite[1]);
    }
    static bool SameSetup(const TrialSample& a, const TrialSample& b)
    {
        return a.width == b.width && a.height == b.height && a.layout == b.layout && a.configuration == b.configuration;
    }
    bool SameView(const TrialSample& a, const TrialSample& b) const
    {
        if (!SameSetup(a, b)) return false;
        if (UsesCandidate() && std::fabs(a.image[region_] - b.image[region_]) > .08f + .6f * baseline_)
            return false;
        float change = 0;
        unsigned count = 0;
        for (unsigned i = 0; i < FlickerTransition::count; ++i) {
            const int x = int(i % FlickerTransition::columns) - int(region_ % FlickerTransition::columns);
            const int y = int(i / FlickerTransition::columns) - int(region_ / FlickerTransition::columns);
            if (std::abs(x) <= 1 && std::abs(y) <= 1) continue;
            change += std::fabs(a.image[i] - b.image[i]);
            ++count;
        }
        return change <= float(count) * .10f;
    }
    Phase phase_ = Phase::Idle;
    uint64_t id_ = 0, expires_ = 0, measured_from_ = 0;
    uint64_t now_ = 0, retry_at_ = 0, retry_delay_ = 8000, last_ordinal_ = 0, watch_at_ = 0;
    uint8_t watch_same_ = 0, watch_opposite_ = 0;
    uint32_t region_ = 0;
    unsigned count_ = 0;
    float baseline_ = 0, replacement_ = 0, confirmation_ = 0;
    const char* reason_ = "";
    TrialSample anchor_;
    static constexpr unsigned incumbent_window = 8;
    std::array<TrialSample, incumbent_window> window_{};
};

} // namespace na::referee
