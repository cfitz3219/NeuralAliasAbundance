// J2 camera tracker: jitter extraction (known and estimated), camera cuts and
// the unjittered matrices handed to the motion arbiter, on matrices built as
// Perspective x View in both depth conventions and handednesses.
#include "test_support.hpp"
#include "core/temporal_jitter.hpp"
#include "core/motion_arbiter.hpp"
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
na::motion::Tier2Input MotionInput(const CameraFrame& f) {
    na::motion::Tier2Input result{};
    result.valid = true;
    for (size_t i = 0; i < 16; ++i) {
        result.inv_view_proj_current.m[i] = static_cast<float>(f.inv_view_proj_current_row[i]);
        result.view_proj_previous.m[i] = static_cast<float>(f.view_proj_previous_row[i]);
    }
    result.jitter_current_px[0] = static_cast<float>(f.jitter_px_x);
    result.jitter_current_px[1] = static_cast<float>(f.jitter_px_y);
    return result;
}
} // namespace

int main()
{
    return test::run({
        {"camera-relative CBV origin reproduces world-space pixel motion", [] {
             for (bool reverse : {false, true}) for (double sign : {1.0, -1.0}) {
                 const Matrix p = Perspective(reverse, sign);
                 CameraTracker relative, world;
                 std::array<double, 3> origin{4, 2, -3};
                 for (uint32_t frame = 1; frame <= 6; ++frame) {
                     const double yaw = frame <= 3 ? 0.2 : 0.2 + 0.01 * (frame - 3);
                     origin[0] += 0.25; origin[1] += 0.1; origin[2] += 0.05;
                     const auto jitter = neural_alias::temporal_jitter(frame, 8);
                     const double known[2] = {jitter.x_pixels, jitter.y_pixels};
                     const Matrix relative_vp = Mul(p, View(yaw, 0, 0, 0));
                     const Matrix world_vp = Mul(p, View(yaw, origin[0], origin[1], origin[2]));
                     const auto a = relative.UpdateCameraRelative(
                         neural_alias::jitter_projection(relative_vp, jitter, W, H), origin, frame, W, H, known);
                     const auto b = world.Update(neural_alias::jitter_projection(world_vp, jitter, W, H), frame, W, H, known);
                     CHECK(a.valid && b.valid && a.camera_cut == b.camera_cut);
                     CHECK(Close(a.unjittered, relative_vp, 1e-6));
                     CHECK(Close(Mul(a.inv_view_proj_current_row, a.view_proj_previous_row),
                                 Mul(b.inv_view_proj_current_row, b.view_proj_previous_row), 1e-6));
                     const auto ra = MotionInput(a), rb = MotionInput(b);
                     for (uint32_t x : {W / 4, W / 2, 3 * W / 4}) for (uint32_t y : {H / 4, H / 2}) {
                         float ax{}, ay{}, bx{}, by{};
                         const float z = reverse ? 0.02f : 0.98f;
                         CHECK(na::motion::ReprojectPixel(ra, W, H, x, y, z, &ax, &ay, nullptr));
                         CHECK(na::motion::ReprojectPixel(rb, W, H, x, y, z, &bx, &by, nullptr));
                         NEAR(ax, bx, 0.02); NEAR(ay, by, 0.02); // production float matrices
                         if (frame == 2) CHECK(std::abs(ax) + std::abs(ay) > 1.0f); // pure pan must not vanish
                     }
                 }
                 origin[0] += 100;
                 const auto cut = relative.UpdateCameraRelative(Mul(p, View(0.23, 0, 0, 0)), origin, 7, W, H);
                 CHECK(cut.camera_cut);
                 CHECK(Close(cut.view_proj_current_row, cut.view_proj_previous_row, 1e-12));
                 CHECK(relative.Update(Mul(p, View(0.23, 0, 0, 0)), 8, W, H).camera_cut); // coordinate-mode switch
             }
         }},
        {"origin scan excludes matrices and preserves ambiguous float3 candidates", [] {
             std::vector<uint8_t> previous(160, 0xff), current(160, 0xff); // NaN padding
             const float a[3] = {100, 200, 300}, b[3] = {101, 202, 303};
             for (size_t offset : {size_t(0), size_t(68), size_t(128)}) {
                 std::memcpy(previous.data() + offset, a, sizeof a);
                 std::memcpy(current.data() + offset, b, sizeof b);
             }
             const Matrix vp = Mul(Perspective(true, 1), View(0.2, 0, 0, 0));
             const auto found = FindCameraOriginCandidates(previous, current, vp, {{0, 64}}, 4);
             CHECK(found.size() == 2 && found[0].offset == 68 && found[1].offset == 128);
             NEAR(found[0].delta[0], 1, 1e-12); NEAR(found[0].delta[1], 2, 1e-12); NEAR(found[0].delta[2], 3, 1e-12);
             // Simulate the caller selecting the known fixture origin at +68.
             // Feed the actual decoded float3 through the production tracker.
             CameraTracker relative, world;
             const double zero[2] = {0, 0};
             const auto& c = found[0];
             relative.UpdateCameraRelative(vp, c.previous, 1, W, H, zero);
             world.Update(Mul(Perspective(true, 1), View(0.2, c.previous[0], c.previous[1], c.previous[2])), 1, W, H, zero);
             const auto a_frame = relative.UpdateCameraRelative(vp, c.current, 2, W, H, zero);
             const auto b_frame = world.Update(Mul(Perspective(true, 1), View(0.2, c.current[0], c.current[1], c.current[2])), 2, W, H, zero);
             CHECK(Close(Mul(a_frame.inv_view_proj_current_row, a_frame.view_proj_previous_row),
                         Mul(b_frame.inv_view_proj_current_row, b_frame.view_proj_previous_row), 1e-6));
             CHECK(FindCameraOriginCandidates(previous, current, vp, {}, 1).empty());
             CHECK(FindCameraOriginCandidates(previous, previous, vp, {}, 4).empty());
             CHECK(FindCameraOriginCandidates(previous, current, Mul(Perspective(true, 1), View(0.2, 1, 2, 3)), {}, 4).empty());
             current.resize(78); // truncated float3 must not be read
             CHECK(FindCameraOriginCandidates(previous, current, vp, {{0, 64}}, 4).empty());
         }},
        {"known jitter removal preserves moving cameras and intentional off-axis projection", [] {
             CameraTracker tracker;
             for (uint32_t frame = 1; frame <= 16; ++frame) {
                 const Matrix vp = neural_alias::jitter_projection(
                     Mul(Perspective(true, -1), View(0.02 * frame, 0.25 * frame, 3, -4)), {3, -2}, W, H);
                 const auto sample = neural_alias::temporal_jitter(frame, 8);
                 const auto rendered = neural_alias::jitter_projection(vp, sample, W, H);
                 CHECK(Close(UnjitterViewProjection(rendered, sample.x_pixels, sample.y_pixels, W, H), vp, 1e-6));
                 const double known[] = {sample.x_pixels, sample.y_pixels};
                 const auto f = tracker.Update(rendered, frame, W, H, known);
                 CHECK(f.valid && f.jitter_known && Close(f.unjittered, vp, 1e-6));
                 NEAR(f.jitter_px_x, sample.x_pixels, 1e-5); NEAR(f.jitter_px_y, sample.y_pixels, 1e-5);
             }
         }},
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
