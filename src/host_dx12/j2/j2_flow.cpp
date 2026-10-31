#include "host_dx12/j2/j2_flow.hpp"

#include "core/motion_arbiter_gpu.hpp"

#include "host_dx12/nvof/flow_expand.hpp"
#include "host_dx12/nvof/nvof_flow.hpp"
#include "host_dx12/settings.hpp"

#include <windows.h>
#include <wrl/client.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace na::j2::flow {
namespace {

constexpr uint32_t kInputs = 3;  // encoded images: current, previous, one retiring
constexpr uint32_t kGrids = 2;   // engine outputs (one being read while the other is written)
constexpr uint32_t kSlots = 8;   // >= J2's ring (6): descriptor blocks and read-back slots
constexpr uint32_t kPre = 8;     // pre-list allocators
constexpr uint32_t kCounters = 8; // per slot: accepted, invalid, small, cost, offscreen, image, cost_sum, cost_n
constexpr uint32_t kCostMax = 160;     // engine cost above this rejects the block (tuning target, reported as mean_cost)
constexpr float kSnapPx = 0.25f;       // same as the arbiter's snap: smaller residuals are noise
constexpr float kImageAbs = 0.08f;     // the flow must explain the image (mean luma error, 3x3 encoded texels)
constexpr float kImageMargin = 0.02f;  // and never clearly worse than the camera alone (flat areas: both explain it)

// ------------------------------------------------------------------ shaders
const char kShader[] = R"(
Texture2D<float4> t_color : register(t0);
Texture2D<float2> t_raw   : register(t1);
Texture2D<float2> t_res   : register(t2);
Texture2D<float>  t_valid : register(t3);
Texture2D<uint>   t_cost  : register(t4);
Texture2D<float4> t_cur   : register(t5);
Texture2D<float4> t_prev  : register(t6);
RWTexture2D<float4> u_enc : register(u0);
RWTexture2D<float>  u_mask : register(u1);
RWByteAddressBuffer u_cnt : register(u2);
SamplerState s_lin : register(s0);
cbuffer C : register(b0) {
    uint w, h, ew, eh;
    uint mode, block, has_cost, cost_max;
    uint cnt_off, gw, gh, pad0;
    float snap_px, image_margin, image_abs, pad1;
};
float3 srgb_encode(float3 v) { v = saturate(v); return v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1.0 / 2.4) - 0.055; }
// One fixed transform for every image (both images of a pair are encoded alike).
[numthreads(8, 8, 1)]
void encode(uint3 id : SV_DispatchThreadID) {
    if (id.x >= ew || id.y >= eh) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(ew, eh);
    float3 c = t_color.SampleLevel(s_lin, uv, 0).rgb;
    if (any(isnan(c)) || any(isinf(c))) c = float3(0, 0, 0);
    if (mode == 1) { c = max(c, 0.0); c = srgb_encode(c / (1.0 + c)); }
    else c = saturate(c);
    u_enc[id.xy] = float4(c, 1.0); // UAV format B8G8R8A8_UNORM: the view swizzles
}
float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
// Mean luma difference over 3x3 encoded texels between the current image at p
// and the previous image at p + v (v = p_previous - p_current, full-size pixels).
float image_error(float2 p, float2 v) {
    float step_x = float(w) / float(ew), step_y = float(h) / float(eh);
    float e = 0;
    [unroll] for (int j = -1; j <= 1; ++j) {
        [unroll] for (int i = -1; i <= 1; ++i) {
            float2 o = float2(i * step_x, j * step_y);
            float2 uc = (p + o) / float2(w, h);
            float2 up = (p + o + v) / float2(w, h);
            e += abs(luma(t_cur.SampleLevel(s_lin, uc, 0).rgb) - luma(t_prev.SampleLevel(s_lin, up, 0).rgb));
        }
    }
    return e / 9.0;
}
groupshared uint gs[8];
[numthreads(8, 8, 1)]
void accept(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    if (gi < 8) gs[gi] = 0;
    GroupMemoryBarrierWithGroupSync();
    if (id.x < w && id.y < h) {
        uint why = 0; // 0 accepted, 1 invalid, 2 small, 3 cost, 4 offscreen, 5 image
        uint cost = 0;
        bool costed = false;
        float v = t_valid[id.xy];
        float2 r = t_res[id.xy];
        float2 raw = t_raw[id.xy];
        if (!(v > 0.0 && v <= 1.0) || any(isnan(r)) || any(isnan(raw)) || any(isinf(r)) || any(isinf(raw))) why = 1;
        else if (length(r) < snap_px) why = 2;
        if (why == 0 && has_cost != 0) {
            uint2 b = min(id.xy / block, uint2(gw - 1, gh - 1));
            cost = t_cost[b];
            costed = true;
            if (cost > cost_max) why = 3;
        }
        if (why == 0) {
            float2 p = float2(id.xy) + 0.5;
            float2 q = p + raw;
            if (q.x < 0.0 || q.y < 0.0 || q.x > float(w) || q.y > float(h)) why = 4;
            else {
                float e_flow = image_error(p, raw);
                float e_cam = image_error(p, raw - r);
                // Rejected when the image contradicts the flow; accepted where the image
                // cannot tell (flat interiors of moving objects), cost permitting.
                if (!(e_flow <= image_abs && e_flow <= e_cam + image_margin)) why = 5;
            }
        }
        u_mask[id.xy] = why == 0 ? 1.0 : 0.0;
        InterlockedAdd(gs[why], 1u);
        if (costed) { InterlockedAdd(gs[6], cost); InterlockedAdd(gs[7], 1u); }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi < 8 && gs[gi] != 0) u_cnt.InterlockedAdd(cnt_off + gi * 4, gs[gi]);
}
)";

struct Constants {
    uint32_t w, h, ew, eh;
    uint32_t mode, block, has_cost, cost_max;
    uint32_t cnt_off, gw, gh, pad0;
    float snap_px, image_margin, image_abs, pad1;
};
constexpr UINT kConstantWords = static_cast<UINT>(sizeof(Constants) / 4);

