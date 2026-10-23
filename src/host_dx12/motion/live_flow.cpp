#include "host_dx12/motion/live_flow.hpp"
#include "host_dx12/depth_format.hpp"

#include <d3dcompiler.h>
#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "host_dx12/color/color_space.hpp"
#include "host_dx12/nvof/flow_expand.hpp"
#include "host_dx12/nvof/nvof_flow.hpp"
#include "host_dx12/settings.hpp"

using Microsoft::WRL::ComPtr;

namespace na::motion {
namespace {

// Back buffer -> BGRA8 display-encoded SDR (what NVOFA expects).
const char kShader[] = R"(
Texture2D<float4> t_in : register(t0);
RWTexture2D<float4> u_out : register(u0);
cbuffer C : register(b0) { float paper_white; uint width; uint height; uint is_pq; };
static const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
float3 pq_to_nits(float3 e) {
    float3 ep = pow(max(e, 0.0), 1.0 / m2);
    float3 num = max(ep - c1, 0.0), den = c2 - c3 * ep;
    return pow(num / den, 1.0 / m1) * 10000.0;
}
float3 srgb_encode(float3 v) { v = saturate(v); return v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1.0 / 2.4) - 0.055; }
SamplerState s_linear : register(s0);
// width/height = ENGINE input size (display / downscale, A58): the back
// buffer is sampled bilinearly, which is an exact texel read at 1:1.
[numthreads(8, 8, 1)]
void to_bgra8(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(width, height);
    float4 p = t_in.SampleLevel(s_linear, uv, 0);
    // is_pq is the shared shader mode (A60): 0 SDR (already display-encoded), 1 PQ, 2 scRGB (linear, 1.0 = 80 nits).
    float3 c = is_pq == 1 ? srgb_encode(pq_to_nits(p.rgb) / paper_white)
             : (is_pq == 2 ? srgb_encode(p.rgb * 80.0 / paper_white) : p.rgb);
    u_out[id.xy] = float4(c, 1.0); // UAV format is B8G8R8A8_UNORM: the view swizzles
}
// A57: render-size depth (nearest) and motion (bilinear) -> display size.
// The motion output is in DISPLAY pixels: value * scale (render px) * (display / render).
[numthreads(8, 8, 1)]
void up_depth(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    uint sw, sh; t_in.GetDimensions(sw, sh);
    uint2 src = uint2(min((id.x * sw) / width, sw - 1), min((id.y * sh) / height, sh - 1));
    u_out[id.xy] = t_in[src];
}
[numthreads(8, 8, 1)]
void up_motion(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    uint sw, sh; t_in.GetDimensions(sw, sh);
    float2 uv = (float2(id.xy) + 0.5) / float2(width, height);
    float2 mv = t_in.SampleLevel(s_linear, uv, 0).xy;
    // paper_white carries mv_scale_x, is_pq carries asuint(mv_scale_y) here (constants reuse).
    float2 px = mv * float2(paper_white, asfloat(is_pq)) * float2(width, height) / float2(sw, sh);
    u_out[id.xy] = float4(px, 0.0, 0.0);
}
// A57: endpoint error accumulation (fixed point, 1/256 px) into a 4-uint buffer:
// [0] sum |flow - ref| * 256, [1] count, [2] within 1 px, [3] sum |ref| * 256.
Texture2D<float2> t_flow : register(t1);
Texture2D<float2> t_ref : register(t2);
RWByteAddressBuffer u_acc : register(u1);
groupshared uint g_sum, g_cnt, g_in1, g_mag;
[numthreads(8, 8, 1)]
void compare(uint3 id : SV_DispatchThreadID, uint3 tid : SV_GroupThreadID) {
    if (tid.x == 0 && tid.y == 0) { g_sum = 0; g_cnt = 0; g_in1 = 0; g_mag = 0; }
    GroupMemoryBarrierWithGroupSync();
    // A 40-px border is excluded: content entering the frame has no previous
    // position, and the engine's outer blocks are unreliable there.
    if (id.x < width && id.y < height && id.x >= 40 && id.y >= 40 && id.x + 40 < width && id.y + 40 < height) {
        float2 f = t_flow[id.xy];
        float2 r = t_ref[id.xy];
        bool ok = all(abs(f) < 512.0) && all(abs(r) < 512.0) && !any(isnan(f)) && !any(isnan(r));
        if (ok) {
            float e = length(f - r);
            InterlockedAdd(g_sum, (uint)(e * 256.0));
            InterlockedAdd(g_cnt, 1u);
            if (e <= 1.0) InterlockedAdd(g_in1, 1u);
            InterlockedAdd(g_mag, (uint)(length(r) * 256.0));
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (tid.x == 0 && tid.y == 0) {
        u_acc.InterlockedAdd(0, g_sum); u_acc.InterlockedAdd(4, g_cnt); u_acc.InterlockedAdd(8, g_in1); u_acc.InterlockedAdd(12, g_mag);
    }
}
)";
struct Constants { float paper_white; uint32_t width, height, is_pq; };

constexpr uint32_t kRing = 4;     // command allocators / fence slots
constexpr uint32_t kInputs = 3;   // BGRA8 inputs: current, previous, one being overwritten
constexpr uint32_t kOutputs = 2;  // published flow/validity pairs (consumer reads one while we write the other)

struct Slot { ComPtr<ID3D12CommandAllocator> alloc; uint64_t fence_value = 0; };

// A53 (session 9: the game froze after 118 frames): the GAME's queue never
// waits on the engine any more. Conversion runs on the game's queue (it
// needs the back buffer) and signals `fence`; the engine waits on that; the
// expansion runs on OUR OWN queue, which waits on the engine fence and
// signals `own_fence` for consumers. An input is reused only when the
// engine that read it has provably finished (CPU poll, else skip). A
// watchdog latches the feature off when the engine has not answered for
// kEngineTimeoutMs, so a stalled engine can only stall our queue, not the game.
constexpr uint32_t kEngineTimeoutMs = 2000;

struct State {
    std::mutex m;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12GraphicsCommandList> list;      // conversion (game queue)
    ComPtr<ID3D12GraphicsCommandList> own_list;  // expansion (own queue)
    ComPtr<ID3D12CommandQueue> own_queue;
    Slot slots[kRing];
    Slot own_slots[kRing];
    uint32_t slot_index = 0;
    ComPtr<ID3D12Fence> fence;      // game-queue conversion work
    uint64_t fence_next = 1;
    ComPtr<ID3D12Fence> own_fence;  // our queue's expansion work (published to consumers)
    uint64_t own_next = 1;
    ComPtr<ID3D12Fence> of_fence;   // signalled by the engine
    uint64_t of_next = 1;
    uint64_t input_of_value[kInputs] = {}; // engine fence value of the last Execute that READ each input
    ULONGLONG of_issued_tick = 0;   // when the outstanding engine job was issued (watchdog)
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso, pso_up_depth, pso_up_motion, pso_compare;
    ComPtr<ID3D12DescriptorHeap> heap;      // per ring slot: 8 descriptors (4 tables of t0,t1,t2 + u0/u1 layout)
    UINT heap_stride = 0;
    // A57 references from the DLSS evaluate (render size), upsampled at Present.
    ComPtr<ID3D12Resource> depth_copy, motion_copy;   // NPSR at rest
    ComPtr<ID3D12Resource> depth_up, motion_up;       // R32F / RG32F, display size
    DXGI_FORMAT motion_format = DXGI_FORMAT_UNKNOWN;
    uint32_t ref_w = 0, ref_h = 0;
    float ref_scale_x = 0, ref_scale_y = 0;
    uint32_t have_ref = 0, ref_fresh = 0, ref_has_depth = 0;
    ComPtr<ID3D12Resource> acc_buffer;                // 4 uint per ring slot, UAV
    ComPtr<ID3D12Resource> acc_readback;
    uint64_t acc_fence_value[kRing] = {};
    std::atomic<uint32_t> ref_available{0};
    std::atomic<float> ref_err{0.f}, ref_within{0.f}, ref_mag{0.f};
    std::atomic<uint64_t> ref_frames{0};
    // Engine.
    na::nvof::Flow flow;
    bool flow_ready = false;
    ComPtr<ID3D12Resource> inputs[kInputs];
    na::nvof::BufferHandle input_handles[kInputs] = {};
    ComPtr<ID3D12Resource> grid_out;
    na::nvof::BufferHandle grid_handle = nullptr;
    na::nvof::FlowExpand expand;
    ComPtr<ID3D12Resource> flow_out[kOutputs], valid_out[kOutputs];
    uint32_t out_index = 0;
    uint32_t width = 0, height = 0, grid = 4, perf = 5;
    uint32_t downscale = 1, ew = 0, eh = 0; // A58: engine runs at (width/downscale) x (height/downscale)
    uint32_t input_index = 0;    // next input to write
    uint64_t input_frames = 0;   // inputs converted so far
    uint64_t last_of_value = 0;  // engine fence value the last Execute signals
    LARGE_INTEGER of_started{};  // when that Execute was issued
    LARGE_INTEGER qpc_freq{};
    // Depth source.
    ID3D12Resource* depth = nullptr;
    float depth_clear = 1.0f;
    // Published result.
    std::atomic<uint32_t> have_output{0};
    Output latest{};
    // Status.
    std::atomic<uint32_t> active{0}, latched{0}, hdr{0}, available{0};
    std::atomic<uint64_t> frames{0}, skipped{0};
    std::atomic<float> flow_ms{0.f}, avg_ms{0.f};
    char reason[96] = "Motion analysis is off.";
};
State g;

void set_reason(const char* t) { strncpy_s(g.reason, t, _TRUNCATE); }
void latch(const char* t) { set_reason(t); g.latched.store(1, std::memory_order_release); g.active.store(0, std::memory_order_release); }

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
D3D12_RESOURCE_BARRIER uav(ID3D12Resource* r)
{
    D3D12_RESOURCE_BARRIER br{};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    br.UAV.pResource = r;
    return br;
}
bool create_texture(UINT w, UINT h, DXGI_FORMAT fmt, bool uav_flag, D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& out)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = fmt; d.SampleDesc.Count = 1;
    d.Flags = uav_flag ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
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

bool ensure_device_locked(ID3D12Device* dev)
{
    if (g.device) return true;
    g.device = dev;
    QueryPerformanceFrequency(&g.qpc_freq);
    for (Slot& s : g.slots)
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.alloc)))) return false;
    for (Slot& s : g.own_slots)
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.alloc)))) return false;
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.slots[0].alloc.Get(), nullptr, IID_PPV_ARGS(&g.list)))) return false;
    g.list->Close();
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.own_slots[0].alloc.Get(), nullptr, IID_PPV_ARGS(&g.own_list)))) return false;
    g.own_list->Close();
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.own_queue)))) return false;
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)))) return false;
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.own_fence)))) return false;
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.of_fence)))) return false;
    // Table layout per dispatch: t0,t1,t2 then u0,u1 (5 descriptors).
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors = 3; ranges[0].BaseShaderRegister = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ranges[1].NumDescriptors = 2; ranges[1].BaseShaderRegister = 0; ranges[1].OffsetInDescriptorsFromTableStart = 3;
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = 4;
    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 2; rs.pParameters = params; rs.NumStaticSamplers = 1; rs.pStaticSamplers = &samp;
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) return false;
    if (FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g.root)))) return false;
    auto compile = [&](const char* entry, ComPtr<ID3D12PipelineState>& pso) {
        ComPtr<ID3DBlob> code, e2;
        if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "live_flow.hlsl", nullptr, nullptr, entry, "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &e2))) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g.root.Get();
        pd.CS.pShaderBytecode = code->GetBufferPointer();
        pd.CS.BytecodeLength = code->GetBufferSize();
        return SUCCEEDED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)));
    };
    if (!compile("to_bgra8", g.pso) || !compile("up_depth", g.pso_up_depth) || !compile("up_motion", g.pso_up_motion) || !compile("compare", g.pso_compare)) return false;
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 5 * 4 * kRing; // 4 dispatch tables per ring slot
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.heap)))) return false;
    g.heap_stride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    // Accumulator (UAV) + readback for the ground-truth comparison.
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 16 * kRing; bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&g.acc_buffer)))) return false;
        D3D12_HEAP_PROPERTIES rb{}; rb.Type = D3D12_HEAP_TYPE_READBACK;
        bd.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (FAILED(dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g.acc_readback)))) return false;
    }
    return g.expand.Init(dev, kRing);
}

