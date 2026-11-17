// The screen's own motion: one whole-picture camera movement fitted to the places the pair of
// pictures can measure, with no camera and no lens from the game. CPU reference of the `fit_sample`,
// `fit_accum` and `fit_solve` programs (host_dx12/j2/flow_shaders.hpp); the two are kept in step by
// hand: same samples, same terms, same weights, same order of decisions, same constants.
//
// Why it exists (Cort's Sniper Elite Resistance run of 2026-11-17, build 3621e3a8). Measured total
// motion decided every pixel by itself. On a real frame about two thirds of the pixels cannot be
// measured (sky, haze, dark ground: local contrast under total_motion.hpp's texture_min, or neither
// hypothesis explains the pictures), and those were handed the block search's raw vector. With jitter
// that vector changes every frame, so a still scene shimmered. The synthetic fixtures are textured
// everywhere and never showed it. A pixel that cannot be measured now follows the screen's motion,
// and a raw search vector is never handed on.
//
// The model. A rigid camera moving through a still scene moves every pixel by a rotation part that
// depends only on where the pixel is, plus a translation part proportional to 1 / distance. With
// X, Y the pixel's position from the picture's centre (one scale for both axes) and e the depth
// value's distance from its far end (0 at the sky), v = previous position - current position is
//     vx = a0 + a1 X + a2 Y + a6 X^2 + a7 X Y + e (c1 + c3 X)
//     vy = a3 + a4 X + a5 Y + a6 X Y + a7 Y^2 + e (c2 + c3 Y)
// The eight a terms are the classic small-rotation / plane flow, which holds for any lens (the focal
// length only scales them). A depth buffer of a perspective projection is A + B / distance whichever
// way round it runs, so the parallax is linear in e and the unknown A lands in the a terms. Eleven
// numbers, linear in all of them: no lens, no near plane, no matrix from the game.
//
// The fit. Samples are the 8x8 block centres the pictures can measure (total_motion.hpp
// SamplePixel: the best vector itself, with no preference for stillness). One plain least-squares
// step, then reweighted steps. The model is linear, so each step is a complete weighted solve; it is
// written as a correction to the terms so far (the right-hand side holds what the terms leave
// unexplained), which gives the same answer and lets the GPU's 32-bit sums correct their own
// rounding from one step to the next. The weight is 1 / (1 + r^2 / s^2)^2 (Geman-McClure), whose
// scale s starts wide and halves down to robust_px. Its pull on the fit falls as 1 / r^3: something
// moving by itself across a corner of the screen, where a sample has the most leverage, lets go of
// the fit. The plain 1 / (1 + r^2 / s^2) keeps a pull of 1 / r and was still bent by it after forty
// steps (tests/cpu/screen_motion_test.cpp, the vehicle case).
// Things that move by themselves are down-weighted and keep their own measured vectors later.
// The model is used only when enough samples agree with it. It is never asked about a depth nearer
// than the nearest sample that agrees: beyond that it has no evidence, and the parallax terms would
// only carry their noise further (nearness_limit).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace na::motion::screen {

constexpr int kTerms = 11;
constexpr int kPairs = kTerms * (kTerms + 1) / 2; // the normal matrix's upper triangle, row by row
// One sample's accumulators, in the GPU's layout: 0..65 H, 66..76 g (against what the current terms
// leave unexplained), then
constexpr int kCount = 77;   // confirmed samples
constexpr int kEnergy = 78;  // their robust error at the current terms
constexpr int kInliers = 79; // those within the robust scale of the model
constexpr int kNearest = 80;     // the largest e among the inliers (a maximum, not a sum)
constexpr int kValues = 81;

struct Params {
    float robust_px = 1.5f;        // the weight's final scale; a sample within it of the model agrees with it
    float start_px = 12.0f;        // the scale of the first reweighted solve, halved each step down to robust_px
    int steps = 12;                // solves: one plain, the rest reweighted
    float min_samples = 256.0f;    // confirmed samples the fit needs
    float min_agree = 0.5f;        // share of them that must agree with the fitted model
    float max_vector = 512.0f;     // a sample's vector beyond this is not one (pixels, each axis)
    float damping = 1e-4f;         // on the scaled normal matrix's diagonal
    float converged_px = 0.001f;   // a step that moves its samples less than this (weighted RMS) is the last one
};

// One 8x8 block centre. `nearness` is the model's e: |depth - the depth's far end|, 0 for sky or an
// unusable depth value; it grows as things come nearer. (Not called `far`: that bare word is a macro
// in the Windows headers.)
struct Sample {
    float px = 0, py = 0, nearness = 0;
    float vx = 0, vy = 0;
    bool confirmed = false;
};

