// A5 independent content corroboration. No GPU access or production promotion.
#pragma once
#include "signal_candidates.hpp"
#include <array>
#include <limits>

namespace na::classifier {
using Vec3 = std::array<double, 3>;
using Matrix = std::array<double, 16>; // row-major, column-vector multiplication
enum class ContentResult { Unavailable, Contradicted, Consistent };
struct ContentReport {
    ContentResult result{ContentResult::Unavailable};
    size_t checked{}, unresolved{}, mismatches{};
    double max_error{};
    std::string reason;
};
struct SampleIdentity {
    Scope scope{};
    uint64_t resource{}, version{};
    uint32_t subresource{}, width{}, height{};
    bool completed{}, lifetime_valid{}, coverage_complete{};
};
inline bool SameSample(const SampleIdentity& a, const SampleIdentity& b) {
    return SameScope(a.scope, b.scope) && a.resource == b.resource && a.version == b.version &&
        a.subresource == b.subresource && a.width == b.width && a.height == b.height;
}
inline bool Ready(const SampleIdentity& s) {
    return ValidScope(s.scope) && Known(s.resource) && Known(s.version) && s.width && s.height &&
        s.width <= 4096 && s.height <= 4096 && s.completed && s.lifetime_valid && s.coverage_complete;
}
inline bool Finite(const Vec3& p) {
    return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
}
inline bool Tolerance(double t) { return std::isfinite(t) && t > 0 && t <= 0.01; }
inline void Finish(ContentReport& r) {
    if (r.mismatches) {
        r.result = ContentResult::Contradicted; r.reason = "Independent evidence contradicts candidate";
    } else if (r.unresolved || !r.checked) {
        r.reason = "Incomplete content coverage; acquire independent evidence for unresolved pixels";
    } else {
        r.result = ContentResult::Consistent;
        r.reason = "Consistent with supplied independent evidence; evidence provenance still required";
    }
}

// World points must be independently known visible opaque surface positions,
// never reconstructed from this candidate depth (which would be circular).
// Evidence describes one full raster with a qualified camera and jitter.
inline ContentReport CheckDepth(const SampleIdentity& sample, const SampleIdentity& expected,
    const std::vector<double>& depth, const std::vector<Vec3>& world,
    const Matrix& view_projection, double jitter_x, double jitter_y,
    bool reversed, double tolerance = 1e-5) {
    ContentReport r{};
    r.reason = "Missing, stale, incomplete, or malformed depth/camera evidence";
    if (!Ready(sample) || !Ready(expected) || !SameSample(sample, expected) || !Tolerance(tolerance) ||
        !std::isfinite(jitter_x) || !std::isfinite(jitter_y)) return r;
    const size_t count = size_t(sample.width) * sample.height;
    if (depth.size() != count || world.size() != count) return r;
    for (double v : view_projection) if (!std::isfinite(v)) return r;
    for (size_t i = 0; i < count; ++i) {
        if (!Finite(world[i]) || !std::isfinite(depth[i])) return r;
    }
    for (size_t i = 0; i < count; ++i) {
        const auto& p = world[i];
        std::array<double, 4> clip{};
        for (size_t row = 0; row < 4; ++row)
            clip[row] = view_projection[row*4]*p[0] + view_projection[row*4+1]*p[1] +
                view_projection[row*4+2]*p[2] + view_projection[row*4+3];
        if (!std::isfinite(clip[3]) || clip[3] <= 1e-12) { ++r.unresolved; continue; }
        const double x = (clip[0]/clip[3]+1)*0.5*sample.width + jitter_x;
        const double y = (1-clip[1]/clip[3])*0.5*sample.height + jitter_y;
        const double z = clip[2]/clip[3];
        // A wrong camera must not be accepted simply because Z is plausible.
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || z < 0 || z > 1 ||
            std::abs(x-(double(i % sample.width)+0.5)) > 1e-4 ||
            std::abs(y-(double(i / sample.width)+0.5)) > 1e-4) {
            ++r.unresolved; continue;
        }
        ++r.checked;
        const double error = std::abs(depth[i] - (reversed ? 1-z : z));
        r.max_error = std::max(r.max_error, error);
        if (depth[i] < 0 || depth[i] > 1 || error > tolerance) ++r.mismatches;
    }
    Finish(r); return r;
}

struct CompositionWitness {
    SampleIdentity source{}; // exact scene version feeding this composition
    // Linear RGB, known premultiplied foreground, conventional source-over.
    // Captured/controlled blend model required; unknown tone mapping is not this model.
    std::vector<Vec3> composite, foreground;
    std::vector<double> alpha;
};
inline ContentReport CheckColor(const SampleIdentity& sample, const std::vector<Vec3>& color,
    const std::vector<CompositionWitness>& witnesses, double tolerance = 1e-5) {
    ContentReport r{};
    r.reason = "Missing, stale, incomplete, or malformed color/composition evidence";
    if (!Ready(sample) || !Tolerance(tolerance) || witnesses.empty() || witnesses.size() > 4) return r;
    const size_t count = size_t(sample.width) * sample.height;
    if (color.size() != count) return r;
    for (const auto& c : color) if (!Finite(c)) return r;
    for (const auto& w : witnesses) {
        if (!Ready(w.source) || !SameSample(sample, w.source) || w.composite.size() != count ||
            w.foreground.size() != count || w.alpha.size() != count) return r;
        for (size_t i = 0; i < count; ++i)
            if (!Finite(w.composite[i]) || !Finite(w.foreground[i]) ||
                !std::isfinite(w.alpha[i]) || w.alpha[i] < 0 || w.alpha[i] > 1) return r;
    }
    for (size_t i = 0; i < count; ++i) {
        bool visible = false, mismatch = false;
        for (const auto& w : witnesses) {
            const double transmission = 1-w.alpha[i];
            // Opaque or near-opaque HUD cannot establish underlying scene color.
            if (transmission < 0.05) continue;
            visible = true;
            for (size_t c = 0; c < 3; ++c) {
                const double observed = (w.composite[i][c]-w.foreground[i][c])/transmission;
                const double error = std::abs(observed-color[i][c]);
                r.max_error = std::max(r.max_error, error);
                mismatch = mismatch || !std::isfinite(error) || error > tolerance;
            }
        }
        if (!visible) ++r.unresolved;
        else { ++r.checked; if (mismatch) ++r.mismatches; }
    }
    Finish(r); return r;
}
} // namespace na::classifier
