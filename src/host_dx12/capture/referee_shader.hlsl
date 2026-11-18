// The referee's card programs (capture/referee_gpu.cpp). The processor reference of the same
// sampling is in core/referee.hpp (SampleThumb, SampleTiles, SamplePoints): same cells, same tile
// places, same grid, same patch and the same six placements, so the card and a replayed capture
// give the same numbers. Compiled with cs_5_0. t0 previous input, t1 input, t2 output, t3 screen,
// t4 motion; u0 the result buffer (floats, layout in core/referee.hpp).
Texture2D<float4> t_prev   : register(t0);
Texture2D<float4> t_input  : register(t1);
Texture2D<float4> t_output : register(t2);
Texture2D<float4> t_screen : register(t3);
Texture2D<float4> t_motion : register(t4);
RWByteAddressBuffer u_out  : register(u0);
cbuffer C : register(b0) {
    uint src, base, w, h;          // which image (0..3), first float, its size
    uint mw, mh, pad0, pad1;       // motion size
    float sx, sy, djx, djy;        // DLSS's motion scale; jitter now minus jitter before (pixels)
};
static const uint kThumbW = 128, kThumbH = 72, kTaps = 4, kTile = 32, kGridW = 96, kGridH = 54, kValues = 8;
static const float2 kTileAt[8] = { float2(.22,.30), float2(.42,.26), float2(.62,.30), float2(.80,.34),
                                   float2(.20,.66), float2(.38,.72), float2(.60,.70), float2(.78,.64) };
float luma(float4 c) { return log2(max(dot(c.rgb, float3(.2126, .7152, .0722)), 0) + 1e-4); }
float load_log(uint which, int2 p, uint ww, uint hh) {
    p = clamp(p, int2(0, 0), int2(int(ww) - 1, int(hh) - 1));
    float4 c;
    [branch] switch (which) {
        case 0: c = t_prev.Load(int3(p, 0)); break;
        case 1: c = t_input.Load(int3(p, 0)); break;
        case 2: c = t_output.Load(int3(p, 0)); break;
        default: c = t_screen.Load(int3(p, 0)); break;
    }
    return luma(c);
}
void store(uint i, float v) { u_out.Store((base + i) * 4, asuint(v)); }

[numthreads(8, 8, 1)]
void thumb(uint3 id : SV_DispatchThreadID) {
    if (id.x >= kThumbW || id.y >= kThumbH) return;
    float s = 0;
    for (uint b = 0; b < kTaps; ++b)
        for (uint a = 0; a < kTaps; ++a) {
            const int x = int((float(id.x) + (float(a) + .5) / kTaps) / kThumbW * float(w));
            const int y = int((float(id.y) + (float(b) + .5) / kTaps) / kThumbH * float(h));
            s += load_log(src, int2(x, y), w, h);
        }
    store(id.y * kThumbW + id.x, s / float(kTaps * kTaps));
}

[numthreads(32, 32, 1)]
void tiles(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    const uint k = gid.x;
    const int left = int(kTileAt[k].x * float(w)) - int(kTile / 2), top = int(kTileAt[k].y * float(h)) - int(kTile / 2);
    store((k * kTile + tid.y) * kTile + tid.x, load_log(src, int2(left + int(tid.x), top + int(tid.y)), w, h));
}

// Bilinear in log luminance, pixel-centre coordinates (0.5 = the middle of column 0).
float log_at(uint which, float2 q) {
    const float2 f = q - .5;
    const int2 i0 = int2(floor(f));
    const float2 a = f - float2(i0);
    const float v00 = load_log(which, i0, w, h), v10 = load_log(which, i0 + int2(1, 0), w, h);
    const float v01 = load_log(which, i0 + int2(0, 1), w, h), v11 = load_log(which, i0 + int2(1, 1), w, h);
    return lerp(lerp(v00, v10, a.x), lerp(v01, v11, a.x), a.y);
}

[numthreads(8, 6, 1)]
void points(uint3 id : SV_DispatchThreadID) {
    if (id.x >= kGridW || id.y >= kGridH) return;
    const uint at = (id.y * kGridW + id.x) * kValues;
    const float x = floor((float(id.x) + .5) / kGridW * float(w)) + .5;
    const float y = floor((float(id.y) + .5) / kGridH * float(h)) + .5;
    const uint mx = min(uint(x / float(w) * float(mw)), mw - 1), my = min(uint(y / float(h) * float(mh)), mh - 1);
    const float2 m = t_motion.Load(int3(mx, my, 0)).xy;
    const float2 v = m * float2(sx, sy), dj = float2(djx, djy);
    if (any(!isfinite(v))) { [unroll] for (uint k = 0; k < kValues; ++k) store(at + k, -1); return; }
    const float2 shift[6] = { float2(0, 0), v, v - dj, v + dj, .5 * v - dj, 2 * v - dj };
    float e[6] = { 0, 0, 0, 0, 0, 0 };
    float contrast = 0;
    for (int b = -1; b <= 1; ++b)
        for (int a = -1; a <= 1; ++a) {
            const float2 q = float2(x + a, y + b);
            const float cv = log_at(1, q);
            [unroll] for (uint k = 0; k < 6; ++k) e[k] += abs(cv - log_at(0, q + shift[k]));
            const float pv = log_at(0, q);
            contrast += .5 * (abs(log_at(0, q + float2(1, 0)) - pv) + abs(log_at(0, q + float2(0, 1)) - pv));
        }
    [unroll] for (uint k = 0; k < 6; ++k) store(at + k, e[k] / 9);
    store(at + 6, contrast / 9);
    store(at + 7, length(v));
}
