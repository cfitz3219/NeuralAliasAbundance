#include "live_dlaa.hpp"
#include "shared_resources.hpp"
#include "core/motion_arbiter_gpu.hpp"
#include "core/temporal_jitter.hpp"
#include "core/referee_companion.hpp"
#include "core/referee_jitter.hpp"
#include "host_dx12/diag/referee_recovery.hpp"
#include "host_dx12/native11/viewport_jitter.hpp"
#include "host_dx12/adapters/wwii/scene_camera.hpp"
#include "host_dx12/capture/frame_capture.hpp"
#include "host_dx12/capture/j2_capture.hpp"
#include "host_dx12/depth_format.hpp"
#include "host_dx12/dlss/dlss_backend.hpp"
#include "host_dx12/host_boot/session.hpp"
#include "host_dx12/diag/event_log.hpp"
#include "host_dx12/j2/live_dlaa.hpp"
#include "host_dx12/j2/compute_flow.hpp"
#include "host_dx12/j2/replay_motion.hpp"
#include "host_dx12/j2/j2_flow.hpp"
#include "host_dx12/effects/xegtao.hpp"
#include "host_dx12/effects/donut.hpp"
#include "host_dx12/nr/native_fg.hpp"
#include "host_dx12/native11/camera_pick.hpp"
#include "host_dx12/native11/draw_replay.hpp"
#include "host_dx12/native11/input_provider.hpp"
#include "host_dx12/native11/textures.hpp"
#include "host_dx12/observe/producers.hpp"
#include "host_dx12/settings.hpp"
#include "dlaa_remote/client.hpp"
#include "host_dx12/fg/frame_gen.hpp"
#include "host_dx12/on12/d3d11_on12.hpp"
#include "host_dx12/fold/fold_pass.hpp"
#include "core/fold_shading.hpp"
#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>
#include <vector>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>

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
    // Companion route only: the recording that follows DLSS. DLSS itself then runs in another
    // program between the two, so one frame's work is two lists on the same allocator.
    ComPtr<ID3D12GraphicsCommandList> after;
    ComPtr<ID3D11Texture2D> packed_depth;
    ComPtr<ID3D11ShaderResourceView> packed_view;
    ComPtr<ID3D11UnorderedAccessView> float_view;
    DXGI_FORMAT depth_format = DXGI_FORMAT_UNKNOWN;
    uint64_t native_done = 0, effects_done = 0;
    std::shared_ptr<xegtao::Frame> ambient;
    std::shared_ptr<donut::Frame> grade;
    // Draw replay: this frame's replayed positions, packed by the game's context (native11/draw_replay.hpp Pack).
    SharedTexture positions;
    ComPtr<ID3D11UnorderedAccessView> positions_view;
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
    uint64_t stream = 0, last_frame = 0; // the last DELIVERED frame and its input stream
    uint64_t requested_jitter_frame = 0;
    float requested_jitter_x = 0.f, requested_jitter_y = 0.f;
    uint8_t requested_jitter_amount = 0;
    // What was ACQUIRED, kept apart from what was delivered (Astra's correction 4, 2026-11-16): a
    // frame whose DLSS step failed after its recording was submitted has still given its picture to
    // the next pair, and a frame that was never submitted has given nothing.
    uint64_t camera_frame = 0;     // the last frame the camera tracker was fed
    uint64_t flow_frame = 0;       // the last frame whose picture (flow recording) was really submitted
    uint64_t last_reset_frame = 0; // the newest frame delivered with a DLSS reset
    bool measured_last = false;    // the last delivered frame's motion was measured, not from a camera
    neural_alias::JitterMode jitter_history{}; // established by delivered DLAA history
    bool failed = false;
    // Draw replay (native11/draw_replay.hpp): the DX12 host's rasteriser (j2/replay_motion.hpp) on
    // this device. Each frame's positions are copied out of its slot's shared texture into one of
    // these buffers, which stays as the next frame's history; a buffer is reused once the DX12 work
    // that last read it has completed.
    j2::replay_motion::Pass replay;
    bool replay_tried = false;
    std::atomic<HRESULT> replay_handoff_hr{S_OK};
    struct ReplayBuffer { ComPtr<ID3D12Resource> buffer; uint64_t native_done = 0, effects_done = 0; };
    ReplayBuffer replay_buffers[3];
    int replay_prev = -1;          // the buffer holding the last submitted frame's positions (-1 none)
    uint64_t replay_prev_frame = 0;
    std::vector<observe::ReplayRecord> replay_prev_draws;
    uint64_t replay_fed_frame = 0; // the last frame DLSS got replay motion on
    // VARIANT: dlss/in-companion
    // DLSS runs in the 64-bit companion program instead of in this process (a 32-bit game cannot
    // load it). Chosen by what this build is, not by which game it is in.
    bool companion = false;
    // DLSS Quality / Performance (the fold, shared with the DX12 route: fold/fold_pass.hpp).
    na::fold::Pass fold;
    na::fold::Mode fold_next = na::fold::Mode::Off; // the level the next frame's scene is drawn for
    uint64_t fold_rate_frame = 0;                   // the frame the current shading rate was asked after
    uint64_t fold_shown_frames[na::fold::kModes]{}, fold_warm_frames = 0, fold_warm_failed = 0, fold_coarse_frames = 0, fold_fine_frames = 0, fold_unable = 0;
    // Folded flow (as on DX12): the frame whose folded picture the folded working set holds.
    uint64_t folded_last_frame = 0;
    na::fold::Mode folded_last_mode = na::fold::Mode::Off;
    // The picture the levels' DLSS features were made for (with DLAA's; see the fold block).
    uint32_t levels_w = 0, levels_h = 0, levels_preset = ~0u;
    DXGI_FORMAT levels_format = DXGI_FORMAT_UNKNOWN;
    // GPU time of this route's own stages (timestamps on the effects list, read back a few frames later).
    ComPtr<ID3D12QueryHeap> gpu_queries;
    ComPtr<ID3D12Resource> gpu_readback;
    uint64_t gpu_hz = 0;
    bool gpu_recorded[kSlots]{};
};
// Retain submitted work through DLL/process teardown. The game owns its
// immediate context and can still have queued copies when shutdown starts.
Session& State() { static auto* s = new Session; return *s; }
std::mutex mutex;
std::atomic<const char*> reason{"Waiting for a native scene."};
std::atomic<uint64_t> delivered{0}, busy{0}, rejected{0};
// WHY log: frames on which DLSS was told to drop its history, by cause. A reset on every frame
// leaves nothing to accumulate, and the output is then just the jittered frame.
std::atomic<uint64_t> resets_cut{0}, resets_changed{0}, resets_gap{0}, resets_jitter_switch{0};
// VARIANT: motion/measured-total
// Which supplier gave each delivered frame its motion, and the measured supplier's own resets:
// no pair yet (the first picture, by compute_flow's Ticket::no_pair code) and a measured scene cut.
std::atomic<uint64_t> supplied_camera{0}, supplied_measured{0}, resets_no_pair{0}, resets_measured_cut{0};
std::atomic<uint64_t> measured_unavailable{0}, jitter_kept_after_miss{0};
std::atomic<uint32_t> last_no_pair{0};
// Jitter for the next frame is asked for once this frame's recording is submitted, for this many
// frames after the last delivered one. Longer without a delivery and it stops, so a game whose
// DLAA step keeps failing does not show its own picture shaking.
constexpr uint64_t kJitterGraceFrames = 8;
std::atomic<uint64_t> delivered_size{0}, delivered_at{0};
std::atomic<uint32_t> applied_preset{0}, applied_sharpness{0};
// Frames whose ambient shading or bloom picture was copied into the shared colour for the companion.
std::atomic<uint64_t> companion_shaded{0};
std::atomic<int> raster_slot{-1};
std::atomic<bool> evaluation_capture_pending{false};
// Draw replay: when replay motion last reached DLSS (menu status).
std::atomic<uint64_t> replay_fed_at{0}, replay_delivered{0};
// DLSS Quality / Performance on this route: the level's plain-word state, the shading rate asked of the
// provider, and the texture-detail step that goes with a folded level.
na::fold::Status g_fold_status;
std::atomic<uint32_t> g_fold_rate{0};
std::atomic<int> g_fold_mip_extra{0};
constexpr uint64_t kNativeView = 0x4e41544956453131ull;
constexpr uint64_t kFoldView = kNativeView + 0x10; // + level
// Per-stage GPU ms (last and average over the last 64 delivered frames): motion (flow + arbiter), the
// fold, DLSS (with a warming level), and the whole effects list.
enum GpuStage : uint32_t { kGpuStart, kGpuMotion, kGpuFold, kGpuDlss, kGpuEnd, kGpuMarks };
std::atomic<float> gpu_last[4]{}, gpu_avg[4]{};
std::atomic<uint64_t> gpu_samples{0};
std::atomic<uint64_t> folded_flow_frames{0}, sampled_frames{0}; // frames measured on the folded picture; frames whose motion was sampled

// Turns a level's shading rate on or off for the frames after `frame` (the provider applies it from
// its next frame start).
void fold_rate(Session& s, na::fold::Mode m, uint64_t frame)
{
    const uint32_t rate = na::fold::ShadingRateFor(m);
    if (rate == g_fold_rate.load(std::memory_order_relaxed)) return;
    g_fold_rate.store(rate, std::memory_order_relaxed);
    native11::SetSceneCoarseShading(rate);
    s.fold_rate_frame = frame;
    g_fold_mip_extra.store(m != na::fold::Mode::Off ? -1 : 0, std::memory_order_relaxed);
    na::eventlog::Emit("fold11", "block shading asked: rate=%u (0 none, 4 = 2x1, 5 = 2x2) from frame %llu", rate, static_cast<unsigned long long>(frame));
}

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
    // Queue rule. With the DX11 proxy (a package that offers live Frame Generation) DLAA submits on the
    // per-adapter queue the chain presents from: Frame Generation needs its depth and motion on the queue
    // that generates. Otherwise DLAA keeps its own queue, exactly as before.
    if (na::on12::NativeProxyOn()) {
        s.queue.Attach(na::on12::NativePresentQueue(native));
        eventlog::Emit("native11_dlaa", "queue: the DX11 proxy's presentation queue (live Frame Generation needs its inputs there)");
    } else {
        D3D12_COMMAND_QUEUE_DESC q{};
        q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(s.device->CreateCommandQueue(&q, IID_PPV_ARGS(&s.queue)))) return false;
        eventlog::Emit("native11_dlaa", "queue: its own (no DX11 proxy in this package)");
    }
    if (!s.queue ||
        FAILED(s.fences.Create(native, s.device.Get(), s.queue.Get())) ||
        !s.arbiter.Init(s.device.Get(), kSlots)) return false;
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetTempPathW(MAX_PATH, path);
    if (!n || n >= MAX_PATH - 20) return false;
    wcscat_s(path, L"NeuralAlias-ngx");
    CreateDirectoryW(path, nullptr);
    s.companion = dlaa_remote::Wanted();
    if (s.companion ? !dlaa_remote::Start(s.device.Get(), s.queue.Get(), path) : !dlss::EnsureOn(s.device.Get(), path)) return false;
    s.native = native;
    return true;
}

