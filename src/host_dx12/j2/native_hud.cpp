#include "host_dx12/j2/native_hud.hpp"
#include "host_dx12/j2/live_dlaa.hpp"

#include "host_dx12/observe/producers.hpp"
#include "host_dx12/observe/shader_operation.hpp"
#include "host_dx12/observe/texture_operation.hpp"
#include "host_dx12/observe/operation_snapshot.hpp"

#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace na::j2::native_hud {
namespace {
using Microsoft::WRL::ComPtr;

constexpr uint32_t kProxies = 8;     // learned proxies (a chain has 2-4; old ones age out)
constexpr uint32_t kViews = 4;       // render-target view formats per layer (typed, sRGB, ...)
constexpr uint64_t kGraveFrames = 8; // presents before a replaced layer is released

std::atomic<bool> g_installed{false}, g_on{true}, g_cover{false};

struct Proxy {
    na_object_id id = 0;
    uint32_t rw = 0, rh = 0, ow = 0, oh = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint64_t used = 0;           // last present (age-out order)
    // This frame (until TakeFrame):
    bool hud = false;            // the HUD has started: every later draw into the proxy is HUD
    bool cleared = false;        // the layers were cleared for this frame
    na_object_id mirrored = 0;   // the game depth target our native depths currently mirror
    na_object_id hud_ds = 0;     // the depth target the HUD used last frame (kept across frames)
    DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN;
    uint32_t scene = 0, redirected = 0, forwarded = 0;
    uint32_t depth_draws = 0, blended = 0, covered = 0, world = 0, masks = 0; // of `redirected`: with a depth target / with RT0 blending
    // H1b (COMMS b07e083 / Astra 16bbca8): an UPSTREAM target is one the game composes the whole
    // picture in (scene composite, then its HUD) before handing it to the back buffer in one
    // full-screen copy or draw. It is learned from structure and then handled exactly like the
    // back buffer: its HUD draws go to the native layers, so the copy carries the scene only.
    bool upstream = false;
    na_object_id source = 0;     // this frame: the single picture-sized input of the opaque full-screen hand-off into this proxy
    bool source_ambiguous = false;
    bool source_copy = false;    // that hand-off was a whole-resource Copy* (a transfer), not a draw (processing)
    // Upstream only: presents in a row whose hand-off did not come from it (current lineage,
    // Astra 20cddb8 H1b point 3), and paused = its HUD draws stay in it (the original full
    // picture is preserved) until the hand-off comes from it again.
    uint32_t lineage_miss = 0;
    bool paused = false;
};
struct Clear { D3D12_CLEAR_FLAGS flags = D3D12_CLEAR_FLAG_DEPTH; float depth = 1.f; uint8_t stencil = 0; };
struct Layers {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Resource> n[2];
    uint32_t w = 0, h = 0;
    DXGI_FORMAT typeless = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D12DescriptorHeap> rtv; // [2*i + layer]: view format i
    DXGI_FORMAT rtv_format[kViews] = {};
    uint32_t rtv_count = 0;
    UINT rtv_inc = 0;
    ComPtr<ID3D12Resource> d[2];
    uint32_t dw = 0, dh = 0;
    DXGI_FORMAT d_format = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D12DescriptorHeap> dsv; // [2*i + layer]
    DXGI_FORMAT dsv_format[kViews] = {};
    uint32_t dsv_count = 0;
    UINT dsv_inc = 0;
};

std::mutex g_m;
Proxy g_proxy[kProxies];
uint64_t g_frame = 0;
Layers g_l;
struct Grave { uint64_t frame; ComPtr<ID3D12Resource> r; };
std::vector<Grave> g_grave;
std::unordered_map<na_object_id, Clear> g_clears;            // the game's last depth clear per target
std::unordered_map<na_object_id, D3D12_RESOURCE_DESC> g_ds;  // depth targets' descriptions
Stats g_stats;
// H1b learning: the hand-off source seen on consecutive presented frames without any HUD of its own.
struct Candidate { na_object_id id = 0; uint32_t frames = 0; } g_cand;
constexpr uint32_t kPromoteFrames = 3, kDemoteFrames = 3;
bool g_layers_cleared = false;    // this frame's layers were cleared (by the back buffer or its upstream: once, never between them)

// The bind the recording thread made last (set by the raster listener, read by the draw hook).
struct Bind { ID3D12GraphicsCommandList* list = nullptr; uint64_t recording = 0; na_object_id proxy = 0, mask_ds = 0; };
constexpr uint32_t kEarlyMasks = 64; // depth-only draws mirrored per frame before the HUD's first draw
thread_local Bind t_bind;

// Frame-structure trace (2026-11-04): one whole frame every ~30 s (the first after 300 presents),
// at most kTraces per session: every draw outside the scene-depth passes (those are only counted
// per target), every whole-picture copy, in record order, with target size/format, depth use,
// blending, sampled textures and the camera binding. It answers where a game draws its HUD and
// how many hops (copy or processing draw, format changes) lie between that and the back buffer.
constexpr uint32_t kTraceLines = 1500, kTraces = 40;
std::atomic<bool> g_tracing{false};
uint64_t g_trace_next = 300;
uint32_t g_traces = 0, g_trace_lines = 0, g_trace_dispatches = 0, g_trace_dropped = 0;
std::unordered_map<na_object_id, uint32_t> g_trace_scene; // draws on the scene depth, per target
std::string g_trace_frame, g_trace_out;                 // the frame being traced / finished traces

Proxy* find_locked(na_object_id id)
{
    if (!id) return nullptr;
    for (auto& p : g_proxy)
        if (p.id == id) return &p;
    return nullptr;
}

bool is_srgb(DXGI_FORMAT f) { return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; }
DXGI_FORMAT clear_view_of(DXGI_FORMAT typeless)
{
    switch (typeless) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}
float stored_grey(DXGI_FORMAT typeless) // exactly representable: what the clear stores
{
    switch (typeless) {
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return 512.f / 1023.f;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return 1.f;
    default: return 128.f / 255.f;
    }
}

void bury_locked(ComPtr<ID3D12Resource>& r)
{
    if (r) g_grave.push_back({g_frame, r});
    r.Reset();
}

bool committed(ID3D12Device* dev, const D3D12_RESOURCE_DESC& d, D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& out)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    return SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&out)));
}

