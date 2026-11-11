// Optical-flow object motion for the DLSS helper. See helper_flow.hpp.
// The pass order, constants, gates and resource formats follow the DX12 host's
// compute_flow.cpp line for line; only the lifetime handling differs (the
// helper's ring slots instead of recording leases).
#include "bridge/helper_flow.hpp"

#include "core/motion_arbiter_gpu.hpp"
#include "host_dx12/ffx_flow/ffx_flow.hpp"
#include "host_dx12/ffx_flow/ffx_flow_shaders.hpp"
#include "host_dx12/j2/flow_shaders.hpp"
#include "host_dx12/nvof/flow_expand.hpp"

#include <d3dcompiler.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace na::bridge::flow {
using Microsoft::WRL::ComPtr;

namespace {
constexpr auto S = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, U = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr unsigned kTable = 12, kUav = 8; // descriptors per table: SRVs t0..t7, then UAVs u0..u3

// Same layout as compute_flow.cpp's Constants (the j2 kFlowShader cbuffer).
struct Constants {
    unsigned w, h, ew, eh, mode, block, cost, cost_max, offset, gw, gh;
    float ratio = .5f;
    float snap = .5f, margin = .02f, absolute = .08f, texture = .03f;
    unsigned have_hist = 0, agree_min = 1;
    float dj_x = 0, dj_y = 0;
    float agree_px = 3.f, agree_cos = .5f, jitter_x = 0, jitter_y = 0;
    float history_dx = 0, history_dy = 0, pad0 = 0, pad1 = 0;
};
static_assert(sizeof(Constants) == 28 * 4, "root constants");

// compute_flow::GatesFor, unchanged (Cort 2026-11-03: 0 = the Anno-run gates).
struct Gates { float image_ratio, image_abs, texture_min, agree_px, agree_cos, consistency_px; uint32_t agree_min; bool sky; };
Gates GatesFor(uint32_t percent)
{
    const float k = float(percent > 100 ? 100 : percent) / 100.f;
    Gates g{};
    g.image_ratio = .5f + .5f * k;
    g.image_abs = .08f + .12f * k;
    g.texture_min = .03f - .02f * k;
    g.agree_px = 3.f + 3.f * k;
    g.agree_cos = .5f - .4f * k;
    g.agree_min = k >= .5f ? 0u : 1u;
    g.consistency_px = 2.f + 2.f * k;
    g.sky = k > 0.f;
    return g;
}

DXGI_FORMAT Typed(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default: return f;
    }
}

void Move(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
    l->ResourceBarrier(1, &x);
}
void Order(ID3D12GraphicsCommandList* l)
{
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    l->ResourceBarrier(1, &x);
}
} // namespace

// Everything that depends on the render size. Replaced (never re-initialised)
// when the size or colour format changes; old ones are kept until the GPU can
// no longer use them.
struct Sized {
    unsigned w = 0, h = 0;
    DXGI_FORMAT color_format = DXGI_FORMAT_UNKNOWN;
    na::ffx_flow::PairFlow engine;
    ComPtr<ID3D12Resource> image[2], color_copy;
    ComPtr<ID3D12Resource> raw, expanded, residual, valid, mask, hist[2];
    uint64_t retired_at = 0; // record count when replaced
};

struct Slot {
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12Resource> readback;
    bool encoded = false, pair = false, pending = false;
    int image = -1, hist_out = -1;
    uint64_t frame = 0;
    float jitter[2] = {};
};

struct HelperFlow::Impl {
    ComPtr<ID3D12Device> device;
    unsigned slots = 0, stride = 0;
    bool ready = false;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso_encode, pso_accept;
    ComPtr<ID3D12Resource> counters, zero;
    na::nvof::FlowExpand expand;
    na::motion::gpu::Arbiter derive;
    std::vector<Slot> slot;
    std::unique_ptr<Sized> sized;
    std::vector<std::unique_ptr<Sized>> old;
    // History: the last submitted encoded image, and the last submitted pair's candidates.
    int prev_image = -1;
    uint64_t prev_frame = 0;
    float prev_jitter[2] = {};
    bool have_prev = false;
    unsigned hist_read = 0;
    uint64_t hist_frame = 0;
    bool have_hist = false;
    Stats stats;

    void reason(const char* r) { std::snprintf(stats.reason, sizeof(stats.reason), "%s", r); }