// Descriptor table `t` (0..3) of ring slot `slot`: writes SRVs t0..t2 and UAVs u0..u1.
struct TableDesc { ID3D12Resource* srv[3]; DXGI_FORMAT srv_fmt[3]; ID3D12Resource* uav[2]; DXGI_FORMAT uav_fmt[2]; bool uav1_is_buffer; };
D3D12_GPU_DESCRIPTOR_HANDLE write_table(uint32_t slot, uint32_t t, const TableDesc& d)
{
    uint32_t base = (slot * 4 + t) * 5;
    D3D12_CPU_DESCRIPTOR_HANDLE h = g.heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(base) * g.heap_stride;
    for (int i = 0; i < 3; ++i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = d.srv_fmt[i]; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sd.Texture2D.MipLevels = 1;
        if (d.srv[i]) g.device->CreateShaderResourceView(d.srv[i], &sd, h);
        else { sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; g.device->CreateShaderResourceView(nullptr, &sd, h); }
        h.ptr += g.heap_stride;
    }
    for (int i = 0; i < 2; ++i) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        if (i == 1 && d.uav1_is_buffer && d.uav[1]) {
            ud.Format = DXGI_FORMAT_R32_TYPELESS; ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            ud.Buffer.NumElements = 4 * kRing; ud.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            g.device->CreateUnorderedAccessView(d.uav[1], nullptr, &ud, h);
        } else {
            ud.Format = d.uav_fmt[i]; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            if (d.uav[i]) g.device->CreateUnorderedAccessView(d.uav[i], nullptr, &ud, h);
            else { ud.Format = DXGI_FORMAT_R32_FLOAT; g.device->CreateUnorderedAccessView(nullptr, nullptr, &ud, h); }
        }
        h.ptr += g.heap_stride;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gh = g.heap->GetGPUDescriptorHandleForHeapStart();
    gh.ptr += static_cast<UINT64>(base) * g.heap_stride;
    return gh;
}

