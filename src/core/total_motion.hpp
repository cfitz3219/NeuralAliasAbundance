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
//  - Where the picture cannot measure (flat, or no hypothesis explains it) the pixel follows the
//    SCREEN'S OWN MOTION (core/screen_motion.hpp: one camera movement fitted to the places that can
//    be measured), with validity kApproximate. The search's raw vector is never handed on: on a real
//    frame most pixels are in this case, and that vector changes with every jitter sample (Cort's
//    Sniper Elite Resistance run, 2026-11-17: a still scene shimmered). With no screen motion to
//    follow, nothing is known: vector 0, validity 0, and that zero must not be read as "still".
//  - With a screen motion, IT is the hypothesis "nothing moved by itself" in place of exact
//    stillness: a pixel is measured as moving with the screen unless its own vector explains the
//    picture clearly better. With none, the hypothesis is exact stillness, as before.
//  - With a screen motion, a pixel's own vector counts only when it LEAVES THE SAMPLING SNAP'S RANGE
//    around the screen's motion (`bound`, one pixel either way on each axis) and explains the picture
//    clearly better than the screen's motion does anywhere inside that range. The stabilised
//    pictures are a jittered, point-sampled raster shifted back: anything thinner than a pixel (a
//    distant light, a wire, a mast, a hard edge) lands on a different pixel with every jitter sample,
//    so between two stabilised pictures it moves by up to a pixel either way while standing still.
//    Measured as motion, that handed DLSS a different vector for a still light on every frame (Cort's
//    Sniper Elite Resistance run on 774fc12f, 2026-11-17: distant lights strobed and went solid up
//    close, where they cover several pixels; his still captured pair: 3.1 % of the screen "moving",
//    85 % of it within one pixel of the screen's motion). The screen's motion itself is fitted over
//    thousands of samples, where those snaps average out, so it holds every slow pan. What this
//    costs: a thing moving by itself less than a pixel a frame against the screen follows the screen.
//    Without a screen motion nothing changes: a slow pan is still measured pixel by pixel.
//  - ONE SAMPLE IS NOT A TEXTURE. The window's contrast is taken with the one sample that carries
//    most of its range set aside (the smaller of "second brightest minus darkest" and "brightest
//    minus second darkest"). A light smaller than a pixel is present in one jitter sample and gone in
//    the next; a window whose whole contrast is that one sample has nothing a match can hold on to,
//    and the nearest other dot was taken for it (same run: of 32 such lights in one captured pair, 14
//    were handed a false vector; 0 with this rule and the one above). Such a window cannot measure:
//    the pixel follows the screen. What this costs: fewer pixels measure (on that capture 31.5 % of
//    the screen before, 16.8 % after), and weakly textured pixels inside a moving thing follow the
//    screen more often.
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
    bool snap = true;          // with a screen motion: an own vector must leave the sampling snap's range (false: the rule before)
    bool lone = true;          // the window's contrast without the one sample that carries most of it (false: the rule before)
};

constexpr float kMeasured = 1.0f;    // validity: the two pictures confirm the vector (moving or still)
constexpr float kApproximate = 0.5f; // validity: the screen's own motion, where the pictures could not measure
constexpr int kSpacing = 2;          // pixels between the window's 3x3 samples (a 5x5 pixel footprint)

// The measure counters' codes (compute_flow.hpp Stats::total_sum).
// kStill: measured, and moving with the screen (exactly still when there is no screen motion).
// kScreen: not measurable, follows the screen's motion. kOutside and kUnconfirmed (the search's raw
// vector handed on) are no longer produced; their slots stay so old logs keep their meaning.
enum Why : uint8_t { kMoving = 0, kNothing = 1, kScreen = 2, kOutside = 4, kUnconfirmed = 5, kStill = 7 };

struct Result {
    float x = 0, y = 0;
    float valid = 0;
    uint8_t why = kNothing;
};

// The current picture around p: nine samples two pixels apart, each one's gradient (central
// differences one pixel either side), the structure sums and the local contrast (`lone`: with the
// one sample that carries most of the range set aside). 33 samples.
struct Window {
    float lc[9];
    float gx[9], gy[9];
    float a11 = 0, a12 = 0, a22 = 0;
    float contrast = 0;
};

inline Window WindowAt(const Image& cur, float px, float py, bool lone = true)
{
    float side[3][4], updown[4][3]; // one pixel left/right of each sample; one pixel above/below
    for (int j = -1; j <= 1; ++j)
        for (int m = 0; m < 4; ++m) side[j + 1][m] = Sample(cur, px + float(2 * m - 3), py + float(kSpacing * j));
    for (int m = 0; m < 4; ++m)
        for (int i = -1; i <= 1; ++i) updown[m][i + 1] = Sample(cur, px + float(kSpacing * i), py + float(2 * m - 3));
    Window w;
    float lo = 1e9f, lo2 = 1e9f, hi = -1e9f, hi2 = -1e9f; // the darkest and the brightest sample, and the next of each
    for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i) {
            const int k = (j + 1) * 3 + (i + 1);
            w.lc[k] = Sample(cur, px + float(kSpacing * i), py + float(kSpacing * j));
            w.gx[k] = 0.5f * (side[j + 1][i + 2] - side[j + 1][i + 1]);
            w.gy[k] = 0.5f * (updown[j + 2][i + 1] - updown[j + 1][i + 1]);
            w.a11 += w.gx[k] * w.gx[k];
            w.a12 += w.gx[k] * w.gy[k];
            w.a22 += w.gy[k] * w.gy[k];
            const float c = w.lc[k];
            if (c > hi) { hi2 = hi; hi = c; }
            else if (c > hi2) hi2 = c;
            if (c < lo) { lo2 = lo; lo = c; }
            else if (c < lo2) lo2 = c;
        }
    }
    w.contrast = lone ? (std::max)(0.0f, (std::min)(hi2 - lo, hi - lo2)) : hi - lo;
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

