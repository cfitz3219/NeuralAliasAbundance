// Native DX11 input provider (WWII 40-50 fps plan, B1). See input_provider.hpp.
//
// Ported from the PR #90 DX11 host (src/host_dx11/dx11_host.cpp on claude/project-thread-2b5ysj): the
// same hook set and frame model, with that host's own bridge/helper replaced by the SceneCallback, so
// the existing DX12 backend (Astra's B2 handoff) does the evaluation inside the one program.
//
// Hook set (vtable functions of d3d11.dll's own context classes, found on a small private device;
// hooking the function covers every game context of that class). Immediate and deferred contexts
// are different classes, so both are hooked:
//   state:  OMSetRenderTargets(+AndUAVs), OMSetDepthStencilState, VS/PSSetConstantBuffers(+1),
//           RSSetViewports, ClearState
//   events: Draw* (6), ClearDepthStencilView, PS/CS/VSSetShaderResources,
//           CopyResource / CopySubresourceRegion / ResolveSubresource (reads),
//           Map/Unmap + UpdateSubresource(+1) on constant buffers
//   lists:  FinishCommandList (deferred), ExecuteCommandList (immediate)
//   frame:  IDXGISwapChain::Present / Present1 / ResizeBuffers
//
// Rules:
//   * never act on a context of another device (video players, overlays, our own devices);
//   * everything we and the consumer do runs with t_inside set, so it is forwarded unobserved;
//   * the scene colour and depth are held by references taken from the game's own live views, and
//     released before ResizeBuffers (a held back buffer would make ResizeBuffers fail);
//   * any missing evidence skips the frame with a reason; the game's frame is never changed then.
//   * native sampler policy is handled by samplers.cpp, sharing the own-work exclusion.
#include "host_dx12/native11/input_provider.hpp"
#include "host_dx12/native11/samplers.hpp"
#include "host_dx12/native11/textures.hpp"

#include "core/temporal_jitter.hpp"
#include "host_dx12/diag/referee_recovery.hpp"
#include "host_dx12/adapters/wwii/scene_camera.hpp"
#include "host_dx12/diag/event_log.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
#include "host_dx12/native11/draw_replay.hpp"
#include "host_dx12/native11/hud_free.hpp"
#include "host_dx12/native11/camera_pick.hpp"
#include "host_dx12/native11/depth_direction.hpp"
#include "host_dx12/native11/depth_reads.hpp"
#include "host_dx12/native11/frame_model.hpp"
#include "host_dx12/native11/screen_check.hpp"
#include "host_dx12/native11/world_raster.hpp"
#include "core/referee_world_check.hpp"
#include "core/dxbc_inputs.hpp"
#include "host_dx12/background/work.hpp"
#include "host_dx12/native11/lit_scene.hpp"
#include "host_dx12/native11/split_camera.hpp"
#include "host_dx12/native11/viewport_jitter.hpp"
#include "host_dx12/native11/coarse11.hpp"
#include "providers/camera/camera_admission.hpp"
#include "providers/camera/camera_tracker.hpp"

#include <d3d11_4.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace na::native11 {
namespace {

using Microsoft::WRL::ComPtr;

// ID3D11DeviceContext vtable (d3d11.h order).
enum CtxSlot : uint32_t {
    kVSSetConstantBuffers = 7, kPSSetShaderResources = 8, kPSSetShader = 9, kPSSetConstantBuffers = 16, kDrawIndexed = 12, kDraw = 13, kMap = 14, kUnmap = 15,
    kDrawIndexedInstanced = 20, kDrawInstanced = 21, kVSSetShaderResources = 25, kOMSetRenderTargets = 33,
    kOMSetRenderTargetsAndUAVs = 34, kOMSetDepthStencilState = 36, kDrawIndexedInstancedIndirect = 39,
    kBegin = 27, kEnd = 28, kDrawInstancedIndirect = 40, kDrawAuto = 38, kDispatch = 41, kDispatchIndirect = 42,
    kRSSetViewports = 44, kCopySubresourceRegion = 46, kCopyResource = 47, kUpdateSubresource = 48,
    kClearRenderTargetView = 50, kClearDepthStencilView = 53, kResolveSubresource = 57, kExecuteCommandList = 58, kCSSetShaderResources = 67,
    kClearState = 110, kFinishCommandList = 114,
    // ID3D11DeviceContext1
    kCopyRegion1 = 115, kDiscardResource = 117, kDiscardView = 118, kSwapContextState = 131, kClearView = 132, kDiscardView1 = 133,
    kUpdateSubresource1 = 116, kVSSetConstantBuffers1 = 119, kPSSetConstantBuffers1 = 123,
};
enum SwapSlot : uint32_t { kPresent = 8, kResizeBuffers = 13, kPresent1 = 22 };

constexpr size_t kMaxCbBytes = 64 * 1024; // unchanged whole-buffer path
constexpr uint32_t kLargeCbWindowBytes = 16 * 1024;
constexpr uint32_t kLargeCbDriftBytes = 2 * 1024; // adjacent offsets in a reused upload ring
struct CaptureWindow { uint32_t begin = 0, size = 0; };
constexpr size_t kLargeCbWindowCount = 4;
constexpr uint32_t kLargeCbTotalBytes = 32 * 1024;
struct CaptureWindows {
    std::array<CaptureWindow, kLargeCbWindowCount> ranges{};
    size_t count = 0;
    uint32_t bytes = 0;

    void Remember(CaptureWindow window)
    {
        if (!window.size) return;
        for (size_t i = 0; i < count; ++i) {
            const auto& saved = ranges[i];
            const uint32_t begin = std::min(saved.begin, window.begin);
            const uint32_t end = std::max(saved.begin + saved.size, window.begin + window.size);
            if (std::max(saved.begin, window.begin) > std::min(saved.begin + saved.size, window.begin + window.size) ||
                end - begin > kLargeCbWindowBytes ||
                bytes - saved.size + (end - begin) > kLargeCbTotalBytes) continue;
            bytes = bytes - saved.size + (end - begin);
            ranges[i] = {begin, end - begin};
            return;
        }
        if (count < ranges.size() && bytes + window.size <= kLargeCbTotalBytes) {
            ranges[count++] = window;
            bytes += window.size;
        }
    }
};
using CameraBytes = std::shared_ptr<const std::vector<uint8_t>>;
struct CameraUpload {
    uint32_t version = 0;
    CameraBytes bytes;
    uint32_t first_constant = 0; // position of bytes[0] in the real buffer
    bool windowed = false;
};
struct CameraUploads {
    CameraUpload primary;
    std::vector<CameraUpload> additional;
};
using DeferredUpload = std::pair<size_t, CameraUploads>;
// VARIANT: camera11/exact-range
// A large buffer the game fills piece by piece (D3D11_MAP_WRITE_NO_OVERWRITE) places each frame's
// constants wherever its allocator has reached, so the place moves with what is drawn. No kept
// window can follow that. When a scene draw binds a range of such a buffer, the range itself is
// read at the draw, through the same no-overwrite mapping the game uses: earlier contents are
// preserved by that mapping's contract, and nothing the game wrote is changed.
struct ExactRange {
    int slot = -1;
    CameraUpload upload; // windowed: exactly the bound range
};
using ExactSet = std::shared_ptr<const std::vector<ExactRange>>;
using DeferredExact = std::pair<size_t, ExactSet>; // from this draw event on (an empty set clears)
constexpr uint32_t kExactMaxConstants = 1024;      // 16 KiB per bound range
std::atomic<uint32_t> g_exact_frame{1};            // bumps at Present: a range is read again each frame
std::atomic<uint64_t> g_exact_reads{0}, g_exact_failed{0}, g_exact_draws{0};
constexpr size_t kMaxListEvents = 200000; // per deferred recording; beyond this the list's tail is not modelled
constexpr size_t kMaxCandidates = 8;       // constant buffers searched for the camera (VS and PS)
constexpr size_t kMaxLists = 512;          // recorded command lists kept

enum class DepthStage : size_t { Pixel, Vertex, Compute };
constexpr UINT kDepthSlots = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
std::atomic<bool> g_depth_reads_enabled{false};
struct DepthReads {
    std::bitset<kDepthSlots> slots[3];
    std::unique_ptr<DepthReadTwin> twin;
    Id resource = 0;
    uint32_t frame = 0;
    uint64_t sample = 0;
    UINT width = 0, height = 0;
    bool cleared = false, seen = false, mixed = false, dirty = true, deferred = false, expanded = false;
    // Qualify recovery for each clearing/lighting role independently. A late
    // unsafe read must not make a stable earlier reader change texture.
    struct Reader {
        uint64_t key = 0;
        uint32_t frame = 0;
        uint8_t good = 0;
        bool blocked = false;
        const char* reason = nullptr;
    };
    std::array<Reader, 24> readers{};

    void Start(uint32_t ordinal, Id depth)
    {
        if (frame == ordinal && resource == depth) return;
        if (resource != depth) {
            twin.reset();
            readers = {};
        }
        frame = ordinal;
        resource = depth;
        sample = 0;
        width = height = 0;
        cleared = seen = mixed = deferred = expanded = false;
        dirty = true;
    }
    void Write(uint64_t raster_sample, bool expanded_viewport, UINT w, UINT h)
    {
        if (!raster_sample || !w || !h ||
            (seen && (sample != raster_sample || expanded != expanded_viewport || width != w || height != h))) mixed = true;
        sample = raster_sample;
        width = w;
        height = h;
        expanded = expanded_viewport;
        seen = dirty = true;
    }
    void Report(Reader& reader, const char* state, const char* reason)
    {
        if (reader.reason && std::strcmp(reader.reason, reason) == 0) return;
        reader.reason = reason;
        static std::atomic<unsigned> budget{0};
        if (budget.fetch_add(1, std::memory_order_relaxed) >= 256) return;
        na::eventlog::Emit("depth_reads", "variant=depth-reads/twin api=dx11 frame=%u reader=%llx state=%s why=\"%s\"",
                           frame - 1, static_cast<unsigned long long>(reader.key), state, reason);
    }
    bool Refresh(ID3D11DeviceContext* context, ID3D11Resource* depth, uint64_t key, bool query)
    {
        auto it = std::find_if(readers.begin(), readers.end(), [key](const Reader& r) { return r.key == key; });
        if (it == readers.end()) {
            it = std::min_element(readers.begin(), readers.end(), [](const Reader& a, const Reader& b) {
                return a.frame < b.frame;
            });
            *it = {key};
        }
        auto& reader = *it;
        const bool new_frame = reader.frame != frame;
        reader.frame = frame;
        const auto refuse = [&](const char* reason) {
            reader.blocked = true;
            reader.good = 0;
            Report(reader, "refused", reason);
            return false;
        };
        if (deferred || mixed) return refuse(deferred ? "deferred game work in this frame" : "mixed or unqualified depth writes");
        if (!cleared || !seen) return refuse("waiting for a clear and viewport-jittered scene depth writes");
        if (dirty && query) return refuse("a game raster query is active");
        if (reader.blocked) {
            if (new_frame && reader.good < 3) ++reader.good;
            if (reader.good < 3) {
                Report(reader, "recovering", "waiting for three usable frames from this reader");
                return false;
            }
            reader.blocked = false;
        }
        if (dirty) {
            if (!twin) twin = std::make_unique<DepthReadTwin>();
            float offset[2];
            std::memcpy(offset, &sample, sizeof offset);
            if (!twin->Refresh(context, depth, offset[0], offset[1], expanded, width, height)) return refuse(twin->Reason());
            dirty = false;
        }
        Report(reader, "on", "current scene depth corrected");
        return true;
    }
};

// The view the scene used on a selected texture (typed format, subresource).
struct ViewUse { uint32_t format = 0, mip = 0, slice = 0; bool texture2d = false; };
struct Ctx {
    struct WorldJob {
        WorldRaster raster;
        Id color = 0, depth = 0;
        uint64_t proof = 0;
        UINT level = 0, width = 0, height = 0;
        DXGI_FORMAT color_format{}, depth_format{};
        std::atomic<bool> ready{false};
        std::atomic<bool> refused{false}, proof_current{true};
        uint64_t last_receipt = 0;
        unsigned missed_evaluations = 0;
        CornerCopies corners; // full-size pictures the world reads, made small in a corner (the game's main thread only)
        WorldProbe probe;     // the Referee's eye on the small picture
    };
    std::shared_ptr<WorldJob> world;
    bool world_dirty = false;
    uint64_t world_closed_frame = ~uint64_t{0};
    std::optional<bool> world_uavs;
    // The full-size pictures the bound pixel shader reads (a copy of the frame for water, depth for soft
    // edges): which slots, and whether the shader finds its place by pixel position. Settled once per
    // binding of shader and views (ScanWorldInputs).
    struct WorldInputUse {
        bool scanned = false, impossible = false, by_position = true;
        bool own_colour = false, own_depth = false; // the world's own picture is among them
        uint8_t n = 0;
        uint8_t slot[4] = {};
        uint32_t w[4] = {}, h[4] = {}; // the size each takes when made small (a half-size picture stays half the small size)
        uint64_t shader = 0;
    } world_inputs;
    std::unordered_set<uint64_t> world_inputs_seen; // shaders with such reads the Referee has been asked to look at
    bool world_inputs_new = false;
    // This frame's count of the world's draws: the rehearsal and the WHY log read it at the frame's end.
    struct WorldTally {
        uint32_t went_small = 0, would = 0, breaks = 0, inputs = 0, corners = 0; // ("small" alone is a macro in the Windows headers)
        uint32_t restores = 0; // times the full-size pictures were (or, in a rehearsal, would be) brought up to date mid-frame
        uint32_t held[size_t(WorldHold::kCount)] = {};
        bool last_passed = false;
    } tally;
    uint64_t tally_frame = ~uint64_t{0};
    uint64_t world_small_frame = ~uint64_t{0};    // the last frame some draw went to the small pictures
    uint64_t world_given_up_frame = ~uint64_t{0}; // too many full-size draws between the small ones: the rest stay full size
    uint64_t world_list_frame = ~uint64_t{0};     // a played-back recording wrote the world's pictures
    uint64_t world_run = 0;                       // moves whenever a full-size picture the world reads may have changed
    uint64_t world_own_run = 0;                   // moves whenever the world's own full-size pictures changed
    bool world_depth_dirty = false;               // the small depth holds draws the full-size depth does not have yet
    bool would_dirty = false, would_depth_dirty = false; // the same two, as they would be, while the draws are only counted
    bool would_depth_write = false;               // the draw just counted writes depth
    bool world_stencil_written = false, stencil_writes = false;
    bool pristine_color = false, pristine_depth = false;
    FLOAT clear_rgba[4]{}, clear_z = 1;
    UINT clear_flags = 0; UINT8 clear_s = 0;
    uint64_t world_color_clear = ~uint64_t{0}, world_depth_clear = ~uint64_t{0};
    uint64_t world_frame = ~uint64_t{0};
    uint64_t world_draw_frame = ~uint64_t{0};
    uint32_t world_draws = 0;
    bool game = false; // a context of the game's device
    uint32_t gen = 0;  // the game-device generation that answer was given in
    uint64_t sampler_policy = ~uint64_t{0};
    SamplerShadow sampler_shadow;
    uint64_t texture_policy = ~uint64_t{0};
    Id screen_color = 0, screen_peer = 0;
    ID3D11PixelShader* pixel_shader = nullptr; // context owns the binding
    uint64_t writer_shader = 0;
    bool pixel_shader_known = false;
    std::bitset<D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> screen_slots;
    // One scene-size intermediate whose producer has already passed the screen challenge.
    // D3D11 unbinds its input views when it is bound for writing. Track subsequent bindings.
    struct PostPicture {
        Id resource = 0;
        uint64_t incarnation = 0, root_proof = 0, frame = 0;
        std::bitset<D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> slots;
    } post;
    PsShadow ps_shadow;                // the game's views behind bound material replacements (textures.hpp)
    bool deferred = false;
    Id rt0 = 0, depth = 0;
    uint8_t rt_count = 0;              // render targets bound together
    bool depth_test = true;
    bool depth_read_only = false;
    uint32_t raster_queries = 0;
    DepthReads depth_reads;
    struct RasterDepthLayer {
        Id resource = 0, depth = 0;
        uint64_t sample = 0;
        uint32_t frame = 0;
        float min_depth = 0, max_depth = 0;
        std::bitset<kDepthSlots> pixel_reads;
    } raster_depth_layer;
    uint32_t target_width = 0, target_height = 0;
    bool depth_layer_target = false, material_layer_target = false;
    bool vpj_shader_depth = false;
    bool vpj_still_mesh = false; // VARIANT: referee/sample-locked-dropout (moved at the Referee's ask)
    bool vpj_overlay = false;    // VARIANT: jitter/scene-overlays (an untested mesh into the scene colour)
    // VARIANT: jitter/stationary-source-readers: depth-like layers written this frame on the game's own grid
    // (R32_FLOAT targets drawn by draws that did not take the live sample), proven exact copies of them, and
    // which pixel-shader slots read one now.
    struct StationaryLayers { Id resource[4] = {}; uint32_t frame = 0; std::bitset<kDepthSlots> pixel_reads; } stationary;
    uint8_t vpj_still_class = 0; // why an untested scene mesh kept the game's grid (StillClass)
    uint8_t depth_func = kCompareLess; // the bound state's comparison when it tests AND writes depth, else 0 (default state: LESS, writing)
    Id cb[kCbBindSlots] = {};          // VS slots, then PS slots at kPsCbBase
    uint32_t cb_first[kCbBindSlots] = {}; // *SSetConstantBuffers1 first constant per slot
    float vp_x = 0, vp_y = 0, vp_w = 0, vp_h = 0; // viewport 0
    D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {}; // the game's own viewports
    UINT vp_n = 0;
    bool vpj_applied = false;          // our offset copies are bound on this context now
    bool vpj_want = false;             // the draw being decided takes the scene's sample (VpjBeforeDraw)
    uint32_t coarse_rate = 0;          // the shading rate we set on this context (0: one per pixel); never cleared by the
                                       // game's state calls (NVAPI keeps it), reset by FinishCommandList
    bool vpj_expanded = false;
    uint64_t vpj_sample = 0;          // the exact sample they carry
    uint32_t cb_num[kCbBindSlots] = {};   // *SSetConstantBuffers1 constant count per slot (0 = whole buffer)
    std::vector<Event> events;        // deferred recording
    bool events_truncated = false;
    // Deferred recording: every colour target it writes (draws, clears, copies), so the HUD-free
    // picture can tell whether a command list draws into the shown picture (hud_free::OnExecuteList).
    std::vector<Id> list_writes;
    bool list_writes_unknown = false;
    ViewUse rt_view, ds_view;
    std::vector<DeferredUpload> cb_bytes; // (event index, bytes) for CbUpdate events
    std::unordered_map<Id, void*> mapped; // candidate constant buffers mapped right now
    // Large constant buffers this context has itself mapped for writing since its state began.
    struct LargeMap { uint32_t size = 0, epoch = 0; bool no_overwrite = false; };
    std::unordered_map<Id, LargeMap> large;
    // The exact range last read for each bound slot, and how long the same range has stayed bound.
    struct ExactSlot { Id id = 0; uint32_t first = 0, count = 0, epoch = 0, frame = 0, repeats = 0; CameraBytes bytes; };
    ExactSlot exact[kCbBindSlots];
    ExactSet exact_set;                     // what the current draw carries
    const void* exact_recorded = nullptr;   // deferred: the set the last recorded entry named
    bool exact_recorded_any = false;
    std::vector<DeferredExact> draw_bytes;  // deferred recording
    void reset_state()
    {
        sampler_policy = ~uint64_t{0};
        sampler_shadow = {};
        texture_policy = ~uint64_t{0};
        ps_shadow.Clear();
        screen_color = screen_peer = 0;
        pixel_shader = nullptr;
        writer_shader = 0;
        pixel_shader_known = false;
        screen_slots.reset();
        post = {};
        rt0 = depth = 0;
        world_inputs.scanned = false;
        stencil_writes = false;
        rt_view = ds_view = {};
        rt_count = 0;
        raster_depth_layer = {};
        target_width = target_height = 0;
        depth_layer_target = material_layer_target = vpj_shader_depth = vpj_still_mesh = vpj_overlay = false;
        stationary = {};
        vpj_still_class = 0;
        depth_test = true;
        depth_read_only = false;
        for (auto& mask : depth_reads.slots) mask.reset();
        depth_func = kCompareLess;
        std::memset(cb, 0, sizeof(cb));
        std::memset(cb_first, 0, sizeof(cb_first));
        std::memset(cb_num, 0, sizeof(cb_num));
        vp_x = vp_y = vp_w = vp_h = 0;
        vp_n = 0;
        vpj_applied = false;
    }
};
struct ListRecord {
    std::vector<Event> events;
    std::vector<Id> writes;      // colour targets the recording wrote (draws, clears, copies)
    bool writes_unknown = false; // too many to list
    std::vector<DeferredUpload> cb_bytes;
    std::vector<DeferredExact> draw_bytes;
    uint64_t order = 0;
    uint64_t incarnation = 0;
    bool complete = false;
};

std::mutex g_install_mu;
std::atomic<bool> g_installed{false};
SceneCallback g_cb = nullptr;
void* g_cb_user = nullptr;
TrialReader g_trial_reader = nullptr;
std::atomic<ParitySignal> g_parity_signal{nullptr}; // referee/parity-role-swap

std::mutex g_mu; // model, lists, camera bytes, references, stats
FrameModel g_model;
ScreenCheck g_screen;
ScreenCheck g_post_screen;
LitScene g_lit;
referee::PlacementTrial g_placement{uint64_t{1} << 57};
referee::StageRecovery g_recovery;
bool ParitySeen(const void* swapchain, const referee::PlacementScope& scope)
{
    const auto seen = g_parity_signal.load(std::memory_order_relaxed);
    return seen && seen(swapchain, scope);
}
referee::PlacementTrial::Scope LightingScope()
{
    return {g_lit.incarnations[0], g_lit.depth_incarnation, g_lit.generation, 0, 0, g_lit.incarnations[1]};
}
std::atomic<Id> g_lit_peer{0};
uint64_t g_last_lit_generation = 0;
std::atomic<bool> g_screen_enabled{false};
std::atomic<bool> g_discover_scale{false};
std::atomic<uint32_t> g_world_level{0};
// The world drawn small (DrawWorld, WorldRouteFrame): this frame's plan, and the books.
std::atomic<bool> g_world_live{false};      // this frame the world's draws really go to the small pictures
std::atomic<bool> g_world_reference{false}; // this frame is drawn at full size and made small afterwards (the Referee's comparison picture)
std::atomic<uint8_t> g_world_inputs{0};     // referee::WorldInputs for this frame
std::atomic<uint8_t> g_world_frame_hold{0}; // a reason from outside the draws that holds the whole world (WorldHold)
std::atomic<const char*> g_world_input_why{nullptr}; // the last reason a full-size input could not be made small

struct WorldRouteState { // under g_mu
    referee::WorldCheck check;
    referee::WorldRehearsal rehearsal;
    WorldRouteStatus status;
    uint64_t probe_seq = 0, frame_tag = 0, steady_frame = 0, logged_frame = 0, asked_frame = 0, last_small_frame = 0;
    bool frame_small = false, frame_reference = false, frame_inputs = false, live = false;
    WorldHold logged_why = WorldHold::None;
    WorldRouteStatus::State logged_state = WorldRouteStatus::State::Off;
    uint64_t job_color = 0, job_depth = 0, job_proof = 0;
    uint32_t job_level = 0;
} g_route;
bool g_was_discovering_scale = false;
std::atomic<bool> g_adaptive_scene{false}; // skip writer tagging on stable frames
std::unordered_map<ID3D11DeviceContext*, std::unique_ptr<Ctx>> g_ctx;
std::map<ID3D11CommandList*, ListRecord> g_lists;
uint64_t g_list_order = 0;
std::atomic<ID3D11Device*> g_device_raw{nullptr};
IDXGISwapChain* g_swap = nullptr; // identity only
Id g_backbuffer = 0;              // identity only, never pins a swap-chain buffer
uint64_t g_region_started = 0;    // largest allowed search window for a provisional colour/depth pair
std::atomic<Id> g_sel_color{0};    // readable from any thread
// VARIANT: scene/after-layers (the rule is native11/frame_model.hpp)
std::atomic<Id> g_sel_layer{0};         // the scene is drawn through material layers: their most-drawn target (0 = no layers)
constexpr size_t kWatch = 4;
std::atomic<Id> g_watch[kWatch];        // targets whose reads the frame model needs besides the scene colour

std::atomic<Id> g_cand[kMaxCandidates + 1] = {};
// Only populated by uploads larger than kMaxCbBytes. An empty window
// means the scene has not yet supplied a bounded range for that buffer.
std::unordered_map<Id, CaptureWindows> g_large_cb_windows;
using BindingKey = std::tuple<Id, int, uint32_t, uint32_t>;
std::map<BindingKey, CameraUpload> g_scene_uploads;
struct SplitVote {
    FrameModel::CameraBinding projection{};
    CameraUpload projection_upload;
    Id world_buffer = 0;
    uint32_t draws = 0;
};
std::map<SplitKey, SplitVote> g_split_votes;
struct SplitDraw {
    CameraBytes world, projection;
    BindingKey world_binding, projection_binding;
    SplitVote* vote = nullptr;
};
SplitDraw g_split_draw;
Id g_split_world = 0;
std::atomic<Id> g_split_candidate{0};
std::atomic<bool> g_collect_camera{true};
// VARIANT: camera11/all-uploads-when-buffers-rotate
// Uploads are normally kept only for the buffers the last frame's scene ranked highest. A game
// that takes its constant buffers from a rotating pool binds a different buffer object at the same
// place every frame, so that short list never holds this frame's camera: it is found on a frame
// that keeps every upload and lost on the next, on and on. Seen twice in a row, every upload is
// kept from then on. The camera's place (stage, slot, range) is what stays; the object is not.
bool g_collect_all = false;
uint64_t g_twin_wait_until = 0;   // camera/positioned-twin (across ranges): no twin was found; next look at this frame
uint32_t g_rotation_misses = 0;   // found while keeping everything, then no bytes at all on the short list
bool g_frame_fired = false, g_frame_found = false, g_frame_no_bytes = false; // this frame's boundary
bool g_prev_found_collecting = false;
std::unordered_map<Id, CameraUploads> g_cb_latest;
std::unordered_map<Id, uint64_t> g_cb_incarnation; // candidate camera buffers: lifetime id, tagged at upload (live object)

ComPtr<ID3D11Resource> g_color_ref, g_depth_ref;
ViewUse g_color_view, g_depth_view;
float g_scene_vp_x = 0, g_scene_vp_y = 0, g_scene_vp_w = 0, g_scene_vp_h = 0; // scene viewport this frame
uint32_t g_camera_first = 0, g_camera_num = 0; // exact bound range of the selected camera
uint64_t g_stream = 0;                    // SceneFrame::stream; bumps when any input object changes
uint64_t g_stream_key[4] = {};            // colour and depth lifetime ids, camera buffer id and its lifetime id (0 = untagged)
uint64_t g_scene_stream = 0;              // SceneFrame::scene_stream; bumps when the colour or depth object changes, never for a camera
uint64_t g_scene_key[2] = {};             // colour and depth lifetime ids
// VARIANT: camera/optional
// Whether a frame needs a camera to be handed over (input_provider.hpp CameraUse). Off: nothing
// camera-related runs at all, on any thread, so it is read without g_mu.
// The effective camera acquisition mode, including an optional reader such as XeGTAO.
std::atomic<uint8_t> g_camera_use{static_cast<uint8_t>(CameraUse::Required)};
std::atomic<bool> g_eval_paused{false}, g_camera_suspended{false};
uint32_t g_camera_failures = 0, g_camera_retry_frames = 0;
bool g_frame_delivered = false;
bool g_frame_delivered_lit = false;
constexpr uint32_t kCameraSearchBurst = 8, kCameraRetryFrames = 120;

CameraUse CameraSetting()
{
    return static_cast<CameraUse>(g_camera_use.load(std::memory_order_relaxed));
}
bool CameraTracking()
{
    return !g_eval_paused.load(std::memory_order_relaxed) && !g_camera_suspended.load(std::memory_order_relaxed) &&
           CameraSetting() != CameraUse::Off;
}
// VARIANT: depth/direction-from-depth-writes
// Which end of the selected depth is near, from that depth's own scene writes (depth_direction.hpp).
// Gathered on every frame, under g_mu; used when the frame has no camera to say it.
DepthDirection g_depth_dir;

CameraPick g_camera;
// VARIANT: camera11/held
// The last camera delivered, kept so a frame or two on which the game's camera could not be read
// still gets DLAA with that camera instead of a gap (a gap resets the temporal history, which shows
// as a flash of the unprocessed picture). The DX12 host holds its camera the same way.
constexpr uint64_t kHoldFrames = 3;
struct HeldCamera {
    bool valid = false;
    CameraPick cam;
    CameraUpload upload;
    uint64_t version = 0, frame = 0;
    std::optional<CameraFacts::RelativePose> split;
    Id cb = 0;
    int slot = -1;
    uint32_t first = 0, num = 0;
    Id color = 0, depth = 0;
};
HeldCamera g_held;
// The relative camera (providers/camera/relative_camera.hpp): where its two ranges sit. Seeded with the
// places one known game uses, replaced by what this game's own draws show when the seed gives no vote.
camera::RelativeLayout g_rel_layout = wwii::RelativeSeedLayout();
camera::RelativeLearner g_rel_learner;
bool g_rel_learned = false;  // the layout in use came from this game's draws, not from the seed
uint32_t g_rel_silent = 0;   // consecutive scene frames on which no draw voted under the layout
uint32_t g_rel_wait = 0;     // scene frames before the next learning attempt
camera::NoInverseCamera g_no_inverse; // the shared start and continue rules (the same ones the DX12 host uses)
Selection g_last_selection;
uint64_t g_frame = 0;
float g_next_jitter[2] = {0, 0};
bool g_jitter_requested = false;
neural_alias::JitterSample g_jitter{0, 0};
bool g_jitter_active = false;
bool g_jitter_written = false; // this frame's camera upload carried our sample
// Viewport jitter (viewport_jitter.hpp): the route used on the native path. The frame's sample is
// applied to the game's viewports on scene draws, on whichever context records them, so it is read
// without g_mu. While it is the route, the camera-matrix write (JitterUpload) stays out of it.
std::atomic<bool> g_vpj_route{false}; // SetJitterRoute; the host turns it on for the native path
std::atomic<uint64_t> g_vpj_sample{0}; // publish both float axes as one value
// Coarse shading (SetSceneCoarseShading): the rate asked for, and whether this frame's scene is still open.
std::atomic<uint32_t> g_coarse{0};
std::atomic<bool> g_coarse_open{false};
std::atomic<uint64_t> g_coarse_draws{0}, g_coarse_sets{0}, g_coarse_restores{0}, g_coarse_failures{0}, g_coarse_before_lists{0}, g_coarse_frames{0};
std::atomic<Id> g_sel_depth{0};
std::atomic<uint64_t> g_vpj_stopped_after_scene{0};
std::atomic<Id> g_sel_depth2{0}; // the second depth the picture is drawn with (scene/after-layers-second-depth)
std::atomic<uint32_t> g_sel_depth_w{0}, g_sel_depth_h{0};
uint32_t g_vpj_frame_draws = 0; // executed scene draws with a nonzero sample, under g_mu
uint32_t g_vpj_frame_unshifted = 0; // draws into the scene colour left unshifted on a shifted frame, under g_mu
// WHY log, under g_mu: how much of a shifted frame's geometry carried the sample, by vertices, and
// the meshes drawn with the scene depth bound that kept the game's own viewport (DLSS gets no
// sub-pixel samples for those: their edges stay as the game drew them).
uint64_t g_vpj_frame_shifted_vertices = 0, g_vpj_frame_still_vertices = 0;
uint32_t g_vpj_frame_still_meshes = 0, g_vpj_frame_still_untested = 0, g_vpj_frame_shader_depth = 0;
// VARIANT: referee/sample-locked-dropout
// Set by the Referee through SetStillMeshJitter while it tries or keeps its reaction to dropouts.
std::atomic<bool> g_vpj_still_asked{false};
uint32_t g_vpj_frame_still_moved = 0;
// VARIANT: jitter/scene-overlays (on) | jitter/depth-tested-only (off; the Referee's dropout watch takes the
// move back when pixels drop out on the samples after it). core/temporal_jitter.hpp SceneOverlayTakesJitter.
std::atomic<bool> g_vpj_overlays{true};
uint32_t g_vpj_frame_overlay_moved = 0;
// Untested scene meshes that kept the game's grid, by why (counted per frame for the log).
enum StillClass : uint8_t { kStillNone = 0, kStillDeferred, kStillOtherTarget, kStillOwnViewport, kStillRuleOff, kStillOther,
                            kStillStationarySource, kStillFullScreen, kStillClasses };
// VARIANT: referee/coverage-trial: the Referee's class (core/referee_coverage.hpp LeftClass) of a StillClass.
int CoverageClassOf(uint8_t still)
{
    using L = na::referee::LeftClass;
    switch (still) {
    case kStillOtherTarget: return int(L::OtherTarget);
    case kStillOwnViewport: return int(L::OwnViewport);
    case kStillRuleOff: return int(L::RuleOff);
    case kStillStationarySource: return int(L::StationarySource);
    case kStillDeferred: return int(L::Deferred);
    case kStillOther: return int(L::Other);
    case kStillFullScreen: return int(L::FullScreen);
    default: return -1;
    }
}
const char* StillClassName(uint8_t still)
{
    const int c = CoverageClassOf(still);
    return c >= 0 ? na::referee::LeftClassName(unsigned(c)) : "none";
}
// The classes the Referee moves onto the sample (kept, or on trial), the one on trial, and the draw signatures:
// moved during the current trial, and put back by an earlier one (those stay on the game's grid).
std::atomic<uint32_t> g_cov_mask{0};
std::atomic<int> g_cov_trial{-1};
std::mutex g_cov_m;
std::unordered_set<uint64_t> g_cov_trial_sigs, g_cov_still_sigs; // under g_cov_m
uint64_t g_cov_decisions = 0;                                    // under g_cov_m
std::atomic<uint64_t> g_cov_moved{0}, g_cov_kept_still{0};
std::atomic<uint64_t> g_cov_left_total[8]{}; // every frame's left-off draws by LeftClass, summed (tests, log)
uint64_t CoverageSignature(const void* pixel_shader, uint64_t vertices, uint8_t still)
{
    return (reinterpret_cast<uintptr_t>(pixel_shader) * 1099511628211ull) ^ (vertices << 8) ^ still;
}
// WHY log (owed 2026-11-27): once a minute, every scene-colour draw of one frame left off the jitter, named.
std::atomic<bool> g_left_name_frame{false};
// WHY log (owed 2026-11-27): our CPU time on the game's thread, timed on one frame in eight (the timer itself costs):
// camera finding and constant-upload keeping (camera), our own work around each draw call (draw_ours, the game's
// draw call itself excluded, which is draw_driver). Reported per timed frame in milliseconds.
std::atomic<bool> g_time_frame{false};
std::atomic<uint64_t> g_tsc_camera{0}, g_tsc_draw_ours{0}, g_tsc_draw_driver{0}, g_timed_frames{0};
std::atomic<uint64_t> g_cal_tsc0{0}, g_cal_qpc0{0};
struct TscScope {
    uint64_t t0;
    std::atomic<uint64_t>& acc;
    explicit TscScope(std::atomic<uint64_t>& a) : t0(g_time_frame.load(std::memory_order_relaxed) ? __rdtsc() : 0), acc(a) {}
    ~TscScope() { if (t0) acc.fetch_add(__rdtsc() - t0, std::memory_order_relaxed); }
};
struct DrawTimer {
    uint64_t t0 = g_time_frame.load(std::memory_order_relaxed) ? __rdtsc() : 0, driver = 0;
    ~DrawTimer()
    {
        if (!t0) return;
        const uint64_t all = __rdtsc() - t0;
        g_tsc_draw_ours.fetch_add(all > driver ? all - driver : 0, std::memory_order_relaxed);
        g_tsc_draw_driver.fetch_add(driver, std::memory_order_relaxed);
    }
};
struct DriverScope {
    DrawTimer& t;
    uint64_t t0;
    explicit DriverScope(DrawTimer& timer) : t(timer), t0(timer.t0 ? __rdtsc() : 0) {}
    ~DriverScope() { if (t0) t.driver += __rdtsc() - t0; }
};
double TscPerMs()
{
    LARGE_INTEGER q{}, f{};
    QueryPerformanceCounter(&q);
    QueryPerformanceFrequency(&f);
    const uint64_t t0 = g_cal_tsc0.load(), q0 = g_cal_qpc0.load();
    if (!t0 || !f.QuadPart || uint64_t(q.QuadPart) <= q0) return 0;
    const double ms = double(uint64_t(q.QuadPart) - q0) * 1000.0 / double(f.QuadPart);
    return ms > 100 ? double(__rdtsc() - t0) / ms : 0;
}
std::atomic<uint64_t> g_left_name_at{0};
uint32_t g_left_named = 0;
// The context's stationary layers of this frame (jitter/stationary-source-readers).
bool StationaryHas(const Ctx* x, Id r);
void StationaryAdd(Ctx* x, Id r);
void StationaryRemove(Ctx* x, Id r);
uint32_t g_vpj_frame_still_by[kStillClasses] = {};
constexpr uint64_t kFullScreenPassVertices = 6; // two triangles: more than this is a mesh
std::atomic<uint64_t> g_vpj_unshifted{0};
std::atomic<uint64_t> g_vpj_told{0}; // the sample last handed to the consumer (two floats, pixels)
uint64_t g_vpj_executed_sample = 0;
bool g_vpj_executed_expanded = false;
bool g_vpj_seen_draw = false, g_vpj_mixed = false;
std::atomic<uint64_t> g_vpj_mixed_frames{0};
std::atomic<uint64_t> g_vpj_frames{0}, g_vpj_draws{0}, g_vpj_sets{0}, g_vpj_restores{0}, g_vpj_unknown{0};
std::atomic<uint32_t> g_vpj_last_frame_draws{0};
ProviderStats g_stats;
char g_reason[192] = {};

thread_local bool t_inside = false; // our own and the consumer's calls are not observed
thread_local bool t_in_present = false;
thread_local ID3D11DeviceContext* t_last_ctx = nullptr;
thread_local Ctx* t_last = nullptr;
thread_local uint32_t t_last_gen = 0;
// Which device is "the game's" can change while the game runs (a game may make a second device and
// move to it). Every remembered answer to "is this context the game's?" carries the generation it was
// given in; a new game device makes all of them be asked again.
std::atomic<uint32_t> g_device_gen{1};
std::atomic<uint64_t> g_ctx_rechecked{0}, g_ctx_became_game{0}, g_foreign_calls{0};

struct Inside {
    bool was;
    Inside() : was(t_inside) { t_inside = true; }
    ~Inside() { t_inside = was; }
};

// The game sees its own full-size views whenever control returns to it. Materialize
// deferred pixel writes before a consumer or a game write can observe those views.
void FinishWorld(ID3D11DeviceContext* c, Ctx* x)
{
    if (!x) return;
    if (x->would_dirty) { // counted only: this is where the full-size pictures would be brought up to date
        x->would_dirty = x->would_depth_dirty = false;
        if (x->tally_frame == g_frame) ++x->tally.restores;
    }
    if (!x->world_dirty) return;
    Inside own;
    x->world->raster.Restore(c, x->world_stencil_written);
    x->world_stencil_written = false;
    x->world_dirty = x->world_depth_dirty = false;
    ++x->world_own_run;
    if (x->tally_frame == g_frame) ++x->tally.restores;
}
void TouchWorld(ID3D11DeviceContext* c, Ctx* x, Id id, bool write, bool picture = true)
{
    if (!x) return;
    if (write && picture) ++x->world_run; // a copy, a clear or a full-size draw: a picture the world reads may have changed
    if (!x->world || (id != x->world->color && id != x->world->depth)) return;
    FinishWorld(c, x);
    if (write) {
        x->world_frame = ~uint64_t{0};
        ++x->world_own_run;
        if (id == x->world->color) x->pristine_color = false;
        else x->pristine_depth = false;
    }
}

void SetReason(const char* r)
{
    std::snprintf(g_reason, sizeof g_reason, "%s", r);
    g_stats.last_miss = g_reason;
}

Id IdOf(ID3D11Resource* r) { return reinterpret_cast<Id>(r); }

// Lifetime id of a D3D11 object, kept in its private data: an object created at a freed object's
// address has no tag yet and gets a new value, which the pointer alone cannot tell apart.
// {6b0f8a2e-3c1d-4e57-9a40-7d2b51c9e3f1}
const GUID kIncarnationGuid = {0x6b0f8a2e, 0x3c1d, 0x4e57, {0x9a, 0x40, 0x7d, 0x2b, 0x51, 0xc9, 0xe3, 0xf1}};
std::atomic<uint64_t> g_next_incarnation{1};
uint64_t IncarnationOf(ID3D11DeviceChild* o)
{
    if (!o) return 0;
    uint64_t v = 0;
    UINT size = sizeof(v);
    if (SUCCEEDED(o->GetPrivateData(kIncarnationGuid, &size, &v)) && size == sizeof(v) && v) return v;
    v = g_next_incarnation.fetch_add(1, std::memory_order_relaxed);
    o->SetPrivateData(kIncarnationGuid, sizeof(v), &v);
    return v;
}
Id ResourceOfView(ID3D11View* v)
{
    if (!v) return 0;
    ID3D11Resource* r = nullptr;
    v->GetResource(&r);
    if (!r) return 0;
    const Id id = IdOf(r);
    r->Release();
    return id;
}
ResourceInfo InfoOf(ID3D11Resource* r, bool depth)
{
    ResourceInfo i;
    i.depth = depth;
    D3D11_RESOURCE_DIMENSION dim{};
    r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return i;
    D3D11_TEXTURE2D_DESC d{};
    static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
    i.width = d.Width;
    i.height = d.Height;
    i.format = d.Format;
    i.samples = d.SampleDesc.Count;
    return i;
}
bool IsCandidate(Id id)
{
    if (!id || !CameraTracking()) return false;
    if (g_collect_camera.load(std::memory_order_relaxed) ||
        g_split_candidate.load(std::memory_order_relaxed) == id) return true;
    for (auto& c : g_cand)
        if (c.load(std::memory_order_relaxed) == id) return true;
    return false;
}
// The Ctx for a context of the game's device (nullptr otherwise, or inside our own work).
Ctx* Get(ID3D11DeviceContext* c)
{
    if (t_inside || !c) return nullptr;
    const uint32_t gen = g_device_gen.load(std::memory_order_acquire);
    if (t_last_ctx == c && t_last_gen == gen) {
        if (t_last && t_last->game) return t_last;
        g_foreign_calls.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    ID3D11Device* game = g_device_raw.load(std::memory_order_acquire);
    if (!game) return nullptr;
    std::lock_guard<std::mutex> l(g_mu);
    auto& slot = g_ctx[c];
    const bool fresh = !slot;
    if (fresh) slot = std::make_unique<Ctx>();
    if (fresh || slot->gen != gen) {
        // Asked again for every new game device: an answer given while another device was the game's
        // (or before this one was announced) must not stand, or a game that moves to a second device
        // is never seen again.
        Inside in;
        ComPtr<ID3D11Device> d;
        c->GetDevice(&d);
        const bool was = slot->game;
        const bool is = d.Get() == game;
        if (!fresh) {
            g_ctx_rechecked.fetch_add(1, std::memory_order_relaxed);
            if (is && !was) g_ctx_became_game.fetch_add(1, std::memory_order_relaxed);
            // What was remembered belongs to the other device's objects.
            slot->reset_state();
            slot->world.reset();
            slot->world_dirty = false;
            slot->world_uavs.reset();
            slot->world_frame = slot->world_color_clear = slot->world_depth_clear = ~uint64_t{0};
            slot->depth_reads = {};
            slot->raster_queries = 0;
            slot->events.clear();
            slot->cb_bytes.clear();
            slot->mapped.clear();
            slot->large.clear();
            slot->draw_bytes.clear();
        }
        slot->game = is;
        slot->gen = gen;
        slot->deferred = c->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED;
    }
    t_last_ctx = c;
    t_last = slot.get();
    t_last_gen = gen;
    if (slot->game) return slot.get();
    g_foreign_calls.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

// ---------------------------------------------------------------- originals
using PFN_VSSetCB = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
using PFN_VSSetCB1 = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, UINT, UINT, ID3D11Buffer* const*, const UINT*, const UINT*);
using PFN_SetSRV = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
using PFN_DrawIndexed = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using PFN_Draw = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using PFN_DrawIndexedInstanced = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using PFN_DrawInstanced = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using PFN_DrawIndirect = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using PFN_DrawAuto = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using PFN_Dispatch = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
using PFN_Query = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Asynchronous*);
using PFN_Map = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
using PFN_Unmap = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using PFN_OMSetRT = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
using PFN_OMSetRTUAV = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT,
                                                UINT, ID3D11UnorderedAccessView* const*, const UINT*);
