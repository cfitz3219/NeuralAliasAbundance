// M3 rigid camera correction basis (2026-11-04, camera_fit_gpu.hpp). Pure, no host types:
// tests/cpu/camera_fit_basis_test.cpp.
#pragma once

#include <cmath>

namespace na::motion::gpu {

// M3 rigid camera correction (2026-11-04, camera_fit_gpu.hpp): the composed current->previous
// clip matrix C (exactly what inv_view_proj_current carries) and six basis matrices G_k, one per
// camera-motion parameter (translation x/y/z in world units, small rotation x/y/z in radians
// about the eye), all row-major storage, row-vector algebra. A fitted correction is
// G = sum(theta_k * G_k) and the corrected reprojection is ndc * (C + G).
struct FitBasis { float C[16]{}; float G[6][16]{}; };

inline FitBasis MakeFitBasis(const double composed_row[16], const double inv_cur_row[16], const double vp_prev_row[16], const double eye[3])
{
    FitBasis b{};
    for (int i = 0; i < 16; ++i) b.C[i] = static_cast<float>(composed_row[i]);
    // F_k (4x4, row-vector algebra): the clip-space change of a homogeneous world point Xh for a
    // unit change of parameter k. Translation t_j: row 3 = row j of VP_prev (d = t enters with
    // the point's w). Rotation w_k about the eye: d = w x (X - eye) = (X - eye) * N_k with
    // N_k = -[e_k]x * VP_prev(rows 0..2), so rows 0..2 = N_k and row 3 = -eye * N_k.
    // G_k = inv_cur * F_k, so ndc * G_k is the clip change at that pixel.
    double F[6][16] = {};
    for (int j = 0; j < 3; ++j)
        for (int c = 0; c < 4; ++c) F[j][12 + c] = vp_prev_row[j * 4 + c];
    static const double skew[3][3][3] = {
        {{0, 0, 0}, {0, 0, -1}, {0, 1, 0}},
        {{0, 0, 1}, {0, 0, 0}, {-1, 0, 0}},
        {{0, -1, 0}, {1, 0, 0}, {0, 0, 0}}};
    for (int k = 0; k < 3; ++k) {
        double N[3][4] = {};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) {
                double v = 0;
                for (int m = 0; m < 3; ++m) v += skew[k][r][m] * vp_prev_row[m * 4 + c];
                N[r][c] = -v;
            }
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) F[3 + k][r * 4 + c] = N[r][c];
        for (int c = 0; c < 4; ++c) F[3 + k][12 + c] = -(eye[0] * N[0][c] + eye[1] * N[1][c] + eye[2] * N[2][c]);
    }
    for (int k = 0; k < 6; ++k)
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) {
                double v = 0;
                for (int m = 0; m < 4; ++m) v += inv_cur_row[r * 4 + m] * F[k][m * 4 + c];
                b.G[k][r * 4 + c] = static_cast<float>(v);
            }
    return b;
}

// True when a row-vector view-projection matrix carries no camera orientation: no entry that
// mixes x, y and z (roll: [0][1]/[1][0]; yaw: [0][2]; pitch: [1][2]) and nothing feeding w from
// x or y. A projection alone (SE5 0ce11a8: every captured set, off-diagonals <= 8.4e-11) has
// them all at zero; any real view rotation puts sine terms there. Jitter/off-centre terms
// ([2][0], [2][1]) and translation are allowed. Then the camera term of the motion is zero
// whatever the player does, and camera motion has to be reconstructed from the image (M3).
inline bool CarriesNoView(const double vp_row[16])
{
    const double scale = std::fabs(vp_row[0]) > std::fabs(vp_row[5]) ? std::fabs(vp_row[0]) : std::fabs(vp_row[5]);
    if (!(scale > 1e-9)) return false;
    const int mixed[6] = {1, 2, 3, 4, 6, 7};
    for (int i : mixed)
        if (!(std::fabs(vp_row[i]) <= 1e-6 * scale)) return false;
    return true;
}

} // namespace na::motion::gpu
