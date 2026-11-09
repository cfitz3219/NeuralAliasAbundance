#include "host_dx12/j2/game_motion.hpp"

#include <d3dcompiler.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace na::j2::game_motion {
namespace {

// Mirrors ScoreCpu (game_motion_decide.hpp) on a stride grid, and the convert step.
const char kSource[] = R"(
Texture2D<float2> t_c0   : register(t0);
Texture2D<float2> t_c1   : register(t1);
Texture2D<float4> t_cur  : register(t2);
Texture2D<float4> t_prev : register(t3);
SamplerState s_lin       : register(s0);
RWByteAddressBuffer u_stats : register(u0);
RWTexture2D<float2> u_xy    : register(u1);
RWTexture2D<float>  u_valid : register(u2);

cbuffer C : register(b0) {
    uint w, h;          // colour allocation
    uint sw, sh;        // active extent (scale of UV / NDC encodings)
    uint gw, gh;        // score grid
    uint step, count;   // grid step, candidates
    uint unorm0, unorm1;
    uint use_c, use_h;  // convert: candidate and encoding
};

#define NH 12u
#define SLOTS 18u
#define S_ZERO 12u
#define S_SCORED 13u
#define S_MOVING 14u
#define S_NONFINITE 15u
#define S_MAXX 16u
#define S_MAXY 17u

groupshared uint gs[2u * SLOTS];

bool finite1(float a) { return (asuint(a) & 0x7fffffffu) < 0x7f800000u; }
float luma(float4 c) { const float l = max(dot(c.rgb, float3(0.2126, 0.7152, 0.0722)), 0.0); return l / (1.0 + l); }
float2 scale_of(uint k) {
    const uint kind = k / 4u;
    float2 a = float2(1.0, 1.0);
    if (kind == 1u) a = float2(sw, sh);
    if (kind == 2u) a = 0.5 * float2(sw, sh);
    return float2((k & 1u) ? -a.x : a.x, (k & 2u) ? -a.y : a.y);
}
float2 read_c(uint c, uint2 p) { return c == 0u ? t_c0[p] : t_c1[p]; }
uint enc(float e) { return (uint)(min(abs(e), 1.0) * 1024.0); }

void score_one(uint c, uint2 p) {
    const uint base = c * SLOTS;
    float2 r = read_c(c, p);
    if (!finite1(r.x) || !finite1(r.y)) { InterlockedAdd(gs[base + S_NONFINITE], 1u); return; }
    if ((c == 0u ? unorm0 : unorm1) != 0u) r = r * 2.0 - 1.0;
    if (abs(r.x) <= 1e-6 && abs(r.y) <= 1e-6) return;
    InterlockedAdd(gs[base + S_MOVING], 1u);
    InterlockedMax(gs[base + S_MAXX], asuint(abs(r.x)));
    InterlockedMax(gs[base + S_MAXY], asuint(abs(r.y)));
    float lo = 1.0, hi = 0.0;
    [unroll] for (int j = -1; j <= 1; ++j)
        [unroll] for (int i = -1; i <= 1; ++i) { const float v = luma(t_cur[uint2(int2(p) + int2(i, j))]); lo = min(lo, v); hi = max(hi, v); }
    if (hi - lo < 0.02) return;
    const float cl = luma(t_cur[p]);
    InterlockedAdd(gs[base + S_SCORED], 1u);
    InterlockedAdd(gs[base + S_ZERO], enc(cl - luma(t_prev[p])));
    const float2 inv = 1.0 / float2(w, h);
    const float2 pos = float2(p) + 0.5;
    [unroll] for (uint k = 0u; k < NH; ++k) {
        const float2 q = (pos + r * scale_of(k)) * inv;
        InterlockedAdd(gs[base + k], enc(cl - luma(t_prev.SampleLevel(s_lin, q, 0))));
    }
}

[numthreads(8, 8, 1)]
void score(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    if (gi < 2u * SLOTS) gs[gi] = 0u;
    GroupMemoryBarrierWithGroupSync();
    const uint2 p = uint2(1u + id.x * step, 1u + id.y * step);
    if (id.x < gw && id.y < gh && p.x + 1u < sw && p.y + 1u < sh)
        for (uint c = 0u; c < count; ++c) score_one(c, p);
    GroupMemoryBarrierWithGroupSync();
    if (gi < 2u * SLOTS) {
        const uint v = gs[gi];
        const uint s = gi % SLOTS;
        uint old;
        if (v != 0u) {
            if (s == S_MAXX || s == S_MAXY) u_stats.InterlockedMax(gi * 4u, v, old);
            else u_stats.InterlockedAdd(gi * 4u, v, old);
        }
    }
}

[numthreads(8, 8, 1)]
void convert(uint3 id : SV_DispatchThreadID) {
    if (id.x >= sw || id.y >= sh) return;
    float2 r = read_c(use_c, id.xy);
    bool ok = finite1(r.x) && finite1(r.y);
    if (ok && (use_c == 0u ? unorm0 : unorm1) != 0u) r = r * 2.0 - 1.0;
    ok = ok && (abs(r.x) > 1e-6 || abs(r.y) > 1e-6);
    u_xy[id.xy] = ok ? r * scale_of(use_h) : float2(0.0, 0.0);
    u_valid[id.xy] = ok ? 1.0 : 0.0;
}
)";

