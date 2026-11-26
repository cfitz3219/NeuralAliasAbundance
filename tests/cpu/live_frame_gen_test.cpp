// Live Frame Generation on Route B: menu choice, presenting thread, pacing, HUD sheet learning
// by shape and the HUD paste rule (core/live_frame_gen.hpp).
#include "test_support.hpp"
#include "core/live_frame_gen.hpp"
#include <cmath>
#include <cstring>
#include <vector>
using namespace na::fglive;

namespace {
struct Out { uint64_t id; uint32_t w, h; };
struct Op {
    bool compute = false, transfer = false, blend = false, depth = false;
    std::vector<Out> out;
};
Op draw(uint64_t rt, uint32_t w, uint32_t h, bool blend, bool depth) { Op o; o.blend = blend; o.depth = depth; o.out.push_back({rt, w, h}); return o; }
Op copy(uint64_t dst, uint32_t w, uint32_t h) { Op o; o.transfer = true; o.out.push_back({dst, w, h}); return o; }
Op dispatch(uint64_t dst, uint32_t w, uint32_t h) { Op o; o.compute = true; o.out.push_back({dst, w, h}); return o; }
constexpr uint64_t kShown = 900, kSheet = 77, kScene = 5;
constexpr uint32_t W = 3440, H = 1440;
// A typical traced frame: scene, post, a long HUD run, the final draw.
std::vector<Op> typical(uint32_t hud_draws)
{
    std::vector<Op> ops;
    ops.push_back(draw(kScene, W, H, false, true));
    ops.push_back(dispatch(kScene, W, H));
    for (uint32_t i = 0; i < hud_draws; ++i) ops.push_back(draw(kSheet, W, H, true, false));
    ops.push_back(draw(kShown, W, H, false, false)); // tone map + HUD composite into the shown picture
    return ops;
}
}

