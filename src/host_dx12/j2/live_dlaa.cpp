#include "host_dx12/j2/live_dlaa.hpp"
#include "host_dx12/j2/msaa_resolve.hpp"
#include "host_dx12/j2/nis_sharpen.hpp"
#include "host_dx12/j2/route_selector.hpp"
#include "host_dx12/j2/consumer_profile.hpp"
#include "host_dx12/capture/j2_capture.hpp"
#include "host_dx12/ngx_observe/ngx_observe.hpp"
#include "host_dx12/upscalers/ffx_intercept.hpp"
#include "host_dx12/upscalers/xess_intercept.hpp"

#include "core/motion_arbiter_gpu.hpp"
#include "core/temporal_jitter.hpp"
#include "host_dx12/depth_format.hpp"
#include "host_dx12/dlss/dlss_backend.hpp"
#include "host_dx12/observe/producers.hpp"
#include "host_dx12/settings.hpp"
#include "providers/camera/camera_tracker.hpp"

#include <windows.h>
#include <wrl/client.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace na::j2 {
namespace {

constexpr uint32_t kRing = 6;            // frames in flight before a slot is reused (never waited on)
constexpr uint32_t kBlockBytes = 512;    // root CBVs have no size: read this much (the producer's own bound)
constexpr size_t kMaxBlocks = 256;       // unique constant blocks examined per frame (until the camera slot is learned)
constexpr uint32_t kMaxBlockRead = 1024; // bytes read from one constant buffer
constexpr uint64_t kViewId = 0x4A32;     // DLSS view for this source ("J2")

struct Slot {
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    UINT64 fence_value = 0;
};

struct PendingHistory {
    na::camera::CameraTracker tracker;
    bool cut = false;
    uint32_t capture = 0; // J2 evidence capture token for this evaluation (0 = none)
};

struct State {
    std::mutex m;
    std::atomic<bool> enabled{false};
    std::atomic<bool> configured{false};
    std::atomic<bool> paused{false}; // the user's on/off switch in the overlay
    bool resume_reset = false;       // history is stale after a pause
    bool latched = false;            // stopped for this session after a failure (reason kept)
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    UINT64 fence_next = 1;
    Slot slots[kRing];
    uint32_t slot = 0;
    // Our textures (render size == output size: DLAA).
    ComPtr<ID3D12Resource> depth32, motion, provenance, output;
    ComPtr<ID3D12Resource> color_in; // plain copy of the scene colour's first slice / mip (DLSS input)
    ComPtr<ID3D12Resource> sharp;    // RCAS output (when sharpening is on)
    MsaaResolve msaa;
    UINT samples = 1, depth_samples = 1, sample_quality = 0, depth_quality = 0;
    DXGI_FORMAT depth_format = DXGI_FORMAT_UNKNOWN;
    uint32_t w = 0, h = 0;
    DXGI_FORMAT color_format = DXGI_FORMAT_UNKNOWN;
    na::motion::gpu::Arbiter arbiter;
    bool arbiter_ok = false;
    na::camera::CameraTracker tracker;
    uint64_t frame = 1;             // game Present interval, not Execute attempt
    uint64_t submitted_frame = 0;   // last interval that actually submitted DLAA
    // Games may draw the scene across several submissions per frame (Sniper Elite
    // Resistance). DLAA belongs after the LAST of them, before post-processing
    // reads the colour. Counted per frame; the previous frame's count is the target.
    // The target is the most common count over the last 16 frames (ties: the
    // smaller), so one odd frame cannot make DLAA alternate early/late/missing.
    // Mid-frame readers of the colour (refraction, reflections) do not end the
    // scene: Sniper Elite Resistance samples it between its scene submissions.
    uint32_t scene_batches_cur = 0, scene_batches_target = 0;
    uint8_t batch_hist[16] = {};
    uint32_t batch_hist_n = 0;
    // Inline insertion (games that record the end of the scene AND their
    // post-processing in one command list, Sniper Elite Resistance): DLAA is
    // recorded INTO the game's list at the moment it binds the scene colour for
    // reading, then the game's state is restored. Explicitly enabled in settings.
    bool inline_mode = false; // mirrors the route selector's Inline decision
    // Automatic route selection (COMMS-Continuation step 2): the selector decides
    // between-list / inline / none per view from per-frame structure; Execute and
    // the inline trigger only carry out its decision.
    na::j2::RouteSelector router;
    na::j2::FrameAggregator frame_agg;
    uint64_t route_prev_late = 0, route_prev_post_scene = 0, route_prev_no_camera = 0, route_prev_no_state = 0;
    uint32_t route_gen_applied = 0;
    bool dlss_ready = false;
    // Placement profile (docs/PLACEMENT-DESIGN-RESPONSE.md): role classes of the scene
    // colour's consumers, learned from SUBMITTED frames; the inline trigger fires at an
    // accepted class, else the boundary matcher decides as before.
    na::j2::ConsumerProfile profile;
    std::vector<na::j2::ColorEvent> frame_seq; // this frame's writes/consumers of the colour, submission order
    bool frame_seq_complete = true;
    uint64_t scene_color_id = 0;
    uint32_t scene_w = 0, scene_h = 0;         // selected colour, mip 0 (class size reference)
    bool class_fired = false;                   // this frame's DLAA was triggered by a class
    na::j2::ClassKey fired_class;
    uint64_t fired_producer = 0;
    uint64_t route_prev_late_class = 0;
    uint64_t route_prev_external = 0; // the game's own upscaler evaluations so far (NGX + FSR + XeSS)
    uint32_t frame_rejects = 0;       // inline trigger rejections in the current frame
    char frame_reject_text[320] = {}; // the frame's first rejection, with both boundaries
    uint64_t dlss_tried_frame = ~0ull; // one early start attempt per frame
    bool inline_sample_locked = false; // NGX recorded: no new raster sample may start this frame
    bool inline_camera_submitted = false; // an earlier depth/camera batch may already be executing
    struct LearnedInsertion { uint64_t color = 0, depth = 0; na::j2::InsertionDecision decision; };
    LearnedInsertion insertion_frame, insertion_learned;
    char insertion_trace[2048] = {};
    size_t insertion_trace_used = 0;
    uint64_t inl_frame = 0; // last frame an inline DLAA was recorded
    struct InlinePending {
        const void* list = nullptr;
        uint64_t rec = 0, frame = 0;
        uint32_t slot = 0;
        uint32_t writes = 0;
        uint64_t color = 0;
        PendingHistory history;
        bool valid = false;
    } inl;
    int64_t camera_offset = -1; // learned offset of the ViewProjection inside its block
    int64_t camera_param = -1;  // learned root parameter that binds the camera block
    int64_t camera_inv_delta = 64; // learned byte distance from the ViewProjection to its inverse
    int64_t camera_proj_delta = INT64_MIN; // learned distance to the camera's own projection matrix (none = INT64_MIN)
    bool camera_table = false;  // ... through a descriptor table
    // J1: this frame's injected camera jitter (see jitter_batch).
    struct Jitter {
        bool valid = false;
        bool applied = false;
        uint64_t res = 0, off = 0;           // the camera block it was found in
        double px[2] = {0, 0};               // sample, render pixels (DLSS: colour sampled at centre - jitter)
        uint8_t vp[2][64], inv[2][64];       // original matrices as stored (row / column layout)
        uint8_t jvp[2][64], jinv[2][64];     // jittered replacements
        bool has_proj = false;               // the camera's separate projection matrix (same block)
        uint8_t proj[64], jproj[64];         // as the game stores it / jittered
        uint32_t phase = 0;
        int layout = 0;                      // 0 row-major, 1 column-major (as the game stores it)
    } jit;
    bool jitter_on = true; // DLAA needs jitter (with the texture mip bias set at start-up)
    Stats stats;
};
// Never destroyed: at process exit the observation hooks' own state may already
// be gone, and releasing D3D12 objects then would call into them.
State& g = *new State;

void reason(const char* r) { std::snprintf(g.stats.reason, sizeof(g.stats.reason), "%s", r); }

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, UINT before, UINT after)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(before);
    b.Transition.StateAfter = static_cast<D3D12_RESOURCE_STATES>(after);
    return b;
}
D3D12_RESOURCE_BARRIER uav_barrier(ID3D12Resource* r)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = r;
    return b;
}
// Appends a transition unless the states are equal.
void add(std::vector<D3D12_RESOURCE_BARRIER>& v, ID3D12Resource* r, UINT before, UINT after)
{
    if (before != after) v.push_back(transition(r, before, after));
}
// One subresource only (the scene's mip / slice; the rest of the texture is never touched).
void addsub(std::vector<D3D12_RESOURCE_BARRIER>& v, ID3D12Resource* r, UINT sub, UINT before, UINT after)
{
    if (before == after) return;
    D3D12_RESOURCE_BARRIER b = transition(r, before, after);
    b.Transition.Subresource = sub;
    v.push_back(b);
}
void flush(ID3D12GraphicsCommandList* l, std::vector<D3D12_RESOURCE_BARRIER>& v)
{
    if (!v.empty()) l->ResourceBarrier(static_cast<UINT>(v.size()), v.data());
    v.clear();
}

bool texture(UINT w, UINT h, DXGI_FORMAT fmt, bool uav, D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& out)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    out.Reset();
    return SUCCEEDED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&out)));
}
// A typed format for our own copies of a (possibly typeless) game texture.
DXGI_FORMAT typed(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    default: return f;
    }
}
bool uav_store(DXGI_FORMAT fmt)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{};
    fs.Format = fmt;
    return SUCCEEDED(g.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))) &&
           (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
}
bool is_float_color(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R11G11B10_FLOAT || f == DXGI_FORMAT_R32G32B32A32_FLOAT ||
           f == DXGI_FORMAT_R32G32B32_FLOAT;
}

// ---- Debug view (Argon's motion-vector audit) ---------------------------------
// Replaces the DLAA result with a picture of what DLAA was given, so it can be
// judged in the game: 1 motion (hue = direction, brightness = speed, 4 px = full;
// black = no motion), 2 fine motion (0.2 px = full: reveals sub-pixel false
// motion on a still camera), 3 depth (banded). Magenta = non-finite motion.
std::atomic<int> g_debug_view{0};
struct DebugPass {
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12DescriptorHeap> heap;
    UINT inc = 0;
    bool tried = false, ok = false;
} g_dbg;
constexpr char kDebugHlsl[] = R"(
Texture2D<float2> t_mv : register(t0);
Texture2D<float> t_depth : register(t1);
RWTexture2D<float4> u_out : register(u0);
cbuffer C : register(b0) { uint mode; float scale; uint w; uint h; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= w || id.y >= h) return;
    float3 c;
    if (mode == 3) {
        float d = t_depth[id.xy];
        c = frac(d * float3(1.0, 16.0, 256.0));
    } else {
        float2 v = t_mv[id.xy];
        if (any(isnan(v)) || any(isinf(v))) { c = float3(1, 0, 1); }
        else {
            float a = atan2(v.y, v.x) / 6.2831853;
            float3 hue = saturate(abs(frac(a + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0);
            c = hue * saturate(length(v) * scale);
        }
    }
    u_out[id.xy] = float4(c, 1.0);
})";
bool debug_init()
{
    if (g_dbg.tried) return g_dbg.ok;
    g_dbg.tried = true;
    D3D12_DESCRIPTOR_RANGE r[2]{};
    r[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    r[0].NumDescriptors = 2;
    r[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    r[1].NumDescriptors = 1;
    r[1].OffsetInDescriptorsFromTableStart = 2;
    D3D12_ROOT_PARAMETER p[2]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[0].DescriptorTable.NumDescriptorRanges = 2;
    p[0].DescriptorTable.pDescriptorRanges = r;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[1].Constants.Num32BitValues = 4;
    D3D12_ROOT_SIGNATURE_DESC rd{2, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> sig, err, code;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
        FAILED(g.device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_dbg.root))))
        return false;
    if (FAILED(D3DCompile(kDebugHlsl, sizeof(kDebugHlsl) - 1, "j2_debug.hlsl", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err)))
        return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_dbg.root.Get();
    pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    if (FAILED(g.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_dbg.pso)))) return false;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kRing * 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_dbg.heap)))) return false;
    g_dbg.inc = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_dbg.ok = true;
    return true;
}
// Writes the debug picture into g.output (UNORDERED_ACCESS); motion and depth32 are readable (NPSR).
bool debug_record(ID3D12GraphicsCommandList* l, int mode, DXGI_FORMAT out_format)
{
    if (!debug_init()) return false;
    const UINT base = g.slot * 3;
    D3D12_CPU_DESCRIPTOR_HANDLE c = g_dbg.heap->GetCPUDescriptorHandleForHeapStart();
    c.ptr += static_cast<SIZE_T>(base) * g_dbg.inc;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    sv.Format = DXGI_FORMAT_R32G32_FLOAT;
    g.device->CreateShaderResourceView(g.motion.Get(), &sv, c);
    c.ptr += g_dbg.inc;
    sv.Format = DXGI_FORMAT_R32_FLOAT;
    g.device->CreateShaderResourceView(g.depth32.Get(), &sv, c);
    c.ptr += g_dbg.inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.Format = out_format;
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g.device->CreateUnorderedAccessView(g.output.Get(), nullptr, &uv, c);
    D3D12_GPU_DESCRIPTOR_HANDLE gh = g_dbg.heap->GetGPUDescriptorHandleForHeapStart();
    gh.ptr += static_cast<UINT64>(base) * g_dbg.inc;
    ID3D12DescriptorHeap* heaps[] = {g_dbg.heap.Get()};
    l->SetDescriptorHeaps(1, heaps);
    l->SetComputeRootSignature(g_dbg.root.Get());
    l->SetPipelineState(g_dbg.pso.Get());
    l->SetComputeRootDescriptorTable(0, gh);
    const float scale = mode == 2 ? 5.0f : 0.25f;
    uint32_t k[4] = {static_cast<uint32_t>(mode), 0, g.w, g.h};
    std::memcpy(&k[1], &scale, 4);
    l->SetComputeRoot32BitConstants(1, 4, k, 0);
    l->Dispatch((g.w + 7) / 8, (g.h + 7) / 8, 1);
    return true;
}

