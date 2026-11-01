#include "host_dx12/ffx_flow/ffx_flow.hpp"

#include "host_dx12/ffx_flow/ffx_flow_shaders.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace na::ffx_flow {

using Microsoft::WRL::ComPtr;

namespace {
constexpr uint32_t kDescUav = 32, kDescSrv = 32, kTable = kDescUav + kDescSrv; // AMD's embedded root signature
constexpr uint32_t kDispatches = 4 + kLevels * 3;                            // per Record (scale skipped at level 0)
constexpr uint32_t kClears = 4 + 2 * kLevels;                                // clear-view block per slot
constexpr uint32_t kPerSlot = kDispatches * kTable + kClears;
constexpr uint32_t kHistogramBins = 256, kHistogramsPerDim = 3, kHistogramShifts = 3; // ffx_opticalflow.cpp
constexpr D3D12_RESOURCE_STATES UAVS = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr D3D12_RESOURCE_STATES NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

void set_reason(char* reason, size_t cap, const char* text)
{
    if (reason && cap) std::snprintf(reason, cap, "%s", text);
}
}  // namespace

Flow::~Flow() { Shutdown(); }

void Flow::Shutdown()
{
    ready_ = false;
    for (auto& p : passes_) { p.pso.Reset(); p.root.Reset(); }
    for (auto& row : input_) for (auto& t : row) t.res.Reset();
    for (auto& row : flow_) for (auto& t : row) t.res.Reset();
    scd_hist_.res.Reset(); scd_prev_.res.Reset(); scd_temp_.res.Reset(); scd_out_.res.Reset(); out_.res.Reset();
    heap_.Reset(); clear_heap_.Reset();
    device_.Reset();
    first_ = true;
    frame_index_ = parity_ = 0;
}

bool Flow::make_tex(uint32_t w, uint32_t h, DXGI_FORMAT f, Tex& t)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = (std::max)(w, 1u);
    d.Height = (std::max)(h, 1u);
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = f;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    t.state = UAVS;
    t.format = f;
    t.w = static_cast<uint32_t>(d.Width);
    t.h = d.Height;
    return SUCCEEDED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, UAVS, nullptr, IID_PPV_ARGS(&t.res)));
}

D3D12_CPU_DESCRIPTOR_HANDLE Flow::cpu(uint32_t index) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(index) * stride_;
    return h;
}
D3D12_GPU_DESCRIPTOR_HANDLE Flow::gpu(uint32_t index) const
{
    D3D12_GPU_DESCRIPTOR_HANDLE h = heap_->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<UINT64>(index) * stride_;
    return h;
}

