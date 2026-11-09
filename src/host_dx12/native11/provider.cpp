// Native DX11 input provider (WWII 40-50 fps plan, B1). See input_provider.hpp.
//
// Ported from the PR #90 DX11 host (src/host_dx11/dx11_host.cpp on claude/project-thread-2b5ysj): the
// same hook set and frame model, with that host's own bridge/helper replaced by the SceneCallback, so
// the existing DX12 backend (Astra's B2 handoff) does the evaluation inside the one program.
//
// Hook set (vtable functions of d3d11.dll's own context classes, found on a small private device;
// hooking the function covers every game context of that class). Immediate and deferred contexts
// are different classes, so both are hooked:
//   state:  OMSetRenderTargets(+AndUAVs), OMSetDepthStencilState, VSSetConstantBuffers(+1),
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
//   * Not here yet (owned elsewhere or later): sampler mip bias for the native route.
#include "host_dx12/native11/input_provider.hpp"

#include "core/temporal_jitter.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
#include "host_dx12/native11/camera_pick.hpp"
#include "host_dx12/native11/frame_model.hpp"
#include "providers/camera/camera_tracker.hpp"

#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace na::native11 {
namespace {

using Microsoft::WRL::ComPtr;

// ID3D11DeviceContext vtable (d3d11.h order).
enum CtxSlot : uint32_t {
    kVSSetConstantBuffers = 7, kPSSetShaderResources = 8, kDrawIndexed = 12, kDraw = 13, kMap = 14, kUnmap = 15,
    kDrawIndexedInstanced = 20, kDrawInstanced = 21, kVSSetShaderResources = 25, kOMSetRenderTargets = 33,
    kOMSetRenderTargetsAndUAVs = 34, kOMSetDepthStencilState = 36, kDrawIndexedInstancedIndirect = 39,
    kDrawInstancedIndirect = 40, kRSSetViewports = 44, kCopySubresourceRegion = 46, kCopyResource = 47, kUpdateSubresource = 48,
    kClearDepthStencilView = 53, kResolveSubresource = 57, kExecuteCommandList = 58, kCSSetShaderResources = 67,
    kClearState = 110, kFinishCommandList = 114,
    // ID3D11DeviceContext1
    kUpdateSubresource1 = 116, kVSSetConstantBuffers1 = 119,
};
enum SwapSlot : uint32_t { kPresent = 8, kResizeBuffers = 13, kPresent1 = 22 };

constexpr size_t kMaxCbBytes = 16 * 1024;  // camera blocks are far smaller
constexpr size_t kMaxListEvents = 200000; // per deferred recording; beyond this the list's tail is not modelled
constexpr size_t kMaxCandidates = 4;       // constant buffers searched for the camera
constexpr size_t kMaxLists = 512;          // recorded command lists kept

struct Ctx {
    bool game = false; // a context of the game's device
    bool deferred = false;
    Id rt0 = 0, depth = 0;
    bool depth_test = true;
    Id vs_cb[kCbSlots] = {};
    uint32_t vs_first[kCbSlots] = {}; // VSSetConstantBuffers1 first constant per slot
    float vp_x = 0, vp_y = 0, vp_w = 0, vp_h = 0; // viewport 0
    uint32_t vs_num[kCbSlots] = {};   // VSSetConstantBuffers1 constant count per slot (0 = whole buffer)
    std::vector<Event> events;        // deferred recording
    std::vector<std::pair<size_t, std::vector<uint8_t>>> cb_bytes; // (event index, bytes) for CbUpdate events
    std::unordered_map<Id, void*> mapped; // candidate constant buffers mapped right now
    void reset_state()
    {
        rt0 = depth = 0;
        depth_test = true;
        std::memset(vs_cb, 0, sizeof(vs_cb));
        std::memset(vs_first, 0, sizeof(vs_first));
        std::memset(vs_num, 0, sizeof(vs_num));
        vp_x = vp_y = vp_w = vp_h = 0;
    }
};
struct ListRecord {
    std::vector<Event> events;
    std::vector<std::pair<size_t, std::vector<uint8_t>>> cb_bytes;
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
std::unordered_map<Id, std::vector<uint8_t>> g_cb_latest;
std::unordered_map<Id, uint64_t> g_cb_incarnation; // candidate camera buffers: lifetime id, tagged at upload (live object)
std::map<std::pair<Id, uint32_t>, std::vector<uint8_t>> g_cb_versions;

ComPtr<ID3D11Resource> g_color_ref, g_depth_ref;
ViewUse g_color_view, g_depth_view;
float g_scene_vp_x = 0, g_scene_vp_y = 0, g_scene_vp_w = 0, g_scene_vp_h = 0; // scene viewport this frame
uint32_t g_camera_first = 0;              // first constant of the camera binding at the last scene draw
uint32_t g_camera_num = 0;                // its constant count (0 = whole buffer)
uint64_t g_stream = 0;                    // SceneFrame::stream; bumps when any input object changes
uint64_t g_stream_key[3] = {};            // incarnations of colour, depth, camera buffer behind g_stream

CameraPick g_camera;
Selection g_last_selection;
uint64_t g_frame = 0;
float g_next_jitter[2] = {0, 0};
bool g_jitter_requested = false;
neural_alias::JitterSample g_jitter{0, 0};
bool g_jitter_active = false;
bool g_jitter_written = false; // this frame's camera upload carried our sample
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
    PFN_VSSetCB vs_cb = nullptr;
    PFN_VSSetCB1 vs_cb1 = nullptr;
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
// The camera for this frame (under g_mu): the learned block re-read, else a search of the candidates.
bool CameraNow(CameraPick* out, const std::vector<uint8_t>** bytes_out, uint64_t* version_out)
{
    const Selection& s = g_model.Current();
    const auto bytes_of = [&](Id cb, uint64_t* version) -> const std::vector<uint8_t>* {
        const uint32_t v = g_model.CameraVersionThisFrame();
        auto it = v ? g_cb_versions.find({cb, v}) : g_cb_versions.end();
        if (it != g_cb_versions.end()) { *version = v; return &it->second; }
        auto lt = g_cb_latest.find(cb);
        if (lt == g_cb_latest.end()) return nullptr;
        *version = 0; // latest upload, version not attributable to this frame's scene draws
        return &lt->second;
    };
    if (s.camera_cb && g_camera.found) {
        uint64_t version = 0;
        const std::vector<uint8_t>* bytes = bytes_of(s.camera_cb, &version);
        if (bytes && CheckLearned(*bytes, g_camera, out)) { *bytes_out = bytes; *version_out = version; return true; }
        if (bytes) {
            const CameraPick p = FindCamera(*bytes, g_frame + 1);
            if (p.found) { g_camera = p; *out = p; *bytes_out = bytes; *version_out = version; return true; }
        }
        g_camera = CameraPick{};
        g_model.SetCamera(0, -1);
    }
    for (const auto& c : g_model.CameraCandidates(kMaxCandidates)) {
        auto it = g_cb_latest.find(c.first);
        if (it == g_cb_latest.end()) continue;
        const CameraPick p = FindCamera(it->second, g_frame + 1);
        if (!p.found) continue;
        g_camera = p;
        g_model.SetCamera(c.first, c.second);
        *out = p;
        *bytes_out = &it->second;
        *version_out = 0;
        return true;
    }
    return false;
}

// Hands this frame's scene to the consumer, before the game's first read of the scene colour (or
// before the list / HUD draw that holds it). Under g_mu (`lk`), on the immediate context's thread,
// before the read is forwarded. g_mu is let go during the callback so the game's other threads never
// wait behind it; the facts handed over are copies or references we hold.
void Fire(ID3D11DeviceContext* immediate, std::unique_lock<std::mutex>& lk, Boundary boundary)
{
    ++g_stats.fired;
    if (!g_cb) return SetReason("No consumer is attached.");
    const Selection sel = g_model.Current();
    if (!g_color_ref || IdOf(g_color_ref.Get()) != sel.color || !g_depth_ref || IdOf(g_depth_ref.Get()) != sel.depth) {
        ++g_stats.missed;
        return SetReason("The scene colour or depth is not held from a live game view yet.");
    }
    CameraPick cam;
    const std::vector<uint8_t>* bytes = nullptr;
    uint64_t version = 0;
    if (!CameraNow(&cam, &bytes, &version)) {
        ++g_stats.camera_missing;
        return SetReason("No camera matrix (with its inverse) was found in the scene's constant buffers.");
    }
    ++g_stats.camera_found;
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
    std::vector<uint8_t> camera_bytes(*bytes); // the callback runs without g_mu
    f.camera.buffer = sel.camera_cb;
    f.camera.vs_slot = sel.camera_slot;
    f.camera.first_constant = g_camera_first;
    f.camera.num_constants = g_camera_num;
    f.camera.version = version;
    f.camera.bytes = camera_bytes.data();
    f.camera.size = static_cast<uint32_t>(camera_bytes.size());
    f.camera.jitter_applied = g_jitter_written;
    f.camera.jitter_px[0] = g_jitter_written ? static_cast<float>(g_jitter.x_pixels) : 0.f;
    f.camera.jitter_px[1] = g_jitter_written ? static_cast<float>(g_jitter.y_pixels) : 0.f;
    f.boundary = boundary;
    f.scene_draws = g_model.SceneDrawsThisFrame();
    // The camera buffer is only named by pointer here (it may be gone), so its lifetime id is the one
    // tagged when the game last uploaded to it.
    uint64_t cam_inc = 0;
    if (sel.camera_cb) {
        auto it = g_cb_incarnation.find(sel.camera_cb);
        cam_inc = it != g_cb_incarnation.end() ? it->second : sel.camera_cb;
    }
    const uint64_t key[3] = {f.color.incarnation, f.depth.incarnation, cam_inc};
    f.selection_changed = !g_stream || std::memcmp(key, g_stream_key, sizeof(key)) != 0;
    if (f.selection_changed) {
        ++g_stream;
        std::memcpy(g_stream_key, key, sizeof(key));
    }
    f.stream = g_stream;
    g_last_selection = sel;
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

// ---------------------------------------------------------------- events
void Emit(Ctx* x, ID3D11DeviceContext* c, const Event& e, const void* cb_bytes = nullptr, size_t cb_size = 0)
{
    if (x->deferred) {
        if (x->events.size() >= kMaxListEvents) return;
        x->events.push_back(e);
        if (e.kind == Kind::CbUpdate && cb_bytes)
            x->cb_bytes.emplace_back(x->events.size() - 1, std::vector<uint8_t>(static_cast<const uint8_t*>(cb_bytes),
                                                                                static_cast<const uint8_t*>(cb_bytes) + cb_size));
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
    if (e.kind == Kind::CbUpdate && cb_bytes) {
        const auto* p = static_cast<const uint8_t*>(cb_bytes);
        g_cb_latest[e.resource].assign(p, p + cb_size);
        if (g_model.Current().camera_cb == e.resource) g_cb_versions[{e.resource, v}].assign(p, p + cb_size);
    }
}

void DrawEvent(ID3D11DeviceContext* c, uint64_t vertices, bool indirect)
{
    Ctx* x = Get(c);
    if (!x) return;
    Event e;
    e.kind = Kind::Draw;
    e.rt0 = x->rt0;
    e.depth = x->depth;
    e.depth_test = x->depth_test;
    e.vertices = vertices;
    e.indirect = indirect;
    std::memcpy(e.vs_cb, x->vs_cb, sizeof(e.vs_cb));
    if (!x->deferred && e.rt0 && e.rt0 == g_sel_color.load(std::memory_order_relaxed)) {
        // Scene-draw facts the frame model does not keep: viewport extent and the camera's offset binding.
        std::lock_guard<std::mutex> l(g_mu);
        const Selection& s = g_model.Current();
        if (e.depth == s.depth) {
            g_scene_vp_x = x->vp_x;
            g_scene_vp_y = x->vp_y;
            g_scene_vp_w = x->vp_w;
            g_scene_vp_h = x->vp_h;
            if (s.camera_slot >= 0 && s.camera_slot < kCbSlots) {
                g_camera_first = x->vs_first[s.camera_slot];
                g_camera_num = x->vs_num[s.camera_slot];
            }
        }
    }
    Emit(x, c, e);
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
void CbUpdate(ID3D11DeviceContext* c, ID3D11Resource* r, const void* bytes, size_t size)
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
    Emit(x, c, e, keep ? bytes : nullptr, keep ? std::min(size, kMaxCbBytes) : 0);
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
    uint32_t w = 0, h = 0;
    neural_alias::JitterSample sample{0, 0};
    {
        std::lock_guard<std::mutex> l(g_mu);
        if (!g_jitter_active || cb != g_model.Current().camera_cb || !g_camera.found) return false;
        learned = g_camera;
        w = g_model.Current().color_info.width;
        h = g_model.Current().color_info.height;
        sample = g_jitter;
    }
    if (!w || !h) return false;
    size = std::min(size, kMaxCbBytes);
    std::vector<uint8_t> v(static_cast<uint8_t*>(bytes), static_cast<uint8_t*>(bytes) + size);
    CameraPick now;
    if (!CheckLearned(v, learned, &now)) return false;
    na::camera::VpGeometry geo{};
    if (!na::camera::DecomposeViewProjection(now.vp, geo)) return false;
    const double aspect = geo.sy / geo.sx, screen = double(w) / h;
    if (std::fabs(aspect - screen) > 0.01 * screen) return false; // not the main camera
    const auto jittered = neural_alias::jitter_projection(now.vp, sample, w, h);
    na::camera::Matrix inv{};
    if (!na::camera::Invert(jittered, inv)) return false;
    StoreMatrix(static_cast<uint8_t*>(bytes), size, now.offset, now.layout, jittered);
    const int64_t io = static_cast<int64_t>(now.offset) + now.inverse_delta;
    if (io >= 0) StoreMatrix(static_cast<uint8_t*>(bytes), size, static_cast<uint64_t>(io), now.layout, inv);
    std::lock_guard<std::mutex> l(g_mu);
    ++g_stats.jitter_written;
    g_jitter_written = true;
    return true;
}

// ---------------------------------------------------------------- detours
void STDMETHODCALLTYPE VSSetConstantBuffers_d(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11Buffer* const* b)
{
    Ctx* x = Get(c);
    if (x && b)
        for (UINT i = 0; i < n && start + i < static_cast<UINT>(kCbSlots); ++i) { x->vs_cb[start + i] = IdOf(b[i]); x->vs_first[start + i] = 0; x->vs_num[start + i] = 0; }
    O(c, x).vs_cb(c, start, n, b);
}
void STDMETHODCALLTYPE VSSetConstantBuffers1_d(ID3D11DeviceContext1* c, UINT start, UINT n, ID3D11Buffer* const* b, const UINT* first,
                                               const UINT* num)
{
    Ctx* x = Get(c);
    if (x && b)
        for (UINT i = 0; i < n && start + i < static_cast<UINT>(kCbSlots); ++i) {
            x->vs_cb[start + i] = IdOf(b[i]);
            x->vs_first[start + i] = first ? first[i] : 0;
            x->vs_num[start + i] = first && num ? num[i] : 0;
        }
    O(c, x).vs_cb1(c, start, n, b, first, num);
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
        if (IsCandidate(id)) x->mapped[id] = m->pData;
        else if (!x->mapped.empty()) x->mapped.erase(id);
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
                JitterUpload(it->second, size, id); // before the GPU can see the bytes
                CbUpdate(c, r, it->second, size);
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
            for (size_t i = 0; i < events.size(); ++i) {
                const uint32_t v = g_model.Feed(events[i]);
                if (events[i].kind == Kind::CbUpdate && next_bytes < bytes.size() && bytes[next_bytes].first == i) {
                    if (g_model.Current().camera_cb == events[i].resource) g_cb_versions[{events[i].resource, v}] = bytes[next_bytes].second;
                    g_cb_latest[events[i].resource] = bytes[next_bytes].second;
                    ++next_bytes;
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
void OnPresent(IDXGISwapChain* swap)
{
    if (t_inside) return;
    ID3D11Device* game = g_device_raw.load(std::memory_order_acquire);
    if (!game) return;
    std::lock_guard<std::mutex> l(g_mu);
    if (swap != g_swap) {
        // A swap chain of the game's device (fullscreen toggles can recreate it) is adopted; one of
        // another device (a launcher window, a video, our own) is ignored.
        Inside in;
        ComPtr<ID3D11Device> d;
        if (FAILED(swap->GetDevice(IID_PPV_ARGS(&d))) || d.Get() != game) return;
        g_swap = swap;
        DXGI_SWAP_CHAIN_DESC sd{};
        swap->GetDesc(&sd);
        g_model.SetSwapChainSize(sd.BufferDesc.Width, sd.BufferDesc.Height);
    }
    ++g_frame;
    ++g_stats.frames;
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
    g_cb_versions.clear();
    g_sel_color.store(s.color, std::memory_order_relaxed);
    g_scene_vp_x = g_scene_vp_y = g_scene_vp_w = g_scene_vp_h = 0;
    // Next frame's jitter: the backend's sample, written into the main camera's uploads.
    g_jitter_written = false;
    g_jitter_active = g_jitter_requested && g_camera.found;
    g_jitter = neural_alias::JitterSample{g_next_jitter[0], g_next_jitter[1]};
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
    if (s == g_swap) {
        // Release everything that could hold a back buffer, or ResizeBuffers fails.
        std::lock_guard<std::mutex> l(g_mu);
        g_color_ref.Reset();
        g_depth_ref.Reset();
        g_model.Forget(g_model.Current().color); // the scene targets are usually recreated with the new size
        g_sel_color.store(0);
    }
    const HRESULT hr = g_resize(s, n, w, h, f, flags);
    if (s == g_swap && SUCCEEDED(hr)) {
        std::lock_guard<std::mutex> l(g_mu);
        DXGI_SWAP_CHAIN_DESC sd{};
        s->GetDesc(&sd);
        g_model.SetSwapChainSize(sd.BufferDesc.Width, sd.BufferDesc.Height);
    }
    return hr;
}

// ---------------------------------------------------------------- install
LRESULT CALLBACK DummyProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

bool HookCtx(ID3D11DeviceContext* ctx, int set, bool deferred)
{
    Originals& o = g_o[set];
    struct H { uint32_t slot; void* detour; void** out; };
    std::vector<H> hooks = {
        {kVSSetConstantBuffers, reinterpret_cast<void*>(&VSSetConstantBuffers_d), reinterpret_cast<void**>(&o.vs_cb)},
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
    }
    for (const H& h : hooks)
        if (!na::hooking::PrepareVTableSlot(ctx, h.slot, h.detour, h.out)) return false;
    return true;
}

bool InstallHooks()
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
    ok = ok && na::hooking::PrepareVTableSlot(swap.Get(), kPresent, reinterpret_cast<void*>(&Present_d), reinterpret_cast<void**>(&g_present));
    ok = ok && na::hooking::PrepareVTableSlot(swap.Get(), kResizeBuffers, reinterpret_cast<void*>(&ResizeBuffers_d), reinterpret_cast<void**>(&g_resize));
    ok = ok && (!swap1 || na::hooking::PrepareVTableSlot(swap1.Get(), kPresent1, reinterpret_cast<void*>(&Present1_d), reinterpret_cast<void**>(&g_present1)));
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

bool Install(void* device, SceneCallback cb, void* user)
{
    std::lock_guard<std::mutex> g(g_install_mu);
    {
        std::lock_guard<std::mutex> l(g_mu);
        g_cb = cb;
        g_cb_user = user;
    }
    if (device) g_device_raw.store(static_cast<ID3D11Device*>(device), std::memory_order_release);
    if (g_installed.load()) return true;
    if (!InstallHooks()) return false;
    g_installed.store(true);
    return true;
}

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
    return std::snprintf(out, n,
                         "native11 frames=%llu fired=%llu missed=%llu unplaceable=%llu contradictions=%llu selection_changes=%llu "
                         "camera_found=%llu camera_missing=%llu jitter_written=%llu%s%s%s",
                         static_cast<unsigned long long>(s.frames), static_cast<unsigned long long>(s.fired),
                         static_cast<unsigned long long>(s.missed), static_cast<unsigned long long>(s.unplaceable),
                         static_cast<unsigned long long>(s.contradictions), static_cast<unsigned long long>(s.selection_changes),
                         static_cast<unsigned long long>(s.camera_found), static_cast<unsigned long long>(s.camera_missing),
                         static_cast<unsigned long long>(s.jitter_written), s.last_miss && s.last_miss[0] ? " last_miss=\"" : "",
                         s.last_miss && s.last_miss[0] ? s.last_miss : "", s.last_miss && s.last_miss[0] ? "\"" : "");
}

} // namespace na::native11