    bool tex(unsigned w, unsigned h, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& r)
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w;
        d.Height = h;
        d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Format = f;
        d.Flags = flags;
        return SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)));
    }
    bool buffer(unsigned bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& r)
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = type;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = bytes;
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (type == D3D12_HEAP_TYPE_DEFAULT) d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)));
    }
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(unsigned s, unsigned table, unsigned i)
    {
        auto a = slot[s].heap->GetCPUDescriptorHandleForHeapStart();
        a.ptr += SIZE_T(table * kTable + i) * stride;
        return a;
    }
    void srv(unsigned s, unsigned table, unsigned i, ID3D12Resource* r, DXGI_FORMAT f)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.Format = f;
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(r, &d, cpu(s, table, i));
    }
    void uav(unsigned s, unsigned table, unsigned i, ID3D12Resource* r, DXGI_FORMAT f)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
        d.Format = f;
        d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(r, nullptr, &d, cpu(s, table, i));
    }
    // compute_flow's defaults(): null views of the right types, and the counters.
    void defaults(unsigned s, unsigned table)
    {
        srv(s, table, 0, nullptr, DXGI_FORMAT_R8G8B8A8_UNORM);
        srv(s, table, 1, nullptr, DXGI_FORMAT_R32G32_FLOAT);
        srv(s, table, 2, nullptr, DXGI_FORMAT_R32G32_FLOAT);
        srv(s, table, 3, nullptr, DXGI_FORMAT_R32_FLOAT);
        srv(s, table, 4, nullptr, DXGI_FORMAT_R8_UINT);
        srv(s, table, 5, nullptr, DXGI_FORMAT_R8G8B8A8_UNORM);
        srv(s, table, 6, nullptr, DXGI_FORMAT_R8G8B8A8_UNORM);
        srv(s, table, 7, nullptr, DXGI_FORMAT_R16G16B16A16_FLOAT);
        uav(s, table, kUav + 0, nullptr, DXGI_FORMAT_R8G8B8A8_UNORM);
        uav(s, table, kUav + 1, nullptr, DXGI_FORMAT_R32_FLOAT);
        uav(s, table, kUav + 3, nullptr, DXGI_FORMAT_R8G8B8A8_UNORM);
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
        d.Format = DXGI_FORMAT_R32_TYPELESS;
        d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        d.Buffer.NumElements = 12;
        d.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(counters.Get(), nullptr, &d, cpu(s, table, kUav + 2));
    }
    void dispatch(ID3D12GraphicsCommandList* l, unsigned s, unsigned table, ID3D12PipelineState* pso, const Constants& c, unsigned w, unsigned h)
    {
        ID3D12DescriptorHeap* heaps[] = {slot[s].heap.Get()};
        l->SetDescriptorHeaps(1, heaps);
        l->SetComputeRootSignature(root.Get());
        l->SetPipelineState(pso);
        auto g = slot[s].heap->GetGPUDescriptorHandleForHeapStart();
        g.ptr += UINT64(table * kTable) * stride;
        l->SetComputeRootDescriptorTable(0, g);
        l->SetComputeRoot32BitConstants(1, sizeof(Constants) / 4, &c, 0);
        l->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    }

    // compute_flow_programs.inc: the two j2 kFlowShader entry points, one root signature.
    bool programs()
    {
        ComPtr<ID3DBlob> cs_encode, cs_accept, err;
        if (FAILED(D3DCompile(na::j2::kFlowShader, sizeof na::j2::kFlowShader - 1, "j2_flow", nullptr, nullptr, "encode", "cs_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs_encode, &err)) ||
            FAILED(D3DCompile(na::j2::kFlowShader, sizeof na::j2::kFlowShader - 1, "j2_flow", nullptr, nullptr, "accept", "cs_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs_accept, &err)))
            return false;
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 8;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 4;
        ranges[1].OffsetInDescriptorsFromTableStart = 8;
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
        D3D12_ROOT_SIGNATURE_DESC rd{};
        rd.NumParameters = 2;
        rd.pParameters = params;
        rd.NumStaticSamplers = 1;
        rd.pStaticSamplers = &smp;
        ComPtr<ID3DBlob> blob;
        if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
            FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root))))
            return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = root.Get();
        pd.CS = {cs_encode->GetBufferPointer(), cs_encode->GetBufferSize()};
        if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso_encode)))) return false;
        pd.CS = {cs_accept->GetBufferPointer(), cs_accept->GetBufferSize()};
        return SUCCEEDED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso_accept)));
    }

    std::unique_ptr<Sized> build(unsigned w, unsigned h, DXGI_FORMAT color_format, bool need_copy)
    {
        auto z = std::make_unique<Sized>();
        z->w = w;
        z->h = h;
        z->color_format = color_format;
        if (!z->engine.Init(device.Get(), w, h, slots)) {
            reason(z->engine.Error());
            return nullptr;
        }
        const auto uf = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        bool ok = tex(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, uf, S, z->image[0]) && tex(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, uf, S, z->image[1]) &&
                  tex(w, h, DXGI_FORMAT_R32G32_FLOAT, uf, S, z->raw) && tex(w, h, DXGI_FORMAT_R32_FLOAT, uf, S, z->expanded) &&
                  tex(w, h, DXGI_FORMAT_R32G32_FLOAT, uf, S, z->residual) && tex(w, h, DXGI_FORMAT_R32_FLOAT, uf, S, z->valid) &&
                  tex(w, h, DXGI_FORMAT_R32_FLOAT, uf, S, z->mask) && tex(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, uf, S, z->hist[0]) &&
                  tex(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, uf, S, z->hist[1]);
        // The shared colour texture is the session's maximum render size; the encode
        // samples normalised coordinates, so a smaller frame is copied out first.
        if (ok && need_copy) ok = tex(w, h, color_format, D3D12_RESOURCE_FLAG_NONE, S, z->color_copy);
        if (!ok) {
            reason("The helper could not make its optical-flow textures.");
            return nullptr;
        }
        return z;
    }
};

