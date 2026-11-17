// The screen's own motion (core/screen_motion.hpp) with core/total_motion.hpp, on pictures made
// like a real frame: Cort's Sniper Elite Resistance captures of 2026-11-17 (build 3621e3a8) showed
// about two thirds of a real frame too low in contrast for the per-pixel measurement, and those
// pixels were handed the block search's raw vector, which changes with every jitter sample. The
// earlier fixtures are textured everywhere and never showed it. Here most of the picture is dark and
// flat, the depth runs from a sky to near ground, and the camera is a real pinhole camera with a
// lens the fit is never told.
//
// What must hold:
//  - exact camera motion (walk, strafe, turn, roll, all at once) is reproduced by the eleven terms
//    to a small fraction of a pixel, for two different lenses and both depth directions' far ends;
//  - noise, things moving by themselves and plain wrong samples do not pull the fit away;
//  - too few samples, or samples that do not agree, give no model at all;
//  - the model is never asked about a depth nearer than its nearest agreeing sample;
//  - on a mostly flat picture, still or moving, a pixel that cannot be measured follows the fit and
//    never the search's vector, however wrong the search is there. Nearer than the nearest patch
//    that can be measured it carries that patch's depth's motion: less than the truth, and stated.
// The numbers asserted are loose bounds around what this reference measures today; the printed
// lines are the measurements themselves.
#include "test_support.hpp"
#include "core/screen_motion.hpp"
#include "core/total_motion.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace na::motion;

