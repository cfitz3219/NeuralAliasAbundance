// Which draws draw replay keeps replaying (core/replay_movers.hpp; Cort 2026-11-17 "Yes on one"):
// a draw seen moving apart from the screen three times is a mover, a still scene never is, not
// being seen teaches nothing quickly, a mover that stands still is let go after `top` frames, and
// with the admission of producers.cpp a frame's cost falls to the movers plus a small rotating share.
#include "test_support.hpp"
#include "core/replay_budget.hpp"
#include "core/replay_movers.hpp"
#include <vector>

using namespace na::replay_movers;
namespace rb = na::replay_budget;

int main()
{
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
    };
    const Params p;

    // One frame's verdict.
    check(Judge(0, 0, p) == Seen::Unseen, "no visible sample: not seen");
    check(Judge(400, 0, p) == Seen::Still, "visible, nothing apart: still");
    check(Judge(400, 1, p) == Seen::Still, "one stray sample is not a moving frame");
    check(Judge(400, 19, p) == Seen::Still, "under a twentieth of the draw apart: still");
    check(Judge(400, 20, p) == Seen::Moving, "a twentieth of the draw apart: moving");
    check(Judge(3, 2, p) == Seen::Moving, "a small draw needs two samples");

    { // a still scene: never remembered, never a mover
        Table t;
        for (uint64_t f = 1; f <= 5000; ++f) t.Report(1, 2, 300, 0, f);
        check(!t.IsMover(1, 2) && t.Movers() == 0 && t.Suspects() == 0, "a still draw is never a mover and takes no memory");
    }
    { // three moving frames make a mover; two do not
        Table t;
        t.Report(1, 2, 300, 100, 1);
        t.Report(1, 2, 300, 100, 2);
        check(!t.IsMover(1, 2) && t.Suspects() == 1, "two moving frames: a suspect, not yet a mover");
        check(t.Report(1, 2, 300, 100, 3) == Change::Promoted && t.IsMover(1, 2), "the third moving frame makes it a mover");
        check(!t.IsMover(1, 3) && !t.IsMover(2, 2), "another draw with the same signature or the same buffers is not");
    }
    { // one odd frame (a wrong screen motion for a frame) is forgotten
        Table t;
        t.Report(7, 7, 300, 300, 1);
        for (uint64_t f = 2; f <= 301; ++f) t.Report(7, 7, 300, 0, f);
        check(!t.IsMover(7, 7) && t.Suspects() == 0, "a single moving frame followed by stillness leaves nothing");
    }
    { // a mover standing still: kept for a while, then let go; moving again in between keeps it
        Table t;
        for (uint64_t f = 1; f <= 10; ++f) t.Report(5, 6, 300, 100, f); // score at the ceiling
        uint64_t f = 11;
        for (; f < 11 + p.top - 1; ++f) t.Report(5, 6, 300, 0, f);
        check(t.IsMover(5, 6), "a mover is still one just before `top` still frames");
        check(t.Report(5, 6, 300, 0, f) == Change::LetGo && !t.IsMover(5, 6) && t.LetGo() == 1, "and is let go at `top` still frames");
        for (uint64_t k = 0; k < 3; ++k) t.Report(5, 6, 300, 100, f + 1 + k);
        check(t.IsMover(5, 6) && t.Promoted() == 2, "it is a mover again after three moving frames");
    }
    { // replayed but not seen (behind a wall): four times slower than being seen still
        Table t;
        for (uint64_t f = 1; f <= 10; ++f) t.Report(5, 6, 300, 100, f);
        for (uint64_t f = 11; f < 11 + 3 * p.top; ++f) t.Report(5, 6, 0, 0, f);
        check(t.IsMover(5, 6), "a mover out of sight for three times `top` frames is kept");
        for (uint64_t f = 11 + 3 * p.top; f <= 11 + 4 * p.top; ++f) t.Report(5, 6, 0, 0, f);
        check(!t.IsMover(5, 6), "and let go after four times `top`");
    }
    { // memory is bounded, and movers are the last to be dropped
        Params q;
        q.capacity = 64;
        Table t(q);
        for (uint64_t k = 0; k < 8; ++k)
            for (uint64_t f = 1; f <= 3; ++f) t.Report(100 + k, 1, 300, 100, f); // eight movers, reported long ago
        for (uint64_t k = 0; k < 1000; ++k) t.Report(1000 + k, 1, 300, 100, 10 + k); // a stream of suspects
        bool all = true;
        for (uint64_t k = 0; k < 8; ++k) all = all && t.IsMover(100 + k, 1);
        check(all && t.Movers() == 8, "old movers survive a stream of new suspects");
        check(t.Movers() + t.Suspects() <= q.capacity, "the table stays within its capacity");
    }
    { // The admission as producers.cpp runs it: movers first, a quarter of the budget for the rest.
        // 3,800 draws of about 4,000 vertices (15 million), 40 of them move.
        struct Draw { uint64_t sig, vb, verts; bool moves; };
        std::vector<Draw> d;
        uint64_t s = 99;
        for (uint32_t i = 0; i < 3800; ++i) { s = rb::Mix(s, i); d.push_back({rb::Mix(s, 1), rb::Mix(s, 2), 2000 + s % 4000, i % 95 == 0}); }
        const uint64_t budget = 1000000, probe = budget / 4;
        const uint32_t rotate = 24;
        Table t;
        uint32_t threshold = rb::kFull, phase = 0;
        uint64_t used_last = 0, movers_found_at = 0;
        std::vector<char> prev(d.size(), 0), cur(d.size(), 0);
        for (uint64_t frame = 1; frame <= 6000; ++frame) {
            if (frame % rotate == 0) phase = rb::NextPhase(phase, threshold); // the next slice of draws
            uint64_t mover_used = 0, probe_used = 0, offered = 0;
            bool full = false;
            for (size_t i = 0; i < d.size(); ++i) {
                cur[i] = 0;
                if (t.IsMover(d[i].sig, d[i].vb)) {
                    if (mover_used + d[i].verts <= budget) { mover_used += d[i].verts; cur[i] = 1; }
                    continue;
                }
                const rb::Verdict v = rb::DecideWithin(rb::SweepValue(d[i].sig, d[i].vb, phase), threshold, d[i].verts, probe_used,
                                                       rb::DrawLimit(budget), rb::ProbeFrameLimit(budget, probe));
                if (v == rb::Verdict::DrawTooLarge) continue;
                offered += d[i].verts;
                if (v == rb::Verdict::FrameFull) full = true;
                if (v == rb::Verdict::Admit) { probe_used += d[i].verts; cur[i] = 1; }
            }
            // The replay pass pairs a draw with its twin of the frame before and reports what it saw.
            for (size_t i = 0; i < d.size(); ++i)
                if (cur[i] && prev[i]) t.Report(d[i].sig, d[i].vb, 200, d[i].moves ? 60 : 0, frame);
            threshold = rb::NextThreshold(threshold, offered, probe_used, probe, full);
            used_last = mover_used + probe_used;
            if (!movers_found_at && t.Movers() == 40) movers_found_at = frame;
            prev.swap(cur);
        }
        size_t wrong = 0;
        for (const Draw& x : d) if (t.IsMover(x.sig, x.vb) != x.moves) ++wrong;
        check(t.Movers() == 40 && wrong == 0, "every moving draw is found and no still one is taken for one");
        check(movers_found_at && movers_found_at <= 3000, "all of them within 3,000 frames");
        check(used_last < budget / 2, "the frame then replays under half the budget: the movers and the rotating share");
        std::printf("movers found by frame %llu; vertices a frame at the end %llu (budget %llu)\n",
                    (unsigned long long)movers_found_at, (unsigned long long)used_last, (unsigned long long)budget);
    }

    std::printf("replay_movers: %s (%d failures)\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
}
