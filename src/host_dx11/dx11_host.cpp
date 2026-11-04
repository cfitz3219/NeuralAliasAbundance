// DX11 host. See dx11_host.hpp and docs/DX11-HOOKS.md.
//
// Hook set (vtable functions of d3d11.dll's own context classes, found on a
// small private device; hooking the function covers every game context of that
// class). Immediate and deferred contexts are different classes, so both are
// hooked:
//   state:  OMSetRenderTargets(+AndUAVs), OMSetDepthStencilState,
//           VSSetConstantBuffers(+1), ClearState
//   events: Draw* (6), ClearDepthStencilView, PS/CS/VSSetShaderResources,
//           CopyResource / CopySubresourceRegion / ResolveSubresource (reads),
//           Map/Unmap + UpdateSubresource(+1) on constant buffers
//   lists:  FinishCommandList (deferred), ExecuteCommandList (immediate)
//   frame:  IDXGISwapChain::Present / Present1 / ResizeBuffers
//   device: CreateSamplerState (mip bias, only with jitter)
//
// Rules the hooks follow:
//   * never act on a context of another device (video players, overlays);
//   * everything the bridge does runs with t_inside set, so our own calls are
//     forwarded without being observed;
//   * the scene colour and depth are kept alive by references taken from the
//     game's own live views, and released before ResizeBuffers (a held back
//     buffer would make ResizeBuffers fail);
//   * any missing evidence skips the frame with a reason in the log; the game's
//     frame is never changed then.
#include "host_dx11/dx11_host.hpp"

#include "bridge/dx11_bridge.hpp"
#include "core/temporal_jitter.hpp"
#include "host_dx11/camera_pick.hpp"
#include "host_dx11/frame_model.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
#include "providers/camera/camera_tracker.hpp"

