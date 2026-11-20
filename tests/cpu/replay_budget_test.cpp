// Draw replay's vertex budget (core/replay_budget.hpp): a frame replays about the budget, the same
// draws in adjacent frames, a subset when the threshold comes down, every draw over time.
#include "test_support.hpp"
#include "core/replay_budget.hpp"
#include <vector>

using namespace na::replay_budget;

namespace {
struct Draw { uint64_t sig, vb, verts; };
// A frame like the one in the log: thousands of draws, a few very large, about `total` vertices.
std::vector<Draw> Scene(uint32_t draws, uint64_t seed)
{
    std::vector<Draw> d;
    d.push_back({11, 12, 900000}); // one instanced draw larger than half the budget
    uint64_t s = seed;
    for (uint32_t i = 0; i < draws; ++i) {
        s = Mix(s, i);
        const uint64_t verts = 300 + (s % 4000) + ((s >> 20) % 97 == 0 ? 150000 : 0);
        d.push_back({Mix(s, 1), Mix(s, 2), verts});
    }
    return d;
}
struct Frame { uint64_t used = 0, offered = 0; uint32_t admitted = 0, too_large = 0, full = 0; std::vector<char> in; };
Frame Play(const std::vector<Draw>& d, uint64_t frame, uint32_t threshold, uint64_t budget, uint32_t rotate)
{
    Frame f;
    f.in.assign(d.size(), 0);
    for (size_t i = 0; i < d.size(); ++i) {
        const Verdict v = Decide(DrawValue(d[i].sig, d[i].vb, frame, rotate), threshold, d[i].verts, f.used, budget);
        if (v == Verdict::DrawTooLarge) { ++f.too_large; continue; }
        if (v == Verdict::FrameFull) { ++f.full; break; } // the caller stops asking for the rest of the frame
        f.offered += d[i].verts;
        if (v == Verdict::Admit) { f.used += d[i].verts; ++f.admitted; f.in[i] = 1; }
    }
    return f;
}
}

int main()
{
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
    };
    const uint64_t budget = 1000000;
    const uint32_t rotate = 240;
    const std::vector<Draw> scene = Scene(3800, 7);
    uint64_t total = 0;
    for (const Draw& d : scene) total += d.verts;

    { // no budget: everything is admitted, as before
        const Frame f = Play(scene, 10, kFull, 0, rotate);
        check(f.admitted == scene.size() && f.used == total, "a budget of 0 admits every draw");
    }
    { // the threshold settles so a frame replays about the budget, and never over 5/4 of it
        uint32_t t = kFull;
        Frame f;
        uint64_t worst = 0;
        for (uint64_t frame = 1; frame <= 30; ++frame) {
            f = Play(scene, frame, t, budget, rotate);
            if (f.used > worst) worst = f.used;
            t = NextThreshold(t, f.offered, f.used, budget, f.full != 0);
        }
        std::printf("scene %llu verts in %zu draws: settled at %llu verts in %u draws, threshold %u\n",
                    (unsigned long long)total, scene.size(), (unsigned long long)f.used, f.admitted, t);
        check(worst <= FrameLimit(budget), "no frame goes over 5/4 of the budget, the first included");
        check(f.used > budget * 7 / 10 && f.used <= FrameLimit(budget), "the settled frame replays about the budget");
        check(f.full == 0, "the settled frame does not hit its limit");
        check(f.too_large == 1, "the draw larger than half the budget is refused by itself");
        // adjacent frames admit the same draws
        const Frame g = Play(scene, 13, t, budget, rotate);
        check(g.in == f.in, "the next frame admits exactly the same draws");
        // a lower threshold admits a subset
        const Frame h = Play(scene, 14, t / 2, budget, rotate);
        bool subset = true;
        for (size_t i = 0; i < scene.size(); ++i) subset = subset && (!h.in[i] || f.in[i]);
        check(subset && h.admitted < f.admitted && h.admitted > 0, "a lower threshold admits a subset of the same draws");
        // rotation: over periods, every draw that fits is tried
        std::vector<char> ever(scene.size(), 0);
        for (uint64_t period = 0; period < 200; ++period) {
            Frame r;
            uint32_t tp = t;
            for (uint64_t k = 0; k < 8; ++k) { // the threshold settles again inside each period
                r = Play(scene, period * rotate + k, tp, budget, rotate);
                tp = NextThreshold(tp, r.offered, r.used, budget, r.full != 0);
            }
            for (size_t i = 0; i < scene.size(); ++i) ever[i] = ever[i] || r.in[i];
        }
        size_t tried = 0, could = 0;
        for (size_t i = 0; i < scene.size(); ++i) { if (scene[i].verts <= DrawLimit(budget)) { ++could; tried += ever[i] ? 1 : 0; } }
        std::printf("coverage over periods: %zu of %zu draws\n", tried, could);
        check(tried * 100 >= could * 98, "over 200 rotation periods at least 98 % of the draws were replayed at some time");
    }
    { // a small scene under the budget is replayed whole
        const std::vector<Draw> small = Scene(150, 3);
        std::vector<Draw> s(small.begin() + 1, small.end());
        uint32_t t = 9000; // coming from a heavy view
        Frame f = Play(s, 1, t, budget, rotate);
        t = NextThreshold(t, f.offered, f.used, budget, false);
        f = Play(s, 2, t, budget, rotate);
        check(t == kFull && f.admitted == s.size(), "a light view goes back to replaying every draw in one frame");
    }
    { // the threshold's own rules
        check(NextThreshold(kFull, 4000000, 0, 1000000, false) == kFull / 4, "four times the budget offered and nothing admitted yet: a quarter");
        check(NextThreshold(16384, 4000000, 2000000, 1000000, false) == 8192, "twice the budget admitted: half the threshold");
        check(NextThreshold(16384, 4000000, 1050000, 1000000, false) == 16384, "within a tenth of the budget: left alone");
        check(NextThreshold(16384, 4000000, 10, 1000000, false) == 32768, "one odd frame moves it by a factor of two at most");
        check(NextThreshold(20000, 0, 0, 1000000, false) == 20000, "nothing offered keeps the threshold");
        check(NextThreshold(20000, 500000, 1250000, 1000000, true) == 15000, "a frame that hit its limit comes down a quarter at least");
        check(NextThreshold(1, 1ull << 50, 1ull << 40, 1000000, true) == 1, "never below 1");
        check(NextThreshold(123, 5, 5, 0, false) == kFull, "no budget: every draw");
    }
    std::printf("replay_budget: %s (%d failures)\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
}
