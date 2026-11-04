// M3 camera-fit math (camera_fit_basis.hpp + the solver of camera_fit_gpu.cpp, 2026-11-04).
// A CPU replica of the GPU shader's per-sample Jacobian, Cauchy IRLS and damped Jacobi-scaled
// Gauss-Newton runs on synthetic depth + flow:
//  - SE5 pattern: the tracker's previous VP lacks the camera translation (C = identity while the
//    player walks); the fit must recover the eye delta and explain the flow to < 0.05 px;
//  - a combined translation + small rotation is recovered;
//  - 25 % outliers (HUD, movers, bad matches) do not pull the fit;
//  - a still scene is not accepted (nothing to fix);
//  - reversed depth works the same;
//  - CarriesNoView: a projection alone (SE5 0ce11a8) vs a matrix with a real orientation.
#include "test_support.hpp"
#include "core/camera_fit_basis.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace na::motion::gpu;
using M = std::array<double, 16>;

namespace {
M mul(const M& a, const M& b) {
    M r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) r[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
    return r;
}
M inverse(const M& m) {
    double a[4][8];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 8; ++j) a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i ? 1 : 0);
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int r = c + 1; r < 4; ++r) if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
        for (int j = 0; j < 8; ++j) std::swap(a[c][j], a[p][j]);
        const double d = a[c][c];
        for (int j = 0; j < 8; ++j) a[c][j] /= d;
        for (int r = 0; r < 4; ++r) if (r != c) { const double f = a[r][c]; for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j]; }
    }
    M r{};
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) r[i * 4 + j] = a[i][j + 4];
    return r;
}
// Row-vector view matrix: X_cam = (X - eye) * R, R = rotation by yaw (about y).
M view(const double eye[3], double yaw) {
    const double c = std::cos(yaw), s = std::sin(yaw);
    M r{c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, 0, 1};
    M t{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -eye[0], -eye[1], -eye[2], 1};
    return mul(t, r);
}
M proj(bool reversed) {
    const double n = 0.1, f = 1000, xs = 1.0 / std::tan(0.6) / (16.0 / 9.0), ys = 1.0 / std::tan(0.6);
    if (!reversed) return M{xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, f / (f - n), 1, 0, 0, -n * f / (f - n), 0};
    return M{xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, n / (n - f), 1, 0, 0, -n * f / (n - f), 0};
}
std::array<double, 4> rowmul(const std::array<double, 4>& p, const M& m) {
    std::array<double, 4> r{};
    for (int c = 0; c < 4; ++c) for (int k = 0; k < 4; ++k) r[c] += p[k] * m[k * 4 + c];
    return r;
}
std::array<float, 4> rowmulf(const float p[4], const float* m) {
    std::array<float, 4> r{};
    for (int c = 0; c < 4; ++c) r[c] = p[0] * m[c] + p[1] * m[4 + c] + p[2] * m[8 + c] + p[3] * m[12 + c];
    return r;
}

struct Sample { uint32_t x, y; float depth; float fx, fy; };
constexpr uint32_t W = 640, H = 360, kStep = 8;
uint32_t rng = 12345;
double uniform() { rng = rng * 1664525u + 1013904223u; return double(rng >> 8) / double(1u << 24); }

// Scene: every 8x8 block centre sees a static point at a random view depth 2..60.
std::vector<Sample> scene(const M& P, const M& vp_cur, const M& vp_true_prev, const float jit[2], double outliers) {
    const M inv = inverse(vp_cur);
    std::vector<Sample> out;
    for (uint32_t y = kStep / 2; y < H; y += kStep)
        for (uint32_t x = kStep / 2; x < W; x += kStep) {
            const double px = x + 0.5 - jit[0], py = y + 0.5 - jit[1];
            const double nx = px / W * 2 - 1, ny = 1 - py / H * 2;
            // Depth of a point at view distance z: clip = (.., A z + B, z) with A = P[10], B = P[14].
            const double z = 2 + 58 * uniform();
            const double d = (P[10] * z + P[14]) / z;
            const auto p0 = [&](double dd) { auto w = rowmul({nx, ny, dd, 1}, inv); for (auto& v : w) v /= w[3]; return w; };
            auto world = p0(d);
            auto pc = rowmul(world, vp_true_prev);
            const double ppx = (pc[0] / pc[3] + 1) * 0.5 * W, ppy = (1 - pc[1] / pc[3]) * 0.5 * H;
            Sample s{x, y, float(d), float(ppx - px), float(ppy - py)};
            if (uniform() < outliers) { s.fx += float(40 * (uniform() - 0.5)); s.fy += float(40 * (uniform() - 0.5)); }
            out.push_back(s);
        }
    return out;
}

struct Fit { double theta[6]{}; double e0 = 0, e = 0, n = 0; bool accepted = false; };

