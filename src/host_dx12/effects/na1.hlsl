// The NA1 enhancer's card programs (docs/NA1-ENHANCER.md sections 2 to 4), one entry point per pass.
// Compiled at build time with FXC as cs_5_0 (cmake/na1_shaders.cmake); used by effects/na1.cpp.
//
// KEPT IN STEP BY HAND with the processor reference in src/core/na1_reference.hpp: the same
// constants, the same decisions where the contract is silent, and the arithmetic written in the
// same order, so that tests/host/na1_test.cpp can compare the two pixel by pixel. Change one and
// change the other.
//
// cs_5_0 rules kept here: every picture is read through a shader resource view (no typed UAV
// loads), a pass writes only the texel or the cell its own thread owns, and loops have constant
// bounds.

Texture2D<float4> Picture : register(t0);   // the DLSS output as stored
Texture2D<float>  Depth   : register(t1);   // raw device depth
Texture2D<float2> Yq      : register(t2);   // (y, q) of the level a pass works on
Texture2D<float>  Scalar  : register(t3);   // BlurY: the level after x. Down: the blurred level. Compose: b1
Texture2D<float>  B2      : register(t4);
Texture2D<float2> Yq2     : register(t5);
Texture2D<float>  B3      : register(t6);
Texture2D<float2> Yq4     : register(t7);
Texture2D<float>  DeltaA  : register(t8);   // lighting layers from the other parts, in stops
Texture2D<float>  DeltaB  : register(t9);
Texture2D<float4> After   : register(t10);  // Measure: the picture after NA1 (the picture itself when unchanged)
RWTexture2D<float4> OutColour : register(u0);
RWTexture2D<float2> OutYq     : register(u1);
RWTexture2D<float>  OutScalar : register(u2);
RWByteAddressBuffer Cells     : register(u3);

// One per frame. The layout is pinned by static_asserts on FrameConstants in na1.cpp.
cbuffer FrameC : register(b0) {
    uint2 size; uint2 size2;                              // full, half
    uint2 size4; uint srgb; uint reversed;                // quarter; 8-bit sRGB picture; depth 1 is near
    float d_texture; float d_light; float d_colour; float d_key;
    float d_part_a; float d_part_b; uint has_a; uint has_b;
    float s_mid; float s_m0; float s_w_dark; float s_p85;
    float s_p97; float s_tint_r; float s_tint_g; float s_tint_b;
    int c_mode; float c_x0; float c_dx; float c_mean_y;   // display curve: 0 display-referred, 1 table, 2 assumed
    float4 c_cy[4];                                       // the table's 16 knots
    float measure_key; uint unchanged; uint frame_pad0; uint frame_pad1;
};
// One per dispatch (root constants). Pinned by static_asserts on PassConstants in na1.cpp.
cbuffer PassC : register(b1) {
    uint2 lsize;          // size of the level this pass reads
    int taps;             // blur: number of offsets each side (radius / spacing)
    int spacing;          // blur: distance between offsets
    float half_radius;    // blur: 0.5 * radius
    uint3 pass_pad;
};

static const float kLumFloor = 1e-3;
static const float kSkyBelow = 1e-5;
static const float kEdgeStop = 0.04;
static const float kLayerGain[3] = { 0.5, 0.4, 0.25 };
static const float kSoftKnee = 0.6;
static const float kLightSlope = 0.35;
static const float kNearBlack = 0.000492;
static const float kPureWhite = 0.9957;
static const uint kCellsW = 128, kCellsH = 72, kCellFloats = 24;
static const uint kBlurTapsMost = 6;     // the three levels use 3, 6 and 6 offsets a side
static const uint kCellStepsMost = 128;  // samples along one side of a cell, at stride 2, for any legal picture

// Tested on the bits so that no compiler setting can fold it away.
bool is_number(float v) { return (asuint(v) & 0x7f800000u) != 0x7f800000u; }
float finite1(float v) { return is_number(v) ? v : 0.0; }
float3 finite3(float3 v) { return float3(finite1(v.x), finite1(v.y), finite1(v.z)); }