using PFN_OMSetDSS = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilState*, UINT);
using PFN_RSSetViewports = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
using PFN_CopyRegion = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT,
                                                const D3D11_BOX*);
using PFN_CopyResource = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
using PFN_UpdateSub = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
using PFN_UpdateSub1 = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT, UINT);
using PFN_ClearDSV = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
using PFN_Resolve = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, ID3D11Resource*, UINT, DXGI_FORMAT);
using PFN_Execute = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
using PFN_ClearState = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using PFN_Finish = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, BOOL, ID3D11CommandList**);
using PFN_Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using PFN_Present1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using PFN_Resize = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

struct Originals {
    PFN_VSSetCB vs_cb = nullptr, ps_cb = nullptr;
    PFN_VSSetCB1 vs_cb1 = nullptr, ps_cb1 = nullptr;
    void (STDMETHODCALLTYPE *ps_shader)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT) = nullptr;
    void (STDMETHODCALLTYPE *clear_rtv)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT*) = nullptr;
    PFN_SetSRV ps_srv = nullptr, cs_srv = nullptr, vs_srv = nullptr;
    PFN_DrawIndexed draw_indexed = nullptr;
    PFN_Draw draw = nullptr;
    PFN_DrawIndexedInstanced draw_indexed_instanced = nullptr;
    PFN_DrawInstanced draw_instanced = nullptr;
    PFN_DrawIndirect draw_indexed_indirect = nullptr, draw_indirect = nullptr;
    PFN_DrawAuto draw_auto = nullptr;
    PFN_Dispatch dispatch = nullptr;
    PFN_DrawIndirect dispatch_indirect = nullptr;
    PFN_Query begin = nullptr, end = nullptr;
    PFN_Map map = nullptr;
    PFN_Unmap unmap = nullptr;
    PFN_OMSetRT om_rt = nullptr;
    PFN_OMSetRTUAV om_rt_uav = nullptr;
    PFN_OMSetDSS om_dss = nullptr;
    PFN_RSSetViewports rs_vp = nullptr;
    PFN_CopyRegion copy_region = nullptr;
    PFN_CopyResource copy_resource = nullptr;
    PFN_UpdateSub update = nullptr;
    PFN_UpdateSub1 update1 = nullptr;
    void (STDMETHODCALLTYPE *copy1)(ID3D11DeviceContext1*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*, UINT) = nullptr;
    void (STDMETHODCALLTYPE *discard_resource)(ID3D11DeviceContext1*, ID3D11Resource*) = nullptr;
    void (STDMETHODCALLTYPE *discard_view)(ID3D11DeviceContext1*, ID3D11View*) = nullptr;
    void (STDMETHODCALLTYPE *discard_view1)(ID3D11DeviceContext1*, ID3D11View*, const D3D11_RECT*, UINT) = nullptr;
    void (STDMETHODCALLTYPE *clear_view)(ID3D11DeviceContext1*, ID3D11View*, const FLOAT*, const D3D11_RECT*, UINT) = nullptr;
    void (STDMETHODCALLTYPE *swap_state)(ID3D11DeviceContext1*, ID3DDeviceContextState*, ID3DDeviceContextState**) = nullptr;
    PFN_ClearDSV clear_dsv = nullptr;
    PFN_Resolve resolve = nullptr;
    PFN_Execute execute = nullptr;
    PFN_ClearState clear_state = nullptr;
    PFN_Finish finish = nullptr;
};
Originals g_o[2]; // immediate-context class, deferred-context class
PFN_Present g_present = nullptr;
PFN_Present1 g_present1 = nullptr;
PFN_Resize g_resize = nullptr;

inline const Originals& O(ID3D11DeviceContext* c, Ctx* x)
{
    if (x) return g_o[x->deferred ? 1 : 0];
    Inside in;
    return g_o[c->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED ? 1 : 0];
}

// ---------------------------------------------------------------- the boundary
// Order rejected candidates by how far they passed the WWII layout checks.
// Count only the strongest result at a missed boundary, not every bound buffer.
enum class PoseCheck { NoPacket, Extent, Repeat, Origin, Shape, Native, Legacy, Generic, NoInverse, Started, Continued };

// `learned_binding`: these bytes are the range the learned camera was found at.
CameraPick PickForScene(const std::vector<uint8_t>& bytes, const CameraPick& learned, bool learned_binding,
                        uint32_t width, uint32_t height, uint64_t frame, PoseCheck& check)
{
    // The general rules first: the learned place re-read, then a view-projection with its inverse.
    // A camera learned without an inverse stays the camera only while it still fills the scene picture.
    CameraPick pick;
    if (CheckLearned(bytes, learned, &pick, learned_binding) &&
        (!pick.derived_inverse || camera::MatchesSceneAspect(pick.vp, width, height))) {
        // VARIANT: camera/positioned-twin (the rule is providers/camera/camera_admission.hpp)
        camera::Candidate twin;
        if (pick.derived_inverse && camera::IsCameraRelative(pick.vp) &&
            camera::PositionedTwin(bytes.data(), bytes.size(), pick.vp, width, height, twin)) {
            pick.offset = twin.byte_offset;
            pick.layout = twin.layout;
            pick.vp = twin.matrix;
            pick.reversed = twin.reversed;
        }
        check = pick.derived_inverse ? PoseCheck::NoInverse : PoseCheck::Generic;
        return pick;
    }
    pick = FindCamera(bytes, frame);
    if (pick.found) {
        check = PoseCheck::Generic;
        return pick;
    }
    // VARIANT: camera11/packed
    // Then the known packed layouts (adapters/wwii): a camera-relative packet that carries the
    // world origin and redundant witnesses instead of an inverse. An adapter only adds a
    // candidate the general rules could not prove; it never replaces one they found.
    wwii::SceneCamera scene;
    const char* why = nullptr;
    if (wwii::DecodeSceneCamera(bytes, static_cast<uint32_t>(bytes.size()), width, height, scene, &why)) {
        pick = {};
        pick.found = pick.reversed = pick.camera_relative = true;
        pick.vp = scene.vp;
        pick.raster_jitter_qualified = scene.raster_jitter_qualified;
        check = scene.raster_jitter_qualified ? PoseCheck::Legacy : PoseCheck::Native;
        return pick;
    }
    if (std::strcmp(why, "vp_shape") == 0) check = PoseCheck::Shape;
    else if (std::strcmp(why, "vp_repeat") == 0) check = PoseCheck::Repeat;
    else if (std::strncmp(why, "origin", 6) == 0 || std::strcmp(why, "empty_origin") == 0) check = PoseCheck::Origin;
    else if (std::strcmp(why, "extent") == 0) check = PoseCheck::Extent;
    return {};
}

std::vector<uint8_t> BoundCameraBytes(const uint8_t* bytes, size_t size, uint32_t first, uint32_t count)
{
    const size_t offset = size_t(first) * 16;
    if (!bytes || offset >= size) return {};
    const size_t length = count ? std::min(size_t(count) * 16, size - offset) : size - offset;
    return {bytes + offset, bytes + offset + length};
}
// Large-buffer snapshots must contain the entire shader-visible range.
// Normal uploads retain their existing range semantics.
bool UploadRange(const CameraUpload& upload, uint32_t first, uint32_t count, size_t& start, size_t& length)
{
    if (!upload.bytes || first < upload.first_constant) return false;
    if (!SplitBoundRange(upload.bytes->size(), first - upload.first_constant, count, start, length)) return false;
    return !upload.windowed || (count && length == size_t(count) * 16);
}

void LearnLargeCbWindows()
{
    if (g_large_cb_windows.empty()) return;
    decltype(g_large_cb_windows) next;
    const auto& selection = g_model.Current();
    auto bindings = g_model.SceneCameraBindings();
    // Prefer the binding that supplied a qualified camera. Remaining ranges
    // must be used by a meaningful share of scene draws, not one object draw.
    const auto camera = [&](const auto& binding) {
        return binding.buffer == selection.camera_cb && binding.slot == selection.camera_slot &&
               binding.first == g_camera_first && binding.count == g_camera_num;
    };
    std::stable_partition(bindings.begin(), bindings.end(), camera);
    for (const auto& binding : bindings) {
        if (next.size() >= kMaxCandidates && !next.count(binding.buffer)) continue;
        if (!binding.count || binding.count > kLargeCbWindowBytes / 16 ||
            binding.first > UINT32_MAX / 16 - binding.count ||
            !g_large_cb_windows.count(binding.buffer) ||
            (!camera(binding) && binding.draws * uint64_t{4} < g_model.SceneDrawsThisFrame())) continue;
        const uint32_t begin = binding.first * 16;
        const uint32_t length = binding.count * 16;
        const uint32_t padding = std::min(kLargeCbDriftBytes, (kLargeCbWindowBytes - length) / 2);
        const uint32_t window_begin = begin > padding ? begin - padding : 0;
        const uint32_t trailing = std::min(padding, UINT32_MAX - begin - length);
        next[binding.buffer].Remember({window_begin, begin + length + trailing - window_begin});
    }
    // The same ring can alternate between disjoint regions. Keep recent
    // windows behind this frame's strongest evidence, within the copy budget.
    for (auto& [id, windows] : next) {
        const auto& previous = g_large_cb_windows.at(id);
        for (size_t i = 0; i < previous.count; ++i) windows.Remember(previous.ranges[i]);
    }
    g_large_cb_windows.swap(next);
}

CaptureWindows CameraWindows(Id id)
{
    std::lock_guard<std::mutex> l(g_mu);
    const auto it = g_large_cb_windows.find(id);
    if (it != g_large_cb_windows.end()) return it->second;
    if (g_large_cb_windows.size() < 64) g_large_cb_windows.try_emplace(id);
    return {};
}

const CameraUpload* UploadForDraw(Id id, uint32_t first, uint32_t count)
{
    const auto found = g_cb_latest.find(id);
    if (found == g_cb_latest.end()) return nullptr;
    const auto& uploads = found->second;
    const uint32_t version = g_model.CbVersion(id);
    if (uploads.primary.version != version) return nullptr;
    if (!uploads.primary.windowed) return &uploads.primary;
    size_t start = 0, length = 0;
    if (UploadRange(uploads.primary, first, count, start, length)) return &uploads.primary;
    for (const auto& upload : uploads.additional)
        if (UploadRange(upload, first, count, start, length)) return &upload;
    return &uploads.primary; // retains provenance for diagnostics, but range admission will reject it
}

bool SplitCamera(uint32_t w, uint32_t h, CameraPick& pick, const CameraUpload*& upload,
                 uint64_t& version, std::optional<CameraFacts::RelativePose>& pose)
{
    // VARIANT: camera/relative (the rule itself is providers/camera/relative_camera.hpp)
    auto chosen = g_split_votes.cend();
    camera::Matrix vp{};
    const SplitVerdict verdict = ChooseSplitCamera(
        g_rel_layout, g_split_votes, [](const SplitVote& vote) { return vote.draws; }, w, h,
        g_model.SceneDrawsThisFrame(), chosen, vp);
    if (chosen != g_split_votes.cend()) pick.vp = vp;
    switch (verdict) {
    case SplitVerdict::Chosen: break;
    case SplitVerdict::NoVotes: ++g_stats.split_no_votes; break;
    case SplitVerdict::NoProjection: ++g_stats.split_no_projection; break;
    case SplitVerdict::FewDraws: ++g_stats.split_few_draws; break;
    case SplitVerdict::NoMajority: ++g_stats.split_no_majority; break;
    case SplitVerdict::SmallShare: ++g_stats.split_small_share; break;
    }
    if (verdict != SplitVerdict::Chosen) return false;
    const SplitKey* best_key = &chosen->first;
    const SplitVote* best = &chosen->second;
    const auto& b = best->projection;
    pose = CameraFacts::RelativePose{pick.vp, {best_key->second[0], best_key->second[1], best_key->second[2]}};
    pick.found = pick.camera_relative = true;
    pick.reversed = g_rel_layout.reversed;
    pick.raster_jitter_qualified = false;
    g_camera = pick;
    g_model.SetCamera(b.buffer, b.slot);
    g_camera_first = b.first;
    g_camera_num = b.count;
    g_split_world = best->world_buffer;
    upload = &best->projection_upload;
    version = b.version;
    return true;
}

// VARIANT: camera/no-inverse
// The game uploads no inverse beside its camera, so the inverse-pair rule can never find it. The
// shared rules (providers/camera/camera_admission.hpp, the same ones the DX12 host uses) admit a
// view-projection that continues the last main camera, or one that keeps its place and moves like
// one camera over consecutive scene frames. Only the list of bound ranges is DX11's own.
// The ranges this scene's draws bound, as the shared camera rules take them (best-ranked first).
void SceneBlocks(const std::vector<FrameModel::CameraBinding>& bindings, std::vector<camera::AdmissionBlock>& blocks,
                 std::vector<size_t>& source, std::vector<const CameraUpload*>& uploads)
{
    for (size_t i = 0; i < bindings.size() && blocks.size() < camera::NoInverseCamera::kMaxBlocks; ++i) {
        const auto& b = bindings[i];
        const auto it = g_scene_uploads.find({b.buffer, b.slot, b.first, b.count});
        if (it == g_scene_uploads.end() || it->second.version != b.version || !it->second.bytes) continue;
        size_t start = 0, length = 0;
        if (!UploadRange(it->second, b.first, b.count, start, length)) continue;
        camera::AdmissionBlock block;
        // The place as the shaders see it: stage and slot, and the bound range. Never the buffer.
        block.slot.binding = static_cast<uint64_t>(b.slot);
        block.slot.range = (uint64_t(b.first) << 32) | b.count;
        block.slot.size = length;
        block.bytes = it->second.bytes->data() + start;
        block.size = length;
        blocks.push_back(block);
        source.push_back(i);
        uploads.push_back(&it->second);
    }
}
bool AdmitWithoutInverse(const std::vector<FrameModel::CameraBinding>& bindings, uint32_t w, uint32_t h, CameraPick* out,
                         const CameraUpload** upload_out, uint64_t* version_out, PoseCheck& result)
{
    std::vector<camera::AdmissionBlock> blocks;
    std::vector<size_t> source;                       // blocks[i] came from bindings[source[i]]
    std::vector<const CameraUpload*> uploads; // ... and these kept bytes
    SceneBlocks(bindings, blocks, source, uploads);
    const camera::Admission a = g_no_inverse.Admit(blocks, w, h, g_frame);
    if (a.how == camera::Admitted::None) {
        switch (a.why) {
        case camera::NotAdmitted::Admitted: break;
        case camera::NotAdmitted::PositionGame: ++g_stats.start_position_game; break;
        case camera::NotAdmitted::NoCandidate: ++g_stats.start_no_candidate; break;
        case camera::NotAdmitted::InverseGame: ++g_stats.start_inverse_game; break;
        case camera::NotAdmitted::Waiting: ++g_stats.start_waiting; break;
        }
        return false;
    }
    const auto& b = bindings[source[a.block]];
    CameraPick pick;
    pick.found = pick.derived_inverse = true;
    pick.offset = a.candidate.byte_offset; // inside the bound range, as CheckLearned re-reads it
    pick.layout = a.candidate.layout;
    pick.vp = a.candidate.matrix;
    pick.reversed = a.candidate.reversed;
    result = a.how == camera::Admitted::Started ? PoseCheck::Started : PoseCheck::Continued;
    g_camera = pick;
    g_model.SetCamera(b.buffer, b.slot);
    g_camera_first = b.first;
    g_camera_num = b.count;
    *out = pick;
    *upload_out = uploads[a.block];
    *version_out = b.version;
    return true;
}

// The camera for this frame (under g_mu): the learned block re-read, else a search of the candidates.
bool CameraSearch(CameraPick* out, const CameraUpload** upload_out, uint64_t* version_out,
                  PoseCheck& result, std::optional<CameraFacts::RelativePose>& split_pose)
{
    const Selection& s = g_model.Current();
    const uint32_t w = g_scene_vp_w >= 1.f ? static_cast<uint32_t>(g_scene_vp_w + 0.5f) : s.color_info.width;
    const uint32_t h = g_scene_vp_h >= 1.f ? static_cast<uint32_t>(g_scene_vp_h + 0.5f) : s.color_info.height;
    // Prefer the stable split source when viewport jitter owns raster positioning.
    const auto split = [&] {
        if (!SplitCamera(w, h, *out, *upload_out, *version_out, split_pose)) return false;
        result = PoseCheck::Native;
        return true;
    };
    const bool viewport = g_vpj_route.load(std::memory_order_relaxed);
    if (viewport && split()) return true;
    auto bindings = g_model.SceneCameraBindings();
    // Best-ranked first: the ranges most scene draws read.
    std::stable_sort(bindings.begin(), bindings.end(), [](const auto& l, const auto& r) { return l.draws > r.draws; });
    // Prefer the learned place, but never substitute its last frame's bytes.
    // VARIANT: camera/learned-place (the rule is providers/camera/camera_admission.hpp)
    std::vector<camera::BoundRange> ranges;
    ranges.reserve(bindings.size());
    for (const auto& b : bindings) ranges.push_back({b.buffer, b.slot, b.first, b.count, b.draws});
    const int learned_index = camera::LearnedRange(ranges, s.camera_cb, s.camera_slot, g_camera_first, g_camera_num);
    uint32_t learned_first = 0, learned_count = 0;
    Id learned_buffer = 0;
    int learned_slot = -1;
    if (learned_index >= 0) {
        const auto& b = bindings[static_cast<size_t>(learned_index)];
        learned_buffer = b.buffer; learned_slot = b.slot; learned_first = b.first; learned_count = b.count;
    }
    const auto is_learned = [&](const auto& b) {
        return learned_index >= 0 && b.buffer == learned_buffer && b.slot == learned_slot &&
               b.first == learned_first && b.count == learned_count;
    };
    std::stable_partition(bindings.begin(), bindings.end(), is_learned);
    struct Found { CameraPick pick; const FrameModel::CameraBinding* binding; const CameraUpload* upload; PoseCheck check; };
    const auto accept = [&](const Found& f) {
        result = f.check;
        g_camera = f.pick;
        g_model.SetCamera(f.binding->buffer, f.binding->slot);
        g_camera_first = f.binding->first;
        g_camera_num = f.binding->count;
        *out = f.pick;
        *upload_out = f.upload;
        *version_out = f.binding->version;
    };
    std::optional<Found> relative_first;
    for (const auto& b : bindings) {
        const auto it = g_scene_uploads.find({b.buffer, b.slot, b.first, b.count});
        if (it == g_scene_uploads.end() || it->second.version != b.version || !it->second.bytes) continue;
        const auto& upload = it->second;
        size_t start = 0, length = 0;
        if (!UploadRange(upload, b.first, b.count, start, length)) continue;
        const std::vector<uint8_t> bound(upload.bytes->data() + start, upload.bytes->data() + start + length);
        PoseCheck check = PoseCheck::NoPacket;
        const CameraPick p = PickForScene(bound, g_camera, is_learned(b), w, h, g_frame + 1, check);
        if (!p.found) {
            if (!relative_first) result = std::max(result, check);
            continue;
        }
        // VARIANT: camera/positioned-twin (across ranges)
        // A game may upload its camera twice in different ranges: once with its position and once
        // relative to the camera. Which range most draws read changes with what is on screen, so
        // the first one found can flip between the two from frame to frame, and each flip looks
        // like a camera cut (p45020: 754 cuts in 55 s, history reset on two frames of every three).
        // A position-less camera found by the general rules is therefore kept aside while the rest
        // of the scene's ranges are looked through for the same camera with its position.
        const bool general = check == PoseCheck::Generic || check == PoseCheck::NoInverse;
        if (!relative_first && general && g_frame >= g_twin_wait_until && camera::IsCameraRelative(p.vp)) {
            relative_first = Found{p, &b, &upload, check};
            continue;
        }
        if (relative_first) {
            if (!camera::IsPositionedTwin(p.vp, relative_first->pick.vp)) continue;
            ++g_stats.twin_across_ranges;
        }
        accept(Found{p, &b, &upload, check});
        return true;
    }
    if (relative_first) {
        // No range holds it with a position: the game has only the relative one. Not looked for
        // again for a while, so such a game pays for the wider search about once a second.
        g_twin_wait_until = g_frame + 64;
        accept(*relative_first);
        return true;
    }
    if (!viewport && split()) return true;
    if (AdmitWithoutInverse(bindings, w, h, out, upload_out, version_out, result)) return true;
    g_camera = {};
    g_model.SetCamera(0, -1);
    return false;
}
// VARIANT: camera/relative (learning where the two ranges sit)
// At the scene boundary. Runs only while the game lacks a camera position and the layout in use has
// given no vote for several scene frames, so a game whose layout works pays nothing here.
void LearnRelativeLayout(bool found, const CameraPick& cam)
{
    g_rel_silent = g_split_votes.empty() ? g_rel_silent + 1 : 0;
    if (g_rel_learner.armed()) {
        camera::RelativeLayout learned;
        if (g_rel_learner.Finish(g_model.SceneDrawsThisFrame(), learned)) {
            g_rel_layout = learned;
            g_rel_learned = true;
            g_rel_silent = 0;
            ++g_stats.relative_learned;
            na::eventlog::Emit("native11_relative",
                               "learned frame=%llu world{slot=%d places=%u at=%u,%u form=%d} projection{slot=%d size=%u at=%u copies=%u layout=%s reversed=%d}",
                               static_cast<unsigned long long>(g_frame), static_cast<int>(learned.world_binding), learned.witnesses,
                               learned.witness[0].offset, learned.witness[1].offset, static_cast<int>(learned.witness[0].form),
                               static_cast<int>(learned.projection_binding), learned.projection_size, learned.projection_offset,
                               learned.projection_copies, learned.projection_layout == camera::Layout::RowMajor ? "row" : "column",
                               learned.reversed ? 1 : 0);
        } else {
            ++g_stats.relative_learn_failed; // a projection, but no world range witnessed a position on enough draws
            g_rel_wait = 64;
        }
        return;
    }
    if (g_rel_wait) { --g_rel_wait; return; }
    const bool lacks_position = !found || (cam.derived_inverse && camera::IsCameraRelative(cam.vp));
    if (!lacks_position) return;
    if (g_rel_learned) {
        // A learned layout that stops giving votes: back to the seed first. It costs nothing to
        // try, and learning starts again from it if it stays silent too.
        if (g_rel_silent < 2) return;
        g_rel_layout = wwii::RelativeSeedLayout();
        g_rel_learned = false;
        g_rel_silent = 0;
        return;
    }
    if (g_rel_silent < 8) return;
    const Selection& s = g_model.Current();
    const uint32_t w = g_scene_vp_w >= 1.f ? static_cast<uint32_t>(g_scene_vp_w + 0.5f) : s.color_info.width;
    const uint32_t h = g_scene_vp_h >= 1.f ? static_cast<uint32_t>(g_scene_vp_h + 0.5f) : s.color_info.height;
    std::vector<camera::AdmissionBlock> blocks;
    std::vector<size_t> source;
    std::vector<const CameraUpload*> uploads;
    SceneBlocks(g_model.SceneCameraBindings(), blocks, source, uploads);
    if (g_rel_learner.FindProjection(blocks, w, h)) return;
    g_rel_wait = 64; // not a camera-relative scene, or no block that says which matrix is this frame's
    if (g_rel_learner.ambiguous()) {
        ++g_stats.relative_ambiguous;
        na::eventlog::Emit("native11_relative", "projection_ambiguous frame=%llu: blocks hold several different camera-relative scene projections and none holds exactly one",
                           static_cast<unsigned long long>(g_frame));
    }
}

// VARIANT: camera11/held (when it applies)
// True when this frame's scene draws bound the held camera's buffer at that slot with at least one
// range that could not be read here. That is the only miss a held camera covers: the game drew
// with constants we could not see, at the place its camera comes from. A frame on which every such
// range was read and none gave a camera (contradicting positions, a matrix that does not fit the
// picture) is evidence, and stays a miss.
bool HeldCameraUnreadable(Id buffer, int slot)
{
    for (const auto& b : g_model.SceneCameraBindings()) {
        if (b.buffer != buffer || b.slot != slot) continue;
        const auto it = g_scene_uploads.find({b.buffer, b.slot, b.first, b.count});
        if (it == g_scene_uploads.end() || it->second.version != b.version || !it->second.bytes) return true;
        size_t start = 0, length = 0;
        if (!UploadRange(it->second, b.first, b.count, start, length)) return true;
    }
    return false;
}

bool CameraNow(CameraPick* out, const CameraUpload** upload_out, uint64_t* version_out,
               PoseCheck& result, std::optional<CameraFacts::RelativePose>& split_pose)
{
    const bool found = CameraSearch(out, upload_out, version_out, result, split_pose);
    LearnRelativeLayout(found, *out);
    if (!found) return false;
    // What the shared rules need to know about this game, whichever rule found the camera.
    if (out->camera_relative) g_no_inverse.PositionWitnessed(); // the game shows where the camera is
    g_no_inverse.Remember(out->vp, result == PoseCheck::Generic, g_frame);
    return true;
}

void CountPoseCheck(PoseCheck result)
{
    switch (result) {
    case PoseCheck::NoPacket: ++g_stats.pose_no_packet; break;
    case PoseCheck::Extent: ++g_stats.pose_extent; break;
    case PoseCheck::Repeat: ++g_stats.pose_repeat; break;
    case PoseCheck::Origin: ++g_stats.pose_origin; break;
    case PoseCheck::Shape: ++g_stats.pose_shape; break;
    case PoseCheck::Native: ++g_stats.pose_native; break;
    case PoseCheck::Legacy: ++g_stats.pose_legacy; break;
    case PoseCheck::Generic: ++g_stats.pose_generic; break;
    case PoseCheck::NoInverse: ++g_stats.pose_no_inverse; break;
    case PoseCheck::Started: ++g_stats.pose_started; break;
    case PoseCheck::Continued: ++g_stats.pose_continued; break;
    }
}

// Diagnostics for a camera miss (under g_mu), at most every 5 s: what the scene draws read and why
// each candidate was turned down, so one run names the failing link (Cort 2026-11-09: no guessing).
// One summary event, plus one event per candidate with the first 1 KB of its bound bytes in hex
// for the first four rounds and then once a minute (offline match against the WWII layouts).
uint64_t g_cam_diag_tick = 0;
uint32_t g_cam_diag_rounds = 0;
void DiagnoseCameraMiss()
{
    const uint64_t now = GetTickCount64();
    if (g_cam_diag_tick && now - g_cam_diag_tick < 5000) return;
    g_cam_diag_tick = now;
    const bool dump = g_cam_diag_rounds < 4 || g_cam_diag_rounds % 12 == 0;
    ++g_cam_diag_rounds;
    const Selection& s = g_model.Current();
    const uint32_t w = g_scene_vp_w >= 1.f ? static_cast<uint32_t>(g_scene_vp_w + 0.5f) : s.color_info.width;
    const uint32_t h = g_scene_vp_h >= 1.f ? static_cast<uint32_t>(g_scene_vp_h + 0.5f) : s.color_info.height;
    const auto cands = g_model.SceneCameraBindings();
    na::eventlog::Emit("native11_cam",
                       "frame=%llu colour=%ux%u fmt=%u depth=%ux%u fmt=%u scene_vp=%.0fx%.0f+%.0f+%.0f decode_wh=%ux%u scene_draws=%u "
                       "cb_bindings=%zu ranked=%zu searched=%zu kept_buffers=%zu draw_snapshots=%zu learned_cb=%d large_mapped_buffers=%zu",
                       static_cast<unsigned long long>(g_frame), s.color_info.width, s.color_info.height, s.color_info.format,
                       s.depth_info.width, s.depth_info.height, s.depth_info.format, g_scene_vp_w, g_scene_vp_h, g_scene_vp_x,
                       g_scene_vp_y, w, h, g_model.SceneDrawsThisFrame(), g_model.CbSceneBindings(), g_model.RankedCandidates(),
                       cands.size(), g_cb_latest.size(), g_scene_uploads.size(), s.camera_cb ? 1 : 0, g_large_cb_windows.size());
    int rank = 0;
    for (const auto& c : cands) {
        if (++rank > 12) break;
        const uint32_t first = c.first, count = c.count, draws = c.draws, versions = 1;
        const uint32_t version = c.version;
        const bool used = true;
        const auto latest = g_cb_latest.find(c.buffer);
        const size_t kept = latest == g_cb_latest.end() ? 0 : (latest->second.primary.bytes ? latest->second.primary.bytes->size() : 0);
        const uint32_t latest_version = latest == g_cb_latest.end() ? 0 : latest->second.primary.version;
        const CameraUpload* upload = nullptr;
        const char* source = "no_draw_snapshot";
        const auto snap = g_scene_uploads.find({c.buffer, c.slot, c.first, c.count});
        if (snap != g_scene_uploads.end() && snap->second.version == version && snap->second.bytes) {
            upload = &snap->second; source = "scene_draw";
        }
        const char* why = "-";
        const char* generic = "-";
        size_t bound_len = 0;
        std::vector<uint8_t> bound;
        if (upload) {
            size_t start = 0, length = 0;
            if (UploadRange(*upload, first, count, start, length))
                bound.assign(upload->bytes->data() + start, upload->bytes->data() + start + length);
            bound_len = bound.size();
            if (bound.empty()) why = "range_outside_kept_bytes";
            else {
                wwii::SceneCamera scene;
                wwii::DecodeSceneCamera(bound, static_cast<uint32_t>(bound.size()), w, h, scene, &why);
                const CameraPick p = FindCamera(bound, g_frame + 1);
                generic = p.found ? "vp+inverse" : "none";
            }
        }
        const int stage_ps = c.slot >= kPsCbBase;
        na::eventlog::Emit("native11_cam",
                           "cand=%d cb=%llx stage=%s slot=%d draws=%u versions=%u used=%d bind_first=%u bind_count=%u kept=%zu "
                           "version=%u latest_version=%u bytes=%s bound=%zu wwii_decode=%s generic=%s captured_first=%u",
                           rank, static_cast<unsigned long long>(c.buffer), stage_ps ? "ps" : "vs",
                           stage_ps ? c.slot - kPsCbBase : c.slot, draws, versions, used ? 1 : 0, first, count, kept, version,
                           latest_version, source, bound_len, why, generic, upload ? upload->first_constant : 0u);
        if (dump && !bound.empty()) {
            static const char hex[] = "0123456789abcdef";
            const size_t n = (std::min)(bound.size(), size_t(1024));
            std::string text(n * 2, '0');
            for (size_t i = 0; i < n; ++i) { text[2 * i] = hex[bound[i] >> 4]; text[2 * i + 1] = hex[bound[i] & 15]; }
            na::eventlog::Emit("native11_cam_bytes", "cand=%d cb=%llx len=%zu hex=%s", rank,
                               static_cast<unsigned long long>(c.buffer), bound.size(), text.c_str());
        }
    }
}

