#include "host_dx12/fg/hud_move.hpp"

#include "host_dx12/color/color_space.hpp"

#include <d3dcompiler.h>

#include <cmath>
#include <cstring>
#include <string>

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace na::fg {
namespace {
// Counters written per classified frame (zeroed before the frame, read back later).
constexpr uint32_t kCounters = 8; // tiles_moving, tiles_fixed, pixels_moving, pixels_fixed, shift_sum (1/16 px), shift_n, -, -
constexpr uint32_t kTileWords = 4; // tile class: class, vx, vy (float bits), interface pixels in the tile
constexpr uint32_t kStatWords = 8; // tile stats: interface, moved, fixed, matched vx, vy (1/16 px), all vx, vy (1/16 px)
constexpr uint32_t kSrvs = 5, kUavs = 6, kPerPage = kSrvs + kUavs;

// Shared by the three passes. NA_PICTURE as fg/hud_paste.cpp (display terms for the interface rule).
constexpr char kCommon[] = R"(
#if NA_PICTURE == 0
float3 display(float3 c) { return saturate(c); }
#elif NA_PICTURE == 1
float3 linear80(float3 e)
{
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 p = pow(saturate(e), 1.0 / m2);
    return pow(max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1) * 125.0;
}
float3 display(float3 c) { return min(pow(max(linear80(c), 0.0), 1.0 / 2.2), 1.0); }
#else
float3 display(float3 c) { return min(pow(max(c, 0.0), 1.0 / 2.2), 1.0); }
#endif
Texture2D<float4> real_n : register(t0);
Texture2D<float4> free_n : register(t1);
Texture2D<float4> real_p : register(t2);
Texture2D<float4> free_p : register(t3);
Texture2D<float2> motion : register(t4);
RWTexture2D<float4> generated : register(u0);
RWByteAddressBuffer tile_stats : register(u1);
RWByteAddressBuffer score_prev : register(u2);
RWByteAddressBuffer score_out : register(u3);
RWByteAddressBuffer tile_class : register(u4);
RWByteAddressBuffer counters : register(u5);
cbuffer K : register(b0) { uint w, h; float2 mv_scale; float t; uint forget, tw, th; };
static const float kSame = 24.0 / 255.0; // the same interface pixel after a move (anti-aliasing at sub-pixel offsets)
bool inside(int2 p) { return p.x >= 0 && p.y >= 0 && p.x < int(w) && p.y < int(h); }
bool hud_n(int2 p) { if (!inside(p)) return false; float3 d = abs(display(real_n[p].rgb) - display(free_n[p].rgb)); return max(d.r, max(d.g, d.b)) > 4.0 / 255.0; }
bool hud_p(int2 p) { if (!inside(p)) return false; float3 d = abs(display(real_p[p].rgb) - display(free_p[p].rgb)); return max(d.r, max(d.g, d.b)) > 4.0 / 255.0; }
float diff(float3 a, float3 b) { float3 d = abs(display(a) - display(b)); return max(d.r, max(d.g, d.b)); }
// The scene's motion at a display pixel, in display pixels; the previous position is p + v.
float2 scene_motion(int2 p)
{
    uint mw, mh; motion.GetDimensions(mw, mh);
    int2 at = min(int2(uint2(p) * uint2(mw, mh) / uint2(w, h)), int2(mw, mh) - 1);
    float2 v = motion[max(at, 0)] * mv_scale * float2(float(w) / float(mw), float(h) / float(mh));
    return all(isfinite(v)) ? v : float2(0, 0);
}
float4 bilinear_n(float2 x)
{
    int2 i = int2(floor(x)); float2 f = x - float2(i);
    int2 m = int2(w, h) - 1;
    float4 a = real_n[clamp(i, 0, m)], b = real_n[clamp(i + int2(1, 0), 0, m)];
    float4 c = real_n[clamp(i + int2(0, 1), 0, m)], d = real_n[clamp(i + int2(1, 1), 0, m)];
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}
// Candidate motions for the scene under an interface pixel: its own, and the scene's around it (a label
// anchored to the map over a unit moving the other way follows the map, not the unit).
static const float2 kRing[8] = {float2(24, 0), float2(-24, 0), float2(0, 24), float2(0, -24),
                                float2(17, 17), float2(-17, 17), float2(17, -17), float2(-17, -17)};
float2 candidate(int2 p, int k) { int2 off = k == 0 ? int2(0, 0) : int2(kRing[clamp(k - 1, 0, 7)]); return scene_motion(clamp(p + off, 0, int2(w, h) - 1)); }
// The candidates in the order they are tried: when the pixel's own motion differs from what most of the
// scene around it does, the surrounding motion first (a map label over a unit moving the other way), the
// pixel's own after it (a marker that follows the unit).
void ordered(int2 p, out float2 c[9])
{
    float2 own = scene_motion(p), ring[8];
    [unroll] for (int k = 0; k < 8; ++k) ring[k] = candidate(p, k + 1);
    int best = 0, best_n = -1;
    [unroll] for (int a = 0; a < 8; ++a) {
        int n = 0;
        [unroll] for (int b = 0; b < 8; ++b) n += length(ring[a] - ring[b]) < 1.0 ? 1 : 0;
        if (n > best_n) { best_n = n; best = a; }
    }
    float2 consensus = ring[0];
    [unroll] for (int s = 0; s < 8; ++s) if (s == best) consensus = ring[s];
    bool apart = length(own - consensus) >= 1.0 && best_n >= 5;
    c[0] = apart ? consensus : own;
    c[1] = apart ? own : consensus;
    [unroll] for (int r = 0; r < 7; ++r) c[r + 2] = ring[r];
}
float3 bilinear_p(float2 x)
{
    int2 i = int2(floor(x)); float2 f = x - float2(i);
    int2 m = int2(w, h) - 1;
    float3 a = real_p[clamp(i, 0, m)].rgb, b = real_p[clamp(i + int2(1, 0), 0, m)].rgb;
    float3 c = real_p[clamp(i + int2(0, 1), 0, m)].rgb, d = real_p[clamp(i + int2(1, 1), 0, m)].rgb;
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}
)";

// One group per 16x16 tile of frame n: its interface pixels and how many match frame n-1 moved by the
// scene's motion, or in place.
constexpr char kClassify[] = R"(
groupshared uint g_hud, g_moved, g_fixed, g_own;
groupshared int g_vx, g_vy, g_ax, g_ay;
[numthreads(16, 16, 1)]
void main(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    if (gi == 0) { g_hud = 0; g_moved = 0; g_fixed = 0; g_own = 0; g_vx = 0; g_vy = 0; g_ax = 0; g_ay = 0; }
    GroupMemoryBarrierWithGroupSync();
    int2 p = int2(gid.xy * 16 + tid.xy);
    // Frame n's interface: where was it in frame n-1?
    if (hud_n(p)) {
        InterlockedAdd(g_hud, 1);
        InterlockedAdd(g_own, 1);
        float2 v0 = scene_motion(p);
        InterlockedAdd(g_ax, int(round(v0.x * 16.0)));
        InterlockedAdd(g_ay, int(round(v0.y * 16.0)));
        float3 now = real_n[p].rgb;
        if (hud_p(p) && diff(now, real_p[p].rgb) < kSame) InterlockedAdd(g_fixed, 1);
        float2 cands[9];
        ordered(p, cands);
        for (int k = 0; k < 9; ++k) {
            float2 v = cands[k];
            float2 q = float2(p) + v;
            int2 qn = int2(round(q));
            if (inside(qn) && hud_p(qn) && min(diff(now, bilinear_p(q)), diff(now, real_p[qn].rgb)) < kSame) {
                InterlockedAdd(g_moved, 1);
                InterlockedAdd(g_vx, int(round(v.x * 16.0)));
                InterlockedAdd(g_vy, int(round(v.y * 16.0)));
                break;
            }
        }
    }
    // Frame n-1's interface: where is it in frame n? (A label whose pattern repeats along the move also
    // matches itself in place; the end it left behind does not.)
    if (hud_p(p)) {
        InterlockedAdd(g_hud, 1);
        float3 was = real_p[p].rgb;
        if (hud_n(p) && diff(was, real_n[p].rgb) < kSame) InterlockedAdd(g_fixed, 1);
        float2 cands[9];
        ordered(p, cands);
        for (int k = 0; k < 9; ++k) {
            float2 v = cands[k];
            float2 q = float2(p) - v;
            int2 qn = int2(round(q));
            if (inside(qn) && hud_n(qn) && min(diff(was, bilinear_n(q).rgb), diff(was, real_n[qn].rgb)) < kSame) {
                InterlockedAdd(g_moved, 1);
                InterlockedAdd(g_vx, int(round(v.x * 16.0)));
                InterlockedAdd(g_vy, int(round(v.y * 16.0)));
                break;
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0) {
        uint at = (gid.y * tw + gid.x) * 32;
        tile_stats.Store(at, g_hud); tile_stats.Store(at + 4, g_moved); tile_stats.Store(at + 8, g_fixed);
        tile_stats.Store(at + 12, asuint(g_vx)); tile_stats.Store(at + 16, asuint(g_vy));
        tile_stats.Store(at + 20, asuint(g_ax)); tile_stats.Store(at + 24, asuint(g_ay)); tile_stats.Store(at + 28, g_own);
    }
}
)";

// One thread per tile: a vote over its 3x3 tile neighbourhood, a score carried along the motion
// (hysteresis), the class and the tile's placement motion.
constexpr char kDecide[] = R"(
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= tw * th) return;
    int2 c = int2(id.x % tw, id.x / tw);
    uint hud = 0, moved = 0, fixed = 0, mine = 0; int vx = 0, vy = 0, ax = 0, ay = 0;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            int2 n = c + int2(dx, dy);
            if (n.x < 0 || n.y < 0 || n.x >= int(tw) || n.y >= int(th)) continue;
            uint at = (uint(n.y) * tw + uint(n.x)) * 32;
            hud += tile_stats.Load(at); moved += tile_stats.Load(at + 4); fixed += tile_stats.Load(at + 8);
            vx += asint(tile_stats.Load(at + 12)); vy += asint(tile_stats.Load(at + 16));
            ax += asint(tile_stats.Load(at + 20)); ay += asint(tile_stats.Load(at + 24));
        }
    // Interface pixels of frame n in this tile (the stats count frame n-1's as well).
    uint own = tile_stats.Load(id.x * 32 + 28);
    // The placement motion: the mean of the motions that matched (the scene the interface is anchored
    // to); without a match, the mean scene motion under it (for the dead zone).
    float2 v = moved ? float2(vx, vy) / (16.0 * float(moved)) : (hud ? float2(ax, ay) / (8.0 * float(hud)) : float2(0, 0));
    float speed = length(v);
    // The tile's content was at c + v in frame n-1: a moving label's score moves with it.
    int prior = 0;
    if (!forget) {
        int2 from = int2(floor((float2(c) * 16.0 + 8.0 + v) / 16.0));
        int moved_score = (from.x >= 0 && from.y >= 0 && from.x < int(tw) && from.y < int(th)) ? asint(score_prev.Load((uint(from.y) * tw + uint(from.x)) * 4)) : 0;
        int fixed_score = asint(score_prev.Load(id.x * 4));
        prior = moved_score > 0 ? moved_score : (fixed_score < 0 ? fixed_score : 0);
    }
    int vote = 0;
    if (hud >= 12 && speed >= 1.0) {
        // Decided by the pixels each hypothesis gets wrong: a uniform panel matches itself moved too,
        // and a repeating pattern partly matches in place, but only the true answer misses (almost) none.
        uint moved_miss = hud - moved, fixed_miss = hud - fixed;
        if (moved * 10 >= hud * 6 && fixed_miss >= 3 * moved_miss + 8) vote = 1;
        else if (fixed * 10 >= hud * 6 && moved_miss >= 3 * fixed_miss + 8) vote = -1;
    }
    int score = hud ? clamp(prior + vote, -3, 3) : 0;
    uint cls = (score > 0 && speed >= 1.0 && own) ? 1u : 0u;
    score_out.Store(id.x * 4, asuint(score));
    tile_class.Store(id.x * 16, cls);
    tile_class.Store(id.x * 16 + 4, asuint(v.x));
    tile_class.Store(id.x * 16 + 8, asuint(v.y));
    tile_class.Store(id.x * 16 + 12, own);
    if (own) {
        if (cls) {
            counters.InterlockedAdd(0, 1); counters.InterlockedAdd(8, own);
            counters.InterlockedAdd(16, uint(round(speed * 8.0))); // |(1-t) v| at x2, in 1/16 px
            counters.InterlockedAdd(20, 1);
        } else {
            counters.InterlockedAdd(4, 1); counters.InterlockedAdd(12, own);
        }
    }
}
)";

