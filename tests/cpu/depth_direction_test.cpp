// Which end of the scene depth is near, with no camera (native11/depth_direction.hpp): proven from
// the selected depth's own scene writes, never from an EQUAL-only pass, a clear value alone or
// writes that disagree; kept across frames that prove nothing (Astra's correction 6, 2026-11-16).
#include "test_support.hpp"
#include "host_dx12/native11/depth_direction.hpp"

using namespace na::native11;

namespace {
// One frame: a clear (or none), then depth-writing draws by comparison, then the hand-over.
struct FrameSpec {
    bool clear = false;
    float clear_value = 0;
    uint32_t less = 0, less_equal = 0, greater = 0, greater_equal = 0, equal = 0, always = 0;
};
DepthCompare Play(DepthDirection& d, const FrameSpec& f, uint64_t depth = 1, DepthVerdict* why = nullptr)
{
    if (f.clear) d.NoteClear(f.clear_value);
    for (uint32_t i = 0; i < f.less; ++i) d.NoteWrite(kCompareLess);
    for (uint32_t i = 0; i < f.less_equal; ++i) d.NoteWrite(kCompareLessEqual);
    for (uint32_t i = 0; i < f.greater; ++i) d.NoteWrite(kCompareGreater);
    for (uint32_t i = 0; i < f.greater_equal; ++i) d.NoteWrite(kCompareGreaterEqual);
    for (uint32_t i = 0; i < f.equal; ++i) d.NoteWrite(kCompareEqual);
    for (uint32_t i = 0; i < f.always; ++i) d.NoteWrite(8);
    const DepthCompare r = d.Decide(depth, why);
    d.EndFrame();
    return r;
}
FrameSpec Standard() { FrameSpec f; f.clear = true; f.clear_value = 1; f.less_equal = 400; return f; }
FrameSpec Reversed() { FrameSpec f; f.clear = true; f.clear_value = 0; f.greater_equal = 400; return f; }
} // namespace

