#include "host_dx12/j2/replay_motion.hpp"

#include <d3dcompiler.h>

#include <climits>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace na::j2::replay_motion {
namespace {

const char kSource[] = R"(
ByteAddressBuffer t_cur   : register(t0);
ByteAddressBuffer t_prev  : register(t1);
Texture2D<float>  t_depth : register(t2);
ByteAddressBuffer t_stamp : register(t3);
cbuffer C : register(b0) {
    uint cur_first, prev_first; // per draw (float4 elements from the buffers' view start)
    uint check, token;          // per draw: check != 0 -> positions valid only if stamp[check-1] == token
    uint prev_check, prev_token; // same for the previous frame's positions
    uint w, h;
    float jx, jy;               // current viewport jitter, absent from the saved clip positions
    uint pad0, pad1;
};
struct V { float4 pos : SV_Position; float4 prev : PREV; };
V vs(uint id : SV_VertexID) {
    V o;
    // The writer list ran after this one (or never): its positions are stale.
    // Clip the whole draw (z < 0) so it leaves no motion and no mask.
    if ((check != 0u && t_stamp.Load((check - 1u) * 4u) != token) ||
        (prev_check != 0u && t_stamp.Load((prev_check - 1u) * 4u) != prev_token)) {
        o.pos = float4(0, 0, -1, 1);
        o.prev = float4(0, 0, 0, 0);
        return o;
    }
    o.pos = asfloat(t_cur.Load4((cur_first + id) * 16u));
    o.prev = asfloat(t_prev.Load4((prev_first + id) * 16u));
    return o;
}
RWByteAddressBuffer u_count : register(u2);
struct O { float2 v : SV_Target0; float m : SV_Target1; };
O ps(V i) {
    const uint2 p = uint2(i.pos.xy);
    const float d = t_depth[p];
    const float z = i.pos.z;
    // Only where this draw is the visible surface (its depth is the scene's).
    if (!isfinite(d) || abs(z - d) > 2e-6) discard;
    if (!(i.prev.w > 1e-6)) discard;
    const float2 n = i.prev.xy / i.prev.w;
    const float2 q = float2((n.x * 0.5 + 0.5) * w, (0.5 - n.y * 0.5) * h);
    const float2 v = q - (i.pos.xy - float2(jx, jy));
    if (!all(isfinite(v)) || dot(v, v) > 65536.0) discard;
    O o;
    o.v = v;
    o.m = 1.0;
    if (((p.x | p.y) & 7u) == 0u) u_count.InterlockedAdd(0, 1u);
    return o;
}
)";

struct Globals { uint32_t w, h; float jx, jy; uint32_t pad0, pad1; };


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

std::vector<Pair> Match(const std::vector<na::observe::ReplayRecord>& cur,
                        const std::vector<na::observe::ReplayRecord>& prev, uint32_t width, uint32_t height)
{
    using Record = na::observe::ReplayRecord;
    struct Key {
        uint64_t sig, vb;
        bool operator==(const Key& other) const { return sig == other.sig && vb == other.vb; }
    };
    struct Hash {
        size_t operator()(const Key& key) const {
            return static_cast<size_t>(key.sig ^ (key.vb * 0x9e3779b97f4a7c15ull));
        }
    };
    const auto index = [&](const std::vector<Record>& records) {
        std::unordered_map<Key, const Record*, Hash> entries;
        entries.reserve(records.size());
        for (const auto& draw : records) {
            if (draw.width != width || draw.height != height) continue;
            const auto [it, inserted] = entries.emplace(Key{draw.sig, draw.vb}, &draw);
            if (!inserted) it->second = nullptr;
        }
        return entries;
    };
    const auto now = index(cur);
    const auto prior = index(prev);
    std::vector<Pair> out;
    out.reserve(now.size());
    for (const auto& draw : cur) {
        const Key key{draw.sig, draw.vb};
        const auto c = now.find(key);
        if (c == now.end() || c->second != &draw) continue;
        const auto p = prior.find(key);
        if (p == prior.end() || !p->second || p->second->verts != draw.verts) continue;
        out.push_back({draw.first, p->second->first, draw.verts,
                       {draw.scissor[0], draw.scissor[1], draw.scissor[2], draw.scissor[3]}, draw.check, draw.token,
                       p->second->check, p->second->token});
    }
    return out;
}

