#include "host_dx12/j2/replay_motion.hpp"

#include <d3dcompiler.h>

#include <climits>
#include <cstring>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace na::j2::replay_motion {
namespace {

const char kSource[] = R"(
ByteAddressBuffer t_cur   : register(t0);
ByteAddressBuffer t_prev  : register(t1);
Texture2D<float>  t_depth : register(t2);
cbuffer C : register(b0) {
    uint cur_first, prev_first; // per draw (float4 elements from the buffers' view start)
    uint w, h;
    float jdx, jdy;             // (previous - current) raster jitter inside the clip positions
    uint pad0, pad1;
};
struct V { float4 pos : SV_Position; float4 prev : PREV; };
V vs(uint id : SV_VertexID) {
    V o;
    o.pos = asfloat(t_cur.Load4((cur_first + id) * 16u));
    o.prev = asfloat(t_prev.Load4((prev_first + id) * 16u));
    return o;
}
struct O { float2 v : SV_Target0; float m : SV_Target1; };
O ps(V i) {
    const uint2 p = uint2(i.pos.xy);
    const float d = t_depth[p];
    const float z = i.pos.z;
    // Only where this draw is the visible surface (its depth is the scene's).
    if (abs(z - d) > max(1e-6, 0.01 * max(abs(z), abs(d)))) discard;
    if (!(i.prev.w > 1e-6)) discard;
    const float2 n = i.prev.xy / i.prev.w;
    const float2 q = float2((n.x * 0.5 + 0.5) * w, (0.5 - n.y * 0.5) * h);
    const float2 v = q - i.pos.xy - float2(jdx, jdy);
    if (!all(isfinite(v)) || dot(v, v) > 65536.0) discard;
    O o;
    o.v = v;
    o.m = 1.0;
    return o;
}
)";

struct Globals { uint32_t w, h; float jdx, jdy; uint32_t pad0, pad1; };
constexpr uint32_t kSets = 8; // descriptor sets in flight

D3D12_RESOURCE_BARRIER tr(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    return x;
}
bool texture(ID3D12Device* d, UINT w, UINT h, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES st,
             const D3D12_CLEAR_VALUE* clear, ComPtr<ID3D12Resource>& out)
{
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = f; td.SampleDesc.Count = 1; td.Flags = flags;
    return SUCCEEDED(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, st, clear, IID_PPV_ARGS(&out)));
}
void raw_srv(ID3D12Device* d, ID3D12Resource* r, uint64_t offset, D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC s{};
    s.Format = DXGI_FORMAT_R32_TYPELESS;
    s.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    s.Buffer.FirstElement = offset / 4;
    s.Buffer.NumElements = static_cast<UINT>((r->GetDesc().Width - offset) / 4);
    s.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    d->CreateShaderResourceView(r, &s, h);
}

} // namespace

std::vector<Pair> Match(const std::vector<na::observe::ReplayRecord>& cur, const std::vector<na::observe::ReplayRecord>& prev,
                        uint32_t* exact, uint32_t* loose)
{
    *exact = *loose = 0;
    std::unordered_map<uint64_t, std::vector<uint32_t>> by_sig;
    for (uint32_t i = 0; i < prev.size(); ++i) by_sig[prev[i].sig].push_back(i);
    std::unordered_map<uint64_t, uint32_t> cur_count;
    for (const auto& c : cur) ++cur_count[c.sig];
    std::vector<bool> taken(prev.size(), false);
    std::vector<Pair> out;
    out.reserve(cur.size());
    std::vector<uint32_t> pending;
    for (uint32_t ci = 0; ci < cur.size(); ++ci) {
        const auto& c = cur[ci];
        const auto it = by_sig.find(c.sig);
        if (it == by_sig.end()) continue;
        bool done = false;
        for (uint32_t pi : it->second)
            if (!taken[pi] && prev[pi].vb == c.vb && prev[pi].verts == c.verts) {
                taken[pi] = true; out.push_back({c.first, prev[pi].first, c.verts}); ++*exact; done = true; break;
            }
        if (!done) pending.push_back(ci);
    }
    // Buffers moved (double-buffered or posed outside the vertex shader): only a signature that is
    // unique in both frames is safe; two identical meshes could otherwise swap.
    for (uint32_t ci : pending) {
        const auto& c = cur[ci];
        const auto& v = by_sig[c.sig];
        if (v.size() != 1 || cur_count[c.sig] != 1 || taken[v[0]] || prev[v[0]].verts != c.verts) continue;
        taken[v[0]] = true;
        out.push_back({c.first, prev[v[0]].first, c.verts});
        ++*loose;
    }
    return out;
}