// Written out term by term: the reference adds in this order.
float lum(float3 c)
{
    const float l = c.x * 0.2126 + c.y * 0.7152 + c.z * 0.0722;
    return l > 0.0 ? l : 0.0;
}
float log_lum(float l) { return log2(l + kLumFloor); }

float srgb_decode(float v)
{
    const float low = v / 12.92;
    const float high = pow(abs((v + 0.055) / 1.055), 2.4);
    return !(v > 0.0) ? 0.0 : (v <= 0.04045 ? low : high);
}
float srgb_encode(float v)
{
    v = saturate(finite1(v));
    const float low = v * 12.92;
    const float high = 1.055 * pow(abs(v), 1.0 / 2.4) - 0.055;
    return v <= 0.0031308 ? low : high;
}
// One pixel as the passes see it: scene-linear, channels that are not numbers set to zero.
float3 load_linear(float3 stored)
{
    float3 c = finite3(stored);
    if (srgb != 0) c = float3(srgb_decode(c.x), srgb_decode(c.y), srgb_decode(c.z));
    return c;
}

float knot(uint i)
{
    const float4 v = c_cy[i >> 2];
    const uint k = i & 3u;
    return k == 0u ? v.x : (k == 1u ? v.y : (k == 2u ? v.z : v.w));
}
// How bright a scene value is on the screen, 0..1 (section 4).
float shown(float y)
{
    float result;
    if (c_mode == 0) {
        result = saturate(exp2(y) - kLumFloor);
    } else if (c_mode == 1) {
        float t = (y - c_x0) / c_dx;
        t = t < 0.0 ? 0.0 : (t > 15.0 ? 15.0 : t);
        const uint i = min(uint(t), 14u);
        const float f = t - float(i);
        const float a = knot(i), b = knot(i + 1u);
        result = a + (b - a) * f;
    } else {
        const float t = 0.18 * exp2(y - c_mean_y);
        result = t / (1.0 + t);
    }
    return result;
}

// Depth key: proportional to one over distance, whatever the camera. Not a number counts as sky.
float depth_key(float d)
{
    const float q = reversed != 0 ? d : 1.0 - d;
    return (is_number(q) && q > 0.0) ? q : 0.0;
}
float edge_weight(float qa, float qb)
{
    const float rel = 2.0 * (qa - qb) / (qa + qb + 1e-6);
    const float s = rel / kEdgeStop;
    return exp(-0.5 * s * s);
}

// ------------------------------------------------------------------ pass 1: (y, q)
[numthreads(8, 8, 1)]
void Prep(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size.x || id.y >= size.y) return;
    const int3 p = int3(int2(id.xy), 0);
    const float3 rgb = load_linear(Picture.Load(p).rgb);
    OutYq[id.xy] = float2(log_lum(lum(rgb)), depth_key(Depth.Load(p)));
}