// ------------------------------------------------------------------ engine
class NvofEngine final : public Engine {
public:
    bool Available(ID3D12Device* device, char* error, size_t cap) override
    {
        na::nvof::Caps caps;
        return na::nvof::QueryCaps(device, &caps, error, cap) && caps.available;
    }
    bool Init(ID3D12Device* device, uint32_t w, uint32_t h, uint32_t grid, uint32_t perf, bool cost) override
    {
        return f_.Init(device, w, h, grid, perf, cost);
    }
    bool CostEnabled() const override { return f_.CostEnabled(); }
    uint32_t OutWidth() const override { return f_.OutWidth(); }
    uint32_t OutHeight() const override { return f_.OutHeight(); }
    bool RegisterInput(ID3D12Resource* r, ID3D12Fence* f, uint64_t wv, uint64_t sv, void** out) override
    {
        return f_.RegisterInput(r, f, wv, sv, out);
    }
    bool RegisterOutput(ID3D12Resource* r, ID3D12Fence* f, uint64_t wv, uint64_t sv, void** out) override
    {
        return f_.RegisterOutput(r, f, wv, sv, out);
    }
    bool RegisterCost(ID3D12Resource* r, ID3D12Fence* f, uint64_t wv, uint64_t sv, void** out) override
    {
        return f_.RegisterCost(r, f, wv, sv, out);
    }
    bool Unregister(void* hb) override { return f_.Unregister(hb); }
    uint32_t Outstanding() const override { return f_.OutstandingRegistrations(); }
    bool Execute(void* cur, void* prev, void* grid, void* cost, ID3D12Fence* wf, uint64_t wv, ID3D12Fence* sf, uint64_t sv) override
    {
        return f_.Execute(cur, prev, grid, wf, wv, sf, sv, true, cost);
    }
    bool Shutdown() override { return f_.Shutdown(); }
    const char* LastError() const override { return f_.LastError(); }

private:
    na::nvof::Flow f_;
};

// ------------------------------------------------------------------ state
struct Image {
    ComPtr<ID3D12Resource> tex; // BGRA8, kCommon at rest
    void* handle = nullptr;
    uint64_t last_of = 0;       // engine job that last read it
    uint64_t last_j2 = 0;       // J2 list that last read it
    bool committed = false;     // holds a submitted frame's image
    uint64_t frame = 0;
    uint32_t generation = 0;
    float jitter[2] = {0, 0};
};
struct Grid {
    ComPtr<ID3D12Resource> tex, cost; // R16G16_SINT / R8_UINT, kCommon at rest
    void* handle = nullptr;
    void* cost_handle = nullptr;
    uint64_t last_of = 0, last_j2 = 0;
};
struct Pre {
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    uint64_t ready = 0; // ready value signalled behind its last submission
};
// An engine job: its input fence value (the pre list finished) and its own value.
// The watchdog's clock starts only once the input is ready on the GPU, so a busy
// game queue (loading, a long frame) never counts against the engine.
struct Outstanding {
    uint64_t value = 0, ready = 0;
    ULONGLONG started = 0; // 0 until `ready` was observed complete
};

struct State {
    std::mutex m;
    std::atomic<bool> enabled{false}, allow_split{false};
    Engine* engine = nullptr;
    NvofEngine nvof;
    bool latched = false, available_known = false, available = false;
    char reason[160] = "Off.";
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> ready, done; // ready: pre lists finished; done: engine finished
    uint64_t ready_next = 1, done_next = 1;
    ComPtr<ID3D12Fence> j2_fence;
    // Programs.
    bool programs = false;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso_encode, pso_accept;
    ComPtr<ID3D12DescriptorHeap> heap;
    UINT stride = 0;
    ComPtr<ID3D12Resource> dummy_uint; // R8_UINT 1x1 (no cost)
    na::nvof::FlowExpand expand;
    na::motion::gpu::Arbiter derive; // its own descriptor heap: never shares J2's arbiter slots
    ComPtr<ID3D12Resource> counters, zero, readback; // raw UAV buffer, zero source, read-back (mapped)
    uint8_t* readback_ptr = nullptr;
    ComPtr<ID3D12QueryHeap> timestamps;
    double ts_freq = 0;
    uint64_t slot_j2[kSlots] = {};   // J2 fence value whose completion makes the slot's read-back valid
    bool slot_has_ts[kSlots] = {};
    // Size-dependent set (epoch).
    uint32_t epoch = 0;
    bool built = false;
    uint32_t w = 0, h = 0, ew = 0, eh = 0, grid_n = 4, perf = 5, ds = 2;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D12Resource> color_copy;                          // COPY_DEST at rest
    ComPtr<ID3D12Resource> raw, expand_valid, residual, valid, mask; // NON_PIXEL_SHADER_RESOURCE at rest
    Image img[kInputs];
    Grid grids[kGrids];
    uint32_t next_grid = 0;
    int last_committed = -1;
    Pre pre[kPre];
    uint32_t pre_next = 0;
    // Watchdog.
    Outstanding jobs[16];
    bool watchdog_started = false;
    std::atomic<uint32_t> watchdog_ms{1000};
    Stats stats;
};
State& g = *new State; // never destroyed (process exit order)