// ---- Sharpening: RCAS (Robust Contrast-Adaptive Sharpening) ------------------
// AMD FidelityFX Super Resolution 1 "RCAS" (ffx_fsr1.h, MIT licence, (c) Advanced
// Micro Devices): a 5-tap cross whose sharpening lobe is limited so the result
// never leaves the local min/max (no ringing / halos), with its noise detection
// (FSR_RCAS_DENOISE) so flat noisy areas are not amplified. Runs on the DLAA
// result before the game's tonemap and HUD, in a Reinhard-compressed copy of the
// HDR values (RCAS expects 0..1), expanded back afterwards. Slider 1..100 maps to
// RCAS sharpness 2..0 stops (100 = strongest); 0 skips the pass entirely.
struct RcasPass {
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12DescriptorHeap> heap;
    UINT inc = 0;
    bool tried = false, ok = false;
} g_rcas;
constexpr char kRcasHlsl[] = R"(
Texture2D<float4> t_in : register(t0);
RWTexture2D<float4> u_out : register(u0);
cbuffer C : register(b0) { float con; uint w; uint h; uint pad; };
#define RCAS_LIMIT (0.25 - (1.0 / 16.0))
float3 fwd(float3 x) { x = max(x, 0.0); return x / (1.0 + x); }
float3 inv(float3 t) { t = min(t, 0.99990); return t / (1.0 - t); }
float3 tap(int2 p) { p = clamp(p, int2(0, 0), int2(w - 1, h - 1)); return fwd(t_in[p].rgb); }
float min3(float a, float b, float c) { return min(a, min(b, c)); }
float max3(float a, float b, float c) { return max(a, max(b, c)); }
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= w || id.y >= h) return;
    int2 p = int2(id.xy);
    //    b
    //  d e f
    //    h
    float3 b = tap(p + int2(0, -1)), d = tap(p + int2(-1, 0)), e = tap(p), f = tap(p + int2(1, 0)), hh = tap(p + int2(0, 1));
    float alpha = t_in[p].a;
    // Luma times 2 (FSR1) for noise detection.
    float bL = b.b * 0.5 + (b.r * 0.5 + b.g), dL = d.b * 0.5 + (d.r * 0.5 + d.g), eL = e.b * 0.5 + (e.r * 0.5 + e.g);
    float fL = f.b * 0.5 + (f.r * 0.5 + f.g), hL = hh.b * 0.5 + (hh.r * 0.5 + hh.g);
    float nz = 0.25 * bL + 0.25 * dL + 0.25 * fL + 0.25 * hL - eL;
    float range = max3(max3(bL, dL, eL), fL, hL) - min3(min3(bL, dL, eL), fL, hL);
    nz = saturate(abs(nz) / max(range, 1e-6));
    nz = -0.5 * nz + 1.0;
    float3 mn4 = min(min(min(b, d), f), hh), mx4 = max(max(max(b, d), f), hh);
    float2 peakC = float2(1.0, -1.0 * 4.0);
    float3 hitMin = min(mn4, e) / max(4.0 * mx4, 1e-6);
    float3 hitMax = (peakC.x - max(mx4, e)) / (4.0 * mn4 + peakC.y);
    float3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-RCAS_LIMIT, min(max3(lobeRGB.r, lobeRGB.g, lobeRGB.b), 0.0)) * con;
    lobe *= nz;
    float rcpL = 1.0 / (4.0 * lobe + 1.0);
    float3 o = (lobe * b + lobe * d + lobe * hh + lobe * f + e) * rcpL;
    u_out[p] = float4(inv(o), alpha);
})";
bool rcas_init()
{
    if (g_rcas.tried) return g_rcas.ok;
    g_rcas.tried = true;
    D3D12_DESCRIPTOR_RANGE r[2]{};
    r[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    r[0].NumDescriptors = 1;
    r[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    r[1].NumDescriptors = 1;
    r[1].OffsetInDescriptorsFromTableStart = 1;
    D3D12_ROOT_PARAMETER p[2]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[0].DescriptorTable.NumDescriptorRanges = 2;
    p[0].DescriptorTable.pDescriptorRanges = r;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[1].Constants.Num32BitValues = 4;
    D3D12_ROOT_SIGNATURE_DESC rd{2, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> sig, err, code;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
        FAILED(g.device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_rcas.root))))
        return false;
    if (FAILED(D3DCompile(kRcasHlsl, sizeof(kRcasHlsl) - 1, "j2_rcas.hlsl", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err)))
        return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_rcas.root.Get();
    pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    if (FAILED(g.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_rcas.pso)))) return false;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kRing * 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rcas.heap)))) return false;
    g_rcas.inc = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_rcas.ok = true;
    return true;
}
// RCAS from `in` (readable, NON_PIXEL_SHADER_RESOURCE) into `out` (UNORDERED_ACCESS).
bool rcas_dispatch(ID3D12GraphicsCommandList* l, ID3D12Resource* in, ID3D12Resource* out, uint32_t w, uint32_t h, uint32_t percent,
                   DXGI_FORMAT fmt, UINT slot)
{
    if (!percent || !in || !out || !rcas_init()) return false;
    const UINT base = slot * 2;
    D3D12_CPU_DESCRIPTOR_HANDLE c = g_rcas.heap->GetCPUDescriptorHandleForHeapStart();
    c.ptr += static_cast<SIZE_T>(base) * g_rcas.inc;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = fmt;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    g.device->CreateShaderResourceView(in, &sv, c);
    c.ptr += g_rcas.inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.Format = fmt;
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g.device->CreateUnorderedAccessView(out, nullptr, &uv, c);
    D3D12_GPU_DESCRIPTOR_HANDLE gh = g_rcas.heap->GetGPUDescriptorHandleForHeapStart();
    gh.ptr += static_cast<UINT64>(base) * g_rcas.inc;
    ID3D12DescriptorHeap* heaps[] = {g_rcas.heap.Get()};
    l->SetDescriptorHeaps(1, heaps);
    l->SetComputeRootSignature(g_rcas.root.Get());
    l->SetPipelineState(g_rcas.pso.Get());
    l->SetComputeRootDescriptorTable(0, gh);
    const float stops = 2.0f * (1.0f - static_cast<float>(percent > 100 ? 100 : percent) / 100.0f);
    const float con = std::exp2(-stops);
    uint32_t k[4] = {0, w, h, 0};
    std::memcpy(&k[0], &con, 4);
    l->SetComputeRoot32BitConstants(1, 4, k, 0);
    l->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    return true;
}
// 101..200 %: NVIDIA Image Scaling sharpen-only on top of full-strength RCAS.
na::j2::NisSharpen g_nis;
bool rcas_record(ID3D12GraphicsCommandList* l, uint32_t percent, DXGI_FORMAT fmt)
{
    return g.sharp && rcas_dispatch(l, g.output.Get(), g.sharp.Get(), g.w, g.h, percent, fmt, g.slot);
}

bool ensure_device(ID3D12CommandQueue* q)
{
    if (g.device && g.queue.Get() == q) return true;
    if (g.device) { reason("Live DLAA follows one queue; another queue submitted the scene."); return false; }
    ComPtr<ID3D12Device> dev;
    if (FAILED(q->GetDevice(IID_PPV_ARGS(&dev)))) return false;
    g.device = dev;
    g.queue = q;
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)))) return false;
    const D3D12_COMMAND_LIST_TYPE type = q->GetDesc().Type;
    for (auto& s : g.slots) {
        if (FAILED(dev->CreateCommandAllocator(type, IID_PPV_ARGS(&s.alloc))) ||
            FAILED(dev->CreateCommandList(0, type, s.alloc.Get(), nullptr, IID_PPV_ARGS(&s.list))))
            return false;
        s.list->Close();
    }
    g.arbiter_ok = g.arbiter.Init(dev.Get(), kRing);
    return true;
}

// DLSS on J2's device (our own NGX instance, data in %TEMP%\NeuralAlias-ngx).
bool dlss_on()
{
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetTempPathW(MAX_PATH, path);
    if (!n || n >= MAX_PATH - 20) { reason("No folder for DLSS data."); return false; }
    wcscat_s(path, L"NeuralAlias-ngx");
    CreateDirectoryW(path, nullptr);
    if (!na::dlss::EnsureOn(g.device.Get(), path)) { reason("DLSS did not start on this device (see DLSS details)."); return false; }
    g.dlss_ready = true;
    return true;
}

// Carry out the route selector's decision (under g.m): inline mode for the inline
// trigger, device + DLSS ready for it, and a history reset when the route changes.
void apply_route_locked(ID3D12CommandQueue* queue)
{
    const auto& d = g.router.Current();
    // Inline is armed only after at least one scene frame was observed under this
    // decision: the trigger matches the boundary learned from a completed frame, and
    // a list recorded before any frame closed has nothing to match (legacy timing).
    g.inline_mode = d.route == na::j2::Route::Inline && d.scene_frames > 0;
    g.stats.inline_mode = g.inline_mode ? 1u : 0u;
    if (d.history_generation != g.route_gen_applied) {
        g.route_gen_applied = d.history_generation;
        g.resume_reset = true;   // output of the old route must not feed the new one
        if (g.inl.valid) na::capture::j2::Discard(g.inl.history.capture, true);
        g.inl.valid = false;     // an inline recording from the old route is not committed
        g.scene_batches_target = 0;
    }
    // Ready before the game RECORDS the next scene list (the inline trigger runs then).
    // Only from Execute (queue given): it runs inside a host-work scope, so NGX's own
    // submissions are not routed back into J2. From Present they would re-enter
    // Execute while g.m is held.
    if (queue && d.route == na::j2::Route::Inline && !g.dlss_ready) {
        if (!g.device) ensure_device(queue);
        if (g.device) dlss_on();
    }
}

// The resource state at the end of the scene list: its last whole-resource
// transition there, else in an earlier list of the same batch.
// State of subresource `sub` after lists[0..last]. Barriers may name that
// subresource alone (depth vs stencil plane, one mip or slice) or all of them.
bool state_after(ID3D12CommandList* const* lists, UINT last, na_object_id id, uint32_t sub, uint32_t* st)
{
    for (int i = static_cast<int>(last); i >= 0; --i)
        if (na::observe::ListLastStateSub(lists[i], id, sub, st)) return true;
    // Not transitioned in this batch up to the scene: the state it carried in.
    return na::observe::KnownSubStateInCurrentSubmission(id, sub, st);
}

