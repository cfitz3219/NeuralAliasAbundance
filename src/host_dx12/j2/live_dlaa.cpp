#include "host_dx12/j2/live_dlaa.hpp"
#include "host_dx12/j2/msaa_resolve.hpp"
#include "host_dx12/j2/nis_sharpen.hpp"
#include "host_dx12/j2/route_selector.hpp"
#include "host_dx12/j2/consumer_profile.hpp"
#include "host_dx12/j2/jitter_writes.hpp"
#include "host_dx12/j2/j2_flow.hpp"
#include "host_dx12/j2/compute_flow.hpp"
#include "host_dx12/capture/j2_capture.hpp"
#include "host_dx12/ngx_observe/ngx_observe.hpp"
#include "host_dx12/upscalers/ffx_intercept.hpp"
#include "host_dx12/upscalers/xess_intercept.hpp"

#include "core/motion_arbiter_gpu.hpp"
#include "core/temporal_jitter.hpp"
#include "host_dx12/depth_format.hpp"
#include "host_dx12/dlss/dlss_backend.hpp"
#include "host_dx12/nr/native_fg.hpp"
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

constexpr uint32_t kRing = 24;           // recordings in flight before a slot is reused (never waited on):
                                         // up to kMaxCandidates per frame (exact inline placement)
constexpr uint32_t kMaxCandidates = 6;   // predicated inline DLAA candidates in one scene recording
constexpr uint32_t kBlockBytes = 512;    // root CBVs have no size: read this much (the producer's own bound)
constexpr size_t kMaxBlocks = 256;       // unique constant blocks examined per frame (until the camera slot is learned)
constexpr uint32_t kMaxBlockRead = 1024; // bytes read from one constant buffer
constexpr uint64_t kViewId = 0x4A32;     // DLSS view for this source ("J2")

struct Slot {
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    UINT64 fence_value = 0;
    std::shared_ptr<const PhysicalInput> physical; // Layer2 payload used by this slot's list: kept
                                                   // until the slot is reused (its fence completed)
};

struct PendingHistory {
    na::camera::CameraTracker tracker;
    bool cut = false;
    uint32_t capture = 0; // J2 evidence capture token for this evaluation (0 = none)
    na::j2::compute_flow::Ticket compute_flow;
};

// CPU metadata of this frame's last submitted scene. No pooled command-list
// pointer survives Execute: games reset those lists before the consumer arrives.
struct SceneInput {
    uint64_t frame = 0, color = 0, depth = 0, producer = 0;
    uint32_t color_sub = 0, depth_sub = 0;
    na::camera::Matrix vp{};
    bool reversed = false;
    double jitter[2] = {};
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
    uint32_t w = 0, h = 0;          // scene colour/depth allocation (J2's input copies)
    uint32_t rw = 0, rh = 0;        // P4-4: active render extent (camera, jitter, motion); == w,h for DLAA
    uint32_t ow = 0, oh = 0;        // P4-4: output size (output/sharp textures); == w,h for DLAA
    DXGI_FORMAT color_format = DXGI_FORMAT_UNKNOWN;
    na::motion::gpu::Arbiter arbiter;
    bool arbiter_ok = false;
    na::camera::CameraTracker tracker;
    // SE5 flashing (Cort 2026-11-02): ~10 % of frames had no readable camera, so they kept
    // the game's raw (aliased, mip-biased, unsharpened) colour between DLAA'd neighbours:
    // a flash on detailed texture. The last two unjittered cameras let such a frame run
    // DLAA on a predicted camera instead (see record(): 4 frames moving, 120 still).
    na::camera::Matrix cam_u0{}, cam_u1{}; // unjittered VPs of the last two read frames (u1 newest)
    uint64_t cam_f0 = 0, cam_f1 = 0;       // their frames (0 = none)
    bool cam_rev = false;
    // SE5 main menu (Cort 2026-11-02): the depth's state at the scene end was unknown on ~1/4
    // of frames (the state store reset: ambiguous_queue_order). The same scene end reports
    // the same depth state frame after frame; once it has been the same on kDepthLearnFrames
    // known frames in a row, a frame whose lookup fails but whose scene list did render into
    // that depth uses it, instead of showing the raw frame.
    uint32_t depth_learned_state = 0;
    uint32_t depth_learned_sub = 0xFFFFFFFFu;
    na_object_id depth_learned_id = 0;
    uint32_t depth_learned_streak = 0;
    uint64_t depth_learned_frame = 0; // last frame the state was known
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
    SceneInput waiting_scene;
    JitterWrites jitter_writes;
    std::vector<na::j2::ColorEvent> frame_seq; // this frame's writes/consumers of the colour, submission order
    bool frame_seq_complete = true;
    uint64_t scene_color_id = 0;
    uint32_t scene_w = 0, scene_h = 0;         // selected colour, mip 0 (class size reference)
    bool class_fired = false;                   // this frame's DLAA was triggered by a class
    na::j2::ClassKey fired_class;
    uint64_t fired_producer = 0;
    uint64_t fired_context = 0;
    uint32_t fired_mode = 0;                    // 1 class, 2 pair, 3 barrier, 4 boundary matcher, 5 between-list reader
    bool fired_barrier_event = false;           // the placement was judged at a barrier event
    // First scene write after this frame's DLAA (the late writer): pipeline, depth bound, where
    // (1 same list after our insertion, 2 a later list of the same batch, 3 a later batch).
    uint64_t late_frame = 0, late_pso = 0;
    uint32_t late_where = 0, late_writes = 0;
    bool late_depth = false;
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
    // Exact inline placement (Cort 2026-11-02: fix the source, not the trigger). SE5 draws
    // its main scene in segments with a colour read in between; which read follows the LAST
    // segment is only known once the list is complete. Every possible end in the recording
    // gets a DLAA candidate wrapped in GPU predication (value 0 = skipped). At submission the
    // whole list is known: the first candidate after the final colour write is switched on,
    // the others stay off, and only that candidate's history is committed.
    // dlss_predication_test (RTX 5070 Ti): predicating the chosen evaluate is bit-exact; each
    // extra switched-off evaluate shifts DLSS by at most 0.02 on one pixel (mean 6e-5), bounded.
    struct InlinePending {
        const void* list = nullptr;
        uint64_t rec = 0, frame = 0;
        uint32_t slot = 0;
        uint32_t writes = 0;
        uint64_t color = 0;
        PendingHistory history;
        // What placed this candidate (credited/blamed only if it is the one that runs).
        uint32_t mode = 0;
        bool by_class = false, at_barrier = false;
        na::j2::ClassKey cls;
        uint64_t producer = 0, context = 0;
    } inl[kMaxCandidates];
    uint32_t inl_n = 0; // candidates of the pending recording (all in one list/recording, one frame)
    ComPtr<ID3D12Resource> pred; // upload, one 8-byte predicate per ring slot (GENERIC_READ includes PREDICATION)
    uint64_t* pred_cpu = nullptr;
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
// P4-4 upscale target: set by Stage P / tests from any thread, read at recording.
std::mutex g_up_m;
UpscaleTarget g_up;
PhysicalInputProvider g_phys; // under g_up_m

// Exactly one outcome per Provided / Unavailable lookup, on every exit of record().
struct PhysicalReport {
    PhysicalInputProvider p;
    uint64_t generation = 0;
    bool armed = false;
    PhysicalOutcome result = PhysicalOutcome::Failed;
    char why[160] = {};
    void arm(const PhysicalInputProvider& pp, uint64_t gen) { p = pp; generation = gen; armed = true; }
    void fail(const char* w) { result = PhysicalOutcome::Failed; std::snprintf(why, sizeof why, "%s", w); }
    ~PhysicalReport();
};

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
// motion on a still camera), 3 depth (banded). White = non-finite motion (no direction
// hue is ever white; magenta is the right-and-up hue, which misled the first Anno read).
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
cbuffer C : register(b0) { uint mode; float scale; uint w; uint h; uint iw; uint ih; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= w || id.y >= h) return;
    uint2 p = id.xy * uint2(iw, ih) / uint2(w, h); // output pixel -> render pixel (P4-4)
    float3 c;
    if (mode == 3) {
        float d = t_depth[p];
        c = frac(d * float3(1.0, 16.0, 256.0));
    } else {
        float2 v = t_mv[p];
        if (any(isnan(v)) || any(isinf(v))) { c = float3(1, 1, 1); }
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
    p[1].Constants.Num32BitValues = 6;
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
    uint32_t k[6] = {static_cast<uint32_t>(mode), 0, g.ow, g.oh, g.rw, g.rh};
    std::memcpy(&k[1], &scale, 4);
    l->SetComputeRoot32BitConstants(1, 6, k, 0);
    l->Dispatch((g.ow + 7) / 8, (g.oh + 7) / 8, 1);
    return true;
}

// ---- Sharpening: RCAS (Robust Contrast-Adaptive Sharpening) ------------------
// AMD FidelityFX Super Resolution 1 "RCAS" (ffx_fsr1.h, MIT licence, (c) Advanced
// Micro Devices): a 5-tap cross with contrast-dependent clipping limiters
// (not a guarantee against edge halos), with its noise detection
// (FSR_RCAS_DENOISE) so flat noisy areas are not amplified. Runs on the DLAA
// result before the game's tonemap and HUD, in a Reinhard-compressed copy of the
// HDR values (RCAS expects 0..1), expanded back afterwards. Slider 1..100 maps to
// RCAS sharpness 2..0 stops; 0 skips the pass entirely. NeuralAlias's 101..150
// extension raises only the global lobe ceiling from 3/16 to 13/64. Contrast
// limiters and noise attenuation remain; 1+4*lobe stays >=3/16. No second filter.
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
cbuffer C : register(b0) { float con; uint w; uint h; float lobeLimit; };
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
    float lobe = max(-lobeLimit, min(max3(lobeRGB.r, lobeRGB.g, lobeRGB.b), 0.0)) * con;
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
    const float extra = percent > 100 ? float((std::min)(percent, 150u) - 100u) / 100.0f : 0.0f;
    const float limit = 3.0f / 16.0f + extra / 32.0f;
    uint32_t k[4] = {0, w, h, 0};
    std::memcpy(&k[0], &con, 4);
    std::memcpy(&k[3], &limit, 4);
    l->SetComputeRoot32BitConstants(1, 4, k, 0);
    l->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    return true;
}
// Retained for the standalone NIS test helper; the live slider uses RCAS only.
na::j2::NisSharpen g_nis;
bool rcas_record(ID3D12GraphicsCommandList* l, uint32_t percent, DXGI_FORMAT fmt)
{
    return g.sharp && rcas_dispatch(l, g.output.Get(), g.sharp.Get(), g.ow, g.oh, percent, fmt, g.slot);
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
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = kRing * 8;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        void* p = nullptr;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g.pred))) ||
            FAILED(g.pred->Map(0, nullptr, &p))) { g.pred.Reset(); return false; }
        g.pred_cpu = static_cast<uint64_t*>(p);
        for (uint32_t i = 0; i < kRing; ++i) g.pred_cpu[i] = 0;
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
// switched_off: the recording was just submitted with every remaining candidate's predicate
// at 0 (they never run). Otherwise the list may still be submitted later: retire captures.
void discard_inline(bool switched_off = false)
{
    for (uint32_t i = 0; i < g.inl_n; ++i) {
        if (switched_off) na::capture::j2::Withdraw(g.inl[i].history.capture);
        else na::capture::j2::Discard(g.inl[i].history.capture, true);
        g.inl[i].history.capture = 0;
    }
    g.inl_n = 0;
}
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
        if (d.history_generation > 1) ++g.stats.history_resets; // the first route is not a reset
        g.resume_reset = true;   // output of the old route must not feed the new one
        discard_inline(); // an inline recording from the old route is not committed
        g.waiting_scene = {};
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
    for (int i = static_cast<int>(last); i >= 0; --i) {
        const auto evidence = na::observe::ListStateSub(lists[i], id, sub, st);
        if (evidence == na::observe::StateLookup::Known) return true;
        if (evidence == na::observe::StateLookup::Unavailable) return false;
    }
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
constexpr uint32_t kJitterPhases = 8; // DLAA; upscaling uses JitterPhasesFor (8 x ratio^2)
uint32_t jitter_phases() { return g.ow && g.rw ? JitterPhasesFor(g.rw, g.ow) : kJitterPhases; }

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
struct { bool valid = false; na::camera::VpGeometry geo{}; int hand = 0; } g_jit_cam;