// Engine + textures for the current extent. Registrations use the queue
// fence at its current value as the handshake (nothing pending on them).
bool ensure_engine_locked(ID3D12CommandQueue* queue, uint32_t w, uint32_t h)
{
    if (g.flow_ready && g.width == w && g.height == h) return true;
    if (g.flow_ready) {
        // Extent changed: the engine must be torn down in the documented order.
        for (uint32_t i = 0; i < kInputs; ++i) if (g.input_handles[i]) { g.flow.Unregister(g.input_handles[i]); g.input_handles[i] = nullptr; }
        if (g.grid_handle) { g.flow.Unregister(g.grid_handle); g.grid_handle = nullptr; }
        for (auto& t : g.inputs) t.Reset();
        g.grid_out.Reset();
        if (!g.flow.Shutdown()) { latch("Motion analysis could not restart after a size change."); return false; }
        g.flow_ready = false;
    }
    na::nvof::Caps caps;
    char err[192];
    if (!na::nvof::QueryCaps(g.device.Get(), &caps, err, sizeof(err)) || !caps.available) {
        g.available.store(0, std::memory_order_release);
        latch("Motion analysis needs NVIDIA hardware optical flow, which this graphics card or driver does not offer.");
        return false;
    }
    g.available.store(1, std::memory_order_release);
    if (!uav_store_supported(DXGI_FORMAT_B8G8R8A8_UNORM)) { latch("Motion analysis cannot write its input format on this graphics card."); return false; }
    g.grid = na::settings::Get().motion_grid.load();
    g.perf = na::settings::Get().motion_perf.load();
    g.downscale = na::settings::Get().motion_downscale.load();
    if (g.downscale != 1 && g.downscale != 2 && g.downscale != 4) g.downscale = 2;
    g.ew = (w + g.downscale - 1) / g.downscale;
    g.eh = (h + g.downscale - 1) / g.downscale;
    if (!g.flow.Init(g.device.Get(), g.ew, g.eh, g.grid, g.perf)) {
        char buf[96];
        snprintf(buf, sizeof(buf), "Motion analysis could not start: %.60s", g.flow.LastError());
        latch(buf);
        return false;
    }
    bool ok = true;
    for (auto& t : g.inputs) ok &= create_texture(g.ew, g.eh, DXGI_FORMAT_B8G8R8A8_UNORM, true, D3D12_RESOURCE_STATE_COMMON, t);
    ok &= create_texture(g.flow.OutWidth(), g.flow.OutHeight(), DXGI_FORMAT_R16G16_SINT, false, D3D12_RESOURCE_STATE_COMMON, g.grid_out);
    for (uint32_t i = 0; i < kOutputs; ++i) {
        ok &= create_texture(w, h, DXGI_FORMAT_R32G32_FLOAT, true, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, g.flow_out[i]);
        ok &= create_texture(w, h, DXGI_FORMAT_R32_FLOAT, true, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, g.valid_out[i]);
    }
    if (!ok) { latch("Motion analysis could not allocate its buffers."); return false; }
    // Registration handshake on a value the queue has already passed.
    uint64_t hv = g.fence_next++;
    queue->Signal(g.fence.Get(), hv);
    for (uint32_t i = 0; i < kInputs; ++i)
        ok &= g.flow.RegisterInput(g.inputs[i].Get(), g.fence.Get(), hv, hv, &g.input_handles[i]);
    ok &= g.flow.RegisterOutput(g.grid_out.Get(), g.fence.Get(), hv, hv, &g.grid_handle);
    if (!ok) {
        char buf[96];
        snprintf(buf, sizeof(buf), "Motion analysis could not register its buffers: %.50s", g.flow.LastError());
        latch(buf);
        return false;
    }
    g.width = w;
    g.height = h;
    g.input_index = 0;
    g.input_frames = 0;
    for (auto& v : g.input_of_value) v = 0;
    g.last_of_value = 0;
    g.of_issued_tick = 0;
    g.flow_ready = true;
    return true;
}

} // namespace

