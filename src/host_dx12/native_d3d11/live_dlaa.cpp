#include "live_dlaa.hpp"
#include "shared_resources.hpp"
#include "core/motion_arbiter_gpu.hpp"
#include "core/temporal_jitter.hpp"
#include "host_dx12/adapters/wwii/scene_camera.hpp"
#include "host_dx12/capture/frame_capture.hpp"
#include "host_dx12/capture/j2_capture.hpp"
#include "host_dx12/depth_format.hpp"
#include "host_dx12/dlss/dlss_backend.hpp"
#include "host_dx12/host_boot/session.hpp"
#include "host_dx12/j2/live_dlaa.hpp"
#include "host_dx12/j2/compute_flow.hpp"
#include "host_dx12/j2/j2_flow.hpp"
#include "host_dx12/effects/xegtao.hpp"
#include "host_dx12/effects/donut.hpp"
#include "host_dx12/nr/native_fg.hpp"
#include "host_dx12/native11/camera_pick.hpp"
#include "host_dx12/native11/input_provider.hpp"
#include "host_dx12/observe/producers.hpp"
#include "host_dx12/settings.hpp"
#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>

namespace na::native_d3d11 {
using Microsoft::WRL::ComPtr;
namespace {
constexpr UINT kSlots = 3;
constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto kWrite = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr auto kCommon = D3D12_RESOURCE_STATE_COMMON;

struct Slot {
    SharedTexture color, depth, output;
    ComPtr<ID3D12Resource> motion, provenance;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D11Texture2D> packed_depth;
    ComPtr<ID3D11ShaderResourceView> packed_view;
    ComPtr<ID3D11UnorderedAccessView> float_view;
    DXGI_FORMAT depth_format = DXGI_FORMAT_UNKNOWN;
    uint64_t native_done = 0, effects_done = 0;
    std::shared_ptr<xegtao::Frame> ambient;
    std::shared_ptr<donut::Frame> grade;
};
struct Session {
    ComPtr<ID3D11Device> native;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    SharedFences fences;
    motion::gpu::Arbiter arbiter;
    ComPtr<ID3D11ComputeShader> convert_depth;
    std::unique_ptr<Slot> slots[kSlots];
    camera::CameraTracker camera;
    native11::CameraPick pick;
    uint64_t stream = 0, last_frame = 0;
    bool failed = false;
};
// Retain submitted work through DLL/process teardown. The game owns its
// immediate context and can still have queued copies when shutdown starts.
Session& State() { static auto* s = new Session; return *s; }
std::mutex mutex;
std::atomic<const char*> reason{"Waiting for a native scene."};
std::atomic<uint64_t> delivered{0}, busy{0}, rejected{0};
std::atomic<uint64_t> delivered_size{0}, delivered_at{0};
std::atomic<uint32_t> applied_preset{0}, applied_sharpness{0};
std::atomic<int> raster_slot{-1};
std::atomic<bool> evaluation_capture_pending{false};

bool Skip(const char* why)
{
    reason.store(why, std::memory_order_relaxed);
    rejected.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool Start(Session& s, ID3D11Device* native)
{
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(native->QueryInterface(IID_PPV_ARGS(&dxgi))) ||
        FAILED(dxgi->GetAdapter(&adapter)) ||
        FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&s.device)))) return false;
    D3D12_COMMAND_QUEUE_DESC q{};
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(s.device->CreateCommandQueue(&q, IID_PPV_ARGS(&s.queue))) ||
        FAILED(s.fences.Create(native, s.device.Get(), s.queue.Get())) ||
        !s.arbiter.Init(s.device.Get(), kSlots)) return false;
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetTempPathW(MAX_PATH, path);
    if (!n || n >= MAX_PATH - 20) return false;
    wcscat_s(path, L"NeuralAlias-ngx");
    CreateDirectoryW(path, nullptr);
    if (!dlss::EnsureOn(s.device.Get(), path)) return false;
    s.native = native;
    return true;
}

bool Texture(ID3D12Device* dev, const ImageDesc& extent, DXGI_FORMAT format, ID3D12Resource** out)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = extent.width;
    d.Height = extent.height;
    d.DepthOrArraySize = d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return SUCCEEDED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, kWrite,
                                                  nullptr, IID_PPV_ARGS(out)));
}

