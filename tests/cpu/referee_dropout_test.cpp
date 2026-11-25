// The Referee's sample-locked dropout watch (core/referee_dropout.hpp), on the jitter sequence and
// the black frames of the Three Kingdoms capture of 2026-11-21 (p32176, frames 1910..1920).
#include "test_support.hpp"
#include "core/referee_dropout.hpp"
#include <functional>
#include <vector>
using namespace na::referee;
namespace {
using S = DropoutWatch::Sample;
// The raster samples the game ran, in order (index.csv of the capture, then the sequence wraps).
const S kSamples[] = {{-.470734f, .419042f}, {.284144f, -.011118f}, {.039021f, -.441278f}, {-.206101f, .128563f},
                      {-.451223f, -.301597f}, {.303654f, .268243f}, {.058532f, -.161917f}, {-.186590f, .407924f},
                      {-.431713f, -.022236f}, {.323165f, -.452396f}, {.078043f, .117445f}, {-.167079f, -.312715f},
                      {-.412202f, .257125f}, {.342675f, -.173035f}, {.097553f, .396806f}, {-.147569f, -.033354f}};
constexpr unsigned kCells = 172 * 72;
// The capture: the ground dropped out when the picture was moved up by 0.30 px or more.
bool Black(S s) { return s.y <= -.30f; }
DropoutPair Pair(S before, S now, bool faulty)
{
    DropoutPair p;
    p.cells = kCells;
    if (!faulty) return p;
    if (!Black(before) && Black(now)) p.gone = 4;
    if (Black(before) && !Black(now)) p.back = 4;
    return p;
}
// Runs consecutive pairs until the phase changes or `most` pairs passed; returns pairs used.
unsigned Run(DropoutWatch& w, unsigned& at, unsigned most, const std::function<bool()>& faulty)
{
    for (unsigned n = 1; n <= most; ++n, ++at) {
        const S before = kSamples[at % 16], now = kSamples[(at + 1) % 16];
        if (w.Observe(Pair(before, now, faulty()), before, now, true)) { ++at; return n; }
    }
    return 0;
}
}

int main()
{
    return test::run({
        {"a cell counts when part of it is exactly black on one side of the pair only", [] {
            // 12 values per cell; black share before at 10, now at 11.
            std::vector<float> field(4 * 12, 0.f);
            field[0 * 12 + 11] = .30f;                           // lit -> black
            field[1 * 12 + 10] = .60f;                           // black -> lit
            field[2 * 12 + 10] = .40f; field[2 * 12 + 11] = .40f; // black on both: a dark object, not a dropout
            field[3 * 12 + 11] = .02f;                           // one black place: below the share
            const auto p = CountDropouts(field.data(), 4, 12, 10, 11);
            CHECK(p.gone == 1 && p.back == 1 && p.cells == 4);
        }},
        {"the capture's dropouts start a trial, and the same samples coming back clean keep the move", [] {
            DropoutWatch w;
            unsigned at = 0;
            const unsigned seen = Run(w, at, 200, [] { return true; });
            CHECK(seen > 0 && seen <= DropoutWatch::kWindow);
            CHECK(w.Current() == DropoutWatch::Phase::Trial && w.MoveStillMeshes());
            CHECK(w.Onsets() >= 3 && w.Returns() >= 2);
            // Every remembered black sample is one the capture went black on.
            CHECK(w.BlackSamples() >= 2);
            for (unsigned i = 0; i < w.BlackSamples(); ++i) CHECK(Black(w.BlackSample(i)));
            const unsigned kept = Run(w, at, 700, [] { return false; });
            CHECK(kept >= DropoutWatch::kTrialLeast && kept < 100); // decided by the challenge samples, not by waiting
            CHECK(w.Current() == DropoutWatch::Phase::Kept && w.MoveStillMeshes());
            CHECK(w.Challenges() >= DropoutWatch::kChallenges && w.TrialEvents() == 0);
            // The original fault returning revokes the change.
            CHECK(Run(w, at, 64, [] { return true; }) > 0);
            CHECK(w.Current() == DropoutWatch::Phase::RuledOut && !w.MoveStillMeshes());
        }},
        {"a trial expires if it never revisits the formerly bad samples", [] {
            for (bool same_picture : {false, true}) {
                DropoutWatch w;
                unsigned at = 0;
                CHECK(Run(w, at, 200, [] { return true; }) > 0);
                CHECK(w.Current() == DropoutWatch::Phase::Trial);
                const DropoutPair clean{0, 0, kCells};
                const S other{0.75f, 0.75f};
                for (unsigned i = 0; i < DropoutWatch::kTrialMost; ++i)
                    w.Observe(clean, other, other, same_picture);
                CHECK(w.Current() == DropoutWatch::Phase::RuledOut && !w.MoveStillMeshes());
                CHECK(w.Challenges() == 0);
            }
        }},
        {"dropouts that continue with the meshes moved put the game's viewports back", [] {
            DropoutWatch w;
            unsigned at = 0;
            CHECK(Run(w, at, 200, [] { return true; }) > 0);
            CHECK(w.Current() == DropoutWatch::Phase::Trial);
            const unsigned out = Run(w, at, 700, [] { return true; });
            CHECK(out > DropoutWatch::kSettle && out < 64);
            CHECK(w.Current() == DropoutWatch::Phase::RuledOut && !w.MoveStillMeshes());
            // It is not tried again at once, and is looked at again later.
            CHECK(Run(w, at, 1000, [] { return true; }) == 0);
            CHECK(Run(w, at, DropoutWatch::kRetryAfter, [] { return true; }) > 0);
            CHECK(w.Current() == DropoutWatch::Phase::Watch);
        }},
        {"a cut to black, a fade and a change of view are not dropouts", [] {
            DropoutWatch w;
            DropoutPair cut; cut.cells = kCells; cut.gone = kCells - 10;
            DropoutPair back; back.cells = kCells; back.back = kCells - 10;
            for (unsigned i = 0; i < 40; ++i) {
                CHECK(!w.Observe(i % 2 ? back : cut, kSamples[i % 16], kSamples[(i + 1) % 16], true));
            }
            DropoutPair local; local.cells = kCells; local.gone = 3;
            DropoutPair local_back; local_back.cells = kCells; local_back.back = 3;
            for (unsigned i = 0; i < 40; ++i) // the view changed on every one of these pairs
                CHECK(!w.Observe(i % 2 ? local_back : local, kSamples[i % 16], kSamples[(i + 1) % 16], false));
            CHECK(w.Current() == DropoutWatch::Phase::Watch && w.Onsets() == 0);
        }},
        {"two isolated events far apart are forgotten", [] {
            DropoutWatch w;
            DropoutPair gone; gone.cells = kCells; gone.gone = 2;
            DropoutPair back; back.cells = kCells; back.back = 2;
            const DropoutPair none{0, 0, kCells};
            for (unsigned round = 0; round < 6; ++round) {
                CHECK(!w.Observe(gone, kSamples[1], kSamples[2], true));
                CHECK(!w.Observe(back, kSamples[2], kSamples[3], true));
                for (unsigned i = 0; i < DropoutWatch::kWindow + 2; ++i) CHECK(!w.Observe(none, kSamples[3], kSamples[5], true));
            }
            CHECK(w.Current() == DropoutWatch::Phase::Watch);
        }},
    });
}