struct Constants { uint32_t w, h, sw, sh, gw, gh, step, count, unorm0, unorm1, use_c, use_h; };
constexpr uint32_t kTable = 7; // t0..t3, u0..u2
constexpr uint32_t kCounterBytes = kCandidates * kSlots * 4;
constexpr uint32_t kStep = 4;
static_assert(kSlots == 18 && kErrZero == 12 && kMaxY == 17, "shader SLOTS and S_* defines");

std::mutex g_stats_m;

DXGI_FORMAT srv_format(DXGI_FORMAT f, bool* unorm)
{
    if (unorm) *unorm = false;
    switch (f) {
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R16G16_UNORM: if (unorm) *unorm = true; return f;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default: return f;
    }
}
D3D12_RESOURCE_BARRIER tr(ID3D12Resource* r, UINT sub, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r;
    x.Transition.Subresource = sub;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    return x;
}
bool buffer(ID3D12Device* d, D3D12_HEAP_TYPE type, UINT64 bytes, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES st, ComPtr<ID3D12Resource>& out)
{
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = type;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = bytes; bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; bd.Flags = flags;
    return SUCCEEDED(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, st, nullptr, IID_PPV_ARGS(&out)));
}
bool texture(ID3D12Device* d, UINT w, UINT h, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES st, ComPtr<ID3D12Resource>& out)
{
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = f; td.SampleDesc.Count = 1; td.Flags = flags;
    return SUCCEEDED(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, st, nullptr, IID_PPV_ARGS(&out)));
}

} // namespace

bool Feed::Init(ID3D12Device* device, uint32_t slots)
{
    if (!device || slots == 0 || slots > 8) return false;
    device_ = device;
    slots_ = slots;
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors = 4;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ranges[1].NumDescriptors = 3; ranges[1].OffsetInDescriptorsFromTableStart = 4;
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = sizeof(Constants) / 4;
    D3D12_STATIC_SAMPLER_DESC smp{};
    smp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    smp.MaxLOD = D3D12_FLOAT32_MAX;
    smp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 2; rs.pParameters = params;
    rs.NumStaticSamplers = 1; rs.pStaticSamplers = &smp;
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) return false;
    if (FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root_)))) return false;
    const UINT cf = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS;
    for (int k = 0; k < 2; ++k) {
        ComPtr<ID3DBlob> code;
        if (FAILED(D3DCompile(kSource, sizeof(kSource) - 1, "game_motion.hlsl", nullptr, nullptr, k ? "convert" : "score", "cs_5_0", cf, 0,
                              &code, &err)))
            return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = root_.Get();
        pd.CS.pShaderBytecode = code->GetBufferPointer();
        pd.CS.BytecodeLength = code->GetBufferSize();
        if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(k ? &pso_convert_ : &pso_score_)))) return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kTable * slots_;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)))) return false;
    stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (!buffer(device, D3D12_HEAP_TYPE_UPLOAD, kCounterBytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, zeros_)) return false;
    {
        void* p = nullptr;
        if (FAILED(zeros_->Map(0, nullptr, &p)) || !p) return false;
        std::memset(p, 0, kCounterBytes);
        zeros_->Unmap(0, nullptr);
    }
    for (uint32_t s = 0; s < slots_; ++s) {
        if (!buffer(device, D3D12_HEAP_TYPE_DEFAULT, kCounterBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST,
                    slot_[s].counters) ||
            !buffer(device, D3D12_HEAP_TYPE_READBACK, kCounterBytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, slot_[s].readback))
            return false;
    }
    return true;
}