// Hands this frame's scene to the consumer, before the game's first read of the scene colour (or
// before the list / HUD draw that holds it). Under g_mu (`lk`), on the immediate context's thread,
// before the read is forwarded. g_mu is let go during the callback so the game's other threads never
// wait behind it; the facts handed over are copies or references we hold.
void Fire(ID3D11DeviceContext* immediate, std::unique_lock<std::mutex>& lk, Boundary boundary, bool verified_reader = false,
          ID3D11Resource* lit_color = nullptr, ViewUse lit_view = {}, uint64_t lit_proof = 0, ScaledReaderFacts scaled = {})
{
    ++g_stats.fired;
    SetSceneHanded(true); // whatever the outcome below, the game's scene draws for this frame are over
    if (!g_cb) return SetReason("No consumer is attached.");
    if (g_model.Current().screen_region && !verified_reader)
        return SetReason("Waiting for the active scene rectangle to reach the screen.");
    // CameraNow may learn the camera for this very callback.
    const Selection& sel = g_model.Current();
    if (boundary == Boundary::BeforeHud && IdOf(g_color_ref.Get()) != sel.color && immediate) {
        // A picture drawn straight onto the screen is not held between frames (see OnPresent). The
        // draw about to happen is into the picture, so the context has it bound: take it from there.
        Inside in;
        ComPtr<ID3D11RenderTargetView> bound;
        immediate->OMGetRenderTargets(1, &bound, nullptr);
        ComPtr<ID3D11Resource> picture;
        if (bound) bound->GetResource(&picture);
        if (picture && IdOf(picture.Get()) == sel.color) g_color_ref = picture;
    }
    if ((!lit_color && (!g_color_ref || IdOf(g_color_ref.Get()) != sel.color)) || !g_depth_ref || IdOf(g_depth_ref.Get()) != sel.depth) {
        ++g_stats.missed;
        return SetReason("The scene colour or depth is not held from a live game view yet.");
    }
    CameraPick cam;
    const CameraUpload* upload = nullptr;
    uint64_t version = 0;
    PoseCheck pose_check = PoseCheck::NoPacket;
    std::optional<CameraFacts::RelativePose> split_pose;
    // VARIANT: camera/optional
    // A camera is looked for exactly as before unless the host said never to (Off). What changes is
    // what a frame without one means: Required, a miss (as before); Optional and Off, the frame is
    // handed over all the same with camera_known = false, and nothing below reads a camera.
    const auto use = CameraTracking() ? CameraSetting() : CameraUse::Off;
    bool found = false;
    if (use != CameraUse::Off) {
        found = CameraNow(&cam, &upload, &version, pose_check, split_pose);
        CountPoseCheck(pose_check);
    }
    g_frame_fired = true;
    g_frame_found = found;
    g_frame_no_bytes = use != CameraUse::Off && !found && pose_check == PoseCheck::NoPacket;
    bool held = false;
    bool have_camera = found;
    if (use == CameraUse::Off) {
        g_held.valid = false;
        ++g_stats.camera_off;
    } else if (!found) {
        DiagnoseCameraMiss();
        // At most kHoldFrames in a row, and only for the same scene textures.
        if (!g_held.valid || g_frame <= g_held.frame || g_frame - g_held.frame > kHoldFrames ||
            g_held.color != sel.color || g_held.depth != sel.depth ||
            !HeldCameraUnreadable(g_held.cb, g_held.slot)) {
            g_held.valid = false;
            ++g_stats.camera_missing;
            if (use == CameraUse::Required)
                return SetReason("No supported camera was found in the scene's constant buffers.");
            ++g_stats.camera_absent; // handed over without one
        } else {
            held = true;
            have_camera = true;
            ++g_stats.camera_held;
            cam = g_held.cam;
            upload = &g_held.upload;
            version = g_held.version;
            split_pose = g_held.split;
            // The learned place stays learned: the next frame re-reads it first.
            g_camera = cam;
            g_model.SetCamera(g_held.cb, g_held.slot);
            g_camera_first = g_held.first;
            g_camera_num = g_held.num;
        }
    } else {
        ++g_stats.camera_found;
    }
    if (split_pose) ++g_stats.pose_split;
    const Selection cam_sel = g_model.Current(); // CameraNow may have just chosen (or changed) the camera buffer
    if (have_camera) {
        // WHY log: one line whenever the kind of camera in use changes (which rule, which place in
        // the block, with or without a position), so a run that alternates between cameras shows it.
        static uint64_t last_signature = ~uint64_t{0};
        static uint32_t logged = 0;
        const bool relative = camera::IsCameraRelative(cam.vp);
        const uint64_t signature = (uint64_t(static_cast<int>(pose_check)) << 48) ^ (uint64_t(cam.offset) << 8) ^
                                   (uint64_t(cam_sel.camera_slot + 1) << 2) ^ (relative ? 2u : 0u) ^ (held ? 1u : 0u);
        if (signature != last_signature && logged < 2000) {
            last_signature = signature;
            ++logged;
            camera::VpGeometry geo{};
            camera::DecomposeViewProjection(cam.vp, geo);
            na::eventlog::Emit("native11_pick",
                               "frame=%llu rule=%d held=%d slot=%d first=%u count=%u offset=%llu relative=%d position=%.1f,%.1f,%.1f exact=%d",
                               static_cast<unsigned long long>(g_frame), static_cast<int>(pose_check), held ? 1 : 0,
                               cam_sel.camera_slot, g_camera_first, g_camera_num, static_cast<unsigned long long>(cam.offset),
                               relative ? 1 : 0, geo.position[0], geo.position[1], geo.position[2],
                               upload->windowed && upload->first_constant == g_camera_first ? 1 : 0);
        }
    }
    if (have_camera && !held) {
        g_held.valid = true;
        g_held.cam = cam;
        g_held.upload = *upload;
        g_held.version = version;
        g_held.split = split_pose;
        g_held.cb = cam_sel.camera_cb;
        g_held.slot = cam_sel.camera_slot;
        g_held.first = g_camera_first;
        g_held.num = g_camera_num;
        g_held.color = sel.color;
        g_held.depth = sel.depth;
        g_held.frame = g_frame;
    }
    SceneFrame f;
    f.presentation_source = g_swap;
    f.frame = g_frame;
    const auto texture = [](TextureFacts& t, ID3D11Resource* r, const ResourceInfo& info, const ViewUse& view) {
        const float vx = g_scene_vp_x, vy = g_scene_vp_y, vw = g_scene_vp_w, vh = g_scene_vp_h;
        t.resource = IdOf(r);
        {
            Inside in;
            t.incarnation = IncarnationOf(r);
        }
        t.d3d11_resource = r;
        t.width = info.width;
        t.height = info.height;
        t.format = info.format;
        t.samples = info.samples;
        t.view_format = view.format ? view.format : info.format;
        t.mip = view.mip;
        t.array_slice = view.slice;
        const uint32_t mw = std::max(1u, info.width >> view.mip), mh = std::max(1u, info.height >> view.mip);
        t.active_x = vx > 0.f ? std::min(mw - 1, static_cast<uint32_t>(vx + 0.5f)) : 0;
        t.active_y = vy > 0.f ? std::min(mh - 1, static_cast<uint32_t>(vy + 0.5f)) : 0;
        t.active_w = vw >= 1.f ? std::min(mw - t.active_x, static_cast<uint32_t>(vw + 0.5f)) : mw - t.active_x;
        t.active_h = vh >= 1.f ? std::min(mh - t.active_y, static_cast<uint32_t>(vh + 0.5f)) : mh - t.active_y;
    };
    // Our own references for the callback's duration (nothing another thread does can free them).
    const ComPtr<ID3D11Resource> color(lit_color ? lit_color : g_color_ref.Get()), depth = g_depth_ref;
    texture(f.color, color.Get(), sel.color_info, lit_color ? lit_view : g_color_view);
    texture(f.depth, depth.Get(), sel.depth_info, g_depth_view);
    {
        // VARIANT: depth/direction-from-depth-writes
        // Which end of the depth is near. The evidence of the depth's own writes is weighed on every
        // frame; a frame with a camera keeps the camera's answer (as before), a frame without one
        // uses what the writes have proven, and is not handed over while nothing is proven.
        DepthVerdict verdict = DepthVerdict::NoWrites;
        const DepthCompare proven = g_depth_dir.Decide(f.depth.incarnation, &verdict);
        const DepthEvidence& ev = g_depth_dir.Last();
        {
            // WHY log: one line whenever the answer or the reason for it changes, with the evidence.
            static uint64_t last_signature = ~uint64_t{0};
            static uint32_t logged = 0;
            const uint64_t signature = (f.depth.incarnation << 8) ^ (uint64_t(static_cast<int>(proven)) << 4) ^
                                       uint64_t(static_cast<int>(verdict)) ^ (have_camera ? 0x80u : 0u) ^
                                       (have_camera && cam.reversed ? 0x40u : 0u);
            if (signature != last_signature && logged < 400) {
                last_signature = signature;
                ++logged;
                na::eventlog::Emit("native11_depth",
                                   "frame=%llu direction=%s verdict=%s writes{near_is_less=%u near_is_greater=%u equal=%u other=%u} "
                                   "clears=%u clear=%.6g clear_mixed=%d flips=%llu camera=%s",
                                   static_cast<unsigned long long>(g_frame),
                                   proven == DepthCompare::NearIsLess ? "near_is_less" : proven == DepthCompare::NearIsGreater ? "near_is_greater" : "unproven",
                                   DepthVerdictName(verdict), ev.less, ev.greater, ev.equal, ev.other, ev.clears, ev.clear,
                                   ev.clear_mixed ? 1 : 0, static_cast<unsigned long long>(g_depth_dir.Flips()),
                                   !have_camera ? "none" : cam.reversed ? "says_near_is_greater" : "says_near_is_less");
            }
        }
        if (have_camera) {
            f.reversed_depth = cam.reversed;
            if (proven != DepthCompare::None && (proven == DepthCompare::NearIsGreater) != cam.reversed)
                ++g_stats.depth_camera_disagrees;
        } else if (proven == DepthCompare::None) {
            ++g_stats.depth_unproven;
            ++g_stats.missed;
            static char why[192];
            std::snprintf(why, sizeof why,
                          "No camera, and the depth direction is not proven yet from the scene's own depth writes (%s: %u and %u writing draws either way, %u clears).",
                          DepthVerdictName(verdict), ev.less, ev.greater, ev.clears);
            return SetReason(why);
        } else {
            f.reversed_depth = proven == DepthCompare::NearIsGreater;
        }
    }
    f.camera_known = have_camera;
    std::vector<uint8_t> camera_bytes; // the callback runs without g_mu
    if (have_camera) {
        camera_bytes = *upload->bytes;
        f.camera.buffer = cam_sel.camera_cb;
        f.camera.vs_slot = cam_sel.camera_slot >= 0 && cam_sel.camera_slot < kPsCbBase ? cam_sel.camera_slot : -1;
        f.camera.ps_slot = cam_sel.camera_slot >= kPsCbBase ? cam_sel.camera_slot - kPsCbBase : -1;
        f.camera.first_constant = g_camera_first;
        f.camera.held = held;
        f.camera.num_constants = g_camera_num;
        f.camera.version = version;
        f.camera.bytes_first_constant = upload->first_constant;
        f.camera.split_pose = split_pose;
        if (cam.derived_inverse) f.camera.no_inverse_pose = cam.vp;
        f.camera.bytes = camera_bytes.data();
        f.camera.size = static_cast<uint32_t>(camera_bytes.size());
    }
    const bool vpj_route = g_vpj_route.load(std::memory_order_relaxed);
    if (vpj_route && g_vpj_mixed) {
        ++g_vpj_mixed_frames;
        ++g_stats.missed;
        return SetReason("Scene draws used different viewport jitter samples or raster footprints.");
    }
    // The raster sample is a fact about how the scene was drawn. Only the matrix route needs a camera
    // (it writes the sample into one); the viewport route never does.
    f.raster.viewport = vpj_route;
    f.raster.shader_depth_aligned = vpj_route && g_vpj_frame_shader_depth != 0;
    f.raster.still_meshes_moved = vpj_route ? g_vpj_frame_still_moved : 0;
    f.raster.qualified = vpj_route || (have_camera && f.camera.vs_slot >= 0 && cam.raster_jitter_qualified);
    f.raster.expanded = vpj_route && g_vpj_executed_expanded;
    f.raster.applied = vpj_route ? g_vpj_executed_sample != 0 : (f.raster.qualified && g_jitter_written);
    if (vpj_route) {
        static_assert(sizeof(f.raster.jitter_px) == sizeof(g_vpj_executed_sample));
        std::memcpy(f.raster.jitter_px, &g_vpj_executed_sample, sizeof(f.raster.jitter_px));
    } else if (f.raster.applied) {
        f.raster.jitter_px[0] = static_cast<float>(g_jitter.x_pixels);
        f.raster.jitter_px[1] = static_cast<float>(g_jitter.y_pixels);
    }
    // The same facts where the camera's readers have always found them.
    f.camera.raster_jitter_qualified = f.raster.qualified;
    f.camera.viewport_jitter = f.raster.viewport;
    f.camera.viewport_expanded = f.raster.expanded;
    f.camera.jitter_applied = f.raster.applied;
    std::memcpy(f.camera.jitter_px, f.raster.jitter_px, sizeof(f.camera.jitter_px));
    {
        // WHY log: what the consumer is told the jitter was, beside what the scene draws carried.
        // The first frames of a session, then a burst of sixteen about every ten seconds, so one
        // whole jitter cycle is on record each time.
        // After the first 1,500 lines, one a minute for the rest of the run (owed 2026-11-27: the 0f63301 run's
        // detail stopped at 19:55, three and a half hours before the end).
        static uint32_t logged = 0;
        static uint64_t burst_tick = 0, burst_left = 48, late_tick = 0;
        const uint64_t now = GetTickCount64();
        if (!burst_left && now - burst_tick >= 10000) burst_left = 16;
        const bool late = logged >= 1500 && now - late_tick >= 60000;
        if (((burst_left && logged < 1500) || late) && (f.camera.jitter_applied || vpj_route)) {
            if (late) late_tick = now;
            else if (!--burst_left) burst_tick = now;
            ++logged;
            float shift[2]{};
            std::memcpy(shift, &g_vpj_executed_sample, sizeof shift);
            na::eventlog::Emit("native11_jitter",
                               "frame=%llu route=%s told_px=%.4f,%.4f viewport_shift_px=%.4f,%.4f applied=%d shifted_draws=%u "
                               "unshifted_colour_draws=%u scene_draws=%u held=%d scene=%.0fx%.0f depth=%ux%u "
                               "shifted_vertices=%llu still_meshes{draws=%u untested=%u vertices=%llu} aligned_shader_depth_draws=%u "
                               "referee_still_meshes{asked=%d moved_draws=%u} variant=%s overlays_moved=%u "
                               "still_by{deferred=%u other_target=%u own_viewport=%u rule_off=%u stationary_source=%u tested_or_other=%u}",
                               static_cast<unsigned long long>(g_frame), vpj_route ? "viewport" : "matrix",
                               f.camera.jitter_px[0], f.camera.jitter_px[1], shift[0], shift[1], f.camera.jitter_applied ? 1 : 0,
                               g_vpj_frame_draws, g_vpj_frame_unshifted, g_model.SceneDrawsThisFrame(), held ? 1 : 0,
                               g_scene_vp_w, g_scene_vp_h, g_sel_depth_w.load(std::memory_order_relaxed),
                               g_sel_depth_h.load(std::memory_order_relaxed),
                               static_cast<unsigned long long>(g_vpj_frame_shifted_vertices), g_vpj_frame_still_meshes,
                               g_vpj_frame_still_untested, static_cast<unsigned long long>(g_vpj_frame_still_vertices),
                               g_vpj_frame_shader_depth, g_vpj_still_asked.load(std::memory_order_relaxed) ? 1 : 0,
                               g_vpj_frame_still_moved,
                               g_vpj_overlays.load(std::memory_order_relaxed) ? "jitter/scene-overlays" : "jitter/depth-tested-only",
                               g_vpj_frame_overlay_moved, g_vpj_frame_still_by[kStillDeferred], g_vpj_frame_still_by[kStillOtherTarget],
                               g_vpj_frame_still_by[kStillOwnViewport], g_vpj_frame_still_by[kStillRuleOff], g_vpj_frame_still_by[kStillStationarySource],
                               g_vpj_frame_still_by[kStillNone] + g_vpj_frame_still_by[kStillOther]);
        }
        uint64_t told = 0;
        std::memcpy(&told, f.camera.jitter_px, sizeof told);
        g_vpj_told.store(told, std::memory_order_relaxed);
    }
    if (have_camera) {
        // WHY log: the delivered camera's path. A camera read from one source moves smoothly; one
        // that hops between two sources steps back and forth.
        static bool have = false;
        static double last_pos[3]{}, last_step[3]{};
        camera::VpGeometry geo{};
        if (camera::DecomposeViewProjection(cam.vp, geo)) {
            if (have) {
                double step[3], len2 = 0, prev2 = 0, dot = 0;
                for (int j = 0; j < 3; ++j) {
                    step[j] = geo.position[j] - last_pos[j];
                    len2 += step[j] * step[j];
                    prev2 += last_step[j] * last_step[j];
                    dot += step[j] * last_step[j];
                }
                if (len2 > 2500.0) ++g_stats.camera_jumps;
                else if (len2 > 1e-6) {
                    ++g_stats.camera_steps;
                    if (prev2 > 1e-6 && dot < -0.5 * std::sqrt(len2 * prev2)) ++g_stats.camera_reversals;
                }
                for (int j = 0; j < 3; ++j) last_step[j] = len2 > 2500.0 ? 0.0 : step[j];
            }
            for (int j = 0; j < 3; ++j) last_pos[j] = geo.position[j];
            have = true;
        }
    }
    f.boundary = boundary;
    f.screen_proof = lit_color ? lit_proof : verified_reader ? g_screen.Proof() : 0;
    f.scaled_reader = scaled;
    if (scaled.target && f.screen_proof) {
        static uint64_t reported = 0;
        if (reported != f.screen_proof) {
            reported = f.screen_proof;
            na::eventlog::Emit("native11_scaled_reader",
                "frame=%llu proof=%llu scene=%ux%u target=%llu extent=%ux%u display_size=%u shader=%llu ps_slot=%u",
                (unsigned long long)f.frame, (unsigned long long)f.screen_proof, sel.color_info.width, sel.color_info.height,
                (unsigned long long)scaled.target, scaled.width, scaled.height, unsigned(scaled.display_size),
                (unsigned long long)scaled.shader, scaled.slot);
        }
    }
    if (g_trial_reader && g_screen_enabled.load(std::memory_order_relaxed)) {
        const auto lighting = LightingScope();
        const bool trial = g_placement.Matches(lighting);
        if (trial && !g_placement.AllowsStageRecovery()) {
            if (g_placement.UsesCandidate() == bool(lit_color)) f.trial_ticket = g_placement.Ticket(g_frame);
        } else if (bool(lit_color) == trial) {
            referee::StageRecovery::Scene scene{lit_color ? lighting.color : f.color.incarnation,
                f.depth.incarnation, f.color.active_w, f.color.active_h,
                f.color.mip, f.depth.mip, f.color.array_slice, f.depth.array_slice, f.color.active_x, f.color.active_y};
            if (lit_color) scene.generation = lighting.generation;
            g_recovery.SetScene(scene);
            f.trial_ticket = g_recovery.Ticket(g_frame);
            f.recovery_tool = g_recovery.ToolInUse();
            f.recovery_configuration = g_recovery.Configuration();
            if (lit_color) {
                f.recovery_region = g_placement.RecoveryRegion();
                f.recovery_floor = g_placement.RecoveryFloor();
            }
        }
    }
    f.scene_draws = g_model.SceneDrawsThisFrame();
    // The camera buffer is only named by pointer here (it may be gone), so its lifetime id is the one
    // tagged when the game last uploaded to it.
    // A buffer selected before its first candidate upload has no tag yet: its first tag is not a change.
    uint64_t cam_inc = 0;
    if (have_camera && cam_sel.camera_cb) {
        auto it = g_cb_incarnation.find(cam_sel.camera_cb);
        if (it != g_cb_incarnation.end()) cam_inc = it->second;
    }
    // The scene's own identity (colour and depth objects). A camera buffer that changes, appears or
    // goes is not a change of scene: it moves `stream` below, which only the camera's consumer reads.
    const uint64_t lit_generation = lit_color ? g_lit.generation : 0;
    const uint64_t history_color = lit_color ? 0 : f.color.incarnation;
    f.scene_changed = !g_scene_stream || g_last_lit_generation != lit_generation ||
                      history_color != g_scene_key[0] || f.depth.incarnation != g_scene_key[1];
    g_last_lit_generation = lit_generation;
    if (f.scene_changed) ++g_scene_stream;
    g_scene_key[0] = history_color;
    g_scene_key[1] = f.depth.incarnation;
    f.scene_stream = g_scene_stream;
    // VARIANT: camera11/all-uploads-when-buffers-rotate
    // A game that binds a different buffer object every frame gives a new camera buffer on every
    // frame; that says nothing about the scene, and counting it as a change of inputs made DLSS
    // drop its history each frame (p40940: reset on 30 of 30 captured frames). A real cut is
    // still found from the camera's own path.
    // A frame handed over without a camera neither changes nor forgets the camera's key: the same
    // camera buffer met again afterwards is not a change of inputs.
    const bool camera_changed = have_camera && !g_collect_all &&
        (cam_sel.camera_cb != g_stream_key[2] || (cam_inc && g_stream_key[3] && cam_inc != g_stream_key[3]));
    f.selection_changed = !g_stream || f.scene_changed || history_color != g_stream_key[0] || f.depth.incarnation != g_stream_key[1] ||
                          camera_changed;
    if (f.selection_changed) ++g_stream;
    g_stream_key[0] = history_color;
    g_stream_key[1] = f.depth.incarnation;
    if (have_camera) {
        g_stream_key[2] = cam_sel.camera_cb;
        if (cam_inc || f.selection_changed) g_stream_key[3] = cam_inc;
    }
    f.stream = g_stream;
    g_last_selection = cam_sel;
    std::shared_ptr<Ctx::WorldJob> world;
    auto world_ctx = g_ctx.find(immediate);
    if (!lit_color && world_ctx != g_ctx.end()) {
        auto* x = world_ctx->second.get();
        FinishWorld(immediate, x);
        x->world_closed_frame = g_frame;
        world = x->world;
        const bool ours = world && world->proof == f.screen_proof && world->color == f.color.resource && world->depth == f.depth.resource;
        if (ours) world->last_receipt = g_frame;
        // Delivered small: some draw went to the small pictures, or this is the Referee's comparison picture
        // (drawn at full size by plan, its draws all able to go small).
        const bool drawn_small = x->world_small_frame == g_frame;
        const bool comparison = !drawn_small && g_world_reference.load(std::memory_order_relaxed) && x->tally_frame == g_frame && x->tally.would;
        if (ours && (drawn_small || comparison) && world->ready.load(std::memory_order_acquire) && world->level == g_world_level.load()) {
            Inside own;
            // The full-size pictures are the newer ones (the comparison picture, or full-size draws after the
            // last small one): the small pictures are made from them, so DLSS is given the whole frame.
            if (x->world_frame != g_frame) world->raster.Seed(immediate);
            g_route.frame_small = drawn_small;
            g_route.frame_reference = comparison;
            g_route.frame_inputs = x->tally.inputs != 0;
            if (g_route.check.WantsSample()) {
                const uint64_t tag = ++g_route.probe_seq;
                if (world->probe.Note(immediate, tag)) g_route.frame_tag = tag;
            }
            f.native_output = f.color;
            f.world_level = world->level;
            f.world_draws = drawn_small ? x->world_draws : x->tally.would;
            const auto lower = [&](TextureFacts& facts, ID3D11Texture2D* low) {
                D3D11_TEXTURE2D_DESC desc{};
                low->GetDesc(&desc);
                facts.format = desc.Format;
                facts.resource = IdOf(low);
                facts.incarnation = IncarnationOf(low);
                facts.d3d11_resource = low;
                facts.active_w = facts.width = desc.Width;
                facts.active_h = facts.height = desc.Height;
            };
            lower(f.color, world->raster.Color());
            lower(f.depth, world->raster.Depth());
            for (int axis = 0; axis < 2; ++axis) {
                const float ratio = axis ? float(f.color.height) / f.native_output.height : float(f.color.width) / f.native_output.width;
                f.raster.jitter_px[axis] *= ratio;
                f.camera.jitter_px[axis] *= ratio;
            }
        }
        if (world && world->probe.Ready()) {
            // A sampled picture that has come back: the Referee's verdict on the frame drawn small.
            Inside own;
            uint64_t tag = 0;
            referee::WorldGrid cells;
            if (world->probe.Poll(immediate, tag, cells)) {
                using V = referee::WorldCheck::Verdict;
                const V verdict = g_route.check.Sampled(tag, cells);
                if (verdict != V::None) {
                    if (verdict == V::Passed || verdict == V::PassedUnseen) g_route.steady_frame = g_frame;
                    na::eventlog::Emit("native11_world_check", "frame=%llu verdict=%s inputs=%s differs=%.3f moved=%.3f looks=%u",
                        (unsigned long long)g_frame,
                        verdict == V::Passed ? "right" : verdict == V::PassedUnseen ? "let-through-unjudged(view moving)" :
                        verdict == V::Retry ? "look-again" : verdict == V::NextInputs ? "wrong:next-inputs" : "wrong:every-way",
                        referee::Name(g_route.check.Inputs()), g_route.check.LastFraction(), g_route.check.LastMotion(), g_route.check.Looks());
                }
            }
        }
    }
    const SceneCallback cb = g_cb;
    void* user = g_cb_user;
    lk.unlock();
    bool ok = false;
    {
        Inside in; // the consumer's D3D11 calls on the game's context are not observed
        ok = cb(user, immediate, f);
    }
    lk.lock();
    if (!ok && f.world_level && world) {
        // Restore already placed the spatial result. A rejecting evaluator cannot repeatedly
        // reduce the next frame; the ordinary renderer and the other resolution routes resume.
        if (++world->missed_evaluations >= 3) {
            world->refused.store(true, std::memory_order_release);
            world->ready.store(false, std::memory_order_release);
        }
    }
    if (ok && f.world_level && world) world->missed_evaluations = 0;
    if (!ok) ++g_stats.missed;
    else if (g_frame == f.frame) {
        g_frame_delivered = true;
        g_frame_delivered_lit = lit_proof != 0;
        if (g_screen_enabled.load(std::memory_order_relaxed)) {
            if (lit_proof) {
                g_lit.screen.NoteDelivery(true, f.scene_draws);
                const auto deliveries = g_lit.screen.Delivered();
                if (deliveries <= 2 || (deliveries & (deliveries - 1)) == 0)
                    na::eventlog::Emit("native11_lit_role", "frame=%llu role_deliveries=%llu scene_stream=%llu",
                        (unsigned long long)f.frame, (unsigned long long)deliveries, (unsigned long long)f.scene_stream);
            } else g_screen.NoteDelivery(verified_reader, f.scene_draws);
        }
    }
}

CameraUpload CaptureUpload(const void* data, size_t size, uint32_t first_constant, bool windowed)
{
    const auto* begin = static_cast<const uint8_t*>(data);
    return {0, std::make_shared<const std::vector<uint8_t>>(begin, begin + size), first_constant, windowed};
}

void SaveCameraUpload(Id resource, uint32_t version, CameraUploads uploads)
{
    uploads.primary.version = version;
    for (auto& upload : uploads.additional) upload.version = version;
    g_cb_latest[resource] = std::move(uploads);
}
// `ws` and `ps`: the slots the relative layout names for the world range and the projection range.
void RememberSplitDraw(const Event& e, int ws, int ps, const CameraUpload& world, const CameraUpload& projection)
{
    const BindingKey world_binding{e.cb[ws], ws, e.cb_first[ws], e.cb_count[ws]};
    const BindingKey projection_binding{e.cb[ps], ps, e.cb_first[ps], e.cb_count[ps]};
    if (world.bytes != g_split_draw.world || projection.bytes != g_split_draw.projection ||
        world_binding != g_split_draw.world_binding || projection_binding != g_split_draw.projection_binding) {
        g_split_draw = {world.bytes, projection.bytes, world_binding, projection_binding, nullptr};
        SplitKey key{};
        size_t start = 0, length = 0;
        if (!UploadRange(world, e.cb_first[ws], e.cb_count[ws], start, length) ||
            !UploadRange(projection, e.cb_first[ps], e.cb_count[ps], start, length)) return;
        if (!MakeSplitKey(g_rel_layout, world.bytes->data(), world.bytes->size(), e.cb_first[ws] - world.first_constant, e.cb_count[ws],
                          projection.bytes->data(), projection.bytes->size(), e.cb_first[ps] - projection.first_constant, e.cb_count[ps], key)) return;
        auto [it, inserted] = g_split_votes.try_emplace(key);
        if (inserted) {
            it->second.projection = {e.cb[ps], ps, e.cb_first[ps], e.cb_count[ps], projection.version, 0};
            it->second.projection_upload = projection;
            it->second.world_buffer = e.cb[ws];
        }
        g_split_draw.vote = &it->second;
    }
    if (g_split_draw.vote) ++g_split_draw.vote->draws;
}

// Pin the immutable upload actually used by each scene range. A later upload
// or a different binding in the same ring cannot erase this draw's evidence.
void RememberSceneUploads(const Event& e, const std::vector<ExactRange>* exact = nullptr)
{
    if (e.kind != Kind::Draw || !CameraTracking() || !g_model.IsSceneDraw(e)) return;
    TscScope timed(g_tsc_camera);
    // Ranges read at this very draw stand for the draw's own upload version.
    std::array<CameraUpload, kCbBindSlots> exact_upload;
    if (exact) {
        for (const auto& r : *exact) {
            if (r.slot < 0 || r.slot >= kCbBindSlots || !r.upload.bytes || !e.cb[r.slot] ||
                r.upload.first_constant != e.cb_first[r.slot] ||
                r.upload.bytes->size() != size_t(e.cb_count[r.slot]) * 16) continue;
            exact_upload[r.slot] = r.upload;
            exact_upload[r.slot].version = g_model.CbVersion(e.cb[r.slot]);
        }
    }
    const auto upload_for = [&](int slot) -> const CameraUpload* {
        if (exact_upload[slot].bytes) return &exact_upload[slot];
        return UploadForDraw(e.cb[slot], e.cb_first[slot], e.cb_count[slot]);
    };
    const CameraUpload* split[2]{};
    const int ws = static_cast<int>(g_rel_layout.world_binding), ps = static_cast<int>(g_rel_layout.projection_binding);
    // While the layout is being learned: this draw takes part if it binds the projection range found.
    const bool learning = g_rel_learner.armed();
    const int lp = learning ? static_cast<int>(g_rel_learner.projection_binding()) : -1;
    bool learn_draw = false;
    for (int slot = 0; slot < kCbBindSlots; ++slot) {
        if (!e.cb[slot]) continue;
        const auto* upload = upload_for(slot);
        if (!upload || !upload->bytes) continue;
        g_scene_uploads[{e.cb[slot], slot, e.cb_first[slot], e.cb_count[slot]}] = *upload;
        if (slot == ws) split[0] = upload;
        else if (slot == ps) split[1] = upload;
        if (slot == lp) {
            size_t start = 0, length = 0;
            learn_draw = UploadRange(*upload, e.cb_first[slot], e.cb_count[slot], start, length) &&
                         length == g_rel_learner.projection_size();
        }
    }
    if (split[0] && split[1]) RememberSplitDraw(e, ws, ps, *split[0], *split[1]);
    if (!learn_draw) return;
    for (int slot = 0; slot < kCbSlots; ++slot) { // vertex ranges: a world transform is a vertex input
        if (slot == lp || !e.cb[slot]) continue;
        const auto* upload = upload_for(slot);
        size_t start = 0, length = 0;
        if (!upload || !UploadRange(*upload, e.cb_first[slot], e.cb_count[slot], start, length)) continue;
        g_rel_learner.NoteDraw(static_cast<uint64_t>(slot), upload->bytes->data() + start, length);
    }
}
// Called in immediate draw order and while replaying executed command lists.
void NoteViewportDraw(const Event& e)
{
    if (!g_vpj_route.load(std::memory_order_relaxed) || e.kind != Kind::Draw) return;
    // WHY log: draws into the scene colour that kept the game's own viewport on a frame whose
    // depth-tested scene draws were shifted (full-screen and untested passes).
    if (!e.viewport_sample && g_vpj_executed_sample && e.rt0 && e.rt0 == g_sel_color.load(std::memory_order_relaxed)) {
        ++g_vpj_frame_unshifted;
        if (e.still_class == kStillFullScreen && g_vpj_sample.load(std::memory_order_relaxed)) ++g_vpj_frame_still_by[kStillFullScreen];
        if (g_left_name_frame.load(std::memory_order_relaxed) && g_left_named < 32) {
            ++g_left_named;
            const bool live = g_vpj_sample.load(std::memory_order_relaxed) != 0;
            na::eventlog::Emit("native11_left_draw",
                "frame=%llu class=%s vertices=%llu depth_test=%d depth_bound=%s rt_count=%u deferred_list=%d why=\"%s\"",
                static_cast<unsigned long long>(g_frame), !live ? "after_handover" : e.still_class ? StillClassName(e.still_class) : "tested_or_other",
                static_cast<unsigned long long>(e.vertices), e.depth_test ? 1 : 0,
                !e.depth ? "none" : e.depth == g_sel_depth.load(std::memory_order_relaxed) ? "scene" : "other", unsigned(e.rt_count),
                e.still_class == kStillDeferred ? 1 : 0,
                !live ? "drawn into the scene colour after the hand-over: not part of what DLSS resolves, left on the game's grid by design"
                : e.still_class == kStillFullScreen ? "a full-screen pass (the coverage trial tries it last)"
                : e.still_class ? "an untested scene mesh the overlay rule left on the game's grid (the coverage trial may move its class)"
                : "kept the game's viewport for another reason");
        }
    }
    if (e.viewport_sample) {
        g_vpj_frame_shader_depth += e.viewport_shader_depth ? 1u : 0u;
        g_vpj_frame_still_moved += e.viewport_still_mesh ? 1u : 0u;
        g_vpj_frame_overlay_moved += e.viewport_overlay ? 1u : 0u;
        g_vpj_frame_shifted_vertices += e.vertices;
    } else if (g_vpj_sample.load(std::memory_order_relaxed) && e.depth && e.vertices > kFullScreenPassVertices &&
               (e.depth == g_sel_depth.load(std::memory_order_relaxed) || e.depth == g_sel_depth2.load(std::memory_order_relaxed))) {
        // The sample is still live (the picture has not been handed over) and this mesh did not take it.
        ++g_vpj_frame_still_meshes;
        g_vpj_frame_still_untested += e.depth_test ? 0u : 1u;
        g_vpj_frame_still_vertices += e.vertices;
        ++g_vpj_frame_still_by[e.still_class < kStillClasses ? e.still_class : kStillOther];
    }
    if ((!e.depth_test && !e.viewport_shader_depth) || !g_model.IsSceneDraw(e)) return;
    if (!g_vpj_seen_draw) {
        g_vpj_seen_draw = true;
        g_vpj_executed_sample = e.viewport_sample;
        g_vpj_executed_expanded = e.viewport_expanded;
    } else if (g_vpj_executed_sample != e.viewport_sample || g_vpj_executed_expanded != e.viewport_expanded) {
        g_vpj_mixed = true;
    }
    if (e.viewport_sample) ++g_vpj_frame_draws;
}

// Depth direction evidence (depth_direction.hpp), in immediate draw order and while replaying
// executed command lists, under g_mu: writes into the selected depth and its clears. Draws after
// the scene was handed over are the HUD's and are not evidence about the scene.
void NoteDepthDirection(const Event& e)
{
    const Id depth = g_model.Current().depth;
    if (!depth) return;
    if (e.kind == Kind::Draw) {
        if (e.depth == depth && e.depth_func && !g_frame_fired) g_depth_dir.NoteWrite(e.depth_func);
    } else if (e.kind == Kind::ClearDepth && e.resource == depth && !g_frame_fired) {
        g_depth_dir.NoteClear(e.clear_depth);
    }
}