// The two native colour layers for this proxy's size and format (RENDER_TARGET state).
bool layers_locked(ID3D12Device* dev, const Proxy& p)
{
    const DXGI_FORMAT tl = TypelessOf(p.format);
    if (tl == DXGI_FORMAT_UNKNOWN) return false;
    if (g_l.device.Get() != dev) { // a new device: start over (old objects age out)
        bury_locked(g_l.n[0]); bury_locked(g_l.n[1]); bury_locked(g_l.d[0]); bury_locked(g_l.d[1]);
        g_l = Layers{};
        g_l.device = dev;
    }
    if (!g_l.rtv) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2 * (kViews + 1), D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        D3D12_DESCRIPTOR_HEAP_DESC dd{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2 * kViews, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_l.rtv))) || FAILED(dev->CreateDescriptorHeap(&dd, IID_PPV_ARGS(&g_l.dsv)))) {
            g_l.rtv.Reset(); g_l.dsv.Reset();
            return false;
        }
        g_l.rtv_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        g_l.dsv_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    }
    if (g_l.n[0] && g_l.w == p.ow && g_l.h == p.oh && g_l.typeless == tl) return true;
    bury_locked(g_l.n[0]); bury_locked(g_l.n[1]);
    g_l.rtv_count = 0;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = p.ow;
    d.Height = p.oh;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = tl;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    for (int i = 0; i < 2; ++i)
        if (!committed(dev, d, D3D12_RESOURCE_STATE_RENDER_TARGET, g_l.n[i])) { g_l.n[0].Reset(); g_l.n[1].Reset(); return false; }
    g_l.n[0]->SetName(L"NeuralAlias native HUD (over black)");
    g_l.n[1]->SetName(L"NeuralAlias native HUD (over grey)");
    g_l.w = p.ow; g_l.h = p.oh; g_l.typeless = tl;
    // Slot 0: the plain typed view used for our clears (stores the clear values exactly).
    D3D12_RENDER_TARGET_VIEW_DESC rv{};
    rv.Format = clear_view_of(tl);
    rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    for (int i = 0; i < 2; ++i) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = g_l.rtv->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(i) * g_l.rtv_inc;
        dev->CreateRenderTargetView(g_l.n[i].Get(), &rv, h);
    }
    return true;
}
// The two layers' views in the format the game's pipeline writes; false = no room.
bool rtv_locked(DXGI_FORMAT f, D3D12_CPU_DESCRIPTOR_HANDLE out[2])
{
    uint32_t i = 0;
    while (i < g_l.rtv_count && g_l.rtv_format[i] != f) ++i;
    if (i == g_l.rtv_count) {
        if (g_l.rtv_count == kViews) return false;
        D3D12_RENDER_TARGET_VIEW_DESC rv{};
        rv.Format = f;
        rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        for (int k = 0; k < 2; ++k) {
            D3D12_CPU_DESCRIPTOR_HANDLE h = g_l.rtv->GetCPUDescriptorHandleForHeapStart();
            h.ptr += static_cast<SIZE_T>(2 * (i + 1) + k) * g_l.rtv_inc;
            g_l.device->CreateRenderTargetView(g_l.n[k].Get(), &rv, h);
        }
        g_l.rtv_format[i] = f;
        ++g_l.rtv_count;
    }
    for (int k = 0; k < 2; ++k) {
        out[k] = g_l.rtv->GetCPUDescriptorHandleForHeapStart();
        out[k].ptr += static_cast<SIZE_T>(2 * (i + 1) + k) * g_l.rtv_inc;
    }
    return true;
}
D3D12_CPU_DESCRIPTOR_HANDLE clear_rtv_locked(int k)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_l.rtv->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(k) * g_l.rtv_inc;
    return h;
}
// Two native depth targets shaped like the game's HUD depth (DEPTH_WRITE state), with views in
// the pipeline's depth format.
bool depth_locked(const D3D12_RESOURCE_DESC& game, DXGI_FORMAT view, uint32_t w, uint32_t h, D3D12_CPU_DESCRIPTOR_HANDLE out[2])
{
    if (!g_l.d[0] || g_l.dw != w || g_l.dh != h || g_l.d_format != game.Format) {
        bury_locked(g_l.d[0]); bury_locked(g_l.d[1]);
        g_l.dsv_count = 0;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w;
        d.Height = h;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = game.Format;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        for (int i = 0; i < 2; ++i)
            if (!committed(g_l.device.Get(), d, D3D12_RESOURCE_STATE_DEPTH_WRITE, g_l.d[i])) { g_l.d[0].Reset(); g_l.d[1].Reset(); return false; }
        g_l.d[0]->SetName(L"NeuralAlias native HUD depth A");
        g_l.d[1]->SetName(L"NeuralAlias native HUD depth B");
        g_l.dw = w; g_l.dh = h; g_l.d_format = game.Format;
    }
    uint32_t i = 0;
    while (i < g_l.dsv_count && g_l.dsv_format[i] != view) ++i;
    if (i == g_l.dsv_count) {
        if (g_l.dsv_count == kViews) return false;
        D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = view;
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        for (int k = 0; k < 2; ++k) {
            D3D12_CPU_DESCRIPTOR_HANDLE hd = g_l.dsv->GetCPUDescriptorHandleForHeapStart();
            hd.ptr += static_cast<SIZE_T>(2 * i + k) * g_l.dsv_inc;
            g_l.device->CreateDepthStencilView(g_l.d[k].Get(), &dv, hd);
        }
        g_l.dsv_format[i] = view;
        ++g_l.dsv_count;
    }
    for (int k = 0; k < 2; ++k) {
        out[k] = g_l.dsv->GetCPUDescriptorHandleForHeapStart();
        out[k].ptr += static_cast<SIZE_T>(2 * i + k) * g_l.dsv_inc;
    }
    return true;
}
bool ds_desc_locked(na_object_id id, D3D12_RESOURCE_DESC* out)
{
    const auto it = g_ds.find(id);
    if (it != g_ds.end()) { *out = it->second; return true; }
    ID3D12Resource* r = na::observe::AcquireResource(id);
    if (!r) return false;
    *out = r->GetDesc();
    r->Release();
    if (g_ds.size() > 256) g_ds.clear();
    g_ds[id] = *out;
    return true;
}