HelperFlow::HelperFlow() : p_(new Impl) {}
HelperFlow::~HelperFlow() = default;
bool HelperFlow::Ready() const { return p_->ready; }
const Stats& HelperFlow::GetStats() const { return p_->stats; }

bool HelperFlow::Init(ID3D12Device* device, uint32_t slots)
{
    Impl& g = *p_;
    if (!device || !slots || slots > 8) return false;
    g.device = device;
    g.slots = slots;
    g.stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (!na::ffx_flow::Built()) {
        g.reason("This build has no AMD optical-flow shaders (no DXC at build time).");
        return false;
    }
    if (!na::ffx_flow::DeviceSupported(device)) {
        g.reason("The GPU cannot run AMD optical flow (Shader Model 6.2 with 32/64-lane waves).");
        return false;
    }
    if (!g.programs() || !g.expand.Init(device, slots) || !g.derive.Init(device, slots)) {
        g.reason("The helper could not build its optical-flow passes.");
        return false;
    }
    if (!g.buffer(48, D3D12_HEAP_TYPE_DEFAULT, U, g.counters) ||
        !g.buffer(48, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, g.zero)) {
        g.reason("The helper could not make its optical-flow counters.");
        return false;
    }
    void* m = nullptr;
    if (FAILED(g.zero->Map(0, nullptr, &m))) return false;
    std::memset(m, 0, 48);
    g.zero->Unmap(0, nullptr);
    g.slot.resize(slots);
    for (auto& s : g.slot) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2 * kTable, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s.heap))) ||
            !g.buffer(64, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, s.readback)) {
            g.reason("The helper could not make its optical-flow descriptors.");
            return false;
        }
    }
    g.expand.SetVectorScale(32.f); // AMD outputs integral pixels; FlowExpand decodes NVOFA S10.5 (/32)
    g.ready = true;
    g.stats.engine = 1;
    g.reason("AMD optical flow ready.");
    return true;
}

void HelperFlow::Collect(uint32_t index)
{
    Impl& g = *p_;
    if (!g.ready || index >= g.slot.size()) return;
    Slot& s = g.slot[index];
    if (!s.pending || !s.pair) {
        s.pending = false;
        return;
    }
    s.pending = false;
    void* m = nullptr;
    D3D12_RANGE range{0, 40};
    if (SUCCEEDED(s.readback->Map(0, &range, &m))) {
        const auto* v = static_cast<const uint32_t*>(m);
        for (unsigned j = 0; j < 7; ++j) {
            g.stats.last[j] = v[j];
            g.stats.sum[j] += v[j];
        }
        ++g.stats.sum_pairs;
        D3D12_RANGE none{};
        s.readback->Unmap(0, &none);
    }
}

