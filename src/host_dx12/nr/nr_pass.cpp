#include "host_dx12/nr/nr_pass.hpp"
#include "host_dx12/depth_format.hpp"

#include <d3dcompiler.h>
#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "host_dx12/color/color_space.hpp"
#include "host_dx12/settings.hpp"

#if defined(NA_HAVE_NGX_SDK)
#pragma warning(push, 3)
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_params.h>
#pragma warning(pop)
#endif

using Microsoft::WRL::ComPtr;

namespace na::nr {
namespace {

// ---- Compute shaders (compiled once at first use with d3dcompiler_47) ----
// hdr_to_sdr: HDR back buffer (mode 1 = PQ / HDR10, mode 2 = scRGB linear
// fp16, 1.0 = 80 nits) -> sRGB-encoded SDR working image for NR.
// sdr_to_hdr: applies NR's change as a ratio on the ORIGINAL HDR light
// values, so highlights above paper white are preserved, then re-encodes in
// the back buffer's own encoding (A60: any HDR format, not only HDR10).
const char kShaderSource[] = R"(
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
)";

struct Constants { float paper_white; uint32_t width, height, mode, src_w, src_h; };
constexpr UINT kConstants = sizeof(Constants) / 4;
constexpr uint32_t kEvalSlots = 64; // descriptor groups for evaluate-time depth conversion (ring)

constexpr uint32_t kRing = 8; // A63: was 4; a game running 3+ frames ahead made the pass skip alternate frames (flicker)

using PfnCreate = int32_t(__cdecl*)(void*, uint32_t, void*, void**);
using PfnEvaluate = int32_t(__cdecl*)(void*, const void*, const void*, void*);
using PfnRelease = int32_t(__cdecl*)(void*);
using PfnAllocParams = int32_t(__cdecl*)(void**);
using PfnDestroyParams = int32_t(__cdecl*)(void*);

struct Frame {
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12Resource> depth, motion; // retain a submitted extent until its fence passes
    UINT64 fence_value = 0;
};

struct PendingInputs {
    ID3D12CommandList* list = nullptr; // identity only; never dereferenced/retained
    EvalInputs in{};
    uint32_t valid_w = 0, valid_h = 0; // rendered region copied to (0,0) of the copies
    ComPtr<ID3D12Resource> depth, motion;
};

struct State {
    std::mutex m;
    Runtime rt;
    std::atomic<uint32_t> has_rt{0};
    std::atomic<uint32_t> wants_inputs{0};
    // Device objects (game device).
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12GraphicsCommandList> list;
    Frame frames[kRing];
    uint32_t frame_index = 0;
    ComPtr<ID3D12Fence> fence;
    UINT64 fence_next = 1;
    bool signal_failed = false, reset_pending = true;
    uint64_t last_evaluate_tick = 0, setting_signature = 0;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso_hdr_to_sdr, pso_sdr_to_hdr, pso_up_depth, pso_up_motion;
    ComPtr<ID3D12DescriptorHeap> heap; // shader-visible, 4 tables x 4 slots x ring
    UINT heap_stride = 0;
    ComPtr<ID3D12QueryHeap> queries;
    ComPtr<ID3D12Resource> query_readback;
    // NA-owned textures.
    ComPtr<ID3D12Resource> depth_copy, motion_copy;   // NPSR at rest (render size)
    ComPtr<ID3D12Resource> recording_depth, recording_motion;
    PendingInputs pending[16]; // bounded command-list recordings, no per-frame heap growth
    std::atomic<uint32_t> pending_count{0};
    ComPtr<ID3D12CommandQueue> input_queue;
    ComPtr<ID3D12Resource> depth_up, motion_up;       // UAV at rest (display size, scaled modes only)
    ComPtr<ID3D12Resource> color_in;                  // NPSR at rest
    ComPtr<ID3D12Resource> nr_out;                    // UAV at rest
    ComPtr<ID3D12Resource> hdr_out;                   // UAV at rest (HDR only; back-buffer format)
    uint32_t hdr_mode_used = 0;                       // A60: shader mode of the current resources (1 PQ, 2 scRGB)
    DXGI_FORMAT motion_format = DXGI_FORMAT_UNKNOWN;
    uint32_t in_w = 0, in_h = 0;         // depth/motion rendered extent (region at (0,0) of the copies)
    ComPtr<ID3D12PipelineState> pso_depth_r32; // C12 conversion
    uint32_t eval_slot = 0;
    EvalInputs last_in{};
    uint32_t have_inputs = 0;
    uint32_t inputs_fresh = 0;          // a DLSS input copy was SUBMITTED since the last Present
    // Feature.
    void* params = nullptr;
    void* feature = nullptr;
    uint32_t feat_w = 0, feat_h = 0, feat_hdr = 0;
    DXGI_FORMAT feat_format = DXGI_FORMAT_UNKNOWN;
    uint32_t feat_model = 0; // na::settings nr_model the feature was created with
    uint32_t feature_pending = 0;       // created this frame; evaluate from the next
    UINT64 feature_fence = 0;
    // Status.
    std::atomic<uint32_t> active{0}, latched{0}, hdr{0};
    std::atomic<uint64_t> evaluates{0}, skipped{0};
    std::atomic<uint64_t> inputs_seen{0}, input_overwrites{0}, missing_inputs{0}, busy_slots{0};
    std::atomic<float> last_ms{0.f}, avg_ms{0.f};
    std::atomic<uint32_t> samples{0};
    char reason[96] = "Neural Rendering is off.";
    uint32_t width = 0, height = 0;
};
State g;
thread_local bool internal_submit = false;

void submit_locked(ID3D12CommandQueue* queue, UINT n, ID3D12CommandList* const* lists)
{
    // Our list can re-enter the host's Execute hook while g.m is held.
    const bool previous = internal_submit;
    internal_submit = true;
    queue->ExecuteCommandLists(n, lists);
    internal_submit = previous;
}

void set_reason(const char* text)
{
    strncpy_s(g.reason, text, _TRUNCATE);
}
void latch(const char* text)
{
    set_reason(text);
    g.latched.store(1, std::memory_order_release);
    g.active.store(0, std::memory_order_release);
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER br{};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    br.Transition.StateBefore = a;
    br.Transition.StateAfter = b;
    return br;
}
D3D12_RESOURCE_BARRIER uav_barrier(ID3D12Resource* r)
{
    D3D12_RESOURCE_BARRIER br{};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    br.UAV.pResource = r;
    return br;
}

bool create_texture(UINT w, UINT h, DXGI_FORMAT fmt, bool uav, D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& out)
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

bool uav_store_supported(DXGI_FORMAT fmt)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{};
    fs.Format = fmt;
    if (FAILED(g.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs)))) return false;
    return (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
}