// ---------------------------------------------------------------- events
void StopViewportJitterAfterScene();
void TrackPlacementWrite(const Event& e, const Ctx* live = nullptr)
{
    if (e.kind == Kind::Draw && g_lit.Learned() && g_lit.Peer(e.rt0)) {
        auto& view = g_lit.written[e.rt0 == g_lit.members[1]];
        view = {};
        if (live && e.depth == g_lit.depth && !live->rt_view.mip && !live->rt_view.slice &&
            live->vp_n == 1 && live->vp_x == 0.f && live->vp_y == 0.f &&
            live->vp_w > 0.f && live->vp_h > 0.f) {
            view = {UINT(live->vp_w), UINT(live->vp_h), live->rt_view.format,
                    live->ds_view.format, live->ds_view.mip, live->ds_view.slice};
        }
    }
    // The lighting read has already been admitted with live, intact depth.
    // Clearing depth in a later pass cannot change the color it read.
    if (g_lit.observed_member &&
        ((e.kind == Kind::Draw && g_lit.Peer(e.rt0)) || (e.kind == Kind::ColorWrite && g_lit.Peer(e.resource)))) {
        g_lit.screen.Write(0);
        const auto index = g_lit.observed_member == g_lit.members[1];
        if (g_lit.observed_proven) g_lit.Revoke(g_lit.proofs[index], g_frame);
        g_lit.observed_member = g_lit.observed_write = 0;
    }
    if (!g_screen.Adaptive() || !g_screen_enabled.load(std::memory_order_relaxed)) return;
    const auto& scene = g_model.Current();
    if (!scene.valid()) return;
    if (e.kind == Kind::Draw && (e.rt0 == scene.color || g_model.IsSceneDraw(e))) {
        // A depth/blend variant of the same shader can occupy another pass.
        const uint64_t writer = e.placement_writer ? (e.placement_writer << 8) |
            (uint64_t(e.depth_test) << 7) | (uint64_t(e.depth_func) << 3) | std::min<uint8_t>(7, e.rt_count) : 0;
        g_screen.Write(writer);
    } else if ((e.kind == Kind::ColorWrite && e.resource == scene.color) ||
               (e.kind == Kind::ClearDepth && e.resource == scene.depth)) {
        g_screen.Write(0);
    }
}

void Emit(Ctx* x, ID3D11DeviceContext* c, const Event& e, CameraUploads uploads = {}, const ExactSet& exact = {},
          bool pixel_read = false)
{
    if (x->deferred) {
        if (x->events.size() >= kMaxListEvents) {
            x->events_truncated = true;
            return;
        }
        x->events.push_back(e);
        if (e.kind == Kind::Draw && (exact.get() != x->exact_recorded || !x->exact_recorded_any) &&
            (exact || x->exact_recorded)) {
            x->draw_bytes.emplace_back(x->events.size() - 1, exact);
            x->exact_recorded = exact.get();
            x->exact_recorded_any = true;
        }
        if (e.kind == Kind::CbUpdate && uploads.primary.bytes)
            x->cb_bytes.emplace_back(x->events.size() - 1, std::move(uploads));
        return;
    }
    std::unique_lock<std::mutex> l(g_mu);
    const auto& scene = g_model.Current();
    // VARIANT: point/native11-screen-verified
    const bool screen_enabled = g_screen_enabled.load(std::memory_order_relaxed);
    const bool screen_verified = pixel_read && screen_enabled && g_screen.Verified(scene.color, scene.depth);
    // Let discovery run alongside normal delivery until a proven reader can
    // take its place. A search with no suitable reader must not claim the frame.
    // VARIANT: point/native11-adaptive-reader
    const bool adaptive_read = pixel_read && screen_enabled && g_screen.ReservesPixelRead() &&
        scene.color != g_backbuffer && scene.color_info.samples == 1;
    const auto reserve_lit = [&] { return screen_enabled && g_lit.steer && g_lit.Predicted(g_model); };
    if (e.kind == Kind::Read && !screen_verified && !adaptive_read && g_model.ShouldFireBeforeRead(e.resource) && !reserve_lit()) {
        if (pixel_read && screen_enabled) {
            g_screen.SetScene(scene.color, scene.depth);
            g_screen.NoteOrdinalPixel(g_model.SceneDrawsThisFrame());
        }
        g_model.MarkFired();
        Fire(c, l, Boundary::BeforeRead);
        StopViewportJitterAfterScene();
    } else if (e.kind == Kind::Draw && g_model.ShouldFireBeforeDraw(e) && !reserve_lit()) {
        g_model.MarkFired();
        Fire(c, l, Boundary::BeforeHud);
        StopViewportJitterAfterScene();
    }
    TrackPlacementWrite(e, x);
    const uint32_t v = g_model.Feed(e);
    RememberSceneUploads(e, exact.get());
    NoteViewportDraw(e);
    NoteDepthDirection(e);
    if (e.kind == Kind::CbUpdate && uploads.primary.bytes)
        SaveCameraUpload(e.resource, v, std::move(uploads));
}