void SetDepthSource(ID3D12Resource* depth, float depth_clear)
{
    std::lock_guard<std::mutex> lock(g.m);
    g.depth = depth;
    g.depth_clear = depth_clear;
}

void OnPresent(ID3D12CommandQueue* queue, ID3D12Resource* backbuffer, DXGI_COLOR_SPACE_TYPE color_space)
{
    if (!queue || !backbuffer) return;
    const na::settings::Settings& st = na::settings::Get();
    if (!st.motion_enabled.load(std::memory_order_relaxed)) { g.active.store(0, std::memory_order_release); return; }
    std::lock_guard<std::mutex> lock(g.m);
    if (g.latched.load(std::memory_order_acquire)) return;
    ComPtr<ID3D12Device> dev;
    if (FAILED(backbuffer->GetDevice(IID_PPV_ARGS(&dev))) || !ensure_device_locked(dev.Get())) { latch("Motion analysis could not set up on the graphics card."); return; }
    D3D12_RESOURCE_DESC bb = backbuffer->GetDesc();
    uint32_t w = static_cast<uint32_t>(bb.Width), h = bb.Height;
    na::color::Encoding enc = na::color::Classify(bb.Format, color_space); // A60: PQ, scRGB or SDR
    bool pq = na::color::IsHdr(enc);
    uint32_t shader_mode = na::color::ShaderMode(enc);
    g.hdr.store(pq ? 1u : 0u, std::memory_order_release);
    if (!ensure_engine_locked(queue, w, h)) return;
    Slot& slot = g.slots[g.slot_index];
    Slot& own_slot = g.own_slots[g.slot_index];
    if ((slot.fence_value && g.fence->GetCompletedValue() < slot.fence_value) ||
        (own_slot.fence_value && g.own_fence->GetCompletedValue() < own_slot.fence_value)) {
        g.skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    uint64_t of_done = g.of_fence->GetCompletedValue();
    // Watchdog: an engine job outstanding for too long latches the feature off
    // (only OUR queue is waiting on it; the game's queue never does).
    if (g.last_of_value && of_done < g.last_of_value && g.of_issued_tick && GetTickCount64() - g.of_issued_tick > kEngineTimeoutMs) {
        latch("Motion analysis stopped: the optical-flow engine stopped answering.");
        return;
    }
    // Timing of the previous engine run (CPU-observed).
    if (g.last_of_value && of_done >= g.last_of_value && g.of_started.QuadPart) {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        float ms = 1000.f * static_cast<float>(now.QuadPart - g.of_started.QuadPart) / static_cast<float>(g.qpc_freq.QuadPart);
        g.flow_ms.store(ms);
        float avg = g.avg_ms.load();
        g.avg_ms.store(avg > 0 ? avg * 0.9f + ms * 0.1f : ms);
        g.of_started.QuadPart = 0;
    }
    // The input we are about to overwrite must no longer be read by the engine (CPU check, never a queue wait).
    uint32_t cur = g.input_index;
    if (g.input_of_value[cur] && of_done < g.input_of_value[cur]) { g.skipped.fetch_add(1, std::memory_order_relaxed); return; }

    // Pass 1 (game queue): convert the back buffer into inputs[cur] (BGRA8 SDR),
    // and (A57, DLSS titles) upsample the evaluate-time depth/motion references.
    ID3D12Resource* in_tex = g.inputs[cur].Get();
    slot.alloc->Reset();
    g.list->Reset(slot.alloc.Get(), nullptr);
    D3D12_RESOURCE_BARRIER b1[2] = {transition(backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                                    transition(in_tex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
    g.list->ResourceBarrier(2, b1);
    ID3D12DescriptorHeap* heaps[] = {g.heap.Get()};
    g.list->SetDescriptorHeaps(1, heaps);
    g.list->SetComputeRootSignature(g.root.Get());
    g.list->SetPipelineState(g.pso.Get());
    g.list->SetComputeRootDescriptorTable(0, write_table(g.slot_index, 0, TableDesc{{backbuffer, nullptr, nullptr}, {bb.Format, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM}, {in_tex, nullptr}, {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R32_FLOAT}, false}));
    Constants c{static_cast<float>(st.nr_paper_white.load()), g.ew, g.eh, shader_mode};
    g.list->SetComputeRoot32BitConstants(1, 4, &c, 0);
    g.list->Dispatch((g.ew + 7) / 8, (g.eh + 7) / 8, 1);
    D3D12_RESOURCE_BARRIER b2[3] = {uav(in_tex), transition(in_tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON),
                                    transition(backbuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PRESENT)};
    g.list->ResourceBarrier(3, b2);
    bool use_ref = g.have_ref && g.ref_fresh && g.depth_copy && g.motion_copy;
    g.ref_fresh = 0;
    if (use_ref) {
        bool ok = true;
        if (!g.depth_up || g.depth_up->GetDesc().Width != w || g.depth_up->GetDesc().Height != h) {
            ok &= create_texture(w, h, DXGI_FORMAT_R32_FLOAT, true, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, g.depth_up);
            ok &= create_texture(w, h, DXGI_FORMAT_R32G32_FLOAT, true, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, g.motion_up);
        }
        if (ok) {
            D3D12_RESOURCE_BARRIER bu0[2] = {transition(g.depth_up.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                                             transition(g.motion_up.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
            g.list->ResourceBarrier(2, bu0);
            if (g.ref_has_depth) {
                g.list->SetPipelineState(g.pso_up_depth.Get());
                g.list->SetComputeRootDescriptorTable(0, write_table(g.slot_index, 1, TableDesc{{g.depth_copy.Get(), nullptr, nullptr}, {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM}, {g.depth_up.Get(), nullptr}, {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT}, false}));
                g.list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
            }
            g.list->SetPipelineState(g.pso_up_motion.Get());
            g.list->SetComputeRootDescriptorTable(0, write_table(g.slot_index, 2, TableDesc{{g.motion_copy.Get(), nullptr, nullptr}, {g.motion_format, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM}, {g.motion_up.Get(), nullptr}, {DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32_FLOAT}, false}));
            Constants cm{g.ref_scale_x, w, h, 0};
            std::memcpy(&cm.is_pq, &g.ref_scale_y, sizeof(float)); // asfloat(is_pq) in the shader
            g.list->SetComputeRoot32BitConstants(1, 4, &cm, 0);
            g.list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
            D3D12_RESOURCE_BARRIER bu1[4] = {uav(g.depth_up.Get()), uav(g.motion_up.Get()),
                                             transition(g.depth_up.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                                             transition(g.motion_up.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
            g.list->ResourceBarrier(4, bu1);
        } else {
            use_ref = false;
        }
    }
    g.list->Close();
    ID3D12CommandList* lists[] = {g.list.Get()};
    queue->ExecuteCommandLists(1, lists);
    uint64_t converted = g.fence_next++;
    queue->Signal(g.fence.Get(), converted);
    ++g.input_frames;
    g.input_index = (cur + 1) % kInputs;

    if (g.input_frames < 2) { // no previous frame yet
        slot.fence_value = converted;
        g.slot_index = (g.slot_index + 1) % kRing;
        g.skipped.fetch_add(1, std::memory_order_relaxed);
        set_reason("Motion analysis is starting.");
        return;
    }
    uint32_t prev = (cur + kInputs - 1) % kInputs;
    // Pass 2: the engine (asynchronous): current -> previous flow, after the conversion completed.
    uint64_t ofv = g.of_next++;
    QueryPerformanceCounter(&g.of_started);
    if (!g.flow.Execute(g.input_handles[cur], g.input_handles[prev], g.grid_handle, g.fence.Get(), converted, g.of_fence.Get(), ofv, true)) {
        char buf[96];
        snprintf(buf, sizeof(buf), "Motion analysis stopped: %.60s", g.flow.LastError());
        latch(buf);
        slot.fence_value = converted;
        g.slot_index = (g.slot_index + 1) % kRing;
        return;
    }
    g.last_of_value = ofv;
    g.of_issued_tick = GetTickCount64();
    g.input_of_value[cur] = ofv;
    g.input_of_value[prev] = ofv;
    slot.fence_value = converted;
    // Pass 3 (OUR queue, which waits on the engine; the game's queue never does):
    // expand the grid into the arbiter's raw flow (+ validity).
    g.own_queue->Wait(g.of_fence.Get(), ofv);
    uint32_t oi = g.out_index;
    ID3D12Resource* flow_tex = g.flow_out[oi].Get();
    ID3D12Resource* valid_tex = g.valid_out[oi].Get();
    own_slot.alloc->Reset();
    g.own_list->Reset(own_slot.alloc.Get(), nullptr);
    D3D12_RESOURCE_BARRIER b3[3] = {transition(g.grid_out.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                                    transition(flow_tex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                                    transition(valid_tex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
    g.own_list->ResourceBarrier(3, b3);
    bool recorded;
    ID3D12Resource* depth_for_expand = g.depth ? g.depth : ((use_ref && g.ref_has_depth) ? g.depth_up.Get() : nullptr);
    g.expand.SetVectorScale(static_cast<float>(g.downscale));   // A58: engine pixels -> display pixels
    uint32_t block = g.grid * g.downscale;                       // block size in display pixels
    if (depth_for_expand) {
        na::nvof::ExpandParams ep;
        ep.depth = depth_for_expand;
        ep.validity_out = valid_tex;
        ep.depth_clear = g.depth ? g.depth_clear : 1.0f; // A13: TLOU non-inverted depth clears to 1
        recorded = g.expand.DispatchDepthAware(g.own_list.Get(), g.grid_out.Get(), block, ep, flow_tex, w, h, g.slot_index);
    } else {
        recorded = g.expand.Dispatch(g.own_list.Get(), g.grid_out.Get(), block, flow_tex, w, h, g.slot_index);
    }
    D3D12_RESOURCE_BARRIER b4[4] = {uav(flow_tex), transition(g.grid_out.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
                                    transition(flow_tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                                    transition(valid_tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
    g.own_list->ResourceBarrier(4, b4);
    // A57: compare against the game's motion vectors; accumulate into this slot's 16 bytes, copy to readback.
    if (recorded && use_ref) {
        uint32_t off = g.slot_index * 16;
        // Zero this slot's accumulator (previous readback of it has been consumed: the slot's own fence passed).
        D3D12_RESOURCE_BARRIER to_copy = transition(g.acc_buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        g.own_list->ResourceBarrier(1, &to_copy);
        // (a fresh zero source: the readback buffer's slot after consumption holds stale data, so use a UAV clear instead)
        D3D12_RESOURCE_BARRIER back_uav = transition(g.acc_buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        g.own_list->ResourceBarrier(1, &back_uav);
        UINT zeros[4] = {0, 0, 0, 0};
        D3D12_GPU_DESCRIPTOR_HANDLE tbl = write_table(g.slot_index, 3, TableDesc{{nullptr, flow_tex, g.motion_up.Get()}, {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32_FLOAT}, {nullptr, g.acc_buffer.Get()}, {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_TYPELESS}, true});
        // ClearUnorderedAccessViewUint needs a CPU handle too: recompute it for the u1 descriptor of this table.
        D3D12_CPU_DESCRIPTOR_HANDLE u1_cpu = g.heap->GetCPUDescriptorHandleForHeapStart();
        u1_cpu.ptr += static_cast<SIZE_T>((g.slot_index * 4 + 3) * 5 + 4) * g.heap_stride;
        D3D12_GPU_DESCRIPTOR_HANDLE u1_gpu = tbl; u1_gpu.ptr += static_cast<UINT64>(4) * g.heap_stride;
        ID3D12DescriptorHeap* own_heaps[] = {g.heap.Get()};
        g.own_list->SetDescriptorHeaps(1, own_heaps);
        D3D12_RECT rect{static_cast<LONG>(off / 4), 0, static_cast<LONG>(off / 4 + 4), 1};
        g.own_list->ClearUnorderedAccessViewUint(u1_gpu, u1_cpu, g.acc_buffer.Get(), zeros, 1, &rect);
        D3D12_RESOURCE_BARRIER cleared = uav(g.acc_buffer.Get());
        g.own_list->ResourceBarrier(1, &cleared);
        g.own_list->SetComputeRootSignature(g.root.Get());
        g.own_list->SetPipelineState(g.pso_compare.Get());
        g.own_list->SetComputeRootDescriptorTable(0, tbl);
        Constants cc{0.f, w, h, 0};
        g.own_list->SetComputeRoot32BitConstants(1, 4, &cc, 0);
        g.own_list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        D3D12_RESOURCE_BARRIER b5[2] = {uav(g.acc_buffer.Get()), transition(g.acc_buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE)};
        g.own_list->ResourceBarrier(2, b5);
        g.own_list->CopyBufferRegion(g.acc_readback.Get(), off, g.acc_buffer.Get(), off, 16);
        D3D12_RESOURCE_BARRIER b6 = transition(g.acc_buffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        g.own_list->ResourceBarrier(1, &b6);
    }
    g.own_list->Close();
    if (!recorded) { latch("Motion analysis could not expand the flow field."); return; }
    ID3D12CommandList* own_lists[] = {g.own_list.Get()};
    g.own_queue->ExecuteCommandLists(1, own_lists);
    uint64_t done = g.own_next++;
    g.own_queue->Signal(g.own_fence.Get(), done);
    own_slot.fence_value = done;
    g.acc_fence_value[g.slot_index] = use_ref ? done : 0;
    // Read back the comparison of an older slot whose fence has passed (never waits).
    for (uint32_t k = 0; k < kRing; ++k) {
        uint32_t s2 = (g.slot_index + 1 + k) % kRing;
        if (!g.acc_fence_value[s2] || g.own_fence->GetCompletedValue() < g.acc_fence_value[s2]) continue;
        void* m = nullptr;
        D3D12_RANGE rr{static_cast<SIZE_T>(s2) * 16, static_cast<SIZE_T>(s2) * 16 + 16};
        if (SUCCEEDED(g.acc_readback->Map(0, &rr, &m))) {
            uint32_t v[4];
            std::memcpy(v, static_cast<uint8_t*>(m) + s2 * 16, 16);
            D3D12_RANGE none{0, 0};
            g.acc_readback->Unmap(0, &none);
            if (v[1]) {
                g.ref_err.store(static_cast<float>(v[0]) / 256.f / static_cast<float>(v[1]));
                g.ref_within.store(100.f * static_cast<float>(v[2]) / static_cast<float>(v[1]));
                g.ref_mag.store(static_cast<float>(v[3]) / 256.f / static_cast<float>(v[1]));
                g.ref_frames.fetch_add(1, std::memory_order_relaxed);
                g.ref_available.store(1, std::memory_order_release);
            }
        }
        g.acc_fence_value[s2] = 0;
    }
    g.slot_index = (g.slot_index + 1) % kRing;
    g.out_index = (oi + 1) % kOutputs;
    g.latest = Output{flow_tex, depth_for_expand ? valid_tex : nullptr, g.own_fence.Get(), done, g.frames.load() + 1, w, h, block};
    g.have_output.store(1, std::memory_order_release);
    g.frames.fetch_add(1, std::memory_order_relaxed);
    g.active.store(1, std::memory_order_release);
    set_reason(pq ? (shader_mode == 2 ? "Motion analysis is active (HDR picture, scRGB)." : "Motion analysis is active (HDR picture).")
                  : "Motion analysis is active.");
}

bool WantsInputs()
{
    return na::settings::Get().motion_enabled.load(std::memory_order_relaxed) != 0 && !g.latched.load(std::memory_order_acquire);
}

void OnEvaluate(ID3D12GraphicsCommandList* list, const EvalRef& in)
{
    if (!list || !in.motion) return; // depth optional: without it the reference is motion-only (plain expansion)
    std::lock_guard<std::mutex> lock(g.m);
    if (!WantsInputs()) return;
    ComPtr<ID3D12Device> dev;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&dev))) || !ensure_device_locked(dev.Get())) return;
    D3D12_RESOURCE_DESC md = in.motion->GetDesc();
    uint32_t w = static_cast<uint32_t>(md.Width), h = md.Height;
    bool with_depth = in.depth != nullptr;
    if (with_depth) {
        D3D12_RESOURCE_DESC dd = in.depth->GetDesc();
        if (static_cast<uint32_t>(dd.Width) != w || dd.Height != h) with_depth = false;
        // Argon audit #2: 24/16-bit depth can't be copied as R32F; run motion-only instead.
        if (!na::depth::PlaneZeroCopiesToR32F(dd.Format)) with_depth = false;
    }
    if (!g.motion_copy || g.ref_w != w || g.ref_h != h || g.motion_format != md.Format) {
        if (!create_texture(w, h, DXGI_FORMAT_R32_FLOAT, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, g.depth_copy) ||
            !create_texture(w, h, md.Format, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, g.motion_copy)) {
            g.have_ref = 0;
            return;
        }
        g.ref_w = w; g.ref_h = h; g.motion_format = md.Format;
    }
    D3D12_RESOURCE_BARRIER pre[4] = {
        transition(in.motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
        transition(g.motion_copy.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
        transition(with_depth ? in.depth : nullptr, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
        transition(g.depth_copy.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST)};
    pre[2].Transition.Subresource = 0;
    list->ResourceBarrier(with_depth ? 4 : 2, pre);
    list->CopyResource(g.motion_copy.Get(), in.motion);
    if (with_depth) {
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.Type = dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = in.depth; src.SubresourceIndex = 0;
        dst.pResource = g.depth_copy.Get(); dst.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    D3D12_RESOURCE_BARRIER post[4] = {
        transition(in.motion, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        transition(g.motion_copy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        transition(with_depth ? in.depth : nullptr, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        transition(g.depth_copy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
    post[2].Transition.Subresource = 0;
    list->ResourceBarrier(with_depth ? 4 : 2, post);
    g.ref_scale_x = in.mv_scale_x;
    g.ref_scale_y = in.mv_scale_y;
    g.ref_has_depth = with_depth ? 1u : 0u;
    g.have_ref = 1;
    g.ref_fresh = 1;
}

bool Latest(Output* out)
{
    if (!out || !g.have_output.load(std::memory_order_acquire)) return false;
    std::lock_guard<std::mutex> lock(g.m);
    *out = g.latest;
    return true;
}

Stats GetStats()
{
    Stats s{};
    s.enabled = na::settings::Get().motion_enabled.load(std::memory_order_relaxed);
    s.active = g.active.load(std::memory_order_acquire);
    s.latched_off = g.latched.load(std::memory_order_acquire);
    s.hdr = g.hdr.load(std::memory_order_acquire);
    s.available = g.available.load(std::memory_order_acquire);
    s.frames = g.frames.load(std::memory_order_relaxed);
    s.skipped = g.skipped.load(std::memory_order_relaxed);
    s.flow_ms = g.flow_ms.load();
    s.avg_ms = g.avg_ms.load();
    s.ref_available = g.ref_available.load(std::memory_order_acquire);
    s.ref_mean_err_px = g.ref_err.load();
    s.ref_within_1px_pct = g.ref_within.load();
    s.ref_mean_mag_px = g.ref_mag.load();
    s.ref_frames = g.ref_frames.load(std::memory_order_relaxed);
    std::unique_lock<std::mutex> lock(g.m, std::try_to_lock);
    if (lock.owns_lock()) {
        s.width = g.width; s.height = g.height; s.grid = g.grid * g.downscale; s.perf = g.perf; // grid reported in display pixels
        strncpy_s(s.reason, g.reason, _TRUNCATE);
    } else {
        strncpy_s(s.reason, "Updating.", _TRUNCATE);
    }
    return s;
}

void ResetLatch()
{
    std::lock_guard<std::mutex> lock(g.m);
    g.latched.store(0, std::memory_order_release);
    set_reason("Motion analysis is off.");
}

void Shutdown()
{
    std::lock_guard<std::mutex> lock(g.m);
    auto wait_fence = [](ID3D12Fence* f, uint64_t v) {
        if (f && v && f->GetCompletedValue() < v) {
            HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            f->SetEventOnCompletion(v, ev);
            WaitForSingleObject(ev, 3000);
            CloseHandle(ev);
        }
    };
    wait_fence(g.of_fence.Get(), g.last_of_value);
    wait_fence(g.fence.Get(), g.fence_next - 1);
    wait_fence(g.own_fence.Get(), g.own_next - 1);
    // Teardown rule (A36): unregister and release registered resources, then destroy the engine; fences outlive it.
    for (uint32_t i = 0; i < kInputs; ++i) if (g.input_handles[i]) { g.flow.Unregister(g.input_handles[i]); g.input_handles[i] = nullptr; }
    if (g.grid_handle) { g.flow.Unregister(g.grid_handle); g.grid_handle = nullptr; }
    for (auto& t : g.inputs) t.Reset();
    g.grid_out.Reset();
    if (g.flow_ready) g.flow.Shutdown();
    g.flow_ready = false;
    for (auto& t : g.flow_out) t.Reset();
    for (auto& t : g.valid_out) t.Reset();
    g.depth_copy.Reset(); g.motion_copy.Reset(); g.depth_up.Reset(); g.motion_up.Reset();
    g.acc_buffer.Reset(); g.acc_readback.Reset();
    for (auto& v : g.acc_fence_value) v = 0;
    g.have_ref = 0; g.ref_fresh = 0; g.ref_w = g.ref_h = 0;
    g.ref_available.store(0); g.ref_frames.store(0); g.ref_err.store(0.f); g.ref_within.store(0.f); g.ref_mag.store(0.f);
    g.have_output.store(0, std::memory_order_release);
    g.latest = Output{};
    g.pso.Reset(); g.pso_up_depth.Reset(); g.pso_up_motion.Reset(); g.pso_compare.Reset();
    g.root.Reset(); g.heap.Reset(); g.list.Reset(); g.own_list.Reset(); g.own_queue.Reset();
    for (Slot& s : g.slots) { s.alloc.Reset(); s.fence_value = 0; }
    for (Slot& s : g.own_slots) { s.alloc.Reset(); s.fence_value = 0; }
    g.fence.Reset(); g.of_fence.Reset(); g.own_fence.Reset();
    g.device.Reset();
    g.width = g.height = 0;
    g.input_frames = 0; g.last_of_value = 0; g.fence_next = 1; g.of_next = 1; g.own_next = 1; g.slot_index = 0; g.out_index = 0;
    g.of_issued_tick = 0;
    for (auto& v : g.input_of_value) v = 0;
    g.active.store(0); g.latched.store(0);
    set_reason("Motion analysis is off.");
}

} // namespace na::motion