bool compile(const char* entry, ComPtr<ID3D12PipelineState>& pso)
{
    ComPtr<ID3DBlob> code, err;
    if (FAILED(D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "nr_pass.hlsl", nullptr, nullptr, entry, "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err)))
        return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g.root.Get();
    pd.CS.pShaderBytecode = code->GetBufferPointer();
    pd.CS.BytecodeLength = code->GetBufferSize();
    return SUCCEEDED(g.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)));
}

bool ensure_device_locked(ID3D12Device* dev)
{
    if (g.device) return true;
    g.device = dev;
    for (Frame& f : g.frames)
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.alloc)))) return false;
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.frames[0].alloc.Get(), nullptr, IID_PPV_ARGS(&g.list))))
        return false;
    g.list->Close();
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)))) return false;

    // Root signature: table(t0..t2, u0) + 4 root constants.
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 3;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 3;
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = kConstants;
    params[1].Constants.ShaderRegister = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.ShaderRegister = 0;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 2;
    rs.pParameters = params;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &samp;
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) return false;
    if (FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g.root)))) return false;
    if (!compile("hdr_to_sdr", g.pso_hdr_to_sdr) || !compile("sdr_to_hdr", g.pso_sdr_to_hdr) ||
        !compile("upsample_depth", g.pso_up_depth) || !compile("upsample_motion", g.pso_up_motion) ||
        !compile("depth_to_r32", g.pso_depth_r32))
        return false;

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 16 * kRing + 4 * kEvalSlots;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.heap)))) return false;
    g.heap_stride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_QUERY_HEAP_DESC qh{};
    qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qh.Count = 2 * kRing;
    if (FAILED(dev->CreateQueryHeap(&qh, IID_PPV_ARGS(&g.queries)))) return false;
    D3D12_HEAP_PROPERTIES rb{};
    rb.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 16 * kRing;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return SUCCEEDED(dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                  nullptr, IID_PPV_ARGS(&g.query_readback)));
}

void write_srv(uint32_t slot, ID3D12Resource* r, DXGI_FORMAT fmt)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = fmt;
    d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE h = g.heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slot) * g.heap_stride;
    g.device->CreateShaderResourceView(r, &d, h);
}
void write_uav(uint32_t slot, ID3D12Resource* r, DXGI_FORMAT fmt)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
    d.Format = fmt;
    d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE h = g.heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slot) * g.heap_stride;
    g.device->CreateUnorderedAccessView(r, nullptr, &d, h);
}
D3D12_GPU_DESCRIPTOR_HANDLE table_gpu(uint32_t slot)
{
    D3D12_GPU_DESCRIPTOR_HANDLE h = g.heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<UINT64>(slot) * g.heap_stride;
    return h;
}

void release_feature_locked()
{
    if (g.feature && g.rt.release_feature) reinterpret_cast<PfnRelease>(g.rt.release_feature)(g.feature);
    g.feature = nullptr;
    g.feature_pending = 0;
}

#if defined(NA_HAVE_NGX_SDK)
NVSDK_NGX_Parameter* P() { return static_cast<NVSDK_NGX_Parameter*>(g.params); }
#endif

// Creates (or re-creates) the NR feature for the current extent/format on
// the open list. Returns false and latches on failure.
bool ensure_feature_locked(uint32_t w, uint32_t h, bool hdr, DXGI_FORMAT bb_format)
{
#if defined(NA_HAVE_NGX_SDK)
    const uint32_t model = na::settings::Get().nr_model.load();
    if (g.feature && g.feat_w == w && g.feat_h == h && g.feat_hdr == (hdr ? 1u : 0u) && g.feat_format == bb_format && g.feat_model == model)
        return true;
    release_feature_locked();
    if (!g.params) {
        void* p = nullptr;
        int32_t r = reinterpret_cast<PfnAllocParams>(g.rt.allocate_parameters)(&p);
        if (!NVSDK_NGX_SUCCEED(static_cast<NVSDK_NGX_Result>(r)) || !p) {
            latch("Neural Rendering could not allocate its settings object.");
            return false;
        }
        g.params = p;
    }
    // Capability check once per size: DLSSNR.Available on the capability params.
    if (g.rt.get_capability_parameters) {
        void* cap = nullptr;
        int32_t r = reinterpret_cast<PfnAllocParams>(g.rt.get_capability_parameters)(&cap);
        int avail = 0;
        if (NVSDK_NGX_SUCCEED(static_cast<NVSDK_NGX_Result>(r)) && cap)
            static_cast<NVSDK_NGX_Parameter*>(cap)->Get("DLSSNR.Available", &avail);
        if (!avail) {
            latch("Neural Rendering runtime not found beside the game (nvngx_dlssnr.dll).");
            return false;
        }
    }
    const na::settings::Settings& st = na::settings::Get();
    NVSDK_NGX_Parameter* p = P();
    p->Set("CreationNodeMask", 1u);
    p->Set("VisibilityNodeMask", 1u);
    p->Set("Width", w);
    p->Set("Height", h);
    p->Set("OutWidth", w);
    p->Set("OutHeight", h);
    p->Set("DLSSNR.Enabled", 1u);
    p->Set("DLSSNR.Width", w);
    p->Set("DLSSNR.Height", h);
    // 310.9.x: preset 2 = newer model (the runtime's default), 1 = original model.
    p->Set("DLSSNR.Hint.Render.Preset", model == 1 ? 1u : 2u);
    p->Set("DLSSNR.Style", 0u);
    p->Set("DLSSNR.Intensity", st.nr_intensity.load() / 100.f);
    p->Set("DLSSNR.LocalStructureStrength", st.nr_structure.load() / 100.f);
    p->Set("DLSSNR.LocalToneStrength", st.nr_tone.load() / 100.f);
    p->Set("DLSSNR.SkinStructureStrength", st.nr_skin.load() / 100.f);
    p->Set("DLSSNR.UseAutoMask", 1u);
    p->Set("DLSSNR.UICorrection", 0u);
    void* handle = nullptr;
    int32_t r = reinterpret_cast<PfnCreate>(g.rt.create_feature)(g.list.Get(), 18u, g.params, &handle);
    if (!NVSDK_NGX_SUCCEED(static_cast<NVSDK_NGX_Result>(r)) || !handle) {
        char buf[96];
        snprintf(buf, sizeof(buf), "Neural Rendering could not start (NVIDIA code 0x%08X).", static_cast<unsigned>(r));
        latch(buf);
        return false;
    }
    g.feature = handle;
    g.feat_w = w;
    g.feat_h = h;
    g.feat_hdr = hdr ? 1u : 0u;
    g.feat_format = bb_format;
    g.feat_model = model;
    g.feature_pending = 1;
    g.reset_pending = true;
    return true;
#else
    (void)w; (void)h; (void)hdr; (void)bb_format;
    latch("Neural Rendering needs a build with the NVIDIA SDK.");
    return false;
#endif
}