// Viewport jitter, before a game draw is forwarded: a depth-tested draw into the scene depth gets
// the game's viewports moved by this frame's sample; any other draw gets the game's own back.
// The picture has been handed over: what the game draws after that (marks and HUD that still test
// the scene depth) is not part of what DLSS resolves, so it must not move with the sample.
// Bound offset copies go back to the game's own viewports at the next draw.
void StopViewportJitterAfterScene()
{
    g_coarse_open.store(false, std::memory_order_relaxed); // the scene is handed over: nothing after it is shaded in blocks
    if (g_vpj_sample.load(std::memory_order_relaxed)) {
        g_vpj_sample.store(0, std::memory_order_release);
        g_vpj_stopped_after_scene.fetch_add(1, std::memory_order_relaxed);
    }
}
// A scene can write a depth-like texture as colour and test against that
// same raster later in its mesh shaders, with hardware depth testing off.
// Move these readers only when their producer actually took this sample.
void VpjBeforeDraw(ID3D11DeviceContext* c, Ctx* x, uint64_t vertices = 0)
{
    const uint64_t sample = g_vpj_sample.load(std::memory_order_acquire);
    const bool on = sample != 0;
    x->vpj_shader_depth = x->vpj_still_mesh = false;
    x->vpj_want = false;
    if (!on && !x->vpj_applied) return;
    Id scene_depth = g_sel_depth.load(std::memory_order_relaxed);
    // VARIANT: scene/after-layers-second-depth
    // The scene goes on with a second depth buffer: draws tested against it take the same sample.
    if (const Id second = g_sel_depth2.load(std::memory_order_relaxed); second && x->depth == second && scene_depth) scene_depth = second;
    // Every depth-tested draw into the scene depth moves, whatever colour target is bound: a depth
    // prepass (no colour target) and the colour passes tested against it must rasterise at the same
    // sub-pixel position, or the later passes fail their depth test over whole surfaces
    // (p42728: triangles dropped to black, differently for each sample).
    const auto& layer = x->raster_depth_layer;
    if (on && !x->deferred && !x->depth_test && vertices > kFullScreenPassVertices &&
        x->material_layer_target && x->rt_count > 1 && scene_depth && x->depth == scene_depth &&
        layer.depth == scene_depth && layer.sample == sample && layer.pixel_reads.any() &&
        layer.frame == g_exact_frame.load(std::memory_order_relaxed)) {
        x->vpj_shader_depth = x->vp_n == 1 && x->vp_x == 0.f && x->vp_y == 0.f &&
            x->vp_w == float(x->target_width) && x->vp_h == float(x->target_height) &&
            x->target_width == g_sel_depth_w.load(std::memory_order_relaxed) &&
            x->target_height == g_sel_depth_h.load(std::memory_order_relaxed) &&
            layer.min_depth == x->vps[0].MinDepth && layer.max_depth == x->vps[0].MaxDepth;
    }
    // VARIANT: referee/sample-locked-dropout
    // The Referee saw pixels drop out on some samples: while it asks, a scene mesh no rule above
    // moved takes the full sample as well (any target; never a full-screen pass). Immediate context
    // only: there the sample is cleared the moment the picture is handed over, so nothing after the
    // scene moves; a deferred recording has no such order until it is executed.
    if (on && !x->deferred && !x->vpj_shader_depth && g_vpj_still_asked.load(std::memory_order_relaxed)) {
        neural_alias::StillMesh mesh;
        mesh.asked = mesh.active = true;
        mesh.tests_depth = x->depth_test;
        mesh.scene_depth_bound = scene_depth && x->depth == scene_depth;
        mesh.mesh = vertices > kFullScreenPassVertices;
        mesh.scene_viewport = x->vp_n == 1 && x->vp_x == 0.f && x->vp_y == 0.f &&
            x->vp_w == float(g_sel_depth_w.load(std::memory_order_relaxed)) &&
            x->vp_h == float(g_sel_depth_h.load(std::memory_order_relaxed));
        x->vpj_still_mesh = neural_alias::StillMeshTakesJitter(mesh);
    }
    // VARIANT: jitter/scene-overlays: An untested mesh drawn into the scene's own colour picture through
    // the scene viewport, while the sample is in force (immediate context: the sample is cleared at the
    // hand-over), is scene content laid over the terrain: it takes the sample like the depth-tested draws.
    x->vpj_overlay = false;
    x->vpj_still_class = kStillNone;
    if (on && !x->depth_test && !x->vpj_shader_depth && !x->vpj_still_mesh && scene_depth && x->depth == scene_depth &&
        vertices > kFullScreenPassVertices) {
        neural_alias::SceneOverlay o;
        o.enabled = g_vpj_overlays.load(std::memory_order_relaxed);
        o.active = !x->deferred;
        o.scene_depth_bound = true;
        o.mesh = true;
        const Id colour = g_sel_color.load(std::memory_order_relaxed);
        o.scene_colour = colour && x->rt0 == colour;
        o.reads_stationary_layer = x->stationary.frame == g_exact_frame.load(std::memory_order_relaxed) && x->stationary.pixel_reads.any();
        o.scene_viewport = x->vp_n == 1 && x->vp_x == 0.f && x->vp_y == 0.f &&
            x->vp_w == float(g_sel_depth_w.load(std::memory_order_relaxed)) &&
            x->vp_h == float(g_sel_depth_h.load(std::memory_order_relaxed));
        x->vpj_overlay = neural_alias::SceneOverlayTakesJitter(o);
        if (!x->vpj_overlay)
            x->vpj_still_class = x->deferred ? kStillDeferred : !o.scene_colour ? kStillOtherTarget : !o.scene_viewport ? kStillOwnViewport
                               : !o.enabled ? kStillRuleOff : o.reads_stationary_layer ? kStillStationarySource : kStillOther;
    }
    // A full-screen pass into the scene colour while the sample is in force (before the hand-over): left still by
    // design; the Referee's coverage trial may try it, last.
    bool force = false;
    if (on && !x->depth_test && !x->vpj_shader_depth && !x->vpj_still_mesh && !x->vpj_overlay && vertices && vertices <= kFullScreenPassVertices) {
        const Id colour = g_sel_color.load(std::memory_order_relaxed);
        if (colour && x->rt0 == colour) x->vpj_still_class = kStillFullScreen;
    }
    // VARIANT: referee/coverage-trial: the classes the Referee moves onto the sample, draw signature by draw signature.
    if (on && x->vpj_still_class != kStillNone) {
        const uint32_t mask = g_cov_mask.load(std::memory_order_acquire);
        const int cls = CoverageClassOf(x->vpj_still_class);
        if (cls >= 0 && (mask >> unsigned(cls)) & 1u) {
            const uint64_t sig = CoverageSignature(x->pixel_shader, vertices, x->vpj_still_class);
            std::lock_guard<std::mutex> lock(g_cov_m);
            if (g_cov_still_sigs.count(sig)) {
                g_cov_kept_still.fetch_add(1, std::memory_order_relaxed);
            } else {
                if (g_cov_trial.load(std::memory_order_relaxed) == cls) g_cov_trial_sigs.insert(sig);
                g_cov_moved.fetch_add(1, std::memory_order_relaxed);
                if (x->vpj_still_class == kStillFullScreen) force = true;
                else x->vpj_overlay = true;
                x->vpj_still_class = kStillNone;
            }
        }
    }
    const bool tests_scene_depth = x->depth_test || x->vpj_shader_depth || x->vpj_still_mesh || x->vpj_overlay;
    const bool scene = on && scene_depth && x->depth == scene_depth && tests_scene_depth;
    if (scene && !x->vp_n) g_vpj_unknown.fetch_add(1, std::memory_order_relaxed);
    const bool want = WantsViewportJitter(on, x->depth, scene_depth, tests_scene_depth, x->vp_n) ||
                      (on && x->vp_n && (force || (x->vpj_overlay && x->depth != scene_depth)));
    x->vpj_want = want;
    const UINT n = x->vp_n;
    if (want) {
        g_vpj_draws.fetch_add(1, std::memory_order_relaxed);
    } else if (!x->vpj_applied) {
        return;
    }
    if (want && x->vpj_applied && x->vpj_sample == sample) return;
    float offset[2]{};
    static_assert(sizeof(offset) == sizeof(sample));
    std::memcpy(offset, &sample, sizeof(offset));
    D3D11_VIEWPORT v[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    if (want) {
        ViewportRect r[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        for (UINT i = 0; i < n; ++i) r[i] = {x->vps[i].TopLeftX, x->vps[i].TopLeftY, x->vps[i].Width, x->vps[i].Height};
        x->vpj_expanded = JitterViewports(r, n, offset[0], offset[1],
            g_sel_depth_w.load(std::memory_order_relaxed), g_sel_depth_h.load(std::memory_order_relaxed));
        for (UINT i = 0; i < n; ++i) {
            v[i] = x->vps[i];
            v[i].TopLeftX = r[i].x;
            v[i].TopLeftY = r[i].y;
            v[i].Width = r[i].w;
            v[i].Height = r[i].h;
        }
        x->vpj_applied = true;
        x->vpj_sample = sample;
        g_vpj_sets.fetch_add(1, std::memory_order_relaxed);
    } else {
        x->vpj_applied = false;
        if (!n) return; // nothing known to hand back; the game sets its own before relying on them
        for (UINT i = 0; i < n; ++i) v[i] = x->vps[i];
        g_vpj_restores.fetch_add(1, std::memory_order_relaxed);
    }
    O(c, x).rs_vp(c, n, v); // the original: not a change of the game's viewports
}

// The ranges of large no-overwrite buffers this draw binds, read now. A range is read once it has
// stayed bound for a second scene draw (per-object constants change every draw and are left alone;
// the constants a whole view shares stay), and again after a discard or a new frame.
ExactSet ExactForDraw(ID3D11DeviceContext* c, Ctx* x)
{
    if (x->large.empty()) return {};
    const uint32_t frame = g_exact_frame.load(std::memory_order_relaxed);
    bool changed = false;
    int want[kCbBindSlots];
    int wanted = 0;
    for (int s = 0; s < kCbBindSlots; ++s) {
        Ctx::ExactSlot& k = x->exact[s];
        const Id id = x->cb[s];
        const uint32_t first = x->cb_first[s], count = x->cb_num[s];
        const auto it = id ? x->large.find(id) : x->large.end();
        if (it == x->large.end() || !it->second.no_overwrite || !count || count > kExactMaxConstants ||
            first > UINT16_MAX || (uint64_t(first) + count) * 16 > it->second.size || x->mapped.count(id)) {
            if (k.id) { changed = changed || k.bytes; k = {}; }
            continue;
        }
        if (k.id == id && k.first == first && k.count == count && k.epoch == it->second.epoch && k.frame == frame) {
            if (k.bytes) continue;
            if (++k.repeats >= 1) want[wanted++] = s;
            continue;
        }
        changed = changed || k.bytes;
        k = {};
        k.id = id; k.first = first; k.count = count; k.epoch = it->second.epoch; k.frame = frame;
    }
    // One mapping per buffer per draw serves every slot that waits on it.
    for (int i = 0; i < wanted; ++i) {
        Ctx::ExactSlot& k = x->exact[want[i]];
        if (k.bytes) continue;
        auto* buffer = reinterpret_cast<ID3D11Buffer*>(k.id); // bound on this context now, so alive
        D3D11_MAPPED_SUBRESOURCE m{};
        HRESULT hr = E_FAIL;
        {
            Inside in;
            // The object at this address must still be the buffer the game mapped (same kind and size).
            D3D11_BUFFER_DESC d{};
            buffer->GetDesc(&d);
            const auto known = x->large.find(k.id);
            if (known != x->large.end() && d.Usage == D3D11_USAGE_DYNAMIC && (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) &&
                d.ByteWidth == known->second.size)
                hr = O(c, x).map(c, buffer, 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m);
        }
        if (FAILED(hr) || !m.pData) {
            g_exact_failed.fetch_add(1, std::memory_order_relaxed);
            x->large.erase(k.id); // this context cannot map it this way now; the kept windows remain
            k = {};
            continue;
        }
        const auto* base = static_cast<const uint8_t*>(m.pData);
        for (int j = i; j < wanted; ++j) {
            Ctx::ExactSlot& o = x->exact[want[j]];
            if (o.id != k.id || o.bytes) continue;
            o.bytes = std::make_shared<const std::vector<uint8_t>>(base + size_t(o.first) * 16,
                                                                 base + (size_t(o.first) + o.count) * 16);
            g_exact_reads.fetch_add(1, std::memory_order_relaxed);
            changed = true;
        }
        {
            Inside in;
            O(c, x).unmap(c, buffer, 0);
        }
    }
    if (changed) {
        auto set = std::make_shared<std::vector<ExactRange>>();
        for (int s = 0; s < kCbBindSlots; ++s) {
            const Ctx::ExactSlot& k = x->exact[s];
            if (k.bytes) set->push_back({s, CameraUpload{0, k.bytes, k.first, true}});
        }
        if (set->empty()) x->exact_set.reset(); else x->exact_set = std::move(set);
    }
    if (x->exact_set) g_exact_draws.fetch_add(1, std::memory_order_relaxed);
    return x->exact_set;
}

// Coarse shading before a game draw (SetSceneCoarseShading): the draws that take the scene's sample are
// the draws shaded in blocks, one rule with the DX12 route (observe/producers.cpp); with no sample this
// frame, the depth-tested draws into the scene depth until the scene is handed over.
void CoarseApply(ID3D11DeviceContext* c, Ctx* x, uint32_t rate)
{
    Inside in;
    ID3D11Device* dev = g_device_raw.load(std::memory_order_acquire);
    if (coarse::Apply(c, dev, rate, x->vp_n, g_sel_depth_w.load(std::memory_order_relaxed), g_sel_depth_h.load(std::memory_order_relaxed))) {
        x->coarse_rate = rate;
        (rate ? g_coarse_sets : g_coarse_restores).fetch_add(1, std::memory_order_relaxed);
    } else {
        g_coarse_failures.fetch_add(1, std::memory_order_relaxed);
        if (!rate) x->coarse_rate = 0; // nothing more can be done about it
    }
}
void CoarseBeforeDraw(ID3D11DeviceContext* c, Ctx* x)
{
    const uint32_t rate = g_coarse.load(std::memory_order_relaxed);
    if (!rate && !x->coarse_rate) return;
    bool want = false;
    if (rate && g_coarse_open.load(std::memory_order_relaxed)) {
        if (g_vpj_sample.load(std::memory_order_relaxed)) want = x->vpj_want;
        else {
            const Id scene_depth = g_sel_depth.load(std::memory_order_relaxed);
            want = scene_depth && x->depth == scene_depth && x->depth_test && x->vp_n;
        }
    }
    const uint32_t r = want ? rate : 0;
    if (r) g_coarse_draws.fetch_add(1, std::memory_order_relaxed);
    if (r != x->coarse_rate) CoarseApply(c, x, r);
}

Ctx* DrawEvent(ID3D11DeviceContext* c, uint64_t vertices, bool indirect)
{
    Ctx* x = Get(c);
    if (!x) return nullptr;
    VpjBeforeDraw(c, x, vertices);
    CoarseBeforeDraw(c, x);
    const auto refresh_samplers = [&] {
        const uint64_t policy = SamplerPolicyKey();
        if (x->sampler_policy != policy) {
            RefreshSamplers(c);
            x->sampler_policy = policy;
        }
        // Material textures published or withdrawn since this context last bound: re-bind what it holds.
        const uint64_t textures = TexturePolicyKey();
        if (x->texture_policy != textures) {
            RefreshTextures(c, x->ps_shadow, O(c, x).ps_srv);
            x->texture_policy = textures;
        }
    };
    refresh_samplers();
    Event e;
    e.kind = Kind::Draw;
    e.rt0 = x->rt0;
    e.rt_count = x->rt_count;
    e.depth = x->depth;
    e.depth_test = x->depth_test;
    e.depth_func = x->depth_read_only ? 0 : x->depth_func;
    e.vertices = vertices;
    e.indirect = indirect;
    e.viewport_sample = x->vpj_applied ? x->vpj_sample : 0;
    e.viewport_expanded = x->vpj_applied && x->vpj_expanded;
    e.viewport_shader_depth = x->vpj_shader_depth;
    e.viewport_still_mesh = x->vpj_still_mesh && x->vpj_applied;
    e.viewport_overlay = x->vpj_overlay && x->vpj_applied;
    e.still_class = x->vpj_still_class;
    const bool camera_tracking = CameraTracking();
    if (camera_tracking) {
        TscScope timed(g_tsc_camera);
        std::memcpy(e.cb, x->cb, sizeof(e.cb));
        for (int i = 0; i < kCbBindSlots; ++i) {
            e.cb_first[i] = static_cast<uint16_t>(x->cb_first[i]);
            e.cb_count[i] = static_cast<uint16_t>(x->cb_num[i]);
        }
    }
    const bool picture_draw = e.rt0 && e.rt0 == g_sel_color.load(std::memory_order_relaxed);
    const bool layer_draw = e.depth_test && e.rt0 && e.rt0 == g_sel_layer.load(std::memory_order_relaxed) &&
                            e.depth == g_sel_depth.load(std::memory_order_relaxed);
    if ((picture_draw || layer_draw) && g_adaptive_scene.load(std::memory_order_relaxed)) {
        if (!x->pixel_shader_known) {
            Inside in;
            ComPtr<ID3D11PixelShader> shader;
            c->PSGetShader(&shader, nullptr, nullptr);
            x->pixel_shader = shader.Get();
            x->pixel_shader_known = true;
        }
        if (!x->writer_shader && x->pixel_shader) {
            Inside in;
            x->writer_shader = IncarnationOf(x->pixel_shader);
        }
        e.placement_writer = x->writer_shader;
    }
    if (!x->deferred && (picture_draw || layer_draw)) {
        // Scene-draw facts the frame model does not keep: viewport extent and the camera's offset binding.
        std::lock_guard<std::mutex> l(g_mu);
        const Selection& s = g_model.Current();
        if (e.depth == s.depth) {
            g_scene_vp_x = x->vp_x;
            g_scene_vp_y = x->vp_y;
            g_scene_vp_w = x->vp_w;
            g_scene_vp_h = x->vp_h;
        }
    }
    ExactSet exact;
    if ((picture_draw || layer_draw) && camera_tracking) { TscScope timed(g_tsc_camera); exact = ExactForDraw(c, x); } // camera bytes: not read when no camera is looked for
    Emit(x, c, e, {}, exact);
    if (!x->deferred) refresh_samplers(); // this draw may be the first after the scene (the HUD rules hand over inside Emit)
    return x;
}
// The scene colour first met through a read, before any bind as a render target: its view as read.
ViewUse SrvUse(ID3D11ShaderResourceView* v)
{
    D3D11_SHADER_RESOURCE_VIEW_DESC d{};
    v->GetDesc(&d);
    ViewUse u;
    u.format = d.Format;
    u.texture2d = d.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D;
    if (u.texture2d) u.mip = d.Texture2D.MostDetailedMip;
    else if (d.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY) { u.mip = d.Texture2DArray.MostDetailedMip; u.slice = d.Texture2DArray.FirstArraySlice; }
    else if (d.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY) u.slice = d.Texture2DMSArray.FirstArraySlice;
    return u;
}
// Reads of the scene colour, and of the few other targets the frame model watches (a material
// layer's read is what shows that the scene goes on in another target).
bool Watched(Id r, Id color)
{
    if (!r) return false;
    if (r == color) return true;
    for (const auto& w : g_watch)
        if (w.load(std::memory_order_relaxed) == r) return true;
    return false;
}
void ReadEvents(ID3D11DeviceContext* c, DepthStage stage, UINT start, UINT n, ID3D11ShaderResourceView* const* views)
{
    Ctx* x = Get(c);
    if (!x) return;
    auto& depth_slots = x->depth_reads.slots[static_cast<size_t>(stage)];
    const bool track_depth = !x->deferred && g_depth_reads_enabled.load(std::memory_order_relaxed);
    const bool screen = !x->deferred && stage == DepthStage::Pixel && g_screen_enabled.load(std::memory_order_relaxed);
    if (!track_depth && !views && !screen) return;
    const Id depth = track_depth ? g_sel_depth.load(std::memory_order_relaxed) : 0;
    const Id color = g_sel_color.load(std::memory_order_relaxed);
    const Id peer = screen ? g_lit_peer.load(std::memory_order_relaxed) : 0;
    if (screen && (x->screen_color != color || x->screen_peer != peer)) {
        x->screen_color = color;
        x->screen_peer = peer;
        x->screen_slots.reset();
    }
    if (!depth && !color && !g_watch[0].load(std::memory_order_relaxed)) return;
    Id sent[kWatch + 1] = {};
    size_t sent_n = 0;
    for (UINT i = 0; i < n; ++i) {
        Id r = 0;
        if (views && views[i]) {
            Inside in;
            r = ResourceOfView(views[i]);
        }
        if (screen && start < x->screen_slots.size() && i < x->screen_slots.size() - start)
            x->screen_slots.set(start + i, r && (r == color || r == peer));
        if (screen && x->post.resource && start < x->post.slots.size() && i < x->post.slots.size() - start)
            x->post.slots.set(start + i, r == x->post.resource);
        if (track_depth && start < kDepthSlots && i < kDepthSlots - start)
            depth_slots.set(start + i, r && r == depth);
        if (stage == DepthStage::Pixel && !x->deferred && x->stationary.frame == g_exact_frame.load(std::memory_order_relaxed) &&
            start < kDepthSlots && i < kDepthSlots - start) {
            bool reads = false;
            if (r && StationaryHas(x, r)) {
                const auto use = SrvUse(views[i]);
                reads = use.texture2d && use.format == DXGI_FORMAT_R32_FLOAT && !use.mip;
            }
            x->stationary.pixel_reads.set(start + i, reads);
        }
        if (stage == DepthStage::Pixel && x->raster_depth_layer.resource && start < kDepthSlots && i < kDepthSlots - start) {
            bool reads_layer = false;
            if (r == x->raster_depth_layer.resource) {
                const auto use = SrvUse(views[i]);
                reads_layer = use.texture2d && use.format == DXGI_FORMAT_R32_FLOAT && !use.mip;
            }
            x->raster_depth_layer.pixel_reads.set(start + i, reads_layer);
        }
        if (!r || sent_n == kWatch + 1) continue;
        if (!Watched(r, color)) continue;
        bool again = false;
        for (size_t k = 0; k < sent_n; ++k) again = again || sent[k] == r;
        if (again) continue; // one read of a target per bind call is enough
        if (r == color && !x->deferred) {
            std::lock_guard<std::mutex> l(g_mu);
            if (IdOf(g_color_ref.Get()) != r) {
                g_color_ref.Reset();
                Inside in;
                views[i]->GetResource(&g_color_ref);
                // The view of the picture before this one must not describe it; the next bind as a
                // render target says how the game writes it.
                g_color_view = SrvUse(views[i]);
            }
        }
        Event e;
        e.kind = Kind::Read;
        e.resource = r;
        Emit(x, c, e, {}, {}, stage == DepthStage::Pixel);
        if (sent_n < kWatch + 1) sent[sent_n++] = r;
        if (sent_n == kWatch + 1 && !track_depth && !screen) return;
    }
}
void ReadResource(ID3D11DeviceContext* c, ID3D11Resource* src)
{
    Ctx* x = Get(c);
    if (!x || !src) return;
    const Id r = IdOf(src);
    if (!Watched(r, g_sel_color.load(std::memory_order_relaxed))) return;
    Event e;
    e.kind = Kind::Read;
    e.resource = r;
    Emit(x, c, e);
}
void CbUpdate(ID3D11DeviceContext* c, ID3D11Resource* r, const void* bytes, size_t size, const CaptureWindows* windows = nullptr)
{
    Ctx* x = Get(c);
    if (!x) return;
    Event e;
    e.kind = Kind::CbUpdate;
    e.resource = IdOf(r);
    const bool keep = IsCandidate(e.resource) && bytes && size;
    if (keep) {
        uint64_t inc = 0;
        {
            Inside in;
            inc = IncarnationOf(r);
        }
        std::lock_guard<std::mutex> l(g_mu);
        if (g_cb_incarnation.size() > 256) g_cb_incarnation.clear();
        g_cb_incarnation[e.resource] = inc;
    }
    CameraUploads uploads;
    if (keep && windows) {
        const auto* data = static_cast<const uint8_t*>(bytes);
        for (size_t i = 0; i < windows->count; ++i) {
            const auto window = windows->ranges[i];
            if (window.begin >= size) continue;
            CameraUpload upload = CaptureUpload(data + window.begin, std::min<size_t>(window.size, size - window.begin),
                                                window.begin / 16, true);
            if (!uploads.primary.bytes) uploads.primary = std::move(upload);
            else uploads.additional.push_back(std::move(upload));
        }
    } else if (keep) {
        uploads.primary = CaptureUpload(bytes, std::min(size, kMaxCbBytes), 0, false);
    }
    Emit(x, c, e, std::move(uploads));
}
bool IsConstantBuffer(ID3D11Resource* r, UINT* size)
{
    D3D11_RESOURCE_DIMENSION dim{};
    r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return false;
    D3D11_BUFFER_DESC d{};
    static_cast<ID3D11Buffer*>(r)->GetDesc(&d);
    if (!(d.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) return false;
    *size = d.ByteWidth;
    return true;
}

// Jitter (B3): rewrites the main camera's ViewProjection (and its inverse) in the game's own upload,
// before the GPU sees it. Only the camera whose aspect matches the scene (not shadow or reflection
// cameras). The sample comes from SetNextJitter (the backend's sequence).
bool JitterUpload(void* bytes, size_t size, Id cb)
{
    if (!bytes) return false;
    CameraPick learned;
    uint32_t w = 0, h = 0, first = 0, count = 0;
    neural_alias::JitterSample sample{0, 0};
    {
        std::lock_guard<std::mutex> l(g_mu);
        if (g_vpj_route.load(std::memory_order_relaxed)) return false; // jitter goes through the viewports; never both
        if (!CameraTracking()) return false;
        const auto& selection = g_model.Current();
        // A PS-only pose can reconstruct camera motion. Rewriting it cannot
        // move the scene's raster samples and must not be reported as jitter.
        if (!g_jitter_active || cb != selection.camera_cb || !g_camera.found || !g_camera.raster_jitter_qualified ||
            selection.camera_slot < 0 || selection.camera_slot >= kPsCbBase) return false;
        first = g_camera_first;
        count = g_camera_num;
        learned = g_camera;
        w = g_model.Current().color_info.width;
        h = g_model.Current().color_info.height;
        sample = g_jitter;
    }
    if (!w || !h) return false;
    size = std::min(size, kMaxCbBytes);
    const auto v = BoundCameraBytes(static_cast<const uint8_t*>(bytes), size, first, count);
    CameraPick now;
    wwii::SceneCamera relative;
    if (learned.camera_relative) {
        if (!wwii::DecodeSceneCamera(v, static_cast<uint32_t>(v.size()), w, h, relative) ||
            !relative.raster_jitter_qualified) return false;
        now = learned;
        now.offset = relative.vp_offset;
        now.layout = na::camera::Layout::ColumnMajor;
        now.vp = relative.vp;
    } else if (!CheckLearned(v, learned, &now)) {
        return false;
    }
    na::camera::VpGeometry geo{};
    if (!na::camera::DecomposeViewProjection(now.vp, geo)) return false;
    const double aspect = geo.sy / geo.sx, screen = double(w) / h;
    if (std::fabs(aspect - screen) > 0.01 * screen) return false; // not the main camera
    const auto jittered = neural_alias::jitter_projection(now.vp, sample, w, h);
    auto* dst = static_cast<uint8_t*>(bytes) + size_t(first) * 16;
    size = v.size();
    if (now.camera_relative) {
        // Only the witnessed current projection(s). Leave the previous camera,
        // depth reconstruction matrices and world origin as the game wrote them.
        StoreMatrix(dst, size, now.offset, now.layout, jittered);
        if (relative.vp_duplicate) StoreMatrix(dst, size, relative.vp_duplicate, now.layout, jittered);
    } else {
        na::camera::Matrix inv{};
        if (!na::camera::Invert(jittered, inv)) return false;
        StoreMatrix(dst, size, now.offset, now.layout, jittered);
        const int64_t io = static_cast<int64_t>(now.offset) + now.inverse_delta;
        if (!now.derived_inverse && io >= 0) StoreMatrix(dst, size, static_cast<uint64_t>(io), now.layout, inv);
    }
    std::lock_guard<std::mutex> l(g_mu);
    ++g_stats.jitter_written;
    g_jitter_written = true;
    return true;
}

// ---------------------------------------------------------------- detours
// Constant-buffer binds of one stage (`base` = 0 for VS, kPsCbBase for PS).
void NoteCbs(Ctx* x, int base, UINT start, UINT n, ID3D11Buffer* const* b, const UINT* first, const UINT* num)
{
    if (!x) return;
    for (UINT i = 0; i < n && start + i < static_cast<UINT>(kCbSlots); ++i) {
        const int s = base + static_cast<int>(start + i);
        x->cb[s] = b ? IdOf(b[i]) : 0;
        x->cb_first[s] = b && first ? first[i] : 0;
        x->cb_num[s] = b && first && num ? num[i] : 0;
    }
}
void STDMETHODCALLTYPE VSSetConstantBuffers_d(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11Buffer* const* b)
{
    Ctx* x = Get(c);
    NoteCbs(x, 0, start, n, b, nullptr, nullptr);
    O(c, x).vs_cb(c, start, n, b);
}
void STDMETHODCALLTYPE VSSetConstantBuffers1_d(ID3D11DeviceContext1* c, UINT start, UINT n, ID3D11Buffer* const* b, const UINT* first,
                                               const UINT* num)
{
    Ctx* x = Get(c);
    NoteCbs(x, 0, start, n, b, first, num);
    O(c, x).vs_cb1(c, start, n, b, first, num);
}
void STDMETHODCALLTYPE PSSetConstantBuffers_d(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11Buffer* const* b)
{
    Ctx* x = Get(c);
    NoteCbs(x, kPsCbBase, start, n, b, nullptr, nullptr);
    O(c, x).ps_cb(c, start, n, b);
}
void STDMETHODCALLTYPE PSSetConstantBuffers1_d(ID3D11DeviceContext1* c, UINT start, UINT n, ID3D11Buffer* const* b, const UINT* first,
                                               const UINT* num)
{
    Ctx* x = Get(c);
    NoteCbs(x, kPsCbBase, start, n, b, first, num);
    O(c, x).ps_cb1(c, start, n, b, first, num);
}
void STDMETHODCALLTYPE PSSetShader_d(ID3D11DeviceContext* c, ID3D11PixelShader* shader,
                                     ID3D11ClassInstance* const* instances, UINT count)
{
    Ctx* x = Get(c);
    if (x && (!x->pixel_shader_known || x->pixel_shader != shader)) {
        x->pixel_shader = shader;
        x->pixel_shader_known = true;
        x->writer_shader = 0;
        x->world_inputs.scanned = false;
    }
    O(c, x).ps_shader(c, shader, instances, count);
}

void STDMETHODCALLTYPE PSSetShaderResources_d(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11ShaderResourceView* const* v)
{
    ReadEvents(c, DepthStage::Pixel, start, n, v);
    // Material textures: a published replacement is bound in place of the game's view (textures.hpp).
    ID3D11ShaderResourceView* swapped[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT];
    Ctx* x = Get(c); // null inside our own work and on other devices' contexts: bound as given
    if (x) x->world_inputs.scanned = false;
    const auto* bind = x ? SwapTextures(c, x->ps_shadow, start, n, v, swapped) : v;
    O(c, x).ps_srv(c, start, n, bind);
    if (bind != v) EndTextureSwap(n, v, bind);
}
void STDMETHODCALLTYPE CSSetShaderResources_d(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11ShaderResourceView* const* v)
{
    ReadEvents(c, DepthStage::Compute, start, n, v);
    O(c, t_inside ? nullptr : Get(c)).cs_srv(c, start, n, v);
}
void STDMETHODCALLTYPE VSSetShaderResources_d(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11ShaderResourceView* const* v)
{
    ReadEvents(c, DepthStage::Vertex, start, n, v);
    O(c, t_inside ? nullptr : Get(c)).vs_srv(c, start, n, v);
}
Id DepthReadsSource(Ctx* state)
{
    return state && !state->deferred && g_depth_reads_enabled.load(std::memory_order_relaxed) &&
        g_vpj_route.load(std::memory_order_relaxed) ? g_sel_depth.load(std::memory_order_relaxed) : 0;
}

bool CountsRasterWork(ID3D11Asynchronous* object)
{
    ComPtr<ID3D11Query> query;
    if (!object || FAILED(object->QueryInterface(IID_PPV_ARGS(&query)))) return false;
    D3D11_QUERY_DESC desc{};
    query->GetDesc(&desc);
    return desc.Query == D3D11_QUERY_OCCLUSION || desc.Query == D3D11_QUERY_OCCLUSION_PREDICATE ||
           desc.Query == D3D11_QUERY_PIPELINE_STATISTICS;
}

void STDMETHODCALLTYPE Begin_d(ID3D11DeviceContext* c, ID3D11Asynchronous* object)
{
    Ctx* x = Get(c);
    if (x && !x->deferred && CountsRasterWork(object)) { FinishWorld(c, x); ++x->raster_queries; }
    O(c, x).begin(c, object);
}

void STDMETHODCALLTYPE End_d(ID3D11DeviceContext* c, ID3D11Asynchronous* object)
{
    Ctx* x = Get(c);
    O(c, x).end(c, object);
    if (x && x->raster_queries && CountsRasterWork(object)) --x->raster_queries;
}

class DepthBindings {
public:
    DepthBindings(ID3D11DeviceContext* context, Ctx* state, Id depth, bool compute)
        : context_(context), state_(state)
    {
        if (!depth || (state->depth == depth && !state->depth_read_only)) return;
        auto& reads = state->depth_reads;
        reads.Start(g_exact_frame.load(std::memory_order_relaxed), depth);
        Inside own;
        if (compute) Bind(DepthStage::Compute);
        else {
            Bind(DepthStage::Pixel);
            Bind(DepthStage::Vertex);
        }
    }
    ~DepthBindings()
    {
        if (!count_) return;
        Inside own;
        while (count_) {
            const auto& saved = slots_[--count_];
            Set(saved.stage, saved.slot, saved.view);
            saved.view->Release();
        }
    }
    DepthBindings(const DepthBindings&) = delete;
    DepthBindings& operator=(const DepthBindings&) = delete;

private:
    void Set(DepthStage stage, UINT slot, ID3D11ShaderResourceView* view)
    {
        const auto& original = O(context_, state_);
        const auto set = stage == DepthStage::Pixel ? original.ps_srv :
                         stage == DepthStage::Vertex ? original.vs_srv : original.cs_srv;
        set(context_, slot, 1, &view);
    }
    void Bind(DepthStage stage)
    {
        auto& reads = state_->depth_reads;
        const auto& mask = reads.slots[static_cast<size_t>(stage)];
        if (mask.none()) return;
        uint64_t shader = 0;
        for (UINT slot = 0; slot < kDepthSlots; ++slot) {
            if (!mask.test(slot)) continue;
            ComPtr<ID3D11ShaderResourceView> original;
            if (stage == DepthStage::Pixel) context_->PSGetShaderResources(slot, 1, &original);
            else if (stage == DepthStage::Vertex) context_->VSGetShaderResources(slot, 1, &original);
            else context_->CSGetShaderResources(slot, 1, &original);
            if (!original) continue;
            D3D11_SHADER_RESOURCE_VIEW_DESC view{};
            original->GetDesc(&view);
            if (view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || view.Texture2D.MostDetailedMip != 0 ||
                (view.Texture2D.MipLevels != 1 && view.Texture2D.MipLevels != UINT(-1)) ||
                (view.Format != DXGI_FORMAT_R32_FLOAT && view.Format != DXGI_FORMAT_R16_UNORM &&
                 view.Format != DXGI_FORMAT_R24_UNORM_X8_TYPELESS && view.Format != DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS))
                continue;
            ComPtr<ID3D11Resource> resource;
            original->GetResource(&resource);
            if (IdOf(resource.Get()) != reads.resource) continue;
            if (!shader) {
                if (stage == DepthStage::Pixel) {
                    ComPtr<ID3D11PixelShader> current;
                    context_->PSGetShader(&current, nullptr, nullptr);
                    shader = IncarnationOf(current.Get());
                } else if (stage == DepthStage::Vertex) {
                    ComPtr<ID3D11VertexShader> current;
                    context_->VSGetShader(&current, nullptr, nullptr);
                    shader = IncarnationOf(current.Get());
                } else {
                    ComPtr<ID3D11ComputeShader> current;
                    context_->CSGetShader(&current, nullptr, nullptr);
                    shader = IncarnationOf(current.Get());
                }
                if (!shader) return;
            }
            const uint64_t key = (shader << 11) | (uint64_t(stage) << 9) |
                (uint64_t(state_->depth == reads.resource) << 8) | slot;
            if (!reads.Refresh(context_, resource.Get(), key, state_->raster_queries != 0)) continue;
            if (auto* twin = reads.twin->ViewFor(original.Get())) {
                Set(stage, slot, twin);
                slots_[count_++] = {stage, slot, original.Detach()};
            }
        }
    }
    struct Slot { DepthStage stage; UINT slot; ID3D11ShaderResourceView* view; };
    ID3D11DeviceContext* context_;
    Ctx* state_;
    Slot slots_[kDepthSlots * 2];
    size_t count_ = 0;
};

template<class Forward>
void ForwardDepthRead(ID3D11DeviceContext* context, Ctx* state, bool dispatch, Forward forward)
{
    const Id depth = DepthReadsSource(state);
    if (!depth) { forward(); return; }
    auto& reads = state->depth_reads;
    const bool has_binding = dispatch ? reads.slots[static_cast<size_t>(DepthStage::Compute)].any() :
        (reads.slots[static_cast<size_t>(DepthStage::Pixel)] | reads.slots[static_cast<size_t>(DepthStage::Vertex)]).any();
    // A draw moved onto the sample reads the scene's own depth: both are on the same grid.
    if (has_binding && (dispatch || !(state->vpj_shader_depth || state->vpj_still_mesh)) && (state->depth != depth || state->depth_read_only)) {
        DepthBindings bindings(context, state, depth, dispatch);
        forward();
    } else forward();
    if (!dispatch && state->depth == depth && !state->depth_read_only && state->depth_func) {
        reads.Start(g_exact_frame.load(std::memory_order_relaxed), depth);
        const auto width = g_sel_depth_w.load(std::memory_order_relaxed);
        const auto height = g_sel_depth_h.load(std::memory_order_relaxed);
        const auto valid = [](float dimension, UINT target) {
            return target && dimension >= float(target) * .98f && dimension <= float(target) &&
                dimension == float(static_cast<UINT>(dimension));
        };
        const bool picture = state->vp_n == 1 && state->vp_x == 0.f && state->vp_y == 0.f &&
            valid(state->vp_w, width) && valid(state->vp_h, height);
        reads.Write(state->vpj_applied ? state->vpj_sample : 0, state->vpj_applied && state->vpj_expanded,
                    picture ? static_cast<UINT>(state->vp_w) : 0, picture ? static_cast<UINT>(state->vp_h) : 0);
    }
}

ScreenCheck::Mark CheckScreenReader(ID3D11DeviceContext* c, Ctx* x)
{
    if (!x || x->deferred || x->rt_count != 1 || !x->rt0 || (x->depth && x->depth_test) ||
        (x->screen_slots.none() && x->post.slots.none()) || x->vp_n != 1 ||
        !g_screen_enabled.load(std::memory_order_relaxed)) return {};

    std::unique_lock<std::mutex> lock(g_mu);
    const auto& scene = g_model.Current();
    const auto draws = g_model.SceneDrawsThisFrame();
    ResourceInfo output{};
    if (!g_model.InfoOf(x->rt0, output) || output.samples != 1 || scene.color_info.samples != 1) return {};
    const bool native_output = g_model.IsDisplayExtent(output);
    const bool same_size_post = output.width == scene.color_info.width && output.height == scene.color_info.height;
    const bool scale_search = g_discover_scale.load(std::memory_order_relaxed);
    const bool scaled = scale_search && (g_model.IsDisplayExtent(scene.color_info) || g_model.IsReducedPicture(scene.color_info)) &&
        (native_output || same_size_post) &&
        !x->vp_x && !x->vp_y && x->vp_w == float(output.width) && x->vp_h == float(output.height);
    const bool inspect = scaled || scene.screen_region || g_screen.MayInspect();
    const Id lit = scene.screen_region ? 0 : g_lit.Predicted(g_model);
    const uint64_t lit_write = lit ? g_model.LitWrite(g_lit.Peer(lit), lit) : 0;
    const bool missed_ordinal = !scene.screen_region && !g_model.FiredThisFrame() && g_model.LearnedPointKind() == 1 &&
        !g_screen.Adaptive() && !g_screen.Verified(scene.color, scene.depth);
    if ((!inspect && !missed_ordinal && !lit_write) || !draws || !scene.valid() || scene.color != x->screen_color || x->rt0 == scene.color ||
        scene.color == g_backbuffer || !g_swap || x->raster_queries ||
        IdOf(g_color_ref.Get()) != scene.color || g_color_view.mip || g_color_view.slice) return {};
    // A compositor may draw the active picture into an alignment-padded target.
    // Qualify that reader by its on-screen pixels, with a bounded extent check.
    const auto close_extent = [](uint32_t a, uint32_t b) {
        return a && b && uint64_t((std::min)(a, b)) * 100 >= uint64_t((std::max)(a, b)) * 98;
    };
    const auto& source_size = scene.color_info;
    const bool direct_region = scene.screen_region && x->rt0 == g_backbuffer &&
        referee::PaddedLayouts(source_size.width, source_size.height, output.width, output.height) != 0;
    if (!scaled && !direct_region && (!close_extent(output.width, source_size.width) ||
                           !close_extent(output.height, source_size.height))) return {};
    const auto covers_picture = [](float viewport, uint32_t extent) {
        // Alignment padding can leave a few unused rows in a full-size texture.
        // The exact presented pixels still qualify the reader.
        return viewport <= float(extent) && viewport >= .98f * float(extent);
    };
    const bool full_view = !x->vp_x && !x->vp_y &&
        covers_picture(x->vp_w, output.width) && covers_picture(x->vp_h, output.height);
    const bool centered_view = direct_region && x->vp_w == float(source_size.width) &&
        x->vp_h == float(source_size.height) &&
        std::fabs(2.f * x->vp_x + x->vp_w - float(output.width)) <= 1.f &&
        std::fabs(2.f * x->vp_y + x->vp_h - float(output.height)) <= 1.f;
    if (!full_view && !centered_view) return {};

    Inside own;
    ComPtr<ID3D11Predicate> predicate;
    BOOL predicate_value = FALSE;
    c->GetPredication(&predicate, &predicate_value);
    if (predicate) return {};
    if (scaled) {
        ComPtr<ID3D11RasterizerState> raster;
        c->RSGetState(&raster);
        D3D11_RASTERIZER_DESC rs{};
        if (raster) raster->GetDesc(&rs);
        if (rs.ScissorEnable) {
            UINT count = 1;
            D3D11_RECT clip{};
            c->RSGetScissorRects(&count, &clip);
            if (!count || clip.left > 0 || clip.top > 0 ||
                clip.right < LONG(output.width) || clip.bottom < LONG(output.height)) return {};
        }
    }
    ComPtr<ID3D11PixelShader> shader;
    c->PSGetShader(&shader, nullptr, nullptr);
    if (!shader) return {};
    ComPtr<ID3D11RenderTargetView> target;
    c->OMGetRenderTargets(1, &target, nullptr);
    if (!target) return {};
    D3D11_RENDER_TARGET_VIEW_DESC target_view{};
    target->GetDesc(&target_view);
    if (target_view.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || target_view.Texture2D.MipSlice) return {};
    ComPtr<ID3D11Resource> destination;
    target->GetResource(&destination);
    if (IdOf(destination.Get()) != x->rt0) return {};
    const bool follow_post = scaled && native_output && x->post.resource && x->post.frame == g_frame &&
        x->post.root_proof && x->post.root_proof == g_screen.Proof();
    for (UINT slot = 0; slot < x->screen_slots.size(); ++slot) {
        if (!x->screen_slots.test(slot) && !(follow_post && x->post.slots.test(slot))) continue;
        ComPtr<ID3D11ShaderResourceView> view;
        c->PSGetShaderResources(slot, 1, &view); // the runtime may have automatically unbound it
        if (!view) continue;
        const Id bound = ResourceOfView(view.Get());
        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
        view->GetDesc(&desc);
        if (desc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || desc.Texture2D.MostDetailedMip) continue;
        if (follow_post && bound == x->post.resource) {
            ComPtr<ID3D11Resource> source;
            view->GetResource(&source);
            if (IncarnationOf(source.Get()) != x->post.incarnation) continue;
            g_post_screen.SetScene(x->post.incarnation, x->post.root_proof);
            ScreenCheck::Reader end{IncarnationOf(destination.Get()), IncarnationOf(shader.Get()), slot, uint32_t(target_view.Format)};
            end.scaled = true;
            if (g_post_screen.Matches(end)) {
                g_post_screen.NoteDelivery(true, draws);
                static uint64_t reported = 0;
                if (reported != g_post_screen.Proof()) {
                    reported = g_post_screen.Proof();
                    na::eventlog::Emit("native11_scaled_compositor",
                        "frame=%llu upstream_proof=%llu proof=%llu input=%llu target=%llu extent=%ux%u shader=%llu ps_slot=%u",
                        (unsigned long long)g_frame, (unsigned long long)x->post.root_proof, (unsigned long long)reported,
                        (unsigned long long)x->post.incarnation, (unsigned long long)end.output, output.width, output.height,
                        (unsigned long long)end.shader, slot);
                }
            } else if (!g_screen.AwaitingSamples() && !g_lit.screen.AwaitingSamples()) {
                ComPtr<ID3D11Texture2D> texture;
                if (SUCCEEDED(source.As(&texture))) {
                    auto mark = g_post_screen.Begin(c, end, texture.Get(), desc.Format, g_frame, draws);
                    if (mark.source) return mark;
                }
            }
            continue;
        }
        if (bound != scene.color && bound != lit) continue;
        if (lit_write && bound == lit && !g_lit.Peer(x->rt0) && IdOf(g_depth_ref.Get()) == g_lit.depth &&
            g_color_view.format == static_cast<uint32_t>(desc.Format) &&
            x->vp_w == g_scene_vp_w && x->vp_h == g_scene_vp_h && !g_scene_vp_x && !g_scene_vp_y) {
            ComPtr<ID3D11Resource> source;
            view->GetResource(&source);
            ComPtr<ID3D11Texture2D> texture;
            if (FAILED(source.As(&texture))) continue;
            D3D11_TEXTURE2D_DESC td{};
            texture->GetDesc(&td);
            if (td.MipLevels != 1 || td.ArraySize != 1 || td.SampleDesc.Count != 1) continue;
            ScreenCheck::Reader reader{IncarnationOf(destination.Get()), IncarnationOf(shader.Get()), slot, uint32_t(target_view.Format),
                                       IncarnationOf(source.Get()), IncarnationOf(g_depth_ref.Get())};
            if (!g_lit.ObserveView(lit == g_lit.members[1], reader.color, reader.depth,
                    {UINT(x->vp_w), UINT(x->vp_h), UINT(desc.Format), g_depth_view.format, g_depth_view.mip, g_depth_view.slice}))
                continue;
            auto& probe = g_lit.screen;
            if (probe.Select(reader, true)) {
                const auto member = lit == g_lit.members[1];
                static unsigned logged_readers = 0;
                if (g_lit.proofs[member] != probe.Proof() && logged_readers < 32) {
                    ++logged_readers;
                    na::eventlog::Emit("native11_lit_role",
                        "frame=%llu draw_event=%llu proof=%llu source=%03x destination=%03x shader=%llu slot=%u",
                        (unsigned long long)g_frame, (unsigned long long)g_model.FrameEventCount(),
                        (unsigned long long)probe.Proof(), unsigned((lit >> 4) & 0xfff),
                        unsigned((x->rt0 >> 4) & 0xfff), (unsigned long long)reader.shader, reader.slot);
                }
                g_lit.proofs[member] = probe.Proof();
                g_lit.observed_member = lit;
                g_lit.observed_write = lit_write;
                g_lit.observed_proven = true;
                if (g_lit.steer && !g_model.FiredThisFrame()) {
                    g_model.MarkFired();
                    Fire(c, lock, Boundary::BeforeRead, false, source.Get(), SrvUse(view.Get()), probe.Proof());
                    StopViewportJitterAfterScene();
                    g_lit.observed_proven = g_frame_delivered;
                }
                if (g_lit.steer) return {};
            } else if (!g_screen.AwaitingSamples() && !g_post_screen.AwaitingSamples()) {
                auto mark = probe.Begin(c, reader, texture.Get(), desc.Format, g_frame, draws);
                if (mark.source) {
                    g_lit.observed_member = lit;
                    g_lit.observed_write = lit_write;
                    g_lit.observed_proven = false;
                    return mark;
                }
            }
        }
        if (bound != scene.color || (!inspect && !missed_ordinal) || (lit && g_lit.steer)) continue;
        g_screen.SetScene(scene.color, scene.depth);
        if (missed_ordinal) g_screen.NoteActualPixel(draws);
        if (!inspect) return {};
        ScreenCheck::Reader reader{IncarnationOf(destination.Get()), IncarnationOf(shader.Get()), slot, uint32_t(target_view.Format)};
        reader.scaled = scaled;
        const ScaledReaderFacts scaled_facts = scaled ?
            ScaledReaderFacts{reader.output, reader.shader, slot, output.width, output.height, native_output} : ScaledReaderFacts{};
        if (g_screen.Adaptive()) {
            reader.color = IncarnationOf(g_color_ref.Get());
            if (IdOf(g_depth_ref.Get()) != scene.depth) return {};
            reader.depth = IncarnationOf(g_depth_ref.Get());
            if (!g_screen.Learn(reader, output.width, output.height)) return {};
            if (!g_screen.Select(reader, !g_model.FiredThisFrame())) {
                ComPtr<ID3D11Texture2D> source;
                if (FAILED(g_color_ref.As(&source))) return {};
                if (!g_lit.screen.AwaitingSamples() && !g_post_screen.AwaitingSamples())
                    return g_screen.Begin(c, reader, source.Get(), DXGI_FORMAT(g_color_view.format), g_frame, draws);
                continue;
            }
        } else if (g_screen.Verified(scene.color, scene.depth)) {
            if (!g_screen.Matches(reader)) continue;
        } else {
            ComPtr<ID3D11Texture2D> source;
            if (FAILED(g_color_ref.As(&source))) return {};
            if (!g_lit.screen.AwaitingSamples() && !g_post_screen.AwaitingSamples())
                return g_screen.Begin(c, reader, source.Get(), DXGI_FORMAT(g_color_view.format), g_frame, draws);
            continue;
        }
        if (scaled && same_size_post && x->rt0 != g_backbuffer && g_screen.Proof()) {
            ComPtr<ID3D11Texture2D> intermediate;
            D3D11_TEXTURE2D_DESC image{};
            if (SUCCEEDED(destination.As(&intermediate))) intermediate->GetDesc(&image);
            if ((image.BindFlags & D3D11_BIND_SHADER_RESOURCE) && !(image.BindFlags & D3D11_BIND_UNORDERED_ACCESS) &&
                image.Usage == D3D11_USAGE_DEFAULT && !image.CPUAccessFlags && image.ArraySize == 1 && image.MipLevels == 1) {
                x->post = {};
                x->post.resource = x->rt0;
                x->post.incarnation = reader.output;
                x->post.root_proof = g_screen.Proof();
                x->post.frame = g_frame;
            }
        }
        if (g_model.FiredThisFrame()) return {};
        g_model.MarkFired();
        Fire(c, lock, Boundary::BeforeRead, true, nullptr, {}, 0, scaled_facts);
        StopViewportJitterAfterScene();
        return {};
    }
    return {};
}

// ---- The world drawn small -------------------------------------------------------------------------
// The game's own world draws are sent to smaller colour and depth pictures with a scaled viewport
// (world_raster.hpp); DLSS brings the picture back to full size before the game's reader draws it.
//
// How games draw a world, and what each needs here:
//   * colour and depth together, one draw after another: the plain case;
//   * a depth pass first (no colour bound), then colour testing against it: both go to the small pictures,
//     or the colour pass would test small depth against full-size depth;
//   * colour with no depth bound (sky, fog, a full-screen layer): goes to the small colour;
//   * depth tested but not written while a shader reads it (soft edges): the small depth through a
//     read-only view;
//   * a draw that reads a full-size picture of the frame (water bending what is behind it, soft edges,
//     screen-space shading): the picture is first made small into the corner of a full-size copy when the
//     shader finds its place by pixel position, or given as it is when it addresses it 0..1; the Referee
//     looks at the result and changes the way when the picture comes out wrong (referee_world_check.hpp);
//   * a part viewport or a scissor rectangle: scaled with the picture;
//   * a few draws that cannot go small (several targets, a visibility query, a predicated draw) between
//     the others: the full-size pictures are brought up to date, the draw runs as the game issued it, and
//     the small pictures are refilled from the result. More than kWorldBreaks of those in a frame and the
//     world keeps its full size, with the reason counted.
// Every world draw is counted (small, would-be small, held and why) so the log, RESULTS and the menu can
// say WHY the smaller picture is not being delivered.
constexpr uint32_t kWorldBreaks = 6;
// Each time another picture's draw sits between the world's draws, the full-size pictures are brought up
// to date first (a whole-screen copy). More of those in a frame than this and drawing small would cost
// what it saves: the world keeps its full size.
constexpr uint32_t kWorldRestores = 8;
enum : uint32_t { kCreatePixelShader = 15 }; // ID3D11Device

const char* WorldHoldName(WorldHold why)
{
    static const char* const names[] = {"none", "preparing", "helper-program", "no-render-size", "reader", "on-screen", "other-size", "picture-kind",
        "no-world-draws", "recorded-lists", "several-targets", "viewports", "uavs", "predicated", "full-size-input", "interleaved", "picture-check", "evaluator"};
    static_assert(sizeof names / sizeof names[0] == size_t(WorldHold::kCount), "one name per reason");
    return size_t(why) < size_t(WorldHold::kCount) ? names[size_t(why)] : "?";
}

// What a pixel shader reads, from its own reflection data when the game creates it: which texture slots it
// really uses (a view left bound by an earlier pass is not a read), and whether it uses its pixel position.
struct PsFacts { uint64_t slots[2] = {}; uint8_t known = 0, position = 0; };
const GUID kPsFactsGuid = {0x6b0a61d4, 0x3f0e, 0x4c7f, {0x9a, 0x51, 0x2e, 0x7c, 0x10, 0x6d, 0x88, 0x31}};
using CreatePixelShaderFn = HRESULT (STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);
CreatePixelShaderFn g_create_ps = nullptr;
std::atomic<uint64_t> g_ps_known{0}, g_ps_unknown{0};
HRESULT STDMETHODCALLTYPE CreatePixelShader_d(ID3D11Device* dev, const void* code, SIZE_T size, ID3D11ClassLinkage* linkage, ID3D11PixelShader** out)
{
    const HRESULT hr = g_create_ps(dev, code, size, linkage, out);
    if (FAILED(hr) || !out || !*out || !code || !size || t_inside) return hr;
    PsFacts facts;
    // The program itself is read first (it works on shaders whose description data was stripped); what the
    // description data says is added to it.
    na::dxbc::PixelInputs read;
    if (na::dxbc::ReadPixelInputs(code, size, read)) {
        facts.known = 1;
        facts.slots[0] = read.slots[0]; facts.slots[1] = read.slots[1];
        facts.position = read.position ? 1 : 0;
    }
    ComPtr<ID3D11ShaderReflection> reflection;
    D3D11_SHADER_DESC d{};
    if (SUCCEEDED(D3DReflect(code, size, IID_PPV_ARGS(&reflection))) && SUCCEEDED(reflection->GetDesc(&d))) {
        for (UINT i = 0; i < d.BoundResources; ++i) {
            D3D11_SHADER_INPUT_BIND_DESC b{};
            if (FAILED(reflection->GetResourceBindingDesc(i, &b)) || b.Type != D3D_SIT_TEXTURE) continue;
            for (UINT k = 0; k < (std::max)(b.BindCount, 1u) && b.BindPoint + k < 128; ++k)
                facts.slots[(b.BindPoint + k) >> 6] |= uint64_t{1} << ((b.BindPoint + k) & 63);
        }
        for (UINT i = 0; i < d.InputParameters; ++i) {
            D3D11_SIGNATURE_PARAMETER_DESC p{};
            if (SUCCEEDED(reflection->GetInputParameterDesc(i, &p)) && p.SystemValueType == D3D_NAME_POSITION && (p.ReadWriteMask & 3))
                facts.position = 1;
        }
    }
    (facts.known ? g_ps_known : g_ps_unknown).fetch_add(1, std::memory_order_relaxed);
    (*out)->SetPrivateData(kPsFactsGuid, sizeof facts, &facts);
    return hr;
}

// The full-size pictures the bound pixel shader reads. Called inside our own work.
void ScanWorldInputs(ID3D11DeviceContext* c, Ctx* x, const Ctx::WorldJob& job)
{
    auto& use = x->world_inputs;
    use = {};
    use.scanned = true;
    ComPtr<ID3D11PixelShader> shader;
    c->PSGetShader(&shader, nullptr, nullptr);
    if (!shader) return;
    PsFacts facts;
    UINT size = sizeof facts;
    if (FAILED(shader->GetPrivateData(kPsFactsGuid, &size, &facts)) || size != sizeof facts) facts = {}; // made before our hook: every bound view counts
    use.shader = IncarnationOf(shader.Get());
    use.by_position = !facts.known || facts.position;
    ID3D11ShaderResourceView* views[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
    c->PSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, views);
    for (UINT i = 0; i < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++i) {
        auto* view = views[i];
        if (!view) continue;
        if (!facts.known || ((facts.slots[i >> 6] >> (i & 63)) & 1)) {
            ComPtr<ID3D11Resource> resource;
            view->GetResource(&resource);
            ComPtr<ID3D11Texture2D> texture;
            if (resource && SUCCEEDED(resource.As(&texture))) {
                D3D11_TEXTURE2D_DESC desc{};
                texture->GetDesc(&desc);
                // A picture of the frame has the screen's size, or a half or a quarter of it (shading and fog
                // are often drawn at those), and is drawn or copied into: art loaded once (immutable,
                // CPU-written, or mipped without being a target) only happens to have such a size.
                const UINT nw = job.raster.native_width, nh = job.raster.native_height;
                UINT part = 0;
                for (UINT k : {1u, 2u, 4u})
                    if ((desc.Width == nw / k || desc.Width == (nw + k - 1) / k) && (desc.Height == nh / k || desc.Height == (nh + k - 1) / k)) { part = k; break; }
                const bool target = (desc.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)) != 0;
                const bool drawn = desc.Usage == D3D11_USAGE_DEFAULT && (target || (part == 1 && desc.MipLevels == 1));
                if (drawn && part) {
                    if (use.n < 4) {
                        use.w[use.n] = (std::max)(1u, UINT((uint64_t(desc.Width) * job.width + nw / 2) / nw));
                        use.h[use.n] = (std::max)(1u, UINT((uint64_t(desc.Height) * job.height + nh / 2) / nh));
                        use.slot[use.n++] = uint8_t(i);
                        const Id id = IdOf(texture.Get());
                        if (id == job.color) use.own_colour = true;
                        if (id == job.depth) use.own_depth = true;
                    } else use.impossible = true;
                }
            }
        }
        view->Release();
    }
}

enum class WorldDraw : uint8_t { NotWorld, Would, Small, Held };
template<class Forward>
WorldDraw DrawWorld(ID3D11DeviceContext* c, Ctx* x, Forward& forward)
{
    if (!x || !x->world || x->deferred) return WorldDraw::NotWorld;
    auto& job = *x->world;
    const bool colour = x->rt0 == job.color, depth = x->depth == job.depth;
    // After the reader has drawn, the pictures are the game's again for the rest of the frame.
    if ((!colour && !depth) || x->world_closed_frame == g_frame) return WorldDraw::NotWorld;
    if (x->tally_frame != g_frame) { x->tally = {}; x->tally_frame = g_frame; }
    const auto held = [&](WorldHold why) {
        ++x->tally.held[size_t(why)];
        if (x->tally.last_passed) {
            x->tally.last_passed = false;
            if (++x->tally.breaks > kWorldBreaks) x->world_given_up_frame = g_frame;
        }
        return WorldDraw::Held;
    };
    if (job.refused.load(std::memory_order_acquire)) return held(job.missed_evaluations >= 3 ? WorldHold::Evaluator : WorldHold::PictureKind);
    if (!job.ready.load(std::memory_order_acquire) || job.level != g_world_level.load()) return held(WorldHold::Preparing);
    if (g_frame - job.last_receipt > 1 || !job.proof_current.load(std::memory_order_relaxed) ||
        g_sel_color.load() != job.color || g_sel_depth.load() != job.depth) return held(WorldHold::Reader);
    // Another picture drawn with the world's depth, several pictures at once, or the world's colour with a depth of its own.
    if (!colour ? x->rt_count != 0 : (x->rt_count != 1 || (x->depth && !depth))) return held(WorldHold::SeveralTargets);
    if (x->vp_n != 1) return held(WorldHold::Viewports);
    if (x->raster_queries) return held(WorldHold::Interleaved); // a visibility query counts full-size pixels
    if (x->world_given_up_frame == g_frame) return held(WorldHold::Interleaved);
    Inside own;
    if (!x->world_uavs.has_value()) {
        ComPtr<ID3D11Device> device; c->GetDevice(&device);
        const UINT slots = device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1 ? D3D11_1_UAV_SLOT_COUNT : D3D11_PS_CS_UAV_REGISTER_COUNT;
        ID3D11UnorderedAccessView* views[D3D11_1_UAV_SLOT_COUNT]{};
        c->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, slots, views);
        x->world_uavs = false;
        for (UINT i = 0; i < slots; ++i) if (views[i]) { x->world_uavs = true; views[i]->Release(); }
    }
    if (*x->world_uavs) return held(WorldHold::Uavs);
    ComPtr<ID3D11Predicate> pred; BOOL value;
    c->GetPredication(&pred, &value);
    if (pred) return held(WorldHold::Predicated);
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11DepthStencilView> depth_view;
    c->OMGetRenderTargets(1, &target, &depth_view);
    ID3D11DepthStencilView* low_depth = nullptr;
    bool depth_written = false;
    if (colour) {
        D3D11_RENDER_TARGET_VIEW_DESC cv{};
        if (!target) return held(WorldHold::PictureKind);
        target->GetDesc(&cv);
        if (ResourceOfView(target.Get()) != job.color || cv.Format != job.color_format || cv.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D ||
            cv.Texture2D.MipSlice) return held(WorldHold::PictureKind);
    }
    if (depth) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
        if (!depth_view) return held(WorldHold::PictureKind);
        depth_view->GetDesc(&dv);
        if (ResourceOfView(depth_view.Get()) != job.depth || dv.Format != job.depth_format || dv.ViewDimension != D3D11_DSV_DIMENSION_TEXTURE2D ||
            dv.Texture2D.MipSlice) return held(WorldHold::PictureKind);
        low_depth = job.raster.DepthView(dv.Flags);
        if (!low_depth) return held(WorldHold::PictureKind);
        depth_written = !(dv.Flags & D3D11_DSV_READ_ONLY_DEPTH);
    }
    if (!x->world_inputs.scanned) ScanWorldInputs(c, x, job);
    auto& use = x->world_inputs;
    if (use.impossible) return held(WorldHold::FullSizeInput);
    const auto policy = static_cast<referee::WorldInputs>(g_world_inputs.load(std::memory_order_relaxed));
    const bool corner = use.n && (policy == referee::WorldInputs::Corner || (policy == referee::WorldInputs::ByShader && use.by_position));
    const bool live = g_world_live.load(std::memory_order_relaxed) && !g_world_reference.load(std::memory_order_relaxed);
    // The world's own depth or colour read as a picture (soft edges read the depth): the full-size one must
    // hold this frame so far before it is read or made small. Only when it is behind: a run of such draws
    // that do not write depth brings it up to date once.
    if (live ? (use.own_depth && x->world_depth_dirty) || (use.own_colour && x->world_dirty)
             : (use.own_depth && x->would_depth_dirty) || (use.own_colour && x->would_dirty)) FinishWorld(c, x);
    ID3D11ShaderResourceView* sources[4]{};
    ID3D11ShaderResourceView* copies[4]{};
    const auto release_sources = [&] { for (auto*& v : sources) if (v) { v->Release(); v = nullptr; } };
    if (corner) {
        for (uint8_t i = 0; i < use.n; ++i) c->PSGetShaderResources(use.slot[i], 1, &sources[i]);
        for (uint8_t i = 0; i < use.n && !use.impossible; ++i) {
            const char* why = nullptr;
            if (!sources[i]) continue;
            const Id id = ResourceOfView(sources[i]);
            const uint64_t run = id == job.color || id == job.depth ? x->world_own_run : x->world_run;
            if (live) copies[i] = job.corners.Get(c, sources[i], use.w[i], use.h[i], run, g_frame, &why);
            else if (!job.corners.Can(sources[i], use.w[i], use.h[i], g_frame, &why)) use.impossible = true;
            if (live && !copies[i]) use.impossible = true;
            if (use.impossible) g_world_input_why.store(why, std::memory_order_relaxed);
        }
        if (use.impossible) { release_sources(); return held(WorldHold::FullSizeInput); }
    }
    if (use.n) {
        ++x->tally.inputs;
        if (corner) ++x->tally.corners;
    }
    x->tally.last_passed = true;
    if (!live) {
        // A rehearsal frame, or the Referee's comparison picture: counted, drawn as the game issued it.
        release_sources();
        ++x->tally.would;
        x->would_depth_write = depth_written;
        return WorldDraw::Would;
    }
    if (use.n) {
        // A shader with such a read drawn small for the first time: the Referee looks at the picture again.
        if (x->world_inputs_seen.size() > 8192) x->world_inputs_seen.clear();
        if (x->world_inputs_seen.insert(use.shader).second) x->world_inputs_new = true;
    }
    D3D11_VIEWPORT original{}; UINT count = 1;
    c->RSGetViewports(&count, &original);
    D3D11_RECT rects[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT rect_count = 0;
    {
        ComPtr<ID3D11RasterizerState> state;
        c->RSGetState(&state);
        D3D11_RASTERIZER_DESC rs{};
        if (state) state->GetDesc(&rs);
        if (rs.ScissorEnable) {
            rect_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
            c->RSGetScissorRects(&rect_count, rects);
        }
    }
    if (!x->world_dirty && x->world_frame != g_frame) {
        const bool has_stencil = job.depth_format == DXGI_FORMAT_D24_UNORM_S8_UINT || job.depth_format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        if (x->pristine_color && x->pristine_depth && (!has_stencil || (x->clear_flags & D3D11_CLEAR_STENCIL))) {
            c->ClearRenderTargetView(job.raster.Target(), x->clear_rgba);
            c->ClearDepthStencilView(job.raster.DepthView(), x->clear_flags, x->clear_z, x->clear_s);
        } else job.raster.Seed(c); // what the full-size pictures hold (an earlier pass, a full-size draw) comes along
    }
    if (depth) x->world_stencil_written |= x->stencil_writes;
    x->pristine_color = x->pristine_depth = false;
    const float sx = float(job.width) / job.raster.native_width, sy = float(job.height) / job.raster.native_height;
    auto scaled_view = original;
    scaled_view.TopLeftX *= sx; scaled_view.Width *= sx;
    scaled_view.TopLeftY *= sy; scaled_view.Height *= sy;
    if (rect_count) {
        D3D11_RECT scaled[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        for (UINT i = 0; i < rect_count; ++i)
            scaled[i] = {LONG(std::floor(rects[i].left * sx)), LONG(std::floor(rects[i].top * sy)),
                         LONG(std::ceil(rects[i].right * sx)), LONG(std::ceil(rects[i].bottom * sy))};
        c->RSSetScissorRects(rect_count, scaled);
    }
    if (x->coarse_rate) CoarseApply(c, x, 0);
    auto* low = job.raster.Target();
    c->OMSetRenderTargets(colour ? 1 : 0, colour ? &low : nullptr, low_depth);
    c->RSSetViewports(1, &scaled_view);
    if (corner) for (uint8_t i = 0; i < use.n; ++i) if (copies[i]) c->PSSetShaderResources(use.slot[i], 1, &copies[i]);
    forward();
    if (corner) for (uint8_t i = 0; i < use.n; ++i) if (copies[i]) c->PSSetShaderResources(use.slot[i], 1, &sources[i]);
    release_sources();
    auto* high = target.Get();
    c->OMSetRenderTargets(colour ? 1 : 0, colour ? &high : nullptr, depth_view.Get());
    c->RSSetViewports(1, &original);
    if (rect_count) c->RSSetScissorRects(rect_count, rects);
    x->world_frame = g_frame;
    x->world_small_frame = g_frame;
    x->world_dirty = true;
    if (depth_written) x->world_depth_dirty = true;
    if (x->world_draw_frame != g_frame) { x->world_draw_frame = g_frame; x->world_draws = 0; }
    ++x->world_draws;
    ++x->tally.went_small;
    return WorldDraw::Small;
}


// The frame that just ended is entered in the books (was the world delivered small, and if not, WHY),
// and the coming frame is planned: counted only, drawn small, or drawn at full size for the Referee's
// comparison. Under g_mu, on the presenting thread.
void WorldRouteFrame(uint64_t ended)
{
    using State = WorldRouteStatus::State;
    auto& r = g_route;
    auto& st = r.status;
    const uint32_t asked = g_world_level.load();
    const auto outside = WorldHold(g_world_frame_hold.exchange(0, std::memory_order_relaxed));
    if (!asked) {
        if (st.state != State::Off) {
            na::eventlog::Emit("native11_world_route", "state=off small_frames=%llu comparison_frames=%llu held_frames=%llu looks=%u",
                (unsigned long long)st.small_frames, (unsigned long long)st.reference_frames, (unsigned long long)st.held_frames, r.check.Looks());
            r = {};
        }
        g_world_live.store(false, std::memory_order_relaxed);
        g_world_reference.store(false, std::memory_order_relaxed);
        return;
    }
    Ctx* owner = nullptr;
    for (auto& entry : g_ctx)
        if (entry.second->world && !entry.second->deferred) owner = entry.second.get();
    const auto* job = owner ? owner->world.get() : nullptr;
    // Another level, another world or another proven reader: counted and looked at afresh.
    if (st.level != asked || !r.asked_frame) { r.check.Reset(); r.rehearsal.Reset(); r.asked_frame = ended; r.last_small_frame = 0; r.live = false; st.level = asked; }
    if (job && (job->color != r.job_color || job->depth != r.job_depth || job->proof != r.job_proof || job->level != r.job_level)) {
        r.check.Reset(); r.rehearsal.Reset(); r.live = false;
        r.job_color = job->color; r.job_depth = job->depth; r.job_proof = job->proof; r.job_level = job->level;
    }
    WorldHold why = WorldHold::None;
    bool clean = false;
    Ctx::WorldTally tally{};
    // No world to draw small yet. With no reason given from outside, the picture has not been handed over at
    // all: after ten seconds that is said, not "preparing" for ever.
    if (!job) why = outside != WorldHold::None ? outside : ended - r.asked_frame < 600 ? WorldHold::Preparing : WorldHold::Reader;
    else {
        if (owner->tally_frame == ended) tally = owner->tally;
        WorldHold most = WorldHold::None;
        for (size_t i = 1; i < size_t(WorldHold::kCount); ++i)
            if (tally.held[i] && (most == WorldHold::None || tally.held[i] > tally.held[size_t(most)])) most = WorldHold(i);
        if (job->refused.load(std::memory_order_acquire)) why = job->missed_evaluations >= 3 ? WorldHold::Evaluator : WorldHold::PictureKind;
        else if (!job->ready.load(std::memory_order_acquire)) why = WorldHold::Preparing;
        else if (!(tally.went_small + tally.would))
            why = most != WorldHold::None ? most : owner->world_list_frame == ended ? WorldHold::RecordedLists : outside != WorldHold::None ? outside : WorldHold::NoWorldDraws;
        else if (tally.breaks > kWorldBreaks || tally.restores > kWorldRestores) why = most != WorldHold::None ? most : WorldHold::Interleaved;
        else if (job->last_receipt != ended) why = WorldHold::Reader;
        else clean = true;
        owner->world->corners.Trim(ended);
    }
    r.rehearsal.Frame(clean);
    // The frame that ended, for the Referee.
    const bool delivered = r.frame_small || r.frame_reference;
    r.check.Drawn(r.frame_small ? referee::WorldCheck::Frame::Small : referee::WorldCheck::Frame::Reference, delivered ? r.frame_tag : 0, r.frame_inputs);
    if (r.frame_small) { ++st.small_frames; r.last_small_frame = ended; }
    else if (r.frame_reference) { ++st.reference_frames; r.last_small_frame = ended; }
    else ++st.held_frames;
    r.frame_small = r.frame_reference = r.frame_inputs = false;
    r.frame_tag = 0;
    if (r.check.Rest())
        na::eventlog::Emit("native11_world_check", "frame=%llu verdict=look-again-after-rest next_rest_frames=%u",
            (unsigned long long)ended, r.check.RestFrames());
    if (r.check.Failed()) why = WorldHold::PictureCheck;
    r.live = job && r.rehearsal.Live() && !r.check.Failed();
    if (r.live) {
        // Nothing drawn small goes unjudged: the first small frame, a shader that reads a full-size picture met
        // for the first time, and a picture once let through because the view was moving.
        if (!r.check.Steady() && !r.check.Looking()) r.check.Begin();
        else if (r.check.Steady() && (owner->world_inputs_new || (r.check.Unjudged() && ended - r.steady_frame > 600))) r.check.Begin();
    }
    if (owner) owner->world_inputs_new = false;
    g_world_live.store(r.live, std::memory_order_relaxed);
    g_world_reference.store(r.live && r.check.Next() == referee::WorldCheck::Frame::Reference, std::memory_order_relaxed);
    g_world_inputs.store(uint8_t(r.check.Inputs()), std::memory_order_relaxed);
    // What the menu and the log are told.
    const bool small_now = !r.check.Failed() && (delivered || (st.small_frames + st.reference_frames && ended - r.last_small_frame < 30));
    st.state = small_now ? (r.check.Looking() ? State::Checking : State::Small)
             : (why == WorldHold::Preparing || why == WorldHold::None || ended - r.asked_frame < 240) && !r.check.Failed() ? State::Preparing : State::Held;
    st.why = small_now ? WorldHold::None : why;
    st.small_draws = tally.went_small; st.held_draws = 0; st.breaks = tally.breaks;
    for (size_t i = 1; i < size_t(WorldHold::kCount); ++i) st.held_draws += tally.held[i];
    st.looks = r.check.Looks();
    st.inputs = referee::Name(r.check.Inputs());
    st.corner_fills = job ? job->corners.fills : 0;
    if (st.state != r.logged_state || st.why != r.logged_why || ended - r.logged_frame >= 600) {
        r.logged_state = st.state; r.logged_why = st.why; r.logged_frame = ended;
        char holds[160] = "";
        int at = 0;
        for (size_t i = 1; i < size_t(WorldHold::kCount) && at < int(sizeof holds) - 40; ++i)
            if (tally.held[i]) at += std::snprintf(holds + at, sizeof holds - at, " %s=%u", WorldHoldName(WorldHold(i)), unsigned(tally.held[i]));
        const char* input_why = g_world_input_why.load(std::memory_order_relaxed);
        na::eventlog::Emit("native11_world_route",
            "state=%s level=%u why=\"%s\" frames{small=%llu comparison=%llu held=%llu} last_frame{small_draws=%u would_be_small=%u held_draws=%u breaks=%u brought_up_to_date=%u reads_full_size=%u corner_copies=%u}"
            " held_by{%s } rehearsal=%u/%u live=%d inputs=%s looks=%u corner_fills=%llu shaders{known=%llu unknown=%llu}%s%s",
            st.state == State::Small ? "small" : st.state == State::Checking ? "small(referee-looking)" : st.state == State::Preparing ? "preparing" : "held",
            asked, WorldHoldText(st.why), (unsigned long long)st.small_frames, (unsigned long long)st.reference_frames, (unsigned long long)st.held_frames,
            tally.went_small, tally.would, st.held_draws, tally.breaks, tally.restores, tally.inputs, tally.corners, holds, r.rehearsal.Clean(), r.rehearsal.Needed(), int(r.live),
            st.inputs, st.looks, (unsigned long long)st.corner_fills,
            (unsigned long long)g_ps_known.load(std::memory_order_relaxed), (unsigned long long)g_ps_unknown.load(std::memory_order_relaxed),
            input_why ? " input_refused=" : "", input_why ? input_why : "");
    }
}

template<class Forward>
void ForwardGameDraw(ID3D11DeviceContext* c, Ctx* state, Forward forward)
{
    if (state && state->post.resource && (state->rt0 == state->post.resource || state->rt_count > 1))
        state->post = {};
    const auto mark = CheckScreenReader(c, state);
    // The draw event or this reader can complete the scene before the
    // same draw is forwarded. Restore it on that draw, not one draw later.
    if (state && !state->deferred && state->vpj_applied && !g_vpj_sample.load(std::memory_order_acquire))
        VpjBeforeDraw(c, state);
    if (state && !state->deferred && state->coarse_rate && !g_coarse_open.load(std::memory_order_relaxed))
        CoarseBeforeDraw(c, state); // the scene was handed over by this very draw's event: it is not shaded in blocks
    const WorldDraw drawn = DrawWorld(c, state, forward);
    if (drawn != WorldDraw::Small) {
        if (state && state->world) {
            // A draw at full size (another picture's, one the world could not take small, a rehearsal
            // frame's): the full-size pictures are brought up to date before it, and what it writes into
            // them is carried into the small ones before the next small draw (DrawWorld's Seed).
            // A draw that was only counted stands for a small one: nothing would be brought up to date for
            // it, and after it the small pictures would be ahead again.
            const bool counted = drawn == WorldDraw::Would;
            const bool depth_ahead = counted && (state->would_depth_dirty || state->would_depth_write);
            if (counted) state->would_dirty = state->would_depth_dirty = false;
            FinishWorld(c, state);
            TouchWorld(c, state, state->rt0, true);
            TouchWorld(c, state, state->depth, true);
            if (counted) { state->would_dirty = true; state->would_depth_dirty = depth_ahead; }
        }
        ForwardDepthRead(c, state, false, forward);
    }
    if (state && !state->deferred && state->depth_layer_target) {
        // VARIANT: jitter/stationary-source-readers: a depth-like layer drawn this frame while the sample is live by a
        // draw that kept the game's grid; drawn by a draw on the sample's grid, it is no longer one.
        if (g_vpj_sample.load(std::memory_order_relaxed)) {
            if (!state->vpj_applied) StationaryAdd(state, state->rt0);
            else StationaryRemove(state, state->rt0);
        }
        auto& layer = state->raster_depth_layer;
        const auto sample = g_vpj_sample.load(std::memory_order_relaxed);
        const auto depth = g_sel_depth.load(std::memory_order_relaxed);
        if (sample && state->vpj_applied && state->vpj_expanded &&
            state->vpj_sample == sample && state->depth_test && state->depth == depth &&
            state->target_width == g_sel_depth_w.load(std::memory_order_relaxed) &&
            state->target_height == g_sel_depth_h.load(std::memory_order_relaxed)) {
            layer.resource = state->rt0;
            layer.depth = depth;
            layer.sample = sample;
            layer.min_depth = state->vps[0].MinDepth;
            layer.max_depth = state->vps[0].MaxDepth;
            layer.frame = g_exact_frame.load(std::memory_order_relaxed);
            layer.pixel_reads.reset(); // cannot be both the RTV and an SRV
        } else if (layer.resource == state->rt0) layer = {};
    }
    if (mark.source) {
        Inside own;
        ScreenCheck::Restore(c, mark);
    }
}

// The HUD-free picture (hud_free.hpp): before an immediate-context draw is forwarded. Its second
// draw (T2) goes straight to the original entry point.
void IssueHudFreeDraw(ID3D11DeviceContext* c, const draw_replay::DrawArgs& a, void* user)
{
    Ctx* x = static_cast<Ctx*>(user);
    const UINT inst = a.instances ? a.instances : 1;
    if (a.indexed) O(c, x).draw_indexed_instanced(c, a.count, inst, a.start, a.base_vertex, a.start_instance);
    else O(c, x).draw_instanced(c, a.count, inst, a.start, a.start_instance);
}
// A deferred recording's colour writes (hud_free::OnExecuteList). Cheap: a short list per recording.
constexpr size_t kMaxListWrites = 256;
void NoteListWrite(Ctx* x, Id r)
{
    if (!x || !x->deferred || !r || x->list_writes_unknown) return;
    for (Id w : x->list_writes) if (w == r) return;
    if (x->list_writes.size() >= kMaxListWrites) { x->list_writes_unknown = true; x->list_writes.clear(); return; }
    x->list_writes.push_back(r);
}
void NoteListDrawWrites(ID3D11DeviceContext* c, Ctx* x)
{
    if (!x || !x->deferred) return;
    if (x->rt_count <= 1) { NoteListWrite(x, x->rt0); return; }
    ID3D11RenderTargetView* v[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, v, nullptr);
    for (auto* r : v) if (r) { NoteListWrite(x, ResourceOfView(r)); r->Release(); }
}

void HudFreeBeforeDraw(ID3D11DeviceContext* c, Ctx* x, const draw_replay::DrawArgs& a)
{
    if (x && x->deferred && !t_inside) NoteListDrawWrites(c, x);
    if (!hud_free::Wanted() || !x || x->deferred || t_inside) return;
    hud_free::DrawFacts f;
    f.rt0 = uint64_t(x->rt0);
    f.shown = uint64_t(g_backbuffer);
    f.rt_w = x->target_width;
    f.rt_h = x->target_height;
    f.rt_count = x->rt_count;
    f.depth = x->depth && x->depth_test;
    hud_free::BeforeDraw(c, f, a, &IssueHudFreeDraw, x);
}

// Draw replay (draw_replay.hpp): after the game's draw has run, a qualifying scene draw on the
// immediate context is drawn again with stream output into this frame's position buffer.
void ReplayAfterDraw(ID3D11DeviceContext* c, Ctx* x, const draw_replay::DrawArgs& a)
{
    if (!x || !draw_replay::Active()) return;
    draw_replay::DrawFacts f;
    f.frame = g_frame;
    f.deferred = x->deferred;
    const Id scene_depth = g_sel_depth.load(std::memory_order_relaxed);
    f.scene_depth = scene_depth && x->depth == scene_depth && x->depth_test;
    f.depth_read_only = x->depth_read_only;
    f.query_active = x->raster_queries != 0;
    const auto& vp = x->vps[0];
    const float viewport[]{vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, vp.MinDepth, vp.MaxDepth};
    f.viewport = viewport;
    f.viewports = x->vp_n;
    draw_replay::AfterDraw(c, f, a);
}
void STDMETHODCALLTYPE DrawIndexed_d(ID3D11DeviceContext* c, UINT count, UINT start, INT base)
{
    DrawTimer timer;
    Ctx* x = DrawEvent(c, count, false);
    HudFreeBeforeDraw(c, x, {true, false, count, 1, start, 0, base});
    ForwardGameDraw(c, x, [&] { DriverScope drv(timer); O(c, x).draw_indexed(c, count, start, base); });
    ReplayAfterDraw(c, x, {true, false, count, 1, start, 0, base});
}
void STDMETHODCALLTYPE Draw_d(ID3D11DeviceContext* c, UINT count, UINT start)
{
    DrawTimer timer;
    Ctx* x = DrawEvent(c, count, false);
    HudFreeBeforeDraw(c, x, {false, false, count, 1, start, 0, 0});
    ForwardGameDraw(c, x, [&] { DriverScope drv(timer); O(c, x).draw(c, count, start); });
    ReplayAfterDraw(c, x, {false, false, count, 1, start, 0, 0});
}
void STDMETHODCALLTYPE DrawIndexedInstanced_d(ID3D11DeviceContext* c, UINT count, UINT inst, UINT start, INT base, UINT start_inst)
{
    DrawTimer timer;
    Ctx* x = DrawEvent(c, uint64_t(count) * (inst ? inst : 1), false);
    HudFreeBeforeDraw(c, x, {true, false, count, inst, start, start_inst, base});
    ForwardGameDraw(c, x, [&] { DriverScope drv(timer); O(c, x).draw_indexed_instanced(c, count, inst, start, base, start_inst); });
    if (inst) ReplayAfterDraw(c, x, {true, false, count, inst, start, start_inst, base});
}
void STDMETHODCALLTYPE DrawInstanced_d(ID3D11DeviceContext* c, UINT count, UINT inst, UINT start, UINT start_inst)
{
    DrawTimer timer;
    Ctx* x = DrawEvent(c, uint64_t(count) * (inst ? inst : 1), false);
    HudFreeBeforeDraw(c, x, {false, false, count, inst, start, start_inst, 0});
    ForwardGameDraw(c, x, [&] { DriverScope drv(timer); O(c, x).draw_instanced(c, count, inst, start, start_inst); });
    if (inst) ReplayAfterDraw(c, x, {false, false, count, inst, start, start_inst, 0});
}
void STDMETHODCALLTYPE DrawIndexedInstancedIndirect_d(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT off)
{
    DrawTimer timer;
    Ctx* x = DrawEvent(c, 0, true);
    HudFreeBeforeDraw(c, x, {true, true});
    ForwardGameDraw(c, x, [&] { DriverScope drv(timer); O(c, x).draw_indexed_indirect(c, b, off); });
    ReplayAfterDraw(c, x, {true, true});
}
void STDMETHODCALLTYPE DrawInstancedIndirect_d(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT off)
{
    DrawTimer timer;
    Ctx* x = DrawEvent(c, 0, true);
    HudFreeBeforeDraw(c, x, {false, true});
    ForwardGameDraw(c, x, [&] { DriverScope drv(timer); O(c, x).draw_indirect(c, b, off); });
    ReplayAfterDraw(c, x, {false, true});
}
void STDMETHODCALLTYPE DrawAuto_d(ID3D11DeviceContext* c)
{
    DrawTimer timer;
    Ctx* x = DrawEvent(c, 0, true);
    HudFreeBeforeDraw(c, x, {false, true});
    ForwardGameDraw(c, x, [&] { DriverScope drv(timer); O(c, x).draw_auto(c); });
    ReplayAfterDraw(c, x, {false, true});
}
void STDMETHODCALLTYPE Dispatch_d(ID3D11DeviceContext* c, UINT x_groups, UINT y_groups, UINT z_groups)
{
    Ctx* x = (g_world_level.load(std::memory_order_relaxed) || g_depth_reads_enabled.load(std::memory_order_relaxed)) ? Get(c) : nullptr;
    FinishWorld(c, x);
    if (x) { ++x->world_run; ++x->world_own_run; } // a compute pass may write a picture the world reads
    ForwardDepthRead(c, x, true, [&] { O(c, x).dispatch(c, x_groups, y_groups, z_groups); });
}
void STDMETHODCALLTYPE DispatchIndirect_d(ID3D11DeviceContext* c, ID3D11Buffer* args, UINT offset)
{
    Ctx* x = (g_world_level.load(std::memory_order_relaxed) || g_depth_reads_enabled.load(std::memory_order_relaxed)) ? Get(c) : nullptr;
    FinishWorld(c, x);
    if (x) { ++x->world_run; ++x->world_own_run; } // a compute pass may write a picture the world reads
    ForwardDepthRead(c, x, true, [&] { O(c, x).dispatch_indirect(c, args, offset); });
}
HRESULT STDMETHODCALLTYPE Map_d(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* m)
{
    Ctx* x = Get(c);
    TouchWorld(c, x, IdOf(r), type != D3D11_MAP_READ, false); // buffers are mapped between all draws
    const HRESULT hr = O(c, x).map(c, r, sub, type, flags, m);
    if (x && CameraTracking() && SUCCEEDED(hr) && m && (type == D3D11_MAP_WRITE_DISCARD || type == D3D11_MAP_WRITE_NO_OVERWRITE || type == D3D11_MAP_WRITE ||
                                    type == D3D11_MAP_READ_WRITE)) {
        TscScope timed(g_tsc_camera);
        const Id id = IdOf(r);
        UINT size = 0;
        if (IsCandidate(id) && IsConstantBuffer(r, &size)) {
            x->mapped[id] = m->pData;
            if (size > kMaxCbBytes && (type == D3D11_MAP_WRITE_DISCARD || type == D3D11_MAP_WRITE_NO_OVERWRITE)) {
                if (x->large.size() >= 64 && !x->large.count(id)) x->large.clear();
                auto& large = x->large[id];
                large.size = size;
                if (type == D3D11_MAP_WRITE_DISCARD) ++large.epoch; // old contents are gone
                else large.no_overwrite = true;                    // the game itself proves this mapping works here
            } else if (!x->large.empty()) {
                x->large.erase(id);
            }
        } else if (!x->mapped.empty()) x->mapped.erase(id);
    }
    return hr;
}
void STDMETHODCALLTYPE Unmap_d(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub)
{
    Ctx* x = Get(c);
    if (x) {
        const Id id = IdOf(r);
        if (!CameraTracking()) {
            if (!x->mapped.empty()) x->mapped.erase(id);
            O(c, x).unmap(c, r, sub);
            return;
        }
        TscScope timed(g_tsc_camera);
        UINT size = 0;
        bool cb = false;
        {
            Inside in;
            cb = IsConstantBuffer(r, &size);
        }
        auto it = x->mapped.find(id);
        if (it != x->mapped.end()) {
            // The game's own write-combined mapping is read once, only for the few candidate buffers.
            if (cb && it->second) {
                if (size <= kMaxCbBytes) {
                    JitterUpload(it->second, size, id); // before the GPU can see the bytes
                    CbUpdate(c, r, it->second, size);
                } else {
                    // Observe a bounded window while the map is live. Large
                    // buffers are never rewritten by matrix jitter.
                    const CaptureWindows windows = CameraWindows(id);
                    CbUpdate(c, r, it->second, size, &windows);
                }
            }
            x->mapped.erase(it);
        } else if (cb) {
            CbUpdate(c, r, nullptr, 0); // not a candidate: still a version change for the model
        }
    }
    O(c, x).unmap(c, r, sub);
}
void UpdateCommon(ID3D11DeviceContext* c, Ctx* x, ID3D11Resource* r, const D3D11_BOX* box, const void*& src, std::vector<uint8_t>& tmp)
{
    if (!x || !r || !src || !CameraTracking()) return;
    TscScope timed(g_tsc_camera);
    UINT size = 0;
    bool cb = false;
    {
        Inside in;
        cb = IsConstantBuffer(r, &size);
    }
    if (!cb) return;
    if (box || !IsCandidate(IdOf(r))) { CbUpdate(c, r, nullptr, 0); return; } // constant buffers are updated whole
    if (g_vpj_route.load(std::memory_order_relaxed)) {
        CbUpdate(c, r, src, size);
        return;
    }
    tmp.assign(static_cast<const uint8_t*>(src), static_cast<const uint8_t*>(src) + size);
    if (JitterUpload(tmp.data(), tmp.size(), IdOf(r))) src = tmp.data(); // the game's upload, jittered
    CbUpdate(c, r, tmp.data(), tmp.size());
}
void InvalidateDepthWrite(Ctx* context, ID3D11Resource* resource);
void TrackPlacementColorWrite(ID3D11DeviceContext* c, Ctx* x, Id resource);

void STDMETHODCALLTYPE UpdateSubresource_d(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* src, UINT rp,
                                           UINT dp)
{
    Ctx* x = Get(c);
    std::vector<uint8_t> tmp;
    TouchWorld(c, x, IdOf(r), true, false);
    UpdateCommon(c, x, r, box, src, tmp);
    TrackPlacementColorWrite(c, x, IdOf(r));
    if (x && x->deferred) NoteListWrite(x, IdOf(r));
    O(c, x).update(c, r, sub, box, src, rp, dp);
    InvalidateDepthWrite(x, r);
}
void STDMETHODCALLTYPE UpdateSubresource1_d(ID3D11DeviceContext1* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* src,
                                            UINT rp, UINT dp, UINT flags)
{
    Ctx* x = Get(c);
    std::vector<uint8_t> tmp;
    TouchWorld(c, x, IdOf(r), true, false);
    UpdateCommon(c, x, r, box, src, tmp);
    TrackPlacementColorWrite(c, x, IdOf(r));
    O(c, x).update1(c, r, sub, box, src, rp, dp, flags);
    InvalidateDepthWrite(x, r);
}
ViewUse RtvUse(ID3D11RenderTargetView* v)
{
    D3D11_RENDER_TARGET_VIEW_DESC d{};
    v->GetDesc(&d);
    ViewUse u;
    u.format = d.Format;
    u.texture2d = d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D;
    if (u.texture2d) u.mip = d.Texture2D.MipSlice;
    else if (d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY) { u.mip = d.Texture2DArray.MipSlice; u.slice = d.Texture2DArray.FirstArraySlice; }
    else if (d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY) u.slice = d.Texture2DMSArray.FirstArraySlice;
    return u;
}
ViewUse DsvUse(ID3D11DepthStencilView* v, bool* read_only = nullptr)
{
    D3D11_DEPTH_STENCIL_VIEW_DESC d{};
    v->GetDesc(&d);
    if (read_only) *read_only = (d.Flags & D3D11_DSV_READ_ONLY_DEPTH) != 0;
    ViewUse u;
    u.format = d.Format;
    if (d.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D) u.mip = d.Texture2D.MipSlice;
    else if (d.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DARRAY) { u.mip = d.Texture2DArray.MipSlice; u.slice = d.Texture2DArray.FirstArraySlice; }
    else if (d.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY) u.slice = d.Texture2DMSArray.FirstArraySlice;
    return u;
}
void TrackTargets(Ctx* x, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv)
{
    ID3D11Resource* rt = nullptr;
    ID3D11Resource* ds = nullptr;
    ViewUse rt_use, ds_use;
    x->depth_read_only = false;
    {
        Inside in;
        if (n && rtvs && rtvs[0]) { rtvs[0]->GetResource(&rt); rt_use = RtvUse(rtvs[0]); }
        if (dsv) { dsv->GetResource(&ds); ds_use = DsvUse(dsv, &x->depth_read_only); }
    }
    x->rt0 = IdOf(rt);
    x->depth = IdOf(ds);
    x->rt_view = rt_use;
    x->ds_view = ds_use;
    x->depth_layer_target = x->material_layer_target = false;
    x->target_width = x->target_height = 0;
    UINT bound = 0;
    for (UINT i = 0; rtvs && i < n; ++i)
        if (rtvs[i]) ++bound;
    x->rt_count = static_cast<uint8_t>(bound);
    auto& layer = x->raster_depth_layer;
    if (layer.resource) {
        if (layer.frame != g_exact_frame.load(std::memory_order_relaxed) || layer.resource == x->rt0) layer = {};
        else if (bound > 1) {
            Inside in;
            for (UINT i = 1; i < n; ++i)
                if (ResourceOfView(rtvs[i]) == layer.resource) { layer = {}; break; }
        }
    }
    if (rt || ds) {
        std::lock_guard<std::mutex> l(g_mu);
        Inside in;
        if (rt) {
            ResourceInfo info = InfoOf(rt, false);
            // As the game writes it: the view's format, or the texture's when the view names none.
            const uint32_t written = rt_use.format ? rt_use.format : info.format;
            info.float_colour = written == DXGI_FORMAT_R16G16B16A16_FLOAT || written == DXGI_FORMAT_R11G11B10_FLOAT ||
                                written == DXGI_FORMAT_R32G32B32A32_FLOAT || written == DXGI_FORMAT_R32G32B32_FLOAT;
            x->target_width = info.width;
            x->target_height = info.height;
            const bool plain = rt_use.texture2d && !rt_use.mip && info.samples == 1;
            x->depth_layer_target = plain && written == DXGI_FORMAT_R32_FLOAT;
            x->material_layer_target = plain && info.float_colour;
            g_model.SetInfo(IdOf(rt), info);
        }
        if (ds) {
            g_model.SetInfo(IdOf(ds), InfoOf(ds, true));
            if (IdOf(ds) == g_model.Current().depth) {
                if (g_depth_ref.Get() != ds) g_depth_ref = ds;
                g_depth_view = ds_use;
            }
        }
        if (rt && IdOf(rt) == g_model.Current().color) {
            if (g_color_ref.Get() != rt) g_color_ref = rt;
            g_color_view = rt_use;
        }
    }
    if (rt) rt->Release();
    if (ds) ds->Release();
}
void STDMETHODCALLTYPE OMSetRenderTargets_d(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv)
{
    Ctx* x = Get(c);
    FinishWorld(c, x);
    if (x) { x->world_uavs.reset(); TrackTargets(x, n, rtvs, dsv); }
    O(c, x).om_rt(c, n, rtvs, dsv);
}
void STDMETHODCALLTYPE OMSetRenderTargetsAndUAVs_d(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv,
                                                   UINT us, UINT un, ID3D11UnorderedAccessView* const* uavs, const UINT* counts)
{
    Ctx* x = Get(c);
    FinishWorld(c, x);
    if (x) x->world_uavs.reset();
    if (x && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) TrackTargets(x, n, rtvs, dsv);
    O(c, x).om_rt_uav(c, n, rtvs, dsv, us, un, uavs, counts);
}
void STDMETHODCALLTYPE OMSetDepthStencilState_d(ID3D11DeviceContext* c, ID3D11DepthStencilState* s, UINT ref)
{
    Ctx* x = Get(c);
    if (x) {
        x->stencil_writes = false;
        if (!s) { x->depth_test = true; x->depth_func = kCompareLess; } // default state: depth test on, LESS, writing
        else {
            D3D11_DEPTH_STENCIL_DESC d{};
            Inside in;
            s->GetDesc(&d);
            const auto changes_stencil = [](const D3D11_DEPTH_STENCILOP_DESC& face) {
                return face.StencilFailOp != D3D11_STENCIL_OP_KEEP || face.StencilDepthFailOp != D3D11_STENCIL_OP_KEEP ||
                    face.StencilPassOp != D3D11_STENCIL_OP_KEEP;
            };
            x->stencil_writes = d.StencilEnable && d.StencilWriteMask && (changes_stencil(d.FrontFace) || changes_stencil(d.BackFace));
            x->depth_test = d.DepthEnable != FALSE;
            // Depth direction evidence: the comparison counts only on draws that also write the depth.
            x->depth_func = d.DepthEnable && d.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL ? static_cast<uint8_t>(d.DepthFunc) : uint8_t{0};
        }
    }
    O(c, x).om_dss(c, s, ref);
}
void STDMETHODCALLTYPE RSSetViewports_d(ID3D11DeviceContext* c, UINT n, const D3D11_VIEWPORT* v)
{
    Ctx* x = Get(c);
    if (x) {
        x->vp_x = n && v ? v[0].TopLeftX : 0.f;
        x->vp_y = n && v ? v[0].TopLeftY : 0.f;
        x->vp_w = n && v ? v[0].Width : 0.f;
        x->vp_h = n && v ? v[0].Height : 0.f;
        // The game's own viewports replace ours; the next scene draw moves them again.
        x->vp_n = v ? (std::min)(n, UINT(D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE)) : 0u;
        for (UINT i = 0; i < x->vp_n; ++i) x->vps[i] = v[i];
        x->vpj_applied = false;
    }
    O(c, x).rs_vp(c, n, v);
}
void InvalidateDepthWrite(Ctx* context, ID3D11Resource* resource)
{
    const Id depth = DepthReadsSource(context);
    if (!depth || IdOf(resource) != depth) return;
    auto& reads = context->depth_reads;
    reads.Start(g_exact_frame.load(std::memory_order_relaxed), depth);
    reads.mixed = true;
}

ID3D11Resource* DepthCopySource(ID3D11DeviceContext* c, Ctx* x, ID3D11Resource* dst, ID3D11Resource* src)
{
    const Id depth = DepthReadsSource(x);
    if (!depth || IdOf(src) != depth || dst == src) return src;
    auto& reads = x->depth_reads;
    reads.Start(g_exact_frame.load(std::memory_order_relaxed), depth);
    Inside own;
    constexpr uint64_t copy_reader = ~uint64_t{0};
    if (!reads.Refresh(c, src, copy_reader, x->raster_queries != 0)) return src;
    auto* twin = reads.twin->SourceForCopy(dst);
    return twin ? twin : src;
}

bool StationaryHas(const Ctx* x, Id r)
{
    if (!x || !r || x->stationary.frame != g_exact_frame.load(std::memory_order_relaxed)) return false;
    for (const Id s : x->stationary.resource) if (s == r) return true;
    return false;
}
void StationaryAdd(Ctx* x, Id r)
{
    if (!x || !r) return;
    const uint32_t frame = g_exact_frame.load(std::memory_order_relaxed);
    if (x->stationary.frame != frame) { x->stationary = {}; x->stationary.frame = frame; }
    for (Id& s : x->stationary.resource) if (s == r) return;
    for (Id& s : x->stationary.resource) if (!s) { s = r; return; }
    x->stationary.resource[0] = r; // four layers a frame at most: the newest replaces the oldest
}
void StationaryRemove(Ctx* x, Id r)
{
    if (!x || !r) return;
    for (Id& s : x->stationary.resource) if (s == r) s = 0;
}

void TrackPlacementColorWrite(ID3D11DeviceContext* c, Ctx* x, Id resource)
{
    if (!x || !resource) return;
    if (x->post.resource == resource) x->post = {};
    if (x->raster_depth_layer.resource == resource) x->raster_depth_layer = {};
    const Id peer = g_lit_peer.load(std::memory_order_relaxed);
    if ((!peer && !g_adaptive_scene.load(std::memory_order_relaxed)) ||
        (resource != peer && resource != g_sel_color.load(std::memory_order_relaxed))) return;
    Event e;
    e.kind = Kind::ColorWrite;
    e.resource = resource;
    Emit(x, c, e);
}

void STDMETHODCALLTYPE ClearRenderTargetView_d(ID3D11DeviceContext* c, ID3D11RenderTargetView* view, const FLOAT color[4])
{
    Ctx* x = Get(c);
    if (x && x->world) {
        const auto id = ResourceOfView(view);
        TouchWorld(c, x, id, true);
        if (id == x->world->color) {
            x->world_color_clear = g_frame;
            std::copy_n(color, 4, x->clear_rgba);
            x->pristine_color = true;
        }
    }
    if (x && (x->post.resource || x->raster_depth_layer.resource || g_adaptive_scene.load(std::memory_order_relaxed) ||
              g_lit_peer.load(std::memory_order_relaxed)))
        TrackPlacementColorWrite(c, x, ResourceOfView(view));
    if (x && x->deferred) NoteListWrite(x, ResourceOfView(view));
    O(c, x).clear_rtv(c, view, color);
}

void STDMETHODCALLTYPE CopySubresourceRegion_d(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT ds, UINT x0, UINT y0, UINT z0, ID3D11Resource* src,
                                               UINT ss, const D3D11_BOX* box)
{
    Ctx* x = Get(c);
    TouchWorld(c, x, IdOf(src), false);
    TouchWorld(c, x, IdOf(dst), true);
    ReadResource(c, src);
    if (x && !x->deferred && !t_inside) hud_free::BeforeCopy(uint64_t(IdOf(dst)), 0, 0);
    if (x && x->deferred) NoteListWrite(x, IdOf(dst));
    auto* source = !ds && !ss && !x0 && !y0 && !z0 && !box ? DepthCopySource(c, x, dst, src) : src;
    O(c, x).copy_region(c, dst, ds, x0, y0, z0, source, ss, box);
    InvalidateDepthWrite(x, dst);
    TrackPlacementColorWrite(c, x, IdOf(dst));
}
void STDMETHODCALLTYPE CopyResource_d(ID3D11DeviceContext* c, ID3D11Resource* dst, ID3D11Resource* src)
{
    Ctx* x = Get(c);
    TouchWorld(c, x, IdOf(src), false);
    TouchWorld(c, x, IdOf(dst), true);
    ReadResource(c, src);
    if (x && !x->deferred && !t_inside) hud_free::BeforeCopy(uint64_t(IdOf(dst)), 0, 0);
    if (x && x->deferred) NoteListWrite(x, IdOf(dst));
    O(c, x).copy_resource(c, dst, DepthCopySource(c, x, dst, src));
    InvalidateDepthWrite(x, dst);
    TrackPlacementColorWrite(c, x, IdOf(dst));
    // VARIANT: jitter/stationary-source-readers: an exact whole copy of a stationary layer is one too.
    if (x && !x->deferred) { if (StationaryHas(x, IdOf(src))) StationaryAdd(x, IdOf(dst)); else StationaryRemove(x, IdOf(dst)); }
}
void STDMETHODCALLTYPE ResolveSubresource_d(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT ds, ID3D11Resource* src, UINT ss, DXGI_FORMAT f)
{
    Ctx* x = Get(c);
    TouchWorld(c, x, IdOf(src), false);
    TouchWorld(c, x, IdOf(dst), true);
    ReadResource(c, src);
    if (x && x->deferred) NoteListWrite(x, IdOf(dst));
    O(c, x).resolve(c, dst, ds, src, ss, f);
    InvalidateDepthWrite(x, dst);
    TrackPlacementColorWrite(c, x, IdOf(dst));
}
void STDMETHODCALLTYPE ClearDepthStencilView_d(ID3D11DeviceContext* c, ID3D11DepthStencilView* v, UINT flags, FLOAT d, UINT8 s)
{
    Ctx* x = Get(c);
    if (x && x->world) {
        const auto id = ResourceOfView(v);
        TouchWorld(c, x, id, true);
        if (id == x->world->depth && (flags & D3D11_CLEAR_DEPTH)) {
            x->world_depth_clear = g_frame;
            x->clear_flags = flags; x->clear_z = d; x->clear_s = s;
            x->pristine_depth = true;
        }
    }
    if (x && v && (flags & D3D11_CLEAR_DEPTH)) {
        Id id = 0;
        {
            Inside in;
            id = ResourceOfView(v);
        }
        Event e;
        e.kind = Kind::ClearDepth;
        e.resource = id;
        e.clear_depth = d;
        Emit(x, c, e);
        if (id && id == DepthReadsSource(x)) {
            auto& reads = x->depth_reads;
            reads.Start(g_exact_frame.load(std::memory_order_relaxed), id);
            reads.cleared = true;
            reads.seen = reads.mixed = false;
            reads.dirty = true;
        }
    }
    O(c, x).clear_dsv(c, v, flags, d, s);
}
// D3D11.1 can access or discard a view without an OM binding change.
void STDMETHODCALLTYPE CopyRegion1_d(ID3D11DeviceContext1* c, ID3D11Resource* dst, UINT ds, UINT x0, UINT y, UINT z,
                                     ID3D11Resource* src, UINT ss, const D3D11_BOX* box, UINT flags)
{
    Ctx* x = Get(c);
    TouchWorld(c, x, IdOf(src), false); TouchWorld(c, x, IdOf(dst), true);
    O(c,x).copy1(c,dst,ds,x0,y,z,src,ss,box,flags);
}
void STDMETHODCALLTYPE DiscardResource_d(ID3D11DeviceContext1* c, ID3D11Resource* r)
{
    Ctx* x = Get(c); TouchWorld(c,x,IdOf(r),true); O(c,x).discard_resource(c,r);
}
void STDMETHODCALLTYPE DiscardView_d(ID3D11DeviceContext1* c, ID3D11View* view)
{
    Ctx* x = Get(c); if (x && x->world) TouchWorld(c,x,ResourceOfView(view),true);
    O(c,x).discard_view(c,view);
}
void STDMETHODCALLTYPE DiscardView1_d(ID3D11DeviceContext1* c, ID3D11View* view,const D3D11_RECT* rects, UINT n)
{
    Ctx* x = Get(c); if (x && x->world) TouchWorld(c,x,ResourceOfView(view),true);
    O(c,x).discard_view1(c,view,rects,n);
}
void STDMETHODCALLTYPE ClearView_d(ID3D11DeviceContext1* c, ID3D11View* view,const FLOAT* color,const D3D11_RECT* rects, UINT n)
{
    Ctx* x = Get(c); if (x && x->world) TouchWorld(c,x,ResourceOfView(view),true);
    O(c,x).clear_view(c,view,color,rects,n);
}
void STDMETHODCALLTYPE SwapState_d(ID3D11DeviceContext1* c, ID3DDeviceContextState* state, ID3DDeviceContextState** previous)
{
    Ctx* x = Get(c); FinishWorld(c,x);
    if (x && x->world) {
        x->world_uavs.reset();
        x->world_color_clear = x->world_depth_clear = x->world_frame = ~uint64_t{0};
        // Unknown bindings invalidate draw eligibility. The job's color, depth and
        // copy lists still belong to the same objects across a context state swap.
        x->reset_state();
        x->pristine_color = x->pristine_depth = false;
    }
    O(c,x).swap_state(c,state,previous);
}
void STDMETHODCALLTYPE ClearState_d(ID3D11DeviceContext* c)
{
    Ctx* x = Get(c);
    FinishWorld(c, x);
    if (x) { x->world_uavs = false; x->reset_state(); }
    O(c, x).clear_state(c);
}
HRESULT STDMETHODCALLTYPE FinishCommandList_d(ID3D11DeviceContext* c, BOOL restore, ID3D11CommandList** out)
{
    Ctx* x = Get(c);
    HRESULT hr;
    {
        // Runtime state restoration can call the hooked setters with physical
        // bindings. Keep the game's tracked state for restore=TRUE.
        Inside in;
        hr = O(c, x).finish(c, restore, out);
    }
    if (x) {
        if (SUCCEEDED(hr) && out && *out) {
            std::lock_guard<std::mutex> l(g_mu);
            ListRecord& rec = g_lists[*out];
            rec.events = std::move(x->events);
            rec.cb_bytes = std::move(x->cb_bytes);
            rec.draw_bytes = std::move(x->draw_bytes);
            rec.order = ++g_list_order;
            rec.incarnation = IncarnationOf(*out);
            rec.complete = !x->events_truncated;
            rec.writes = std::move(x->list_writes);
            rec.writes_unknown = x->list_writes_unknown;
            if (g_lists.size() > kMaxLists) {
                auto oldest = g_lists.begin();
                for (auto it = g_lists.begin(); it != g_lists.end(); ++it)
                    if (it->second.order < oldest->second.order) oldest = it;
                g_lists.erase(oldest);
            }
        }
        x->events.clear();
        x->events_truncated = false;
        x->coarse_rate = 0; // the driver starts the next recording at one shading per pixel (vrs11_test (ii))
        x->list_writes.clear();
        x->list_writes_unknown = false;
        x->cb_bytes.clear();
        x->draw_bytes.clear();
        // A new recording must map with discard before it may map without overwrite again.
        x->large.clear();
        for (auto& k : x->exact) k = {};
        x->exact_set.reset();
        x->exact_recorded = nullptr;
        x->exact_recorded_any = false;
        if (!restore) x->reset_state();
    }
    return hr;
}
void STDMETHODCALLTYPE ExecuteCommandList_d(ID3D11DeviceContext* c, ID3D11CommandList* list, BOOL restore)
{
    Ctx* x = Get(c);
    FinishWorld(c, x);
    if (x && list) { x->post = {}; x->world_frame = ~uint64_t{0}; ++x->world_run; ++x->world_own_run; }
    if (x && list && x->world && !x->deferred && !t_inside && x->world_list_frame != g_frame) {
        // WHY log: a world drawn through recordings made on other threads cannot be redirected here.
        std::lock_guard<std::mutex> l(g_mu);
        auto it = g_lists.find(list);
        if (it != g_lists.end() && it->second.incarnation == IncarnationOf(list)) {
            const auto& w = it->second.writes;
            if (it->second.writes_unknown || std::find(w.begin(), w.end(), x->world->color) != w.end()) x->world_list_frame = g_frame;
        }
    }
    if (x && !x->deferred && x->coarse_rate && !t_inside) {
        // The immediate context's coarse shading would also apply to the list's draws (vrs11_test (ii)):
        // a list shades in blocks only what its own recording asked for.
        g_coarse_before_lists.fetch_add(1, std::memory_order_relaxed);
        CoarseApply(c, x, 0);
    }
    if (x && list && !t_inside && (x->deferred || hud_free::Wanted())) {
        // What the list's recording wrote: carried into an enclosing recording, or told to the HUD-free picture.
        std::vector<Id> writes;
        bool unknown = true;
        {
            std::lock_guard<std::mutex> l(g_mu);
            auto it = g_lists.find(list);
            if (it != g_lists.end() && it->second.incarnation == IncarnationOf(list)) { writes = it->second.writes; unknown = it->second.writes_unknown; }
        }
        if (x->deferred) {
            if (unknown) { x->list_writes_unknown = true; x->list_writes.clear(); }
            else for (Id w : writes) NoteListWrite(x, w);
        } else {
            hud_free::OnExecuteList(writes.data(), writes.size(), !unknown);
        }
    }
    if (const Id depth = list ? DepthReadsSource(x) : 0) {
        auto& reads = x->depth_reads;
        reads.Start(g_exact_frame.load(std::memory_order_relaxed), depth);
        reads.deferred = true;
    }
    if (x && !x->deferred && list) {
        std::unique_lock<std::mutex> l(g_mu);
        auto it = g_lists.find(list);
        if (it != g_lists.end() && it->second.incarnation != IncarnationOf(list)) it = g_lists.end();
        if (g_screen_enabled.load(std::memory_order_relaxed) && (it == g_lists.end() || !it->second.complete))
            g_lit.LostObservation();
        if (it != g_lists.end() && g_model.ShouldFireBeforeList(g_model.Summarize(it->second.events))) {
            g_model.MarkFired();
            Fire(c, l, Boundary::BeforeList);
            StopViewportJitterAfterScene();
            it = g_lists.find(list); // g_mu was let go during the callback
        }
        if (it != g_lists.end()) {
            size_t next_bytes = 0;
            const auto& events = it->second.events;
            const auto& bytes = it->second.cb_bytes;
            const auto& exact = it->second.draw_bytes;
            size_t next_exact = 0;
            const std::vector<ExactRange>* current_exact = nullptr;
            for (size_t i = 0; i < events.size(); ++i) {
                TrackPlacementWrite(events[i]);
                const uint32_t v = g_model.Feed(events[i]);
                if (next_exact < exact.size() && exact[next_exact].first == i) current_exact = exact[next_exact++].second.get();
                RememberSceneUploads(events[i], current_exact);
                NoteViewportDraw(events[i]);
                NoteDepthDirection(events[i]);
                if (events[i].kind == Kind::CbUpdate && next_bytes < bytes.size() && bytes[next_bytes].first == i) {
                    const auto& upload = bytes[next_bytes++].second;
                    SaveCameraUpload(events[i].resource, v, upload);
                }
            }
        }
    }
    // Drivers without native command lists replay the list through the immediate context's own
    // (hooked) functions: those calls are already in the model from the recording.
    {
        Inside in;
        O(c, x).execute(c, list, restore);
    }
    if (x && !restore) x->reset_state();
    else if (x && list) x->raster_depth_layer = {}; // unproved intervening writes in another recording
}

// ---------------------------------------------------------------- frame boundary
void ObserveBackbuffer(IDXGISwapChain* swap)
{
    Inside in;
    ComPtr<ID3D11Resource> buffer;
    g_backbuffer = SUCCEEDED(swap->GetBuffer(0, IID_PPV_ARGS(&buffer))) ? IdOf(buffer.Get()) : 0;
}

bool OnPresent(IDXGISwapChain* swap, PresentReceipt* receipt = nullptr)
{
    if (t_inside) return false;
    ID3D11Device* game = g_device_raw.load(std::memory_order_acquire);
    if (!game) return false;
    std::lock_guard<std::mutex> l(g_mu);
    if (swap != g_swap) {
        // A swap chain of the game's device (fullscreen toggles can recreate it) is adopted; one of
        // another device (a launcher window, a video, our own) is ignored.
        Inside in;
        ComPtr<ID3D11Device> d;
        if (FAILED(swap->GetDevice(IID_PPV_ARGS(&d))) || d.Get() != game) return false;
        g_swap = swap;
        g_screen.ResetPlacement();
        g_post_screen.ResetPlacement();
        g_placement.ClearPresentation();
        g_lit.Clear();
        g_recovery.Clear();
        g_lit_peer.store(0, std::memory_order_relaxed);
        DXGI_SWAP_CHAIN_DESC sd{};
        swap->GetDesc(&sd);
        g_model.SetSwapChainSize(sd.BufferDesc.Width, sd.BufferDesc.Height);
        ObserveBackbuffer(swap);
    }
    for (auto& entry : g_ctx)
        if (entry.second->game && entry.second->gen == g_device_gen.load() && !entry.second->deferred)
            FinishWorld(entry.first, entry.second.get());
    if (hud_free::Wanted()) { // the HUD-free picture: the frame ends here (learning, per-frame counts)
        Inside in;
        DXGI_SWAP_CHAIN_DESC hd{};
        swap->GetDesc(&hd);
        ComPtr<ID3D11DeviceContext> immediate;
        game->GetImmediateContext(&immediate);
        hud_free::OnPresent(immediate.Get(), uint64_t(g_backbuffer), hd.BufferDesc.Width, hd.BufferDesc.Height);
    }
    const auto previous = g_model.Current();
    const bool scale_search = g_discover_scale.load(std::memory_order_relaxed);
    if (scale_search != g_was_discovering_scale) {
        g_was_discovering_scale = scale_search;
        if (g_model.IsDisplayExtent(previous.color_info) || g_model.IsReducedPicture(previous.color_info)) {
            g_screen.ResetPlacement();
            g_post_screen.ResetPlacement();
        }
    }
    const bool reserved_before = g_screen.ReservesPixelRead();
    if (g_screen_enabled.load(std::memory_order_relaxed)) {
        Inside own;
        ComPtr<ID3D11DeviceContext> context;
        ComPtr<ID3D11Texture2D> screen;
        if (g_screen.AwaitingSamples() || g_lit.screen.AwaitingSamples() || g_post_screen.AwaitingSamples()) {
            game->GetImmediateContext(&context);
            swap->GetBuffer(0, IID_PPV_ARGS(&screen));
        }
        if (const char* outcome = g_screen.Present(context.Get(), screen.Get(), g_frame, g_model.SceneDrawsThisFrame(), g_model.FiredThisFrame(), receipt != nullptr, previous.screen_region)) {
            na::eventlog::Emit("native11_screen", "candidate=%s paired=%llu confirmed=%llu refuted=%llu",
                               outcome, (unsigned long long)g_screen.Paired(),
                               (unsigned long long)g_screen.Confirmed(), (unsigned long long)g_screen.Refuted());
        }
        if (g_lit.Learned()) {
            if (const char* outcome = g_lit.screen.Present(context.Get(), screen.Get(), g_frame, g_model.SceneDrawsThisFrame(), g_model.FiredThisFrame(), receipt != nullptr))
                na::eventlog::Emit("native11_lit_role", "frame=%llu reader=%s", (unsigned long long)g_frame, outcome);
        }
        if (g_post_screen.Active())
            g_post_screen.Present(context.Get(), screen.Get(), g_frame, g_model.SceneDrawsThisFrame(), false, receipt != nullptr);
    } else {
        if (g_screen.Active()) g_screen.ResetPlacement();
        if (g_post_screen.Active()) g_post_screen.ResetPlacement();
    }
    if (receipt) *receipt = {g_frame, g_frame_delivered || g_model.SceneDrawsThisFrame() != 0, g_frame_delivered,
                             g_eval_paused.load(std::memory_order_relaxed)};
    ++g_frame;
    g_exact_frame.fetch_add(1, std::memory_order_relaxed);
    ++g_stats.frames;
    if (CameraTracking()) LearnLargeCbWindows();
    // A swap chain's buffer is never held across a present: a game may let go of its swap chain at
    // any time, and a reference of ours would keep the old one alive. Fire takes it again.
    if (g_backbuffer && g_color_ref && IdOf(g_color_ref.Get()) == g_backbuffer) g_color_ref.Reset();
    // VARIANT: point/before-hud-on-screen
    // VARIANT: point/before-untested-hud
    // (frame_model.hpp) the model
    // is told which target is the screen; a picture drawn straight onto it has no read to wait for.
    g_model.SetScreenTarget(g_backbuffer);
    const bool check_lit = g_screen_enabled.load(std::memory_order_relaxed);
    const bool was_steering = g_lit.steer;
    if (previous.screen_region && g_screen.Verified(previous.color, previous.depth))
        g_region_started = g_frame;
    if (previous.screen_region && g_frame - g_region_started > 96) {
        // VARIANT: scene/pushed-without-screen-proof
        // No pixel-for-pixel match on the screen in 96 frames (a stretched picture never gives one).
        // DLAA is not held back for that: it goes ahead on this picture at its own size.
        na::eventlog::Emit("native11_picture_region", "result=pushed_without_screen_proof extent=%ux%u",
            previous.color_info.width, previous.color_info.height);
        g_model.PushScreenRegion();
    }
    g_model.EndFrame(check_lit && !previous.screen_region,
        check_lit && was_steering ? g_lit.Peer(g_model.Current().color) : 0, check_lit);
    const auto& picked = g_model.Current();
    if (picked.color != previous.color || picked.depth != previous.depth) g_post_screen.ResetPlacement();
    // WHY log: frames keep coming with a scene-like picture that cannot be used. Said once per reason
    // and extent, and again every 600 frames while it lasts, in the words the results file shows.
    if (const auto& refused = g_model.RefusedScene(); !picked.valid() && refused.why != FrameModel::NoScene::None) {
        static FrameModel::Refused said;
        static uint64_t said_at = 0;
        char why[224];
        switch (refused.why) {
        case FrameModel::NoScene::Multisampled:
            std::snprintf(why, sizeof why, "%s", "NeuralAlias cannot run DLAA on top of in-game anti-aliasing. Please disable all in-game AA.");
            break;
        default:
            std::snprintf(why, sizeof why,
                "The game's picture (%ux%u) was not found on the screen (%ux%u) in 96 frames: trying again shortly.",
                refused.width, refused.height, refused.screen_width, refused.screen_height);
            break;
        }
        SetReason(why);
        if (said.why != refused.why || said.width != refused.width || said.height != refused.height ||
            said.samples != refused.samples || g_frame - said_at >= 600) {
            said = refused;
            said_at = g_frame;
            na::eventlog::Emit("native11_no_scene", "frame=%llu picture=%ux%u samples=%u screen=%ux%u scene_vertices=%llu why=\"%s\"",
                static_cast<unsigned long long>(g_frame), refused.width, refused.height, refused.samples,
                refused.screen_width, refused.screen_height, static_cast<unsigned long long>(refused.vertices), why);
        }
    }
    if (picked.screen_region && (!previous.screen_region || previous.color != picked.color || previous.depth != picked.depth)) {
        g_region_started = g_frame;
        na::eventlog::Emit("native11_picture_region", "result=probing extent=%ux%u",
            picked.color_info.width, picked.color_info.height);
    }
    const auto order = g_model.CompletedLitOrder();
    g_lit.Finish(check_lit && (picked.color == order.late || picked.color == order.early) && picked.depth == order.depth
                 ? order : FrameModel::LitOrder{});
    if (g_trial_reader && check_lit) {
        const auto before = g_placement.Comparison().Current();
        const auto scope = g_lit.steer ? LightingScope() : referee::PlacementTrial::Scope{};
        static referee::TrialSample sample;
        referee::AdvancePlacement(g_placement, scope, bool(scope), bool(scope), g_frame_delivered_lit,
            g_recovery, g_frame - 1, GetTickCount64(), g_trial_reader,
            [&](const referee::TrialSample& seen, bool, bool) {
                return scope.OwnsPicture(seen.context);
            }, sample, scope && ParitySeen(swap, scope));
        const auto& trial = g_placement.Comparison();
        if (before != trial.Current())
            na::eventlog::Emit("native11_route_trial",
                "next_frame=%llu phase=%s region=%u stops=%.3f/%.3f/%.3f regional=%u verified_reduction=%.4f why=%s",
                (unsigned long long)g_frame, trial.Name(), trial.Region(),
                trial.Baseline(), trial.Candidate(), trial.Confirmation(), unsigned(trial.Regional()),
                trial.VerifiedReduction(), trial.Reason());
        if (before != trial.Current()) referee::EmitTrialEvidence("native11_route", g_frame, trial);
        g_lit.steer = g_lit.steer && g_placement.UsesCandidate();
    } else {
        g_placement.ClearPresentation();
        g_recovery.Clear();
    }
    if (was_steering != g_lit.steer)
        na::eventlog::Emit("native11_lit_role", "next_frame=%llu steering=%d", (unsigned long long)g_frame, int(g_lit.steer));
    g_lit_peer.store(g_lit.Learned() ? g_lit.Peer(picked.color) : 0, std::memory_order_relaxed);
    if (g_screen_enabled.load(std::memory_order_relaxed)) g_screen.SetScene(picked.color, picked.depth);
    g_adaptive_scene.store(g_screen.Adaptive(), std::memory_order_relaxed);
    if (reserved_before != g_screen.ReservesPixelRead()) {
        na::eventlog::Emit("native11_placement", "next_frame=%llu reader=%s",
                           static_cast<unsigned long long>(g_frame),
                           g_screen.ReservesPixelRead() ? "screen_proven" : "ordinary");
    }
    const auto& ms = g_model.GetStats();
    {
        // WHY log: one line whenever the place DLSS runs at changes kind, with what the frame showed.
        static int last_kind = -1;
        static bool last_on_screen = false;
        static uint32_t logged = 0;
        const int kind = g_model.LearnedPointKind();
        const bool selected = g_model.Current().valid();
        if (selected && (kind != last_kind || ms.on_screen != last_on_screen) && logged < 500) {
            last_kind = kind;
            last_on_screen = ms.on_screen;
            ++logged;
            const char* name = kind == 1 ? "before-read" : kind == 2 ? "before-hud-on-screen" : kind == 3 ? "before-untested-hud" : "none";
            const char* why =
                kind == 1 ? "the picture is read after its last scene draw" :
                kind == 2 ? "the picture is the screen target and is not read; the first draw into it without the scene depth ends the scene" :
                kind == 3 ? "the picture is the screen target, is not read, and every draw into it keeps the scene depth; the first draw after the last depth-tested one that does not test depth ends the scene" :
                !ms.on_screen ? "the picture is not read after its last scene draw and is not the screen target" :
                !ms.last_tested_draws ? "the picture is the screen target but no draw into it tested depth" :
                "the picture is the screen target but nothing was drawn into it after its last depth-tested draw (no HUD this frame)";
            na::eventlog::Emit("native11_point",
                               "frame=%llu point=%s on_screen=%d draws=%u scene_draws=%u depth_tested=%u reads=%u read_after_scene=%u "
                               "hud_draw_after_scene=%u untested_draw_after_tested=%u why=\"%s\"",
                               static_cast<unsigned long long>(g_frame), name, ms.on_screen ? 1 : 0, ms.last_draws,
                               ms.last_scene_draws, ms.last_tested_draws, ms.last_reads, ms.last_read_after_scene,
                               ms.last_draw_after_scene, ms.last_untested_after_tested, why);
        }
    }
    g_stats.unplaceable = ms.unplaceable;
    g_stats.contradictions = ms.contradictions;
    const bool scene_changed = g_stats.selection_changes != ms.selection_changes;
    g_stats.selection_changes = ms.selection_changes;
    size_t k = 0;
    const Selection& s = g_model.Current();
    if (s.camera_cb) g_cand[k++].store(s.camera_cb);
    for (const auto& c : g_model.CameraCandidates(kMaxCandidates))
        if (k < kMaxCandidates + 1 && c.first != s.camera_cb) g_cand[k++].store(c.first);
    while (k < kMaxCandidates + 1) g_cand[k++].store(0);
    g_scene_uploads.clear();
    g_split_draw = {};
    g_split_votes.clear();
    g_split_candidate.store(g_split_world, std::memory_order_relaxed);
    g_split_world = 0;
    // If measured motion already delivered the frame, failed Optional searches may sleep.
    // Keep Required mode searching and retry automatically after scene changes or at low cadence.
    if (CameraSetting() == CameraUse::Optional && !g_eval_paused.load(std::memory_order_relaxed)) {
        if (g_camera_suspended.load(std::memory_order_relaxed)) {
            if (ms.last_scene_draws && g_camera_retry_frames) --g_camera_retry_frames;
            if (!g_camera_retry_frames || scene_changed || (g_frame_fired && !g_frame_delivered)) {
                g_camera_failures = kCameraSearchBurst - 1; // one retry frame unless the camera returns
                g_camera_suspended.store(false, std::memory_order_relaxed);
                g_collect_camera.store(true, std::memory_order_relaxed);
                g_model.SetCameraTracking(true);
            }
        } else if (g_frame_found) g_camera_failures = 0;
        else if (g_frame_fired && g_frame_delivered && ++g_camera_failures >= kCameraSearchBurst) {
            g_camera_retry_frames = kCameraRetryFrames;
            g_camera_suspended.store(true, std::memory_order_relaxed);
            g_held.valid = false;
            g_cb_latest.clear();
            g_large_cb_windows.clear();
            g_model.SetCameraTracking(false);
            na::eventlog::Emit("native11_camera_budget", "frame=%llu retry_scene_frames=%u",
                               static_cast<unsigned long long>(g_frame), g_camera_retry_frames);
        }
    }
    // Learning the relative layout needs every scene range's upload for one frame.
    if (CameraTracking() && g_frame_fired && !g_collect_all) {
        const bool collecting = g_collect_camera.load(std::memory_order_relaxed); // how this frame's uploads were kept
        if (!collecting && g_frame_no_bytes && g_prev_found_collecting) {
            if (++g_rotation_misses >= 2) {
                g_collect_all = true;
                na::eventlog::Emit("native11_camera_bytes",
                                   "frame=%llu keep=all why=\"twice the camera was found on a frame that kept every upload and the next frame, keeping "
                                   "only last frame's highest-ranked buffers, had no bytes for any range its scene bound: the game binds a different "
                                   "buffer object each frame\" kept_buffers=%zu",
                                   static_cast<unsigned long long>(g_frame), g_cb_latest.size());
            }
        } else if (!collecting && g_frame_found) {
            g_rotation_misses = 0; // the short list held the camera: buffers keep their identity
        }
        g_prev_found_collecting = collecting && g_frame_found;
    }
    g_frame_fired = g_frame_found = g_frame_no_bytes = g_frame_delivered = false;
    g_frame_delivered_lit = false;
    g_depth_dir.EndFrame(); // the next frame's evidence starts empty; what is proven stays
    SetSceneHanded(false);
    TexturesOnPresent();
    g_collect_camera.store(CameraTracking() && (!g_camera.found || g_rel_learner.armed() || g_collect_all || g_camera_failures),
                           std::memory_order_relaxed);
    // Retire upload shadows not in the next scene's acquisition set. Draw-time
    // snapshots keep their own ownership until this frame is closed.
    if (g_cb_latest.size() > 512) {
        for (auto it = g_cb_latest.begin(); it != g_cb_latest.end();) {
            bool keep = it->first == s.camera_cb || it->first == g_split_candidate.load(std::memory_order_relaxed);
            for (const auto& candidate : g_cand) keep |= candidate.load() == it->first;
            if (!keep) it = g_cb_latest.erase(it); else ++it;
        }
    }
    g_sel_color.store(s.color, std::memory_order_relaxed);
    g_sel_layer.store(g_model.Layer(), std::memory_order_relaxed);
    g_sel_depth2.store(g_model.SecondDepth(), std::memory_order_relaxed);
    {
        const auto& watch = g_model.Watch();
        for (size_t i = 0; i < kWatch; ++i) g_watch[i].store(i < watch.size() ? watch[i] : 0, std::memory_order_relaxed);
        // WHY log: which target is the picture and by which rule, whenever that changes.
        const auto& note = g_model.LastChoice();
        static uint64_t last_signature = ~uint64_t{0};
        static uint32_t logged = 0;
        const uint64_t signature = (s.color * 1099511628211ull) ^ (note.most_drawn << 1) ^ (note.picture * 31) ^
                                   static_cast<uint64_t>(note.rule) ^ (reinterpret_cast<uintptr_t>(note.why) << 3);
        const bool report_selection = signature != last_signature && logged < 300 && !note.candidates.empty();
        const bool report_unselected = !s.valid() && !g_model.LastFlow().empty() && (g_frame % 120 == 1);
        if (report_selection || report_unselected) {
            char list[2400] = "";
            int at = 0;
            for (const auto& n : note.candidates) {
                if (at >= static_cast<int>(sizeof list) - 200) break;
                at += std::snprintf(list + at, sizeof list - static_cast<size_t>(at), " [%s%s fmt=%u draws=%llu multi=%llu first=%llu last=%llu first_read=%llu read_after=%llu last_read=%llu float=%d same_size=%d]",
                                    n.id == s.color ? "SELECTED " : "", n.id == note.picture ? "picture" : n.id == note.most_drawn ? "most-drawn" : "other",
                                    n.format, static_cast<unsigned long long>(n.draws), static_cast<unsigned long long>(n.multi_target_draws),
                                    static_cast<unsigned long long>(n.first_draw), static_cast<unsigned long long>(n.last_draw),
                                    static_cast<unsigned long long>(n.first_read), static_cast<unsigned long long>(n.read_after),
                                    static_cast<unsigned long long>(n.last_read), n.float_colour ? 1 : 0, n.same_size ? 1 : 0);
            }
            if (report_selection) {
                last_signature = signature;
                ++logged;
                na::eventlog::Emit("native11_scene", "frame=%llu rule=%s layered=%d why=\"%s\" candidates:%s",
                    static_cast<unsigned long long>(g_frame),
                    note.rule != FrameModel::PictureRule::AfterLayers ? "most-drawn" : note.second_depth ? "after-layers-second-depth" : "after-layers",
                    g_model.Layered() ? 1 : 0, note.why, list);
            }
            // WHY log: the order of that frame, so a renderer we have not met shows where its picture
            // is finished. Runs of draws into one target (d) and the reads we watch (r), in order;
            // #id tells two targets of the same format apart; z#id is the depth buffer bound.
            static uint32_t flows = 0, unselected_flows = 0;
            if ((report_selection && flows < 16) || (report_unselected && unselected_flows < 12)) {
                if (report_unselected) ++unselected_flows;
                else ++flows;
                // A long frame goes out as several lines (part=1, 2, ...): one event line holds about 4,000 characters.
                char flow[3600] = "";
                int fa = 0;
                unsigned part = 1;
                const auto emit = [&] {
                    na::eventlog::Emit("native11_flow", "frame=%llu steps=%u dropped=%u part=%u:%s", static_cast<unsigned long long>(g_frame),
                                       static_cast<unsigned>(g_model.LastFlow().size()), g_model.LastFlowDropped(), part++, flow);
                    fa = 0;
                    flow[0] = 0;
                };
                for (const auto& f : g_model.LastFlow()) {
                    if (fa >= static_cast<int>(sizeof flow) - 184) emit();
                    ResourceInfo ri{};
                    g_model.InfoOf(f.target, ri);
                    const unsigned tag = static_cast<unsigned>((f.target >> 4) & 0xfff);
                    const char* role = f.target == s.color ? "PICTURE" : f.target == g_backbuffer ? "screen" : f.target == note.most_drawn ? "most-drawn" : "";
                    if (f.read)
                        fa += std::snprintf(flow + fa, sizeof flow - fa, " [r #%03x fmt=%u %s]", tag, ri.format, role);
                    else
                    {
                        char z[64] = "no-depth";
                        if (f.depth) {
                            ResourceInfo depth_info{};
                            g_model.InfoOf(f.depth, depth_info);
                            std::snprintf(z, sizeof z, "z#%03x%s(%ux%u,s=%u)", static_cast<unsigned>((f.depth >> 4) & 0xfff),
                                f.depth == s.depth ? "=scene" : f.depth == note.second_depth ? "=second" : "",
                                depth_info.width, depth_info.height, depth_info.samples);
                        }
                        // v= all the run's vertices: a few per draw is a full-screen pass, many is geometry.
                        fa += std::snprintf(flow + fa, sizeof flow - fa, " [d #%03x fmt=%u %ux%u %s n=%u multi=%u tested=%u v=%llu %s]", tag, ri.format,
                                            ri.width, ri.height, z, f.draws, f.multi, f.tested, static_cast<unsigned long long>(f.vertices), role);
                    }
                }
                if (fa || part == 1) emit();
            }
        }
    }
    g_scene_vp_x = g_scene_vp_y = g_scene_vp_w = g_scene_vp_h = 0;
    // Next frame's jitter: the backend's sample, written into the main camera's uploads.
    g_jitter_written = false;
    // A request covers exactly the frame that starts here: without a new one (a missed or failed
    // evaluation), the next frame's camera is the game's own, unjittered.
    const bool region_ready = !s.screen_region || g_screen.Verified(s.color, s.depth);
    g_jitter_active = region_ready && g_jitter_requested && g_camera.found && CameraTracking();
    g_jitter = neural_alias::JitterSample{g_next_jitter[0], g_next_jitter[1]};
    // Viewport route: the same one-frame request, applied on the scene draws of the frame starting here.
    const uint32_t jittered_draws = g_vpj_frame_draws;
    g_vpj_frame_draws = 0;
    g_vpj_unshifted.fetch_add(g_vpj_frame_unshifted, std::memory_order_relaxed);
    g_vpj_frame_unshifted = 0;
    g_vpj_frame_shifted_vertices = g_vpj_frame_still_vertices = 0;
    g_vpj_frame_still_meshes = g_vpj_frame_still_untested = g_vpj_frame_shader_depth = g_vpj_frame_still_moved = 0;
    g_vpj_frame_overlay_moved = 0;
    for (uint8_t sc = 0; sc < kStillClasses; ++sc) {
        const int cls = CoverageClassOf(sc);
        if (cls >= 0 && g_vpj_frame_still_by[sc]) g_cov_left_total[cls].fetch_add(g_vpj_frame_still_by[sc], std::memory_order_relaxed);
    }
    for (auto& n : g_vpj_frame_still_by) n = 0;
    {   // once a minute, one frame's draws left off the jitter are named in the log (native11_left_draw)
        const uint64_t tick = GetTickCount64();
        const bool name = tick - g_left_name_at.load(std::memory_order_relaxed) >= 60000;
        if (name) g_left_name_at.store(tick, std::memory_order_relaxed);
        g_left_name_frame.store(name, std::memory_order_relaxed);
        g_left_named = 0;
        static uint64_t frames = 0;
        if (!g_cal_tsc0.load(std::memory_order_relaxed)) {
            LARGE_INTEGER q{};
            QueryPerformanceCounter(&q);
            g_cal_qpc0.store(uint64_t(q.QuadPart));
            g_cal_tsc0.store(__rdtsc());
        }
        const bool timed = (++frames & 7) == 0;
        g_time_frame.store(timed, std::memory_order_relaxed);
        if (timed) g_timed_frames.fetch_add(1, std::memory_order_relaxed);
    }
    g_vpj_seen_draw = g_vpj_mixed = false;
    g_vpj_executed_sample = 0;
    g_vpj_executed_expanded = false;
    g_vpj_last_frame_draws.store(jittered_draws, std::memory_order_relaxed);
    if (jittered_draws) g_vpj_frames.fetch_add(1, std::memory_order_relaxed);
    WorldRouteFrame(g_frame - 1);
    for (auto& entry : g_ctx) {
        auto& x = *entry.second;
        if (!x.world) continue;
        x.world->proof_current.store(g_screen.KnowsProof(x.world->proof), std::memory_order_relaxed);
        if (!g_world_level.load() || x.world->color != s.color || x.world->depth != s.depth) x.world.reset();
    }
    g_sel_depth.store(s.depth, std::memory_order_relaxed);
    g_sel_depth_w.store(s.depth_info.width, std::memory_order_relaxed);
    g_sel_depth_h.store(s.depth_info.height, std::memory_order_relaxed);
    uint64_t sample = 0;
    if (region_ready && g_vpj_route.load(std::memory_order_relaxed) && g_jitter_requested && s.valid() &&
        (g_next_jitter[0] != 0.f || g_next_jitter[1] != 0.f)) {
        std::memcpy(&sample, g_next_jitter, sizeof(sample));
    }
    g_vpj_sample.store(sample, std::memory_order_release);
    g_jitter_requested = false;
    const bool coarse = g_coarse.load(std::memory_order_relaxed) != 0;
    g_coarse_open.store(coarse, std::memory_order_relaxed); // the next frame's scene may be shaded in blocks
    if (coarse) g_coarse_frames.fetch_add(1, std::memory_order_relaxed);
    return true;
}

HRESULT STDMETHODCALLTYPE Present_d(IDXGISwapChain* s, UINT sync, UINT flags)
{
    const bool outer = !t_in_present;
    PresentReceipt receipt{};
    const bool observed = outer && !(flags & DXGI_PRESENT_TEST) && OnPresent(s, &receipt);
    t_in_present = true;
    const HRESULT hr = g_present(s, sync, flags);
    if (observed) ObservePresentResult(s, receipt.ordinal, hr == S_OK && !(flags & DXGI_PRESENT_DO_NOT_SEQUENCE));
    if (outer) t_in_present = false;
    return hr;
}
HRESULT STDMETHODCALLTYPE Present1_d(IDXGISwapChain1* s, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p)
{
    const bool outer = !t_in_present;
    PresentReceipt receipt{};
    const bool observed = outer && !(flags & DXGI_PRESENT_TEST) && OnPresent(s, &receipt);
    t_in_present = true;
    const HRESULT hr = g_present1(s, sync, flags, p);
    if (observed) ObservePresentResult(s, receipt.ordinal, hr == S_OK && !(flags & DXGI_PRESENT_DO_NOT_SEQUENCE) &&
        (!p || (!p->DirtyRectsCount && !p->pScrollRect)));
    if (outer) t_in_present = false;
    return hr;
}
HRESULT STDMETHODCALLTYPE ResizeBuffers_d(IDXGISwapChain* s, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags)
{
    BeforeResize(s);
    const HRESULT hr = g_resize(s, n, w, h, f, flags);
    AfterResize(s, hr);
    return hr;
}

void ReleaseResizeInputs(IDXGISwapChain* s)
{
    std::lock_guard<std::mutex> l(g_mu);
    if (s == g_swap) {
        g_screen.ResetPlacement();
        g_post_screen.ResetPlacement();
        g_placement.ClearPresentation();
        g_lit.Clear();
        g_recovery.Clear();
        g_lit_peer.store(0, std::memory_order_relaxed);
        g_adaptive_scene.store(false, std::memory_order_relaxed);
        for (auto& entry : g_ctx) {
            FinishWorld(entry.first, entry.second.get());
            entry.second->world.reset();
            entry.second->world_frame = ~uint64_t{0};
        }
        // Release everything that could hold a back buffer, or ResizeBuffers fails.
        g_color_ref.Reset();
        g_depth_ref.Reset();
        g_backbuffer = 0;
        g_model.Forget(g_model.Current().color); // the scene targets are usually recreated with the new size
        g_split_draw = {};
        g_split_votes.clear();
        g_sel_color.store(0);
        g_sel_layer.store(0);
        for (auto& w : g_watch) w.store(0);
        g_sel_depth.store(0);
        g_sel_depth2.store(0);
        g_vpj_sample.store(0);
    }
}
void UpdateResizeExtent(IDXGISwapChain* s, long result)
{
    std::lock_guard<std::mutex> l(g_mu);
    if (s == g_swap) {
        ObserveBackbuffer(s);
        if (FAILED(result)) return;
        DXGI_SWAP_CHAIN_DESC sd{};
        s->GetDesc(&sd);
        g_model.SetSwapChainSize(sd.BufferDesc.Width, sd.BufferDesc.Height);
    }
}

// ---------------------------------------------------------------- install
LRESULT CALLBACK DummyProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

bool HookCtx(ID3D11DeviceContext* ctx, int set, bool deferred)
{
    Originals& o = g_o[set];
    struct H { uint32_t slot; void* detour; void** out; };
    std::vector<H> hooks = {
        {kVSSetConstantBuffers, reinterpret_cast<void*>(&VSSetConstantBuffers_d), reinterpret_cast<void**>(&o.vs_cb)},
        {kPSSetConstantBuffers, reinterpret_cast<void*>(&PSSetConstantBuffers_d), reinterpret_cast<void**>(&o.ps_cb)},
        {kPSSetShader, reinterpret_cast<void*>(&PSSetShader_d), reinterpret_cast<void**>(&o.ps_shader)},
        {kPSSetShaderResources, reinterpret_cast<void*>(&PSSetShaderResources_d), reinterpret_cast<void**>(&o.ps_srv)},
        {kCSSetShaderResources, reinterpret_cast<void*>(&CSSetShaderResources_d), reinterpret_cast<void**>(&o.cs_srv)},
        {kVSSetShaderResources, reinterpret_cast<void*>(&VSSetShaderResources_d), reinterpret_cast<void**>(&o.vs_srv)},
        {kDrawIndexed, reinterpret_cast<void*>(&DrawIndexed_d), reinterpret_cast<void**>(&o.draw_indexed)},
        {kDraw, reinterpret_cast<void*>(&Draw_d), reinterpret_cast<void**>(&o.draw)},
        {kDrawIndexedInstanced, reinterpret_cast<void*>(&DrawIndexedInstanced_d), reinterpret_cast<void**>(&o.draw_indexed_instanced)},
        {kDrawInstanced, reinterpret_cast<void*>(&DrawInstanced_d), reinterpret_cast<void**>(&o.draw_instanced)},
        {kDrawIndexedInstancedIndirect, reinterpret_cast<void*>(&DrawIndexedInstancedIndirect_d), reinterpret_cast<void**>(&o.draw_indexed_indirect)},
        {kDrawInstancedIndirect, reinterpret_cast<void*>(&DrawInstancedIndirect_d), reinterpret_cast<void**>(&o.draw_indirect)},
        {kDrawAuto, reinterpret_cast<void*>(&DrawAuto_d), reinterpret_cast<void**>(&o.draw_auto)},
        {kDispatch, reinterpret_cast<void*>(&Dispatch_d), reinterpret_cast<void**>(&o.dispatch)},
        {kDispatchIndirect, reinterpret_cast<void*>(&DispatchIndirect_d), reinterpret_cast<void**>(&o.dispatch_indirect)},
        {kBegin, reinterpret_cast<void*>(&Begin_d), reinterpret_cast<void**>(&o.begin)},
        {kEnd, reinterpret_cast<void*>(&End_d), reinterpret_cast<void**>(&o.end)},
        {kMap, reinterpret_cast<void*>(&Map_d), reinterpret_cast<void**>(&o.map)},
        {kUnmap, reinterpret_cast<void*>(&Unmap_d), reinterpret_cast<void**>(&o.unmap)},
        {kOMSetRenderTargets, reinterpret_cast<void*>(&OMSetRenderTargets_d), reinterpret_cast<void**>(&o.om_rt)},
        {kOMSetRenderTargetsAndUAVs, reinterpret_cast<void*>(&OMSetRenderTargetsAndUAVs_d), reinterpret_cast<void**>(&o.om_rt_uav)},
        {kOMSetDepthStencilState, reinterpret_cast<void*>(&OMSetDepthStencilState_d), reinterpret_cast<void**>(&o.om_dss)},
        {kRSSetViewports, reinterpret_cast<void*>(&RSSetViewports_d), reinterpret_cast<void**>(&o.rs_vp)},
        {kCopySubresourceRegion, reinterpret_cast<void*>(&CopySubresourceRegion_d), reinterpret_cast<void**>(&o.copy_region)},
        {kCopyResource, reinterpret_cast<void*>(&CopyResource_d), reinterpret_cast<void**>(&o.copy_resource)},
        {kUpdateSubresource, reinterpret_cast<void*>(&UpdateSubresource_d), reinterpret_cast<void**>(&o.update)},
        {kClearRenderTargetView, reinterpret_cast<void*>(&ClearRenderTargetView_d), reinterpret_cast<void**>(&o.clear_rtv)},
        {kClearDepthStencilView, reinterpret_cast<void*>(&ClearDepthStencilView_d), reinterpret_cast<void**>(&o.clear_dsv)},
        {kResolveSubresource, reinterpret_cast<void*>(&ResolveSubresource_d), reinterpret_cast<void**>(&o.resolve)},
        {kClearState, reinterpret_cast<void*>(&ClearState_d), reinterpret_cast<void**>(&o.clear_state)},
    };
    if (deferred) hooks.push_back({kFinishCommandList, reinterpret_cast<void*>(&FinishCommandList_d), reinterpret_cast<void**>(&o.finish)});
    else hooks.push_back({kExecuteCommandList, reinterpret_cast<void*>(&ExecuteCommandList_d), reinterpret_cast<void**>(&o.execute)});
    ComPtr<ID3D11DeviceContext1> c1;
    if (SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&c1)))) {
        hooks.push_back({kCopyRegion1, reinterpret_cast<void*>(&CopyRegion1_d), reinterpret_cast<void**>(&o.copy1)});
        hooks.push_back({kDiscardResource, reinterpret_cast<void*>(&DiscardResource_d), reinterpret_cast<void**>(&o.discard_resource)});
        hooks.push_back({kDiscardView, reinterpret_cast<void*>(&DiscardView_d), reinterpret_cast<void**>(&o.discard_view)});
        hooks.push_back({kDiscardView1, reinterpret_cast<void*>(&DiscardView1_d), reinterpret_cast<void**>(&o.discard_view1)});
        hooks.push_back({kClearView, reinterpret_cast<void*>(&ClearView_d), reinterpret_cast<void**>(&o.clear_view)});
        hooks.push_back({kSwapContextState, reinterpret_cast<void*>(&SwapState_d), reinterpret_cast<void**>(&o.swap_state)});
        hooks.push_back({kUpdateSubresource1, reinterpret_cast<void*>(&UpdateSubresource1_d), reinterpret_cast<void**>(&o.update1)});
        hooks.push_back({kVSSetConstantBuffers1, reinterpret_cast<void*>(&VSSetConstantBuffers1_d), reinterpret_cast<void**>(&o.vs_cb1)});
        hooks.push_back({kPSSetConstantBuffers1, reinterpret_cast<void*>(&PSSetConstantBuffers1_d), reinterpret_cast<void**>(&o.ps_cb1)});
    }
    for (const H& h : hooks)
        if (!na::hooking::PrepareVTableSlot(ctx, h.slot, h.detour, h.out)) return false;
    return true;
}