int main()
{
    return test::run({
        {"writes that keep the smaller value prove smaller is nearer; the larger, the opposite", [] {
            DepthDirection a, b;
            DepthVerdict why{};
            CHECK(Play(a, Standard(), 1, &why) == DepthCompare::NearIsLess && why == DepthVerdict::Proven);
            CHECK(Play(b, Reversed(), 1, &why) == DepthCompare::NearIsGreater && why == DepthVerdict::Proven);
            // No clear seen at all (a game that clears with a full-screen draw): the writes are the proof.
            DepthDirection c;
            FrameSpec no_clear;
            no_clear.less = 12;
            CHECK(Play(c, no_clear, 1, &why) == DepthCompare::NearIsLess && why == DepthVerdict::Proven);
        }},
        {"a draw that does not write the depth is not evidence", [] {
            DepthDirection d;
            d.NoteClear(1.f);
            d.NoteWrite(0); // tested, not written: the provider passes 0
            d.NoteWrite(0);
            DepthVerdict why{};
            CHECK(d.Decide(1, &why) == DepthCompare::None && why == DepthVerdict::ClearOnly);
            CHECK(d.Last().less == 0 && d.Last().greater == 0);
        }},
        {"an EQUAL-only pass proves nothing", [] {
            DepthDirection d;
            FrameSpec f;
            f.clear = true; f.clear_value = 1; f.equal = 300;
            DepthVerdict why{};
            CHECK(Play(d, f, 1, &why) == DepthCompare::None && why == DepthVerdict::ClearOnly);
            FrameSpec bare;
            bare.equal = 300; bare.always = 4;
            CHECK(Play(d, bare, 1, &why) == DepthCompare::None && why == DepthVerdict::NoWrites);
        }},
        {"a clear value alone proves nothing, whichever value it is", [] {
            for (float value : {0.f, 1.f, 0.5f}) {
                DepthDirection d;
                FrameSpec f;
                f.clear = true; f.clear_value = value;
                DepthVerdict why{};
                CHECK(Play(d, f, 1, &why) == DepthCompare::None && why == DepthVerdict::ClearOnly);
            }
        }},
        {"writes that disagree prove nothing unless the clear sides with most of them", [] {
            DepthVerdict why{};
            DepthDirection none;
            FrameSpec split;
            split.less = 100; split.greater = 100;
            CHECK(Play(none, split, 1, &why) == DepthCompare::None && why == DepthVerdict::Conflict);
            DepthDirection half; // the clear agrees with one side, but that side is only half the writes
            split.clear = true; split.clear_value = 1;
            CHECK(Play(half, split, 1, &why) == DepthCompare::None && why == DepthVerdict::Conflict);
            DepthDirection most; // a few odd draws against a scene the clear agrees with
            FrameSpec f;
            f.clear = true; f.clear_value = 1; f.less_equal = 390; f.greater = 10;
            CHECK(Play(most, f, 1, &why) == DepthCompare::NearIsLess && why == DepthVerdict::Proven);
            DepthDirection wrong_side; // the clear agrees with the few
            f.clear_value = 0;
            CHECK(Play(wrong_side, f, 1, &why) == DepthCompare::None && why == DepthVerdict::Conflict);
            DepthDirection unusual; // a clear that is neither end settles nothing
            f.clear_value = 0.5f;
            CHECK(Play(unusual, f, 1, &why) == DepthCompare::None && why == DepthVerdict::Conflict);
        }},
        {"writes that agree while the clear says the opposite prove nothing", [] {
            DepthDirection d;
            FrameSpec f;
            f.clear = true; f.clear_value = 0; f.less_equal = 400;
            DepthVerdict why{};
            CHECK(Play(d, f, 1, &why) == DepthCompare::None && why == DepthVerdict::ClearDisagrees);
            // Cleared twice to different values in one frame: the clear is no evidence, the writes still are.
            DepthDirection twice;
            twice.NoteClear(0.f);
            twice.NoteClear(1.f);
            for (int i = 0; i < 50; ++i) twice.NoteWrite(kCompareLessEqual);
            CHECK(twice.Decide(1, &why) == DepthCompare::NearIsLess && why == DepthVerdict::Proven);
        }},
        {"a proven direction is kept across frames that prove nothing", [] {
            DepthDirection d;
            DepthVerdict why{};
            CHECK(Play(d, Reversed()) == DepthCompare::NearIsGreater);
            FrameSpec menu; // nothing drawn into the depth
            FrameSpec equal_only;
            equal_only.clear = true; equal_only.clear_value = 0; equal_only.equal = 200;
            FrameSpec conflict;
            conflict.less = 5; conflict.greater = 5;
            FrameSpec clear_only;
            clear_only.clear = true; clear_only.clear_value = 1; // even a clear to the other end
            for (const FrameSpec& f : {menu, equal_only, conflict, clear_only, menu}) {
                CHECK(Play(d, f, 1, &why) == DepthCompare::NearIsGreater && why == DepthVerdict::Kept);
            }
            CHECK(d.Flips() == 0);
        }},
        {"one odd frame does not turn the direction round; three in a row do", [] {
            DepthDirection d;
            DepthVerdict why{};
            CHECK(Play(d, Standard()) == DepthCompare::NearIsLess);
            CHECK(Play(d, Reversed(), 1, &why) == DepthCompare::NearIsLess && why == DepthVerdict::Kept);
            CHECK(Play(d, Standard(), 1, &why) == DepthCompare::NearIsLess && why == DepthVerdict::Proven);
            CHECK(Play(d, Reversed()) == DepthCompare::NearIsLess);
            CHECK(Play(d, Reversed()) == DepthCompare::NearIsLess);
            CHECK(Play(d, Reversed(), 1, &why) == DepthCompare::NearIsGreater && why == DepthVerdict::Proven);
            CHECK(d.Flips() == 1);
        }},
        {"another depth buffer starts from nothing", [] {
            DepthDirection d;
            DepthVerdict why{};
            CHECK(Play(d, Standard(), 7) == DepthCompare::NearIsLess);
            FrameSpec menu;
            CHECK(Play(d, menu, 8, &why) == DepthCompare::None && why == DepthVerdict::NoWrites);
            CHECK(Play(d, Reversed(), 8, &why) == DepthCompare::NearIsGreater && why == DepthVerdict::Proven);
        }},
        {"comparisons are read as D3D11 numbers them", [] {
            CHECK(CompareOf(2) == DepthCompare::NearIsLess && CompareOf(4) == DepthCompare::NearIsLess);
            CHECK(CompareOf(5) == DepthCompare::NearIsGreater && CompareOf(7) == DepthCompare::NearIsGreater);
            for (uint8_t f : {uint8_t(0), uint8_t(1), uint8_t(3), uint8_t(6), uint8_t(8)}) CHECK(CompareOf(f) == DepthCompare::None);
        }},
    });
}