// CPU replica of camera_fit_gpu.cpp (accum + solve, 3 iterations + final evaluation).
Fit fit(const std::vector<Sample>& samples, const FitBasis& b, const float jit[2], float depth_clear) {
    Fit r;
    float th[6] = {};
    for (int it = 0; it <= 6; ++it) {
        double tot[30] = {};
        for (const auto& s : samples) {
            const float d = s.depth;
            if (!(d > 0 && d < 1 && d != depth_clear)) continue;
            const float px = float(s.x) + 0.5f - jit[0], py = float(s.y) + 0.5f - jit[1];
            const float ndc[4] = {px / W * 2 - 1, 1 - py / H * 2, d, 1};
            auto clip = rowmulf(ndc, b.C);
            std::array<float, 4> dc[6];
            for (int k = 0; k < 6; ++k) {
                dc[k] = rowmulf(ndc, b.G[k]);
                for (int c = 0; c < 4; ++c) clip[c] += th[k] * dc[k][c];
            }
            if (!(clip[3] > 1e-12f)) continue;
            const float iw = 1 / clip[3];
            const float predx = (clip[0] * iw + 1) * 0.5f * W, predy = (1 - clip[1] * iw) * 0.5f * H;
            const float rx = s.fx - (predx - px), ry = s.fy - (predy - py);
            float J0[6], J1[6];
            for (int k = 0; k < 6; ++k) {
                J0[k] = (dc[k][0] * clip[3] - clip[0] * dc[k][3]) * iw * iw * 0.5f * W;
                J1[k] = -(dc[k][1] * clip[3] - clip[1] * dc[k][3]) * iw * iw * 0.5f * H;
            }
            const float rr = rx * rx + ry * ry, c2 = 1.5f * 1.5f, w = 1 / (1 + rr / c2);
            int idx = 0;
            for (int a = 0; a < 6; ++a) for (int c = a; c < 6; ++c) tot[idx++] += w * (J0[a] * J0[c] + J1[a] * J1[c]);
            for (int a = 0; a < 6; ++a) tot[21 + a] += w * (J0[a] * rx + J1[a] * ry);
            tot[27] += 0.5 * c2 * std::log(1 + rr / c2);
            tot[28] += 1;
        }
        const double n = tot[28], e = n > 0 ? tot[27] / n : 0;
        if (it == 0) { r.e0 = e; }
        if (it < 6) {
            double A[6][6], bb[6], sc[6];
            int idx = 0;
            for (int a = 0; a < 6; ++a) for (int c = a; c < 6; ++c) { A[a][c] = A[c][a] = tot[idx++]; }
            for (int a = 0; a < 6; ++a) { bb[a] = tot[21 + a]; sc[a] = A[a][a] > 1e-20 ? 1 / std::sqrt(A[a][a]) : 0; }
            for (int a = 0; a < 6; ++a) {
                for (int c = 0; c < 6; ++c) A[a][c] *= sc[a] * sc[c];
                A[a][a] = (sc[a] > 0 ? A[a][a] : 0) + 0.01 + (sc[a] > 0 ? 0 : 1);
                bb[a] *= sc[a];
            }
            double L[6][6] = {};
            for (int j = 0; j < 6; ++j) {
                double ds = A[j][j];
                for (int m = 0; m < j; ++m) ds -= L[j][m] * L[j][m];
                L[j][j] = std::sqrt(std::fmax(ds, 1e-12));
                for (int i = j + 1; i < 6; ++i) { double t = A[i][j]; for (int m = 0; m < j; ++m) t -= L[i][m] * L[j][m]; L[i][j] = t / L[j][j]; }
            }
            double yv[6], xv[6];
            for (int i = 0; i < 6; ++i) { double t = bb[i]; for (int m = 0; m < i; ++m) t -= L[i][m] * yv[m]; yv[i] = t / L[i][i]; }
            for (int i = 5; i >= 0; --i) { double t = yv[i]; for (int m = i + 1; m < 6; ++m) t -= L[m][i] * xv[m]; xv[i] = t / L[i][i]; }
            for (int k = 0; k < 6; ++k) th[k] += float(xv[k] * sc[k]);
            continue;
        }
        r.e = e; r.n = n;
        r.accepted = n >= 256 && r.e0 > 0.05 && e < 0.7 * r.e0;
    }
    for (int k = 0; k < 6; ++k) r.theta[k] = th[k];
    return r;
}

// Mean pixel error of the corrected reprojection against the true motion (inliers only).
double residual_px(const std::vector<Sample>& clean, const FitBasis& b, const Fit& f, const float jit[2]) {
    float G[16] = {};
    for (int k = 0; k < 6; ++k) for (int i = 0; i < 16; ++i) G[i] += float(f.theta[k]) * b.G[k][i];
    double sum = 0;
    for (const auto& s : clean) {
        const float px = float(s.x) + 0.5f - jit[0], py = float(s.y) + 0.5f - jit[1];
        const float ndc[4] = {px / W * 2 - 1, 1 - py / H * 2, s.depth, 1};
        float m[16];
        for (int i = 0; i < 16; ++i) m[i] = b.C[i] + (f.accepted ? G[i] : 0);
        const auto c = rowmulf(ndc, m);
        const float vx = (c[0] / c[3] + 1) * 0.5f * W - px, vy = (1 - c[1] / c[3]) * 0.5f * H - py;
        sum += std::hypot(vx - s.fx, vy - s.fy);
    }
    return sum / double(clean.size());
}