// The vector that explains the window best, among the refined hypothesis (hx, hy) and the refined
// search vector. False: the pictures cannot measure here (flat, or neither explains them).
// `e_with`: the error the hypothesis leaves exactly; `limit`: what counts as explaining the picture;
// `e_snap`: the least error the hypothesis leaves within its refinement's bound (the sampling snap's range).
inline bool BestVector(const Image& cur, const Image& prev, float px, float py, bool raw_on, float raw_x, float raw_y, float hx,
                       float hy, const Params& p, float& bx, float& by, float& eb, float& e_with, float& limit, float& e_snap)
{
    bx = hx;
    by = hy;
    eb = e_with = limit = e_snap = 0;
    const Window w = WindowAt(cur, px, py, p.lone);
    if (w.contrast < p.texture_min) return false;
    float zx, zy, ez;
    Refine(prev, w, px, py, hx, hy, p, zx, zy, ez, e_with);
    bx = zx;
    by = zy;
    eb = e_snap = ez;
    if (raw_on && (raw_x != hx || raw_y != hy)) {
        float rx, ry, er, er0;
        Refine(prev, w, px, py, raw_x, raw_y, p, rx, ry, er, er0);
        if (er <= ez) { bx = rx; by = ry; eb = er; }
    }
    // Explaining the picture: a small error in itself, and small beside the window's own contrast
    // (in a low-contrast window every position leaves a small error).
    limit = (std::min)(p.image_abs, p.image_rel * w.contrast);
    return eb <= limit;
}

inline bool RawOn(const Image& cur, float px, float py, bool have_raw, float raw_x, float raw_y)
{
    return have_raw && px + raw_x >= 0.0f && py + raw_y >= 0.0f && px + raw_x <= float(cur.width) && py + raw_y <= float(cur.height);
}

// One sample for the screen's motion (screen_motion.hpp): the best vector itself, below a pixel,
// with no preference for exact stillness. A single pixel prefers "still" so that jitter is not
// reported as motion; over thousands of samples that preference would be a bias (every slow
// parallax read as zero), and their noise averages out instead. False: not a sample.
inline bool SamplePixel(const Image& cur, const Image& prev, float px, float py, bool have_raw, float raw_x, float raw_y,
                        float& vx, float& vy, const Params& p = {})
{
    float eb, e_with, limit, e_snap;
    return BestVector(cur, prev, px, py, RawOn(cur, px, py, have_raw, raw_x, raw_y), raw_x, raw_y, 0.0f, 0.0f, p, vx, vy, eb, e_with, limit, e_snap);
}

// One pixel, at most 33 + 2 * 27 samples. (px, py): its position in the stabilised pictures (raster pixel centre minus the
// frame's jitter). `raw`: the block search's vector there after the depth-aware expansion, and
// whether that expansion vouches for it. `screen`: the screen's own motion at this pixel
// (screen_motion.hpp Evaluate), when there is a valid one.
inline Result MeasurePixel(const Image& cur, const Image& prev, float px, float py, bool have_raw, float raw_x,
                           float raw_y, const Params& p = {}, bool have_screen = false, float screen_x = 0.0f,
                           float screen_y = 0.0f)
{
    Result r;
    const float hx = have_screen ? screen_x : 0.0f, hy = have_screen ? screen_y : 0.0f;
    float bx, by, eb, e_with, limit, e_snap;
    if (!BestVector(cur, prev, px, py, RawOn(cur, px, py, have_raw, raw_x, raw_y), raw_x, raw_y, hx, hy, p, bx, by, eb, e_with, limit, e_snap)) {
        if (have_screen) { // follows the screen; with none, nothing is known: vector 0, validity 0
            r.x = hx;
            r.y = hy;
            r.valid = kApproximate;
            r.why = kScreen;
        }
        return r;
    }
    r.valid = kMeasured;
    bool with;
    if (have_screen && p.snap) {
        // Inside the snap's range the picture cannot tell motion from sampling: with the screen.
        // Outside it, the own vector must beat the best the screen's motion does anywhere inside.
        const bool outside = std::fabs(bx - hx) > p.bound || std::fabs(by - hy) > p.bound;
        with = !outside || (e_snap <= limit && !(eb <= p.still_ratio * e_snap));
    } else {
        with = (bx == hx && by == hy) || (e_with <= limit && !(eb <= p.still_ratio * e_with));
    }
    if (with) { r.x = hx; r.y = hy; r.why = kStill; }
    else { r.x = bx; r.y = by; r.why = kMoving; }
    return r;
}

} // namespace na::motion::total