// Each pixel q of a generated picture at time t: screen-fixed interface pasted exactly as T2 (wins on
// top), otherwise the moving interface whose place at time t is q.
constexpr char kPaste[] = R"(
groupshared uint flags[18 * 18];
uint tile_cls(int2 p) { return tile_class.Load(((uint(p.y) / 16) * tw + uint(p.x) / 16) * 16); }
float2 tile_v(int2 p) { uint at = ((uint(p.y) / 16) * tw + uint(p.x) / 16) * 16; return float2(asfloat(tile_class.Load(at + 4)), asfloat(tile_class.Load(at + 8))); }
uint fixed_hud_at(int2 p) { return hud_n(p) && tile_cls(p) == 0 ? 1u : 0u; }
[numthreads(16, 16, 1)]
void main(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    int2 origin = int2(gid.xy) * 16 - 1;
    for (uint i = gi; i < 18 * 18; i += 256) flags[i] = fixed_hud_at(origin + int2(i % 18, i / 18));
    GroupMemoryBarrierWithGroupSync();
    int2 q = int2(gid.xy * 16 + tid.xy);
    if (!inside(q)) return;
    uint any = 0;
    [unroll] for (int dy = 0; dy < 3; ++dy)
        [unroll] for (int dx = 0; dx < 3; ++dx) any |= flags[(tid.y + dy) * 18 + tid.x + dx];
    if (any) { generated[q] = real_n[q]; return; }
    // Moving interface: frame n's pixel p shows at q = p - (1 - t) v at time t (the previous place is p + v).
    // The scene motion at q, or around it (q may lie over something moving the other way), finds the
    // tile; that tile's placement motion places the pixel, so a label moves as one piece.
    float2 p = float2(0, 0);
    bool found = false;
    float2 cands[9];
    ordered(q, cands);
    for (int k = 0; k < 9 && !found; ++k) {
        float2 v = cands[k];
        if (length(v) < 1.0) continue;
        p = float2(q) - (1.0 - t) * v;
        for (int it = 0; it < 2; ++it) {
            int2 pn = clamp(int2(round(p)), 0, int2(w, h) - 1);
            if (tile_cls(pn) != 1) break;
            p = float2(q) - (1.0 - t) * tile_v(pn);
            int2 pm = int2(round(p));
            found = it == 1 && inside(pm) && tile_cls(pm) == 1;
        }
    }
    if (!found) return;
    int2 pn = int2(round(p));
    // Only the interface itself is carried: weighted by how much of the sample's footprint is interface,
    // so the scene in the gaps of a label (which may move differently: a unit under it) stays Frame
    // Generation's.
    int2 i0 = int2(floor(p)); float2 f = p - float2(i0);
    float wgt = lerp(lerp(hud_n(i0) ? 1.0 : 0.0, hud_n(i0 + int2(1, 0)) ? 1.0 : 0.0, f.x),
                     lerp(hud_n(i0 + int2(0, 1)) ? 1.0 : 0.0, hud_n(i0 + int2(1, 1)) ? 1.0 : 0.0, f.x), f.y);
    if (wgt > 0) {
        float4 g = generated[q];
        generated[q] = lerp(g, bilinear_n(p), wgt);
    }
}
)";