// ---- J1: camera jitter ---------------------------------------------------
// DLAA needs a different sub-pixel sample every frame. Anno (like most games
// without temporal AA) renders with a fixed camera, so J1 shifts it: before a
// batch is forwarded, the camera ViewProjection and its inverse in the bound
// constant buffers are replaced by jittered copies (core/temporal_jitter.hpp,
// Halton 2,3 over 8 phases, DLSS's recommendation for DLAA). The same
// replacement goes into every bound block that holds an identical copy, so a
// depth pre-pass in an earlier batch and the scene agree. J2 then reports the
// known sample to the camera tracker (which removes it exactly) and to DLSS.
// Runs only while DLAA is producing frames, so the shake is never left
// unresolved on screen.
constexpr uint32_t kJitterPhases = 8;

void store_matrix(const na::camera::Matrix& m, na::camera::Layout layout, uint8_t out[64])
{
    for (size_t r = 0; r < 4; ++r)
        for (size_t c = 0; c < 4; ++c) {
            const float v = static_cast<float>(m[r * 4 + c]);
            std::memcpy(out + (layout == na::camera::Layout::RowMajor ? r * 4 + c : c * 4 + r) * 4, &v, 4);
        }
}

// P is this camera's projection when P^-1 * VP is a rigid view (VP = P * V).
bool is_projection_of(const na::camera::Matrix& p, const na::camera::Matrix& vp)
{
    na::camera::Matrix pinv{}, v{};
    if (!na::camera::Invert(p, pinv)) return false;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            double a = 0;
            for (int k = 0; k < 4; ++k) a += pinv[r * 4 + k] * vp[k * 4 + c];
            v[r * 4 + c] = a;
        }
    na::camera::Candidate cand{};
    return na::camera::RigidView(v, cand);
}

// A camera block's bytes hold a VP + inverse at the learned offset: start this frame's jitter.
// Last main camera the jitter started from (a block outside the learned camera
// slot may start a frame only if it shows that same camera, not a light's).
struct { bool valid = false; na::camera::VpGeometry geo{}; } g_jit_cam;

bool same_camera(const na::camera::Matrix& m)
{
    na::camera::VpGeometry v{};
    if (!g_jit_cam.valid || !na::camera::DecomposeViewProjection(m, v)) return false;
    const auto& p = g_jit_cam.geo;
    double dot = 0, d2 = 0, len2 = 0;
    for (int j = 0; j < 3; ++j) {
        dot += v.forward[j] * p.forward[j];
        d2 += (v.position[j] - p.position[j]) * (v.position[j] - p.position[j]);
        len2 += p.position[j] * p.position[j];
    }
    return dot > 0.98 && std::abs(v.sy - p.sy) <= 0.05 * std::abs(p.sy) && d2 <= 0.01 * len2 + 100.0;
}

bool begin_jitter(uint64_t res, uint64_t off, const std::vector<uint8_t>& bytes, bool slot)
{
    const size_t n = bytes.size();
    if (g.camera_offset < 0 || static_cast<uint64_t>(g.camera_offset) + 64 > n || !g.w || !g.h) return false;
    for (auto layout : {na::camera::Layout::RowMajor, na::camera::Layout::ColumnMajor}) {
        const auto m = na::camera::LoadMatrix(bytes, static_cast<size_t>(g.camera_offset), layout);
        na::camera::Candidate c{};
        if (!na::camera::ViewProjection(m, c)) continue;
        if (!slot && !same_camera(m)) continue;
        // The inverse at its learned position. A block from the camera slot must
        // carry it; another block (an earlier pass with a smaller constant buffer)
        // may start the frame from the ViewProjection alone, the inverse being
        // computed; the game's own inverse copy is adopted when the slot block shows it.
        const int64_t io = g.camera_offset + g.camera_inv_delta;
        const bool inv_here = io >= 0 && static_cast<uint64_t>(io) + 64 <= n;
        na::camera::Matrix inv{};
        if (inv_here) {
            inv = na::camera::LoadMatrix(bytes, static_cast<size_t>(io), layout);
            if (!na::camera::IsInversePair(m, inv)) {
                if (slot) continue;
                if (!na::camera::Invert(m, inv)) continue;
            }
        } else {
            if (slot || !na::camera::Invert(m, inv)) continue;
        }
        g_jit_cam.valid = na::camera::DecomposeViewProjection(m, g_jit_cam.geo);
        const neural_alias::JitterSample js = neural_alias::temporal_jitter(++g.jit.phase, kJitterPhases);
        std::array<double, 16> src{};
        for (int i = 0; i < 16; ++i) src[i] = m[i];
        const auto jm = neural_alias::jitter_projection(src, js, g.w, g.h);
        na::camera::Matrix jvp{}, jinv{};
        for (int i = 0; i < 16; ++i) jvp[i] = jm[i];
        if (!na::camera::Invert(jvp, jinv)) return false;
        auto& j = g.jit;
        j.applied = false;
        j.res = res;
        j.off = off;
        j.px[0] = js.x_pixels;
        j.px[1] = js.y_pixels;
        j.layout = layout == na::camera::Layout::RowMajor ? 0 : 1;
        for (int l = 0; l < 2; ++l) {
            const auto L = l ? na::camera::Layout::ColumnMajor : na::camera::Layout::RowMajor;
            store_matrix(m, L, j.vp[l]);
            store_matrix(inv, L, j.inv[l]);
            store_matrix(jvp, L, j.jvp[l]);
            store_matrix(jinv, L, j.jinv[l]);
        }
        // Keep the bytes exactly as the game stored them for matching (float round trip).
        std::memcpy(j.vp[j.layout], bytes.data() + g.camera_offset, 64);
        if (inv_here && na::camera::IsInversePair(m, na::camera::LoadMatrix(bytes, static_cast<size_t>(io), layout)))
            std::memcpy(j.inv[j.layout], bytes.data() + io, 64);
        // The camera's separate projection (passes that multiply by P themselves): T * P.
        j.has_proj = false;
        const int64_t po = g.camera_proj_delta == INT64_MIN ? -1 : g.camera_offset + g.camera_proj_delta;
        if (po >= 0 && static_cast<uint64_t>(po) + 64 <= n) {
            const auto pm = na::camera::LoadMatrix(bytes, static_cast<size_t>(po), layout);
            if (is_projection_of(pm, m)) {
                std::array<double, 16> ps{};
                for (int i = 0; i < 16; ++i) ps[i] = pm[i];
                const auto jp = neural_alias::jitter_projection(ps, js, g.w, g.h);
                na::camera::Matrix jpm{};
                for (int i = 0; i < 16; ++i) jpm[i] = jp[i];
                std::memcpy(j.proj, bytes.data() + po, 64);
                store_matrix(jpm, layout, j.jproj);
                j.has_proj = true;
            }
        }
        j.valid = true;
        return true;
    }
    return false;
}

// Replaces this frame's VP / inverse in the bound constant buffers of this batch.
// The frame's sample starts at the first block holding a VP + inverse at the
// learned offset (blocks of the learned camera slot are tried first); then every
// exact copy of that VP / inverse in ANY bound block of the batch is replaced,
// because an earlier pass (a depth pre-pass, often with its own root signature)
// must see the same jittered camera as the scene, or depth tests disagree.
void jitter_batch(ID3D12CommandList* const* lists, UINT count)
{
    // Only while DLAA is producing frames: the previous game frame submitted one
    // (a batch without the scene must not count as "not running").
    const bool running = g.submitted_frame && g.frame == g.submitted_frame + 1;
    // Inline NGX parameters are fixed at recording. An earlier submitted camera
    // batch may start the sample; recording then freezes that choice. Never start
    // a new sample at submission after NGX already recorded zero jitter.
    if (!g.jitter_on || (g.inline_mode && g.inline_sample_locked && !g.jit.valid) ||
        g.camera_param < 0 || g.camera_offset < 0 || !running || !g.w) return;
    struct JB { uint64_t res, off; uint32_t size; bool slot; };
    std::map<std::pair<uint64_t, uint64_t>, JB> blocks;
    for (UINT i = 0; i < count && blocks.size() < 256; ++i) {
        if (!lists[i]) continue;
        for (const auto& b : na::observe::ListRootConstantBuffers(lists[i])) {
            const uint32_t sz = b.table && b.size ? b.size : kBlockBytes;
            JB& k = blocks[{b.resource, b.offset}];
            k.res = b.resource;
            k.off = b.offset;
            k.size = sz < kMaxBlockRead ? sz : kMaxBlockRead;
            k.slot = k.slot || (b.param == static_cast<uint32_t>(g.camera_param) && b.table == g.camera_table);
            if (blocks.size() >= 256) break;
        }
    }
    std::vector<const JB*> order;
    for (const auto& kv : blocks) order.push_back(&kv.second);
    std::stable_sort(order.begin(), order.end(), [](const JB* a, const JB* b) { return a->slot && !b->slot; });
    std::vector<uint8_t> bytes(kMaxBlockRead);
    for (const JB* k : order) {
        const uint32_t n = na::observe::ReadBuffer(k->res, k->off, bytes.data(), k->size);
        if (n < 64) continue;
        std::vector<uint8_t> view(bytes.begin(), bytes.begin() + n);
        const auto holds = [&](const uint8_t (&m)[2][64]) {
            for (size_t o = 0; o + 64 <= n; o += 16)
                if (!std::memcmp(view.data() + o, m[0], 64) || !std::memcmp(view.data() + o, m[1], 64)) return true;
            return false;
        };
        if (!g.jit.valid || !holds(g.jit.vp)) {
            if (g.jit.valid && holds(g.jit.jvp)) continue; // already jittered by an earlier batch of this frame
            if (g.jit.valid) { ++g.stats.other_camera_blocks; continue; } // a different matrix (another camera): leave it
            if (!begin_jitter(k->res, k->off, view, k->slot)) continue;
        }
        if (k->slot) { // adopt the game's exact inverse bytes (they may differ from a computed inverse)
            const int64_t vo = g.camera_offset, io = vo + g.camera_inv_delta;
            if (io >= 0 && static_cast<uint64_t>(io) + 64 <= n && static_cast<uint64_t>(vo) + 64 <= n &&
                !std::memcmp(view.data() + vo, g.jit.vp[g.jit.layout], 64) && std::memcmp(view.data() + io, g.jit.inv[g.jit.layout], 64)) {
                const auto L = g.jit.layout ? na::camera::Layout::ColumnMajor : na::camera::Layout::RowMajor;
                if (na::camera::IsInversePair(na::camera::LoadMatrix(view, static_cast<size_t>(vo), L), na::camera::LoadMatrix(view, static_cast<size_t>(io), L)))
                    std::memcpy(g.jit.inv[g.jit.layout], view.data() + io, 64);
            }
            // ...and the camera's projection, if the frame started in a block without it.
            const int64_t po = g.camera_proj_delta == INT64_MIN ? -1 : vo + g.camera_proj_delta;
            if (!g.jit.has_proj && po >= 0 && static_cast<uint64_t>(po) + 64 <= n && static_cast<uint64_t>(vo) + 64 <= n &&
                !std::memcmp(view.data() + vo, g.jit.vp[g.jit.layout], 64)) {
                const auto L = g.jit.layout ? na::camera::Layout::ColumnMajor : na::camera::Layout::RowMajor;
                const auto vm = na::camera::LoadMatrix(view, static_cast<size_t>(vo), L);
                const auto pm = na::camera::LoadMatrix(view, static_cast<size_t>(po), L);
                if (is_projection_of(pm, vm)) {
                    std::array<double, 16> ps{};
                    for (int i = 0; i < 16; ++i) ps[i] = pm[i];
                    const auto jp = neural_alias::jitter_projection(ps, neural_alias::JitterSample{g.jit.px[0], g.jit.px[1]}, g.w, g.h);
                    na::camera::Matrix jpm{};
                    for (int i = 0; i < 16; ++i) jpm[i] = jp[i];
                    std::memcpy(g.jit.proj, view.data() + po, 64);
                    store_matrix(jpm, L, g.jit.jproj);
                    g.jit.has_proj = true;
                }
            }
        }
        for (size_t o = 0; o + 64 <= n; o += 16)
            for (int l = 0; l < 2; ++l) {
                const uint8_t* rep = nullptr;
                if (!std::memcmp(view.data() + o, g.jit.vp[l], 64)) rep = g.jit.jvp[l];
                else if (!std::memcmp(view.data() + o, g.jit.inv[l], 64)) rep = g.jit.jinv[l];
                else if (l == 0 && g.jit.has_proj && !std::memcmp(view.data() + o, g.jit.proj, 64)) { rep = g.jit.jproj; ++g.stats.jitter_proj_writes; }
                if (rep && na::observe::WriteBuffer(k->res, k->off + o, rep, 64) == 64) {
                    ++g.stats.jitter_writes;
                    if (rep == g.jit.jvp[l]) g.jit.applied = true;
                }
                if (rep) break;
            }
    }
}

