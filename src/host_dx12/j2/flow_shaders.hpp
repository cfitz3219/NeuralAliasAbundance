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
//  - With a model, a pixel's own vector counts only when it leaves the sampling snap's range around
//    the model's motion (kTotalBound either way on each axis) and explains the pictures clearly
//    better than the model does anywhere inside that range: the stabilised pictures are a jittered,
//    point-sampled raster shifted back, so a thing thinner than a pixel moves by up to a pixel
//    between them while standing still (total_motion.hpp says what was seen and what this costs).
//  - One sample is not a texture: the window's contrast is taken with the one sample that carries
//    most of its range set aside.
//  - Validity written: 1 measured (with the screen, or moving by itself), 0.5 the screen's own
//    motion where the pictures could not measure, 0 nothing known (no model to follow).
static const float kTotalImageAbs = 0.08, kTotalImageRel = 0.15, kTotalTextureMin = 0.03;
static const float kTotalStillRatio = 0.5, kTotalBound = 1.0, kTotalDamp = 2e-5, kTotalEdge = 0.05;
static const float kTotalApproximate = 0.5;
float cur_luma(float2 q) { return luma(t_cur.SampleLevel(s_lin, q / float2(w, h), 0).rgb); }
float prev_luma(float2 q) { return luma(t_prev.SampleLevel(s_lin, q / float2(w, h), 0).rgb); }
// The current picture around p: nine places two pixels apart, each one's gradient (central
// differences one pixel either side), the structure sums and the local contrast (with the one
// sample that carries most of the range set aside). 33 reads.
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
    float lo = 1e9, lo2 = 1e9, hi = -1e9, hi2 = -1e9; // the darkest and the brightest sample, and the next of each
    [unroll] for (int wj = -1; wj <= 1; ++wj) {
        [unroll] for (int wi = -1; wi <= 1; ++wi) {
            int k = (wj + 1) * 3 + (wi + 1);
            float c = cur_luma(p + float2(2 * wi, 2 * wj));
            float2 g = 0.5 * float2(side[(wj + 1) * 4 + wi + 2] - side[(wj + 1) * 4 + wi + 1],
                                    updown[(wj + 2) * 3 + wi + 1] - updown[(wj + 1) * 3 + wi + 1]);
            win.lc[k] = c;
            win.g[k] = g;
            win.a += float3(g.x * g.x, g.x * g.y, g.y * g.y);
            if (c > hi) { hi2 = hi; hi = c; }
            else if (c > hi2) hi2 = c;
            if (c < lo) { lo2 = lo; lo = c; }
            else if (c < lo2) lo2 = c;
        }
    }
    win.contrast = max(0.0, min(hi2 - lo, hi - lo2));
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
// neither explains them). e_with: the error `hyp` leaves exactly; limit: what counts as explaining;
// e_snap: the least error `hyp` leaves within its refinement's bound (the sampling snap's range).
bool best_vector(float2 p, float2 hyp, bool raw_on, float2 raw, out float2 best, out float e_best, out float e_with, out float limit, out float e_snap) {
    best = hyp; e_best = 0; e_with = 0; limit = 0; e_snap = 0;
    Window win = window_at(p);
    bool textured = win.contrast >= kTotalTextureMin;
    if (textured) {
        float4 z = refine(p, hyp, win);
        best = z.xy; e_best = z.z; e_snap = z.z; e_with = z.w;
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
//      mode                     fit_solve: 0 a solving step, 1 the final evaluation, 2 the depth groups' split
#define fit_row offset        // which group of depths this fit is: 0 the far group (every depth when there is no split), 1 the near group
#define fit_tile_now agree_px  // the tile scale of the weights this pass uses; 0 no tile weights at all; under 0 none yet at this step
#define fit_tile_next agree_cos // the tile scale of the weights this pass leaves for the next; 0 or less: every weight 1
static const float kFitRobust = 1.5, kFitMinSamples = 256.0, kFitMinAgree = 0.5, kFitMaxVector = 512.0, kFitDamping = 1e-4;
static const float kFitTileFinal = 0.1, kFitTileNoise = 9.0; // screen_motion.hpp Params::tile_final_px, tile_noise
// screen_motion.hpp LayerParams and kDepthBins.
#define FIT_BINS 50
static const uint kFitGapBins = 3u, kFitNearSpan = 6u;
static const float kFitGapStray = 0.002;
#define FIT_TERMS 11
#define FIT_PAIRS 66   // the normal matrix's upper triangle, row by row
#define FIT_COUNT 77   // confirmed samples
#define FIT_ENERGY 78  // their robust error at the current terms
#define FIT_INLIERS 79 // those within kFitRobust of the model
#define FIT_NEAREST 80     // the largest depth distance among those (a maximum, not a sum)
#define FIT_VALUES 81
// The fit's buffer (u_cnt in fit_accum and fit_solve), in words: 0..10 the terms, 11 nearness_limit,
// 12 valid, 13 confirmed samples, 14 those agreeing, 15 their mean robust error, 16 the fit failed
// (too few samples, or a step could not be solved), 17 steps solved, 18 the stop state (FIT_STOP: 0
// solving; 1 the last step no longer moved its samples, so the next pass over them is the final
// evaluation; 2 that pass is in the buffer and the remaining passes do nothing); from FIT_HEAD each
// thread group's FIT_VALUES partial sums, and after the last group's each group's FIT_TILE words. A
// thread group is one TILE of 8 x 8 samples (screen_motion.hpp TileGrid). Its tile words: the sums
// over its samples of w rx, w ry, w, w r^2 with the sample weight alone, as this pass found them,
// and the tile's weight for the next pass (fit_solve writes it; screen_motion.hpp WeighTiles).
// Word 19: the nearness that parts the far group from the near group; 0 with one group
// (screen_motion.hpp FindSplit). fit_solve's mode 2 writes it and the fits of both groups read it.
#define FIT_HEAD 20
#define FIT_STOP 18
#define FIT_SPLIT 19
#define FIT_TILE 5
#define FIT_GATHER (FIT_VALUES + 4) // what a lane gathers per sample: the 81 values and the four tile sums
uint fit_tiles_across() { return (gw + 7u) / 8u; }
uint fit_tiles() { return fit_tiles_across() * ((gh + 7u) / 8u); }
uint fit_tile_word(uint tile, uint k) { return (FIT_HEAD + fit_tiles() * FIT_VALUES + tile * FIT_TILE + k) * 4; }
// Whether a sample of this nearness belongs to the group this fit is for (screen_motion.hpp Window).
bool fit_holds(float nearness, float split) { return fit_row == 0u ? (split <= 0.0 || nearness < split) : (split > 0.0 && nearness >= split); }
static const float kFitConverged = 0.001; // screen_motion.hpp Params::converged_px
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
        float2 best; float e_best, e_with, limit, e_snap;
        bool confirmed = best_vector(p, float2(0, 0), raw_on, raw, best, e_best, e_with, limit, e_snap);
        o = float4(best, nearness_of(t_color[c].x), confirmed ? 1.0 : 0.0);
    }
    u_enc[id.xy] = o;
}
// One pass over the samples at the buffer's terms (screen_motion.hpp Accumulate): 64 samples a
// thread group, their sums stored per group. t_hist is bound as the fit's grid. Each lane writes
// its own sample's values, then lane t sums value t (and t + 64) over the group's samples in lane
// order: one barrier, and the 81 sums are made and stored side by side.
groupshared float gf[64 * FIT_GATHER];
groupshared uint gc[64];
[numthreads(64, 1, 1)]
void fit_accum(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
    // The fit has stopped and its final pass is already in the buffer: nothing to do. (No lane
    // leaves before the barrier.)
    const bool idle = u_cnt.Load(FIT_STOP * 4) >= 2u;
    const uint across = fit_tiles_across();
    const uint2 cell = uint2((gid.x % across) * 8u + (gi & 7u), (gid.x / across) * 8u + (gi >> 3));
    uint used = 0u;
    if (!idle && cell.x < gw && cell.y < gh) {
        const float4 a = t_hist[cell];
        if (a.w > 0.5 && fin(a.x) && fin(a.y) && fin(a.z) && abs(a.x) < kFitMaxVector && abs(a.y) < kFitMaxVector &&
            fit_holds(a.z, asfloat(u_cnt.Load(FIT_SPLIT * 4)))) {
            used = 1u;
            const uint base = gi * FIT_GATHER;
            const float2 p = float2(cell * block + block / 2) + 0.5 - float2(jitter_x, jitter_y);
            float bu[FIT_TERMS], bv[FIT_TERMS];
            fit_terms(p, a.z, bu, bv);
            float2 m = 0;
            [unroll] for (uint k = 0; k < FIT_TERMS; ++k) m += asfloat(u_cnt.Load(k * 4)) * float2(bu[k], bv[k]);
            const float2 r = a.xy - m;
            const float rr = dot(r, r), c2 = kFitRobust * kFitRobust;
            // 1 / (1 + r^2 / s^2)^2: its pull on the fit falls as 1 / r^3 (screen_motion.hpp).
            const float qw = fit_scale > 0.0 ? 1.0 / (1.0 + rr / (fit_scale * fit_scale)) : 1.0;
            // The tile's weight, made from the pass before this one (screen_motion.hpp WeighTiles).
            const float tile = fit_tile_now > 0.0 ? asfloat(u_cnt.Load(fit_tile_word(gid.x, 4))) : 1.0;
            const float ws = qw * qw, wgt = ws * tile;
            uint idx = 0;
            [unroll] for (uint ia = 0; ia < FIT_TERMS; ++ia) {
                [unroll] for (uint ib = ia; ib < FIT_TERMS; ++ib) { gf[base + idx] = wgt * (bu[ia] * bu[ib] + bv[ia] * bv[ib]); ++idx; }
            }
            // The right-hand side holds what the terms so far leave unexplained: each step solves for a correction.
            [unroll] for (uint ic = 0; ic < FIT_TERMS; ++ic) gf[base + FIT_PAIRS + ic] = wgt * (bu[ic] * r.x + bv[ic] * r.y);
            const bool agrees = rr <= c2;
            gf[base + FIT_COUNT] = 1.0;
            gf[base + FIT_ENERGY] = 0.5 * rr / (1.0 + rr / c2);
            gf[base + FIT_INLIERS] = agrees ? 1.0 : 0.0;
            // The model is asked no nearer than the nearest agreeing sample of a tile that still holds the fit.
            gf[base + FIT_NEAREST] = agrees && tile > 0.5 ? a.z : 0.0;
            // This pass's own disagreement for the tile, with the sample weight alone.
            gf[base + FIT_VALUES + 0] = ws * r.x;
            gf[base + FIT_VALUES + 1] = ws * r.y;
            gf[base + FIT_VALUES + 2] = ws;
            gf[base + FIT_VALUES + 3] = ws * rr;
        }
    }
    gc[gi] = used;
    GroupMemoryBarrierWithGroupSync();
    if (!idle) {
        [unroll] for (uint part = 0; part < 2; ++part) {
            const uint t = gi + part * 64;
            if (t < FIT_GATHER) {
                float sum = 0;
                [loop] for (uint lane = 0; lane < 64; ++lane) {
                    if (gc[lane] != 0u) {
                        const float x = gf[lane * FIT_GATHER + t];
                        sum = t == FIT_NEAREST ? max(sum, x) : sum + x;
                    }
                }
                if (t < FIT_VALUES) u_cnt.Store((FIT_HEAD + gid.x * FIT_VALUES + t) * 4, asuint(sum));
                else u_cnt.Store(fit_tile_word(gid.x, t - FIT_VALUES), asuint(sum));
            }
        }
    }
}
)" R"(
// One thread group: the groups' sums reduced, then one step of the fit (screen_motion.hpp Solve:
// the scaled, damped normal equations by Cholesky, as a correction to the terms) or, with mode 1,
// the final evaluation (screen_motion.hpp Fit's last pass). u_enc is bound as the model `measure`
// reads (RG32F, 9 x 2; row fit_row: 0 the far group or the one fit of every depth, 1 the near
// group): texels 0..4 the terms in pairs, 5 the last term and nearness_limit, 6 valid and the
// confirmed samples, 7 those agreeing and their mean robust error, 8 the split's nearness.
// A solving step also leaves every tile's weight for the next pass (screen_motion.hpp WeighTiles).
// Mode 2, before the fits: the samples' depths are counted in half-octave bins (t_hist is bound as
// the fit's grid) and the split is found (screen_motion.hpp DepthCounts, FindSplit).
// The 81 values are reduced side by side: lane v + 81 k (k = 0, 1, 2) sums value v over every third
// group, then lane v joins its three parts. Two barriers in all.
groupshared float gr[256];
groupshared float gt[FIT_VALUES];
groupshared float gq[512];             // each lane's share of the tiles' spread and weight
groupshared float gvar;                // the samples' spread about their tiles' means
groupshared uint gbin[64 * FIT_BINS];  // mode 2: 64 lanes' own counts per bin
uint fit_bin(float nearness) { // screen_motion.hpp DepthBin
    if (!(nearness > 5.9604645e-8)) return 0u;
    return uint(clamp(49 + int(floor(2.0 * log2(nearness))), 1, FIT_BINS - 1));
}
void fit_split(uint gi) {
    if (gi < 64u) {
        uint own[FIT_BINS];
        [loop] for (uint b0 = 0; b0 < FIT_BINS; ++b0) own[b0] = 0u;
        const uint cells = gw * gh;
        [loop] for (uint at = gi; at < cells; at += 64u) {
            const float4 a = t_hist[uint2(at % gw, at / gw)];
            if (a.w > 0.5 && fin(a.x) && fin(a.y) && fin(a.z) && abs(a.x) < kFitMaxVector && abs(a.y) < kFitMaxVector) own[fit_bin(a.z)] += 1u;
        }
        [loop] for (uint b1 = 0; b1 < FIT_BINS; ++b1) gbin[gi * FIT_BINS + b1] = own[b1];
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi != 0u) return;
    float bins[FIT_BINS];
    float total = 0;
    [loop] for (uint b = 0; b < FIT_BINS; ++b) {
        uint n = 0u;
        [loop] for (uint lane = 0; lane < 64u; ++lane) n += gbin[lane * FIT_BINS + b];
        bins[b] = float(n);
        total += float(n);
    }
    const float stray_max = kFitGapStray * total;
    // The nearest bin that holds more than strays.
    uint top = FIT_BINS - 1;
    float nearer = 0;
    [loop] for (uint q = 0; q < FIT_BINS; ++q) {
        if (top > 0u && nearer + bins[top] <= stray_max) { nearer += bins[top]; --top; }
    }
    float split = 0, below = 0;
    uint best = 0u;
    [loop] for (uint first = 1; first < FIT_BINS; ++first) {
        below += bins[first - 1];
        // The longest run starting here that holds no more than the stray allowance.
        float held = 0;
        uint run = 0u;
        bool open = true;
        [loop] for (uint c = 0; c < FIT_BINS; ++c) {
            if (c >= first && open) {
                if (held + bins[c] <= stray_max) { held += bins[c]; ++run; }
                else open = false;
            }
        }
        const float above = total - below - held;
        const bool near_is_one_group = top < first + run + kFitNearSpan;
        if (below >= kFitMinSamples && run >= kFitGapBins && above >= kFitMinSamples && run > best && near_is_one_group) {
            best = run;
            split = exp2(0.5 * (float(first + run / 2u) - 49.0)); // the lower edge of the run's middle bin
        }
    }
    u_cnt.Store(FIT_SPLIT * 4, asuint(split));
}
[numthreads(256, 1, 1)]
void fit_solve(uint gi : SV_GroupIndex) {
    if (mode == 2u) { fit_split(gi); return; }
    const uint groups = fit_tiles();
    const uint stop = u_cnt.Load(FIT_STOP * 4);
    // A solving step after the fit has stopped: 1 keeps this pass's sums as the final evaluation,
    // 2 has nothing left to do. Either way nothing is reduced or solved.
    const bool idle = mode == 0u && stop >= 1u;
    float part = 0;
    if (!idle && gi < 3 * FIT_VALUES) {
        const uint value = gi % FIT_VALUES;
        [loop] for (uint g = gi / FIT_VALUES; g < groups; g += 3) {
            const float x = asfloat(u_cnt.Load((FIT_HEAD + g * FIT_VALUES + value) * 4));
            part = value == FIT_NEAREST ? max(part, x) : part + x;
        }
    }
    gr[gi] = part;
    // The tiles' weights for the next pass: first the samples' own spread about their tiles' means.
    const bool weigh = !idle && mode == 0u;
    float spread = 0, weight = 0;
    if (weigh) {
        [loop] for (uint g1 = gi; g1 < groups; g1 += 256u) {
            const float sw = asfloat(u_cnt.Load(fit_tile_word(g1, 2)));
            if (sw > 0.0) {
                const float2 mean = float2(asfloat(u_cnt.Load(fit_tile_word(g1, 0))), asfloat(u_cnt.Load(fit_tile_word(g1, 1)))) / sw;
                spread += asfloat(u_cnt.Load(fit_tile_word(g1, 3))) - sw * dot(mean, mean);
                weight += sw;
            }
        }
    }
    gq[gi] = spread;
    gq[256 + gi] = weight;
    GroupMemoryBarrierWithGroupSync();
    if (gi < FIT_VALUES) {
        const float x0 = gr[gi], x1 = gr[gi + FIT_VALUES], x2 = gr[gi + 2 * FIT_VALUES];
        gt[gi] = gi == FIT_NEAREST ? max(x0, max(x1, x2)) : x0 + x1 + x2;
    }
    if (gi == 255u) { // a lane with nothing else to do here
        float sp = 0, wt = 0;
        [loop] for (uint lane = 0; lane < 256u; ++lane) { sp += gq[lane]; wt += gq[256 + lane]; }
        gvar = wt > 0.0 ? max(sp / wt, 0.0) : 0.0;
    }
    GroupMemoryBarrierWithGroupSync();
    if (weigh) {
        [loop] for (uint g2 = gi; g2 < groups; g2 += 256u) {
            float tw = 1.0;
            const float sw = asfloat(u_cnt.Load(fit_tile_word(g2, 2)));
            if (fit_tile_next > 0.0 && sw > 0.0) {
                const float2 mean = float2(asfloat(u_cnt.Load(fit_tile_word(g2, 0))), asfloat(u_cnt.Load(fit_tile_word(g2, 1)))) / sw;
                const float qt = 1.0 / (1.0 + dot(mean, mean) / (fit_tile_next * fit_tile_next + kFitTileNoise * gvar / sw));
                tw = qt * qt;
            }
            u_cnt.Store(fit_tile_word(g2, 4), asuint(tw));
        }
    }
    if (gi != 0) return;
    if (idle) {
        if (stop == 1u) u_cnt.Store(FIT_STOP * 4, 2u);
        return;
    }
    const float n = gt[FIT_COUNT];
    bool failed = u_cnt.Load(16 * 4) != 0u;
    if (mode == 0u) {
        if (failed) return;
        u_cnt.Store(13 * 4, asuint(n));
        bool ok = n >= kFitMinSamples;
        // Plain loops, not unrolled: one thread runs this once a step, and the program stays small to
        // compile. Every loop runs over all the terms and tests its own range inside: fxc tries a loop's
        // body once before its condition, and a loop that starts beyond the last term (i = j + 1 at the
        // last j) is refused as an out-of-bounds access (X3504).
        float A[FIT_TERMS][FIT_TERMS], b[FIT_TERMS], sc[FIT_TERMS];
        uint idx = 0;
        [loop] for (uint a = 0; a < FIT_TERMS; ++a) {
            [loop] for (uint c = 0; c < FIT_TERMS; ++c) {
                if (c >= a) { A[a][c] = gt[idx]; A[c][a] = gt[idx]; ++idx; }
            }
        }
        [loop] for (uint a1 = 0; a1 < FIT_TERMS; ++a1) { b[a1] = gt[FIT_PAIRS + a1]; sc[a1] = A[a1][a1] > 1e-20 ? rsqrt(A[a1][a1]) : 0.0; }
        [loop] for (uint a2 = 0; a2 < FIT_TERMS; ++a2) {
            [loop] for (uint c2 = 0; c2 < FIT_TERMS; ++c2) A[a2][c2] *= sc[a2] * sc[c2];
            // A term no sample carries (every depth at the far end, say) keeps its value.
            A[a2][a2] = (sc[a2] > 0 ? A[a2][a2] : 0.0) + kFitDamping + (sc[a2] > 0 ? 0.0 : 1.0);
            b[a2] *= sc[a2];
        }
        float L[FIT_TERMS][FIT_TERMS], yv[FIT_TERMS], xv[FIT_TERMS];
        [loop] for (uint r0 = 0; r0 < FIT_TERMS; ++r0) {
            yv[r0] = 0; xv[r0] = 0;
            [loop] for (uint c3 = 0; c3 < FIT_TERMS; ++c3) L[r0][c3] = 0;
        }
        [loop] for (uint j = 0; j < FIT_TERMS; ++j) {
            float d = A[j][j];
            [loop] for (uint m = 0; m < FIT_TERMS; ++m) { if (m < j) d -= L[j][m] * L[j][m]; }
            if (!(d > 1e-12)) ok = false;
            const float dj = sqrt(max(d, 1e-12));
            L[j][j] = dj;
            [loop] for (uint i5 = 0; i5 < FIT_TERMS; ++i5) {
                if (i5 > j) {
                    float t = A[i5][j];
                    [loop] for (uint m2 = 0; m2 < FIT_TERMS; ++m2) { if (m2 < j) t -= L[i5][m2] * L[j][m2]; }
                    L[i5][j] = t / dj;
                }
            }
        }
        [loop] for (uint i6 = 0; i6 < FIT_TERMS; ++i6) {
            float t = b[i6];
            [loop] for (uint m3 = 0; m3 < FIT_TERMS; ++m3) { if (m3 < i6) t -= L[i6][m3] * yv[m3]; }
            yv[i6] = t / L[i6][i6];
        }
        [loop] for (uint q7 = 0; q7 < FIT_TERMS; ++q7) {
            const uint i7 = FIT_TERMS - 1 - q7;
            float t = yv[i7];
            [loop] for (uint m4 = 0; m4 < FIT_TERMS; ++m4) { if (m4 > i7) t -= L[m4][i7] * xv[m4]; }
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
            // How far this correction moved the samples the step trusted (screen_motion.hpp StepSize):
            // delta' H delta over the sum of the weights, which is H's first entry. Once the weight
            // has its final scale, a step that no longer moves them is the last.
            float q = 0;
            uint hi = 0;
            [loop] for (uint qa = 0; qa < FIT_TERMS; ++qa) {
                [loop] for (uint qb = 0; qb < FIT_TERMS; ++qb) {
                    if (qb >= qa) { q += (qa == qb ? 1.0 : 2.0) * (xv[qa] * sc[qa]) * (xv[qb] * sc[qb]) * gt[hi]; ++hi; }
                }
            }
            const float moved = gt[0] > 1e-20 ? sqrt(max(q, 0.0) / gt[0]) : 0.0;
            // (and, with tile weights, once those have their final scale too)
            const bool tiles_final = fit_tile_now == 0.0 || (fit_tile_now > 0.0 && fit_tile_now <= kFitTileFinal);
            if (fit_scale > 0.0 && fit_scale <= kFitRobust && tiles_final && moved < kFitConverged) u_cnt.Store(FIT_STOP * 4, 1u);
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
    [unroll] for (uint k4 = 0; k4 < 6; ++k4) u_enc[uint2(k4, fit_row)] = float4(th2[2 * k4], th2[2 * k4 + 1], 0, 0);
    u_enc[uint2(6, fit_row)] = float4(valid ? 1.0 : 0.0, n, 0, 0);
    u_enc[uint2(7, fit_row)] = float4(agree, energy, 0, 0);
    u_enc[uint2(8, fit_row)] = float4(asfloat(u_cnt.Load(FIT_SPLIT * 4)), 0, 0, 0);
}
// The model at a position (screen_motion.hpp Evaluate). t_res is bound as fit_solve's model; with
// nothing bound there every read is 0 and there is no model.
// `row`: which group's fit (screen_row).
float2 screen_at(float2 p, float nearness, int row) {
    float bu[FIT_TERMS], bv[FIT_TERMS];
    fit_terms(p, nearness, bu, bv);
    float2 v = 0;
    [unroll] for (uint k = 0; k < FIT_TERMS; ++k) {
        const float2 pr = t_res[int2(k / 2, row)];
        v += ((k & 1u) != 0u ? pr.y : pr.x) * float2(bu[k], bv[k]);
    }
    return v;
}
// The fit a pixel of this depth is given (screen_motion.hpp ModelFor): its own group's when that is
// valid, else the other group's; -1: no model at all. A model texture of one row has one fit.
int screen_row(float nearness) {
    const float split = t_res[int2(8, 0)].x;
    const int own = split > 0.0 && nearness >= split ? 1 : 0;
    if (t_res[int2(6, own)].x > 0.5) return own;
    return t_res[int2(6, 1 - own)].x > 0.5 ? 1 - own : -1;
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
            const float own_nearness = nearness_of(t_color[id.xy].x);
            const int row = screen_row(own_nearness);
            bool have_screen = row >= 0;
            float2 hyp = 0;
            if (have_screen) hyp = screen_at(p, min(own_nearness, t_res[int2(5, row)].y), row);
            float2 best; float e_best, e_with, limit, e_snap;
            if (best_vector(p, hyp, raw_on, raw, best, e_best, e_with, limit, e_snap)) {
                valid = 1.0;
                // The hypothesis stands unless the pixel's own vector explains the picture clearly
                // better. With a model: inside the sampling snap's range the pictures cannot tell
                // motion from sampling, and outside it the own vector must beat the best the model
                // does anywhere inside.
                bool with_screen;
                if (have_screen) {
                    bool outside = any(abs(best - hyp) > kTotalBound);
                    with_screen = !outside || (e_snap <= limit && !(e_best <= kTotalStillRatio * e_snap));
                } else {
                    with_screen = all(best == hyp) || (e_with <= limit && !(e_best <= kTotalStillRatio * e_with));
                }
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
// A group covers a 32x32 tile (compute_flow.cpp kCutTile). On ordinary frames it exits
// before touching either image. On a cut, the 8x8 lanes write sixteen coalesced patches.
[numthreads(8, 8, 1)]
void cut_show(uint3 group : SV_GroupID, uint3 lane : SV_GroupThreadID) {
    if (u_cnt.Load(4) == 0u) return;
    const uint2 first = group.xy * 32 + lane.xy;
    [unroll] for (uint y = 0; y < 4; ++y) {
        [unroll] for (uint x = 0; x < 4; ++x) {
            const uint2 p = first + uint2(x, y) * 8;
            if (p.x < w && p.y < h)
                u_enc[p] = t_color.SampleLevel(s_lin, (float2(p) + 0.5) / float2(w, h), 0);
        }
    }
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
