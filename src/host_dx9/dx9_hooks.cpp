// DX9 device hooks (DX9 plan step D3, docs/DX9-PLAN.md). Owner: Claude.
//
// Hooks the game's IDirect3DDevice9 methods (vtable, MinHook) and feeds what
// they do to the ears (dx9_ears.hpp): targets, draws with/without depth test,
// reads of the scene colour, depth clears, vertex-shader constants, fixed-
// function transforms, Present. At the point the ears find, the fire function
// (the transport, D4) runs on the game's thread before the call it precedes.
//
// Only the most recently created device is watched. One mutex guards the ears;
// it is never held while the fire function runs, and calls made from inside
// the fire function pass straight through.
#include "host_dx9/dx9_hooks.hpp"

#include "host_dx9/dx9_ears.hpp"
#include "host_dx9/vtable_hook.hpp"

#include <wrl/client.h>

#include <atomic>
#include <mutex>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace na::dx9 {
namespace {

std::mutex g_mu;
Ears g_ears;
std::atomic<IDirect3DDevice9*> g_dev{nullptr};
std::atomic<FireFn> g_fire{nullptr};
thread_local bool t_in_fire = false;
HookStats g_stats; // under g_mu
Id g_last_color = 0, g_last_depth = 0;

struct SurfaceInfo {
    Id id = 0;
    ResourceInfo info;
    D3DSURFACE_DESC desc{};
};
std::unordered_map<IDirect3DSurface9*, SurfaceInfo> g_surfaces; // under g_mu

bool Watched(const void* dev) { return dev && dev == static_cast<const void*>(g_dev.load(std::memory_order_acquire)) && !t_in_fire; }

// A surface's identity: its texture when it is a texture level, else itself.
// Re-checked by its description each time (a freed surface's address can be reused).
const SurfaceInfo& Describe(IDirect3DSurface9* s)
{
    static const SurfaceInfo none{};
    if (!s) return none;
    D3DSURFACE_DESC d{};
    if (FAILED(s->GetDesc(&d))) return none;
    SurfaceInfo& e = g_surfaces[s];
    if (e.id && std::memcmp(&e.desc, &d, sizeof(d)) == 0) return e;
    e.desc = d;
    e.id = reinterpret_cast<Id>(s);
    ComPtr<IDirect3DTexture9> tex;
    if (SUCCEEDED(s->GetContainer(IID_PPV_ARGS(&tex))) && tex) e.id = reinterpret_cast<Id>(tex.Get());
    e.info.width = d.Width;
    e.info.height = d.Height;
    e.info.format = static_cast<uint32_t>(d.Format);
    e.info.samples = d.MultiSampleType >= D3DMULTISAMPLE_2_SAMPLES ? static_cast<uint32_t>(d.MultiSampleType) : 1u;
    e.info.depth = (d.Usage & D3DUSAGE_DEPTHSTENCIL) != 0;
    if (g_surfaces.size() > 4096) { // stale entries from games that churn surfaces
        const SurfaceInfo keep = e;
        g_surfaces.clear();
        return g_surfaces[s] = keep;
    }
    return e;
}

// Runs the fire function outside the lock. Call with the lock released.
void Fire(IDirect3DDevice9* dev, int where)
{
    const FireFn f = g_fire.load(std::memory_order_acquire);
    if (!f) return;
    t_in_fire = true;
    f(dev, where);
    t_in_fire = false;
}

// Re-reads the state a state block or Reset may have changed behind our back.
void Resync(IDirect3DDevice9* dev)
{
    ComPtr<IDirect3DSurface9> rt, ds;
    dev->GetRenderTarget(0, &rt);
    dev->GetDepthStencilSurface(&ds);
    DWORD z = D3DZB_TRUE;
    dev->GetRenderState(D3DRS_ZENABLE, &z);
    ComPtr<IDirect3DVertexShader9> vs;
    dev->GetVertexShader(&vs);
    float regs[kVsRegisters * 4];
    const bool have_regs = SUCCEEDED(dev->GetVertexShaderConstantF(0, regs, kVsRegisters));
    D3DMATRIX v{}, p{};
    const bool have_view = SUCCEEDED(dev->GetTransform(D3DTS_VIEW, &v));
    const bool have_proj = SUCCEEDED(dev->GetTransform(D3DTS_PROJECTION, &p));
    std::lock_guard<std::mutex> l(g_mu);
    const SurfaceInfo& r = Describe(rt.Get());
    g_ears.SetRenderTarget0(r.id, r.info);
    const SurfaceInfo& d = Describe(ds.Get());
    g_ears.SetDepth(d.id, d.info);
    g_ears.SetZEnable(z != D3DZB_FALSE);
    g_ears.SetVertexShader(vs != nullptr);
    if (have_regs) g_ears.SetVsConstants(0, regs, kVsRegisters);
    if (have_view) g_ears.SetTransform(D3DTS_VIEW, &v._11);
    if (have_proj) g_ears.SetTransform(D3DTS_PROJECTION, &p._11);
}

// ------------------------------------------------------------------ detours
#define NA_ORIG(method, F) const auto orig = method.Original<F>(self); if (!orig) return D3DERR_INVALIDCALL

HRESULT WINAPI SetRenderTarget_h(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
HRESULT WINAPI SetDepthStencilSurface_h(IDirect3DDevice9*, IDirect3DSurface9*);
HRESULT WINAPI Clear_h(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
HRESULT WINAPI SetTransform_h(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);
HRESULT WINAPI SetRenderState_h(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
HRESULT WINAPI SetTexture_h(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
HRESULT WINAPI StretchRect_h(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);
HRESULT WINAPI GetRenderTargetData_h(IDirect3DDevice9*, IDirect3DSurface9*, IDirect3DSurface9*);
HRESULT WINAPI DrawPrimitive_h(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
HRESULT WINAPI DrawIndexedPrimitive_h(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
HRESULT WINAPI DrawPrimitiveUP_h(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
HRESULT WINAPI DrawIndexedPrimitiveUP_h(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);
HRESULT WINAPI SetVertexShader_h(IDirect3DDevice9*, IDirect3DVertexShader9*);
HRESULT WINAPI SetVertexShaderConstantF_h(IDirect3DDevice9*, UINT, const float*, UINT);
HRESULT WINAPI Present_h(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
HRESULT WINAPI PresentEx_h(IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
HRESULT WINAPI SwapChainPresent_h(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
HRESULT WINAPI StateBlockApply_h(IDirect3DStateBlock9*);

// IDirect3DDevice9 vtable slots (d3d9.h order).
Method m_set_rt{37, reinterpret_cast<void*>(&SetRenderTarget_h)};
Method m_set_ds{39, reinterpret_cast<void*>(&SetDepthStencilSurface_h)};
Method m_clear{43, reinterpret_cast<void*>(&Clear_h)};
Method m_set_transform{44, reinterpret_cast<void*>(&SetTransform_h)};
Method m_set_rs{57, reinterpret_cast<void*>(&SetRenderState_h)};
Method m_set_texture{65, reinterpret_cast<void*>(&SetTexture_h)};
Method m_stretch{34, reinterpret_cast<void*>(&StretchRect_h)};
Method m_get_rtdata{32, reinterpret_cast<void*>(&GetRenderTargetData_h)};
Method m_draw{81, reinterpret_cast<void*>(&DrawPrimitive_h)};
Method m_draw_indexed{82, reinterpret_cast<void*>(&DrawIndexedPrimitive_h)};
Method m_draw_up{83, reinterpret_cast<void*>(&DrawPrimitiveUP_h)};
Method m_draw_indexed_up{84, reinterpret_cast<void*>(&DrawIndexedPrimitiveUP_h)};
Method m_set_vs{92, reinterpret_cast<void*>(&SetVertexShader_h)};
Method m_set_vsc{94, reinterpret_cast<void*>(&SetVertexShaderConstantF_h)};
Method m_present{17, reinterpret_cast<void*>(&Present_h)};
Method m_present_ex{121, reinterpret_cast<void*>(&PresentEx_h)};
Method m_sc_present{3, reinterpret_cast<void*>(&SwapChainPresent_h)};
Method m_sb_apply{5, reinterpret_cast<void*>(&StateBlockApply_h)};

HRESULT WINAPI SetRenderTarget_h(IDirect3DDevice9* self, DWORD index, IDirect3DSurface9* s)
{
    using F = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
    NA_ORIG(m_set_rt, F);
    const HRESULT hr = orig(self, index, s);
    if (index == 0 && SUCCEEDED(hr) && Watched(self)) {
        std::lock_guard<std::mutex> l(g_mu);
        const SurfaceInfo& r = Describe(s);
        g_ears.SetRenderTarget0(r.id, r.info);
    }
    return hr;
}

HRESULT WINAPI SetDepthStencilSurface_h(IDirect3DDevice9* self, IDirect3DSurface9* s)
{
    using F = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DSurface9*);
    NA_ORIG(m_set_ds, F);
    const HRESULT hr = orig(self, s);
    if (SUCCEEDED(hr) && Watched(self)) {
        std::lock_guard<std::mutex> l(g_mu);
        const SurfaceInfo& d = Describe(s);
        g_ears.SetDepth(d.id, d.info);
    }
    return hr;
}

HRESULT WINAPI Clear_h(IDirect3DDevice9* self, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR c, float z, DWORD stencil)
{
    using F = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
    NA_ORIG(m_clear, F);
    if ((flags & D3DCLEAR_ZBUFFER) && Watched(self)) {
        std::lock_guard<std::mutex> l(g_mu);
        g_ears.ClearDepth();
    }
    return orig(self, count, rects, flags, c, z, stencil);
}

HRESULT WINAPI SetTransform_h(IDirect3DDevice9* self, D3DTRANSFORMSTATETYPE state, const D3DMATRIX* m)
{
    using F = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);
    NA_ORIG(m_set_transform, F);
    if ((state == D3DTS_VIEW || state == D3DTS_PROJECTION) && m && Watched(self)) {
        std::lock_guard<std::mutex> l(g_mu);
        g_ears.SetTransform(static_cast<uint32_t>(state), &m->_11);
    }
    return orig(self, state, m);
}

HRESULT WINAPI SetRenderState_h(IDirect3DDevice9* self, D3DRENDERSTATETYPE state, DWORD value)
{
    using F = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
    NA_ORIG(m_set_rs, F);
    if (state == D3DRS_ZENABLE && Watched(self)) {
        std::lock_guard<std::mutex> l(g_mu);
        g_ears.SetZEnable(value != D3DZB_FALSE);
    }
    return orig(self, state, value);
}

// A read of `id`: fires first when it is the learned point.
void ReadOf(IDirect3DDevice9* dev, Id id)
{
    bool fire = false;
    {
        std::lock_guard<std::mutex> l(g_mu);
        if (g_ears.BeforeRead(id)) {
            g_ears.MarkFired(FirePoint::BeforeRead);
            fire = true;
        }
        g_ears.Read(id);
    }
    if (fire) Fire(dev, 1);
}

HRESULT WINAPI SetTexture_h(IDirect3DDevice9* self, DWORD stage, IDirect3DBaseTexture9* tex)
{
    using F = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
    NA_ORIG(m_set_texture, F);
    if (tex && Watched(self)) ReadOf(self, reinterpret_cast<Id>(tex));
    return orig(self, stage, tex);
}

HRESULT WINAPI StretchRect_h(IDirect3DDevice9* self, IDirect3DSurface9* src, const RECT* sr, IDirect3DSurface9* dst, const RECT* dr,
                             D3DTEXTUREFILTERTYPE filter)
{
    using F = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);
    NA_ORIG(m_stretch, F);
    if (src && Watched(self)) {
        Id id;
        {
            std::lock_guard<std::mutex> l(g_mu);
            id = Describe(src).id;
        }
        ReadOf(self, id);
    }
    return orig(self, src, sr, dst, dr, filter);
}

HRESULT WINAPI GetRenderTargetData_h(IDirect3DDevice9* self, IDirect3DSurface9* rt, IDirect3DSurface9* dst)
{
    using F = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DSurface9*, IDirect3DSurface9*);
    NA_ORIG(m_get_rtdata, F);
    if (rt && Watched(self)) {
        Id id;
        {
            std::lock_guard<std::mutex> l(g_mu);
            id = Describe(rt).id;
        }
        ReadOf(self, id);
    }
    return orig(self, rt, dst);
}

// A draw of `vertices`: fires first at the HUD-rule point.
void DrawOf(IDirect3DDevice9* dev, uint64_t vertices)
{
    bool fire = false;
    {
        std::lock_guard<std::mutex> l(g_mu);
        if (g_ears.BeforeDraw(vertices)) {
            g_ears.MarkFired(FirePoint::BeforeHudDraw);
            fire = true;
        }
        g_ears.Draw(vertices);
    }
    if (fire) Fire(dev, 2);
}

HRESULT WINAPI DrawPrimitive_h(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, UINT start, UINT count)
{
    using F = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
    NA_ORIG(m_draw, F);
    if (Watched(self)) DrawOf(self, Vertices(static_cast<uint32_t>(type), count));
    return orig(self, type, start, count);
}

HRESULT WINAPI DrawIndexedPrimitive_h(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, INT base, UINT min_index, UINT num_vertices,
                                      UINT start_index, UINT prim_count)
{
    using F = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
    NA_ORIG(m_draw_indexed, F);
    if (Watched(self)) DrawOf(self, Vertices(static_cast<uint32_t>(type), prim_count));
    return orig(self, type, base, min_index, num_vertices, start_index, prim_count);
}

HRESULT WINAPI DrawPrimitiveUP_h(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride)
{
    using F = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
    NA_ORIG(m_draw_up, F);
    if (Watched(self)) DrawOf(self, Vertices(static_cast<uint32_t>(type), count));
    return orig(self, type, count, data, stride);
}

HRESULT WINAPI DrawIndexedPrimitiveUP_h(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, UINT min_index, UINT num_vertices, UINT prim_count,
                                        const void* idx, D3DFORMAT idx_format, const void* vtx, UINT stride)
{
    using F = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);
    NA_ORIG(m_draw_indexed_up, F);
    if (Watched(self)) DrawOf(self, Vertices(static_cast<uint32_t>(type), prim_count));
    return orig(self, type, min_index, num_vertices, prim_count, idx, idx_format, vtx, stride);
}

HRESULT WINAPI SetVertexShader_h(IDirect3DDevice9* self, IDirect3DVertexShader9* vs)
{
    using F = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DVertexShader9*);
    NA_ORIG(m_set_vs, F);
    if (Watched(self)) {
        std::lock_guard<std::mutex> l(g_mu);
        g_ears.SetVertexShader(vs != nullptr);
    }
    return orig(self, vs);
}

HRESULT WINAPI SetVertexShaderConstantF_h(IDirect3DDevice9* self, UINT start, const float* data, UINT count)
{
    using F = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, const float*, UINT);
    NA_ORIG(m_set_vsc, F);
    if (data && Watched(self)) {
        std::lock_guard<std::mutex> l(g_mu);
        g_ears.SetVsConstants(start, data, count);
    }
    return orig(self, start, data, count);
}

void PublishLocked()
{
    const auto& sel = g_ears.Model().Current();
    const auto& ms = g_ears.Model().GetStats();
    const auto& es = g_ears.Stats();
    const Camera& cam = g_ears.LastCamera();
    g_stats.frames = static_cast<uint32_t>(es.frames);
    g_stats.selected = sel.valid() ? 1u : 0u;
    g_stats.fired_read = static_cast<uint32_t>(es.fired_read);
    g_stats.fired_hud = static_cast<uint32_t>(es.fired_hud);
    g_stats.fired_present = static_cast<uint32_t>(es.fired_present);
    g_stats.camera_found = cam.found ? 1u : 0u;
    g_stats.camera_source = static_cast<uint32_t>(cam.source);
    g_stats.camera_register = cam.vp_register;
    g_stats.point_kind = static_cast<uint32_t>(g_ears.Model().LearnedPointKind());
    g_stats.scene_draws = ms.last_scene_draws;
    g_stats.draws = ms.last_draws;
    g_stats.color_w = sel.color_info.width;
    g_stats.color_h = sel.color_info.height;
    g_stats.color_format = sel.color_info.format;
    g_stats.depth_format = sel.depth_info.format;
}

// End of a frame: the Present fallback point, then the ears learn.
void FrameEnd(IDirect3DDevice9* dev)
{
    bool fire = false;
    {
        std::lock_guard<std::mutex> l(g_mu);
        if (g_ears.PresentShouldFire()) {
            g_ears.MarkFired(FirePoint::AtPresent);
            fire = true;
        }
    }
    if (fire) Fire(dev, 3);
    std::lock_guard<std::mutex> l(g_mu);
    g_ears.Present();
    PublishLocked();
    const auto& sel = g_ears.Model().Current();
    if (sel.color != g_last_color || sel.depth != g_last_depth) {
        g_last_color = sel.color;
        g_last_depth = sel.depth;
        Log("frame %u: scene colour %ux%u fmt %u, depth fmt %u%s", g_stats.frames, sel.color_info.width, sel.color_info.height,
            sel.color_info.format, sel.depth_info.format, sel.valid() ? "" : " (none)");
    }
    if (g_stats.frames == 30 || g_stats.frames % 600 == 0) {
        const auto& ms = g_ears.Model().GetStats();
        Log("frame %u: point %s, fired read %u hud %u present %u, missed %llu contradictions %llu, camera %s c%u, scene draws %u of %u",
            g_stats.frames, g_stats.point_kind == 1 ? "before the scene read" : g_stats.point_kind == 2 ? "before the HUD" : "Present",
            g_stats.fired_read, g_stats.fired_hud, g_stats.fired_present, static_cast<unsigned long long>(ms.missed),
            static_cast<unsigned long long>(ms.contradictions),
            g_stats.camera_source == 1 ? "shader constants" : g_stats.camera_source == 2 ? "fixed function" : "none", g_stats.camera_register,
            g_stats.scene_draws, g_stats.draws);
    }
}

HRESULT WINAPI Present_h(IDirect3DDevice9* self, const RECT* sr, const RECT* dr, HWND w, const RGNDATA* dirty)
{
    using F = HRESULT(WINAPI*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
    NA_ORIG(m_present, F);
    if (Watched(self)) FrameEnd(self);
    return orig(self, sr, dr, w, dirty);
}

HRESULT WINAPI PresentEx_h(IDirect3DDevice9Ex* self, const RECT* sr, const RECT* dr, HWND w, const RGNDATA* dirty, DWORD flags)
{
    using F = HRESULT(WINAPI*)(IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
    NA_ORIG(m_present_ex, F);
    if (Watched(self)) FrameEnd(self);
    return orig(self, sr, dr, w, dirty, flags);
}

HRESULT WINAPI SwapChainPresent_h(IDirect3DSwapChain9* self, const RECT* sr, const RECT* dr, HWND w, const RGNDATA* dirty, DWORD flags)
{
    using F = HRESULT(WINAPI*)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
    NA_ORIG(m_sc_present, F);
    if (!t_in_fire) {
        ComPtr<IDirect3DDevice9> dev;
        if (SUCCEEDED(self->GetDevice(&dev)) && Watched(dev.Get())) {
            // Only the implicit swap chain ends the frame (extra swap chains are tool windows).
            ComPtr<IDirect3DSwapChain9> implicit;
            if (SUCCEEDED(dev->GetSwapChain(0, &implicit)) && implicit.Get() == self) FrameEnd(dev.Get());
        }
    }
    return orig(self, sr, dr, w, dirty, flags);
}

HRESULT WINAPI StateBlockApply_h(IDirect3DStateBlock9* self)
{
    using F = HRESULT(WINAPI*)(IDirect3DStateBlock9*);
    NA_ORIG(m_sb_apply, F);
    const HRESULT hr = orig(self);
    if (SUCCEEDED(hr) && !t_in_fire) {
        ComPtr<IDirect3DDevice9> dev;
        if (SUCCEEDED(self->GetDevice(&dev)) && Watched(dev.Get())) Resync(dev.Get());
    }
    return hr;
}

void SetSwapSize(const D3DPRESENT_PARAMETERS* pp)
{
    if (!pp) return;
    std::lock_guard<std::mutex> l(g_mu);
    g_ears.SetSwapChainSize(pp->BackBufferWidth, pp->BackBufferHeight);
}

} // namespace

void AttachDevice(IDirect3DDevice9* dev, const D3DPRESENT_PARAMETERS* pp)
{
    if (!dev) return;
    int failed = 0;
    for (Method* m : {&m_set_rt, &m_set_ds, &m_clear, &m_set_transform, &m_set_rs, &m_set_texture, &m_stretch, &m_get_rtdata, &m_draw,
                      &m_draw_indexed, &m_draw_up, &m_draw_indexed_up, &m_set_vs, &m_set_vsc, &m_present})
        if (!HookMethod(*m, dev)) ++failed;
    ComPtr<IDirect3DDevice9Ex> ex;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&ex))) && ex && !HookMethod(m_present_ex, ex.Get())) ++failed;
    ComPtr<IDirect3DSwapChain9> sc;
    if (SUCCEEDED(dev->GetSwapChain(0, &sc)) && sc && !HookMethod(m_sc_present, sc.Get())) ++failed;
    ComPtr<IDirect3DStateBlock9> sb;
    if (SUCCEEDED(dev->CreateStateBlock(D3DSBT_PIXELSTATE, &sb)) && sb && !HookMethod(m_sb_apply, sb.Get())) ++failed;
    if (failed) Log("%d device hook(s) failed", failed);
    {
        std::lock_guard<std::mutex> l(g_mu);
        g_ears.Reset();
        g_surfaces.clear();
        g_last_color = g_last_depth = 0;
    }
    g_dev.store(dev, std::memory_order_release);
    SetSwapSize(pp);
    Resync(dev);
    Log("watching device %p (%ux%u)", static_cast<void*>(dev), pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0);
}

void BeforeReset(IDirect3DDevice9* dev)
{
    if (!Watched(dev)) return;
    std::lock_guard<std::mutex> l(g_mu);
    g_ears.Reset();
    g_surfaces.clear();
    g_last_color = g_last_depth = 0;
}

void AfterReset(IDirect3DDevice9* dev, const D3DPRESENT_PARAMETERS* pp, HRESULT hr)
{
    if (!Watched(dev) || FAILED(hr)) return;
    SetSwapSize(pp);
    Resync(dev);
}

HookStats GetHookStats()
{
    std::lock_guard<std::mutex> l(g_mu);
    return g_stats;
}

void SetFire(FireFn fn) { g_fire.store(fn, std::memory_order_release); }

} // namespace na::dx9
