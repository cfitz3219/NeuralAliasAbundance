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

#include "core/temporal_jitter.hpp"
#include "host_dx9/dx9_ears.hpp"
#include "host_dx9/vs_patch.hpp"
#include "host_dx9/vtable_hook.hpp"

#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

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

// ---- D5 jitter, all under g_mu.
// Patched vertex shaders read the jitter from constant register g_cj (see
// vs_patch.hpp); before each draw the device's cJ is set to the jitter (scene
// draw), zero (other draws) or the game's own value (unpatched shader), only
// when it changes. Fixed-function scene draws get a jittered projection the
// same way. The mip bias is added to the game's own on all 16 samplers.
JitterConfig g_jcfg;
std::atomic<bool> g_dlss_live{false};
uint32_t g_cj = 0;                       // 0 = no patching
std::unordered_set<const void*> g_patched;
bool g_vs_bound = false, g_vs_patched = false;
bool g_jitter_on = false;                // this frame
float g_jx = 0, g_jy = 0;                // this frame
uint32_t g_jitter_index = 0;
float g_game_cj[4]{}, g_dev_cj[4]{};
bool g_dev_cj_known = false;
D3DMATRIX g_game_proj{}, g_dev_proj{};
bool g_have_game_proj = false, g_dev_proj_known = false;
constexpr DWORD kSamplers = 16;
float g_samp_game[kSamplers]{};
float g_bias_now = 0;
uint32_t g_frame_jittered = 0, g_frame_unjittered = 0;
uint32_t g_patch_logs = 0;

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

// What the fire function gets, taken under the lock when the point is reached.
FireInfo InfoLocked()
{
    FireInfo i;
    const auto& sel = g_ears.Model().Current();
    // Ids are the game's own texture/surface pointers (single inheritance from IDirect3DResource9).
    i.color = reinterpret_cast<IDirect3DResource9*>(static_cast<uintptr_t>(sel.color));
    i.depth = reinterpret_cast<IDirect3DResource9*>(static_cast<uintptr_t>(sel.depth));
    i.width = sel.color_info.width;
    i.height = sel.color_info.height;
    i.format = sel.color_info.format;
    i.frame = g_ears.Stats().frames;
    // The jitter DLSS is told: this frame's, when most scene draws so far carried it.
    if (g_jitter_on && g_frame_jittered > g_frame_unjittered) {
        i.jitter_x = g_jx;
        i.jitter_y = g_jy;
    }
    return i;
}

// Runs the fire function outside the lock. Call with the lock released.
void Fire(IDirect3DDevice9* dev, int where, const FireInfo& info)
{
    const FireFn f = g_fire.load(std::memory_order_acquire);
    if (!f || !info.color || !info.depth) return;
    t_in_fire = true;
    f(dev, where, info);
    t_in_fire = false;
}

void WriteBias(IDirect3DDevice9* dev);

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
    float bias[kSamplers]{};
    for (DWORD i = 0; i < kSamplers; ++i) {
        DWORD b = 0;
        dev->GetSamplerState(i, D3DSAMP_MIPMAPLODBIAS, &b);
        std::memcpy(&bias[i], &b, 4);
    }
    {
        std::lock_guard<std::mutex> l(g_mu);
        // What differs from what we last wrote is the game's own (a state block, or Reset's defaults).
        if (have_regs && g_cj && (!g_dev_cj_known || std::memcmp(&regs[g_cj * 4], g_dev_cj, 16) != 0))
            std::memcpy(g_game_cj, &regs[g_cj * 4], 16);
        if (have_proj && (!g_dev_proj_known || std::memcmp(&p, &g_dev_proj, sizeof(p)) != 0)) {
            g_game_proj = p;
            g_have_game_proj = true;
        }
        g_dev_cj_known = g_dev_proj_known = false;
        for (DWORD i = 0; i < kSamplers; ++i)
            if (std::fabs(bias[i] - (g_samp_game[i] + g_bias_now)) > 1e-6f) g_samp_game[i] = bias[i];
        g_vs_bound = vs != nullptr;
        g_vs_patched = vs && g_patched.count(vs.Get()) != 0;
    }
    WriteBias(dev);
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
HRESULT WINAPI CreateVertexShader_h(IDirect3DDevice9*, const DWORD*, IDirect3DVertexShader9**);
HRESULT WINAPI SetSamplerState_h(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);

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
Method m_create_vs{91, reinterpret_cast<void*>(&CreateVertexShader_h)};
Method m_set_sampler{69, reinterpret_cast<void*>(&SetSamplerState_h)};