D3D12_RESOURCE_BARRIER uav_barrier(ID3D12Resource* r)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = r;
    return b;
}
D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    return x;
}
ComPtr<ID3D12Resource> buffer(ID3D12Device* d, uint64_t bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = heap == D3D12_HEAP_TYPE_DEFAULT ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> r;
    if (FAILED(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r)))) return nullptr;
    return r;
}
} // namespace

bool HudMove::Init(ID3D12Device* device)
{
    if (ready_) return true;
    if (!device) return false;
    D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kSrvs, 0, 0, 0},
                                        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kUavs, 0, 0, kSrvs}};
    D3D12_ROOT_PARAMETER p[2]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[0].DescriptorTable = {2, ranges};
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[1].Constants = {0, 0, 8};
    D3D12_ROOT_SIGNATURE_DESC rd{2, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
        FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root_))))
        return false;
    const auto make = [&](const char* body, uint32_t encoding, ComPtr<ID3D12PipelineState>& out) {
        std::string src = std::string(kCommon) + body;
        char mode[2] = {char('0' + encoding), 0};
        const D3D_SHADER_MACRO defines[] = {{"NA_PICTURE", mode}, {nullptr, nullptr}};
        ComPtr<ID3DBlob> code, e;
        if (FAILED(D3DCompile(src.data(), src.size(), "hud_move", defines, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &e))) {
            if (e) error_.assign(static_cast<const char*>(e->GetBufferPointer()), e->GetBufferSize());
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = root_.Get();
        pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
        return SUCCEEDED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&out)));
    };
    if (!make(kDecide, 2, decide_)) return false;
    for (uint32_t e = 0; e < kEncodings; ++e) {
        if (!make(kClassify, e, classify_[e])) classify_[e].Reset();
        if (!make(kPaste, e, paste_[e])) paste_[e].Reset();
    }
    if (!classify_[2] || !paste_[2]) return false;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kPerPage * kPages, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)))) return false;
    inc_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    device_ = device;
    counters_ = buffer(device, kCounters * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    counters_rb_ = buffer(device, uint64_t(kCounters) * 4 * kPages, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!counters_ || !counters_rb_) return false;
    ready_ = true;
    return true;
}