struct Model {
    float theta[kTerms] = {};
    float nearness_limit = 0; // the model is evaluated at min(e, nearness_limit)
    bool valid = false;
    float samples = 0, agree = 0, energy = 0; // confirmed samples, those agreeing, mean robust error at the fit
    int steps = 0;                            // solves made
};

// The two rows of one pixel's terms. `scale` = 2 / height for both axes.
inline void Terms(float px, float py, float nearness, uint32_t width, uint32_t height, float bu[kTerms], float bv[kTerms])
{
    const float scale = 2.0f / float(height);
    const float X = (px - 0.5f * float(width)) * scale, Y = (py - 0.5f * float(height)) * scale;
    bu[0] = 1; bu[1] = X; bu[2] = Y; bu[3] = 0; bu[4] = 0; bu[5] = 0; bu[6] = X * X; bu[7] = X * Y; bu[8] = nearness; bu[9] = 0;   bu[10] = nearness * X;
    bv[0] = 0; bv[1] = 0; bv[2] = 0; bv[3] = 1; bv[4] = X; bv[5] = Y; bv[6] = X * Y; bv[7] = Y * Y; bv[8] = 0;   bv[9] = nearness; bv[10] = nearness * Y;
}

// The screen's motion at a pixel. Only for a valid model.
inline void Evaluate(const Model& m, float px, float py, float nearness, uint32_t width, uint32_t height, float& vx, float& vy)
{
    float bu[kTerms], bv[kTerms];
    Terms(px, py, (std::min)(nearness, m.nearness_limit), width, height, bu, bv);
    vx = vy = 0;
    for (int k = 0; k < kTerms; ++k) { vx += m.theta[k] * bu[k]; vy += m.theta[k] * bv[k]; }
}

inline bool Usable(const Sample& s, const Params& p)
{
    return s.confirmed && std::isfinite(s.vx) && std::isfinite(s.vy) && std::isfinite(s.nearness) && std::fabs(s.vx) < p.max_vector &&
           std::fabs(s.vy) < p.max_vector;
}

// The reweighting scale of solve `step` (1 = the first reweighted one): wide first, so that a plain
// first answer pulled off by things moving by themselves is not locked in, then down to robust_px.
inline float ScaleAt(int step, const Params& p)
{
    float c = p.start_px;
    for (int i = 1; i < step && c > p.robust_px; ++i) c *= 0.5f;
    return (std::max)(c, p.robust_px);
}

// One pass over the samples at the given terms. `scale_px` 0: the first, plain step. Agreement and
// the robust error are always counted at robust_px.
inline void Accumulate(const Sample* samples, size_t count, uint32_t width, uint32_t height, const float theta[kTerms], float scale_px,
                       const Params& p, double sums[kValues])
{
    for (int i = 0; i < kValues; ++i) sums[i] = 0;
    const float c2 = p.robust_px * p.robust_px, s2 = scale_px * scale_px;
    const bool weighted = scale_px > 0;
    for (size_t s = 0; s < count; ++s) {
        const Sample& a = samples[s];
        if (!Usable(a, p)) continue;
        float bu[kTerms], bv[kTerms];
        Terms(a.px, a.py, a.nearness, width, height, bu, bv);
        float mx = 0, my = 0;
        for (int k = 0; k < kTerms; ++k) { mx += theta[k] * bu[k]; my += theta[k] * bv[k]; }
        const float rx = a.vx - mx, ry = a.vy - my;
        const float rr = rx * rx + ry * ry;
        const float q = weighted ? 1.0f / (1.0f + rr / s2) : 1.0f;
        const float w = q * q;
        int idx = 0;
        for (int i = 0; i < kTerms; ++i)
            for (int j = i; j < kTerms; ++j) sums[idx++] += double(w * (bu[i] * bu[j] + bv[i] * bv[j]));
        for (int i = 0; i < kTerms; ++i) sums[kPairs + i] += double(w * (bu[i] * rx + bv[i] * ry));
        sums[kCount] += 1.0;
        sums[kEnergy] += double(0.5f * rr / (1.0f + rr / c2));
        if (rr <= c2) {
            sums[kInliers] += 1.0;
            sums[kNearest] = (std::max)(sums[kNearest], double(a.nearness));
        }
    }
}