// J1 coverage diagnostics: work drawn AFTER this frame's DLAA that still uses the
// jittered camera or the (jittered) scene depth gets the shake without DLAA
// resolving it (shimmer). Counted per list.
void count_post_dlaa(ID3D12CommandList* const* lists, UINT first, UINT count, na_object_id depth_id)
{
    const size_t lo = g.camera_offset >= 0 ? static_cast<size_t>(g.camera_offset) : 0;
    std::vector<uint8_t> bytes(64);
    for (UINT i = first; i < count; ++i) {
        if (!lists[i]) continue;
        if (depth_id && na::observe::ListReferences(lists[i], depth_id)) ++g.stats.post_depth_lists;
        if (g.camera_offset < 0) continue;
        for (const auto& b : na::observe::ListRootConstantBuffers(lists[i])) {
            if (na::observe::ReadBuffer(b.resource, b.offset + lo, bytes.data(), 64) != 64) continue;
            if (!std::memcmp(bytes.data(), g.jit.jvp[0], 64) || !std::memcmp(bytes.data(), g.jit.jvp[1], 64)) { ++g.stats.post_camera_lists; break; }
        }
    }
}

// This frame's camera: a constant buffer holding a perspective ViewProjection
// together with its inverse, bound in this batch up to the scene list. Root CBVs
// and CBVs reached through descriptor tables both count (Anno binds its camera
// through a table). Lists that use the scene depth are searched first, then the
// rest; within that, the binding slot that held the camera last time leads.
struct CamDiag { size_t lists = 0, binds = 0, blocks = 0, read = 0, vp = 0, cands = 0; } g_cd;

struct Block {
    uint64_t resource = 0, offset = 0;
    uint32_t size = 0, param = 0, uses = 0, list_rank = 0;
    bool table = false;
};

bool check_learned(const std::vector<uint8_t>& bytes, na::camera::Matrix* vp, bool* reversed)
{
    const size_t n = bytes.size();
    if (g.camera_offset < 0 || static_cast<uint64_t>(g.camera_offset) + 64 > n) return false;
    for (auto layout : {na::camera::Layout::RowMajor, na::camera::Layout::ColumnMajor}) {
        const auto m = na::camera::LoadMatrix(bytes, static_cast<size_t>(g.camera_offset), layout);
        na::camera::Candidate c{};
        if (!na::camera::ViewProjection(m, c)) continue;
        for (int64_t d : {g.camera_inv_delta}) { // the learned inverse position (Anno: not adjacent)
            const int64_t off = g.camera_offset + d;
            if (off < 0 || static_cast<uint64_t>(off) + 64 > n) continue;
            if (na::camera::IsInversePair(m, na::camera::LoadMatrix(bytes, static_cast<size_t>(off), layout))) {
                *vp = m;
                *reversed = c.reversed;
                return true;
            }
        }
    }
    return false;
}

bool g_injected = false; // the camera found this frame carries J1's jitter (g.jit.px)