bool PreparePackedDepth(Session& s, Slot& slot, const ImageDesc& depth, DXGI_FORMAT packed)
{
    if (!s.convert_depth) {
        static constexpr char shader[] = R"(
Texture2D<float> inputDepth : register(t0);
RWTexture2D<float> outputDepth : register(u0);
[numthreads(8, 8, 1)] void main(uint3 p : SV_DispatchThreadID) {
    uint w, h;
    outputDepth.GetDimensions(w, h);
    if (p.x < w && p.y < h) outputDepth[p.xy] = inputDepth.Load(int3(p.xy, 0));
}
)";
        ComPtr<ID3DBlob> code;
        if (FAILED(D3DCompile(shader, sizeof shader - 1, "Native depth", nullptr, nullptr,
                              "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, nullptr)) ||
            FAILED(s.native->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(),
                                                  nullptr, &s.convert_depth))) return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = depth.width; desc.Height = depth.height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = packed; desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = packed == DXGI_FORMAT_R24G8_TYPELESS ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS
                                                      : DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = 1;
    return SUCCEEDED(s.native->CreateTexture2D(&desc, nullptr, &slot.packed_depth)) &&
           SUCCEEDED(s.native->CreateShaderResourceView(slot.packed_depth.Get(), &view, &slot.packed_view)) &&
           SUCCEEDED(s.native->CreateUnorderedAccessView(slot.depth.Native(), nullptr, &slot.float_view));
}

void CopyDepth(Session& s, Slot& slot, ID3D11DeviceContext* ctx, ID3D11Texture2D* source, UINT sub)
{
    auto* target = slot.packed_depth ? slot.packed_depth.Get() : slot.depth.Native();
    ctx->CopySubresourceRegion(target, 0, 0, 0, 0, source, sub, nullptr);
    if (!slot.packed_depth) return;
    ComPtr<ID3D11ComputeShader> previous;
    ComPtr<ID3D11ShaderResourceView> previous_srv;
    ComPtr<ID3D11UnorderedAccessView> previous_uav;
    ID3D11ClassInstance* instances[D3D11_SHADER_MAX_INTERFACES]{};
    UINT count = D3D11_SHADER_MAX_INTERFACES;
    ctx->CSGetShader(&previous, instances, &count);
    ctx->CSGetShaderResources(0, 1, &previous_srv);
    ctx->CSGetUnorderedAccessViews(0, 1, &previous_uav);
    auto* srv = slot.packed_view.Get();
    auto* uav = slot.float_view.Get();
    ctx->CSSetShader(s.convert_depth.Get(), nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    const auto& d = slot.depth.Description();
    ctx->Dispatch((d.width + 7) / 8, (d.height + 7) / 8, 1);
    srv = previous_srv.Get();
    uav = previous_uav.Get();
    ctx->CSSetShaderResources(0, 1, &srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    ctx->CSSetShader(previous.Get(), instances, count);
    for (UINT i = 0; i < count; ++i) if (instances[i]) instances[i]->Release();
}

bool Prepare(Session& s, Slot& slot, const ImageDesc& color, const ImageDesc& depth, DXGI_FORMAT packed)
{
    auto* dev = s.device.Get();
    auto out = color;
    out.writable12 = true;
    if (FAILED(slot.color.Create(s.native.Get(), dev, color)) ||
        FAILED(slot.depth.Create(s.native.Get(), dev, depth)) ||
        FAILED(slot.output.Create(s.native.Get(), dev, out)) ||
        !Texture(dev, color, DXGI_FORMAT_R32G32_FLOAT, &slot.motion) ||
        !Texture(dev, color, DXGI_FORMAT_R8_UINT, &slot.provenance) ||
        FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(),
                                       nullptr, IID_PPV_ARGS(&slot.list))) ||
        FAILED(slot.list->Close())) return false;
    slot.depth_format = packed;
    return packed == DXGI_FORMAT_R32_FLOAT || PreparePackedDepth(s, slot, depth, packed);
}

void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* image,
             D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {image, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &b);
}

