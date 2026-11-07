// DX9 device hooks (DX9 plan step D3): feed the game's DX9 calls to the ears
// (dx9_ears.hpp) and run DLSS at the point they find. Windows only.
#pragma once

#include <d3d9.h>

#include <cstdint>

namespace na::dx9 {

struct HookStats {
    uint32_t frames = 0, selected = 0;      // frames seen; 1 = scene colour + depth picked now
    uint32_t fired_read = 0, fired_hud = 0, fired_present = 0;
    uint32_t camera_found = 0, camera_source = 0, camera_register = 0; // source: 1 shader constants, 2 fixed function
    uint32_t point_kind = 0;                // 1 before a read, 2 before the HUD (HUD rule), 0 none (Present)
    uint32_t scene_draws = 0, draws = 0;    // last frame
    uint32_t color_w = 0, color_h = 0, color_format = 0, depth_format = 0;
    // D5 jitter (this frame's, published at its start).
    uint32_t jitter_on = 0, jitter_register = 0; // the constant register the patched shaders read it from
    float jitter_x = 0, jitter_y = 0;            // render pixels, picture moved right/down by this much
    float mip_bias = 0;                          // added to the game's own D3DSAMP_MIPMAPLODBIAS
    uint32_t shaders_patched = 0, shaders_unpatched = 0;
    uint32_t jittered_draws = 0, unjittered_scene_draws = 0; // last frame
};

// D5: camera jitter for DLSS. mode 0 = off (shaders are not even patched),
// 1 = on while DLSS delivers results, 2 = always (test). Set before AttachDevice.
struct JitterConfig {
    uint32_t mode = 1;
    uint32_t phases = 8;
    float mip_bias = -1.f; // while jittering
};
void SetJitterConfig(const JitterConfig& c);
// The transport says whether DLSS is delivering (mode 1 jitters only then).
void SetDlssLive(bool live);

// Called by the proxy: a device was created (hooks its methods, starts watching it).
void AttachDevice(IDirect3DDevice9* dev, const D3DPRESENT_PARAMETERS* pp);
// Around IDirect3DDevice9::Reset(Ex).
void BeforeReset(IDirect3DDevice9* dev);
void AfterReset(IDirect3DDevice9* dev, const D3DPRESENT_PARAMETERS* pp, HRESULT hr);
HookStats GetHookStats();

// What the DLSS point hands over: the scene colour and depth (a texture or a
// surface each; alive at this moment, not AddRef'd) and their size.
struct FireInfo {
    IDirect3DResource9* color = nullptr;
    IDirect3DResource9* depth = nullptr;
    uint32_t width = 0, height = 0, format = 0; // format: the colour's D3DFORMAT
    uint64_t frame = 0;
    float jitter_x = 0, jitter_y = 0; // the jitter this frame's scene was drawn with (render pixels)
    // D6: the camera the scene was drawn with (unjittered: the jitter is added in
    // the shader, never to the game's matrices), column-vector ViewProjection.
    bool camera = false, depth_reversed = false;
    double vp[16]{};
};

// What runs at the DLSS point (D4 sets it to the transport). Called on the
// game's render thread with the device, before the call it precedes.
// where: 1 before a read, 2 before the HUD, 3 at Present.
using FireFn = void (*)(IDirect3DDevice9* dev, int where, const FireInfo& info);
void SetFire(FireFn fn);

} // namespace na::dx9