void set_reason(const char* r) { std::snprintf(g.reason, sizeof g.reason, "%s", r); }
void latch(const char* r)
{
    set_reason(r);
    g.latched = true;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b, UINT sub = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
{
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r;
    x.Transition.Subresource = sub;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    return x;
}
D3D12_RESOURCE_BARRIER uav(ID3D12Resource* r)
{
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    x.UAV.pResource = r;
    return x;
}
constexpr D3D12_RESOURCE_STATES NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr D3D12_RESOURCE_STATES UAVS = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr D3D12_RESOURCE_STATES kCommon = D3D12_RESOURCE_STATE_COMMON;

bool texture(uint32_t w, uint32_t h, DXGI_FORMAT f, bool uav_flag, D3D12_RESOURCE_STATES st, ComPtr<ID3D12Resource>& out)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = f;
    d.SampleDesc.Count = 1;
    d.Flags = uav_flag ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    out.Reset();
    return SUCCEEDED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, IID_PPV_ARGS(&out)));
}
bool buffer(uint64_t bytes, D3D12_HEAP_TYPE type, bool uav_flag, D3D12_RESOURCE_STATES st, ComPtr<ID3D12Resource>& out)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = type;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = uav_flag ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    out.Reset();
    return SUCCEEDED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, IID_PPV_ARGS(&out)));
}
bool typed_uav_store(DXGI_FORMAT f)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{f};
    return SUCCEEDED(g.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, static_cast<UINT>(sizeof fs))) &&
           (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
}
bool is_float(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R11G11B10_FLOAT: case DXGI_FORMAT_R9G9B9E5_SHAREDEXP: case DXGI_FORMAT_R16G16_FLOAT:
        return true;
    default:
        return false;
    }
}

bool build_programs()
{
    if (g.programs) return true;
    ComPtr<ID3DBlob> cs_encode, cs_accept, err;
    if (FAILED(D3DCompile(kShader, sizeof kShader - 1, "j2_flow", nullptr, nullptr, "encode", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs_encode, &err)) ||
        FAILED(D3DCompile(kShader, sizeof kShader - 1, "j2_flow", nullptr, nullptr, "accept", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs_accept, &err)))
        return false;
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 7;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 3;
    ranges[1].OffsetInDescriptorsFromTableStart = 7;
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = kConstantWords;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC smp{};
    smp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    smp.MaxLOD = D3D12_FLOAT32_MAX;
    smp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 2;
    rd.pParameters = params;
    rd.NumStaticSamplers = 1;
    rd.pStaticSamplers = &smp;
    ComPtr<ID3DBlob> blob;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
        FAILED(g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g.root))))
        return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g.root.Get();
    pd.CS = {cs_encode->GetBufferPointer(), cs_encode->GetBufferSize()};
    if (FAILED(g.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g.pso_encode)))) return false;
    pd.CS = {cs_accept->GetBufferPointer(), cs_accept->GetBufferSize()};
    if (FAILED(g.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g.pso_accept)))) return false;
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kSlots * 2 * 10; // per slot: encode table, accept table
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.heap)))) return false;
    g.stride = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (!texture(1, 1, DXGI_FORMAT_R8_UINT, false, NPSR, g.dummy_uint)) return false;
    if (!g.expand.Init(g.device.Get(), kSlots) || !g.derive.Init(g.device.Get(), kSlots)) return false;
    if (!buffer(kSlots * kCounters * 4, D3D12_HEAP_TYPE_DEFAULT, true, UAVS, g.counters) ||
        !buffer(kCounters * 4, D3D12_HEAP_TYPE_DEFAULT, false, kCommon, g.zero) ||
        !buffer(kSlots * (kCounters * 4 + 16), D3D12_HEAP_TYPE_READBACK, false, D3D12_RESOURCE_STATE_COPY_DEST, g.readback))
        return false;
    void* p = nullptr;
    D3D12_RANGE all{0, kSlots * (kCounters * 4 + 16)};
    if (FAILED(g.readback->Map(0, &all, &p))) return false;
    g.readback_ptr = static_cast<uint8_t*>(p);
    D3D12_QUERY_HEAP_DESC qd{};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = kSlots * 2;
    if (FAILED(g.device->CreateQueryHeap(&qd, IID_PPV_ARGS(&g.timestamps)))) g.timestamps.Reset(); // timing is optional
    UINT64 freq = 0;
    if (g.queue && SUCCEEDED(g.queue->GetTimestampFrequency(&freq)) && freq) g.ts_freq = static_cast<double>(freq);
    const D3D12_COMMAND_LIST_TYPE type = g.queue->GetDesc().Type;
    for (auto& p2 : g.pre) {
        if (FAILED(g.device->CreateCommandAllocator(type, IID_PPV_ARGS(&p2.alloc))) ||
            FAILED(g.device->CreateCommandList(0, type, p2.alloc.Get(), nullptr, IID_PPV_ARGS(&p2.list))))
            return false;
        p2.list->Close();
    }
    g.programs = true;
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE cpu_desc(uint32_t slot, uint32_t table, uint32_t i)
{
    D3D12_CPU_DESCRIPTOR_HANDLE hnd = g.heap->GetCPUDescriptorHandleForHeapStart();
    hnd.ptr += static_cast<SIZE_T>((slot * 2 + table) * 10 + i) * g.stride;
    return hnd;
}
D3D12_GPU_DESCRIPTOR_HANDLE gpu_table(uint32_t slot, uint32_t table)
{
    D3D12_GPU_DESCRIPTOR_HANDLE hnd = g.heap->GetGPUDescriptorHandleForHeapStart();
    hnd.ptr += static_cast<UINT64>((slot * 2 + table) * 10) * g.stride;
    return hnd;
}
void srv(uint32_t slot, uint32_t table, uint32_t i, ID3D12Resource* r, DXGI_FORMAT f)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = f;
    d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Texture2D.MipLevels = 1;
    g.device->CreateShaderResourceView(r, &d, cpu_desc(slot, table, i));
}
void uav_tex(uint32_t slot, uint32_t table, uint32_t i, ID3D12Resource* r, DXGI_FORMAT f)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
    d.Format = f;
    d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g.device->CreateUnorderedAccessView(r, nullptr, &d, cpu_desc(slot, table, i));
}
void uav_raw(uint32_t slot, uint32_t table, uint32_t i)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
    d.Format = DXGI_FORMAT_R32_TYPELESS;
    d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    d.Buffer.NumElements = kSlots * kCounters;
    d.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    g.device->CreateUnorderedAccessView(g.counters.Get(), nullptr, &d, cpu_desc(slot, table, i));
}
// Fills every descriptor of a table (unused ones get valid placeholder views).
void table_defaults(uint32_t slot, uint32_t table)
{
    for (uint32_t i = 0; i < 7; ++i) {
        const bool is_uint = i == 4;
        srv(slot, table, i, is_uint ? g.dummy_uint.Get() : g.raw.Get(), is_uint ? DXGI_FORMAT_R8_UINT : DXGI_FORMAT_R32G32_FLOAT);
    }
    uav_tex(slot, table, 7, g.img[0].tex.Get(), DXGI_FORMAT_B8G8R8A8_UNORM);
    uav_tex(slot, table, 8, g.mask.Get(), DXGI_FORMAT_R32_FLOAT);
    uav_raw(slot, table, 9);
}