bool Subresource(ID3D11Texture2D* texture, const native11::TextureFacts& facts,
                 D3D11_TEXTURE2D_DESC& desc, UINT& sub, UINT& w, UINT& h)
{
    texture->GetDesc(&desc);
    if (desc.SampleDesc.Count != 1 || facts.mip >= desc.MipLevels || facts.mip >= 32 ||
        facts.array_slice >= desc.ArraySize) return false;
    sub = D3D11CalcSubresource(facts.mip, facts.array_slice, desc.MipLevels);
    w = (std::max)(1u, desc.Width >> facts.mip);
    h = (std::max)(1u, desc.Height >> facts.mip);
    return facts.active_x < w && facts.active_y < h && facts.active_w && facts.active_h &&
           facts.active_w <= w - facts.active_x && facts.active_h <= h - facts.active_y;
}

camera::CameraFrame Pose(Session& s, const native11::SceneFrame& frame, UINT w, UINT h)
{
    const auto& c = frame.camera;
    const size_t start = size_t(c.first_constant) * 16;
    if (!c.bytes || start >= c.size) return {};
    const size_t length = c.num_constants ? (std::min)(size_t(c.num_constants) * 16, c.size - start) : c.size - start;
    std::vector<uint8_t> bytes(c.bytes + start, c.bytes + start + length);
    if (frame.selection_changed || frame.stream != s.stream || frame.frame != s.last_frame + 1) {
        s.camera.Reset();
        s.pick = {};
    }
    const double jitter[2]{c.jitter_applied ? c.jitter_px[0] : 0, c.jitter_applied ? c.jitter_px[1] : 0};
    wwii::SceneCamera scene;
    if (wwii::DecodeSceneCamera(bytes, static_cast<uint32_t>(length), w, h, scene))
        return s.camera.UpdateCameraRelative(scene.vp, scene.origin, frame.frame, w, h, jitter);
    native11::CameraPick pick;
    if (!native11::CheckLearned(bytes, s.pick, &pick))
        pick = native11::FindCamera(bytes, c.version ? c.version : frame.frame + 1);
    if (!pick.found) return {};
    s.pick = pick;
    return s.camera.Update(pick.vp, frame.frame, w, h, jitter);
}

bool FailSession(Session& s, const char* why)
{
    s.failed = true;
    return Skip(why);
}
} // namespace