// `shared`: the companion program opens this texture too.
bool Texture(ID3D12Device* dev, const ImageDesc& extent, DXGI_FORMAT format, ID3D12Resource** out, bool shared = false)
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
    return SUCCEEDED(dev->CreateCommittedResource(&heap, shared ? D3D12_HEAP_FLAG_SHARED : D3D12_HEAP_FLAG_NONE, &d, kWrite,
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
        !Texture(dev, color, DXGI_FORMAT_R32G32_FLOAT, &slot.motion, s.companion) ||
        !Texture(dev, color, DXGI_FORMAT_R8_UINT, &slot.provenance) ||
        FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(),
                                       nullptr, IID_PPV_ARGS(&slot.list))) ||
        FAILED(slot.list->Close())) return false;
    if (s.companion && (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(),
                                                      nullptr, IID_PPV_ARGS(&slot.after))) ||
                        FAILED(slot.after->Close()))) return false;
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
    if (c.first_constant < c.bytes_first_constant) return {};
    const size_t start = size_t(c.first_constant - c.bytes_first_constant) * 16;
    if (!c.bytes || start >= c.size) return {};
    const size_t length = c.num_constants ? (std::min)(size_t(c.num_constants) * 16, c.size - start) : c.size - start;
    std::vector<uint8_t> bytes(c.bytes + start, c.bytes + start + length);
    // The tracker's previous camera must be the frame before this one. A frame delivered with
    // measured motion in between (no camera that frame) leaves the tracker a frame behind.
    if (frame.selection_changed || frame.stream != s.stream || frame.frame != s.last_frame + 1 ||
        frame.frame != s.camera_frame + 1) {
        s.camera.Reset();
        s.pick = {};
    }
    s.camera_frame = frame.frame;
    // A viewport shift never enters the uploaded matrix. Removing it from the
    // matrix here would also create false camera motion on a still scene.
    const auto& rf = frame.raster; // the raster sample: a fact of the frame, not of the camera
    const bool matrix_jitter = rf.applied && !rf.viewport;
    const double jitter[2]{matrix_jitter ? rf.jitter_px[0] : 0, matrix_jitter ? rf.jitter_px[1] : 0};
    camera::CameraFrame result;
    wwii::SceneCamera scene;
    const auto raster = [&](const camera::Matrix& vp) {
        return native11::RasterViewProjection(vp, rf.viewport && rf.expanded, w, h);
    };
    if (c.split_pose) {
        result = s.camera.UpdateCameraRelative(raster(c.split_pose->projection), c.split_pose->origin,
                                               frame.frame, w, h, jitter);
    } else if (c.no_inverse_pose) {
        // Admitted by the shared no-inverse rules in the provider: a proof over several frames.
        result = s.camera.Update(raster(*c.no_inverse_pose), frame.frame, w, h, jitter);
    } else if (wwii::DecodeSceneCamera(bytes, static_cast<uint32_t>(length), w, h, scene)) {
        result = s.camera.UpdateCameraRelative(raster(scene.vp), scene.origin, frame.frame, w, h, jitter);
    } else {
        native11::CameraPick pick;
        if (!native11::CheckLearned(bytes, s.pick, &pick))
            pick = native11::FindCamera(bytes, c.version ? c.version : frame.frame + 1);
        if (!pick.found) return {};
        s.pick = pick;
        result = s.camera.Update(raster(pick.vp), frame.frame, w, h, jitter);
    }
    if (rf.viewport && rf.applied) {
        result.jitter_px_x = rf.jitter_px[0];
        result.jitter_px_y = rf.jitter_px[1];
    }
    return result;
}

// What the replay pass learned about each draw goes to the DX11 producer's movers table.
void ReplayLearnSink(const observe::ReplayObservation* seen, size_t n)
{
    std::vector<native11::draw_replay::Observation> o(n);
    for (size_t i = 0; i < n; ++i) o[i] = {seen[i].sig, seen[i].vb, seen[i].visible, seen[i].apart};
    native11::draw_replay::Learn(o.data(), n);
}

HRESULT MakeReplayBuffer(ID3D12Device* dev, ComPtr<ID3D12Resource>& out)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = uint64_t(native11::draw_replay::kCapacity) * 16u;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, kCommon, nullptr, IID_PPV_ARGS(&out));
}

void ReplayHandoffFailed(Session& s, HRESULT hr, const char* operation)
{
    s.replay_handoff_hr.store(hr, std::memory_order_relaxed);
    na::eventlog::Emit("draw_replay11_pass",
        "shared_positions=failed operation=%s hr=0x%08lX format=RGBA8_UNORM size=%ux%u; replay disabled for this device session",
        operation, static_cast<unsigned long>(hr), native11::draw_replay::kPackWidth, native11::draw_replay::kPackRows);
}

bool FailSession(Session& s, const char* why)
{
    s.failed = true;
    return Skip(why);
}
} // namespace

void SyncCameraUse()
{
    const bool paused = !j2::Enabled() || j2::Paused();
    if (g_fold_rate.load(std::memory_order_relaxed)) {
        // Coarse shading only while folded frames are really delivered: a paused or stalled DLAA never
        // leaves the game's own picture drawn in blocks.
        const auto last = delivered_at.load(std::memory_order_relaxed);
        if (paused || !last || GetTickCount64() - last > 250) {
            std::lock_guard<std::mutex> lk(mutex);
            auto& s = State();
            fold_rate(s, na::fold::Mode::Off, 0);
            s.fold_next = na::fold::Mode::Off;
        }
    }
    native11::SetEvaluationPaused(paused);
    native11::SetDepthReads(!paused && j2::Jitter());
    // Apply the switch at Present even when a frame supplies no scene to DLAA.
    const auto& options = settings::Get();
    native11::draw_replay::Configure(!paused && options.j2_draw_replay.load(std::memory_order_relaxed) != 0 &&
                                     SUCCEEDED(State().replay_handoff_hr.load(std::memory_order_relaxed)),
                                     options.j2_replay_budget.load(std::memory_order_relaxed),
                                     options.j2_replay_movers.load(std::memory_order_relaxed) != 0);
    const int setting = settings::Get().j2_camera.load(std::memory_order_relaxed);
    // XeGTAO reads projection geometry independently of DLAA's motion supplier.
    const bool ambient = options.xegtao_enabled.load(std::memory_order_relaxed) &&
                         options.xegtao_amount.load(std::memory_order_relaxed) != 0;
    native11::SetCameraUse(setting == 2 ? native11::CameraUse::Required :
        (setting == 1 || ambient) ? native11::CameraUse::Optional : native11::CameraUse::Off);
}

