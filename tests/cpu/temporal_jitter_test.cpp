// reconstructed 2026-10 — independent sample values and projected displacement.
#include "test_support.hpp"
#include "core/temporal_jitter.hpp"
#include <cmath>
using namespace neural_alias;
int main() { return test::run({
    {"overlay jitter configurations wrap and reset both axes", [] {
        for (uint32_t i = 0; i < JitterDiagnostic::count; ++i) {
            JitterDiagnostic config{i};
            auto xy = config.Declared(.25f, -.125f);
            const auto& scale = JitterDiagnostic::scales[i];
            CHECK(xy.x == .25f * scale.x && xy.y == -.125f * scale.y);
            JitterDiagnostic swapped{config.ToggleAxes()};
            auto yx = swapped.Declared(.25f, -.125f);
            CHECK(yx.x == xy.y && yx.y == xy.x);
            CHECK(swapped.ToggleAxes() == i);
        }
        JitterDiagnostic last{JitterDiagnostic::count - 1};
        CHECK(last.WithConfig(last.Config() + 1) == 0);
        JitterDiagnostic normal{};
        CHECK(normal.Declared(.25f, -.125f).x == .25f && normal.Declared(.25f, -.125f).y == -.125f);
    }},
    {"a mesh that tests a sampled depth layer in its shader takes the scene's sample", [] {
        ShaderDepthReader r;
        r.active = r.scene_depth_bound = r.layer_took_sample = r.reads_layer = r.mesh = r.several_targets = r.scene_viewport = true;
        CHECK(ShaderDepthReaderTakesJitter(r));
        CHECK(ShaderDepthReaderRefusal(r) == ShaderDepthRefusal::None);
        // The hardware-tested draw is the ordinary rule's, not this one's.
        CHECK(SceneDrawTakesJitter(true, 7, 7, true, false));
        auto no = r; no.tests_depth = true; CHECK(!ShaderDepthReaderTakesJitter(no));
        // Each missing fact keeps the game's own viewport, and says which one.
        no = r; no.layer_took_sample = false; CHECK(!ShaderDepthReaderTakesJitter(no)); CHECK(ShaderDepthReaderRefusal(no) == ShaderDepthRefusal::NoLayer);
        no = r; no.reads_layer = false; CHECK(!ShaderDepthReaderTakesJitter(no)); CHECK(ShaderDepthReaderRefusal(no) == ShaderDepthRefusal::NotRead);
        no = r; no.writes_layer = true; CHECK(!ShaderDepthReaderTakesJitter(no)); CHECK(ShaderDepthReaderRefusal(no) == ShaderDepthRefusal::WritesLayer);
        no = r; no.several_targets = false; CHECK(!ShaderDepthReaderTakesJitter(no)); CHECK(ShaderDepthReaderRefusal(no) == ShaderDepthRefusal::OneTarget);
        no = r; no.scene_viewport = false; CHECK(!ShaderDepthReaderTakesJitter(no)); CHECK(ShaderDepthReaderRefusal(no) == ShaderDepthRefusal::Viewport);
        // A full-screen pass, another depth buffer, or no sample: never.
        no = r; no.mesh = false; CHECK(!ShaderDepthReaderTakesJitter(no));
        no = r; no.scene_depth_bound = false; CHECK(!ShaderDepthReaderTakesJitter(no));
        no = r; no.active = false; CHECK(!ShaderDepthReaderTakesJitter(no));
    }},
    {"a mesh the fixed rules left behind moves only while the Referee asks", [] {
        StillMesh m;
        m.asked = m.active = m.scene_depth_bound = m.mesh = m.scene_viewport = true;
        CHECK(StillMeshTakesJitter(m));
        auto no = m; no.asked = false; CHECK(!StillMeshTakesJitter(no));       // no ask: the game's own grid
        no = m; no.active = false; CHECK(!StillMeshTakesJitter(no));           // no sample, or the scene is handed over
        no = m; no.tests_depth = true; CHECK(!StillMeshTakesJitter(no));       // the ordinary rule's draw
        no = m; no.scene_depth_bound = false; CHECK(!StillMeshTakesJitter(no)); // another view
        no = m; no.mesh = false; CHECK(!StillMeshTakesJitter(no));             // a full-screen pass never moves
        no = m; no.scene_viewport = false; CHECK(!StillMeshTakesJitter(no));   // a partial view
    }},
    {"known Halton samples and wrapping", [] {
        const double xs[] = {0, -.25, .25, -.375};
        const double ys[] = {-1./6, 1./6, -7./18, -1./18};
        for (uint32_t i = 0; i < 4; ++i) {
            auto a = temporal_jitter(i, 8), b = temporal_jitter(i + 8, 8);
            NEAR(a.x_pixels, xs[i], 1e-12); NEAR(a.y_pixels, ys[i], 1e-12);
            NEAR(a.x_pixels, b.x_pixels, 0); NEAR(a.y_pixels, b.y_pixels, 0);
        }
        for (uint32_t i = 0; i < 4096; ++i) {
            auto s = temporal_jitter(i, 4096);
            CHECK(s.x_pixels >= -.5 && s.x_pixels < .5 && s.y_pixels >= -.5 && s.y_pixels < .5);
        }
    }},
    {"R2 samples: known values, range, mean near zero, wrapping", [] {
        const auto a = temporal_jitter_r2(0, 16); // n = 1
        NEAR(a.x_pixels, -0.2451223337533073, 1e-12); NEAR(a.y_pixels, -0.4301597090019468, 1e-12);
        double mx = 0, my = 0;
        for (uint32_t i = 0; i < 16; ++i) {
            const auto s = temporal_jitter_r2(i, 16), w = temporal_jitter_r2(i + 16, 16);
            CHECK(s.x_pixels >= -.5 && s.x_pixels < .5 && s.y_pixels >= -.5 && s.y_pixels < .5);
            NEAR(s.x_pixels, w.x_pixels, 0); NEAR(s.y_pixels, w.y_pixels, 0);
            mx += s.x_pixels; my += s.y_pixels;
        }
        CHECK(std::fabs(mx / 16) < 0.1 && std::fabs(my / 16) < 0.1);
        for (uint32_t period : {0u, 1u, 4097u}) test::invalid_argument([=] { temporal_jitter_r2(0, period); });
    }},
    {"invalid period and dimensions", [] {
        for (uint32_t period : {0u, 1u, 4097u}) test::invalid_argument([=] { temporal_jitter(0, period); });
        std::array<double, 16> m{};
        test::invalid_argument([&] { jitter_projection(m, {}, 0, 1080); });
        test::invalid_argument([&] { jitter_projection(m, {}, 1920, 0); });
    }},
    {"pixel displacement preserves depth and perspective W", [] {
        const std::array<double, 16> m{2,0,0,0, 0,3,0,0, 0,0,.1,1, 0,0,1,0};
        const auto original = m;
        const auto j = jitter_projection(m, {.25, -.375}, 800, 600);
        const std::array<double, 4> p{1,2,8,1};
        auto mul = [&](const std::array<double, 16>& a, unsigned row) {
            double v = 0; for (unsigned col = 0; col < 4; ++col) v += a[row * 4 + col] * p[col]; return v;
        };
        NEAR((mul(j, 0) / mul(j, 3) - mul(m, 0) / mul(m, 3)) * 400, .25, 1e-12);
        NEAR((mul(j, 1) / mul(j, 3) - mul(m, 1) / mul(m, 3)) * -300, -.375, 1e-12);
        NEAR(mul(j, 2), mul(m, 2), 0); NEAR(mul(j, 3), mul(m, 3), 0); CHECK(m == original);
    }},
}); }