// True when every GPU use of the size-dependent set has finished.
bool set_idle()
{
    const uint64_t of_done = g.done ? g.done->GetCompletedValue() : 0;
    const uint64_t j2_done = g.j2_fence ? g.j2_fence->GetCompletedValue() : UINT64_MAX;
    const uint64_t rd_done = g.ready ? g.ready->GetCompletedValue() : 0;
    for (const Image& im : g.img)
        if (of_done < im.last_of || j2_done < im.last_j2) return false;
    for (const Grid& gr : g.grids)
        if (of_done < gr.last_of || j2_done < gr.last_j2) return false;
    for (const Pre& p : g.pre)
        if (rd_done < p.ready) return false;
    return true;
}
// Unregisters and releases the size-dependent set; the engine is destroyed last
// (A36 teardown rule). Only called when set_idle() or the device is gone.
void release_set()
{
    if (g.built) {
        for (Image& im : g.img) if (im.handle) { g.engine->Unregister(im.handle); im.handle = nullptr; }
        for (Grid& gr : g.grids) {
            if (gr.handle) { g.engine->Unregister(gr.handle); gr.handle = nullptr; }
            if (gr.cost_handle) { g.engine->Unregister(gr.cost_handle); gr.cost_handle = nullptr; }
        }
    }
    for (Image& im : g.img) im = Image{};
    for (Grid& gr : g.grids) gr = Grid{};
    g.color_copy.Reset(); g.raw.Reset(); g.expand_valid.Reset(); g.residual.Reset(); g.valid.Reset(); g.mask.Reset();
    if (g.built && g.engine->Outstanding() == 0) g.engine->Shutdown();
    g.built = false;
    g.last_committed = -1;
    ++g.epoch;
}

bool build_set(uint32_t w, uint32_t h, DXGI_FORMAT f)
{
    const na::settings::Settings& st = na::settings::Get();
    uint32_t grid = st.motion_grid.load(), perf = st.motion_perf.load(), ds = st.motion_downscale.load();
    if (grid != 1 && grid != 2 && grid != 4) grid = 4;
    if (perf != 5 && perf != 10 && perf != 20) perf = 5;
    if (ds != 1 && ds != 2 && ds != 4) ds = 2;
    const uint32_t ew = (w + ds - 1) / ds, eh = (h + ds - 1) / ds;
    bool cost = true;
    if (!g.engine->Init(g.device.Get(), ew, eh, grid, perf, true)) {
        cost = false;
        if (!g.engine->Init(g.device.Get(), ew, eh, grid, perf, false)) {
            char b[160];
            std::snprintf(b, sizeof b, "Moving-object motion could not start the optical-flow engine: %.90s", g.engine->LastError());
            latch(b);
            return false;
        }
    }
    g.built = true;
    bool ok = texture(w, h, f, false, D3D12_RESOURCE_STATE_COPY_DEST, g.color_copy) &&
              texture(w, h, DXGI_FORMAT_R32G32_FLOAT, true, NPSR, g.raw) &&
              texture(w, h, DXGI_FORMAT_R32_FLOAT, true, NPSR, g.expand_valid) &&
              texture(w, h, DXGI_FORMAT_R32G32_FLOAT, true, NPSR, g.residual) &&
              texture(w, h, DXGI_FORMAT_R32_FLOAT, true, NPSR, g.valid) &&
              texture(w, h, DXGI_FORMAT_R32_FLOAT, true, NPSR, g.mask);
    for (Image& im : g.img) ok = ok && texture(ew, eh, DXGI_FORMAT_B8G8R8A8_UNORM, true, kCommon, im.tex);
    for (Grid& gr : g.grids) {
        ok = ok && texture(g.engine->OutWidth(), g.engine->OutHeight(), DXGI_FORMAT_R16G16_SINT, false, kCommon, gr.tex);
        if (cost) ok = ok && texture(g.engine->OutWidth(), g.engine->OutHeight(), DXGI_FORMAT_R8_UINT, false, kCommon, gr.cost);
    }
    if (!ok) { latch("Moving-object motion could not allocate its textures."); release_set(); return false; }
    // Registration handshake on a value the queue passes right away.
    const uint64_t hv = g.ready_next++;
    g.queue->Signal(g.ready.Get(), hv);
    for (Image& im : g.img) ok = ok && g.engine->RegisterInput(im.tex.Get(), g.ready.Get(), hv, hv, &im.handle);
    for (Grid& gr : g.grids) {
        ok = ok && g.engine->RegisterOutput(gr.tex.Get(), g.ready.Get(), hv, hv, &gr.handle);
        if (ok && cost && !g.engine->RegisterCost(gr.cost.Get(), g.ready.Get(), hv, hv, &gr.cost_handle)) {
            gr.cost_handle = nullptr; // the engine refused the cost buffer: run without it
            cost = false;
        }
    }
    if (!ok) {
        char b[160];
        std::snprintf(b, sizeof b, "Moving-object motion could not register its images: %.90s", g.engine->LastError());
        latch(b);
        release_set();
        return false;
    }
    if (!cost) for (Grid& gr : g.grids) if (gr.cost_handle) { g.engine->Unregister(gr.cost_handle); gr.cost_handle = nullptr; }
    g.w = w; g.h = h; g.ew = ew; g.eh = eh; g.grid_n = grid; g.perf = perf; g.ds = ds; g.format = f;
    g.stats.cost = cost && g.engine->CostEnabled() ? 1u : 0u;
    g.stats.width = w; g.stats.height = h; g.stats.engine_w = ew; g.stats.engine_h = eh;
    g.stats.grid = grid; g.stats.perf = perf; g.stats.downscale = ds;
    return true;
}