bool find_camera(ID3D12CommandList* const* lists, UINT scene, na_object_id depth_id, na::camera::Matrix* vp, bool* reversed)
{
    g_cd = {};
    g_injected = false;
    // Lists to search: the scene list, then earlier lists using the depth, then the others.
    std::vector<std::pair<uint32_t, UINT>> order_lists;
    for (int i = static_cast<int>(scene); i >= 0; --i) {
        if (!lists[i]) continue;
        const uint32_t rank = static_cast<UINT>(i) == scene ? 0u : (na::observe::ListReferences(lists[i], depth_id) ? 1u : 2u);
        order_lists.push_back({rank, static_cast<UINT>(i)});
    }
    std::stable_sort(order_lists.begin(), order_lists.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::map<std::pair<uint64_t, uint64_t>, Block> blocks;
    for (const auto& ol : order_lists) {
        const auto binds = na::observe::ListRootConstantBuffers(lists[ol.second]);
        if (binds.empty()) continue;
        ++g_cd.lists;
        g_cd.binds += binds.size();
        for (const auto& b : binds) {
            Block& k = blocks[{b.resource, b.offset}];
            if (!k.uses) {
                k.resource = b.resource;
                k.offset = b.offset;
                k.size = b.table && b.size ? b.size : kBlockBytes;
                k.param = b.param;
                k.table = b.table;
                k.list_rank = ol.first;
            }
            ++k.uses;
        }
    }
    std::vector<const Block*> order;
    order.reserve(blocks.size());
    for (const auto& kv : blocks) order.push_back(&kv.second);
    const auto learned = [](const Block* b) { return g.camera_param >= 0 && b->param == static_cast<uint32_t>(g.camera_param) && b->table == g.camera_table; };
    std::stable_sort(order.begin(), order.end(), [&](const Block* a, const Block* b) {
        if (learned(a) != learned(b)) return learned(a);
        if (a->list_rank != b->list_rank) return a->list_rank < b->list_rank;
        return a->uses > b->uses;
    });
    if (order.size() > kMaxBlocks) order.resize(kMaxBlocks);
    g_cd.blocks = order.size();
    std::vector<uint8_t> bytes(kMaxBlockRead);
    for (const Block* o : order) {
        const uint32_t want = o->size < kMaxBlockRead ? o->size : kMaxBlockRead;
        const uint32_t n = na::observe::ReadBuffer(o->resource, o->offset, bytes.data(), want);
        if (n < 128) continue;
        ++g_cd.read;
        // An in-process read at submission time, inside the submitting call:
        // its scope is known (FindMatrices rejects snapshots without one).
        na::camera::Snapshot s{};
        s.session = s.epoch = 1;
        s.submission = s.content_version = g.frame;
        s.resource = o->resource;
        s.base_offset = 0; // offsets are relative to the block
        s.complete = s.lifetime_valid = true;
        s.sync = na::camera::Sync::AppSyncUnknown;
        s.bytes.assign(bytes.begin(), bytes.begin() + n);
        if (learned(o) && check_learned(s.bytes, vp, reversed)) {
            ++g.stats.camera_learned_hits;
            const size_t lo = static_cast<size_t>(g.camera_offset);
            g_injected = g.jit.valid && lo + 64 <= s.bytes.size() &&
                         (!std::memcmp(s.bytes.data() + lo, g.jit.jvp[0], 64) || !std::memcmp(s.bytes.data() + lo, g.jit.jvp[1], 64));
            return true;
        }
        const auto scan = na::camera::FindMatrices(s);
        g_cd.cands += scan.candidates.size();
        for (const auto& c : scan.candidates) {
            if (c.kind == na::camera::Kind::ViewProjection) ++g_cd.vp;
            if (c.kind != na::camera::Kind::ViewProjection || !c.has_inverse_pair) continue;
            g.camera_offset = static_cast<int64_t>(c.byte_offset);
            g.camera_inv_delta = static_cast<int64_t>(c.inverse_byte_offset) - static_cast<int64_t>(c.byte_offset);
            g.camera_proj_delta = INT64_MIN;
            for (const auto& pc : scan.candidates)
                if (pc.kind == na::camera::Kind::Projection && pc.layout == c.layout && is_projection_of(pc.matrix, c.matrix)) {
                    g.camera_proj_delta = static_cast<int64_t>(pc.byte_offset) - static_cast<int64_t>(c.byte_offset);
                    break;
                }
            g.stats.camera_proj_delta = g.camera_proj_delta == INT64_MIN ? 0 : g.camera_proj_delta;
            g.stats.camera_inv_delta = g.camera_inv_delta;
            ++g.stats.camera_scans;
            g.camera_param = static_cast<int64_t>(o->param);
            g.camera_table = o->table;
            g.stats.camera_offset = c.byte_offset;
            *vp = c.matrix;
            *reversed = c.reversed;
            const size_t lo = static_cast<size_t>(c.byte_offset);
            g_injected = g.jit.valid && lo + 64 <= s.bytes.size() &&
                         (!std::memcmp(s.bytes.data() + lo, g.jit.jvp[0], 64) || !std::memcmp(s.bytes.data() + lo, g.jit.jvp[1], 64));
            return true;
        }
    }
    return false;
}

na::motion::Mat4 to_mat4(const na::camera::Matrix& m)
{
    na::motion::Mat4 r{};
    for (int i = 0; i < 16; ++i) r.m[i] = static_cast<float>(m[i]);
    return r;
}

// Records the DLAA list for this frame. Returns false (nothing recorded worth
// submitting) when a precondition fails.
// inl: record INTO this game command list (inline insertion, during the game's
// recording) instead of J2's own list; *touched reports whether anything was
// recorded into it (the caller then restores the game's state).
bool record(ID3D12CommandList* const* lists, UINT scene, na_object_id depth_id, na_object_id color_id,
            ID3D12GraphicsCommandList** out_list, PendingHistory& pending, ID3D12GraphicsCommandList* inl = nullptr,
            bool* touched = nullptr)
{
    ComPtr<ID3D12Resource> color, depth;
    color.Attach(na::observe::AcquireResource(color_id));
    depth.Attach(na::observe::AcquireResource(depth_id));
    if (!color || !depth) { reason("Scene resources are not available."); return false; }
    const D3D12_RESOURCE_DESC cd = color->GetDesc(), dd = depth->GetDesc();
    g.stats.color_mips = cd.MipLevels ? cd.MipLevels : 1;
    g.stats.color_slices = cd.DepthOrArraySize;
    g.stats.color_samples = cd.SampleDesc.Count;
    g.stats.depth_samples = dd.SampleDesc.Count;
    g.stats.depth_slices = dd.DepthOrArraySize;
    // AA settings are not an eligibility condition. Each input is converted
    // independently: a game may already have resolved color but retain MSAA depth.
    if (cd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || dd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        reason("The scene colour or depth is not a 2D texture.");
        return false;
    }
    // The subresources (mip + slice) the scene pass rendered into, from its views:
    // arrays and mip chains are fine, J2 works on exactly that one through its own copies.
    uint32_t csub = 0, dsub = 0;
    if (!na::observe::ListSceneSubresource(lists[scene], color_id, depth_id, &csub, &dsub) || csub == 0xFFFFFFFFu || dsub == 0xFFFFFFFFu) {
        reason("The scene is drawn through a view NeuralAlias cannot place (several array slices at once, or an unusual view).");
        return false;
    }
    // States of exactly the two subresources J2 copies from and restores.
    uint32_t sc = 0, sd = 0;
    // Inline: the state recorded so far in this list, else the state all submitted work left.
    const auto state_now = [&](na_object_id id, uint32_t sub, uint32_t* st) {
        return na::observe::ListLastStateSub(inl, id, sub, st) || na::observe::KnownSubStateNow(id, sub, st);
    };
    const bool have_sc = inl ? state_now(color_id, csub, &sc) : state_after(lists, scene, color_id, csub, &sc);
    const bool have_sd = inl ? state_now(depth_id, dsub, &sd) : state_after(lists, scene, depth_id, dsub, &sd);
    if (!have_sc || !have_sd) {
        ++g.stats.no_state;
        if (!have_sc) ++g.stats.no_state_color; else ++g.stats.no_state_depth;
        reason(!have_sc ? "Waiting: the scene colour's state at the end of the scene pass is not established."
                        : "Waiting: the scene depth's state at the end of the scene pass is not established.");
        return false;
    }
    g.stats.color_sub = csub;
    const uint32_t cmip = csub % g.stats.color_mips;
    const uint32_t dmip = dsub % (dd.MipLevels ? dd.MipLevels : 1u);
    const uint32_t w = (std::max)(1u, static_cast<uint32_t>(cd.Width >> cmip)), h = (std::max)(1u, cd.Height >> cmip);
    if ((std::max)(1u, static_cast<uint32_t>(dd.Width >> dmip)) != w || (std::max)(1u, dd.Height >> dmip) != h ||
        !na::depth::PlaneZeroCopiesToR32F(dd.Format)) {
        reason("Scene depth does not match the colour size or is not 32-bit depth.");
        return false;
    }
    const DXGI_FORMAT cf = typed(cd.Format); // our copies' format
    const bool color_msaa = cd.SampleDesc.Count > 1, depth_msaa = dd.SampleDesc.Count > 1;

    // Camera first (nothing is allocated for a frame that cannot run); the tracker
    // update comes after the size check, because a resize resets the tracker.
    na::camera::Matrix vp{};
    bool reversed = false;
    if (!find_camera(lists, scene, depth_id, &vp, &reversed)) {
        ++g.stats.no_camera;
        g.camera_offset = g.camera_param = -1; // relearn
        char why[160];
        std::snprintf(why, sizeof why, "Waiting for the camera (lists %zu, binds %zu, blocks %zu, read %zu, matrices %zu, view-proj %zu).",
                      g_cd.lists, g_cd.binds, g_cd.blocks, g_cd.read, g_cd.cands, g_cd.vp);
        reason(why);
        return false;
    }
    g_jit_cam.valid = na::camera::DecomposeViewProjection(vp, g_jit_cam.geo); // J1: the main camera, for earlier batches next frame
    // Ring slot (never wait) and size/format changes (only once the GPU is done with the old set).
    const UINT64 done = g.fence->GetCompletedValue();
    Slot& s = g.slots[g.slot];
    if (done == UINT64_MAX) { reason("Device removed."); return false; }
    if (s.fence_value && done < s.fence_value) { ++g.stats.busy; reason("Skipped: the previous use of this frame slot is still running."); return false; }
    const bool resize = !g.output || !g.color_in || g.w != w || g.h != h || g.color_format != cf ||
                        g.samples != cd.SampleDesc.Count || g.sample_quality != cd.SampleDesc.Quality ||
                        g.depth_samples != dd.SampleDesc.Count || g.depth_quality != dd.SampleDesc.Quality || g.depth_format != dd.Format;
    if (resize) {
        if (done + 1 < g.fence_next) { ++g.stats.busy; reason("Waiting for the previous picture size to finish."); return false; }
        if (!uav_store(cf) || !uav_store(DXGI_FORMAT_R32G32_FLOAT)) { reason("This scene colour format cannot be written by DLSS here."); return false; }
        g.msaa = MsaaResolve{};
        if (color_msaa || depth_msaa) {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT support{cf};
            if (FAILED(g.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof support)) ||
                (color_msaa && !(support.Support1 & D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE)) ||
                !g.msaa.Init(g.device.Get(), w, h, cf, dd, cd.SampleDesc, kRing)) {
                g.output.Reset();
                reason("Could not prepare the MSAA resolve pipeline.");
                return false;
            }
        }
        if (!texture(w, h, DXGI_FORMAT_R32_FLOAT, depth_msaa, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, g.depth32) ||
            !texture(w, h, DXGI_FORMAT_R32G32_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g.motion) ||
            !texture(w, h, DXGI_FORMAT_R8_UINT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g.provenance) ||
            !texture(w, h, cf, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g.output) ||
            !texture(w, h, cf, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, g.color_in) ||
            !texture(w, h, cf, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g.sharp)) {
            g.output.Reset();
            reason("Could not allocate the DLAA working textures.");
            return false;
        }
        g.w = w; g.h = h; g.color_format = cf;
        g.samples = cd.SampleDesc.Count; g.sample_quality = cd.SampleDesc.Quality;
        g.depth_samples = dd.SampleDesc.Count; g.depth_quality = dd.SampleDesc.Quality; g.depth_format = dd.Format;
        g.resume_reset = true; // remains pending if this recording is not submitted
    }
    const double zero[2] = {0, 0};
    const double* injected = g_injected ? g.jit.px : zero; // J1: the exact sample we injected (removed from the matrix)
    pending.tracker = g.tracker;
    // DLSS's history is the last frame it evaluated, and the tracker's previous
    // camera is that same frame, so motion stays correct across a few skipped game
    // frames (a frame whose scene submissions differed): no reset, no visible
    // restart. A long gap (or resume/resize) starts over.
    constexpr uint64_t kMaxGapFrames = 4;
    if (g.resume_reset || (g.submitted_frame && g.frame > g.submitted_frame + kMaxGapFrames)) pending.tracker.Reset();
    const na::camera::CameraFrame cam = pending.tracker.Update(vp, g.frame, w, h, injected);
    if (!cam.valid) { ++g.stats.no_camera; reason("The camera matrix could not be inverted."); return false; }
    pending.cut = cam.camera_cut;
    if (!inl && (FAILED(s.alloc->Reset()) || FAILED(s.list->Reset(s.alloc.Get(), nullptr)))) { reason("Could not reset the DLAA command list."); return false; }
    ID3D12GraphicsCommandList* l = inl ? inl : s.list.Get();
    if (touched) *touched = true;
    std::vector<D3D12_RESOURCE_BARRIER> b;
    constexpr UINT NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, SRC = D3D12_RESOURCE_STATE_COPY_SOURCE,
                   DST = D3D12_RESOURCE_STATE_COPY_DEST, UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    // 1. Copy or resolve exactly the scene views into single-sample DLSS inputs.
    const UINT color_source = color_msaa ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE : SRC;
    const UINT color_dest = color_msaa ? D3D12_RESOURCE_STATE_RESOLVE_DEST : DST;
    addsub(b, depth.Get(), dsub, sd, SRC);
    add(b, g.depth32.Get(), NPSR, depth_msaa ? UAV : DST);
    addsub(b, color.Get(), csub, sc, color_source);
    add(b, g.color_in.Get(), NPSR, color_dest);
    flush(l, b);
    if (color_msaa) {
        l->ResolveSubresource(g.color_in.Get(), 0, color.Get(), csub, cf);
    } else {
        D3D12_TEXTURE_COPY_LOCATION cs{}, cdst{};
        cs.Type = cdst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        cs.pResource = color.Get(); // the subresource the scene was drawn into
        cs.SubresourceIndex = csub;
        cdst.pResource = g.color_in.Get();
        l->CopyTextureRegion(&cdst, 0, 0, 0, &cs, nullptr);
    }
    add(b, g.color_in.Get(), color_dest, NPSR);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.Type = dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = depth.Get();
    src.SubresourceIndex = dsub; // plane 0 of the slice / mip the scene used
    dst.pResource = depth_msaa ? g.msaa.DepthCopy() : g.depth32.Get();
    dst.SubresourceIndex = 0;
    l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    addsub(b, depth.Get(), dsub, SRC, sd);
    if (depth_msaa) {
        addsub(b, g.msaa.DepthCopy(), 0, DST, NPSR);
        flush(l, b);
        g.msaa.ResolveDepth(l, g.depth32.Get(), w, h, reversed, g.slot);
        b.push_back(uav_barrier(g.depth32.Get()));
        add(b, g.depth32.Get(), UAV, NPSR);
        addsub(b, g.msaa.DepthCopy(), 0, NPSR, DST);
    } else add(b, g.depth32.Get(), DST, NPSR);
    flush(l, b);
    // 2/3. camera motion vectors
    bool have_motion = false;
    if (g.arbiter_ok) {
        na::motion::gpu::Inputs in{};
        in.depth = g.depth32.Get();
        in.tier2_valid_flag = true;
        in.depth_clear = reversed ? 0.0f : 1.0f;
        // One composed current->previous clip matrix (double, then float); the
        // arbiter's second matrix is identity. Never chain two float matrices
        // through world space (false motion on a still camera -> DLAA blur).
        in.inv_view_proj_current = to_mat4(na::camera::ComposeReprojectionRow(cam.inv_view_proj_current_row, cam.view_proj_previous_row));
        {
            na::camera::Matrix id{};
            id[0] = id[5] = id[10] = id[15] = 1.0;
            in.view_proj_previous = to_mat4(id);
        }
        // J1: this frame's raster sample sits at pixel centre - jitter in the unjittered
        // projection; the arbiter removes it before reprojecting (Astra, 378663e finding).
        in.jitter_current_px[0] = static_cast<float>(cam.jitter_px_x);
        in.jitter_current_px[1] = static_cast<float>(cam.jitter_px_y);
        na::motion::gpu::Outputs o{};
        o.xy = g.motion.Get();
        o.provenance = g.provenance.Get();
        o.width = w;
        o.height = h;
        have_motion = g.arbiter.Dispatch(l, in, o, g.slot);
    }
    if (!have_motion) {
        if (inl) { // hand the game's list back with the scene colour in its own state
            addsub(b, color.Get(), csub, color_source, sc);
            flush(l, b);
        } else {
            l->Close();
        }
        reason("Stopped: motion vectors could not be built.");
        g.latched = true;
        return false;
    }
    b.push_back(uav_barrier(g.motion.Get()));
    add(b, g.motion.Get(), UAV, NPSR);
    flush(l, b);
    // 4. DLSS at native resolution
    na::dlss::EvalDesc e{};
    e.color = g.color_in.Get();
    e.depth = g.depth32.Get();
    e.motion = g.motion.Get();
    e.output = g.output.Get();
    e.render_w = e.max_render_w = e.out_w = w;
    e.render_h = e.max_render_h = e.out_h = h;
    e.quality = na::dlss::Quality::DLAA;
    e.preset = na::settings::Get().j2_preset.load(); // Cort: preset L by default
    e.view_id = kViewId;
    e.jitter_x = static_cast<float>(cam.jitter_px_x); // DLSS: colour sampled at pixel centre - jitter
    e.jitter_y = static_cast<float>(cam.jitter_px_y);
    if (inl) {
        g.stats.inline_jitter_x = e.jitter_x;
        g.stats.inline_jitter_y = e.jitter_y;
    }
    e.reset = cam.camera_cut || resize || g.resume_reset;
    e.hdr = is_float_color(cf);
    e.depth_inverted = reversed;
    g.stats.depth_reversed = reversed;
    e.auto_exposure = true;
    const bool evaluated = na::dlss::Evaluate(l, e);
    if (!evaluated) ++g.stats.dlss_failed;
    if (evaluated && na::capture::j2::Armed()) {
        // Evidence capture: copied in this same list right after the evaluate, so the GPU
        // order makes them exactly this evaluation's inputs and raw DLSS output.
        na::capture::j2::Planes cp;
        cp.color = g.color_in.Get();
        cp.depth = g.depth32.Get();
        cp.motion = g.motion.Get();
        cp.output = g.output.Get();
        na::capture::j2::Identity ci;
        ci.present_ordinal = g.frame;
        ci.submission = g.stats.submissions;
        ci.recording = inl ? na::observe::ListRecording(inl) : 0;
        ci.view_color = color_id;
        ci.view_depth = depth_id;
        ci.view_id = static_cast<uint32_t>(kViewId);
        const auto& rd = g.router.Current();
        ci.route = static_cast<uint32_t>(inl ? na::j2::Route::Inline : na::j2::Route::Between);
        ci.route_mode = static_cast<uint32_t>(g.router.Mode());
        ci.history_generation = rd.history_generation;
        // Inline: the root-layout hash of the consumer class that triggered DLAA, when a class did,
        // else the route's boundary signature.
        ci.insertion_signature = inl ? (g.class_fired ? g.fired_class.root : rd.signature) : 0;
        ci.color_sub = csub;
        ci.width = w;
        ci.height = h;
        ci.color_format = static_cast<uint32_t>(cf);
        ci.jitter_x = e.jitter_x;
        ci.jitter_y = e.jitter_y;
        ci.mv_scale_x = e.mv_scale_x;
        ci.mv_scale_y = e.mv_scale_y;
        ci.reset = e.reset ? 1u : 0u;
        ci.camera_cut = cam.camera_cut ? 1u : 0u;
        ci.preset = static_cast<uint32_t>(e.preset);
        ci.sharpness = na::settings::Get().j2_sharpness.load(std::memory_order_relaxed);
        ci.debug_view = static_cast<uint32_t>(g_debug_view.load(std::memory_order_relaxed));
        pending.capture = na::capture::j2::RecordEvaluation(l, cp, ci);
    }
    if (evaluated) {
        const int dv = g_debug_view.load(std::memory_order_relaxed);
        if (dv >= 1 && dv <= 3) {
            b.push_back(uav_barrier(g.output.Get()));
            flush(l, b);
            debug_record(l, dv, cf); // replaces the DLAA result with the debug picture
        } else if (const uint32_t pct = na::settings::Get().j2_sharpness.load(std::memory_order_relaxed)) {
            // RCAS: output -> sharp, then back into output so the write-back path is unchanged.
            // Above 100 %, NIS sharpens sharp -> output instead of the copy.
            b.push_back(uav_barrier(g.output.Get()));
            add(b, g.output.Get(), UAV, NPSR);
            flush(l, b);
            if (pct > 100 && rcas_record(l, 100, cf)) {
                b.push_back(uav_barrier(g.sharp.Get()));
                add(b, g.sharp.Get(), UAV, NPSR);
                add(b, g.output.Get(), NPSR, UAV);
                flush(l, b);
                if (g_nis.Dispatch(g.device.Get(), l, g.sharp.Get(), g.output.Get(), g.w, g.h, cf, is_float_color(cf), pct, g.slot)) {
                    ++g.stats.nis_frames;
                    add(b, g.sharp.Get(), NPSR, UAV);
                } else { // NIS unavailable: keep the RCAS result
                    add(b, g.output.Get(), UAV, DST);
                    add(b, g.sharp.Get(), NPSR, SRC);
                    flush(l, b);
                    l->CopyResource(g.output.Get(), g.sharp.Get());
                    add(b, g.output.Get(), DST, UAV);
                    add(b, g.sharp.Get(), SRC, UAV);
                }
                ++g.stats.sharpened_frames;
            } else if (pct <= 100 && rcas_record(l, pct, cf)) {
                b.push_back(uav_barrier(g.sharp.Get()));
                add(b, g.sharp.Get(), UAV, SRC);
                add(b, g.output.Get(), NPSR, DST);
                flush(l, b);
                l->CopyResource(g.output.Get(), g.sharp.Get());
                add(b, g.sharp.Get(), SRC, UAV);
                add(b, g.output.Get(), DST, UAV);
                ++g.stats.sharpened_frames;
            } else {
                add(b, g.output.Get(), NPSR, UAV);
            }
            flush(l, b);
        }
    }
    // 5. result -> scene colour, everything back to the game's states
    if (evaluated) {
        b.push_back(uav_barrier(g.output.Get()));
        const UINT output_read = color_msaa ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : SRC;
        const UINT color_write = color_msaa ? D3D12_RESOURCE_STATE_RENDER_TARGET : DST;
        add(b, g.output.Get(), UAV, output_read);
        addsub(b, color.Get(), csub, color_source, color_write);
        flush(l, b);
        if (color_msaa) {
            g.msaa.WriteColor(l, g.output.Get(), color.Get(), csub / g.stats.color_mips, w, h, g.slot);
        } else {
            D3D12_TEXTURE_COPY_LOCATION cs{}, cdst{};
            cs.Type = cdst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            cs.pResource = g.output.Get();
            cdst.pResource = color.Get(); // back into the scene's subresource
            cdst.SubresourceIndex = csub;
            l->CopyTextureRegion(&cdst, 0, 0, 0, &cs, nullptr);
        }
        add(b, g.output.Get(), output_read, UAV);
        addsub(b, color.Get(), csub, color_write, sc);
    } else {
        addsub(b, color.Get(), csub, color_source, sc);
    }
    add(b, g.motion.Get(), NPSR, UAV);
    flush(l, b);
    const bool closed = inl ? true : SUCCEEDED(l->Close()); // the game closes its own list
    if (!closed) {
        // A rejected list repeats every frame and may cost the device: stop for this
        // session and say exactly what was being processed. Never retried.
        char why[160];
        std::snprintf(why, sizeof why, "Stopped: the DLAA list was rejected (colour fmt %u mips %u slices %u state 0x%X, depth fmt %u state 0x%X). %s",
                      static_cast<unsigned>(cd.Format), static_cast<unsigned>(cd.MipLevels), static_cast<unsigned>(cd.DepthOrArraySize), sc, static_cast<unsigned>(dd.Format), sd,
                      na::dlss::GetStats().reason);
        reason(why);
        g.latched = true;
        return false;
    }
    if (!evaluated) {
        char why[160];
        std::snprintf(why, sizeof why, "DLSS refused the frame: %s", na::dlss::GetStats().reason[0] ? na::dlss::GetStats().reason : "no reason given.");
        reason(why);
        return false; // nothing useful to submit; retried next frame
    }
    g.stats.width = w;
    g.stats.height = h;
    g.stats.color_format = static_cast<uint32_t>(cd.Format);
    *out_list = l;
    return true;
}

