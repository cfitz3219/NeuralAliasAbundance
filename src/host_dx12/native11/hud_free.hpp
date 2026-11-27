// The HUD-free picture on the native DX11 route: the same two variants as on DX12 (j2/live_dlaa.cpp),
// learned the same way (core/live_frame_gen.hpp ChooseHudVariant) from one recorded frame:
//  - hudfree/sheet-redraw, a separate HUD sheet composited by one final draw into the shown picture: at the sheet's
//    first draw of the frame its contents are copied (pre-HUD); at the final draw the sheet's contents
//    are swapped for that copy, the same draw is issued once more into our picture, and the game's
//    render targets and sheet are put back before the game's own draw runs.
//  - hudfree/snapshot-before-interface, the interface drawn straight onto the shown picture: right before its first interface draw
//    (the learned k-th draw into it in the frame), the shown picture is copied into ours.
// Everything runs on the game's immediate context in order, so no state needs to be proven: D3D11
// tracks resource states itself. Deferred contexts are not followed draw by draw: the provider
// reports each command list the immediate context runs with every colour target its recording
// wrote (OnExecuteList). A list that writes the shown picture where the copy point would bake it
// in (between the scene and the first interface draw), writes the sheet before the final draw, or
// whose writes are not known, makes learning refuse with that plain reason; on a learned frame it
// makes that frame's picture unusable (real frames only for that frame).
//
// The provider (native11/provider.cpp) calls BeforeDraw for every immediate-context draw, BeforeCopy
// for copies, and OnPresent at the game's Present; nothing runs while the picture is not wanted.
#pragma once

#include <d3d11.h>
#include <d3d12.h>
#include <cstdint>

#include "host_dx12/native11/draw_replay.hpp"

namespace na::native11::hud_free {

// Wanted while x2 is on (and in tests). Learning starts at the next frame.
void SetWanted(bool on);
bool Wanted();

struct DrawFacts {
    uint64_t rt0 = 0;            // the first bound render target (identity)
    uint64_t shown = 0;          // the swap chain's back buffer (identity)
    uint32_t rt_w = 0, rt_h = 0; // its size
    uint32_t rt_count = 0;
    bool depth = false;          // a depth buffer is bound and tested
};
using IssueDraw = void (*)(ID3D11DeviceContext* c, const draw_replay::DrawArgs& a, void* user);
// Before the game's draw is forwarded (immediate context only). `issue` draws with the original
// (unhooked) entry point; used for hudfree/sheet-redraw's second draw.
void BeforeDraw(ID3D11DeviceContext* c, const DrawFacts& f, const draw_replay::DrawArgs& a, IssueDraw issue, void* user);
// A copy into `dst` (identity) of `w` x `h` on the immediate context (learning only).
void BeforeCopy(uint64_t dst, uint32_t w, uint32_t h);
// The immediate context runs a command list recorded on a deferred context. `writes`: every colour
// target (identity) its recording wrote by draws, clears and copies; known=false: not known (the
// list was recorded before the provider was installed, or wrote too many targets to list).
void OnExecuteList(const uint64_t* writes, size_t count, bool known);
// At the game's Present: ends the frame (learning, per-frame counts, relearning).
void OnPresent(ID3D11DeviceContext* immediate, uint64_t shown, uint32_t shown_w, uint32_t shown_h);

// The HUD-free picture of the frame now being presented (same size; the back buffer's format family),
// or null with `why` in plain words. Valid until the next Present.
ID3D11Texture2D* Picture(const char** why);
// Live x2 on a DX11 proxy chain: the picture is made as an image shared with this D3D12 device (the
// presentation queue's), and the presenter reads it as a D3D12 resource (COMMON at rest; written by
// D3D11 before the frame's Present, ordered by the proxy's shared fences). Null device: D3D11 only.
void SetEffectsDevice(ID3D12Device* device);
ID3D12Resource* EffectsPicture(const char** why);

// F8 on the native DX11 route: for the next `frames` frames, the shown picture is saved just before
// each of its first 4 draws ("pre-interface" candidates, whatever shape the frame has) and, when a
// variant is learned, the HUD-free picture itself. Files go to `dir` (the capture's folder):
// pre-interface-<frame>-before-draw<k>.raw / -hudfree.raw (tight rows) and pre-interface.json (one
// line per picture: frame, kind, size, format, the learned method). The pictures are read back from
// staging copies a frame or more later (never waiting on the GPU). While capturing, learning runs too.
void CaptureFrames(const wchar_t* dir, uint32_t frames);

struct Stats {
    uint32_t wanted = 0, learned = 0, variant = 0; // na::fglive::HudVariant
    uint64_t learn_attempts = 0, pictures = 0, skipped = 0, relearns = 0;
    char verdict[200] = "";
    char method[128] = "";
    char last_skip[112] = "";
};
Stats GetStats();
int FormatStatus(char* out, size_t capacity);
void Reset(); // device change / tests: forget everything (GPU objects released)

} // namespace na::native11::hud_free
