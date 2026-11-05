#include "host_dx12/j2/hud_unblend.hpp"
#include "host_dx12/j2/native_hud.hpp"
#include "host_dx12/j2/live_dlaa.hpp"
#include "host_dx12/observe/producers.hpp"

#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace na::j2::hud_unblend {
namespace {
using Microsoft::WRL::ComPtr;
constexpr uint32_t kSets = 3;  // frames in flight whose layers may still be read by Present
constexpr uint32_t kViews = 4; // render-target view formats per set

std::atomic<bool> g_configured{false}, g_on{true};
std::mutex g_m;
Stats g_stats;
uint64_t g_frame = 0;
struct Dims { uint32_t rw = 0, rh = 0, ow = 0, oh = 0; DXGI_FORMAT proxy = DXGI_FORMAT_UNKNOWN; } g_dims;

struct Set {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Resource> n[2], l[2]; // native black/grey, render-size black/grey (RENDER_TARGET between uses)
    uint32_t rw = 0, rh = 0, ow = 0, oh = 0;
    DXGI_FORMAT typeless = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D12DescriptorHeap> rtv; // [view i * 4 + target]: targets l0, l1, n0, n1; then 4 clear views (non-sRGB)
    DXGI_FORMAT views[kViews] = {};
    uint32_t nviews = 0;
    UINT inc = 0;
    // The frame being recorded into it:
    uint64_t frame = ~0ull;
    bool cleared = false;
    DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN;
    uint32_t draws = 0, covered = 0;
};
Set g_set[kSets];

struct Bind { ID3D12GraphicsCommandList* list = nullptr; uint64_t recording = 0; na_object_id rt = 0; bool candidate = false; };
thread_local Bind t_bind;
std::unordered_map<na_object_id, D3D12_RESOURCE_DESC> g_shapes; // render targets seen (bounded)

// Self-check window.
uint64_t g_w_frames = 0, g_w_ui = 0, g_w_opaque = 0, g_w_id = 0, g_w_dec = 0, g_w_enc = 0;
std::atomic<uint32_t> g_verdict{0};

bool is_srgb(DXGI_FORMAT f) { return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; }
// Same numbers as native_hud: what the clear stores, exactly representable, written through a
// non-sRGB view so the stored value is the number itself (GreyFor reports how `view` reads it).
float stored_grey(DXGI_FORMAT typeless)
{
    switch (typeless) {
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return 512.f / 1023.f;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return 1.f;
    default: return 128.f / 255.f;
    }
}
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

bool committed(ID3D12Device* dev, uint32_t w, uint32_t h, DXGI_FORMAT typeless, ComPtr<ID3D12Resource>& out, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = typeless;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&out)))) return false;
    out->SetName(name);
    return true;
}
bool set_ready_locked(ID3D12Device* dev, Set& s, const Dims& d)
{
    const DXGI_FORMAT typeless = native_hud::TypelessOf(d.proxy);
    if (typeless == DXGI_FORMAT_UNKNOWN) return false;
    if (s.device.Get() == dev && s.rw == d.rw && s.rh == d.rh && s.ow == d.ow && s.oh == d.oh && s.typeless == typeless && s.n[0]) return true;
    s = Set{};
    s.device = dev;
    if (!committed(dev, d.ow, d.oh, typeless, s.n[0], L"NeuralAlias HUD native black") ||
        !committed(dev, d.ow, d.oh, typeless, s.n[1], L"NeuralAlias HUD native grey") ||
        !committed(dev, d.rw, d.rh, typeless, s.l[0], L"NeuralAlias HUD small black") ||
        !committed(dev, d.rw, d.rh, typeless, s.l[1], L"NeuralAlias HUD small grey")) { s = Set{}; return false; }
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kViews * 4 + 4, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s.rtv)))) { s = Set{}; return false; }
    s.inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    {
        ID3D12Resource* const t[4] = {s.l[0].Get(), s.l[1].Get(), s.n[0].Get(), s.n[1].Get()};
        D3D12_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = clear_view_of(typeless);
        rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        for (int k = 0; k < 4; ++k) {
            D3D12_CPU_DESCRIPTOR_HANDLE h = s.rtv->GetCPUDescriptorHandleForHeapStart();
            h.ptr += SIZE_T(kViews * 4 + k) * s.inc;
            dev->CreateRenderTargetView(t[k], &rd, h);
        }
    }
    s.rw = d.rw; s.rh = d.rh; s.ow = d.ow; s.oh = d.oh; s.typeless = typeless;
    return true;
}
// RTVs of the four targets through `view` (made on first use; at most kViews formats per set).
bool rtvs_locked(Set& s, DXGI_FORMAT view, D3D12_CPU_DESCRIPTOR_HANDLE out[4])
{
    uint32_t i = 0;
    for (; i < s.nviews; ++i) if (s.views[i] == view) break;
    if (i == s.nviews) {
        if (s.nviews == kViews) return false;
        ID3D12Resource* const t[4] = {s.l[0].Get(), s.l[1].Get(), s.n[0].Get(), s.n[1].Get()};
        D3D12_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = view;
        rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        for (int k = 0; k < 4; ++k) {
            D3D12_CPU_DESCRIPTOR_HANDLE h = s.rtv->GetCPUDescriptorHandleForHeapStart();
            h.ptr += SIZE_T(i * 4 + k) * s.inc;
            s.device->CreateRenderTargetView(t[k], &rd, h);
        }
        s.views[i] = view;
        ++s.nviews;
    }
    for (int k = 0; k < 4; ++k) {
        out[k] = s.rtv->GetCPUDescriptorHandleForHeapStart();
        out[k].ptr += SIZE_T(i * 4 + k) * s.inc;
    }
    return true;
}
// Background: black / grey through the non-sRGB clear views (stores the numbers themselves).
void clear_background_locked(ID3D12GraphicsCommandList* l, Set& s)
{
    const float grey = stored_grey(s.typeless);
    const float black[4] = {0.f, 0.f, 0.f, 1.f}, back[4] = {grey, grey, grey, 1.f};
    for (int k = 0; k < 4; ++k) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = s.rtv->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(kViews * 4 + k) * s.inc;
        l->ClearRenderTargetView(h, (k & 1) ? back : black, 0, nullptr);
    }
}
// The game's own clear colour, through the game's view (stored as the game's clear stores it).
void clear_colour_locked(ID3D12GraphicsCommandList* l, const D3D12_CPU_DESCRIPTOR_HANDLE rtv[4], const float* colour)
{
    for (int k = 0; k < 4; ++k) l->ClearRenderTargetView(rtv[k], colour, 0, nullptr);
}
void forward_locked(const char* why) { ++g_stats.forwarded; std::snprintf(g_stats.last_forward, sizeof g_stats.last_forward, "%s", why); }
void draw(ID3D12GraphicsCommandList* l, const na::observe::ShaderCall& c)
{
    if (c.kind == na::observe::ShaderKind::Draw) l->DrawInstanced(UINT(c.args[0]), UINT(c.args[1]), UINT(c.args[2]), UINT(c.args[3]));
    else l->DrawIndexedInstanced(UINT(c.args[0]), UINT(c.args[1]), UINT(c.args[2]), INT(c.args[3]), UINT(c.args[4]));
}
} // namespace

