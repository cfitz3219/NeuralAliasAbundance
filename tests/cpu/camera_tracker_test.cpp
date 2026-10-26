// J2 camera tracker: jitter extraction (known and estimated), camera cuts and
// the unjittered matrices handed to the motion arbiter, on matrices built as
// Perspective x View in both depth conventions and handednesses.
#include "test_support.hpp"
#include "core/temporal_jitter.hpp"
#include "providers/camera/camera_tracker.hpp"

#include <cmath>

using namespace na::camera;

namespace {
constexpr uint32_t W = 2752, H = 1152;

Matrix Perspective(bool reverse, double sign)
{
    const double n = 0.25, f = 2000.0, fov = 0.9, aspect = double(W) / H;
    const double sy = 1.0 / std::tan(fov / 2), sx = sy / aspect;
    return {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, sign * (reverse ? n / (n - f) : f / (f - n)),
            reverse ? n * f / (f - n) : -n * f / (f - n), 0, 0, sign, 0};
}
Matrix View(double yaw, double x, double y, double z)
{
    const double c = std::cos(yaw), s = std::sin(yaw);
    // rows: right, up, forward; translation = -R * position
    Matrix v{c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, 0, 1};
    v[3] = -(v[0] * x + v[1] * y + v[2] * z);
    v[7] = -(v[4] * x + v[5] * y + v[6] * z);
    v[11] = -(v[8] * x + v[9] * y + v[10] * z);
    return v;
}
Matrix Mul(const Matrix& a, const Matrix& b)
{
    Matrix o{};
    for (size_t r = 0; r < 4; ++r) for (size_t c = 0; c < 4; ++c) for (size_t k = 0; k < 4; ++k) o[r * 4 + c] += a[r * 4 + k] * b[k * 4 + c];
    return o;
}
bool Close(const Matrix& a, const Matrix& b, double eps)
{
    for (size_t i = 0; i < 16; ++i) if (std::abs(a[i] - b[i]) > eps * std::max(1.0, std::abs(b[i]))) return false;
    return true;
}
} // namespace

int main()
{
    return test::run({
        {"injected jitter is recovered exactly and stripped, every depth convention and handedness", [] {
             for (bool reverse : {false, true})
                 for (double sign : {1.0, -1.0}) {
                     CameraTracker t;
                     const Matrix vp = Mul(Perspective(reverse, sign), View(0.4, 10, 2, -30));
                     for (uint32_t i = 0; i < 8; ++i) {
                         const auto s = neural_alias::temporal_jitter(i, 8);
                         const auto jittered = neural_alias::jitter_projection(vp, s, W, H);
                         const double inj[2] = {s.x_pixels, s.y_pixels};
                         const CameraFrame f = t.Update(jittered, i + 1, W, H, inj);
                         CHECK(f.valid && f.jitter_known);
                         CHECK(std::abs(f.jitter_px_x - s.x_pixels) < 1e-9 && std::abs(f.jitter_px_y - s.y_pixels) < 1e-9);
                         CHECK(Close(f.unjittered, vp, 1e-9));                       // jitter fully removed
                         CHECK(Close(f.view_proj_current_row, TransposeMatrix(vp), 1e-9)); // arbiter convention
                         const Matrix id = Mul(f.view_proj_current_row, f.inv_view_proj_current_row);
                         CHECK(Close(id, Matrix{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, 1e-6));
                         CHECK(f.camera_cut == (i == 0)); // first frame starts history
                     }
                 }
         }},
        {"game-native jitter is estimated from the off-axis terms around their running mean", [] {
             CameraTracker t;
             const Matrix vp = Mul(Perspective(true, 1.0), View(0.1, 0, 5, 0));
             double worst = 0;
             for (uint32_t i = 0; i < 64; ++i) {
                 const auto s = neural_alias::temporal_jitter(i, 8);
                 const CameraFrame f = t.Update(neural_alias::jitter_projection(vp, s, W, H), i + 1, W, H);
                 CHECK(f.valid && !f.jitter_known);
                 if (i >= 16) worst = std::max(worst, std::max(std::abs(f.jitter_px_x - s.x_pixels), std::abs(f.jitter_px_y - s.y_pixels)));
             }
             std::printf("estimated jitter: worst error after warm-up %.4f px\n", worst);
             CHECK(worst < 0.25); // running-mean estimate; a known (injected) sample is exact
         }},
        {"a constant off-axis frustum is not mistaken for jitter", [] {
             CameraTracker t;
             const Matrix vp = Mul(Perspective(false, 1.0), View(0, 0, 0, 0));
             const neural_alias::JitterSample shift{3.0, -2.0}; // constant shift, every frame
             CameraFrame f{};
             for (uint32_t i = 0; i < 32; ++i) f = t.Update(neural_alias::jitter_projection(vp, shift, W, H), i + 1, W, H);
             CHECK(f.valid && std::abs(f.jitter_px_x) < 1e-6 && std::abs(f.jitter_px_y) < 1e-6);
         }},
        {"cuts: teleport, fast turn, zoom and frame gaps reset history; ordinary motion does not", [] {
             const Matrix p = Perspective(true, 1.0);
             CameraTracker t;
             CHECK(t.Update(Mul(p, View(0, 0, 0, 0)), 1, W, H).camera_cut);
             CHECK(!t.Update(Mul(p, View(0.02, 0.5, 0, 0)), 2, W, H).camera_cut);      // walking / panning
             CHECK(t.Update(Mul(p, View(0.02, 200, 0, 0)), 3, W, H).camera_cut);        // teleport
             CHECK(t.Update(Mul(p, View(1.2, 200, 0, 0)), 4, W, H).camera_cut);         // 68 degree snap
             CHECK(!t.Update(Mul(p, View(1.21, 200, 0, 0)), 5, W, H).camera_cut);
             CHECK(t.Update(Mul(p, View(1.21, 200, 0, 0)), 12, W, H).camera_cut);       // camera missing 7 frames
             CameraTracker z;
             z.Update(Mul(p, View(0, 0, 0, 0)), 1, W, H);
             Matrix zoom = p; zoom[0] *= 1.6; zoom[5] *= 1.6;                          // FOV change
             CHECK(z.Update(Mul(zoom, View(0, 0, 0, 0)), 2, W, H).camera_cut);
         }},
        {"previous-frame matrix for motion is the last unjittered camera, or the current one on a cut", [] {
             const Matrix p = Perspective(true, 1.0);
             CameraTracker t;
             const CameraFrame a = t.Update(Mul(p, View(0, 0, 0, 0)), 1, W, H);
             CHECK(Close(a.view_proj_previous_row, a.view_proj_current_row, 1e-12));
             const CameraFrame b = t.Update(Mul(p, View(0.01, 0.3, 0, 0)), 2, W, H);
             CHECK(Close(b.view_proj_previous_row, a.view_proj_current_row, 1e-12));
         }},
        {"non-camera matrices are rejected", [] {
             CameraTracker t;
             Matrix identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
             CHECK(!t.Update(identity, 1, W, H).valid);
             Matrix world = View(0.3, 4, 5, 6); world[0] = 3; // scaled object transform
             CHECK(!t.Update(world, 2, W, H).valid);
             CHECK(!t.Update(Mul(Perspective(true, 1.0), View(0, 0, 0, 0)), 3, 0, H).valid);
         }},
    });
}
