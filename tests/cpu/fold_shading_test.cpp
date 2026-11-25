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
    {"the screen keeps its mode until the new one has proven itself", [] {
        Handover h;
        auto s = h.Begin(Mode::Off, true);
        CHECK(s.show == Mode::Off && s.warm == Mode::Off && !s.switched);
        for (uint32_t i = 0; i < Handover::kWarmFrames; ++i) {
            s = h.Begin(Mode::Performance, true);
            CHECK(s.show == Mode::Off && s.warm == Mode::Performance && !s.switched && !s.show_reset);
            CHECK(s.warm_reset == (i == 0));
            h.WarmResult(true);
        }
        s = h.Begin(Mode::Performance, true);
        CHECK(s.show == Mode::Performance && s.warm == Mode::Off && s.switched && !s.show_reset);
        s = h.Begin(Mode::Performance, true);
        CHECK(s.show == Mode::Performance && !s.switched);
        // one fold to the other: the first stays on screen while the second warms
        s = h.Begin(Mode::Quality, true);
        CHECK(s.show == Mode::Performance && s.warm == Mode::Quality);
        // back to DLAA is immediate, with a new history
        s = h.Begin(Mode::Off, true);
        CHECK(s.show == Mode::Off && s.switched && s.show_reset);
        CHECK(h.switches == 2);
    }},
    {"a failed warm-up frame starts the count again and never reaches the screen", [] {
        Handover h;
        for (uint32_t i = 0; i < 5; ++i) { h.Begin(Mode::Quality, true); h.WarmResult(true); }
        auto s = h.Begin(Mode::Quality, true); h.WarmResult(false);
        CHECK(s.show == Mode::Off && h.good == 0);
        s = h.Begin(Mode::Quality, true);
        CHECK(s.show == Mode::Off && s.warm == Mode::Quality && s.warm_reset);
    }},
    {"a mode that keeps failing is refused and DLAA stays", [] {
        Handover h;
        for (uint32_t i = 0; i < Handover::kGiveUp; ++i) { const auto s = h.Begin(Mode::Performance, true); CHECK(s.show == Mode::Off); h.WarmResult(false); }
        CHECK(h.refused == Mode::Performance && h.refusals == 1);
        auto s = h.Begin(Mode::Performance, true);
        CHECK(s.show == Mode::Off && s.warm == Mode::Off); // no more attempts for the same request
        s = h.Begin(Mode::Quality, true);                  // another request is tried afresh
        CHECK(s.warm == Mode::Quality && h.refused == Mode::Off);
        h.Begin(Mode::Off, true);
        s = h.Begin(Mode::Performance, true);              // asking again after DLAA tries again
        CHECK(s.warm == Mode::Performance);
    }},
    {"a picture the fold cannot run on stays on DLAA", [] {
        Handover h;
        const auto s = h.Begin(Mode::Performance, false);
        CHECK(s.show == Mode::Off && s.warm == Mode::Off);
    }},
}); }
