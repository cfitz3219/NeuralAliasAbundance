// P4-4 projection window: the windowed camera puts the scene in the top-left corner,
// pixel for pixel the image a camera at the window's size would draw.
#include "test_support.hpp"
#include "core/projection_window.hpp"
#include "core/temporal_jitter.hpp"
#include <cmath>
using namespace neural_alias;
namespace {
using M = std::array<double, 16>;
std::array<double, 4> mul(const M& a, const std::array<double, 4>& p)
{
    std::array<double, 4> r{};
    for (unsigned i = 0; i < 4; ++i) for (unsigned k = 0; k < 4; ++k) r[i] += a[i * 4 + k] * p[k];
    return r;
}
// D3D viewport: NDC -> pixel (y down) for a w x h viewport at the origin.
void pixel(const std::array<double, 4>& c, double w, double h, double& px, double& py)
{
    px = (c[0] / c[3] + 1) * 0.5 * w;
    py = (1 - c[1] / c[3]) * 0.5 * h;
}
// A perspective camera with a view rotation and translation, reverse-Z style depth.
M camera()
{
    const M p{1.2, 0, 0, 0, 0, 2.1, 0, 0, 0, 0, 0.001, 0.5, 0, 0, 1, 0};
    const double c = std::cos(0.3), s = std::sin(0.3);
    const M v{c, 0, s, 4, 0, 1, 0, -2, -s, 0, c, 9, 0, 0, 0, 1};
    M vp{};
    for (unsigned i = 0; i < 4; ++i) for (unsigned j = 0; j < 4; ++j) for (unsigned k = 0; k < 4; ++k) vp[i * 4 + j] += p[i * 4 + k] * v[k * 4 + j];
    return vp;
}
}
int main() { return test::run({
    {"corner pixels equal the render-size image's pixels", [] {
        const double W = 3440, H = 1440, rw = 2293, rh = 960;
        const M vp = camera();
        const M win = window_projection(vp, rw / W, rh / H);
        for (const std::array<double, 4> p : {std::array<double, 4>{0, 0, 0, 1}, {3, -1, 2, 1}, {-5, 2.5, -4, 1}, {1, 1, 6, 1}}) {
            double ux, uy, wx, wy;
            pixel(mul(vp, p), rw, rh, ux, uy);  // the scene drawn at render size
            pixel(mul(win, p), W, H, wx, wy);   // the windowed scene on the native canvas
            NEAR(wx, ux, 1e-6); NEAR(wy, uy, 1e-6);
            const auto a = mul(vp, p), b = mul(win, p);
            NEAR(a[2], b[2], 1e-12); NEAR(a[3], b[3], 1e-12); // depth and W unchanged
        }
    }},
    {"the view's edges land on the corner's edges", [] {
        const M id{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        const M win = window_projection(id, 0.5, 0.25);
        double x, y;
        pixel(mul(win, {-1, 1, 0, 1}), 100, 100, x, y); NEAR(x, 0, 1e-12); NEAR(y, 0, 1e-12);
        pixel(mul(win, {1, -1, 0, 1}), 100, 100, x, y); NEAR(x, 50, 1e-12); NEAR(y, 25, 1e-12);
        const M same = window_projection(id, 1, 1);
        for (unsigned i = 0; i < 16; ++i) NEAR(same[i], id[i], 0);
    }},
    {"jitter in render pixels stays the same native-pixel shift", [] {
        const double W = 1920, H = 1080, rw = 1280, rh = 720;
        const M vp = camera();
        const JitterSample js{0.25, -0.375};
        const M wj = window_projection(jitter_projection(vp, js, uint32_t(rw), uint32_t(rh)), rw / W, rh / H);
        const M w0 = window_projection(vp, rw / W, rh / H);
        const std::array<double, 4> p{2, 1, 5, 1};
        double ax, ay, bx, by;
        pixel(mul(wj, p), W, H, ax, ay);
        pixel(mul(w0, p), W, H, bx, by);
        NEAR(ax - bx, 0.25, 1e-6); NEAR(ay - by, -0.375, 1e-6); // jitter_projection: +y pixels = down
    }},
    {"invalid fractions", [] {
        const M m{};
        test::invalid_argument([&] { window_projection(m, 0, 0.5); });
        test::invalid_argument([&] { window_projection(m, 0.5, 1.5); });
    }},
}); }
