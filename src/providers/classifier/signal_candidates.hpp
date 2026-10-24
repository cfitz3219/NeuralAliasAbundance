// A5: portable, metadata-only acquisition ranking. Not a host wire ABI.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace na::classifier {
enum class Signal { Depth, SceneColor };
enum class Format { Unknown, Depth, ScalarFloat, ColorFloat, ColorUnorm, TwoChannelFloat };
enum class Confidence { Unavailable, Candidate }; // No qualified-input state yet.

// All IDs/order come from an observed submission, never pointer/Present guesses.
struct Scope {
    uint64_t session{}, epoch{}, submission{}, queue{}, list{}, recording{};
};
inline bool SameScope(const Scope& a, const Scope& b) {
    return a.session == b.session && a.epoch == b.epoch &&
        a.submission == b.submission && a.queue == b.queue &&
        a.list == b.list && a.recording == b.recording;
}
inline bool Known(uint64_t value) { return value && value != UINT64_MAX; }
inline bool ValidScope(const Scope& s) {
    return Known(s.session) && Known(s.epoch) && Known(s.submission) &&
        Known(s.queue) && Known(s.list) && Known(s.recording);
}
struct Window {
    Scope scope{};
    uint32_t width{}, height{}; // observed render extent, not display-size guess
    uint64_t scene_end{}, hud_begin{}; // same recording, one-based operation order
    bool coverage_complete{}, order_unambiguous{}, loss_free{};
};
struct Observation {
    Scope scope{};
    uint64_t resource{}, content_version{};
    uint32_t subresource{}, width{}, height{};
    Format format{Format::Unknown};
    bool alive{}, external_work{};
    bool depth_write{}, color_write{};
    uint64_t first_write{}, last_write{};
    bool clear_known{};
    double clear_value{};
};
struct Candidate {
    uint64_t resource{}, content_version{};
    uint32_t subresource{};
    Confidence confidence{Confidence::Unavailable};
    unsigned acquisition_score{}; // heuristic priority, NOT probability/certainty
    std::vector<std::string> reasons;
    std::vector<std::string> acquisition_tasks;
};
struct Ranking {
    std::vector<Candidate> candidates;
    bool ambiguous{};
    bool capacity_exceeded{};
    // Deliberately no selected resource: metadata cannot authorize evaluation.
};

inline Candidate Describe(Signal signal, const Window& w, const Observation& o) {
    Candidate c{};
    c.resource = o.resource; c.content_version = o.content_version;
    c.subresource = o.subresource;
    const auto reject = [&c](const char* why) { c.reasons.emplace_back(why); };
    if (!ValidScope(w.scope) || !SameScope(w.scope, o.scope))
        reject("Missing or mismatched session/epoch/submission/queue/recording identity");
    if (!w.coverage_complete || !w.order_unambiguous || !w.loss_free)
        reject("Incomplete, lost, or ambiguous execution evidence; reacquire history");
    if (!Known(o.resource) || !Known(o.content_version) || !o.alive || !o.external_work)
        reject("Missing live resource/content identity or observation is injector work");
    if (!w.width || !w.height || !o.width || !o.height ||
        o.width != w.width || o.height != w.height)
        reject("Render extent missing or different; acquire subrect/scale evidence");
    if (!Known(w.scene_end) || w.hud_begin == UINT64_MAX || (w.hud_begin && w.hud_begin <= w.scene_end) ||
        !Known(o.first_write) || !Known(o.last_write) || o.last_write < o.first_write || o.last_write > w.scene_end)
        reject("No intact content version at scene boundary; acquire earlier snapshot");
    if (o.clear_known && !std::isfinite(o.clear_value))
        reject("Non-finite clear observation");
    const bool depth = signal == Signal::Depth;
    const bool format_ok = depth ? (o.format == Format::Depth || o.format == Format::ScalarFloat)
        : (o.format == Format::ColorFloat || o.format == Format::ColorUnorm);
    if (!format_ok || (depth ? !o.depth_write : !o.color_write))
        reject("Format/write role lacks evidence for requested signal");
    if (!c.reasons.empty()) return c;

    c.confidence = Confidence::Candidate;
    c.acquisition_score = 40; // matching extent + observed write role
    c.reasons.emplace_back("Observed render extent and write role match");
    c.acquisition_score += depth && o.format == Format::Depth ? 20 : 10;
    c.reasons.emplace_back("Format is plausible; semantic identity remains unknown");
    if (o.last_write == w.scene_end) {
        c.acquisition_score += 10;
        c.reasons.emplace_back("Written at observed scene boundary");
    }
    if (depth && o.clear_known && (o.clear_value == 0.0 || o.clear_value == 1.0)) {
        c.acquisition_score += 5;
        c.reasons.emplace_back("Clear is consistent with a depth endpoint; convention unproven");
    }
    if (w.hud_begin) {
        c.acquisition_score += 5;
        c.reasons.emplace_back("Content version precedes observed HUD boundary; absence unproven");
    }
    c.acquisition_tasks.emplace_back("Acquire completed asynchronous content sample with matching identity and lifetime");
    c.acquisition_tasks.emplace_back("Check finite value distribution; metadata score is not content evidence");
    if (depth)
        c.acquisition_tasks.emplace_back("Verify depth against independently qualified camera and depth convention");
    else
        c.acquisition_tasks.emplace_back("Verify pre-HUD scene contents and exclude postprocess/history/UI buffers");
    return c;
}

inline Ranking Rank(Signal signal, const Window& w, const std::vector<Observation>& observations) {
    Ranking r{};
    // Worker-side bounded batches only. Never truncate and rank a partial set.
    if (observations.size() > 256) { r.capacity_exceeded = true; return r; }
    r.candidates.reserve(observations.size());
    for (const auto& o : observations) r.candidates.push_back(Describe(signal, w, o));
    // Duplicate observations cannot count as independent support. Conflicting
    // metadata for the same content identity is ambiguous, not another candidate.
    for (size_t i = 0; i < r.candidates.size(); ++i) {
        bool duplicate = false;
        for (size_t j = 0; j < r.candidates.size(); ++j) {
            if (i != j && observations[i].resource == observations[j].resource &&
                observations[i].subresource == observations[j].subresource &&
                observations[i].content_version == observations[j].content_version) {
                duplicate = true; break;
            }
        }
        if (duplicate) {
            auto& c = r.candidates[i];
            c.confidence = Confidence::Unavailable; c.acquisition_score = 0;
            c.reasons.emplace_back("Duplicate content identity; normalize observations before ranking");
        }
    }
    std::stable_sort(r.candidates.begin(), r.candidates.end(), [](const Candidate& a, const Candidate& b) {
        return a.acquisition_score > b.acquisition_score;
    });
    r.ambiguous = r.candidates.size() > 1 && r.candidates[0].acquisition_score > 0 &&
        r.candidates[0].acquisition_score == r.candidates[1].acquisition_score;
    return r;
}
} // namespace na::classifier