using SetVscFn = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, const float*, UINT);
using SetTransformFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);
using SetSamplerFn = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);

// The game's mip bias plus ours on every sampler (through the original: the game never sees it).
void WriteBias(IDirect3DDevice9* dev)
{
    float v[kSamplers];
    {
        std::lock_guard<std::mutex> l(g_mu);
        for (DWORD i = 0; i < kSamplers; ++i) v[i] = g_samp_game[i] + g_bias_now;
    }
    const auto orig = m_set_sampler.Original<SetSamplerFn>(dev);
    if (!orig) return;
    for (DWORD i = 0; i < kSamplers; ++i) {
        DWORD b;
        std::memcpy(&b, &v[i], 4);
        orig(dev, i, D3DSAMP_MIPMAPLODBIAS, b);
    }
}

// What a draw needs set first: cJ, or the fixed-function projection.
struct DrawFix {
    bool cj = false, proj = false;
    uint32_t reg = 0;
    float v[4]{};
    D3DMATRIX m{};
};

DrawFix PlanDrawLocked()
{
    DrawFix f;
    if (!g_jcfg.mode) return f;
    const bool scene = g_ears.NextDrawIsScene();
    const bool jit = scene && g_jitter_on;
    const auto& ci = g_ears.Model().Current().color_info;
    const float dx = ci.width ? 2.f * g_jx / static_cast<float>(ci.width) : 0.f;
    const float dy = ci.height ? -2.f * g_jy / static_cast<float>(ci.height) : 0.f;
    if (g_vs_bound) {
        if (!g_cj) {
            if (jit) ++g_frame_unjittered;
            return f;
        }
        float want[4] = {0, 0, 0, 0};
        if (g_vs_patched) {
            if (jit) { want[0] = dx; want[1] = dy; ++g_frame_jittered; }
        } else {
            std::memcpy(want, g_game_cj, 16);
            if (jit) ++g_frame_unjittered;
        }
        if (!g_dev_cj_known || std::memcmp(want, g_dev_cj, 16) != 0) {
            std::memcpy(g_dev_cj, want, 16);
            g_dev_cj_known = true;
            f.cj = true;
            f.reg = g_cj;
            std::memcpy(f.v, want, 16);
        }
    } else if (g_have_game_proj) {
        // Row vectors: clip = v * P, so clip.x += dx * clip.w is column 0 += dx * column 3.
        D3DMATRIX want = g_game_proj;
        if (jit) {
            for (int r = 0; r < 4; ++r) {
                want.m[r][0] += dx * g_game_proj.m[r][3];
                want.m[r][1] += dy * g_game_proj.m[r][3];
            }
            ++g_frame_jittered;
        }
        if (!g_dev_proj_known || std::memcmp(&want, &g_dev_proj, sizeof(want)) != 0) {
            g_dev_proj = want;
            g_dev_proj_known = true;
            f.proj = true;
            f.m = want;
        }
    } else if (jit) {
        ++g_frame_unjittered;
    }
    return f;
}

