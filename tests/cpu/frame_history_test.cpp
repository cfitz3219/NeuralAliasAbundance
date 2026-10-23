// reconstructed 2026-10 — history generation and invalidation semantics.
#include "test_support.hpp"
#include "core/frame_history.hpp"
using namespace diagnostics;
namespace {
FrameObservation observation(uint64_t id, uint64_t time) { return {id, time, 640, 360, 1, 2, 3, true, true}; }
}
int main() { return test::run({
    {"first frame and continuous generation", [] {
        FrameHistory h; auto a = h.observe(observation(10, 100));
        CHECK(a.usable && a.reset && a.generation == 1 && a.reason == ResetReason::first_frame);
        a = h.observe(observation(11, 350)); CHECK(a.usable && !a.reset && a.generation == 1);
        a = h.observe(observation(12, 601)); CHECK(a.reset && a.generation == 2 && a.reason == ResetReason::discontinuity);
    }},
    {"gaps, repeats, rewind and time reversal", [] {
        for (const auto& bad : {observation(1, 110), observation(0, 110), observation(3, 110), observation(2, 99)}) {
            FrameHistory h; h.observe(observation(1, 100)); auto d = h.observe(bad);
            CHECK(d.reset && d.reason == ResetReason::discontinuity);
        }
    }},
    {"layout changes reset", [] {
        for (int field = 0; field < 5; ++field) {
            FrameHistory h; h.observe(observation(1, 100)); auto f = observation(2, 116);
            switch (field) { case 0: ++f.width; break; case 1: ++f.height; break;
                case 2: ++f.color_format; break; case 3: ++f.depth_format; break; default: ++f.motion_format; }
            CHECK(h.observe(f).reason == ResetReason::layout_change);
        }
    }},
    {"unusable frames never advance generation; recovery resets", [] {
        FrameHistory h; h.observe(observation(1, 100)); auto f = observation(2, 116); f.usable = false;
        auto d = h.observe(f); CHECK(!d.usable && !d.reset && d.generation == 1);
        CHECK(d.reason == ResetReason::missing_inputs); CHECK(h.observe(f).generation == 1);
        d = h.observe(observation(3, 132)); CHECK(d.reset && d.generation == 2);
        f = observation(4, 148); f.enabled = false; CHECK(h.observe(f).reason == ResetReason::disabled);
        h.invalidate(ResetReason::display_reset);
        CHECK(h.observe(observation(5, 164)).reason == ResetReason::display_reset);
        h.invalidate(ResetReason::none); CHECK(!h.observe(observation(6, 180)).reset);
    }},
}); }