bool Flow::Init(ID3D12Device* device, uint32_t width, uint32_t height, uint32_t slots, bool hdr, char* reason, size_t cap)
{
    Shutdown();
    if (!device || !width || !height || !slots) { set_reason(reason, cap, "bad arguments"); return false; }
    if (!Built()) { set_reason(reason, cap, "AMD optical flow was not compiled into this build (no DXC)"); return false; }
    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_2};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm)) || sm.HighestShaderModel < D3D_SHADER_MODEL_6_2) {
        set_reason(reason, cap, "the device has no Shader Model 6.2 (needed by AMD optical flow)");
        return false;
    }
    device_ = device;
    w_ = width; h_ = height;
    hdr_ = hdr;
    slots_ = slots;
    out_w_ = (width + kBlock - 1) / kBlock;
    out_h_ = (height + kBlock - 1) / kBlock;
    // Pipelines straight from the blobs (each carries its root signature).
    for (int p = 0; p < 7; ++p) {
        const Blob b = Shader(static_cast<Pass>(p), hdr);
        if (!b.data || FAILED(device->CreateRootSignature(0, b.data, b.size, IID_PPV_ARGS(&passes_[p].root)))) {
            set_reason(reason, cap, "AMD optical flow: root signature rejected");
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
        cd.pRootSignature = passes_[p].root.Get();
        cd.CS = {b.data, b.size};
        if (FAILED(device->CreateComputePipelineState(&cd, IID_PPV_ARGS(&passes_[p].pso)))) {
            char t[160];
            std::snprintf(t, sizeof t, "AMD optical flow: pipeline '%s' failed", PassName(static_cast<Pass>(p)));
            set_reason(reason, cap, t);
            return false;
        }
    }
    // Resources (ffx_opticalflow.cpp opticalflowCreate).
    bool ok = true;
    uint32_t fw = out_w_, fh = out_h_;
    for (uint32_t l = 0; l < kLevels; ++l) {
        for (int i = 0; i < 2; ++i) {
            ok = ok && make_tex(width >> l, height >> l, DXGI_FORMAT_R8_UINT, input_[i][l]);
            ok = ok && make_tex(fw, fh, DXGI_FORMAT_R16G16_SINT, flow_[i][l]);
        }
        fw = (fw + 1) / 2;
        fh = (fh + 1) / 2;
    }
    const uint32_t hist_w = kHistogramBins * kHistogramsPerDim * kHistogramsPerDim;
    ok = ok && make_tex(hist_w, 1, DXGI_FORMAT_R32_UINT, scd_hist_);
    ok = ok && make_tex(hist_w, 1, DXGI_FORMAT_R32_FLOAT, scd_prev_);
    ok = ok && make_tex(3, 1, DXGI_FORMAT_R32_UINT, scd_temp_);
    ok = ok && make_tex(3, 1, DXGI_FORMAT_R32_UINT, scd_out_);
    ok = ok && make_tex(out_w_, out_h_, DXGI_FORMAT_R16G16_SINT, out_);
    if (!ok) { set_reason(reason, cap, "AMD optical flow: could not create its textures"); return false; }
    // Descriptors: a block per slot, every entry a null view until a dispatch fills it.
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kPerSlot * slots;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)))) { set_reason(reason, cap, "AMD optical flow: descriptor heap"); return false; }
    hd.NumDescriptors = kClears * slots;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&clear_heap_)))) { set_reason(reason, cap, "AMD optical flow: clear heap"); return false; }
    stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_UNORDERED_ACCESS_VIEW_DESC nu{};
    nu.Format = DXGI_FORMAT_R8_UINT;
    nu.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_SHADER_RESOURCE_VIEW_DESC ns{};
    ns.Format = DXGI_FORMAT_R8_UINT;
    ns.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    ns.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    ns.Texture2D.MipLevels = 1;
    for (uint32_t s = 0; s < slots; ++s) {
        for (uint32_t d = 0; d < kDispatches; ++d) {
            const uint32_t base = s * kPerSlot + d * kTable;
            for (uint32_t i = 0; i < kDescUav; ++i) device->CreateUnorderedAccessView(nullptr, nullptr, &nu, cpu(base + i));
            for (uint32_t i = 0; i < kDescSrv; ++i) device->CreateShaderResourceView(nullptr, &ns, cpu(base + kDescUav + i));
        }
    }
    first_ = true;
    frame_index_ = parity_ = 0;
    stats_ = Stats{};
    ready_ = true;
    set_reason(reason, cap, "ready");
    return true;
}

void Flow::want(Tex& t, D3D12_RESOURCE_STATES s, bool uav_barrier_if_same)
{
    if (!t.res) return;
    if (t.state == s && (s != UAVS || !uav_barrier_if_same)) return;
    if (npending_ == 32) flush(list_);
    D3D12_RESOURCE_BARRIER& b = pending_[npending_++];
    b = {};
    if (t.state == s) {
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; // written by an earlier pass in this list
        b.UAV.pResource = t.res.Get();
        return;
    }
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = t.res.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = t.state;
    b.Transition.StateAfter = s;
    t.state = s;
}

void Flow::flush(ID3D12GraphicsCommandList* list)
{
    if (npending_) list->ResourceBarrier(npending_, pending_);
    npending_ = 0;
}

void Flow::clear(ID3D12GraphicsCommandList* list, Tex& t)
{
    want(t, UAVS);
    flush(list);
    const uint32_t i = slot_ * kPerSlot + kDispatches * kTable + clears_;
    const uint32_t ci = slot_ * kClears + clears_;
    ++clears_;
    D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
    d.Format = t.format;
    d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE c = clear_heap_->GetCPUDescriptorHandleForHeapStart();
    c.ptr += static_cast<SIZE_T>(ci) * stride_;
    device_->CreateUnorderedAccessView(t.res.Get(), nullptr, &d, cpu(i));
    device_->CreateUnorderedAccessView(t.res.Get(), nullptr, &d, c);
    if (t.format == DXGI_FORMAT_R32_FLOAT) {
        const float zero[4] = {0, 0, 0, 0};
        list->ClearUnorderedAccessViewFloat(gpu(i), c, t.res.Get(), zero, 0, nullptr);
    } else {
        const UINT zero[4] = {0, 0, 0, 0};
        list->ClearUnorderedAccessViewUint(gpu(i), c, t.res.Get(), zero, 0, nullptr);
    }
}

