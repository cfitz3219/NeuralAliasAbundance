// The display curve fitted from the Referee's thumbnails (docs/NA1-ENHANCER.md section 4).
#include "core/enhancer_screen_curve.hpp"
#include "test_support.hpp"

#include <cstring>
#include <limits>
#include <vector>

using na::enhancer::FitScreenCurve;
using na::na1::ref::Curve;
using na::na1::ref::Shown;

namespace {
constexpr uint32_t kCells = 128 * 72;
// A game's tone curve: exposure, then a soft shoulder.
float Tone(float y) { const float t = 1.5f * (std::exp2(y) - 1e-3f); return t / (1.f + t); }
float Noise(uint32_t i)
{
    uint32_t n = (i + 1) * 2654435761u;
    n ^= n >> 15; n *= 2246822519u; n ^= n >> 13;
    return float(n & 0xffffu) / 65535.f;
}
// Scene brightness spread over ten stops, as NA1's own y.
std::vector<float> Scene() { std::vector<float> y(kCells); for (uint32_t i = 0; i < kCells; ++i) y[i] = -8.f + 10.f * Noise(i); return y; }
float WorstError(const Curve& c, float from, float to)
{
    float worst = 0;
    for (float y = from; y <= to; y += .05f) worst = (std::max)(worst, std::fabs(Shown(c, y) - Tone(y)));
    return worst;
}
}