// ------------------------------------------------------------------ passes 2, 4, 6: blur
// One direction of a blur that does not cross depth steps. Taps are added nearest offset first,
// the lower coordinate before the higher. `vertical` is a literal at each call, so each entry
// point keeps only its own branch.
void blur_tap(int2 n, bool vertical, float spatial, float qc, inout float num, inout float den)
{
    const float2 yq = Yq.Load(int3(n, 0));
    const float v = vertical ? Scalar.Load(int3(n, 0)) : yq.x;
    const float wt = spatial * edge_weight(yq.y, qc);
    num += wt * v;
    den += wt;
}
float blur(int2 c, bool vertical)
{
    const int2 last = int2(lsize) - int2(1, 1);
    const float2 centre = Yq.Load(int3(c, 0));
    const float qc = centre.y;
    float num = vertical ? Scalar.Load(int3(c, 0)) : centre.x;
    float den = 1.0;
    [loop] for (int k = 0; k < int(kBlurTapsMost); ++k) {
        if (k >= taps) break;
        const int o = (k + 1) * spacing;
        const float s = float(o) / half_radius;
        const float spatial = exp(-0.5 * s * s);
        int2 lower = c, higher = c;
        if (vertical) {
            lower.y = clamp(c.y - o, 0, last.y);
            higher.y = clamp(c.y + o, 0, last.y);
        } else {
            lower.x = clamp(c.x - o, 0, last.x);
            higher.x = clamp(c.x + o, 0, last.x);
        }
        blur_tap(lower, vertical, spatial, qc, num, den);
        blur_tap(higher, vertical, spatial, qc, num, den);
    }
    return num / den;
}
[numthreads(8, 8, 1)]
void BlurX(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= lsize.x || id.y >= lsize.y) return;
    OutScalar[id.xy] = blur(int2(id.xy), false);
}
[numthreads(8, 8, 1)]
void BlurY(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= lsize.x || id.y >= lsize.y) return;
    OutScalar[id.xy] = blur(int2(id.xy), true);
}

// ------------------------------------------------------------------ passes 3, 5: halve
// Halves a blurred level, keeping the nearest surface's depth.
[numthreads(8, 8, 1)]
void Down(uint3 id : SV_DispatchThreadID)
{
    const uint2 out_size = (lsize + uint2(1u, 1u)) / 2u;
    if (id.x >= out_size.x || id.y >= out_size.y) return;
    const uint2 last = lsize - uint2(1u, 1u);
    float q[4];
    float b[4];
    float q_near = 0.0;
    uint k;
    [unroll] for (k = 0u; k < 4u; ++k) {
        const uint2 s = min(id.xy * 2u + uint2(k & 1u, k >> 1), last);
        const int3 p = int3(int2(s), 0);
        q[k] = Yq.Load(p).y;
        b[k] = Scalar.Load(p);
        q_near = max(q_near, q[k]);
    }
    float num = 0.0, den = 0.0;
    [unroll] for (k = 0u; k < 4u; ++k) {
        const float wt = edge_weight(q[k], q_near);
        num += wt * b[k];
        den += wt;
    }
    OutYq[id.xy] = float2(num / den, q_near);
}