bool HudMove::make_buffers(uint32_t tiles_w, uint32_t tiles_h)
{
    if (tiles_w == tw_ && tiles_h == th_ && tile_stats_) return true;
    const uint64_t tiles = uint64_t(tiles_w) * tiles_h;
    tile_stats_ = buffer(device_.Get(), tiles * kStatWords * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    score_[0] = buffer(device_.Get(), tiles * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    score_[1] = buffer(device_.Get(), tiles * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    tile_class_ = buffer(device_.Get(), tiles * kTileWords * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    class_rb_ = buffer(device_.Get(), tiles * kTileWords * 4, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!tile_stats_ || !score_[0] || !score_[1] || !tile_class_ || !class_rb_) { tw_ = th_ = 0; tile_stats_.Reset(); return false; }
    tw_ = tiles_w;
    th_ = tiles_h;
    forget_ = true;
    return true;
}

namespace {
void raw_uav(ID3D12Device* d, ID3D12Resource* r, uint32_t bytes, D3D12_CPU_DESCRIPTOR_HANDLE at)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
    u.Format = DXGI_FORMAT_R32_TYPELESS;
    u.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    u.Buffer.NumElements = bytes / 4;
    u.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    d->CreateUnorderedAccessView(r, nullptr, &u, at);
}
} // namespace

bool HudMove::Classify(ID3D12GraphicsCommandList* list, ID3D12Resource* real, ID3D12Resource* hudfree, ID3D12Resource* real_prev,
                       ID3D12Resource* hudfree_prev, ID3D12Resource* motion, float mv_scale_x, float mv_scale_y, uint32_t w, uint32_t h,
                       DXGI_FORMAT format, uint32_t encoding, uint64_t frame_id)
{
    if (!ready_ || !list || !real || !hudfree || !real_prev || !hudfree_prev || !motion || !w || !h || encoding >= kEncodings ||
        !classify_[encoding] || !paste_[encoding])
        return false;
    (void)frame_id;
    if (!make_buffers((w + kTile - 1) / kTile, (h + kTile - 1) / kTile)) return false;
    collect();
    const uint32_t page = page_++ % kPages;
    auto cpu = heap_->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += SIZE_T(page) * kPerPage * inc_;
    auto gpu = heap_->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += UINT64(page) * kPerPage * inc_;
    const DXGI_FORMAT vf = na::color::ViewFormat(format);
    ID3D12Resource* srvs[kSrvs] = {real, hudfree, real_prev, hudfree_prev, motion};
    for (uint32_t i = 0; i < kSrvs; ++i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = i == 4 ? DXGI_FORMAT_UNKNOWN : vf;
        if (i == 4) v.Format = motion->GetDesc().Format == DXGI_FORMAT_R16G16_FLOAT ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R32G32_FLOAT;
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(srvs[i], &v, cpu);
        cpu.ptr += inc_;
    }
    {   // u0: no generated picture in this pass (a null view of the right kind)
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format = vf;
        u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device_->CreateUnorderedAccessView(nullptr, nullptr, &u, cpu);
        cpu.ptr += inc_;
    }
    const uint32_t tiles = tw_ * th_;
    const uint32_t in = ping_ & 1, out = in ^ 1;
    ID3D12Resource* raws[5] = {tile_stats_.Get(), score_[in].Get(), score_[out].Get(), tile_class_.Get(), counters_.Get()};
    const uint32_t sizes[5] = {tiles * kStatWords * 4, tiles * 4, tiles * 4, tiles * kTileWords * 4, kCounters * 4};
    for (uint32_t i = 0; i < 5; ++i) { raw_uav(device_.Get(), raws[i], sizes[i], cpu); cpu.ptr += inc_; }
    ++ping_;
    {   // this frame's counters start at zero
        ComPtr<ID3D12GraphicsCommandList2> l2;
        if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&l2)))) {
            D3D12_RESOURCE_BARRIER b = transition(counters_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            list->ResourceBarrier(1, &b);
            D3D12_WRITEBUFFERIMMEDIATE_PARAMETER wp[kCounters];
            D3D12_WRITEBUFFERIMMEDIATE_MODE wm[kCounters];
            for (uint32_t i = 0; i < kCounters; ++i) { wp[i] = {counters_->GetGPUVirtualAddress() + i * 4, 0u}; wm[i] = D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT; }
            l2->WriteBufferImmediate(kCounters, wp, wm);
            b = transition(counters_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list->ResourceBarrier(1, &b);
        }
    }
    ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root_.Get());
    list->SetComputeRootDescriptorTable(0, gpu);
    struct { uint32_t w, h; float sx, sy, t; uint32_t forget, tw, th; } k{w, h, mv_scale_x, mv_scale_y, .5f, forget_ ? 1u : 0u, tw_, th_};
    list->SetComputeRoot32BitConstants(1, 8, &k, 0);
    list->SetPipelineState(classify_[encoding].Get());
    list->Dispatch(tw_, th_, 1);
    D3D12_RESOURCE_BARRIER b = uav_barrier(tile_stats_.Get());
    list->ResourceBarrier(1, &b);
    list->SetPipelineState(decide_.Get());
    list->Dispatch((tiles + 63) / 64, 1, 1);
    D3D12_RESOURCE_BARRIER after[3] = {uav_barrier(tile_class_.Get()), uav_barrier(score_[out].Get()), uav_barrier(counters_.Get())};
    list->ResourceBarrier(3, after);
    forget_ = false;
    // Read back this frame's counters and classes (a few frames late; the class copy is for tests).
    D3D12_RESOURCE_BARRIER rb[2] = {transition(counters_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                    transition(tile_class_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE)};
    list->ResourceBarrier(2, rb);
    const uint32_t cpage = uint32_t(classified_ % kPages); // the counters' own ring (the descriptor pages are shared with Paste)
    list->CopyBufferRegion(counters_rb_.Get(), uint64_t(cpage) * kCounters * 4, counters_.Get(), 0, kCounters * 4);
    list->CopyBufferRegion(class_rb_.Get(), 0, tile_class_.Get(), 0, uint64_t(tiles) * kTileWords * 4);
    D3D12_RESOURCE_BARRIER back[2] = {transition(counters_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                                      transition(tile_class_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
    list->ResourceBarrier(2, back);
    submitted_[cpage] = ++classified_;
    last_page_ = page;
    return true;
}

bool HudMove::Paste(ID3D12GraphicsCommandList* list, ID3D12Resource* generated, ID3D12Resource* real, ID3D12Resource* hudfree,
                    ID3D12Resource* motion, float mv_scale_x, float mv_scale_y, float t, uint32_t w, uint32_t h, DXGI_FORMAT format,
                    uint32_t encoding)
{
    if (!ready_ || !tile_stats_ || !list || !generated || !real || !hudfree || !motion || encoding >= kEncodings || !paste_[encoding]) return false;
    if ((w + kTile - 1) / kTile != tw_ || (h + kTile - 1) / kTile != th_) return false;
    const uint32_t page = page_++ % kPages;
    auto cpu = heap_->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += SIZE_T(page) * kPerPage * inc_;
    auto gpu = heap_->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += UINT64(page) * kPerPage * inc_;
    const DXGI_FORMAT vf = na::color::ViewFormat(format);
    ID3D12Resource* srvs[kSrvs] = {real, hudfree, real, hudfree, motion}; // the previous pair is not read here
    for (uint32_t i = 0; i < kSrvs; ++i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = i == 4 ? (motion->GetDesc().Format == DXGI_FORMAT_R16G16_FLOAT ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R32G32_FLOAT) : vf;
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(srvs[i], &v, cpu);
        cpu.ptr += inc_;
    }
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format = vf;
        u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device_->CreateUnorderedAccessView(generated, nullptr, &u, cpu);
        cpu.ptr += inc_;
    }
    const uint32_t tiles = tw_ * th_;
    ID3D12Resource* raws[5] = {tile_stats_.Get(), score_[0].Get(), score_[1].Get(), tile_class_.Get(), counters_.Get()};
    const uint32_t sizes[5] = {tiles * kStatWords * 4, tiles * 4, tiles * 4, tiles * kTileWords * 4, kCounters * 4};
    for (uint32_t i = 0; i < 5; ++i) { raw_uav(device_.Get(), raws[i], sizes[i], cpu); cpu.ptr += inc_; }
    ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root_.Get());
    list->SetComputeRootDescriptorTable(0, gpu);
    struct { uint32_t w, h; float sx, sy, t; uint32_t forget, tw, th; } k{w, h, mv_scale_x, mv_scale_y, t, 0u, tw_, th_};
    list->SetComputeRoot32BitConstants(1, 8, &k, 0);
    list->SetPipelineState(paste_[encoding].Get());
    D3D12_RESOURCE_BARRIER b = uav_barrier(generated);
    list->ResourceBarrier(1, &b); // the runtime's last writes to `generated` land before ours
    list->Dispatch((w + 15) / 16, (h + 15) / 16, 1);
    list->ResourceBarrier(1, &b);
    ++stats_.pictures;
    return true;
}

void HudMove::collect()
{
    if (!counters_rb_) return;
    // Pages written at least 8 classifications ago have long finished on the GPU.
    void* m = nullptr;
    D3D12_RANGE r{0, SIZE_T(kCounters) * 4 * kPages};
    if (FAILED(counters_rb_->Map(0, &r, &m)) || !m) return;
    for (uint32_t page = 0; page < kPages; ++page) {
        if (!submitted_[page] || submitted_[page] + 8 > classified_) continue;
        const uint32_t* c = static_cast<const uint32_t*>(m) + page * kCounters;
        ++stats_.frames;
        stats_.tiles_moving += c[0];
        stats_.tiles_fixed += c[1];
        stats_.pixels_moving += c[2];
        stats_.pixels_fixed += c[3];
        stats_.shift_px_sum += c[4] / 16.0;
        stats_.shift_n += c[5];
        submitted_[page] = 0;
    }
    D3D12_RANGE none{0, 0};
    counters_rb_->Unmap(0, &none);
}

HudMove::Stats HudMove::Read()
{
    Stats s = stats_;
    s.fallback_pair = fb_pair_.load();
    s.fallback_reset = fb_reset_.load();
    s.fallback_motion = fb_motion_.load();
    s.fallback_off = fb_off_.load();
    s.fallback_size = fb_size_.load();
    s.fallback = s.fallback_pair + s.fallback_reset + s.fallback_motion + s.fallback_off + s.fallback_size;
    return s;
}

void HudMove::NoteFallback(const char* why)
{
    if (!why) return;
    if (!std::strcmp(why, "pair")) fb_pair_.fetch_add(1);
    else if (!std::strcmp(why, "reset")) fb_reset_.fetch_add(1);
    else if (!std::strcmp(why, "motion")) fb_motion_.fetch_add(1);
    else if (!std::strcmp(why, "off")) fb_off_.fetch_add(1);
    else fb_size_.fetch_add(1);
}

int HudMove::TileClassForTests(uint32_t tx, uint32_t ty)
{
    if (!class_rb_ || tx >= tw_ || ty >= th_) return -1;
    void* m = nullptr;
    if (FAILED(class_rb_->Map(0, nullptr, &m)) || !m) return -1;
    const uint32_t cls = static_cast<const uint32_t*>(m)[(ty * tw_ + tx) * kTileWords];
    D3D12_RANGE none{0, 0};
    class_rb_->Unmap(0, &none);
    return int(cls);
}

} // namespace na::fg
