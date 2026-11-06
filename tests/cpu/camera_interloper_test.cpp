// Camera interlopers (SE5 menu, run p25748): a second camera flips into the main camera's slot
// for 1-3 frames with a fixed offset (translation 356.5; fov +1.378 then -1.378). Pure logic.
#include "test_support.hpp"
#include "host_dx12/j2/camera_interloper.hpp"
using na::j2::CameraPose;
using na::j2::InterloperFilter;
using V = InterloperFilter::Verdict;
namespace {
CameraPose pose(double x, double y, double z, double fov = 0.8) {
    CameraPose p; p.position = {x, y, z}; p.forward = {0, 0, 1}; p.fov = fov; return p;
}
}
int main() {
    return test::run({
        {"a camera that flips away and back is learned; later flips keep the main camera", [] {
            InterloperFilter f;
            const CameraPose main = pose(10, 2, 5), other = pose(10 + 356.5, 2, 5);
            uint64_t fr = 1;
            CHECK(f.Judge(main, fr++) == V::First);
            for (int i = 0; i < 10; ++i) CHECK(f.Judge(main, fr++) == V::Continuous);
            CHECK(f.Judge(other, fr++) == V::Cut);          // first flip: indistinguishable from a real cut
            CHECK(f.Judge(other, fr++) == V::Continuous);
            CHECK(f.Judge(main, fr++) == V::Cut);           // back: the jump target is learned
            CHECK(f.KnownCount() == 1 && f.Learned() == 1);
            unsigned cuts = 0, kept = 0;
            for (int round = 0; round < 50; ++round) {
                for (int i = 0; i < 6; ++i) { const auto v = f.Judge(main, fr++); cuts += v == V::Cut; }
                for (int i = 0; i < 1 + round % 3; ++i) { CHECK(f.IsInterloper(other)); const auto v = f.Judge(other, fr++); kept += v == V::Interloper; cuts += v == V::Cut; }
            }
            CHECK(cuts == 0);
            CHECK(kept == f.Rejected() && kept > 0);
            CHECK(!f.IsInterloper(main));
        }},
        {"the offset follows a moving main camera (a follow camera keeps its offset)", [] {
            InterloperFilter f;
            uint64_t fr = 1;
            double x = 0;
            auto main = [&] { return pose(x, 0, 0); };
            auto other = [&] { return pose(x + 100, 0, 0); };
            f.Judge(main(), fr++);
            f.Judge(other(), fr++); f.Judge(main(), fr++); // learned
            CHECK(f.KnownCount() == 1);
            for (int i = 0; i < 200; ++i) {
                x += 3; // walking: continuous
                CHECK(f.Judge(main(), fr++) == V::Continuous);
                if (i % 7 == 0) CHECK(f.Judge(other(), fr++) == V::Interloper);
            }
        }},
        {"a fov-only interloper (+1.378 then back) is learned too", [] {
            InterloperFilter f;
            uint64_t fr = 1;
            const CameraPose main = pose(0, 0, 0, 0.6), wide = pose(0, 0, 0, 0.6 + 1.378);
            f.Judge(main, fr++);
            CHECK(f.Judge(wide, fr++) == V::Cut);
            CHECK(f.Judge(main, fr++) == V::Cut);
            CHECK(f.IsInterloper(wide));
            CHECK(f.Judge(wide, fr++) == V::Interloper);
            CHECK(f.Judge(main, fr++) == V::Continuous);
        }},
        {"a real cut that stays is never learned, and the new view becomes main", [] {
            InterloperFilter f;
            uint64_t fr = 1;
            const CameraPose a = pose(0, 0, 0), b = pose(500, 0, 0);
            f.Judge(a, fr++);
            CHECK(f.Judge(b, fr++) == V::Cut);
            for (int i = 0; i < 30; ++i) CHECK(f.Judge(b, fr++) == V::Continuous);
            CHECK(f.KnownCount() == 0);
            // Returning long after the window is another real cut, not an interloper.
            CHECK(f.Judge(a, fr++) == V::Cut);
            for (int i = 0; i < 30; ++i) CHECK(f.Judge(a, fr++) == V::Continuous);
            CHECK(f.KnownCount() == 0);
        }},
        {"starting on the interloper still learns the right camera", [] {
            InterloperFilter f;
            uint64_t fr = 1;
            const CameraPose main = pose(0, 0, 0), other = pose(356.5, 0, 0);
            CHECK(f.Judge(other, fr++) == V::First);
            CHECK(f.Judge(main, fr++) == V::Cut);
            for (int i = 0; i < 20; ++i) f.Judge(main, fr++); // main dominates: no return within the window
            CHECK(f.KnownCount() == 0);
            f.Judge(other, fr++); f.Judge(main, fr++);
            CHECK(f.IsInterloper(other) && !f.IsInterloper(main));
        }},
        {"reset forgets everything; at most four interlopers are kept", [] {
            InterloperFilter f;
            uint64_t fr = 1;
            f.Judge(pose(0, 0, 0), fr++);
            for (int k = 1; k <= 6; ++k) { f.Judge(pose(100.0 * k, 0, 0), fr++); f.Judge(pose(0, 0, 0), fr++); }
            CHECK(f.KnownCount() == 4);
            f.Reset();
            CHECK(f.KnownCount() == 0 && !f.IsInterloper(pose(100, 0, 0)));
        }},
    });
}