bool HelperFlow::Record(ID3D12GraphicsCommandList* l, uint32_t index, const Frame& f, const na::motion::gpu::Inputs& camera, Tier3& out)
{
    Impl& g = *p_;
    out = {};
    if (!g.ready || !l || index >= g.slot.size() || !f.color || !f.depth || !f.w || !f.h) return false;
    Slot& s = g.slot[index];
    s.encoded = s.pair = s.pending = false;
    s.hist_out = -1;
    ++g.stats.recorded;
    // Retire replaced size sets once every ring slot has been reused since.
    for (size_t i = 0; i < g.old.size();)
        if (g.stats.recorded > g.old[i]->retired_at + g.slots + 1) g.old.erase(g.old.begin() + static_cast<long>(i));
        else ++i;

    const D3D12_RESOURCE_DESC cd = f.color->GetDesc();
    const DXGI_FORMAT cf = Typed(cd.Format);
    const bool need_copy = cd.Width != f.w || cd.Height != f.h;
    if (!g.sized || g.sized->w != f.w || g.sized->h != f.h || g.sized->color_format != cf || (need_copy && !g.sized->color_copy)) {
        if (g.sized) {
            g.sized->retired_at = g.stats.recorded;
            g.old.push_back(std::move(g.sized));
        }
        g.sized = g.build(f.w, f.h, cf, need_copy);
        g.have_prev = g.have_hist = false;
        ++g.stats.resets;
        if (!g.sized) {
            ++g.stats.unavailable;
            return false;
        }
    }
    Sized& z = *g.sized;
    const unsigned w = f.w, h = f.h;

    // Colour region -> its own texture when the shared one is larger.
    ID3D12Resource* color = f.color;
    if (need_copy) {
        Move(l, f.color, S, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Move(l, z.color_copy.Get(), S, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = z.color_copy.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = f.color;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const D3D12_BOX box{0, 0, 0, w, h, 1};
        l->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
        Move(l, f.color, D3D12_RESOURCE_STATE_COPY_SOURCE, S);
        Move(l, z.color_copy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, S);
        color = z.color_copy.Get();
    }

    const Gates gates = GatesFor(f.loosen);
    Constants cb{};
    cb.ratio = gates.image_ratio;
    cb.absolute = gates.image_abs;
    cb.texture = gates.texture_min;
    cb.agree_px = gates.agree_px;
    cb.agree_cos = gates.agree_cos;
    cb.agree_min = gates.agree_min;
    cb.w = cb.ew = w;
    cb.h = cb.eh = h;
    cb.mode = f.hdr ? 1u : 0u;
    cb.block = 8;
    cb.gw = (w + 7) / 8;
    cb.gh = (h + 7) / 8;
    // Jitter-stabilised encode: our hooks add the jitter themselves, so it is known
    // exactly (the DX12 host's "raster confirmed" case): sample texel q at q + jitter.
    cb.jitter_x = f.jitter[0];
    cb.jitter_y = f.jitter[1];

    // 1. Encode this frame's colour (kept as the next frame's previous image).
    const int cur = g.prev_image == 0 ? 1 : 0;
    ID3D12Resource* im = z.image[cur].Get();
    Move(l, im, S, U);
    g.defaults(index, 0);
    g.srv(index, 0, 0, color, cf);
    g.uav(index, 0, kUav + 0, im, DXGI_FORMAT_R8G8B8A8_UNORM);
    g.dispatch(l, index, 0, g.pso_encode.Get(), cb, w, h);
    Order(l);
    Move(l, im, U, S);
    s.encoded = true;
    s.image = cur;
    s.frame = f.frame;
    s.jitter[0] = f.jitter[0];
    s.jitter[1] = f.jitter[1];

    const bool pair = g.have_prev && !f.reset && g.prev_image >= 0 && g.prev_frame + 1 == f.frame;
    if (!pair) {
        g.reason("AMD flow: capturing the first image of a pair; camera motion only this frame.");
        return false;
    }
    ID3D12Resource* prev = z.image[g.prev_image].Get();

    // 2. AMD FidelityFX flow, current -> previous, 8x8 blocks of integral pixels.
    if (!z.engine.Record(l, im, prev, index)) {
        ++g.stats.unavailable;
        g.reason("AMD flow refused the image pair; camera motion only.");
        return false;
    }
    // 3. Depth-aware expansion to full size, with validity.
    Move(l, z.raw.Get(), S, U);
    Move(l, z.expanded.Get(), S, U);
    na::nvof::ExpandParams ep;
    ep.depth = f.depth;
    ep.validity_out = z.expanded.Get();
    ep.depth_clear = camera.depth_clear;
    ep.consistency_px = gates.consistency_px;
    ep.sky = gates.sky;
    ep.image_to_raster_px[0] = f.jitter[0];
    ep.image_to_raster_px[1] = f.jitter[1];
    bool ok = g.expand.DispatchDepthAware(l, z.engine.Grid(), 8, ep, z.raw.Get(), w, h, index);
    Order(l);
    Move(l, z.raw.Get(), U, S);
    Move(l, z.expanded.Get(), U, S);
    // 4. Residual = flow - camera motion. Stabilised images carry no jitter shift,
    //    so jitter_previous = jitter_current (no jitter delta).
    Move(l, z.residual.Get(), S, U);
    Move(l, z.valid.Get(), S, U);
    ok = ok && g.derive.DispatchDeriveResidual(l, z.raw.Get(), f.depth, camera, camera.jitter_current_px, z.residual.Get(), z.valid.Get(),
                                               w, h, index, z.expanded.Get());
    Order(l);
    Move(l, z.residual.Get(), U, S);
    Move(l, z.valid.Get(), U, S);
    // 5. Acceptance (image check, texture, temporal agreement, AMD scene change).
    Move(l, z.mask.Get(), S, U);
    Move(l, g.counters.Get(), U, D3D12_RESOURCE_STATE_COPY_DEST);
    l->CopyBufferRegion(g.counters.Get(), 0, g.zero.Get(), 0, 40);
    Move(l, g.counters.Get(), D3D12_RESOURCE_STATE_COPY_DEST, U);
    cb.cost = 2;
    // The pair's stabilisation delta; zero here because our jitter is known exactly.
    cb.dj_x = cb.dj_y = 0.f;
    cb.history_dx = -(f.jitter[0] - g.prev_jitter[0]);
    cb.history_dy = -(f.jitter[1] - g.prev_jitter[1]);
    const unsigned hist_out = g.hist_read ^ 1u;
    cb.have_hist = (g.have_hist && g.hist_frame == g.prev_frame) ? 1u : 0u;
    g.defaults(index, 1);
    g.srv(index, 1, 4, z.engine.SceneChange(), DXGI_FORMAT_R32_UINT);
    g.srv(index, 1, 1, z.raw.Get(), DXGI_FORMAT_R32G32_FLOAT);
    g.srv(index, 1, 2, z.residual.Get(), DXGI_FORMAT_R32G32_FLOAT);
    g.srv(index, 1, 3, z.valid.Get(), DXGI_FORMAT_R32_FLOAT);
    g.srv(index, 1, 5, im, DXGI_FORMAT_R8G8B8A8_UNORM);
    g.srv(index, 1, 6, prev, DXGI_FORMAT_R8G8B8A8_UNORM);
    g.srv(index, 1, 7, z.hist[g.hist_read].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    g.uav(index, 1, kUav + 1, z.mask.Get(), DXGI_FORMAT_R32_FLOAT);
    g.uav(index, 1, kUav + 3, z.hist[hist_out].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    Move(l, z.hist[hist_out].Get(), S, U);
    if (ok) g.dispatch(l, index, 1, g.pso_accept.Get(), cb, w, h);
    Order(l);
    Move(l, z.mask.Get(), U, S);
    Move(l, z.hist[hist_out].Get(), U, S);
    z.engine.Finish(l);
    Move(l, g.counters.Get(), U, D3D12_RESOURCE_STATE_COPY_SOURCE);
    l->CopyBufferRegion(s.readback.Get(), 0, g.counters.Get(), 0, 40);
    Move(l, g.counters.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, U);
    if (!ok) {
        ++g.stats.unavailable;
        g.reason("AMD flow expansion or residual refused; camera motion only.");
        return false;
    }
    out.residual = z.residual.Get();
    out.valid = z.valid.Get();
    out.mask = z.mask.Get();
    s.pair = true;
    s.hist_out = static_cast<int>(hist_out);
    ++g.stats.pairs;
    g.reason("AMD optical flow running.");
    return true;
}

void HelperFlow::Submitted(uint32_t index)
{
    Impl& g = *p_;
    if (!g.ready || index >= g.slot.size()) return;
    Slot& s = g.slot[index];
    if (!s.encoded) return;
    s.pending = true; // its counters can be read once the GPU finished this slot
    g.prev_image = s.image;
    g.prev_frame = s.frame;
    g.prev_jitter[0] = s.jitter[0];
    g.prev_jitter[1] = s.jitter[1];
    g.have_prev = true;
    if (s.pair && s.hist_out >= 0) {
        g.hist_read = static_cast<unsigned>(s.hist_out);
        g.hist_frame = s.frame;
        g.have_hist = true;
    } else {
        g.have_hist = false;
    }
    s.encoded = false;
}

} // namespace na::bridge::flow
