#pragma once
#include "core/referee.hpp"
#include <array>

namespace na::referee::shadow {

enum class Path : uint8_t { None, Between, Inline, Native11 };

inline const char* Name(Path path)
{
    switch (path) {
    case Path::Between: return "between";
    case Path::Inline: return "inline";
    case Path::Native11: return "native11";
    default: return "none";
    }
}

struct Placement {
    Path path = Path::None;
    uint64_t boundary = 0;

    bool operator==(const Placement& other) const { return path == other.path && boundary == other.boundary; }
};

struct Context {
    Placement placement;
    uint64_t color = 0, depth = 0, generation = 0;
    uint32_t subresource = 0, width = 0, height = 0;
    uint64_t native_screen_proof = 0;
    uint64_t trial_ticket = 0, trial_configuration = 0;
    uint32_t recovery_region = 0;
    float recovery_floor = 0; // same affected region of an already verified placement

    bool SameScene(const Context& other) const
    {
        return color && depth && color == other.color && depth == other.depth && subresource == other.subresource &&
               width == other.width && height == other.height;
    }
    bool operator==(const Context& other) const
    {
        return placement == other.placement && color == other.color && depth == other.depth &&
               generation == other.generation && subresource == other.subresource && width == other.width && height == other.height &&
               native_screen_proof == other.native_screen_proof && trial_ticket == other.trial_ticket &&
               trial_configuration == other.trial_configuration && recovery_region == other.recovery_region &&
               recovery_floor == other.recovery_floor;
    }
    bool SameTemporalSequence(const Context& other) const
    {
        if (*this == other) return true;
        // Native11 allocates a common scene generation only after the
        // rotating owners have both been verified. Preserve physical proof
        // identity for screen votes and use the logical scene for motion.
        return placement.path == Path::Native11 && placement == other.placement &&
               generation && generation == other.generation &&
               trial_ticket == other.trial_ticket && trial_configuration == other.trial_configuration &&
               recovery_region == other.recovery_region && recovery_floor == other.recovery_floor &&
               native_screen_proof && other.native_screen_proof &&
               color && other.color && depth && depth == other.depth && subresource == other.subresource &&
               width == other.width && height == other.height;
    }
};

// Observe every eligible Present, including the ones that never produced a DLSS sample.
// A gap, menu or another swap chain ends the sequence; it cannot establish a transition.
struct DeliveryContinuity {
    uint64_t changes = 0, longest_miss = 0;
    uint64_t miss_run = 0, last_ordinal = 0;
    const void* source = nullptr;
    bool last_submitted = false;
    void Observe(const void* swapchain, uint64_t ordinal, bool eligible, bool submitted)
    {
        const bool next = source && source == swapchain && ordinal > last_ordinal && ordinal - last_ordinal == 1;
        if (eligible && next && submitted != last_submitted) ++changes;
        miss_run = eligible && !submitted ? (next ? miss_run : 0) + 1 : 0;
        longest_miss = (std::max)(longest_miss, miss_run);
        source = eligible ? swapchain : nullptr;
        last_ordinal = ordinal;
        last_submitted = submitted;
    }
};

enum class ScreenVote { Unknown, Input, Output };

inline ScreenVote VoteScreen(const SampleScore& score)
{
    const uint32_t decided = score.tiles_input + score.tiles_output;
    if (score.tiles_textured < 4 || decided < 4 || decided * 2 < score.tiles_textured) return ScreenVote::Unknown;
    if (score.tiles_output * 4 >= decided * 3) return ScreenVote::Output;
    // A strong output majority shows delivery. A strong input majority with
    // independent output regions cannot establish a whole-screen bypass.
    if (score.tiles_output < 2 && score.tiles_input * 4 >= decided * 3) return ScreenVote::Input;
    return ScreenVote::Unknown;
}

// A mild reconstruction can preserve the same edges as its input. Look at
// registered pixels where the two actually differ. A close match to both,
// mixed ownership or a later tone curve outside tolerance is inconclusive.
inline ScreenVote DistinguishTrialOutput(const SampleScore& score, const Sample& s,
                                         uint32_t width, uint32_t height, const TileLocations& locations, TileVotes* local = nullptr)
{
    const auto vote = VoteScreen(score);
    if (local) *local = score.local;
    if (!s.data || !s.have_screen || !s.tiles_comparable) return vote;
    const bool whole = vote == ScreenVote::Unknown && !(score.tiles_input && score.tiles_output);
    if (!whole && !local) return vote;
    const float* input = s.data + BlockOffset(kTileInput);
    const float* output = s.data + BlockOffset(kTileOutput);
    const float* screen = s.registered_screen_tiles ? s.registered_screen_tiles : s.data + BlockOffset(kTileScreen);
    unsigned input_tiles = 0, output_tiles = 0;
    for (unsigned tile = 0; tile < kTiles; ++tile) {
        if (!whole && ((local->input | local->output) & (1u << tile))) continue;
        unsigned different = 0, at_input = 0, at_output = 0, first_different = 0, first_input = 0;
        for (unsigned j = 0; j < kTile*kTile; ++j) {
            const unsigned i = tile*kTile*kTile + j;
            const float gap = std::fabs(input[i] - output[i]);
            if (!std::isfinite(gap) || gap < .05f) continue;
            ++different;
            const float tolerance = (std::min)(.04f, .25f * gap);
            const bool near_input = std::fabs(screen[i] - input[i]) <= tolerance;
            at_input += near_input;
            if (j < kTile * kTile / 2) { ++first_different; first_input += near_input; }
            at_output += std::fabs(screen[i] - output[i]) <= tolerance;
        }
        if (different < 16) continue;
        if (at_input * 4 >= different * 3) {
            input_tiles |= 1u << tile;
            if (local && first_different >= 8 && different - first_different >= 8 &&
                first_input * 4 >= first_different * 3 && (at_input - first_input) * 4 >= (different - first_different) * 3)
                local->confirmed_input |= uint8_t(1u << tile);
        }
        if (at_output * 4 >= different * 3) output_tiles |= 1u << tile;
    }
    if (local) {
        const uint8_t conflict = uint8_t((input_tiles & local->output) | (output_tiles & local->input));
        local->input = uint8_t((local->input | input_tiles) & ~conflict & score.textured_tiles);
        local->output = uint8_t((local->output | output_tiles) & ~conflict & score.textured_tiles);
        local->confirmed_input &= local->input;
    }
    if (local && local->input && !(local->input & (local->input - 1)) && !(local->input & local->confirmed_input) &&
        (local->output & (local->output - 1))) {
        // One small disputed area can be real. Require two non-overlapping textured
        // halves to independently prefer input, in addition to the clear control tiles.
        unsigned tile = 0;
        while (!(local->input & (1u << tile))) ++tile;
        bool confirmed = true;
        for (unsigned half = 0; confirmed && half < 2; ++half) {
            const unsigned i = (2 * tile + half) * kTile * kTile / 2;
            const float ci = EdgeCorrelation(input + i, screen + i, kTile, kTile / 2);
            const float co = EdgeCorrelation(output + i, screen + i, kTile, kTile / 2);
            confirmed = ci >= .85f && ci - co >= Params{}.tile_margin &&
                        EdgeEnergy(output + i, kTile, kTile / 2) >= Params{}.tile_energy;
        }
        if (confirmed) local->confirmed_input = local->input;
    }
    if (!whole) return vote;
    const auto independent = [&](unsigned votes) {
        for (unsigned i = 0; i < kTiles; ++i) if (votes & (1u << i))
            for (unsigned j = 0; j < i; ++j) if (votes & (1u << j)) {
                if (std::abs(TileLeft(i, width, locations) - TileLeft(j, width, locations)) >= int(kTile) ||
                    std::abs(TileTop(i, height, locations) - TileTop(j, height, locations)) >= int(kTile)) return true;
            }
        return false;
    };
    if (!input_tiles && !score.tiles_input && independent(output_tiles)) return ScreenVote::Output;
    if (!output_tiles && !score.tiles_output && independent(input_tiles)) return ScreenVote::Input;
    return ScreenVote::Unknown;
}

struct Evidence {
    Context context;
    Window measured;
    Placement alternative;
    uint32_t alternative_votes = 0;
    bool alternatives_disagree = false;