// ------------------------------------------------------------------ pass 7: compose
// Depth-aware upsampling of a coarse level (section 2.2). `quarter` is a literal at each call.
float up(bool quarter, uint2 xy, float q)
{
    const float factor = quarter ? 4.0 : 2.0;
    const int2 last = int2(quarter ? size4 : size2) - int2(1, 1);
    const float2 t = (float2(xy) + 0.5) / factor - 0.5;
    const float2 t0 = floor(t);
    const float2 f = t - t0;
    const int2 i0 = int2(t0);
    float num = 0.0, den = 0.0;
    [unroll] for (int j = 0; j < 2; ++j) {
        [unroll] for (int i = 0; i < 2; ++i) {
            const int3 p = int3(clamp(i0 + int2(i, j), int2(0, 0), last), 0);
            const float bilinear = (i != 0 ? f.x : 1.0 - f.x) * (j != 0 ? f.y : 1.0 - f.y);
            const float qc = quarter ? Yq4.Load(p).y : Yq2.Load(p).y;
            const float v = quarter ? B3.Load(p) : B2.Load(p);
            const float wt = bilinear * (edge_weight(qc, q) + 1e-3);
            num += wt * v;
            den += wt;
        }
    }
    return num / den;
}
float3 neutral_colour()
{
    return float3((1.0 - s_w_dark) + s_w_dark * s_tint_r,
                  (1.0 - s_w_dark) + s_w_dark * s_tint_g,
                  (1.0 - s_w_dark) + s_w_dark * s_tint_b);
}
[numthreads(8, 8, 1)]
void Compose(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size.x || id.y >= size.y) return;
    const int3 p = int3(int2(id.xy), 0);
    const float4 stored = Picture.Load(p);
    const float2 yq = Yq.Load(p);
    const float y = yq.x, q = yq.y;
    const float b1 = Scalar.Load(p);
    const float u2 = up(false, id.xy, q);
    const float u3 = up(true, id.xy, q);
    const float layer[3] = { y - b1, b1 - u2, u2 - u3 };
    const float sh = shown(y);
    const float pool = s_w_dark * 0.4 * saturate((sh - s_p85) / (s_p97 - s_p85 + 1e-9));
    float add = 0.0;
    [unroll] for (int k = 0; k < 3; ++k) {
        const float d = layer[k];
        const float g = 1.0 + kLayerGain[k] * d_texture;
        const float s = abs(d) / kSoftKnee;
        float e = d * (g - 1.0) / (1.0 + s * s);
        if (d > 0.0) e *= 1.0 + pool;
        add += e;
    }
    if (!(q < kSkyBelow)) add += -kLightSlope * d_light * (u3 - s_mid);
    const float da = has_a != 0 ? finite1(DeltaA.Load(p)) : 0.0;
    const float db = has_b != 0 ? finite1(DeltaB.Load(p)) : 0.0;
    add += da * d_part_a + db * d_part_b;
    const float hi = saturate((sh - 0.55) / 0.40);
    const float black_top = s_m0 / 16.0, black_span = s_m0 / 16.0 - s_m0 / 64.0;
    const float lo = black_span > 0.0 ? saturate((black_top - sh) / black_span) : 0.0;  // m0 of zero: no black to protect
    // (1 - hi) to the power 1.5, written as t * sqrt(t): exactly zero at full white on every card.
    const float room = 1.0 - hi;
    add = add > 0.0 ? add * (room * sqrt(room)) : add * (1.0 - lo);
    // Exactness rule: a pixel nothing changes is stored back as it was loaded, bit for bit.
    if (add == 0.0 && d_colour == 0.0 && d_key == 0.0) {
        OutColour[id.xy] = stored;
        return;
    }
    float3 rgb = load_linear(stored.rgb);
    rgb *= exp2(add);
    if (d_colour > 0.0) {
        const float yl = lum(rgb);
        const float3 r = yl * neutral_colour();
        const float3 t = (rgb - r) / (yl + 1e-5);
        const float dist2 = t.x * t.x + t.y * t.y + t.z * t.z;
        const float grow = 1.0 + 0.6 * d_colour * (1.0 - saturate(sqrt(dist2) / 1.2));
        rgb = max(r + grow * (rgb - r), 0.0);
    }
    rgb *= exp2(d_key);
    if (srgb != 0) rgb = float3(srgb_encode(rgb.x), srgb_encode(rgb.y), srgb_encode(rgb.z));
    OutColour[id.xy] = float4(rgb, stored.a);  // alpha always passes through
}

