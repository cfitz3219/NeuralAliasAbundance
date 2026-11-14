#pragma once
// AMD-path scene encode / image-consistency acceptance. Kept in step with j2_flow.cpp's kShader
// (the Anno-run gates: jitter-stabilised encode fe8ea3e, temporal agreement 5c5fb0d/319e2ae,
// ratio image check 8ca89a6, snap 0.5 px aa798b6), with AMD's whole-frame scene-change
// rejection (has_cost 2). NeuralAlias code, not a new optical-flow solver.
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
    float history_dx,history_dy,pad0,pad1;
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
)";
}
