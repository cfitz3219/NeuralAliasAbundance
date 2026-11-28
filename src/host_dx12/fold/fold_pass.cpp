#include "host_dx12/fold/fold_pass.hpp"

#include "host_dx12/diag/event_log.hpp"
#include "host_dx12/diag/release_trace.hpp"
#include "host_dx12/diag/vram_ledger.hpp"
#include "host_dx12/gpu/mem_alloc.hpp"
#include "host_dx12/j2/fold_shader.hpp"
#include "host_dx12/j2/live_dlaa.hpp"

#include <d3dcompiler.h>

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>
#include <thread>
#include <iterator>

using Microsoft::WRL::ComPtr;

namespace na::fold {
namespace {

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
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

bool texture(ID3D12Device* device, UINT w, UINT h, DXGI_FORMAT fmt, ComPtr<ID3D12Resource>& out)
{
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    out.Reset();
    return SUCCEEDED(na::gpu::mem::CreateResource(device, D3D12_HEAP_TYPE_DEFAULT, d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                  out.GetAddressOf()));
}

} // namespace

bool Pass::Init(ID3D12Device* device, uint32_t ring)
{
    if (tried_) return ok_;
    tried_ = true;
    if (!device) return false;
    device_ = device;
    ring_ = ring ? ring : 1;
    D3D12_DESCRIPTOR_RANGE r[2]{};
    r[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    r[0].NumDescriptors = 3;
    r[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    r[1].NumDescriptors = 3;
    r[1].OffsetInDescriptorsFromTableStart = 3;
    D3D12_ROOT_PARAMETER p[2]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[0].DescriptorTable.NumDescriptorRanges = 2;
    p[0].DescriptorTable.pDescriptorRanges = r;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[1].Constants.Num32BitValues = 5;
    D3D12_ROOT_SIGNATURE_DESC rd{2, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> sig, err, code;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
        FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root_))))
        return false;
    if (FAILED(D3DCompile(na::j2::kFoldHlsl, sizeof(na::j2::kFoldHlsl) - 1, "j2_fold.hlsl", nullptr, nullptr, "main", "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err)))
        return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root_.Get();
    pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso_)))) return false;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, ring_ * 6 * 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)))) return false;
    inc_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ok_ = true;
    return true;
}

bool Pass::Targets(Mode m, uint32_t w, uint32_t h, DXGI_FORMAT color_format, uint32_t ow, uint32_t oh, DXGI_FORMAT out_format)
{
    if (!ok_) return false;
    const auto blk = BlockFor(m);
    const uint32_t fw = Folded(w, blk.x), fh = Folded(h, blk.y);
    if (fw < 16 || fh < 16) return false;
    auto& p = planes_[static_cast<uint32_t>(m)];
    if (!p.color || p.w != fw || p.h != fh || p.color_format != color_format) {
        p = Planes{};
        if (!texture(device_.Get(), fw, fh, color_format, p.color) || !texture(device_.Get(), fw, fh, DXGI_FORMAT_R32_FLOAT, p.depth) ||
            !texture(device_.Get(), fw, fh, DXGI_FORMAT_R32G32_FLOAT, p.motion)) {
            p = Planes{};
            return false;
        }
        p.w = fw; p.h = fh; p.color_format = color_format;
        p.color->SetName(L"NeuralAlias folded colour");
        p.depth->SetName(L"NeuralAlias folded depth");
        p.motion->SetName(L"NeuralAlias folded motion");
        char name[48];
        std::snprintf(name, sizeof name, "fold_planes_%s_%s", tag_, Name(m));
        na::vram::Set(name, na::vram::SizeOf(p.color.Get()) + na::vram::SizeOf(p.depth.Get()) + na::vram::SizeOf(p.motion.Get()),
                      "a DLSS level was asked for: its folded colour, depth and motion");
    }
    (void)ow; (void)oh; (void)out_format;
    return true;
}