bool Feed::ensure_textures(ID3D12Resource* color, uint32_t rw, uint32_t rh)
{
    const D3D12_RESOURCE_DESC cd = color->GetDesc();
    if (!prev_ || prev_format_ != cd.Format || prev_->GetDesc().Width != cd.Width || prev_->GetDesc().Height != cd.Height) {
        prev_.Reset();
        prev_valid_ = false;
        if (!texture(device_.Get(), static_cast<UINT>(cd.Width), cd.Height, cd.Format, D3D12_RESOURCE_FLAG_NONE,
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, prev_))
            return false;
        prev_format_ = cd.Format;
    }
    if (!xy_ || tw_ != rw || th_ != rh) {
        xy_.Reset(); valid_.Reset();
        if (!texture(device_.Get(), rw, rh, DXGI_FORMAT_R32G32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, xy_) ||
            !texture(device_.Get(), rw, rh, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, valid_))
            return false;
        tw_ = rw; th_ = rh;
    }
    return true;
}

void Feed::collect(uint32_t slot)
{
    SlotState& s = slot_[slot];
    if (!s.recorded) return;
    s.recorded = false;
    uint32_t* p = nullptr;
    const D3D12_RANGE whole{0, kCounterBytes};
    if (FAILED(s.readback->Map(0, &whole, reinterpret_cast<void**>(&p))) || !p) return;
    uint32_t v[kCandidates * kSlots];
    std::memcpy(v, p, sizeof(v));
    std::memset(p, 0, sizeof(v)); // a recording that never ran reads back as nothing measured
    s.readback->Unmap(0, &whole);
    std::lock_guard<std::mutex> lock(g_stats_m);
    ++stats_.collected;
    for (uint32_t c = 0; c < kCandidates; ++c) {
        if (!s.candidate_id[c] || s.candidate_id[c] != current_id_[c]) continue; // measured on a texture no longer in that seat
        const uint32_t* q = v + c * kSlots;
        Measure m;
        for (uint32_t k = 0; k < kHypotheses; ++k) m.err[k] = q[kErr0 + k];
        m.err_zero = q[kErrZero]; m.scored = q[kScored]; m.moving = q[kMoving]; m.non_finite = q[kNonFinite];
        std::memcpy(&m.max_x, &q[kMaxX], 4); std::memcpy(&m.max_y, &q[kMaxY], 4);
        decider_.Add(c, m);
    }
}