// The resource's 2D size and format (0 = unknown).
bool dims(na_object_id id, uint32_t* w, uint32_t* h, DXGI_FORMAT* f)
{
    ComPtr<ID3D12Resource> r;
    r.Attach(na::observe::AcquireResource(id));
    if (!r) return false;
    const D3D12_RESOURCE_DESC d = r->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return false;
    *w = static_cast<uint32_t>(d.Width); *h = d.Height; *f = d.Format;
    return true;
}
// H1b: an opaque full-screen draw into a back buffer before its HUD: note the one picture-sized
// texture it reads (more than one = a composite of several inputs, not a hand-off: no source).
void note_source_locked(Proxy& p, ID3D12GraphicsCommandList* list)
{
    if (p.upstream) return;
    na::observe::NativeOperation op;
    uint64_t one = 0;
    if (!na::observe::ListNativeOperation(list, false, op) ||
        !PictureSource(op, p.id, p.rw, p.rh, one)) {
        p.source = 0; p.source_ambiguous = true; return;
    }
    if (one && !p.source_ambiguous)
        p.source = p.source && p.source != one ? (p.source_ambiguous = true, 0) : one;
}
void note_forward_locked(Proxy& p, const char* why)
{
    ++p.forwarded;
    ++g_stats.forwarded;
    std::snprintf(g_stats.last_forward, sizeof g_stats.last_forward, "%s", why);
}

void trace_line_locked(const char* text)
{
    if (g_trace_dispatches) {
        char d[64];
        std::snprintf(d, sizeof d, "hud_trace {\"t\":\"dispatch\",\"n\":%u}\r\n", g_trace_dispatches);
        g_trace_dispatches = 0;
        if (g_trace_lines < kTraceLines) { g_trace_frame += d; ++g_trace_lines; } else ++g_trace_dropped;
    }
    if (g_trace_lines >= kTraceLines) { ++g_trace_dropped; return; }
    g_trace_frame += text;
    ++g_trace_lines;
}
void trace_draw(ID3D12GraphicsCommandList* l, const na::observe::ShaderCall& call)
{
    if (call.kind == na::observe::ShaderKind::Dispatch) { std::lock_guard<std::mutex> lock(g_m); ++g_trace_dispatches; return; }
    na::observe::DrawInfo st;
    if (!na::observe::ListDrawState(l, st)) return;
    uint64_t scene_depth = 0, scene_color = 0;
    na::observe::GetLiveSelection(&scene_depth, &scene_color);
    if (st.ds && st.ds == scene_depth) { std::lock_guard<std::mutex> lock(g_m); ++g_trace_scene[st.rt]; return; }
    na_object_id srv[8];
    bool incomplete = false;
    const uint32_t n = na::observe::ListCurrentGraphicsSrvs(l, srv, 8, &incomplete);
    bool cam = false;
    {
        na::observe::LiveBinding cb[16];
        const uint32_t m = na::observe::ListCurrentGraphicsCbvs(l, cb, 16);
        for (uint32_t i = 0; i < m && !cam; ++i) cam = na::j2::CameraBinding(cb[i].resource, cb[i].offset, cb[i].param, cb[i].table);
    }
    uint32_t w = 0, h = 0; DXGI_FORMAT f{};
    if (st.rt) dims(st.rt, &w, &h, &f);
    char b[512];
    int k = std::snprintf(b, sizeof b,
        "hud_trace {\"t\":\"draw\",\"rt\":%llu,\"rts\":%u,\"w\":%u,\"h\":%u,\"fmt\":%u,\"view\":%u,\"ds\":%llu,\"depth_on\":%d,\"blend\":%d,\"n\":%llu,\"inst\":%llu,\"cam\":%d,\"srv_incomplete\":%d,\"srv\":[",
        (unsigned long long)st.rt, st.rt_count, w, h, unsigned(f), st.rtv_format, (unsigned long long)st.ds,
        st.depth_enable == NA_UNKNOWN_U32 ? -1 : int(st.depth_enable != 0), st.blend_mask == NA_UNKNOWN_U32 ? -1 : int(st.blend_mask & 1u),
        (unsigned long long)call.args[0], (unsigned long long)call.args[1], cam ? 1 : 0, incomplete ? 1 : 0);
    for (uint32_t i = 0; i < n && k > 0 && k < int(sizeof b) - 48; ++i) {
        uint32_t sw = 0, sh = 0; DXGI_FORMAT sf{};
        dims(srv[i], &sw, &sh, &sf);
        k += std::snprintf(b + k, sizeof b - k, "%s[%llu,%u,%u,%u]", i ? "," : "", (unsigned long long)srv[i], sw, sh, unsigned(sf));
    }
    if (k > 0 && k < int(sizeof b) - 6) std::snprintf(b + k, sizeof b - k, "]}\r\n");
    else return;
    std::lock_guard<std::mutex> lock(g_m);
    if (g_tracing.load(std::memory_order_relaxed)) trace_line_locked(b);
}

void draw(ID3D12GraphicsCommandList* l, const na::observe::ShaderCall& c)
{
    if (c.kind == na::observe::ShaderKind::Draw)
        l->DrawInstanced(UINT(c.args[0]), UINT(c.args[1]), UINT(c.args[2]), UINT(c.args[3]));
    else
        l->DrawIndexedInstanced(UINT(c.args[0]), UINT(c.args[1]), UINT(c.args[2]), INT(c.args[3]), UINT(c.args[4]));
}

