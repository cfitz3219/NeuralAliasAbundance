#include "test_support.hpp"
#include "providers/classifier/signal_candidates.hpp"
using namespace na::classifier;
static Window window() { return {{1, 2, 3, 4, 5, 6}, 640, 360, 20, 30, true, true, true}; }
static Observation depth() {
    return {window().scope, 7, 1, 0, 640, 360, Format::Depth,
        true, true, true, false, 2, 20, true, 1.0};
}
static void unavailable(const Window& w, const Observation& o) {
    const auto c = Describe(Signal::Depth, w, o);
    CHECK(c.confidence == Confidence::Unavailable);
    CHECK(c.acquisition_score == 0 && !c.reasons.empty());
}
int main() { return test::run({
    {"over-capacity input fails visibly instead of ranking a truncated set", [] {
        const auto r = Rank(Signal::Depth, window(), std::vector<Observation>(257, depth()));
        CHECK(r.capacity_exceeded && r.candidates.empty());
    }},
    {"plausible depth remains unverified with explicit acquisition work", [] {
        const auto c = Describe(Signal::Depth, window(), depth());
        CHECK(c.confidence == Confidence::Candidate && c.acquisition_score > 0);
        CHECK(c.acquisition_tasks.size() == 3);
    }},
    {"every scope component prevents stale or cross-queue evidence", [] {
        for (unsigned i = 0; i < 6; ++i) {
            auto o = depth();
            uint64_t* fields[] = {&o.scope.session, &o.scope.epoch, &o.scope.submission,
                &o.scope.queue, &o.scope.list, &o.scope.recording};
            ++*fields[i]; unavailable(window(), o);
            auto w = window(); w.scope = {}; o.scope = {}; unavailable(w, o);
        }
    }},
    {"loss and ambiguity remain invalid even with plausible metadata", [] {
        auto w = window(); w.loss_free = false; unavailable(w, depth());
        w = window(); w.order_unambiguous = false; unavailable(w, depth());
        w = window(); w.coverage_complete = false; unavailable(w, depth());
    }},
    {"wire unknown sentinels are not positive identity evidence", [] {
        auto w = window(); auto o = depth(); w.scope.recording = UINT64_MAX; o.scope = w.scope;
        unavailable(w,o);
        o = depth(); o.content_version = UINT64_MAX; unavailable(window(),o);
        w = window(); w.scene_end = UINT64_MAX; w.hud_begin = 0;
        o = depth(); o.last_write = UINT64_MAX; unavailable(w,o);
    }},
    {"released injector and unknown content cannot become candidates", [] {
        auto o = depth(); o.alive = false; unavailable(window(), o);
        o = depth(); o.external_work = false; unavailable(window(), o);
        o = depth(); o.content_version = 0; unavailable(window(), o);
        o = depth(); o.resource = 0; unavailable(window(), o);
    }},
    {"late overwrites and invalid boundaries require earlier acquisition", [] {
        auto o = depth(); o.last_write = 31; unavailable(window(), o);
        o = depth(); o.first_write = 21; unavailable(window(), o);
        o = depth(); o.first_write = 0; unavailable(window(), o);
        auto w = window(); w.hud_begin = 20; unavailable(w, depth());
        w = window(); w.scene_end = 0; unavailable(w, depth());
    }},
    {"extent and format alone cannot confer semantics", [] {
        auto o = depth(); o.width = 1280; unavailable(window(), o);
        o = depth(); o.format = Format::TwoChannelFloat; unavailable(window(), o);
        o = depth(); o.depth_write = false; unavailable(window(), o);
        o = depth(); o.clear_value = std::nan(""); unavailable(window(), o);
        auto w = window(); w.width = 0; unavailable(w, depth());
    }},
    {"scene color requires independent HUD verification", [] {
        auto o = depth(); o.format = Format::ColorFloat; o.color_write = true;
        o.depth_write = false;
        auto c = Describe(Signal::SceneColor, window(), o);
        CHECK(c.confidence == Confidence::Candidate);
        CHECK(c.acquisition_tasks.back().find("HUD") != std::string::npos);
        auto w = window(); w.hud_begin = 0;
        CHECK(Describe(Signal::SceneColor, w, o).confidence == Confidence::Candidate);
        o.last_write = 30;
        CHECK(Describe(Signal::SceneColor, window(), o).confidence == Confidence::Unavailable);
    }},
    {"equal shadow and scene metadata abstain without manufactured certainty", [] {
        auto a = depth(), b = depth(); b.resource = 8;
        const auto r = Rank(Signal::Depth, window(), {a, b});
        CHECK(r.ambiguous && r.candidates.size() == 2);
        CHECK(r.candidates[0].confidence == Confidence::Candidate);
        CHECK(Rank(Signal::Depth, window(), {}).candidates.empty());
    }},
    {"duplicate identities are invalidated independent of input order", [] {
        auto a = depth(), b = depth(); b.format = Format::Unknown;
        for (const auto& input : {std::vector<Observation>{a, b}, std::vector<Observation>{b, a}}) {
            const auto r = Rank(Signal::Depth, window(), input);
            for (const auto& c : r.candidates) CHECK(c.confidence == Confidence::Unavailable);
        }
    }},
    {"priority orders acquisition only and does not retain prior frame results", [] {
        auto a = depth(), b = depth(); b.resource = 8; b.last_write = 19; b.clear_known = false;
        const auto r = Rank(Signal::Depth, window(), {b, a});
        CHECK(!r.ambiguous && r.candidates[0].resource == 7);
        auto w = window(); ++w.scope.epoch;
        for (const auto& c : Rank(Signal::Depth, w, {a, b}).candidates)
            CHECK(c.confidence == Confidence::Unavailable);
    }}
}); }
