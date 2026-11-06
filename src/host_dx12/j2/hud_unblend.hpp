// HUD by un-blending (render-small, 2026-11-05). A different route from native_hud's
// hand-off tracking, which never engaged in Sniper Elite 5 (the canvas reaches the back
// buffer through a draw whose input we cannot see).
//
// Current-picture ownership and validation:
//   * Every UI-looking draw (render-size SDR target, not a full-screen pass, not bound to the
//     scene camera or depth) is left exactly as the game made it AND replayed four times:
//     over black and over grey at the game's own size (L0, L1), and over black and over grey
//     at the screen size with the viewport and scissor scaled (N0, N1). Whatever the game
//     blends, out = N0 + dst * (N1 - N0) / grey is its exact effect on a destination.
//   * Each render target owns its layers. An observed whole-image copy freezes and transfers
//     them; recording order or a similar size cannot make another target the presented HUD.
//   * Snapshot the actual pre-HUD scene S, including pixels hidden behind opaque text. GPU
//     validation checks BOTH the completed picture and P = L0 + m*S, per pixel, in this frame.
//     DLSS gets S where that relationship holds; elsewhere the original current picture is
//     protected. Native layers compose after reconstruction. A stale multi-frame diagnostic
//     verdict cannot alternately switch the whole image on and off.
//   * Existing recording leases retain every snapshot through actual queue submission and
//     GPU retirement. A retained Present owner also prevents pool reuse.
// What this cannot do: text the game rasterised into an atlas at render size stays at that
// detail (it is magnified cleanly, not reconstructed); draws that depend on the pixel
// position (full-screen effects) are left in the picture on purpose.
#pragma once
#include <d3d12.h>
#include <cstdint>
#include <string>
#include "host_dx12/observe/shader_operation.hpp"

namespace na::j2::native_hud { struct Layer; }
namespace na::observe { struct TextureCall; }
namespace na::j2::hud_unblend {
void SetConfigured(bool on); // boot ([present] hud_unblend=1): takes over native_hud's hooks
bool Configured();
void SetEnabled(bool on);    // live switch
bool Enabled();              // configured and on
// native_hud's hooks call these while Enabled() (same observer seams, nothing installed twice).
void OnBind(ID3D12GraphicsCommandList* l, uint64_t recording);
na::observe::ShaderScope OnDraw(ID3D12GraphicsCommandList* l, const na::observe::ShaderCall& call);
void OnClear(ID3D12GraphicsCommandList* l, uint64_t target, const float color[4]);
void OnTexture(ID3D12GraphicsCommandList* l, const na::observe::TextureCall&);
void OnClosing(ID3D12GraphicsCommandList* l);
// Present (render-small): the layers for the frame that ends now. True when UI draws were
// replayed this frame; out->compose says whether the self-check allows laying them on.
bool TakeFrame(ID3D12Resource* frame, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, native_hud::Layer* out,
               ID3D12CommandQueue* queue = nullptr);
// Completed GPU self-check of one frame (PresentUpscale, after its fence): UI pixels, pixels
// the UI covers fully, and of those how many equal L0 as stored / sRGB-decoded / sRGB-encoded.
void ReportCheck(uint32_t ui_px, uint32_t opaque, uint32_t match_id, uint32_t match_dec, uint32_t match_enc);

struct Stats {
    uint64_t frames = 0, hud_frames = 0, composed = 0, held = 0;
    uint64_t replayed = 0, blended = 0, forwarded = 0, world = 0, scene = 0, covered = 0, cover_blend = 0, restore_failed = 0;
    uint64_t opaque_blend = 0, irreversible = 0; // full-screen draws with blending on but DestBlend ZERO; draws whose blend cannot be undone
    uint64_t other_target = 0, retargeted = 0; // historical counters; association no longer uses this heuristic
    uint64_t copies = 0, copied_layers = 0, unsupported_copies = 0;
    uint64_t requested_target = 0, matched_target = 0;
    uint32_t pending_recordings = 0;
    bool current_picture = false;
    uint64_t cleared_after = 0, view_changed = 0, sets_missed = 0;
    uint32_t last_draws = 0, last_view = 0, last_covered = 0;
    char last_forward[64] = {};
    // Self-check: checked frames; sums over the current window and the last verdict.
    uint64_t check_frames = 0, ui_px = 0, opaque_px = 0, match_id = 0, match_dec = 0, match_enc = 0;
    uint32_t rate_id = 0, rate_dec = 0, rate_enc = 0; // permille of the last decided window
    uint32_t verdict = 0; // 0 undecided (composing), 1 identity confirmed, 2 mismatch (holding)
};
Stats GetStats();
std::string StatusLine();
} // namespace na::j2::hud_unblend