bool EvaluateFrame(void*, void* context, const native11::SceneFrame& frame)
{
    // The native provider has witnessed a real rendered scene. Wake the
    // existing session worker so it can qualify settings and enable effects.
    if (context) host::RendererObserved();
    native11::SetNextJitter(0, 0);
    // Seal at the scene boundary. Later draws belong to post-processing or HUD.
    const bool replay_setting = settings::Get().j2_draw_replay.load(std::memory_order_relaxed) != 0 &&
                                SUCCEEDED(State().replay_handoff_hr.load(std::memory_order_relaxed));
    const auto sealed = replay_setting ? native11::draw_replay::Seal(frame.frame) : native11::draw_replay::Sealed{};
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
    if (!Subresource(color.Get(), c, cd, csub, cw, ch) || !Subresource(depth.Get(), d, dd, dsub, dw, dh)) {
        // Says WHY: the common case is the game's own multisample anti-aliasing.
        if (cd.SampleDesc.Count > 1 || dd.SampleDesc.Count > 1) {
            static char why[160];
            std::snprintf(why, sizeof why,
                          "The game's own anti-aliasing is on (scene picture %ux, depth %ux multisampled): DLAA needs it off in the game's settings.",
                          cd.SampleDesc.Count, dd.SampleDesc.Count ? dd.SampleDesc.Count : 1);
            return Skip(why);
        }
        return Skip("Scene texture layout unsupported (mip, array slice or active area outside the texture).");
    }
    auto cf = static_cast<DXGI_FORMAT>(c.view_format);
    // VARIANT: colour/srgb-view-as-stored
    // A picture the game writes through an sRGB view holds
    // the same display-ready bytes as one written plainly; they are carried as stored, through
    // the plain 8-bit sibling, so nothing is converted on the way in or out.
    if (cf == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) cf = DXGI_FORMAT_R8G8B8A8_UNORM;
    const bool hdr = cf == DXGI_FORMAT_R11G11B10_FLOAT || cf == DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (!hdr && cf != DXGI_FORMAT_R8G8B8A8_UNORM) {
        if (cf == DXGI_FORMAT_B8G8R8A8_UNORM || cf == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || cf == DXGI_FORMAT_B8G8R8X8_UNORM ||
            cf == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB)
            return Skip("Scene colour is an 8-bit picture stored blue-first (BGRA); that order is not supported yet.");
        if (cf == DXGI_FORMAT_R10G10B10A2_UNORM) return Skip("Scene colour is a 10-bit picture (R10G10B10A2); not supported yet.");
        return Skip("Scene colour format unsupported.");
    }
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
    // VARIANT: camera/optional
    // [j2] camera: 2 = a camera is required (as before 2026-11-16), 1 = used when found, 0 = never
    // looked for. A camera can improve the motion; it never decides whether DLAA starts, keeps
    // delivering or keeps its history.
    const int camera_setting = settings::Get().j2_camera.load(std::memory_order_relaxed);
    const auto ambient_options = xegtao::CurrentOptions();
    const bool ambient_requested = ambient_options.enabled && ambient_options.amount != 0;
    camera::CameraFrame cam{};
    if (frame.camera_known && (camera_setting != 0 || ambient_requested)) cam = Pose(s, frame, ci.width, ci.height);
    if (camera_setting == 2 && !cam.valid) return Skip("Waiting for the current scene camera.");
    // VARIANT: motion/measured-total
    // Whose motion this frame gets. The camera's, when there is a valid one. Measured from the two
    // pictures (j2/compute_flow Frame::total) when there is none, and also on the frame a camera
    // first appears while measured frames are being delivered: the tracker has no previous camera
    // on that frame and would ask for a reset that the measured motion does not need.
    const bool measured = camera_setting != 2 &&
        (camera_setting == 0 || !cam.valid ||
            (cam.camera_cut && s.measured_last && frame.frame == s.last_frame + 1 && !frame.scene_changed));
    using Tool = referee::RecoveryTool;
    const auto lighting = donut::CurrentOptions();
    const uint32_t preset = settings::Get().j2_preset.load();
    const bool jitter_requested = j2::Jitter();
    const neural_alias::JitterDiagnostic debug_jitter{settings::Get().j2_debug_jitter.load(std::memory_order_relaxed)};
    const auto configuration = [&] {
        return referee::EvaluationConfiguration(preset == 255 ? 12u : preset, cf, df, hdr, frame.reversed_depth,
            camera_setting, measured, jitter_requested, j2::flow::Enabled(), debug_jitter.value);
    };
    // Settings changes take precedence even between GPU observations.
    const Tool tool = frame.recovery_tool != Tool::None && settings::Get().referee_enabled.load() &&
        frame.recovery_configuration == configuration() ? frame.recovery_tool : Tool::None;
    uint8_t recorded_tools = 0, applied_tool = 0;
    const auto recorded = [&](Tool alternative) {
        recorded_tools |= referee::Bit(alternative);
        if (alternative == tool) applied_tool |= referee::Bit(alternative);
    };
    observe::HostWorkScope own;
    if (!s.native && !Start(s, native.Get())) {
        // Says WHY: on the companion route the companion's own reason is the useful one.
        static char why[200];
        const auto remote = dlaa_remote::GetStats();
        if (!s.companion || !remote.reason[0]) return FailSession(s, "Native DX12 effects device could not start.");
        std::snprintf(why, sizeof why, "DLAA companion: %s", remote.reason);
        return FailSession(s, why);
    }
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
        if (s.companion && !dlaa_remote::Bind(index, ci.width, ci.height, slot->color.Effects(), slot->depth.Effects(),
                                              slot->motion.Get(), slot->output.Effects()))
            return FailSession(s, "The DLAA companion could not open the shared scene images.");
        owned = std::move(slot);
    }
    auto& slot = *owned;
    auto* list = slot.list.Get();
    // Both interop fences retired this slot. End the old recording before
    // reusing its private descriptors, flow history and effect ownership.
    nr::native::Reset(list);
    if (slot.after) nr::native::Reset(slot.after.Get());
    slot.ambient.reset();
    slot.grade.reset();
    if (FAILED(slot.allocator->Reset()) || FAILED(list->Reset(slot.allocator.Get(), nullptr)))
        return FailSession(s, "Could not reset the native effects list.");
    auto* shared_depth = slot.depth.Effects();
    // Per-stage GPU time: this slot's last readings are complete (both fences retired it), read them now.
    if (!s.gpu_queries && !s.companion) {
        D3D12_QUERY_HEAP_DESC qh{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kSlots * kGpuMarks, 0};
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = UINT64(kSlots) * kGpuMarks * 8;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(s.device->CreateQueryHeap(&qh, IID_PPV_ARGS(&s.gpu_queries))) ||
            FAILED(s.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s.gpu_readback))) ||
            FAILED(s.queue->GetTimestampFrequency(&s.gpu_hz))) {
            s.gpu_queries.Reset(); s.gpu_readback.Reset(); s.gpu_hz = 0;
        }
    }
    if (s.gpu_queries && s.gpu_recorded[index]) {
        s.gpu_recorded[index] = false;
        uint64_t* t = nullptr;
        const D3D12_RANGE r{size_t(index) * kGpuMarks * 8, size_t(index + 1) * kGpuMarks * 8};
        if (SUCCEEDED(s.gpu_readback->Map(0, &r, reinterpret_cast<void**>(&t))) && t) {
            const uint64_t* m = t + size_t(index) * kGpuMarks;
            const double ms = 1000.0 / double(s.gpu_hz);
            const float stage[4] = {float(double(m[kGpuMotion] - m[kGpuStart]) * ms), float(double(m[kGpuFold] - m[kGpuMotion]) * ms),
                                    float(double(m[kGpuDlss] - m[kGpuFold]) * ms), float(double(m[kGpuEnd] - m[kGpuStart]) * ms)};
            const uint64_t n = gpu_samples.fetch_add(1, std::memory_order_relaxed) + 1;
            const float k = n < 64 ? 1.f / float(n) : 1.f / 64.f;
            for (int i = 0; i < 4; ++i) {
                if (m[kGpuEnd] < m[kGpuStart]) break;
                gpu_last[i].store(stage[i], std::memory_order_relaxed);
                gpu_avg[i].store(gpu_avg[i].load(std::memory_order_relaxed) * (1.f - k) + stage[i] * k, std::memory_order_relaxed);
            }
            const D3D12_RANGE none{0, 0};
            s.gpu_readback->Unmap(0, &none);
        }
    }
    const auto gpu_mark = [&](GpuStage at) {
        if (s.gpu_queries && list) list->EndQuery(s.gpu_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index * kGpuMarks + at);
    };
    gpu_mark(kGpuStart);
    Barrier(list, slot.color.Effects(), kCommon, kRead);
    Barrier(list, shared_depth, kCommon, kRead);
    Barrier(list, slot.output.Effects(), kCommon, kWrite);
    motion::gpu::Inputs in{};
    in.depth = shared_depth;
    in.tier2_valid_flag = !measured; // measured: no reprojection, no matrices, no identity stand-in
    in.depth_clear = frame.reversed_depth ? 0.f : 1.f;
    if (!measured) {
        // The same as the DX12 host (j2/live_dlaa.cpp): current clip to previous clip as ONE matrix
        // composed in double. Chained per pixel as two float matrices through large world
        // coordinates, a still camera gave about 0.1 px of false motion on every frame.
        const camera::Matrix composed = camera::ComposeReprojectionRow(cam.inv_view_proj_current_row, cam.view_proj_previous_row);
        for (size_t i = 0; i < 16; ++i) {
            in.inv_view_proj_current.m[i] = static_cast<float>(composed[i]);
            in.view_proj_previous.m[i] = i % 5 == 0 ? 1.f : 0.f;
        }
    }
    // The raster sample of this picture. With a camera it is the tracker's (the same value on the
    // viewport route); with none it is the provider's own fact: the sample the scene draws carried,
    // or exactly zero when none was applied.
    in.jitter_current_px[0] = measured ? (frame.raster.applied ? frame.raster.jitter_px[0] : 0.f) : static_cast<float>(cam.jitter_px_x);
    in.jitter_current_px[1] = measured ? (frame.raster.applied ? frame.raster.jitter_px[1] : 0.f) : static_cast<float>(cam.jitter_px_y);
    // DLSS drops its history when its last picture is not the frame before this one, or the scene's
    // textures changed. With a camera, also on a camera cut or a changed camera buffer (as before).
    // With measured motion a changed camera buffer is not a change of scene; the measured supplier
    // adds its own two causes below (no pair yet, a measured scene cut).
    bool reset_history = measured ? frame.scene_changed || frame.frame != s.last_frame + 1
                                  : cam.camera_cut || frame.selection_changed || frame.frame != s.last_frame + 1;
    if (!measured && cam.camera_cut) resets_cut.fetch_add(1, std::memory_order_relaxed);
    else if (measured ? frame.scene_changed : frame.selection_changed) resets_changed.fetch_add(1, std::memory_order_relaxed);
    else if (reset_history) resets_gap.fetch_add(1, std::memory_order_relaxed);
    // A draw can still carry the previous setting for one frame. Reset on the first picture that
    // matches the new setting. Occasional missing samples while the setting is unchanged retain history.
    const auto jitter_history = neural_alias::JitterHistoryForFrame(s.jitter_history, jitter_requested,
                                                                   frame.raster.applied, debug_jitter);
    const bool jitter_switched = neural_alias::JitterHistoryChanged(s.jitter_history, jitter_history);
    reset_history |= jitter_switched;
    // Sampled motion (as on DX12): while a folded level is settled on screen and nothing else needs the
    // whole motion field, the per-pixel passes decide only the pixel that level keeps of each block,
    // written straight into its folded motion plane.
    na::fold::Mode sampled_mode = na::fold::Mode::Off;
    ID3D12Resource* sampled_motion = nullptr;
    const na::fold::Mode fold_setting = (!j2::Enabled() || j2::Paused()) ? na::fold::Mode::Off
                                        : na::fold::ModeFrom(settings::Get().j2_fold_mode.load(std::memory_order_relaxed));
    {
        const na::fold::Mode shown = s.fold.hand.shown;
        if (shown != na::fold::Mode::Off && fold_setting == shown && s.fold.hand.warming == na::fold::Mode::Off && s.fold.Ok() &&
            !ambient_requested && !s.companion && !capture::j2::Armed()) {
            const auto blk = na::fold::BlockFor(shown);
            const auto& planes = s.fold.PlanesFor(shown);
            if (planes.motion && planes.w == na::fold::Folded(ci.width, blk.x) && planes.h == na::fold::Folded(ci.height, blk.y)) {
                sampled_mode = shown;
                sampled_motion = planes.motion.Get();
            }
        }
    }
    // Folded flow (as on DX12): with no camera in use the whole measuring stage runs on the level's own
    // folded picture. The folded size's working set is built on its own thread from the moment a level is
    // asked for; the level's first frame on screen records its folded picture beside the whole-picture pair.
    bool folded_ready = false;
    if (fold_setting != na::fold::Mode::Off && measured && s.fold.Ok()) {
        const auto ab = na::fold::BlockFor(fold_setting);
        j2::compute_flow::Prepare(s.device.Get(), s.queue.Get(), na::fold::Folded(ci.width, ab.x), na::fold::Folded(ci.height, ab.y));
    }
    if (sampled_mode != na::fold::Mode::Off && measured) {
        const auto& rp = s.fold.PlanesFor(sampled_mode);
        folded_ready = j2::compute_flow::Ready(rp.w, rp.h);
    }
    const bool folded_flow = folded_ready && s.folded_last_mode == sampled_mode && s.folded_last_frame != 0 &&
                             s.folded_last_frame == s.flow_frame && s.flow_frame + 1 == frame.frame;
    const bool folded_prime = folded_ready && !folded_flow;
    j2::compute_flow::Ticket flow_prime;
    {
        static int was_folded_flow = -1;
        if (static_cast<int>(folded_flow) != was_folded_flow) {
            was_folded_flow = folded_flow ? 1 : 0;
            na::eventlog::Emit("fold11", "motion search %s at frame %llu (level %s)", folded_flow ? "ON THE FOLDED PICTURE" : "on the whole picture",
                               static_cast<unsigned long long>(frame.frame), na::fold::Name(sampled_mode));
        }
    }
    j2::compute_flow::Ticket flow;
    ID3D12Resource* screen_model = nullptr; // the measured supplier's fitted screen motion (what replay learns movers against)
    if (measured || j2::flow::Enabled()) {
        j2::compute_flow::Frame ff;
        ff.frame = frame.frame;
        // Measured: the pair is made of pictures really submitted on consecutive frames, whether or
        // not the frame before was delivered. Camera: the last delivered frame, as before.
        ff.previous = !measured ? s.last_frame : s.flow_frame + 1 == frame.frame ? s.flow_frame : 0;
        // The pictures' identity is the scene's (colour and depth objects), for both suppliers, so a
        // pair is kept across a change of supplier. A camera supplier's other reasons to start a new
        // history reach the flow through ff.reset, as they always did.
        ff.generation = static_cast<uint32_t>(frame.scene_stream); ff.source_generation = frame.scene_stream;
        ff.slot = index; ff.reset = measured ? frame.scene_changed : reset_history; ff.hdr = hdr;
        ff.jitter[0] = ff.raster_jitter[0] = in.jitter_current_px[0];
        ff.jitter[1] = ff.raster_jitter[1] = in.jitter_current_px[1];
        // Measured: the sample is exactly known on every frame (zero when none was applied), so an
        // unjittered first picture pairs with the jittered one after it.
        ff.raster_sample = true; ff.raster_confirmed = measured || frame.raster.applied;
        ff.use_screen_fit = tool != Tool::ScreenFit;
        ff.total = measured; ff.last_reset_frame = s.last_reset_frame;
        {   // A folded frame (core/fold_shading.hpp): the picture is in blocks; strict acceptance rules (as on DX12).
            const na::fold::Mode drawn = g_fold_rate.load(std::memory_order_relaxed) && frame.frame > s.fold_rate_frame ? s.fold.hand.shown : na::fold::Mode::Off;
            if (drawn != na::fold::Mode::Off) {
                const auto fb = na::fold::BlockFor(drawn);
                ff.block_x = static_cast<uint8_t>(fb.x); ff.block_y = static_cast<uint8_t>(fb.y);
            }
            j2::compute_flow::SetLoosen(drawn != na::fold::Mode::Off ? 0u : settings::kObjectFlowLoosen);
            if (sampled_mode != na::fold::Mode::Off && !folded_flow) {
                const auto sb = na::fold::BlockFor(sampled_mode);
                const auto sp = na::fold::Pick(sampled_mode);
                ff.sample_x = static_cast<uint8_t>(sb.x); ff.sample_y = static_cast<uint8_t>(sb.y);
                ff.sample_pick_x = static_cast<uint8_t>(sp.x); ff.sample_pick_y = static_cast<uint8_t>(sp.y);
            }
        }
        j2::compute_flow::Tier3 tier;
        if (folded_flow || folded_prime) {
            // The level's folded colour and depth now, on the warm-up's table (no level warms here).
            const auto fold_blk = na::fold::BlockFor(sampled_mode);
            const auto fold_pick = na::fold::Pick(sampled_mode);
            auto& fp = s.fold.PlanesFor(sampled_mode);
            s.fold.Dispatch(list, slot.color.Effects(), shared_depth, slot.motion.Get(), fp, fold_blk, fold_pick, (index % s.fold.Ring()) * 2 + 1, true);
            D3D12_RESOURCE_BARRIER fb[4]{};
            fb[0].Type = fb[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            fb[0].UAV.pResource = fp.color.Get(); fb[1].UAV.pResource = fp.depth.Get();
            list->ResourceBarrier(2, fb);
            Barrier(list, fp.color.Get(), kWrite, kRead);
            Barrier(list, fp.depth.Get(), kWrite, kRead);
            motion::gpu::Inputs fold_in = in;
            fold_in.depth = fp.depth.Get();
            j2::compute_flow::Frame fold_frame = ff;
            fold_frame.block_x = fold_frame.block_y = 1; // one shaded value a pixel in the folded picture
            fold_frame.sample_x = fold_frame.sample_y = 1; fold_frame.sample_pick_x = fold_frame.sample_pick_y = 0;
            fold_frame.jitter[0] = na::fold::DeclaredJitter(fold_blk.x, fold_pick.x, in.jitter_current_px[0]);
            fold_frame.jitter[1] = na::fold::DeclaredJitter(fold_blk.y, fold_pick.y, in.jitter_current_px[1]);
            fold_frame.raster_jitter[0] = fold_frame.jitter[0]; fold_frame.raster_jitter[1] = fold_frame.jitter[1];
            fold_in.jitter_current_px[0] = fold_frame.jitter[0]; fold_in.jitter_current_px[1] = fold_frame.jitter[1];
            j2::compute_flow::Tier3 fold_tier;
            auto fold_ticket = j2::compute_flow::Record(list, s.queue.Get(), fp.color.Get(), fp.depth.Get(), fold_in, fold_frame, fold_tier);
            Barrier(list, fp.color.Get(), kRead, kWrite);
            Barrier(list, fp.depth.Get(), kRead, kWrite);
            s.folded_last_frame = fold_ticket.context ? frame.frame : 0;
            s.folded_last_mode = sampled_mode;
            if (folded_flow) {
                flow = fold_ticket; tier = fold_tier;
                in.estimated_folded = true; // the arbiter reads it at the output position and scales it by the block
                folded_flow_frames.fetch_add(1, std::memory_order_relaxed);
            } else {
                flow_prime = fold_ticket; // its picture only; this frame's motion is the whole-picture pair's
            }
        }
        if (!folded_flow) flow = j2::compute_flow::Record(list, s.queue.Get(), slot.color.Effects(), shared_depth, in, ff, tier);
        screen_model = folded_flow ? nullptr : tier.screen_model; // a folded model is in folded pixels: nothing learned
        if (measured) {
            if (!flow.context || (flow.pair && !tier.total)) {
                // No measurement can be made at all (its programs or working memory are missing, or
                // the pair's own pass failed). Without motion DLAA would restart on every frame, so
                // the frame is left as the game drew it, and the flow's own reason says why.
                list->Close();
                measured_unavailable.fetch_add(1, std::memory_order_relaxed);
                static char why[224];
                std::snprintf(why, sizeof why, "No camera in use, and motion could not be measured: %s", j2::compute_flow::GetStats().reason);
                return Skip(why);
            }
            if (flow.pair) {
                // TOTAL motion, labelled as estimated (never as the game's own), in the slot that stands
                // behind tier 1 (motion_arbiter.hpp EstimatedInput): the same slot the DX12 host uses, so
                // a tier-1 supplier added here later covers its own pixels and cannot erase the rest.
                if (tier.screen_model || (tool == Tool::ScreenFit && tier.total_valid)) recorded(Tool::ScreenFit);
                in.estimated_valid_flag = true;
                in.estimated_motion = tier.total; in.estimated_valid = tier.total_valid;
                if (flow.cut_reset && !reset_history) {
                    // A scene cut measured on an earlier pair, learned without waiting on the GPU:
                    // this frame carries the reset. Until then those frames showed their own picture.
                    reset_history = true;
                    resets_measured_cut.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                // The first picture of a pair: nothing to measure against yet. DLSS starts a new
                // history on it (a reset needs no motion), and the next frame is measured against it.
                last_no_pair.store(flow.no_pair, std::memory_order_relaxed);
                if (!reset_history) resets_no_pair.fetch_add(1, std::memory_order_relaxed);
                reset_history = true;
            }
        } else {
            if (tier.residual) {
                in.tier3_valid_flag = true;
                in.tier3_residual = tier.residual; in.tier3_valid = tier.valid; in.tier3_mask = tier.mask;
            }
            in.fix = tier.fix;
        }
    }
    // Tier 1 from the game's own draws replayed with stream output (native11/draw_replay.hpp), as
    // the DX12 host does: exact per-object motion where a replayed draw is the visible surface. The
    // camera or the measured motion keeps every other pixel (the arbiter's tiers behind it).
    // Matrix jitter would sit inside the clip positions: replay is used with viewport jitter only.
    int replay_cur = -1, replay_prev_used = -1;
    bool replay_motion_selected = false;
    std::vector<observe::ReplayRecord> replay_draws;
    if (replay_setting && SUCCEEDED(s.replay_handoff_hr.load(std::memory_order_relaxed)) &&
        !sealed.draws.empty() && sealed.verts <= native11::draw_replay::kCapacity &&
        !(frame.raster.applied && !frame.raster.viewport)) {
        if (!s.replay_tried) {
            s.replay_tried = true;
            if (s.replay.Init(s.device.Get())) s.replay.SetLearnSink(&ReplayLearnSink);
            na::eventlog::Emit("draw_replay11_pass", "ready=%d", s.replay.Ready() ? 1 : 0);
        }
        for (int i = 0; s.replay.Ready() && i < 3 && replay_cur < 0; ++i) {
            auto& b = s.replay_buffers[i];
            if (i == s.replay_prev) continue; // the history this frame reads
            if (!s.fences.Retired(b.native_done, b.effects_done)) continue;
            if (!b.buffer) {
                const HRESULT hr = MakeReplayBuffer(s.device.Get(), b.buffer);
                if (FAILED(hr)) { ReplayHandoffFailed(s, hr, "allocate_DX12_history_buffer"); break; }
            }
            replay_cur = i;
        }
        if (replay_cur >= 0 && !slot.positions_view) {
            const ImageDesc pd{native11::draw_replay::kPackWidth, native11::draw_replay::kPackRows, DXGI_FORMAT_R8G8B8A8_UNORM, true};
            const char* operation = nullptr;
            HRESULT hr = slot.positions.Create(s.native.Get(), s.device.Get(), pd, &operation);
            if (SUCCEEDED(hr)) {
                operation = "create_DX11_pack_UAV";
                hr = s.native->CreateUnorderedAccessView(slot.positions.Native(), nullptr, &slot.positions_view);
            }
            if (FAILED(hr)) {
                slot.positions.Reset(); // This slot has retired and its new allocation has never been submitted.
                replay_cur = -1;
                ReplayHandoffFailed(s, hr, operation);
            }
        }
        // The game's context packs the positions now (in its order: after the frame's last replayed
        // draw, before the native signal below); this list copies them out after its wait.
        if (replay_cur >= 0 && !native11::draw_replay::Pack(ctx, slot.positions_view.Get(), sealed.verts)) replay_cur = -1;
    }
    if (replay_cur >= 0) {
        auto* buffer = s.replay_buffers[replay_cur].buffer.Get();
        auto* packed = slot.positions.Effects();
        const UINT rows = native11::draw_replay::PackRows(sealed.verts);
        Barrier(list, packed, kCommon, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(list, buffer, kCommon, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION to{}, from{};
        to.pResource = buffer;
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, native11::draw_replay::kPackWidth, rows, 1,
                                        native11::draw_replay::kPackWidth * 4u};
        from.pResource = packed;
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const D3D12_BOX box{0, 0, 0, native11::draw_replay::kPackWidth, rows, 1};
        list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
        Barrier(list, packed, D3D12_RESOURCE_STATE_COPY_SOURCE, kCommon);
        Barrier(list, buffer, D3D12_RESOURCE_STATE_COPY_DEST, kCommon);
        observe::ReplayFrame rf;
        rf.cur = buffer;
        replay_draws.reserve(sealed.draws.size());
        for (const auto& replayed : sealed.draws) {
            observe::ReplayRecord r{};
            r.sig = replayed.sig; r.vb = replayed.vb; r.first = replayed.first; r.verts = replayed.verts;
            r.width = replayed.width; r.height = replayed.height;
            std::copy(replayed.scissor, replayed.scissor + 4, r.scissor);
            replay_draws.push_back(r);
        }
        rf.cur_draws = replay_draws;
        if (s.replay_prev >= 0 && s.replay_prev_frame + 1 == frame.frame) {
            replay_prev_used = s.replay_prev;
            rf.prev = s.replay_buffers[s.replay_prev].buffer.Get();
            rf.prev_draws = s.replay_prev_draws;
        }
        // What the pass compares each draw with, to learn which move by themselves: the camera this
        // frame used, or with no camera the screen motion fitted for this pair (as the DX12 host).
        j2::replay_motion::Reference learn{};
        learn.depth_far = in.depth_clear;
        if (!measured && in.tier2_valid_flag) {
            learn.mode = 1;
            std::memcpy(learn.matrix, in.inv_view_proj_current.m, sizeof learn.matrix);
            learn.fix = in.fix;
        } else if (measured && screen_model) {
            learn.mode = 2;
            learn.model = screen_model;
        }
        const bool viewport_sample = frame.raster.applied && frame.raster.viewport;
        const float viewport_jitter[2] = {viewport_sample ? frame.raster.jitter_px[0] : 0.f, viewport_sample ? frame.raster.jitter_px[1] : 0.f};
        const auto rm = s.replay.Record(list, s.queue.Get(), shared_depth, ci.width, ci.height, frame.reversed_depth, reset_history,
                                        viewport_jitter, &learn, rf, viewport_sample && frame.raster.expanded);
        if (rm.motion) {
            in.tier1_motion = rm.motion;
            in.tier1_valid = rm.valid;
            in.tier1_valid_flag = true;
            in.tier1_source = motion::Tier1Source::Engine;
            s.replay_fed_frame = frame.frame;
            replay_motion_selected = true;
        }
    }
    if (replay_setting) {
        // WHY log, as the DX12 host's draw_replay lines: what was replayed, what stopped the rest,
        // what the pass matched and drew, and what the movers rule spends the budget on.
        const auto p = s.replay.GetStats();
        static uint64_t next_log = 0, logs = 0;
        if (frame.frame >= next_log) {
            next_log = frame.frame + (logs ? 600 : 120);
            ++logs;
            const auto q = native11::draw_replay::GetStats();
            na::eventlog::Emit("draw_replay11", "route=native_dx11 on=%d noted=%llu built=%llu build_failed=%llu replayed=%llu verts=%llu "
                "twin_queued=%llu building=%llu twin_failed=%llu no_shader=%llu overflow=%llu sealed=%llu packed=%llu pack_failed=%llu gates{%s} | "
                "frames=%llu fed=%llu draws=%llu prev_draws=%llu exact=%llu unmatched=%llu raster_verts=%llu gpu_read=%llu gpu_nonzero=%llu "
                "gpu_samples=%llu gpu_last=%u last_draws=%u last_pairs=%u fed_now=%d",
                q.on ? 1 : 0, (unsigned long long)q.noted, (unsigned long long)q.built, (unsigned long long)q.build_failed,
                (unsigned long long)q.replayed, (unsigned long long)q.vertices, (unsigned long long)q.twin_queued,
                (unsigned long long)q.twin_building, (unsigned long long)q.twin_failed, (unsigned long long)q.no_shader,
                (unsigned long long)q.overflow, (unsigned long long)q.sealed, (unsigned long long)q.packed, (unsigned long long)q.pack_failed, q.gates,
                (unsigned long long)p.frames, (unsigned long long)p.fed, (unsigned long long)p.draws, (unsigned long long)p.prev_draws,
                (unsigned long long)p.matched_exact, (unsigned long long)p.unmatched, (unsigned long long)p.verts,
                (unsigned long long)p.gpu_read, (unsigned long long)p.gpu_nonzero, (unsigned long long)p.gpu_samples, p.gpu_last,
                p.last_draws, p.last_pairs, s.replay_fed_frame == frame.frame ? 1 : 0);
            na::eventlog::Emit("draw_replay11_budget", "variant=replay/movers-first budget=%llu threshold=%u of 65536 not_sampled=%llu "
                "draw_too_large=%llu frame_full=%llu movers_on=%d movers=%llu suspects=%llu promoted=%llu let_go=%llu mover_draws=%llu "
                "mover_verts=%llu movers_left_out=%llu rotating_share_verts=%llu reports=%llu compared_with{camera=%llu screen_fit=%llu nothing=%llu} "
                "samples{seen=%llu apart=%llu}",
                (unsigned long long)q.budget, q.budget_threshold, (unsigned long long)q.not_sampled, (unsigned long long)q.draw_too_large,
                (unsigned long long)q.frame_full, q.movers_on ? 1 : 0, (unsigned long long)q.movers, (unsigned long long)q.suspects,
                (unsigned long long)q.promoted, (unsigned long long)q.let_go, (unsigned long long)q.mover_draws, (unsigned long long)q.mover_verts,
                (unsigned long long)q.movers_left_out, (unsigned long long)q.probe_verts, (unsigned long long)q.reports,
                (unsigned long long)p.learn_camera, (unsigned long long)p.learn_screen, (unsigned long long)p.learn_none,
                (unsigned long long)p.learn_visible, (unsigned long long)p.learn_apart);
        }
    }
    motion::gpu::Outputs motion_out{slot.motion.Get(), slot.provenance.Get(), nullptr, ci.width, ci.height};
    if (sampled_mode != na::fold::Mode::Off) { // straight into the level's folded plane (UNORDERED_ACCESS at rest)
        sampled_frames.fetch_add(1, std::memory_order_relaxed);
        const auto sb = na::fold::BlockFor(sampled_mode);
        const auto sp = na::fold::Pick(sampled_mode);
        const auto& planes = s.fold.PlanesFor(sampled_mode);
        motion_out.xy = sampled_motion;
        motion_out.width = planes.w; motion_out.height = planes.h; // the provenance's top left holds the kept pixels' tags
        motion_out.block_x = sb.x; motion_out.block_y = sb.y; motion_out.pick_x = sp.x; motion_out.pick_y = sp.y;
        motion_out.source_width = ci.width; motion_out.source_height = ci.height;
    }
    motion::gpu::RecordedSuppliers suppliers;
    in.prefer_camera_without_residual = tool == Tool::CameraResidual;
    if (!s.arbiter.Dispatch(list, in, motion_out, index, &suppliers)) {
        list->Close();
        return Skip(measured ? "Measured motion could not be recorded." : "Camera motion could not be recorded.");
    }
    if (suppliers.camera_with_residual_available) recorded(Tool::CameraResidual);
    if (!suppliers.estimated) {
        recorded_tools &= uint8_t(~referee::Bit(Tool::ScreenFit));
        applied_tool &= uint8_t(~referee::Bit(Tool::ScreenFit));
    }
    Barrier(list, slot.motion.Get(), kWrite, kRead);
    gpu_mark(kGpuMotion);
    dlss::EvalDesc eval{};
    eval.color = slot.color.Effects();
    // The two optional passes (ambient shading, bloom) each hand DLSS a picture of their own. On the
    // companion route that picture is copied into the shared scene colour below, because the shared
    // colour is the one the companion reads.
    if (ambient_requested) {
        xegtao::Inputs ai;
        ai.color = eval.color; ai.depth = shared_depth; ai.motion = slot.motion.Get();
        ai.color_format = cf; ai.width = ci.width; ai.height = ci.height;
        ai.srgb = !hdr; ai.clear_depth = frame.reversed_depth ? 0.f : 1.f;
        ai.jitter_x = in.jitter_current_px[0]; ai.jitter_y = in.jitter_current_px[1];
        ai.frame = frame.frame; ai.motion_reference_frame = s.last_frame;
        ai.scene_epoch = frame.scene_stream; ai.reset = reset_history || cam.camera_cut;
        const auto column = camera::TransposeMatrix(cam.view_proj_current_row);
        // The projection is independent of whether DLAA uses measured motion during camera acquisition.
        if (!cam.valid) xegtao::ScenePass().Unavailable("Waiting for scene projection for XeGTAO.");
        else if (xegtao::FromViewProjection(column.data(), ai.projection)) {
            auto options = ambient_options;
            const bool history_requested = options.temporal > 0;
            if (tool == Tool::AmbientHistory) options.temporal = 0;
            slot.ambient = xegtao::ScenePass().Prepare(s.device.Get(), s.queue.Get(), ai, options);
            if (slot.ambient && slot.ambient->Record(list)) {
                eval.color = slot.ambient->Color();
                if (history_requested && (tool == Tool::AmbientHistory ||
                    slot.ambient->CompositedVisibility() != slot.ambient->Occlusion())) recorded(Tool::AmbientHistory);
            }
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
    const auto declared = debug_jitter.Declared(in.jitter_current_px[0], in.jitter_current_px[1]);
    eval.jitter_x = declared.x;
    eval.jitter_y = declared.y;
    eval.reset = reset_history;
    eval.preset = preset == 255 ? 12 : preset;
    eval.view_id = kNativeView;
    eval.ordered_feature_group = kNativeView; // the folded levels' features are created in the safe order (dlss_backend)
    if (lighting.tone || lighting.bloom) {
        donut::Inputs gi;
        gi.color = eval.color; gi.format = cf; gi.width = ci.width; gi.height = ci.height;
        gi.srgb = !hdr; gi.reset = reset_history; gi.stream = kNativeView;
        auto options = lighting;
        if (tool == Tool::Bloom) options.bloom_amount *= .5f;
        slot.grade = donut::ScenePass().Prepare(s.device.Get(), s.queue.Get(), gi, options);
        if (slot.grade && slot.grade->Record(list)) {
            eval.color = slot.grade->Color();
            if (options.bloom && options.bloom_amount > 0) recorded(Tool::Bloom);
        }
    }
    // DLSS Quality / Performance (the fold, fold/fold_pass.hpp, the same as on the DX12 route). The level
    // on screen keeps running; a newly asked level is run beside it into a picture nobody sees until it has
    // proven itself (Handover). A level that cannot save the game's shading is refused when it is chosen,
    // and the level on screen stays.
    ID3D12Resource* const full_color = eval.color; // full size: what a measured cut shows (RecordCutShow)
    na::fold::Mode fold_shown = na::fold::Mode::Off;
    // CPU time of this frame's level work (ms), logged on the frames of a waiting period and a switch.
    const auto now_ms = [] { LARGE_INTEGER t, f; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f); return double(t.QuadPart) * 1000.0 / double(f.QuadPart); };
    double fold_cpu_prepare = 0, fold_cpu_warm = 0, fold_cpu_show = 0;
    bool fold_log_frame = false;
    {
        using na::fold::Mode;
        const bool paused = !j2::Enabled() || j2::Paused();
        const Mode asked = paused ? Mode::Off : na::fold::ModeFrom(settings::Get().j2_fold_mode.load(std::memory_order_relaxed));
        const char* why = nullptr;
        const char* coarse_why = nullptr;
        const double t_prepare = now_ms();
        if (asked != Mode::Off || s.fold.hand.shown != Mode::Off) {
            const DXGI_FORMAT in_format = eval.color->GetDesc().Format;
            if (s.companion) why = "DLSS runs in the separate 64-bit helper program for this game, where the folded picture cannot be handed over yet";
            else if (!native11::CoarseShadingAvailable(&coarse_why)) why = na::fold::kNoCoarseShading;
            else if (!s.fold.Init(s.device.Get(), kSlots)) why = "the fold pass could not be prepared on this graphics card";
            else for (const Mode m : {asked, s.fold.hand.shown})
                if (m != Mode::Off && !why && !s.fold.Targets(m, ci.width, ci.height, in_format, ci.width, ci.height, slot.output.Effects()->GetDesc().Format))
                    why = "the folded working textures could not be made for this colour format";
        }
        fold_cpu_prepare = now_ms() - t_prepare;
        g_fold_status.Why("fold11", why);
        if (why) ++s.fold_unable;
        // The levels' DLSS features are made together with DLAA's, on the first frame at this picture size and
        // before DLAA's own (smallest first: DLSS 310.9.16 spoils a larger feature when a smaller one is made after
        // it, and making DLAA's again would cost the same once more and restart its history). A level asked for
        // later has nothing left to make: its waiting period only warms its history and the switch adds no work.
        bool prewarmed = false;
        {
            const DXGI_FORMAT in_format = eval.color->GetDesc().Format;
            if (settings::Get().j2_fold_prewarm.load(std::memory_order_relaxed) && !s.companion && native11::CoarseShadingAvailable() &&
                (s.levels_w != ci.width || s.levels_h != ci.height || s.levels_format != in_format || s.levels_preset != preset)) {
                const double t_levels = now_ms();
                const DXGI_FORMAT out_format = slot.output.Effects()->GetDesc().Format;
                bool ok = s.fold.Init(s.device.Get(), kSlots);
                for (const Mode m : {Mode::Performance, Mode::Quality})
                    ok = ok && s.fold.Targets(m, ci.width, ci.height, in_format, ci.width, ci.height, out_format);
                for (const Mode m : {Mode::Performance, Mode::Quality}) {
                    if (!ok) break;
                    dlss::EvalDesc pe{};
                    s.fold.Record(list, eval, m, index, m == Mode::Quality, &pe, kFoldView, preset); // its own table each
                    pe.output = s.fold.Unseen();
                    pe.reset = true;
                    ok = dlss::Evaluate(list, pe);
                    s.fold.Done(list, m);
                }
                prewarmed = true;
                s.levels_w = ci.width; s.levels_h = ci.height; s.levels_format = in_format; s.levels_preset = preset;
                na::eventlog::Emit("fold11", "levels' DLSS made with DLAA's at %ux%u (Performance, then Quality, then DLAA): %s in %.2f ms on this first frame",
                                   ci.width, ci.height, ok ? "ready" : "NOT made (a level asked for will make its own in its waiting period)", now_ms() - t_levels);
            }
        }
        // On that frame the tables are taken: the screen shows DLAA (a new picture size starts DLSS over anyway).
        const auto step = s.fold.hand.Begin(prewarmed ? Mode::Off : asked, !why && !prewarmed);
        fold_log_frame = step.warm != Mode::Off || step.switched;
        const bool coarse = g_fold_rate.load(std::memory_order_relaxed) != 0 && frame.frame > s.fold_rate_frame;
        if (step.warm != Mode::Off) {
            dlss::EvalDesc we{};
            s.fold.Record(list, eval, step.warm, index, true, &we, kFoldView, preset);
            we.output = s.fold.Unseen();
            we.reset = eval.reset || step.warm_reset;
            const double t_warm = now_ms();
            const bool ok = dlss::Evaluate(list, we);
            fold_cpu_warm = now_ms() - t_warm;
            s.fold.Done(list, step.warm);
            s.fold.hand.WarmResult(ok);
            ++s.fold_warm_frames;
            if (!ok) {
                ++s.fold_warm_failed;
                static char warm_why[160];
                std::snprintf(warm_why, sizeof warm_why, "DLSS did not accept the folded picture while it was being readied: %s", dlss::GetStats().reason);
                g_fold_status.Why("fold11", warm_why);
            }
            if (s.fold.hand.refused == step.warm)
                na::eventlog::Emit("fold11", "REFUSED %s after %u failed warm-up frames: DLSS did not accept the folded picture (%ux%u from %ux%u). The screen stays on %s.",
                                   na::fold::Name(step.warm), na::fold::Handover::kGiveUp, we.render_w, we.render_h, ci.width, ci.height, na::fold::Name(s.fold.hand.shown));
        }
        if (step.show_reset) eval.reset = true;
        // Sampled motion holds only the kept pixels of the level decided before this point. Should the screen
        // not show that level after all, this frame has no usable motion: DLSS restarts its history on it.
        const bool sampled_ok = sampled_mode != Mode::Off && step.show == sampled_mode && s.fold.PlanesFor(sampled_mode).motion.Get() == sampled_motion;
        if (sampled_mode != Mode::Off && !sampled_ok) {
            eval.reset = true;
            na::eventlog::Emit("fold11", "sampled motion for %s not usable at frame %llu (screen shows %s): DLSS history restarted on this frame",
                               na::fold::Name(sampled_mode), static_cast<unsigned long long>(frame.frame), na::fold::Name(step.show));
        }
        if (step.show != Mode::Off) {
            dlss::EvalDesc fe{};
            s.fold.Record(list, eval, step.show, index, false, &fe, kFoldView, preset, sampled_ok);
            eval = fe;
            fold_shown = step.show;
            ++(coarse ? s.fold_coarse_frames : s.fold_fine_frames);
        }
        ++s.fold_shown_frames[static_cast<uint32_t>(step.show)];
        // The level the next frame's scene is drawn for: shading rate, sample spacing and texture detail.
        s.fold_next = asked != Mode::Off ? s.fold.hand.shown : Mode::Off;
        fold_rate(s, s.fold_next, frame.frame);
        const auto cs = native11::GetCoarseShadingStats();
        if (step.switched)
            na::eventlog::Emit("fold11", "SWITCHED to %s at frame %llu: DLSS input %ux%u -> output %ux%u, history %s. asked=%s switches=%llu refusals=%llu",
                               na::fold::Name(step.show), static_cast<unsigned long long>(frame.frame), eval.render_w, eval.render_h, eval.out_w, eval.out_h,
                               step.show_reset ? "restarted" : "already warm", na::fold::Name(asked),
                               static_cast<unsigned long long>(s.fold.hand.switches), static_cast<unsigned long long>(s.fold.hand.refusals));
        if ((asked != Mode::Off || step.show != Mode::Off) && frame.frame % 600 == 0)
            na::eventlog::Emit("fold11", "asked=%s shown=%s warming=%s good=%u | frames dlaa=%llu quality=%llu performance=%llu warm=%llu warm_failed=%llu drawn_in_blocks=%llu drawn_per_pixel=%llu unable=%llu | "
                               "coarse rate=%u draws=%llu sets=%llu restores=%llu before_lists=%llu failures=%llu frames=%llu | declared=%.4f,%.4f input=%ux%u",
                               na::fold::Name(asked), na::fold::Name(step.show), na::fold::Name(s.fold.hand.warming), s.fold.hand.good,
                               static_cast<unsigned long long>(s.fold_shown_frames[0]), static_cast<unsigned long long>(s.fold_shown_frames[1]),
                               static_cast<unsigned long long>(s.fold_shown_frames[2]), static_cast<unsigned long long>(s.fold_warm_frames),
                               static_cast<unsigned long long>(s.fold_warm_failed), static_cast<unsigned long long>(s.fold_coarse_frames),
                               static_cast<unsigned long long>(s.fold_fine_frames), static_cast<unsigned long long>(s.fold_unable),
                               cs.rate, static_cast<unsigned long long>(cs.draws), static_cast<unsigned long long>(cs.sets),
                               static_cast<unsigned long long>(cs.restores), static_cast<unsigned long long>(cs.before_lists),
                               static_cast<unsigned long long>(cs.failures), static_cast<unsigned long long>(cs.frames),
                               eval.jitter_x, eval.jitter_y, eval.render_w, eval.render_h);
        g_fold_status.asked.store(static_cast<uint32_t>(asked), std::memory_order_relaxed);
        g_fold_status.shown.store(static_cast<uint32_t>(step.show), std::memory_order_relaxed);
        g_fold_status.warming.store(static_cast<uint32_t>(s.fold.hand.warming), std::memory_order_relaxed);
        g_fold_status.good.store(s.fold.hand.good, std::memory_order_relaxed);
        g_fold_status.refused.store(static_cast<uint32_t>(s.fold.hand.refused), std::memory_order_relaxed);
        g_fold_status.shading.store(coarse_why && *coarse_why ? 0u : 1u, std::memory_order_relaxed);
        g_fold_status.w.store(eval.render_w, std::memory_order_relaxed);
        g_fold_status.h.store(eval.render_h, std::memory_order_relaxed);
    }
    gpu_mark(kGpuFold);
    // Companion route: this recording ends here with every shared image handed back in the common
    // state, DLSS runs in the companion at submission, and the rest of the frame is recorded into
    // a second list as if DLSS had run. Whether it really did is known at submission (below).
    ID3D12GraphicsCommandList* before_dlss = nullptr;
    bool evaluated = false;
    if (s.companion) {
        if (eval.color != slot.color.Effects()) {
            // Motion and flow above have already read the game's own picture from the shared colour,
            // and sharpening later uses it only as its target, so nothing else needs the original.
            constexpr auto kCopyFrom = D3D12_RESOURCE_STATE_COPY_SOURCE, kCopyTo = D3D12_RESOURCE_STATE_COPY_DEST;
            Barrier(list, eval.color, kRead, kCopyFrom);
            Barrier(list, slot.color.Effects(), kRead, kCopyTo);
            D3D12_TEXTURE_COPY_LOCATION to{}, from{};
            to.pResource = slot.color.Effects();
            to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            from.pResource = eval.color;
            from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            const D3D12_BOX all{0, 0, 0, ci.width, ci.height, 1};
            list->CopyTextureRegion(&to, 0, 0, 0, &from, &all);
            Barrier(list, eval.color, kCopyFrom, kRead);
            Barrier(list, slot.color.Effects(), kCopyTo, kRead);
            eval.color = slot.color.Effects();
            companion_shaded.fetch_add(1, std::memory_order_relaxed);
        }
        Barrier(list, slot.color.Effects(), kRead, kCommon);
        Barrier(list, shared_depth, kRead, kCommon);
        Barrier(list, slot.motion.Get(), kRead, kCommon);
        Barrier(list, slot.output.Effects(), kWrite, kCommon);
        if (FAILED(list->Close())) return FailSession(s, "Native effects command list could not close.");
        before_dlss = list;
        list = slot.after.Get();
        if (FAILED(list->Reset(slot.allocator.Get(), nullptr))) return FailSession(s, "Could not reset the native effects list.");
        Barrier(list, slot.color.Effects(), kCommon, kRead);
        Barrier(list, shared_depth, kCommon, kRead);
        Barrier(list, slot.motion.Get(), kCommon, kRead);
        Barrier(list, slot.output.Effects(), kCommon, kWrite);
        evaluated = true;
    } else {
        const double t_show = now_ms();
        evaluated = dlss::Evaluate(list, eval);
        fold_cpu_show = now_ms() - t_show;
    }
    if (fold_log_frame)
        na::eventlog::Emit("fold11", "level work at frame %llu (CPU ms on the game's thread): prepare %.2f, warming level's DLSS %.2f, shown level's DLSS %.2f; features created so far %llu",
                           static_cast<unsigned long long>(frame.frame), fold_cpu_prepare, fold_cpu_warm, fold_cpu_show,
                           static_cast<unsigned long long>(dlss::GetStats().features_created));
    gpu_mark(kGpuDlss);
    if (evaluated && fg::WantsInputs()) {
        // Live Frame Generation: this frame's depth and motion (render size, value * mv_scale = render
        // pixels), recorded on this list, published when it is submitted (fg::Execute below). No game
        // camera is given (the proven camera-free configuration); the motion is prepared by the module.
        fg::EvalInputs fi;
        fi.engine_frame = frame.frame;
        fi.depth = eval.depth; // a folded level's planes when one is on screen (as on the DX12 route)
        fi.motion = eval.motion;
        fi.render_w = eval.render_w;
        fi.render_h = eval.render_h;
        fi.out_w = eval.out_w;
        fi.out_h = eval.out_h;
        fi.jitter_x = eval.jitter_x;
        fi.jitter_y = eval.jitter_y;
        fi.mv_scale_x = eval.mv_scale_x;
        fi.mv_scale_y = eval.mv_scale_y;
        fi.mv_low_res = true;
        fi.reset = eval.reset ? 1u : 0u;
        fi.reset_reason = eval.reset ? (!measured && cam.camera_cut ? 1u : 2u) : 0u;
        fi.depth_inverted = eval.depth_inverted ? 1u : 0u;
        fi.mv_jittered = eval.mv_jittered ? 1u : 0u;
        fi.camera_free = true;
        fg::OnEvaluate(list, fi);
    }
    uint32_t capture_token = 0;
    const bool capture_armed = evaluated && capture::j2::Armed();
    if (evaluated && !capture_armed && capture::j2::RefereeWanted()) {
        capture::j2::Planes planes;
        planes.color = eval.color;
        planes.motion = eval.motion;
        planes.output = eval.output;
        capture::j2::Identity id;
        id.present_ordinal = frame.frame;
        id.jitter_x = eval.jitter_x;
        id.jitter_y = eval.jitter_y;
        if (s.requested_jitter_frame == frame.frame && frame.raster.applied && !debug_jitter.value &&
            frame.recovery_tool == referee::RecoveryTool::None)
            id.referee_jitter_amount = referee::VerifiedJitterAmount(s.requested_jitter_amount,
                s.requested_jitter_x, s.requested_jitter_y, eval.jitter_x, eval.jitter_y);
        id.mv_scale_x = eval.mv_scale_x;
        id.mv_scale_y = eval.mv_scale_y;
        id.reset = eval.reset;
        id.dlss_mv_jittered = eval.mv_jittered;
        id.dlss_mv_lowres = !eval.mv_display_res;
        referee::shadow::Context identity{
            {referee::shadow::Path::Native11, static_cast<uint64_t>(frame.boundary)},
            frame.color.incarnation, frame.depth.incarnation, frame.scene_stream, csub, 0, 0, frame.screen_proof};
        identity.trial_ticket = frame.trial_ticket;
        identity.trial_configuration = configuration();
        identity.recovery_region = frame.recovery_region;
        identity.recovery_floor = frame.recovery_floor;
        capture_token = capture::j2::RecordReferee(list, planes, id, eval.render_w, eval.render_h, eval.out_w, eval.out_h, identity);
    } else if (evaluated && eval.reset) {
        referee::gpu::NotifyEvaluationReset();
    }
    // A measured scene cut that DLSS has not been reset for yet: the temporal output still holds the
    // old scene, so those frames show the picture DLSS was given instead. Decided on the GPU, in
    // execution order, with no wait (compute_flow.hpp RecordCutShow). Nothing happens on other frames.
    if (measured && evaluated && flow.pair)
        j2::compute_flow::RecordCutShow(list, flow, full_color, slot.output.Effects());
    if (capture_armed) {
        // A saved capture also takes the scene's source textures, once per capture folder.
        static std::wstring texture_dump_for;
        if (const wchar_t* folder = capture::j2::Directory(); folder && *folder && texture_dump_for != folder) {
            // Recorded only once the request is taken: while an earlier dump still runs, ask again next frame.
            if (native11::RequestTextureDump((std::wstring(folder) + L"\\textures").c_str())) texture_dump_for = folder;
        }
        capture::j2::Planes planes;
        planes.color = eval.color;
        planes.depth = shared_depth;
        planes.motion = slot.motion.Get();
        planes.output = slot.output.Effects();
        // Flow saw the original scene, before optional AO/tone mapping. Keep
        // that source available in an armed capture instead of judging flow
        // against a differently shaded image. No extra live readback.
        if (eval.color != slot.color.Effects())
            planes.extra[planes.extra_count++] = {"source_color", slot.color.Effects(), kRead};
        capture::j2::Identity id;
        id.present_ordinal = id.inputs_frame = frame.frame;
        id.view_color = c.incarnation;
        id.view_depth = d.incarnation;
        id.width = ci.width;
        id.height = ci.height;
        id.color_format = cf;
        id.jitter_x = eval.jitter_x;
        id.jitter_y = eval.jitter_y;
        id.raster_jitter_x = in.jitter_current_px[0];
        id.raster_jitter_y = in.jitter_current_px[1];
        id.depth_reversed = frame.reversed_depth;
        id.camera_cut = !measured && cam.camera_cut;
        id.cut_reason = measured ? 0u : static_cast<uint32_t>(cam.cut_reason);
        id.flow_ran = in.tier3_valid_flag || in.tier1_valid_flag || in.estimated_valid_flag;
        id.flow_loosen = settings::kObjectFlowLoosen;
        id.flow_jitter_x = flow.frame.jitter[0];
        id.flow_jitter_y = flow.frame.jitter[1];
        id.flow_jitter_convention = flow.colour_convention;
        id.flow_jitter_epoch = flow.colour_epoch;
        id.preset = eval.preset;
        id.sharpness = settings::Get().j2_sharpness.load();
        id.dlss_hdr = eval.hdr;
        id.dlss_auto_exposure = eval.auto_exposure;
        id.dlss_depth_inverted = frame.reversed_depth;
        id.reset = eval.reset;
        // Measured motion: the capture says there was no camera, and holds no matrices.
        id.camera_valid = !measured;
        id.camera_source = measured ? 0u : 1u;
        if (!measured) {
            std::copy(cam.view_proj_current_row.begin(), cam.view_proj_current_row.end(), id.vp_cur);
            std::copy(cam.view_proj_previous_row.begin(), cam.view_proj_previous_row.end(), id.vp_prev);
            std::copy(cam.inv_view_proj_current_row.begin(), cam.inv_view_proj_current_row.end(), id.inv_vp_cur);
            std::copy(cam.position.begin(), cam.position.end(), id.eye);
        }
        capture_token = capture::j2::RecordEvaluation(list, planes, id);
    }
    auto* result = slot.output.Native();
    uint32_t sharpened = 0;
    const uint32_t slider = (std::min)(settings::Get().j2_sharpness.load(), 100u);
    const bool reduced = tool == Tool::Sharpen;
    const uint32_t sharpness = settings::RcasStrength(slider, reduced);
    if (evaluated && sharpness) {
        Barrier(list, slot.output.Effects(), kWrite, kRead);
        Barrier(list, slot.color.Effects(), kRead, kWrite);
        if (j2::RecordNativeRcas(s.device.Get(), list, slot.output.Effects(), slot.color.Effects(), shared_depth,
                                 ci.width, ci.height, sharpness, cf, index, frame.reversed_depth)) {
            result = slot.color.Native();
            recorded(Tool::Sharpen);
            sharpened = reduced ? slider / 2 : slider;
        }
        Barrier(list, slot.output.Effects(), kRead, kCommon);
        Barrier(list, slot.color.Effects(), kWrite, kCommon);
    } else {
        Barrier(list, slot.output.Effects(), kWrite, kCommon);
        Barrier(list, slot.color.Effects(), kRead, kCommon);
    }
    Barrier(list, shared_depth, kRead, kCommon);
    Barrier(list, slot.motion.Get(), kRead, kWrite);
    if (fold_shown != na::fold::Mode::Off) s.fold.Done(list, fold_shown); // the folded planes back, after every reader above
    gpu_mark(kGpuEnd);
    if (s.gpu_queries && list && !s.companion) {
        list->ResolveQueryData(s.gpu_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index * kGpuMarks, kGpuMarks, s.gpu_readback.Get(), UINT64(index) * kGpuMarks * 8);
        s.gpu_recorded[index] = true;
    }
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
    if (before_dlss) {
        ID3D12CommandList* head[] = {before_dlss};
        {
            nr::native::Submission submitted(s.queue.Get(), 1, head);
            s.queue->ExecuteCommandLists(1, head);
        }
        dlaa_remote::Frame rf{};
        rf.slot = index;
        rf.preset = eval.preset;
        rf.view_id = eval.view_id;
        rf.jitter_x = eval.jitter_x; rf.jitter_y = eval.jitter_y;
        rf.mv_scale_x = eval.mv_scale_x; rf.mv_scale_y = eval.mv_scale_y;
        rf.pre_exposure = eval.pre_exposure; rf.exposure_scale = eval.exposure_scale;
        rf.quality = static_cast<uint32_t>(eval.quality);
        rf.flags = (eval.reset ? dlaa_remote::kReset : 0u) | (eval.hdr ? dlaa_remote::kHdr : 0u) |
                   (eval.depth_inverted ? dlaa_remote::kDepthInverted : 0u) | (eval.mv_jittered ? dlaa_remote::kMvJittered : 0u) |
                   (eval.auto_exposure ? dlaa_remote::kAutoExposure : 0u);
        // Orders the rest of this frame after the companion's output. The second list still runs
        // when DLSS did not: it returns the shared images to their resting state.
        dlaa_remote::Handoff sent;
        evaluated = dlaa_remote::Evaluate(rf, &sent);
        // Gap audit 2026-11-21 item 12: the Referee judges a companion frame only when the
        // prepared inputs, the companion's result and (through the present result that the
        // capture already waits for) the accepted screen are the same frame.
        referee::CompanionHandoff handoff{frame.frame, sent.asked, sent.answered, sent.asked_slot, sent.answered_slot,
                                          sent.replied, sent.ran};
        const auto binding = referee::Judge(handoff);
        static std::atomic<uint8_t> last_binding{0xff};
        if (last_binding.exchange(uint8_t(binding), std::memory_order_relaxed) != uint8_t(binding) ||
            binding == referee::CompanionBinding::Stale)
            eventlog::Emit("referee_companion", "frame=%llu asked=%llu answered=%llu slot=%u/%u binding=%s witnessed=%u why=%s",
                           (unsigned long long)handoff.ordinal, (unsigned long long)handoff.asked,
                           (unsigned long long)handoff.answered, handoff.asked_slot, handoff.answered_slot,
                           referee::CompanionBindingName(binding),
                           unsigned(binding == referee::CompanionBinding::Bound && capture_token != 0),
                           referee::CompanionBindingWhy(binding));
        if ((!evaluated || binding != referee::CompanionBinding::Bound) && capture_token) {
            capture::j2::Discard(capture_token, false);
            capture_token = 0;
        }
    }
    ID3D12CommandList* lists[] = {list};
    dlss::BeforeExecute(s.queue.Get(), 1, lists);
    {
        nr::native::Submission submitted(s.queue.Get(), 1, lists);
        // The hooked ExecuteCommandLists (overlay) publishes Frame Generation's inputs once this list is submitted.
        s.queue->ExecuteCommandLists(1, lists);
    }
    j2::compute_flow::Committed(flow);
    j2::compute_flow::Committed(flow_prime);
    // The picture is acquired: really submitted, whatever happens to this frame's DLSS step.
    if (flow.context) s.flow_frame = frame.frame;
    // Replay history: this frame's positions are the next frame's previous ones (submitted, so the
    // next frame's copy and reads come after them on this queue).
    if (replay_cur >= 0) {
        s.replay_prev = replay_cur;
        s.replay_prev_frame = frame.frame;
        s.replay_prev_draws = std::move(replay_draws);
    } else {
        s.replay_prev = -1;
        s.replay_prev_draws.clear();
    }
    const auto ask_next_jitter = [&] {
        const auto phase = static_cast<uint32_t>(frame.frame);
        const bool r2 = settings::Get().j2_jitter_r2.load() != 0;
        const na::fold::Mode level = s.fold_next;
        const auto spacing = na::fold::JitterSpacing(level, level != na::fold::Mode::Off ? s.fold.hand.warming : na::fold::Mode::Off);
        const auto fold_block = spacing.block;
        const auto fold_pick = spacing.pick;
        const bool spaced = fold_block.x * fold_block.y > 1;
        const auto sample = !spaced ? native11::DlaaJitterSample(phase, r2)
            : r2 ? neural_alias::temporal_jitter_r2(phase, na::fold::JitterPhases(spacing, 16))
                 : neural_alias::temporal_jitter(phase, na::fold::JitterPhases(spacing, 8));
        // The matched depth and material raster can use the full sequence.
        // VARIANT: referee/sample-locked-dropout
        // The Referee's reaction to pixels that drop out on some samples: the meshes the fixed rules
        // left on the game's grid take the sample too, and the sample stays at full strength while
        // it tries or keeps that (a weaker sample would hide the dropouts it is checking for).
        const bool still_meshes = !debug_jitter.value && referee::gpu::SceneStillMeshes(frame.presentation_source,
            referee::shadow::Path::Native11, frame.color.incarnation, frame.depth.incarnation, frame.scene_stream,
            frame.frame, frame.screen_proof);
        native11::SetStillMeshJitter(still_meshes);
        const auto amount = !frame.raster.shader_depth_aligned && !still_meshes && !debug_jitter.value &&
            frame.recovery_tool == referee::RecoveryTool::None &&
            referee::RouteTrial::WatchTicket(frame.trial_ticket)
            ? referee::gpu::SceneJitterAmount(frame.presentation_source, referee::shadow::Path::Native11,
                frame.color.incarnation, frame.depth.incarnation, frame.scene_stream, frame.frame, frame.screen_proof)
            : referee::normal_jitter;
        s.requested_jitter_frame = frame.frame + 1;
        s.requested_jitter_amount = amount;
        // A folded level: the sample moves around the kept pixel's block, at the block's scale.
        s.requested_jitter_x = static_cast<float>(sample.x_pixels) * (amount * .01f) * static_cast<float>(fold_block.x) +
                               na::fold::KeptPixelCentring(fold_block.x, fold_pick.x); // 0 for a one-pixel block
        s.requested_jitter_y = static_cast<float>(sample.y_pixels) * (amount * .01f) * static_cast<float>(fold_block.y) +
                               na::fold::KeptPixelCentring(fold_block.y, fold_pick.y);
        native11::SetNextJitter(s.requested_jitter_x, s.requested_jitter_y);
    };
    // Measured: the next frame's sample is asked for here, not at delivery, so one failed DLSS step
    // does not take a sample out of the sequence. Bounded: see kJitterGraceFrames.
    if (measured && frame.raster.qualified && j2::Jitter() && s.last_frame && frame.frame > s.last_frame &&
        frame.frame - s.last_frame <= kJitterGraceFrames) {
        ask_next_jitter();
        if (frame.frame != s.last_frame + 1) jitter_kept_after_miss.fetch_add(1, std::memory_order_relaxed);
    }
    dlss::OnExecuted(s.queue.Get(), 1, lists);
    if (capture_token) {
        capture::j2::Submitted(capture_token, s.queue.Get(), frame.frame, recorded_tools, applied_tool);
        if (capture::j2::NeedsPresentCapture(capture_token))
            evaluation_capture_pending.store(true, std::memory_order_release);
    }
    if (FAILED(s.fences.SignalEffects(&slot.effects_done))) return FailSession(s, "DX12 effects submission failed.");
    // The replay buffers this frame wrote or read are free again once this frame's DX12 work is done.
    for (const int used : {replay_cur, replay_prev_used})
        if (used >= 0) { s.replay_buffers[used].native_done = slot.native_done; s.replay_buffers[used].effects_done = slot.effects_done; }
    if (!evaluated) {
        if (!s.companion) return Skip("DLAA evaluation rejected this frame.");
        static char why[200];
        std::snprintf(why, sizeof why, "DLAA companion: %s", dlaa_remote::GetStats().reason);
        if (dlaa_remote::GetStats().failed) return FailSession(s, why);
        return Skip(why);
    }
    if (FAILED(s.fences.WaitNative(slot.effects_done))) return FailSession(s, "Native result dependency failed.");
    ctx->CopySubresourceRegion(color.Get(), csub, c.active_x, c.active_y, 0, result, 0, nullptr);
    // This signal retires the slot. The game's subsequent submission can
    // flush it naturally because no effects queue waits on the value.
    if (FAILED(s.fences.SignalNative(&slot.native_done, false))) return FailSession(s, "Native result submission failed.");
    s.jitter_history = jitter_history;
    if (jitter_switched) resets_jitter_switch.fetch_add(1, std::memory_order_relaxed);
    s.stream = frame.stream;
    s.last_frame = frame.frame;
    s.measured_last = measured;
    if (eval.reset) s.last_reset_frame = frame.frame; // submitted and delivered: this is the reset a measured cut waits for
    (measured ? supplied_measured : supplied_camera).fetch_add(1, std::memory_order_relaxed);
    delivered.fetch_add(1, std::memory_order_relaxed);
    if (replay_motion_selected) {
        replay_delivered.fetch_add(1, std::memory_order_relaxed);
        replay_fed_at.store(GetTickCount64(), std::memory_order_relaxed);
    }
    delivered_size.store((uint64_t(ci.width) << 32) | ci.height, std::memory_order_relaxed);
    applied_preset.store(eval.preset, std::memory_order_relaxed);
    applied_sharpness.store(sharpened, std::memory_order_relaxed);
    // >= 0 means jitter is in the picture: the provider rasterised this frame's scene with the sample.
    raster_slot.store(frame.raster.applied ? (std::max)(frame.camera.vs_slot, 0) : -1, std::memory_order_relaxed);
    delivered_at.store(GetTickCount64(), std::memory_order_relaxed);
    if (frame.raster.qualified && j2::Jitter()) ask_next_jitter();
    reason.store(measured ? "Native DX11 DLAA delivered before the scene consumer (motion measured, no camera)."
                          : "Native DX11 DLAA delivered before the scene consumer.", std::memory_order_relaxed);
    return true;
}

void CapturePresent(IDXGISwapChain* swap, ID3D12CommandQueue* fallback_queue,
                    const native11::PresentReceipt& receipt, PresentPicture picture)
{
    const bool game_picture = picture == PresentPicture::Game;
    if (game_picture) {
        if (const auto lost_proof = referee::gpu::TakeNativeScreenLoss(swap))
            native11::RejectScreenProof(lost_proof);
    }
    const bool eval = game_picture && evaluation_capture_pending.exchange(false, std::memory_order_acq_rel);
    const bool burst = !game_picture && capture::Pending();
    if (!eval && !burst && !(game_picture && settings::Get().referee_enabled.load(std::memory_order_relaxed))) return;
    struct PresentCopy {
        SharedTexture image;
        SharedFences fences;
        ComPtr<ID3D11Device> native;
        ComPtr<ID3D12CommandQueue> queue;
        uint64_t native_done = 0, effects_done = 0;
    };
    static auto* state = new std::unique_ptr<PresentCopy>;
    referee::gpu::Presentation frame{receipt.ordinal, swap, receipt.EligibleForDlaa(), receipt.submitted, true};
    frame.await_result = true;
    const bool witness = game_picture && referee::gpu::NeedsScreen(frame);
    ComPtr<ID3D12CommandQueue> evaluation_queue;
    if (witness || eval || burst) {
        // The inputs, copies and witness reduction must use the same queue.
        // The overlay's separate queue can reach Present before our sample
        // fence completes, even when both queues belong to the same device.
        std::lock_guard<std::mutex> lock(mutex);
        evaluation_queue = State().queue;
    }
    auto* queue = evaluation_queue ? evaluation_queue.Get() : fallback_queue;
    const auto execute = [](ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) { q->ExecuteCommandLists(n, lists); };
    if (eval) capture::j2::OnFrameClosed(receipt.ordinal, 0);
    const auto complete = [queue, eval, game_picture, burst, frame, execute](ID3D12Resource* image) {
        if (eval) capture::j2::OnPresent(queue, image, execute, frame);
        else if (game_picture) referee::gpu::OnPresent(frame, queue, image, execute);
        if (burst && image) {
            char note[256]{};
            FormatStatus(note, sizeof note);
            capture::NotePresent(note);
            capture::OnPresent(queue, image, false);
        }
    };
    if (!eval && !burst && !witness) return complete(nullptr);
    const auto unavailable = [&](const char* stage, HRESULT hr = S_OK) {
        static uint64_t next_log = 0;
        const auto now = GetTickCount64();
        if (witness && now >= next_log) {
            next_log = now + 5000;
            na::eventlog::Emit("native11_witness", "frame=%llu missed=%s hr=0x%08lx",
                               static_cast<unsigned long long>(receipt.ordinal), stage, static_cast<unsigned long>(hr));
        }
        complete(nullptr);
    };
    if (!queue || (witness && !evaluation_queue)) return unavailable("evaluation_queue");
    ComPtr<ID3D11Texture2D> backbuffer;
    HRESULT hr = swap->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
    if (FAILED(hr)) return unavailable("backbuffer", hr);
    native11::OwnWorkScope own;
    observe::HostWorkScope host_work;
    D3D11_TEXTURE2D_DESC desc{};
    backbuffer->GetDesc(&desc);
    ComPtr<ID3D11Device> native;
    backbuffer->GetDevice(&native);
    const ImageDesc image{desc.Width, desc.Height, desc.Format};
    if (!CompatibleImageCopy(desc, 0, image)) return unavailable("incompatible_backbuffer");
    auto& copy = *state;
    if (!copy || copy->queue.Get() != queue || copy->native.Get() != native.Get() ||
        copy->image.Description().width != image.width || copy->image.Description().height != image.height ||
        copy->image.Description().format != image.format) {
        if (copy && !copy->fences.Retired(copy->native_done, copy->effects_done)) return unavailable("old_copy_in_flight");
        auto next = std::make_unique<PresentCopy>();
        ComPtr<ID3D12Device> effects;
        if (FAILED(hr = queue->GetDevice(IID_PPV_ARGS(&effects))) ||
            FAILED(hr = next->image.Create(native.Get(), effects.Get(), image)) ||
            FAILED(hr = next->fences.Create(native.Get(), effects.Get(), queue))) return unavailable("create_copy", hr);
        next->native = native;
        next->queue = queue;
        copy = std::move(next);
    }
    if (copy->effects_done && FAILED(hr = copy->fences.WaitNative(copy->effects_done))) return unavailable("native_wait", hr);
    copy->fences.NativeContext()->CopySubresourceRegion(copy->image.Native(), 0, 0, 0, 0, backbuffer.Get(), 0, nullptr);
    if (FAILED(hr = copy->fences.SignalNative(&copy->native_done)) ||
        FAILED(hr = copy->fences.WaitEffects(copy->native_done))) return unavailable("submit_copy", hr);
    complete(copy->image.Effects());
    copy->fences.SignalEffects(&copy->effects_done);
}
int FormatStatus(char* out, size_t capacity)
{
    // last_ms: how long ago the last frame was delivered (-1 = never). A provider-level miss leaves
    // `reason` at its last success, so this is what shows DLAA has stopped.
    const auto last = delivered_at.load(std::memory_order_relaxed);
    const long long last_ms = last ? static_cast<long long>(GetTickCount64() - last) : -1;
    // motion_supplier: delivered frames by whose motion they got. measured{}: that supplier's own
    // resets (the first picture of a pair, with the last reason code of compute_flow's Ticket::no_pair;
    // a measured scene cut), frames left alone because nothing could be measured, and jitter samples
    // kept in sequence after a frame that was not delivered.
    int n = std::snprintf(out, capacity, "native11_dlaa delivered=%llu last_ms=%lld busy=%llu rejected=%llu preset=%u sharpness=%u raster_slot=%d history_resets{camera_cut=%llu inputs_changed=%llu frame_gap=%llu jitter_switch=%llu} "
                         "camera_setting=%d motion_supplier{camera=%llu measured=%llu} replay_delivered=%llu measured{reset_first_picture=%llu last_no_pair=%u reset_scene_cut=%llu unavailable=%llu jitter_kept_after_miss=%llu} \"%s\"",
                         static_cast<unsigned long long>(delivered.load()), last_ms,
                         static_cast<unsigned long long>(busy.load()),
                         static_cast<unsigned long long>(rejected.load()), applied_preset.load(), applied_sharpness.load(),
                         raster_slot.load(), static_cast<unsigned long long>(resets_cut.load()),
                         static_cast<unsigned long long>(resets_changed.load()), static_cast<unsigned long long>(resets_gap.load()),
                         static_cast<unsigned long long>(resets_jitter_switch.load()),
                         settings::Get().j2_camera.load(std::memory_order_relaxed),
                         static_cast<unsigned long long>(supplied_camera.load()), static_cast<unsigned long long>(supplied_measured.load()),
                         static_cast<unsigned long long>(replay_delivered.load()),
                         static_cast<unsigned long long>(resets_no_pair.load()), last_no_pair.load(),
                         static_cast<unsigned long long>(resets_measured_cut.load()),
                         static_cast<unsigned long long>(measured_unavailable.load()),
                         static_cast<unsigned long long>(jitter_kept_after_miss.load()),
                         reason.load());
    // Per-stage GPU ms on this route (last/average), and the level on screen.
    if (n > 0 && static_cast<size_t>(n) + 200 < capacity) {
        const auto cs = native11::GetCoarseShadingStats();
        const int more = std::snprintf(out + n, capacity - static_cast<size_t>(n),
            " gpu_ms{motion=%.2f/%.2f fold=%.2f/%.2f dlss=%.2f/%.2f list=%.2f/%.2f samples=%llu} fold11{shown=%s asked=%s coarse_rate=%u coarse_draws=%llu coarse_frames=%llu before_lists=%llu failures=%llu sampled_motion=%llu folded_flow=%llu}",
            gpu_last[0].load(), gpu_avg[0].load(), gpu_last[1].load(), gpu_avg[1].load(), gpu_last[2].load(), gpu_avg[2].load(),
            gpu_last[3].load(), gpu_avg[3].load(), static_cast<unsigned long long>(gpu_samples.load()),
            na::fold::Name(na::fold::ModeFrom(g_fold_status.shown.load())), na::fold::Name(na::fold::ModeFrom(g_fold_status.asked.load())),
            cs.rate, static_cast<unsigned long long>(cs.draws), static_cast<unsigned long long>(cs.frames),
            static_cast<unsigned long long>(cs.before_lists), static_cast<unsigned long long>(cs.failures),
            static_cast<unsigned long long>(sampled_frames.load()), static_cast<unsigned long long>(folded_flow_frames.load()));
        if (more > 0) n += more;
    }
    // The handoff cost to the 64-bit companion, as numbers, on the same line the results read.
    if (dlaa_remote::Wanted() && n > 0 && static_cast<size_t>(n) + 2 < capacity) {
        out[n++] = ' ';
        const int more = dlaa_remote::FormatStatus(out + n, capacity - static_cast<size_t>(n));
        if (more > 0) n += more;
        // Proof that ambient shading or bloom reached the picture the companion reads.
        if (static_cast<size_t>(n) + 40 < capacity) {
            const int tail = std::snprintf(out + n, capacity - static_cast<size_t>(n), " companion_shaded=%llu",
                                           static_cast<unsigned long long>(companion_shaded.load(std::memory_order_relaxed)));
            if (tail > 0) n += tail;
        }
    }
    return n;
}
void FoldStatus(char* out, size_t capacity) { g_fold_status.Format(out, capacity); }
float FoldMipExtra() { return static_cast<float>(g_fold_mip_extra.load(std::memory_order_relaxed)); }

bool FormatReplayStatus(char* out, size_t capacity)
{
    if (!out || !capacity) return false;
    out[0] = '\0';
    if (!settings::Get().j2_draw_replay.load(std::memory_order_relaxed)) return false;
    const HRESULT handoff = State().replay_handoff_hr.load(std::memory_order_relaxed);
    if (FAILED(handoff)) {
        std::snprintf(out, capacity, "Replay unavailable: DX11/DX12 position handoff failed (0x%08lX).",
                      static_cast<unsigned long>(handoff));
        return true;
    }
    const auto d = native11::draw_replay::GetStats();
    const auto fed = replay_fed_at.load(std::memory_order_relaxed);
    const char* state = fed && GetTickCount64() - fed < 1000 ? "Replay motion available to DLSS." :
        !d.built && d.build_failed ? "Could not prepare a replay pipeline." :
        "Waiting for matching scene draws.";
    std::snprintf(out, capacity, "%s", state);
    return true;
}

bool FormatMenuStatus(char* out, size_t capacity)
{
    const auto last = delivered_at.load(std::memory_order_relaxed);
    const auto size = delivered_size.load(std::memory_order_relaxed);
    const bool running = j2::Enabled() && !j2::Paused() && last && GetTickCount64() - last < 1000;
    if (running)
        std::snprintf(out, capacity, "Native DX11 DLAA running at %u × %u. %llu frames improved.%s",
                      unsigned(size >> 32), unsigned(size), static_cast<unsigned long long>(delivered.load()),
                      j2::Jitter() && raster_slot.load() < 0 ? " Jitter is waiting for a matched scene sample." : "");
    else if (j2::Paused()) {
        std::snprintf(out, capacity, "Native DX11 DLAA is off.");
    } else {
        // p47656: DLAA stopped for 36 min while this line still read "delivered" (the consumer's
        // last reason). Provider misses never reach the consumer, so name them here.
        const auto provider = native11::Stats();
        const bool provider_miss = provider.last_miss && provider.last_miss[0];
        const char* why = provider_miss ? provider.last_miss : reason.load();
        if (last)
            std::snprintf(out, capacity, "DLAA is NOT running: you are seeing the game's own picture (last DLAA frame %llu s ago). %s",
                          static_cast<unsigned long long>((GetTickCount64() - last) / 1000), why);
        else
            std::snprintf(out, capacity, "DLAA has not run yet. %s", why);
    }
    return running;
}
} // namespace na::native_d3d11