void ApplyDrawFix(IDirect3DDevice9* dev, const DrawFix& f)
{
    if (f.cj)
        if (const auto o = m_set_vsc.Original<SetVscFn>(dev)) o(dev, f.reg, f.v, 1);
    if (f.proj)
        if (const auto o = m_set_transform.Original<SetTransformFn>(dev)) o(dev, D3DTS_PROJECTION, &f.m);
}

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
        if (state == D3DTS_PROJECTION) {
            g_game_proj = g_dev_proj = *m;
            g_have_game_proj = g_dev_proj_known = true;
        }
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
    FireInfo info;
    {
        std::lock_guard<std::mutex> l(g_mu);
        if (g_ears.BeforeRead(id)) {
            g_ears.MarkFired(FirePoint::BeforeRead);
            info = InfoLocked();
            fire = true;
        }
        g_ears.Read(id);
    }
    if (fire) Fire(dev, 1, info);
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
    FireInfo info;
    DrawFix f;
    {
        std::lock_guard<std::mutex> l(g_mu);
        if (g_ears.BeforeDraw(vertices)) {
            g_ears.MarkFired(FirePoint::BeforeHudDraw);
            info = InfoLocked();
            fire = true;
        }
        g_ears.Draw(vertices);
        f = PlanDrawLocked();
    }
    if (fire) Fire(dev, 2, info);
    ApplyDrawFix(dev, f);
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
        g_vs_bound = vs != nullptr;
        g_vs_patched = vs && g_patched.count(vs) != 0;
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
        if (g_cj && g_cj >= start && g_cj - start < count) { // the game's own value of cJ (for its unpatched shaders)
            std::memcpy(g_game_cj, data + (g_cj - start) * 4, 16);
            std::memcpy(g_dev_cj, g_game_cj, 16);
            g_dev_cj_known = true;
        }
    }
    return orig(self, start, data, count);
}

HRESULT WINAPI CreateVertexShader_h(IDirect3DDevice9* self, const DWORD* fn, IDirect3DVertexShader9** out)
{
    using F = HRESULT(WINAPI*)(IDirect3DDevice9*, const DWORD*, IDirect3DVertexShader9**);
    NA_ORIG(m_create_vs, F);
    uint32_t cj = 0;
    if (fn && out && Watched(self)) {
        std::lock_guard<std::mutex> l(g_mu);
        cj = g_cj;
    }
    if (!cj) return orig(self, fn, out);
    const VsPatch p = PatchVertexShader(reinterpret_cast<const uint32_t*>(fn), size_t(1) << 20, cj);
    if (p.ok) {
        const HRESULT hr = orig(self, reinterpret_cast<const DWORD*>(p.code.data()), out);
        if (SUCCEEDED(hr) && *out) {
            std::lock_guard<std::mutex> l(g_mu);
            g_patched.insert(*out);
            ++g_stats.shaders_patched;
            if (p.relative_constants && g_patch_logs < 8) {
                ++g_patch_logs;
                Log("vs_%u_%u patched (r%u); it indexes constants with a0, c%u assumed out of its range", p.major, p.minor, p.temp, cj);
            }
            return hr;
        }
        std::lock_guard<std::mutex> l(g_mu);
        if (g_patch_logs < 8) { ++g_patch_logs; Log("vs_%u_%u: the runtime refused the patched shader (0x%08lx), original kept", p.major, p.minor, static_cast<unsigned long>(hr)); }
    } else {
        std::lock_guard<std::mutex> l(g_mu);
        if (g_patch_logs < 8) { ++g_patch_logs; Log("vertex shader not patched (no jitter on its draws): %s", p.why); }
    }
    const HRESULT hr = orig(self, fn, out);
    if (SUCCEEDED(hr) && *out) {
        std::lock_guard<std::mutex> l(g_mu);
        g_patched.erase(*out); // a freed patched shader's address can come back
        ++g_stats.shaders_unpatched;
    }
    return hr;
}

HRESULT WINAPI SetSamplerState_h(IDirect3DDevice9* self, DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value)
{
    using F = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);
    NA_ORIG(m_set_sampler, F);
    if (type == D3DSAMP_MIPMAPLODBIAS && sampler < kSamplers && Watched(self)) {
        float v = 0;
        std::memcpy(&v, &value, 4);
        std::lock_guard<std::mutex> l(g_mu);
        g_samp_game[sampler] = v;
        v += g_bias_now;
        std::memcpy(&value, &v, 4);
    }
    return orig(self, sampler, type, value);
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
    g_stats.jitter_on = g_jitter_on ? 1u : 0u;
    g_stats.jitter_register = g_cj;
    g_stats.jitter_x = g_jx;
    g_stats.jitter_y = g_jy;
    g_stats.mip_bias = g_bias_now;
}

