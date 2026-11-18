#pragma once
#include "core/referee.hpp"

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

    bool SameScene(const Context& other) const
    {
        return color && depth && color == other.color && depth == other.depth && subresource == other.subresource &&
               width == other.width && height == other.height;
    }
    bool operator==(const Context& other) const
    {
        return placement == other.placement && color == other.color && depth == other.depth &&
               generation == other.generation && subresource == other.subresource && width == other.width && height == other.height;
    }
};

struct Evidence {
    Context context;
    Window measured;
    Placement alternative;
    uint32_t alternative_votes = 0;
    bool alternatives_disagree = false;

    void Add(const SampleScore& score, Placement observed = {})
    {
        measured.Add(score);
        const uint32_t decided = score.tiles_output + score.tiles_input;
        if (score.tiles_textured < 4 || decided < 4 || decided * 2 < score.tiles_textured ||
            score.tiles_input * 4 < decided * 3 ||
            observed.path == Path::None || observed == context.placement) return;
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

} // namespace na::referee::shadow
