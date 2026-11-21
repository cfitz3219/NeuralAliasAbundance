#pragma once

#include "core/referee_trace.hpp"
#include "core/referee_selection.hpp"

namespace na::referee {

// Coordinates identify the sampled footprint even after a probe relocates.
struct RegionalTiles {
    struct Position { int x = 0, y = 0; };
    std::array<Position, kTiles> at{};
    std::array<float, kTiles> detail{}; // presented high-frequency energy at the fixed witnesses
    uint8_t input = 0, output = 0, confirmed_input = 0;

    void Set(TileVotes votes, uint32_t w, uint32_t h, const TileLocations& locations)
    {
        input = votes.input;
        output = votes.output;
        confirmed_input = votes.confirmed_input;
        for (unsigned i = 0; i < kTiles; ++i) {
            at[i] = {TileLeft(i, w, locations), TileTop(i, h, locations)};
            if (at[i].x < 0 || at[i].y < 0 || at[i].x + int(kTile) > int(w) || at[i].y + int(kTile) > int(h)) {
                input &= uint8_t(~(1u << i));
                output &= uint8_t(~(1u << i));
            }
        }
    }
    bool Mixed() const
    {
        if (!(input & (input - 1)) && !(input & confirmed_input)) return false;
        if (!(output & (output - 1)) || (input & output)) return false;
        for (unsigned i = 0; i < kTiles; ++i) if (((input | output) >> i) & 1u)
            for (unsigned j = 0; j < i; ++j) if (((input | output) >> j) & 1u)
                if (std::abs(at[i].x - at[j].x) < int(kTile) && std::abs(at[i].y - at[j].y) < int(kTile)) return false;
        return true;
    }
    enum class Match { Unknown, Original, Improved, Worse };
    Match Compare(const RegionalTiles& current) const
    {
        unsigned raw = 0, enhanced = 0, required = 0;
        bool missing = false;
        for (unsigned i = 0; i < kTiles; ++i) {
            const unsigned bit = 1u << i;
            if (!((input | output) & bit)) continue;
            if (at[i].x != current.at[i].x || at[i].y != current.at[i].y) { missing = true; continue; }
            if (output & bit) {
                if (current.input & bit) return Match::Worse;
                if (!(current.output & bit)) { missing = true; continue; }
                if (detail[i] > .02f && current.detail[i] < .75f * detail[i]) return Match::Worse;
            } else {
                ++required;
                raw += (current.input & bit) != 0;
                enhanced += (current.output & bit) != 0;
            }
        }
        if (missing) return Match::Unknown;
        if (required && raw == required && (required > 1 || (current.confirmed_input & input))) return Match::Original;
        if (required && enhanced == required && !current.input) return Match::Improved;
        return Match::Unknown;
    }
};

struct TrialSample {
    static constexpr unsigned subdivisions = FlickerTransition::subdivisions;
    using Change = std::array<float, subdivisions * subdivisions>;
    uint64_t ordinal = 0;
    uint64_t observed_at = 0; // monotonic time of the receipt, supplied by the live observer
    uint64_t configuration = 0;
    uint32_t layout = 0, width = 0, height = 0;
    std::array<Change, FlickerTransition::count> change{};
    std::array<float, FlickerTransition::count> image{};
    std::array<float, FlickerTransition::count> contrast{};
    bool delivered = false, bypassed = false;
    RegionalTiles tiles;
    shadow::Context context;
    shadow::Placement alternate;
    bool comparable = true;
    bool broad_light = false; // same rendered structure with a shared lighting shift
    float screen_structure = 0.f;
    unsigned quiet_regions = 0;
    FlickerTransition origin{};
    uint8_t tools = 0, applied = 0; // intersection of the two submitted recordings
    float motion_error = -1; // mean declared-motion patch error over textured screen points
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
    sample.broad_light = false;
    sample.quiet_regions = 0;
    sample.screen_structure = EdgeCorrelation(before, after, kThumbW, kThumbH);
    if (!(sample.screen_structure >= .90f)) return false;
    constexpr unsigned n = TrialSample::subdivisions;
    constexpr unsigned cols = Region::columns * n, rows = Region::rows * n;
    static_assert(kThumbW % cols == 0 && kThumbH >= rows);
    sample.change = {};
    sample.image = {};
    sample.contrast = {};
    for (uint32_t y = 0; y < kThumbH; ++y)
        for (uint32_t x = 0; x < kThumbW; ++x) {
            const uint32_t i = y * kThumbW + x;
            if (!std::isfinite(before[i]) || !std::isfinite(after[i])) return false;
            const unsigned cx = x * cols / kThumbW, cy = y * rows / kThumbH;
            const unsigned region = (cy / n) * Region::columns + cx / n;
            sample.change[region][(cy % n) * n + cx % n] += after[i] - before[i];
            sample.image[region] += (after[i] + before[i]) * .5f;
            if (x && (x - 1) * Region::columns / kThumbW == x * Region::columns / kThumbW)
                sample.contrast[region] += .5f * (std::fabs(after[i] - after[i - 1]) + std::fabs(before[i] - before[i - 1]));
            if (y && (y - 1) * Region::rows / kThumbH == y * Region::rows / kThumbH)
                sample.contrast[region] += .5f * (std::fabs(after[i] - after[i - kThumbW]) + std::fabs(before[i] - before[i - kThumbW]));
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
    for (auto& value : sample.contrast) value *= float(Region::count) / kThumbFloats;
    sample.quiet_regions = static_cast<unsigned>(std::count_if(sample.change.begin(), sample.change.end(),
        [](const auto& region) { return TrialMagnitude(region) < .055f; }));
    if (sample.quiet_regions >= Region::count * 3 / 4) return true;
    // A verified lighting reader may be worth comparing when a reflection
    // changes several regions at once. Require much stronger geometry and
    // two thirds of the view to agree after a common brightness shift. A
    // purely global flash has no local excess and does not qualify.
    if (sample.screen_structure < .98f) return false;
    std::array<float, Region::count> midpoints{};
    for (unsigned i = 0; i < Region::count; ++i) {
        for (const auto value : sample.change[i]) midpoints[i] += value;
        midpoints[i] /= sample.change[i].size();
    }
    auto middle = midpoints.begin() + midpoints.size() / 2;
    std::nth_element(midpoints.begin(), middle, midpoints.end());
    const float offset = *middle;
    unsigned matched = 0;
    float strongest = 0;
    for (auto region : sample.change) {
        for (auto& value : region) value -= offset;
        const float residual = TrialMagnitude(region);
        matched += residual < .055f;
        strongest = (std::max)(strongest, residual);
    }
    sample.broad_light = matched * 3 >= Region::count * 2 && strongest >= .12f;
    return sample.broad_light;
}

// Why a screen receipt did not advance a phase. The owner of each gate counts
// its own refusals, so a phase that runs out of time can name what blocked it.
enum class TrialGate : uint8_t {
    Experiment, // receipt carried another trial's ticket, or repeated a pair
    Stale,      // receipt was too old to describe the current picture
    Scope,      // receipt showed a different scene or route than the one on trial
    Incomparable, // the two frames of the pair could not be compared
    Undelivered,  // the replacement was not confirmed on the screen
    Invalid,    // non-finite measurement
    View,       // the camera or picture moved away from the measured view
    Region,     // the disputed output area could not be verified
    Symptom,    // the full window did not show the recurring symptom
    Motion,     // no usable motion measurement for a motion control
    Count
};
inline const char* TrialGateName(TrialGate gate)
{
    constexpr const char* names[] = {"other_experiment", "stale", "other_scope", "not_comparable", "not_delivered",
                                     "invalid", "view_changed", "region_unverified", "symptom_absent", "no_motion"};
    return names[static_cast<unsigned>(gate)];
}
struct TrialEvidence {
    uint32_t receipts = 0; // screen receipts offered to the phase
    std::array<uint32_t, static_cast<unsigned>(TrialGate::Count)> refused{};
    uint32_t Used() const
    {
        uint32_t total = 0;
        for (auto n : refused) total += n;
        return receipts - (std::min)(receipts, total);
    }
    // The gate that refused the most receipts; Count when none did.
    TrialGate Decisive() const
    {
        const auto most = std::max_element(refused.begin(), refused.end());
        return *most ? static_cast<TrialGate>(most - refused.begin()) : TrialGate::Count;
    }
};
// What a finished comparison established. Only Harm excludes the candidate.
enum class TrialOutcome : uint8_t { Pending, Improved, Harm, Insufficient, Unavailable, Slower };
inline const char* TrialOutcomeName(TrialOutcome outcome)
{
    constexpr const char* names[] = {"pending", "improved", "harm", "insufficient_evidence", "unavailable", "slower"};
    return names[static_cast<unsigned>(outcome)];
}

// VARIANT: referee/controlled-route-trial
// API adapters provide a proved candidate and the ability to restore the
// incumbent. Three runs share a screen region and a registered scene.
class RouteTrial {
public:
    enum class Phase : uint8_t { Idle, Baseline, Candidate, ConfirmBaseline, Kept, Rejected };
    static constexpr unsigned samples = 4;
    static constexpr float minimum_flicker = .12f;
    static constexpr float residual_fraction = .10f;
    static constexpr uint64_t warmup_frames = 16, maximum_ms = 18000;
    // VARIANT: referee/inconclusive-retry
    // A comparison that ended without a verdict (the original symptom did not
    // return on the recheck, or one unrelated change spoiled the window) says
    // nothing against the candidate. It may run again on fresh evidence after
    // a growing wait, at most this many times for one scope and setup.
    // Demonstrated harm (RuledOut) is never reopened here.
    static constexpr unsigned inconclusive_limit = 3;
    // VARIANT: referee/frame-time-acceptance
    // A replacement that cures the symptom must not pay for it in frame time.
    // Each phase measures the real time per game frame between its own
    // accepted receipts (median of the intervals, so one load spike or menu
    // hitch cannot decide). The replacement is compared with the SLOWER of the
    // two ordinary-route measurements, so a game that slowed down by itself
    // during the trial does not condemn a good route. Without three intervals
    // in every phase there is no timing verdict and the picture decides alone.
    static constexpr float frame_cost_ratio = 1.10f, frame_cost_ms = .5f;
    static constexpr unsigned frame_intervals = 3;