void Pass::Release(Mode m)
{
    auto& p = planes_[static_cast<uint32_t>(m)];
    if (!p.color && !p.depth && !p.motion) return;
    released_.push_back({p, static_cast<uint64_t>(GetTickCount64())});
    p = Planes{};
    char name[48];
    std::snprintf(name, sizeof name, "fold_planes_%s_%s", tag_, Name(m));
    na::vram::Set(name, 0, "the level is not on screen or asked for: its folded planes are released");
}

void Pass::Collect()
{
    constexpr uint64_t kAfterMs = 2000; // every frame that used them has long finished on the GPU
    const uint64_t now = GetTickCount64();
    const auto keep = std::stable_partition(released_.begin(), released_.end(), [now](const Released& r) { return now - r.at_ms < kAfterMs; });
    if (keep == released_.end()) return;
    // The planes' last references are dropped on a thread of their own: giving a level's planes back to the
    // driver can take a frame's worth of time (measured for the motion contexts: a 66 ms frame).
    std::vector<Released> gone(std::make_move_iterator(keep), std::make_move_iterator(released_.end()));
    released_.erase(keep, released_.end());
    try {
        std::thread([g = std::move(gone)]() mutable {
            LARGE_INTEGER q0{}, q1{}, hz{};
            const ULONGLONG t0 = GetTickCount64();
            QueryPerformanceCounter(&q0);
            const size_t n = g.size();
            g.clear();
            QueryPerformanceCounter(&q1);
            QueryPerformanceFrequency(&hz);
            const double ms = double(q1.QuadPart - q0.QuadPart) * 1000.0 / double(hz.QuadPart);
            na::diag::release_trace::Note("fold_planes", ms, t0);
            na::eventlog::Emit("release_trace", "what=fold_planes sets=%zu thread=background ms=%.2f at_tick=%llu", n, ms,
                               static_cast<unsigned long long>(t0));
        }).detach();
    } catch (...) {
    }
}

void Pass::Describe(const dlss::EvalDesc& base, Mode m, uint64_t view_base, uint32_t preset, dlss::EvalDesc* out)
{
    *out = base;
    if (m == Mode::Off) return;
    const auto blk = BlockFor(m);
    const uint32_t fw = Folded(base.render_w, blk.x), fh = Folded(base.render_h, blk.y);
    out->color = out->depth = out->motion = nullptr;
    out->render_w = out->max_render_w = fw;
    out->render_h = out->max_render_h = fh;
    out->mv_display_res = false;
    out->quality = m == Mode::Performance ? dlss::Quality::MaxPerformance : dlss::Quality::MaxQuality;
    out->preset = na::j2::PresetFor(preset, fw, base.out_w);
    out->view_id = view_base + static_cast<uint64_t>(m);
    out->bias_current_color = nullptr;
}

