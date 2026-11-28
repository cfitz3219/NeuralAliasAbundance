// Scale hunt: memory matchers, the shared board and the response judge (core/scale_hunt.hpp).
#include "test_support.hpp"
#include "core/scale_hunt.hpp"
#include <cstring>
#include <vector>
using namespace na::scale::hunt;

namespace {
template <class T> void Put(std::vector<uint8_t>& m, size_t at, T v) { std::memcpy(&m[at], &v, sizeof v); }

// A pretend game for the judge: how it answers a command.
struct Game {
    uint32_t out_w = 3440, out_h = 1440;
    double scale = 1.0;        // what it draws at now
    double pending = 0;        // commanded, not applied yet
    int delay = 0, countdown = -1;
    double step = 0;           // snaps to multiples of this (0 = exact)
    bool deaf = false;         // ignores commands
    int revert_after = 0;      // frames after applying before it puts 1.0 back (0 = never)
    bool own_clock = false;    // reverts on its own timer, whatever we do
    int since_apply = 0;
    uint64_t frame = 0;
    void Command(double s)
    {
        if (deaf) return;
        pending = step > 0 ? std::round(s / step) * step : s;
        if (countdown < 0) countdown = delay;
    }
    Frame Step(bool world = true)
    {
        ++frame;
        if (countdown == 0) { scale = pending; since_apply = 0; }
        if (countdown >= 0) --countdown;
        ++since_apply;
        if (revert_after && scale != 1.0 && since_apply > revert_after) scale = 1.0;
        if (own_clock && revert_after && frame % uint64_t(revert_after) == 0) scale = 1.0;
        Frame f; f.index = frame; f.world_seen = world;
        f.out_w = out_w; f.out_h = out_h;
        f.world_w = uint32_t(std::lround(out_w * scale)); f.world_h = uint32_t(std::lround(out_h * scale));
        return f;
    }
};
// Runs the Referee's loop: observe, then command when the judge asks. Returns commands issued.
int Run(Game& g, ResponseJudge& j, int frames, int menu_from = -1, int menu_to = -1)
{
    int commands = 0;
    for (int i = 0; i < frames; ++i) {
        const Frame f = g.Step(!(i >= menu_from && i < menu_to));
        j.Observe(f);
        if (j.NeedsCommand(f.index)) { g.Command(j.Wanted()); j.Commanded(f.index); ++commands; }
    }
    return commands;
}
} // namespace