Output Feed::Record(ID3D12GraphicsCommandList* list, uint32_t slot, const Candidate* cands, uint32_t count,
                    ID3D12Resource* color, uint32_t rw, uint32_t rh, bool reset)
{
    Output out{};
    if (!list || !pso_score_ || slot >= slots_ || !color || rw < 4 || rh < 4) return out;
    collect(slot);
    if (count == 0) {
        // Nothing to score: free the seats (so a texture found later starts fresh) and stop
        // before any texture or descriptor work (WWII p49040: every evaluation came here).
        std::lock_guard<std::mutex> lock(g_stats_m);
        for (uint32_t c = 0; c < kCandidates; ++c)
            if (current_id_[c]) { decider_.ResetCandidate(c); current_id_[c] = 0; ++stats_.candidate_changes; }
        return out;
    }
    if (count > kCandidates) count = kCandidates;
    // Stable seats: a texture keeps its seat while it is found; a new one takes a free seat.
    uint64_t next[kCandidates]{};
    const Candidate* seat[kCandidates]{};
    for (uint32_t i = 0; i < count; ++i)
        for (uint32_t c = 0; c < kCandidates; ++c)
            if (cands[i].id && cands[i].id == current_id_[c]) { next[c] = cands[i].id; seat[c] = &cands[i]; }
    for (uint32_t i = 0; i < count; ++i) {
        bool placed = false;
        for (uint32_t c = 0; c < kCandidates; ++c) placed = placed || seat[c] == &cands[i];
        for (uint32_t c = 0; !placed && c < kCandidates; ++c)
            if (!seat[c] && !next[c] && cands[i].resource) { next[c] = cands[i].id; seat[c] = &cands[i]; placed = true; }
    }
    {
        std::lock_guard<std::mutex> lock(g_stats_m);
        for (uint32_t c = 0; c < kCandidates; ++c)
            if (next[c] != current_id_[c]) { decider_.ResetCandidate(c); current_id_[c] = next[c]; ++stats_.candidate_changes; }
    }
    if (!ensure_textures(color, rw, rh)) return out;
    const D3D12_RESOURCE_DESC cd = color->GetDesc();
    if (rw > cd.Width || rh > cd.Height) return out;
    const auto NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    // Descriptors for this slot.
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slot) * kTable * stride_;
    D3D12_GPU_DESCRIPTOR_HANDLE gh = heap_->GetGPUDescriptorHandleForHeapStart();
    gh.ptr += static_cast<UINT64>(slot) * kTable * stride_;
    bool unorm[kCandidates]{};
    uint32_t live = 0;
    for (uint32_t c = 0; c < kCandidates; ++c) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        ID3D12Resource* r = seat[c] ? seat[c]->resource : nullptr;
        // A seat may be empty while the next one is filled; an empty seat views seat 0's texture
        // (the shader reads only `count` seats, and counters of an empty seat are never used).
        if (!r) r = seat[0] ? seat[0]->resource : (seat[1] ? seat[1]->resource : nullptr);
        if (!r) return out;
        sd.Format = srv_format(r->GetDesc().Format, &unorm[c]);
        device_->CreateShaderResourceView(r, &sd, D3D12_CPU_DESCRIPTOR_HANDLE{h.ptr + c * stride_});
        if (seat[c]) live = c + 1;
    }
    for (uint32_t k = 0; k < 2; ++k) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        sd.Format = srv_format(cd.Format, nullptr);
        device_->CreateShaderResourceView(k ? prev_.Get() : color, &sd, D3D12_CPU_DESCRIPTOR_HANDLE{h.ptr + (2 + k) * stride_});
    }
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = kCounterBytes / 4;
        ud.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device_->CreateUnorderedAccessView(slot_[slot].counters.Get(), nullptr, &ud, D3D12_CPU_DESCRIPTOR_HANDLE{h.ptr + 4 * stride_});
        D3D12_UNORDERED_ACCESS_VIEW_DESC td{};
        td.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        td.Format = DXGI_FORMAT_R32G32_FLOAT;
        device_->CreateUnorderedAccessView(xy_.Get(), nullptr, &td, D3D12_CPU_DESCRIPTOR_HANDLE{h.ptr + 5 * stride_});
        td.Format = DXGI_FORMAT_R32_FLOAT;
        device_->CreateUnorderedAccessView(valid_.Get(), nullptr, &td, D3D12_CPU_DESCRIPTOR_HANDLE{h.ptr + 6 * stride_});
    }

    // Candidates to NPSR (subresource 0, the only one read), and back at the end.
    D3D12_RESOURCE_BARRIER to[kCandidates], back[kCandidates];
    uint32_t nb = 0;
    for (uint32_t c = 0; c < kCandidates; ++c)
        if (seat[c] && !(seat[c]->state & NPSR)) {
            to[nb] = tr(seat[c]->resource, 0, seat[c]->state, NPSR);
            back[nb] = tr(seat[c]->resource, 0, NPSR, seat[c]->state);
            ++nb;
        }
    if (nb) list->ResourceBarrier(nb, to);

    Constants k{};
    k.w = static_cast<uint32_t>(cd.Width); k.h = cd.Height; k.sw = rw; k.sh = rh;
    k.step = kStep; k.gw = (rw - 2 + kStep - 1) / kStep; k.gh = (rh - 2 + kStep - 1) / kStep;
    k.count = live; k.unorm0 = unorm[0]; k.unorm1 = unorm[1];
    int used_c = -1, used_h = -1;
    {
        std::lock_guard<std::mutex> lock(g_stats_m);
        used_c = decider_.UsedCandidate(); used_h = decider_.UsedEncoding();
        if (force_ && used_c < 0 && decider_.LastBest() >= 0) {
            used_c = static_cast<int>(decider_.LastCandidate()); used_h = decider_.LastBest();
        }
    }
    if (used_c >= 0 && !seat[used_c]) used_c = -1;
    k.use_c = used_c >= 0 ? static_cast<uint32_t>(used_c) : 0; k.use_h = used_h >= 0 ? static_cast<uint32_t>(used_h) : 0;

    ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root_.Get());
    list->SetComputeRootDescriptorTable(0, gh);
    list->SetComputeRoot32BitConstants(1, sizeof(Constants) / 4, &k, 0);

    const bool score = prev_valid_ && !reset && live > 0;
    SlotState& s = slot_[slot];
    if (score) {
        list->CopyBufferRegion(s.counters.Get(), 0, zeros_.Get(), 0, kCounterBytes);
        D3D12_RESOURCE_BARRIER b = tr(s.counters.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, UAV);
        list->ResourceBarrier(1, &b);
        list->SetPipelineState(pso_score_.Get());
        list->Dispatch((k.gw + 7) / 8, (k.gh + 7) / 8, 1);
        b = tr(s.counters.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, UAV, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &b);
        list->CopyBufferRegion(s.readback.Get(), 0, s.counters.Get(), 0, kCounterBytes);
        b = tr(s.counters.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(1, &b);
        s.recorded = true;
        for (uint32_t c = 0; c < kCandidates; ++c) s.candidate_id[c] = seat[c] ? seat[c]->id : 0;
    }
    if (used_c >= 0) {
        D3D12_RESOURCE_BARRIER b[2] = {tr(xy_.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, NPSR, UAV),
                                       tr(valid_.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, NPSR, UAV)};
        list->ResourceBarrier(2, b);
        list->SetPipelineState(pso_convert_.Get());
        list->Dispatch((rw + 7) / 8, (rh + 7) / 8, 1);
        for (auto& x : b) std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
        list->ResourceBarrier(2, b);
        out.motion = xy_.Get();
        out.valid = valid_.Get();
    }
    if (nb) list->ResourceBarrier(nb, back);

    // This frame's colour becomes the next frame's previous picture.
    {
        D3D12_RESOURCE_BARRIER b[2] = {tr(color, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, NPSR, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                       tr(prev_.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, NPSR, D3D12_RESOURCE_STATE_COPY_DEST)};
        list->ResourceBarrier(2, b);
        list->CopyResource(prev_.Get(), color);
        for (auto& x : b) std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
        list->ResourceBarrier(2, b);
        prev_valid_ = true;
    }
    std::lock_guard<std::mutex> lock(g_stats_m);
    ++stats_.recorded;
    if (score) ++stats_.scored;
    if (out.motion) ++stats_.fed;
    return out;
}

Stats Feed::GetStats() const
{
    std::lock_guard<std::mutex> lock(g_stats_m);
    Stats s = stats_;
    s.measured = decider_.Measured(); s.good = decider_.GoodFrames();
    s.locked = decider_.Locked(); s.dropped = decider_.Dropped();
    s.used_candidate = decider_.UsedCandidate(); s.used_encoding = decider_.UsedEncoding();
    s.last_best = decider_.LastBest(); s.last_candidate = decider_.LastCandidate();
    s.last_ratio = decider_.LastRatio(); s.last_second = decider_.LastSecond();
    const Measure& m = decider_.Last();
    s.last_scored = m.scored; s.last_moving = m.moving; s.last_non_finite = m.non_finite;
    s.last_max_x = m.max_x; s.last_max_y = m.max_y;
    s.candidates = (current_id_[0] ? 1u : 0u) + (current_id_[1] ? 1u : 0u);
    return s;
}

void Feed::Describe(char* text, size_t capacity) const
{
    if (!text || !capacity) return;
    const Stats s = GetStats();
    std::snprintf(text, capacity,
                  "game_motion{cands=%u scored=%llu measured=%llu good=%llu used=%d/%s%s%s locked=%llu dropped=%llu fed=%llu "
                  "last{c=%u best=%d ratio=%.3f second=%.3f px=%llu moving=%llu nonfinite=%llu max=%.4g,%.4g}}",
                  s.candidates, (unsigned long long)s.scored, (unsigned long long)s.measured, (unsigned long long)s.good,
                  s.used_candidate, s.used_encoding >= 0 ? KindName(static_cast<uint32_t>(s.used_encoding)) : "-",
                  s.used_encoding >= 0 && (s.used_encoding & 1) ? "-x" : "", s.used_encoding >= 0 && (s.used_encoding & 2) ? "-y" : "",
                  (unsigned long long)s.locked, (unsigned long long)s.dropped, (unsigned long long)s.fed, s.last_candidate, s.last_best,
                  s.last_ratio, s.last_second, (unsigned long long)s.last_scored, (unsigned long long)s.last_moving,
                  (unsigned long long)s.last_non_finite, s.last_max_x, s.last_max_y);
}

} // namespace na::j2::game_motion
