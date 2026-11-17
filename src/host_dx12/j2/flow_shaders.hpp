#pragma once
// AMD-path scene encode / image-consistency acceptance. Kept in step with j2_flow.cpp's kShader
// (the Anno-run gates: jitter-stabilised encode fe8ea3e, temporal agreement 5c5fb0d/319e2ae,
// ratio image check 8ca89a6, snap 0.5 px aa798b6), with AMD's whole-frame scene-change
// rejection (has_cost 2). NeuralAlias code, not a new optical-flow solver.
//
// `measure`, `cut_state` and `cut_show` are the camera-free path (2026-11-16): the pair's TOTAL
// motion measured on the two pictures, and what a scene cut does to the picture shown. `accept`
// and its gates are the residual path against a camera and are not changed by them.
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
//  - Validity written: 1 measured (moving or still), 0.5 the search's vector handed on because the
//    pictures could not confirm it (flat, or neither hypothesis explains them), 0 nothing known.
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
// t_raw: the search's vector after the depth-aware expansion; t_valid: that expansion's validity.
// u_enc is bound as the RG32F total field here, u_mask as its validity.
[numthreads(8, 8, 1)]
void measure(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    if (gi < 10) gs[gi] = 0;
    GroupMemoryBarrierWithGroupSync();
    if (id.x < w && id.y < h) {
        // 0 measured moving, 1 nothing known, 4 the search's vector from outside the frame,
        // 5 the search's vector unconfirmed, 7 measured still. Slot 8: this pair is a scene cut.
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
            Window win = window_at(p);
            bool measured = false;
            if (win.contrast >= kTotalTextureMin) {
                float4 z = refine(p, float2(0, 0), win);
                float2 best = z.xy;
                float e_best = z.z, e_still = z.w;
                if (raw_on && any(raw != 0.0)) {
                    float4 r = refine(p, raw, win);
                    if (r.z <= z.z) { best = r.xy; e_best = r.z; }
                }
                // Explaining the picture: a small error in itself and small beside the window's own contrast.
                float limit = min(kTotalImageAbs, kTotalImageRel * win.contrast);
                if (e_best <= limit) {
                    measured = true;
                    valid = 1.0;
                    // Exact stillness is a hypothesis too: a vector replaces it only by explaining the picture clearly better.
                    bool still = all(best == 0.0) || (e_still <= limit && !(e_best <= kTotalStillRatio * e_still));
                    if (still) why = 7;
                    else { why = 0; v = best; }
                }
            }
            if (!measured && have_raw) { v = raw; valid = kTotalApproximate; why = raw_on ? 5 : 4; }
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
