// From the Measure pass's cells to the numbers the governor steers by (docs/NA1-ENHANCER.md section 3).
#include "core/enhancer_stats.hpp"
#include "test_support.hpp"

#include <limits>
#include <vector>

using namespace na::na1::ref;
using na::enhancer::Numbers;
using na::enhancer::Summarise;

namespace {
constexpr uint32_t kCellCount = kCellsW * kCellsH;
// Every cell the same: 10 samples, all solid, a mid-grey that the screen shows at 0.2.
std::vector<float> Plain()
{
    std::vector<float> cells(size_t(kCellCount) * kCellFloats, 0.f);
    for (uint32_t i = 0; i < kCellCount; ++i) {
        float* c = cells.data() + size_t(i) * kCellFloats;
        c[kCount] = 10; c[kYBefore] = c[kYAfter] = -20; c[kShownBefore] = c[kShownAfter] = 2;
        c[kSolid] = 10; c[kSolidY] = -20; c[kSolidR] = c[kSolidG] = c[kSolidB] = 2;
        c[kStrengthBefore] = c[kStrengthAfter] = 3; c[kVarietyBefore] = c[kVarietyAfter] = 4;
        c[kFineBefore] = c[kFineAfter] = 1; c[kFineBefore2] = c[kFineAfter2] = 2;
    }
    return cells;
}
float* CellAt(std::vector<float>& cells, uint32_t x, uint32_t y) { return cells.data() + (size_t(y) * kCellsW + x) * kCellFloats; }
}