// Winding of a ViewProjection: the sign of the determinant of its x, y and w rows (3x3 part).
// A mirrored camera (a planar water / mirror reflection) has the opposite winding of the main
// camera in every engine, whatever its own convention (left- or right-handed): a universal
// tell, used once the main camera is known.
int handedness(const na::camera::Matrix& m)
{
    const double a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g3 = m[12], h = m[13], i = m[14];
    const double det = a * (e * i - f * h) - b * (d * i - f * g3) + c * (d * h - e * g3);
    return det > 0 ? 1 : det < 0 ? -1 : 0;
}
void remember_main_camera(const na::camera::Matrix& m)
{
    g_jit_cam.valid = na::camera::DecomposeViewProjection(m, g_jit_cam.geo);
    g_jit_cam.hand = handedness(m);
}

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

// A block from the camera slot starts this frame's jitter only if it continues the last main
// camera: a water reflection's mirrored camera (or a sky / light camera) can sit in the same slot
// and be bound earlier in the frame. Generous enough for real motion within one frame.
bool continues_main_camera(const na::camera::Matrix& m)
{
    na::camera::VpGeometry v{};
    if (!g_jit_cam.valid) return true;
    if (!na::camera::DecomposeViewProjection(m, v)) return false;
    if (g_jit_cam.hand && handedness(m) == -g_jit_cam.hand) return false; // mirrored: a reflection
    const auto& p = g_jit_cam.geo;
    double dot = 0, d2 = 0;
    for (int j = 0; j < 3; ++j) {
        dot += v.forward[j] * p.forward[j];
        d2 += (v.position[j] - p.position[j]) * (v.position[j] - p.position[j]);
    }
    return std::acos(std::clamp(dot, -1.0, 1.0)) <= 0.25 && d2 <= 25.0 * 25.0 && std::abs(v.sy - p.sy) <= 0.05 * std::abs(p.sy);
}