void watchdog_loop()
{
    for (;;) {
        const uint32_t ms = g.watchdog_ms.load();
        Sleep((std::max)(10u, ms / 10));
        std::lock_guard<std::mutex> lock(g.m);
        if (!g.done) continue;
        const uint64_t completed = g.done->GetCompletedValue();
        const uint64_t ready = g.ready->GetCompletedValue();
        const ULONGLONG now = GetTickCount64();
        for (Outstanding& o : g.jobs) {
            if (!o.value) continue;
            if (completed >= o.value) { o = Outstanding{}; continue; }
            if (ready < o.ready) continue;              // input not ready yet: not the engine's time
            if (!o.started) { o.started = now; continue; }
            if (now - o.started < ms) continue;
            // The engine stopped answering: release whatever waits on it, stop flow.
            g.done->Signal((std::max)(o.value, g.done_next - 1));
            ++g.stats.watchdog_trips;
            latch("Moving-object motion stopped: the optical-flow engine stopped answering (motion is camera-only).");
            for (Outstanding& o2 : g.jobs) o2 = Outstanding{};
            break;
        }
    }
}

bool ensure_engine(ID3D12Device* device, ID3D12CommandQueue* queue)
{
    if (g.device && g.device.Get() != device) { set_reason("Moving-object motion follows one device."); return false; }
    if (g.queue && g.queue.Get() != queue) { set_reason("Moving-object motion follows one queue."); return false; }
    if (!g.device) {
        g.device = device;
        g.queue = queue;
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.ready))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.done)))) {
            latch("Moving-object motion could not create its fences.");
            return false;
        }
    }
    if (!g.engine) g.engine = &g.nvof;
    if (!g.available_known) {
        char err[160] = {};
        g.available = g.engine->Available(device, err, sizeof err);
        g.available_known = true;
        g.stats.available = g.available ? 1u : 0u;
        if (!g.available) {
            char b[160];
            std::snprintf(b, sizeof b, "Moving-object motion needs NVIDIA hardware optical flow: %.100s", err[0] ? err : "not offered here");
            latch(b);
            return false;
        }
    }
    if (!g.available) return false;
    if (!typed_uav_store(DXGI_FORMAT_B8G8R8A8_UNORM)) { latch("Moving-object motion cannot write its engine input format here."); return false; }
    if (!build_programs()) { latch("Moving-object motion could not build its GPU programs."); return false; }
    if (!g.watchdog_started) {
        g.watchdog_started = true;
        std::thread(watchdog_loop).detach();
    }
    return true;
}

void read_back()
{
    if (!g.j2_fence || !g.readback_ptr) return;
    const uint64_t done = g.j2_fence->GetCompletedValue();
    for (uint32_t s = 0; s < kSlots; ++s) {
        if (!g.slot_j2[s] || done < g.slot_j2[s]) continue;
        uint32_t c[kCounters];
        std::memcpy(c, g.readback_ptr + s * (kCounters * 4 + 16), sizeof c);
        g.stats.px_accepted = c[0]; g.stats.px_invalid = c[1]; g.stats.px_small = c[2];
        g.stats.px_cost = c[3]; g.stats.px_offscreen = c[4]; g.stats.px_image = c[5];
        g.stats.mean_cost = c[7] ? static_cast<float>(c[6]) / static_cast<float>(c[7]) : 0.f;
        if (g.slot_has_ts[s] && g.ts_freq > 0) {
            uint64_t ts[2];
            std::memcpy(ts, g.readback_ptr + s * (kCounters * 4 + 16) + kCounters * 4, sizeof ts);
            if (ts[1] > ts[0]) {
                const float ms = static_cast<float>(static_cast<double>(ts[1] - ts[0]) * 1000.0 / g.ts_freq);
                g.stats.ms_last = ms;
                g.stats.ms_avg = g.stats.ms_avg > 0 ? g.stats.ms_avg * 0.9f + ms * 0.1f : ms;
            }
        }
        g.slot_j2[s] = 0;
        g.slot_has_ts[s] = false;
    }
}

void track_job(uint64_t value, uint64_t ready)
{
    for (Outstanding& o : g.jobs) {
        if (o.value) continue;
        o.value = value;
        o.ready = ready;
        o.started = 0;
        return;
    }
    // Full (16 jobs outstanding): the oldest slot is reused.
    g.jobs[0] = Outstanding{value, ready, 0};
}

} // namespace

void SetEngineForTests(Engine* engine)
{
    std::lock_guard<std::mutex> lock(g.m);
    g.engine = engine;
    g.available_known = false;
    g.latched = false;
}
void SetWatchdogMsForTests(uint32_t ms) { g.watchdog_ms.store(ms ? ms : 1000); }
void SetEnabled(bool on)
{
    g.enabled.store(on);
    std::lock_guard<std::mutex> lock(g.m);
    g.stats.enabled = on ? 1u : 0u;
    if (!on) { g.stats.active = 0; if (!g.latched) set_reason("Off."); }
    else if (!g.latched) set_reason("Waiting for the first frame.");
    g.last_committed = -1; // a pair never spans an off period
}
bool Enabled() { return g.enabled.load(); }
void SetAllowSplit(bool on) { g.allow_split.store(on); }
bool AllowSplit() { return g.allow_split.load(); }

