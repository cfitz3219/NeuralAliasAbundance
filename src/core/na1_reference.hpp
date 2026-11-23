#pragma once

// The processor reference of the NA1 enhancer: the picture maths of docs/NA1-ENHANCER.md sections
// 2 and 4 and the Measure pass of section 3, one function per card pass. The shader in
// host_dx12/effects/na1.hlsl is kept in step with this file by hand; tests/host/na1_test.cpp
// compares the two texture by texture, which is why every pass is callable on its own and why the
// arithmetic is single precision in the order the contract writes it.
//
// Where the contract is silent this file decides, and the shader must do the same:
//   - luminance is max(dot(rgb, weights), 0) everywhere it is used;
//   - a depth value that is not a finite number counts as sky (q = 0);
//   - a lighting-delta value that is not a finite number counts as no change;
//   - Measure treats channels below zero as zero (colour strength of a negative pixel means nothing);
//   - blur taps are added nearest offset first, the lower coordinate before the higher.
//   - a scene m0 of zero or less switches the near-black protection off instead of dividing by zero
//     (the governor never hands one out).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace na::na1::ref {

constexpr uint32_t kCellsW = 128, kCellsH = 72, kCellFloats = 24;

// How bright a scene value is on the screen (section 4).
struct Curve {
    int mode = 2;            // 0 display-referred (8-bit), 1 fitted table, 2 assumed
    float x0 = 0, dx = 1;    // the table's first knot and knot spacing, in y
    float cy[16] = {};       // screen value at each knot, 0..1 linear
    float mean_y = 0;        // the picture's mean y, for the assumed curve
};
// What the Referee measured about the scene; the governor smooths it.
struct Scene {
    float mid = 0, m0 = 0.18f, w_dark = 0, p85 = 0.5f, p97 = 0.9f;
    float tint[3] = {1, 1, 1};
    Curve curve;
};
struct Dials {
    float texture = 0, light = 0, colour = 0, key = 0;
    float part[2] = {0, 0};  // the dials of the two lighting-delta layers
};

constexpr float kLumFloor = 1e-3f;       // y = log2(lum + floor): black is not minus infinity
constexpr float kSkyBelow = 1e-5f;       // q under this is sky
constexpr float kEdgeStop = 0.04f;       // relative depth step at which a neighbour stops counting
constexpr float kLayerGain[3] = {0.5f, 0.4f, 0.25f};
constexpr float kSoftKnee = 0.6f;        // stops: detail larger than this gains less
constexpr float kLightSlope = 0.35f;
constexpr float kNearBlack = 0.000492f;  // 8/255 at gamma 2.2
constexpr float kPureWhite = 0.9957f;

inline float Sat(float v) { return v < 0 ? 0.f : v > 1 ? 1.f : v; }  // a NaN comes out as itself
inline float Finite(float v) { return std::isfinite(v) ? v : 0.f; }
inline float Lum(float r, float g, float b)
{
    const float l = r * 0.2126f + g * 0.7152f + b * 0.0722f;
    return l > 0 ? l : 0.f;
}
inline float LogLum(float lum) { return std::log2(lum + kLumFloor); }

// Exact piecewise sRGB, for 8-bit pictures (they are stored encoded).
inline float SrgbDecode(float v)
{
    if (!(v > 0)) return 0.f;
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}
inline float SrgbEncode(float v)
{
    v = Sat(Finite(v));
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f;
}

inline float Shown(const Curve& c, float y)
{
    if (c.mode == 0) return Sat(std::exp2(y) - kLumFloor);
    if (c.mode == 1) {
        float t = (y - c.x0) / c.dx;
        t = t < 0 ? 0.f : t > 15.f ? 15.f : t;
        const int i = (std::min)(int(t), 14);
        const float f = t - float(i);
        return c.cy[i] + (c.cy[i + 1] - c.cy[i]) * f;
    }
    const float t = 0.18f * std::exp2(y - c.mean_y);
    return t / (1.f + t);
}