void SetConfigured(bool on) { g_configured.store(on); }
bool Configured() { return g_configured.load(); }
void SetEnabled(bool on) { g_on.store(on); }
bool Enabled() { return g_configured.load(std::memory_order_relaxed) && g_on.load(std::memory_order_relaxed); }

void OnBind(ID3D12GraphicsCommandList* l, uint64_t recording)
{
    Bind b{l, recording, 0, false};
    na_object_id rt = 0, ds = 0;
    if (na::observe::ListCurrentTargets(l, &rt, &ds) && rt) {
        std::lock_guard<std::mutex> lock(g_m);
        b.rt = rt;
        if (g_dims.ow) {
            auto it = g_shapes.find(rt);
            if (it == g_shapes.end()) {
                ComPtr<ID3D12Resource> r;
                r.Attach(na::observe::AcquireResource(rt));
                if (r) {
                    if (g_shapes.size() >= 512) g_shapes.clear();
                    it = g_shapes.emplace(rt, r->GetDesc()).first;
                }
            }
            if (it != g_shapes.end()) {
                const auto& d = it->second;
                b.candidate = d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.MipLevels == 1 && d.DepthOrArraySize == 1 &&
                              d.SampleDesc.Count == 1 && d.Width == g_dims.rw && d.Height == g_dims.rh &&
                              native_hud::TypelessOf(d.Format) != DXGI_FORMAT_UNKNOWN &&
                              native_hud::TypelessOf(d.Format) == native_hud::TypelessOf(g_dims.proxy);
            }
        }
    }
    t_bind = b;
}

