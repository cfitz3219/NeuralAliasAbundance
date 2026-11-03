#include "host_dx12/j2/native_hud.hpp"

#include "host_dx12/observe/producers.hpp"
#include "host_dx12/observe/shader_operation.hpp"
#include "host_dx12/observe/texture_operation.hpp"

#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace na::j2::native_hud {
namespace {
using Microsoft::WRL::ComPtr;

constexpr uint32_t kProxies = 8;     // learned proxies (a chain has 2-4; old ones age out)
constexpr uint32_t kViews = 4;       // render-target view formats per layer (typed, sRGB, ...)
constexpr uint64_t kGraveFrames = 8; // presents before a replaced layer is released

std::atomic<bool> g_installed{false}, g_on{true};

struct Proxy {
    na_object_id id = 0;
    uint32_t rw = 0, rh = 0, ow = 0, oh = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint64_t used = 0;           // last present (age-out order)
    // This frame (until TakeFrame):
    bool hud = false;            // the HUD has started: every later draw into the proxy is HUD
    bool cleared = false;        // the layers were cleared for this frame
    na_object_id mirrored = 0;   // the game depth target our native depths currently mirror
    DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN;
    uint32_t scene = 0, redirected = 0, forwarded = 0;
    uint32_t depth_draws = 0, blended = 0, covered = 0; // of `redirected`: with a depth target / with RT0 blending
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

// The bind the recording thread made last (set by the raster listener, read by the draw hook).
struct Bind { ID3D12GraphicsCommandList* list = nullptr; uint64_t recording = 0; na_object_id proxy = 0; };
thread_local Bind t_bind;

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

void note_forward_locked(Proxy& p, const char* why)
{
    ++p.forwarded;
    ++g_stats.forwarded;
    std::snprintf(g_stats.last_forward, sizeof g_stats.last_forward, "%s", why);
}

void draw(ID3D12GraphicsCommandList* l, const na::observe::ShaderCall& c)
{
    if (c.kind == na::observe::ShaderKind::Draw)
        l->DrawInstanced(UINT(c.args[0]), UINT(c.args[1]), UINT(c.args[2]), UINT(c.args[3]));
    else
        l->DrawIndexedInstanced(UINT(c.args[0]), UINT(c.args[1]), UINT(c.args[2]), INT(c.args[3]), UINT(c.args[4]));
}

na::observe::ShaderScope redirect(ID3D12GraphicsCommandList* l, na::observe::ShaderCall& call)
{
    na::observe::DrawInfo st;
    if (!na::observe::ListDrawState(l, st) || st.recording != t_bind.recording || st.rt != t_bind.proxy) return {};
    std::lock_guard<std::mutex> lock(g_m);
    Proxy* p = find_locked(st.rt);
    if (!p || !p->ow) return {};
    const bool shaped = SceneShaped(st.pipeline_known && st.blend_mask != NA_UNKNOWN_U32, st.blend_mask, uint64_t(call.args[0]), uint64_t(call.args[1]));
    if (!p->hud) {
        if (shaped) {
            ++p->scene;
            ++g_stats.scene_draws;
            return {};
        }
        p->hud = true;
    } else if (call.args[0] <= 6 && call.args[1] == 1 &&
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
        const float grey = stored_grey(g_l.typeless);
        const float black[4] = {0.f, 0.f, 0.f, 1.f}, back[4] = {grey, grey, grey, 1.f};
        l->ClearRenderTargetView(clear_rtv_locked(0), black, 0, nullptr);
        l->ClearRenderTargetView(clear_rtv_locked(1), back, 0, nullptr);
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
        if (call.kind == na::observe::ShaderKind::Dispatch || !t_bind.proxy || t_bind.list != l || !g_on.load(std::memory_order_relaxed)) return {};
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
        if (!c.target_id || (c.kind != na::observe::TextureKind::ClearDepth && c.kind != na::observe::TextureKind::ClearColor)) return {};
        std::lock_guard<std::mutex> lock(g_m);
        if (c.kind == na::observe::TextureKind::ClearDepth) {
            Clear& k = g_clears[c.target_id];
            if (c.flags & D3D12_CLEAR_FLAG_DEPTH) k.depth = c.depth;
            if (c.flags & D3D12_CLEAR_FLAG_STENCIL) k.stencil = c.stencil;
            if (g_clears.size() > 512) g_clears.clear();
            if (c.rect_count || !g_l.d[0] || !g_l.dsv_count) return {};
            for (const auto& p : g_proxy)
                if (p.id && p.hud && p.mirrored == c.target_id) {
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
    if (g_on.load(std::memory_order_relaxed) && na::observe::ListCurrentTargets(l, &rt, &ds) && rt) {
        std::lock_guard<std::mutex> lock(g_m);
        if (find_locked(rt)) b.proxy = rt;
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
    na::observe::SetRasterStateListener(&on_bind);
    na::observe::SetShaderOperationHook(na::observe::ShaderLane::Reconstruction, std::make_shared<DrawHook>());
    na::observe::SetTextureOperationHook(std::make_shared<ClearHook>());
}
bool Installed() { return g_installed.load(); }
void SetEnabled(bool on) { g_on.store(on); }
bool Enabled() { return g_installed.load() && g_on.load(); }

bool TakeFrame(ID3D12Resource* frame, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, Layer* out)
{
    if (!g_installed.load() || !frame) return false;
    const na_object_id id = na::observe::IdOf(frame);
    std::lock_guard<std::mutex> lock(g_m);
    ++g_frame;
    g_grave.erase(std::remove_if(g_grave.begin(), g_grave.end(), [](const Grave& g) { return g.frame + kGraveFrames < g_frame; }), g_grave.end());
    if (!id) return false;
    Proxy* p = find_locked(id);
    if (!p || p->rw != rw || p->rh != rh || p->ow != ow || p->oh != oh) {
        if (!p) { // learn it, replacing the proxy presented longest ago
            p = &g_proxy[0];
            for (auto& q : g_proxy) if (q.used < p->used) p = &q;
        }
        *p = Proxy{};
        p->id = id; p->rw = rw; p->rh = rh; p->ow = ow; p->oh = oh;
        p->format = frame->GetDesc().Format;
        p->used = g_frame;
        return false;
    }
    p->used = g_frame;
    const bool hud = g_on.load() && p->cleared && p->redirected > 0 && g_l.n[0] && g_l.w == ow && g_l.h == oh;
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
    g_stats.last_depth = p->depth_draws; g_stats.last_blended = p->blended; g_stats.last_covered = p->covered;
    g_stats.proxy_format = uint32_t(p->format); g_stats.view_format = uint32_t(p->view);
    g_stats.grey = p->view != DXGI_FORMAT_UNKNOWN ? GreyFor(p->view) : 0.f;
    p->hud = p->cleared = false;
    p->mirrored = 0;
    p->view = DXGI_FORMAT_UNKNOWN;
    p->scene = p->redirected = p->forwarded = 0;
    p->depth_draws = p->blended = p->covered = 0;
    return hud;
}

Stats GetStats()
{
    std::lock_guard<std::mutex> lock(g_m);
    return g_stats;
}
std::string StatusLine()
{
    const Stats s = GetStats();
    char b[512];
    std::snprintf(b, sizeof b,
                  "native_hud on=%u frames=%llu hud_frames=%llu scene_draws=%llu redirected=%llu forwarded=%llu restore_failed=%llu "
                  "covered=%llu last_frame=scene:%u,hud:%u,forwarded:%u,depth:%u,blended:%u,covered:%u proxy_fmt=%u view_fmt=%u grey=%.4f last_forward=\"%s\"",
                  Enabled() ? 1u : 0u, (unsigned long long)s.frames, (unsigned long long)s.hud_frames, (unsigned long long)s.scene_draws,
                  (unsigned long long)s.redirected, (unsigned long long)s.forwarded, (unsigned long long)s.restore_failed, (unsigned long long)s.covered,
                  s.last_scene, s.last_redirected, s.last_forwarded, s.last_depth, s.last_blended, s.last_covered, s.proxy_format, s.view_format,
                  double(s.grey), s.last_forward);
    return b;
}

} // namespace na::j2::native_hud