bool InstallHooks(bool host_owns_present)
{
    if (!na::hooking::Initialize()) return SetReason("The hooking core failed to initialise."), false;
    // A private device and swap chain, only to reach d3d11.dll's and dxgi.dll's functions.
    WNDCLASSW wc{};
    wc.lpfnWndProc = DummyProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"NeuralAliasNative11Probe";
    RegisterClassW(&wc);
    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    if (!wnd) return SetReason("Could not create the probe window."), false;
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 64;
    sd.BufferDesc.Height = 64;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = wnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> imm;
    ComPtr<IDXGISwapChain> swap;
    const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &sd, &swap, &dev, nullptr, &imm);
    if (FAILED(hr))
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &sd, &swap, &dev, nullptr, &imm);
    if (FAILED(hr)) {
        DestroyWindow(wnd);
        return SetReason("Could not create the probe DX11 device."), false;
    }
    ComPtr<ID3D11DeviceContext> def;
    dev->CreateDeferredContext(0, &def);
    ComPtr<IDXGISwapChain1> swap1;
    swap.As(&swap1);
    bool ok = na::hooking::BeginInstallWait("native11");
    ok = ok && na::hooking::PrepareVTableSlot(dev.Get(), kCreatePixelShader, reinterpret_cast<void*>(&CreatePixelShader_d), reinterpret_cast<void**>(&g_create_ps));
    ok = ok && HookCtx(imm.Get(), 0, false);
    ok = ok && (!def || HookCtx(def.Get(), 1, true));
    if (!host_owns_present) {
        ok = ok && na::hooking::PrepareVTableSlot(swap.Get(), kPresent, reinterpret_cast<void*>(&Present_d), reinterpret_cast<void**>(&g_present));
        ok = ok && na::hooking::PrepareVTableSlot(swap.Get(), kResizeBuffers, reinterpret_cast<void*>(&ResizeBuffers_d), reinterpret_cast<void**>(&g_resize));
        ok = ok && (!swap1 || na::hooking::PrepareVTableSlot(swap1.Get(), kPresent1, reinterpret_cast<void*>(&Present1_d), reinterpret_cast<void**>(&g_present1)));
    }
    if (ok) ok = na::hooking::CommitInstall();
    else na::hooking::AbandonInstall();
    if (!def) g_o[1] = g_o[0];
    swap.Reset();
    def.Reset();
    imm.Reset();
    dev.Reset();
    DestroyWindow(wnd);
    if (!ok) SetReason("Installing the DX11 hooks failed.");
    return ok;
}

} // namespace