// The start of the next frame: is it jittered, and by how much. Returns
// whether the mip bias changed (the samplers are then rewritten).
bool NextFrameLocked()
{
    g_stats.jittered_draws = g_frame_jittered;
    g_stats.unjittered_scene_draws = g_frame_unjittered;
    g_frame_jittered = g_frame_unjittered = 0;
    const bool was = g_jitter_on;
    g_jitter_on = g_jcfg.mode == 2 || (g_jcfg.mode == 1 && g_dlss_live.load(std::memory_order_acquire));
    if (g_jitter_on) {
        const auto js = neural_alias::temporal_jitter(g_jitter_index++, std::clamp<uint32_t>(g_jcfg.phases, 2u, 64u));
        g_jx = static_cast<float>(js.x_pixels);
        g_jy = static_cast<float>(js.y_pixels);
    } else {
        g_jx = g_jy = 0;
    }
    if (was != g_jitter_on)
        Log("frame %u: jitter %s", g_stats.frames, g_jitter_on ? "on" : "off (DLSS is not delivering)");
    const float bias = g_jitter_on ? g_jcfg.mip_bias : 0.f;
    if (bias == g_bias_now) return false;
    g_bias_now = bias;
    return true;
}

// End of a frame: the Present fallback point, then the ears learn.
void FrameEnd(IDirect3DDevice9* dev)
{
    bool fire = false;
    FireInfo info;
    {
        std::lock_guard<std::mutex> l(g_mu);
        if (g_ears.PresentShouldFire()) {
            g_ears.MarkFired(FirePoint::AtPresent);
            info = InfoLocked();
            fire = true;
        }
    }
    if (fire) Fire(dev, 3, info);
    std::unique_lock<std::mutex> l(g_mu);
    g_ears.Present();
    const bool bias_changed = NextFrameLocked();
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
        if (g_cj)
            Log("frame %u: jitter %s (%.3f, %.3f) via c%u, mip bias %.2f; shaders patched %u, unpatched %u; last frame %u draws jittered, %u scene draws not",
                g_stats.frames, g_jitter_on ? "on" : "off", g_jx, g_jy, g_cj, g_bias_now, g_stats.shaders_patched, g_stats.shaders_unpatched,
                g_stats.jittered_draws, g_stats.unjittered_scene_draws);
    }
    l.unlock();
    if (bias_changed) WriteBias(dev);
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
                      &m_draw_indexed, &m_draw_up, &m_draw_indexed_up, &m_set_vs, &m_set_vsc, &m_present, &m_create_vs, &m_set_sampler})
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
        // D5: the jitter register is the last constant the device has (a shader that reads it is left unpatched).
        D3DCAPS9 caps{};
        const DWORD consts = SUCCEEDED(dev->GetDeviceCaps(&caps)) ? std::min<DWORD>(caps.MaxVertexShaderConst, 256) : 0;
        g_cj = g_jcfg.mode && consts >= 16 ? consts - 1 : 0;
        g_patched.clear();
        g_vs_bound = g_vs_patched = false;
        g_jitter_on = false;
        g_jx = g_jy = 0;
        g_dev_cj_known = g_dev_proj_known = g_have_game_proj = false;
        std::memset(g_game_cj, 0, sizeof(g_game_cj));
        std::memset(g_samp_game, 0, sizeof(g_samp_game));
        g_bias_now = 0;
        g_frame_jittered = g_frame_unjittered = 0;
        g_stats.shaders_patched = g_stats.shaders_unpatched = 0;
        g_patch_logs = 0;
    }
    g_dev.store(dev, std::memory_order_release);
    SetSwapSize(pp);
    Resync(dev);
    Log("watching device %p (%ux%u); jitter mode %u, %u phases, mip bias %.2f, register c%u", static_cast<void*>(dev),
        pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0, g_jcfg.mode, g_jcfg.phases, g_jcfg.mip_bias, g_cj);
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

void SetJitterConfig(const JitterConfig& c)
{
    std::lock_guard<std::mutex> l(g_mu);
    g_jcfg = c;
}

void SetDlssLive(bool live) { g_dlss_live.store(live, std::memory_order_release); }

} // namespace na::dx9