// Inline insertion runs immediately before a draw/dispatch whose bound SRVs
// include scene colour. The consuming PSO/targets are now established; bindings
// alone are not reads. Prior recordings teach the first use after the final
// scene write. The host restores game state before forwarding the real call.
void on_table_bind(ID3D12GraphicsCommandList* l, const na_object_id* srvs, uint32_t n, bool compute)
{
    if (!g.enabled.load(std::memory_order_relaxed) || g.paused.load(std::memory_order_relaxed)) return;
    uint64_t depth_id = 0, color_id = 0;
    na::observe::GetLiveSelection(&depth_id, &color_id);
    if (!color_id || !depth_id) return;
    bool hit = false;
    for (uint32_t i = 0; i < n && !hit; ++i) hit = srvs[i] == color_id;
    if (!hit) return;
    const na::observe::HostWorkScope host; // our commands in the game's list are not the game's
    std::lock_guard<std::mutex> lock(g.m);
    if (!g.inline_mode || g.latched || !g.device) return;
    if (!na::observe::ListRendersTo(l, color_id, depth_id)) return; // the scene was not drawn in this recording
    const uint64_t rec = na::observe::ListRecording(l);
    if (!rec) return;
    uint32_t csub = 0, dsub = 0, cst = 0;
    if (!na::observe::ListSceneSubresource(l, color_id, depth_id, &csub, &dsub)) return;
    na_object_id cur_rt = 0, cur_ds = 0;
    na::observe::ListCurrentTargets(l, &cur_rt, &cur_ds);
    const bool reading = compute || cur_rt != color_id ||
                         (na::observe::ListLastStateSub(l, color_id, csub, &cst) && !(cst & D3D12_RESOURCE_STATE_RENDER_TARGET));
    if (!reading) return;
    ++g.stats.inline_candidates;
    if (g.inl_frame == g.frame) return; // one DLAA per frame: later candidates are post-processing
    const auto current = na::observe::ListInsertionDecision(l, color_id);
    const auto& learned = g.insertion_learned;
    // Role-class trigger (placement design): at the ACTUAL consumer (draw/dispatch/copy),
    // fire when its class is accepted by the profile. A class known to be a temporary
    // reader blocks the boundary matcher too (a fact from earlier frames). Otherwise the
    // boundary matcher below decides exactly as before, so coverage never drops below it.
    enum : uint32_t { kByClass = 1, kByPair = 2, kByLegacy = 4 };
    uint32_t matched_mode = kByLegacy;
    bool by_class = false;
    na::j2::ClassKey live_class;
    uint64_t live_producer = 0;
    {
        const auto live = na::observe::ListLiveConsumer(l, color_id);
        // An MSAA depth resolve draws (render target, viewport, topology), which the state
        // restore does not cover: graphics consumers with MSAA depth stay on the boundary matcher.
        const bool raster_safe = live.op != na::j2::Op::GraphicsRead || g.depth_samples <= 1;
        if (live.valid && live.after_write && raster_safe && g.scene_w) {
            live_class = na::j2::MakeClass(live.raw, g.scene_w, g.scene_h);
            live_producer = live.producer_pso;
            using V = na::j2::ConsumerProfile::Verdict;
            const V v = g.profile.Judge(live_class, live_producer);
            if (v == V::Rejected) { ++g.stats.class_rejected_skips; return; }
            if (v == V::Accepted || v == V::AcceptedWithProducer) {
                by_class = true;
                matched_mode = v == V::Accepted ? kByClass : kByPair;
            } else {
                std::snprintf(g.stats.class_note, sizeof g.stats.class_note,
                    "%s class{op=%u root=%llu size=%u/%u fmt=%u srvs=%u/%u} producer=%llu",
                    v == V::Ambiguous ? "ambiguous" : "unknown", static_cast<unsigned>(live_class.op),
                    (unsigned long long)live_class.root, live_class.w_class, live_class.h_class, live_class.fmt,
                    live_class.color_srvs, live_class.view_srvs, (unsigned long long)live_producer);
            }
        }
    }
    if (by_class) {
        if (g.inl.valid) {
            ++g.stats.inline_waited;
            ++g.stats.inline_rejects[Stats::kRejPending];
            if (!g.frame_rejects++)
                std::snprintf(g.frame_reject_text, sizeof g.frame_reject_text, "class trigger: previous recording not submitted");
            return;
        }
        g.stats.inline_target = learned.decision.final_candidate;
    } else {
    // Optional passes change the absolute transition ordinal. Match the producer
    // of the learned final scene boundary, retaining an occurrence only among
    // boundaries from that same producer (not unrelated intermediate reads).
    // Same predicates as before, evaluated one by one so a rejection names its cause.
    using S = Stats;
    uint32_t rej = S::kRejCount;
    if (!learned.decision.final_candidate || !learned.color) rej = S::kRejNoLearned;
    else if (learned.color != color_id || learned.depth != depth_id) rej = S::kRejResource;
    else if (learned.decision.final_transition && learned.decision.boundary_writer) {
        if (current.boundary_writer != learned.decision.boundary_writer) rej = S::kRejWriter;
        else if (current.writer_occurrence != learned.decision.writer_occurrence) rej = S::kRejOccurrence;
    } else if (learned.decision.final_transition) {
        if (current.final_transition != learned.decision.final_transition) rej = S::kRejTransition;
    } else if (current.candidates != learned.decision.final_candidate) rej = S::kRejCandidate;
    if (rej == S::kRejCount && current.destination != learned.decision.destination) rej = S::kRejDestination;
    if (rej == S::kRejCount && current.pipeline != learned.decision.pipeline) rej = S::kRejPipeline;
    if (rej == S::kRejCount) g.stats.inline_target = learned.decision.final_candidate;
    if (rej == S::kRejCount && (g.inl_frame == g.frame || g.inl.valid)) {
        // Already inserted this frame (normal for later candidates) or the previous
        // recording was never submitted (a real stall): only the latter is a rejection.
        if (g.inl.valid && g.inl_frame != g.frame) rej = S::kRejPending;
        else return;
    }
    if (rej != S::kRejCount) {
        ++g.stats.inline_waited;
        ++g.stats.inline_rejects[rej];
        if (!g.frame_rejects++) {
            static const char* kWhy[] = {"no learned boundary", "different scene colour/depth", "different boundary writer",
                                         "different writer occurrence", "different transition ordinal", "different candidate ordinal",
                                         "different consumer destination", "different consumer pipeline", "previous recording not submitted"};
            std::snprintf(g.frame_reject_text, sizeof g.frame_reject_text,
                "reject=%s learned{writer=%llu occ=%u tr=%u cand=%u dst=%llu pso=%llu} actual{writer=%llu occ=%u tr=%u cand=%u dst=%llu pso=%llu}",
                kWhy[rej], (unsigned long long)learned.decision.boundary_writer, learned.decision.writer_occurrence,
                learned.decision.final_transition, learned.decision.final_candidate, (unsigned long long)learned.decision.destination,
                (unsigned long long)learned.decision.pipeline, (unsigned long long)current.boundary_writer, current.writer_occurrence,
                current.final_transition, current.candidates, (unsigned long long)current.destination, (unsigned long long)current.pipeline);
        }
        return;
    }
    } // legacy boundary matcher
    if (!na::observe::ListStateRestorable(l)) { ++g.stats.inline_refused; reason("The scene list's state cannot be restored after DLAA here (a bundle ran, or inside a render pass); left untouched."); return; }
    ID3D12CommandList* one[1] = {l};
    // The depth/earlier scene batches must already have established this frame's
    // raster sample. Apply that SAME sample to camera copies in this recording
    // before reading its camera and recording motion/NGX parameters.
    // Recording can precede ALL camera submissions. In that order establish the
    // sample here; Execute patches the earlier recorded pre-pass to this same
    // sample before forwarding it. Do not start late after an unpatched camera
    // batch has already been forwarded to the GPU.
    if ((g.jit.valid && g.jit.applied) || !g.inline_camera_submitted) jitter_batch(one, 1);
    else g.jit.valid = false;
    g.inline_sample_locked = true;
    PendingHistory pending;
    ID3D12GraphicsCommandList* out = nullptr;
    bool touched = false;
    const uint32_t slot = g.slot;
    const bool ok = record(one, 0, depth_id, color_id, &out, pending, l, &touched);
    if (touched && !na::observe::RestoreListState(l)) {
        // Checked as restorable above; this cannot normally happen. Stop for the session.
        g.latched = true;
        na::capture::j2::Discard(pending.capture, true);
        reason("Stopped: the game's list state could not be restored after inline DLAA.");
        return;
    }
    if (!ok) { na::capture::j2::Discard(pending.capture, true); return; } // the game's list still runs our copies
    g.slot = (g.slot + 1) % kRing;
    g.inl.list = l;
    g.inl.rec = rec;
    g.inl.frame = g.frame;
    g.inl.slot = slot;
    g.inl.writes = current.writes;
    g.inl.color = color_id;
    g.inl.history = pending;
    g.inl.valid = true;
    g.inl_frame = g.frame;
    ++g.stats.inline_recorded;
    if (matched_mode == kByClass) ++g.stats.class_matched;
    else if (matched_mode == kByPair) ++g.stats.class_pair_matched;
    else ++g.stats.legacy_matched;
    g.class_fired = by_class;
    g.fired_class = live_class;
    g.fired_producer = live_producer;
}

} // namespace