int main() { return test::run({
    {"memory: the world size side by side, as whole numbers and as floats", [] {
        std::vector<uint8_t> m(256, 0);
        Put<uint32_t>(m, 16, 2293); Put<uint32_t>(m, 20, 960);
        Put<float>(m, 64, 2293.f); Put<float>(m, 68, 960.f);
        Put<uint32_t>(m, 128, 2293); Put<uint32_t>(m, 132, 961); // not the pair
        std::vector<Hit> hits;
        const Target t{2293, 960, 3440, 1440};
        SweepBlock(m.data(), m.size(), 0x1000, t, {}, hits);
        int pi = 0, pf = 0;
        for (const Hit& h : hits) {
            if (h.kind == Kind::SizePairI32) { ++pi; CHECK(h.address == 0x1010); }
            if (h.kind == Kind::SizePairF32) { ++pf; CHECK(h.address == 0x1040); }
        }
        CHECK(pi == 1 && pf == 1);
    }},
    {"memory: the scale as float, double, percent; nothing in bulk at full size", [] {
        std::vector<uint8_t> m(256, 0);
        Put<float>(m, 8, 0.6667f); Put<double>(m, 32, 2293.0 / 3440.0); Put<int32_t>(m, 80, 67); Put<float>(m, 96, 66.7f);
        Put<float>(m, 120, 0.70f); // close, but not it
        std::vector<Hit> hits;
        const Target t{2293, 960, 3440, 1440};
        SweepBlock(m.data(), m.size(), 0, t, {}, hits);
        bool f32 = false, f64 = false, pct = false, pctf = false, wrong = false;
        for (const Hit& h : hits) {
            f32 |= h.kind == Kind::ScaleF32 && h.address == 8;
            f64 |= h.kind == Kind::ScaleF64 && h.address == 32;
            pct |= h.kind == Kind::PercentI32 && h.address == 80;
            pctf |= h.kind == Kind::PercentF32 && h.address == 96;
            wrong |= h.address == 120;
        }
        CHECK(f32 && f64 && pct && pctf && !wrong);
        // At full size a 1.0 is everywhere: only size pairs are collected.
        std::vector<uint8_t> ones(256, 0);
        for (size_t i = 0; i < 256; i += 4) Put<float>(ones, i, 1.0f);
        Put<uint32_t>(ones, 40, 3440); Put<uint32_t>(ones, 44, 1440);
        hits.clear();
        SweepBlock(ones.data(), ones.size(), 0, Target{3440, 1440, 3440, 1440}, {}, hits);
        CHECK(hits.size() == 1 && hits[0].kind == Kind::SizePairI32 && hits[0].address == 40);
    }},
    {"board: the address that follows the world outranks chance matches and timers", [] {
        std::vector<uint8_t> m(256, 0);
        auto holds = [&](uint64_t a, Kind k, const Target& t) { return a + 8 <= m.size() && Holds(m.data() + a, m.size() - a, k, t); };
        Target t{2293, 960, 3440, 1440};
        Put<float>(m, 8, 0.6667f);   // the real scale
        Put<float>(m, 40, 0.6667f);  // a chance match (never changes)
        Put<float>(m, 72, 0.6667f);  // a timer (changes on its own)
        Put<uint32_t>(m, 100, 2293); Put<uint32_t>(m, 104, 960); // the render size it produces
        Board b;
        std::vector<Hit> hits;
        SweepBlock(m.data(), m.size(), 0, t, {}, hits);
        for (const Hit& h : hits) b.Report(h.address, h.kind, kMemory, true);
        b.Report(8, Kind::ScaleF32, kCallChain, true); // a second method agrees on the real one
        b.Report(72, Kind::ScaleF32, kCallChain, true);
        Put<float>(m, 72, 0.9f);
        b.Steady(t, holds); // the timer moved while the world did not
        // The slider moves to 50%.
        t = Target{1720, 720, 3440, 1440};
        Put<float>(m, 8, 0.5f); Put<uint32_t>(m, 100, 1720); Put<uint32_t>(m, 104, 720);
        b.SizeChanged(t, holds);
        const auto top = b.Ranked(3);
        CHECK(top.size() == 3);
        CHECK(top[0].address == 8 && top[0].Confirmed());
        CHECK(top[1].address == 100 && top[1].kind == Kind::SizePairI32 && top[1].Confirmed());
        CHECK(!top[2].Confirmed());
        CHECK(b.ConfirmedCount() == 2);
        // The chance match was found by memory only and missed: forgotten.
        bool chance = false;
        for (const auto& e : b.Ranked(100)) chance |= e.address == 40;
        CHECK(!chance);
    }},
    {"judge: a game that answers after a delay is commanded once", [] {
        Game g; g.delay = 40;
        ResponseJudge j; j.Ask(0.667, 0);
        const int commands = Run(g, j, 300);
        CHECK(j.State() == Verdict::Responded && j.HoldPolicy() == Hold::Once && commands == 1);
        NEAR(j.Achieved(), 0.667, 0.012);
    }},
    {"judge: a game that never answers is reported, not retried forever", [] {
        Game g; g.deaf = true;
        ResponseJudge j; j.Ask(0.667, 0);
        const int commands = Run(g, j, 600);
        CHECK(j.State() == Verdict::Ignored && commands == 1);
    }},
    {"judge: menus and loading screens are not evidence either way", [] {
        Game g; g.delay = 400; // answers long after the deadline, but the wait was a loading screen
        ResponseJudge j; j.Ask(0.5, 0);
        Run(g, j, 700, 5, 395);
        CHECK(j.State() == Verdict::Responded);
    }},
    {"judge: a game that snaps to its own steps is accepted at that step", [] {
        Game g; g.step = 0.05; g.delay = 2;
        ResponseJudge j; j.Ask(0.667, 0);
        const int commands = Run(g, j, 300);
        CHECK(j.State() == Verdict::Snapped && commands == 1);
        NEAR(j.Achieved(), 0.65, 0.005);
    }},
    {"judge: a game that reverts after a while gets the command again, then ahead of time", [] {
        Game g; g.revert_after = 300;
        ResponseJudge j; j.Ask(0.5, 0);
        Run(g, j, 1000);
        CHECK(j.HoldPolicy() == Hold::Periodic && j.Reverts() == 2);
        const uint32_t before = j.Reverts();
        Run(g, j, 5000); // commanded before each revert from here on: no more reverts
        CHECK(j.Reverts() == before);
        CHECK(j.State() == Verdict::Responded && j.Period() > 0 && j.Period() < 300);
    }},
    {"judge: a game that reverts at once is commanded every frame", [] {
        Game g; g.revert_after = 1;
        ResponseJudge j; j.Ask(0.5, 0);
        Run(g, j, 200);
        CHECK(j.HoldPolicy() == Hold::EveryFrame);
        const uint32_t before = j.Reverts();
        const int commands = Run(g, j, 500);
        CHECK(commands == 500 && j.Reverts() == before && j.State() == Verdict::Responded);
    }},
    {"judge: asking for nothing stops the commands", [] {
        Game g; ResponseJudge j; j.Ask(0.5, 0);
        Run(g, j, 50);
        j.Ask(0, 50);
        CHECK(Run(g, j, 50) == 0 && j.State() == Verdict::Idle);
    }},
}); }
