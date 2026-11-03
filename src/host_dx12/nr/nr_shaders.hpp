#pragma once
namespace na::nr::shaders {
inline constexpr char kSource[] = R"(
Texture2D<float4> t_a : register(t0);
Texture2D<float4> t_b : register(t1);
Texture2D<float4> t_c : register(t2);
RWTexture2D<float4> u_out : register(u0);
cbuffer C : register(b0) { float paper_white; uint width; uint height; uint mode; uint src_w; uint src_h; };

static const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
float3 pq_to_nits(float3 e) {
    float3 ep = pow(saturate(e), 1.0 / m2);
    float3 num = max(ep - c1, 0.0), den = c2 - c3 * ep;
    return pow(num / den, 1.0 / m1) * 10000.0;
}
float3 nits_to_pq(float3 n) {
    float3 y = pow(saturate(n / 10000.0), m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}
// mode 2 (scRGB): linear, 1.0 = 80 nits (sRGB reference white), may exceed 1 or be negative.
float3 hdr_to_nits(float3 v) { return mode == 2 ? v * 80.0 : pq_to_nits(v); }
float3 nits_to_hdr(float3 n) { return mode == 2 ? n / 80.0 : nits_to_pq(n); }
float3 srgb_encode(float3 v) {
    v = saturate(v);
    return v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
}
float3 srgb_decode(float3 s) {
    s = saturate(s);
    return s <= 0.04045 ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4);
}
// Phase-4 scaled modes: bring render-size depth/motion to display size.
// Depth and motion choose the same nearest source pixel, preserving their
// surface correspondence. MVecScale carries the pixel-unit conversion.
SamplerState s_linear : register(s0);
[numthreads(8, 8, 1)]
void upsample_depth(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    uint sw = src_w, sh = src_h; // the rendered region, not the (padded) allocation
    uint2 src = uint2(min((id.x * sw) / width, sw - 1), min((id.y * sh) / height, sh - 1));
    u_out[id.xy] = t_a[src];
}
[numthreads(8, 8, 1)]
void upsample_motion(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    uint sw = src_w, sh = src_h;
    uint2 src = uint2(min((id.x * sw) / width, sw - 1), min((id.y * sh) / height, sh - 1));
    u_out[id.xy] = t_a[src];
}
// Argon audit #2 / C12: 24-bit (R24_UNORM_X8 view) or 16-bit (R16_UNORM view)
// depth -> R32F, for the rendered region starting at (src_w, src_h).
[numthreads(8, 8, 1)]
void depth_to_r32(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    u_out[id.xy] = t_a[id.xy + uint2(src_w, src_h)].rrrr;
}
[numthreads(8, 8, 1)]
void hdr_to_sdr(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    float3 n = hdr_to_nits(t_a[id.xy].rgb) / paper_white;
    u_out[id.xy] = float4(srgb_encode(n), 1.0);
}
[numthreads(8, 8, 1)]
void sdr_to_hdr(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    float4 orig = t_a[id.xy];                            // original HDR pixel
    float3 nr = srgb_decode(t_b[id.xy].rgb);             // NR result, linear 0..1
    float3 sdr = srgb_decode(t_c[id.xy].rgb);            // NR input, linear 0..1
    // `precise`: the fast reciprocal would make x/x a hair under 1, and an
    // fp16 store truncates, so an unchanged pixel would lose a step per frame.
    precise float3 ratio = clamp((nr + 0.002) / (sdr + 0.002), 0.25, 4.0);
    ratio = all(nr == sdr) ? 1.0 : ratio;
    // scRGB is linear already: scale the stored value directly (a ratio of 1
    // reproduces the input bit for bit; a nits round trip would not).
    float3 out_rgb = mode == 2 ? orig.rgb * ratio : nits_to_pq(pq_to_nits(orig.rgb) * ratio);
    u_out[id.xy] = float4(out_rgb, orig.a);
}
[numthreads(8, 8, 1)]
void hdr_region_to_sdr(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    float4 orig = t_a[id.xy + uint2(src_w, src_h)];
    u_out[id.xy] = float4(srgb_encode(hdr_to_nits(orig.rgb) / paper_white), orig.a);
}
[numthreads(8, 8, 1)]
void hdr_region_output(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    float4 orig = t_a[id.xy + uint2(src_w, src_h)];
    float3 nr = srgb_decode(t_b[id.xy].rgb), sdr = srgb_decode(t_c[id.xy].rgb);
    precise float3 ratio = clamp((nr + 0.002) / (sdr + 0.002), 0.25, 4.0);
    ratio = all(nr == sdr) ? 1.0 : ratio;
    float3 rgb = mode == 2 ? orig.rgb * ratio : nits_to_pq(pq_to_nits(orig.rgb) * ratio);
    u_out[id.xy] = float4(rgb, orig.a);
}
)";
}