na::observe::ShaderScope OnDraw(ID3D12GraphicsCommandList* l, const na::observe::ShaderCall& call)
{
    if (call.kind == na::observe::ShaderKind::Dispatch || t_bind.list != l || !t_bind.candidate) return {};
    na::observe::DrawInfo st;
    if (!na::observe::ListDrawState(l, st) || st.recording != t_bind.recording || st.rt != t_bind.rt) return {};
    const uint64_t verts = uint64_t(call.args[0]), inst = uint64_t(call.args[1]);
    const bool blend_known = st.pipeline_known && st.blend_mask != NA_UNKNOWN_U32;
    const bool blended = blend_known && (st.blend_mask & 1u) != 0;
    std::lock_guard<std::mutex> lock(g_m);
    const Dims d = g_dims;
    if (!d.ow) return {};
    Set& s = g_set[g_frame % kSets];
    const bool mine = s.frame == g_frame && s.cleared;
    const float* vp0 = st.viewport_count ? st.viewports[0] : nullptr;
    const int32_t* sc0 = st.scissor_count ? st.scissors[0] : nullptr;
    // Full-screen passes (tonemap, colour grading, fades, vignettes) are image processing:
    // they stay in the picture. An opaque one hides everything drawn before it, so the
    // layers start again; a blended one leaves the earlier UI on top (the un-blend formula
    // is linear, so the composite still comes out as the game's).
    if (verts <= 6 && inst == 1 && native_hud::CoversTarget(vp0, sc0, d.rw, d.rh)) {
        if (!blended) {
            if (mine && s.draws) {
                clear_background_locked(l, s);
                s.draws = 0;
                ++s.covered;
                ++g_stats.covered;
            }
            ++g_stats.scene;
        } else ++g_stats.cover_blend;
        return {};
    }
    {   // Drawn with the scene camera, or depth-tested on the scene depth: part of the picture.
        na::observe::LiveBinding cb[16];
        const uint32_t n = na::observe::ListCurrentGraphicsCbvs(l, cb, 16);
        for (uint32_t i = 0; i < n; ++i)
            if (na::j2::CameraBinding(cb[i].resource, cb[i].offset, cb[i].param, cb[i].table)) { ++g_stats.world; return {}; }
        uint64_t scene_depth = 0, scene_color = 0;
        na::observe::GetLiveSelection(&scene_depth, &scene_color);
        if (scene_depth && st.ds == scene_depth && st.pipeline_known && st.depth_enable != NA_UNKNOWN_U32 && st.depth_enable != 0u &&
            st.dsv_format != DXGI_FORMAT_UNKNOWN) { ++g_stats.world; return {}; }
    }
    const DXGI_FORMAT view = static_cast<DXGI_FORMAT>(st.rtv_format);
    const char* why = nullptr;
    if (!st.restorable) why = "targets cannot be handed back";
    else if (!st.pipeline_known) why = "unknown pipeline";
    else if (st.rt_count != 1 || st.pso_rt_count != 1) why = "more than one target";
    else if (st.samples != 1) why = "multisampled";
    else if (!st.viewport_count || !st.scissor_count) why = "no viewport or scissor";
    else if (st.ds && st.dsv_format != DXGI_FORMAT_UNKNOWN) why = "uses a depth/stencil target";
    else if (native_hud::TypelessOf(view) != native_hud::TypelessOf(d.proxy)) why = "view format differs from the picture";
    else if (!blend_known) why = "blend state unknown";
    if (why) { forward_locked(why); return {}; }
    ComPtr<ID3D12Device> dev;
    if (FAILED(l->GetDevice(IID_PPV_ARGS(&dev))) || !set_ready_locked(dev.Get(), s, d)) { forward_locked("could not make the layers"); return {}; }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[4];
    if (s.frame != g_frame) { s.frame = g_frame; s.cleared = false; s.draws = 0; s.covered = 0; s.view = DXGI_FORMAT_UNKNOWN; }
    if (!s.cleared) {
        if (!rtvs_locked(s, view, rtv)) { forward_locked("too many view formats"); return {}; }
        clear_background_locked(l, s);
        s.cleared = true;
        s.view = view;
    } else {
        if (view != s.view) ++g_stats.view_changed; // the grey background follows the first view
        if (!rtvs_locked(s, view, rtv)) { forward_locked("too many view formats"); return {}; }
    }
    // Everything below is host work recorded into the game's list; the game's own draw follows.
    l->OMSetRenderTargets(1, &rtv[0], FALSE, nullptr); // L0: the game's viewport and scissor
    draw(l, call);
    l->OMSetRenderTargets(1, &rtv[1], FALSE, nullptr); // L1
    draw(l, call);
    const float sx = float(d.ow) / float(d.rw), sy = float(d.oh) / float(d.rh);
    D3D12_VIEWPORT vp[16];
    D3D12_RECT sc[16];
    for (uint32_t i = 0; i < st.viewport_count; ++i) {
        float o[6];
        native_hud::ScaleViewport(st.viewports[i], sx, sy, o);
        vp[i] = {o[0], o[1], o[2], o[3], o[4], o[5]};
    }
    for (uint32_t i = 0; i < st.scissor_count; ++i) {
        int32_t o[4];
        native_hud::ScaleScissor(st.scissors[i], sx, sy, d.ow, d.oh, o);
        sc[i] = {o[0], o[1], o[2], o[3]};
    }
    l->RSSetViewports(st.viewport_count, vp);
    l->RSSetScissorRects(st.scissor_count, sc);
    l->OMSetRenderTargets(1, &rtv[2], FALSE, nullptr); // N0
    draw(l, call);
    l->OMSetRenderTargets(1, &rtv[3], FALSE, nullptr); // N1
    draw(l, call);
    if (!na::observe::RestoreBindingState(l, false, false, 0, true, true, true)) ++g_stats.restore_failed;
    ++s.draws;
    ++g_stats.replayed;
    if (blended) ++g_stats.blended;
    return {};
}