bool evaluate_locked(bool hdr)
{
#if defined(NA_HAVE_NGX_SDK)
    const na::settings::Settings& st = na::settings::Get();
    NVSDK_NGX_Parameter* p = P();
    p->Set("DLSSNR.Intensity", st.nr_intensity.load() / 100.f);
    p->Set("DLSSNR.LocalStructureStrength", st.nr_structure.load() / 100.f);
    p->Set("DLSSNR.LocalToneStrength", st.nr_tone.load() / 100.f);
    p->Set("DLSSNR.SkinStructureStrength", st.nr_skin.load() / 100.f);
    bool scaled = g.in_w != g.width || g.in_h != g.height;
    p->Set("DLSSNR.Color", g.color_in.Get());
    p->Set("DLSSNR.Depth", scaled ? g.depth_up.Get() : g.depth_copy.Get());
    p->Set("DLSSNR.MVec", scaled ? g.motion_up.Get() : g.motion_copy.Get());
    p->Set("DLSSNR.Output", g.nr_out.Get());
    p->Set("DLSSNR.Reset", g.reset_pending || g.last_in.reset ? 1u : 0u);
    p->Set("DLSSNR.DepthInverted", g.last_in.depth_inverted);
    // Motion values are in normalised units; the scale carries pixels. When
    // upsampled to display size the scale grows by the same ratio.
    float sx = scaled ? static_cast<float>(g.width) / static_cast<float>(g.in_w) : 1.f;
    float sy = scaled ? static_cast<float>(g.height) / static_cast<float>(g.in_h) : 1.f;
    p->Set("DLSSNR.MVecScaleX", g.last_in.mv_scale_x * sx);
    p->Set("DLSSNR.MVecScaleY", g.last_in.mv_scale_y * sy);
    for (const char* k : {"Color", "Depth", "MVec", "Output"}) {
        char key[64];
        snprintf(key, sizeof(key), "DLSSNR.%sSubrectBaseX", k); p->Set(key, 0u);
        snprintf(key, sizeof(key), "DLSSNR.%sSubrectBaseY", k); p->Set(key, 0u);
        snprintf(key, sizeof(key), "DLSSNR.%sSubrectWidth", k); p->Set(key, g.width);
        snprintf(key, sizeof(key), "DLSSNR.%sSubrectHeight", k); p->Set(key, g.height);
    }
    (void)hdr;
    int32_t r = reinterpret_cast<PfnEvaluate>(g.rt.evaluate_feature)(g.list.Get(), g.feature, g.params, nullptr);
    if (!NVSDK_NGX_SUCCEED(static_cast<NVSDK_NGX_Result>(r))) {
        char buf[96];
        snprintf(buf, sizeof(buf), "Neural Rendering stopped (NVIDIA code 0x%08X).", static_cast<unsigned>(r));
        latch(buf);
        return false;
    }
    g.reset_pending = false;
    g.last_evaluate_tick = GetTickCount64();
    g.evaluates.fetch_add(1, std::memory_order_relaxed);
    return true;
#else
    (void)hdr;
    return false;
#endif
}

void read_timings_locked(ID3D12CommandQueue* queue)
{
    // Read the slot from kRing-1 frames ago if its fence has passed.
    uint32_t old = (g.frame_index + 1) % kRing;
    Frame& f = g.frames[old];
    if (!f.fence_value || g.fence->GetCompletedValue() < f.fence_value) return;
    UINT64 freq = 0;
    if (FAILED(queue->GetTimestampFrequency(&freq)) || !freq) return;
    void* m = nullptr;
    D3D12_RANGE rr{static_cast<SIZE_T>(old) * 16, static_cast<SIZE_T>(old) * 16 + 16};
    if (FAILED(g.query_readback->Map(0, &rr, &m))) return;
    UINT64 st[2];
    std::memcpy(st, static_cast<uint8_t*>(m) + old * 16, 16);
    D3D12_RANGE none{0, 0};
    g.query_readback->Unmap(0, &none);
    if (st[1] > st[0] && st[0]) {
        float ms = 1000.f * static_cast<float>(st[1] - st[0]) / static_cast<float>(freq);
        g.last_ms.store(ms);
        float avg = g.avg_ms.load();
        g.avg_ms.store(avg > 0 ? avg * 0.9f + ms * 0.1f : ms);
        g.samples.fetch_add(1, std::memory_order_relaxed);
    }
    f.fence_value = 0; // consumed
}

} // namespace

void SetRuntime(const Runtime& rt)
{
    std::lock_guard<std::mutex> lock(g.m);
    g.rt = rt;
    bool ok = rt.create_feature && rt.evaluate_feature && rt.release_feature && rt.allocate_parameters;
    g.has_rt.store(ok ? 1u : 0u, std::memory_order_release);
}
bool HasRuntime() { return g.has_rt.load(std::memory_order_acquire) != 0; }