bool Pass::Init(ID3D12Device* device)
{
    if (!device) return false;
    device_ = device;
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors = 3;
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &range;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = 2 + sizeof(Globals) / 4;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 2; rs.pParameters = params;
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) return false;
    if (FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root_)))) return false;
    const UINT cf = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS;
    ComPtr<ID3DBlob> vs, ps;
    if (FAILED(D3DCompile(kSource, sizeof(kSource) - 1, "replay_motion.hlsl", nullptr, nullptr, "vs", "vs_5_0", cf, 0, &vs, &err))) return false;
    if (FAILED(D3DCompile(kSource, sizeof(kSource) - 1, "replay_motion.hlsl", nullptr, nullptr, "ps", "ps_5_0", cf, 0, &ps, &err))) return false;
    for (int rev = 0; rev < 2; ++rev) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        d.pRootSignature = root_.Get();
        d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        for (auto& b : d.BlendState.RenderTarget) b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        d.SampleMask = UINT_MAX;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        d.RasterizerState.DepthClipEnable = TRUE;
        d.DepthStencilState.DepthEnable = TRUE;
        d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        d.DepthStencilState.DepthFunc = rev ? D3D12_COMPARISON_FUNC_GREATER_EQUAL : D3D12_COMPARISON_FUNC_LESS_EQUAL;
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = 2;
        d.RTVFormats[0] = DXGI_FORMAT_R32G32_FLOAT;
        d.RTVFormats[1] = DXGI_FORMAT_R32_FLOAT;
        d.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        d.SampleDesc.Count = 1;
        if (FAILED(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso_[rev])))) { pso_[0].Reset(); return false; }
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 3 * kSets; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srv_)))) { pso_[0].Reset(); return false; }
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 2; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_)))) { pso_[0].Reset(); return false; }
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; hd.NumDescriptors = 1;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsv_)))) { pso_[0].Reset(); return false; }
    srv_stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    rtv_stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    // Committed resources start zeroed: the source of the counter reset.
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 1u << 20; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&zeros_)))) {
        pso_[0].Reset(); return false;
    }
    return true;
}

