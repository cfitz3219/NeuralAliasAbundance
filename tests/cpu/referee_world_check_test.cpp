// Referee: the look that decides whether a world drawn small is still the right picture
// (core/referee_world_check.hpp), and the rehearsal that decides when drawing small may start.
#include "test_support.hpp"
#include "core/referee_world_check.hpp"
using namespace na::referee;
namespace {
WorldGrid Scene(int shift = 0)
{
    WorldGrid g{};
    for (uint32_t y = 0; y < kWorldGridH; ++y)
        for (uint32_t x = 0; x < kWorldGridW; ++x) g[y * kWorldGridW + x] = uint8_t(40 + ((x + uint32_t(shift)) * 3 + y * 5) % 160);
    return g;
}
// The lower half shows the wrong place (what a pixel-addressed read does when given the picture as it is).
WorldGrid Wrong(const WorldGrid& from)
{
    WorldGrid g = from;
    for (uint32_t i = kWorldCells / 2; i < kWorldCells; ++i) g[i] = uint8_t(255 - g[i]);
    return g;
}
// One look: two comparison frames and one small frame; returns the verdict of the small frame.
WorldCheck::Verdict Look(WorldCheck& c, uint64_t& tag, const WorldGrid& ref0, const WorldGrid& ref1, const WorldGrid& drawn_small, bool inputs = true)
{
    CHECK(c.Next() == WorldCheck::Frame::Reference && c.WantsSample());
    c.Drawn(WorldCheck::Frame::Reference, ++tag, inputs);
    CHECK(c.Next() == WorldCheck::Frame::Reference && c.WantsSample());
    c.Drawn(WorldCheck::Frame::Reference, ++tag, inputs);
    CHECK(c.Next() == WorldCheck::Frame::Small && c.WantsSample());
    c.Drawn(WorldCheck::Frame::Small, ++tag, inputs);
    // While the pictures are on their way back, nothing unjudged is drawn small.
    CHECK(c.Next() == WorldCheck::Frame::Reference && !c.WantsSample());
    CHECK(c.Sampled(tag - 2, ref0) == WorldCheck::Verdict::None);
    CHECK(c.Sampled(tag - 1, ref1) == WorldCheck::Verdict::None);
    return c.Sampled(tag, drawn_small);
}
} // namespace
int main() { return test::run({
    {"the same picture passes and the world stays small", [] {
        WorldCheck c; uint64_t tag = 0;
        c.Begin();
        CHECK(Look(c, tag, Scene(), Scene(), Scene()) == WorldCheck::Verdict::Passed);
        CHECK(c.Steady() && !c.Failed() && c.Next() == WorldCheck::Frame::Small && !c.WantsSample());
        CHECK(c.Inputs() == WorldInputs::ByShader);
    }},
    {"a wrong picture is looked at twice, then the next way of giving inputs is tried", [] {
        WorldCheck c; uint64_t tag = 0;
        c.Begin();
        CHECK(Look(c, tag, Scene(), Scene(), Wrong(Scene())) == WorldCheck::Verdict::Retry);
        CHECK(c.Inputs() == WorldInputs::ByShader);
        CHECK(Look(c, tag, Scene(), Scene(), Wrong(Scene())) == WorldCheck::Verdict::NextInputs);
        CHECK(c.Inputs() == WorldInputs::AsDrawn && c.Looking());
        // The second way draws it right.
        CHECK(Look(c, tag, Scene(), Scene(), Scene()) == WorldCheck::Verdict::Passed);
        CHECK(c.Steady() && c.Inputs() == WorldInputs::AsDrawn);
    }},
    {"when every way draws it wrong the world keeps its full size", [] {
        WorldCheck c; uint64_t tag = 0;
        c.Begin();
        WorldCheck::Verdict v = WorldCheck::Verdict::None;
        for (int i = 0; i < 6; ++i) v = Look(c, tag, Scene(), Scene(), Wrong(Scene()));
        CHECK(v == WorldCheck::Verdict::Failed && c.Failed() && c.Inputs() == WorldInputs::Corner);
        CHECK(c.Next() == WorldCheck::Frame::Reference);
        c.Begin(); // a failed check does not start again by itself
        CHECK(c.Failed());
        c.Reset(); // another scene: everything is looked at again
        CHECK(!c.Failed() && c.Inputs() == WorldInputs::ByShader);
    }},
    {"a failed world is looked at again after a rest, and each failure makes the rest longer", [] {
        WorldCheck c; uint64_t tag = 0;
        c.Begin();
        CHECK(!c.Rest()); // nothing failed: no rest is counted
        for (int i = 0; i < 6; ++i) Look(c, tag, Scene(), Scene(), Wrong(Scene()));
        CHECK(c.Failed());
        for (uint32_t i = 0; i + 1 < WorldCheck::kFirstRest; ++i) CHECK(!c.Rest());
        CHECK(c.Rest() && !c.Failed() && !c.Looking() && c.Inputs() == WorldInputs::ByShader);
        CHECK(c.RestFrames() == WorldCheck::kFirstRest * 4);
        c.Begin();
        for (int i = 0; i < 6; ++i) Look(c, tag, Scene(), Scene(), Wrong(Scene()));
        CHECK(c.Failed());
        for (uint32_t i = 0; i + 1 < WorldCheck::kFirstRest * 4; ++i) CHECK(!c.Rest());
        CHECK(c.Rest());
        // The rest never grows past the longest one.
        for (int round = 0; round < 6; ++round) {
            c.Begin();
            for (int i = 0; i < 6; ++i) Look(c, tag, Scene(), Scene(), Wrong(Scene()));
            while (!c.Rest()) {}
        }
        CHECK(c.RestFrames() == WorldCheck::kLongestRest);
        c.Reset(); // another level or another world: the first rest again
        CHECK(c.RestFrames() == WorldCheck::kFirstRest);
    }},
    {"no draw read a full-size picture: the other ways are not tried", [] {
        WorldCheck c; uint64_t tag = 0;
        c.Begin();
        CHECK(Look(c, tag, Scene(), Scene(), Wrong(Scene()), false) == WorldCheck::Verdict::Retry);
        CHECK(Look(c, tag, Scene(), Scene(), Wrong(Scene()), false) == WorldCheck::Verdict::Failed);
    }},
    {"a slow pan is not mistaken for a wrong picture", [] {
        WorldCheck c; uint64_t tag = 0;
        c.Begin();
        // Each frame the picture slides a little: frame to frame the same few cells change.
        WorldGrid a = Scene(0), b = Scene(0), s = Scene(0);
        for (uint32_t i = 0; i < 100; ++i) { b[i * 7] = uint8_t(b[i * 7] + 60); s[i * 7 + 3] = uint8_t(s[i * 7 + 3] + 60); }
        CHECK(CompareWorld(a, b).fraction > 0.04f);
        CHECK(Look(c, tag, a, b, s) == WorldCheck::Verdict::Passed);
    }},
    {"a view moving too fast to judge is looked at again, then let through unjudged", [] {
        WorldCheck c; uint64_t tag = 0;
        c.Begin();
        for (uint32_t i = 0; i + 1 < WorldCheck::kUnseenTries; ++i)
            CHECK(Look(c, tag, Scene(0), Scene(17), Scene(31)) == WorldCheck::Verdict::Retry);
        CHECK(Look(c, tag, Scene(0), Scene(17), Scene(31)) == WorldCheck::Verdict::PassedUnseen);
        CHECK(c.Steady() && c.Unjudged());
        c.Begin(); // the caller looks again later
        CHECK(Look(c, tag, Scene(), Scene(), Scene()) == WorldCheck::Verdict::Passed && !c.Unjudged());
    }},
    {"a look whose pictures never come back starts again", [] {
        WorldCheck c; uint64_t tag = 0;
        c.Begin();
        c.Drawn(WorldCheck::Frame::Reference, ++tag, true);
        c.Drawn(WorldCheck::Frame::Reference, ++tag, true);
        c.Drawn(WorldCheck::Frame::Small, ++tag, true);
        for (uint32_t i = 0; i <= WorldCheck::kAwaitFrames; ++i) c.Drawn(WorldCheck::Frame::Reference, 0, true);
        CHECK(c.Looking() && c.WantsSample() && c.Next() == WorldCheck::Frame::Reference);
        // A frame not drawn as asked, or not sampled, does not move the look on.
        c.Drawn(WorldCheck::Frame::Small, ++tag, true);
        c.Drawn(WorldCheck::Frame::Reference, 0, true);
        CHECK(c.WantsSample() && c.Next() == WorldCheck::Frame::Reference);
    }},
    {"a new shader that reads a full-size picture is looked at without losing the way that works", [] {
        WorldCheck c; uint64_t tag = 0;
        c.Begin();
        for (int i = 0; i < 2; ++i) Look(c, tag, Scene(), Scene(), Wrong(Scene()));
        CHECK(Look(c, tag, Scene(), Scene(), Scene()) == WorldCheck::Verdict::Passed);
        c.Begin();
        CHECK(c.Looking() && c.Inputs() == WorldInputs::AsDrawn);
        CHECK(Look(c, tag, Scene(), Scene(), Scene()) == WorldCheck::Verdict::Passed);
    }},
    {"rehearsal: drawing small starts after clean frames and each fall asks for a longer run", [] {
        WorldRehearsal r;
        for (uint32_t i = 0; i + 1 < WorldRehearsal::kFirst; ++i) { r.Frame(true); CHECK(!r.Live()); }
        r.Frame(true);
        CHECK(r.Live());
        r.Frame(false);
        CHECK(!r.Live() && r.Needed() == WorldRehearsal::kFirst * 8);
        for (uint32_t i = 0; i < WorldRehearsal::kFirst * 8; ++i) r.Frame(true);
        CHECK(r.Live());
        for (int i = 0; i < 8; ++i) { r.Frame(false); for (uint32_t k = 0; k < r.Needed(); ++k) r.Frame(true); }
        CHECK(r.Needed() == WorldRehearsal::kLongest && r.Live());
        // A frame that is not clean while still rehearsing does not lengthen the run.
        WorldRehearsal q;
        q.Frame(true); q.Frame(false);
        CHECK(q.Needed() == WorldRehearsal::kFirst);
    }},
}); }
