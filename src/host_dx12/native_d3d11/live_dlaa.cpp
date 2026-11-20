#include "live_dlaa.hpp"
#include "shared_resources.hpp"
#include "core/motion_arbiter_gpu.hpp"
#include "core/temporal_jitter.hpp"
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
std::atomic<uint64_t> replay_fed_at{0};

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

bool ReplayBufferMade(ID3D12Device* dev, ComPtr<ID3D12Resource>& out)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = uint64_t(native11::draw_replay::kCapacity) * 16u;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return SUCCEEDED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, kCommon, nullptr, IID_PPV_ARGS(&out)));
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
    native11::SetEvaluationPaused(paused);
    native11::SetDepthReads(!paused && j2::Jitter());
    // Apply the switch at Present even when a frame supplies no scene to DLAA.
    const auto& options = settings::Get();
    native11::draw_replay::Configure(!paused && options.j2_draw_replay.load(std::memory_order_relaxed) != 0,
                                     options.j2_replay_budget.load(std::memory_order_relaxed),
                                     options.j2_replay_movers.load(std::memory_order_relaxed) != 0);
    const int setting = settings::Get().j2_camera.load(std::memory_order_relaxed);
    native11::SetCameraUse(setting == 0 ? native11::CameraUse::Off : setting == 2 ? native11::CameraUse::Required
                                                                                : native11::CameraUse::Optional);
}