bool Pass::ensure(uint32_t rw, uint32_t rh)
{
    if (motion_ && tw_ == rw && th_ == rh) return true;
    motion_.Reset(); valid_.Reset(); zdepth_.Reset();
    const auto NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_CLEAR_VALUE c0{}; c0.Format = DXGI_FORMAT_R32G32_FLOAT;
    D3D12_CLEAR_VALUE c1{}; c1.Format = DXGI_FORMAT_R32_FLOAT;
    if (!texture(device_.Get(), rw, rh, DXGI_FORMAT_R32G32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, NPSR, &c0, motion_) ||
        !texture(device_.Get(), rw, rh, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, NPSR, &c1, valid_) ||
        !texture(device_.Get(), rw, rh, DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE,
                 D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, zdepth_)) {
        motion_.Reset(); valid_.Reset(); zdepth_.Reset();
        return false;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE r = rtv_->GetCPUDescriptorHandleForHeapStart();
    device_->CreateRenderTargetView(motion_.Get(), nullptr, r);
    r.ptr += rtv_stride_;
    device_->CreateRenderTargetView(valid_.Get(), nullptr, r);
    device_->CreateDepthStencilView(zdepth_.Get(), nullptr, dsv_->GetCPUDescriptorHandleForHeapStart());
    tw_ = rw; th_ = rh;
    return true;
}

Output Pass::Record(ID3D12GraphicsCommandList* l, ID3D12Resource* depth, uint32_t rw, uint32_t rh, bool reversed, bool reset,
                    const float jitter_delta[2])
{
    Output out;
    if (!Ready() || !l || !depth || !rw || !rh) return out;
    na::observe::ReplayFrame f;
    if (!na::observe::ReplayTakeFrame(&f)) return out;
    ++stats_.frames;
    // Next frame's stream-output counters start at zero (the slot decayed to COMMON: promoted to
    // COPY_DEST here, then handed to the game's lists as STREAM_OUT).
    if (f.next && f.counter_bytes <= zeros_->GetDesc().Width) {
        l->CopyBufferRegion(f.next, 0, zeros_.Get(), 0, f.counter_bytes);
        const D3D12_RESOURCE_BARRIER b = tr(f.next, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
        l->ResourceBarrier(1, &b);
        na::observe::ReplaySlotZeroed(f.next_slot);
    }
    stats_.draws += f.cur_draws.size();
    stats_.prev_draws += f.prev_draws.size();
    stats_.last_draws = static_cast<uint32_t>(f.cur_draws.size());
    stats_.last_pairs = 0;
    if (reset || !f.prev || f.cur_draws.empty() || f.prev_draws.empty()) return out;
    uint32_t exact = 0, loose = 0;
    const std::vector<Pair> pairs = Match(f.cur_draws, f.prev_draws, &exact, &loose);
    stats_.matched_exact += exact; stats_.matched_loose += loose;
    stats_.unmatched += f.cur_draws.size() - pairs.size();
    stats_.last_pairs = static_cast<uint32_t>(pairs.size());
    if (pairs.empty() || !ensure(rw, rh)) return out;

    const auto NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto PSR = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const auto RT = D3D12_RESOURCE_STATE_RENDER_TARGET;
    const auto SO = D3D12_RESOURCE_STATE_STREAM_OUT;
    // cur was written by this frame's lists (STREAM_OUT); prev decayed to COMMON after the frame
    // before and is promoted to a read state here.
    D3D12_RESOURCE_BARRIER b[4] = {tr(f.cur, SO, NPSR), tr(depth, NPSR, NPSR | PSR), tr(motion_.Get(), NPSR, RT), tr(valid_.Get(), NPSR, RT)};
    l->ResourceBarrier(4, b);

    static uint32_t set = 0;
    set = (set + 1) % kSets;
    D3D12_CPU_DESCRIPTOR_HANDLE h = srv_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(set) * 3 * srv_stride_;
    D3D12_GPU_DESCRIPTOR_HANDLE gh = srv_->GetGPUDescriptorHandleForHeapStart();
    gh.ptr += static_cast<UINT64>(set) * 3 * srv_stride_;
    raw_srv(device_.Get(), f.cur, f.data_offset, h);
    h.ptr += srv_stride_;
    raw_srv(device_.Get(), f.prev, f.data_offset, h);
    h.ptr += srv_stride_;
    D3D12_SHADER_RESOURCE_VIEW_DESC ds{};
    ds.Format = DXGI_FORMAT_R32_FLOAT;
    ds.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    ds.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    ds.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(depth, &ds, h);

    D3D12_CPU_DESCRIPTOR_HANDLE rt[2] = {rtv_->GetCPUDescriptorHandleForHeapStart(), rtv_->GetCPUDescriptorHandleForHeapStart()};
    rt[1].ptr += rtv_stride_;
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_->GetCPUDescriptorHandleForHeapStart();
    const float zero[4] = {0, 0, 0, 0};
    l->ClearRenderTargetView(rt[0], zero, 0, nullptr);
    l->ClearRenderTargetView(rt[1], zero, 0, nullptr);
    l->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, reversed ? 0.f : 1.f, 0, 0, nullptr);
    ID3D12DescriptorHeap* heaps[1] = {srv_.Get()};
    l->SetDescriptorHeaps(1, heaps);
    l->SetGraphicsRootSignature(root_.Get());
    l->SetPipelineState(pso_[reversed ? 1 : 0].Get());
    l->SetGraphicsRootDescriptorTable(0, gh);
    const Globals g{rw, rh, jitter_delta ? jitter_delta[0] : 0.f, jitter_delta ? jitter_delta[1] : 0.f, 0, 0};
    l->SetGraphicsRoot32BitConstants(1, sizeof(Globals) / 4, &g, 2);
    const D3D12_VIEWPORT vp{0.f, 0.f, static_cast<float>(rw), static_cast<float>(rh), 0.f, 1.f};
    const D3D12_RECT sc{0, 0, static_cast<LONG>(rw), static_cast<LONG>(rh)};
    l->RSSetViewports(1, &vp);
    l->RSSetScissorRects(1, &sc);
    l->OMSetRenderTargets(2, rt, FALSE, &dsv);
    l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (const Pair& p : pairs) {
        const uint32_t firsts[2] = {p.cur_first, p.prev_first};
        l->SetGraphicsRoot32BitConstants(1, 2, firsts, 0);
        l->DrawInstanced(p.verts, 1, 0, 0);
        stats_.verts += p.verts;
    }
    D3D12_RESOURCE_BARRIER a[4] = {tr(f.cur, NPSR, SO), tr(depth, NPSR | PSR, NPSR), tr(motion_.Get(), RT, NPSR), tr(valid_.Get(), RT, NPSR)};
    l->ResourceBarrier(4, a);
    ++stats_.fed;
    out.motion = motion_.Get();
    out.valid = valid_.Get();
    return out;
}

} // namespace na::j2::replay_motion