void OnClear(ID3D12GraphicsCommandList* l, uint64_t target, const float color[4])
{
    if (t_bind.list != l || !t_bind.candidate || t_bind.rt != target) return;
    std::lock_guard<std::mutex> lock(g_m);
    Set& s = g_set[g_frame % kSets];
    if (s.frame != g_frame || !s.cleared || !s.draws) return;
    // A clear after the UI started replaces everything under it: both layers take the colour,
    // so the composite shows exactly that colour there.
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[4];
    if (rtvs_locked(s, s.view, rtv)) { clear_colour_locked(l, rtv, color); ++g_stats.cleared_after; }
}

bool TakeFrame(ID3D12Resource* frame, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, native_hud::Layer* out)
{
    if (!frame || !rw || !rh || !ow || !oh) return false;
    std::lock_guard<std::mutex> lock(g_m);
    const DXGI_FORMAT pf = frame->GetDesc().Format;
    if (g_dims.rw != rw || g_dims.rh != rh || g_dims.ow != ow || g_dims.oh != oh || g_dims.proxy != pf) {
        g_dims = Dims{rw, rh, ow, oh, pf};
        g_shapes.clear();
    }
    Set& s = g_set[g_frame % kSets];
    const bool hud = s.frame == g_frame && s.cleared && s.draws > 0 && s.rw == rw && s.rh == rh && s.ow == ow && s.oh == oh;
    ++g_stats.frames;
    g_stats.last_draws = hud ? s.draws : 0;
    g_stats.last_covered = hud ? s.covered : 0;
    g_stats.last_view = hud ? uint32_t(s.view) : 0u;
    const bool compose = g_verdict.load(std::memory_order_relaxed) != 2u;
    if (hud) {
        ++g_stats.hud_frames;
        if (compose) ++g_stats.composed; else ++g_stats.held;
        if (out) {
            out->n0 = s.n[0].Get(); out->n1 = s.n[1].Get();
            out->l0 = s.l[0].Get(); out->l1 = s.l[1].Get();
            out->view = s.view;
            out->grey = native_hud::GreyFor(s.view);
            out->srgb = is_srgb(s.view);
            out->unblend = true;
            out->compose = compose;
        }
    }
    // The set of the frame that is kSets presents back is free again (its Present's GPU work
    // was submitted before anything recorded from now on).
    ++g_frame;
    Set& next = g_set[g_frame % kSets];
    if (next.frame != g_frame) { next.frame = ~0ull; next.cleared = false; next.draws = 0; next.covered = 0; }
    return hud;
}