// Depth key: proportional to one over distance, whatever the camera.
inline float DepthKey(float d, bool reversed)
{
    const float q = reversed ? d : 1.f - d;
    return std::isfinite(q) && q > 0 ? q : 0.f;
}
inline float EdgeWeight(float qa, float qb)
{
    const float rel = 2.f * (qa - qb) / (qa + qb + 1e-6f);
    const float s = rel / kEdgeStop;
    return std::exp(-0.5f * s * s);
}

// One pixel as the passes see it: linear, with channels that are not numbers set to zero.
inline void LoadLinear(const float* px, bool srgb, float rgb[3])
{
    for (int c = 0; c < 3; ++c) rgb[c] = srgb ? SrgbDecode(Finite(px[c])) : Finite(px[c]);
}

// Pass 1. yq: w*h*2, (y, q) per pixel.
inline void Prep(const float* rgba, const float* depth, uint32_t w, uint32_t h, bool reversed, float* yq, bool srgb = false)
{
    const size_t n = size_t(w) * h;
    for (size_t i = 0; i < n; ++i) {
        float rgb[3];
        LoadLinear(rgba + i * 4, srgb, rgb);
        yq[i * 2] = LogLum(Lum(rgb[0], rgb[1], rgb[2]));
        yq[i * 2 + 1] = DepthKey(depth[i], reversed);
    }
}

// Passes 2, 4, 6: one direction of a blur that does not cross depth steps. `p` has `p_stride`
// floats per pixel with the value first (a yq texture is its own p with stride 2); q comes from
// `yq`. Offsets are step, 2*step, .. radius.
inline void Blur1D(const float* p, uint32_t p_stride, const float* yq, uint32_t w, uint32_t h,
                   int radius, int step, bool vertical, float* out)
{
    float spatial[16] = {};
    const int taps = (std::min)(radius / step, 16);
    for (int k = 0; k < taps; ++k) {
        const float s = float((k + 1) * step) / (0.5f * float(radius));
        spatial[k] = std::exp(-0.5f * s * s);
    }
    const int iw = int(w), ih = int(h);
    for (int y = 0; y < ih; ++y)
        for (int x = 0; x < iw; ++x) {
            const size_t c = size_t(y) * w + size_t(x);
            const float qc = yq[c * 2 + 1];
            float num = p[c * p_stride], den = 1.f;
            for (int k = 0; k < taps; ++k) {
                const int o = (k + 1) * step;
                for (int side = -1; side <= 1; side += 2) {
                    const int nx = vertical ? x : (std::clamp)(x + side * o, 0, iw - 1);
                    const int ny = vertical ? (std::clamp)(y + side * o, 0, ih - 1) : y;
                    const size_t ni = size_t(ny) * w + size_t(nx);
                    const float wt = spatial[k] * EdgeWeight(yq[ni * 2 + 1], qc);
                    num += wt * p[ni * p_stride];
                    den += wt;
                }
            }
            out[c] = num / den;
        }
}
inline void BlurX(const float* yq, uint32_t w, uint32_t h, int radius, int step, float* out)
{
    Blur1D(yq, 2, yq, w, h, radius, step, false, out);
}
inline void BlurY(const float* blurred_x, const float* yq, uint32_t w, uint32_t h, int radius, int step, float* out)
{
    Blur1D(blurred_x, 1, yq, w, h, radius, step, true, out);
}

constexpr uint32_t HalfSize(uint32_t v) { return (v + 1) / 2; }

