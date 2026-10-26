// J2 temporal camera tracker (Argon's TemporalCameraTracker, built on the
// existing na::camera::ViewProjection decomposition so there is one
// definition of "a camera matrix"). Per frame, for the chosen camera
// ViewProjection candidate:
//  - jitter in render pixels: either the KNOWN sample NeuralAlias injected
//    (J1), or, for game-native jitter, the offset relative to a running mean
//    of the matrix's own off-axis terms (separates zero-mean jitter from a
//    constant off-axis frustum);
//  - camera cut / teleport: frame gap, translation jump, rotation jump or a
//    field-of-view change, so DLSS / frame generation reset their history;
//  - unjittered current and previous matrices in the motion arbiter's
//    convention (row-major storage, ROW-vector algebra: the transpose of the
//    column-vector matrix the decomposition works in), plus the inverse.
// Matrix convention in: row-major storage, column-vector algebra (clip = M * p),
// as FindMatrices/LoadMatrix normalise both memory layouts. Offsets are the
// "translate clip X/Y by W" amounts that core/temporal_jitter.hpp's
// jitter_projection() applies, so extraction is its exact inverse for either
// handedness: pixels_x = dx * width / 2, pixels_y = -dy * height / 2.
#pragma once
#include "providers/camera/matrix_candidates.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace na::camera {

struct CameraFrame {
    bool valid{};                 // matrix is a perspective ViewProjection and the unjittered matrix inverts
    bool camera_cut{};            // reset temporal history this frame
    bool jitter_known{};          // jitter came from the injected sample, not estimated
    double jitter_px_x{}, jitter_px_y{}; // render pixels, +X right, +Y down
    double fov_y{}, aspect{};     // radians; width/height of the projection scales
    std::array<double, 3> position{}, forward{};
    Matrix unjittered{};          // column-vector convention (as input)
    // Motion arbiter convention (row-major storage, row-vector algebra):
    Matrix view_proj_current_row{}, inv_view_proj_current_row{};
    Matrix view_proj_previous_row{}; // previous frame's unjittered VP (== current on a cut)
};

// Geometry of a column-vector ViewProjection: off-axis offsets (relative to
// row 3), focal scales, forward axis. False if `m` is not a perspective VP.
struct VpGeometry { double dx{}, dy{}, sx{}, sy{}; std::array<double, 3> forward{}, position{}; };
inline bool DecomposeViewProjection(const Matrix& m, VpGeometry& g) {
    Candidate c{};
    if (!ViewProjection(m, c)) return false;
    double norm = 0;
    for (size_t j = 0; j < 3; ++j) norm += m[12 + j] * m[12 + j];
    const double len = std::sqrt(norm);
    double dx = 0, dy = 0;
    for (size_t j = 0; j < 3; ++j) { dx += m[j] * m[12 + j]; dy += m[4 + j] * m[12 + j]; }
    dx /= norm; dy /= norm;
    double xx = 0, yy = 0;
    for (size_t j = 0; j < 3; ++j) {
        const double x = m[j] - dx * m[12 + j], y = m[4 + j] - dy * m[12 + j];
        xx += x * x; yy += y * y;
    }
    g.dx = dx; g.dy = dy;
    g.sx = std::sqrt(xx) / len; g.sy = std::sqrt(yy) / len;
    for (size_t j = 0; j < 3; ++j) g.forward[j] = m[12 + j] / len;
    g.position = c.position;
    return g.sx > 0 && g.sy > 0;
}

inline Matrix TransposeMatrix(const Matrix& m) {
    Matrix t{};
    for (size_t r = 0; r < 4; ++r) for (size_t c = 0; c < 4; ++c) t[r * 4 + c] = m[c * 4 + r];
    return t;
}