struct Case { double eye_prev[3], eye_cur[3]; double yaw_prev, yaw_cur; bool reversed; double outliers; };

void run_case(const Case& k, bool expect_accept, double* t_out = nullptr) {
    const M P = proj(k.reversed);
    const M vp_cur = mul(view(k.eye_cur, k.yaw_cur), P);
    const M vp_true_prev = mul(view(k.eye_prev, k.yaw_prev), P);
    // The tracker's previous VP: correct rotation, translation missing (camera-relative VP).
    const M vp_tracked_prev = mul(view(k.eye_cur, k.yaw_prev), P);
    const M inv_cur = inverse(vp_cur);
    const M composed = mul(inv_cur, vp_tracked_prev);
    const FitBasis b = MakeFitBasis(composed.data(), inv_cur.data(), vp_tracked_prev.data(), k.eye_cur);
    const float jit[2] = {0.31f, -0.27f};
    rng = 12345;
    const auto noisy = scene(P, vp_cur, vp_true_prev, jit, k.outliers);
    rng = 12345;
    const auto clean = scene(P, vp_cur, vp_true_prev, jit, 0);
    const Fit f = fit(noisy, b, jit, k.reversed ? 0.f : 1.f);
    CHECK(f.accepted == expect_accept);
    if (expect_accept) {
        CHECK(residual_px(clean, b, f, jit) < 0.05);
        if (t_out) for (int i = 0; i < 3; ++i) t_out[i] = f.theta[i];
    }
}
} // namespace

int main() {
    return test::run({
        {"walking with the translation missing is recovered", [] {
            double t[3];
            run_case({{0, 1.7, 0}, {0.12, 1.7, 0.35}, 0.2, 0.2, false, 0}, true, t);
            // dX = eye_cur - eye_prev (the world point appears shifted by the missing step).
            CHECK(std::fabs(t[0] - 0.12) < 0.01);
            CHECK(std::fabs(t[2] - 0.35) < 0.02);
            CHECK(std::fabs(t[1]) < 0.01);
        }},
        {"translation plus a small turn the tracker missed part of", [] {
            // Tracked previous VP carries yaw_prev; the true previous camera also turned 0.004 rad more.
            const Case k{{0, 1.7, 0}, {0.05, 1.7, 0.2}, 0.1, 0.1, false, 0};
            const M P = proj(false);
            const M vp_cur = mul(view(k.eye_cur, k.yaw_cur), P);
            const M vp_true_prev = mul(view(k.eye_prev, k.yaw_prev + 0.004), P);
            const M vp_tracked_prev = mul(view(k.eye_cur, k.yaw_prev), P);
            const M inv_cur = inverse(vp_cur);
            const M composed = mul(inv_cur, vp_tracked_prev);
            const FitBasis b = MakeFitBasis(composed.data(), inv_cur.data(), vp_tracked_prev.data(), k.eye_cur);
            const float jit[2] = {0, 0};
            rng = 777;
            const auto s = scene(P, vp_cur, vp_true_prev, jit, 0);
            const Fit f = fit(s, b, jit, 1.f);
            CHECK(f.accepted);
            CHECK(residual_px(s, b, f, jit) < 0.05);
        }},
        {"a quarter outliers do not pull the fit", [] {
            run_case({{0, 1.7, 0}, {0.1, 1.7, 0.3}, -0.3, -0.3, false, 0.25}, true);
        }},
        {"a still scene is not accepted", [] {
            run_case({{0, 1.7, 0}, {0, 1.7, 0}, 0.2, 0.2, false, 0}, false);
        }},
        {"reversed depth", [] {
            run_case({{1, 1.7, 2}, {1.08, 1.75, 2.3}, 0.5, 0.5, true, 0.1}, true);
        }},
        {"a projection alone carries no view; any real orientation does", [] {
            M P = proj(false);
            CHECK(CarriesNoView(P.data()));
            P[8] = 0.0004; P[9] = -0.0003; // jitter / off-centre terms are allowed
            CHECK(CarriesNoView(P.data()));
            CHECK(CarriesNoView(proj(true).data()));
            const double origin[3] = {0, 0, 0}, eye[3] = {3, 1.7, -2};
            CHECK(CarriesNoView(mul(view(eye, 0), proj(false)).data())); // translation only: no orientation
            CHECK(!CarriesNoView(mul(view(origin, 0.01), proj(false)).data())); // camera-relative, turned
            CHECK(!CarriesNoView(mul(view(eye, -0.4), proj(true)).data()));
            M pitch{1, 0, 0, 0, 0, std::cos(0.02), std::sin(0.02), 0, 0, -std::sin(0.02), std::cos(0.02), 0, 0, 0, 0, 1};
            CHECK(!CarriesNoView(mul(pitch, proj(false)).data()));
            M zero{};
            CHECK(!CarriesNoView(zero.data()));
        }},
    });
}