void ReportCheck(uint32_t ui_px, uint32_t opaque, uint32_t match_id, uint32_t match_dec, uint32_t match_enc)
{
    std::lock_guard<std::mutex> lock(g_m);
    ++g_stats.check_frames;
    g_w_ui += ui_px; g_w_opaque += opaque; g_w_id += match_id; g_w_dec += match_dec; g_w_enc += match_enc;
    g_stats.ui_px = g_w_ui; g_stats.opaque_px = g_w_opaque; g_stats.match_id = g_w_id; g_stats.match_dec = g_w_dec; g_stats.match_enc = g_w_enc;
    if (++g_w_frames < 30) return;
    if (g_w_opaque >= 500) { // enough fully covered UI pixels to decide
        const auto permille = [&](uint64_t n) { return uint32_t(n * 1000u / g_w_opaque); };
        g_stats.rate_id = permille(g_w_id); g_stats.rate_dec = permille(g_w_dec); g_stats.rate_enc = permille(g_w_enc);
        const uint32_t v = g_stats.rate_id >= 850 ? 1u : 2u;
        g_stats.verdict = v;
        g_verdict.store(v, std::memory_order_relaxed);
    }
    g_w_frames = g_w_ui = g_w_opaque = g_w_id = g_w_dec = g_w_enc = 0;
}

Stats GetStats() { std::lock_guard<std::mutex> lock(g_m); return g_stats; }
std::string StatusLine()
{
    const Stats s = GetStats();
    char b[1024];
    std::snprintf(b, sizeof b,
        "hud_unblend on=%u frames=%llu hud_frames=%llu composed=%llu held=%llu replayed=%llu blended=%llu forwarded=%llu world=%llu scene=%llu covered=%llu "
        "cover_blend=%llu cleared_after=%llu view_changed=%llu restore_failed=%llu last{draws=%u covered=%u view=%u} last_forward=\"%s\" "
        "check{frames=%llu window{ui_px=%llu opaque=%llu id=%llu dec=%llu enc=%llu} permille{id=%u dec=%u enc=%u} verdict=%s}",
        Enabled() ? 1u : 0u, (unsigned long long)s.frames, (unsigned long long)s.hud_frames, (unsigned long long)s.composed, (unsigned long long)s.held,
        (unsigned long long)s.replayed, (unsigned long long)s.blended, (unsigned long long)s.forwarded, (unsigned long long)s.world, (unsigned long long)s.scene,
        (unsigned long long)s.covered, (unsigned long long)s.cover_blend, (unsigned long long)s.cleared_after, (unsigned long long)s.view_changed,
        (unsigned long long)s.restore_failed, s.last_draws, s.last_covered, s.last_view, s.last_forward,
        (unsigned long long)s.check_frames, (unsigned long long)s.ui_px, (unsigned long long)s.opaque_px, (unsigned long long)s.match_id,
        (unsigned long long)s.match_dec, (unsigned long long)s.match_enc, s.rate_id, s.rate_dec, s.rate_enc,
        s.verdict == 1 ? "identity" : s.verdict == 2 ? "mismatch_holding" : "undecided");
    return b;
}
} // namespace na::j2::hud_unblend