    void Start(uint64_t id, uint64_t frame, uint64_t now)
    {
        const auto delay = retry_delay_;
        const auto inconclusive = inconclusive_;
        *this = {};
        retry_delay_ = delay;
        inconclusive_ = inconclusive;
        now_ = now;
        id_ = id;
        Next(Phase::Baseline, frame);
    }
    void StartAt(uint64_t id, uint64_t frame, uint64_t now, const TrialSample& witness)
    {
        Start(id, frame, now);
        anchor_ = witness;
        region_ = witness.origin.region;
        targeted_ = true;
        if (witness.origin.recovery_floor > 0)
            flicker_floor_ = std::clamp(witness.origin.recovery_floor, FlickerTransition::scoped_minimum, minimum_flicker);
    }
    // VARIANT: referee/proved-roles-kept
    // Two same-size scene targets that exchange roles every frame make the
    // physical-target route evaluate a different stage on alternate frames:
    // DLAA gets the lit picture on one frame and the unlit one on the next,
    // and the game's later pass adds the missing light on top. The screen
    // then flips brightness with frame parity, opposite to the DLAA input
    // (Shadow of War 18dea9e: 73 of 73 traced flashes).
    // Admission (PlacementTrial::KeepProvedRoles): both roles proved on the
    // screen AND the Referee recognized that parity signature on the
    // ordinary route (referee_parity.hpp). Then the ordinary route is the
    // cause, not a fair incumbent: no new baseline is taken and no timer
    // restores it. The incumbent's already observed regional evidence is
    // kept, and the role route is rolled back only on positive damage in a
    // matched view: lost detail, or a recurring flash in an area the
    // incumbent showed quiet. Sustained loss of delivery or proof of bypass
    // also ends it; an isolated missed submission does not.
    static constexpr unsigned incumbent_samples = 4, missed_limit = 30;
    void KeepProvedRoles(uint64_t id, uint64_t frame, uint64_t now,
                         const std::array<const TrialSample*, incumbent_samples>& incumbent)
    {
        Start(id, frame, now);
        structural_ = true;
        incumbent_count_ = 0;
        for (const auto* source : incumbent) {
            if (!source) break;
            auto& ref = incumbent_[incumbent_count_++];
            ref.width = source->width; ref.height = source->height;
            ref.configuration = source->configuration; ref.layout = source->layout;
            ref.image = source->image;
            ref.contrast = source->contrast;
            for (unsigned r = 0; r < FlickerTransition::count; ++r)
                ref.change[r] = TrialMagnitude(source->change[r]);
        }
        if (incumbent_count_) anchor_ = *incumbent[incumbent_count_ - 1];
        Next(Phase::Kept, frame);
        reason_ = incumbent_count_ ? "both rotating scene roles reach the screen and the parity flicker was recognized; the role-following route is kept"
                                   : "both rotating scene roles reach the screen and the parity flicker was recognized; kept without incumbent regional evidence";
    }
    bool Structural() const { return structural_ && phase_ == Phase::Kept; }
    bool WasStructural() const { return structural_; }
    unsigned IncumbentSamples() const { return structural_ ? incumbent_count_ : 0; }
    bool RuledOut() const { return phase_ == Phase::Rejected && ruled_out_; }
    bool Regional() const { return regional_; }
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
        return phase == Phase::Idle || phase == Phase::Kept || phase == Phase::Rejected;
    }
    uint64_t Ticket(uint64_t frame) const
    {
        const bool watch = phase_ == Phase::Kept || (phase_ == Phase::Rejected && !retry_at_ && (baseline_ || regional_));
        return (Active() || watch) && frame >= measured_from_ ? (id_ << 3) | static_cast<uint8_t>(phase_) : 0;
    }
    void Tick(uint64_t now, bool available)
    {
        now_ = now;
        if (structural_) {
            // The adapter falls back per frame on its own. Only a sustained
            // loss ends the keep; one missed submission does not.
            if (!Structural()) return;
            missed_ = available ? 0 : missed_ + 1;
            if (missed_ >= missed_limit) Retry("the role-following route stopped delivering", TrialOutcome::Unavailable);
            return;
        }
        if (UsesCandidate() && !available) Retry("replacement lost its screen route", TrialOutcome::Unavailable);
        if (phase_ == Phase::Kept && regional_ && now - regional_seen_ >= maximum_ms) Retry("regional screen evidence expired");
        if (Active() && now >= expires_) Retry(TimeoutReason());
    }
    // A gate outside this class (freshness, scene ownership, motion) refused a
    // receipt that was meant for the running phase.
    void Note(TrialGate gate)
    {
        if (!Active()) return;
        ++live_.receipts;
        ++live_.refused[static_cast<unsigned>(gate)];
    }
    // Evidence of the phase that most recently ended (or of the running one).
    const TrialEvidence& Evidence() const { return Active() ? live_ : closed_; }
    const char* EvidencePhase() const
    {
        constexpr const char* names[] = {"idle", "baseline", "replacement", "baseline_recheck", "kept", "rejected"};
        return names[static_cast<unsigned>(Active() ? phase_ : closed_phase_)];
    }
    TrialOutcome Outcome() const { return outcome_; }
    unsigned InconclusiveAttempts() const { return inconclusive_; }
    void Add(uint64_t ticket, const TrialSample& sample, uint64_t next_frame)
    {
        if (!ticket) return;
        if (ticket != Ticket(sample.ordinal) || sample.ordinal <= last_ordinal_) { Note(TrialGate::Experiment); return; }
        if (Active()) ++live_.receipts;
        last_ordinal_ = sample.ordinal;
        if ((baseline_ || regional_) && !SameSetup(anchor_, sample)) {
            Retry("the display layout or evaluation settings changed");
            return;
        }
        if (phase_ == Phase::Rejected) return;
        if (UsesCandidate() && sample.bypassed) {
            if (structural_) {
                // The roles are proved again before the next keep.
                Retry("the role-following route lost visible delivery");
                return;
            }
            ruled_out_ = true;
            Reject("replacement lost visible delivery");
            return;
        }
        if (!sample.comparable || (UsesCandidate() && !sample.delivered && !regional_)) {
            Refuse(sample.comparable ? TrialGate::Undelivered : TrialGate::Incomparable);
            if (regional_ && Active()) count_ = lost_region_ = 0;
            if (Structural()) { count_ = 0; prior_detail_loss_ = 0; }
            watch_same_ = watch_opposite_ = 0;
            return;
        }
        const auto finite = [](float v) { return std::isfinite(v); };
        if (!std::all_of(sample.change.begin(), sample.change.end(), [&](const auto& region) {
                return std::all_of(region.begin(), region.end(), finite);
            }) ||
            !std::all_of(sample.image.begin(), sample.image.end(), finite)) { Refuse(TrialGate::Invalid); return; }
        if (structural_) {
            KeepStructural(sample);
            return;
        }
        if ((targeted_ || phase_ != Phase::Baseline) && !SameView(anchor_, sample)) {
            Refuse(TrialGate::View);
            if (regional_ && Active()) count_ = 0;
            watch_same_ = watch_opposite_ = 0;
            return;
        }
        if (regional_ && UsesCandidate()) {
            const auto match = anchor_.tiles.Compare(sample.tiles);
            if (match == RegionalTiles::Match::Worse) {
                Retry("the replacement degraded a verified output area");
                return;
            }
            if (match == RegionalTiles::Match::Original && phase_ == Phase::Candidate) {
                count_ = 0;
                if (++lost_region_ >= 2) {
                    ruled_out_ = true;
                    Reject("the original regional bypass persisted on the replacement");
                }
                return;
            }
            if (match != RegionalTiles::Match::Improved) {
                Refuse(TrialGate::Region);
                count_ = 0;
                if (phase_ == Phase::Kept) {
                    if (++lost_region_ >= 2) Retry("regional delivery could no longer be verified");
                } else lost_region_ = 0;
                return;
            }
            lost_region_ = 0;
            regional_seen_ = now_;
        }
        if (phase_ == Phase::Kept) {
            // Keep ordinary sampling cadence after a win. Two reversals in each
            // direction at the original severity reopen discovery. A single
            // highlight or a different view cannot displace a working route.
            if (now_ - watch_at_ > maximum_ms) { watch_same_ = watch_opposite_ = 0; count_ = 0; }
            if (spatial_only_) {
                watch_at_ = now_;
                window_[count_++] = sample;
                const TrialSample* pattern = nullptr;
                if (count_ >= samples && Repeated(region_, nullptr, pattern) >= minimum_flicker) {
                    Retry("flicker appeared in the region recovered by the replacement");
                } else if (count_ == window_.size()) {
                    std::move(window_.begin() + 1, window_.end(), window_.begin());
                    --count_;
                }
                return;
            }
            watch_at_ = now_;
            const auto& change = sample.change[region_];
            const bool strong = !ImprovesWith(TrialMagnitude(change), (std::min)(baseline_, confirmation_));
            const int direction = strong ? TrialDirection(anchor_.change[region_], change) : 0;
            watch_same_ = uint8_t((watch_same_ << 1) | (direction > 0));
            watch_opposite_ = uint8_t((watch_opposite_ << 1) | (direction < 0));
            if ((watch_same_ & (watch_same_ - 1)) && (watch_opposite_ & (watch_opposite_ - 1))) {
                ruled_out_ = targeted_;
                Retry("the measured flicker returned on the replacement");
            }
            return;
        }
        Time(sample);
        window_[count_++] = sample;
        if (count_ < samples) return;
        if (phase_ == Phase::Baseline || phase_ == Phase::ConfirmBaseline) {
            float strongest = 0;
            const TrialSample* pattern = nullptr;
            if (phase_ == Phase::Baseline) {
                for (uint32_t i = 0; i < FlickerTransition::count; ++i) {
                    if (targeted_ && i != region_) continue;
                    const TrialSample* reference = nullptr;
                    const float amplitude = Repeated(i, nullptr, reference);
                    if (!targeted_) {
                        auto& evidence = regions_[i];
                        evidence.baseline = amplitude;
                        if (amplitude >= minimum_flicker) evidence.pattern = reference->change[i];
                    }
                    if (amplitude > strongest) {
                        strongest = amplitude;
                        region_ = i;
                        pattern = reference;
                    }
                }
                if (pattern) anchor_ = *pattern;
            } else {
                strongest = Repeated(region_, &anchor_.change[region_], pattern);
            }
            if (phase_ == Phase::Baseline && !targeted_) {
                const auto& first = window_[0];
                bool mixed = first.width && first.height && first.tiles.Mixed();
                unsigned mixed_frames = 0;
                for (unsigned i = 0; i < count_; ++i) {
                    mixed_frames += window_[i].tiles.Mixed();
                    mixed = mixed && first.tiles.Compare(window_[i].tiles) == RegionalTiles::Match::Original &&
                            SameSetup(first, window_[i]);
                }
                if (mixed) {
                    if (strongest < minimum_flicker) {
                        anchor_ = first;
                        for (unsigned i = 0; i < kTiles; ++i) if (first.tiles.input & (1u << i)) {
                            const auto at = first.tiles.at[i];
                            region_ = (at.x + kTile / 2) * FlickerTransition::columns / first.width +
                                (at.y + kTile / 2) * FlickerTransition::rows / first.height * FlickerTransition::columns;
                            break;
                        }
                    }
                    anchor_.tiles = first.tiles;
                } else if (mixed_frames >= 2) strongest = 0;
                regional_ = mixed;
                spatial_only_ = mixed && strongest < minimum_flicker;
            }
            bool comparable = strongest >= flicker_floor_ || spatial_only_;
            for (unsigned i = 0; regional_ && comparable && i < count_; ++i)
                comparable = anchor_.tiles.Compare(window_[i].tiles) == RegionalTiles::Match::Original;
            for (unsigned i = 0; comparable && i < count_; ++i) comparable = SameView(anchor_, window_[i]);
            if (!comparable) {
                Refuse(TrialGate::Symptom);
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
                if (!spatial_only_ && (!Improves(confirmation_) || confirmation_ < baseline_ * .67f || confirmation_ > baseline_ * 1.5f)) {
                    Inconclusive("the original flicker did not return consistently");
                    return;
                }
                if (CostsFrameTime()) {
                    Inconclusive("the replacement cost too much frame time against both ordinary measurements",
                                 TrialOutcome::Slower);
                    return;
                }
                if (!targeted_ && !regional_) SelectResidualRegion();
                Next(Phase::Kept, next_frame);
                retry_delay_ = 8000;
                inconclusive_ = 0;
                outcome_ = TrialOutcome::Improved;
                regional_seen_ = now_;
                reason_ = regional_ ? "disputed tiles switched to reconstructed detail, clear controls held, and the original bypass returned on recheck" :
                    "the replacement reduced flicker against both ordinary-route measurements";
            }
        } else if (phase_ == Phase::Candidate) {
            replacement_ = 0.f;
            affected_replacement_ = 0.f;
            unsigned detail_losses = 0;
            for (unsigned i = 0; i < count_; ++i) {
                if (targeted_ && anchor_.contrast[region_] > .04f &&
                    window_[i].contrast[region_] < .75f * anchor_.contrast[region_]) ++detail_losses;
                for (const auto& change : window_[i].change) replacement_ = (std::max)(replacement_, TrialMagnitude(change));
                affected_replacement_ = (std::max)(affected_replacement_, TrialMagnitude(window_[i].change[region_]));
            }
            if (detail_losses >= 2) {
                ruled_out_ = true;
                Reject("the replacement lost visible detail in the affected area");
                return;
            }
            if (spatial_only_ && replacement_ >= minimum_flicker) {
                Inconclusive("new visible variation during the regional replacement");
                return;
            }
            if (!spatial_only_ && !Improves(baseline_)) {
                // A single unrelated flash is grounds to restore ordinary
                // rendering. It is insufficient to blacklist this tool.
                ruled_out_ = !targeted_;
                for (unsigned region = 0; targeted_ && !ruled_out_ && region < FlickerTransition::count; ++region) {
                    const TrialSample* pattern = nullptr;
                    const auto severity = Repeated(region, region == region_ ? &anchor_.change[region_] : nullptr, pattern);
                    ruled_out_ = !ImprovesWith(severity, baseline_);
                }
                if (!ruled_out_ && count_ < window_.size()) return;
                if (ruled_out_) Reject("the replacement did not improve the presented picture");
                else Inconclusive("an isolated change prevented a reliable verdict");
                return;
            }
            if (!targeted_ && !regional_) {
                for (uint32_t i = 0; i < regions_.size(); ++i) {
                    auto& evidence = regions_[i];
                    const TrialSample* pattern = nullptr;
                    const float recurring = Repeated(i, nullptr, pattern);
                    // Whole-screen peak alone can hide a smaller new flash
                    // elsewhere once the original strongest one is removed.
                    if (recurring >= minimum_flicker &&
                        recurring > evidence.baseline * 1.3f + minimum_flicker / 3.f) {
                        ruled_out_ = true;
                        Reject("the replacement introduced or worsened recurring flicker in another area");
                        return;
                    }
                    if (recurring >= FlickerTransition::scoped_minimum && evidence.baseline >= minimum_flicker)
                        evidence.replacement = Repeated(i, &evidence.pattern, pattern);
                }
            }
            Next(Phase::ConfirmBaseline, next_frame);
        }
    }
    const char* Reason() const { return reason_; }
    // Median milliseconds per game frame in the ordinary, replacement and
    // ordinary-recheck phases; negative where it could not be measured.
    float FrameMs(Phase phase) const
    {
        const unsigned i = static_cast<unsigned>(phase) - static_cast<unsigned>(Phase::Baseline);
        return i < frame_ms_.size() ? frame_ms_[i] : -1.f;
    }
    float Baseline() const { return baseline_; }
    float Candidate() const { return replacement_; }
    float Confirmation() const { return confirmation_; }
    float CandidateAffectedRegion() const { return affected_replacement_; }
    uint32_t Region() const { return region_; }
    // Same pixels at the displayed output in all three phases. Use the
    // quieter of the two incumbent measurements as the denominator.
    float VerifiedReduction() const
    {
        if (phase_ != Phase::Kept || spatial_only_) return -1.f;
        return 1.f - affected_replacement_ / (std::min)(baseline_, confirmation_);
    }
    uint32_t ResidualRegion() const { return residual_region_; }
    float ResidualBaseline() const { return phase_ == Phase::Kept ? residual_baseline_ : 0.f; }

private:
    // Kept by structure: watch the delivered picture. Roll back only on
    // positive damage in a view matching the incumbent's; hand a recurring
    // residual to stage recovery and let that target expire once quiet.
    unsigned MatchingViews(const TrialSample& sample) const
    {
        unsigned matches = 0;
        for (unsigned k = 0; k < incumbent_count_; ++k) {
            const auto& ref = incumbent_[k];
            if (!SameSetup(ref, sample)) continue;
            float change = 0;
            unsigned count = 0;
            for (unsigned r = 0; r < FlickerTransition::count; ++r) {
                if (ref.change[r] >= minimum_flicker / 3.f) continue;
                change += std::fabs(ref.image[r] - sample.image[r]);
                ++count;
            }
            if (count >= FlickerTransition::count / 2 && change <= float(count) * .10f) matches |= 1u << k;
        }
        return matches;
    }
    float IncumbentFlicker(unsigned region, unsigned views) const
    {
        float most = 0.f;
        for (unsigned i = 0; i < incumbent_count_; ++i)
            if (views & (1u << i)) most = (std::max)(most, incumbent_[i].change[region]);
        return most;
    }
    void KeepStructural(const TrialSample& sample)
    {
        const unsigned views = MatchingViews(sample);
        unsigned lost_views = 0;
        for (unsigned k = 0; k < incumbent_count_; ++k) {
            if (!(views & (1u << k))) continue;
            unsigned lost = 0;
            for (unsigned r = 0; r < FlickerTransition::count && lost < 2; ++r) {
                const float detail = incumbent_[k].contrast[r];
                if (detail > .04f && sample.contrast[r] < .75f * detail) ++lost;
            }
            if (lost >= 2) lost_views |= 1u << k;
        }
        // If any comparable ordinary picture already had this level of
        // detail, the replacement has not been shown to cause the loss.
        const unsigned loss = lost_views == views ? views : 0;
        if (prior_detail_loss_ & loss) {
            ruled_out_ = true;
            Retry("the role-following route lost visible detail against the ordinary route's picture");
            return;
        }
        prior_detail_loss_ = loss;
        if (count_ && !SameSetup(window_[0], sample)) count_ = 0;
        window_views_[count_] = views;
        window_[count_++] = sample;
        if (count_ >= samples) {
            unsigned common_views = views;
            for (unsigned i = 0; i < count_; ++i) common_views &= window_views_[i];
            float strongest = 0.f;
            uint32_t at = 0;
            const TrialSample* pattern = nullptr;
            for (uint32_t i = 0; i < FlickerTransition::count; ++i) {
                const TrialSample* reference = nullptr;
                const float amplitude = Repeated(i, nullptr, reference);
                // A recurring flash where the incumbent was measured quiet is
                // damage done by the role route, not a residual.
                if (common_views && amplitude >= minimum_flicker &&
                    IncumbentFlicker(i, common_views) < minimum_flicker / 3.f) {
                    ruled_out_ = true;
                    Retry("the role-following route introduced flicker in an area the ordinary route showed quiet");
                    return;
                }
                if (amplitude > strongest) { strongest = amplitude; at = i; pattern = reference; }
            }
            if (residual_baseline_ > 0.f) {
                const TrialSample* reference = nullptr;
                const float current = Repeated(residual_region_, nullptr, reference);
                // The followed area went quiet on comparable samples: expire
                // its score so a smaller remaining flash can be followed.
                if (current < FlickerTransition::scoped_minimum) residual_baseline_ = 0.f;
                else if (at == residual_region_) residual_baseline_ = current;
            }
            if (strongest >= minimum_flicker && pattern &&
                (residual_baseline_ <= 0.f || (at != residual_region_ && strongest > residual_baseline_))) {
                residual_region_ = region_ = at;
                residual_baseline_ = strongest;
                reason_ = "role-following route kept; a recurring residual was handed to stage recovery";
            }
        }
        if (count_ == window_.size()) {
            std::move(window_views_.begin() + 1, window_views_.end(), window_views_.begin());
            std::move(window_.begin() + 1, window_.end(), window_.begin());
            --count_;
        }
    }
    void SelectResidualRegion()
    {
        float greatest = 0.f;
        for (uint32_t i = 0; i < regions_.size(); ++i) {
            const auto& evidence = regions_[i];
            if (evidence.baseline < minimum_flicker || evidence.replacement < FlickerTransition::scoped_minimum)
                continue;
            const TrialSample* pattern = nullptr;
            const float confirmation = Repeated(i, &evidence.pattern, pattern);
            if (confirmation < evidence.baseline * .67f || confirmation > evidence.baseline * 1.5f)
                continue;
            const float original = (std::min)(evidence.baseline, confirmation);
            if (evidence.replacement > original * residual_fraction && evidence.replacement > greatest) {
                greatest = evidence.replacement;
                residual_region_ = i;
                residual_baseline_ = original;
            }
        }
    }
    void Refuse(TrialGate gate)
    {
        if (Active()) ++live_.refused[static_cast<unsigned>(gate)];
    }
    void Time(const TrialSample& sample)
    {
        if (sample.observed_at && timed_at_ && sample.observed_at > timed_at_ && sample.ordinal > timed_ordinal_) {
            intervals_[interval_count_ % intervals_.size()] =
                float(sample.observed_at - timed_at_) / float(sample.ordinal - timed_ordinal_);
            ++interval_count_;
        }
        timed_at_ = sample.observed_at;
        timed_ordinal_ = sample.ordinal;
    }
    float MedianInterval() const
    {
        const unsigned n = (std::min)(interval_count_, unsigned(intervals_.size()));
        if (n < frame_intervals) return -1.f;
        // Insertion sort over at most eight values with explicit bounds.
        std::array<float, 8> sorted{};
        for (unsigned i = 0; i < n && i < sorted.size(); ++i) {
            unsigned at = i;
            for (; at > 0 && sorted[at - 1] > intervals_[i]; --at) sorted[at] = sorted[at - 1];
            sorted[at] = intervals_[i];
        }
        return n % 2 ? sorted[n / 2] : .5f * (sorted[n / 2 - 1] + sorted[n / 2]);
    }
    bool CostsFrameTime() const
    {
        const float ordinary = FrameMs(Phase::Baseline), replacement = FrameMs(Phase::Candidate);
        const float recheck = MedianInterval(); // the recheck phase is still open here
        if (!(ordinary > 0 && replacement > 0 && recheck > 0)) return false;
        const float slower = (std::max)(ordinary, recheck);
        return replacement > slower * frame_cost_ratio && replacement - slower > frame_cost_ms;
    }
    void Close()
    {
        if (!Active()) return;
        frame_ms_[static_cast<unsigned>(phase_) - static_cast<unsigned>(Phase::Baseline)] = MedianInterval();
        interval_count_ = 0;
        timed_at_ = timed_ordinal_ = 0;
        closed_ = live_;
        closed_phase_ = phase_;
        live_ = {};
    }
    // Name the gate that kept the phase from finishing instead of one generic
    // timeout. Zero receipts is its own answer.
    const char* TimeoutReason() const
    {
        if (!live_.receipts) return "timed out: no screen receipt arrived for this phase";
        switch (live_.Decisive()) {
        case TrialGate::Experiment: return "timed out: the receipts belonged to another experiment";
        case TrialGate::Stale: return "timed out: the receipts were too old to use";
        case TrialGate::Scope: return "timed out: the receipts showed a different scene or route";
        case TrialGate::Incomparable: return "timed out: the frame pairs could not be compared";
        case TrialGate::Undelivered: return "timed out: the replacement was not confirmed on the screen";
        case TrialGate::Invalid: return "timed out: the measurements were not finite";
        case TrialGate::View: return "timed out: the view kept changing";
        case TrialGate::Region: return "timed out: the disputed output area could not be verified";
        case TrialGate::Symptom: return "timed out: the symptom did not recur in a full window";
        case TrialGate::Motion: return "timed out: no usable motion measurement arrived";
        default: return "timed out: too few usable screen receipts to fill a window";
        }
    }
    void Next(Phase phase, uint64_t frame)
    {
        Close();
        phase_ = phase;
        count_ = lost_region_ = 0;
        // A slow but conclusive baseline must leave the candidate its own
        // observation window. Only phase progress renews this deadline.
        expires_ = now_ + maximum_ms;
        measured_from_ = frame + warmup_frames;
    }
    void Reject(const char* reason, TrialOutcome outcome = TrialOutcome::Insufficient)
    {
        Close();
        phase_ = Phase::Rejected;
        reason_ = reason;
        outcome_ = ruled_out_ ? TrialOutcome::Harm : outcome;
    }
    void Retry(const char* reason, TrialOutcome outcome = TrialOutcome::Insufficient)
    {
        Reject(reason, outcome);
        retry_at_ = now_ + retry_delay_;
        retry_delay_ = (std::min)(retry_delay_ * 2, uint64_t{60000});
    }
    // No verdict either way: allow a bounded number of fresh attempts.
    void Inconclusive(const char* reason, TrialOutcome outcome = TrialOutcome::Insufficient)
    {
        if (inconclusive_ >= inconclusive_limit) { Reject(reason, outcome); return; }
        ++inconclusive_;
        Retry(reason, outcome);
    }
    bool ImprovesWith(float replacement, float ordinary) const
    {
        return replacement <= ordinary * .70f && ordinary - replacement >= flicker_floor_ / 3.f;
    }
    bool Improves(float value) const { return ImprovesWith(replacement_, value); }
    float Repeated(uint32_t region, const TrialSample::Change* prior_pattern, const TrialSample*& pattern) const
    {
        // Two samples must move the same pixels one way, and two the
        // opposite way. Quiet intervening pairs need not be transitions.
        std::array<float, incumbent_window> magnitude{};
        const auto end = magnitude.begin() + count_;
        for (unsigned i = 0; i < count_; ++i) magnitude[i] = TrialMagnitude(window_[i].change[region]);
        pattern = &window_[std::max_element(magnitude.begin(), end) - magnitude.begin()];
        const auto& reference = prior_pattern ? *prior_pattern : pattern->change[region];
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
    template<class Reference>
    static bool SameSetup(const Reference& a, const TrialSample& b)
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
    bool targeted_ = false, ruled_out_ = false, regional_ = false, spatial_only_ = false, structural_ = false;
    unsigned missed_ = 0, prior_detail_loss_ = 0, incumbent_count_ = 0;
    struct IncumbentView {
        uint64_t configuration = 0;
        uint32_t layout = 0, width = 0, height = 0;
        std::array<float, FlickerTransition::count> image{}, contrast{}, change{};
    };
    std::array<IncumbentView, incumbent_samples> incumbent_{};
    uint8_t lost_region_ = 0;
    uint64_t regional_seen_ = 0;
    uint32_t region_ = 0;
    uint32_t residual_region_ = 0;
    unsigned count_ = 0;
    float baseline_ = 0, replacement_ = 0, confirmation_ = 0, affected_replacement_ = 0;
    float flicker_floor_ = minimum_flicker;
    float residual_baseline_ = 0.f;
    const char* reason_ = "";
    unsigned inconclusive_ = 0;
    TrialOutcome outcome_ = TrialOutcome::Pending;
    TrialEvidence live_{}, closed_{};
    Phase closed_phase_ = Phase::Idle;
    std::array<float, 3> frame_ms_{-1.f, -1.f, -1.f};
    std::array<float, 8> intervals_{};
    unsigned interval_count_ = 0;
    uint64_t timed_at_ = 0, timed_ordinal_ = 0;
    TrialSample anchor_;
    static constexpr unsigned incumbent_window = 8;
    std::array<unsigned, incumbent_window> window_views_{};
    std::array<TrialSample, incumbent_window> window_{};
    struct RegionEvidence {
        TrialSample::Change pattern{};
        float baseline = 0.f, replacement = 0.f;
    };
    std::array<RegionEvidence, FlickerTransition::count> regions_{};
};

} // namespace na::referee
