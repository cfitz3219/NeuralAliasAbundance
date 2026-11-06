#include "host_dx12/j2/hud_unblend.hpp"
#include "host_dx12/gpu/mem_alloc.hpp"
#include "host_dx12/j2/native_hud.hpp"
#include "host_dx12/j2/hud_picture.hpp"
#include "host_dx12/j2/live_dlaa.hpp"
#include "host_dx12/observe/producers.hpp"
#include "host_dx12/observe/texture_operation.hpp"
#include "host_dx12/observe/operation_snapshot.hpp"
#include "host_dx12/nr/native_fg.hpp"

#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

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
    na_object_id target = 0; // exact destination identity, not the most recently recorded candidate
    bool cleared = false;
    DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN;
    uint32_t draws = 0, covered = 0;
    std::vector<na::nr::native::RecordingLease> uses;
    std::shared_ptr<native_hud::Picture> picture;
    bool scene_captured = false, full_captured = false, clean = true;
    ID3D12GraphicsCommandList* writer = nullptr;
    uint64_t writer_id = 0, recording = 0;
};
struct FrameSet {
    std::unordered_map<na_object_id, std::shared_ptr<Set>> active, transferred;
};
FrameSet g_set[kSets];
std::vector<std::shared_ptr<Set>> g_pool;
constexpr size_t kMaxTargets = 8, kMaxPool = 48;

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
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = typeless;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    // D3D12 Memory Allocator (placed): safe here because every layer's first GPU operation in
    // a frame is clear_background_locked's full ClearRenderTargetView (see OnDraw/TakeFrame:
    // layers are only read in frames that cleared them).
    if (FAILED(na::gpu::mem::CreateResource(dev, D3D12_HEAP_TYPE_DEFAULT, d, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, out.ReleaseAndGetAddressOf()))) return false;
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
std::shared_ptr<Set> acquire_set_locked(ID3D12Device* dev, const Dims& d)
{
    for (const auto& p : g_pool) {
        if (p.use_count() != 1 || !std::all_of(p->uses.begin(), p->uses.end(), [](const auto& u) { return u.Retired(); })) continue;
        if (!set_ready_locked(dev, *p, d)) return {};
        p->uses.clear(); p->frame = g_frame; p->cleared = false; p->draws = p->covered = 0; p->target = 0;
        p->view = DXGI_FORMAT_UNKNOWN;
        p->scene_captured = p->full_captured = false; p->clean = true;
        p->writer = nullptr; p->writer_id = p->recording = 0;
        if (p->picture) p->picture->BeginCycle();
        return p;
    }
    if (g_pool.size() >= kMaxPool) return {};
    auto p = std::make_shared<Set>();
    if (!set_ready_locked(dev, *p, d)) return {};
    p->frame = g_frame; g_pool.push_back(p); return p;
}
bool retain_locked(ID3D12GraphicsCommandList* l, const std::shared_ptr<Set>& p)
{
    auto use = na::nr::native::RetainRecording(l, p);
    if (!use) return false;
    if (std::none_of(p->uses.begin(), p->uses.end(), [&](const auto& u) { return u.SameRecording(use); })) p->uses.push_back(use);
    return true;
}
void copy_layers(ID3D12GraphicsCommandList* l, Set& dst, const Set& src)
{
    ID3D12Resource* to[] = {dst.l[0].Get(), dst.l[1].Get(), dst.n[0].Get(), dst.n[1].Get()};
    ID3D12Resource* from[] = {src.l[0].Get(), src.l[1].Get(), src.n[0].Get(), src.n[1].Get()};
    D3D12_RESOURCE_BARRIER b[8]{};
    for (UINT i = 0; i < 8; ++i) {
        b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[i].Transition = {i < 4 ? from[i] : to[i - 4], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                           D3D12_RESOURCE_STATE_RENDER_TARGET, i < 4 ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_COPY_DEST};
    }
    l->ResourceBarrier(8, b);
    for (UINT i = 0; i < 4; ++i) l->CopyResource(to[i], from[i]);
    for (auto& v : b) std::swap(v.Transition.StateBefore, v.Transition.StateAfter);
    l->ResourceBarrier(8, b);
    dst.draws = src.draws; dst.covered = src.covered; dst.view = src.view; dst.cleared = true;
    dst.clean = src.clean;
}
bool picture_locked(Set& s, const D3D12_RESOURCE_DESC& desc)
{
    if (!s.picture || !s.picture->Matches(s.device.Get(), desc, s.ow, s.oh))
        s.picture = native_hud::Picture::Create(s.device.Get(), desc, s.ow, s.oh);
    return !!s.picture;
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
    // Blending switched on but the destination replaced (DestBlend ZERO) is an opaque draw.
    // Cort's p23896 run (2026-11-05): every replayed pixel came out independent of the
    // background (ui_px == opaque, 1.3M px a frame) and differed from the final picture, which
    // is what a full-screen post pass with blending "on" but ONE/ZERO factors looks like.
    const bool blended = blend_known && (st.blend_mask & 1u) != 0 && st.blend_class != 3;
    std::lock_guard<std::mutex> lock(g_m);
    const Dims d = g_dims;
    if (!d.ow) return {};
    FrameSet& sets = g_set[g_frame % kSets];
    const auto previous = sets.active.find(t_bind.rt);
    Set* current = previous == sets.active.end() ? nullptr : previous->second.get();
    const bool mine = current && current->cleared;
    const float* vp0 = st.viewport_count ? st.viewports[0] : nullptr;
    const int32_t* sc0 = st.scissor_count ? st.scissors[0] : nullptr;
    // Draws with a viewport covering the whole target and at most 6 vertices. An opaque one
    // (tonemap, colour grading) replaces everything drawn before it: it stays in the picture
    // and the layers start again. A BLENDED one is replayed like any other UI draw: UI sprites
    // are drawn with a full-target viewport too (only their vertices are small), and a game
    // that renders its UI into a canvas lays that canvas on with one blended full-screen
    // triangle whose input we cannot see (SE5, Cort's run of 2026-11-05: ~29 such draws per
    // frame were skipped, so most menus were missing from the layers and the self-check never
    // matched). Replaying a blended draw is exact whatever it reads: its effect on the
    // destination is linear, which is all the un-blend formula needs.
    if (verts <= 6 && inst == 1 && native_hud::CoversTarget(vp0, sc0, d.rw, d.rh)) {
        if (!blended) {
            if (blend_known && (st.blend_mask & 1u) != 0) ++g_stats.opaque_blend;
            if (mine && current->draws) {
                clear_background_locked(l, *current);
                current->draws = 0;
                current->scene_captured = current->full_captured = false;
                current->clean = true;
                if (current->picture) current->picture->BeginCycle();
                ++current->covered;
                ++g_stats.covered;
            }
            sets.transferred.erase(t_bind.rt);
            ++g_stats.scene;
            return {};
        }
        ++g_stats.cover_blend; // replayed below (unless forwarded for another reason)
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
    // Target identity is independent of recording order. Each candidate owns its own
    // layers; only an observed transfer can associate it with the presented resource.
    if (!st.restorable) why = "targets cannot be handed back";
    else if (!st.pipeline_known) why = "unknown pipeline";
    else if (st.rt_count != 1 || st.pso_rt_count != 1) why = "more than one target";
    else if (st.samples != 1) why = "multisampled";
    else if (!st.viewport_count || !st.scissor_count) why = "no viewport or scissor";
    else if (st.ds && st.dsv_format != DXGI_FORMAT_UNKNOWN) why = "uses a depth/stencil target";
    else if (native_hud::TypelessOf(view) != native_hud::TypelessOf(d.proxy)) why = "view format differs from the picture";
    else if (!blend_known) why = "blend state unknown";
    else if (st.blend_class == 4) { ++g_stats.irreversible; why = "blend reads the destination's alpha or uses min/max"; }
    else if (!st.shader_readonly || st.query_active) why = "draw has side effects";
    if (why) { forward_locked(why); return {}; }
    ComPtr<ID3D12Device> dev;
    if (FAILED(l->GetDevice(IID_PPV_ARGS(&dev)))) { forward_locked("no device"); return {}; }
    std::shared_ptr<Set> owner;
    if (previous != sets.active.end()) owner = previous->second;
    else {
        if (sets.active.size() >= kMaxTargets) { forward_locked("candidate pool busy"); return {}; }
        owner = acquire_set_locked(dev.Get(), d);
        if (!owner) { forward_locked("could not make the layers"); return {}; }
        sets.active.emplace(t_bind.rt, owner);
        const auto inherited = sets.transferred.find(t_bind.rt);
        if (inherited != sets.transferred.end()) {
            const auto& base = *inherited->second;
            copy_layers(l, *owner, base);
            if (base.picture && base.scene_captured && picture_locked(*owner, base.picture->Scene()->GetDesc()))
                owner->scene_captured = owner->picture->SnapshotScene(l, base.picture->Scene(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
    }
    if (!retain_locked(l, owner)) { forward_locked("could not retain recording"); return {}; }
    Set& s = *owner;
    s.target = t_bind.rt;
    s.writer = l; s.writer_id = st.list; s.recording = st.recording;
    s.full_captured = false;
    s.clean &= st.blend_class >= 1 && st.blend_class <= 3;
    na::observe::NativeOperation op;
    if (!na::observe::ListNativeOperation(l, false, op) || !op.reflection_complete) s.clean = false;
    else for (const auto& input : op.descriptors) if (input.used && input.view.kind == NA_DESC_SRV && input.resource && input.view.resource != st.rt) {
        const auto desc = input.resource->GetDesc();
        // A batch can carry the world through a sampled image as well as draw
        // text. Such a source needs substitution in the native replay; an
        // empty UI canvas is not a reconstruction input for that world.
        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Width == d.rw && desc.Height == d.rh &&
            native_hud::TypelessOf(desc.Format) != DXGI_FORMAT_UNKNOWN) s.clean = false;
    }
    if (!s.scene_captured) {
        ComPtr<ID3D12Resource> original; original.Attach(na::observe::AcquireResource(t_bind.rt));
        if (original && picture_locked(s, original->GetDesc()))
            s.scene_captured = s.picture->SnapshotScene(l, original.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[4];
    if (s.frame != g_frame) { s.frame = g_frame; s.cleared = false; s.draws = 0; s.covered = 0; s.view = DXGI_FORMAT_UNKNOWN; s.target = 0; }
    if (!s.cleared) {
        s.target = t_bind.rt;
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
    if (!target) return;
    (void)l; (void)color;
    std::lock_guard<std::mutex> lock(g_m);
    auto& sets = g_set[g_frame % kSets];
    const auto erased = sets.active.erase(target) + sets.transferred.erase(target);
    if (erased) ++g_stats.cleared_after;
}

void OnTexture(ID3D12GraphicsCommandList* l, const na::observe::TextureCall& c)
{
    // Runtime wrappers may implement a CopyResource through an internal
    // CopyTextureRegion on another list interface. Only the observed game's
    // recording owns this handoff; the implementation detail is not another
    // submitted game recording and must not replace its frozen layers.
    if (!na::observe::ListRecording(l)) return;
    if (!c.source || !c.destination ||
        (c.kind != na::observe::TextureKind::Copy && c.kind != na::observe::TextureKind::CopyRegion)) return;
    const auto dst_id = na::observe::IdOf(c.destination), src_id = na::observe::IdOf(c.source);
    if (!dst_id || !src_id || dst_id == src_id) return;
    const auto sd = c.source->GetDesc(), dd = c.destination->GetDesc();
    const bool whole = sd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && dd.Dimension == sd.Dimension &&
        sd.Width == dd.Width && sd.Height == dd.Height && sd.DepthOrArraySize == 1 && dd.DepthOrArraySize == 1 &&
        sd.MipLevels == 1 && dd.MipLevels == 1 && sd.SampleDesc.Count == 1 && dd.SampleDesc.Count == 1 &&
        native_hud::TypelessOf(sd.Format) == native_hud::TypelessOf(dd.Format) &&
        (c.kind == na::observe::TextureKind::Copy || (!c.x && !c.y && !c.z &&
          c.source_location.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && c.source_location.SubresourceIndex == 0 &&
          c.destination_location.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && c.destination_location.SubresourceIndex == 0 &&
          (!c.box || (!c.box->left && !c.box->top && !c.box->front && c.box->right == sd.Width &&
                      c.box->bottom == sd.Height && c.box->back == 1))));
    std::lock_guard<std::mutex> lock(g_m);
    auto& sets = g_set[g_frame % kSets];
    ++g_stats.copies;
    std::shared_ptr<Set> source;
    const auto active = sets.active.find(src_id), transferred = sets.transferred.find(src_id);
    if (active != sets.active.end() && active->second->draws) source = active->second;
    else if (transferred != sets.transferred.end()) source = transferred->second;
    sets.active.erase(dst_id); sets.transferred.erase(dst_id);
    if (!whole) ++g_stats.unsupported_copies;
    if (!whole || !source || sets.transferred.size() >= kMaxTargets) return;
    // Freeze at the actual handoff, before a later source draw can replace the
    // layers. Keep both snapshots alive through the submitted copy recording.
    auto frozen = acquire_set_locked(source->device.Get(), g_dims);
    if (!frozen || !retain_locked(l, source) || !retain_locked(l, frozen)) return;
    copy_layers(l, *frozen, *source);
    if (source->picture && source->scene_captured && picture_locked(*frozen, source->picture->Scene()->GetDesc())) {
        frozen->scene_captured = frozen->picture->SnapshotScene(l, source->picture->Scene(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        uint32_t source_state = 0;
        if (na::observe::ListStateSub(l, src_id, 0, &source_state) == na::observe::StateLookup::Known)
            frozen->full_captured = frozen->picture->SnapshotFull(l, c.source, static_cast<D3D12_RESOURCE_STATES>(source_state));
    }
    frozen->target = dst_id;
    sets.transferred.emplace(dst_id, std::move(frozen));
    ++g_stats.copied_layers;
}

void OnClosing(ID3D12GraphicsCommandList* l)
{
    const auto id = na::observe::IdOf(l), recording = na::observe::ListRecording(l);
    if (!id || !recording) return;
    std::lock_guard<std::mutex> lock(g_m);
    for (const auto& item : g_set[g_frame % kSets].active) {
        auto& s = *item.second;
        if (s.writer_id != id || s.recording != recording || !s.draws || !s.picture || !s.scene_captured) continue;
        uint32_t state = 0;
        if (na::observe::ListStateSub(l, s.target, 0, &state) != na::observe::StateLookup::Known) continue;
        ComPtr<ID3D12Resource> original; original.Attach(na::observe::AcquireResource(s.target));
        if (original) s.full_captured = s.picture->SnapshotFull(l, original.Get(), static_cast<D3D12_RESOURCE_STATES>(state));
    }
}

bool TakeFrame(ID3D12Resource* frame, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, native_hud::Layer* out,
               ID3D12CommandQueue* queue)
{
    if (!frame || !rw || !rh || !ow || !oh) return false;
    std::lock_guard<std::mutex> lock(g_m);
    const DXGI_FORMAT pf = frame->GetDesc().Format;
    if (g_dims.rw != rw || g_dims.rh != rh || g_dims.ow != ow || g_dims.oh != oh || g_dims.proxy != pf) {
        g_dims = Dims{rw, rh, ow, oh, pf};
        g_shapes.clear();
        for (auto& old : g_set) { old.active.clear(); old.transferred.clear(); }
    }
    auto& sets = g_set[g_frame % kSets];
    const auto id = na::observe::IdOf(frame);
    std::shared_ptr<Set> owner;
    const auto active = sets.active.find(id), transferred = sets.transferred.find(id);
    if (active != sets.active.end()) owner = active->second;
    else if (transferred != sets.transferred.end()) owner = transferred->second;
    g_stats.requested_target = id;
    g_stats.matched_target = owner ? owner->target : 0;
    g_stats.pending_recordings = 0;
    if (owner) for (const auto& use : owner->uses) if (!use.Ready(queue)) ++g_stats.pending_recordings;
    const bool hud = owner && owner->cleared && owner->draws > 0 && owner->rw == rw && owner->rh == rh &&
        owner->ow == ow && owner->oh == oh && queue && !owner->uses.empty() &&
        std::all_of(owner->uses.begin(), owner->uses.end(), [&](const auto& u) { return u.Ready(queue); });
    ++g_stats.frames;
    g_stats.last_draws = hud ? owner->draws : 0;
    g_stats.last_covered = hud ? owner->covered : 0;
    g_stats.last_view = hud ? uint32_t(owner->view) : 0u;
    const bool current_proof = hud && owner->picture && owner->scene_captured && owner->full_captured;
    g_stats.current_picture = current_proof;
    const bool compose = current_proof || g_verdict.load(std::memory_order_relaxed) != 2u;
    if (hud) {
        ++g_stats.hud_frames;
        if (compose) ++g_stats.composed; else ++g_stats.held;
        if (out) {
            Set& s = *owner;
            out->n0 = s.n[0].Get(); out->n1 = s.n[1].Get();
            out->l0 = s.l[0].Get(); out->l1 = s.l[1].Get();
            out->view = s.view;
            out->grey = native_hud::GreyFor(s.view);
            out->srgb = is_srgb(s.view);
            out->unblend = true;
            out->compose = compose;
            out->owner = owner;
            if (current_proof) {
                s.picture->SetValidationLayers(s.l[0].Get(), s.l[1].Get());
                out->picture = s.picture; out->clean_allowed = s.clean;
            }
        }
    }
    // The set of the frame that is kSets presents back is free again (its Present's GPU work
    // was submitted before anything recorded from now on).
    ++g_frame;
    auto& next = g_set[g_frame % kSets];
    next.active.clear(); next.transferred.clear();
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
        "cover_blend=%llu opaque_blend=%llu irreversible=%llu other_target=%llu retargeted=%llu cleared_after=%llu view_changed=%llu restore_failed=%llu last{draws=%u covered=%u view=%u} last_forward=\"%s\" "
        "check{frames=%llu window{ui_px=%llu opaque=%llu id=%llu dec=%llu enc=%llu} permille{id=%u dec=%u enc=%u} diagnostic_verdict=%s}",
        Enabled() ? 1u : 0u, (unsigned long long)s.frames, (unsigned long long)s.hud_frames, (unsigned long long)s.composed, (unsigned long long)s.held,
        (unsigned long long)s.replayed, (unsigned long long)s.blended, (unsigned long long)s.forwarded, (unsigned long long)s.world, (unsigned long long)s.scene,
        (unsigned long long)s.covered, (unsigned long long)s.cover_blend, (unsigned long long)s.opaque_blend, (unsigned long long)s.irreversible, (unsigned long long)s.other_target, (unsigned long long)s.retargeted, (unsigned long long)s.cleared_after, (unsigned long long)s.view_changed,
        (unsigned long long)s.restore_failed, s.last_draws, s.last_covered, s.last_view, s.last_forward,
        (unsigned long long)s.check_frames, (unsigned long long)s.ui_px, (unsigned long long)s.opaque_px, (unsigned long long)s.match_id,
        (unsigned long long)s.match_dec, (unsigned long long)s.match_enc, s.rate_id, s.rate_dec, s.rate_enc,
        s.verdict == 1 ? "identity" : s.verdict == 2 ? "mismatch_holding" : "undecided");
    char routing[256];
    std::snprintf(routing, sizeof routing, " routing{copies=%llu linked=%llu unsupported=%llu requested=%llu matched=%llu pending=%u method=%s}",
                  (unsigned long long)s.copies, (unsigned long long)s.copied_layers, (unsigned long long)s.unsupported_copies,
                  (unsigned long long)s.requested_target, (unsigned long long)s.matched_target, s.pending_recordings,
                  s.current_picture ? "current_picture" : s.last_draws ? "legacy_unblend" : "none");
    return std::string(b) + routing;
}
} // namespace na::j2::hud_unblend