// A depth/stencil-only draw (no colour target) on the HUD's depth target while the HUD is being
// drawn natively: UI clipping masks (stencil) and depth pre-passes of UI 3D models. Our native
// depth mirrors must get them too, or the masked text and the model fail their tests in the
// layers (Anno: menu text visible only in some places, the main menu's character head and the
// loading tips missing, Cort 2026-11-03). The game's own draw still runs.
void mirror_depth_only(ID3D12GraphicsCommandList* l, const na::observe::ShaderCall& call, const na::observe::DrawInfo& st)
{
    if (!st.ds || !st.pipeline_known || st.pso_rt_count != 0 || st.dsv_format == DXGI_FORMAT_UNKNOWN ||
        !st.restorable || st.samples != 1 || !st.viewport_count || !st.scissor_count)
        return;
    std::lock_guard<std::mutex> lock(g_m);
    // The HUD's depth target: the one mirrored now, or the one the HUD used last frame (a mask can
    // come before the frame's first HUD draw).
    Proxy* p = nullptr;
    for (auto& q : g_proxy)
        if (q.id && q.ow && (q.mirrored == st.ds || (!q.mirrored && q.hud_ds == st.ds))) { p = &q; break; }
    if (!p || !g_on.load()) return;
    // Before the HUD's first draw the target may still hold the picture's depth passes: only a few.
    if (!p->hud && p->masks >= kEarlyMasks) return;
    D3D12_RESOURCE_DESC dd{};
    D3D12_CPU_DESCRIPTOR_HANDLE dsv[2];
    ComPtr<ID3D12Device> dev;
    if (FAILED(l->GetDevice(IID_PPV_ARGS(&dev))) || !layers_locked(dev.Get(), *p)) return;
    if (!ds_desc_locked(st.ds, &dd) || dd.SampleDesc.Count != 1 ||
        !depth_locked(dd, static_cast<DXGI_FORMAT>(st.dsv_format), p->ow, p->oh, dsv))
        return;
    if (p->mirrored != st.ds) { // start the mirrors from the game's last clear of this target
        Clear c{};
        const auto it = g_clears.find(st.ds);
        if (it != g_clears.end()) c = it->second;
        else c.depth = (st.depth_func == D3D12_COMPARISON_FUNC_GREATER || st.depth_func == D3D12_COMPARISON_FUNC_GREATER_EQUAL) ? 0.f : 1.f;
        const D3D12_CLEAR_FLAGS f = D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL;
        l->ClearDepthStencilView(dsv[0], f, c.depth, c.stencil, 0, nullptr);
        l->ClearDepthStencilView(dsv[1], f, c.depth, c.stencil, 0, nullptr);
        p->mirrored = st.ds;
    }
    const float sx = float(p->ow) / float(p->rw), sy = float(p->oh) / float(p->rh);
    D3D12_VIEWPORT vp[16];
    D3D12_RECT sc[16];
    for (uint32_t i = 0; i < st.viewport_count; ++i) {
        float o[6];
        ScaleViewport(st.viewports[i], sx, sy, o);
        vp[i] = {o[0], o[1], o[2], o[3], o[4], o[5]};
    }
    for (uint32_t i = 0; i < st.scissor_count; ++i) {
        int32_t o[4];
        ScaleScissor(st.scissors[i], sx, sy, p->ow, p->oh, o);
        sc[i] = {o[0], o[1], o[2], o[3]};
    }
    l->RSSetViewports(st.viewport_count, vp);
    l->RSSetScissorRects(st.scissor_count, sc);
    for (int k = 0; k < 2; ++k) {
        l->OMSetRenderTargets(0, nullptr, FALSE, &dsv[k]);
        draw(l, call);
    }
    if (!na::observe::RestoreBindingState(l, false, false, 0, true, true, true)) ++g_stats.restore_failed;
    ++p->masks;
    ++g_stats.masks;
}