Ticket Prepare(ID3D12Device* device, ID3D12CommandQueue* queue, const Source& src)
{
    Ticket t;
    if (!g.enabled.load() || !device || !queue || !src.color) return t;
    std::lock_guard<std::mutex> lock(g.m);
    read_back();
    if (g.latched) { g.stats.active = 0; return t; }
    if (!ensure_engine(device, queue)) { g.stats.active = 0; return t; }
    D3D12_RESOURCE_DESC cd = src.color->GetDesc();
    if (cd.SampleDesc.Count > 1) {
        ++g.stats.skipped_msaa;
        set_reason("Camera-only: the scene colour is multisampled (moving-object motion supports single-sample scenes).");
        g.last_committed = -1;
        return t;
    }
    // Size, format or settings change: rebuild once every use of the old set retired.
    const na::settings::Settings& st = na::settings::Get();
    const bool changed = !g.built || g.w != src.width || g.h != src.height || g.format != src.format ||
                         g.grid_n != st.motion_grid.load() || g.perf != st.motion_perf.load() || g.ds != st.motion_downscale.load();
    if (changed) {
        if (g.built && !set_idle()) { ++g.stats.skipped_resize; set_reason("Moving-object motion is restarting for a new picture size."); return t; }
        if (g.built) release_set();
        if (!build_set(src.width, src.height, src.format)) { g.stats.active = 0; return t; }
    }
    const uint64_t of_done = g.done->GetCompletedValue();
    const uint64_t j2_done = g.j2_fence ? g.j2_fence->GetCompletedValue() : 0;
    // The image slot to write: never the previous image, never one still read.
    const int prev = g.last_committed;
    int cur = -1;
    for (uint32_t k = 1; k <= kInputs; ++k) {
        const int c = static_cast<int>((static_cast<uint32_t>(prev < 0 ? 0 : prev) + k) % kInputs);
        if (c == prev) continue;
        const Image& im = g.img[c];
        if (of_done >= im.last_of && j2_done >= im.last_j2) { cur = c; break; }
    }
    Pre& pre = g.pre[g.pre_next];
    if (cur < 0 || g.ready->GetCompletedValue() < pre.ready) {
        ++g.stats.skipped_busy;
        set_reason("Camera-only this frame: the previous motion images are still in use.");
        g.last_committed = -1; // the next frame cannot pair with a skipped one
        return t;
    }
    // Pair only with the frame J2's camera history pairs with.
    bool pair = prev >= 0 && !src.reset && src.previous_frame && g.img[prev].committed &&
                g.img[prev].frame == src.previous_frame && g.img[prev].generation == src.generation;
    if (!pair && prev >= 0) ++g.stats.skipped_reset;
    if (pair && !src.allow_pair) { pair = false; ++g.stats.skipped_split; }
    Grid* gr = nullptr;
    uint32_t gi = 0;
    if (pair) {
        gi = g.next_grid;
        gr = &g.grids[gi];
        if (of_done < gr->last_of || j2_done < gr->last_j2) { pair = false; gr = nullptr; ++g.stats.skipped_busy; }
    }
    // Pre list: copy the scene subresource, encode it into img[cur].
    if (FAILED(pre.alloc->Reset()) || FAILED(pre.list->Reset(pre.alloc.Get(), nullptr))) {
        latch("Moving-object motion could not reset its command list.");
        return t;
    }
    ID3D12GraphicsCommandList* l = pre.list.Get();
    const uint32_t slot = src.slot % kSlots;
    if (pair && g.timestamps) l->EndQuery(g.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);
    Image& im = g.img[cur];
    const D3D12_RESOURCE_STATES sc = static_cast<D3D12_RESOURCE_STATES>(src.state);
    {
        D3D12_RESOURCE_BARRIER b[1];
        UINT n = 0;
        if (sc != D3D12_RESOURCE_STATE_COPY_SOURCE) b[n++] = transition(src.color, sc, D3D12_RESOURCE_STATE_COPY_SOURCE, src.subresource);
        if (n) l->ResourceBarrier(n, b);
    }
    D3D12_TEXTURE_COPY_LOCATION from{}, to{};
    from.Type = to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.pResource = src.color;
    from.SubresourceIndex = src.subresource;
    to.pResource = g.color_copy.Get();
    to.SubresourceIndex = 0;
    l->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    {
        D3D12_RESOURCE_BARRIER b[3];
        UINT n = 0;
        if (sc != D3D12_RESOURCE_STATE_COPY_SOURCE) b[n++] = transition(src.color, D3D12_RESOURCE_STATE_COPY_SOURCE, sc, src.subresource);
        b[n++] = transition(g.color_copy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, NPSR);
        b[n++] = transition(im.tex.Get(), kCommon, UAVS);
        l->ResourceBarrier(n, b);
    }
    table_defaults(slot, 0);
    srv(slot, 0, 0, g.color_copy.Get(), src.format);
    uav_tex(slot, 0, 7, im.tex.Get(), DXGI_FORMAT_B8G8R8A8_UNORM);
    Constants c{};
    c.w = src.width; c.h = src.height; c.ew = g.ew; c.eh = g.eh;
    c.mode = is_float(src.format) ? 1u : 0u;
    ID3D12DescriptorHeap* heaps[] = {g.heap.Get()};
    l->SetDescriptorHeaps(1, heaps);
    l->SetComputeRootSignature(g.root.Get());
    l->SetPipelineState(g.pso_encode.Get());
    l->SetComputeRootDescriptorTable(0, gpu_table(slot, 0));
    l->SetComputeRoot32BitConstants(1, kConstantWords, &c, 0);
    l->Dispatch((g.ew + 7) / 8, (g.eh + 7) / 8, 1);
    {
        D3D12_RESOURCE_BARRIER b[3] = {uav(im.tex.Get()), transition(im.tex.Get(), UAVS, kCommon),
                                       transition(g.color_copy.Get(), NPSR, D3D12_RESOURCE_STATE_COPY_DEST)};
        l->ResourceBarrier(3, b);
    }
    if (FAILED(l->Close())) { latch("Moving-object motion's command list was rejected."); return t; }
    im.committed = false; // being overwritten; becomes a previous image only once submitted
    t.valid = true;
    t.pre = l;
    t.pre_slot = g.pre_next;
    g.pre_next = (g.pre_next + 1) % kPre;
    t.ready_value = g.ready_next++;
    pre.ready = t.ready_value;
    t.cur = static_cast<uint32_t>(cur);
    t.slot = slot;
    t.epoch = g.epoch;
    t.frame = src.frame;
    t.generation = src.generation;
    t.jitter_cur[0] = src.jitter_px[0];
    t.jitter_cur[1] = src.jitter_px[1];
    if (pair) {
        const Image& pv = g.img[prev];
        const uint64_t fv = g.done_next++;
        if (!g.engine->Execute(im.handle, pv.handle, gr->handle, gr->cost_handle, g.ready.Get(), t.ready_value, g.done.Get(), fv)) {
            ++g.stats.engine_failed;
            char b[160];
            std::snprintf(b, sizeof b, "Moving-object motion stopped: %.120s", g.engine->LastError());
            latch(b); // this frame still encodes and runs camera-only; no queue wait is enqueued
            pair = false;
        } else {
            t.pair = true;
            t.flow_value = fv;
            t.prev = static_cast<uint32_t>(prev);
            t.grid = gi;
            t.jitter_prev[0] = pv.jitter[0];
            t.jitter_prev[1] = pv.jitter[1];
            im.last_of = fv;
            g.img[prev].last_of = fv;
            gr->last_of = fv;
            g.next_grid = (gi + 1) % kGrids;
            track_job(fv, t.ready_value);
            ++g.stats.pairs;
        }
    }
    if (!t.pair) ++g.stats.encode_only;
    g.stats.active = 1;
    set_reason(t.pair ? "Running." : "Collecting the first image of a pair.");
    return t;
}

