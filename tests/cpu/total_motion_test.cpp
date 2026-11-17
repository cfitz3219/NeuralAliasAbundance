// Measured total motion (core/total_motion.hpp): the CPU reference of the flow pass's `measure`
// program, on synthetic picture pairs made the way the live path makes them. A scene is drawn
// with no anti-aliasing at the frame's raster jitter, kept at 8 bits, then resampled by that jitter
// into the unjittered frame (the encode step), and the pair is measured with no camera at all.
//
// What must hold (Astra's corrections of 2026-11-16):
//  - a still scene under jitter alone measures as still, not as small motion;
//  - 0.125 px and 0.25 px pans, which the whole-pixel search reports as zero, are measured as
//    motion in the right direction, and never thrown away by a small-motion cutoff;
//  - a 3.4 px pan with the search's whole-pixel answer (3) is measured to a fraction of a pixel;
//  - a reversal is measured on the frame it happens (no rule looks at the frame before);
//  - a moving foreground over a still background gets each its own motion;
//  - a wrong search vector does not win against what the picture shows;
//  - motion the search did not find and the picture cannot explain is "not known", never "still";
//  - a flat area is never a measurement, and the search's raw vector is never handed on for it
//    (Cort's Sniper Elite Resistance run, 2026-11-17): it follows the screen's own motion when
//    there is one (core/screen_motion.hpp), and is "not known" when there is none;
//  - with a screen motion, a pixel moving with the screen is measured as exactly that, and a thing
//    moving by itself keeps its own vector.
// The numbers asserted are loose bounds around what this reference measures today; the printed
// lines are the measurements themselves.
#include "test_support.hpp"
#include "core/total_motion.hpp"
#include "core/motion_arbiter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iterator>
#include <limits>
#include <vector>

using namespace na::motion::total;

namespace {
constexpr int kW = 160, kH = 120;

float Hash(int x, int y)
{
    unsigned h = unsigned(x) * 374761393u + unsigned(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float((h ^ (h >> 16)) & 0xffff) / 65535.f;
}
float Noise(float x, float y)
{
    const int xi = int(std::floor(x)), yi = int(std::floor(y));
    float tx = x - float(xi), ty = y - float(yi);
    tx = tx * tx * (3 - 2 * tx);
    ty = ty * ty * (3 - 2 * ty);
    return (Hash(xi, yi) * (1 - tx) + Hash(xi + 1, yi) * tx) * (1 - ty) +
           (Hash(xi, yi + 1) * (1 - tx) + Hash(xi + 1, yi + 1) * tx) * ty;
}
// A textured ground; with `edges`, slanted hard-edged bars and a disc that alias when drawn.
float Ground(float x, float y, bool edges)
{
    float v = 0.5f + 0.5f * (0.5f * (Noise(x / 6.f, y / 6.f) - 0.5f) + 0.3f * (Noise(x / 2.5f + 7, y / 2.5f + 3) - 0.5f) +
                             0.2f * std::sin(x * 0.35f + y * 0.21f));
    if (edges) {
        const float u = x * 0.94f + y * 0.34f;
        if (std::fmod(std::fabs(u), 23.f) < 9.f) v = v * 0.3f + 0.6f;
        const float dx = x - 90, dy = y - 60;
        if (dx * dx + dy * dy < 20 * 20) v = 0.15f;
    }
    return std::clamp(v, 0.f, 1.f);
}
float Q8(float v) { return std::round(std::clamp(v, 0.f, 1.f) * 255.f) / 255.f; }

using World = std::function<float(float, float)>; // luma at an unjittered frame position

// The game's frame: pixel centre p shows the unjittered position p - jitter. No anti-aliasing.
std::vector<float> Draw(const World& world, float jx, float jy)
{
    std::vector<float> f(size_t(kW) * kH);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) f[size_t(y) * kW + x] = Q8(world(float(x) + 0.5f - jx, float(y) + 0.5f - jy));
    return f;
}
// The flow pass's encode: texel q of the stabilised picture holds the frame at q + jitter.
std::vector<float> Encode(const std::vector<float>& frame, float jx, float jy)
{
    const Image im{frame.data(), kW, kH};
    std::vector<float> e(size_t(kW) * kH);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) e[size_t(y) * kW + x] = Q8(Sample(im, float(x) + 0.5f + jx, float(y) + 0.5f + jy));
    return e;
}

