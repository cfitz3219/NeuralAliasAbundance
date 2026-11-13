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

#include "core/temporal_jitter.hpp"
#include "host_dx12/adapters/wwii/scene_camera.hpp"
#include "host_dx12/diag/event_log.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
#include "host_dx12/native11/camera_pick.hpp"
#include "host_dx12/native11/frame_model.hpp"
#include "host_dx12/native11/split_camera.hpp"
#include "host_dx12/native11/viewport_jitter.hpp"
#include "providers/camera/camera_admission.hpp"
#include "providers/camera/camera_tracker.hpp"

#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace na::native11 {
namespace {

using Microsoft::WRL::ComPtr;

// ID3D11DeviceContext vtable (d3d11.h order).
enum CtxSlot : uint32_t {
    kVSSetConstantBuffers = 7, kPSSetShaderResources = 8, kPSSetConstantBuffers = 16, kDrawIndexed = 12, kDraw = 13, kMap = 14, kUnmap = 15,
    kDrawIndexedInstanced = 20, kDrawInstanced = 21, kVSSetShaderResources = 25, kOMSetRenderTargets = 33,
    kOMSetRenderTargetsAndUAVs = 34, kOMSetDepthStencilState = 36, kDrawIndexedInstancedIndirect = 39,
    kDrawInstancedIndirect = 40, kRSSetViewports = 44, kCopySubresourceRegion = 46, kCopyResource = 47, kUpdateSubresource = 48,
    kClearDepthStencilView = 53, kResolveSubresource = 57, kExecuteCommandList = 58, kCSSetShaderResources = 67,
    kClearState = 110, kFinishCommandList = 114,
    // ID3D11DeviceContext1
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

struct Ctx {
    bool game = false; // a context of the game's device
    uint64_t sampler_policy = ~uint64_t{0};
    bool deferred = false;
    Id rt0 = 0, depth = 0;
    bool depth_test = true;
    Id cb[kCbBindSlots] = {};          // VS slots, then PS slots at kPsCbBase
    uint32_t cb_first[kCbBindSlots] = {}; // *SSetConstantBuffers1 first constant per slot
    float vp_x = 0, vp_y = 0, vp_w = 0, vp_h = 0; // viewport 0
    D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {}; // the game's own viewports
    UINT vp_n = 0;
    bool vpj_applied = false;          // our offset copies are bound on this context now
    uint64_t vpj_sample = 0;          // the exact sample they carry
    uint32_t cb_num[kCbBindSlots] = {};   // *SSetConstantBuffers1 constant count per slot (0 = whole buffer)
    std::vector<Event> events;        // deferred recording
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
        rt0 = depth = 0;
        depth_test = true;
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
    std::vector<DeferredUpload> cb_bytes;
    std::vector<DeferredExact> draw_bytes;
    uint64_t order = 0;
};
// The view the scene used on a selected texture (typed format, subresource).
struct ViewUse { uint32_t format = 0, mip = 0, slice = 0; };

std::mutex g_install_mu;
std::atomic<bool> g_installed{false};
SceneCallback g_cb = nullptr;
void* g_cb_user = nullptr;

std::mutex g_mu; // model, lists, camera bytes, references, stats
FrameModel g_model;
std::unordered_map<ID3D11DeviceContext*, std::unique_ptr<Ctx>> g_ctx;
std::map<ID3D11CommandList*, ListRecord> g_lists;
uint64_t g_list_order = 0;
std::atomic<ID3D11Device*> g_device_raw{nullptr};
IDXGISwapChain* g_swap = nullptr; // identity only
std::atomic<Id> g_sel_color{0};    // readable from any thread

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
std::unordered_map<Id, CameraUploads> g_cb_latest;
std::unordered_map<Id, uint64_t> g_cb_incarnation; // candidate camera buffers: lifetime id, tagged at upload (live object)

ComPtr<ID3D11Resource> g_color_ref, g_depth_ref;
ViewUse g_color_view, g_depth_view;
float g_scene_vp_x = 0, g_scene_vp_y = 0, g_scene_vp_w = 0, g_scene_vp_h = 0; // scene viewport this frame
uint32_t g_camera_first = 0, g_camera_num = 0; // exact bound range of the selected camera
uint64_t g_stream = 0;                    // SceneFrame::stream; bumps when any input object changes
uint64_t g_stream_key[4] = {};            // colour and depth lifetime ids, camera buffer id and its lifetime id (0 = untagged)

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
std::atomic<Id> g_sel_depth{0};
std::atomic<uint32_t> g_sel_depth_w{0}, g_sel_depth_h{0};
uint32_t g_vpj_frame_draws = 0; // executed scene draws with a nonzero sample, under g_mu
uint32_t g_vpj_frame_unshifted = 0; // draws into the scene colour left unshifted on a shifted frame, under g_mu
std::atomic<uint64_t> g_vpj_unshifted{0};
std::atomic<uint64_t> g_vpj_told{0}; // the sample last handed to the consumer (two floats, pixels)
uint64_t g_vpj_executed_sample = 0;
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

struct Inside {
    bool was;
    Inside() : was(t_inside) { t_inside = true; }
    ~Inside() { t_inside = was; }
};

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
    if (!id) return false;
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
    if (t_last_ctx == c) return t_last && t_last->game ? t_last : nullptr;
    ID3D11Device* game = g_device_raw.load(std::memory_order_acquire);
    if (!game) return nullptr;
    std::lock_guard<std::mutex> l(g_mu);
    auto& slot = g_ctx[c];
    if (!slot) {
        slot = std::make_unique<Ctx>();
        Inside in;
        ComPtr<ID3D11Device> d;
        c->GetDevice(&d);
        slot->game = d.Get() == game;
        slot->deferred = c->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED;
    }
    t_last_ctx = c;
    t_last = slot.get();
    return slot->game ? slot.get() : nullptr;
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
    PFN_SetSRV ps_srv = nullptr, cs_srv = nullptr, vs_srv = nullptr;
    PFN_DrawIndexed draw_indexed = nullptr;
    PFN_Draw draw = nullptr;
    PFN_DrawIndexedInstanced draw_indexed_instanced = nullptr;
    PFN_DrawInstanced draw_instanced = nullptr;
    PFN_DrawIndirect draw_indexed_indirect = nullptr, draw_indirect = nullptr;
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
            result = std::max(result, check);
            continue;
        }
        result = check;
        g_camera = p;
        g_model.SetCamera(b.buffer, b.slot);
        g_camera_first = b.first;
        g_camera_num = b.count;
        *out = p;
        *upload_out = &upload;
        *version_out = b.version;
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
void Fire(ID3D11DeviceContext* immediate, std::unique_lock<std::mutex>& lk, Boundary boundary)
{
    ++g_stats.fired;
    if (!g_cb) return SetReason("No consumer is attached.");
    // CameraNow may learn the camera for this very callback.
    const Selection& sel = g_model.Current();
    if (!g_color_ref || IdOf(g_color_ref.Get()) != sel.color || !g_depth_ref || IdOf(g_depth_ref.Get()) != sel.depth) {
        ++g_stats.missed;
        return SetReason("The scene colour or depth is not held from a live game view yet.");
    }
    CameraPick cam;
    const CameraUpload* upload = nullptr;
    uint64_t version = 0;
    PoseCheck pose_check = PoseCheck::NoPacket;
    std::optional<CameraFacts::RelativePose> split_pose;
    const bool found = CameraNow(&cam, &upload, &version, pose_check, split_pose);
    CountPoseCheck(pose_check);
    bool held = false;
    if (!found) {
        DiagnoseCameraMiss();
        // At most kHoldFrames in a row, and only for the same scene textures.
        if (!g_held.valid || g_frame <= g_held.frame || g_frame - g_held.frame > kHoldFrames ||
            g_held.color != sel.color || g_held.depth != sel.depth ||
            !HeldCameraUnreadable(g_held.cb, g_held.slot)) {
            g_held.valid = false;
            ++g_stats.camera_missing;
            return SetReason("No supported camera was found in the scene's constant buffers.");
        }
        held = true;
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
    } else {
        ++g_stats.camera_found;
    }
    if (split_pose) ++g_stats.pose_split;
    const Selection cam_sel = g_model.Current(); // CameraNow may have just chosen (or changed) the camera buffer
    {
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
    if (!held) {
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
    const ComPtr<ID3D11Resource> color = g_color_ref, depth = g_depth_ref;
    texture(f.color, color.Get(), sel.color_info, g_color_view);
    texture(f.depth, depth.Get(), sel.depth_info, g_depth_view);
    f.reversed_depth = cam.reversed;
    std::vector<uint8_t> camera_bytes(*upload->bytes); // the callback runs without g_mu
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
    const bool vpj_route = g_vpj_route.load(std::memory_order_relaxed);
    if (vpj_route && g_vpj_mixed) {
        ++g_vpj_mixed_frames;
        ++g_stats.missed;
        return SetReason("Scene draws used different viewport jitter samples.");
    }
    f.camera.raster_jitter_qualified = vpj_route || (f.camera.vs_slot >= 0 && cam.raster_jitter_qualified);
    f.camera.viewport_jitter = vpj_route;
    f.camera.jitter_applied = vpj_route ? g_vpj_executed_sample != 0 : (f.camera.raster_jitter_qualified && g_jitter_written);
    if (vpj_route) {
        static_assert(sizeof(f.camera.jitter_px) == sizeof(g_vpj_executed_sample));
        std::memcpy(f.camera.jitter_px, &g_vpj_executed_sample, sizeof(f.camera.jitter_px));
    } else if (f.camera.jitter_applied) {
        f.camera.jitter_px[0] = static_cast<float>(g_jitter.x_pixels);
        f.camera.jitter_px[1] = static_cast<float>(g_jitter.y_pixels);
    }
    {
        // WHY log: what the consumer is told the jitter was, beside what the scene draws carried.
        // The first frames of a session, then a burst of sixteen about every ten seconds, so one
        // whole jitter cycle is on record each time.
        static uint32_t logged = 0;
        static uint64_t burst_tick = 0, burst_left = 48;
        const uint64_t now = GetTickCount64();
        if (!burst_left && now - burst_tick >= 10000) burst_left = 16;
        if (burst_left && logged < 1500 && (f.camera.jitter_applied || vpj_route)) {
            if (!--burst_left) burst_tick = now;
            ++logged;
            float shift[2]{};
            std::memcpy(shift, &g_vpj_executed_sample, sizeof shift);
            na::eventlog::Emit("native11_jitter",
                               "frame=%llu route=%s told_px=%.4f,%.4f viewport_shift_px=%.4f,%.4f applied=%d shifted_draws=%u "
                               "unshifted_colour_draws=%u scene_draws=%u held=%d scene=%.0fx%.0f depth=%ux%u",
                               static_cast<unsigned long long>(g_frame), vpj_route ? "viewport" : "matrix",
                               f.camera.jitter_px[0], f.camera.jitter_px[1], shift[0], shift[1], f.camera.jitter_applied ? 1 : 0,
                               g_vpj_frame_draws, g_vpj_frame_unshifted, g_model.SceneDrawsThisFrame(), held ? 1 : 0,
                               g_scene_vp_w, g_scene_vp_h, g_sel_depth_w.load(std::memory_order_relaxed),
                               g_sel_depth_h.load(std::memory_order_relaxed));
        }
        uint64_t told = 0;
        std::memcpy(&told, f.camera.jitter_px, sizeof told);
        g_vpj_told.store(told, std::memory_order_relaxed);
    }
    {
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
    f.scene_draws = g_model.SceneDrawsThisFrame();
    // The camera buffer is only named by pointer here (it may be gone), so its lifetime id is the one
    // tagged when the game last uploaded to it.
    // A buffer selected before its first candidate upload has no tag yet: its first tag is not a change.
    uint64_t cam_inc = 0;
    if (cam_sel.camera_cb) {
        auto it = g_cb_incarnation.find(cam_sel.camera_cb);
        if (it != g_cb_incarnation.end()) cam_inc = it->second;
    }
    f.selection_changed = !g_stream || f.color.incarnation != g_stream_key[0] || f.depth.incarnation != g_stream_key[1] ||
                          cam_sel.camera_cb != g_stream_key[2] || (cam_inc && g_stream_key[3] && cam_inc != g_stream_key[3]);
    if (f.selection_changed) ++g_stream;
    g_stream_key[0] = f.color.incarnation;
    g_stream_key[1] = f.depth.incarnation;
    g_stream_key[2] = cam_sel.camera_cb;
    if (cam_inc || f.selection_changed) g_stream_key[3] = cam_inc;
    f.stream = g_stream;
    g_last_selection = cam_sel;
    const SceneCallback cb = g_cb;
    void* user = g_cb_user;
    lk.unlock();
    bool ok = false;
    {
        Inside in; // the consumer's D3D11 calls on the game's context are not observed
        ok = cb(user, immediate, f);
    }
    lk.lock();
    if (!ok) ++g_stats.missed;
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
    if (e.kind != Kind::Draw || !g_model.IsSceneDraw(e)) return;
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
    if (!e.viewport_sample && g_vpj_executed_sample && e.rt0 && e.rt0 == g_sel_color.load(std::memory_order_relaxed))
        ++g_vpj_frame_unshifted;
    if (!e.depth_test || !g_model.IsSceneDraw(e)) return;
    if (!g_vpj_seen_draw) {
        g_vpj_seen_draw = true;
        g_vpj_executed_sample = e.viewport_sample;
    } else if (g_vpj_executed_sample != e.viewport_sample) {
        g_vpj_mixed = true;
    }
    if (e.viewport_sample) ++g_vpj_frame_draws;
}

// ---------------------------------------------------------------- events
void Emit(Ctx* x, ID3D11DeviceContext* c, const Event& e, CameraUploads uploads = {}, const ExactSet& exact = {})
{
    if (x->deferred) {
        if (x->events.size() >= kMaxListEvents) return;
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
    if (e.kind == Kind::Read && g_model.ShouldFireBeforeRead(e.resource)) {
        g_model.MarkFired();
        Fire(c, l, Boundary::BeforeRead);
    } else if (e.kind == Kind::Draw && g_model.ShouldFireBeforeDraw(e)) {
        g_model.MarkFired();
        Fire(c, l, Boundary::BeforeHud);
    }
    const uint32_t v = g_model.Feed(e);
    RememberSceneUploads(e, exact.get());
    NoteViewportDraw(e);
    if (e.kind == Kind::CbUpdate && uploads.primary.bytes)
        SaveCameraUpload(e.resource, v, std::move(uploads));
}

// Viewport jitter, before a game draw is forwarded: a depth-tested draw into the scene depth gets
// the game's viewports moved by this frame's sample; any other draw gets the game's own back.
void VpjBeforeDraw(ID3D11DeviceContext* c, Ctx* x)
{
    const uint64_t sample = g_vpj_sample.load(std::memory_order_acquire);
    const bool on = sample != 0;
    if (!on && !x->vpj_applied) return;
    const Id scene_depth = g_sel_depth.load(std::memory_order_relaxed);
    // Every depth-tested draw into the scene depth moves, whatever colour target is bound: a depth
    // prepass (no colour target) and the colour passes tested against it must rasterise at the same
    // sub-pixel position, or the later passes fail their depth test over whole surfaces
    // (p42728: triangles dropped to black, differently for each sample).
    const bool scene = on && scene_depth && x->depth == scene_depth && x->depth_test;
    if (scene && !x->vp_n) g_vpj_unknown.fetch_add(1, std::memory_order_relaxed);
    const bool want = WantsViewportJitter(on, x->depth, scene_depth, x->depth_test, x->vp_n);
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
        JitterViewports(r, n, offset[0], offset[1], g_sel_depth_w.load(std::memory_order_relaxed), g_sel_depth_h.load(std::memory_order_relaxed));
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

void DrawEvent(ID3D11DeviceContext* c, uint64_t vertices, bool indirect)
{
    Ctx* x = Get(c);
    if (!x) return;
    VpjBeforeDraw(c, x);
    const uint64_t policy = SamplerPolicyKey();
    if (x->sampler_policy != policy) {
        RefreshSamplers(c);
        x->sampler_policy = policy;
    }
    Event e;
    e.kind = Kind::Draw;
    e.rt0 = x->rt0;
    e.depth = x->depth;
    e.depth_test = x->depth_test;
    e.vertices = vertices;
    e.indirect = indirect;
    e.viewport_sample = x->vpj_applied ? x->vpj_sample : 0;
    std::memcpy(e.cb, x->cb, sizeof(e.cb));
    for (int i = 0; i < kCbBindSlots; ++i) {
        e.cb_first[i] = static_cast<uint16_t>(x->cb_first[i]);
        e.cb_count[i] = static_cast<uint16_t>(x->cb_num[i]);
    }
    if (!x->deferred && e.rt0 && e.rt0 == g_sel_color.load(std::memory_order_relaxed)) {
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
    if (e.rt0 && e.rt0 == g_sel_color.load(std::memory_order_relaxed)) exact = ExactForDraw(c, x);
    Emit(x, c, e, {}, exact);
}
void ReadEvents(ID3D11DeviceContext* c, UINT n, ID3D11ShaderResourceView* const* views)
{
    Ctx* x = Get(c);
    if (!x || !views) return;
    const Id color = g_sel_color.load(std::memory_order_relaxed);
    if (!color) return;
    for (UINT i = 0; i < n; ++i) {
        if (!views[i]) continue;
        Id r = 0;
        {
            Inside in;
            r = ResourceOfView(views[i]);
        }
        if (r != color) continue;
        if (!x->deferred) {
            std::lock_guard<std::mutex> l(g_mu);
            if (IdOf(g_color_ref.Get()) != r) {
                g_color_ref.Reset();
                Inside in;
                views[i]->GetResource(&g_color_ref);
            }
        }
        Event e;
        e.kind = Kind::Read;
        e.resource = r;
        Emit(x, c, e);
        return; // one read per bind call is enough
    }
}
void ReadResource(ID3D11DeviceContext* c, ID3D11Resource* src)
{
    Ctx* x = Get(c);
    if (!x || !src) return;
    const Id r = IdOf(src);
    if (!r || r != g_sel_color.load(std::memory_order_relaxed)) return;
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
void STDMETHODCALLTYPE PSSetShaderResources_d(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11ShaderResourceView* const* v)
{
    ReadEvents(c, n, v);
    O(c, t_inside ? nullptr : Get(c)).ps_srv(c, start, n, v);
}
void STDMETHODCALLTYPE CSSetShaderResources_d(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11ShaderResourceView* const* v)
{
    ReadEvents(c, n, v);
    O(c, t_inside ? nullptr : Get(c)).cs_srv(c, start, n, v);
}
void STDMETHODCALLTYPE VSSetShaderResources_d(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11ShaderResourceView* const* v)
{
    ReadEvents(c, n, v);
    O(c, t_inside ? nullptr : Get(c)).vs_srv(c, start, n, v);
}
void STDMETHODCALLTYPE DrawIndexed_d(ID3D11DeviceContext* c, UINT count, UINT start, INT base)
{
    DrawEvent(c, count, false);
    O(c, t_inside ? nullptr : Get(c)).draw_indexed(c, count, start, base);
}
void STDMETHODCALLTYPE Draw_d(ID3D11DeviceContext* c, UINT count, UINT start)
{
    DrawEvent(c, count, false);
    O(c, t_inside ? nullptr : Get(c)).draw(c, count, start);
}
void STDMETHODCALLTYPE DrawIndexedInstanced_d(ID3D11DeviceContext* c, UINT count, UINT inst, UINT start, INT base, UINT start_inst)
{
    DrawEvent(c, uint64_t(count) * (inst ? inst : 1), false);
    O(c, t_inside ? nullptr : Get(c)).draw_indexed_instanced(c, count, inst, start, base, start_inst);
}
void STDMETHODCALLTYPE DrawInstanced_d(ID3D11DeviceContext* c, UINT count, UINT inst, UINT start, UINT start_inst)
{
    DrawEvent(c, uint64_t(count) * (inst ? inst : 1), false);
    O(c, t_inside ? nullptr : Get(c)).draw_instanced(c, count, inst, start, start_inst);
}
void STDMETHODCALLTYPE DrawIndexedInstancedIndirect_d(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT off)
{
    DrawEvent(c, 0, true);
    O(c, t_inside ? nullptr : Get(c)).draw_indexed_indirect(c, b, off);
}
void STDMETHODCALLTYPE DrawInstancedIndirect_d(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT off)
{
    DrawEvent(c, 0, true);
    O(c, t_inside ? nullptr : Get(c)).draw_indirect(c, b, off);
}
HRESULT STDMETHODCALLTYPE Map_d(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* m)
{
    Ctx* x = Get(c);
    const HRESULT hr = O(c, x).map(c, r, sub, type, flags, m);
    if (x && SUCCEEDED(hr) && m && (type == D3D11_MAP_WRITE_DISCARD || type == D3D11_MAP_WRITE_NO_OVERWRITE || type == D3D11_MAP_WRITE ||
                                    type == D3D11_MAP_READ_WRITE)) {
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
    if (!x || !r || !src) return;
    UINT size = 0;
    bool cb = false;
    {
        Inside in;
        cb = IsConstantBuffer(r, &size);
    }
    if (!cb) return;
    if (box || !IsCandidate(IdOf(r))) { CbUpdate(c, r, nullptr, 0); return; } // constant buffers are updated whole
    tmp.assign(static_cast<const uint8_t*>(src), static_cast<const uint8_t*>(src) + size);
    if (JitterUpload(tmp.data(), tmp.size(), IdOf(r))) src = tmp.data(); // the game's upload, jittered
    CbUpdate(c, r, tmp.data(), tmp.size());
}
void STDMETHODCALLTYPE UpdateSubresource_d(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* src, UINT rp,
                                           UINT dp)
{
    Ctx* x = Get(c);
    std::vector<uint8_t> tmp;
    UpdateCommon(c, x, r, box, src, tmp);
    O(c, x).update(c, r, sub, box, src, rp, dp);
}
void STDMETHODCALLTYPE UpdateSubresource1_d(ID3D11DeviceContext1* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* src,
                                            UINT rp, UINT dp, UINT flags)
{
    Ctx* x = Get(c);
    std::vector<uint8_t> tmp;
    UpdateCommon(c, x, r, box, src, tmp);
    O(c, x).update1(c, r, sub, box, src, rp, dp, flags);
}
ViewUse RtvUse(ID3D11RenderTargetView* v)
{
    D3D11_RENDER_TARGET_VIEW_DESC d{};
    v->GetDesc(&d);
    ViewUse u;
    u.format = d.Format;
    if (d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D) u.mip = d.Texture2D.MipSlice;
    else if (d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY) { u.mip = d.Texture2DArray.MipSlice; u.slice = d.Texture2DArray.FirstArraySlice; }
    else if (d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY) u.slice = d.Texture2DMSArray.FirstArraySlice;
    return u;
}
ViewUse DsvUse(ID3D11DepthStencilView* v)
{
    D3D11_DEPTH_STENCIL_VIEW_DESC d{};
    v->GetDesc(&d);
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
    {
        Inside in;
        if (n && rtvs && rtvs[0]) { rtvs[0]->GetResource(&rt); rt_use = RtvUse(rtvs[0]); }
        if (dsv) { dsv->GetResource(&ds); ds_use = DsvUse(dsv); }
    }
    x->rt0 = IdOf(rt);
    x->depth = IdOf(ds);
    if (rt || ds) {
        std::lock_guard<std::mutex> l(g_mu);
        Inside in;
        if (rt) g_model.SetInfo(IdOf(rt), InfoOf(rt, false));
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
    if (x) TrackTargets(x, n, rtvs, dsv);
    O(c, x).om_rt(c, n, rtvs, dsv);
}
void STDMETHODCALLTYPE OMSetRenderTargetsAndUAVs_d(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv,
                                                   UINT us, UINT un, ID3D11UnorderedAccessView* const* uavs, const UINT* counts)
{
    Ctx* x = Get(c);
    if (x && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) TrackTargets(x, n, rtvs, dsv);
    O(c, x).om_rt_uav(c, n, rtvs, dsv, us, un, uavs, counts);
}
void STDMETHODCALLTYPE OMSetDepthStencilState_d(ID3D11DeviceContext* c, ID3D11DepthStencilState* s, UINT ref)
{
    Ctx* x = Get(c);
    if (x) {
        if (!s) x->depth_test = true; // default state: depth test on
        else {
            D3D11_DEPTH_STENCIL_DESC d{};
            Inside in;
            s->GetDesc(&d);
            x->depth_test = d.DepthEnable != FALSE;
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
void STDMETHODCALLTYPE CopySubresourceRegion_d(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT ds, UINT x0, UINT y0, UINT z0, ID3D11Resource* src,
                                               UINT ss, const D3D11_BOX* box)
{
    ReadResource(c, src);
    O(c, t_inside ? nullptr : Get(c)).copy_region(c, dst, ds, x0, y0, z0, src, ss, box);
}
void STDMETHODCALLTYPE CopyResource_d(ID3D11DeviceContext* c, ID3D11Resource* dst, ID3D11Resource* src)
{
    ReadResource(c, src);
    O(c, t_inside ? nullptr : Get(c)).copy_resource(c, dst, src);
}
void STDMETHODCALLTYPE ResolveSubresource_d(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT ds, ID3D11Resource* src, UINT ss, DXGI_FORMAT f)
{
    ReadResource(c, src);
    O(c, t_inside ? nullptr : Get(c)).resolve(c, dst, ds, src, ss, f);
}
void STDMETHODCALLTYPE ClearDepthStencilView_d(ID3D11DeviceContext* c, ID3D11DepthStencilView* v, UINT flags, FLOAT d, UINT8 s)
{
    Ctx* x = Get(c);
    if (x && v && (flags & D3D11_CLEAR_DEPTH)) {
        Id id = 0;
        {
            Inside in;
            id = ResourceOfView(v);
        }
        Event e;
        e.kind = Kind::ClearDepth;
        e.resource = id;
        Emit(x, c, e);
    }
    O(c, x).clear_dsv(c, v, flags, d, s);
}
void STDMETHODCALLTYPE ClearState_d(ID3D11DeviceContext* c)
{
    Ctx* x = Get(c);
    if (x) x->reset_state();
    O(c, x).clear_state(c);
}
HRESULT STDMETHODCALLTYPE FinishCommandList_d(ID3D11DeviceContext* c, BOOL restore, ID3D11CommandList** out)
{
    Ctx* x = Get(c);
    const HRESULT hr = O(c, x).finish(c, restore, out);
    if (x) {
        if (SUCCEEDED(hr) && out && *out) {
            std::lock_guard<std::mutex> l(g_mu);
            ListRecord& rec = g_lists[*out];
            rec.events = std::move(x->events);
            rec.cb_bytes = std::move(x->cb_bytes);
            rec.draw_bytes = std::move(x->draw_bytes);
            rec.order = ++g_list_order;
            if (g_lists.size() > kMaxLists) {
                auto oldest = g_lists.begin();
                for (auto it = g_lists.begin(); it != g_lists.end(); ++it)
                    if (it->second.order < oldest->second.order) oldest = it;
                g_lists.erase(oldest);
            }
        }
        x->events.clear();
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
    if (x && !x->deferred && list) {
        std::unique_lock<std::mutex> l(g_mu);
        auto it = g_lists.find(list);
        if (it != g_lists.end() && g_model.ShouldFireBeforeList(g_model.Summarize(it->second.events))) {
            g_model.MarkFired();
            Fire(c, l, Boundary::BeforeList);
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
                const uint32_t v = g_model.Feed(events[i]);
                if (next_exact < exact.size() && exact[next_exact].first == i) current_exact = exact[next_exact++].second.get();
                RememberSceneUploads(events[i], current_exact);
                NoteViewportDraw(events[i]);
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
}

// ---------------------------------------------------------------- frame boundary
bool OnPresent(IDXGISwapChain* swap)
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
        DXGI_SWAP_CHAIN_DESC sd{};
        swap->GetDesc(&sd);
        g_model.SetSwapChainSize(sd.BufferDesc.Width, sd.BufferDesc.Height);
    }
    ++g_frame;
    g_exact_frame.fetch_add(1, std::memory_order_relaxed);
    ++g_stats.frames;
    LearnLargeCbWindows();
    g_model.EndFrame();
    const auto& ms = g_model.GetStats();
    g_stats.unplaceable = ms.unplaceable;
    g_stats.contradictions = ms.contradictions;
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
    // Learning the relative layout needs every scene range's upload for one frame.
    g_collect_camera.store(!g_camera.found || g_rel_learner.armed(), std::memory_order_relaxed);
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
    g_scene_vp_x = g_scene_vp_y = g_scene_vp_w = g_scene_vp_h = 0;
    // Next frame's jitter: the backend's sample, written into the main camera's uploads.
    g_jitter_written = false;
    // A request covers exactly the frame that starts here: without a new one (a missed or failed
    // evaluation), the next frame's camera is the game's own, unjittered.
    g_jitter_active = g_jitter_requested && g_camera.found;
    g_jitter = neural_alias::JitterSample{g_next_jitter[0], g_next_jitter[1]};
    // Viewport route: the same one-frame request, applied on the scene draws of the frame starting here.
    const uint32_t jittered_draws = g_vpj_frame_draws;
    g_vpj_frame_draws = 0;
    g_vpj_unshifted.fetch_add(g_vpj_frame_unshifted, std::memory_order_relaxed);
    g_vpj_frame_unshifted = 0;
    g_vpj_seen_draw = g_vpj_mixed = false;
    g_vpj_executed_sample = 0;
    g_vpj_last_frame_draws.store(jittered_draws, std::memory_order_relaxed);
    if (jittered_draws) g_vpj_frames.fetch_add(1, std::memory_order_relaxed);
    g_sel_depth.store(s.depth, std::memory_order_relaxed);
    g_sel_depth_w.store(s.depth_info.width, std::memory_order_relaxed);
    g_sel_depth_h.store(s.depth_info.height, std::memory_order_relaxed);
    uint64_t sample = 0;
    if (g_vpj_route.load(std::memory_order_relaxed) && g_jitter_requested && s.valid() &&
        (g_next_jitter[0] != 0.f || g_next_jitter[1] != 0.f)) {
        std::memcpy(&sample, g_next_jitter, sizeof(sample));
    }
    g_vpj_sample.store(sample, std::memory_order_release);
    g_jitter_requested = false;
    return true;
}

HRESULT STDMETHODCALLTYPE Present_d(IDXGISwapChain* s, UINT sync, UINT flags)
{
    const bool outer = !t_in_present;
    if (outer && !(flags & DXGI_PRESENT_TEST)) OnPresent(s);
    t_in_present = true;
    const HRESULT hr = g_present(s, sync, flags);
    if (outer) t_in_present = false;
    return hr;
}
HRESULT STDMETHODCALLTYPE Present1_d(IDXGISwapChain1* s, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p)
{
    const bool outer = !t_in_present;
    if (outer && !(flags & DXGI_PRESENT_TEST)) OnPresent(s);
    t_in_present = true;
    const HRESULT hr = g_present1(s, sync, flags, p);
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
        // Release everything that could hold a back buffer, or ResizeBuffers fails.
        g_color_ref.Reset();
        g_depth_ref.Reset();
        g_model.Forget(g_model.Current().color); // the scene targets are usually recreated with the new size
        g_split_draw = {};
        g_split_votes.clear();
        g_sel_color.store(0);
        g_sel_depth.store(0);
        g_vpj_sample.store(0);
    }
}
void UpdateResizeExtent(IDXGISwapChain* s, long result)
{
    std::lock_guard<std::mutex> l(g_mu);
    if (s == g_swap && SUCCEEDED(result)) {
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
        {kPSSetShaderResources, reinterpret_cast<void*>(&PSSetShaderResources_d), reinterpret_cast<void**>(&o.ps_srv)},
        {kCSSetShaderResources, reinterpret_cast<void*>(&CSSetShaderResources_d), reinterpret_cast<void**>(&o.cs_srv)},
        {kVSSetShaderResources, reinterpret_cast<void*>(&VSSetShaderResources_d), reinterpret_cast<void**>(&o.vs_srv)},
        {kDrawIndexed, reinterpret_cast<void*>(&DrawIndexed_d), reinterpret_cast<void**>(&o.draw_indexed)},
        {kDraw, reinterpret_cast<void*>(&Draw_d), reinterpret_cast<void**>(&o.draw)},
        {kDrawIndexedInstanced, reinterpret_cast<void*>(&DrawIndexedInstanced_d), reinterpret_cast<void**>(&o.draw_indexed_instanced)},
        {kDrawInstanced, reinterpret_cast<void*>(&DrawInstanced_d), reinterpret_cast<void**>(&o.draw_instanced)},
        {kDrawIndexedInstancedIndirect, reinterpret_cast<void*>(&DrawIndexedInstancedIndirect_d), reinterpret_cast<void**>(&o.draw_indexed_indirect)},
        {kDrawInstancedIndirect, reinterpret_cast<void*>(&DrawInstancedIndirect_d), reinterpret_cast<void**>(&o.draw_indirect)},
        {kMap, reinterpret_cast<void*>(&Map_d), reinterpret_cast<void**>(&o.map)},
        {kUnmap, reinterpret_cast<void*>(&Unmap_d), reinterpret_cast<void**>(&o.unmap)},
        {kOMSetRenderTargets, reinterpret_cast<void*>(&OMSetRenderTargets_d), reinterpret_cast<void**>(&o.om_rt)},
        {kOMSetRenderTargetsAndUAVs, reinterpret_cast<void*>(&OMSetRenderTargetsAndUAVs_d), reinterpret_cast<void**>(&o.om_rt_uav)},
        {kOMSetDepthStencilState, reinterpret_cast<void*>(&OMSetDepthStencilState_d), reinterpret_cast<void**>(&o.om_dss)},
        {kRSSetViewports, reinterpret_cast<void*>(&RSSetViewports_d), reinterpret_cast<void**>(&o.rs_vp)},
        {kCopySubresourceRegion, reinterpret_cast<void*>(&CopySubresourceRegion_d), reinterpret_cast<void**>(&o.copy_region)},
        {kCopyResource, reinterpret_cast<void*>(&CopyResource_d), reinterpret_cast<void**>(&o.copy_resource)},
        {kUpdateSubresource, reinterpret_cast<void*>(&UpdateSubresource_d), reinterpret_cast<void**>(&o.update)},
        {kClearDepthStencilView, reinterpret_cast<void*>(&ClearDepthStencilView_d), reinterpret_cast<void**>(&o.clear_dsv)},
        {kResolveSubresource, reinterpret_cast<void*>(&ResolveSubresource_d), reinterpret_cast<void**>(&o.resolve)},
        {kClearState, reinterpret_cast<void*>(&ClearState_d), reinterpret_cast<void**>(&o.clear_state)},
    };
    if (deferred) hooks.push_back({kFinishCommandList, reinterpret_cast<void*>(&FinishCommandList_d), reinterpret_cast<void**>(&o.finish)});
    else hooks.push_back({kExecuteCommandList, reinterpret_cast<void*>(&ExecuteCommandList_d), reinterpret_cast<void**>(&o.execute)});
    ComPtr<ID3D11DeviceContext1> c1;
    if (SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&c1)))) {
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

bool ObservePresent(void* swapchain) { return OnPresent(static_cast<IDXGISwapChain*>(swapchain)); }
void BeforeResize(void* swapchain) { ReleaseResizeInputs(static_cast<IDXGISwapChain*>(swapchain)); }
void AfterResize(void* swapchain, long result) { UpdateResizeExtent(static_cast<IDXGISwapChain*>(swapchain), result); }
OwnWorkScope::OwnWorkScope() : was_(t_inside) { t_inside = true; }
OwnWorkScope::~OwnWorkScope() { t_inside = was_; }

bool Install(void* device, SceneCallback cb, void* user, bool host_owns_present)
{
    std::lock_guard<std::mutex> g(g_install_mu);
    {
        std::lock_guard<std::mutex> l(g_mu);
        g_cb = cb;
        g_cb_user = user;
    }
    if (device) g_device_raw.store(static_cast<ID3D11Device*>(device), std::memory_order_release);
    if (g_installed.load()) return true;
    if (!InstallHooks(host_owns_present)) return false;
    g_installed.store(true);
    return true;
}

bool IsGameWork(void* context) { return Get(static_cast<ID3D11DeviceContext*>(context)) != nullptr; }
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
    g_color_ref.Reset();
    g_depth_ref.Reset();
    g_lists.clear();
    g_ctx.clear();
    g_sel_color.store(0);
    g_sel_depth.store(0);
    g_vpj_sample.store(0);
}

void SetJitterRoute(JitterRoute route)
{
    g_vpj_route.store(route == JitterRoute::Viewport, std::memory_order_relaxed);
    if (route != JitterRoute::Viewport) g_vpj_sample.store(0, std::memory_order_release); // bound copies restore at the next draw
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
    return g_stats;
}

int FormatStatus(char* out, unsigned long n)
{
    const ProviderStats s = Stats();
    float told[2]{};
    const uint64_t told_bits = g_vpj_told.load(std::memory_order_relaxed);
    std::memcpy(told, &told_bits, sizeof told);
    camera::RelativeLayout layout;
    bool layout_learned = false;
    {
        std::lock_guard<std::mutex> l(g_mu);
        layout = g_rel_layout;
        layout_learned = g_rel_learned;
    }
    return std::snprintf(out, n,
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
                         "jitter_told{px=%.4f,%.4f unshifted_colour_draws=%llu}%s%s%s",
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
                         static_cast<unsigned long long>(g_vpj_unshifted.load()), s.last_miss && s.last_miss[0] ? " last_miss=\"" : "",
                         s.last_miss && s.last_miss[0] ? s.last_miss : "", s.last_miss && s.last_miss[0] ? "\"" : "");
}

} // namespace na::native11