int main()
{
    return test::run({
        {"a known tone curve is recovered", [] {
            const auto y = Scene();
            std::vector<float> screen(kCells);
            for (uint32_t i = 0; i < kCells; ++i) screen[i] = Tone(y[i]);
            Curve c;
            const char* why = nullptr;
            CHECK(FitScreenCurve(y.data(), screen.data(), kCells, c, &why, na::na1::ref::kLumFloor));
            CHECK(why && std::strlen(why) > 0);
            CHECK(c.mode == 1);
            NEAR(c.x0, -7.95f, .05); NEAR(c.x0 + 15 * c.dx, 1.95f, .05);
            NEAR(c.mean_y, -3.f, .1);
            CHECK(WorstError(c, -7.9f, 1.9f) < .015f);
            for (int i = 1; i < 16; ++i) CHECK(c.cy[i] >= c.cy[i - 1]);
            NEAR(Shown(c, -30.f), c.cy[0], 0);   // held at the ends
            NEAR(Shown(c, 30.f), c.cy[15], 0);
        }},
        {"one cell in ten showing something else (a HUD) does not move it", [] {
            const auto y = Scene();
            std::vector<float> screen(kCells);
            for (uint32_t i = 0; i < kCells; ++i) screen[i] = i % 10 == 3 ? (i % 20 == 3 ? 1.f : 0.f) : Tone(y[i]);
            Curve c;
            const char* why = nullptr;
            CHECK(FitScreenCurve(y.data(), screen.data(), kCells, c, &why, na::na1::ref::kLumFloor));
            CHECK(WorstError(c, -7.9f, 1.9f) < .03f);
            // All of the outliers bright, as a white panel over the picture would be.
            for (uint32_t i = 0; i < kCells; ++i) screen[i] = i % 10 == 3 ? 1.f : Tone(y[i]);
            CHECK(FitScreenCurve(y.data(), screen.data(), kCells, c, &why, na::na1::ref::kLumFloor));
            CHECK(WorstError(c, -7.9f, 1.9f) < .04f);
        }},
        {"the Referee's thumbnails (log2 of luma plus 1e-4) are taken as they are", [] {
            const auto y = Scene();
            std::vector<float> thumb(kCells), screen(kCells);
            for (uint32_t i = 0; i < kCells; ++i) {
                const float lum = std::exp2(y[i]) - 1e-3f;
                thumb[i] = std::log2(lum + na::enhancer::kRefereeLumaFloor);
                screen[i] = Tone(y[i]);
            }
            Curve c;
            const char* why = nullptr;
            CHECK(FitScreenCurve(thumb.data(), screen.data(), kCells, c, &why));
            CHECK(WorstError(c, -7.9f, 1.9f) < .015f);
            NEAR(na::enhancer::YFromLog2(std::log2(.25f + 1e-4f), 1e-4f), std::log2(.25f + 1e-3f), 1e-5);
        }},
        {"noisy cells still give a curve that never falls", [] {
            const auto y = Scene();
            std::vector<float> screen(kCells);
            for (uint32_t i = 0; i < kCells; ++i) screen[i] = Tone(y[i]) * (.6f + .8f * Noise(i * 7 + 1));
            Curve c;
            const char* why = nullptr;
            CHECK(FitScreenCurve(y.data(), screen.data(), 600, c, &why, na::na1::ref::kLumFloor));
            for (int i = 1; i < 16; ++i) CHECK(c.cy[i] >= c.cy[i - 1]);
            for (float v : c.cy) CHECK(v >= 0 && v <= 1);
        }},
        {"brightness levels nobody measured are filled in between their neighbours", [] {
            std::vector<float> y(kCells), screen(kCells);
            for (uint32_t i = 0; i < kCells; ++i) {
                y[i] = i % 2 ? -6.f + 1.5f * Noise(i) : -1.f + 2.f * Noise(i);  // a dark part and a bright part, nothing between
                screen[i] = Tone(y[i]);
            }
            Curve c;
            const char* why = nullptr;
            CHECK(FitScreenCurve(y.data(), screen.data(), kCells, c, &why, na::na1::ref::kLumFloor));
            for (int i = 1; i < 16; ++i) CHECK(c.cy[i] >= c.cy[i - 1]);
            CHECK(Shown(c, -2.75f) > Tone(-4.5f) && Shown(c, -2.75f) < Tone(-1.f));
        }},
        {"when no curve can be trusted it says why and leaves the old one alone", [] {
            const auto y = Scene();
            std::vector<float> screen(kCells);
            for (uint32_t i = 0; i < kCells; ++i) screen[i] = Tone(y[i]);
            Curve c = na::enhancer::AssumedCurve(-2.5f);
            const Curve kept = c;
            const char* why = nullptr;
            CHECK(!FitScreenCurve(y.data(), screen.data(), 40, c, &why));
            CHECK(why && std::strstr(why, "too few"));
            CHECK(!FitScreenCurve(nullptr, screen.data(), kCells, c, &why) && std::strlen(why) > 0);
            std::vector<float> narrow(kCells);
            for (uint32_t i = 0; i < kCells; ++i) narrow[i] = -3.f + .5f * Noise(i);
            CHECK(!FitScreenCurve(narrow.data(), screen.data(), kCells, c, &why, na::na1::ref::kLumFloor));
            CHECK(std::strstr(why, "too little brightness"));
            std::vector<float> covered(kCells, .4f);  // the screen shows a loading picture, not the scene
            CHECK(!FitScreenCurve(y.data(), covered.data(), kCells, c, &why, na::na1::ref::kLumFloor));
            CHECK(std::strstr(why, "does not get brighter"));
            std::vector<float> broken(kCells, std::numeric_limits<float>::quiet_NaN());
            CHECK(!FitScreenCurve(broken.data(), screen.data(), kCells, c, &why));
            CHECK(c.mode == kept.mode && c.mean_y == kept.mean_y && c.x0 == kept.x0 && c.dx == kept.dx);
            CHECK(!FitScreenCurve(y.data(), screen.data(), 40, c, nullptr));  // the reason is optional
        }},
        {"the assumed curve puts the picture's average at middle grey, and 8-bit is what it is", [] {
            const Curve c = na::enhancer::AssumedCurve(-4.f);
            CHECK(c.mode == 2);
            NEAR(Shown(c, -4.f), .18f / 1.18f, 1e-6);
            CHECK(Shown(c, 6.f) < 1.f && Shown(c, 6.f) > .99f);
            NEAR(na::enhancer::AssumedCurve(std::numeric_limits<float>::quiet_NaN()).mean_y, 0.f, 0);
            const Curve d = na::enhancer::DisplayReferredCurve();
            CHECK(d.mode == 0);
            NEAR(Shown(d, na::na1::ref::LogLum(.5f)), .5f, 1e-6);
        }},
    });
}
