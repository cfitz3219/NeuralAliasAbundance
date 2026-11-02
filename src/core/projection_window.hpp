#pragma once
#include <array>
#include <cmath>
#include <stdexcept>

namespace neural_alias {
// P4-4 projection window. Same convention as jitter_projection: row-major storage,
// column vectors, D3D viewport (pixel y down), clip row 3 = w. Maps the matrix's clip
// space so its NDC [-1,1]^2 lands in the TOP-LEFT sx x sy fraction of the viewport:
// x' = sx*x + (sx-1)*w, y' = sy*y + (1-sy)*w. Z and W are untouched (depth, reverse-Z and
// perspective are preserved). Apply after jitter: a jitter of j render pixels at the
// window's extent stays j native pixels, because one render pixel is one native pixel.
inline std::array<double, 16> window_projection(const std::array<double, 16>& m, double sx, double sy)
{
    if (!(sx > 0 && sx <= 1 && sy > 0 && sy <= 1)) throw std::invalid_argument("window fraction must be in (0,1]");
    auto r = m;
    for (unsigned c = 0; c < 4; ++c) {
        r[c] = sx * m[c] + (sx - 1.0) * m[12 + c];
        r[4 + c] = sy * m[4 + c] + (1.0 - sy) * m[12 + c];
    }
    return r;
}

// Matrices the game DERIVES from its camera (column-vector, row-major as above):
//   forward  M = A * B      (A rewrites only x,y: clip -> UV / pixel space)
//   inverse  M = inv(B) * C (C maps UV / pixel / NDC back to clip; C = I is the plain inverse)
// where B is a camera matrix we replace with B' (VP, P, previous VP). Water, SSR and
// fog passes reconstruct positions from the depth with such matrices; left logical they
// read the packed corner as if it were the whole frame. Recognised only when A (or C)
// is "screen-like": its z,w rows are identity, x,y never read z, and every x,y entry is a
// canonical value (0, +-0.5, +-1, +-2, or +-native size / 1, 2, 4). On success `out` = A * B' (or inv(B') * C).
struct DerivedSpace { double w = 0, h = 0; };
inline double derived_snap(double v, const DerivedSpace& s)
{
    const double c[] = {0, .5, 1, 2, s.w, s.h, s.w / 2, s.h / 2, s.w / 4, s.h / 4};
    double best = 0, d = 1e300;
    for (double k : c) for (double sg : {1.0, -1.0}) if (std::fabs(v - sg * k) < d) { d = std::fabs(v - sg * k); best = sg * k; }
    return best;
}
inline std::array<double, 16> mat_mul(const std::array<double, 16>& a, const std::array<double, 16>& b)
{
    std::array<double, 16> o{};
    for (unsigned r = 0; r < 4; ++r) for (unsigned c = 0; c < 4; ++c) {
        double v = 0; for (unsigned k = 0; k < 4; ++k) v += a[r * 4 + k] * b[k * 4 + c]; o[r * 4 + c] = v;
    }
    return o;
}
// Screen-like transform from an estimate: z,w rows identity, x,y snapped to canonical
// values and never reading z. False when x or y would not depend on x,y.
inline bool derived_screen(const std::array<double, 16>& est, const DerivedSpace& s, std::array<double, 16>& t)
{
    t = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    for (unsigned r = 0; r < 2; ++r) {
        for (unsigned c = 0; c < 4; ++c) t[r * 4 + c] = c == 2 ? 0 : derived_snap(est[r * 4 + c], s);
        if (t[r * 4 + 0] == 0 && t[r * 4 + 1] == 0) return false;
    }
    return true;
}
// m equals r within float precision (per row, relative to the row's largest entry).
inline bool derived_equal(const std::array<double, 16>& m, const std::array<double, 16>& r, double tol)
{
    for (unsigned i = 0; i < 4; ++i) {
        double mx = 1e-30; for (unsigned c = 0; c < 4; ++c) mx = std::fmax(mx, std::fabs(m[i * 4 + c]));
        for (unsigned c = 0; c < 4; ++c) if (!(std::fabs(m[i * 4 + c] - r[i * 4 + c]) <= tol * mx)) return false;
    }
    return true;
}
// 1 forward, 2 inverse, 0 not derived from b. The candidate transform is snapped to
// canonical values and must rebuild m to float precision, so a camera pan, another
// camera or arbitrary data is not mistaken for a screen transform of this camera.
inline int derive_rebase(const std::array<double, 16>& m, const std::array<double, 16>& b, const std::array<double, 16>& binv,
                         const std::array<double, 16>& bnew, const std::array<double, 16>& bnewinv, const DerivedSpace& s,
                         std::array<double, 16>& out)
{
    constexpr double kTol = 2e-5; // a few float ulps of the game's own arithmetic
    for (double v : m) if (!std::isfinite(v)) return 0;
    std::array<double, 16> t{};
    // Forward: M = A * B, z,w rows equal B's.
    if (derived_equal({m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15], 0, 0, 0, 0, 0, 0, 0, 0},
                      {b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15], 0, 0, 0, 0, 0, 0, 0, 0}, kTol) &&
        derived_screen(mat_mul(m, binv), s, t) && derived_equal(m, mat_mul(t, b), kTol)) {
        out = mat_mul(t, bnew);
        return 1;
    }
    // Inverse: M = inv(B) * C. Cheap reject first: C's w row (B's w row times M) is (0,0,0,1).
    for (unsigned c = 0; c < 4; ++c) {
        double v = 0; for (unsigned k = 0; k < 4; ++k) v += b[12 + k] * m[k * 4 + c];
        if (!(std::fabs(v - (c == 3 ? 1.0 : 0.0)) <= 1e-2)) return 0;
    }
    if (derived_screen(mat_mul(b, m), s, t) && derived_equal(m, mat_mul(binv, t), 4 * kTol)) {
        out = mat_mul(bnewinv, t);
        return 2;
    }
    return 0;
}
} // namespace neural_alias
