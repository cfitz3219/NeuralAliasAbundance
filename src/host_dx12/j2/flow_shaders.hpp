#pragma once
// AMD-path scene encode / image-consistency acceptance. Kept in step with j2_flow.cpp's kShader
// (the Anno-run gates: jitter-stabilised encode fe8ea3e, temporal agreement 5c5fb0d/319e2ae,
// ratio image check 8ca89a6, snap 0.5 px aa798b6), with AMD's whole-frame scene-change
// rejection (has_cost 2). NeuralAlias code, not a new optical-flow solver.
//
// `measure`, `cut_state` and `cut_show` are the camera-free path (2026-11-16): the pair's TOTAL
// motion measured on the two pictures, and what a scene cut does to the picture shown. `accept`
// and its gates are the residual path against a camera and are not changed by them.
// `fit_sample`, `fit_accum` and `fit_solve` (2026-11-17) fit the screen's own motion for `measure`
// to follow where the pictures cannot measure (core/screen_motion.hpp, VARIANT motion/screen-fit).
namespace na::j2 {
inline const char kFlowShader[] = R"(
Texture2D<float4> t_color : register(t0);
Texture2D<float2> t_raw   : register(t1);
Texture2D<float2> t_res   : register(t2);
Texture2D<float>  t_valid : register(t3);
Texture2D<uint>   t_cost  : register(t4);
Texture2D<float4> t_cur   : register(t5);
Texture2D<float4> t_prev  : register(t6);
Texture2D<float4> t_hist  : register(t7); // previous pair: xy residual candidate, z agreeing pairs so far
RWTexture2D<float4> u_enc : register(u0);
RWTexture2D<float>  u_mask : register(u1);
RWByteAddressBuffer u_cnt : register(u2);
RWTexture2D<float4> u_hist : register(u3);
SamplerState s_lin : register(s0);
cbuffer C : register(b0) {
    uint w, h, ew, eh;
    uint mode, block, has_cost, cost_max;
    uint cnt_off, gw, gh; float image_ratio;
    float snap_px, image_margin, image_abs, texture_min;
    uint have_hist, agree_min; float dj_x, dj_y; // dj: this pair's stabilisation jitter delta (current - previous)
    float agree_px, agree_cos, jitter_x, jitter_y;
    float history_dx,history_dy; uint frame_now, frame_reset; // cut_state: this frame; the newest frame submitted with a DLSS reset
};
float3 srgb_encode(float3 v) { v = saturate(v); return v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1.0 / 2.4) - 0.055; }
// One fixed transform for every image (both images of a pair are encoded alike), sampled
// at the frame's jitter offset so the encoded image is in the unjittered frame: texel
// (id + 0.5) covers full-size position q = (id + 0.5) * w / ew, found at texel q + jitter.
[numthreads(8, 8, 1)]
void encode(uint3 id : SV_DispatchThreadID) {
    if (id.x >= ew || id.y >= eh) return;
    float2 q = (float2(id.xy) + 0.5) * float2(w, h) / float2(ew, eh);
    float2 uv = (q + float2(jitter_x, jitter_y)) / float2(w, h);
    float3 c = t_color.SampleLevel(s_lin, uv, 0).rgb;
    if (any(isnan(c)) || any(isinf(c))) c = float3(0, 0, 0);
    if (mode == 1) { c = max(c, 0.0); c = srgb_encode(c / (1.0 + c)); }
    else c = saturate(c);
    u_enc[id.xy] = float4(c, 1.0); // UAV format B8G8R8A8_UNORM: the view swizzles
    // The same fixed colour transform without an assumed jitter shift. The
    // source calibration needs these exact raw-grid images, not two images
    // that have already been resampled using the convention it is testing.
    float3 raw_color = t_color.SampleLevel(s_lin, q / float2(w, h), 0).rgb;
    if (any(isnan(raw_color)) || any(isinf(raw_color))) raw_color = 0;
    if (mode == 1) { raw_color = max(raw_color, 0.0); raw_color = srgb_encode(raw_color / (1.0 + raw_color)); }
    else raw_color = saturate(raw_color);
    u_hist[id.xy] = float4(raw_color, 1.0); // encode pass's private raw-colour UAV
}
float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
// Mean luma difference over 3x3 encoded texels between the current image at p
// and the previous image at p + v (v = p_previous - p_current, full-size pixels).
float image_error(float2 p, float2 v) {
    float step_x = float(w) / float(ew), step_y = float(h) / float(eh);
    float e = 0;
    [unroll] for (int j = -1; j <= 1; ++j) {
        [unroll] for (int i = -1; i <= 1; ++i) {
            float2 o = float2(i * step_x, j * step_y);
            float2 uc = (p + o) / float2(w, h);
            float2 up = (p + o + v) / float2(w, h);
            e += abs(luma(t_cur.SampleLevel(s_lin, uc, 0).rgb) - luma(t_prev.SampleLevel(s_lin, up, 0).rgb));
        }
    }
    return e / 9.0;
}
// Local contrast of the current image around p (same 3x3 encoded texels).
float texture_at(float2 p) {
    float step_x = float(w) / float(ew), step_y = float(h) / float(eh);
    float lo = 1e9, hi = -1e9;
    [unroll] for (int j = -1; j <= 1; ++j) {
        [unroll] for (int i = -1; i <= 1; ++i) {
            float l = luma(t_cur.SampleLevel(s_lin, (p + float2(i * step_x, j * step_y)) / float2(w, h), 0).rgb);
            lo = min(lo, l); hi = max(hi, l);
        }
    }
    return hi - lo;
}
groupshared uint gs[10];
[numthreads(8, 8, 1)]
void accept(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    if (gi < 10) gs[gi] = 0;
    GroupMemoryBarrierWithGroupSync();
    if (id.x < w && id.y < h) {
        uint why = 0; // 0 accepted, 1 invalid, 2 small, 3 cost, 4 offscreen, 5 image (or flat), 6 flicker (no agreement yet)
        float streak = 0;
        uint cost = 0;
        bool costed = false;
        float v = t_valid[id.xy];
        float2 r = t_res[id.xy];
        float2 raw = t_raw[id.xy];
        if (!(v > 0.0 && v <= 1.0) || any(isnan(r)) || any(isnan(raw)) || any(isinf(r)) || any(isinf(raw))) why = 1;
        else if (length(r) < snap_px) why = 2;
        // AMD scene-change output shares the uint input, with a distinct mode (has_cost 2).
        if (has_cost == 2 && (t_cost[int2(1, 0)] & 15u) != 0) why = 1;
        if (why == 0 && has_cost == 1) {
            uint2 b = min(id.xy / block, uint2(gw - 1, gh - 1));
            cost = t_cost[b];
            costed = true;
            if (cost > cost_max) why = 3;
        }
        if (why == 0) {
            float2 p = float2(id.xy) + 0.5 - float2(jitter_x,jitter_y);
            float2 q = p + raw;
            if (q.x < 0.0 || q.y < 0.0 || q.x > float(w) || q.y > float(h)) why = 4;
            else if (texture_at(p) < texture_min) why = 5; // flat: the image cannot confirm a residual
            else {
                float e_flow = image_error(p, raw);
                // Camera alone, under both jitter conventions: when the colour carries no jitter,
                // the stabilised pair is shifted by dj and that shift is not object motion.
                float e_cam = min(image_error(p, raw - r), image_error(p, raw - r + float2(dj_x, dj_y)));
                // Camera wins an ambiguous match. The old absolute .02 allowance
                // admitted nonzero motion even when camera-only was exact (WWII
                // still timber). Requiring actual improvement preserves real slow
                // motion without imposing a larger displacement cutoff. At the
                // loosest ratio (1), equal matches still cannot invent movement.
                if (!(e_flow <= image_abs && e_flow < e_cam && e_flow <= image_ratio * e_cam)) why = 5;
            }
        }
        if (why == 0) {
            // Temporal agreement: the previous pair's candidate where this point came from.
            if (have_hist != 0) {
                float2 from = float2(id.xy) + 0.5 + raw + float2(history_dx,history_dy);
                float4 hp = t_hist.Load(int3(clamp(int2(floor(from)), int2(0, 0), int2(int(w) - 1, int(h) - 1)), 0));
                float2 hr = hp.xy;
                float lr = length(r), lh = length(hr);
                bool agree = all(isfinite(hr)) && lh >= snap_px && dot(r, hr) > agree_cos * lr * lh &&
                             length(r - hr) <= max(agree_px, lr);
                streak = agree ? hp.z + 1.0 : 0.0;
            }
            if (streak < float(agree_min)) why = 6;
        }
        u_hist[id.xy] = why == 0 || why == 6 ? float4(r, streak, 0.0) : float4(0, 0, 0, 0);
        u_mask[id.xy] = why == 0 ? 1.0 : 0.0;
        InterlockedAdd(gs[why], 1u);
        if (costed) { InterlockedAdd(gs[8], cost); InterlockedAdd(gs[9], 1u); }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi < 10 && gs[gi] != 0) u_cnt.InterlockedAdd(cnt_off + gi * 4, gs[gi]);
}

)" R"(
// ---- Measured total motion, no camera. core/total_motion.hpp is this program on the CPU (the
// fixtures in tests/cpu/total_motion_test.cpp run on it); the two are kept in step by hand: same
// samples, same order of decisions, same constants. None of the residual gates above are used and
// the looseness slider does not reach it.
//  - The block search's whole-pixel vector, zero included, is a hypothesis. Each hypothesis is
//    refined below a pixel on the pictures, and "nothing moved" is always one of them.
//  - No small-motion cutoff, no temporal rule.
//  - Where the pictures cannot measure (flat, or no hypothesis explains them) the pixel follows the
//    screen's own motion (the model fit_solve wrote), and the search's raw vector is never handed
//    on. With a model, IT is the hypothesis "nothing moved by itself" in place of exact stillness.
//  - Validity written: 1 measured (with the screen, or moving by itself), 0.5 the screen's own
//    motion where the pictures could not measure, 0 nothing known (no model to follow).
static const float kTotalImageAbs = 0.08, kTotalImageRel = 0.15, kTotalTextureMin = 0.03;
static const float kTotalStillRatio = 0.5, kTotalBound = 1.0, kTotalDamp = 2e-5, kTotalEdge = 0.05;
static const float kTotalApproximate = 0.5;
float cur_luma(float2 q) { return luma(t_cur.SampleLevel(s_lin, q / float2(w, h), 0).rgb); }
float prev_luma(float2 q) { return luma(t_prev.SampleLevel(s_lin, q / float2(w, h), 0).rgb); }
// The current picture around p: nine places two pixels apart, each one's gradient (central
// differences one pixel either side), the structure sums and the local contrast. 33 reads.
struct Window { float lc[9]; float2 g[9]; float3 a; float contrast; };
Window window_at(float2 p) {
    Window win;
    win.a = 0;
    float side[12], updown[12];
    [unroll] for (int sj = -1; sj <= 1; ++sj) {
        [unroll] for (int sm = 0; sm < 4; ++sm) side[(sj + 1) * 4 + sm] = cur_luma(p + float2(2 * sm - 3, 2 * sj));
    }
    [unroll] for (int um = 0; um < 4; ++um) {
        [unroll] for (int ui = -1; ui <= 1; ++ui) updown[um * 3 + (ui + 1)] = cur_luma(p + float2(2 * ui, 2 * um - 3));
    }
    float lo = 1e9, hi = -1e9;
    [unroll] for (int wj = -1; wj <= 1; ++wj) {
        [unroll] for (int wi = -1; wi <= 1; ++wi) {
            int k = (wj + 1) * 3 + (wi + 1);
            float c = cur_luma(p + float2(2 * wi, 2 * wj));
            float2 g = 0.5 * float2(side[(wj + 1) * 4 + wi + 2] - side[(wj + 1) * 4 + wi + 1],
                                    updown[(wj + 2) * 3 + wi + 1] - updown[(wj + 1) * 3 + wi + 1]);
            win.lc[k] = c;
            win.g[k] = g;
            win.a += float3(g.x * g.x, g.x * g.y, g.y * g.y);
            lo = min(lo, c); hi = max(hi, c);
        }
    }
    win.contrast = hi - lo;
    return win;
}
// Bounded refinement of one hypothesis v0: two damped Gauss-Newton steps on the window with the
// current picture's gradients; on an edge the vector moves across the edge only. Returns the best
// vector met (v0 included) in xy, its mean |luma| error in z, and the error of v0 exactly in w.
float4 refine(float2 p, float2 v0, Window win) {
    float a11 = win.a.x + kTotalDamp, a22 = win.a.z + kTotalDamp, a12 = win.a.y;
    float det = a11 * a22 - a12 * a12;
    float mid = 0.5 * (a11 + a22);
    float spread = sqrt(max(0.0, 0.25 * (a11 - a22) * (a11 - a22) + a12 * a12));
    float l1 = mid + spread, l2 = mid - spread;
    float2 strong = float2(a12, l1 - a11);
    float len = length(strong);
    if (len < 1e-12) strong = float2(1, 0); else strong /= len;
    bool one_way = l2 < kTotalEdge * l1;
    float2 c = v0, best = v0;
    float e = 0, e0 = 0;
    [unroll] for (int it = 0; it <= 2; ++it) {
        float2 b = 0;
        float err = 0;
        [unroll] for (int rj = -1; rj <= 1; ++rj) {
            [unroll] for (int ri = -1; ri <= 1; ++ri) {
                int k = (rj + 1) * 3 + (ri + 1);
                float d = prev_luma(p + float2(2 * ri, 2 * rj) + c) - win.lc[k];
                b += win.g[k] * d;
                err += abs(d);
            }
        }
        err /= 9.0;
        if (it == 0) { e0 = err; e = err; }
        else if (err < e) { e = err; best = c; }
        if (it < 2) {
            // previous(x + v + d) ~ previous(x + v) + g.d  =>  d = -A^-1 b
            float2 d2 = one_way ? (-dot(strong, b) / l1) * strong
                                : -float2(a22 * b.x - a12 * b.y, a11 * b.y - a12 * b.x) / det;
            c = clamp(c + d2, v0 - kTotalBound, v0 + kTotalBound);
        }
    }
    return float4(best, e, e0);
}
// The vector that explains the window best, among the refined hypothesis `hyp` and the refined
// search vector (total_motion.hpp BestVector). False: the pictures cannot measure here (flat, or
// neither explains them). e_with: the error `hyp` leaves exactly; limit: what counts as explaining.
bool best_vector(float2 p, float2 hyp, bool raw_on, float2 raw, out float2 best, out float e_best, out float e_with, out float limit) {
    best = hyp; e_best = 0; e_with = 0; limit = 0;
    Window win = window_at(p);
    bool textured = win.contrast >= kTotalTextureMin;
    if (textured) {
        float4 z = refine(p, hyp, win);
        best = z.xy; e_best = z.z; e_with = z.w;
        if (raw_on && any(raw != hyp)) {
            float4 r = refine(p, raw, win);
            if (r.z <= z.z) { best = r.xy; e_best = r.z; }
        }
        // Explaining the picture: a small error in itself and small beside the window's own contrast.
        limit = min(kTotalImageAbs, kTotalImageRel * win.contrast);
    }
    return textured && e_best <= limit;
}
)" R"(
// ---- The screen's own motion. core/screen_motion.hpp is these programs on the CPU (and says why
// they exist and what the eleven terms are); the two are kept in step by hand: same samples, same
// terms, same weights, same order of decisions, same constants.
// VARIANT: motion/screen-fit
// The fit's programs and `measure` read three of the residual path's constants under other names
// (their own meaning is not used by them, and the block stays the 28 words every user binds):
#define depth_far history_dx  // the depth value at the far end: 0 for reversed depth, 1 otherwise
#define fit_scale history_dy  // fit_accum: this step's weight scale in pixels; 0 = the plain first step
//      mode                     fit_solve: 0 a solving step, 1 the final evaluation
static const float kFitRobust = 1.5, kFitMinSamples = 256.0, kFitMinAgree = 0.5, kFitMaxVector = 512.0, kFitDamping = 1e-4;
#define FIT_TERMS 11
#define FIT_PAIRS 66   // the normal matrix's upper triangle, row by row
#define FIT_COUNT 77   // confirmed samples
#define FIT_ENERGY 78  // their robust error at the current terms
#define FIT_INLIERS 79 // those within kFitRobust of the model
#define FIT_NEAREST 80     // the largest depth distance among those (a maximum, not a sum)
#define FIT_VALUES 81
// The fit's buffer (u_cnt in fit_accum and fit_solve), in words: 0..10 the terms, 11 nearness_limit,
// 12 valid, 13 confirmed samples, 14 those agreeing, 15 their mean robust error, 16 the fit failed
// (too few samples, or a step could not be solved), 17 steps solved; from FIT_HEAD each thread
// group's FIT_VALUES partial sums.
#define FIT_HEAD 20
bool fin(float a) { return (asuint(a) & 0x7fffffffu) < 0x7f800000u; }
// A depth value's distance from the far end; 0 for a value that is not a depth.
float nearness_of(float d) { return fin(d) && d >= 0.0 && d <= 1.0 ? abs(d - depth_far) : 0.0; }
// The two rows of one position's terms (screen_motion.hpp Terms).
void fit_terms(float2 p, float nearness, out float bu[FIT_TERMS], out float bv[FIT_TERMS]) {
    float sc = 2.0 / float(h);
    float X = (p.x - 0.5 * float(w)) * sc, Y = (p.y - 0.5 * float(h)) * sc;
    bu[0] = 1; bu[1] = X; bu[2] = Y; bu[3] = 0; bu[4] = 0; bu[5] = 0; bu[6] = X * X; bu[7] = X * Y; bu[8] = nearness; bu[9] = 0;   bu[10] = nearness * X;
    bv[0] = 0; bv[1] = 0; bv[2] = 0; bv[3] = 1; bv[4] = X; bv[5] = Y; bv[6] = X * Y; bv[7] = Y * Y; bv[8] = 0;   bv[9] = nearness; bv[10] = nearness * Y;
}
// One sample per 8x8 block, at its centre: the best vector itself, with no preference for
// stillness (total_motion.hpp SamplePixel). t_color is bound as the depth; u_enc as the fit's grid
// (RGBA32F, one texel per block): xy the vector, z the depth's distance from its far end, w 1 when
// the pictures confirm the vector.
[numthreads(8, 8, 1)]
void fit_sample(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gw || id.y >= gh) return;
    uint2 c = id.xy * block + block / 2;
    float4 o = 0;
    bool cut = has_cost == 2 && (t_cost[int2(1, 0)] & 15u) != 0; // a scene change: no vector of this pair means anything
    if (!cut && c.x < w && c.y < h) {
        float ev = t_valid[c];
        float2 raw = t_raw[c];
        bool have_raw = ev > 0.0 && ev <= 1.0 && !any(isnan(raw)) && !any(isinf(raw));
        float2 p = float2(c) + 0.5 - float2(jitter_x, jitter_y);
        float2 q = p + raw;
        bool raw_on = have_raw && q.x >= 0.0 && q.y >= 0.0 && q.x <= float(w) && q.y <= float(h);
        float2 best; float e_best, e_with, limit;
        bool confirmed = best_vector(p, float2(0, 0), raw_on, raw, best, e_best, e_with, limit);
        o = float4(best, nearness_of(t_color[c].x), confirmed ? 1.0 : 0.0);
    }
    u_enc[id.xy] = o;
}
// One pass over the samples at the buffer's terms (screen_motion.hpp Accumulate): 64 samples a
// thread group, their sums stored per group. t_hist is bound as the fit's grid.
groupshared float gf[64 * FIT_VALUES];
[numthreads(64, 1, 1)]
void fit_accum(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
    float v[FIT_VALUES];
    uint i;
    for (i = 0; i < FIT_VALUES; ++i) v[i] = 0;
    const uint s = gid.x * 64 + gi;
    if (s < gw * gh) {
        const uint2 cell = uint2(s % gw, s / gw);
        const float4 a = t_hist[cell];
        if (a.w > 0.5 && fin(a.x) && fin(a.y) && fin(a.z) && abs(a.x) < kFitMaxVector && abs(a.y) < kFitMaxVector) {
            const float2 p = float2(cell * block + block / 2) + 0.5 - float2(jitter_x, jitter_y);
            float bu[FIT_TERMS], bv[FIT_TERMS];
            fit_terms(p, a.z, bu, bv);
            float2 m = 0;
            [unroll] for (uint k = 0; k < FIT_TERMS; ++k) m += asfloat(u_cnt.Load(k * 4)) * float2(bu[k], bv[k]);
            const float2 r = a.xy - m;
            const float rr = dot(r, r), c2 = kFitRobust * kFitRobust;
            // 1 / (1 + r^2 / s^2)^2: its pull on the fit falls as 1 / r^3 (screen_motion.hpp).
            const float qw = fit_scale > 0.0 ? 1.0 / (1.0 + rr / (fit_scale * fit_scale)) : 1.0;
            const float wgt = qw * qw;
            uint idx = 0;
            [unroll] for (uint ia = 0; ia < FIT_TERMS; ++ia) {
                [unroll] for (uint ib = ia; ib < FIT_TERMS; ++ib) { v[idx] = wgt * (bu[ia] * bu[ib] + bv[ia] * bv[ib]); ++idx; }
            }
            // The right-hand side holds what the terms so far leave unexplained: each step solves for a correction.
            [unroll] for (uint ic = 0; ic < FIT_TERMS; ++ic) v[FIT_PAIRS + ic] = wgt * (bu[ic] * r.x + bv[ic] * r.y);
            v[FIT_COUNT] = 1.0;
            v[FIT_ENERGY] = 0.5 * rr / (1.0 + rr / c2);
            if (rr <= c2) { v[FIT_INLIERS] = 1.0; v[FIT_NEAREST] = a.z; }
        }
    }
    for (i = 0; i < FIT_VALUES; ++i) gf[gi * FIT_VALUES + i] = v[i];
    GroupMemoryBarrierWithGroupSync();
    for (uint st = 32; st > 0; st >>= 1) {
        if (gi < st) {
            for (i = 0; i < FIT_NEAREST; ++i) gf[gi * FIT_VALUES + i] += gf[(gi + st) * FIT_VALUES + i];
            gf[gi * FIT_VALUES + FIT_NEAREST] = max(gf[gi * FIT_VALUES + FIT_NEAREST], gf[(gi + st) * FIT_VALUES + FIT_NEAREST]);
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0) for (i = 0; i < FIT_VALUES; ++i) u_cnt.Store((FIT_HEAD + gid.x * FIT_VALUES + i) * 4, asuint(gf[i]));
}
)" R"(
// One thread group: the groups' sums reduced, then one step of the fit (screen_motion.hpp Solve:
// the scaled, damped normal equations by Cholesky, as a correction to the terms) or, with mode 1,
// the final evaluation (screen_motion.hpp Fit's last pass). u_enc is bound as the model `measure`
// reads (RG32F, 8 x 1): texels 0..4 the terms in pairs, 5 the last term and nearness_limit, 6 valid and
// the confirmed samples, 7 those agreeing and their mean robust error.
groupshared float gr[256];
groupshared float gt[FIT_VALUES];
[numthreads(256, 1, 1)]
void fit_solve(uint gi : SV_GroupIndex) {
    const uint groups = (gw * gh + 63) / 64;
    for (uint i = 0; i < FIT_VALUES; ++i) {
        float s = 0;
        for (uint g = gi; g < groups; g += 256) {
            const float x = asfloat(u_cnt.Load((FIT_HEAD + g * FIT_VALUES + i) * 4));
            s = i == FIT_NEAREST ? max(s, x) : s + x;
        }
        gr[gi] = s;
        GroupMemoryBarrierWithGroupSync();
        for (uint st = 128; st > 0; st >>= 1) {
            if (gi < st) gr[gi] = i == FIT_NEAREST ? max(gr[gi], gr[gi + st]) : gr[gi] + gr[gi + st];
            GroupMemoryBarrierWithGroupSync();
        }
        if (gi == 0) gt[i] = gr[0];
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi != 0) return;
    const float n = gt[FIT_COUNT];
    bool failed = u_cnt.Load(16 * 4) != 0u;
    if (mode == 0u) {
        if (failed) return;
        u_cnt.Store(13 * 4, asuint(n));
        bool ok = n >= kFitMinSamples;
        // Plain loops, not unrolled: one thread runs this once a step, and the program stays small to compile.
        float A[FIT_TERMS][FIT_TERMS], b[FIT_TERMS], sc[FIT_TERMS];
        uint idx = 0;
        [loop] for (uint a = 0; a < FIT_TERMS; ++a) {
            [loop] for (uint c = a; c < FIT_TERMS; ++c) { A[a][c] = gt[idx]; A[c][a] = gt[idx]; ++idx; }
        }
        [loop] for (uint a1 = 0; a1 < FIT_TERMS; ++a1) { b[a1] = gt[FIT_PAIRS + a1]; sc[a1] = A[a1][a1] > 1e-20 ? rsqrt(A[a1][a1]) : 0.0; }
        [loop] for (uint a2 = 0; a2 < FIT_TERMS; ++a2) {
            [loop] for (uint c2 = 0; c2 < FIT_TERMS; ++c2) A[a2][c2] *= sc[a2] * sc[c2];
            // A term no sample carries (every depth at the far end, say) keeps its value.
            A[a2][a2] = (sc[a2] > 0 ? A[a2][a2] : 0.0) + kFitDamping + (sc[a2] > 0 ? 0.0 : 1.0);
            b[a2] *= sc[a2];
        }
        float L[FIT_TERMS][FIT_TERMS];
        [loop] for (uint r0 = 0; r0 < FIT_TERMS; ++r0) { [loop] for (uint c3 = 0; c3 < FIT_TERMS; ++c3) L[r0][c3] = 0; }
        [loop] for (uint j = 0; j < FIT_TERMS; ++j) {
            float d = A[j][j];
            [loop] for (uint m = 0; m < j; ++m) d -= L[j][m] * L[j][m];
            if (!(d > 1e-12)) ok = false;
            L[j][j] = sqrt(max(d, 1e-12));
            [loop] for (uint i5 = j + 1; i5 < FIT_TERMS; ++i5) {
                float t = A[i5][j];
                [loop] for (uint m2 = 0; m2 < j; ++m2) t -= L[i5][m2] * L[j][m2];
                L[i5][j] = t / L[j][j];
            }
        }
        float yv[FIT_TERMS], xv[FIT_TERMS];
        [loop] for (uint i6 = 0; i6 < FIT_TERMS; ++i6) {
            float t = b[i6];
            [loop] for (uint m3 = 0; m3 < i6; ++m3) t -= L[i6][m3] * yv[m3];
            yv[i6] = t / L[i6][i6];
        }
        [loop] for (int i7 = FIT_TERMS - 1; i7 >= 0; --i7) {
            float t = yv[i7];
            [loop] for (int m4 = i7 + 1; m4 < FIT_TERMS; ++m4) t -= L[m4][i7] * xv[m4];
            xv[i7] = t / L[i7][i7];
        }
        float th[FIT_TERMS];
        [unroll] for (uint k = 0; k < FIT_TERMS; ++k) {
            const float delta = xv[k] * sc[k];
            if (!fin(delta)) ok = false;
            th[k] = asfloat(u_cnt.Load(k * 4)) + delta;
        }
        if (ok) {
            [unroll] for (uint k2 = 0; k2 < FIT_TERMS; ++k2) u_cnt.Store(k2 * 4, asuint(th[k2]));
            u_cnt.Store(17 * 4, u_cnt.Load(17 * 4) + 1u);
        } else {
            u_cnt.Store(16 * 4, 1u);
        }
        return;
    }
    // The fitted terms against the samples once more: how many agree, and how near the nearest of those is.
    const float agree = gt[FIT_INLIERS];
    const bool valid = !failed && n >= kFitMinSamples && agree >= kFitMinAgree * n;
    const float energy = n > 0.0 ? gt[FIT_ENERGY] / n : 0.0, nearness_limit = failed ? 0.0 : gt[FIT_NEAREST];
    u_cnt.Store(11 * 4, asuint(nearness_limit));
    u_cnt.Store(12 * 4, valid ? 1u : 0u);
    u_cnt.Store(13 * 4, asuint(n));
    u_cnt.Store(14 * 4, asuint(failed ? 0.0 : agree));
    u_cnt.Store(15 * 4, asuint(failed ? 0.0 : energy));
    float th2[FIT_TERMS + 1];
    [unroll] for (uint k3 = 0; k3 < FIT_TERMS; ++k3) th2[k3] = valid ? asfloat(u_cnt.Load(k3 * 4)) : 0.0;
    th2[FIT_TERMS] = valid ? nearness_limit : 0.0;
    [unroll] for (uint k4 = 0; k4 < 6; ++k4) u_enc[uint2(k4, 0)] = float4(th2[2 * k4], th2[2 * k4 + 1], 0, 0);
    u_enc[uint2(6, 0)] = float4(valid ? 1.0 : 0.0, n, 0, 0);
    u_enc[uint2(7, 0)] = float4(agree, energy, 0, 0);
}
// The model at a position (screen_motion.hpp Evaluate). t_res is bound as fit_solve's model; with
// nothing bound there every read is 0 and there is no model.
float2 screen_at(float2 p, float nearness) {
    float bu[FIT_TERMS], bv[FIT_TERMS];
    fit_terms(p, nearness, bu, bv);
    float2 v = 0;
    [unroll] for (uint k = 0; k < FIT_TERMS; ++k) {
        const float2 pr = t_res[int2(k / 2, 0)];
        v += ((k & 1u) != 0u ? pr.y : pr.x) * float2(bu[k], bv[k]);
    }
    return v;
}
)" R"(
// t_raw: the search's vector after the depth-aware expansion; t_valid: that expansion's validity;
// t_res: the screen's own motion (fit_solve's model), or nothing; t_color: the depth, read only
// with a model. u_enc is bound as the RG32F total field here, u_mask as its validity.
[numthreads(8, 8, 1)]
void measure(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    if (gi < 10) gs[gi] = 0;
    GroupMemoryBarrierWithGroupSync();
    if (id.x < w && id.y < h) {
        // 0 measured, moving by itself; 1 nothing known; 2 not measurable, follows the screen's
        // motion; 7 measured, moving with the screen (exactly still when there is no model).
        // 4 and 5 (the search's vector handed on) are no longer written. Slot 8: a scene cut.
        uint why = 1;
        float2 v = 0;
        float valid = 0;
        if (has_cost == 2 && (t_cost[int2(1, 0)] & 15u) != 0) {
            // AMD's whole-frame scene change: no vector of this pair means anything. The picture
            // shown and the DLSS reset are cut_state's and the caller's business, not this field's.
            if (id.x == 0 && id.y == 0) InterlockedAdd(gs[8], 1u);
        } else {
            float ev = t_valid[id.xy];
            float2 raw = t_raw[id.xy];
            bool have_raw = ev > 0.0 && ev <= 1.0 && !any(isnan(raw)) && !any(isinf(raw));
            float2 p = float2(id.xy) + 0.5 - float2(jitter_x, jitter_y);
            float2 q = p + raw;
            bool raw_on = have_raw && q.x >= 0.0 && q.y >= 0.0 && q.x <= float(w) && q.y <= float(h);
            // The hypothesis "nothing moved by itself": the screen's own motion here, never asked
            // about a depth nearer than its nearest agreeing sample; exact stillness with no model.
            bool have_screen = t_res[int2(6, 0)].x > 0.5;
            float2 hyp = 0;
            if (have_screen) hyp = screen_at(p, min(nearness_of(t_color[id.xy].x), t_res[int2(5, 0)].y));
            float2 best; float e_best, e_with, limit;
            if (best_vector(p, hyp, raw_on, raw, best, e_best, e_with, limit)) {
                valid = 1.0;
                // The hypothesis stands unless the pixel's own vector explains the picture clearly better.
                bool with_screen = all(best == hyp) || (e_with <= limit && !(e_best <= kTotalStillRatio * e_with));
                if (with_screen) { why = 7; v = hyp; }
                else { why = 0; v = best; }
            } else if (have_screen) { v = hyp; valid = kTotalApproximate; why = 2; }
        }
        u_enc[id.xy] = float4(v, 0, 0);
        u_mask[id.xy] = valid;
        InterlockedAdd(gs[why], 1u);
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi < 10 && gs[gi] != 0) u_cnt.InterlockedAdd(cnt_off + gi * 4, gs[gi]);
}
)" R"(
// ---- A scene cut with no camera (Astra's correction 2). DLSS is given vectors only, so a pair with
// no usable vector does not drop its history: only a reset does, and the cut is known here on the
// GPU first. Until the CPU has read it back and submitted that reset, the temporal output would
// still blend the old scene in, so those frames show their own current picture instead.
// u_cnt is the context's state buffer here, updated in execution order:
//   word 0  the frame of the newest pair flagged as a scene change     word 2  1 once there is one
//   word 1  1 when this frame shows its current picture instead of the temporal output
// frame_reset is the newest frame submitted with a DLSS reset, this one included when it carries it.
[numthreads(1, 1, 1)]
void cut_state(uint3 id : SV_DispatchThreadID) {
    uint flagged = u_cnt.Load(0), have = u_cnt.Load(8);
    if (has_cost == 2 && (t_cost[int2(1, 0)] & 15u) != 0) { flagged = frame_now; have = 1u; }
    bool pending = have != 0u && int(flagged - frame_reset) > 0;
    u_cnt.Store(0, flagged);
    u_cnt.Store(8, have);
    u_cnt.Store(4, pending ? 1u : 0u);
}
// After DLSS: t_color is the picture DLSS was given, u_enc its output, w x h the output's size
// (the same size under DLAA, where this is an exact copy; a plain enlargement otherwise).
[numthreads(8, 8, 1)]
void cut_show(uint3 id : SV_DispatchThreadID) {
    if (id.x >= w || id.y >= h || u_cnt.Load(4) == 0u) return;
    u_enc[id.xy] = t_color.SampleLevel(s_lin, (float2(id.xy) + 0.5) / float2(w, h), 0);
}
)";