// ------------------------------------------------------------------ pass 8: measure
// The picture before and after NA1 reduced to 128 x 72 cells of 24 sums (section 3). One thread
// owns one cell: it walks the cell's pixels at stride 2, row by row, and stores the 24 sums.
float3 measure_load(bool after_picture, int2 p)
{
    const int3 at = int3(p, 0);
    float3 stored;
    if (after_picture) stored = After.Load(at).rgb; else stored = Picture.Load(at).rgb;
    return max(load_linear(stored), 0.0);  // a channel below zero counts as zero here
}
float log_at(bool after_picture, int2 p)
{
    const int2 c = clamp(p, int2(0, 0), int2(size) - int2(1, 1));
    return log_lum(lum(measure_load(after_picture, c)));
}
bool solid_at(int2 p)
{
    const int2 c = clamp(p, int2(0, 0), int2(size) - int2(1, 1));
    return !(depth_key(Depth.Load(int3(c, 0))) < kSkyBelow);
}
bool ring(int2 p, int r)
{
    const bool a = solid_at(p + int2(-r, 0));
    const bool b = solid_at(p + int2(r, 0));
    const bool c = solid_at(p + int2(0, -r));
    const bool d = solid_at(p + int2(0, r));
    return a || b || c || d;
}
float fine_at(bool after_picture, int2 p, float centre)
{
    return centre - 0.25 * (log_at(after_picture, p + int2(-2, 0)) + log_at(after_picture, p + int2(2, 0)) +
                            log_at(after_picture, p + int2(0, -2)) + log_at(after_picture, p + int2(0, 2)));
}
[numthreads(8, 8, 1)]
void Measure(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= kCellsW || id.y >= kCellsH) return;
    const int x0 = int(id.x * size.x / kCellsW), x1 = int((id.x + 1u) * size.x / kCellsW);
    const int y0 = int(id.y * size.y / kCellsH), y1 = int((id.y + 1u) * size.y / kCellsH);
    const float3 neutral = neutral_colour();
    // The 24 sums, in the order of na::na1::ref::Cell.
    float s[24];
    uint n;
    [unroll] for (n = 0u; n < 24u; ++n) s[n] = 0.0;
    [loop] for (uint j = 0u; j < kCellStepsMost; ++j) {
        const int py = y0 + int(j) * 2;
        if (py >= y1) break;
        [loop] for (uint i = 0u; i < kCellStepsMost; ++i) {
            const int px = x0 + int(i) * 2;
            if (px >= x1) break;
            const int2 at = int2(px, py);
            const float3 b = measure_load(false, at);
            const float3 a = measure_load(true, at);
            const float lum_b = lum(b), lum_a = lum(a);
            const float y_b = log_lum(lum_b), y_a = log_lum(lum_a);
            const float shown_b = shown(y_b), shown_a = shown(y_a);
            const float max_b = max(b.x, max(b.y, b.z)), min_b = min(b.x, min(b.y, b.z));
            const float max_a = max(a.x, max(a.y, a.z)), min_a = min(a.x, min(a.y, a.z));
            const float disp_b = max_b * (shown_b + 1e-5) / (lum_b + 1e-3);
            const float disp_a = max_a * (shown_a + 1e-5) / (lum_a + 1e-3);
            s[0] += 1.0;                                 // count
            s[1] += y_b;
            s[2] += y_a;
            s[3] += shown_b;
            s[4] += shown_a;
            if (disp_b <= kNearBlack) s[5] += 1.0;      // near black before, after
            if (disp_a <= kNearBlack) s[6] += 1.0;
            if (disp_b >= kPureWhite) s[7] += 1.0;      // pure white before, after
            if (disp_a >= kPureWhite) s[8] += 1.0;
            if (solid_at(at)) {
                s[9] += 1.0;                             // solid count
                s[10] += y_b;
                s[11] += b.x;
                s[12] += b.y;
                s[13] += b.z;
                s[14] += (max_b - min_b) / (max_b + 1e-4);   // colour strength
                s[15] += (max_a - min_a) / (max_a + 1e-4);
                const float3 tb = b / (lum_b + 1e-5) - neutral;
                const float3 ta = a / (lum_a + 1e-5) - neutral;
                s[16] += sqrt(tb.x * tb.x + tb.y * tb.y + tb.z * tb.z);  // variety
                s[17] += sqrt(ta.x * ta.x + ta.y * ta.y + ta.z * ta.z);
                const float fine_b = fine_at(false, at, y_b);
                const float fine_a = fine_at(true, at, y_a);
                s[18] += fine_b;
                s[19] += fine_b * fine_b;
                s[20] += fine_a;
                s[21] += fine_a * fine_a;
            } else if (ring(at, 8) && !ring(at, 3)) {
                s[22] += 1.0;                            // the sky band beside a solid edge
                s[23] += abs(y_a - y_b - measure_key);   // an even key shift is not edge glow
            }
        }
    }
    const uint base = (id.y * kCellsW + id.x) * kCellFloats * 4u;
    [unroll] for (n = 0u; n < 24u; ++n) Cells.Store(base + n * 4u, asuint(s[n]));
}