bool PrepareWorldRaster(void* context, const SceneFrame& f, uint32_t level, uint32_t w, uint32_t h)
{
    if (f.world_level) return false; // already small
    const auto hold = [](WorldHold why) { g_world_frame_hold.store(uint8_t(why), std::memory_order_relaxed); return false; };
    if (!level || !w || !h) return hold(WorldHold::NoRenderSize);
    if (!f.screen_proof || !f.scaled_reader.target) return hold(WorldHold::Reader);
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_ctx.find(static_cast<ID3D11DeviceContext*>(context));
    if (it == g_ctx.end() || !g_swap || level != g_world_level.load()) return hold(WorldHold::Preparing);
    if (f.screen_proof != g_screen.Proof()) return hold(WorldHold::Reader);
    if (f.color.resource == g_backbuffer) return hold(WorldHold::OnScreen);
    if (!g_model.IsDisplayExtent(g_model.Current().color_info)) return hold(WorldHold::OtherSize);
    if (f.color.active_x || f.color.active_y || f.depth.active_x || f.depth.active_y ||
        f.color.width != f.color.active_w || f.color.height != f.color.active_h ||
        f.depth.width != f.color.width || f.depth.height != f.color.height || f.color.mip || f.depth.mip ||
        f.color.array_slice || f.depth.array_slice || w >= f.color.width || h >= f.color.height) return hold(WorldHold::PictureKind);
    auto& x = *it->second;
    auto job = x.world;
    if (job && job->level == level && job->proof == f.screen_proof &&
        job->color == f.color.resource && job->depth == f.depth.resource && job->width == w && job->height == h) {
        return !job->refused.load(std::memory_order_acquire);
    }
    ComPtr<ID3D11Resource> color = static_cast<ID3D11Resource*>(f.color.d3d11_resource);
    ComPtr<ID3D11Resource> depth = static_cast<ID3D11Resource*>(f.depth.d3d11_resource);
    job = std::make_shared<Ctx::WorldJob>();
    job->color = f.color.resource; job->depth = f.depth.resource;
    job->color_format = DXGI_FORMAT(f.color.view_format); job->depth_format = DXGI_FORMAT(f.depth.view_format);
    job->last_receipt = f.frame;
    job->proof = f.screen_proof; job->level = level; job->width = w; job->height = h;
    if (!background::Submit([job, color, depth] {
        OwnWorkScope own;
        ComPtr<ID3D11Device> device; color->GetDevice(&device);
        D3D11_RENDER_TARGET_VIEW_DESC cv{}; cv.Format = job->color_format; cv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        D3D11_DEPTH_STENCIL_VIEW_DESC dv{}; dv.Format = job->depth_format; dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11RenderTargetView> rtv; ComPtr<ID3D11DepthStencilView> dsv;
        const bool pictures = SUCCEEDED(device->CreateRenderTargetView(color.Get(), &cv, &rtv)) &&
            SUCCEEDED(device->CreateDepthStencilView(depth.Get(), &dv, &dsv)) && job->raster.Prepare(rtv.Get(), dsv.Get(), job->width, job->height);
        // Without the Referee's eye nothing drawn small could be judged: no eye, no small world.
        const bool ok = pictures && job->probe.Prepare(device.Get(), job->raster.Color(), job->color_format);
        job->ready.store(ok, std::memory_order_release);
        job->refused.store(!ok, std::memory_order_release);
        na::eventlog::Emit("native11_world_raster", "level=%u prepared=%d pictures=%d render=%ux%u", job->level, int(ok), int(pictures), job->width, job->height);
    })) return hold(WorldHold::Preparing);
    FinishWorld(it->first, &x);
    x.world_inputs.scanned = false;
    x.world_inputs_seen.clear();
    x.world_inputs_new = false;
    x.world = std::move(job);
    x.world_frame = x.world_small_frame = x.world_color_clear = x.world_depth_clear = ~uint64_t{0};
    return true;
}