bool EvaluateFrame(void*, void* context, const native11::SceneFrame& frame)
{
    // The native provider has witnessed a real rendered scene. Wake the
    // existing session worker so it can qualify settings and enable effects.
    if (context) host::RendererObserved();
    native11::SetNextJitter(0, 0);
    // Seal at the scene boundary. Later draws belong to post-processing or HUD.
    const bool replay_setting = settings::Get().j2_draw_replay.load(std::memory_order_relaxed) != 0;
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
    camera::CameraFrame cam{};
    if (frame.camera_known && camera_setting != 0) cam = Pose(s, frame, ci.width, ci.height);
    if (camera_setting == 2 && !cam.valid) return Skip("Waiting for the current scene camera.");
    // VARIANT: motion/measured-total
    // Whose motion this frame gets. The camera's, when there is a valid one. Measured from the two
    // pictures (j2/compute_flow Frame::total) when there is none, and also on the frame a camera
    // first appears while measured frames are being delivered: the tracker has no previous camera
    // on that frame and would ask for a reset that the measured motion does not need.
    const bool measured = camera_setting != 2 &&
        (!cam.valid || (cam.camera_cut && s.measured_last && frame.frame == s.last_frame + 1 && !frame.scene_changed));
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
        j2::compute_flow::Tier3 tier;
        flow = j2::compute_flow::Record(list, s.queue.Get(), slot.color.Effects(), shared_depth, in, ff, tier);
        screen_model = tier.screen_model;
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
    std::vector<observe::ReplayRecord> replay_draws;
    if (replay_setting && !sealed.draws.empty() && sealed.verts <= native11::draw_replay::kCapacity &&
        !(frame.raster.applied && !frame.raster.viewport)) {
        if (!s.replay_tried) {
            s.replay_tried = true;
            if (s.replay.Init(s.device.Get())) s.replay.SetLearnSink(&ReplayLearnSink);
            na::eventlog::Emit("draw_replay11_pass", "ready=%d", s.replay.Ready() ? 1 : 0);
        }
        for (int i = 0; s.replay.Ready() && i < 3 && replay_cur < 0; ++i) {
            auto& b = s.replay_buffers[i];
            if (i == s.replay_prev) continue; // the history this frame reads
            if (b.buffer ? !s.fences.Retired(b.native_done, b.effects_done) : !ReplayBufferMade(s.device.Get(), b.buffer)) continue;
            replay_cur = i;
        }
        if (replay_cur >= 0 && !slot.positions_view) {
            const ImageDesc pd{native11::draw_replay::kPackWidth, native11::draw_replay::kPackRows, DXGI_FORMAT_R32G32B32A32_FLOAT, true};
            if (FAILED(slot.positions.Create(s.native.Get(), s.device.Get(), pd)) ||
                FAILED(s.native->CreateUnorderedAccessView(slot.positions.Native(), nullptr, &slot.positions_view))) {
                slot.positions_view.Reset();
                replay_cur = -1;
                na::eventlog::Emit("draw_replay11_pass", "shared_positions=failed");
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
        to.PlacedFootprint.Footprint = {DXGI_FORMAT_R32G32B32A32_FLOAT, native11::draw_replay::kPackWidth, rows, 1,
                                        native11::draw_replay::kPackWidth * 16u};
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
            replay_fed_at.store(GetTickCount64(), std::memory_order_relaxed);
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
    dlss::EvalDesc eval{};
    eval.color = slot.color.Effects();
    // The two optional passes (ambient shading, bloom) each hand DLSS a picture of their own. On the
    // companion route that picture is copied into the shared scene colour below, because the shared
    // colour is the one the companion reads.
    if (settings::Get().xegtao_enabled.load()) {
        xegtao::Inputs ai;
        ai.color = eval.color; ai.depth = shared_depth; ai.motion = slot.motion.Get();
        ai.color_format = cf; ai.width = ci.width; ai.height = ci.height;
        ai.srgb = !hdr; ai.clear_depth = frame.reversed_depth ? 0.f : 1.f;
        ai.jitter_x = in.jitter_current_px[0]; ai.jitter_y = in.jitter_current_px[1];
        ai.frame = frame.frame; ai.motion_reference_frame = s.last_frame;
        ai.scene_epoch = frame.stream; ai.reset = reset_history;
        const auto column = camera::TransposeMatrix(cam.view_proj_current_row);
        // Ambient shading rebuilds positions from the depth: that needs the camera's projection.
        if (measured) xegtao::ScenePass().Unavailable("Ambient shading needs the scene camera; this frame has none.");
        else if (xegtao::FromViewProjection(column.data(), ai.projection)) {
            auto options = xegtao::CurrentOptions();
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
    constexpr uint64_t kNativeView = 0x4e41544956453131ull;
    eval.view_id = kNativeView;
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
        evaluated = dlss::Evaluate(list, eval);
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
        id.mv_scale_x = eval.mv_scale_x;
        id.mv_scale_y = eval.mv_scale_y;
        id.reset = eval.reset;
        id.dlss_mv_jittered = eval.mv_jittered;
        id.dlss_mv_lowres = !eval.mv_display_res;
        referee::shadow::Context identity{
            {referee::shadow::Path::Native11, static_cast<uint64_t>(frame.boundary)},
            frame.color.incarnation, frame.depth.incarnation, frame.scene_stream, csub, 0, 0, frame.screen_proof};
        identity.trial_ticket = frame.trial_ticket;
        if (identity.trial_ticket) identity.trial_configuration = configuration();
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
        j2::compute_flow::RecordCutShow(list, flow, eval.color, slot.output.Effects());
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
    const uint32_t sharpness = settings::RcasStrength(settings::Get().j2_sharpness.load(), tool == Tool::Sharpen);
    if (evaluated && sharpness) {
        Barrier(list, slot.output.Effects(), kWrite, kRead);
        Barrier(list, slot.color.Effects(), kRead, kWrite);
        if (j2::RecordNativeRcas(s.device.Get(), list, slot.output.Effects(), slot.color.Effects(), shared_depth,
                                 ci.width, ci.height, sharpness, cf, index, frame.reversed_depth)) {
            result = slot.color.Native();
            recorded(Tool::Sharpen);
            sharpened = sharpness / 2; // reported on the menu scale
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
        evaluated = dlaa_remote::Evaluate(rf);
        if (!evaluated && capture_token) {
            capture::j2::Discard(capture_token, false);
            capture_token = 0;
        }
    }
    ID3D12CommandList* lists[] = {list};
    dlss::BeforeExecute(s.queue.Get(), 1, lists);
    {
        nr::native::Submission submitted(s.queue.Get(), 1, lists);
        s.queue->ExecuteCommandLists(1, lists);
    }
    j2::compute_flow::Committed(flow);
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
        const auto sample = native11::DlaaJitterSample(phase, settings::Get().j2_jitter_r2.load() != 0);
        native11::SetNextJitter(static_cast<float>(sample.x_pixels), static_cast<float>(sample.y_pixels));
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
                         "camera_setting=%d motion_supplier{camera=%llu measured=%llu} measured{reset_first_picture=%llu last_no_pair=%u reset_scene_cut=%llu unavailable=%llu jitter_kept_after_miss=%llu} \"%s\"",
                         static_cast<unsigned long long>(delivered.load()), last_ms,
                         static_cast<unsigned long long>(busy.load()),
                         static_cast<unsigned long long>(rejected.load()), applied_preset.load(), applied_sharpness.load(),
                         raster_slot.load(), static_cast<unsigned long long>(resets_cut.load()),
                         static_cast<unsigned long long>(resets_changed.load()), static_cast<unsigned long long>(resets_gap.load()),
                         static_cast<unsigned long long>(resets_jitter_switch.load()),
                         settings::Get().j2_camera.load(std::memory_order_relaxed),
                         static_cast<unsigned long long>(supplied_camera.load()), static_cast<unsigned long long>(supplied_measured.load()),
                         static_cast<unsigned long long>(resets_no_pair.load()), last_no_pair.load(),
                         static_cast<unsigned long long>(resets_measured_cut.load()),
                         static_cast<unsigned long long>(measured_unavailable.load()),
                         static_cast<unsigned long long>(jitter_kept_after_miss.load()),
                         reason.load());
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
bool FormatReplayStatus(char* out, size_t capacity)
{
    if (!out || !capacity) return false;
    out[0] = '\0';
    if (!settings::Get().j2_draw_replay.load(std::memory_order_relaxed)) return false;
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