    void Add(const SampleScore& score, Placement observed = {})
    {
        measured.Add(score);
        if (VoteScreen(score) != ScreenVote::Input || observed.path == Path::None ||
            observed == context.placement) return;
        if (alternative_votes && !(alternative == observed)) alternatives_disagree = true;
        alternative = observed;
        ++alternative_votes;
    }
};

struct Choice {
    const char* action;
    const char* reason;
    Placement candidate{};
};

inline bool StrongCount(uint32_t matching, uint32_t samples)
{
    return matching >= 3 && uint64_t(matching) * 4 >= uint64_t(samples) * 3;
}

inline Choice Preview(const Evidence& current, const Evidence* alternative = nullptr)
{
    const auto& votes = current.measured;
    if (current.context.placement.path == Path::None || !current.context.color || !current.context.depth)
        return {"hold", "the sample has no identified scene and placement"};

    const auto verdict = Judge(votes);
    if (verdict.picture == Verdict::Pass && votes.output_frames && votes.input_frames)
        return {"check_continuity", "some sampled screens favor the output and others the input", current.context.placement};
    // VARIANT: referee/composite-evidence
    if (verdict.picture == Verdict::Pass && votes.mixed_frames >= 2)
        return {"check_composite", "separate regions of each sampled screen favor different stages", current.context.placement};
    if (verdict.picture == Verdict::Pass && verdict.result == Verdict::Pass &&
        StrongCount(votes.output_frames, votes.samples)) {
        if (verdict.motion == Verdict::Fail || verdict.jitter == Verdict::Fail)
            return {"check_inputs", "the output reaches the screen; motion or jitter failed", current.context.placement};
        return {"keep", "the screen repeatedly shows the output", current.context.placement};
    }
    if (verdict.picture == Verdict::Fail && StrongCount(votes.neither_frames, votes.samples))
        return {"check_scene", "the evaluated picture and the screen repeatedly disagree"};
    if (verdict.picture == Verdict::Pass && verdict.result == Verdict::Fail &&
        StrongCount(votes.input_frames, votes.samples)) {
        if (alternative && current.context.SameScene(alternative->context) &&
            current.context.generation == alternative->context.generation &&
            alternative->context.placement.path != Path::None &&
            !(alternative->context.placement == current.context.placement)) {
            const auto proof = Judge(alternative->measured);
            if (proof.picture == Verdict::Pass && proof.result == Verdict::Pass &&
                proof.motion != Verdict::Fail && proof.jitter != Verdict::Fail &&
                StrongCount(alternative->measured.output_frames, alternative->measured.samples))
                return {"prefer", "the separately measured alternative repeatedly reached the screen", alternative->context.placement};
        }
        if (!current.alternatives_disagree && StrongCount(current.alternative_votes, votes.input_frames))
            return {"probe", "the input repeatedly reached the screen and another boundary was observed", current.alternative};
        return {"check_placement", "the input repeatedly reached the screen; no consistent alternative boundary was observed"};
    }
    return {"hold", "too little consistent evidence to change placement"};
}

// A rotating scene target or optional pass must not erase its other recent observations.
// Keep only scores, never GPU resources. Unrelated presentations and resets start a new history.
class RecentSamples {
public:
    static constexpr size_t capacity = 12;
    void Clear() { count_ = next_ = 0; }
    void Add(const Context& context, uint64_t ordinal, const SampleScore& score, Placement observed = {})
    {
        if (count_) {
            const auto& last = entries_[(next_ + capacity - 1) % capacity];
            if (ordinal <= last.ordinal || context.generation != last.context.generation) Clear();
        }
        entries_[next_] = {context, ordinal, score, observed};
        next_ = (next_ + 1) % capacity;
        if (count_ < capacity) ++count_;
    }

    Evidence For(const Context& context, uint64_t& first) const
    {
        first = 0;
        Evidence e;
        e.context = context;
        for (size_t i = 0; i < count_; ++i) {
            const auto& entry = entries_[(next_ + capacity - count_ + i) % capacity];
            if (!(entry.context == context)) continue;
            if (!e.measured.samples) first = entry.ordinal;
            e.Add(entry.score, entry.observed);
        }
        return e;
    }

    Choice Choose(const Evidence& current) const
    {
        const auto decision = Preview(current);
        if (decision.candidate.path == Path::None || decision.candidate == current.context.placement) return decision;
        // Only compare the boundary independently observed on this scene's bypassed frames.
        Context other = current.context;
        other.placement = decision.candidate;
        uint64_t first = 0;
        const auto alternative = For(other, first);
        return Preview(current, &alternative);
    }

private:
    struct Entry { Context context; uint64_t ordinal = 0; SampleScore score; Placement observed; };
    std::array<Entry, capacity> entries_{};
    size_t next_ = 0, count_ = 0;
};

} // namespace na::referee::shadow
