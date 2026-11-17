// Can AMD's pyramid search follow the flow fixtures' pictures? (R3 of Astra's audit, 2026-11-16:
// `j2_compute_flow_test.exe 1920` failed its camera-fit case while the 512x256 run passed.)
// CI cannot run the real search, so this runs a CPU model of it (ffx_search_model.hpp) on the same
// pictures the GPU fixture draws (tests/host/flow_fixture_pictures.hpp), panned 6 pixels.
//  - The four-sine picture nearly repeats about 94 pixels away. At 512x256 the pyramid's coarse
//    levels are too small to reach that far and the search is right; at 1920x1080 it locks onto the
//    repeat on most of the frame. No camera explains those vectors, so the camera fit's flow test
//    (30 % less energy over all samples) refuses a fit that did find the pan.
//  - The non-repeating picture is followed at both sizes, which is what the fit case needs.
// A known limit this pins, not a pass: a picture that repeats over most of the frame can still
// defeat the search in a game.
#include "test_support.hpp"
#include "ffx_search_model.hpp"
#include "../host/flow_fixture_pictures.hpp"

namespace {
struct Outcome { double right = 0, far = 0; };
// Share of 8x8 blocks whose vector is exactly the pan, and the share more than 64 pixels off it.
Outcome Follow(int w, int h, float (*picture)(float, float), const char* name)
{
    constexpr int pan = 6;
    const auto draw = [&](int dx) {
        ffx_model::Luma l;
        l.w = w; l.h = h; l.p.resize(size_t(w) * size_t(h));
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) l.p[size_t(y) * size_t(w) + size_t(x)] = uint8_t(std::lround(255.f * picture(float(x - dx), float(y))));
        return l;
    };
    std::vector<ffx_model::Grid> levels;
    const ffx_model::Grid g = ffx_model::Flow(draw(pan), draw(0), &levels);
    unsigned right = 0, far = 0;
    for (const auto& v : g.v) {
        if (v.x == -pan && v.y == 0) ++right;
        if (std::abs(v.x + pan) > 64 || std::abs(v.y) > 64) ++far;
    }
    Outcome o;
    o.right = double(right) / double(g.v.size());
    o.far = double(far) / double(g.v.size());
    std::printf("  %dx%d %s, 6 px pan: %.1f %% of %zu blocks right, %.1f %% more than 64 px off\n", w, h, name, 100. * o.right,
                g.v.size(), 100. * o.far);
    // Where it goes wrong: the most common vector of each pyramid level, in that level's pixels.
    for (size_t l = 0; l < levels.size(); ++l) {
        const int n = int(levels.size()) - 1 - int(l);
        unsigned best = 0; ffx_model::Vector common{};
        for (const auto& a : levels[l].v) {
            unsigned same = 0;
            for (const auto& b : levels[l].v) if (a.x == b.x && a.y == b.y) ++same;
            if (same > best) { best = same; common = a; }
            if (best * 2 > levels[l].v.size()) break;
        }
        if (n >= 2) std::printf("    level %d (1 px = %d): most common (%d, %d) = (%d, %d) full pixels, %u of %zu blocks\n", n, 1 << n, common.x,
                                common.y, common.x << n, common.y << n, best, levels[l].v.size());
    }
    return o;
}
} // namespace

int main()
{
    return test::run({
        {"small picture: the search follows both pictures", [] {
            CHECK(Follow(512, 256, test_pictures::RepeatingWaves, "four-sine picture").right > .95);
            CHECK(Follow(512, 256, test_pictures::NonRepeating, "non-repeating picture").right > .95);
        }},
        {"1920x1080: the four-sine picture is lost to its own repeat (known limit, R3)", [] {
            const Outcome o = Follow(1920, 1080, test_pictures::RepeatingWaves, "four-sine picture");
            CHECK(o.right < .4);
            CHECK(o.far > .4);
        }},
        {"1920x1080: the non-repeating picture is followed", [] {
            const Outcome o = Follow(1920, 1080, test_pictures::NonRepeating, "non-repeating picture");
            CHECK(o.right > .95);
            CHECK(o.far < .01);
        }},
    });
}
