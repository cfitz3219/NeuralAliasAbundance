// Game motion decider (SE5 menu smear, 2026-11-06): a candidate texture is used only under an
// encoding that predicts the picture better than no motion, on most measured frames. Pure logic
// plus the CPU reference of the GPU score pass.
#include "test_support.hpp"
#include "host_dx12/j2/game_motion_decide.hpp"
#include <cmath>
#include <vector>
using namespace na::j2::game_motion;
namespace {
constexpr uint32_t W = 160, H = 96;
float pattern(float x, float y) { return 0.5f + 0.25f * std::sin(x * 0.37f) * std::cos(y * 0.29f) + 0.2f * std::sin((x + 2 * y) * 0.11f); }
// A textured square [40,100)x[30,70) moving by (dx, dy) px per frame over a textured still wall.
struct Scene {
    std::vector<float> prev, cur;
    void make(float t, float dx, float dy) {
        prev.assign(W * H, 0); cur.assign(W * H, 0);
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                const float fx = static_cast<float>(x) + 0.5f, fy = static_cast<float>(y) + 0.5f;
                auto img = [&](float s) { // the object at offset s*(dx,dy)
                    const float ox = fx - s * dx, oy = fy - s * dy;
                    const bool in = ox >= 40 && ox < 100 && oy >= 30 && oy < 70;
                    return in ? 1.0f - pattern(ox * 1.7f, oy * 1.3f) * 0.8f : pattern(fx, fy) * 0.6f;
                };
                prev[y * W + x] = img(t); cur[y * W + x] = img(t + 1);
            }
    }
    bool on_object(uint32_t x, uint32_t y, float t, float dx, float dy) const {
        const float ox = static_cast<float>(x) + 0.5f - (t + 1) * dx, oy = static_cast<float>(y) + 0.5f - (t + 1) * dy;
        return ox >= 40 && ox < 100 && oy >= 30 && oy < 70;
    }
};
// Candidate holding the object's motion in UV, game convention current - previous (so the arbiter
// encoding is "uv, both signs negative" = h 4 + 1 + 2 = 7).
std::vector<float> uv_candidate(const Scene& s, float t, float dx, float dy) {
    std::vector<float> c(W * H * 2, 0.f);
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x)
            if (s.on_object(x, y, t, dx, dy)) { c[(y * W + x) * 2] = dx / W; c[(y * W + x) * 2 + 1] = dy / H; }
    return c;
}
Measure measure(const std::vector<float>& cand, const Scene& s, bool unorm = false) {
    Measure m; ScoreCpu(cand.data(), s.cur.data(), s.prev.data(), W, H, unorm, 2, &m); return m;
}
}
int main() {
    return test::run({
        {"true motion in UV locks the right encoding; the scale maps it to pixels", [] {
            Decider d; Scene s;
            for (int f = 0; f < 40; ++f) { s.make(static_cast<float>(f % 5), 3.5f, -1.0f); d.Add(0, measure(uv_candidate(s, static_cast<float>(f % 5), 3.5f, -1.0f), s)); }
            CHECK(d.UsedCandidate() == 0);
            CHECK(d.UsedEncoding() == 7);
            float sx, sy; Scale(7, W, H, &sx, &sy);
            CHECK(std::fabs(3.5f / W * sx + 3.5f) < 1e-4f && std::fabs(-1.0f / H * sy - 1.0f) < 1e-4f); // v = previous - current
            CHECK(d.LastRatio() < 0.5);
        }},
        {"an empty texture is never used", [] {
            Decider d; Scene s;
            const std::vector<float> empty(W * H * 2, 0.f);
            for (int f = 0; f < 80; ++f) { s.make(static_cast<float>(f), 3.5f, 0); d.Add(0, measure(empty, s)); }
            CHECK(d.UsedCandidate() == -1 && d.GoodFrames() == 0);
        }},
        {"motion that does not match the picture (another frame's, or not motion) is never used", [] {
            Decider d; Scene s;
            for (int f = 0; f < 80; ++f) {
                s.make(static_cast<float>(f), 3.5f, 0);
                d.Add(0, measure(uv_candidate(s, static_cast<float>(f), 0.0f, 6.0f), s)); // wrong direction (and a different region)
                std::vector<float> noise(W * H * 2);
                for (size_t i = 0; i < noise.size(); ++i) noise[i] = std::sin(static_cast<float>(i) * 12.9898f) * 0.05f;
                d.Add(1, measure(noise, s));
            }
            CHECK(d.UsedCandidate() == -1);
        }},
        {"a still picture with a written but zero texture leaves nothing to score", [] {
            Decider d; Scene s; s.make(0, 0, 0);
            for (int f = 0; f < 40; ++f) d.Add(0, measure(std::vector<float>(W * H * 2, 0.f), s));
            CHECK(d.Last().scored == 0 && d.UsedCandidate() == -1);
        }},
        {"UNORM encoding (0.5 = still) is decoded before scoring", [] {
            Decider d; Scene s;
            for (int f = 0; f < 40; ++f) {
                s.make(static_cast<float>(f % 3), -2.0f, 2.5f);
                auto c = uv_candidate(s, static_cast<float>(f % 3), -2.0f, 2.5f);
                for (auto& v : c) v = v * 0.5f + 0.5f;
                d.Add(0, measure(c, s, true));
            }
            CHECK(d.UsedCandidate() == 0 && d.UsedEncoding() == 7);
        }},
        {"a used encoding is dropped when the texture stops predicting, and when the candidate changes", [] {
            Decider d; Scene s;
            for (int f = 0; f < 40; ++f) { s.make(static_cast<float>(f % 5), 3.5f, -1.0f); d.Add(0, measure(uv_candidate(s, static_cast<float>(f % 5), 3.5f, -1.0f), s)); }
            CHECK(d.UsedCandidate() == 0);
            for (int f = 0; f < 40; ++f) { s.make(static_cast<float>(f % 5), 3.5f, -1.0f); d.Add(0, measure(uv_candidate(s, static_cast<float>(f % 5), 0.0f, 6.0f), s)); } // no longer this picture's motion
            CHECK(d.UsedCandidate() == -1 && d.Dropped() == 1);
            for (int f = 0; f < 40; ++f) { s.make(static_cast<float>(f % 5), 3.5f, -1.0f); d.Add(0, measure(uv_candidate(s, static_cast<float>(f % 5), 3.5f, -1.0f), s)); }
            CHECK(d.UsedCandidate() == 0);
            d.ResetCandidate(0);
            CHECK(d.UsedCandidate() == -1 && d.Dropped() == 2);
        }},
    });
}