na::observe::ShaderScope redirect(ID3D12GraphicsCommandList* l, na::observe::ShaderCall& call)
{
    na::observe::DrawInfo st;
    if (!na::observe::ListDrawState(l, st) || st.recording != t_bind.recording) return {};
    // Depth/stencil only (the game's draw still runs): no colour target bound, or a pipeline
    // without one while the proxy is bound.
    if (st.ds && (st.rt_count == 0 ? st.ds == t_bind.mask_ds
                                   : t_bind.proxy && st.rt == t_bind.proxy && st.pipeline_known && st.pso_rt_count == 0)) {
        mirror_depth_only(l, call, st);
        return {};
    }
    if (!t_bind.proxy || st.rt != t_bind.proxy) return {};
    std::lock_guard<std::mutex> lock(g_m);
    Proxy* p = find_locked(st.rt);
    if (!p || !p->ow) return {};
    if (p->upstream && p->paused) return {}; // lineage not current: the original picture keeps its HUD
    const bool shaped = SceneShaped(st.pipeline_known && st.blend_mask != NA_UNKNOWN_U32, st.blend_mask, uint64_t(call.args[0]), uint64_t(call.args[1]));
    // A draw bound to the camera block J2 jitters is drawn with the (jittered) camera: part of the
    // picture, whatever its order. In the native layer it would keep its jitter and shimmer
    // (Anno's foliage flicker, 2026-11-03: Cort saw it stop when those draws left the layer).
    {
        na::observe::LiveBinding cb[16];
        const uint32_t n = na::observe::ListCurrentGraphicsCbvs(l, cb, 16);
        for (uint32_t i = 0; i < n; ++i)
            if (na::j2::CameraBinding(cb[i].resource, cb[i].offset, cb[i].param, cb[i].table)) {
                ++p->world;
                ++g_stats.world;
                return {};
            }
        // Depth-tested against the published scene depth: drawn into the 3D picture (a world
        // marker, a late effect), whatever its order. Combined evidence, never a single cue.
        uint64_t scene_depth = 0, scene_color = 0;
        na::observe::GetLiveSelection(&scene_depth, &scene_color);
        if (scene_depth && st.ds == scene_depth && st.pipeline_known && st.depth_enable != NA_UNKNOWN_U32 && st.depth_enable != 0u &&
            st.dsv_format != DXGI_FORMAT_UNKNOWN) {
            ++p->world;
            ++g_stats.world_depth;
            return {};
        }
    }
    if (!p->hud) {
        if (shaped) {
            ++p->scene;
            ++g_stats.scene_draws;
            if (!p->upstream) {
                note_source_locked(*p, l);
                p->source_copy = false;
            }
            return {};
        }
        p->hud = true;
    } else if (g_cover.load(std::memory_order_relaxed) && call.args[0] <= 6 && call.args[1] == 1 &&
               CoversTarget(st.viewport_count ? st.viewports[0] : nullptr, st.scissor_count ? st.scissors[0] : nullptr, p->rw, p->rh)) {
        // A full-screen draw after the HUD started (Anno's pause menu: a blurred copy of the scene,
        // read by pixel position, so at native size it showed the render-size picture in the top-left
        // corner and edge streaks elsewhere, Cort 2026-11-03). Full-screen passes belong with the
        // picture: it goes to the proxy, where DLSS takes it. An opaque one hides everything drawn
        // before it, so the layers start again; a blended one leaves the earlier HUD on top.
        if (shaped && p->cleared) {
            const float grey = stored_grey(g_l.typeless);
            const float black[4] = {0.f, 0.f, 0.f, 1.f}, back[4] = {grey, grey, grey, 1.f};
            l->ClearRenderTargetView(clear_rtv_locked(0), black, 0, nullptr);
            l->ClearRenderTargetView(clear_rtv_locked(1), back, 0, nullptr);
        }
        ++p->covered;
        ++g_stats.covered;
        return {};
    }
    const char* why = nullptr;
    const DXGI_FORMAT view = static_cast<DXGI_FORMAT>(st.rtv_format);
    if (!st.restorable) why = "the list's targets cannot be handed back";
    else if (!st.pipeline_known) why = "unknown pipeline";
    else if (st.rt_count != 1 || st.pso_rt_count != 1) why = "draws to more than one target";
    else if (st.samples != 1) why = "multisampled";
    else if (!st.viewport_count || !st.scissor_count) why = "no viewport or scissor";
    else if (TypelessOf(view) != TypelessOf(p->format)) why = "pipeline format differs from the back buffer";
    if (why) { note_forward_locked(*p, why); return {}; }
    ComPtr<ID3D12Device> dev;
    if (FAILED(l->GetDevice(IID_PPV_ARGS(&dev))) || !layers_locked(dev.Get(), *p)) { note_forward_locked(*p, "could not make the native layers"); return {}; }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[2], dsv[2];
    if (!rtv_locked(view, rtv)) { note_forward_locked(*p, "too many view formats"); return {}; }
    // Depth: the game's HUD depth is mirrored at native size by two targets (one per layer).
    bool depth = false;
    na_object_id ds = st.ds;
    if (ds && st.dsv_format == DXGI_FORMAT_UNKNOWN) ds = 0; // the pipeline does not use a depth target
    if (ds) {
        D3D12_RESOURCE_DESC dd{};
        if (!ds_desc_locked(ds, &dd)) { note_forward_locked(*p, "depth target unknown"); return {}; }
        if (dd.SampleDesc.Count != 1) { note_forward_locked(*p, "multisampled depth"); return {}; }
        if (!depth_locked(dd, static_cast<DXGI_FORMAT>(st.dsv_format), p->ow, p->oh, dsv)) { note_forward_locked(*p, "could not make the native depth"); return {}; }
        depth = true;
    }
    const float sx = float(p->ow) / float(p->rw), sy = float(p->oh) / float(p->rh);
    // Everything below is recorded into the game's list (host work, not observed).
    if (!p->cleared) {
        if (!g_layers_cleared) {
            const float grey = stored_grey(g_l.typeless);
            const float black[4] = {0.f, 0.f, 0.f, 1.f}, back[4] = {grey, grey, grey, 1.f};
            l->ClearRenderTargetView(clear_rtv_locked(0), black, 0, nullptr);
            l->ClearRenderTargetView(clear_rtv_locked(1), back, 0, nullptr);
            g_layers_cleared = true;
        }
        p->cleared = true;
        p->view = view;
    }
    if (depth && p->mirrored != ds) {
        Clear c{};
        const auto it = g_clears.find(ds);
        if (it != g_clears.end()) c = it->second;
        else c.depth = (st.depth_func == D3D12_COMPARISON_FUNC_GREATER || st.depth_func == D3D12_COMPARISON_FUNC_GREATER_EQUAL) ? 0.f : 1.f;
        const D3D12_CLEAR_FLAGS f = D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL;
        l->ClearDepthStencilView(dsv[0], f, c.depth, c.stencil, 0, nullptr);
        l->ClearDepthStencilView(dsv[1], f, c.depth, c.stencil, 0, nullptr);
        p->mirrored = ds;
    }
    D3D12_VIEWPORT vp[16];
    D3D12_RECT sc[16];
    for (uint32_t i = 0; i < st.viewport_count; ++i) {
        float o[6];
        ScaleViewport(st.viewports[i], sx, sy, o);
        vp[i] = {o[0], o[1], o[2], o[3], o[4], o[5]};
    }
    for (uint32_t i = 0; i < st.scissor_count; ++i) {
        int32_t o[4];
        ScaleScissor(st.scissors[i], sx, sy, p->ow, p->oh, o);
        sc[i] = {o[0], o[1], o[2], o[3]};
    }
    for (int k = 0; k < 2; ++k) {
        l->OMSetRenderTargets(1, &rtv[k], FALSE, depth ? &dsv[k] : nullptr);
        if (k == 0) {
            l->RSSetViewports(st.viewport_count, vp);
            l->RSSetScissorRects(st.scissor_count, sc);
        }
        draw(l, call);
    }
    if (!na::observe::RestoreBindingState(l, false, false, 0, true, true, true)) ++g_stats.restore_failed;
    ++p->redirected;
    ++g_stats.redirected;
    if (depth) ++p->depth_draws;
    if (st.pipeline_known && st.blend_mask != NA_UNKNOWN_U32 && (st.blend_mask & 1u)) ++p->blended;
    return {na::observe::ShaderAction::Replaced, nullptr};
}

class DrawHook final : public na::observe::ShaderOperationHook {
public:
    na::observe::ShaderScope Before(ID3D12GraphicsCommandList* l, na::observe::ShaderCall& call) noexcept override
    {
        if (g_tracing.load(std::memory_order_relaxed)) trace_draw(l, call);
        if (call.kind == na::observe::ShaderKind::Dispatch || (!t_bind.proxy && !t_bind.mask_ds) || t_bind.list != l || !g_on.load(std::memory_order_relaxed)) return {};
        return redirect(l, call);
    }
    void After(ID3D12GraphicsCommandList*, const na::observe::ShaderCall&, na::observe::ShaderScope&) noexcept override {}
};