// Exact inverse of core/temporal_jitter.hpp for a KNOWN pixel offset.
// Unknown native jitter still needs a baseline; decomposition alone cannot
// distinguish jitter from a deliberately off-centre projection.
inline Matrix UnjitterViewProjection(const Matrix& m, double x_px, double y_px,
                                    uint32_t width, uint32_t height) {
    if (!width || !height) throw std::invalid_argument("zero render extent");
    Matrix result = m;
    const double dx = 2.0 * x_px / width, dy = -2.0 * y_px / height;
    for (size_t c = 0; c < 4; ++c) {
        result[c] -= dx * m[12 + c];
        result[4 + c] -= dy * m[12 + c];
    }
    return result;
}

// Column-vector algebra. A point reconstructed relative to the CURRENT origin
// becomes previous-relative by adding current_origin - previous_origin.
// Thus previous_relative_vp * T(delta), NOT T(delta) * previous_relative_vp.
// Transpose the returned matrix when passing it to the row-vector arbiter.
inline Matrix ComposeCameraRelativeViewProj(const Matrix& previous_relative_vp,
                                           const std::array<double, 3>& delta_world_pos) {
    Matrix result = previous_relative_vp;
    for (size_t r = 0; r < 4; ++r)
        for (size_t c = 0; c < 3; ++c)
            result[r * 4 + 3] += previous_relative_vp[r * 4 + c] * delta_world_pos[c];
    return result;
}

struct CameraByteRange { size_t offset{}, size{}; };
struct CameraOriginCandidate {
    size_t offset{};
    std::array<double, 3> previous{}, current{}, delta{};
};

// Candidate generation only: caller supplies consecutive snapshots of the SAME
// camera block/layout and excludes all known matrix ranges. Scan float-aligned
// triples to cover packed float3 and float4 slots; keep every plausible match
// rather than silently choosing the first smooth vector as the camera origin.
inline std::vector<CameraOriginCandidate> FindCameraOriginCandidates(
    const std::vector<uint8_t>& previous, const std::vector<uint8_t>& current,
    const Matrix& relative_vp, const std::vector<CameraByteRange>& excluded,
    double max_translation) {
    std::vector<CameraOriginCandidate> result;
    VpGeometry geometry{};
    if (!std::isfinite(max_translation) || max_translation <= 0 ||
        !DecomposeViewProjection(relative_vp, geometry)) return result;
    for (double p : geometry.position) if (std::abs(p) > 1e-6) return result;
    const size_t size = std::min(previous.size(), current.size());
    for (size_t offset = 0; offset <= size && size - offset >= 12; offset += 4) {
        bool overlap = false;
        for (const auto& range : excluded) {
            if (range.size && range.offset < offset + 12 &&
                (range.offset >= offset || range.size > offset - range.offset)) overlap = true;
        }
        if (overlap) continue;
        CameraOriginCandidate candidate{};
        candidate.offset = offset;
        double distance2 = 0;
        for (size_t i = 0; i < 3; ++i) {
            float a{}, b{};
            std::memcpy(&a, previous.data() + offset + 4 * i, 4);
            std::memcpy(&b, current.data() + offset + 4 * i, 4);
            candidate.previous[i] = a;
            candidate.current[i] = b;
            candidate.delta[i] = double(b) - double(a);
            distance2 += candidate.delta[i] * candidate.delta[i];
        }
        if (std::isfinite(distance2) && distance2 > 0 && std::sqrt(distance2) <= max_translation)
            result.push_back(candidate);
    }
    return result;
}

class CameraTracker {
public:
    struct Limits {
        double max_translation = 25.0;   // world units per frame
        double max_rotation = 0.65;      // radians per frame
        double max_fov_change = 0.15;    // radians per frame
        uint64_t max_frame_gap = 4;      // frames without a camera before history is stale
    };