// VARIANT: flow/frame-candidates
// The block search's grid reconsidered against the frame's own most common vectors, before the
// expansion, the camera fit and `measure` read it. core/flow_candidates.hpp is this pass on the CPU
// and says why it exists and what it keeps; the two are kept in step by hand: same candidates, same
// samples, same order of decisions, same constants. Same root signature and constants block as
// kFlowShader (a separate source because its registers hold other kinds of resource):
//   t0 the search's grid (R16G16_SINT, one vector per 8x8 block)   t5 / t6 the pair's pictures
//   u0 votes, 256 x 256 R32_UINT, one cell per vector within 127 pixels
//   u1 the grid handed on
//   u2 words 0..7 the candidates, strongest first, as (support << 16) | (y + 128) << 8 | (x + 128);
//      word 8 blocks that took a candidate, word 9 blocks that had one to compare
//   mode: cand_pick's round (0..7)      cost_max: the support a vector needs to be offered
// cand_clear, cand_count and cand_pick run one thread per block; cand_apply one thread GROUP per block.
inline const char kCandidateShader[] = R"(
Texture2D<int2>   t_grid : register(t0);
Texture2D<float4> t_cur  : register(t5);
Texture2D<float4> t_prev : register(t6);
RWTexture2D<uint> u_votes : register(u0);
RWTexture2D<int2> u_grid  : register(u1);
RWByteAddressBuffer u_top : register(u2);
SamplerState s_lin : register(s0);
cbuffer C : register(b0) {
    uint w, h, ew, eh;
    uint mode, block, has_cost, cost_max;
    uint cnt_off, gw, gh; float image_ratio;
    float snap_px, image_margin, image_abs, texture_min;
    uint have_hist, agree_min; float dj_x, dj_y;
    float agree_px, agree_cos, jitter_x, jitter_y;
    float history_dx, history_dy; uint frame_now, frame_reset;
};
static const int kWindow = 127, kReach = 8;
static const uint kCount = 8;
static const float kRatio = 0.8, kGain = 0.001;
float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
bool votes(int2 v) { return any(v != 0) && abs(v.x) <= kWindow && abs(v.y) <= kWindow; }
uint key_of(int2 v) { return (uint(v.y + 128) << 8) | uint(v.x + 128); }
int2 vector_of(uint packed) { return int2(int(packed & 255u) - 128, int((packed >> 8) & 255u) - 128); }
[numthreads(8, 8, 1)]
void cand_clear(uint3 id : SV_DispatchThreadID) {
    if (id.x < 256 && id.y < 256) u_votes[id.xy] = 0u;
}
[numthreads(8, 8, 1)]
void cand_count(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gw || id.y >= gh) return;
    int2 v = t_grid.Load(int3(id.xy, 0));
    if (votes(v)) InterlockedAdd(u_votes[uint2(v + 128)], 1u);
}
// One round per candidate: the strongest vector not already taken by an earlier round.
[numthreads(8, 8, 1)]
void cand_pick(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gw || id.y >= gh) return;
    int2 v = t_grid.Load(int3(id.xy, 0));
    if (!votes(v)) return;
    uint n = u_votes[uint2(v + 128)];
    if (n < cost_max) return;
    uint packed = (min(n, 65535u) << 16) | key_of(v);
    bool taken = false;
    [unroll] for (uint j = 0; j < 7; ++j) {
        if (j < mode && u_top.Load(j * 4) == packed) taken = true;
    }
    if (!taken) u_top.InterlockedMax(mode * 4, packed);
}
// One thread group is one 8x8 block and one thread is one of its pixels, so a block's 64 pixels are
// compared side by side (the first version walked them one after another in a single thread and
// cost about 1.2 ms on a busy 1920x1080 pair on the RTX card; Astra's timing, COMMS dc69b00a).
// The decisions are flow_candidates.hpp Decide exactly: each pixel writes its own differences, and
// thread 0 adds them up in the same order (rows, then columns) the CPU reference does.
//   step 1  per pixel: the block's own vector at whole pixels, and every candidate that may be offered
//   thread 0: the candidate with the least block error; nothing more to do when there is none, or
//           when the own vector's whole-pixel error is already under kGain (no candidate can then
//           be better by kGain: the exact shortcut Astra pointed out)
//   step 2  per pixel: the own vector and that candidate at their nine half-pixel places
//   thread 0: the decision, the grid and the two counters
// Dispatched as gw x gh groups. No thread leaves before the last barrier.
float prev_at(uint2 q, float2 v) { return luma(t_prev.SampleLevel(s_lin, (float2(q) + 0.5 + v) / float2(w, h), 0).rgb); }
groupshared float gc_err[18 * 64]; // step 1: slot 0 the own vector, slots 1..8 the candidates; step 2: slots 0..8 own, 9..17 the candidate
groupshared int gc_pick;           // the candidate chosen for step 2 (its index), or -1
groupshared uint gc_step2;         // 1 when step 2 is needed
[numthreads(8, 8, 1)]
void cand_apply(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID, uint lane : SV_GroupIndex) {
    uint2 b = group.xy;
    bool in_grid = b.x < gw && b.y < gh;
    uint2 q = b * 8 + thread.xy;
    bool inside = in_grid && q.x < w && q.y < h;
    int2 own = in_grid ? t_grid.Load(int3(b, 0)) : int2(0, 0);
    bool moving = in_grid && any(own != 0); // a block the search called still stays still
    int2 lo = int2(b) * 8, hi = min(lo + 8, int2(w, h));
    float c = inside ? luma(t_cur.Load(int3(q, 0)).rgb) : 0.0;
    uint offered = 0; // bit k: candidate k may be offered to this block (the same in every thread of the group)
    gc_err[lane] = inside && moving ? abs(c - prev_at(q, float2(own))) : 0.0;
    [unroll] for (uint k = 0; k < kCount; ++k) {
        uint packed = u_top.Load(k * 4);
        int2 cv = vector_of(packed);
        int2 d = abs(cv - own);
        // nearer than the search's own reach: the search compared those itself;
        // and the whole block must come from inside the picture
        bool may = moving && packed != 0u && max(d.x, d.y) > kReach && all(lo + cv >= 0) && all(hi + cv <= int2(w, h));
        if (may) { // slots of candidates not offered are never read
            offered |= 1u << k;
            gc_err[(k + 1) * 64 + lane] = inside ? abs(c - prev_at(q, float2(cv))) : 0.0;
        }
    }
    GroupMemoryBarrierWithGroupSync();
    float pixels = float(max(hi.x - lo.x, 0) * max(hi.y - lo.y, 0));
    if (lane == 0) {
        int pick = -1;
        float least = 1e9;
        [loop] for (uint k1 = 0; k1 < kCount; ++k1) {
            if ((offered >> k1) & 1u) {
                float e = 0;
                [loop] for (uint i1 = 0; i1 < 64; ++i1) e += gc_err[(k1 + 1) * 64 + i1];
                e /= max(pixels, 1.0);
                if (e < least) { least = e; pick = int(k1); }
            }
        }
        float e_whole = 0;
        [loop] for (uint i0 = 0; i0 < 64; ++i0) e_whole += gc_err[i0];
        e_whole /= max(pixels, 1.0);
        gc_pick = pick;
        gc_step2 = pick >= 0 && !(e_whole < kGain) ? 1u : 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    int picked = gc_pick;
    bool step2 = gc_step2 != 0u;
    int2 best = picked >= 0 ? vector_of(u_top.Load(uint(max(picked, 0)) * 4)) : own;
    if (step2) { // the slots are read only when step 2 ran
        [unroll] for (int sj = -1; sj <= 1; ++sj) {
            [unroll] for (int si = -1; si <= 1; ++si) {
                uint s = uint((sj + 1) * 3 + (si + 1));
                float2 half_pixel = 0.5 * float2(si, sj);
                gc_err[s * 64 + lane] = inside ? abs(c - prev_at(q, float2(own) + half_pixel)) : 0.0;
                gc_err[(9 + s) * 64 + lane] = inside ? abs(c - prev_at(q, float2(best) + half_pixel)) : 0.0;
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0 && in_grid) {
        int2 chosen = own;
        if (picked >= 0) u_top.InterlockedAdd(36, 1u); // the block had a candidate to compare
        if (step2) {
            // each vector at its best of the nine half-pixel places
            float e_own = 1e9, e_best = 1e9;
            [loop] for (uint s2 = 0; s2 < 9; ++s2) {
                float a = 0, o = 0;
                [loop] for (uint i2 = 0; i2 < 64; ++i2) { o += gc_err[s2 * 64 + i2]; a += gc_err[(9 + s2) * 64 + i2]; }
                e_own = min(e_own, o / max(pixels, 1.0));
                e_best = min(e_best, a / max(pixels, 1.0));
            }
            if (e_best <= kRatio * e_own && e_own - e_best >= kGain) { chosen = best; u_top.InterlockedAdd(32, 1u); }
        }
        u_grid[b] = chosen;
    }
}
)";
}
