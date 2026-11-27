// The Referee's coverage trial (core/referee_coverage.hpp, VARIANT referee/coverage-trial): classes of scene draws
// left off the jitter are tried on the sample one at a time when the unjittered-edge share stays high; a class is
// kept only when the share drops with no new black, put back otherwise, and full-screen passes are tried last.
// Shares from Cort's 0f63301 Hearts of Iron IV run: quiet stretches 0.01-0.3 %, noisy ones 1-2.8 %.
#include "test_support.hpp"
#include "core/referee_coverage.hpp"
using namespace na::referee;
namespace {
CoverageWindow W(float share, std::initializer_list<std::pair<LeftClass, uint64_t>> left, float black_events = 0, float black_cells = 0)
{
    CoverageWindow w;
    w.judged = true;
    w.share = share;
    w.black_events = black_events;
    w.black_cells = black_cells;
    for (const auto& [c, n] : left) w.left[unsigned(c)] = n;
    return w;
}
} // namespace

int main()
{
    return test::run({
        {"quiet stretches start nothing", [] {
            CoverageTrial t;
            for (int i = 0; i < 20; ++i) CHECK(!t.Observe(W(.003f, {{LeftClass::OwnViewport, 400}}), 1000 * i));
            CHECK(t.MoveMask() == 0 && t.Current() == CoverageTrial::Phase::Watch);
        }},
        {"a high share tries the first class with draws, and keeps it when the share drops with no black", [] {
            CoverageTrial t;
            const auto high = W(.025f, {{LeftClass::OwnViewport, 400}, {LeftClass::FullScreen, 100}});
            CHECK(!t.Observe(high, 0));
            CHECK(t.Observe(high, 5000));
            CHECK(t.TrialClass() == int(LeftClass::OwnViewport) && (t.MoveMask() & (1u << unsigned(LeftClass::OwnViewport))));
            CHECK(t.Observe(W(.004f, {{LeftClass::FullScreen, 100}}), 10000)); // settling window
            CHECK(!t.Observe(W(.002f, {{LeftClass::FullScreen, 100}}), 15000));
            CHECK(t.Observe(W(.002f, {{LeftClass::FullScreen, 100}}), 20000));
            CHECK(t.VerdictOf(unsigned(LeftClass::OwnViewport)) == CoverageTrial::Verdict::Move);
            CHECK(t.LastKept() && t.LastClass() == int(LeftClass::OwnViewport) && t.Decisions() == 1);
            CHECK(t.MoveMask() == (1u << unsigned(LeftClass::OwnViewport)));
            std::printf("    %s\n", t.Why());
        }},
        {"a class that brings black is put back at once, and the next class is tried", [] {
            CoverageTrial t;
            const auto high = W(.02f, {{LeftClass::StationarySource, 50}, {LeftClass::OwnViewport, 400}});
            t.Observe(high, 0);
            t.Observe(high, 5000);
            CHECK(t.TrialClass() == int(LeftClass::OwnViewport)); // own viewport comes before stationary-source readers
            t.Observe(high, 10000);                             // settle
            CHECK(t.Observe(W(.019f, {}, 0, 6.f), 15000));      // lasting black appears: back at once
            CHECK(t.VerdictOf(unsigned(LeftClass::OwnViewport)) == CoverageTrial::Verdict::Still && !t.LastKept());
            CHECK(t.MoveMask() == 0);
            std::printf("    %s\n", t.Why());
            t.Observe(high, 20000);
            t.Observe(high, 25000);
            CHECK(t.TrialClass() == int(LeftClass::StationarySource));
        }},
        {"Astra's true black case: a stationary-source reader that goes black when moved stays on the game's grid", [] {
            CoverageTrial t;
            const auto high = W(.015f, {{LeftClass::StationarySource, 60}});
            t.Observe(high, 0);
            t.Observe(high, 5000);
            CHECK(t.TrialClass() == int(LeftClass::StationarySource));
            CHECK(t.Observe(W(.015f, {}, 1.f, 0), 10000)); // onsets and returns of black while settling
            CHECK(t.VerdictOf(unsigned(LeftClass::StationarySource)) == CoverageTrial::Verdict::Still);
            CHECK(t.MoveMask() == 0);
        }},
        {"a class that changes nothing is put back; full-screen passes only after every other class", [] {
            CoverageTrial t;
            const auto high = W(.02f, {{LeftClass::RuleOff, 30}, {LeftClass::FullScreen, 100}});
            t.Observe(high, 0);
            t.Observe(high, 5000);
            CHECK(t.TrialClass() == int(LeftClass::RuleOff));
            t.Observe(high, 10000);
            t.Observe(high, 15000);
            CHECK(t.Observe(high, 20000));
            CHECK(t.VerdictOf(unsigned(LeftClass::RuleOff)) == CoverageTrial::Verdict::Still);
            t.Observe(high, 25000);
            t.Observe(high, 30000);
            CHECK(t.TrialClass() == int(LeftClass::FullScreen));
        }},
        {"a put-back class is tried again after five minutes", [] {
            CoverageTrial t;
            const auto high = W(.02f, {{LeftClass::OtherTarget, 30}});
            t.Observe(high, 0); t.Observe(high, 5000); t.Observe(high, 10000); t.Observe(high, 15000); t.Observe(high, 20000);
            CHECK(t.VerdictOf(unsigned(LeftClass::OtherTarget)) == CoverageTrial::Verdict::Still);
            t.Observe(high, 25000); t.Observe(high, 30000);
            CHECK(t.TrialClass() < 0);
            t.Observe(high, 20000 + CoverageTrial::kRetryMs + 1);
            t.Observe(high, 25000 + CoverageTrial::kRetryMs + 1);
            CHECK(t.TrialClass() == int(LeftClass::OtherTarget));
        }},
        {"the jitter trial's request starts a trial after one high window", [] {
            CoverageTrial t;
            t.Kick();
            CHECK(t.Observe(W(.01f, {{LeftClass::Deferred, 5}}), 0));
            CHECK(t.TrialClass() == int(LeftClass::Deferred));
        }},
        {"classes that did only harm in earlier launches go after the others", [] {
            CoverageTrial t;
            t.SetAvoided(1u << unsigned(LeftClass::OtherTarget));
            const auto high = W(.02f, {{LeftClass::OtherTarget, 30}, {LeftClass::Other, 30}});
            t.Observe(high, 0);
            t.Observe(high, 5000);
            CHECK(t.TrialClass() == int(LeftClass::Other));
        }},
    });
}
