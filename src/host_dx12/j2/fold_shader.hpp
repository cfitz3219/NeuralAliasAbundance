#pragma once
namespace na::j2 {
inline constexpr char kFoldHlsl[] = R"(
Texture2D<float4> t_color : register(t0);
Texture2D<float> t_depth : register(t1);
Texture2D<float2> t_motion : register(t2);
RWTexture2D<float4> u_color : register(u0);
RWTexture2D<float> u_depth : register(u1);
RWTexture2D<float2> u_motion : register(u2);
// motion_ready: the motion plane was already written at the folded size (the arbiter decided only the
// kept pixels: live_dlaa.cpp, sampled motion); it is left as it is. 0: folded here like the others.
cbuffer C : register(b0) { uint bx; uint by; uint px; uint py; uint motion_ready; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    u_color.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    uint2 source = id.xy * uint2(bx, by) + uint2(px, py);
    u_color[id.xy] = t_color[source];
    u_depth[id.xy] = t_depth[source];
    if (motion_ready == 0u) u_motion[id.xy] = t_motion[source];
})";
}