bool EvaluateFrame(void*, void* context, const native11::SceneFrame& frame)
{
    // The native provider has witnessed a real rendered scene. Wake the
    // existing session worker so it can qualify settings and enable effects.
    if (context) host::RendererObserved();
    native11::SetNextJitter(0, 0);
    if (!context || !j2::Enabled() || j2::Paused()) return Skip("DLAA paused.");
    std::lock_guard<std::mutex> lock(mutex);
    auto& s = State();
    if (s.failed) return false;
    auto* ctx = static_cast<ID3D11DeviceContext*>(context);
    ComPtr<ID3D11Device> native;
    ctx->GetDevice(&native);
    if (s.native && native.Get() != s.native.Get()) return Skip("Native DX11 device changed.");
    const auto& c = frame.color;
    const auto& d = frame.depth;
    ComPtr<ID3D11Texture2D> color, depth;
    auto* cr = static_cast<ID3D11Resource*>(c.d3d11_resource);
    auto* dr = static_cast<ID3D11Resource*>(d.d3d11_resource);
    if (!cr || !dr || FAILED(cr->QueryInterface(IID_PPV_ARGS(&color))) ||
        FAILED(dr->QueryInterface(IID_PPV_ARGS(&depth)))) return Skip("Scene textures unavailable.");
    D3D11_TEXTURE2D_DESC cd{}, dd{};
    UINT csub, dsub, cw, ch, dw, dh;
    if (!Subresource(color.Get(), c, cd, csub, cw, ch) ||
        !Subresource(depth.Get(), d, dd, dsub, dw, dh)) return Skip("Scene texture layout unsupported.");
    const auto cf = static_cast<DXGI_FORMAT>(c.view_format);
    const bool hdr = cf == DXGI_FORMAT_R11G11B10_FLOAT || cf == DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (!hdr && cf != DXGI_FORMAT_R8G8B8A8_UNORM) return Skip("Scene colour format unsupported.");
    ImageDesc ci{c.active_w, c.active_h, cf, true};
    const DXGI_FORMAT df = na::depth::IsDepth24(dd.Format) ? DXGI_FORMAT_R24G8_TYPELESS :
        dd.Format == DXGI_FORMAT_R32G8X24_TYPELESS || dd.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT
        ? DXGI_FORMAT_R32G8X24_TYPELESS : DXGI_FORMAT_R32_FLOAT;
    ImageDesc di{dw, dh, DXGI_FORMAT_R32_FLOAT, df != DXGI_FORMAT_R32_FLOAT};
    if (dw != ci.width || dh != ci.height || d.active_x || d.active_y ||
        !CompatibleImageCopy(dd, dsub, {dw, dh, df})) return Skip("Scene depth requires a different preparation path.");
    // Validate the format family against the full source mip, then copy just
    // the active colour rectangle into the prepared top-left input.
    if (!CompatibleImageCopy(cd, csub, {cw, ch, cf})) return Skip("Scene colour copy incompatible.");
    const auto cam = Pose(s, frame, ci.width, ci.height);
    if (!cam.valid) return Skip("Waiting for the current scene camera.");
    observe::HostWorkScope own;
    if (!s.native && !Start(s, native.Get())) return FailSession(s, "Native DX12 effects device could not start.");
    UINT index = 0;
    for (; index < kSlots; ++index) {
        const auto& slot = s.slots[index];
        if (!slot || s.fences.Retired(slot->native_done, slot->effects_done)) break;
    }
    if (index == kSlots) {
        busy.fetch_add(1, std::memory_order_relaxed);
        return Skip("Waiting for a free native frame slot.");
    }
    auto& owned = s.slots[index];
    if (!owned || owned->color.Description().width != ci.width || owned->color.Description().height != ci.height ||
        owned->color.Description().format != ci.format || owned->depth_format != df) {
        auto slot = std::make_unique<Slot>();
        if (!Prepare(s, *slot, ci, di, df)) return FailSession(s, "Could not create shared native scene images.");
        owned = std::move(slot);
    }
    auto& slot = *owned;
    auto* list = slot.list.Get();
    // Both interop fences retired this slot. End the old recording before
    // reusing its private descriptors, flow history and effect ownership.
    nr::native::Reset(list);
    slot.ambient.reset();
    slot.grade.reset();
    if (FAILED(slot.allocator->Reset()) || FAILED(list->Reset(slot.allocator.Get(), nullptr)))
        return FailSession(s, "Could not reset the native effects list.");
    auto* shared_depth = slot.depth.Effects();
    Barrier(list, slot.color.Effects(), kCommon, kRead);
    Barrier(list, shared_depth, kCommon, kRead);
    Barrier(list, slot.output.Effects(), kCommon, kWrite);
    motion::gpu::Inputs in{};
    in.depth = shared_depth;
    in.tier2_valid_flag = true;
    in.depth_clear = frame.reversed_depth ? 0.f : 1.f;
    for (size_t i = 0; i < 16; ++i) {
        in.inv_view_proj_current.m[i] = static_cast<float>(cam.inv_view_proj_current_row[i]);
        in.view_proj_previous.m[i] = static_cast<float>(cam.view_proj_previous_row[i]);
    }
    in.jitter_current_px[0] = static_cast<float>(cam.jitter_px_x);
    in.jitter_current_px[1] = static_cast<float>(cam.jitter_px_y);
    const bool reset_history = cam.camera_cut || frame.selection_changed || frame.frame != s.last_frame + 1;
    j2::compute_flow::Ticket flow;
    if (j2::flow::Enabled()) {
        j2::compute_flow::Frame ff;
        ff.frame = frame.frame; ff.previous = s.last_frame;
        ff.generation = static_cast<uint32_t>(frame.stream); ff.source_generation = frame.stream;
        ff.slot = index; ff.reset = reset_history; ff.hdr = hdr;
        ff.jitter[0] = ff.raster_jitter[0] = in.jitter_current_px[0];
        ff.jitter[1] = ff.raster_jitter[1] = in.jitter_current_px[1];
        ff.raster_sample = true; ff.raster_confirmed = frame.camera.jitter_applied;
        j2::compute_flow::Tier3 tier;
        j2::compute_flow::SetLoosen(settings::Get().j2_flow_loosen.load());
        flow = j2::compute_flow::Record(list, s.queue.Get(), slot.color.Effects(), shared_depth, in, ff, tier);
        if (tier.residual) {
            in.tier3_valid_flag = true;
            in.tier3_residual = tier.residual; in.tier3_valid = tier.valid; in.tier3_mask = tier.mask;
        }
        in.fix = tier.fix;
    }
    motion::gpu::Outputs motion_out{slot.motion.Get(), slot.provenance.Get(), nullptr, ci.width, ci.height};
    if (!s.arbiter.Dispatch(list, in, motion_out, index)) {
        list->Close();
        return Skip("Camera motion could not be recorded.");
    }
    Barrier(list, slot.motion.Get(), kWrite, kRead);
    dlss::EvalDesc eval{};
    eval.color = slot.color.Effects();
    if (settings::Get().xegtao_enabled.load()) {
        xegtao::Inputs ai;
        ai.color = eval.color; ai.depth = shared_depth; ai.motion = slot.motion.Get();
        ai.color_format = cf; ai.width = ci.width; ai.height = ci.height;
        ai.srgb = !hdr; ai.clear_depth = frame.reversed_depth ? 0.f : 1.f;
        ai.jitter_x = in.jitter_current_px[0]; ai.jitter_y = in.jitter_current_px[1];
        ai.frame = frame.frame; ai.motion_reference_frame = s.last_frame;
        ai.scene_epoch = frame.stream; ai.reset = reset_history;
        const auto column = camera::TransposeMatrix(cam.view_proj_current_row);
        if (xegtao::FromViewProjection(column.data(), ai.projection)) {
            slot.ambient = xegtao::ScenePass().Prepare(s.device.Get(), s.queue.Get(), ai, xegtao::CurrentOptions());
            if (slot.ambient && slot.ambient->Record(list)) eval.color = slot.ambient->Color();
        } else xegtao::ScenePass().Unavailable("Waiting for supported native camera geometry.");
    }
    eval.depth = shared_depth;
    eval.motion = slot.motion.Get();
    eval.output = slot.output.Effects();
    eval.render_w = eval.out_w = ci.width;
    eval.render_h = eval.out_h = ci.height;
    eval.quality = dlss::Quality::DLAA;
    eval.hdr = eval.auto_exposure = hdr;
    eval.depth_inverted = frame.reversed_depth;
    eval.jitter_x = in.jitter_current_px[0];
    eval.jitter_y = in.jitter_current_px[1];
    eval.reset = reset_history;
    const uint32_t preset = settings::Get().j2_preset.load();
    eval.preset = preset == 255 ? 12 : preset;
    constexpr uint64_t kNativeView = 0x4e41544956453131ull;
    eval.view_id = kNativeView;
    const auto lighting = donut::CurrentOptions();
    if (lighting.tone || lighting.bloom) {
        donut::Inputs gi;
        gi.color = eval.color; gi.format = cf; gi.width = ci.width; gi.height = ci.height;
        gi.srgb = !hdr; gi.reset = reset_history; gi.stream = kNativeView;
        slot.grade = donut::ScenePass().Prepare(s.device.Get(), s.queue.Get(), gi, lighting);
        if (slot.grade && slot.grade->Record(list)) eval.color = slot.grade->Color();
    }
    const bool evaluated = dlss::Evaluate(list, eval);
    uint32_t capture_token = 0;
    if (evaluated && capture::j2::Armed()) {
        capture::j2::Planes planes;
        planes.color = eval.color;
        planes.depth = shared_depth;
        planes.motion = slot.motion.Get();
        planes.output = slot.output.Effects();
        capture::j2::Identity id;
        id.present_ordinal = id.inputs_frame = frame.frame;
        id.view_color = c.incarnation;
        id.view_depth = d.incarnation;
        id.width = ci.width;
        id.height = ci.height;
        id.color_format = cf;
        id.jitter_x = eval.jitter_x;
        id.jitter_y = eval.jitter_y;
        id.preset = eval.preset;
        id.sharpness = settings::Get().j2_sharpness.load();
        id.dlss_hdr = eval.hdr;
        id.dlss_auto_exposure = eval.auto_exposure;
        id.dlss_depth_inverted = frame.reversed_depth;
        id.reset = eval.reset;
        id.camera_valid = true;
        id.camera_source = 1;
        std::copy(cam.view_proj_current_row.begin(), cam.view_proj_current_row.end(), id.vp_cur);
        std::copy(cam.view_proj_previous_row.begin(), cam.view_proj_previous_row.end(), id.vp_prev);
        std::copy(cam.inv_view_proj_current_row.begin(), cam.inv_view_proj_current_row.end(), id.inv_vp_cur);
        std::copy(cam.position.begin(), cam.position.end(), id.eye);
        capture_token = capture::j2::RecordEvaluation(list, planes, id);
    }
    auto* result = slot.output.Native();
    uint32_t sharpened = 0;
    const uint32_t sharpness = settings::Get().j2_sharpness.load();
    if (evaluated && sharpness) {
        Barrier(list, slot.output.Effects(), kWrite, kRead);
        Barrier(list, slot.color.Effects(), kRead, kWrite);
        if (j2::RecordNativeRcas(s.device.Get(), list, slot.output.Effects(), slot.color.Effects(), shared_depth,
                                 ci.width, ci.height, sharpness, cf, index, frame.reversed_depth)) {
            result = slot.color.Native();
            sharpened = sharpness;
        }
        Barrier(list, slot.output.Effects(), kRead, kCommon);
        Barrier(list, slot.color.Effects(), kWrite, kCommon);
    } else {
        Barrier(list, slot.output.Effects(), kWrite, kCommon);
        Barrier(list, slot.color.Effects(), kRead, kCommon);
    }
    Barrier(list, shared_depth, kRead, kCommon);
    Barrier(list, slot.motion.Get(), kRead, kWrite);
    if (FAILED(list->Close())) {
        capture::j2::Discard(capture_token, false);
        return FailSession(s, "Native effects command list could not close.");
    }

    // The D3D11 runtime retains submitted native copy resources itself.
    // Do not keep a game backbuffer reference across ResizeBuffers.
    D3D11_BOX box{c.active_x, c.active_y, 0, c.active_x + ci.width, c.active_y + ci.height, 1};
    ctx->CopySubresourceRegion(slot.color.Native(), 0, 0, 0, 0, color.Get(), csub, &box);
    CopyDepth(s, slot, ctx, depth.Get(), dsub);
    if (FAILED(s.fences.SignalNative(&slot.native_done)) || FAILED(s.fences.WaitEffects(slot.native_done))) {
        capture::j2::Discard(capture_token, false);
        return FailSession(s, "Native scene submission failed.");
    }
    ID3D12CommandList* lists[] = {list};
    dlss::BeforeExecute(s.queue.Get(), 1, lists);
    {
        nr::native::Submission submitted(s.queue.Get(), 1, lists);
        s.queue->ExecuteCommandLists(1, lists);
    }
    j2::compute_flow::Committed(flow);
    dlss::OnExecuted(s.queue.Get(), 1, lists);
    if (capture_token) {
        capture::j2::Submitted(capture_token, s.queue.Get(), frame.frame);
        evaluation_capture_pending.store(true, std::memory_order_release);
    }
    if (FAILED(s.fences.SignalEffects(&slot.effects_done))) return FailSession(s, "DX12 effects submission failed.");
    if (!evaluated) return Skip("DLAA evaluation rejected this frame.");
    if (FAILED(s.fences.WaitNative(slot.effects_done))) return FailSession(s, "Native result dependency failed.");
    ctx->CopySubresourceRegion(color.Get(), csub, c.active_x, c.active_y, 0, result, 0, nullptr);
    // This signal retires the slot. The game's subsequent submission can
    // flush it naturally because no effects queue waits on the value.
    if (FAILED(s.fences.SignalNative(&slot.native_done, false))) return FailSession(s, "Native result submission failed.");
    s.stream = frame.stream;
    s.last_frame = frame.frame;
    delivered.fetch_add(1, std::memory_order_relaxed);
    delivered_size.store((uint64_t(ci.width) << 32) | ci.height, std::memory_order_relaxed);
    applied_preset.store(eval.preset, std::memory_order_relaxed);
    applied_sharpness.store(sharpened, std::memory_order_relaxed);
    raster_slot.store(frame.camera.vs_slot, std::memory_order_relaxed);
    delivered_at.store(GetTickCount64(), std::memory_order_relaxed);
    if (frame.camera.vs_slot >= 0 && j2::Jitter()) {
        const auto phase = static_cast<uint32_t>(frame.frame);
        const auto sample = settings::Get().j2_jitter_r2.load()
            ? neural_alias::temporal_jitter_r2(phase, 8) : neural_alias::temporal_jitter(phase, 8);
        native11::SetNextJitter(static_cast<float>(sample.x_pixels), static_cast<float>(sample.y_pixels));
    }
    reason.store("Native DX11 DLAA delivered before the scene consumer.", std::memory_order_relaxed);
    return true;
}