int main()
{
    return test::run({
        {"identical pictures give texture 1.0, broad 1.0 and no key shift", [] {
            // A real Measure of a rough picture against itself.
            const uint32_t w = 320, h = 180;
            std::vector<float> rgba(size_t(w) * h * 4, 1.f), depth(size_t(w) * h, .5f);
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x) {
                    const float v = .2f * (1.f + .3f * std::sin(float(x) * .8f) * std::cos(float(y) * .6f) + .4f * std::sin(float(x + y) * .05f));
                    float* o = rgba.data() + (size_t(y) * w + x) * 4;
                    o[0] = v * 1.2f; o[1] = v; o[2] = v * .7f;
                    if (y < 30) depth[size_t(y) * w + x] = 1.f;
                }
            Scene scene;
            scene.curve.mean_y = LogLum(.2f);
            std::vector<float> cells(size_t(kCellCount) * kCellFloats);
            Measure(rgba.data(), rgba.data(), depth.data(), w, h, false, scene, 0.f, cells.data());
            const Numbers n = Summarise(cells.data(), true);
            CHECK(n.valid);
            NEAR(n.texture, 1.f, 0);
            NEAR(n.broad, 1.f, 0);
            NEAR(n.key_shift, 0.f, 0);
            NEAR(n.glow, 0.f, 0);
            CHECK(n.colour_before == n.colour_after && n.variety_before == n.variety_after);
            CHECK(n.dark_before == n.dark_after && n.white_before == n.white_after);
            CHECK(n.colour_before > .3f && n.colour_before < .5f);  // (1.2 - 0.7) / 1.2
            CHECK(n.solid_cells > kCellCount * 3 / 4 && n.solid_cells < kCellCount);
            NEAR(n.tint[0] * .2126f + n.tint[1] * .7152f + n.tint[2] * .0722f, 1.f, 1e-5);
            NEAR(n.tint[0] / n.tint[1], 1.2f, 1e-3);
            NEAR(n.tint[2] / n.tint[1], .7f, 1e-3);
        }},
        {"means, percentages and the fine-contrast ratio come straight from the sums", [] {
            auto cells = Plain();
            for (uint32_t i = 0; i < kCellCount; ++i) {
                float* c = cells.data() + size_t(i) * kCellFloats;
                c[kBlackBefore] = 1; c[kBlackAfter] = .5f; c[kWhiteBefore] = .2f; c[kWhiteAfter] = .3f;
                c[kStrengthAfter] = 3.3f; c[kVarietyAfter] = 4.8f;
                c[kFineAfter] = 2; c[kFineAfter2] = 8;   // every fine value doubled: mean x2, squares x4
                c[kShownAfter] = 4;
            }
            const Numbers n = Summarise(cells.data(), true);
            CHECK(n.valid);
            NEAR(n.dark_before, 10.f, 1e-5); NEAR(n.dark_after, 5.f, 1e-5);
            NEAR(n.white_before, 2.f, 1e-5); NEAR(n.white_after, 3.f, 1e-5);
            NEAR(n.colour_before, .3f, 1e-6); NEAR(n.colour_after, .33f, 1e-6);
            NEAR(n.variety_before, .4f, 1e-6); NEAR(n.variety_after, .48f, 1e-6);
            NEAR(n.texture, 2.f, 1e-5);
            NEAR(n.key_shift, 1.f, 1e-6);
            NEAR(n.m0, .2f, 1e-6);
            NEAR(n.key_stops, -std::log2(.2f), 1e-5);
            NEAR(n.mean_y, -2.f, 1e-6);
            NEAR(n.mid, -2.f, 1e-6);
            NEAR(n.range, 0.f, 1e-6);
            NEAR(n.p85, .2f, 1e-6); NEAR(n.p97, .2f, 1e-6);
            CHECK(n.solid_cells == kCellCount);
            for (float t : n.tint) NEAR(t, 1.f, 1e-5);
        }},
        {"a dark scene is told by the measured curve only; an assumed curve treats it as ordinary", [] {
            auto cells = Plain();
            for (uint32_t i = 0; i < kCellCount; ++i) cells[size_t(i) * kCellFloats + kShownBefore] = .5f;  // m0 0.05: 4.3 stops under white
            NEAR(Summarise(cells.data(), true).w_dark, 1.f, 0);
            NEAR(Summarise(cells.data(), false).w_dark, 0.f, 0);
            for (uint32_t i = 0; i < kCellCount; ++i) cells[size_t(i) * kCellFloats + kShownBefore] = 1.f;  // m0 0.1: 3.32 stops
            NEAR(Summarise(cells.data(), true).w_dark, (3.3219f - 2.6f) / 1.4f, 1e-3);
            for (uint32_t i = 0; i < kCellCount; ++i) cells[size_t(i) * kCellFloats + kShownBefore] = 3.f;  // m0 0.3
            NEAR(Summarise(cells.data(), true).w_dark, 0.f, 0);
        }},
        {"lighting range, middle and the bright percentiles are read across the cells", [] {
            auto cells = Plain();
            for (uint32_t y = 0; y < kCellsH; ++y)
                for (uint32_t x = 0; x < kCellsW; ++x) {
                    float* c = CellAt(cells, x, y);
                    const float level = -8.f + 6.f * float(x) / float(kCellsW - 1);  // six stops, left to right
                    c[kSolidY] = c[kYBefore] = c[kYAfter] = 10 * level;
                    c[kShownBefore] = c[kShownAfter] = 10 * float(x) / float(kCellsW - 1);
                }
            const Numbers n = Summarise(cells.data(), true);
            NEAR(n.mid, -5.f, .03);
            NEAR(n.range, 5.4f, .05);           // 90% of six stops
            NEAR(n.w_range, 1.f, 0);
            NEAR(n.p85, .85f, .01); NEAR(n.p97, .97f, .01);
            CHECK(n.valid);
        }},
        {"sky cells take no part in the solid statistics", [] {
            auto cells = Plain();
            for (uint32_t y = 0; y < kCellsH / 2; ++y)
                for (uint32_t x = 0; x < kCellsW; ++x) {
                    float* c = CellAt(cells, x, y);
                    c[kSolid] = 4; c[kSolidY] = 4 * 3.f;                 // under half solid: a bright sky cell
                    c[kSolidR] = 40; c[kSolidG] = 4; c[kSolidB] = 4;     // whose few solid pixels are red
                    c[kStrengthBefore] = c[kStrengthAfter] = 1.2f; c[kVarietyBefore] = c[kVarietyAfter] = 1.6f;
                    c[kFineBefore] = c[kFineAfter] = .4f; c[kFineBefore2] = c[kFineAfter2] = .8f;
                }
            const Numbers n = Summarise(cells.data(), true);
            CHECK(n.solid_cells == kCellCount / 2);
            NEAR(n.mid, -2.f, 1e-6);                      // not pulled up by the sky cells' solid pixels
            for (float t : n.tint) NEAR(t, 1.f, 1e-5);    // nor tinted by them
            NEAR(n.colour_before, .3f, 1e-6);             // per solid pixel, wherever it is
        }},
        {"edge glow is believed only with 200 band samples", [] {
            auto cells = Plain();
            CellAt(cells, 5, 5)[kBand] = 199; CellAt(cells, 5, 5)[kBandGlow] = 199 * .05f;
            NEAR(Summarise(cells.data(), true).glow, 0.f, 0);
            CellAt(cells, 6, 5)[kBand] = 1; CellAt(cells, 6, 5)[kBandGlow] = .05f;
            NEAR(Summarise(cells.data(), true).glow, .05f, 1e-6);
        }},
        {"broad contrast compares the thumbnails' large shapes, not their detail or their level", [] {
            auto cells = Plain();
            for (uint32_t y = 0; y < kCellsH; ++y)
                for (uint32_t x = 0; x < kCellsW; ++x) {
                    float* c = CellAt(cells, x, y);
                    const float shape = std::sin(float(x) * .25f) + std::cos(float(y) * .3f);
                    c[kYBefore] = 10 * (-2.f + shape);
                    c[kYAfter] = 10 * (-1.f + 1.2f * shape);  // shapes 20% stronger, the whole picture a stop brighter
                }
            NEAR(Summarise(cells.data(), true).broad, 1.2f, 1e-3);
            for (uint32_t i = 0; i < kCellCount; ++i) cells[size_t(i) * kCellFloats + kYAfter] = cells[size_t(i) * kCellFloats + kYBefore] + 30;
            NEAR(Summarise(cells.data(), true).broad, 1.f, 1e-4);
        }},
        {"a picture with no fine contrast or no solid pixels still gives usable numbers", [] {
            auto cells = Plain();
            for (uint32_t i = 0; i < kCellCount; ++i) {
                float* c = cells.data() + size_t(i) * kCellFloats;
                c[kFineBefore] = c[kFineAfter] = c[kFineBefore2] = c[kFineAfter2] = 0;
            }
            Numbers n = Summarise(cells.data(), true);
            CHECK(n.valid);
            NEAR(n.texture, 1.f, 0);
            NEAR(n.broad, 1.f, 0);
            for (uint32_t i = 0; i < kCellCount; ++i)
                for (uint32_t k = kSolid; k < kCellFloats; ++k) cells[size_t(i) * kCellFloats + k] = 0;
            n = Summarise(cells.data(), true);
            CHECK(n.valid && n.solid_cells == 0);
            NEAR(n.texture, 1.f, 0);
            NEAR(n.colour_before, 0.f, 0);
            for (float t : n.tint) NEAR(t, 1.f, 0);
        }},
        {"nothing measured, or a readback holding a non-number, is not valid", [] {
            std::vector<float> empty(size_t(kCellCount) * kCellFloats, 0.f);
            CHECK(!Summarise(empty.data(), true).valid);
            CHECK(!Summarise(nullptr, true).valid);
            auto cells = Plain();
            CellAt(cells, 100, 50)[kYAfter] = std::numeric_limits<float>::quiet_NaN();
            CHECK(!Summarise(cells.data(), true).valid);
            cells = Plain();
            CellAt(cells, 0, 0)[kFineAfter2] = std::numeric_limits<float>::infinity();
            CHECK(!Summarise(cells.data(), true).valid);
        }},
        {"percentiles take a straight line between neighbours", [] {
            std::vector<float> v{4, 1, 3, 2, 5};
            NEAR(na::enhancer::Percentile(v, 50), 3.f, 0);
            NEAR(na::enhancer::Percentile(v, 0), 1.f, 0);
            NEAR(na::enhancer::Percentile(v, 100), 5.f, 0);
            NEAR(na::enhancer::Percentile(v, 90), 4.6f, 1e-6);
            std::vector<float> none;
            NEAR(na::enhancer::Percentile(none, 50), 0.f, 0);
        }},
    });
}