// The scaled, damped normal equations by Cholesky: the correction to add to the terms the sums were
// taken at. False when they cannot be solved.
inline bool Solve(const double sums[kValues], const Params& p, float delta[kTerms])
{
    double A[kTerms][kTerms], b[kTerms], sc[kTerms];
    int idx = 0;
    for (int i = 0; i < kTerms; ++i)
        for (int j = i; j < kTerms; ++j) { A[i][j] = A[j][i] = sums[idx]; ++idx; }
    for (int i = 0; i < kTerms; ++i) {
        b[i] = sums[kPairs + i];
        sc[i] = A[i][i] > 1e-20 ? 1.0 / std::sqrt(A[i][i]) : 0.0;
    }
    for (int i = 0; i < kTerms; ++i) {
        for (int j = 0; j < kTerms; ++j) A[i][j] *= sc[i] * sc[j];
        // A term no sample carries (all depths at the far end, say) keeps zero.
        A[i][i] = (sc[i] > 0 ? A[i][i] : 0.0) + double(p.damping) + (sc[i] > 0 ? 0.0 : 1.0);
        b[i] *= sc[i];
    }
    double L[kTerms][kTerms] = {};
    for (int j = 0; j < kTerms; ++j) {
        double d = A[j][j];
        for (int m = 0; m < j; ++m) d -= L[j][m] * L[j][m];
        if (!(d > 1e-12)) return false;
        L[j][j] = std::sqrt(d);
        for (int i = j + 1; i < kTerms; ++i) {
            double t = A[i][j];
            for (int m = 0; m < j; ++m) t -= L[i][m] * L[j][m];
            L[i][j] = t / L[j][j];
        }
    }
    double y[kTerms], x[kTerms];
    for (int i = 0; i < kTerms; ++i) {
        double t = b[i];
        for (int m = 0; m < i; ++m) t -= L[i][m] * y[m];
        y[i] = t / L[i][i];
    }
    for (int i = kTerms - 1; i >= 0; --i) {
        double t = y[i];
        for (int m = i + 1; m < kTerms; ++m) t -= L[m][i] * x[m];
        x[i] = t / L[i][i];
    }
    for (int i = 0; i < kTerms; ++i) {
        const float v = float(x[i] * sc[i]);
        if (!std::isfinite(v)) return false;
        delta[i] = v;
    }
    return true;
}

// How far a correction moves the samples its step trusted: the weighted RMS of B * delta over them,
// in pixels. delta' H delta is that weighted sum of squares, and H[0][0] is the sum of the weights
// (the first term's row is the constant 1).
inline float StepSize(const double sums[kValues], const float delta[kTerms])
{
    double q = 0;
    int idx = 0;
    for (int i = 0; i < kTerms; ++i)
        for (int j = i; j < kTerms; ++j) { q += (i == j ? 1.0 : 2.0) * double(delta[i]) * double(delta[j]) * sums[idx]; ++idx; }
    return sums[0] > 1e-20 ? float(std::sqrt((std::max)(q, 0.0) / sums[0])) : 0.0f;
}

inline Model Fit(const Sample* samples, size_t count, uint32_t width, uint32_t height, const Params& p = {})
{
    Model m;
    if (!samples || !width || !height) return m;
    double sums[kValues];
    float theta[kTerms] = {};
    bool ok = true;
    for (int step = 0; step < p.steps && ok; ++step) {
        const float scale = step > 0 ? ScaleAt(step, p) : 0.0f;
        Accumulate(samples, count, width, height, theta, scale, p, sums);
        if (sums[kCount] < double(p.min_samples)) { ok = false; break; }
        float delta[kTerms];
        ok = Solve(sums, p, delta);
        if (!ok) break;
        for (int k = 0; k < kTerms; ++k) theta[k] += delta[k];
        ++m.steps;
        // Once the weight has its final scale, a step that no longer moves its samples is the last:
        // further steps would repeat it. (Measured on Cort's captured frames: 6 to 9 solves of 12.)
        if (scale > 0 && scale <= p.robust_px && StepSize(sums, delta) < p.converged_px) break;
    }
    // The fitted terms against the samples once more: how many agree, and how near the nearest of those is.
    if (ok) Accumulate(samples, count, width, height, theta, p.robust_px, p, sums);
    m.samples = float(sums[kCount]);
    if (!ok) return m;
    m.agree = float(sums[kInliers]);
    m.energy = float(sums[kEnergy] / sums[kCount]);
    m.nearness_limit = float(sums[kNearest]);
    for (int k = 0; k < kTerms; ++k) m.theta[k] = theta[k];
    m.valid = m.agree >= p.min_agree * m.samples;
    return m;
}

} // namespace na::motion::screen
