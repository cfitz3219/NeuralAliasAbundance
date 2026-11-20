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
Texture2D<float4> t_prev_output : register(t5);
Texture2D<float4> t_prev_screen : register(t6);
RWByteAddressBuffer u_out  : register(u0);
cbuffer C : register(b0) {
    uint src, base, w, h;          // which image (0..3), first float, its size
    uint mw, mh, pad0, pad1;       // motion size for points, comparison grid size for tiles
    float sx, sy, djx, djy;        // DLSS's motion scale; jitter now minus jitter before (pixels)
    uint4 tileCells[2];
};
static const uint kThumbW = 128, kThumbH = 72, kTaps = 4, kTile = 32, kGridW = 96, kGridH = 54, kValues = 9;
static const float2 kTileAt[8] = { float2(.22,.30), float2(.42,.26), float2(.62,.30), float2(.80,.34),
                                   float2(.20,.66), float2(.38,.72), float2(.60,.70), float2(.78,.64) };
float load_luma(uint which, int2 p, uint ww, uint hh) {
    p = clamp(p, int2(0, 0), int2(int(ww) - 1, int(hh) - 1));
    float4 c;
    [branch] switch (which) {
        case 0: c = t_prev.Load(int3(p, 0)); break;
        case 1: c = t_input.Load(int3(p, 0)); break;
        case 2: c = t_output.Load(int3(p, 0)); break;
        case 5: c = t_prev_output.Load(int3(p, 0)); break;
        case 6: c = t_prev_screen.Load(int3(p, 0)); break;
        default: c = t_screen.Load(int3(p, 0)); break;
    }
    return max(dot(c.rgb, float3(.2126, .7152, .0722)), 0) + 1e-4;
}
float load_log(uint which, int2 p, uint ww, uint hh) { return log2(load_luma(which, p, ww, hh)); }
void store(uint i, float v) { u_out.Store((base + i) * 4, asuint(v)); }

[numthreads(8, 8, 1)]
void thumb(uint3 id : SV_DispatchThreadID) {
    if (id.x >= kThumbW || id.y >= kThumbH) return;
    float s = 0;
    const uint tw = pad0 != 0 ? mw : w, th = pad0 != 0 ? mh : h;
    const int2 origin = pad0 != 0 ? int2((w - mw) / 2, (h - mh) / 2) : int2(0,0);
    for (uint b = 0; b < kTaps; ++b)
        for (uint a = 0; a < kTaps; ++a) {
            const int x = int((float(id.x) + (float(a) + .5) / kTaps) / kThumbW * float(tw));
            const int y = int((float(id.y) + (float(b) + .5) / kTaps) / kThumbH * float(th));
            s += load_log(src, int2(x, y) + origin, w, h);
        }
    store(id.y * kThumbW + id.x, s / float(kTaps * kTaps));
}

[numthreads(32, 32, 1)]
void tiles(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    const uint k = gid.x;
    const uint cell = tileCells[k / 4][k % 4];
    int2 centre = int2(kTileAt[k] * float2(mw, mh));
    if (cell != 0) {
        uint2 xy = uint2((cell - 1) % kThumbW, (cell - 1) / kThumbW);
        centre = int2((2 * xy + 1) * uint2(mw, mh) / (2 * uint2(kThumbW, kThumbH)));
    }
    const int left = centre.x - int(kTile / 2), top = centre.y - int(kTile / 2);
    const int2 grid_pixel = int2(left + int(tid.x), top + int(tid.y));
    if (pad0 != 0) {
        // Only the alternate screen dispatch sets pad0. The grid is the unscaled scene.
        const uint2 margin = uint2(w - mw, h - mh);
        const uint nx = 1 + (margin.x & 1);
        const uint2 origin = margin / 2 + uint2(gid.y % nx, gid.y / nx);
        store((gid.y * 8 + k) * kTile * kTile + tid.y * kTile + tid.x,
              load_log(src, grid_pixel + int2(origin), w, h));
        return;
    }
    float value;
    if (mw == w && mh == h) value = load_log(src, grid_pixel, w, h);
    else {
        // One comparison footprint in screen pixels; map each pixel centre to
        // the equivalent point in the source and filter linear luminance.
        const float2 mapped = (float2(grid_pixel) + .5) * float2(w, h) / float2(mw, mh) - .5;
        const int2 p = int2(floor(mapped));
        const float2 f = frac(mapped);
        const float a = lerp(load_luma(src, p, w, h), load_luma(src, p + int2(1, 0), w, h), f.x);
        const float b = lerp(load_luma(src, p + int2(0, 1), w, h), load_luma(src, p + int2(1, 1), w, h), f.x);
        value = log2(lerp(a, b, f.y));
    }
    store((k * kTile + tid.y) * kTile + tid.x, value);
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

bool patch_visible(float2 q) {
    return all(q >= 1.5) && all(q <= float2(w, h) - 1.5);
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
    const float2 centre = float2(x, y);
    if (any(!isfinite(v)) || any(!isfinite(dj)) || !patch_visible(centre) || !patch_visible(centre + v) ||
        !patch_visible(centre + v - dj) || !patch_visible(centre + v + dj)) {
        [unroll] for (uint k = 0; k < kValues; ++k) store(at + k, -1);
        return;
    }
    const float2 shift[6] = { float2(0, 0), v, v - dj, v + dj, .5 * v - dj, 2 * v - dj };
    float e[6] = { 0, 0, 0, 0, 0, 0 };
    float contrast = 0, delta[9], sum = 0;
    uint support = 0, mask = 0;
    [unroll] for (int b = -1; b <= 1; ++b)
        for (int a = -1; a <= 1; ++a) {
            const float2 q = float2(x + a, y + b);
            const float cv = log_at(1, q);
            const float pv = log_at(0, q);
            const float d = cv - pv;
            const uint tap = (b + 1) * 3 + a + 1;
            delta[tap] = d;
            if (max(cv, pv) > -10) { sum += d; ++support; mask |= 1u << tap; }
            e[0] += abs(d);
            [unroll] for (uint k = 1; k < 6; ++k) e[k] += abs(cv - log_at(0, q + shift[k]));
            contrast += .5 * (abs(log_at(0, q + float2(1, 0)) - pv) + abs(log_at(0, q + float2(0, 1)) - pv));
        }
    [unroll] for (uint k = 0; k < 6; ++k) store(at + k, k >= 4 && !patch_visible(centre + shift[k]) ? -1 : e[k] / 9);
    store(at + 6, contrast / 9);
    store(at + 7, length(v));
    float structure = 0;
    const float inv_support = rcp(float(max(support, 1u)));
    [unroll] for (uint i = 0; i < 9; ++i)
        if (mask & (1u << i)) structure += abs(delta[i] - sum * inv_support);
    store(at + 8, support >= 5 ? structure * inv_support : -1);
}