// The game's clears: depth clear values are remembered (our mirrors start from them), and a
// clear of a mirrored depth or of a proxy whose HUD has started is applied to ours too.
class ClearHook final : public na::observe::TextureOperationHook {
public:
    na::observe::ShaderScope Before(ID3D12GraphicsCommandList* l, const na::observe::TextureCall& c) noexcept override
    {
        if ((c.kind == na::observe::TextureKind::Copy || c.kind == na::observe::TextureKind::CopyRegion) && c.destination && c.source) {
            // H1b: a whole-picture copy into a back buffer is a hand-off from its source.
            const na_object_id dst = na::observe::IdOf(c.destination), src = na::observe::IdOf(c.source);
            std::lock_guard<std::mutex> lock(g_m);
            if (g_tracing.load(std::memory_order_relaxed)) {
                uint32_t sw = 0, sh = 0, dw = 0, dh = 0; DXGI_FORMAT sf{}, df{};
                dims(src, &sw, &sh, &sf);
                dims(dst, &dw, &dh, &df);
                char t[256];
                std::snprintf(t, sizeof t, "hud_trace {\"t\":\"%s\",\"src\":[%llu,%u,%u,%u],\"dst\":[%llu,%u,%u,%u],\"whole\":%d}\r\n",
                              c.kind == na::observe::TextureKind::Copy ? "copy" : "copy_region", (unsigned long long)src, sw, sh, unsigned(sf),
                              (unsigned long long)dst, dw, dh, unsigned(df), (c.kind == na::observe::TextureKind::Copy || (!c.box && !c.x && !c.y)) ? 1 : 0);
                trace_line_locked(t);
            }
            Proxy* p = find_locked(dst);
            if (p && !p->upstream && src && (c.kind == na::observe::TextureKind::Copy || (!c.box && !c.x && !c.y))) {
                uint32_t w = 0, h = 0; DXGI_FORMAT f{};
                if (dims(src, &w, &h, &f) && w == p->rw && h == p->rh) {
                    if (p->source && p->source != src) { p->source = 0; p->source_ambiguous = true; }
                    else if (!p->source_ambiguous) { p->source = src; p->source_copy = true; }
                }
            }
            return {};
        }
        if (!c.target_id || (c.kind != na::observe::TextureKind::ClearDepth && c.kind != na::observe::TextureKind::ClearColor)) return {};
        std::lock_guard<std::mutex> lock(g_m);
        if (c.kind == na::observe::TextureKind::ClearDepth) {
            Clear& k = g_clears[c.target_id];
            if (c.flags & D3D12_CLEAR_FLAG_DEPTH) k.depth = c.depth;
            if (c.flags & D3D12_CLEAR_FLAG_STENCIL) k.stencil = c.stencil;
            if (g_clears.size() > 512) g_clears.clear();
            if (c.rect_count || !g_l.d[0] || !g_l.dsv_count) return {};
            for (const auto& p : g_proxy)
                if (p.id && p.mirrored == c.target_id) {
                    for (int i = 0; i < 2; ++i) {
                        D3D12_CPU_DESCRIPTOR_HANDLE h = g_l.dsv->GetCPUDescriptorHandleForHeapStart();
                        h.ptr += static_cast<SIZE_T>(i) * g_l.dsv_inc;
                        l->ClearDepthStencilView(h, c.flags, c.depth, c.stencil, 0, nullptr);
                    }
                    break;
                }
            return {};
        }
        Proxy* p = find_locked(c.target_id);
        if (!p || !p->hud || !p->cleared || !g_l.n[0] || c.rect_count) return {};
        // A clear after the HUD started replaces everything under it: both layers take the
        // colour, so the composite shows exactly that colour there.
        for (int i = 0; i < 2; ++i) l->ClearRenderTargetView(clear_rtv_locked(i), c.color, 0, nullptr);
        return {};
    }
    void After(ID3D12GraphicsCommandList*, const na::observe::TextureCall&, na::observe::ShaderScope&) noexcept override {}
};

void on_bind(ID3D12GraphicsCommandList* l, const na::observe::RasterScissors& s)
{
    na_object_id rt = 0, ds = 0;
    Bind b{l, s.recording, 0};
    if (g_on.load(std::memory_order_relaxed) && na::observe::ListCurrentTargets(l, &rt, &ds) && (rt || ds)) {
        std::lock_guard<std::mutex> lock(g_m);
        if (rt) {
            if (find_locked(rt)) b.proxy = rt;
        } else {
            for (const auto& q : g_proxy) // depth only: the HUD's depth target (a UI mask)?
                if (q.id && (q.mirrored == ds || (!q.mirrored && q.hud_ds == ds))) { b.mask_ds = ds; break; }
        }
    }
    t_bind = b;
}

} // namespace

DXGI_FORMAT TypelessOf(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM:
        return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}
float GreyFor(DXGI_FORMAT view)
{
    const float g = stored_grey(TypelessOf(view));
    if (!is_srgb(view)) return g;
    return g <= 0.04045f ? g / 12.92f : std::pow((g + 0.055f) / 1.055f, 2.4f);
}
bool SceneShaped(bool blend_known, uint32_t blend_mask, uint64_t count, uint64_t instances)
{
    return blend_known && (blend_mask & 1u) == 0 && count >= 3 && count <= 6 && instances == 1;
}
bool CoversTarget(const float* vp, const int32_t* sc, uint32_t w, uint32_t h)
{
    if (!vp || !sc || !w || !h) return false;
    const bool v = vp[0] <= 0.5f && vp[1] <= 0.5f && vp[0] + vp[2] >= float(w) - 0.5f && vp[1] + vp[3] >= float(h) - 0.5f;
    const bool s = sc[0] <= 0 && sc[1] <= 0 && sc[2] >= int32_t(w) && sc[3] >= int32_t(h);
    return v && s;
}
void ScaleViewport(const float in[6], float sx, float sy, float out[6])
{
    out[0] = in[0] * sx; out[1] = in[1] * sy; out[2] = in[2] * sx; out[3] = in[3] * sy; out[4] = in[4]; out[5] = in[5];
}
void ScaleScissor(const int32_t in[4], float sx, float sy, uint32_t ow, uint32_t oh, int32_t out[4])
{
    const auto clampx = [&](double v) { return int32_t((std::max)(0.0, (std::min)(double(ow), v))); };
    const auto clampy = [&](double v) { return int32_t((std::max)(0.0, (std::min)(double(oh), v))); };
    out[0] = clampx(std::floor(in[0] * double(sx) + 1e-4));
    out[1] = clampy(std::floor(in[1] * double(sy) + 1e-4));
    out[2] = clampx(std::ceil(in[2] * double(sx) - 1e-4));
    out[3] = clampy(std::ceil(in[3] * double(sy) - 1e-4));
}