bool Pass::Init(ID3D12Device* device)
{
    if (!device) return false;
    device_ = device;
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors = 4;
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &range;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = 6 + sizeof(Globals) / 4;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].Descriptor.ShaderRegister = 2;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 3; rs.pParameters = params;
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
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 4 * kSets; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srv_)))) { pso_[0].Reset(); return false; }
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 2; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_)))) { pso_[0].Reset(); return false; }
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; hd.NumDescriptors = 1;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsv_)))) { pso_[0].Reset(); return false; }
    srv_stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    {
        D3D12_HEAP_PROPERTIES hp{}; D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 256; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&zeros_)))) { pso_[0].Reset(); return false; }
        void* z = nullptr;
        if (FAILED(zeros_->Map(0, nullptr, &z))) { pso_[0].Reset(); return false; }
        std::memset(z, 0, 256);
        zeros_->Unmap(0, nullptr);
        for (uint32_t i = 0; i < kSets; ++i) {
            hp.Type = D3D12_HEAP_TYPE_DEFAULT; bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&count_[i])))) { pso_[0].Reset(); return false; }
            hp.Type = D3D12_HEAP_TYPE_READBACK; bd.Flags = D3D12_RESOURCE_FLAG_NONE;
            if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback_[i])))) { pso_[0].Reset(); return false; }
        }
    }
    rtv_stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
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

void Pass::harvest()
{
    for (uint32_t i = 0; i < kSets; ++i) {
        if (!pending_read_[i] || !descriptor_use_[i] || !descriptor_use_[i].Completed()) continue;
        pending_read_[i] = false;
        const D3D12_RANGE range{0, 4};
        void* m = nullptr;
        if (FAILED(readback_[i]->Map(0, &range, &m)) || !m) continue;
        uint32_t v = 0;
        std::memcpy(&v, m, 4);
        const D3D12_RANGE none{0, 0};
        readback_[i]->Unmap(0, &none);
        ++stats_.gpu_read;
        if (v) ++stats_.gpu_nonzero;
        stats_.gpu_samples += v;
        stats_.gpu_last = v;
    }
}

