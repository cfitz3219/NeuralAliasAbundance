// P4-4 slider route: the verdict that confirms (or rejects) one live render-scale candidate
// from the scene size J2 observes.
#include "test_support.hpp"
#include "host_dx12/scale/live_driver.hpp"

int main() { return test::run({
    {"judge", [] {
    using namespace na::scale;
    // DLAA 3440 wide -> Quality: expected ~2293.
    CHECK(JudgeExtent(3440, 1.0, 2.0 / 3.0, 2293, 1) == Judge::Changed);
    CHECK(JudgeExtent(3440, 1.0, 2.0 / 3.0, 2240, 1) == Judge::Changed);   // the game rounds (within 3 %)
    CHECK(JudgeExtent(3440, 1.0, 2.0 / 3.0, 3440, 10) == Judge::Wait);     // not yet: a few frames to apply
    CHECK(JudgeExtent(3440, 1.0, 2.0 / 3.0, 3440, kJudgeFrames) == Judge::Unchanged);
    CHECK(JudgeExtent(3440, 1.0, 2.0 / 3.0, 1720, kJudgeFrames) == Judge::Unchanged); // some other size
    // Quality -> Performance, from a reduced baseline.
    CHECK(JudgeExtent(2293, 2.0 / 3.0, 0.5, 1720, 3) == Judge::Changed);
    // No evidence (J2 not running, or no baseline): never a rejection.
    CHECK(JudgeExtent(0, 1.0, 0.5, 1720, kJudgeFrames) == Judge::Wait);
    CHECK(JudgeExtent(3440, 1.0, 0.5, 0, kJudgeFrames * 10) == Judge::Wait);
    }},
}); }