// Passes 3 and 5: halve a blurred level, keeping the nearest surface's depth. `b` is w*h,
// `yq` the (.., q) the blur ran on; yq_out is HalfSize(w)*HalfSize(h)*2.
inline void Down(const float* b, const float* yq, uint32_t w, uint32_t h, float* yq_out)
{
    const uint32_t hw = HalfSize(w), hh = HalfSize(h);
    for (uint32_t y = 0; y < hh; ++y)
        for (uint32_t x = 0; x < hw; ++x) {
            size_t at[4];
            float q_near = 0;
            for (uint32_t k = 0; k < 4; ++k) {
                const uint32_t sx = (std::min)(x * 2 + (k & 1), w - 1), sy = (std::min)(y * 2 + (k >> 1), h - 1);
                at[k] = size_t(sy) * w + sx;
                q_near = (std::max)(q_near, yq[at[k] * 2 + 1]);
            }
            float num = 0, den = 0;
            for (uint32_t k = 0; k < 4; ++k) {
                const float wt = EdgeWeight(yq[at[k] * 2 + 1], q_near);
                num += wt * b[at[k]];
                den += wt;
            }
            const size_t o = (size_t(y) * hw + x) * 2;
            yq_out[o] = num / den;
            yq_out[o + 1] = q_near;
        }
}

// Depth-aware upsampling of a coarse level (section 2.2). `v` is cw*ch, `yq_coarse` carries its q.
inline float Up(const float* v, const float* yq_coarse, uint32_t cw, uint32_t ch, float factor, uint32_t x, uint32_t y, float q)
{
    const float tx = (float(x) + 0.5f) / factor - 0.5f, ty = (float(y) + 0.5f) / factor - 0.5f;
    const float fx0 = std::floor(tx), fy0 = std::floor(ty);
    const float fx = tx - fx0, fy = ty - fy0;
    const int ix = int(fx0), iy = int(fy0);
    float num = 0, den = 0;
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            const int cx = (std::clamp)(ix + i, 0, int(cw) - 1), cy = (std::clamp)(iy + j, 0, int(ch) - 1);
            const size_t ci = size_t(cy) * cw + size_t(cx);
            const float bilinear = (i ? fx : 1.f - fx) * (j ? fy : 1.f - fy);
            const float wt = bilinear * (EdgeWeight(yq_coarse[ci * 2 + 1], q) + 1e-3f);
            num += wt * v[ci];
            den += wt;
        }
    return num / den;
}

// Everything the first six passes make. None of it depends on a dial, so a tool that tries many
// dial values on one picture builds it once.
struct Pyramid {
    uint32_t w = 0, h = 0, w2 = 0, h2 = 0, w4 = 0, h4 = 0;
    std::vector<float> yq;         // pass 1, w*h*2
    std::vector<float> b1x, b1;    // pass 2 after x, after y; w*h
    std::vector<float> yq2;        // pass 3, w2*h2*2
    std::vector<float> b2x, b2;    // pass 4
    std::vector<float> yq4;        // pass 5, w4*h4*2
    std::vector<float> b3x, b3;    // pass 6
};
inline void BuildPyramid(const float* rgba, const float* depth, uint32_t w, uint32_t h, bool reversed, Pyramid& p, bool srgb = false)
{
    p.w = w; p.h = h; p.w2 = HalfSize(w); p.h2 = HalfSize(h); p.w4 = HalfSize(p.w2); p.h4 = HalfSize(p.h2);
    const size_t n1 = size_t(w) * h, n2 = size_t(p.w2) * p.h2, n4 = size_t(p.w4) * p.h4;
    p.yq.resize(n1 * 2); p.b1x.resize(n1); p.b1.resize(n1);
    p.yq2.resize(n2 * 2); p.b2x.resize(n2); p.b2.resize(n2);
    p.yq4.resize(n4 * 2); p.b3x.resize(n4); p.b3.resize(n4);
    Prep(rgba, depth, w, h, reversed, p.yq.data(), srgb);
    BlurX(p.yq.data(), w, h, 3, 1, p.b1x.data());
    BlurY(p.b1x.data(), p.yq.data(), w, h, 3, 1, p.b1.data());
    Down(p.b1.data(), p.yq.data(), w, h, p.yq2.data());
    BlurX(p.yq2.data(), p.w2, p.h2, 6, 1, p.b2x.data());
    BlurY(p.b2x.data(), p.yq2.data(), p.w2, p.h2, 6, 1, p.b2.data());
    Down(p.b2.data(), p.yq2.data(), p.w2, p.h2, p.yq4.data());
    BlurX(p.yq4.data(), p.w4, p.h4, 12, 2, p.b3x.data());
    BlurY(p.b3x.data(), p.yq4.data(), p.w4, p.h4, 12, 2, p.b3.data());
}