bool WantsInputs()
{
    return HasRuntime() && na::settings::Get().nr_enabled.load(std::memory_order_relaxed) != 0 &&
           !g.latched.load(std::memory_order_acquire);
}

// C12: records the typed-view depth -> R32F conversion of the region
// (bx, by, w, h) of `depth` (NON_PIXEL_SHADER_RESOURCE, stays so) into (0,0) of
// `out` (R32F UAV-capable, NON_PIXEL_SHADER_RESOURCE -> left in COPY_DEST).
// Changes the list's descriptor heaps, root signature and pipeline. Caller holds g.m.
void record_depth_to_r32_locked(ID3D12GraphicsCommandList* list, ID3D12Resource* depth, DXGI_FORMAT view, ID3D12Resource* out,
                                uint32_t bx, uint32_t by, uint32_t w, uint32_t h)
{
    const uint32_t slot = 16 * kRing + 4 * (g.eval_slot++ % kEvalSlots);
    write_srv(slot + 0, depth, view);
    write_srv(slot + 1, depth, view);
    write_srv(slot + 2, depth, view);
    write_uav(slot + 3, out, DXGI_FORMAT_R32_FLOAT);
    D3D12_RESOURCE_BARRIER pre = transition(out, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->ResourceBarrier(1, &pre);
    ID3D12DescriptorHeap* heaps[] = {g.heap.Get()};
    const Constants c{0.f, w, h, 0u, bx, by};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(g.root.Get());
    list->SetPipelineState(g.pso_depth_r32.Get());
    list->SetComputeRootDescriptorTable(0, table_gpu(slot));
    list->SetComputeRoot32BitConstants(1, kConstants, &c, 0);
    list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    D3D12_RESOURCE_BARRIER done[2] = {
        uav_barrier(out),
        transition(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST)};
    list->ResourceBarrier(2, done);
}

void OnEvaluate(ID3D12GraphicsCommandList* list, const EvalInputs& in)
{
    if (!list || !in.depth || !in.motion) return;
    std::lock_guard<std::mutex> lock(g.m);
    if (!WantsInputs()) return;
    ComPtr<ID3D12Device> dev;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&dev)))) return;
    if (!ensure_device_locked(dev.Get())) {
        latch("Neural Rendering could not set up on the graphics card.");
        return;
    }
    D3D12_RESOURCE_DESC dd = in.depth->GetDesc(), md = in.motion->GetDesc();
    // Argon audit #2 / C12: 32-bit depth is copied; 24/16-bit depth is converted
    // to R32F by a compute pass through a typed view (never an invalid copy).
    const bool copy_depth = na::depth::PlaneZeroCopiesToR32F(dd.Format);
    DXGI_FORMAT depth_view = DXGI_FORMAT_UNKNOWN;
    switch (dd.Format) {
    // Only typeless resources may be viewed with another format; a fully typed
    // D24_UNORM_S8_UINT / D16_UNORM resource cannot be read by a shader.
    case DXGI_FORMAT_R24G8_TYPELESS: depth_view = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_UNORM: depth_view = DXGI_FORMAT_R16_UNORM; break;
    default: break;
    }
    if (!copy_depth && (depth_view == DXGI_FORMAT_UNKNOWN || (dd.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Neural Rendering is not available in this game yet: its %s depth cannot be read by a shader.",
                 na::depth::DepthKind(dd.Format));
        latch(buf);
        return;
    }
    // Argon audit #4.1/#4.2: only the rendered region (subrect) is used, so
    // padded allocations and dynamic resolution work. Motion vectors must be
    // render-sized (MVLowRes) for the region to match the depth region.
    const uint32_t rw = in.render_w, rh = in.render_h;
    const uint32_t dbx = in.depth_base_x == 0xFFFFFFFFu ? 0 : in.depth_base_x, dby = in.depth_base_y == 0xFFFFFFFFu ? 0 : in.depth_base_y;
    const uint32_t mbx = in.mv_base_x == 0xFFFFFFFFu ? 0 : in.mv_base_x, mby = in.mv_base_y == 0xFFFFFFFFu ? 0 : in.mv_base_y;
    const auto fits = [](const D3D12_RESOURCE_DESC& d, uint64_t bx, uint64_t by, uint64_t w, uint64_t h) {
        return w && h && bx + w <= d.Width && by + h <= d.Height;
    };
    if (!rw || !rh || rw == 0xFFFFFFFFu || rh == 0xFFFFFFFFu || !in.mv_low_res || !fits(dd, dbx, dby, rw, rh) || !fits(md, mbx, mby, rw, rh)) {
        g.skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Copies are allocated at the source allocation size (no per-frame churn
    // under dynamic resolution); the region lands at (0,0).
    const uint32_t w = static_cast<uint32_t>(dd.Width), h = dd.Height;
    PendingInputs* pending = nullptr;
    for (auto& p : g.pending) if (p.list == list) { pending = &p; break; }
    if (!pending) for (auto& p : g.pending) if (!p.list) { pending = &p; break; }
    if (!pending) {
        latch("Neural Rendering stopped: too many unsubmitted input lists.");
        return;
    }
    const bool need_uav = !copy_depth;
    if (!g.recording_depth || !g.recording_motion || g.recording_depth->GetDesc().Width != w ||
        g.recording_depth->GetDesc().Height != h || g.recording_motion->GetDesc().Format != md.Format ||
        g.recording_motion->GetDesc().Width != md.Width || g.recording_motion->GetDesc().Height != md.Height ||
        (need_uav && !(g.recording_depth->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))) {
        // Depth: the game's depth plane as a plain float texture (UAV-capable when converted).
        if (!create_texture(w, h, DXGI_FORMAT_R32_FLOAT, need_uav, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, g.recording_depth) ||
            !create_texture(static_cast<UINT>(md.Width), md.Height, md.Format, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, g.recording_motion)) {
            latch("Neural Rendering could not allocate its motion buffers.");
            return;
        }
    }
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.Type = dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    if (copy_depth) {
        D3D12_RESOURCE_BARRIER pre[4] = {
            transition(in.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition(in.motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition(g.recording_depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
            transition(g.recording_motion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST)};
        pre[0].Transition.Subresource = 0; // depth plane only
        list->ResourceBarrier(4, pre);
        src.pResource = in.depth;
        dst.pResource = g.recording_depth.Get();
        const D3D12_BOX dbox{dbx, dby, 0, dbx + rw, dby + rh, 1};
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, &dbox);
    } else {
        // C12: typed SRV of the depth plane -> R32F UAV. The DLSS evaluate that
        // just ran already changed this list's descriptor heaps/root state, and
        // the caller must restore them after it (NGX/FFX contract), so this
        // pass records in that same window.
        record_depth_to_r32_locked(list, in.depth, depth_view, g.recording_depth.Get(), dbx, dby, rw, rh);
        D3D12_RESOURCE_BARRIER pre[2] = {
            transition(in.motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition(g.recording_motion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST)};
        list->ResourceBarrier(2, pre);
    }
    src.pResource = in.motion;
    dst.pResource = g.recording_motion.Get();
    const D3D12_BOX mbox{mbx, mby, 0, mbx + rw, mby + rh, 1};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, &mbox);
    D3D12_RESOURCE_BARRIER post[4] = {
        transition(in.motion, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        transition(g.recording_depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        transition(g.recording_motion.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        transition(in.depth, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
    post[3].Transition.Subresource = 0;
    list->ResourceBarrier(copy_depth ? 4 : 3, post);
    pending->valid_w = rw;
    pending->valid_h = rh;
    if (!pending->list) g.pending_count.fetch_add(1, std::memory_order_release);
    pending->list = list;
    pending->in = in;
    pending->in.depth = pending->in.motion = nullptr; // no borrowed resource pointers outlive the call
    pending->depth = g.recording_depth;
    pending->motion = g.recording_motion;
    g.inputs_seen.fetch_add(1, std::memory_order_relaxed);
}

void Execute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists, ExecuteFn forward)
{
    if (internal_submit || !g.pending_count.load(std::memory_order_acquire)) {
        forward(queue, count, lists);
        return;
    }
    std::unique_lock<std::mutex> lock(g.m);
    bool found = false;
    for (UINT i = 0; i < count; ++i)
        for (const auto& p : g.pending) if (p.list && p.list == lists[i]) found = true;
    if (!found) {
        lock.unlock();
        forward(queue, count, lists);
        return;
    }
    // Hold the short CPU lock through the actual forward/publication, so a
    // concurrent Present cannot consume a receipt before its copies are queued.
    forward(queue, count, lists);
    for (UINT i = 0; i < count; ++i) for (auto& p : g.pending) {
        if (!p.list || p.list != lists[i]) continue;
        if (g.input_queue && g.input_queue.Get() != queue) {
            latch("Neural Rendering stopped: input queues changed.");
        } else {
            g.input_queue = queue;
            g.last_in = p.in;
            g.depth_copy = p.depth;
            g.motion_copy = p.motion;
            g.in_w = p.valid_w;
            g.in_h = p.valid_h;
            g.motion_format = p.motion->GetDesc().Format;
            g.have_inputs = 1;
            if (g.inputs_fresh) g.input_overwrites.fetch_add(1, std::memory_order_relaxed);
            g.inputs_fresh = 1;
        }
        p = {};
        g.pending_count.fetch_sub(1, std::memory_order_release);
    }
}

void OnPresent(ID3D12CommandQueue* queue, ID3D12Resource* backbuffer, DXGI_COLOR_SPACE_TYPE color_space)
{
    if (!queue || !backbuffer) return;
    const na::settings::Settings& st = na::settings::Get();
    if (!st.nr_enabled.load(std::memory_order_relaxed)) {
        g.active.store(0, std::memory_order_release);
        std::unique_lock<std::mutex> lock(g.m, std::try_to_lock);
        if (!lock.owns_lock()) return;
        g.inputs_fresh = 0; g.reset_pending = true;
        // Free the expensive feature/work images after their OWN GPU fence.
        // Keep the small input-copy set: recorded game lists can still refer
        // to it. No wait, working-set trim or process-wide cache purge.
        if (!g.signal_failed && g.feature && g.fence && g.fence->GetCompletedValue() != UINT64_MAX &&
            g.fence->GetCompletedValue() >= g.fence_next - 1) {
            release_feature_locked();
            if (g.params && g.rt.destroy_parameters)
                reinterpret_cast<PfnDestroyParams>(g.rt.destroy_parameters)(g.params);
            g.params = nullptr;
            g.color_in.Reset(); g.nr_out.Reset(); g.hdr_out.Reset();
            g.depth_up.Reset(); g.motion_up.Reset();
            for (auto& frame : g.frames) { frame.depth.Reset(); frame.motion.Reset(); }
            set_reason("Neural Rendering is off; processing memory released.");
        }
        return;
    }
    std::lock_guard<std::mutex> lock(g.m);
    if (!HasRuntime()) {
        set_reason("Neural Rendering is waiting for the DLSS connection.");
        return;
    }
    if (g.latched.load(std::memory_order_acquire)) return;
    if (g.input_queue && g.input_queue.Get() != queue) {
        latch("Neural Rendering stopped: picture and inputs use different queues.");
        return;
    }
    if (!g.have_inputs) {
        set_reason("Waiting for the game's first DLSS frame.");
        g.skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // A44 (session 7: the pause menu was being processed on stale depth and
    // motion): only a frame the game rendered through DLSS is processed.
    // Menu-only frames, loading screens and videos pass untouched.
    if (!g.inputs_fresh) {
        g.reset_pending = true;
        g.missing_inputs.fetch_add(1, std::memory_order_relaxed);
        set_reason("Neural Rendering is paused: the game is not drawing through DLSS right now (menus, videos).");
        g.active.store(0, std::memory_order_release);
        g.skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g.inputs_fresh = 0;
    const uint64_t signature = st.nr_intensity.load() | (uint64_t(st.nr_structure.load()) << 8) |
        (uint64_t(st.nr_tone.load()) << 16) | (uint64_t(st.nr_skin.load()) << 24) | (uint64_t(st.nr_paper_white.load()) << 32);
    if (signature != g.setting_signature || GetTickCount64() - g.last_evaluate_tick > 250) g.reset_pending = true;
    g.setting_signature = signature;
    ComPtr<ID3D12Device> dev;
    if (FAILED(backbuffer->GetDevice(IID_PPV_ARGS(&dev))) || !ensure_device_locked(dev.Get())) {
        latch("Neural Rendering could not set up on the graphics card.");
        return;
    }
    D3D12_RESOURCE_DESC bb = backbuffer->GetDesc();
    uint32_t w = static_cast<uint32_t>(bb.Width), h = bb.Height;
    if (w < g.in_w || h < g.in_h) {
        g.reset_pending = true;
        set_reason("Neural Rendering is waiting: the drawing size is larger than the screen.");
        g.active.store(0, std::memory_order_release);
        g.skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    bool scaled = w != g.in_w || h != g.in_h; // Quality/Balanced/Performance: upsample depth/MV
    // A60: the chain's encoding decides (PQ / scRGB / SDR); the user can force
    // "HDR picture" (then the format picks PQ or scRGB) or "Standard picture".
    uint32_t hdr_mode = st.nr_hdr_mode.load();
    na::color::Encoding enc = na::color::Classify(bb.Format, color_space);
    if (hdr_mode == 2) enc = na::color::Encoding::SDR;
    else if (hdr_mode == 1 && enc == na::color::Encoding::SDR)
        enc = bb.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? na::color::Encoding::scRGB : na::color::Encoding::HDR10;
    if (enc == na::color::Encoding::HDR10 && bb.Format != DXGI_FORMAT_R10G10B10A2_UNORM) enc = na::color::Encoding::SDR;
    if (enc == na::color::Encoding::scRGB && bb.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) enc = na::color::Encoding::SDR;
    bool hdr = na::color::IsHdr(enc);
    uint32_t shader_mode = na::color::ShaderMode(enc);
    // Frame slot: never wait; skip when the allocator from kRing frames ago is busy.
    Frame& f = g.frames[g.frame_index];
    const auto completed = g.fence->GetCompletedValue();
    if (completed == UINT64_MAX || g.signal_failed) { latch("Neural Rendering stopped: GPU completion is unavailable."); return; }
    if (f.fence_value && completed < f.fence_value) {
        g.reset_pending = true;
        g.busy_slots.fetch_add(1, std::memory_order_relaxed);
        set_reason("Neural Rendering skipped: its command buffer is still in use.");
        g.skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    read_timings_locked(queue);
    // All eight in-flight frames can reference the same work textures and
    // feature. Replacing them requires the LAST submission, not just this slot.
    const bool work_changes = !g.color_in || g.width != w || g.height != h || g.hdr.load() != (hdr ? 1u : 0u) ||
        g.hdr_mode_used != shader_mode || g.feat_format != bb.Format || (g.feature && g.feat_model != st.nr_model.load());
    const bool scaled_changes = scaled && (!g.depth_up || g.depth_up->GetDesc().Width != w || g.depth_up->GetDesc().Height != h ||
        !g.motion_up || g.motion_up->GetDesc().Format != g.motion_format);
    if ((work_changes || scaled_changes) && completed < g.fence_next - 1) {
        g.reset_pending = true; g.active.store(0); g.skipped.fetch_add(1);
        set_reason("Neural Rendering is waiting for its previous picture size to finish.");
        return; // nonblocking: retry a later frame, keep every old resource alive
    }
    // Resources for this extent/mode.
    if (!g.color_in || g.width != w || g.height != h || g.hdr.load() != (hdr ? 1u : 0u) || g.hdr_mode_used != shader_mode ||
        g.feat_format != bb.Format) {
        bool ok = true;
        if (hdr) {
            ok &= uav_store_supported(DXGI_FORMAT_R16G16B16A16_FLOAT) && uav_store_supported(bb.Format);
            ok &= create_texture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, true, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, g.color_in);
            ok &= create_texture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g.nr_out);
            ok &= create_texture(w, h, bb.Format, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g.hdr_out); // re-encoded in the chain's format
        } else {
            ok &= uav_store_supported(bb.Format);
            ok &= create_texture(w, h, bb.Format, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, g.color_in);
            ok &= create_texture(w, h, bb.Format, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g.nr_out);
            g.hdr_out.Reset();
        }
        if (!ok) {
            latch("Neural Rendering does not support this picture format on this graphics card.");
            return;
        }
        g.width = w;
        g.height = h;
        g.hdr.store(hdr ? 1u : 0u, std::memory_order_release);
        g.hdr_mode_used = shader_mode;
    }
    if (scaled && (!g.depth_up || g.depth_up->GetDesc().Width != w || g.depth_up->GetDesc().Height != h ||
                   !g.motion_up || g.motion_up->GetDesc().Format != g.motion_format)) {
        if (!uav_store_supported(DXGI_FORMAT_R32_FLOAT) || !uav_store_supported(g.motion_format) ||
            !create_texture(w, h, DXGI_FORMAT_R32_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g.depth_up) ||
            !create_texture(w, h, g.motion_format, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g.motion_up)) {
            latch("Neural Rendering could not prepare the scaled-mode buffers on this graphics card.");
            return;
        }
    }

    f.depth = g.depth_copy;
    f.motion = g.motion_copy;
    if (FAILED(f.alloc->Reset()) || FAILED(g.list->Reset(f.alloc.Get(), nullptr))) {
        latch("Neural Rendering could not prepare its command buffer.");
        return;
    }
    if (!ensure_feature_locked(w, h, hdr, bb.Format)) {
        g.list->Close();
        return;
    }
    bool run = g.feature_pending == 0; // the create recorded this frame executes first
    uint32_t qbase = g.frame_index * 2;
    g.list->EndQuery(g.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, qbase);
    if (run) {
        ID3D12DescriptorHeap* heaps[] = {g.heap.Get()};
        uint32_t slot = g.frame_index * 16;
        Constants c{static_cast<float>(st.nr_paper_white.load()), w, h, shader_mode, g.in_w, g.in_h};
        if (scaled) {
            // Phase 4: render-size depth/MV -> display size (nearest / bilinear).
            write_srv(slot + 8, g.depth_copy.Get(), DXGI_FORMAT_R32_FLOAT);
            write_srv(slot + 9, g.depth_copy.Get(), DXGI_FORMAT_R32_FLOAT);
            write_srv(slot + 10, g.depth_copy.Get(), DXGI_FORMAT_R32_FLOAT);
            write_uav(slot + 11, g.depth_up.Get(), DXGI_FORMAT_R32_FLOAT);
            write_srv(slot + 12, g.motion_copy.Get(), g.motion_format);
            write_srv(slot + 13, g.motion_copy.Get(), g.motion_format);
            write_srv(slot + 14, g.motion_copy.Get(), g.motion_format);
            write_uav(slot + 15, g.motion_up.Get(), g.motion_format);
            g.list->SetDescriptorHeaps(1, heaps);
            g.list->SetComputeRootSignature(g.root.Get());
            g.list->SetComputeRoot32BitConstants(1, kConstants, &c, 0);
            g.list->SetPipelineState(g.pso_up_depth.Get());
            g.list->SetComputeRootDescriptorTable(0, table_gpu(slot + 8));
            g.list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
            g.list->SetPipelineState(g.pso_up_motion.Get());
            g.list->SetComputeRootDescriptorTable(0, table_gpu(slot + 12));
            g.list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
            D3D12_RESOURCE_BARRIER bu[4] = {
                uav_barrier(g.depth_up.Get()), uav_barrier(g.motion_up.Get()),
                transition(g.depth_up.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                transition(g.motion_up.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
            g.list->ResourceBarrier(4, bu);
        }
        if (hdr) {
            // Pass 1: PQ back buffer -> SDR working image.
            D3D12_RESOURCE_BARRIER b1[2] = {
                transition(backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                transition(g.color_in.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
            g.list->ResourceBarrier(2, b1);
            write_srv(slot + 0, backbuffer, bb.Format);
            write_srv(slot + 1, backbuffer, bb.Format);
            write_srv(slot + 2, backbuffer, bb.Format);
            write_uav(slot + 3, g.color_in.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
            g.list->SetDescriptorHeaps(1, heaps);
            g.list->SetComputeRootSignature(g.root.Get());
            g.list->SetPipelineState(g.pso_hdr_to_sdr.Get());
            g.list->SetComputeRootDescriptorTable(0, table_gpu(slot));
            g.list->SetComputeRoot32BitConstants(1, kConstants, &c, 0);
            g.list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
            D3D12_RESOURCE_BARRIER b2[2] = {
                uav_barrier(g.color_in.Get()),
                transition(g.color_in.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
            g.list->ResourceBarrier(2, b2);
        } else {
            D3D12_RESOURCE_BARRIER b1[2] = {
                transition(backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE),
                transition(g.color_in.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST)};
            g.list->ResourceBarrier(2, b1);
            g.list->CopyResource(g.color_in.Get(), backbuffer);
            D3D12_RESOURCE_BARRIER b2[2] = {
                transition(backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                transition(g.color_in.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
            g.list->ResourceBarrier(2, b2);
        }
        // NR evaluate: Color=color_in (NPSR), Depth/MVec (NPSR), Output=nr_out (UAV).
        if (!evaluate_locked(hdr)) {
            // Leave the back buffer presentable and give up this frame.
            g.reset_pending = true;
            if (scaled) {
                D3D12_RESOURCE_BARRIER restore[2] = {
                    transition(g.depth_up.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                    transition(g.motion_up.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
                g.list->ResourceBarrier(2, restore);
            }
            D3D12_RESOURCE_BARRIER back = transition(backbuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PRESENT);
            g.list->ResourceBarrier(1, &back);
            g.list->Close();
            ID3D12CommandList* lists[] = {g.list.Get()};
            submit_locked(queue, 1, lists);
            f.fence_value = g.fence_next++;
            if (FAILED(queue->Signal(g.fence.Get(), f.fence_value))) {
                g.signal_failed = true; latch("Neural Rendering stopped: GPU completion signal failed.");
            }
            g.frame_index = (g.frame_index + 1) % kRing;
            return;
        }
        D3D12_RESOURCE_BARRIER nr_done = uav_barrier(g.nr_out.Get());
        g.list->ResourceBarrier(1, &nr_done);
        if (scaled) {
            D3D12_RESOURCE_BARRIER back_up[2] = {
                transition(g.depth_up.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                transition(g.motion_up.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
            g.list->ResourceBarrier(2, back_up);
        }
        if (hdr) {
            // Pass 2: ratio on the original HDR -> PQ, then copy into the back buffer.
            D3D12_RESOURCE_BARRIER b3[1] = {
                transition(g.nr_out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
            g.list->ResourceBarrier(1, b3);
            write_srv(slot + 4, backbuffer, bb.Format);
            write_srv(slot + 5, g.nr_out.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
            write_srv(slot + 6, g.color_in.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
            write_uav(slot + 7, g.hdr_out.Get(), bb.Format);
            g.list->SetDescriptorHeaps(1, heaps); // NGX changed state
            g.list->SetComputeRootSignature(g.root.Get());
            g.list->SetPipelineState(g.pso_sdr_to_hdr.Get());
            g.list->SetComputeRootDescriptorTable(0, table_gpu(slot + 4));
            g.list->SetComputeRoot32BitConstants(1, kConstants, &c, 0);
            g.list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
            D3D12_RESOURCE_BARRIER b4[3] = {
                uav_barrier(g.hdr_out.Get()),
                transition(g.hdr_out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
                transition(backbuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST)};
            g.list->ResourceBarrier(3, b4);
            g.list->CopyResource(backbuffer, g.hdr_out.Get());
            D3D12_RESOURCE_BARRIER b5[3] = {
                transition(backbuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT),
                transition(g.hdr_out.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                transition(g.nr_out.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
            g.list->ResourceBarrier(3, b5);
        } else {
            D3D12_RESOURCE_BARRIER b3[2] = {
                transition(g.nr_out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
                transition(backbuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST)};
            g.list->ResourceBarrier(2, b3);
            g.list->CopyResource(backbuffer, g.nr_out.Get());
            D3D12_RESOURCE_BARRIER b4[2] = {
                transition(backbuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT),
                transition(g.nr_out.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
            g.list->ResourceBarrier(2, b4);
        }
    }
    g.list->EndQuery(g.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, qbase + 1);
    g.list->ResolveQueryData(g.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, qbase, 2, g.query_readback.Get(),
                             static_cast<UINT64>(g.frame_index) * 16);
    if (FAILED(g.list->Close())) {
        latch("Neural Rendering could not finish its command buffer.");
        return;
    }
    ID3D12CommandList* lists[] = {g.list.Get()};
    submit_locked(queue, 1, lists);
    f.fence_value = g.fence_next++;
    if (FAILED(queue->Signal(g.fence.Get(), f.fence_value))) {
        g.signal_failed = true; latch("Neural Rendering stopped: GPU completion signal failed."); return;
    }
    g.frame_index = (g.frame_index + 1) % kRing;
    if (g.feature_pending) {
        g.feature_pending = 0; // executes before the next frame's evaluate on the same queue
        set_reason("Neural Rendering is starting.");
    } else {
        g.active.store(1, std::memory_order_release);
        char why[96];
        snprintf(why, sizeof(why), "Neural Rendering is active%s%s%s.", hdr ? " (" : "", hdr ? (shader_mode == 2 ? "HDR picture, scRGB" : "HDR picture") : "",
                 scaled ? (hdr ? ", scaled mode)" : " (scaled mode)") : (hdr ? ")" : ""));
        set_reason(why);
    }
}

Stats GetStats()
{
    Stats s{};
    s.runtime_present = g.has_rt.load(std::memory_order_acquire);
    s.active = g.active.load(std::memory_order_acquire);
    s.latched_off = g.latched.load(std::memory_order_acquire);
    s.hdr = g.hdr.load(std::memory_order_acquire);
    s.evaluates = g.evaluates.load(std::memory_order_relaxed);
    s.skipped = g.skipped.load(std::memory_order_relaxed);
    s.inputs_seen = g.inputs_seen.load();
    s.input_overwrites = g.input_overwrites.load();
    s.missing_inputs = g.missing_inputs.load();
    s.busy_slots = g.busy_slots.load();
    s.last_ms = g.last_ms.load();
    s.avg_ms = g.avg_ms.load();
    s.gpu_samples = g.samples.load(std::memory_order_relaxed);
    std::unique_lock<std::mutex> lock(g.m, std::try_to_lock);
    if (lock.owns_lock()) {
        s.gpu_submitted = g.fence_next - 1;
        s.gpu_completed = g.fence ? g.fence->GetCompletedValue() : 0;
        s.width = g.width;
        s.height = g.height;
        strncpy_s(s.reason, g.reason, _TRUNCATE);
    } else {
        strncpy_s(s.reason, "Updating.", _TRUNCATE);
    }
    return s;
}

void ResetLatch()
{
    std::lock_guard<std::mutex> lock(g.m);
    if (g.signal_failed) { set_reason("Restart the game: Neural Rendering lost its GPU completion signal."); return; }
    g.reset_pending = true;
    g.latched.store(0, std::memory_order_release);
    set_reason("Neural Rendering is off.");
}

bool RecordDepthToR32ForTests(ID3D12GraphicsCommandList* list, ID3D12Resource* depth, DXGI_FORMAT view, ID3D12Resource* out,
                              uint32_t bx, uint32_t by, uint32_t w, uint32_t h)
{
    if (!list || !depth || !out) return false;
    std::lock_guard<std::mutex> lock(g.m);
    ComPtr<ID3D12Device> dev;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&dev))) || !ensure_device_locked(dev.Get())) return false;
    record_depth_to_r32_locked(list, depth, view, out, bx, by, w, h);
    return true;
}

void Shutdown()
{
    std::lock_guard<std::mutex> lock(g.m);
    if (g.signal_failed) { latch("Neural Rendering shutdown retained resources after a failed GPU signal."); return; }
    if (g.fence && g.fence_next > 1 && g.fence->GetCompletedValue() < g.fence_next - 1) {
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (ev) {
            if (SUCCEEDED(g.fence->SetEventOnCompletion(g.fence_next - 1, ev))) WaitForSingleObject(ev, 2000);
            CloseHandle(ev);
        }
        if (g.fence->GetCompletedValue() < g.fence_next - 1) {
            latch("Neural Rendering shutdown is waiting for GPU completion; resources retained."); return;
        }
    }
    release_feature_locked();
    if (g.params && g.rt.destroy_parameters) reinterpret_cast<PfnDestroyParams>(g.rt.destroy_parameters)(g.params);
    g.params = nullptr;
    g.depth_copy.Reset();
    g.motion_copy.Reset();
    g.recording_depth.Reset();
    g.recording_motion.Reset();
    for (auto& p : g.pending) p = {};
    g.pending_count.store(0);
    g.input_queue.Reset();
    g.depth_up.Reset();
    g.motion_up.Reset();
    g.color_in.Reset();
    g.nr_out.Reset();
    g.hdr_out.Reset();
    g.hdr_mode_used = 0;
    g.pso_hdr_to_sdr.Reset();
    g.pso_sdr_to_hdr.Reset();
    g.pso_up_depth.Reset();
    g.pso_up_motion.Reset();
    g.root.Reset();
    g.heap.Reset();
    g.queries.Reset();
    g.query_readback.Reset();
    g.list.Reset();
    for (Frame& f : g.frames) { f.alloc.Reset(); f.depth.Reset(); f.motion.Reset(); f.fence_value = 0; }
    g.fence.Reset();
    g.device.Reset();
    g.have_inputs = 0;
    g.inputs_fresh = 0;
    g.in_w = g.in_h = g.width = g.height = 0;
    g.active.store(0);
    g.latched.store(0);
    g.frame_index = 0;
    g.fence_next = 1; g.reset_pending = true; g.last_evaluate_tick = 0; g.setting_signature = 0;
    set_reason("Neural Rendering is off.");
}

} // namespace na::nr