void Install()
{
    if (g_installed.exchange(true)) return;
    na::observe::SetRasterStateTracking(true); // the game's targets/viewports/scissors can be handed back
    na::observe::SetOperationSnapshots(true); // exact used SRVs at candidate handoffs, not a table prefix
    na::observe::SetRasterStateListener(&on_bind);
    na::observe::SetShaderOperationHook(na::observe::ShaderLane::Reconstruction, std::make_shared<DrawHook>());
    na::observe::SetTextureOperationHook(std::make_shared<ClearHook>());
}
bool Installed() { return g_installed.load(); }
void SetEnabled(bool on) { g_on.store(on); }
void SetCoverRule(bool on) { g_cover.store(on); }
bool Enabled() { return g_installed.load() && g_on.load(); }

bool PictureSource(const na::observe::NativeOperation& op, uint64_t destination,
                   uint32_t width, uint32_t height, uint64_t& source)
{
    source = 0;
    if (op.compute || !op.reflection_complete || !width || !height) return false;
    for (const auto& d : op.descriptors) {
        if (!d.used || d.view.kind != NA_DESC_SRV || !d.view.resource || d.view.resource == destination) continue;
        if (!d.resource) return false;
        const auto desc = d.resource->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 || desc.DepthOrArraySize != 1) continue;
        // Only a whole base-level 2D image gives this initial lineage edge.
        // Array/mip/subrect mappings need their actual view geometry, not a
        // resource-size guess. Leave the game's complete picture untouched.
        if (d.view.explicit_view && (d.view.srv.ViewDimension != D3D12_SRV_DIMENSION_TEXTURE2D ||
            d.view.srv.Texture2D.MostDetailedMip != 0 || d.view.srv.Texture2D.PlaneSlice != 0 ||
            d.view.srv.Texture2D.ResourceMinLODClamp != 0.f ||
            d.view.srv.Shader4ComponentMapping != D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING)) continue;
        if (desc.Width != width || desc.Height != height) continue;
        if (source && source != d.view.resource) { source = 0; return false; }
        source = d.view.resource;
    }
    return true;
}