    // `m`: the chosen camera ViewProjection (column-vector convention).
    // `injected`: the jitter NeuralAlias applied this frame (pixels), if any.
    CameraFrame Update(const Matrix& m, uint64_t frame, uint32_t width, uint32_t height,
                       const double* injected_px = nullptr) { return Update(m, frame, width, height, injected_px, Limits{}); }
    CameraFrame Update(const Matrix& m, uint64_t frame, uint32_t width, uint32_t height,
                       const double* injected_px, const Limits& lim,
                       const std::array<double, 3>* world_origin = nullptr) {
        CameraFrame f{};
        VpGeometry g{};
        if (!width || !height || !DecomposeViewProjection(m, g)) return f;
        if (world_origin) {
            for (double p : *world_origin) if (!std::isfinite(p)) return f;
            // The supplied origin is identified by the caller, not by the scan.
            for (size_t i = 0; i < 3; ++i) g.position[i] += (*world_origin)[i];
        }
        f.fov_y = 2.0 * std::atan(1.0 / g.sy);
        f.aspect = g.sy / g.sx;
        f.position = g.position;
        f.forward = g.forward;
        // Cut detection against the previous accepted frame.
        bool cut = !have_ || bool(world_origin) != previous_relative_ ||
                   frame <= last_frame_ || frame - last_frame_ > lim.max_frame_gap;
        if (!cut) {
            double d2 = 0, dot = 0;
            for (size_t j = 0; j < 3; ++j) {
                const double d = g.position[j] - prev_.position[j];
                d2 += d * d;
                dot += g.forward[j] * prev_.forward[j];
            }
            const double angle = std::acos(std::clamp(dot, -1.0, 1.0));
            if (d2 > lim.max_translation * lim.max_translation || angle > lim.max_rotation ||
                std::abs(f.fov_y - prev_fov_) > lim.max_fov_change) cut = true;
        }
        // Jitter: known injected sample, else offset from the running mean.
        double jx = 0, jy = 0; // clip-space translate-by-W amounts to remove
        if (injected_px) {
            f.jitter_known = true;
            f.jitter_px_x = injected_px[0];
            f.jitter_px_y = injected_px[1];
            jx = 2.0 * injected_px[0] / double(width);
            jy = -2.0 * injected_px[1] / double(height);
        } else {
            if (cut) { mean_dx_ = g.dx; mean_dy_ = g.dy; }
            else { mean_dx_ = 0.875 * mean_dx_ + 0.125 * g.dx; mean_dy_ = 0.875 * mean_dy_ + 0.125 * g.dy; }
            jx = g.dx - mean_dx_;
            jy = g.dy - mean_dy_;
            f.jitter_px_x = jx * double(width) * 0.5;
            f.jitter_px_y = -jy * double(height) * 0.5;
        }
        f.unjittered = UnjitterViewProjection(m, f.jitter_px_x, f.jitter_px_y, width, height);
        f.view_proj_current_row = TransposeMatrix(f.unjittered);
        if (!Invert(f.view_proj_current_row, f.inv_view_proj_current_row)) return f;
        f.camera_cut = cut;
        f.view_proj_previous_row = cut ? f.view_proj_current_row : prev_vp_row_;
        if (world_origin && !cut) {
            std::array<double, 3> delta{};
            for (size_t i = 0; i < 3; ++i) delta[i] = (*world_origin)[i] - previous_origin_[i];
            f.view_proj_previous_row = TransposeMatrix(ComposeCameraRelativeViewProj(
                TransposeMatrix(prev_vp_row_), delta));
        }
        f.valid = true;
        prev_ = g;
        prev_fov_ = f.fov_y;
        prev_vp_row_ = f.view_proj_current_row;
        last_frame_ = frame;
        have_ = true;
        previous_relative_ = world_origin != nullptr;
        if (world_origin) previous_origin_ = *world_origin;
        return f;
    }
    CameraFrame UpdateCameraRelative(const Matrix& m, const std::array<double, 3>& world_origin,
                                    uint64_t frame, uint32_t width, uint32_t height,
                                    const double* injected_px = nullptr) {
        return Update(m, frame, width, height, injected_px, Limits{}, &world_origin);
    }
    void Reset() { have_ = false; }

private:
    bool have_ = false;
    bool previous_relative_ = false;
    std::array<double, 3> previous_origin_{};
    uint64_t last_frame_ = 0;
    VpGeometry prev_{};
    double prev_fov_ = 0, mean_dx_ = 0, mean_dy_ = 0;
    Matrix prev_vp_row_{};
};

} // namespace na::camera
