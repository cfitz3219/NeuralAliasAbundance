// reconstructed 2026-10 — independent sample values and projected displacement.
#include "test_support.hpp"
#include "core/temporal_jitter.hpp"
using namespace neural_alias;
int main() { return test::run({
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