// Pass 7. rgba and out are w*h*4; with srgb they hold stored (encoded) 0..1 values. A pixel that
// nothing changes is written back bit for bit, and so is every alpha.
inline void Compose(const float* rgba, const Pyramid& p, const Dials& dials, const Scene& scene,
                    const float* delta_a, const float* delta_b, float* out, bool srgb = false)
{
    const uint32_t w = p.w, h = p.h;
    const float ref[3] = {(1.f - scene.w_dark) + scene.w_dark * scene.tint[0],
                          (1.f - scene.w_dark) + scene.w_dark * scene.tint[1],
                          (1.f - scene.w_dark) + scene.w_dark * scene.tint[2]};
    const float black_top = scene.m0 / 16.f, black_span = scene.m0 / 16.f - scene.m0 / 64.f;
    const float key_gain = std::exp2(dials.key);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const size_t i = size_t(y) * w + x;
            const float* in = rgba + i * 4;
            float* o = out + i * 4;
            const float yv = p.yq[i * 2], q = p.yq[i * 2 + 1];
            const float b1 = p.b1[i];
            const float u2 = Up(p.b2.data(), p.yq2.data(), p.w2, p.h2, 2.f, x, y, q);
            const float u3 = Up(p.b3.data(), p.yq4.data(), p.w4, p.h4, 4.f, x, y, q);
            const float layer[3] = {yv - b1, b1 - u2, u2 - u3};
            const float shown = Shown(scene.curve, yv);
            const float pool = scene.w_dark * 0.4f * Sat((shown - scene.p85) / (scene.p97 - scene.p85 + 1e-9f));
            float add = 0;
            for (int k = 0; k < 3; ++k) {
                const float d = layer[k];
                const float g = 1.f + kLayerGain[k] * dials.texture;
                const float s = std::fabs(d) / kSoftKnee;
                float e = d * (g - 1.f) / (1.f + s * s);
                if (d > 0) e *= 1.f + pool;
                add += e;
            }
            if (!(q < kSkyBelow)) add += -kLightSlope * dials.light * (u3 - scene.mid);
            const float da = delta_a ? Finite(delta_a[i]) : 0.f, db = delta_b ? Finite(delta_b[i]) : 0.f;
            add += da * dials.part[0] + db * dials.part[1];
            const float hi = Sat((shown - 0.55f) / 0.40f);
            const float lo = black_span > 0 ? Sat((black_top - shown) / black_span) : 0.f;  // m0 of zero: no black to protect
            add = add > 0 ? add * std::pow(1.f - hi, 1.5f) : add * (1.f - lo);
            if (add == 0 && dials.colour == 0 && dials.key == 0) {
                std::memcpy(o, in, 4 * sizeof(float));
                continue;
            }
            float rgb[3];
            LoadLinear(in, srgb, rgb);
            const float gain = std::exp2(add);
            for (int c = 0; c < 3; ++c) rgb[c] *= gain;
            if (dials.colour > 0) {
                const float yl = Lum(rgb[0], rgb[1], rgb[2]);
                float r[3], dist2 = 0;
                for (int c = 0; c < 3; ++c) {
                    r[c] = yl * ref[c];
                    const float t = (rgb[c] - r[c]) / (yl + 1e-5f);
                    dist2 += t * t;
                }
                const float grow = 1.f + 0.6f * dials.colour * (1.f - Sat(std::sqrt(dist2) / 1.2f));
                for (int c = 0; c < 3; ++c) rgb[c] = (std::max)(r[c] + grow * (rgb[c] - r[c]), 0.f);
            }
            for (int c = 0; c < 3; ++c) {
                rgb[c] *= key_gain;
                o[c] = srgb ? SrgbEncode(rgb[c]) : rgb[c];
            }
            std::memcpy(o + 3, in + 3, sizeof(float));
        }
}

