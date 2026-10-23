// reconstructed 2026-10 — request identity, deadlines and input validation.
#include "test_support.hpp"
#include "host_dx12/ngx_observe/size_confirm.hpp"
#include <limits>
using namespace na::ngx::confirm;
namespace {
EvalObs good(uint64_t handle = 7) { return {640, 360, true, false, true, handle}; }
Monitor counting(uint32_t need = 2) {
    Monitor m; m.Arm(640, 360, need, 100, 50, 80, 200); m.OnCreate(640, 360, 7, 101); m.MarkRestored(110); return m;
}
}
int main() { return test::run({
    {"scalar poison, finite values and signed motion scales", [] {
        CHECK(ScalarInputsCoherent(.5f, -.5f, -640, 360));
        for (float invalid : {1e38f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
            CHECK(!ScalarInputsCoherent(invalid, 0, 1, 1)); CHECK(!ScalarInputsCoherent(0, 0, invalid, 1));
        }
        CHECK(!ScalarInputsCoherent(1.01f, 0, 1, 1)); CHECK(!ScalarInputsCoherent(0, 0, 0, 1));
        CHECK(!ScalarInputsCoherent(0, 0, 1, 0));
    }},
    {"subrect coverage uses wide arithmetic", [] {
        CHECK(CoversSubrect(800, 600, 10, 20, 790, 580));
        CHECK(!CoversSubrect(800, 600, 10, 20, 791, 580));
        CHECK(!CoversSubrect(800, 600, UINT32_MAX, 0, 1, 1));
        CHECK(!CoversSubrect(100, 100, UINT32_MAX - 1, 0, 10, 1));
        CHECK(!CoversSubrect(0, 600, 0, 0, 1, 1)); CHECK(!CoversSubrect(800, 600, 0, 0, 0, 1));
    }},
    {"feature flags stay bound to handles and erase/reuse", [] {
        FeatureFlags f; f.Set(7, 1); f.Set(8, 2); CHECK(f.Get(7) == 1 && f.Get(8) == 2);
        f.Set(7, 3); CHECK(f.Get(7) == 3); f.Erase(7); CHECK(f.Get(7) == FeatureFlags::kUnknown);
        f.Set(0, 0); CHECK(f.Get(0) == FeatureFlags::kUnknown);
        for (uint64_t id = 10; id < 30; ++id) f.Set(id, static_cast<uint32_t>(id));
        CHECK(f.Get(29) == 29); CHECK(f.Get(10) == FeatureFlags::kUnknown);
    }},
    {"creation inside resize is retained but early evaluations do not count", [] {
        Monitor m; const auto id = m.Arm(640, 360, 2, 100, 50, 80, 200);
        m.OnCreate(640, 360, 7, 101); m.OnEvaluate(good(), 102);
        CHECK(m.snapshot().before_restore == 1 && m.snapshot().streak == 0);
        m.MarkRestored(110); m.OnEvaluate(good(), 111); CHECK(m.state() == State::Counting);
        m.OnEvaluate(good(), 112); CHECK(m.state() == State::Confirmed);
        CHECK(m.snapshot().id == id && m.snapshot().matching == 2 && m.snapshot().req_w == 640);
        m.OnCreate(320, 180, 8, 113); m.Cancel(114); CHECK(m.state() == State::Confirmed);
    }},
    {"each invalid eligible evaluation breaks consecutive streak", [] {
        for (int mode = 0; mode < 4; ++mode) {
            auto m = counting(); m.OnEvaluate(good(), 111); auto bad = good();
            if (mode == 0) bad.handle = 0; else if (mode == 1) bad.w = 320;
            else if (mode == 2) bad.coherent = false; else bad.ok = false;
            m.OnEvaluate(bad, 112); CHECK(m.snapshot().streak == 0);
            m.OnEvaluate(good(), 113); CHECK(m.state() == State::Counting);
            m.OnEvaluate(good(), 114); CHECK(m.state() == State::Confirmed);
        }
    }},
    {"other features ignored; replacement creation restarts streak", [] {
        auto m = counting(); m.OnEvaluate(good(), 111); m.OnEvaluate(good(8), 112);
        CHECK(m.snapshot().streak == 1 && m.snapshot().other_feature == 1);
        m.OnCreate(640, 360, 8, 113); CHECK(m.snapshot().streak == 0);
        m.OnEvaluate(good(), 114); CHECK(m.snapshot().streak == 0);
        m.OnEvaluate(good(8), 115); m.OnEvaluate(good(8), 116); CHECK(m.state() == State::Confirmed);
    }},
    {"deadline boundaries and finished requests cannot revive", [] {
        Monitor m; m.Arm(640, 360, 2, 100, 50, 80, 200); m.MarkRestored(110); m.OnCreate(640, 360, 7, 160);
        CHECK(m.state() == State::Failed && m.snapshot().reason == static_cast<uint32_t>(Reason::NoCreation));
        m.OnEvaluate(good(), 161); CHECK(m.state() == State::Failed);
        m = counting(); m.OnEvaluate(good(), 111); m.OnEvaluate(good(), 190);
        CHECK(m.state() == State::Failed && m.snapshot().reason == static_cast<uint32_t>(Reason::TooFewEvaluations));
        m.Arm(640, 360, 1, 100, 50, 80, 200); m.Tick(300);
        CHECK(m.snapshot().reason == static_cast<uint32_t>(Reason::NoRestoration));
    }},
    {"overall deadline survives replacement, cancellation and rearm", [] {
        auto m = counting(); m.OnCreate(640, 360, 8, 180); m.OnCreate(640, 360, 9, 250); m.OnCreate(640, 360, 10, 299);
        m.OnEvaluate(good(10), 300); CHECK(m.state() == State::Failed);
        auto old = m.snapshot().id; m.Arm(320, 180, 0, 400, 50, 80, 200);
        CHECK(m.snapshot().id > old && m.snapshot().need == 1 && m.snapshot().req_w == 320);
        m.Cancel(401); CHECK(m.snapshot().reason == static_cast<uint32_t>(Reason::Cancelled));
        m.Arm(640, 360, 1, 500, 50, 80, 200); m.MarkRestored(510); m.OnCreate(640, 360, 0, 511); m.Tick(560);
        CHECK(m.snapshot().reason == static_cast<uint32_t>(Reason::UnknownFeature));
    }},
}); }