#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <share.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace na::dx11 {
namespace {

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------- slots
// ID3D11DeviceContext vtable (d3d11.h order).
enum CtxSlot : uint32_t {
    kVSSetConstantBuffers = 7, kPSSetShaderResources = 8, kDrawIndexed = 12, kDraw = 13, kMap = 14, kUnmap = 15,
    kDrawIndexedInstanced = 20, kDrawInstanced = 21, kVSSetShaderResources = 25, kOMSetRenderTargets = 33,
    kOMSetRenderTargetsAndUAVs = 34, kOMSetDepthStencilState = 36, kDrawIndexedInstancedIndirect = 39,
    kDrawInstancedIndirect = 40, kCopySubresourceRegion = 46, kCopyResource = 47, kUpdateSubresource = 48,
    kClearDepthStencilView = 53, kResolveSubresource = 57, kExecuteCommandList = 58, kCSSetShaderResources = 67,
    kClearState = 110, kFinishCommandList = 114,
    // ID3D11DeviceContext1
    kUpdateSubresource1 = 116, kVSSetConstantBuffers1 = 119,
};
enum SwapSlot : uint32_t { kPresent = 8, kResizeBuffers = 13, kPresent1 = 22 };
constexpr uint32_t kCreateSamplerState = 23; // ID3D11Device

constexpr size_t kMaxCbBytes = 16 * 1024;   // camera blocks are far smaller (mapped memory is slow to read back)
constexpr size_t kMaxListEvents = 200000;  // per deferred recording; beyond this the list is marked incomplete
constexpr size_t kMaxCandidates = 4;        // constant buffers searched for the camera
constexpr size_t kMaxLists = 512;           // recorded command lists kept

// ---------------------------------------------------------------- state
struct Ctx {
    bool game = false;     // a context of the game's device
    bool deferred = false;
    Id rt0 = 0, depth = 0;
    bool depth_test = true;
    Id vs_cb[kCbSlots] = {};
    // deferred recording
    std::vector<Event> events;
    std::vector<std::pair<size_t, std::vector<uint8_t>>> cb_bytes; // (event index, bytes) for CbUpdate events
    std::unordered_map<Id, void*> mapped; // candidate constant buffers mapped right now
    void reset_state()
    {
        rt0 = depth = 0;
        depth_test = true;
        std::memset(vs_cb, 0, sizeof(vs_cb));
    }
};

struct ListRecord {
    std::vector<Event> events;
    std::vector<std::pair<size_t, std::vector<uint8_t>>> cb_bytes;
    uint64_t order = 0;
};

HostConfig g_cfg;
std::atomic<bool> g_installed{false};
std::mutex g_install_mu;

std::mutex g_mu; // model, lists, camera bytes, bridge, stats, logging
FrameModel g_model;
std::unordered_map<ID3D11DeviceContext*, std::unique_ptr<Ctx>> g_ctx;
std::map<ID3D11CommandList*, ListRecord> g_lists;
uint64_t g_list_order = 0;
ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11DeviceContext> g_immediate;
IDXGISwapChain* g_swap = nullptr; // not owned: identity only
std::atomic<ID3D11Device*> g_device_raw{nullptr};
std::atomic<Id> g_sel_color{0}; // the selected scene colour, readable from any thread

// Candidate constant buffers whose bytes we keep (lock-free check in Map/Unmap).
std::atomic<Id> g_cand[kMaxCandidates + 1] = {};
// Bytes of candidate buffers: latest, and per version this frame for the camera.
std::unordered_map<Id, std::vector<uint8_t>> g_cb_latest;
std::map<std::pair<Id, uint32_t>, std::vector<uint8_t>> g_cb_versions;

// Live references to the selected scene textures (from the game's own views).
ComPtr<ID3D11Resource> g_color_ref, g_depth_ref;
ComPtr<ID3D11RenderTargetView> g_target_rtv;
Id g_target_for = 0;

CameraPick g_camera;
na::camera::CameraTracker g_tracker;
bool g_tracker_reset = true;
uint64_t g_frame = 0;
neural_alias::JitterSample g_jitter{0, 0};
bool g_jitter_active = false;
bool g_jitter_written = false; // this frame's camera upload carried our sample
std::atomic<float> g_main_aspect{0.f};

na::bridge::Dx11Bridge g_bridge;
uint32_t g_bridge_w = 0, g_bridge_h = 0;
HostStats g_stats;
FILE* g_log = nullptr;
size_t g_log_bytes = 0;
Selection g_logged_sel;
bool g_logged_camera = false;
char g_last_logged_reason[192] = {};

thread_local bool t_inside = false; // our own calls (bridge, getters) are not observed
thread_local bool t_in_present = false;
thread_local ID3D11DeviceContext* t_last_ctx = nullptr;
thread_local Ctx* t_last = nullptr;

void Log(const char* fmt, ...)
{
    if (!g_log || g_log_bytes > (4u << 20)) return;
    va_list a;
    va_start(a, fmt);
    char line[1024];
    const int n = std::vsnprintf(line, sizeof(line), fmt, a);
    va_end(a);
    if (n <= 0) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    g_log_bytes += static_cast<size_t>(std::fprintf(g_log, "%02u:%02u:%02u.%03u f=%llu %s\n", t.wHour, t.wMinute, t.wSecond,
                                                    t.wMilliseconds, static_cast<unsigned long long>(g_frame), line));
    std::fflush(g_log);
}

void SetReason(const char* r)
{
    strncpy_s(g_stats.reason, r, _TRUNCATE);
    if (std::strcmp(g_last_logged_reason, r) != 0) {
        strncpy_s(g_last_logged_reason, r, _TRUNCATE);
        Log("skip: %s", r);
    }
}

Id IdOf(ID3D11Resource* r) { return reinterpret_cast<Id>(r); }

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

// The Ctx for a context of the game's device (nullptr otherwise, or while inside our own work).
Ctx* Get(ID3D11DeviceContext* c)
{
    if (t_inside || !c) return nullptr;
    if (t_last_ctx == c) return t_last && t_last->game ? t_last : nullptr;
    ID3D11Device* game = g_device_raw.load(std::memory_order_acquire);
    if (!game) return nullptr; // the game's device is known at its first Present
    std::lock_guard<std::mutex> l(g_mu);
    auto& slot = g_ctx[c];
    if (!slot) {
        slot = std::make_unique<Ctx>();
        t_inside = true;
        ComPtr<ID3D11Device> d;
        c->GetDevice(&d);
        slot->game = d.Get() == game;
        slot->deferred = c->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED;
        t_inside = false;
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
using PFN_CreateSampler = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_SAMPLER_DESC*, ID3D11SamplerState**);

// Two sets: immediate-context class and deferred-context class (index 0 / 1).
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
Originals g_o[2];
PFN_Present g_present = nullptr;
PFN_Present1 g_present1 = nullptr;
PFN_Resize g_resize = nullptr;
PFN_CreateSampler g_create_sampler = nullptr;

// The original for this context's class. Immediate and deferred functions are
// hooked separately; when both classes share one function, both entries hold
// the same trampoline.
inline const Originals& O(ID3D11DeviceContext* c, Ctx* x)
{
    if (x) return g_o[x->deferred ? 1 : 0];
    return g_o[c->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED ? 1 : 0];
}

// ---------------------------------------------------------------- events
void FireBeforeRead(ID3D11DeviceContext* immediate); // below

// Feeds one event for a context: live for the immediate one, recorded for deferred ones.
void Emit(Ctx* x, ID3D11DeviceContext* c, const Event& e, const void* cb_bytes = nullptr, size_t cb_size = 0)
{
    if (x->deferred) {
        if (x->events.size() >= kMaxListEvents) return; // runaway recording: drop (the model then misses this list's tail)
        x->events.push_back(e);
        if (e.kind == Kind::CbUpdate && cb_bytes)
            x->cb_bytes.emplace_back(x->events.size() - 1, std::vector<uint8_t>(static_cast<const uint8_t*>(cb_bytes),
                                                                                static_cast<const uint8_t*>(cb_bytes) + cb_size));
        return;
    }
    std::lock_guard<std::mutex> l(g_mu);
    if (e.kind == Kind::Read && g_model.ShouldFireBeforeRead(e.resource)) {
        g_model.MarkFired();
        FireBeforeRead(c);
    }
    const uint32_t v = g_model.Feed(e);
    if (e.kind == Kind::CbUpdate && cb_bytes) {
        std::vector<uint8_t> b(static_cast<const uint8_t*>(cb_bytes), static_cast<const uint8_t*>(cb_bytes) + cb_size);
        if (g_model.Current().camera_cb == e.resource) g_cb_versions[{e.resource, v}] = b;
        g_cb_latest[e.resource] = std::move(b);
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
        t_inside = true;
        const Id r = ResourceOfView(views[i]);
        t_inside = false;
        if (r != color) continue;
        // Keep the colour alive from the game's own live view (before a possible fire).
        if (!x->deferred) {
            std::lock_guard<std::mutex> l(g_mu);
            if (IdOf(g_color_ref.Get()) != r) {
                g_color_ref.Reset();
                t_inside = true;
                views[i]->GetResource(&g_color_ref);
                t_inside = false;
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
    Emit(x, c, e, keep ? bytes : nullptr, keep ? (size < kMaxCbBytes ? size : kMaxCbBytes) : 0);
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

// Jitter (J1 for DX11): rewrites the main camera's ViewProjection (and its
// inverse) in the game's own upload, before the GPU sees it. Only the camera
// whose aspect matches the screen (not shadow or reflection cameras).
bool JitterUpload(void* bytes, size_t size, Id cb)
{
    if (!g_cfg.jitter || !bytes) return false;
    CameraPick learned;
    uint32_t w = 0, h = 0;
    {
        std::lock_guard<std::mutex> l(g_mu);
        if (!g_jitter_active || cb != g_model.Current().camera_cb || !g_camera.found) return false;
        learned = g_camera;
        w = g_bridge_w;
        h = g_bridge_h;
    }
    if (!w || !h) return false;
    if (size > kMaxCbBytes) size = kMaxCbBytes;
    std::vector<uint8_t> v(static_cast<uint8_t*>(bytes), static_cast<uint8_t*>(bytes) + size);
    CameraPick now;
    if (!CheckLearned(v, learned, &now)) return false;
    na::camera::VpGeometry geo{};
    if (!na::camera::DecomposeViewProjection(now.vp, geo)) return false;
    const double aspect = geo.sy / geo.sx, screen = double(w) / h;
    if (std::fabs(aspect - screen) > 0.01 * screen) return false; // not the main camera
    const auto jittered = neural_alias::jitter_projection(now.vp, g_jitter, w, h);
    na::camera::Matrix inv{};
    if (!na::camera::Invert(jittered, inv)) return false;
    StoreMatrix(static_cast<uint8_t*>(bytes), size, now.offset, now.layout, jittered);
    const int64_t io = static_cast<int64_t>(now.offset) + now.inverse_delta;
    if (io >= 0) StoreMatrix(static_cast<uint8_t*>(bytes), size, static_cast<uint64_t>(io), now.layout, inv);
    std::lock_guard<std::mutex> l(g_mu);
    ++g_stats.jitter_writes;
    g_jitter_written = true;
    return true;
}

// ---------------------------------------------------------------- the DLSS point
DXGI_FORMAT TypedFormat(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R11G11B10_FLOAT: return f;
    default: return f;
    }
}
bool IsFloat(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R16G16B16A16_TYPELESS || f == DXGI_FORMAT_R32G32B32A32_FLOAT ||
           f == DXGI_FORMAT_R32G32B32A32_TYPELESS || f == DXGI_FORMAT_R11G11B10_FLOAT;
}

// The camera for this frame (under g_mu). Searches the candidate buffers once,
// then re-reads the learned offset.
bool CameraNow(CameraPick* out)
{
    const Selection& s = g_model.Current();
    if (s.camera_cb && g_camera.found) {
        const uint32_t v = g_model.CameraVersionThisFrame();
        auto it = v ? g_cb_versions.find({s.camera_cb, v}) : g_cb_versions.end();
        const std::vector<uint8_t>* bytes = nullptr;
        if (it != g_cb_versions.end()) bytes = &it->second;
        else {
            auto lt = g_cb_latest.find(s.camera_cb);
            if (lt != g_cb_latest.end()) bytes = &lt->second;
        }
        if (bytes && CheckLearned(*bytes, g_camera, out)) return true;
        if (bytes) {
            const CameraPick p = FindCamera(*bytes, g_frame + 1);
            if (p.found) { g_camera = p; *out = p; return true; }
        }
        Log("camera: the learned camera block no longer holds a camera; searching again");
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
        g_tracker_reset = true;
        Log("camera: found in vertex constant buffer slot %d at byte %llu (%s, inverse %+lld bytes, %s depth)", c.second,
            static_cast<unsigned long long>(p.offset), p.layout == na::camera::Layout::RowMajor ? "row-major" : "column-major",
            static_cast<long long>(p.inverse_delta), p.reversed ? "reversed" : "standard");
        *out = p;
        return true;
    }
    return false;
}

// Runs DLSS on the scene colour, in place, before the game's first read of it.
// Called under g_mu, on the immediate context's thread, before the read is forwarded.
void FireBeforeRead(ID3D11DeviceContext* immediate)
{
    ++g_stats.fired;
    const Selection sel = g_model.Current();
    if (!g_color_ref || IdOf(g_color_ref.Get()) != sel.color || !g_depth_ref || IdOf(g_depth_ref.Get()) != sel.depth) {
        ++g_stats.refused;
        return SetReason("The scene colour or depth is not held from a live game view yet.");
    }
    if (sel.color_info.samples > 1) {
        ++g_stats.refused;
        return SetReason("The scene colour is multisampled; DLSS needs it resolved first (not supported yet).");
    }
    const uint32_t w = sel.color_info.width, h = sel.color_info.height;
    CameraPick cam;
    if (!CameraNow(&cam)) {
        ++g_stats.no_camera;
        return SetReason("No camera matrix (with its inverse) was found in the scene's constant buffers.");
    }
    g_stats.camera = true;
    g_stats.camera_offset = cam.offset;
    g_stats.depth_reversed = cam.reversed;

    // Bridge at this size (restart on any change: resize, Alt-Tab, new scene target).
    t_inside = true;
    struct Leave { ~Leave() { t_inside = false; } } leave;
    if (!g_bridge.Started() || g_bridge.NeedsRestart() || g_bridge_w != w || g_bridge_h != h) {
        g_bridge.Stop();
        na::bridge::Config bc;
        bc.limits = na::bridge::SessionLimits{w, h, w, h};
        bc.backend = na::bridge::Backend::Helper; // camera motion is built in the DX12 helper
        bc.wait = g_cfg.gpu_wait ? na::bridge::WaitMode::Gpu : na::bridge::WaitMode::CpuDeadline;
        bc.helper_path = g_cfg.helper_path;
        bc.data_path = g_cfg.data_path;
        bc.copy_test = g_cfg.copy_test;
        if (const char* why = g_bridge.Start(immediate, bc)) {
            ++g_stats.failed;
            return SetReason(why);
        }
        g_bridge_w = w;
        g_bridge_h = h;
        g_tracker_reset = true;
        Log("bridge: started for %ux%u (helper %s)", w, h, g_cfg.copy_test ? "copy test" : "DLSS");
    }
    if (g_target_for != sel.color || !g_target_rtv) {
        g_target_rtv.Reset();
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = TypedFormat(static_cast<DXGI_FORMAT>(sel.color_info.format));
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(g_device->CreateRenderTargetView(g_color_ref.Get(), &rd, &g_target_rtv))) {
            ++g_stats.refused;
            return SetReason("Could not make a render view on the scene colour.");
        }
        g_target_for = sel.color;
    }

    if (g_tracker_reset) g_tracker.Reset();
    const double injected[2] = {g_jitter.x_pixels, g_jitter.y_pixels};
    const na::camera::CameraFrame f = g_tracker.Update(cam.vp, g_frame, w, h, g_jitter_written ? injected : nullptr);
    if (!f.valid) {
        ++g_stats.no_camera;
        return SetReason("The camera matrix could not be inverted.");
    }
    const bool reset = g_tracker_reset || f.camera_cut;
    g_tracker_reset = false;
    {
        na::camera::VpGeometry geo{};
        if (na::camera::DecomposeViewProjection(f.unjittered, geo)) g_main_aspect.store(static_cast<float>(geo.sy / geo.sx));
    }

    na::bridge::FrameRecord rec;
    rec.source_frame = g_frame;
    rec.render_w = rec.output_w = w;
    rec.render_h = rec.output_h = h;
    rec.mode = static_cast<uint32_t>(na::bridge::Mode::DLAA);
    rec.preset = g_cfg.preset;
    rec.jitter_x = static_cast<float>(f.jitter_px_x);
    rec.jitter_y = static_cast<float>(f.jitter_px_y);
    if (std::fabs(rec.jitter_x) > 1.f || std::fabs(rec.jitter_y) > 1.f) rec.jitter_x = rec.jitter_y = 0.f; // estimated jitter out of range
    rec.flags = na::bridge::kCameraMotion | na::bridge::kAutoExposure;
    if (IsFloat(static_cast<DXGI_FORMAT>(sel.color_info.format))) rec.flags |= na::bridge::kHdr;
    if (cam.reversed) rec.flags |= na::bridge::kDepthInverted;
    if (reset) rec.flags |= na::bridge::kReset;
    rec.depth_clear = cam.reversed ? 0.f : 1.f;
    const auto reproject = na::camera::ComposeReprojectionRow(f.inv_view_proj_current_row, f.view_proj_previous_row);
    for (int i = 0; i < 16; ++i) rec.reproject[i] = static_cast<float>(reproject[i]);

    na::bridge::Inputs in;
    in.color_texture = g_color_ref.Get();
    in.depth_texture = g_depth_ref.Get();
    const na::bridge::Result r = g_bridge.Process(immediate, in, g_target_rtv.Get(), rec);
    switch (r) {
    case na::bridge::Result::Ok: ++g_stats.delivered; g_stats.reason[0] = 0; g_last_logged_reason[0] = 0; break;
    case na::bridge::Result::Repeated: ++g_stats.repeated; break;
    case na::bridge::Result::NotReady: ++g_stats.not_ready; break;
    case na::bridge::Result::Failed:
        ++g_stats.failed;
        SetReason(g_bridge.Reason());
        break;
    }
    if (r == na::bridge::Result::NotReady && g_bridge.Reason()[0]) SetReason(g_bridge.Reason());
    g_stats.helper_motion_frames = g_bridge.HelperMotionFrames();
}

// ---------------------------------------------------------------- detours
#define CTX_PRE(c) Ctx* x = Get(c)

void STDMETHODCALLTYPE VSSetConstantBuffers_d(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11Buffer* const* b)
{
    CTX_PRE(c);
    if (x && b)
        for (UINT i = 0; i < n && start + i < static_cast<UINT>(kCbSlots); ++i) x->vs_cb[start + i] = IdOf(b[i]);
    O(c, x).vs_cb(c, start, n, b);
}
void STDMETHODCALLTYPE VSSetConstantBuffers1_d(ID3D11DeviceContext1* c, UINT start, UINT n, ID3D11Buffer* const* b, const UINT* first,
                                               const UINT* num)
{
    CTX_PRE(c);
    if (x && b)
        for (UINT i = 0; i < n && start + i < static_cast<UINT>(kCbSlots); ++i) x->vs_cb[start + i] = IdOf(b[i]);
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
    CTX_PRE(c);
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
    CTX_PRE(c);
    if (x) {
        const Id id = IdOf(r);
        auto it = x->mapped.find(id);
        if (it != x->mapped.end()) {
            UINT size = 0;
            t_inside = true;
            const bool cb = IsConstantBuffer(r, &size);
            t_inside = false;
            if (cb && it->second) {
                JitterUpload(it->second, size, id); // before the GPU can see the bytes
                CbUpdate(c, r, it->second, size);
            }
            x->mapped.erase(it);
        } else {
            // Not a candidate: still a version change for the model.
            UINT size = 0;
            t_inside = true;
            const bool cb = IsConstantBuffer(r, &size);
            t_inside = false;
            if (cb) CbUpdate(c, r, nullptr, 0);
        }
    }
    O(c, x).unmap(c, r, sub);
}
void UpdateCommon(ID3D11DeviceContext* c, Ctx* x, ID3D11Resource* r, const D3D11_BOX* box, const void*& src, std::vector<uint8_t>& tmp)
{
    if (!x || !r || !src) return;
    UINT size = 0;
    t_inside = true;
    const bool cb = IsConstantBuffer(r, &size);
    t_inside = false;
    if (!cb) return;
    if (box || !IsCandidate(IdOf(r))) { CbUpdate(c, r, nullptr, 0); return; } // constant buffers are updated whole
    tmp.assign(static_cast<const uint8_t*>(src), static_cast<const uint8_t*>(src) + size);
    if (JitterUpload(tmp.data(), tmp.size(), IdOf(r))) src = tmp.data(); // the game's upload, jittered
    CbUpdate(c, r, tmp.data(), tmp.size());
}
void STDMETHODCALLTYPE UpdateSubresource_d(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* src, UINT rp,
                                           UINT dp)
{
    CTX_PRE(c);
    std::vector<uint8_t> tmp;
    UpdateCommon(c, x, r, box, src, tmp);
    O(c, x).update(c, r, sub, box, src, rp, dp);
}
void STDMETHODCALLTYPE UpdateSubresource1_d(ID3D11DeviceContext1* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* src,
                                            UINT rp, UINT dp, UINT flags)
{
    CTX_PRE(c);
    std::vector<uint8_t> tmp;
    UpdateCommon(c, x, r, box, src, tmp);
    O(c, x).update1(c, r, sub, box, src, rp, dp, flags);
}
void TrackTargets(Ctx* x, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv)
{
    t_inside = true;
    ID3D11Resource* rt = nullptr;
    if (n && rtvs && rtvs[0]) rtvs[0]->GetResource(&rt);
    ID3D11Resource* ds = nullptr;
    if (dsv) dsv->GetResource(&ds);
    t_inside = false;
    x->rt0 = IdOf(rt);
    x->depth = IdOf(ds);
    if (rt || ds) {
        std::lock_guard<std::mutex> l(g_mu);
        if (rt) g_model.SetInfo(IdOf(rt), InfoOf(rt, false));
        if (ds) {
            g_model.SetInfo(IdOf(ds), InfoOf(ds, true));
            // Keep the selected depth alive from the game's own live view.
            if (IdOf(ds) == g_model.Current().depth && g_depth_ref.Get() != ds) g_depth_ref = ds;
        }
        if (rt && IdOf(rt) == g_model.Current().color && g_color_ref.Get() != rt) g_color_ref = rt;
    }
    if (rt) rt->Release();
    if (ds) ds->Release();
}
void STDMETHODCALLTYPE OMSetRenderTargets_d(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv)
{
    CTX_PRE(c);
    if (x) TrackTargets(x, n, rtvs, dsv);
    O(c, x).om_rt(c, n, rtvs, dsv);
}
void STDMETHODCALLTYPE OMSetRenderTargetsAndUAVs_d(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv,
                                                   UINT us, UINT un, ID3D11UnorderedAccessView* const* uavs, const UINT* counts)
{
    CTX_PRE(c);
    if (x && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) TrackTargets(x, n, rtvs, dsv);
    O(c, x).om_rt_uav(c, n, rtvs, dsv, us, un, uavs, counts);
}
void STDMETHODCALLTYPE OMSetDepthStencilState_d(ID3D11DeviceContext* c, ID3D11DepthStencilState* s, UINT ref)
{
    CTX_PRE(c);
    if (x) {
        if (!s) x->depth_test = true; // default state: depth test on
        else {
            D3D11_DEPTH_STENCIL_DESC d{};
            s->GetDesc(&d);
            x->depth_test = d.DepthEnable != FALSE;
        }
    }
    O(c, x).om_dss(c, s, ref);
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
    CTX_PRE(c);
    if (x && v && (flags & D3D11_CLEAR_DEPTH)) {
        t_inside = true;
        const Id id = ResourceOfView(v);
        t_inside = false;
        Event e;
        e.kind = Kind::ClearDepth;
        e.resource = id;
        Emit(x, c, e);
    }
    O(c, x).clear_dsv(c, v, flags, d, s);
}
void STDMETHODCALLTYPE ClearState_d(ID3D11DeviceContext* c)
{
    CTX_PRE(c);
    if (x) x->reset_state();
    O(c, x).clear_state(c);
}
HRESULT STDMETHODCALLTYPE FinishCommandList_d(ID3D11DeviceContext* c, BOOL restore, ID3D11CommandList** out)
{
    CTX_PRE(c);
    const HRESULT hr = O(c, x).finish(c, restore, out);
    if (x) {
        if (SUCCEEDED(hr) && out && *out) {
            std::lock_guard<std::mutex> l(g_mu);
            ListRecord& rec = g_lists[*out];
            rec.events = std::move(x->events);
            rec.cb_bytes = std::move(x->cb_bytes);
            rec.order = ++g_list_order;
            ++g_stats.lists_recorded;
            if (g_lists.size() > kMaxLists) { // drop the oldest records (lists the game has long released)
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
    CTX_PRE(c);
    if (x && !x->deferred && list) {
        std::lock_guard<std::mutex> l(g_mu);
        auto it = g_lists.find(list);
        if (it != g_lists.end()) {
            ++g_stats.lists_executed;
            if (g_model.ShouldFireBeforeList(g_model.Summarize(it->second.events))) {
                g_model.MarkFired();
                FireBeforeRead(c);
            }
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
    // Drivers without native command lists (and WARP) have d3d11 replay the list
    // through the immediate context's own (hooked) functions: those calls are
    // already in the model from the recording, so they are not observed again.
    const bool was_inside = t_inside;
    t_inside = true;
    O(c, x).execute(c, list, restore);
    t_inside = was_inside;
    if (x && !restore) x->reset_state();
}

// ---------------------------------------------------------------- frame boundary
void LogFrame()
{
    const auto& st = g_model.GetStats();
    const Selection& s = g_model.Current();
    if (s.depth != g_logged_sel.depth || s.color != g_logged_sel.color) {
        g_logged_sel = s;
        if (s.valid())
            Log("selection: scene depth %ux%u format %u, scene colour %ux%u format %u x%u samples", s.depth_info.width, s.depth_info.height,
                s.depth_info.format, s.color_info.width, s.color_info.height, s.color_info.format, s.color_info.samples);
        else
            Log("selection: none");
    }
    const bool periodic = g_frame <= 600 ? g_frame % 60 == 0 : g_frame % 600 == 0;
    if (!periodic) return;
    Log("frame: draws=%u scene=%u reads=%u point=read#%u fired=%llu delivered=%llu repeated=%llu not_ready=%llu failed=%llu "
        "no_camera=%llu refused=%llu missed=%llu unplaceable=%llu contradictions=%llu camera_share=%.2f lists=%llu/%llu "
        "jitter_writes=%llu helper_motion=%u%s%s",
        st.last_draws, st.last_scene_draws, st.last_reads, st.learned_read, static_cast<unsigned long long>(g_stats.fired),
        static_cast<unsigned long long>(g_stats.delivered), static_cast<unsigned long long>(g_stats.repeated),
        static_cast<unsigned long long>(g_stats.not_ready), static_cast<unsigned long long>(g_stats.failed),
        static_cast<unsigned long long>(g_stats.no_camera), static_cast<unsigned long long>(g_stats.refused),
        static_cast<unsigned long long>(st.missed), static_cast<unsigned long long>(st.unplaceable),
        static_cast<unsigned long long>(st.contradictions), st.camera_share, static_cast<unsigned long long>(g_stats.lists_executed),
        static_cast<unsigned long long>(g_stats.lists_recorded), static_cast<unsigned long long>(g_stats.jitter_writes),
        g_stats.helper_motion_frames, st.last_miss[0] ? " miss: " : "", st.last_miss);
}

void OnPresent(IDXGISwapChain* swap)
{
    if (t_inside) return;
    if (!g_device_raw.load()) {
        ComPtr<ID3D11Device> d;
        if (FAILED(swap->GetDevice(IID_PPV_ARGS(&d))) || !d) return; // a DX12 swap chain: not ours
        std::lock_guard<std::mutex> l(g_mu);
        g_device = d;
        g_device->GetImmediateContext(&g_immediate);
        g_swap = swap;
        g_device_raw.store(d.Get(), std::memory_order_release);
        g_stats.device_found = true;
        DXGI_SWAP_CHAIN_DESC sd{};
        swap->GetDesc(&sd);
        g_model.SetSwapChainSize(sd.BufferDesc.Width, sd.BufferDesc.Height);
        Log("device: DX11 game device found at its first Present (%ux%u, format %u, feature level 0x%x)", sd.BufferDesc.Width,
            sd.BufferDesc.Height, sd.BufferDesc.Format, d->GetFeatureLevel());
        return;
    }
    std::lock_guard<std::mutex> l(g_mu);
    if (swap != g_swap) {
        // A new swap chain of the same device (fullscreen toggles can recreate it) is adopted;
        // one of another device (a launcher window, a video) is ignored.
        ComPtr<ID3D11Device> d;
        if (FAILED(swap->GetDevice(IID_PPV_ARGS(&d))) || d.Get() != g_device.Get()) return;
        g_swap = swap;
        DXGI_SWAP_CHAIN_DESC sd{};
        swap->GetDesc(&sd);
        g_model.SetSwapChainSize(sd.BufferDesc.Width, sd.BufferDesc.Height);
        Log("swap chain: the game presents through a new swap chain now (%ux%u)", sd.BufferDesc.Width, sd.BufferDesc.Height);
    }
    ++g_frame;
    ++g_stats.presents;
    g_model.EndFrame();
    // Candidates for next frame: the learned camera buffer first, then the ranking.
    size_t k = 0;
    const Selection& s = g_model.Current();
    if (s.camera_cb) g_cand[k++].store(s.camera_cb);
    for (const auto& c : g_model.CameraCandidates(kMaxCandidates))
        if (k < kMaxCandidates + 1 && c.first != s.camera_cb) g_cand[k++].store(c.first);
    while (k < kMaxCandidates + 1) g_cand[k++].store(0);
    g_cb_versions.clear();
    g_sel_color.store(s.color, std::memory_order_relaxed);
    g_stats.selected = s.valid();
    g_stats.color_w = s.color_info.width;
    g_stats.color_h = s.color_info.height;
    g_stats.color_format = s.color_info.format;
    g_stats.depth_format = s.depth_info.format;
    g_stats.last_draws = g_model.GetStats().last_draws;
    g_stats.last_scene_draws = g_model.GetStats().last_scene_draws;
    // Next frame's jitter sample.
    g_jitter_written = false;
    g_jitter_active = g_cfg.jitter && g_camera.found && g_bridge.Started();
    g_jitter = g_jitter_active ? neural_alias::temporal_jitter(static_cast<uint32_t>(g_frame), g_cfg.jitter_phases ? g_cfg.jitter_phases : 8)
                               : neural_alias::JitterSample{0, 0};
    if (g_camera.found != g_logged_camera) {
        g_logged_camera = g_camera.found;
        if (!g_camera.found) Log("camera: none");
    }
    LogFrame();
}

// One frame boundary per game Present, even if one Present calls the other inside DXGI.
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
        t_inside = true;
        g_bridge.Stop();
        t_inside = false;
        g_target_rtv.Reset();
        g_target_for = 0;
        g_color_ref.Reset();
        g_depth_ref.Reset();
        g_model.Forget(g_model.Current().color); // the scene targets are usually recreated with the new size
        g_sel_color.store(0);
        g_bridge_w = g_bridge_h = 0;
        g_tracker_reset = true;
        Log("resize: %ux%u (bridge stopped, restarts at the next frame)", w, h);
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
HRESULT STDMETHODCALLTYPE CreateSamplerState_d(ID3D11Device* d, const D3D11_SAMPLER_DESC* desc, ID3D11SamplerState** out)
{
    if (!t_inside && desc && g_cfg.jitter && g_cfg.mip_bias != 0.f && desc->MaxLOD > 0.f) {
        const D3D11_FILTER f = desc->Filter;
        const bool mip_linear_or_aniso = (f & 0x1) || f == D3D11_FILTER_ANISOTROPIC || f == D3D11_FILTER_COMPARISON_ANISOTROPIC;
        const bool comparison = (f & 0x80) != 0;
        if (mip_linear_or_aniso && !comparison) {
            D3D11_SAMPLER_DESC b = *desc;
            b.MipLODBias = std::max(-16.f, std::min(15.99f, b.MipLODBias + g_cfg.mip_bias));
            {
                std::lock_guard<std::mutex> l(g_mu);
                ++g_stats.samplers_biased;
            }
            return g_create_sampler(d, &b, out);
        }
    }
    return g_create_sampler(d, desc, out);
}

// ---------------------------------------------------------------- install
LRESULT CALLBACK DummyProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

void* Slot(void* object, uint32_t index) { return (*reinterpret_cast<void***>(object))[index]; }

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

bool InstallLocked(const HostConfig& cfg, std::string* why)
{
    g_cfg = cfg;
    if (!na::hooking::Initialize()) { *why = "hooking core failed to initialise"; return false; }
    // A private device and swap chain: only to reach d3d11.dll's and dxgi.dll's functions.
    WNDCLASSW wc{};
    wc.lpfnWndProc = DummyProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"NeuralAliasDX11Probe";
    RegisterClassW(&wc);
    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    if (!wnd) { *why = "could not create the probe window"; return false; }
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
        *why = "could not create the probe DX11 device";
        return false;
    }
    ComPtr<ID3D11DeviceContext> def;
    dev->CreateDeferredContext(0, &def);
    ComPtr<IDXGISwapChain1> swap1;
    swap.As(&swap1);

    bool ok = na::hooking::BeginInstallWait("dx11-host");
    ok = ok && HookCtx(imm.Get(), 0, false);
    ok = ok && (!def || HookCtx(def.Get(), 1, true));
    ok = ok && na::hooking::PrepareVTableSlot(swap.Get(), kPresent, reinterpret_cast<void*>(&Present_d), reinterpret_cast<void**>(&g_present));
    ok = ok && na::hooking::PrepareVTableSlot(swap.Get(), kResizeBuffers, reinterpret_cast<void*>(&ResizeBuffers_d), reinterpret_cast<void**>(&g_resize));
    ok = ok && (!swap1 || na::hooking::PrepareVTableSlot(swap1.Get(), kPresent1, reinterpret_cast<void*>(&Present1_d), reinterpret_cast<void**>(&g_present1)));
    ok = ok && na::hooking::PrepareVTableSlot(dev.Get(), kCreateSamplerState, reinterpret_cast<void*>(&CreateSamplerState_d),
                                              reinterpret_cast<void**>(&g_create_sampler));
    if (ok) ok = na::hooking::CommitInstall();
    else na::hooking::AbandonInstall();
    if (!def) g_o[1] = g_o[0]; // no deferred class (should not happen on FL 11_0): never used
    // Where the deferred class shares a function with the immediate one, MinHook
    // returned the same trampoline into both sets: nothing to fix up.
    swap.Reset();
    def.Reset();
    imm.Reset();
    dev.Reset();
    DestroyWindow(wnd);
    if (!ok) {
        char err[160] = {};
        na::hooking::LastError(err, sizeof(err));
        *why = std::string("hook install failed: ") + err;
        return false;
    }
    return true;
}

std::wstring Dir(HMODULE m)
{
    wchar_t p[MAX_PATH]{};
    GetModuleFileNameW(m, p, MAX_PATH);
    std::wstring s(p);
    const auto slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : s.substr(0, slash);
}

std::wstring LocalAppData(const wchar_t* sub)
{
    wchar_t buf[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH)) return L"";
    return std::wstring(buf) + L"\\NeuralAlias\\" + sub;
}

HMODULE SelfModule()
{
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&SelfModule), &m);
    return m;
}

void OpenLog(const HostConfig& cfg)
{
    std::wstring path = cfg.log_path;
    if (path.empty()) {
        const std::wstring dir = LocalAppData(L"dx11");
        if (dir.empty()) return;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        path = dir + L"\\NeuralAliasDX11.log";
    }
    const auto dot = path.find_last_of(L'.');
    path.insert(dot == std::wstring::npos ? path.size() : dot, L"-" + std::to_wstring(GetCurrentProcessId()));
    // Shared for reading: the log can be read (or collected) while the game runs.
    g_log = _wfsopen(path.c_str(), L"w", _SH_DENYWR);
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, g_stats.log_path, MAX_PATH, nullptr, nullptr);
}

struct IniArgs {
    HMODULE self;
    std::wstring ini;
};

float IniFloat(const wchar_t* key, float def, const std::wstring& ini)
{
    wchar_t b[32]{};
    GetPrivateProfileStringW(L"dx11", key, L"", b, 32, ini.c_str());
    return b[0] ? static_cast<float>(_wtof(b)) : def;
}

DWORD WINAPI InitThread(LPVOID p)
{
    std::unique_ptr<IniArgs> a(static_cast<IniArgs*>(p));
    HMODULE pinned = nullptr; // hooks go in: never unload
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&InitThread), &pinned);
    HostConfig c;
    const wchar_t* ini = a->ini.c_str();
    c.jitter = GetPrivateProfileIntW(L"dx11", L"jitter", 0, ini) == 1;
    c.mip_bias = IniFloat(L"mip_bias", -1.f, a->ini);
    c.jitter_phases = GetPrivateProfileIntW(L"dx11", L"jitter_phases", 8, ini);
    c.preset = GetPrivateProfileIntW(L"dx11", L"preset", 12, ini);
    c.gpu_wait = GetPrivateProfileIntW(L"dx11", L"cpu_wait", 0, ini) == 0;
    c.copy_test = GetPrivateProfileIntW(L"dx11", L"copy_test", 0, ini) == 1;
    wchar_t b[MAX_PATH]{};
    GetPrivateProfileStringW(L"dx11", L"log", L"", b, MAX_PATH, ini);
    c.log_path = b;
    Install(c);
    return 0;
}

} // namespace

bool Install(const HostConfig& config)
{
    std::lock_guard<std::mutex> g(g_install_mu);
    if (g_installed.load()) return true;
    HostConfig c = config;
    if (c.helper_path.empty()) c.helper_path = Dir(SelfModule()) + L"\\na_bridge_helper64.exe";
    OpenLog(c);
    Log("NeuralAlias DX11 host starting: jitter=%d mip_bias=%.2f preset=%u wait=%s%s", c.jitter ? 1 : 0, c.mip_bias, c.preset,
        c.gpu_wait ? "gpu" : "cpu", c.copy_test ? " COPY TEST (no DLSS)" : "");
    std::string why;
    if (!InstallLocked(c, &why)) {
        Log("install failed: %s", why.c_str());
        strncpy_s(g_stats.reason, why.c_str(), _TRUNCATE);
        return false;
    }
    g_installed.store(true);
    g_stats.installed = true;
    Log("hooks installed (%u); waiting for the game's first Present", na::hooking::InstalledCount());
    return true;
}

bool StartFromIni(HMODULE self, const wchar_t* ini_name)
{
    std::wstring ini = Dir(self) + L"\\" + ini_name;
    if (GetPrivateProfileIntW(L"dx11", L"enabled", 0, ini.c_str()) != 1) return false;
    auto* a = new IniArgs{self, ini};
    HANDLE t = CreateThread(nullptr, 0, InitThread, a, 0, nullptr);
    if (!t) { delete a; return false; }
    CloseHandle(t);
    return true;
}

HostStats GetStats()
{
    std::lock_guard<std::mutex> l(g_mu);
    return g_stats;
}

} // namespace na::dx11