struct Pair {
    std::vector<float> previous, current;
    float jitter[2];
};
constexpr float kJitterPrevious[2] = {0.25f, -0.17f}, kJitterCurrent[2] = {-0.25f, 0.33f};
Pair MakePair(const World& before, const World& now, bool jitter)
{
    const float jp[2] = {jitter ? kJitterPrevious[0] : 0, jitter ? kJitterPrevious[1] : 0};
    const float jc[2] = {jitter ? kJitterCurrent[0] : 0, jitter ? kJitterCurrent[1] : 0};
    return {Encode(Draw(before, jp[0], jp[1]), jp[0], jp[1]), Encode(Draw(now, jc[0], jc[1]), jc[0], jc[1]), {jc[0], jc[1]}};
}

struct Tally {
    int pixels = 0, moving = 0, still = 0, approximate = 0, nothing = 0;
    double mean_x = 0, mean_y = 0; // over measured pixels
    float median = 0, p95 = 0;     // error against the truth, over measured pixels
    double Share(int n) const { return pixels ? double(n) / pixels : 0; }
};
// truth / raw: per-pixel functions of the pixel centre (the search's vector may be absent).
struct Raw { bool have; float x, y; };
Tally Measure(const Pair& pair, const std::function<void(float, float, float&, float&)>& truth,
              const std::function<Raw(float, float)>& raw, const char* name, int x0 = 10, int y0 = 10, int x1 = kW - 10, int y1 = kH - 10)
{
    const Image cur{pair.current.data(), kW, kH}, prev{pair.previous.data(), kW, kH};
    Tally t;
    std::vector<float> errors;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const Raw s = raw(float(x) + 0.5f, float(y) + 0.5f);
            const Result r = MeasurePixel(cur, prev, float(x) + 0.5f - pair.jitter[0], float(y) + 0.5f - pair.jitter[1], s.have, s.x, s.y);
            ++t.pixels;
            if (r.why == kMoving) ++t.moving;
            else if (r.why == kStill) ++t.still;
            else if (r.valid > 0) ++t.approximate;
            else ++t.nothing;
            CHECK(r.valid == kMeasured ? (r.why == kMoving || r.why == kStill) : r.why == kNothing); // no screen motion given: nothing is approximate
            CHECK(r.valid > 0 || (r.x == 0 && r.y == 0));
            if (r.valid == kMeasured) {
                float tx = 0, ty = 0;
                truth(float(x) + 0.5f, float(y) + 0.5f, tx, ty);
                errors.push_back(std::hypot(r.x - tx, r.y - ty));
                t.mean_x += r.x;
                t.mean_y += r.y;
            }
        }
    }
    if (!errors.empty()) {
        std::sort(errors.begin(), errors.end());
        t.median = errors[errors.size() / 2];
        t.p95 = errors[errors.size() * 95 / 100];
        t.mean_x /= double(errors.size());
        t.mean_y /= double(errors.size());
    }
    std::printf("  %-34s moving %5.1f%%  still %5.1f%%  approximate %5.1f%%  not known %5.1f%%  mean (%+.3f, %+.3f)  error median %.3f p95 %.3f\n",
                name, 100 * t.Share(t.moving), 100 * t.Share(t.still), 100 * t.Share(t.approximate), 100 * t.Share(t.nothing),
                t.mean_x, t.mean_y, t.median, t.p95);
    return t;
}

// The whole scene slides right by `d` pixels between the two frames: v = previous - current = -d.
Pair Pan(float d, bool edges, bool jitter)
{
    return MakePair([=](float x, float y) { return Ground(x, y, edges); }, [=](float x, float y) { return Ground(x - d, y, edges); }, jitter);
}
auto Uniform(float vx, float vy) { return [=](float, float, float& x, float& y) { x = vx; y = vy; }; }
auto Search(bool have, float x, float y) { return [=](float, float) { return Raw{have, x, y}; }; }
} // namespace