bool RecordFusion(ID3D12GraphicsCommandList* l, Ticket& t, ID3D12Resource* depth32, const na::motion::gpu::Inputs& camera, Tier3* out)
{
    if (!l || !t.valid || !t.pair || !depth32 || !out) return false;
    std::lock_guard<std::mutex> lock(g.m);
    if (t.epoch != g.epoch || !g.built) return false;
    Grid& gr = g.grids[t.grid];
    Image& cur = g.img[t.cur];
    Image& prev = g.img[t.prev];
    const uint32_t slot = t.slot;
    const bool cost = gr.cost_handle != nullptr;
    // 1. Engine outputs and both images readable; expansion targets writable.
    {
        D3D12_RESOURCE_BARRIER b[7];
        UINT n = 0;
        b[n++] = transition(gr.tex.Get(), kCommon, NPSR);
        if (cost) b[n++] = transition(gr.cost.Get(), kCommon, NPSR);
        b[n++] = transition(cur.tex.Get(), kCommon, NPSR);
        b[n++] = transition(prev.tex.Get(), kCommon, NPSR);
        b[n++] = transition(g.raw.Get(), NPSR, UAVS);
        b[n++] = transition(g.expand_valid.Get(), NPSR, UAVS);
        l->ResourceBarrier(n, b);
    }
    // 2. Grid -> full-size raw flow (pixels, p_previous - p_current), depth-aware with
    //    validity. Engine pixels are scaled by the downscale both in position (block) and value.
    g.expand.SetVectorScale(static_cast<float>(g.ds));
    na::nvof::ExpandParams ep;
    ep.depth = depth32;
    ep.validity_out = g.expand_valid.Get();
    ep.depth_clear = camera.depth_clear;
    bool ok = g.expand.DispatchDepthAware(l, gr.tex.Get(), g.grid_n * g.ds, ep, g.raw.Get(), g.w, g.h, slot);
    {
        D3D12_RESOURCE_BARRIER b[6] = {uav(g.raw.Get()), uav(g.expand_valid.Get()),
                                       transition(g.raw.Get(), UAVS, NPSR), transition(g.expand_valid.Get(), UAVS, NPSR),
                                       transition(g.residual.Get(), NPSR, UAVS), transition(g.valid.Get(), NPSR, UAVS)};
        l->ResourceBarrier(6, b);
    }
    // 3. Residual = raw - camera motion - (jitter_prev - jitter_cur), this exact pair, once.
    ok = ok && g.derive.DispatchDeriveResidual(l, g.raw.Get(), depth32, camera, t.jitter_prev, g.residual.Get(), g.valid.Get(),
                                               g.w, g.h, slot, g.expand_valid.Get());
    {
        D3D12_RESOURCE_BARRIER b[5] = {uav(g.residual.Get()), uav(g.valid.Get()),
                                       transition(g.residual.Get(), UAVS, NPSR), transition(g.valid.Get(), UAVS, NPSR),
                                       transition(g.mask.Get(), NPSR, UAVS)};
        l->ResourceBarrier(5, b);
    }
    // 4. Acceptance: engine cost + image check against camera-only; counters per slot.
    const uint32_t off = slot * kCounters * 4;
    {
        D3D12_RESOURCE_BARRIER b = transition(g.counters.Get(), UAVS, D3D12_RESOURCE_STATE_COPY_DEST);
        l->ResourceBarrier(1, &b);
        l->CopyBufferRegion(g.counters.Get(), off, g.zero.Get(), 0, kCounters * 4);
        b = transition(g.counters.Get(), D3D12_RESOURCE_STATE_COPY_DEST, UAVS);
        l->ResourceBarrier(1, &b);
    }
    table_defaults(slot, 1);
    srv(slot, 1, 1, g.raw.Get(), DXGI_FORMAT_R32G32_FLOAT);
    srv(slot, 1, 2, g.residual.Get(), DXGI_FORMAT_R32G32_FLOAT);
    srv(slot, 1, 3, g.valid.Get(), DXGI_FORMAT_R32_FLOAT);
    srv(slot, 1, 4, cost ? gr.cost.Get() : g.dummy_uint.Get(), DXGI_FORMAT_R8_UINT);
    srv(slot, 1, 5, cur.tex.Get(), DXGI_FORMAT_B8G8R8A8_UNORM);
    srv(slot, 1, 6, prev.tex.Get(), DXGI_FORMAT_B8G8R8A8_UNORM);
    uav_tex(slot, 1, 8, g.mask.Get(), DXGI_FORMAT_R32_FLOAT);
    Constants c{};
    c.w = g.w; c.h = g.h; c.ew = g.ew; c.eh = g.eh;
    c.block = g.grid_n * g.ds;
    c.has_cost = cost ? 1u : 0u;
    c.cost_max = kCostMax;
    c.cnt_off = off;
    c.gw = g.engine->OutWidth(); c.gh = g.engine->OutHeight();
    c.snap_px = kSnapPx; c.image_margin = kImageMargin; c.image_abs = kImageAbs;
    ID3D12DescriptorHeap* heaps[] = {g.heap.Get()};
    l->SetDescriptorHeaps(1, heaps);
    l->SetComputeRootSignature(g.root.Get());
    l->SetPipelineState(g.pso_accept.Get());
    l->SetComputeRootDescriptorTable(0, gpu_table(slot, 1));
    l->SetComputeRoot32BitConstants(1, kConstantWords, &c, 0);
    if (ok) l->Dispatch((g.w + 7) / 8, (g.h + 7) / 8, 1);
    {
        D3D12_RESOURCE_BARRIER b[8];
        UINT n = 0;
        b[n++] = uav(g.mask.Get());
        b[n++] = uav(g.counters.Get());
        b[n++] = transition(g.mask.Get(), UAVS, NPSR);
        b[n++] = transition(g.counters.Get(), UAVS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        b[n++] = transition(gr.tex.Get(), NPSR, kCommon);
        if (cost) b[n++] = transition(gr.cost.Get(), NPSR, kCommon);
        b[n++] = transition(cur.tex.Get(), NPSR, kCommon);
        b[n++] = transition(prev.tex.Get(), NPSR, kCommon);
        l->ResourceBarrier(n, b);
    }
    const uint64_t rb = static_cast<uint64_t>(slot) * (kCounters * 4 + 16);
    l->CopyBufferRegion(g.readback.Get(), rb, g.counters.Get(), off, kCounters * 4);
    {
        D3D12_RESOURCE_BARRIER b = transition(g.counters.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, UAVS);
        l->ResourceBarrier(1, &b);
    }
    if (g.timestamps) {
        l->EndQuery(g.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
        l->ResolveQueryData(g.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2, 2, g.readback.Get(), rb + kCounters * 4);
    }
    if (!ok) return false;
    t.consumed = true;
    out->residual = g.residual.Get();
    out->mask = g.mask.Get();
    out->valid = g.valid.Get();
    return true;
}

void Submit(ID3D12CommandQueue* queue, Ticket& t, ID3D12CommandList* const* batch, UINT count, UINT ours, ExecuteFn forward)
{
    if (!t.valid) { forward(queue, count, batch); return; }
    if (t.pair && t.consumed && ours > 0 && ours < count) {
        forward(queue, ours, batch);                 // game lists up to the scene + our pre list
        queue->Signal(g.ready.Get(), t.ready_value); // the engine may start
        queue->Wait(g.done.Get(), t.flow_value);     // GPU wait: J2's list reads the flow
        {
            std::lock_guard<std::mutex> lock(g.m);
            ++g.stats.consumed;
        }
        forward(queue, count - ours, batch + ours);  // J2's list + the rest of the game's batch
        return;
    }
    forward(queue, count, batch);
    queue->Signal(g.ready.Get(), t.ready_value);
}

void Committed(Ticket& t, ID3D12Fence* j2_fence, uint64_t value)
{
    if (!t.valid) return;
    std::lock_guard<std::mutex> lock(g.m);
    if (!g.j2_fence && j2_fence) g.j2_fence = j2_fence;
    if (t.epoch != g.epoch) return;
    Image& im = g.img[t.cur];
    im.committed = true;
    im.frame = t.frame;
    im.generation = t.generation;
    im.jitter[0] = t.jitter_cur[0];
    im.jitter[1] = t.jitter_cur[1];
    g.last_committed = static_cast<int>(t.cur);
    if (t.consumed) {
        im.last_j2 = value;
        g.img[t.prev].last_j2 = value;
        g.grids[t.grid].last_j2 = value;
        g.slot_j2[t.slot] = value;
        g.slot_has_ts[t.slot] = g.timestamps != nullptr;
    }
    t = Ticket{};
}

void Abandon(ID3D12CommandQueue* queue, Ticket& t)
{
    if (!t.valid) return;
    std::lock_guard<std::mutex> lock(g.m);
    // Never leave the engine waiting: release its input fence (the image it reads
    // is stale, its output is never read), and never pair with this image.
    if (queue && g.ready) queue->Signal(g.ready.Get(), t.ready_value);
    if (t.epoch == g.epoch) g.img[t.cur].committed = false;
    g.last_committed = -1;
    ++g.stats.abandoned;
    t = Ticket{};
}

void Shutdown()
{
    std::lock_guard<std::mutex> lock(g.m);
    if (g.engine) release_set();
    for (auto& p : g.pre) { p.list.Reset(); p.alloc.Reset(); p.ready = 0; }
    if (g.readback && g.readback_ptr) { g.readback->Unmap(0, nullptr); g.readback_ptr = nullptr; }
    g.expand = na::nvof::FlowExpand{};
    g.derive = na::motion::gpu::Arbiter{};
    g.counters.Reset(); g.zero.Reset(); g.readback.Reset(); g.timestamps.Reset(); g.dummy_uint.Reset();
    g.root.Reset(); g.pso_encode.Reset(); g.pso_accept.Reset(); g.heap.Reset();
    g.programs = false;
    for (Outstanding& o : g.jobs) o = Outstanding{};
    for (auto& v : g.slot_j2) v = 0;
    g.j2_fence.Reset();
    g.ready.Reset(); g.done.Reset();
    g.ready_next = g.done_next = 1;
    g.queue.Reset(); g.device.Reset();
    g.available_known = false;
    g.latched = false;
    g.stats = Stats{};
    g.stats.enabled = g.enabled.load() ? 1u : 0u;
    set_reason(g.enabled.load() ? "Waiting for the first frame." : "Off.");
}

Stats GetStats()
{
    std::lock_guard<std::mutex> lock(g.m);
    Stats s = g.stats;
    s.enabled = g.enabled.load() ? 1u : 0u;
    s.latched = g.latched ? 1u : 0u;
    std::snprintf(s.reason, sizeof s.reason, "%s", g.reason);
    return s;
}

} // namespace na::j2::flow