bool begin_jitter(uint64_t res, uint64_t off, const std::vector<uint8_t>& in_bytes, bool slot)
{
    const size_t n = in_bytes.size();
    if (g.camera_offset < 0 || static_cast<uint64_t>(g.camera_offset) + 64 > n || !g.rw || !g.rh) return false;
    // jitter_batch has normalized this exact buffer range using the write ledger.
    const auto& bytes = in_bytes;
    for (auto layout : {na::camera::Layout::RowMajor, na::camera::Layout::ColumnMajor}) {
        const auto m = na::camera::LoadMatrix(bytes, static_cast<size_t>(g.camera_offset), layout);
        na::camera::Candidate c{};
        if (!na::camera::ViewProjection(m, c)) continue;
        if (!slot && !same_camera(m)) continue;
        if (slot && !continues_main_camera(m)) { ++g.stats.jitter_slot_other_camera; continue; }
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
        remember_main_camera(m);
        const neural_alias::JitterSample js = neural_alias::temporal_jitter(++g.jit.phase, jitter_phases());
        std::array<double, 16> src{};
        for (int i = 0; i < 16; ++i) src[i] = m[i];
        const auto jm = neural_alias::jitter_projection(src, js, g.rw, g.rh);
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
                const auto jp = neural_alias::jitter_projection(ps, js, g.rw, g.rh);
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
        g.stats.jitter_rebased += g.jitter_writes.Normalize(k->res, k->off, view.data(), view.size());
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
                    const auto jp = neural_alias::jitter_projection(ps, neural_alias::JitterSample{g.jit.px[0], g.jit.px[1]}, g.rw, g.rh);
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
                if (rep && g.jitter_writes.Apply(k->res, k->off + o, view.data() + o, rep, [&] {
                    return na::observe::WriteBuffer(k->res, k->off + o, rep, 64) == 64;
                })) {
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

// Several blocks can hold a valid ViewProjection + inverse in the same frame: the main camera,
// a water reflection's mirrored camera, a sky camera, a light's. Ranking blocks by how often they
// are bound picks whichever dominates the frame's draws, which changes with where the player
// looks (Sniper: looking down at water or up at the sky alternated cameras: no camera, false cuts,
// unjittered frames, flicker; Cort 2026-11-01). Among the candidates the one most continuous with the
// last main camera wins (holding the matrix J1 jittered is only a tie-break).
struct CamPick {
    na::camera::Matrix vp{};
    bool reversed = false, injected = false;
    double score = 0;
    const Block* block = nullptr;
    int64_t offset = 0, inv_delta = 0, proj_delta = INT64_MIN;
};
bool holds_jittered(const std::vector<uint8_t>& bytes, size_t off)
{
    return g.jit.valid && off + 64 <= bytes.size() &&
           (!std::memcmp(bytes.data() + off, g.jit.jvp[0], 64) || !std::memcmp(bytes.data() + off, g.jit.jvp[1], 64));
}
double continuity(const na::camera::Matrix& m, bool injected)
{
    // Holding J1's jittered matrix is only a tie-break: J1 could have started on the wrong camera.
    const double bonus = injected ? -0.05 : 0.0;
    na::camera::VpGeometry v{};
    if (!g_jit_cam.valid) return bonus;
    if (!na::camera::DecomposeViewProjection(m, v)) return 1e9;
    const double mirrored = g_jit_cam.hand && handedness(m) == -g_jit_cam.hand ? 10.0 : 0.0; // a reflection's camera
    const auto& p = g_jit_cam.geo;
    double dot = 0, d2 = 0;
    for (int j = 0; j < 3; ++j) {
        dot += v.forward[j] * p.forward[j];
        d2 += (v.position[j] - p.position[j]) * (v.position[j] - p.position[j]);
    }
    return bonus + mirrored + std::acos(std::clamp(dot, -1.0, 1.0)) + std::sqrt(d2) / 25.0 + std::abs(v.sy - p.sy);
}
const CamPick* best_pick(const std::vector<CamPick>& v)
{
    const CamPick* best = nullptr;
    for (const auto& c : v) if (!best || c.score < best->score) best = &c;
    if (best && best != &v.front()) ++g.stats.camera_alternatives;
    if (v.size() > 1) ++g.stats.camera_contested;
    return best;
}

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
    const auto read_block = [&](const Block* o, na::camera::Snapshot& s) {
        const uint32_t want = o->size < kMaxBlockRead ? o->size : kMaxBlockRead;
        const uint32_t n = na::observe::ReadBuffer(o->resource, o->offset, bytes.data(), want);
        if (n < 128) return false;
        ++g_cd.read;
        // An in-process read at submission time, inside the submitting call:
        // its scope is known (FindMatrices rejects snapshots without one).
        s.session = s.epoch = 1;
        s.submission = s.content_version = g.frame;
        s.resource = o->resource;
        s.base_offset = 0; // offsets are relative to the block
        s.complete = s.lifetime_valid = true;
        s.sync = na::camera::Sync::AppSyncUnknown;
        s.bytes.assign(bytes.begin(), bytes.begin() + n);
        return true;
    };
    // 1. The learned slot: every block bound there this frame is a candidate (bounded).
    std::vector<CamPick> picks;
    for (const Block* o : order) {
        if (!learned(o) || picks.size() >= 8) break;
        na::camera::Snapshot s{};
        if (!read_block(o, s)) continue;
        CamPick c;
        if (!check_learned(s.bytes, &c.vp, &c.reversed)) continue;
        c.injected = holds_jittered(s.bytes, static_cast<size_t>(g.camera_offset));
        c.score = continuity(c.vp, c.injected);
        c.block = o;
        picks.push_back(c);
    }
    if (const CamPick* p = picks.empty() ? nullptr : best_pick(picks)) {
        ++g.stats.camera_learned_hits;
        *vp = p->vp;
        *reversed = p->reversed;
        g_injected = p->injected;
        return true;
    }
    // 2. A full search; a few candidates are compared the same way before the slot is relearned.
    struct BlockNote { uint32_t param, size, uses, read; bool table; uint32_t vp, inv, proj, view; };
    std::vector<BlockNote> notes;
    CamPick loose;               // a ViewProjection WITHOUT its inverse that continues the main camera
    bool have_loose = false;     // (accepted for this frame only; the slot is not relearned from it)
    for (const Block* o : order) {
        if (picks.size() >= 4) break;
        na::camera::Snapshot s{};
        const bool read_ok = read_block(o, s);
        const bool noted = notes.size() < 12;
        if (noted) notes.push_back({o->param, o->size, o->uses, read_ok ? static_cast<uint32_t>(s.bytes.size()) : 0u, o->table, 0, 0, 0, 0});
        if (!read_ok) continue;
        const auto scan = na::camera::FindMatrices(s);
        g_cd.cands += scan.candidates.size();
        for (const auto& c : scan.candidates) {
            if (noted) {
                BlockNote& bn = notes.back();
                if (c.kind == na::camera::Kind::ViewProjection) { ++bn.vp; if (c.has_inverse_pair) ++bn.inv; }
                else if (c.kind == na::camera::Kind::Projection) ++bn.proj;
                else ++bn.view;
            }
            if (c.kind == na::camera::Kind::ViewProjection) ++g_cd.vp;
            if (c.kind == na::camera::Kind::ViewProjection && !c.has_inverse_pair && !have_loose && same_camera(c.matrix)) {
                loose.vp = c.matrix;
                loose.reversed = c.reversed;
                loose.block = o;
                have_loose = true;
            }
            if (c.kind != na::camera::Kind::ViewProjection || !c.has_inverse_pair) continue;
            CamPick k;
            k.vp = c.matrix;
            k.reversed = c.reversed;
            k.block = o;
            k.offset = static_cast<int64_t>(c.byte_offset);
            k.inv_delta = static_cast<int64_t>(c.inverse_byte_offset) - static_cast<int64_t>(c.byte_offset);
            for (const auto& pc : scan.candidates)
                if (pc.kind == na::camera::Kind::Projection && pc.layout == c.layout && is_projection_of(pc.matrix, c.matrix)) {
                    k.proj_delta = static_cast<int64_t>(pc.byte_offset) - static_cast<int64_t>(c.byte_offset);
                    break;
                }
            k.injected = holds_jittered(s.bytes, c.byte_offset);
            k.score = continuity(k.vp, k.injected);
            picks.push_back(k);
            break; // one per block
        }
    }
    const CamPick* p = picks.empty() ? nullptr : best_pick(picks);
    if (!p && have_loose) {
        // The main camera's ViewProjection without an inverse beside it (a smaller per-view
        // block): the tracker inverts it itself; the inverse pair was only a validation, and
        // continuity with the last main camera validates it instead. The slot is left as learned.
        ++g.stats.camera_inverse_computed;
        *vp = loose.vp;
        *reversed = loose.reversed;
        g_injected = false;
        return true;
    }
    if (!p) {
        // Why the camera was lost (diagnostics for frames like Sniper's steep views).
        if (!g_cd.read) ++g.stats.camera_lost_unread;
        else if (!g_cd.vp) ++g.stats.camera_lost_no_vp;
        else ++g.stats.camera_lost_no_inverse;
        if (g_cd.blocks >= kMaxBlocks) ++g.stats.camera_lost_capped;
        // What the frame held (the last loss only; the status log writes it).
        char* t = g.stats.camera_lost_evidence;
        size_t cap = sizeof g.stats.camera_lost_evidence;
        int n = std::snprintf(t, cap, "frame=%llu lists=%zu binds=%zu blocks=%zu read=%zu vp=%zu cands=%zu learned{param=%lld table=%d off=%lld inv=%lld} main_cam=%d blocks[param/table size uses read vp/inv/proj/view]:",
                              static_cast<unsigned long long>(g.frame), g_cd.lists, g_cd.binds, g_cd.blocks, g_cd.read, g_cd.vp, g_cd.cands,
                              static_cast<long long>(g.camera_param), g.camera_table ? 1 : 0, static_cast<long long>(g.camera_offset),
                              static_cast<long long>(g.camera_inv_delta), g_jit_cam.valid ? 1 : 0);
        for (const auto& bn : notes) {
            if (n < 0 || static_cast<size_t>(n) >= cap) break;
            const int m = std::snprintf(t + n, cap - static_cast<size_t>(n), " %u/%d %u %u %u %u/%u/%u/%u", bn.param, bn.table ? 1 : 0, bn.size, bn.uses, bn.read, bn.vp, bn.inv, bn.proj, bn.view);
            if (m < 0) break;
            n += m;
        }
        return false;
    }
    g.camera_offset = p->offset;
    g.camera_inv_delta = p->inv_delta;
    g.camera_proj_delta = p->proj_delta;
    g.stats.camera_proj_delta = g.camera_proj_delta == INT64_MIN ? 0 : g.camera_proj_delta;
    g.stats.camera_inv_delta = g.camera_inv_delta;
    ++g.stats.camera_scans;
    g.camera_param = static_cast<int64_t>(p->block->param);
    g.camera_table = p->block->table;
    g.stats.camera_offset = static_cast<uint64_t>(p->offset);
    *vp = p->vp;
    *reversed = p->reversed;
    g_injected = p->injected;
    return true;
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
// SE5 late-scene diagnostic (Cort 2026-11-02: flicker only while panning). Records the first
// scene write after this frame's DLAA; note_late_frame logs it with what placed the DLAA.
void note_late_writer(ID3D12CommandList* list, uint64_t color, uint32_t skip_writes, uint32_t where, uint32_t writes)
{
    if (g.late_frame == g.frame) { g.late_writes += writes; return; }
    g.late_frame = g.frame; g.late_where = where; g.late_writes = writes; g.late_pso = 0; g.late_depth = false;
    std::vector<na::j2::ColorEvent> events;
    if (!list || !na::observe::ListColorEvents(list, color, events)) return;
    uint32_t seen = 0;
    for (const auto& e : events) {
        if (!e.write) continue;
        if (seen++ < skip_writes) continue;
        g.late_pso = e.producer; g.late_depth = e.with_depth; return;
    }
}
void note_late_frame()
{
    static const char* const kMode[] = {"none", "class", "pair", "barrier", "boundary", "between", "later", "any_boundary"};
    static const char* const kWhere[] = {"?", "same_list", "later_list", "later_batch"};
    char one[200];
    const bool have = g.late_frame == g.frame;
    std::snprintf(one, sizeof one, "%llu:%s%s/op=%u/prod=%llu/ctx=%llu->late{%s pso=%llu depth=%u writes=%u} ",
        static_cast<unsigned long long>(GetTickCount64() / 100 % 100000000), kMode[g.fired_mode < 8 ? g.fired_mode : 0],
        g.fired_barrier_event ? "@barrier" : "", static_cast<unsigned>(g.fired_class.op),
        (unsigned long long)g.fired_producer, (unsigned long long)g.fired_context,
        have ? kWhere[g.late_where < 4 ? g.late_where : 0] : "?", have ? (unsigned long long)g.late_pso : 0ull,
        have && g.late_depth ? 1u : 0u, have ? g.late_writes : 0u);
    char merged[sizeof g.stats.late_detail];
    std::snprintf(merged, sizeof merged, "%s%s", one, g.stats.late_detail); // newest first, keep what fits
    std::memcpy(g.stats.late_detail, merged, sizeof merged);
    ++g.stats.late_frames;
}

bool record(ID3D12CommandList* const* lists, UINT scene, na_object_id depth_id, na_object_id color_id,
            ID3D12GraphicsCommandList** out_list, PendingHistory& pending, ID3D12GraphicsCommandList* inl = nullptr,
            bool* touched = nullptr, const SceneInput* saved = nullptr, UINT prefix = 0,
            na::j2::flow::Ticket* flow_ticket = nullptr, bool flow_pair_ok = false)
{
    // P4-4 Layer2: an optional immutable physical snapshot for these logical ids.
    std::shared_ptr<const PhysicalInput> phys;
    PhysicalReport phys_report;
    {
        PhysicalInputProvider pp;
        { std::lock_guard<std::mutex> lk(g_up_m); pp = g_phys; }
        if (pp.lookup) {
            const PhysicalLookup pl = pp.lookup(pp.ctx, color_id, depth_id, inl, &phys);
            if (pl == PhysicalLookup::Unavailable) {
                phys_report.arm(pp, phys ? phys->generation : 0);
                phys_report.result = PhysicalOutcome::Unavailable;
                std::snprintf(phys_report.why, sizeof phys_report.why, "replacement unavailable");
                reason("Layer2: this recording was redirected but its replacement is unavailable; not evaluated.");
                return false;
            }
            if (pl != PhysicalLookup::Provided) phys.reset();
            else {
                phys_report.arm(pp, phys ? phys->generation : 0);
                if (!phys || !phys->color || !phys->depth || !phys->output || !phys->owner || !phys->render_w || !phys->render_h ||
                    phys->logical_color != color_id || phys->logical_depth != depth_id || phys->output == phys->color) {
                    phys_report.fail("the physical snapshot is incomplete or names other logical ids");
                    reason("Layer2: the physical snapshot is incomplete or names other logical ids.");
                    return false;
                }
            }
        }
    }
    const auto phys_fail = [&](const char* why) {
        if (phys) phys_report.fail(why);
        reason(why);
        return false;
    };
    ComPtr<ID3D12Resource> color, depth;
    if (phys) { color = phys->color; depth = phys->depth; } // logical ids remain the identity
    else {
        color.Attach(na::observe::AcquireResource(color_id));
        depth.Attach(na::observe::AcquireResource(depth_id));
    }
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
    if (phys) { csub = phys->color_sub; dsub = phys->depth_sub; }
    else if (saved) { csub = saved->color_sub; dsub = saved->depth_sub; }
    if ((!phys && !saved && !na::observe::ListSceneSubresource(lists[scene], color_id, depth_id, &csub, &dsub)) || csub == 0xFFFFFFFFu || dsub == 0xFFFFFFFFu) {
        if (phys) return phys_fail("Layer2: the physical subresources are invalid.");
        reason("The scene is drawn through a view NeuralAlias cannot place (several array slices at once, or an unusual view).");
        return false;
    }
    // States of exactly the two subresources J2 copies from and restores.
    uint32_t sc = 0, sd = 0;
    // Inline: the state recorded so far in this list, else the state all submitted work left.
    const auto state_now = [&](na_object_id id, uint32_t sub, uint32_t* st) {
        const auto evidence = na::observe::ListStateSub(inl, id, sub, st);
        return evidence == na::observe::StateLookup::Known ||
               (evidence == na::observe::StateLookup::Absent && na::observe::KnownSubStateNow(id, sub, st));
    };
    const auto entry_state = [&](na_object_id id, uint32_t sub, uint32_t* st) {
        return prefix ? state_after(lists, prefix - 1, id, sub, st)
                      : na::observe::KnownSubStateInCurrentSubmission(id, sub, st);
    };
    const auto state_of = [&](na_object_id id, uint32_t sub, uint32_t* st) {
        return saved ? entry_state(id, sub, st) : inl ? state_now(id, sub, st) : state_after(lists, scene, id, sub, st);
    };
    // Layer2 companions are private textures, not observed: their exact states come with them.
    const bool have_sc = phys ? (sc = phys->color_state, true) : state_of(color_id, csub, &sc);
    bool have_sd = phys ? (sd = phys->depth_state, true) : state_of(depth_id, dsub, &sd);
    if (inl && !phys && !saved) {
        // Inline only, learning and using only the state the list STARTED with (this list has
        // not transitioned the depth so far): Astra, COMMS 2026-11-02, rendering into the depth
        // proves its use, not its state. The same scene end reported that entry state on
        // kDepthLearnFrames known frames in a row before it is used for an unknown one.
        constexpr uint32_t kDepthLearnFrames = 30;
        constexpr uint64_t kDepthLearnMaxAge = 300;
        uint32_t in_list = 0;
        const bool untouched = na::observe::ListStateSub(inl, depth_id, dsub, &in_list) == na::observe::StateLookup::Absent;
        const bool same_target = g.depth_learned_id == depth_id && g.depth_learned_sub == dsub;
        if (have_sd && untouched) {
            if (g.depth_learned_frame != g.frame) { // once per frame
                if (same_target && g.depth_learned_state == sd) ++g.depth_learned_streak;
                else { g.depth_learned_streak = 1; g.depth_learned_state = sd; g.depth_learned_id = depth_id; g.depth_learned_sub = dsub; }
                g.depth_learned_frame = g.frame;
            }
        } else if (!have_sd && untouched && have_sc && same_target && g.depth_learned_streak >= kDepthLearnFrames &&
                   g.frame - g.depth_learned_frame <= kDepthLearnMaxAge && na::observe::ListRendersTo(inl, color_id, depth_id)) {
            sd = g.depth_learned_state;
            have_sd = true;
            ++g.stats.depth_state_assumed;
        }
    }
    if (!have_sc || !have_sd) {
        ++g.stats.no_state;
        if (!have_sc) ++g.stats.no_state_color; else ++g.stats.no_state_depth;
        if (!have_sd) na::observe::DescribeStateEvidence(inl ? inl : saved ? nullptr : lists[scene], depth_id, dsub,
                                                        g.stats.depth_state_evidence, sizeof g.stats.depth_state_evidence);
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

    // P4-4: upscaling destination (slider route). Any reason not to use it falls back
    // to DLAA at the scene's size, written back as before.
    UpscaleTarget up;
    { std::lock_guard<std::mutex> lk(g_up_m); up = g_up; }
    ComPtr<ID3D12Resource> dest;
    uint32_t rw = w, rh = h, ow = w, oh = h, dsub_out = 0, sdest = 0;
    const auto refuse_upscale = [&](const char* why) {
        dest.Reset();
        rw = ow = w; rh = oh = h;
        ++g.stats.upscale_refused;
        std::snprintf(g.stats.upscale_note, sizeof g.stats.upscale_note, "%s", why);
    };
    if (phys) { // Layer2: the destination and extent come from the same snapshot
        up.dest = 0;
        up.dest_sub = phys->output_sub;
        up.render_w = phys->render_w;
        up.render_h = phys->render_h;
    }
    if (phys || up.dest) {
        if (phys) dest = phys->output;
        else dest.Attach(na::observe::AcquireResource(up.dest));
        const uint32_t erw = up.render_w ? up.render_w : w, erh = up.render_h ? up.render_h : h;
        if (!dest) refuse_upscale("Upscale destination is not available.");
        else if (color_msaa) refuse_upscale("Upscaling from an MSAA scene colour is not supported.");
        else if (erw > w || erh > h) refuse_upscale("The render extent is larger than the scene colour.");
        else {
            const D3D12_RESOURCE_DESC od = dest->GetDesc();
            const uint32_t omips = od.MipLevels ? od.MipLevels : 1u;
            const uint32_t omip = up.dest_sub % omips;
            const uint32_t dw = (std::max)(1u, static_cast<uint32_t>(od.Width >> omip)), dh = (std::max)(1u, od.Height >> omip);
            if (od.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || od.SampleDesc.Count != 1 ||
                up.dest_sub >= omips * od.DepthOrArraySize)
                refuse_upscale("The upscale destination is not a single-sample 2D texture.");
            else if (typed(od.Format) != cf)
                refuse_upscale("The upscale destination's format differs from the scene colour (no conversion yet).");
            else if (dw < erw || dh < erh)
                refuse_upscale("The upscale destination is smaller than the render extent.");
            else {
                rw = erw; rh = erh; ow = dw; oh = dh; dsub_out = up.dest_sub;
            }
        }
    }
    if (phys) {
        // A Layer2 frame never falls back to DLAA: that would write into its private companion.
        if (!dest) return phys_fail(g.stats.upscale_note[0] ? g.stats.upscale_note : "Layer2: the physical output cannot be used.");
        sdest = phys->output_state;
    } else {
        if (dest && up.dest == color_id) refuse_upscale("The upscale destination is the scene colour itself.");
        // The destination may be untouched by the game until after J2's insertion: then the
        // state all submitted work left it in.
        if (dest && !state_of(up.dest, dsub_out, &sdest) && !na::observe::KnownSubStateNow(up.dest, dsub_out, &sdest)) refuse_upscale("Waiting: the upscale destination's state is not established.");
    }
    const bool upscaling = dest != nullptr;

    // Camera first (nothing is allocated for a frame that cannot run); the tracker
    // update comes after the size check, because a resize resets the tracker.
    na::camera::Matrix vp{};
    bool reversed = false;
    bool held = false, from_j1 = false;
    if (saved) { vp = saved->vp; reversed = saved->reversed; }
    const bool found = saved || find_camera(lists, scene, depth_id, &vp, &reversed);
    if (!found && g.jit.valid && g.jit.applied && g.cam_f1) {
        // Astra (COMMS 2026-11-02): the scene list's own blocks may not hold the camera, but
        // J1 found this frame's camera in an earlier batch and rewrote it with this sample:
        // that is the camera the frame was rendered with, and its jitter is known.
        const auto L = g.jit.layout ? na::camera::Layout::ColumnMajor : na::camera::Layout::RowMajor;
        const std::vector<uint8_t> jb(g.jit.jvp[g.jit.layout], g.jit.jvp[g.jit.layout] + 64);
        const na::camera::Matrix m = na::camera::LoadMatrix(jb, 0, L);
        na::camera::Candidate c{};
        if (na::camera::ViewProjection(m, c)) {
            vp = m;
            reversed = g.cam_rev;
            from_j1 = true;
            ++g.stats.camera_from_j1;
        }
    }
    if (!found && !from_j1) {
        // A camera read within the last few frames: predict this frame's (unjittered,
        // so no jitter is claimed for it) rather than leave the game's raw picture showing.
        // A still camera (the last two reads identical: SE5's menu) holds much longer; a
        // moving one stays within the tracker's gap limit. The same frame may also reuse
        // its own camera (an earlier attempt read it but did not run).
        constexpr uint64_t kMaxHeldFrames = 4, kMaxStillHeldFrames = 120;
        bool still = g.cam_f0 != 0;
        for (size_t i = 0; still && i < 16; ++i)
            still = std::fabs(g.cam_u0[i] - g.cam_u1[i]) <= 1e-7 * (std::max)(1.0, std::fabs(g.cam_u1[i]));
        const uint64_t limit = still ? kMaxStillHeldFrames : kMaxHeldFrames;
        const bool fresh = g.cam_f1 && g.frame >= g.cam_f1 && g.frame - g.cam_f1 <= limit;
        if (fresh && !g.resume_reset) {
            const bool velocity = g.cam_f0 && g.cam_f1 == g.cam_f0 + 1 && g.frame == g.cam_f1 + 1;
            vp = velocity ? na::camera::ExtrapolateViewProjection(g.cam_u0, g.cam_u1) : g.cam_u1;
            reversed = g.cam_rev;
            held = true;
            ++g.stats.camera_held;
            if (velocity) ++g.stats.camera_extrapolated;
        }
    }
    if (!found && !held && !from_j1) {
        ++g.stats.no_camera;
        g.camera_offset = g.camera_param = -1; // relearn
        char why[160];
        std::snprintf(why, sizeof why, "Waiting for the camera (lists %zu, binds %zu, blocks %zu, read %zu, matrices %zu, view-proj %zu).",
                      g_cd.lists, g_cd.binds, g_cd.blocks, g_cd.read, g_cd.cands, g_cd.vp);
        reason(why);
        return false;
    }
    if (!held) remember_main_camera(vp); // J1: the main camera, for earlier batches next frame
    // Ring slot (never wait) and size/format changes (only once the GPU is done with the old set).
    const UINT64 done = g.fence->GetCompletedValue();
    Slot& s = g.slots[g.slot];
    if (done == UINT64_MAX) { reason("Device removed."); return false; }
    if (s.fence_value && done < s.fence_value) { ++g.stats.busy; reason("Skipped: the previous use of this frame slot is still running."); return false; }
    // Astra (COMMS 2026-11-02): J1 jitters the camera with the extent known at that moment
    // (g.rw/g.rh), which a render-scale change only updates below. The frame was rasterised with
    // a clip offset of 2*px/old_extent, i.e. px*new/old pixels at the new extent: rescale it.
    const uint32_t jit_rw = g.rw, jit_rh = g.rh;
    const bool resize = !g.output || !g.color_in || g.w != w || g.h != h || g.color_format != cf ||
                        g.rw != rw || g.rh != rh || g.ow != ow || g.oh != oh ||
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
            !texture(ow, oh, cf, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g.output) ||
            !texture(w, h, cf, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, g.color_in) ||
            !texture(ow, oh, cf, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g.sharp)) {
            g.output.Reset();
            reason("Could not allocate the DLAA working textures.");
            return false;
        }
        g.w = w; g.h = h; g.color_format = cf;
        g.rw = rw; g.rh = rh; g.ow = ow; g.oh = oh;
        g.samples = cd.SampleDesc.Count; g.sample_quality = cd.SampleDesc.Quality;
        g.depth_samples = dd.SampleDesc.Count; g.depth_quality = dd.SampleDesc.Quality; g.depth_format = dd.Format;
        g.resume_reset = true; // remains pending if this recording is not submitted
    }
    const double zero[2] = {0, 0};
    const double* injected = held ? zero : from_j1 ? g.jit.px : saved ? saved->jitter : g_injected ? g.jit.px : zero; // sample belongs to the captured camera
    double rescaled[2] = {0, 0};
    if (injected != zero && jit_rw && jit_rh && (jit_rw != rw || jit_rh != rh)) {
        rescaled[0] = injected[0] * double(rw) / double(jit_rw);
        rescaled[1] = injected[1] * double(rh) / double(jit_rh);
        injected = rescaled;
        ++g.stats.jitter_extent_rescaled;
    }
    if (!saved && !held && !from_j1 && !g_injected && g.jitter_on) { // a DLAA frame without J1's jitter: why
        if (!g.jit.valid) ++g.stats.unjit_no_sample;
        else if (!g.jit.applied) ++g.stats.unjit_not_applied;
        else ++g.stats.unjit_not_held;
    }
    pending.tracker = g.tracker;
    // DLSS's history is the last frame it evaluated, and the tracker's previous
    // camera is that same frame, so motion stays correct across a few skipped game
    // frames (a frame whose scene submissions differed): no reset, no visible
    // restart. A long gap (or resume/resize) starts over.
    constexpr uint64_t kMaxGapFrames = 4;
    const bool tracker_restart = g.resume_reset || (g.submitted_frame && g.frame > g.submitted_frame + kMaxGapFrames);
    if (tracker_restart) pending.tracker.Reset();
    const na::camera::CameraFrame cam = pending.tracker.Update(vp, g.frame, rw, rh, injected); // active extent
    if (!cam.valid) { ++g.stats.no_camera; reason("The camera matrix could not be inverted."); return false; }
    if (!held) { // the newest read camera (one per frame: a later attempt in the same frame replaces it)
        if (g.cam_f1 != g.frame) { g.cam_u0 = g.cam_u1; g.cam_f0 = g.cam_f1; }
        g.cam_u1 = cam.unjittered; g.cam_f1 = g.frame; g.cam_rev = reversed;
    }
    pending.cut = cam.camera_cut;
    if (cam.camera_cut) { // Cort 2026-11-02: an on/off blink every ~30 s matched cuts while standing still: record why
        static const char* const kWhy[] = {"none", "no_history", "gap", "translation", "rotation", "fov", "origin"};
        const char* why = tracker_restart ? (g.resume_reset ? "restart_resume" : "restart_gap") : kWhy[cam.cut_reason < 7 ? cam.cut_reason : 0];
        char one[120];
        std::snprintf(one, sizeof one, "%llu:%s=%.3g%s%s ", static_cast<unsigned long long>(GetTickCount64() / 100 % 100000000),
                      why, cam.cut_value, held ? "/held" : "", from_j1 ? "/j1" : "");
        // Newest first, keep what fits.
        char merged[sizeof g.stats.recent_cuts];
        std::snprintf(merged, sizeof merged, "%s%s", one, g.stats.recent_cuts);
        std::memcpy(g.stats.recent_cuts, merged, sizeof merged);
        if (tracker_restart) ++g.stats.cut_restarts;
        else if (cam.cut_reason == 2) ++g.stats.cut_gap;
        else if (cam.cut_reason >= 3 && cam.cut_reason <= 5) ++g.stats.cut_motion;
    }
    // P4-0b: moving-object motion (between-lists route only). The pre list is
    // submitted right before this list; the flow pairs with the frame the camera
    // history pairs with. Any failure leaves this frame camera-only.
    // Flow works on the whole colour at DLAA size; upscaled frames stay camera-only for now.
    if (flow_ticket && !inl && !color_msaa && !upscaling && na::j2::flow::Enabled()) {
        na::j2::flow::Source fs;
        fs.color = color.Get();
        fs.subresource = csub;
        fs.state = sc;
        fs.format = cf;
        fs.width = w;
        fs.height = h;
        fs.frame = g.frame;
        fs.previous_frame = g.submitted_frame;
        fs.generation = g.route_gen_applied;
        fs.reset = tracker_restart || cam.camera_cut || resize;
        fs.allow_pair = flow_pair_ok;
        fs.jitter_px[0] = static_cast<float>(cam.jitter_px_x);
        fs.jitter_px[1] = static_cast<float>(cam.jitter_px_y);
        fs.slot = g.slot;
        *flow_ticket = na::j2::flow::Prepare(g.device.Get(), g.queue.Get(), fs);
    }
    // Layer2 payload lifetime, before any command references it: J2's own list keeps it in its
    // ring slot (released when the slot is reused, after its fence); a game list retains it
    // through every submission (the existing recording retention). No retention = no use.
    if (phys && inl) {
        const auto lease = na::nr::native::RetainRecording(inl, std::shared_ptr<void>(std::const_pointer_cast<PhysicalInput>(phys)));
        if (!lease) return phys_fail("Layer2: the game list's recording could not retain the physical inputs.");
    }
    if (!inl) s.physical = phys; // the previous use of this slot has completed (checked above)
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
        // AMD is an in-list dispatch, after this frame's resolved pre-DLAA
        // colour/depth copies. It needs no batch split or external queue wait.
        if(na::j2::flow::Enabled() && (inl || !flow_ticket || !flow_ticket->valid)){
            na::j2::compute_flow::Frame frame;frame.frame=g.frame;frame.previous=g.submitted_frame;
            frame.generation=g.router.Current().history_generation;frame.slot=g.slot;
            frame.reset=cam.camera_cut || resize || g.resume_reset;frame.hdr=is_float_color(cf);frame.speculative=inl!=nullptr; // inline: one of the predicated candidates
            frame.jitter[0]=in.jitter_current_px[0];frame.jitter[1]=in.jitter_current_px[1];
            na::j2::compute_flow::Tier3 tier;
            pending.compute_flow=na::j2::compute_flow::Record(l,g.queue.Get(),g.color_in.Get(),g.depth32.Get(),in,frame,tier);
            if(tier.residual){in.tier3_valid_flag=true;in.tier3_residual=tier.residual;in.tier3_valid=tier.valid;in.tier3_mask=tier.mask;}
        }
        // P4-0b: camera + accepted moving-object residual (tier 3) when this frame's pair ran.
        na::j2::flow::Tier3 t3;
        if (flow_ticket && flow_ticket->pair && na::j2::flow::RecordFusion(l, *flow_ticket, g.depth32.Get(), in, &t3)) {
            in.tier3_residual = t3.residual;
            in.tier3_mask = t3.mask;
            in.tier3_valid = t3.valid;
            in.tier3_valid_flag = true;
        }
        na::motion::gpu::Outputs o{};
        o.xy = g.motion.Get();
        o.provenance = g.provenance.Get();
        o.width = rw; // the active extent (top left); render pixels
        o.height = rh;
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
    e.render_w = e.max_render_w = rw; // active extent at the top left of the inputs
    e.render_h = e.max_render_h = rh;
    e.out_w = ow;
    e.out_h = oh;
    e.quality = na::dlss::QualityForScale(rw, rh, ow, oh); // DLAA when equal
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
            b.push_back(uav_barrier(g.output.Get()));
            add(b, g.output.Get(), UAV, NPSR);
            flush(l, b);
            if (rcas_record(l, pct, cf)) {
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
    // 5. result -> scene colour (DLAA) or the upscale destination, everything back to the game's states
    if (evaluated && upscaling) {
        b.push_back(uav_barrier(g.output.Get()));
        add(b, g.output.Get(), UAV, SRC);
        addsub(b, color.Get(), csub, color_source, sc); // the game's scene colour is left as drawn
        addsub(b, dest.Get(), dsub_out, sdest, DST);
        flush(l, b);
        D3D12_TEXTURE_COPY_LOCATION cs{}, cdst{};
        cs.Type = cdst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        cs.pResource = g.output.Get();
        cdst.pResource = dest.Get();
        cdst.SubresourceIndex = dsub_out;
        const D3D12_BOX box{0, 0, 0, ow, oh, 1};
        l->CopyTextureRegion(&cdst, 0, 0, 0, &cs, &box);
        add(b, g.output.Get(), SRC, UAV);
        addsub(b, dest.Get(), dsub_out, DST, sdest);
    } else if (evaluated) {
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
    g.stats.render_w = rw;
    g.stats.render_h = rh;
    g.stats.out_w = ow;
    g.stats.out_h = oh;
    g.stats.jitter_phases = jitter_phases();
    if (upscaling) { ++g.stats.upscaled_frames; g.stats.upscale_note[0] = 0; }
    if (phys) phys_report.result = PhysicalOutcome::Evaluated;
    *out_list = l;
    return true;
}

// Inline insertion runs immediately before a draw/dispatch whose bound SRVs
// include scene colour. The consuming PSO/targets are now established; bindings
// alone are not reads. Prior recordings teach the first use after the final
// scene write. The host restores game state before forwarding the real call.
void on_table_bind(ID3D12GraphicsCommandList* l, const na_object_id* srvs, uint32_t n, bool compute)
{
    if (!na::settings::EffectsReady() || !g.enabled.load(std::memory_order_relaxed) || g.paused.load(std::memory_order_relaxed)) return;
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
    if (g.submitted_frame == g.frame) return; // a placement path already owns this frame
    const auto current = na::observe::ListInsertionDecision(l, color_id);
    // A later possible end in the SAME recording as this frame's first candidate: more scene
    // content was drawn since the previous candidate, so the previous one may have been too
    // early. Record another (predicated) candidate; submission picks the exact one.
    const bool later = g.inl_n && g.inl[0].frame == g.frame;
    if (later) {
        if (g.inl[0].list != l || g.inl[0].rec != rec) return; // one scene recording per frame
        if (current.writes <= g.inl[g.inl_n - 1].writes) return; // nothing new since the last candidate
        if (g.inl_n == kMaxCandidates) { ++g.stats.inline_candidates_full; return; }
    } else if (g.inl_frame == g.frame) return; // this frame's candidates were already released
    const auto& learned = g.insertion_learned;
    // Role-class trigger (placement design): at the ACTUAL consumer (draw/dispatch/copy),
    // fire when its class is accepted by the profile. A class known to be a temporary
    // reader blocks the boundary matcher too (a fact from earlier frames). Otherwise the
    // boundary matcher below decides exactly as before, so coverage never drops below it.
    enum : uint32_t { kByClass = 1, kByPair = 2, kByBarrier = 3, kByLegacy = 4, kByBoundaryAny = 7 };
    uint32_t matched_mode = kByLegacy;
    bool by_class = false;
    na::j2::ClassKey live_class;
    uint64_t live_producer = 0, live_context = 0;
    bool fired_at_barrier = false; // the event being judged is a barrier (whichever trigger fires)
    {
        const auto live = na::observe::ListLiveConsumer(l, color_id);
        // An MSAA depth resolve draws (render target, viewport, topology), which the state
        // restore does not cover: graphics consumers with MSAA depth stay on the boundary matcher.
        const bool raster_safe = live.op != na::j2::Op::GraphicsRead || g.depth_samples <= 1;
        if (live.barrier && live.after_write) {
            // Qualified scene-ending pattern: the drawing pipeline of the last write before
            // this barrier, plus (when that pipeline also appears mid-scene) the class of
            // the latest consumer earlier in this recording. Accepted -> fire here (boundary
            // matcher timing); known mid-scene -> veto; seen in both roles and not separated
            // by the context -> the boundary matcher is NOT allowed to fire here either; a
            // qualified consumer later in this recording may still take the frame.
            live_class.op = na::j2::Op::Barrier;
            live_producer = live.producer_pso;
            live_context = live.has_prior && g.scene_w ? na::j2::ClassHash(na::j2::MakeClass(live.prior, g.scene_w, g.scene_h)) : 0;
            fired_at_barrier = true;
            using V = na::j2::ConsumerProfile::Verdict;
            const V v = g.profile.Judge(live_class, live_producer, live_context);
            // A learned mid-scene read cannot START a frame's candidates; a later candidate is
            // recorded anyway (submission decides with the complete list, not the profile).
            if (v == V::Rejected && !later) { ++g.stats.barrier_vetoes; return; }
            // Ambiguous (the pattern was seen both mid-scene and at completion, and the
            // context does not separate them): the boundary matcher below is the
            // distinguishing check: it fires only when the writer pipeline, its
            // occurrence, destination and pipeline all equal the learned boundary. A
            // wrong fire is still a contradiction that demotes this pattern
            // (fired_at_barrier). An ambiguous pattern is not completion proof;
            // the between-list consumer path can also recover a missed boundary.
            if (v == V::Ambiguous) ++g.stats.barrier_ambiguous_skips;
            if (v == V::AcceptedWithProducer || v == V::AcceptedQualified) { by_class = true; matched_mode = kByBarrier; }
        } else if (live.valid && live.after_write && raster_safe && g.scene_w) {
            live_class = na::j2::MakeClass(live.raw, g.scene_w, g.scene_h);
            live_producer = live.producer_pso;
            using V = na::j2::ConsumerProfile::Verdict;
            const V v = g.profile.Judge(live_class, live_producer);
            if (v == V::Rejected && !later) { ++g.stats.class_rejected_skips; return; }
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
    if (later) {
        // A later candidate needs no trigger match: submission decides with the complete list.
    } else if (by_class) {
        if (g.inl_n) {
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
    // Exact placement: the writer and its occurrence only predicted WHICH boundary of the
    // scene recording is the last one, and panning changes both (SE5 pan run: 41 early and
    // 55 missed frames, rejections writer +1165, occurrence +998). Every boundary of the
    // scene recording now gets a candidate and submission picks the real one, so they are
    // recorded (not rejected) here; the consumer's destination and pipeline still must match.
    bool relaxed = false;
    if (rej == S::kRejWriter || rej == S::kRejOccurrence || rej == S::kRejTransition || rej == S::kRejCandidate) {
        ++g.stats.inline_relaxed_rejects[rej == S::kRejWriter ? 0 : rej == S::kRejOccurrence ? 1 : 2];
        rej = S::kRejCount;
        relaxed = true;
    }
    if (rej == S::kRejCount && current.destination != learned.decision.destination) rej = S::kRejDestination;
    if (rej == S::kRejCount && current.pipeline != learned.decision.pipeline) rej = S::kRejPipeline;
    if (rej == S::kRejCount && relaxed) matched_mode = kByBoundaryAny;
    if (rej == S::kRejCount) g.stats.inline_target = learned.decision.final_candidate;
    if (rej == S::kRejCount && g.inl_n) rej = S::kRejPending; // the previous recording was never submitted (a real stall)
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
    if (!na::observe::ListStateRestorable(l)) { ++g.stats.inline_refused; reason("The scene list's state cannot be restored after DLAA here (a bundle ran, inside a render pass, or the game's own predication is active); left untouched."); return; }
    if (!g.pred_cpu) return;
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
    // The predicate of a slot is read by its previous submission until that completes.
    if (g.slots[slot].fence_value && g.fence->GetCompletedValue() < g.slots[slot].fence_value) {
        ++g.stats.busy;
        reason("Skipped: the previous use of this frame slot is still running.");
        return;
    }
    // A later candidate exists only because the scene was written after every earlier one,
    // so none of those can be chosen any more (choose_inline): their capture slots go back.
    if (later)
        for (uint32_t k = 0; k < g.inl_n; ++k) { na::capture::j2::Withdraw(g.inl[k].history.capture); g.inl[k].history.capture = 0; }
    // Every command of this candidate runs only if submission switches its predicate on.
    // The game's own predication is not active here (ListStateRestorable), so clearing it after is exact.
    g.pred_cpu[slot] = 0;
    l->SetPredication(g.pred.Get(), static_cast<UINT64>(slot) * 8, D3D12_PREDICATION_OP_EQUAL_ZERO);
    const bool ok = record(one, 0, depth_id, color_id, &out, pending, l, &touched);
    l->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    if (touched && !na::observe::RestoreListState(l)) {
        // Checked as restorable above; this cannot normally happen. Stop for the session.
        g.latched = true;
        na::capture::j2::Discard(pending.capture, true);
        reason("Stopped: the game's list state could not be restored after inline DLAA.");
        return;
    }
    if (!ok) { na::capture::j2::Discard(pending.capture, true); return; } // the game's list still runs our copies
    g.slot = (g.slot + 1) % kRing;
    auto& c = g.inl[g.inl_n++];
    c = State::InlinePending{};
    c.list = l;
    c.rec = rec;
    c.frame = g.frame;
    c.slot = slot;
    c.writes = current.writes;
    c.color = color_id;
    c.history = pending;
    c.mode = later ? 6u : matched_mode; // 6: a later candidate (no trigger)
    c.by_class = !later && by_class;
    c.at_barrier = fired_at_barrier;
    c.cls = live_class;
    c.producer = live_producer;
    c.context = live_context;
    g.inl_frame = g.frame;
    if (later) { ++g.stats.inline_later_candidates; return; }
    ++g.stats.inline_recorded;
    if (matched_mode == kByClass) ++g.stats.class_matched;
    else if (matched_mode == kByPair) ++g.stats.class_pair_matched;
    else if (matched_mode == kByBarrier) ++g.stats.barrier_matched;
    else if (matched_mode == kByBoundaryAny) ++g.stats.boundary_any_matched;
    else ++g.stats.legacy_matched;
}

// Exact placement at submission: the first candidate after the recording's final colour
// write. None (the last write has no read after it in this list) -> the last candidate,
// which has the fewest late writes (the behaviour before exact placement).
uint32_t choose_inline(uint32_t total_writes, bool* exact)
{
    *exact = false;
    for (uint32_t i = 0; i < g.inl_n; ++i)
        if (g.inl[i].writes >= total_writes) { *exact = true; return i; }
    return g.inl_n - 1;
}

// Inline and between-list are complementary opportunities in the SAME frame.
// Only an actual, learned post-entry consumer can trigger this fallback. No
// previous-frame batch ordinal and no guess about the final drawing PSO is used.
int record_between_consumer(ID3D12CommandList* const* lists, UINT count,
                            uint64_t color, uint64_t depth,
                            ID3D12GraphicsCommandList** ours, PendingHistory& pending)
{
    auto& saved = g.waiting_scene;
    if (saved.frame != g.frame || saved.color != color || saved.depth != depth) saved = {};
    for (UINT i = 0; i < count; ++i) {
        if (!lists[i]) continue;
        std::vector<na::j2::ColorEvent> events;
        if (!na::observe::ListColorEvents(lists[i], color, events)) { saved = {}; continue; }
        bool writes = false, consumed = false;
        uint64_t producer = 0;
        for (const auto& e : events) {
            if (e.write) { writes = true; consumed = false; producer = e.producer; continue; }
            if (!na::j2::IsConsumer(e.op)) continue; // a barrier alone is not a consumer
            if (!writes && !consumed && saved.frame == g.frame) {
                const auto cls = na::j2::MakeClass(e.raw, g.scene_w, g.scene_h);
                const auto verdict = g.profile.Judge(cls, saved.producer);
                using V = na::j2::ConsumerProfile::Verdict;
                if ((verdict == V::Accepted || verdict == V::AcceptedWithProducer) &&
                    record(lists, 0, depth, color, ours, pending, nullptr, nullptr, &saved, i)) {
                    g.class_fired = true;
                    g.fired_mode = 5;
                    g.fired_barrier_event = false;
                    g.fired_class = cls;
                    g.fired_producer = saved.producer;
                    g.fired_context = 0;
                    saved = {};
                    return static_cast<int>(i);
                }
            }
            consumed = true;
            saved = {}; // never insert later after a consumer already carried the image onward
        }
        if (writes) {
            saved = {};
            // End-of-list metadata is useful only if its final image has not
            // already been consumed within that recording.
            if (!consumed && na::observe::ListRendersTo(lists[i], color, depth) &&
                na::observe::ListSceneSubresource(lists[i], color, depth, &saved.color_sub, &saved.depth_sub) &&
                find_camera(lists, i, depth, &saved.vp, &saved.reversed)) {
                saved.frame = g.frame; saved.color = color; saved.depth = depth; saved.producer = producer;
                saved.jitter[0] = g_injected ? g.jit.px[0] : 0;
                saved.jitter[1] = g_injected ? g.jit.px[1] : 0;
            }
        }
    }
    return -1;
}

} // namespace

void SetEnabled(bool on)
{
    g.enabled.store(on);
    g.configured.store(on);
    if (on) na::observe::SetTableBindCallback(&on_table_bind);
}
bool Enabled() { return na::settings::EffectsReady() && g.enabled.load(); }
PhysicalReport::~PhysicalReport()
{
    if (!armed) return;
    if (result == PhysicalOutcome::Evaluated) ++g.stats.physical_evaluated;
    else if (result == PhysicalOutcome::Unavailable) ++g.stats.physical_unavailable;
    else ++g.stats.physical_failed;
    if (p.outcome) p.outcome(p.ctx, generation, result, why[0] ? why : "J2 could not evaluate this frame (see its status)");
}
void SetPhysicalInputProvider(const PhysicalInputProvider& p) { std::lock_guard<std::mutex> lk(g_up_m); g_phys = p; }
void SetUpscaleTarget(const UpscaleTarget& t) { std::lock_guard<std::mutex> lk(g_up_m); g_up = t; }
UpscaleTarget GetUpscaleTarget() { std::lock_guard<std::mutex> lk(g_up_m); return g_up; }
uint32_t JitterPhasesFor(uint32_t render_w, uint32_t out_w)
{
    if (!render_w || out_w <= render_w) return kJitterPhases;
    const double r = static_cast<double>(out_w) / render_w;
    return static_cast<uint32_t>((std::min)(64.0, std::ceil(kJitterPhases * r * r)));
}
float UpscaleMipBias(uint32_t render_w, uint32_t out_w)
{
    if (!render_w || !out_w) return -1.0f;
    return static_cast<float>(std::log2(static_cast<double>(render_w) / out_w)) - 1.0f;
}
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
            if (!o.executed && g.stats.frames) { // a present without DLAA, once DLAA has run at all: why
                if (o.camera_missed) ++g.stats.miss_no_camera;
                else if (o.depth_state_missed) ++g.stats.miss_no_state;
                else if (g.inline_mode && g.inl_frame != g.frame) ++g.stats.miss_no_candidate;
                else ++g.stats.miss_other;
            }
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
            if (g.submitted_frame == g.frame && g.stats.inline_late_writes + g.stats.post_scene_batches > g.route_prev_late_class) note_late_frame();
            // A class-triggered DLAA followed by more scene content: demote that class now.
            if (g.class_fired && g.submitted_frame == g.frame && g.stats.inline_late_writes + g.stats.post_scene_batches > g.route_prev_late_class) {
                // The route tolerates an isolated misplacement; the pattern that caused it
                // does not keep its acceptance.
                g.profile.Contradict(g.fired_class, g.fired_producer, g.frame, g.fired_context);
                if (g.fired_class.op == na::j2::Op::Barrier) ++g.stats.barrier_contradictions;
                else ++g.stats.class_contradictions;
            }
            g.route_prev_late_class = g.stats.inline_late_writes + g.stats.post_scene_batches;
            g.class_fired = false;
            g.fired_mode = 0;
            g.fired_barrier_event = false;
            g.frame_seq.clear();
            g.frame_seq_complete = true;
            na::observe::ResetDerived();
            g.stats.profile_accepted = g.profile.AcceptedCount();
            g.stats.profile_rejected = g.profile.RejectedCount();
            g.stats.profile_ambiguous = g.profile.AmbiguousCount();
            g.stats.profile_entries = static_cast<uint32_t>(g.profile.Size());
            // An inline recording that was never submitted within two frames is released
            // (its capture slot retired; the frame already counted as missed).
            if (g.inl_n && g.frame >= g.inl[0].frame + 2) {
                discard_inline();
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
    if (!na::settings::EffectsReady() || !g.enabled.load(std::memory_order_relaxed) || g.paused.load(std::memory_order_relaxed) || !queue || !lists || !count) {
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
        if ((decision.writes || decision.all_reads) && g.frame_seq_complete) {
            // Placement profile: this list's writes and consumers, in submission order.
            // 512 events silently dropped later lists in the Sniper frames and
            // taught early barriers as final ones. Bound the complete frame, and
            // never train on an unmarked prefix when that bound is exceeded.
            if (!na::observe::ListColorEvents(lists[i], selected_color, g.frame_seq)) g.frame_seq_complete = false;
            constexpr size_t kMaxFrameEvents = 16384;
            if (g.frame_seq.size() > kMaxFrameEvents) {
                g.frame_seq.resize(kMaxFrameEvents);
                g.frame_seq_complete = false;
            }
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
                decision.writer_occurrence, g.inl_n && g.inl[0].list == lists[i] ? "*" : "");
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
        bool has = false;
        uint32_t chosen = 0;
        for (UINT i = 0; i < count; ++i) {
            if (!lists[i]) continue;
            const auto d = na::observe::ListInsertionDecision(lists[i], selected_color);
            if (g.inl_n && !has && lists[i] == g.inl[0].list && na::observe::ListRecording(lists[i]) == g.inl[0].rec) {
                // The whole recording is known now: switch on exactly one candidate, before forwarding.
                bool exact = false;
                chosen = choose_inline(d.writes, &exact);
                for (uint32_t k = 0; k < g.inl_n; ++k) g.pred_cpu[g.inl[k].slot] = k == chosen ? 1u : 0u;
                if (exact) ++g.stats.inline_exact;
                if (chosen) ++g.stats.inline_moved_later; // the first (trigger) candidate was too early
                g.stats.inline_last_count = g.inl_n;
                const auto& c = g.inl[chosen];
                if (d.writes > c.writes) { g.stats.inline_late_writes += d.writes - c.writes; note_late_writer(lists[i], selected_color, c.writes, 1, d.writes - c.writes); }
                has = after = true;
            } else if (after) { g.stats.inline_late_writes += d.writes; if (d.writes) note_late_writer(lists[i], selected_color, 0, 2, d.writes); }
        }
        // Inline mode: no J2 list is inserted; DLAA is already inside the game's list.
        // Its history is committed when that list is actually submitted.
        jitter_batch(lists, count);
        for (UINT i = 0; i < count; ++i)
            if (lists[i] && na::observe::ListReferences(lists[i], selected_depth) &&
                !na::observe::ListRootConstantBuffers(lists[i]).empty())
                g.inline_camera_submitted = true;
        if (!g.inl_n && g.submitted_frame != g.frame && g.queue.Get() == queue && g.dlss_ready &&
            selected_color && selected_depth && g.router.Mode() == na::j2::RouteMode::Auto) {
            ID3D12GraphicsCommandList* ours = nullptr;
            PendingHistory pending;
            const int before = record_between_consumer(lists, count, selected_color, selected_depth, &ours, pending);
            if (before >= 0) {
                std::vector<ID3D12CommandList*> batch(lists, lists + count);
                batch.insert(batch.begin() + before, ours);
                Slot& slot = g.slots[g.slot];
                g.slot = (g.slot + 1) % kRing;
                na::dlss::BeforeExecute(queue, static_cast<UINT>(batch.size()), batch.data());
                forward(queue, static_cast<UINT>(batch.size()), batch.data());
                na::dlss::OnExecuted(queue, static_cast<UINT>(batch.size()), batch.data());
                g.tracker = pending.tracker;
                g.submitted_frame = g.frame;
                g.resume_reset = false;
                g.stats.active = true;
                ++g.stats.frames;
                ++g.stats.between_observed;
                if (pending.cut) ++g.stats.cuts;
                for (UINT i = static_cast<UINT>(before); i < count; ++i)
                    if (lists[i]) g.stats.inline_late_writes += na::observe::ListInsertionDecision(lists[i], selected_color).writes;
                slot.fence_value = g.fence_next++;
                queue->Signal(g.fence.Get(), slot.fence_value);
                na::capture::j2::Submitted(pending.capture, queue, g.stats.submissions);
                na::j2::compute_flow::Committed(pending.compute_flow);
                reason("Live DLAA is running before a learned post-processing consumer in a separate list.");
                return;
            }
            na::capture::j2::Discard(pending.capture, false);
        }
        lock.unlock();
        forward(queue, count, lists);
        if (!has) return;
        lock.lock();
        if (chosen >= g.inl_n) { discard_inline(); return; } // released while unlocked (route change)
        auto& c = g.inl[chosen];
        g.tracker = c.history.tracker;
        g.submitted_frame = g.frame;
        g.resume_reset = false;
        if (c.history.cut) ++g.stats.cuts;
        ++g.stats.frames;
        ++g.stats.inline_frames;
        if (g.stats.inline_jitter_x != 0 || g.stats.inline_jitter_y != 0) ++g.stats.inline_jittered;
        g.stats.active = true;
        // Every candidate's commands (even switched off) and its predicate are in this submission.
        const UINT64 fv = g.fence_next++;
        for (uint32_t k = 0; k < g.inl_n; ++k) g.slots[g.inl[k].slot].fence_value = fv;
        queue->Signal(g.fence.Get(), fv);
        na::capture::j2::Submitted(c.history.capture, queue, g.stats.submissions);
        c.history.capture = 0;
        na::j2::compute_flow::Committed(c.history.compute_flow);
        // What placed the DLAA that ran (a later candidate was not placed by a trigger).
        g.class_fired = c.by_class || (c.at_barrier && c.mode != 6);
        g.fired_mode = c.mode;
        g.fired_barrier_event = c.at_barrier;
        g.fired_class = c.cls;
        g.fired_producer = c.producer;
        g.fired_context = c.context;
        discard_inline(true); // the others never ran (predicate 0 in this submission); their captures are free again
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
            if (scene_here && !restarts) {
                ++g.scene_batches_cur; ++g.stats.post_scene_batches;
                for (UINT i = 0; i < count; ++i)
                    if (lists[i] && na::observe::ListRendersTo(lists[i], csel, dsel)) { note_late_writer(lists[i], csel, 0, 3, 1); break; }
            }
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
    // P4-0b: the flow pair needs a GPU wait between our pre list and our DLAA list.
    // Without the diagnostic split switch that is only done when no game list follows
    // the scene list in this submission (the game's lists keep their grouping).
    na::j2::flow::Ticket flow_ticket;
    const bool flow_pair_ok = scene >= 0 && (static_cast<UINT>(scene) + 1 == count || na::j2::flow::AllowSplit());
    if (scene < 0) {
        ++g.stats.no_scene_list;
    } else if (wait_later) {
        ++g.stats.waited_batches;
    } else if (!ensure_device(queue)) {
        // reason set
    } else {
        if (!dlss_on()) {
            // reason set
        } else if (record(lists, static_cast<UINT>(scene), depth_id, color_id, &ours, pending, nullptr, nullptr, nullptr, 0,
                          &flow_ticket, flow_pair_ok)) {
            g.stats.later_passes = na::observe::ListTargetsAfter(lists[scene], color_id, depth_id);
        }
    }
    if (!wait_later) g.stats.active = ours != nullptr;
    if (scene >= 0 && !wait_later) g.jit.valid = false; // the scene closes this frame's jitter: the next camera block starts a new sample
    if (!ours) {
        na::capture::j2::Discard(pending.capture, false); // our list is reset before any GPU use
        na::j2::flow::Abandon(queue, flow_ticket);        // never leaves the flow engine waiting
        lock.unlock();
        forward(queue, count, lists);
        return;
    }
    if (before_reader) ++g.stats.between_observed; // an observed reader follows in this batch
    else ++g.stats.between_predicted;              // placed after a predicted last scene list
    std::vector<ID3D12CommandList*> batch(lists, lists + count);
    UINT ours_at = static_cast<UINT>(scene) + 1;
    if (flow_ticket.valid) batch.insert(batch.begin() + ours_at++, flow_ticket.pre); // P4-0b pre list, right before ours
    batch.insert(batch.begin() + ours_at, ours);
    const UINT n2 = static_cast<UINT>(batch.size());
    Slot& s = g.slots[g.slot];
    g.slot = (g.slot + 1) % kRing;
    na::dlss::BeforeExecute(queue, n2, batch.data());
    na::j2::flow::Submit(queue, flow_ticket, batch.data(), n2, ours_at, forward); // one forward unless a flow pair is consumed
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
    na::j2::flow::Committed(flow_ticket, g.fence.Get(), s.fence_value);
    na::j2::compute_flow::Committed(pending.compute_flow);
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
    na::j2::flow::Shutdown(); // before the device and J2's fence go
    na::j2::compute_flow::Reset();
    g.fence.Reset(); g.queue.Reset(); g.device.Reset();
    g.arbiter = na::motion::gpu::Arbiter{};
    g_dbg = DebugPass{};
    g_rcas = RcasPass{};
    g_nis.Reset();
    g.arbiter_ok = false;
    g.tracker.Reset();
    g.cam_f0 = g.cam_f1 = 0;
    g.depth_learned_streak = 0; g.depth_learned_frame = 0; g.depth_learned_id = 0; g.depth_learned_sub = 0xFFFFFFFFu;
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
    g.waiting_scene = {};
    g.jitter_writes.Clear();
    g.frame_seq.clear();
    g.frame_seq_complete = true;
    g.scene_color_id = 0;
    g.scene_w = g.scene_h = 0;
    g.class_fired = false;
    g.route_prev_late_class = 0;
    g.insertion_frame = g.insertion_learned = {};
    g.inl_n = 0;
    g.inl_frame = 0;
    if (g.pred) g.pred->Unmap(0, nullptr);
    g.pred.Reset();
    g.pred_cpu = nullptr;
    g.inline_sample_locked = false;
    g.inline_camera_submitted = false;
    g.resume_reset = false;
    g.w = g.h = 0;
    g.rw = g.rh = g.ow = g.oh = 0;
}

} // namespace na::j2