int main()
{
    return test::run({
        {"a still scene measures as exactly still, with and without jitter", [] {
            for (int edges = 0; edges < 2; ++edges) {
                const Tally plain = Measure(Pan(0, edges != 0, false), Uniform(0, 0), Search(true, 0, 0), edges ? "still, edges, no jitter" : "still, no jitter");
                // The flat pixels between the edges are not known: no screen motion is given here.
                const double flat = edges ? 0.12 : 0.02;
                CHECK(plain.moving == 0 && plain.Share(plain.nothing) < flat && plain.p95 == 0);
                const Tally jittered = Measure(Pan(0, edges != 0, true), Uniform(0, 0), Search(true, 0, 0), edges ? "still, edges, jitter only" : "still, jitter only");
                // Jitter alone is not motion: most of the picture reads exactly still, the rest within a tenth of a pixel.
                CHECK(jittered.Share(jittered.still) > 0.80 && jittered.Share(jittered.nothing) < flat);
                CHECK(jittered.p95 < 0.12f);
                CHECK(std::fabs(jittered.mean_x) < 0.02 && std::fabs(jittered.mean_y) < 0.02);
            }
        }},
        {"slow pans the whole-pixel search reports as zero are measured", [] {
            for (float d : {0.125f, 0.25f}) {
                char name[64];
                std::snprintf(name, sizeof name, "pan %.3f px, search says 0", d);
                const Tally t = Measure(Pan(d, false, true), Uniform(-d, 0), Search(true, 0, 0), name);
                // Measured as motion on most of a textured picture, in the right direction, no cutoff:
                CHECK(t.Share(t.moving) > 0.60 && t.Share(t.nothing) < 0.02);
                CHECK(t.mean_x < -0.5 * d && t.mean_x > -1.2 * d && std::fabs(t.mean_y) < 0.03);
                // The worst a pixel can be is "read as still": an error of the pan itself, never more.
                CHECK(t.p95 <= d + 0.02f);
                std::snprintf(name, sizeof name, "pan %.3f px, edges", d);
                const Tally e = Measure(Pan(d, true, true), Uniform(-d, 0), Search(true, 0, 0), name);
                CHECK(e.mean_x < -0.25 * d && e.mean_x > -1.3 * d && e.Share(e.nothing) < 0.12);
            }
        }},
        {"a 3.4 px pan with the search's whole-pixel answer is measured below a pixel", [] {
            const Tally t = Measure(Pan(3.4f, false, true), Uniform(-3.4f, 0), Search(true, -3, 0), "pan 3.4 px, search says 3");
            CHECK(t.Share(t.moving) > 0.95 && t.median < 0.08f && t.p95 < 0.2f);
            NEAR(t.mean_x, -3.4, 0.03);
            const Tally e = Measure(Pan(3.4f, true, true), Uniform(-3.4f, 0), Search(true, -3, 0), "pan 3.4 px, edges");
            // Hard edges drawn with no anti-aliasing are the worst case: still a fraction of a pixel.
            CHECK(e.Share(e.moving) > 0.85 && e.median < 0.2f && e.p95 < 0.6f);
            NEAR(e.mean_x, -3.4, 0.06);
        }},
        {"a reversal is measured on the frame it happens", [] {
            const Tally forward = Measure(Pan(2.3f, false, true), Uniform(-2.3f, 0), Search(true, -2, 0), "pan +2.3 px");
            const Tally back = Measure(Pan(-2.3f, false, true), Uniform(2.3f, 0), Search(true, 2, 0), "then -2.3 px");
            CHECK(forward.Share(forward.moving) > 0.95 && back.Share(back.moving) > 0.95);
            NEAR(forward.mean_x, -2.3, 0.03);
            NEAR(back.mean_x, 2.3, 0.03);
        }},
        {"a moving foreground and a still background each get their own motion", [] {
            // A textured square (x 50..110, y 30..90 at rest) slides right by 2.5 px over still ground.
            const auto square = [](float x, float y) { return 0.5f + 0.45f * std::sin(x * 0.9f) * std::sin(y * 0.8f + 0.5f); };
            const auto world = [=](float shift) {
                return [=](float x, float y) {
                    const float sx = x - shift;
                    return sx >= 50 && sx < 110 && y >= 30 && y < 90 ? square(sx, y) : Ground(x, y, false);
                };
            };
            const Pair pair = MakePair(world(0), world(2.5f), true);
            const auto inside = [](float x, float y) { return x >= 52.5f && x < 110 && y >= 30 && y < 90; }; // the square now
            const auto truth = [=](float x, float y, float& vx, float& vy) { vx = inside(x, y) ? -2.5f : 0; vy = 0; };
            const auto raw = [=](float x, float y) { return Raw{true, inside(x, y) ? -3.f : 0.f, 0}; }; // depth-aware: each side its own
            const Tally fg = Measure(pair, truth, raw, "foreground interior", 60, 38, 104, 84);
            CHECK(fg.Share(fg.moving) > 0.90 && fg.median < 0.1f);
            NEAR(fg.mean_x, -2.5, 0.05);
            const Tally bg = Measure(pair, truth, raw, "background away from it", 10, 10, 40, 110);
            CHECK(bg.Share(bg.still) > 0.80 && bg.p95 < 0.12f);
            // Ground the square uncovered this frame (x 50..52.5) has no previous picture of itself.
            const Tally uncovered = Measure(pair, truth, raw, "ground just uncovered", 50, 34, 52, 86);
            CHECK(uncovered.Share(uncovered.moving) < 0.35);
        }},
        {"a wrong search vector does not win against the picture", [] {
            const Tally t = Measure(Pan(-0.2f, false, true), Uniform(0.2f, 0), Search(true, 5, 0), "truth +0.2 px, search says 5");
            CHECK(t.Share(t.moving) + t.Share(t.still) > 0.95 && t.p95 <= 0.22f);
            CHECK(t.mean_x > 0.1 && t.mean_x < 0.25);
        }},
        {"motion nobody found is not known, and never still", [] {
            const Tally t = Measure(Pan(3.4f, false, true), Uniform(-3.4f, 0), Search(false, 0, 0), "pan 3.4 px, no search vector");
            CHECK(t.Share(t.nothing) > 0.93 && t.Share(t.still) < 0.02 && t.approximate == 0);
        }},
        {"a flat area is never a measurement and never gets the search's vector", [] {
            const auto flat = [](float, float) { return 0.4f; };
            // No screen motion to follow: not known, whatever the search says.
            const Tally with = Measure(MakePair(flat, flat, true), Uniform(0, 0), Search(true, -3, 0), "flat, search says 3");
            CHECK(with.nothing == with.pixels && with.approximate == 0);
            const Tally without = Measure(MakePair(flat, flat, true), Uniform(0, 0), Search(false, 0, 0), "flat, no search vector");
            CHECK(without.nothing == without.pixels);
            const Pair pair = MakePair(flat, flat, false);
            const Image cur{pair.current.data(), kW, kH}, prev{pair.previous.data(), kW, kH};
            // A search vector pointing outside the frame is not handed on either.
            const Result out = MeasurePixel(cur, prev, 2.5f, 50.5f, true, -9, 0);
            CHECK(out.why == kNothing && out.valid == 0 && out.x == 0 && out.y == 0);
            // With a screen motion the flat pixel follows it exactly, marked as an approximation.
            const Result follows = MeasurePixel(cur, prev, 80.5f, 50.5f, true, -9, 4, {}, true, -1.75f, 0.5f);
            CHECK(follows.why == kScreen && follows.valid == kApproximate && follows.x == -1.75f && follows.y == 0.5f);
        }},
        {"with a screen motion, what moves with the screen is measured as that and the rest keeps its own vector", [] {
            // The whole picture pans by 3.4 px; the screen's motion says so (as a fit would), the search says 3.
            const Pair pan = Pan(3.4f, false, true);
            const Image cur{pan.current.data(), kW, kH}, prev{pan.previous.data(), kW, kH};
            int with = 0, own = 0, total = 0;
            float worst = 0;
            for (int y = 10; y < kH - 10; ++y)
                for (int x = 10; x < kW - 10; ++x) {
                    const Result r = MeasurePixel(cur, prev, float(x) + 0.5f - pan.jitter[0], float(y) + 0.5f - pan.jitter[1], true, -3, 0, {}, true, -3.4f, 0);
                    ++total;
                    if (r.why == kStill) { ++with; CHECK(r.x == -3.4f && r.y == 0 && r.valid == kMeasured); }
                    else if (r.why == kMoving) { ++own; worst = (std::max)(worst, std::hypot(r.x + 3.4f, r.y)); }
                }
            std::printf("  pan 3.4 px with the screen's motion given: with the screen %5.1f%%  own vector %5.1f%%  worst own error %.3f\n",
                        100.0 * with / total, 100.0 * own / total, worst);
            CHECK(with > total * 9 / 10 && worst < 0.6f);
            // A square sliding by 2.5 px over still ground, and the screen's motion is "still": the square keeps its own vector.
            const auto square = [](float x, float y) { return 0.5f + 0.45f * std::sin(x * 0.9f) * std::sin(y * 0.8f + 0.5f); };
            const auto world = [=](float shift) {
                return [=](float x, float y) {
                    const float sx = x - shift;
                    return sx >= 50 && sx < 110 && y >= 30 && y < 90 ? square(sx, y) : Ground(x, y, false);
                };
            };
            const Pair pair = MakePair(world(0), world(2.5f), true);
            const Image c2{pair.current.data(), kW, kH}, p2{pair.previous.data(), kW, kH};
            int moving = 0, inside = 0;
            double mean = 0;
            for (int y = 38; y < 84; ++y)
                for (int x = 60; x < 104; ++x) {
                    const Result r = MeasurePixel(c2, p2, float(x) + 0.5f - pair.jitter[0], float(y) + 0.5f - pair.jitter[1], true, -3, 0, {}, true, 0, 0);
                    ++inside;
                    if (r.why == kMoving) { ++moving; mean += r.x; }
                }
            CHECK(moving > inside * 9 / 10);
            NEAR(mean / moving, -2.5, 0.05);
        }},
        {"through the arbiter a measured field keeps its values and is never labelled the game's own", [] {
            // Astra's correction 5: the tier-1 arithmetic is reused, the label and the counter are not.
            using namespace na::motion;
            const float nan = std::numeric_limits<float>::quiet_NaN();
            float field[]{0, 0, 0.1f, -0.0625f, -3.4f, 2.25f, 40, -70, 9, 9, nan, 1};
            float valid[]{kMeasured, kMeasured, kMeasured, kApproximate, 0, kMeasured}; // measured still, slow, sub-pixel, approximation, nothing known, malformed
            float residual[12];
            std::fill(std::begin(residual), std::end(residual), 50.f);
            float out[12]{};
            uint8_t provenance[6]{};
            Tier1Input measured{true, {field, 6, 1, 12}, {valid, 6, 1, 6}};
            measured.source = Tier1Source::Estimated;
            Tier2Input no_camera{}; // not valid: no depth, no matrices, no identity stand-in
            for (float& f : no_camera.inv_view_proj_current.m) f = nan;
            for (float& f : no_camera.view_proj_previous.m) f = nan;
            Tier3Input stray{};
            stray.valid = true;
            stray.residual = {residual, 6, 1, 12};
            Output o{out, provenance, 6, 1};
            Stats s;
            CHECK(Arbitrate(measured, no_camera, stray, {0.25f, 1.f}, o, &s)); // residual snap 0.25, clamp 1: neither may touch it
            for (int i = 0; i < 4; ++i) {
                CHECK(provenance[i] == NA_MOTION_ESTIMATED);
                NEAR(out[i * 2], field[i * 2], 0);
                NEAR(out[i * 2 + 1], field[i * 2 + 1], 0);
            }
            for (int i = 4; i < 6; ++i) CHECK(provenance[i] == NA_MOTION_INVALID && out[i * 2] == 0 && out[i * 2 + 1] == 0);
            CHECK(s.pixels_estimated == 4 && s.pixels_engine == 0 && s.pixels_camera == 0 && s.pixels_invalid == 2);
            CHECK(s.pixels_residual_added == 0 && s.pixels_residual_snapped == 0 && s.pixels_residual_clamped == 0);
            CHECK(s.tier_used == Tier::EstimatedTotal);
            // The same field from the game itself: the label and counters of before the tag, unchanged.
            Tier1Input native{true, {field, 6, 1, 12}, {valid, 6, 1, 6}};
            CHECK(Arbitrate(native, no_camera, stray, {0.25f, 1.f}, o, &s));
            for (int i = 0; i < 4; ++i) CHECK(provenance[i] == NA_MOTION_ENGINE && out[i * 2] == field[i * 2]);
            CHECK(s.pixels_engine == 4 && s.pixels_estimated == 0 && s.tier_used == Tier::EngineDxil);
        }},
        {"the game's or a replay's vectors cover their own pixels and the measured field keeps the rest", [] {
            // Astra, 2026-11-17, on Cort's Sniper Elite Resistance run of 3621e3a8: with no camera, draw
            // replay's partial field replaced the measured one whole, and every pixel outside its mask
            // was handed no motion at all. The measured field now stands behind tier 1, per pixel.
            using namespace na::motion;
            const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
            //                 replay    replay    hole      hole      hole     replay nan  replay nan  malformed
            float replay[]{    6, -2,    0, 0,     77, 77,   77, 77,   77, 77,  nan, 1,     2, inf,     5, 5};
            float covered[]{   1,        1,        0,        0,        0,       1,          1,          -3};
            float measured[]{  -1, -1,   -1, -1,   0.5f, 3,  -4, 0.25f, 9, 9,   1.5f, -2,   8, 8,       0.75f, 0};
            float confirmed[]{ kMeasured, kMeasured, kMeasured, kApproximate, 0, kMeasured, 0,          kMeasured};
            float out[16]{};
            uint8_t provenance[8]{};
            Tier1Input engine{true, {replay, 8, 1, 16}, {covered, 8, 1, 8}};
            EstimatedInput behind{true, {measured, 8, 1, 16}, {confirmed, 8, 1, 8}};
            Tier2Input no_camera{};
            Tier3Input none{};
            Output o{out, provenance, 8, 1};
            Stats s;
            CHECK(Arbitrate(engine, behind, no_camera, none, {}, o, &s));
            // Where the replay has a vector it is the replay's, zero included, and labelled the game's own.
            CHECK(provenance[0] == NA_MOTION_ENGINE && out[0] == 6 && out[1] == -2);
            CHECK(provenance[1] == NA_MOTION_ENGINE && out[2] == 0 && out[3] == 0);
            // A hole in the replay keeps its measured vector, measured or following the screen, labelled estimated.
            CHECK(provenance[2] == NA_MOTION_ESTIMATED && out[4] == 0.5f && out[5] == 3);
            CHECK(provenance[3] == NA_MOTION_ESTIMATED && out[6] == -4 && out[7] == 0.25f);
            // Neither has anything: not known, and never the stale numbers either field carries there.
            CHECK(provenance[4] == NA_MOTION_INVALID && out[8] == 0 && out[9] == 0);
            // A replay vector that is not a number is no vector: the measured one stands in ...
            CHECK(provenance[5] == NA_MOTION_ESTIMATED && out[10] == 1.5f && out[11] == -2);
            // ... and with no measured one either, nothing is known.
            CHECK(provenance[6] == NA_MOTION_INVALID && out[12] == 0 && out[13] == 0);
            // A malformed replay mask rejects the replay's pixel, not the measured one behind it.
            CHECK(provenance[7] == NA_MOTION_ESTIMATED && out[14] == 0.75f && out[15] == 0);
            CHECK(s.pixels_engine == 2 && s.pixels_estimated == 4 && s.pixels_invalid == 2 && s.pixels_camera == 0);
            CHECK(s.pixels_rejected == 3); // two vectors that are not numbers, one malformed mask
            // Without the slot (how 3621e3a8 called it) the same replay field leaves every hole with nothing.
            CHECK(Arbitrate(engine, no_camera, none, {}, o, &s));
            CHECK(s.pixels_engine == 2 && s.pixels_estimated == 0 && s.pixels_invalid == 6);
            // The measured field alone, with no tier 1 at all: every confirmed pixel, as before.
            CHECK(Arbitrate(Tier1Input{}, behind, no_camera, none, {}, o, &s));
            CHECK(s.pixels_estimated == 6 && s.pixels_engine == 0 && s.pixels_invalid == 2 && s.tier_used == Tier::EstimatedTotal);
            // A measured field of the wrong size is refused whole and counted.
            EstimatedInput wrong{true, {measured, 4, 2, 8}, {confirmed, 8, 1, 8}};
            CHECK(Arbitrate(engine, wrong, no_camera, none, {}, o, &s));
            CHECK(s.pixels_estimated == 0 && s.pixels_engine == 2 && s.pixels_rejected == 8 + 3);
        }},
    });
}
