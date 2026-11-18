#pragma once

namespace na::depth::unjitter {

// core/depth_unjitter.hpp uses the same slope and curvature rule.
inline constexpr char kSource[] = R"(
Texture2D<float> depth : register(t0);
cbuffer Args : register(b0) {
    float2 jitter; int2 last; float curve; float noise;
#ifdef NA_RASTER_EXPANSION
    float2 inverse_size;
#endif
};
float4 vs(uint i : SV_VertexID) : SV_Position {
    float2 p = float2((i << 1) & 2, i & 2);
    return float4(p * 2 - 1, 0, 1);
}
bool continues(float c, float n1, float n2, float floor_) {
    precise float first = n1 - c;
    precise float twice = 2.0 * n1;
    precise float second = (c - twice) + n2;
    precise float allowed = curve * abs(first);
    return abs(second) <= max(allowed, floor_);
}
float slope(float m2, float m1, float c, float p1, float p2, float j, bool minus_inside, bool plus_inside) {
    if (j == 0.0) return 0.0;
    precise float floor_ = noise * max(abs(c), max(abs(m1), abs(p1)));
    bool plus = plus_inside && continues(c, p1, p2, floor_);
    bool minus = minus_inside && continues(c, m1, m2, floor_);
    precise float g_plus = p1 - c;
    precise float g_minus = c - m1;
    if (j > 0.0) return plus ? g_plus : (minus ? g_minus : 0.0);
    return minus ? g_minus : (plus ? g_plus : 0.0);
}
float at(int2 p) { return depth.Load(int3(clamp(p, int2(0, 0), last), 0)); }
float ps(float4 position : SV_Position) : SV_Depth {
    int2 p = int2(position.xy);
    float2 offset = jitter;
#ifdef NA_RASTER_EXPANSION
    offset += (position.xy - 0.5 * float2(last + 1)) * inverse_size;
#endif
    float c = at(p);
    float gx = slope(at(p + int2(-2, 0)), at(p + int2(-1, 0)), c, at(p + int2(1, 0)), at(p + int2(2, 0)), offset.x,
        p.x >= 2, p.x + 2 <= last.x);
    float gy = slope(at(p + int2(0, -2)), at(p + int2(0, -1)), c, at(p + int2(0, 1)), at(p + int2(0, 2)), offset.y,
        p.y >= 2, p.y + 2 <= last.y);
    precise float mx = offset.x * gx;
    precise float my = offset.y * gy;
    precise float r = (c + mx) + my;
    return clamp(r, 0.0, 1.0);
})";

} // namespace na::depth::unjitter