void CapturePresent(IDXGISwapChain* swap, ID3D12CommandQueue* queue)
{
    const bool eval = evaluation_capture_pending.exchange(false, std::memory_order_acq_rel);
    const bool burst = capture::Pending();
    if (!eval && !burst) return;
    struct PresentCopy {
        SharedTexture image;
        SharedFences fences;
        ComPtr<ID3D11Device> native;
        ComPtr<ID3D12CommandQueue> queue;
        uint64_t native_done = 0, effects_done = 0;
    };
    static auto* state = new std::unique_ptr<PresentCopy>;
    const auto stats = native11::Stats();
    if (eval) capture::j2::OnFrameClosed(stats.frames - 1, 0);
    const auto complete = [queue, eval, burst](ID3D12Resource* image) {
        if (eval) capture::j2::OnPresent(queue, image,
            [](ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) { q->ExecuteCommandLists(n, lists); });
        if (burst && image) {
            char note[256]{};
            FormatStatus(note, sizeof note);
            capture::NotePresent(note);
            capture::OnPresent(queue, image, false);
        }
    };
    ComPtr<ID3D11Texture2D> backbuffer;
    if (!queue || FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) return complete(nullptr);
    native11::OwnWorkScope own;
    observe::HostWorkScope host_work;
    D3D11_TEXTURE2D_DESC desc{};
    backbuffer->GetDesc(&desc);
    ComPtr<ID3D11Device> native;
    backbuffer->GetDevice(&native);
    const ImageDesc image{desc.Width, desc.Height, desc.Format};
    if (!CompatibleImageCopy(desc, 0, image)) return complete(nullptr);
    auto& copy = *state;
    if (!copy || copy->queue.Get() != queue || copy->native.Get() != native.Get() ||
        copy->image.Description().width != image.width || copy->image.Description().height != image.height ||
        copy->image.Description().format != image.format) {
        if (copy && !copy->fences.Retired(copy->native_done, copy->effects_done)) return complete(nullptr);
        auto next = std::make_unique<PresentCopy>();
        ComPtr<ID3D12Device> effects;
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&effects))) ||
            FAILED(next->image.Create(native.Get(), effects.Get(), image)) ||
            FAILED(next->fences.Create(native.Get(), effects.Get(), queue))) return complete(nullptr);
        next->native = native;
        next->queue = queue;
        copy = std::move(next);
    }
    if (copy->effects_done && FAILED(copy->fences.WaitNative(copy->effects_done))) return complete(nullptr);
    copy->fences.NativeContext()->CopySubresourceRegion(copy->image.Native(), 0, 0, 0, 0, backbuffer.Get(), 0, nullptr);
    if (FAILED(copy->fences.SignalNative(&copy->native_done)) ||
        FAILED(copy->fences.WaitEffects(copy->native_done))) return complete(nullptr);
    complete(copy->image.Effects());
    copy->fences.SignalEffects(&copy->effects_done);
}
int FormatStatus(char* out, size_t capacity)
{
    return std::snprintf(out, capacity, "native11_dlaa delivered=%llu busy=%llu rejected=%llu preset=%u sharpness=%u raster_slot=%d \"%s\"",
                         static_cast<unsigned long long>(delivered.load()),
                         static_cast<unsigned long long>(busy.load()),
                         static_cast<unsigned long long>(rejected.load()), applied_preset.load(), applied_sharpness.load(),
                         raster_slot.load(), reason.load());
}
bool FormatMenuStatus(char* out, size_t capacity)
{
    const auto last = delivered_at.load(std::memory_order_relaxed);
    const auto size = delivered_size.load(std::memory_order_relaxed);
    const bool running = j2::Enabled() && !j2::Paused() && last && GetTickCount64() - last < 1000;
    if (running)
        std::snprintf(out, capacity, "Native DX11 DLAA running at %u × %u. %llu frames improved.%s",
                      unsigned(size >> 32), unsigned(size), static_cast<unsigned long long>(delivered.load()),
                      raster_slot.load() < 0 ? " Camera motion is available. Jitter is waiting for the vertex camera." : "");
    else {
        const auto provider = native11::Stats();
        const char* waiting = !delivered.load() && provider.last_miss && provider.last_miss[0]
            ? provider.last_miss : reason.load();
        std::snprintf(out, capacity, "%s", j2::Paused() ? "Native DX11 DLAA is off." : waiting);
    }
    return running;
}
} // namespace na::native_d3d11