int main() { return test::run({
    {"the menu choice is stored as the shown multiplier and clamped to this build", [] {
        CHECK(GeneratedFromStored(0) == 0 && GeneratedFromStored(1) == 0);
        CHECK(GeneratedFromStored(2) == 1);
        CHECK(GeneratedFromStored(3) == 1);       // x3 saved by a later build: this build offers x2 at most
        CHECK(GeneratedFromStored(3, 2) == 2);    // a build that offers x3 reads it back
        CHECK(StoredFromGenerated(0) == 0 && StoredFromGenerated(1) == 2 && StoredFromGenerated(2) == 3);
        for (uint32_t g = 0; g <= kMaxGeneratedThisBuild; ++g) CHECK(GeneratedFromStored(StoredFromGenerated(g)) == g);
        CHECK(!std::strcmp(ChoiceLabel(0), "Off") && !std::strcmp(ChoiceLabel(1), "x2"));
    }},
    {"presentation goes back to the game's thread only once the presenter thread is idle", [] {
        CHECK(NextOwner(false, Owner::GameThread, 0) == Owner::GameThread);
        CHECK(NextOwner(true, Owner::GameThread, 0) == Owner::PresenterThread);
        CHECK(NextOwner(true, Owner::PresenterThread, 3) == Owner::PresenterThread);
        CHECK(NextOwner(false, Owner::PresenterThread, 2) == Owner::PresenterThread); // still showing queued frames
        CHECK(NextOwner(false, Owner::PresenterThread, 0) == Owner::GameThread);      // drained: Off is today's path again
    }},
    {"pacing: x2 shows the generated picture at the anchor and the real one half a frame later", [] {
        const Plan off = PlanPictures(16.0, 0);
        CHECK(off.pictures == 1 && off.offset_ms[0] == 0.0);
        const Plan x2 = PlanPictures(16.0, 1);
        CHECK(x2.pictures == 2 && x2.offset_ms[0] == 0.0 && std::fabs(x2.offset_ms[1] - 8.0) < 1e-9);
        const Plan x3 = PlanPictures(15.0, 2);
        CHECK(x3.pictures == 3 && std::fabs(x3.offset_ms[1] - 5.0) < 1e-9 && std::fabs(x3.offset_ms[2] - 10.0) < 1e-9);
        // Even spacing over consecutive frames at a steady rate: 0, 8, 16, 24 ...
        double last = -8.0;
        for (int frame = 0; frame < 4; ++frame)
            for (uint32_t i = 0; i < x2.pictures; ++i) {
                const double t = frame * 16.0 + x2.offset_ms[i];
                CHECK(std::fabs(t - last - 8.0) < 1e-9);
                last = t;
            }
        CHECK(PlanPictures(-1.0, 1).offset_ms[1] == 0.0); // unknown interval: no invented delay
        CHECK(!GeneratedTooLate(3.0, 8.0) && GeneratedTooLate(9.0, 8.0));
        CHECK(!GeneratedTooLate(3.5, 2.0) && GeneratedTooLate(4.5, 2.0)); // 4 ms floor
    }},
    {"HUD sheet: found by shape in a typical frame", [] {
        const auto ops = typical(85);
        const Sheet s = LearnSheet(ops, kShown, W, H);
        CHECK(s.verdict == SheetVerdict::Found);
        CHECK(s.sheet == kSheet && s.run == 85 && s.other == 0 && s.final_op == ops.size() - 1);
    }},
    {"HUD sheet: refusals say why", [] {
        CHECK(LearnSheet(typical(85), 12345, W, H).verdict == SheetVerdict::NoShownWrite);
        CHECK(LearnSheet(typical(3), kShown, W, H).verdict == SheetVerdict::ShortRun);
        {   // the shown picture is filled by a copy
            auto ops = typical(85); ops.back() = copy(kShown, W, H);
            CHECK(LearnSheet(ops, kShown, W, H).verdict == SheetVerdict::ShownNotDrawn);
        }
        {   // the HUD is drawn straight into the shown picture: no separate sheet
            std::vector<Op> ops{draw(kScene, W, H, false, true), draw(kShown, W, H, false, false)};
            for (int i = 0; i < 20; ++i) ops.push_back(draw(kShown, W, H, true, false));
            const Sheet s = LearnSheet(ops, kShown, W, H);
            CHECK(s.verdict == SheetVerdict::NoRun);
        }
        {   // a smaller target before the final draw is not a screen-size sheet
            auto ops = typical(0);
            ops.insert(ops.end() - 1, 10, draw(kSheet, W / 2, H / 2, true, false));
            CHECK(LearnSheet(ops, kShown, W, H).verdict == SheetVerdict::NoRun);
        }
        {   // a depth-tested run is scene work, not a HUD
            auto ops = typical(0);
            ops.insert(ops.end() - 1, 10, draw(kSheet, W, H, true, true));
            CHECK(LearnSheet(ops, kShown, W, H).verdict == SheetVerdict::NoRun);
        }
        {   // the sheet is also written by an opaque pass earlier in the frame
            auto ops = typical(40);
            ops.insert(ops.begin() + 1, draw(kSheet, W, H, false, false));
            const Sheet s = LearnSheet(ops, kShown, W, H);
            CHECK(s.verdict == SheetVerdict::OtherWrites && s.other == 1);
        }
        {   // compute and copies between the HUD draws do not break the run
            auto ops = typical(10);
            ops.insert(ops.end() - 3, dispatch(9, 64, 64));
            CHECK(LearnSheet(ops, kShown, W, H).verdict == SheetVerdict::Found);
        }
        for (auto v : {SheetVerdict::Found, SheetVerdict::NoShownWrite, SheetVerdict::ShownNotDrawn, SheetVerdict::NoRun,
                       SheetVerdict::ShortRun, SheetVerdict::OtherWrites})
            CHECK(std::strlen(SheetWhy(v)) > 10);
    }},
    {"sheet state at the final draw: only states proven inside this recording, all agreeing", [] {
        constexpr uint32_t PSR = 0x80, NPSR_PSR = 0xC0, RT = 0x4;
        const StateSource known_psr{Lookup::Known, PSR}, absent{Lookup::Absent, 0}, unavailable{Lookup::Unavailable, 0};
        // (a) the game moves subresource 0 only: the whole-resource lookup has nothing, the
        // subresource-0 lookup and the tracker know it (Cort's 2026-11-26 run refused this case).
        SheetStateInputs a{true, absent, known_psr, known_psr};
        SheetState s = ResolveSheetState(a);
        CHECK(s.known && s.state == PSR && !std::strcmp(s.source, "subresource-0 transition"));
        // whole-resource transition known, others agree
        SheetStateInputs w{true, {Lookup::Known, NPSR_PSR}, {Lookup::Known, NPSR_PSR}, {Lookup::Known, NPSR_PSR}};
        CHECK(ResolveSheetState(w).known && ResolveSheetState(w).state == NPSR_PSR);
        // only the independent tracker saw it (the main record was incomplete)
        SheetStateInputs t{true, unavailable, unavailable, known_psr};
        CHECK(ResolveSheetState(t).known && !std::strcmp(ResolveSheetState(t).source, "barrier tracker"));
        // (b) the HUD drawn in another list than the final draw: never resolved at recording time
        SheetStateInputs b{false, known_psr, known_psr, known_psr};
        s = ResolveSheetState(b);
        CHECK(!s.known && std::strstr(s.why, "another command list"));
        // disagreement is refused
        SheetStateInputs d{true, {Lookup::Known, PSR}, {Lookup::Known, NPSR_PSR}, absent};
        CHECK(!ResolveSheetState(d).known && std::strstr(ResolveSheetState(d).why, "disagree"));
        // nothing seen at all although drawn and read in this list: an unobserved barrier method
        SheetStateInputs n{true, absent, absent, absent};
        CHECK(!ResolveSheetState(n).known && std::strstr(ResolveSheetState(n).why, "not observed"));
        // incomplete records
        SheetStateInputs u{true, unavailable, absent, unavailable};
        CHECK(!ResolveSheetState(u).known && std::strstr(ResolveSheetState(u).why, "incomplete"));
        // COMMON and render-target states are never used
        SheetStateInputs c{true, {Lookup::Known, 0}, absent, absent};
        CHECK(!ResolveSheetState(c).known && std::strstr(ResolveSheetState(c).why, "COMMON"));
        SheetStateInputs r{true, {Lookup::Known, RT}, absent, absent};
        CHECK(!ResolveSheetState(r).known && std::strstr(ResolveSheetState(r).why, "render target"));
    }},
    {"HUD paste: real pixels within one pixel of a HUD difference replace the generated picture", [] {
        const uint32_t w = 7, h = 5;
        std::vector<float> real(w * h * 3, 0.2f), hf(w * h * 3, 0.2f), gen(w * h * 3, 0.5f);
        // one HUD pixel at (3, 2), and a sub-threshold difference at (0, 0)
        real[(2 * w + 3) * 3 + 1] = 0.9f;
        real[0] = 0.2f + 0.001f;
        PasteHud(real.data(), hf.data(), gen.data(), w, h);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const bool inside = x >= 2 && x <= 4 && y >= 1 && y <= 3;
                const float g = gen[(y * w + x) * 3 + 2];
                CHECK(inside ? g == 0.2f : g == 0.5f);
            }
        // display terms: HDR values above 1.0 clip, so two bright values do not count as HUD
        const float a[3] = {4.f, 4.f, 4.f}, b[3] = {6.f, 6.f, 6.f};
        CHECK(!HudPixel(a, b));
        const float c[3] = {0.f, 0.f, 0.f}, d[3] = {-0.5f, 0.f, 0.f}; // negative scRGB clips at 0
        CHECK(!HudPixel(c, d));
        // the threshold is 4/255 in display terms
        const float e[3] = {0.5f, 0.5f, 0.5f};
        float f[3] = {0.5f, 0.5f, 0.5f};
        f[0] = std::pow(DisplayValue(0.5f) + 3.f / 255.f, 2.2f);
        CHECK(!HudPixel(e, f));
        f[0] = std::pow(DisplayValue(0.5f) + 5.f / 255.f, 2.2f);
        CHECK(HudPixel(e, f));
    }},
}); }