inline bool AllZero(const Dials& d, const float* delta_a, const float* delta_b)
{
    return d.texture == 0 && d.light == 0 && d.colour == 0 && d.key == 0 &&
           (!delta_a || d.part[0] == 0) && (!delta_b || d.part[1] == 0);
}

// The whole enhancer on a linear picture. rgba: w*h*4 linear floats; depth: w*h raw device depth;
// delta_a/delta_b (w*h, stops) may be null; out: w*h*4.
inline void Enhance(const float* rgba, const float* depth, uint32_t w, uint32_t h, bool reversed, const Dials& dials,
                    const Scene& scene, const float* delta_a, const float* delta_b, float* out)
{
    if (AllZero(dials, delta_a, delta_b)) {  // nothing to do: no pass runs
        std::memcpy(out, rgba, size_t(w) * h * 4 * sizeof(float));
        return;
    }
    Pyramid p;
    BuildPyramid(rgba, depth, w, h, reversed, p);
    Compose(rgba, p, dials, scene, delta_a, delta_b, out);
}
// The same on an 8-bit sRGB picture: takes and returns stored 0..1 values. Use a Scene whose
// curve has mode 0. A pixel that is not changed comes back bit for bit, with no decode and encode.
inline void EnhanceSrgb8(const float* stored, const float* depth, uint32_t w, uint32_t h, bool reversed, const Dials& dials,
                         const Scene& scene, const float* delta_a, const float* delta_b, float* out)
{
    if (AllZero(dials, delta_a, delta_b)) {
        std::memcpy(out, stored, size_t(w) * h * 4 * sizeof(float));
        return;
    }
    Pyramid p;
    BuildPyramid(stored, depth, w, h, reversed, p, true);
    Compose(stored, p, dials, scene, delta_a, delta_b, out, true);
}

// Cell columns (section 3).
enum Cell : uint32_t {
    kCount, kYBefore, kYAfter, kShownBefore, kShownAfter, kBlackBefore, kBlackAfter, kWhiteBefore, kWhiteAfter,
    kSolid, kSolidY, kSolidR, kSolidG, kSolidB, kStrengthBefore, kStrengthAfter, kVarietyBefore, kVarietyAfter,
    kFineBefore, kFineBefore2, kFineAfter, kFineAfter2, kBand, kBandGlow
};
static_assert(kBandGlow + 1 == kCellFloats);