void Flow::dispatch(ID3D12GraphicsCommandList* list, int pass, const Binding* uavs, uint32_t nu, const Binding* srvs, uint32_t ns,
                    const Constants& c, uint32_t x, uint32_t y, uint32_t z)
{
    flush(list);
    const uint32_t base = slot_ * kPerSlot + cursor_ * kTable;
    ++cursor_;
    for (uint32_t i = 0; i < nu; ++i) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
        d.Format = uavs[i].format;
        d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device_->CreateUnorderedAccessView(uavs[i].res, nullptr, &d, cpu(base + i));
    }
    for (uint32_t i = 0; i < ns; ++i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.Format = srvs[i].format;
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(srvs[i].res, &d, cpu(base + kDescUav + i));
    }
    list->SetComputeRootSignature(passes_[pass].root.Get());
    list->SetPipelineState(passes_[pass].pso.Get());
    list->SetComputeRootDescriptorTable(0, gpu(base));
    list->SetComputeRootDescriptorTable(1, gpu(base + kDescUav));
    list->SetComputeRoot32BitConstants(2, 8, &c, 0);
    list->Dispatch((std::max)(x, 1u), (std::max)(y, 1u), (std::max)(z, 1u));
    ++stats_.dispatches;
}

bool Flow::Record(ID3D12GraphicsCommandList* list, ID3D12Resource* color, DXGI_FORMAT color_format, bool reset, uint32_t slot)
{
    if (!ready_ || !list || !color || slot >= slots_) return false;
    slot_ = slot;
    list_ = list;
    cursor_ = clears_ = 0;
    npending_ = 0;
    stats_.dispatches = 0;
    ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
    list->SetDescriptorHeaps(1, heaps);

    const bool do_clear = reset || first_;
    first_ = false;
    frame_index_ = do_clear ? 0 : frame_index_ + 1;
    if (do_clear) {
        ++stats_.resets;
        clear(list, scd_temp_);
        clear(list, scd_out_);
        clear(list, scd_hist_);
        clear(list, scd_prev_);
        for (auto& row : input_) for (auto& t : row) clear(list, t);
    }
    const uint32_t cur = parity_ & 1, prev = cur ^ 1; // isOddFrame -> input/flow set 2 (index 1)
    Constants c{};
    c.luma_w = static_cast<int32_t>(w_);
    c.luma_h = static_cast<int32_t>(h_);
    c.level = 0;
    c.level_count = kLevels;
    c.frame_index = frame_index_;
    c.transfer_function = 0; // sRGB-encoded input (J2's fixed encode); HDR permutation would use 1 (PQ) / 2 (scRGB)
    c.min_lum = 0.f;
    c.max_lum = 1.f;

    // 1. Prepare luma: colour -> input level 0 (2x2 pixels per thread, 16x16 groups).
    {
        Binding u[] = {{input_[cur][0].res.Get(), DXGI_FORMAT_R8_UINT}};
        Binding s[] = {{color, color_format}};
        want(input_[cur][0], UAVS);
        dispatch(list, static_cast<int>(Pass::PrepareLuma), u, 1, s, 1, c,
                 ((w_ + 1) / 2 + 15) / 16, ((h_ + 1) / 2 + 15) / 16, 1);
    }
    // 2. Luminance pyramid (SPD): levels 1..6 from level 0; one group per 64x64 tile.
    {
        Binding u[kLevels];
        for (uint32_t l = 0; l < kLevels; ++l) { u[l] = {input_[cur][l].res.Get(), DXGI_FORMAT_R8_UINT}; want(input_[cur][l], UAVS); }
        dispatch(list, static_cast<int>(Pass::LuminancePyramid), u, kLevels, nullptr, 0, c, (w_ + 63) / 64, (h_ + 63) / 64, 1);
    }
    // 3. Scene change: histogram of level 0 (9 strata), then divergence against the previous frame.
    {
        Binding u[] = {{scd_hist_.res.Get(), DXGI_FORMAT_R32_UINT}};
        Binding s[] = {{input_[cur][0].res.Get(), DXGI_FORMAT_R8_UINT}};
        want(scd_hist_, UAVS);
        want(input_[cur][0], NPSR);
        const uint32_t strata_w = (w_ / 4) / kHistogramsPerDim;
        dispatch(list, static_cast<int>(Pass::ScdHistogram), u, 1, s, 1, c, (strata_w + 31) / 32, 16, kHistogramsPerDim * kHistogramsPerDim);
    }
    {
        Binding u[] = {{scd_hist_.res.Get(), DXGI_FORMAT_R32_UINT}, {scd_prev_.res.Get(), DXGI_FORMAT_R32_FLOAT},
                       {scd_temp_.res.Get(), DXGI_FORMAT_R32_UINT}, {scd_out_.res.Get(), DXGI_FORMAT_R32_UINT}};
        want(scd_hist_, UAVS); want(scd_prev_, UAVS); want(scd_temp_, UAVS); want(scd_out_, UAVS);
        dispatch(list, static_cast<int>(Pass::ScdDivergence), u, 4, nullptr, 0, c, kHistogramsPerDim * kHistogramsPerDim, kHistogramShifts, 1);
    }
    // 4. Per level, coarse to fine: search, filter, scale (ffx_opticalflow.cpp dispatch()).
    for (int level = static_cast<int>(kLevels) - 1; level >= 0; --level) {
        const uint32_t L = static_cast<uint32_t>(level);
        const bool odd_level = (level & 1) != 0;
        const uint32_t fa = ((cur == 1) != odd_level) ? 1 : 0, fb = fa ^ 1;
        c.level = L;
        Tex& inA = input_[cur][L];
        Tex& inB = input_[prev][L];
        Tex& flowA = flow_[fa][L];
        Tex& flowB = flow_[fb][L];
        const uint32_t lw = (std::max)(w_ >> L, 1u), lh = (std::max)(h_ >> L, 1u);
        {   // search: block match of inA against inB, refining the guess already in flowA
            Binding u[] = {{flowA.res.Get(), DXGI_FORMAT_R16G16_SINT}, {scd_out_.res.Get(), DXGI_FORMAT_R32_UINT}};
            Binding s[] = {{inA.res.Get(), DXGI_FORMAT_R8_UINT}, {inB.res.Get(), DXGI_FORMAT_R8_UINT}};
            want(flowA, UAVS); want(scd_out_, UAVS); want(inA, NPSR); want(inB, NPSR);
            dispatch(list, static_cast<int>(Pass::Search), u, 2, s, 2, c, ((lw + 3) / 4 * 16 + 63) / 64, (lh + 15) / 16, 1);
        }
        Tex& filtered = level == 0 ? out_ : flowB;
        {   // filter: flowA -> flowB (level 0: the caller's output)
            Binding u[] = {{filtered.res.Get(), DXGI_FORMAT_R16G16_SINT}};
            Binding s[] = {{flowA.res.Get(), DXGI_FORMAT_R16G16_SINT}};
            want(filtered, UAVS); want(flowA, NPSR);
            dispatch(list, static_cast<int>(Pass::Filter), u, 1, s, 1, c, (flowA.w + 15) / 16, (flowA.h + 3) / 4, 1);
        }
        if (level > 0) { // scale: flowB -> next finer level's guess (flow set fb at level-1 = fa of that level)
            Tex& next = flow_[fb][L - 1];
            Binding u[] = {{next.res.Get(), DXGI_FORMAT_R16G16_SINT}, {scd_out_.res.Get(), DXGI_FORMAT_R32_UINT}};
            Binding s[] = {{inA.res.Get(), DXGI_FORMAT_R8_UINT}, {inB.res.Get(), DXGI_FORMAT_R8_UINT}, {flowB.res.Get(), DXGI_FORMAT_R16G16_SINT}};
            want(next, UAVS); want(scd_out_, UAVS); want(inA, NPSR); want(inB, NPSR); want(flowB, NPSR);
            dispatch(list, static_cast<int>(Pass::Scale), u, 2, s, 3, c, (next.w + 3) / 4, (next.h + 3) / 4, 1);
        }
    }
    // Leave the inputs writable for the next frame and hand the output over in COMMON.
    for (auto& row : input_) for (auto& t : row) want(t, UAVS, false);
    for (auto& row : flow_) for (auto& t : row) want(t, UAVS, false);
    want(out_, D3D12_RESOURCE_STATE_COMMON);
    flush(list);
    list_ = nullptr;
    parity_ ^= 1;
    ++stats_.records;
    return true;
}

}  // namespace na::ffx_flow
