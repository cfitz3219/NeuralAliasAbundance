// Fold the shading: the arithmetic and the hand-over rule (core/fold_shading.hpp).
#include "test_support.hpp"
#include "core/fold_shading.hpp"
#include <cmath>
using namespace na::fold;
int main() { return test::run({
    {"blocks, sizes and shading rates", [] {
        CHECK(BlockFor(Mode::Off).x == 1 && BlockFor(Mode::Off).y == 1);
        CHECK(BlockFor(Mode::Quality).x == 2 && BlockFor(Mode::Quality).y == 1);
        CHECK(BlockFor(Mode::Performance).x == 2 && BlockFor(Mode::Performance).y == 2);
        CHECK(ShadingRateFor(Mode::Off) == 0 && ShadingRateFor(Mode::Quality) == 4 && ShadingRateFor(Mode::Performance) == 5);
        CHECK(Folded(3440, 2) == 1720 && Folded(1441, 2) == 720 && Folded(1440, 1) == 1440);
        CHECK(JitterPhases(Mode::Off, 16) == 16 && JitterPhases(Mode::Quality, 16) == 16 && JitterPhases(Mode::Performance, 16) == 32);
    }},
    {"one fixed pixel of each block is kept: the last", [] {
        CHECK(Pick(Mode::Performance).x == 1 && Pick(Mode::Performance).y == 1);
        CHECK(Pick(Mode::Quality).x == 1 && Pick(Mode::Quality).y == 0);
        CHECK(Pick(Mode::Off).x == 0 && Pick(Mode::Off).y == 0);
    }},
    {"declared jitter matches where the kept pixel really is", [] {
        // Folded pixel X, block b, kept pixel p: the sample is at full-size b*X + p + 0.5 - j. DLSS
        // reads the declaration as: sample = (X + 0.5 - J) folded pixels = b*(X + 0.5 - J) full-size.
        for (uint32_t b : {1u, 2u}) for (uint32_t p = 0; p < b; ++p) for (float j : {-0.9f, -0.4f, 0.f, 0.3f}) {
            const float X = 5;
            const float J = DeclaredJitter(b, p, j);
            CHECK(std::fabs(b * (X + 0.5f - J) - (b * X + p + 0.5f - j)) < 1e-5f);
        }
        CHECK(DeclaredJitter(1, 0, 0.25f) == 0.25f); // DLAA is unchanged
        // With the draws centred on the kept pixel, DLSS is told the plain sample, inside its half-pixel range.
        for (Mode m : {Mode::Off, Mode::Quality, Mode::Performance}) for (float j : {-0.5f, -0.2f, 0.f, 0.5f}) {
            const Block b = BlockFor(m), p = Pick(m);
            CHECK(std::fabs(DeclaredJitter(b.x, p.x, b.x * j + KeptPixelCentring(b.x, p.x)) - j) < 1e-6f);
            CHECK(std::fabs(DeclaredJitter(b.y, p.y, b.y * j + KeptPixelCentring(b.y, p.y)) - j) < 1e-6f);
        }
        CHECK(KeptPixelCentring(1, 0) == 0.f);
    }},
    {"while a level warms beside another, both are told jitter inside the half-pixel range", [] {
        // The frame is drawn with JitterSpacing(shown, warming); each level declares its own jitter from it.
        for (Mode shown : {Mode::Off, Mode::Quality, Mode::Performance})
            for (Mode warm : {Mode::Off, Mode::Quality, Mode::Performance}) {
                const Spacing sp = JitterSpacing(shown, warm);
                for (float j : {-0.5f, -0.25f, 0.f, 0.25f, 0.49f}) {
                    const float fx = sp.block.x * j + KeptPixelCentring(sp.block.x, sp.pick.x);
                    const float fy = sp.block.y * j + KeptPixelCentring(sp.block.y, sp.pick.y);
                    for (Mode m : {shown, warm}) {
                        if (m == warm && warm == Mode::Off) continue; // nothing warms (DLAA is never warmed: going back is immediate)
                        const Block b = BlockFor(m), p = Pick(m);
                        CHECK(std::fabs(DeclaredJitter(b.x, p.x, fx)) <= 0.5f + 1e-6f);
                        CHECK(std::fabs(DeclaredJitter(b.y, p.y, fy)) <= 0.5f + 1e-6f);
                    }
                }
            }
        // Alone (or with DLAA beside it), a level keeps its own spacing; DLAA is unchanged.
        CHECK(JitterSpacing(Mode::Performance, Mode::Off).block.y == 2 && JitterSpacing(Mode::Off, Mode::Quality).block.x == 1);
        CHECK(JitterSpacing(Mode::Performance, Mode::Quality).block.x == 2 && JitterSpacing(Mode::Performance, Mode::Quality).block.y == 1);
        CHECK(JitterPhases(JitterSpacing(Mode::Off, Mode::Off), 16) == 16 && JitterPhases(JitterSpacing(Mode::Performance, Mode::Off), 16) == 32);
    }},
    {"the screen keeps its mode until everything the new one needs exists", [] {
        Handover h;
        auto s = h.Begin(Mode::Off, true, true);
        CHECK(s.show == Mode::Off && s.prepare == Mode::Off && !s.switched);
        for (uint32_t i = 0; i < 5; ++i) { // its planes, motion working set and DLSS feature are being made
            s = h.Begin(Mode::Performance, true, false);
            CHECK(s.show == Mode::Off && s.prepare == Mode::Performance && !s.switched && !s.show_reset);
            CHECK(h.waited == i + 1);
        }
        s = h.Begin(Mode::Performance, true, true); // all made: the screen changes now, on a new history
        CHECK(s.show == Mode::Performance && s.prepare == Mode::Off && s.switched && s.show_reset);
        s = h.Begin(Mode::Performance, true, false);
        CHECK(s.show == Mode::Performance && !s.switched);
        // one level to the other: the first stays on screen while the second is made
        s = h.Begin(Mode::Quality, true, false);
        CHECK(s.show == Mode::Performance && s.prepare == Mode::Quality);
        // back to DLAA waits for DLAA's feature the same way
        s = h.Begin(Mode::Off, true, false);
        CHECK(s.show == Mode::Performance && s.prepare == Mode::Off && !s.switched);
        s = h.Begin(Mode::Off, true, true);
        CHECK(s.show == Mode::Off && s.switched && s.show_reset);
        CHECK(h.switches == 2);
    }},
    {"a level NVIDIA refuses is given up on and the screen stays", [] {
        Handover h;
        h.Begin(Mode::Quality, true, false);
        h.Refuse(Mode::Quality);
        CHECK(h.refused == Mode::Quality && h.refusals == 1 && h.preparing == Mode::Off);
        auto s = h.Begin(Mode::Quality, true, true);
        CHECK(s.show == Mode::Off && s.prepare == Mode::Off && !s.switched); // no more attempts for the same request
        s = h.Begin(Mode::Performance, true, false);                       // another request is tried afresh
        CHECK(s.prepare == Mode::Performance && h.refused == Mode::Off);
        h.Begin(Mode::Off, true, true);
        s = h.Begin(Mode::Quality, true, false);                           // asking again after DLAA tries again
        CHECK(s.prepare == Mode::Quality);
    }},
    {"a level that fails on screen hands the screen back to DLAA", [] {
        Handover h;
        h.Begin(Mode::Performance, true, true);
        CHECK(h.shown == Mode::Performance);
        h.Refuse(Mode::Performance);
        CHECK(h.shown == Mode::Off && h.refused == Mode::Performance);
        const auto s = h.Begin(Mode::Performance, true, true);
        CHECK(s.show == Mode::Off && !s.switched);
    }},
    {"a picture the fold cannot run on goes back to DLAA at once", [] {
        Handover h;
        auto s = h.Begin(Mode::Performance, false, false);
        CHECK(s.show == Mode::Off && s.prepare == Mode::Off);
        h.Begin(Mode::Performance, true, true);
        s = h.Begin(Mode::Performance, false, false);
        CHECK(s.show == Mode::Off && s.switched);
    }},
}); }