namespace {
constexpr uint32_t kW = 640, kH = 360;
constexpr float kNear = 0.1f;

struct Rng {
    uint32_t s;
    float Next() { s = s * 1664525u + 1013904223u; return float(s >> 8) / 16777216.0f; }
    float Gauss() { float a = 0; for (int i = 0; i < 6; ++i) a += Next(); return (a - 3.0f) * 1.41421356f; }
};

// A pinhole camera: `focal` in pixels, the picture's centre its axis. The previous camera sees a
// point X of the current camera's space at R X + T.
struct Camera {
    float focal = 400;
    float wx = 0, wy = 0, wz = 0; // rotation about x, y, z (radians), applied in that order
    float tx = 0, ty = 0, tz = 0; // world units
};
// v = previous position - current position, for the pixel (px, py) whose depth value is `nearness` from
// the far end (near / distance; 0 = infinitely far).
void TrueFlow(const Camera& c, float px, float py, float nearness, float& vx, float& vy)
{
    const double e = (std::max)(double(nearness), 1e-9), z = double(kNear) / e;
    double X = (double(px) - 0.5 * kW) / c.focal * z, Y = (double(py) - 0.5 * kH) / c.focal * z, Z = z;
    { const double cs = std::cos(double(c.wx)), sn = std::sin(double(c.wx)); const double y = Y * cs - Z * sn, zz = Y * sn + Z * cs; Y = y; Z = zz; }
    { const double cs = std::cos(double(c.wy)), sn = std::sin(double(c.wy)); const double x = X * cs + Z * sn, zz = -X * sn + Z * cs; X = x; Z = zz; }
    { const double cs = std::cos(double(c.wz)), sn = std::sin(double(c.wz)); const double x = X * cs - Y * sn, y = X * sn + Y * cs; X = x; Y = y; }
    X += c.tx; Y += c.ty; Z += c.tz;
    vx = float(X / Z * c.focal + 0.5 * kW - double(px));
    vy = float(Y / Z * c.focal + 0.5 * kH - double(py));
}

// The scene's depth: sky above the horizon, ground coming nearer down the picture, a wall on the right.
float FarAt(float px, float py)
{
    const float horizon = 0.42f * kH;
    float e = py <= horizon ? 0.0f : 0.045f * (py - horizon) / (float(kH) - horizon);
    if (px > 0.8f * kW && py > 0.2f * kH) e = (std::max)(e, 0.012f + 0.01f * (px - 0.8f * kW) / (0.2f * kW));
    return e;
}

std::vector<screen::Sample> BlockSamples(const Camera& c)
{
    std::vector<screen::Sample> s;
    for (uint32_t y = 4; y < kH; y += 8)
        for (uint32_t x = 4; x < kW; x += 8) {
            screen::Sample a;
            a.px = float(x) + 0.5f;
            a.py = float(y) + 0.5f;
            a.nearness = FarAt(a.px, a.py);
            TrueFlow(c, a.px, a.py, a.nearness, a.vx, a.vy);
            a.confirmed = true;
            s.push_back(a);
        }
    return s;
}

struct Errors { float mean = 0, worst = 0; };
// `measured_depths_only`: leave out the pixels nearer than the model's nearness_limit, where it carries the
// nearest measured depth's motion on purpose.
Errors Against(const screen::Model& m, const Camera& c, bool measured_depths_only = false)
{
    Errors e;
    int n = 0;
    for (uint32_t y = 0; y < kH; y += 3)
        for (uint32_t x = 0; x < kW; x += 3) {
            const float px = float(x) + 0.5f, py = float(y) + 0.5f, nearness = FarAt(px, py);
            if (measured_depths_only && nearness > m.nearness_limit) continue;
            float tx, ty, mx, my;
            TrueFlow(c, px, py, nearness, tx, ty);
            screen::Evaluate(m, px, py, nearness, kW, kH, mx, my);
            const float d = std::hypot(mx - tx, my - ty);
            e.mean += d;
            e.worst = (std::max)(e.worst, d);
            ++n;
        }
    e.mean /= float((std::max)(n, 1));
    return e;
}

const Camera kMoves[] = {
    {400, 0, 0, 0, 0, 0, 0},                       // still
    {400, 0, 0, 0, 0, 0, -0.07f},                  // walking forward
    {400, 0, 0, 0, 0.07f, 0, 0},                   // strafing
    {400, 0, 0.004f, 0, 0, 0, 0},                  // turning
    {400, 0.0015f, 0.003f, 0, 0.02f, 0, -0.06f},   // walking, turning and looking up
    {400, 0, 0, 0.003f, 0.05f, 0.01f, 0},          // rolling and strafing
    {400, 0, 0.02f, 0, 0.03f, 0, 0},               // a fast turn
    {250, 0.002f, -0.006f, 0.001f, -0.03f, 0.01f, 0.05f}, // a wide lens, everything at once
};
const char* const kMoveNames[] = {"still", "walk forward", "strafe", "turn", "walk + turn + look up", "roll + strafe", "fast turn",
                                  "wide lens, everything"};

// A picture like a real frame: dark and flat almost everywhere, textured in patches.
float Texture(float x, float y)
{
    const bool patch = (std::fmod(x, 96.0f) < 30.0f && std::fmod(y, 72.0f) < 26.0f);
    if (!patch) return 0.11f + 0.004f * std::sin(x * 0.013f) * std::cos(y * 0.017f); // far under texture_min
    return 0.45f + 0.2f * std::sin(x * 0.55f + y * 0.21f) + 0.16f * std::sin(x * 0.19f - y * 0.61f) + 0.1f * std::cos(x * 0.83f + y * 0.37f);
}
float Q10(float v) { return std::round(std::clamp(v, 0.0f, 1.0f) * 1023.0f) / 1023.0f; }

struct Scene {
    std::vector<float> current, previous;
};
// The current picture shows Texture at each pixel; the previous one shows the same surface where the
// camera saw it a frame ago: previous(p + v(p)) = current(p), inverted by fixed point.
Scene MakeScene(const Camera& c)
{
    Scene s;
    s.current.resize(size_t(kW) * kH);
    s.previous.resize(size_t(kW) * kH);
    for (uint32_t y = 0; y < kH; ++y)
        for (uint32_t x = 0; x < kW; ++x) {
            const float qx = float(x) + 0.5f, qy = float(y) + 0.5f;
            s.current[size_t(y) * kW + x] = Q10(Texture(qx, qy));
            float gx = qx, gy = qy;
            for (int it = 0; it < 6; ++it) {
                float vx, vy;
                TrueFlow(c, gx, gy, FarAt(gx, gy), vx, vy);
                gx = qx - vx;
                gy = qy - vy;
            }
            s.previous[size_t(y) * kW + x] = Q10(Texture(gx, gy));
        }
    return s;
}
} // namespace