void Pass::Dispatch(ID3D12GraphicsCommandList* l, ID3D12Resource* color, ID3D12Resource* depth, ID3D12Resource* motion,
                    const Planes& planes, Block blk, Block pick, uint32_t table, bool motion_ready)
{
    D3D12_CPU_DESCRIPTOR_HANDLE c = heap_->GetCPUDescriptorHandleForHeapStart();
    c.ptr += static_cast<SIZE_T>(table) * 6 * inc_;
    ID3D12Resource* const inputs[] = {color, depth, motion};
    ID3D12Resource* const outputs[] = {planes.color.Get(), planes.depth.Get(), planes.motion.Get()};
    const DXGI_FORMAT formats[] = {typed(planes.color_format), DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT};
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    for (UINT i = 0; i < 3; ++i) {
        sv.Format = uv.Format = formats[i];
        device_->CreateShaderResourceView(inputs[i], &sv, c);
        const D3D12_CPU_DESCRIPTOR_HANDLE output{c.ptr + 3 * inc_};
        device_->CreateUnorderedAccessView(outputs[i], nullptr, &uv, output);
        c.ptr += inc_;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gh = heap_->GetGPUDescriptorHandleForHeapStart();
    gh.ptr += static_cast<UINT64>(table) * 6 * inc_;
    ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
    l->SetDescriptorHeaps(1, heaps);
    l->SetComputeRootSignature(root_.Get());
    l->SetPipelineState(pso_.Get());
    l->SetComputeRootDescriptorTable(0, gh);
    const uint32_t k[5] = {blk.x, blk.y, pick.x, pick.y, motion_ready ? 1u : 0u};
    l->SetComputeRoot32BitConstants(1, 5, k, 0);
    l->Dispatch((planes.w + 7) / 8, (planes.h + 7) / 8, 1);
}

void Pass::Record(ID3D12GraphicsCommandList* l, const dlss::EvalDesc& base, Mode m, uint32_t slot, bool warming,
                  dlss::EvalDesc* out, uint64_t view_base, uint32_t preset, bool motion_ready)
{
    const auto blk = BlockFor(m);
    const auto pick = Pick(m);
    auto& p = planes_[static_cast<uint32_t>(m)];
    const uint32_t table = (slot % ring_) * 2 + (warming ? 1u : 0u);
    Dispatch(l, base.color, base.depth, base.motion, p, blk, pick, table, motion_ready);
    D3D12_RESOURCE_BARRIER b[3];
    UINT n = 0;
    for (ID3D12Resource* r : {p.color.Get(), p.depth.Get(), p.motion.Get()})
        b[n++] = transition(r, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    l->ResourceBarrier(n, b);
    *out = base;
    out->color = p.color.Get();
    out->depth = p.depth.Get();
    out->motion = p.motion.Get();
    out->render_w = out->max_render_w = p.w;
    out->render_h = out->max_render_h = p.h;
    out->mv_scale_x = base.mv_scale_x / static_cast<float>(blk.x); // the kept vectors are full-size pixels
    out->mv_scale_y = base.mv_scale_y / static_cast<float>(blk.y);
    out->mv_display_res = false;
    out->jitter_x = DeclaredJitter(blk.x, pick.x, base.jitter_x);
    out->jitter_y = DeclaredJitter(blk.y, pick.y, base.jitter_y);
    out->quality = m == Mode::Performance ? dlss::Quality::MaxPerformance : dlss::Quality::MaxQuality;
    out->preset = na::j2::PresetFor(preset, p.w, base.out_w);
    out->view_id = view_base + static_cast<uint64_t>(m);
    out->bias_current_color = nullptr;
}

void Pass::Done(ID3D12GraphicsCommandList* l, Mode m)
{
    auto& p = planes_[static_cast<uint32_t>(m)];
    D3D12_RESOURCE_BARRIER b[3];
    UINT n = 0;
    for (ID3D12Resource* r : {p.color.Get(), p.depth.Get(), p.motion.Get()})
        b[n++] = transition(r, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    l->ResourceBarrier(n, b);
}

void Status::Why(const char* source, const char* text)
{
    std::lock_guard<std::mutex> lk(why_m);
    if (std::strncmp(why, text ? text : "", sizeof why - 1) == 0) return;
    std::snprintf(why, sizeof why, "%s", text ? text : "");
    if (text && *text) na::eventlog::Emit(source, "cannot fold: %s", text);
}

void Status::Format(char* out, size_t n)
{
    if (!out || !n) return;
    const Mode a = ModeFrom(asked.load()), s = ModeFrom(shown.load());
    const Mode wm = ModeFrom(warming.load()), r = ModeFrom(refused.load());
    char reason[160] = {};
    { std::lock_guard<std::mutex> lk(why_m); std::snprintf(reason, sizeof reason, "%s", why); }
    if (r != Mode::Off && r == a)
        std::snprintf(out, n, "%s could not start, so %s stays on screen. Reason: %s.", Name(r), Name(s), *reason ? reason : "DLSS did not accept the folded picture");
    else if (a != s && *reason)
        std::snprintf(out, n, "%s stays on screen: %s.", Name(s), reason);
    else if (wm != Mode::Off)
        std::snprintf(out, n, "Getting %s ready (%u frames so far). %s stays on screen until it is.", Name(wm), good.load(), Name(s));
    else if (s == Mode::Off)
        std::snprintf(out, n, "On screen: DLAA (full size).");
    else
        std::snprintf(out, n, "On screen: %s. DLSS gets %ux%u and rebuilds the full picture. Coarse shading: on.", Name(s), w.load(), h.load());
}

} // namespace na::fold