void SetEnabled(bool on)
{
    g.enabled.store(on);
    g.configured.store(on);
    if (on) na::observe::SetTableBindCallback(&on_table_bind);
}
bool Enabled() { return g.enabled.load(); }
bool Configured() { return g.configured.load(); }
void SetPaused(bool paused)
{
    if (!paused && g.paused.load()) { std::lock_guard<std::mutex> lock(g.m); g.resume_reset = true; }
    g.paused.store(paused);
}
bool Paused() { return g.paused.load(); }
bool RecordRcasForTests(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* in, ID3D12Resource* out,
                        uint32_t w, uint32_t h, uint32_t percent, DXGI_FORMAT fmt)
{
    std::lock_guard<std::mutex> lock(g.m);
    if (!g.device) g.device = device;
    if (g.device.Get() != device) return false;
    return rcas_dispatch(list, in, out, w, h, percent, fmt, 0);
}
bool RecordNisForTests(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* in, ID3D12Resource* out,
                       uint32_t w, uint32_t h, uint32_t percent, DXGI_FORMAT fmt, bool hdr)
{
    std::lock_guard<std::mutex> lock(g.m);
    if (percent > 100 && !g_nis.WaitCompiled()) { std::printf("%s\n", g_nis.Error()); return false; }
    const bool ok = g_nis.Dispatch(device, list, in, out, w, h, fmt, hdr, percent, 0);
    if (!ok && percent > 100) std::printf("NIS dispatch refused: %s\n", g_nis.Error());
    return ok;
}
void SetDebugView(int mode) { g_debug_view.store(mode < 0 || mode > 3 ? 0 : mode); }
int DebugView() { return g_debug_view.load(); }
void SetJitter(bool on)
{
    std::lock_guard<std::mutex> lock(g.m);
    if (g.jitter_on != on) g.resume_reset = true; // the input sequence changes character
    g.jitter_on = on;
    if (!on) g.jit.valid = false;
}
bool Jitter()
{
    std::lock_guard<std::mutex> lock(g.m);
    return g.jitter_on;
}

void OnPresent(ID3D12CommandQueue* queue)
{
    std::lock_guard<std::mutex> lock(g.m);
    if (queue && g.queue.Get() == queue) {
        {
            // Route selector: close this frame's structure + outcome.
            na::j2::FrameObservation& o = g.frame_agg.Observation();
            o.executed = g.submitted_frame == g.frame;
            const uint64_t late = g.stats.inline_late_writes + g.stats.post_scene_batches;
            o.later_scene_write = o.executed && late > g.route_prev_late;
            na::capture::j2::OnFrameClosed(g.frame, late - g.route_prev_late); // evidence capture identity
            o.camera_missed = g.stats.no_camera > g.route_prev_no_camera;
            o.depth_state_missed = g.stats.no_state > g.route_prev_no_state;
            {
                // Current-frame ownership by a supported upscaler: the GAME's own evaluations
                // this frame (J2's NGX calls are marked as injector calls and excluded).
                // Lifetime/creation counts are not ownership; when the game stops
                // evaluating, the selector relearns from structure.
                const uint64_t external = na::ngx::GetStats().evaluates_external + na::ffx::GetStats().upscale_dispatches +
                                          na::xess::GetStats().executes;
                o.interceptor = external > g.route_prev_external;
                g.route_prev_external = external;
            }
            g.route_prev_late = late;
            g.route_prev_no_camera = g.stats.no_camera;
            g.route_prev_no_state = g.stats.no_state;
            const auto& d = g.router.OnFrame(o);
            g.frame_agg.Reset();
            auto& r = g.stats.route;
            r.route = static_cast<uint32_t>(d.route);
            r.state = static_cast<uint32_t>(d.state);
            r.mode = static_cast<uint32_t>(g.router.Mode());
            r.matches = d.matches;
            r.observed = d.observed;
            r.generation = d.history_generation;
            r.changes = d.route_changes;
            r.scene_frames = d.scene_frames;
            r.executed_frames = d.executed_frames;
            r.placement_consistent = d.placement_consistent;
            r.contradictions = d.contradictions;
            r.unstable_windows = d.unstable_windows;
            r.contribution_established = d.contribution_established ? 1u : 0u;
            std::snprintf(r.reason, sizeof r.reason, "%s", d.reason);
            std::snprintf(r.missing, sizeof r.missing, "%s", d.missing);
            std::snprintf(r.change, sizeof r.change, "%s", d.change);
            apply_route_locked(nullptr);
        }
        std::memcpy(g.stats.insertion_frame_trace, g.insertion_trace, sizeof g.insertion_trace);
        g.insertion_trace[0] = 0;
        g.insertion_trace_used = 0;
        if (g.inline_mode) {
            ++g.stats.inline_presents;
            if (g.submitted_frame == g.frame) ++g.stats.inline_covered;
            else if (g.insertion_frame.decision.writes)
                // The reason recorded at rejection time this frame (never a recycled
                // earlier success string); else no candidate reached the matcher.
                std::snprintf(g.stats.insertion_last_miss, sizeof g.stats.insertion_last_miss,
                    "frame=%llu rejects=%u %s", (unsigned long long)g.frame, g.frame_rejects,
                    g.frame_reject_text[0] ? g.frame_reject_text
                                           : "no candidate reached the matcher (no read of the scene colour after its last write was seen in a scene recording)");
            if (g.insertion_frame.decision.writes && g.submitted_frame != g.frame) {
                char why[200];
                std::snprintf(why, sizeof why, "Inline DLAA missed this frame (%s).",
                              g.frame_reject_text[0] ? "the scene boundary did not match the learned one; see j2_last_miss" : "no boundary candidate");
                reason(why); // not the previous frame's "running" text
            }
            g.stats.active = g.submitted_frame == g.frame;
        }
        if (g.insertion_frame.color) g.insertion_learned = g.insertion_frame;
        {
            // Placement profile: label this frame's consumers from the submission-ordered
            // writes and reads (facts once the frame is closed) and learn their classes.
            if (g.frame_seq_complete && !g.frame_seq.empty() && g.scene_w) {
                std::vector<na::j2::Labelled> labels;
                na::j2::LabelFrame(g.frame_seq, g.scene_w, g.scene_h, labels);
                for (const auto& lab : labels) g.profile.Observe(lab, g.frame);
                ++g.stats.labelled_frames;
            } else if (!g.frame_seq.empty()) {
                ++g.stats.unlabelled_frames;
            }
            // A class-triggered DLAA followed by more scene content: demote that class now.
            if (g.class_fired && g.submitted_frame == g.frame && g.stats.inline_late_writes + g.stats.post_scene_batches > g.route_prev_late_class) {
                g.profile.Contradict(g.fired_class, g.fired_producer, g.frame);
                ++g.stats.class_contradictions;
            }
            g.route_prev_late_class = g.stats.inline_late_writes + g.stats.post_scene_batches;
            g.class_fired = false;
            g.frame_seq.clear();
            g.frame_seq_complete = true;
            na::observe::ResetDerived();
            g.stats.profile_accepted = g.profile.AcceptedCount();
            g.stats.profile_rejected = g.profile.RejectedCount();
            g.stats.profile_ambiguous = g.profile.AmbiguousCount();
            g.stats.profile_entries = static_cast<uint32_t>(g.profile.Size());
            // An inline recording that was never submitted within two frames is released
            // (its capture slot retired; the frame already counted as missed).
            if (g.inl.valid && g.frame >= g.inl.frame + 2) {
                na::capture::j2::Discard(g.inl.history.capture, true);
                g.inl.history.capture = 0;
                g.inl.valid = false;
                ++g.stats.expired_inline;
            }
        }
        g.insertion_frame = {};
        ++g.frame;
        g.jit.valid = false; // J1: the next camera block starts a new jitter sample
        g.inline_sample_locked = false;
        g.inline_camera_submitted = false;
        g.stats.inline_frame_rejects = g.frame_rejects;
        g.frame_rejects = 0;
        g.frame_reject_text[0] = 0;
        if (g.scene_batches_cur) {
            g.batch_hist[g.batch_hist_n++ % 16] = static_cast<uint8_t>(g.scene_batches_cur > 255 ? 255 : g.scene_batches_cur);
            uint32_t votes[256] = {};
            const uint32_t n = g.batch_hist_n < 16 ? g.batch_hist_n : 16;
            for (uint32_t i = 0; i < n; ++i) ++votes[g.batch_hist[i]];
            uint32_t best = 0;
            for (uint32_t c = 1; c < 256; ++c) if (votes[c] > votes[best]) best = c;
            g.scene_batches_target = best;
            g.stats.scene_batches = g.scene_batches_cur;
            g.stats.scene_batches_target = best;
            if (g.scene_batches_cur > g.stats.scene_batches_max) g.stats.scene_batches_max = g.scene_batches_cur;
        }
        g.scene_batches_cur = 0;
    }
}