void NoteWorldHold(WorldHold why) { g_world_frame_hold.store(uint8_t(why), std::memory_order_relaxed); }

const char* WorldHoldText(WorldHold why)
{
    switch (why) {
    case WorldHold::None: return "";
    case WorldHold::Preparing: return "the smaller pictures are being prepared and the game's draws are being counted";
    case WorldHold::HelperProgram: return "DLSS runs in the separate 64-bit helper program for this 32-bit game, which cannot take a smaller picture yet";
    case WorldHold::NoRenderSize: return "DLSS gave no render size for this level";
    case WorldHold::Reader: return "the draw that puts the game's 3D picture on screen has not been proven yet";
    case WorldHold::OnScreen: return "the game draws its 3D picture straight onto the screen, with no later step to hand the full-size picture to";
    case WorldHold::OtherSize: return "the game's 3D picture is not the size of the screen (the game is already scaling it itself)";
    case WorldHold::PictureKind: return "the game's 3D picture is of a kind that has no small twin (several samples per pixel, part of a larger picture, or an unusual depth format)";
    case WorldHold::NoWorldDraws: return "no draw into the game's 3D picture was seen on its main drawing context";
    case WorldHold::RecordedLists: return "the game records its 3D drawing on other threads and plays it back, which cannot be redirected yet";
    case WorldHold::SeveralTargets: return "the game's 3D draws write several pictures at once (deferred shading), which cannot be redirected yet";
    case WorldHold::Viewports: return "the game's 3D draws use several viewports at once";
    case WorldHold::Uavs: return "the game's 3D draws write through unordered-access views";
    case WorldHold::Predicated: return "the game's 3D draws are predicated on earlier visibility results";
    case WorldHold::FullSizeInput: return "a 3D draw reads a full-size picture that cannot be made small";
    case WorldHold::Interleaved: return "too many draws that must stay full size (visibility queries, other pictures) sit between the game's 3D draws";
    case WorldHold::PictureCheck: return "the Referee saw a wrong picture with every way of drawing this game's 3D picture small";
    case WorldHold::Evaluator: return "DLSS refused the smaller picture three times";
    default: return "?";
    }
}
WorldRouteStatus WorldRoute()
{
    std::lock_guard<std::mutex> lock(g_mu);
    return g_route.status;
}

void SetScreenCheck(bool on, uint32_t discover_scale)
{
    g_world_level.store(on && discover_scale <= 3 ? discover_scale : 0);
    g_discover_scale.store(on && discover_scale, std::memory_order_relaxed);
    g_screen_enabled.store(on, std::memory_order_relaxed);
}

void RejectScreenProof(uint64_t proof)
{
    if (!proof || !g_screen_enabled.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(g_mu);
    for (auto& entry : g_ctx)
        if (entry.second->world && entry.second->world->proof == proof)
            entry.second->world->proof_current.store(false, std::memory_order_relaxed);
    if (g_lit.Revoke(proof, g_frame) || g_screen.RejectProof(proof, g_frame))
        na::eventlog::Emit("native11_screen", "candidate=replaced_after_delivery proof=%llu",
                           static_cast<unsigned long long>(proof));
}
bool ObservePresent(void* swapchain, PresentReceipt* receipt)
{
    return OnPresent(static_cast<IDXGISwapChain*>(swapchain), receipt);
}
void ObservePresentResult(void* swapchain, uint64_t ordinal, bool accepted)
{
    if (!g_screen_enabled.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(g_mu);
    if (swapchain != g_swap) return;
    g_screen.PresentResult(ordinal, accepted);
    g_post_screen.PresentResult(ordinal, accepted);
    g_lit.screen.PresentResult(ordinal, accepted);
}
void BeforeResize(void* swapchain) { ReleaseResizeInputs(static_cast<IDXGISwapChain*>(swapchain)); }
void AfterResize(void* swapchain, long result) { UpdateResizeExtent(static_cast<IDXGISwapChain*>(swapchain), result); }
OwnWorkScope::OwnWorkScope() : was_(t_inside) { t_inside = true; }
OwnWorkScope::~OwnWorkScope() { t_inside = was_; }

void SetParitySignal(ParitySignal seen) { g_parity_signal.store(seen, std::memory_order_relaxed); }
bool Install(void* device, SceneCallback cb, void* user, bool host_owns_present, TrialReader trial_reader)
{
    std::lock_guard<std::mutex> g(g_install_mu);
    {
        std::lock_guard<std::mutex> l(g_mu);
        g_cb = cb;
        g_cb_user = user;
        g_trial_reader = trial_reader;
    }
    if (device) {
        ID3D11Device* was = g_device_raw.exchange(static_cast<ID3D11Device*>(device), std::memory_order_acq_rel);
        if (was != device) {
            const uint32_t gen = g_device_gen.fetch_add(1, std::memory_order_acq_rel) + 1;
            // WHY log: a game that makes more than one device is named, with what changes for us.
            na::eventlog::Emit("native11_device", "generation=%u first=%d why=\"%s\"", gen, was ? 0 : 1,
                               was ? "the game made another device: it is now the one watched, and every context is asked again whose it is"
                                   : "the game's first device");
        }
    }
    if (g_installed.load()) return true;
    if (!InstallHooks(host_owns_present)) return false;
    g_installed.store(true);
    return true;
}

bool IsGameWork(void* context) { return Get(static_cast<ID3D11DeviceContext*>(context)) != nullptr; }
SamplerShadow* SamplerShadowOf(ID3D11DeviceContext* context)
{
    Ctx* x = Get(context);
    return x ? &x->sampler_shadow : nullptr;
}
PsShadow* TextureShadowOf(ID3D11DeviceContext* c)
{
    Ctx* x = Get(c);
    return x ? &x->ps_shadow : nullptr;
}
bool IsOwnWork() { return t_inside; }
void Uninstall()
{
    // Hooks stay (unhooking under a running game is unsafe); the provider goes idle: no game device,
    // no consumer, nothing held.
    std::lock_guard<std::mutex> g(g_install_mu);
    g_device_raw.store(nullptr, std::memory_order_release);
    std::lock_guard<std::mutex> l(g_mu);
    g_cb = nullptr;
    g_cb_user = nullptr;
    g_trial_reader = nullptr;
    g_screen = {};
    g_post_screen = {};
    g_placement.ClearPresentation();
    g_lit.Clear();
    g_recovery.Clear();
    g_lit_peer.store(0, std::memory_order_relaxed);
    g_adaptive_scene.store(false, std::memory_order_relaxed);
    g_world_level.store(0);
    g_route = {};
    g_world_live.store(false);
    g_world_reference.store(false);
    g_world_frame_hold.store(0);
    g_screen_enabled.store(false, std::memory_order_relaxed);
    g_discover_scale.store(false, std::memory_order_relaxed);
    g_was_discovering_scale = false;
    g_swap = nullptr;
    g_backbuffer = 0;
    g_collect_all = false;
    g_camera_suspended.store(false, std::memory_order_relaxed);
    g_eval_paused.store(false, std::memory_order_relaxed);
    g_camera_failures = g_camera_retry_frames = 0;
    g_frame_delivered = false;
    g_frame_delivered_lit = false;
    g_cb_latest.clear();
    g_scene_uploads.clear();
    g_large_cb_windows.clear();
    g_model.SetCameraTracking(CameraTracking());
    g_collect_camera.store(CameraTracking(), std::memory_order_relaxed);
    g_twin_wait_until = 0;
    g_rotation_misses = 0;
    g_frame_fired = g_frame_found = g_frame_no_bytes = g_prev_found_collecting = false;
    SetSceneHanded(false);
    g_color_ref.Reset();
    g_depth_ref.Reset();
    g_lists.clear();
    g_ctx.clear();
    g_sel_color.store(0);
    g_sel_layer.store(0);
    for (auto& w : g_watch) w.store(0);
    g_sel_depth.store(0);
    g_sel_depth2.store(0);
    g_vpj_sample.store(0);
    g_depth_dir.Reset();
}

void SetJitterRoute(JitterRoute route)
{
    g_vpj_route.store(route == JitterRoute::Viewport, std::memory_order_relaxed);
    if (route != JitterRoute::Viewport) g_vpj_sample.store(0, std::memory_order_release); // bound copies restore at the next draw
}

void SetDepthReads(bool enabled)
{
    if (g_depth_reads_enabled.exchange(enabled) != enabled)
        na::eventlog::Emit("depth_reads", "variant=depth-reads/twin api=dx11 state=%s why=\"%s\"",
                           enabled ? "filling" : "off", enabled ? "waiting for qualified raster depth" : "disabled");
}

static void ResetCameraAcquisition()
{
    g_camera_failures = g_camera_retry_frames = 0;
    g_camera_suspended.store(false, std::memory_order_relaxed);
    g_held.valid = false;
    g_cb_latest.clear();
    g_scene_uploads.clear();
    g_large_cb_windows.clear();
    const bool tracking = CameraTracking();
    g_collect_camera.store(tracking, std::memory_order_relaxed);
    g_model.SetCameraTracking(tracking);
}

void SetEvaluationPaused(bool paused)
{
    if (g_eval_paused.exchange(paused, std::memory_order_relaxed) == paused) return;
    std::lock_guard<std::mutex> l(g_mu);
    ResetCameraAcquisition();
    if (paused) {
        g_vpj_sample.store(0, std::memory_order_release);
        g_jitter_active = g_jitter_requested = false;
    }
}

void SetCameraUse(CameraUse use)
{
    const auto next = static_cast<uint8_t>(use);
    if (g_camera_use.exchange(next, std::memory_order_relaxed) == next) return;
    std::lock_guard<std::mutex> l(g_mu);
    ResetCameraAcquisition();
    na::eventlog::Emit("native11_camera_use", "frame=%llu use=%s", static_cast<unsigned long long>(g_frame),
                       use == CameraUse::Off ? "off" : use == CameraUse::Optional ? "optional" : "required");
}

void SetStillMeshJitter(bool asked) { g_vpj_still_asked.store(asked, std::memory_order_relaxed); }
std::atomic<int> g_vpj_overlays_forced{-1}; // tests: -1 the Referee decides, 0 the earlier rule, 1 overlays
void ForceSceneOverlayJitterForTests(int forced) { g_vpj_overlays_forced.store(forced, std::memory_order_relaxed); }
void SetSceneOverlayJitter(bool on, const char* why)
{
    if (const int forced = g_vpj_overlays_forced.load(std::memory_order_relaxed); forced >= 0) { on = forced != 0; why = "a test chose it"; }
    if (g_vpj_overlays.exchange(on, std::memory_order_relaxed) != on)
        na::eventlog::Emit("jitter_variant", "api=dx11 variant=%s why=\"%s\"", on ? "jitter/scene-overlays" : "jitter/depth-tested-only", why ? why : "");
}
bool SceneOverlayJitter() { return g_vpj_overlays.load(std::memory_order_relaxed); }
uint32_t UnjitteredSceneMeshes() { return g_vpj_frame_still_meshes; }
void LeftByClass(uint64_t (&out)[na::referee::kLeftClasses])
{
    for (auto& n : out) n = 0;
    for (uint8_t s = 0; s < kStillClasses; ++s) {
        const int c = CoverageClassOf(s);
        if (c >= 0) out[c] += g_vpj_frame_still_by[s];
    }
}
void SetCoverage(uint32_t mask, int trial, uint64_t decisions, int last_class, bool last_kept)
{
    g_cov_mask.store(mask, std::memory_order_release);
    g_cov_trial.store(trial, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_cov_m);
    if (decisions == g_cov_decisions) return;
    g_cov_decisions = decisions;
    // The trial's draw signatures: put back with their class, they stay on the game's grid from now on
    // (a later trial of the same class moves only the others); kept, the class mask moves them.
    if (!last_kept && last_class >= 0) g_cov_still_sigs.insert(g_cov_trial_sigs.begin(), g_cov_trial_sigs.end());
    na::eventlog::Emit("native11_coverage", "decision=%llu class=%s %s signatures=%zu still_signatures=%zu moved_draws=%llu left_by_signature=%llu",
                       static_cast<unsigned long long>(decisions), na::referee::LeftClassName(unsigned(last_class)),
                       last_kept ? "kept" : "put_back", g_cov_trial_sigs.size(), g_cov_still_sigs.size(),
                       static_cast<unsigned long long>(g_cov_moved.load()), static_cast<unsigned long long>(g_cov_kept_still.load()));
    g_cov_trial_sigs.clear();
}
CoverageStats GetCoverageStats()
{
    CoverageStats s;
    s.mask = g_cov_mask.load();
    s.trial = g_cov_trial.load();
    s.moved_draws = g_cov_moved.load();
    s.left_by_signature = g_cov_kept_still.load();
    for (unsigned c = 0; c < na::referee::kLeftClasses; ++c) s.left_total[c] = g_cov_left_total[c].load();
    std::lock_guard<std::mutex> lock(g_cov_m);
    s.still_signatures = g_cov_still_sigs.size();
    return s;
}
uint32_t OverlayMeshesMoved() { return g_vpj_frame_overlay_moved; }

void SetSceneCoarseShading(uint32_t rate) { g_coarse.store(rate, std::memory_order_relaxed); }
bool CoarseShadingAvailable(const char** why)
{
    return coarse::Available(g_device_raw.load(std::memory_order_acquire), why);
}
CoarseShadingStats GetCoarseShadingStats()
{
    CoarseShadingStats s;
    s.rate = g_coarse.load(std::memory_order_relaxed);
    s.draws = g_coarse_draws.load(std::memory_order_relaxed);
    s.sets = g_coarse_sets.load(std::memory_order_relaxed);
    s.restores = g_coarse_restores.load(std::memory_order_relaxed);
    s.failures = g_coarse_failures.load(std::memory_order_relaxed);
    s.before_lists = g_coarse_before_lists.load(std::memory_order_relaxed);
    s.frames = g_coarse_frames.load(std::memory_order_relaxed);
    return s;
}

void SetNextJitter(float x_px, float y_px)
{
    std::lock_guard<std::mutex> l(g_mu);
    g_next_jitter[0] = x_px;
    g_next_jitter[1] = y_px;
    g_jitter_requested = true;
}

ProviderStats Stats()
{
    std::lock_guard<std::mutex> l(g_mu);
    ProviderStats stats = g_stats;
    stats.screen_confirmed = g_screen.Confirmed();
    stats.screen_refuted = g_screen.Refuted();
    stats.screen_pairs = g_screen.Paired();
    stats.screen_delivered = g_screen.Delivered();
    stats.scaled_compositor_confirmed = g_post_screen.Confirmed();
    stats.scaled_compositor_refuted = g_post_screen.Refuted();
    stats.scaled_compositor_draws = g_post_screen.Delivered();
    return stats;
}

int FormatStatus(char* out, unsigned long n)
{
    const ProviderStats s = Stats();
    float told[2]{};
    const uint64_t told_bits = g_vpj_told.load(std::memory_order_relaxed);
    std::memcpy(told, &told_bits, sizeof told);
    camera::RelativeLayout layout;
    bool layout_learned = false;
    bool scene_layered = false, scene_float = false;
    uint64_t scene_layered_frames = 0;
    uint32_t scene_format = 0;
    uint32_t color_samples = 0, depth_samples = 0;
    int is_backbuffer = -1;
    bool keep_all = false;
    FrameModel::Stats placement;
    DepthCompare depth_proven = DepthCompare::None;
    DepthEvidence depth_seen;
    uint64_t depth_flips = 0;
    {
        std::lock_guard<std::mutex> l(g_mu);
        depth_proven = g_depth_dir.Proven();
        depth_seen = g_depth_dir.Last();
        depth_flips = g_depth_dir.Flips();
        layout = g_rel_layout;
        layout_learned = g_rel_learned;
        scene_layered = g_model.Layered();
        placement = g_model.GetStats();
        keep_all = g_collect_all;
        scene_layered_frames = placement.layered_frames;
        const Selection& selection = g_model.Current();
        scene_format = selection.color_info.format;
        scene_float = selection.color_info.float_colour;
        if (selection.valid()) {
            color_samples = selection.color_info.samples;
            depth_samples = selection.depth_info.samples;
            if (g_backbuffer) is_backbuffer = selection.color == g_backbuffer ? 1 : 0;
        }
    }
    const double tsc_ms = TscPerMs();
    const auto cpu_ms = [&](const std::atomic<uint64_t>& t) { const uint64_t f = g_timed_frames.load(); return tsc_ms > 0 && f ? double(t.load()) / tsc_ms / double(f) : 0.0; };
    const int head = std::snprintf(out, n,
                         "native11 frames=%llu fired=%llu missed=%llu unplaceable=%llu contradictions=%llu selection_changes=%llu "
                         "camera_found=%llu camera_missing=%llu camera_held=%llu jitter_written=%llu "
                         "pose{native=%llu legacy=%llu generic=%llu no_packet=%llu extent=%llu vp_repeat=%llu origin=%llu vp_shape=%llu split=%llu "
                         "started=%llu continued=%llu no_inverse=%llu} "
                         "split_rejected{no_votes=%llu no_projection=%llu few_draws=%llu no_majority=%llu small_share=%llu} "
                         "start_rejected{no_candidate=%llu waiting=%llu inverse_game=%llu position_game=%llu} "
                         "relative_layout{%s world=%d/%u@%u,%u projection=%d/%u@%u learned=%llu no_witness=%llu ambiguous=%llu} "
                         "viewport_jitter{route=%d on=%d frames=%llu scene_draws=%llu last_frame_draws=%u sets=%llu restores=%llu unknown_viewports=%llu mixed=%llu} "
                         "exact_ranges{reads=%llu failed=%llu draws=%llu} "
                         "camera_path{steps=%llu reversals=%llu jumps=%llu} "
                         "jitter_told{px=%.4f,%.4f unshifted_colour_draws=%llu} cpu_ms_per_frame{camera=%.3f draw_ours=%.3f draw_driver=%.3f timed_frames=%llu} "
                         "scene{picture=%s layered_frames=%llu colour_fmt=%u colour_float=%d} "
                         "scene_boundary{selected=%llu draws=%u scene_draws=%u reads=%u after_scene_read=%u after_scene_draw=%u "
                         "learned_read=%u learned_hud=%u backbuffer=%d color_samples=%u depth_samples=%u "
                         "depth_tested=%u after_tested_untested=%u learned_untested_hud=%u fired_hud=%llu fired_untested_hud=%llu} camera_bytes{keep_all=%d twin_across_ranges=%llu}%s%s%s",
                         static_cast<unsigned long long>(s.frames), static_cast<unsigned long long>(s.fired),
                         static_cast<unsigned long long>(s.missed), static_cast<unsigned long long>(s.unplaceable),
                         static_cast<unsigned long long>(s.contradictions), static_cast<unsigned long long>(s.selection_changes),
                         static_cast<unsigned long long>(s.camera_found), static_cast<unsigned long long>(s.camera_missing),
                         static_cast<unsigned long long>(s.camera_held),
                         static_cast<unsigned long long>(s.jitter_written),
                         static_cast<unsigned long long>(s.pose_native), static_cast<unsigned long long>(s.pose_legacy),
                         static_cast<unsigned long long>(s.pose_generic), static_cast<unsigned long long>(s.pose_no_packet),
                         static_cast<unsigned long long>(s.pose_extent), static_cast<unsigned long long>(s.pose_repeat),
                         static_cast<unsigned long long>(s.pose_origin), static_cast<unsigned long long>(s.pose_shape),
                         static_cast<unsigned long long>(s.pose_split),
                         static_cast<unsigned long long>(s.pose_started), static_cast<unsigned long long>(s.pose_continued),
                         static_cast<unsigned long long>(s.pose_no_inverse),
                         static_cast<unsigned long long>(s.split_no_votes), static_cast<unsigned long long>(s.split_no_projection),
                         static_cast<unsigned long long>(s.split_few_draws), static_cast<unsigned long long>(s.split_no_majority),
                         static_cast<unsigned long long>(s.split_small_share),
                         static_cast<unsigned long long>(s.start_no_candidate), static_cast<unsigned long long>(s.start_waiting),
                         static_cast<unsigned long long>(s.start_inverse_game), static_cast<unsigned long long>(s.start_position_game),
                         layout_learned ? "learned" : "seed", static_cast<int>(layout.world_binding), layout.world_size,
                         layout.witness[0].offset, layout.witness[1].offset, static_cast<int>(layout.projection_binding),
                         layout.projection_size, layout.projection_offset,
                         static_cast<unsigned long long>(s.relative_learned), static_cast<unsigned long long>(s.relative_learn_failed),
                         static_cast<unsigned long long>(s.relative_ambiguous),
                         g_vpj_route.load() ? 1 : 0, g_vpj_sample.load() ? 1 : 0,
                         static_cast<unsigned long long>(g_vpj_frames.load()), static_cast<unsigned long long>(g_vpj_draws.load()),
                         g_vpj_last_frame_draws.load(), static_cast<unsigned long long>(g_vpj_sets.load()),
                         static_cast<unsigned long long>(g_vpj_restores.load()),
                         static_cast<unsigned long long>(g_vpj_unknown.load()), static_cast<unsigned long long>(g_vpj_mixed_frames.load()),
                         static_cast<unsigned long long>(g_exact_reads.load()), static_cast<unsigned long long>(g_exact_failed.load()),
                         static_cast<unsigned long long>(g_exact_draws.load()),
                         static_cast<unsigned long long>(s.camera_steps), static_cast<unsigned long long>(s.camera_reversals),
                         static_cast<unsigned long long>(s.camera_jumps), told[0], told[1],
                         static_cast<unsigned long long>(g_vpj_unshifted.load()),
                         cpu_ms(g_tsc_camera), cpu_ms(g_tsc_draw_ours), cpu_ms(g_tsc_draw_driver),
                         static_cast<unsigned long long>(g_timed_frames.load()),
                         scene_layered ? "after-layers" : "most-drawn", static_cast<unsigned long long>(scene_layered_frames),
                         scene_format, scene_float ? 1 : 0,
                         static_cast<unsigned long long>(placement.selected_frames),
                         placement.last_draws, placement.last_scene_draws, placement.last_reads,
                         placement.last_read_after_scene, placement.last_draw_after_scene,
                         placement.learned_read, placement.learned_overlay, is_backbuffer,
                         color_samples, depth_samples, placement.last_tested_draws, placement.last_untested_after_tested,
                         placement.learned_flat, static_cast<unsigned long long>(placement.fired_overlay),
                         static_cast<unsigned long long>(placement.fired_flat), keep_all ? 1 : 0,
                         static_cast<unsigned long long>(s.twin_across_ranges),
                         s.last_miss && s.last_miss[0] ? " last_miss=\"" : "",
                         s.last_miss && s.last_miss[0] ? s.last_miss : "", s.last_miss && s.last_miss[0] ? "\"" : "");
    if (head < 0 || static_cast<size_t>(head) >= n) return head;
    // Whose contexts the calls came from: a game drawing on a device we do not watch shows here as
    // other_device_calls rising while draws stay at zero.
    const int tail = std::snprintf(out + head, n - head,
                                   " game_device{generation=%u contexts_rechecked=%llu became_game=%llu other_device_calls=%llu} second_depth=%d jitter_stopped_after_scene=%llu",
                                   g_device_gen.load(std::memory_order_relaxed),
                                   static_cast<unsigned long long>(g_ctx_rechecked.load(std::memory_order_relaxed)),
                                   static_cast<unsigned long long>(g_ctx_became_game.load(std::memory_order_relaxed)),
                                   static_cast<unsigned long long>(g_foreign_calls.load(std::memory_order_relaxed)),
                                   g_sel_depth2.load(std::memory_order_relaxed) ? 1 : 0,
                                   static_cast<unsigned long long>(g_vpj_stopped_after_scene.load(std::memory_order_relaxed)));
    if (tail < 0 || static_cast<size_t>(head + tail) >= n) return tail < 0 ? head : head + tail;
    // The camera's part in handing frames over, and the depth direction learned without one.
    const auto use = CameraSetting();
    const char* camera_work = g_eval_paused.load(std::memory_order_relaxed) ? "paused" :
        g_camera_suspended.load(std::memory_order_relaxed) ? "backoff" : use == CameraUse::Off ? "off" : "active";
    const int more = std::snprintf(out + head + tail, n - head - tail,
                                   " camera_use{mode=%s work=%s handed_without_looking=%llu handed_none_found=%llu} "
                                   "depth_direction{%s writes_less=%u writes_greater=%u equal=%u clears=%u clear=%.6g unproven_frames=%llu flips=%llu camera_disagrees=%llu}",
                                   use == CameraUse::Off ? "off" : use == CameraUse::Optional ? "optional" : "required", camera_work,
                                   static_cast<unsigned long long>(s.camera_off), static_cast<unsigned long long>(s.camera_absent),
                                   depth_proven == DepthCompare::NearIsLess ? "near_is_less" : depth_proven == DepthCompare::NearIsGreater ? "near_is_greater" : "unproven",
                                   depth_seen.less, depth_seen.greater, depth_seen.equal, depth_seen.clears, depth_seen.clear,
                                   static_cast<unsigned long long>(s.depth_unproven), static_cast<unsigned long long>(depth_flips),
                                   static_cast<unsigned long long>(s.depth_camera_disagrees));
    return more < 0 ? head + tail : head + tail + more;
}

} // namespace na::native11
