#pragma once
// Shared scene encode / image-consistency acceptance. NeuralAlias code, not a new optical-flow solver.
namespace na::j2 {
inline const char kFlowShader[] = R"(
Texture2D<float4> t_color : register(t0);
Texture2D<float2> t_raw   : register(t1);
Texture2D<float2> t_res   : register(t2);
Texture2D<float>  t_valid : register(t3);
Texture2D<uint>   t_cost  : register(t4);
Texture2D<float4> t_cur   : register(t5);
Texture2D<float4> t_prev  : register(t6);
RWTexture2D<float4> u_enc : register(u0);
RWTexture2D<float>  u_mask : register(u1);
RWByteAddressBuffer u_cnt : register(u2);
SamplerState s_lin : register(s0);
cbuffer C : register(b0) {
    uint w, h, ew, eh;
    uint mode, block, has_cost, cost_max;
    uint cnt_off, gw, gh, pad0;
    float snap_px, image_margin, image_abs, texture_min;
};
float3 srgb_encode(float3 v) { v = saturate(v); return v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1.0 / 2.4) - 0.055; }
// One fixed transform for every image (both images of a pair are encoded alike).
[numthreads(8, 8, 1)]
void encode(uint3 id : SV_DispatchThreadID) {
    if (id.x >= ew || id.y >= eh) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(ew, eh);
    float3 c = t_color.SampleLevel(s_lin, uv, 0).rgb;
    if (any(isnan(c)) || any(isinf(c))) c = float3(0, 0, 0);
    if (mode == 1) { c = max(c, 0.0); c = srgb_encode(c / (1.0 + c)); }
    else c = saturate(c);
    u_enc[id.xy] = float4(c, 1.0); // UAV format B8G8R8A8_UNORM: the view swizzles
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
groupshared uint gs[8];
[numthreads(8, 8, 1)]
void accept(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    if (gi < 8) gs[gi] = 0;
    GroupMemoryBarrierWithGroupSync();
    if (id.x < w && id.y < h) {
        uint why = 0; // 0 accepted, 1 invalid, 2 small, 3 cost, 4 offscreen, 5 image (or flat)
        uint cost = 0;
        bool costed = false;
        float v = t_valid[id.xy];
        float2 r = t_res[id.xy];
        float2 raw = t_raw[id.xy];
        if (!(v > 0.0 && v <= 1.0) || any(isnan(r)) || any(isnan(raw)) || any(isinf(r)) || any(isinf(raw))) why = 1;
        else if (length(r) < snap_px) why = 2;
        // AMD scene-change output shares the uint input, with a distinct mode.
        if (has_cost == 2 && (t_cost[int2(1, 0)] & 15u) != 0) why = 1;
        if (why == 0 && has_cost == 1) {
            uint2 b = min(id.xy / block, uint2(gw - 1, gh - 1));
            cost = t_cost[b];
            costed = true;
            if (cost > cost_max) why = 3;
        }
        if (why == 0) {
            float2 p = float2(id.xy) + 0.5;
            float2 q = p + raw;
            if (q.x < 0.0 || q.y < 0.0 || q.x > float(w) || q.y > float(h)) why = 4;
            else if (texture_at(p) < texture_min) why = 5; // flat: the image cannot confirm a residual
            else {
                float e_flow = image_error(p, raw);
                float e_cam = image_error(p, raw - r);
                // Rejected when the image contradicts the flow; accepted where the image
                // cannot tell (flat interiors of moving objects), cost permitting.
                if (!(e_flow <= image_abs && e_flow <= e_cam + image_margin)) why = 5;
            }
        }
        u_mask[id.xy] = why == 0 ? 1.0 : 0.0;
        InterlockedAdd(gs[why], 1u);
        if (costed) { InterlockedAdd(gs[6], cost); InterlockedAdd(gs[7], 1u); }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi < 8 && gs[gi] != 0) u_cnt.InterlockedAdd(cnt_off + gi * 4, gs[gi]);
}
)";
}
