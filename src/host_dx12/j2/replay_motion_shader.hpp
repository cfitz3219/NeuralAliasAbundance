// The draw-replay motion pass's programs (replay_motion.cpp): `vs` (vs_5_0) and `ps` (ps_5_0).
// In a header of its own so the compile check (tests/host/j2_flow_shaders_compile_test.cpp) builds
// them with the real HLSL compiler in CI, where no graphics card runs the pass itself.
#pragma once

namespace na::j2::replay_motion {

inline constexpr char kShader[] = R"(
ByteAddressBuffer t_cur   : register(t0);
ByteAddressBuffer t_prev  : register(t1);
Texture2D<float>  t_depth : register(t2);
ByteAddressBuffer t_stamp : register(t3);
Texture2D<float2> t_model : register(t4); // ref_mode 2: the fitted screen motion (flow_shaders.hpp fit_solve's model)
ByteAddressBuffer t_fix   : register(t5); // ref_mode 1: the arbiter's optional M3 correction
cbuffer C : register(b0) {
    uint cur_first, prev_first; // per draw (float4 elements from the buffers' view start)
    uint check, token;          // per draw: check != 0 -> positions valid only if stamp[check-1] == token
    uint prev_check, prev_token; // same for the previous frame's positions
    uint slot, pad0;            // per draw: its pair of learning counters, 0xffffffff = none
    uint w, h;
    float jx, jy;               // current viewport jitter, absent from the saved clip positions
    uint ref_mode;              // the screen's own motion to compare with: 0 none, 1 the camera, 2 the fitted model; bit 8: t_fix is bound
    float depth_far;            // the depth value at the far end
    float apart_px, apart_rel;  // apart from the screen: further than apart_px + apart_rel * |screen motion|
    row_major float4x4 reproj;  // ref_mode 1: current clip -> previous clip, row-vector algebra (motion_arbiter_gpu.cpp)
};
struct V { float4 pos : SV_Position; float4 prev : PREV; };
V vs(uint id : SV_VertexID) {
    V o;
    // The writer list ran after this one (or never): its positions are stale.
    // Clip the whole draw (z < 0) so it leaves no motion and no mask.
    if ((check != 0u && t_stamp.Load((check - 1u) * 4u) != token) ||
        (prev_check != 0u && t_stamp.Load((prev_check - 1u) * 4u) != prev_token)) {
        o.pos = float4(0, 0, -1, 1);
        o.prev = float4(0, 0, 0, 0);
        return o;
    }
    o.pos = asfloat(t_cur.Load4((cur_first + id) * 16u));
    o.prev = asfloat(t_prev.Load4((prev_first + id) * 16u));
    return o;
}
RWByteAddressBuffer u_count : register(u2);
bool fin(float a) { return (asuint(a) & 0x7fffffffu) < 0x7f800000u; }
// The camera's motion of a still scene at unjittered position c (motion_arbiter_gpu.cpp reproject,
// with the composed matrix and the same optional correction).
bool camera_at(float2 c, float depth, out float2 r) {
    r = 0;
    if (!fin(depth) || depth < 0.0 || depth > 1.0) return false;
    if (depth == depth_far) depth = depth_far == 0.0 ? 1e-6 : 0.999999;
    float4x4 m = reproj;
    if ((ref_mode & 256u) != 0u && t_fix.Load(64) != 0u) {
        [unroll] for (uint k = 0; k < 4; ++k) m[k] += asfloat(t_fix.Load4(k * 16));
    }
    const float4 n = float4(c.x / float(w) * 2.0 - 1.0, 1.0 - c.y / float(h) * 2.0, depth, 1.0);
    const float4 q = float4(
        n.x * m._11 + n.y * m._21 + n.z * m._31 + n.w * m._41,
        n.x * m._12 + n.y * m._22 + n.z * m._32 + n.w * m._42,
        n.x * m._13 + n.y * m._23 + n.z * m._33 + n.w * m._43,
        n.x * m._14 + n.y * m._24 + n.z * m._34 + n.w * m._44);
    if (!fin(q.w) || abs(q.w) < 1e-20) return false;
    r = float2((q.x / q.w + 1.0) * 0.5 * float(w), (1.0 - q.y / q.w) * 0.5 * float(h)) - c;
    return fin(r.x) && fin(r.y);
}
// The fitted screen motion at unjittered position c (core/screen_motion.hpp Evaluate; the model's
// texels are flow_shaders.hpp fit_solve's: 0..5 the eleven terms and the nearness limit, 6.x valid).
bool screen_at(float2 c, float depth, out float2 r) {
    r = 0;
    // Two groups of depths each have their own fit (rows 0 and 1; texel 8 the nearness that parts
    // them, 0 with one group): a pixel is given its own group's when that is valid, else the other's.
    const float own = fin(depth) && depth >= 0.0 && depth <= 1.0 ? abs(depth - depth_far) : 0.0;
    const float split = t_model[int2(8, 0)].x;
    int row = split > 0.0 && own >= split ? 1 : 0;
    if (!(t_model[int2(6, row)].x > 0.5)) row = 1 - row;
    if (!(t_model[int2(6, row)].x > 0.5)) return false;
    const float2 a = t_model[int2(0, row)], b = t_model[int2(1, row)], e = t_model[int2(2, row)];
    const float2 f = t_model[int2(3, row)], g = t_model[int2(4, row)], k = t_model[int2(5, row)];
    const float nearness = min(own, k.y);
    const float sc = 2.0 / float(h);
    const float X = (c.x - 0.5 * float(w)) * sc, Y = (c.y - 0.5 * float(h)) * sc;
    // theta 0..10 = a.x a.y b.x b.y e.x e.y f.x f.y g.x g.y k.x
    r.x = a.x + a.y * X + b.x * Y + f.x * X * X + f.y * X * Y + nearness * (g.x + k.x * X);
    r.y = b.y + e.x * X + e.y * Y + f.x * X * Y + f.y * Y * Y + nearness * (g.y + k.x * Y);
    return fin(r.x) && fin(r.y);
}
struct O { float2 v : SV_Target0; float m : SV_Target1; };
O ps(V i) {
    const uint2 p = uint2(i.pos.xy);
    const float d = t_depth[p];
    const float z = i.pos.z;
    // Only where this draw is the visible surface (its depth is the scene's).
    if (!isfinite(d) || abs(z - d) > 2e-6) discard;
    if (!(i.prev.w > 1e-6)) discard;
    const float2 n = i.prev.xy / i.prev.w;
    const float2 q = float2((n.x * 0.5 + 0.5) * w, (0.5 - n.y * 0.5) * h);
    const float2 c = i.pos.xy - float2(jx, jy);
    const float2 v = q - c;
    if (!all(isfinite(v)) || dot(v, v) > 65536.0) discard;
    O o;
    o.v = v;
    o.m = 1.0;
    if (((p.x | p.y) & 7u) == 0u) u_count.InterlockedAdd(0, 1u);
    // Learning (core/replay_movers.hpp): is this draw, where it is seen, apart from the screen's own motion?
    const uint mode = ref_mode & 255u;
    if (mode != 0u && slot != 0xffffffffu && ((p.x | p.y) & 3u) == 0u) {
        float2 r = 0;
        bool have = false;
        if (mode == 1u) have = camera_at(c, d, r);
        else have = screen_at(c, d, r);
        if (have) {
            u_count.InterlockedAdd(8u + slot * 8u, 1u);
            const float2 e = v - r;
            const float lim = apart_px + apart_rel * length(r);
            if (dot(e, e) > lim * lim) u_count.InterlockedAdd(12u + slot * 8u, 1u);
        }
    }
    return o;
}
)";

} // namespace na::j2::replay_motion