bool TakeFrame(ID3D12Resource* frame, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, Layer* out)
{
    if (!g_installed.load() || !frame) return false;
    const na_object_id id = na::observe::IdOf(frame);
    std::lock_guard<std::mutex> lock(g_m);
    ++g_frame;
    g_layers_cleared = false; // the frame that presents now is complete
    if (g_tracing.load(std::memory_order_relaxed)) { // the traced frame is complete
        g_tracing.store(false);
        char t[256];
        int k = std::snprintf(t, sizeof t, "hud_trace {\"t\":\"end\",\"present\":%llu,\"lines\":%u,\"dropped\":%u,\"back_buffer\":%llu,\"scene_depth_draws\":{",
                              (unsigned long long)g_frame, g_trace_lines, g_trace_dropped, (unsigned long long)id);
        bool first = true;
        for (const auto& e : g_trace_scene) {
            if (k <= 0 || k >= int(sizeof t) - 40) break;
            k += std::snprintf(t + k, sizeof t - k, "%s\"%llu\":%u", first ? "" : ",", (unsigned long long)e.first, e.second);
            first = false;
        }
        if (k > 0 && k < int(sizeof t) - 6) std::snprintf(t + k, sizeof t - k, "}}\r\n");
        trace_line_locked(t);
        if (g_trace_out.size() < (1u << 20)) g_trace_out += g_trace_frame;
        g_trace_frame.clear();
        g_trace_next = g_frame + 1800;
    } else if (g_traces < kTraces && g_frame >= g_trace_next) { // trace the next frame
        ++g_traces;
        g_trace_lines = g_trace_dispatches = g_trace_dropped = 0;
        g_trace_scene.clear();
        g_trace_frame = "hud_trace {\"t\":\"begin\",\"present\":" + std::to_string(g_frame) + "}\r\n";
        g_tracing.store(true);
        g_stats.traces = g_traces;
    }
    g_grave.erase(std::remove_if(g_grave.begin(), g_grave.end(), [](const Grave& g) { return g.frame + kGraveFrames < g_frame; }), g_grave.end());
    if (!id) return false;
    Proxy* p = find_locked(id);
    if (!p || p->rw != rw || p->rh != rh || p->ow != ow || p->oh != oh) {
        if (!p) { // learn it, replacing the proxy presented longest ago (never the upstream)
            for (auto& q : g_proxy) if (!q.upstream && (!p || q.used < p->used)) p = &q;
            if (!p) return false; // every slot holds an upstream: nothing to replace
        }
        *p = Proxy{};
        p->id = id; p->rw = rw; p->rh = rh; p->ow = ow; p->oh = oh;
        p->format = frame->GetDesc().Format;
        p->used = g_frame;
        return false;
    }
    p->used = g_frame;
    // H1b: the upstream picture for this output (handled like the back buffer), and learning.
    Proxy* u = nullptr;
    for (auto& q : g_proxy) if (q.id && q.upstream && q.ow == ow && q.oh == oh && q.rw == rw && q.rh == rh) { u = &q; break; }
    // Current lineage: this present's hand-off came from the upstream (actual output dependency).
    const bool lineage = u && p->source == u->id && !p->source_ambiguous;
    bool recovered = false;
    if (u) {
        u->used = g_frame;
        if (lineage) { u->lineage_miss = 0; u->paused = false; }
        else {
            // Its HUD draws of this frame (if any) are already in the layers: lay them on once so
            // nothing is lost, and keep the next frames' HUD in the picture until lineage returns.
            recovered = u->redirected > 0;
            if (recovered) ++g_stats.lineage_recovered;
            ++g_stats.lineage_miss;
            u->paused = true;
            if (++u->lineage_miss >= kDemoteFrames) { // the structure changed: learn again
                ++g_stats.upstream_dropped;
                *u = Proxy{};
                u = nullptr;
            }
        }
    } else if (g_on.load() && p->redirected == 0 && p->source && !p->source_ambiguous && !p->source_copy) {
        // A full-screen draw reading one picture-sized texture is processing (tonemap, blur,
        // sharpen) until proven a transfer (Astra 20cddb8 H1b point 1): not promoted.
        g_cand = {};
        std::snprintf(g_stats.upstream_why, sizeof g_stats.upstream_why, "hand-off is a draw (processing), not a copy");
    } else if (g_on.load() && p->redirected == 0 && p->source && !p->source_ambiguous) {
        if (g_cand.id == p->source) ++g_cand.frames; else g_cand = {p->source, 1};
        if (g_cand.frames >= kPromoteFrames) {
            uint32_t w = 0, h = 0; DXGI_FORMAT f{};
            if (!dims(g_cand.id, &w, &h, &f)) std::snprintf(g_stats.upstream_why, sizeof g_stats.upstream_why, "candidate gone");
            else if (TypelessOf(f) == DXGI_FORMAT_UNKNOWN || TypelessOf(f) != TypelessOf(p->format))
                // A format change in the hand-off is a colour transformation, not a copy: laying the
                // upstream HUD over the final picture would be wrong (Astra 16bbca8 point 4).
                std::snprintf(g_stats.upstream_why, sizeof g_stats.upstream_why, "hand-off changes format %u->%u", unsigned(f), unsigned(p->format));
            else {
                Proxy* slot = nullptr;
                for (auto& q : g_proxy) if (q.id != p->id && !q.upstream && (!slot || q.used < slot->used)) slot = &q;
                if (!slot) std::snprintf(g_stats.upstream_why, sizeof g_stats.upstream_why, "no free slot");
                else {
                    *slot = Proxy{};
                    slot->id = g_cand.id; slot->upstream = true;
                    slot->rw = rw; slot->rh = rh; slot->ow = ow; slot->oh = oh; slot->format = f; slot->used = g_frame;
                    ++g_stats.upstream_learned;
                    g_stats.upstream_why[0] = 0;
                }
            }
            g_cand = {};
        }
    } else g_cand = {}; // consecutive observations only: any frame without a clean source restarts the count
    g_stats.upstream_now = u ? 1u : 0u;
    g_stats.last_source = p->source ? 1u : p->source_ambiguous ? 2u : 0u;
    p->source = 0; p->source_ambiguous = false; p->source_copy = false;
    bool hud = g_on.load() && p->cleared && p->redirected > 0 && g_l.n[0] && g_l.w == ow && g_l.h == oh;
    const bool up_hud = !hud && u && (lineage || recovered) && g_on.load() && u->cleared && u->redirected > 0 && g_l.n[0] && g_l.w == ow && g_l.h == oh;
    if (up_hud) {
        hud = true;
        ++g_stats.upstream_frames;
        p->view = u->view;
        p->redirected = u->redirected; p->forwarded += u->forwarded; p->world += u->world; p->masks += u->masks;
        p->depth_draws = u->depth_draws; p->blended = u->blended;
    }
    if (u) { // the upstream's frame ends with the back buffer's
        u->hud = u->cleared = false;
        if (u->mirrored) u->hud_ds = u->mirrored;
        u->mirrored = 0;
        if (!up_hud) u->view = DXGI_FORMAT_UNKNOWN;
        u->scene = u->redirected = u->forwarded = 0;
        u->depth_draws = u->blended = u->covered = u->world = u->masks = 0;
    }
    if (hud && out) {
        out->n0 = g_l.n[0].Get();
        out->n1 = g_l.n[1].Get();
        out->view = p->view;
        out->grey = GreyFor(p->view);
        out->srgb = is_srgb(p->view);
    }
    ++g_stats.frames;
    if (hud) ++g_stats.hud_frames;
    g_stats.last_scene = p->scene; g_stats.last_redirected = p->redirected; g_stats.last_forwarded = p->forwarded;
    g_stats.last_depth = p->depth_draws; g_stats.last_blended = p->blended; g_stats.last_covered = p->covered; g_stats.last_world = p->world; g_stats.last_masks = p->masks;
    g_stats.proxy_format = uint32_t(p->format); g_stats.view_format = uint32_t(p->view);
    g_stats.grey = p->view != DXGI_FORMAT_UNKNOWN ? GreyFor(p->view) : 0.f;
    p->hud = p->cleared = false;
    if (p->mirrored) p->hud_ds = p->mirrored;
    p->mirrored = 0;
    p->view = DXGI_FORMAT_UNKNOWN;
    p->scene = p->redirected = p->forwarded = 0;
    p->depth_draws = p->blended = p->covered = p->world = p->masks = 0;
    return hud;
}

std::string TakeTrace()
{
    std::lock_guard<std::mutex> lock(g_m);
    std::string out;
    out.swap(g_trace_out);
    return out;
}
Stats GetStats()
{
    std::lock_guard<std::mutex> lock(g_m);
    return g_stats;
}
std::string StatusLine()
{
    const Stats s = GetStats();
    char b[1024];
    std::snprintf(b, sizeof b,
                  "native_hud on=%u frames=%llu hud_frames=%llu scene_draws=%llu redirected=%llu forwarded=%llu restore_failed=%llu "
                  "covered=%llu world=%llu masks=%llu last_frame=scene:%u,hud:%u,forwarded:%u,depth:%u,blended:%u,covered:%u,world:%u,masks:%u proxy_fmt=%u view_fmt=%u grey=%.4f last_forward=\"%s\" "
                  "upstream{now=%u learned=%llu dropped=%llu hud_frames=%llu last_source=%u why=\"%s\" lineage_miss=%llu recovered=%llu} world_depth=%llu traces=%u",
                  Enabled() ? 1u : 0u, (unsigned long long)s.frames, (unsigned long long)s.hud_frames, (unsigned long long)s.scene_draws,
                  (unsigned long long)s.redirected, (unsigned long long)s.forwarded, (unsigned long long)s.restore_failed, (unsigned long long)s.covered, (unsigned long long)s.world, (unsigned long long)s.masks,
                  s.last_scene, s.last_redirected, s.last_forwarded, s.last_depth, s.last_blended, s.last_covered, s.last_world, s.last_masks, s.proxy_format, s.view_format,
                  double(s.grey), s.last_forward, s.upstream_now, (unsigned long long)s.upstream_learned, (unsigned long long)s.upstream_dropped,
                  (unsigned long long)s.upstream_frames, s.last_source, s.upstream_why, (unsigned long long)s.lineage_miss,
                  (unsigned long long)s.lineage_recovered, (unsigned long long)s.world_depth, s.traces);
    return b;
}

} // namespace na::j2::native_hud