Output Pass::Record(ID3D12GraphicsCommandList* l, ID3D12CommandQueue* q, ID3D12Resource* depth, uint32_t rw, uint32_t rh, bool reversed, bool reset,
                    const float viewport_jitter[2])
{
    Output out;
    if (!Ready() || !l || !depth || !rw || !rh) return out;
    harvest();
    na::observe::ReplayFrame f;
    if (!na::observe::ReplayTakeFrame(l, q, &f)) return out;
    ++stats_.frames;
    stats_.draws += f.cur_draws.size();
    stats_.prev_draws += f.prev_draws.size();
    stats_.last_draws = static_cast<uint32_t>(f.cur_draws.size());
    stats_.last_pairs = 0;
    if (reset || !f.prev || f.cur_draws.empty() || f.prev_draws.empty()) return out;
    const std::vector<Pair> pairs = Match(f.cur_draws, f.prev_draws, rw, rh);
    stats_.matched_exact += pairs.size();
    stats_.unmatched += f.cur_draws.size() - pairs.size();
    stats_.last_pairs = static_cast<uint32_t>(pairs.size());
    if (pairs.empty()) return out;
    if (!f.stamps) for (const Pair& p : pairs) if (p.check || p.prev_check) return out;
    uint32_t set = 0;
    while (set < kSets && descriptor_use_[set] && !descriptor_use_[set].Retired()) ++set;
    if (set == kSets || !ensure(rw, rh)) return out;
    // Recordings can outlive resize or execute again. The descriptor page and
    // every resource it names live through retirement, including Reset/discard.
    auto resources = std::make_shared<std::vector<ComPtr<IUnknown>>>();
    resources->assign({srv_, rtv_, dsv_, motion_, valid_, zdepth_, root_,
                       pso_[reversed ? 1 : 0], ComPtr<IUnknown>(depth), count_[set], readback_[set], zeros_});
    pending_read_[set] = false; // an unread result of a retired, never-completed use is lost
    auto lease = na::nr::native::RetainRecording(l, resources);
    if (!lease) return out;
    descriptor_use_[set] = std::move(lease);

    const auto NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto PSR = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const auto RT = D3D12_RESOURCE_STATE_RENDER_TARGET;
    const auto SO = D3D12_RESOURCE_STATE_STREAM_OUT;
    // cur was written by this frame's lists (STREAM_OUT in this list when it wrote some itself);
    // prev and other lists' positions are in COMMON (writers leave it there at Close).
    // cur is in STREAM_OUT only when this list wrote positions itself; positions
    // of other (already submitted) lists are in COMMON and promote to a read state.
    const auto COMMON = D3D12_RESOURCE_STATE_COMMON;
    // stamps: COMMON between uses (writers stamp at Close via COPY_DEST, even in this list later).
    // Positions from other lists get an explicit transition (not implicit
    // promotion): lists in one ExecuteCommandLists may overlap, and the
    // writer ends with an explicit STREAM_OUT -> COMMON, so the pair orders
    // its stream-output writes before these reads (as the game's own depth
    // hand-over between its lists does).
    std::vector<D3D12_RESOURCE_BARRIER> b = {tr(depth, NPSR, NPSR | PSR), tr(motion_.Get(), NPSR, RT), tr(valid_.Get(), NPSR, RT),
                                             tr(f.cur, f.cur_in_list ? SO : COMMON, NPSR), tr(f.prev, COMMON, NPSR)};
    if (f.stamps) b.push_back(tr(f.stamps, COMMON, NPSR));
    l->ResourceBarrier(static_cast<UINT>(b.size()), b.data());
    l->CopyBufferRegion(count_[set].Get(), 0, zeros_.Get(), 0, 4);
    {
        const D3D12_RESOURCE_BARRIER c = tr(count_[set].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        l->ResourceBarrier(1, &c);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE h = srv_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(set) * 4 * srv_stride_;
    D3D12_GPU_DESCRIPTOR_HANDLE gh = srv_->GetGPUDescriptorHandleForHeapStart();
    gh.ptr += static_cast<UINT64>(set) * 4 * srv_stride_;
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
    h.ptr += srv_stride_;
    if (f.stamps) raw_srv(device_.Get(), f.stamps, 0, h);
    else raw_srv(device_.Get(), f.cur, f.data_offset, h); // never read: no pair has a check

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
    l->SetGraphicsRootUnorderedAccessView(2, count_[set]->GetGPUVirtualAddress());
    const Globals g{rw, rh, viewport_jitter ? viewport_jitter[0] : 0.f, viewport_jitter ? viewport_jitter[1] : 0.f, 0, 0};
    l->SetGraphicsRoot32BitConstants(1, sizeof(Globals) / 4, &g, 6);
    const D3D12_VIEWPORT vp{g.jx, g.jy, static_cast<float>(rw), static_cast<float>(rh), 0.f, 1.f};
    l->RSSetViewports(1, &vp);
    l->OMSetRenderTargets(2, rt, FALSE, &dsv);
    l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (const Pair& p : pairs) {
        l->RSSetScissorRects(1, &p.scissor);
        const uint32_t per_draw[6] = {p.cur_first, p.prev_first, p.check, p.token, p.prev_check, p.prev_token};
        l->SetGraphicsRoot32BitConstants(1, 6, per_draw, 0);
        l->DrawInstanced(p.verts, 1, 0, 0);
        stats_.verts += p.verts;
        if (p.check || p.prev_check) ++stats_.checked;
    }
    for (auto& x : b) std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
    b.push_back(tr(count_[set].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE));
    l->ResourceBarrier(static_cast<UINT>(b.size()), b.data());
    l->CopyBufferRegion(readback_[set].Get(), 0, count_[set].Get(), 0, 4);
    {
        const D3D12_RESOURCE_BARRIER c = tr(count_[set].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        l->ResourceBarrier(1, &c);
    }
    pending_read_[set] = true;
    ++stats_.fed;
    out.motion = motion_.Get();
    out.valid = valid_.Get();
    return out;
}

} // namespace na::j2::replay_motion