void Execute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists, ExecuteFn forward)
{
    if (!g.enabled.load(std::memory_order_relaxed) || g.paused.load(std::memory_order_relaxed) || !queue || !lists || !count) {
        forward(queue, count, lists);
        return;
    }
    const na::observe::HostWorkScope host; // our objects and our list are never observed as the game's
    std::unique_lock<std::mutex> lock(g.m);
    if (g.latched) {
        g.stats.active = false;
        lock.unlock();
        forward(queue, count, lists);
        return;
    }
    ++g.stats.submissions;
    {
        // [j2] route=auto|between|inline (diagnostic overrides); legacy inline=1 = inline override.
        const uint32_t rm = na::settings::Get().j2_route.load();
        const na::j2::RouteMode mode = rm == 1 ? na::j2::RouteMode::ForceBetween
                                     : (rm == 2 || na::settings::Get().j2_inline.load()) ? na::j2::RouteMode::ForceInline
                                     : na::j2::RouteMode::Auto;
        g.router.SetMode(mode);
        apply_route_locked(queue);
    }
    uint64_t selected_depth = 0, selected_color = 0;
    na::observe::GetLiveSelection(&selected_depth, &selected_color);
    if (selected_color && selected_color != g.scene_color_id) {
        // A different scene image: its size is the class reference; the old profile does not apply.
        g.scene_color_id = selected_color;
        g.scene_w = g.scene_h = 0;
        g.profile.Clear();
        ComPtr<ID3D12Resource> c;
        c.Attach(na::observe::AcquireResource(selected_color));
        if (c) {
            const D3D12_RESOURCE_DESC cd = c->GetDesc();
            g.scene_w = static_cast<uint32_t>(cd.Width);
            g.scene_h = cd.Height;
        }
    }
    if (selected_color && selected_depth) for (UINT i = 0; i < count; ++i) {
        if (!lists[i]) continue;
        const auto id = na::observe::IdOf(lists[i]);
        const auto decision = na::observe::ListInsertionDecision(lists[i], selected_color);
        if ((decision.writes || decision.all_reads) && g.frame_seq.size() < 512) {
            // Placement profile: this list's writes and consumers, in submission order.
            if (!na::observe::ListColorEvents(lists[i], selected_color, g.frame_seq)) g.frame_seq_complete = false;
        }
        {
            // Structure for the route selector, in submission order.
            na::j2::ListSummary sum;
            sum.renders_scene = na::observe::ListRendersTo(lists[i], selected_color, selected_depth);
            sum.color_writes = decision.writes;
            sum.reads = decision.all_reads + decision.derived_reads; // a read of a copy of the scene continues its chain
            sum.reads_after_last_write = decision.final_candidate ? 1u : 0u;
            sum.clears_color = sum.renders_scene && na::observe::ListClearsTarget(lists[i], selected_color);
            uint64_t sig = 1469598103934665603ull; // FNV-1a over the boundary's identity
            for (uint64_t v : {decision.boundary_writer, static_cast<uint64_t>(decision.writer_occurrence), decision.destination, decision.pipeline})
                sig = (sig ^ v) * 1099511628211ull;
            sum.boundary_signature = decision.final_candidate ? sig : 0;
            g.frame_agg.AddList(sum);
            if (sum.renders_scene) {
                // Present must be attributable to learn, and every route needs DLSS:
                // start it here (host-work scope), never from Present.
                if (!g.device) ensure_device(queue);
                if (g.device && g.queue.Get() == queue && !g.dlss_ready && g.dlss_tried_frame != g.frame) {
                    g.dlss_tried_frame = g.frame;
                    dlss_on();
                }
            }
        }
        if ((decision.writes || decision.all_reads) && g.insertion_trace_used < sizeof g.insertion_trace - 160) {
            const int n = std::snprintf(g.insertion_trace + g.insertion_trace_used, sizeof g.insertion_trace - g.insertion_trace_used,
                " L%llu:w%u/r%u/f%u/d%llu/p%llu/b%llu:%u%s", (unsigned long long)id, decision.writes,
                decision.all_reads, decision.final_candidate, (unsigned long long)decision.destination,
                (unsigned long long)decision.pipeline, (unsigned long long)decision.boundary_writer,
                decision.writer_occurrence, g.inl.valid && g.inl.list == lists[i] ? "*" : "");
            if (n > 0) g.insertion_trace_used += static_cast<size_t>(n);
        }
        if (!na::observe::ListRendersTo(lists[i], selected_color, selected_depth)) continue;
        // Command-list objects are pooled/reused. Learn the last scene-writing
        // recording in SUBMISSION order across this frame, not the prior job
        // assigned to this particular object. A later write-only list invalidates
        // an earlier consumer; a later consumer after that write replaces it.
        if (decision.writes) {
            if (g.insertion_frame.decision.final_candidate != decision.final_candidate)
                ++g.stats.insertion_changes;
            // Learned in SUBMISSION order from recordings actually submitted (Astra #4);
            // a recording that is never submitted cannot replace the learned anchor.
            g.insertion_frame = {selected_color, selected_depth, decision};
        }
        ++g.stats.insertion_traces;
        if (decision.candidates) {
            g.stats.insertion_list = id;
            g.stats.insertion_reads = decision.candidates;
            g.stats.insertion_final = decision.final_candidate;
            g.stats.insertion_writes = decision.writes;
            g.stats.insertion_destination = decision.destination;
            g.stats.insertion_pipeline = decision.pipeline;
        }
    }
    if (g.inline_mode) {
        // Count actual observed writes after our insertion, including later
        // lists without the scene depth bound. This is not a full-overwrite claim.
        bool after = g.submitted_frame == g.frame;
        for (UINT i = 0; i < count; ++i) {
            if (!lists[i]) continue;
            const auto d = na::observe::ListInsertionDecision(lists[i], g.inl.color);
            if (g.inl.valid && lists[i] == g.inl.list && na::observe::ListRecording(lists[i]) == g.inl.rec) {
                if (d.writes > g.inl.writes) g.stats.inline_late_writes += d.writes - g.inl.writes;
                after = true;
            } else if (after) g.stats.inline_late_writes += d.writes;
        }
        // Inline mode: no J2 list is inserted; DLAA is already inside the game's list.
        // Its history is committed when that list is actually submitted.
        bool has = false;
        if (g.inl.valid)
            for (UINT i = 0; i < count && !has; ++i)
                has = lists[i] == g.inl.list && na::observe::ListRecording(lists[i]) == g.inl.rec;
        jitter_batch(lists, count);
        for (UINT i = 0; i < count; ++i)
            if (lists[i] && na::observe::ListReferences(lists[i], selected_depth) &&
                !na::observe::ListRootConstantBuffers(lists[i]).empty())
                g.inline_camera_submitted = true;
        lock.unlock();
        forward(queue, count, lists);
        if (!has) return;
        lock.lock();
        g.tracker = g.inl.history.tracker;
        g.submitted_frame = g.frame;
        g.resume_reset = false;
        if (g.inl.history.cut) ++g.stats.cuts;
        ++g.stats.frames;
        ++g.stats.inline_frames;
        if (g.stats.inline_jitter_x != 0 || g.stats.inline_jitter_y != 0) ++g.stats.inline_jittered;
        g.stats.active = true;
        g.slots[g.inl.slot].fence_value = g.fence_next++;
        queue->Signal(g.fence.Get(), g.slots[g.inl.slot].fence_value);
        na::capture::j2::Submitted(g.inl.history.capture, queue, g.stats.submissions);
        g.inl.history.capture = 0;
        g.inl.valid = false;
        reason("Live DLAA is running inside the game's scene list (post-processing shares it).");
        return;
    }
    if (g.router.Current().route != na::j2::Route::Between) {
        // Observing, or a route that J2 does not run between lists (none / interceptor).
        const auto& d = g.router.Current();
        char why[200];
        std::snprintf(why, sizeof why, "%s %s", d.route == na::j2::Route::None ? "Choosing where DLAA belongs:" : "Route:", d.reason);
        reason(why);
        lock.unlock();
        forward(queue, count, lists);
        return;
    }
    if (g.submitted_frame == g.frame) {
        ++g.stats.duplicate_submissions;
        uint64_t dsel = 0, csel = 0;
        na::observe::GetLiveSelection(&dsel, &csel);
        if (g.jitter_on) count_post_dlaa(lists, 0, count, dsel); // later batches of a frame whose DLAA already ran
        // Scene drawing after our DLAA (this frame's insertion was too early): counted so
        // the next frame inserts after it, and reported (post_scene).
        if (dsel && csel) {
            // A batch that CLEARS the colour starts the image over (a repeat or another
            // view), not more of the same scene: not counted. A depth clear alone is
            // fine (first-person weapons are drawn that way onto the same image).
            bool scene_here = false, restarts = false;
            for (UINT i = 0; i < count; ++i) {
                if (!lists[i]) continue;
                if (na::observe::ListRendersTo(lists[i], csel, dsel)) {
                    scene_here = true;
                    if (na::observe::ListClearsTarget(lists[i], csel)) restarts = true;
                }
            }
            if (scene_here && !restarts) { ++g.scene_batches_cur; ++g.stats.post_scene_batches; }
        }
        lock.unlock();
        forward(queue, count, lists);
        return;
    }
    uint64_t depth_id = 0, color_id = 0;
    na::observe::GetLiveSelection(&depth_id, &color_id);
    jitter_batch(lists, count); // J1, before anything in this batch is forwarded
    if (!depth_id || !color_id) {
        ++g.stats.no_selection;
        reason("Waiting for discovery to choose the scene depth and colour.");
        lock.unlock();
        forward(queue, count, lists);
        return;
    }
    int scene = -1;
    bool before_reader = false, restarts = false;
    for (UINT i = 0; i < count; ++i) {
        // Prefer the last scene writer BEFORE the first explicit downstream
        // reader (AA, tonemap, etc.). A postprocess list may rebind the scene RTV
        // at its end; that rebind must not move us past its earlier read.
        if (lists[i] && scene >= 0 && na::observe::ListSamplesTexture(lists[i], color_id)) {
            before_reader = true;
            break;
        }
        if (lists[i] && na::observe::ListRendersTo(lists[i], color_id, depth_id)) {
            scene = static_cast<int>(i);
            if (na::observe::ListClearsTarget(lists[i], color_id)) restarts = true;
        }
    }
    // A batch that clears the colour starts the image over: it is scene batch 1 again.
    if (scene >= 0) g.scene_batches_cur = restarts ? 1 : g.scene_batches_cur + 1;
    // More scene batches are expected this frame (the last frame had them) and
    // post-processing has not started: DLAA waits for the last one.
    const bool wait_later = scene >= 0 && g.scene_batches_cur < g.scene_batches_target;
    ID3D12GraphicsCommandList* ours = nullptr;
    PendingHistory pending;
    if (scene < 0) {
        ++g.stats.no_scene_list;
    } else if (wait_later) {
        ++g.stats.waited_batches;
    } else if (!ensure_device(queue)) {
        // reason set
    } else {
        if (!dlss_on()) {
            // reason set
        } else if (record(lists, static_cast<UINT>(scene), depth_id, color_id, &ours, pending)) {
            g.stats.later_passes = na::observe::ListTargetsAfter(lists[scene], color_id, depth_id);
        }
    }
    if (!wait_later) g.stats.active = ours != nullptr;
    if (scene >= 0 && !wait_later) g.jit.valid = false; // the scene closes this frame's jitter: the next camera block starts a new sample
    if (!ours) {
        na::capture::j2::Discard(pending.capture, false); // our list is reset before any GPU use
        lock.unlock();
        forward(queue, count, lists);
        return;
    }
    if (before_reader) ++g.stats.between_observed; // an observed reader follows in this batch
    else ++g.stats.between_predicted;              // placed after a predicted last scene list
    std::vector<ID3D12CommandList*> batch(lists, lists + count);
    batch.insert(batch.begin() + scene + 1, ours);
    const UINT n2 = static_cast<UINT>(batch.size());
    Slot& s = g.slots[g.slot];
    g.slot = (g.slot + 1) % kRing;
    na::dlss::BeforeExecute(queue, n2, batch.data());
    forward(queue, n2, batch.data());
    if (g.jitter_on) count_post_dlaa(lists, static_cast<UINT>(scene) + 1, count, depth_id); // same batch, after the DLAA list
    // The void D3D12 forward has now submitted our list. Failed recording/NGX
    // attempts above must never replace the previous submitted camera.
    g.tracker = pending.tracker;
    g.submitted_frame = g.frame;
    g.resume_reset = false;
    ++g.stats.frames;
    if (pending.cut) ++g.stats.cuts;
    if (g.stats.later_passes) {
        char why[160];
        std::snprintf(why, sizeof why, "Live DLAA is running (inserted after the scene list; it draws %u other targets after the scene).", g.stats.later_passes);
        reason(why);
    } else if (before_reader) {
        reason("Live DLAA is running before a later scene-colour reader; game AA remains enabled.");
    } else {
        reason("Live DLAA is running.");
    }
    na::dlss::OnExecuted(queue, n2, batch.data());
    s.fence_value = g.fence_next++;
    queue->Signal(g.fence.Get(), s.fence_value);
    na::capture::j2::Submitted(pending.capture, queue, g.stats.submissions);
}

Stats GetStats()
{
    std::lock_guard<std::mutex> lock(g.m);
    return g.stats;
}

void Shutdown()
{
    std::lock_guard<std::mutex> lock(g.m);
    g.depth32.Reset(); g.motion.Reset(); g.provenance.Reset(); g.output.Reset(); g.color_in.Reset(); g.sharp.Reset();
    g.msaa = MsaaResolve{};
    for (auto& s : g.slots) { s.list.Reset(); s.alloc.Reset(); s.fence_value = 0; }
    g.fence.Reset(); g.queue.Reset(); g.device.Reset();
    g.arbiter = na::motion::gpu::Arbiter{};
    g_dbg = DebugPass{};
    g_rcas = RcasPass{};
    g_nis.Reset();
    g.arbiter_ok = false;
    g.tracker.Reset();
    g.frame = 1;
    g.submitted_frame = 0;
    g.scene_batches_cur = g.scene_batches_target = 0;
    g.batch_hist_n = 0;
    g.inline_mode = false;
    g.router.Reset();
    g.frame_agg.Reset();
    g.route_prev_late = g.route_prev_post_scene = g.route_prev_no_camera = g.route_prev_no_state = 0;
    g.route_gen_applied = 0;
    g.dlss_ready = false;
    g.dlss_tried_frame = ~0ull;
    g.frame_rejects = 0;
    g.frame_reject_text[0] = 0;
    g.profile.Clear();
    g.frame_seq.clear();
    g.frame_seq_complete = true;
    g.scene_color_id = 0;
    g.scene_w = g.scene_h = 0;
    g.class_fired = false;
    g.route_prev_late_class = 0;
    g.insertion_frame = g.insertion_learned = {};
    g.inl = State::InlinePending{};
    g.inl_frame = 0;
    g.inline_sample_locked = false;
    g.inline_camera_submitted = false;
    g.resume_reset = false;
    g.w = g.h = 0;
}

} // namespace na::j2