// Pass 8: the picture before and after NA1 reduced to kCellsW x kCellsH cells of kCellFloats sums.
// `key` is the key dial the after picture was made with (an even shift is not edge glow).
// cells: kCellsW*kCellsH*kCellFloats floats.
inline void Measure(const float* before, const float* after, const float* depth, uint32_t w, uint32_t h, bool reversed,
                    const Scene& scene, float key, float* cells, bool srgb = false)
{
    const float ref[3] = {(1.f - scene.w_dark) + scene.w_dark * scene.tint[0],
                          (1.f - scene.w_dark) + scene.w_dark * scene.tint[1],
                          (1.f - scene.w_dark) + scene.w_dark * scene.tint[2]};
    const int iw = int(w), ih = int(h);
    auto load = [&](const float* picture, int x, int y, float rgb[3]) {
        LoadLinear(picture + (size_t(y) * w + size_t(x)) * 4, srgb, rgb);
        for (int c = 0; c < 3; ++c) rgb[c] = (std::max)(rgb[c], 0.f);
    };
    auto log_at = [&](const float* picture, int x, int y) {
        float rgb[3];
        load(picture, (std::clamp)(x, 0, iw - 1), (std::clamp)(y, 0, ih - 1), rgb);
        return LogLum(Lum(rgb[0], rgb[1], rgb[2]));
    };
    auto solid_at = [&](int x, int y) {
        const size_t i = size_t((std::clamp)(y, 0, ih - 1)) * w + size_t((std::clamp)(x, 0, iw - 1));
        return !(DepthKey(depth[i], reversed) < kSkyBelow);
    };
    auto ring = [&](int x, int y, int r) {
        return solid_at(x - r, y) || solid_at(x + r, y) || solid_at(x, y - r) || solid_at(x, y + r);
    };
    for (uint32_t cy = 0; cy < kCellsH; ++cy)
        for (uint32_t cx = 0; cx < kCellsW; ++cx) {
            float s[kCellFloats] = {};
            const int x0 = int(uint64_t(cx) * w / kCellsW), x1 = int(uint64_t(cx + 1) * w / kCellsW);
            const int y0 = int(uint64_t(cy) * h / kCellsH), y1 = int(uint64_t(cy + 1) * h / kCellsH);
            for (int y = y0; y < y1; y += 2)
                for (int x = x0; x < x1; x += 2) {
                    float b[3], a[3];
                    load(before, x, y, b);
                    load(after, x, y, a);
                    const float lum_b = Lum(b[0], b[1], b[2]), lum_a = Lum(a[0], a[1], a[2]);
                    const float y_b = LogLum(lum_b), y_a = LogLum(lum_a);
                    const float shown_b = Shown(scene.curve, y_b), shown_a = Shown(scene.curve, y_a);
                    const float max_b = (std::max)(b[0], (std::max)(b[1], b[2])), min_b = (std::min)(b[0], (std::min)(b[1], b[2]));
                    const float max_a = (std::max)(a[0], (std::max)(a[1], a[2])), min_a = (std::min)(a[0], (std::min)(a[1], a[2]));
                    const float disp_b = max_b * (shown_b + 1e-5f) / (lum_b + 1e-3f);
                    const float disp_a = max_a * (shown_a + 1e-5f) / (lum_a + 1e-3f);
                    s[kCount] += 1.f;
                    s[kYBefore] += y_b;
                    s[kYAfter] += y_a;
                    s[kShownBefore] += shown_b;
                    s[kShownAfter] += shown_a;
                    if (disp_b <= kNearBlack) s[kBlackBefore] += 1.f;
                    if (disp_a <= kNearBlack) s[kBlackAfter] += 1.f;
                    if (disp_b >= kPureWhite) s[kWhiteBefore] += 1.f;
                    if (disp_a >= kPureWhite) s[kWhiteAfter] += 1.f;
                    if (solid_at(x, y)) {
                        s[kSolid] += 1.f;
                        s[kSolidY] += y_b;
                        s[kSolidR] += b[0];
                        s[kSolidG] += b[1];
                        s[kSolidB] += b[2];
                        s[kStrengthBefore] += (max_b - min_b) / (max_b + 1e-4f);
                        s[kStrengthAfter] += (max_a - min_a) / (max_a + 1e-4f);
                        float vb = 0, va = 0;
                        for (int c = 0; c < 3; ++c) {
                            const float tb = b[c] / (lum_b + 1e-5f) - ref[c], ta = a[c] / (lum_a + 1e-5f) - ref[c];
                            vb += tb * tb;
                            va += ta * ta;
                        }
                        s[kVarietyBefore] += std::sqrt(vb);
                        s[kVarietyAfter] += std::sqrt(va);
                        const float fine_b = y_b - 0.25f * (log_at(before, x - 2, y) + log_at(before, x + 2, y) +
                                                            log_at(before, x, y - 2) + log_at(before, x, y + 2));
                        const float fine_a = y_a - 0.25f * (log_at(after, x - 2, y) + log_at(after, x + 2, y) +
                                                            log_at(after, x, y - 2) + log_at(after, x, y + 2));
                        s[kFineBefore] += fine_b;
                        s[kFineBefore2] += fine_b * fine_b;
                        s[kFineAfter] += fine_a;
                        s[kFineAfter2] += fine_a * fine_a;
                    } else if (ring(x, y, 8) && !ring(x, y, 3)) {
                        s[kBand] += 1.f;
                        s[kBandGlow] += std::fabs(y_a - y_b - key);
                    }
                }
            std::memcpy(cells + (size_t(cy) * kCellsW + cx) * kCellFloats, s, sizeof s);
        }
}

}  // namespace na::na1::ref