int main()
{
    return test::run({
        {"exact camera motion is reproduced with no lens given", [] {
            int i = 0;
            for (const Camera& c : kMoves) {
                const auto samples = BlockSamples(c);
                const screen::Model m = screen::Fit(samples.data(), samples.size(), kW, kH);
                CHECK(m.valid);
                const Errors e = Against(m, c);
                float biggest = 0;
                for (const auto& a : samples) biggest = (std::max)(biggest, std::hypot(a.vx, a.vy));
                std::printf("  %-24s largest motion %6.2f px  fit error mean %.4f worst %.4f  agree %.0f of %.0f\n", kMoveNames[i], biggest, e.mean,
                            e.worst, m.agree, m.samples);
                // The eleven terms are the small-motion form: a fraction of a pixel on the fastest of these.
                CHECK(e.mean < 0.05f && e.worst < 0.35f);
                CHECK(m.agree > 0.98f * m.samples);
                ++i;
            }
        }},
        {"noise, things moving by themselves and wrong samples do not pull the fit away", [] {
            const Camera c = kMoves[4];
            auto samples = BlockSamples(c);
            Rng rng{12345};
            int own = 0, wrong = 0;
            for (auto& a : samples) {
                a.vx += 0.15f * rng.Gauss();
                a.vy += 0.15f * rng.Gauss();
                // A vehicle crossing the lower left: a quarter of the ground moves by itself.
                if (a.px < 0.3f * kW && a.py > 0.55f * kH) { a.vx += 9.0f; a.vy -= 2.0f; ++own; }
                else if (rng.Next() < 0.12f) { a.vx = 60.0f * (rng.Next() - 0.5f); a.vy = 60.0f * (rng.Next() - 0.5f); ++wrong; }
            }
            const screen::Model m = screen::Fit(samples.data(), samples.size(), kW, kH);
            CHECK(m.valid);
            const Errors e = Against(m, c);
            std::printf("  %zu samples, %d on a thing moving by itself, %d plain wrong: fit error mean %.4f worst %.4f  agree %.0f\n", samples.size(), own,
                        wrong, e.mean, e.worst, m.agree);
            CHECK(e.mean < 0.05f && e.worst < 0.35f);
            CHECK(m.agree >= float(samples.size() - size_t(own) - size_t(wrong)) - 2.0f); // every sample of the scene itself
            // One plain least-squares step alone is pulled off by the same samples: the reweighting is what holds.
            screen::Params plain;
            plain.steps = 1;
            const Errors p = Against(screen::Fit(samples.data(), samples.size(), kW, kH, plain), c);
            std::printf("  the first, unweighted step alone: mean %.4f worst %.4f\n", p.mean, p.worst);
            CHECK(p.mean > 4 * e.mean);
        }},
        {"too few samples or samples that do not agree give no model", [] {
            auto samples = BlockSamples(kMoves[3]);
            size_t kept = 0;
            for (auto& a : samples) a.confirmed = kept++ < 200;
            CHECK(!screen::Fit(samples.data(), samples.size(), kW, kH).valid);
            for (auto& a : samples) a.confirmed = true;
            CHECK(screen::Fit(samples.data(), samples.size(), kW, kH).valid);
            // Every sample its own random vector: nothing to agree with.
            Rng rng{99};
            for (auto& a : samples) { a.vx = 80.0f * (rng.Next() - 0.5f); a.vy = 80.0f * (rng.Next() - 0.5f); }
            const screen::Model none = screen::Fit(samples.data(), samples.size(), kW, kH);
            std::printf("  random vectors: agree %.0f of %.0f, valid %d\n", none.agree, none.samples, none.valid ? 1 : 0);
            CHECK(!none.valid);
            // Unusable numbers are not samples.
            auto bad = BlockSamples(kMoves[3]);
            for (size_t i = 0; i < bad.size(); i += 3) { bad[i].vx = std::nanf(""); bad[i + 1].vy = 4000.0f; }
            const screen::Model m = screen::Fit(bad.data(), bad.size(), kW, kH);
            CHECK(m.valid && Against(m, kMoves[3]).worst < 0.35f);
        }},
        {"the model is not asked about depths nearer than its nearest agreeing sample", [] {
            // Only the sky and the far ground can be measured, and the samples carry a little noise.
            const Camera still = kMoves[0];
            auto samples = BlockSamples(still);
            Rng rng{7};
            float nearness_kept = 0;
            for (auto& a : samples) {
                a.confirmed = a.nearness < 0.004f;
                a.vx += 0.08f * rng.Gauss();
                a.vy += 0.08f * rng.Gauss();
                if (a.confirmed) nearness_kept = (std::max)(nearness_kept, a.nearness);
            }
            const screen::Model m = screen::Fit(samples.data(), samples.size(), kW, kH);
            CHECK(m.valid && m.nearness_limit <= nearness_kept && m.nearness_limit > 0);
            const Errors limited = Against(m, still);
            // The same terms asked about every depth: the parallax terms carry their noise ten times further.
            screen::Model open = m;
            open.nearness_limit = 1.0f;
            const Errors unlimited = Against(open, still);
            std::printf("  still scene, only depths under 0.004 measurable: worst false motion %.4f px limited, %.4f px if asked anyway\n", limited.worst,
                        unlimited.worst);
            CHECK(limited.worst < 0.08f);
            CHECK(unlimited.worst > 2 * limited.worst);
            float a0, a1, b0, b1;
            screen::Evaluate(m, 100.5f, 300.5f, m.nearness_limit, kW, kH, a0, a1);
            screen::Evaluate(m, 100.5f, 300.5f, 0.04f, kW, kH, b0, b1);
            CHECK(a0 == b0 && a1 == b1);
        }},
        {"on a mostly flat picture a pixel that cannot be measured follows the fit, never the search", [] {
            int i = 0;
            for (const Camera& c : {kMoves[0], kMoves[2], kMoves[4]}) {
                const Scene scene = MakeScene(c);
                const total::Image cur{scene.current.data(), kW, kH}, prev{scene.previous.data(), kW, kH};
                // The search: right to the whole pixel where there is texture; on flat ground it returns
                // whatever the jitter made of it, a different wrong vector on every block.
                Rng rng{4242u + uint32_t(i)};
                std::vector<float> raw(size_t(kW / 8) * (kH / 8) * 2);
                for (uint32_t by = 0; by < kH / 8; ++by)
                    for (uint32_t bx = 0; bx < kW / 8; ++bx) {
                        const float px = float(bx * 8 + 4) + 0.5f, py = float(by * 8 + 4) + 0.5f;
                        float tx, ty;
                        TrueFlow(c, px, py, FarAt(px, py), tx, ty);
                        const total::Window w = total::WindowAt(cur, px, py);
                        float* r = &raw[(size_t(by) * (kW / 8) + bx) * 2];
                        if (w.contrast >= 0.03f) { r[0] = std::round(tx); r[1] = std::round(ty); }
                        else { r[0] = std::round(14.0f * (rng.Next() - 0.5f)); r[1] = std::round(14.0f * (rng.Next() - 0.5f)); }
                    }
                // The fit's samples: the block centres the pictures can measure.
                std::vector<screen::Sample> samples;
                for (uint32_t by = 0; by < kH / 8; ++by)
                    for (uint32_t bx = 0; bx < kW / 8; ++bx) {
                        screen::Sample a;
                        a.px = float(bx * 8 + 4) + 0.5f;
                        a.py = float(by * 8 + 4) + 0.5f;
                        a.nearness = FarAt(a.px, a.py);
                        const float* r = &raw[(size_t(by) * (kW / 8) + bx) * 2];
                        a.confirmed = total::SamplePixel(cur, prev, a.px, a.py, true, r[0], r[1], a.vx, a.vy);
                        samples.push_back(a);
                    }
                const screen::Model model = screen::Fit(samples.data(), samples.size(), kW, kH);
                CHECK(model.valid);
                int pixels = 0, follows = 0, with = 0, own = 0, nothing = 0, guesses_old = 0, off_new = 0, off_old = 0, nearer = 0;
                double err_new = 0, err_old = 0, err_nearer = 0;
                for (uint32_t y = 12; y < kH - 12; y += 2)
                    for (uint32_t x = 12; x < kW - 12; x += 2) {
                        const float px = float(x) + 0.5f, py = float(y) + 0.5f, nearness = FarAt(px, py);
                        const float* r = &raw[(size_t(y / 8) * (kW / 8) + x / 8) * 2];
                        float sx, sy, tx, ty;
                        screen::Evaluate(model, px, py, nearness, kW, kH, sx, sy);
                        TrueFlow(c, px, py, nearness, tx, ty);
                        const total::Result now = total::MeasurePixel(cur, prev, px, py, true, r[0], r[1], {}, true, sx, sy);
                        ++pixels;
                        if (now.why == total::kScreen) { ++follows; CHECK(now.x == sx && now.y == sy && now.valid == total::kApproximate); }
                        else if (now.why == total::kStill) ++with;
                        else if (now.why == total::kMoving) ++own;
                        else ++nothing;
                        const float en = std::hypot(now.x - tx, now.y - ty);
                        if (nearness > model.nearness_limit && now.why == total::kScreen) {
                            // Nearer than anything measured: the motion of the nearest measured depth, no more.
                            float lx, ly;
                            TrueFlow(c, px, py, model.nearness_limit, lx, ly);
                            CHECK(std::hypot(now.x - lx, now.y - ly) < 0.3f);
                            ++nearer;
                            err_nearer += en;
                        } else {
                            err_new += en;
                            if (en > 1.0f) ++off_new;
                        }
                        // What 3621e3a8 handed on: the search's vector wherever the pixel was not confirmed.
                        const total::Result before = total::MeasurePixel(cur, prev, px, py, true, r[0], r[1]);
                        const float ox = before.valid == total::kMeasured ? before.x : r[0], oy = before.valid == total::kMeasured ? before.y : r[1];
                        if (before.valid != total::kMeasured) ++guesses_old;
                        const float eo = std::hypot(ox - tx, oy - ty);
                        err_old += eo;
                        if (eo > 1.0f) ++off_old;
                    }
                const Errors fit = Against(model, c, true);
                const int judged = pixels - nearer;
                std::printf("  %-22s cannot be measured %4.1f%% of pixels; fit error at the depths it measured: mean %.3f worst %.3f\n"
                            "      now:    follows the fit %4.1f%%  with the screen %4.1f%%  own vector %4.1f%%  mean error %.3f px  over 1 px %5.2f%%\n"
                            "      nearer than anything measured: %4.1f%% of pixels, given the nearest measured depth's motion, mean %.3f px short\n"
                            "      before: the search's vector on %4.1f%%  mean error %.3f px  over 1 px %5.2f%%\n",
                            i == 0 ? "still" : i == 1 ? "strafe" : "walk + turn + look up", 100.0 * follows / pixels, fit.mean, fit.worst, 100.0 * follows / pixels,
                            100.0 * with / pixels, 100.0 * own / pixels, err_new / judged, 100.0 * off_new / judged, 100.0 * nearer / pixels,
                            nearer ? err_nearer / nearer : 0.0, 100.0 * guesses_old / pixels, err_old / pixels, 100.0 * off_old / pixels);
                CHECK(nothing == 0);
                CHECK(follows > pixels / 2);            // most of the picture cannot be measured, as on a real frame
                CHECK(fit.mean < 0.08f && fit.worst < 0.5f);
                CHECK(err_new / judged < 0.1);
                CHECK(off_new < judged / 100);
                CHECK(nearer < pixels / 6);
                CHECK(off_old > pixels / 3);            // what the run showed: most of the screen handed guesses
                ++i;
            }
        }},
    });
}
