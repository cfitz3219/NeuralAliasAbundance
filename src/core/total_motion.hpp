// Measured total motion: the per-pixel decision of the flow pass's `measure` program
// (host_dx12/j2/flow_shaders.hpp), as a CPU reference the fixtures run on. The two are kept in
// step by hand: same samples, same order of decisions, same constants.
//
// What it answers, for one pixel of a stabilised image pair (both pictures in the unjittered
// frame), with no camera: where was this pixel's content one frame ago? v = p_previous - p_current
// in pixels, +x right, +y down (the motion arbiter's convention for a tier-1 supplier).
//
// Rules (Astra's corrections of 2026-11-16 to the first handoff):
//  - The block search gives whole pixels. A whole-pixel answer, zero included, is a HYPOTHESIS, not
//    a measurement. Each hypothesis is refined below a pixel on the image itself, and "nothing
//    moved" is always one of the hypotheses, so a slow pan the search rounds to zero is still found.
//  - There is no small-motion cutoff. A measured 0.1 px is handed on as 0.1 px.
//  - Exact stillness is a hypothesis too. The refined vector replaces it only when it explains the
//    picture clearly better (still_ratio); otherwise stillness is the measurement. That is evidence
//    from the two pictures, not a threshold on the vector's length.
//  - Where the picture cannot measure (flat, or neither hypothesis explains it) the search's own
//    vector is handed on as an APPROXIMATION with validity kApproximate, never as a measurement;
//    with no search vector either, nothing is known: vector 0, validity 0, and that zero must not
//    be read as "still".
//  - No temporal rule: a reversal is measured on the frame it happens.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace na::motion::total {

struct Image {
    const float* luma = nullptr; // row-major, [0,1]
    uint32_t width = 0, height = 0;
};

// The shader's SampleLevel with a clamp sampler: bilinear, texel centres at +0.5.
inline float Sample(const Image& im, float x, float y)
{
    const float fx = std::clamp(x - 0.5f, 0.0f, float(im.width - 1));
    const float fy = std::clamp(y - 0.5f, 0.0f, float(im.height - 1));
    const uint32_t x0 = static_cast<uint32_t>(fx), y0 = static_cast<uint32_t>(fy);
    const uint32_t x1 = (std::min)(x0 + 1, im.width - 1), y1 = (std::min)(y0 + 1, im.height - 1);
    const float tx = fx - float(x0), ty = fy - float(y0);
    const float* r0 = im.luma + size_t(y0) * im.width;
    const float* r1 = im.luma + size_t(y1) * im.width;
    return (r0[x0] * (1 - tx) + r0[x1] * tx) * (1 - ty) + (r1[x0] * (1 - tx) + r1[x1] * tx) * ty;
}

struct Params {
    float image_abs = 0.08f;   // mean |luma| error a hypothesis may leave, at most ...
    float image_rel = 0.15f;   // ... and at most this share of the window's own contrast, to explain the picture
    float texture_min = 0.03f; // local contrast (max - min over the window) the image needs to measure
    float still_ratio = 0.5f;  // a sub-pixel vector must leave at most this share of the error stillness leaves
    float bound = 1.0f;        // refinement stays within this many pixels of its hypothesis, each axis
    float damp = 2e-5f;        // luma^2: no step where the window has no gradient at all
    float edge = 0.05f;        // weaker gradient direction under this share of the stronger: an edge, moved across itself only
    int steps = 2;             // refinement steps
};

constexpr float kMeasured = 1.0f;    // validity: the two pictures confirm the vector (moving or still)
constexpr float kApproximate = 0.5f; // validity: the search's vector, which the pictures could not confirm
constexpr int kSpacing = 2;          // pixels between the window's 3x3 samples (a 5x5 pixel footprint)

// The measure counters' codes (compute_flow.hpp Stats::total_sum).
enum Why : uint8_t { kMoving = 0, kNothing = 1, kOutside = 4, kUnconfirmed = 5, kStill = 7 };

struct Result {
    float x = 0, y = 0;
    float valid = 0;
    uint8_t why = kNothing;
};

// The current picture around p: nine samples two pixels apart, each one's gradient (central
// differences one pixel either side), the structure sums and the local contrast. 33 samples.
struct Window {
    float lc[9];
    float gx[9], gy[9];
    float a11 = 0, a12 = 0, a22 = 0;
    float contrast = 0;
};

inline Window WindowAt(const Image& cur, float px, float py)
{
    float side[3][4], updown[4][3]; // one pixel left/right of each sample; one pixel above/below
    for (int j = -1; j <= 1; ++j)
        for (int m = 0; m < 4; ++m) side[j + 1][m] = Sample(cur, px + float(2 * m - 3), py + float(kSpacing * j));
    for (int m = 0; m < 4; ++m)
        for (int i = -1; i <= 1; ++i) updown[m][i + 1] = Sample(cur, px + float(kSpacing * i), py + float(2 * m - 3));
    Window w;
    float lo = 1e9f, hi = -1e9f;
    for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i) {
            const int k = (j + 1) * 3 + (i + 1);
            w.lc[k] = Sample(cur, px + float(kSpacing * i), py + float(kSpacing * j));
            w.gx[k] = 0.5f * (side[j + 1][i + 2] - side[j + 1][i + 1]);
            w.gy[k] = 0.5f * (updown[j + 2][i + 1] - updown[j + 1][i + 1]);
            w.a11 += w.gx[k] * w.gx[k];
            w.a12 += w.gx[k] * w.gy[k];
            w.a22 += w.gy[k] * w.gy[k];
            lo = (std::min)(lo, w.lc[k]);
            hi = (std::max)(hi, w.lc[k]);
        }
    }
    w.contrast = hi - lo;
    return w;
}

// Bounded refinement of one hypothesis v0: damped Gauss-Newton steps on the window, with the current
// picture's gradients. Returns the best vector met (the hypothesis itself included), its mean
// |luma| error `e`, and the error `e0` of the hypothesis exactly. 9 samples per evaluation.
inline void Refine(const Image& prev, const Window& w, float px, float py, float v0x, float v0y, const Params& p,
                   float& vx, float& vy, float& e, float& e0)
{
    const float a11 = w.a11 + p.damp, a22 = w.a22 + p.damp, a12 = w.a12;
    const float det = a11 * a22 - a12 * a12;
    // The window's two gradient directions: l1 >= l2, (ex, ey) the stronger one.
    const float half = 0.5f * (a11 + a22);
    const float spread = std::sqrt((std::max)(0.0f, 0.25f * (a11 - a22) * (a11 - a22) + a12 * a12));
    const float l1 = half + spread, l2 = half - spread;
    float ex = a12, ey = l1 - a11;
    const float len = std::sqrt(ex * ex + ey * ey);
    if (len < 1e-12f) { ex = 1.0f; ey = 0.0f; }
    else { ex /= len; ey /= len; }
    const bool edge = l2 < p.edge * l1;
    float cx = v0x, cy = v0y;
    vx = v0x;
    vy = v0y;
    e = e0 = 0;
    for (int step = 0; step <= p.steps; ++step) {
        float bx = 0, by = 0, err = 0;
        for (int j = -1; j <= 1; ++j) {
            for (int i = -1; i <= 1; ++i) {
                const int k = (j + 1) * 3 + (i + 1);
                const float d = Sample(prev, px + float(kSpacing * i) + cx, py + float(kSpacing * j) + cy) - w.lc[k];
                bx += w.gx[k] * d;
                by += w.gy[k] * d;
                err += std::fabs(d);
            }
        }
        err /= 9.0f;
        if (step == 0) e0 = e = err;
        else if (err < e) { e = err; vx = cx; vy = cy; }
        if (step == p.steps) break;
        // previous(x + v + d) ~ previous(x + v) + g.d  =>  d = -A^-1 b; on an edge, only across it.
        float dx, dy;
        if (edge) {
            const float along = -(ex * bx + ey * by) / l1;
            dx = along * ex;
            dy = along * ey;
        } else {
            dx = -(a22 * bx - a12 * by) / det;
            dy = -(a11 * by - a12 * bx) / det;
        }
        cx = std::clamp(cx + dx, v0x - p.bound, v0x + p.bound);
        cy = std::clamp(cy + dy, v0y - p.bound, v0y + p.bound);
    }
}

// One pixel, at most 33 + 2 * 27 samples. (px, py): its position in the stabilised pictures (raster pixel centre minus the
// frame's jitter). `raw`: the block search's vector there after the depth-aware expansion, and
// whether that expansion vouches for it.
inline Result MeasurePixel(const Image& cur, const Image& prev, float px, float py, bool have_raw, float raw_x,
                           float raw_y, const Params& p = {})
{
    Result r;
    const bool raw_on = have_raw && px + raw_x >= 0.0f && py + raw_y >= 0.0f && px + raw_x <= float(cur.width) &&
                        py + raw_y <= float(cur.height);
    const auto approximate = [&] {
        if (!have_raw) return; // nothing known: vector 0, validity 0
        r.x = raw_x;
        r.y = raw_y;
        r.valid = kApproximate;
        r.why = raw_on ? kUnconfirmed : kOutside;
    };
    const Window w = WindowAt(cur, px, py);
    if (w.contrast < p.texture_min) {
        approximate();
        return r;
    }
    float zx, zy, ez, e_still;
    Refine(prev, w, px, py, 0.0f, 0.0f, p, zx, zy, ez, e_still);
    float bx = zx, by = zy, eb = ez;
    if (raw_on && (raw_x != 0.0f || raw_y != 0.0f)) {
        float rx, ry, er, er0;
        Refine(prev, w, px, py, raw_x, raw_y, p, rx, ry, er, er0);
        if (er <= ez) { bx = rx; by = ry; eb = er; }
    }
    // Explaining the picture: a small error in itself, and small beside the window's own contrast
    // (in a low-contrast window every position leaves a small error).
    const float limit = (std::min)(p.image_abs, p.image_rel * w.contrast);
    if (eb > limit) {
        approximate();
        return r;
    }
    r.valid = kMeasured;
    const bool still = (bx == 0.0f && by == 0.0f) || (e_still <= limit && !(eb <= p.still_ratio * e_still));
    if (still) r.why = kStill;
    else { r.x = bx; r.y = by; r.why = kMoving; }
    return r;
}

} // namespace na::motion::total
