// Native-resolution HUD for render-small (P4-4). Sol's plan, built after her review of the
// pixel-mask build (docs/reviews/ANNO-HUD-a6f3f18-through-01653ff.md on astra/native-hud-review).
//
// In render-small the game draws its whole frame, HUD included, into a render-size proxy back
// buffer, and DLSS upscales that at Present. Upscaled HUD text looks soft, and the HUD was
// inside DLSS's input. Here the game's HUD draws are taken out of the proxy instead:
//
//   * Scene: draws into the proxy before the HUD starts that are opaque, full-screen shaped
//     (at most 6 vertices, 1 instance) stay where the game put them (the final composite).
//   * HUD: from the first other draw into the proxy on, every draw into it is recorded twice
//     at the NATIVE size, with the viewport and scissors scaled: once into layer N0 cleared
//     to black and once into layer N1 cleared to grey. The game's own blending runs, onto our
//     two backgrounds, and its HUD depth target is mirrored by two native depth targets.
//   * Present: DLSS gets the proxy, now the scene only. Any blend that is a fixed function
//     of the destination (alpha "over", additive, multiply, opaque, and any stack of them) is
//     out = N0 + dst * (N1 - N0) / grey, per channel, so the HUD is laid onto the DLSS picture
//     exactly as the game would have blended it, at full resolution.
//
// Anything we cannot do exactly (unknown pipeline, two targets, multisampling, a list whose
// targets cannot be handed back) forwards that draw unchanged into the proxy, so the worst case
// is today's behaviour for that draw. Counters say how often (status line native_hud).
#pragma once

#include <d3d12.h>
#include <cstdint>
#include <string>
#include <memory>

namespace na::observe { struct NativeOperation; }
namespace na::capture::j2 {struct Identity;}
namespace na::j2::native_hud {
class Picture;

// Boot (render-small): the draw hook, the clear hook, the bind listener and the observer's
// raster-state snapshots. Before the game creates its command lists. Idempotent.
void Install();
bool Installed();
void SetEnabled(bool on); // live switch; off = every draw goes to the proxy as before
// Full-screen-viewport draws after the HUD starts go to the proxy (opaque ones restart the layers).
// Off by default: Anno draws ordinary UI quads with a full-screen viewport (2b13ab2 lost most of the UI).
void SetCoverRule(bool on);
bool Enabled();

struct Layer {
    ID3D12Resource* n0 = nullptr;  // HUD over black (RENDER_TARGET; return it in that state)
    ID3D12Resource* n1 = nullptr;  // HUD over grey (RENDER_TARGET; likewise)
    DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN; // the format the game drew with (read with it)
    float grey = 0.f;              // N1's background as read through `view` (linear if sRGB)
    bool srgb = false;             // `view` is an sRGB format (the picture itself is stored encoded)
    std::shared_ptr<void> owner;     // layers retained through source recordings and Present GPU completion
    std::shared_ptr<Picture> picture; // draw transfer: complete original was preserved, GPU chooses clean/protect
    std::shared_ptr<Picture> reconstruction; // UI-only input: independent world snapshot for DLSS
    ID3D12Resource* ui_output=nullptr; // native UI seeded separately, then actual glyph draws
    DXGI_FORMAT ui_view=DXGI_FORMAT_UNKNOWN;
    bool clean_allowed = false;
    // hud_unblend: the same UI replayed at the game's own size over black / grey (RENDER_TARGET
    // between uses); PresentUpscale un-blends the UI out of the picture with them before DLSS.
    ID3D12Resource* l0 = nullptr;
    ID3D12Resource* l1 = nullptr;
    bool unblend = false;  // l0/l1 valid: run the un-blend pass and its self-check
    bool compose = false;  // and lay n0/n1 on (false while the self-check reports a mismatch)
    // Independent discovery probe; valid even when no HUD layer was produced.
    std::shared_ptr<Picture> probe;
    std::shared_ptr<void> probe_owner;
    uint64_t probe_source=0,probe_generation=0;
    ID3D12Resource* native_picture=nullptr; // NPSR: DLSS scene followed by the actual native HUD draws
    ID3D12Resource* native_check=nullptr; // original-resolution replay, independently compared with actual full colour
    std::shared_ptr<void> native_owner;
    std::shared_ptr<na::capture::j2::Identity> recording_identity;
};
// Runs after the original batch, before its application completion fence.
// Source leases may precede this batch on the same ordered queue.
void AfterSubmission(ID3D12CommandQueue*,uint32_t count,ID3D12CommandList* const*);
using PreHudRenderer=bool(*)(ID3D12CommandQueue*,ID3D12Resource*,uint32_t,uint32_t,ID3D12Resource*,uint32_t,uint32_t,const Layer&,ID3D12CommandList*);
void SetPreHudRendererForTests(PreHudRenderer); // null restores the production DLSS renderer
// PresentUpscale, for the proxy `frame` about to be upscaled from rw x rh to ow x oh: learns
// the proxy (its HUD draws are redirected from its next frame on) and returns true when this
// frame's HUD went into the layers. Ends the frame either way.
bool TakeFrame(ID3D12Resource* frame, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, Layer* out,
               ID3D12CommandQueue* queue = nullptr);

struct Stats {
    uint32_t last_present_native=0;
    uint64_t proof_tested=0,proof_matched=0,native_partial=0;
    float proof_maximum_error=0;
    uint64_t frames = 0, hud_frames = 0;    // frames presented while learned / with a HUD layer
    uint64_t scene_draws = 0, redirected = 0, forwarded = 0, restore_failed = 0;
    uint64_t covered = 0; // opaque full-screen draws after the HUD started: sent to the proxy, layers restarted
    uint64_t world = 0;   // draws bound to the jittered camera block: left in the proxy
    uint64_t masks = 0;   // depth/stencil-only draws on the HUD depth, copied into the native depths
    uint32_t last_scene = 0, last_redirected = 0, last_forwarded = 0; // the last presented frame
    uint32_t last_depth = 0, last_blended = 0, last_covered = 0, last_world = 0, last_masks = 0;  // of its redirected draws: depth-tested / blended
    uint32_t proxy_format = 0, view_format = 0; // DXGI formats of the proxy and of the HUD's views
    float grey = 0.f;                           // the grey layer's background as read through the view
    char last_forward[80] = {};             // why the last forwarded HUD draw was not redirected
    // H1b upstream picture: in use now, learned / dropped (structure changed) counts, presents whose
    // HUD came from it, the last frame's hand-off source (0 none, 1 one, 2 ambiguous), and why a
    // candidate was refused.
    uint32_t upstream_now = 0, last_source = 0;
    uint64_t upstream_learned = 0, upstream_dropped = 0, upstream_frames = 0;
    char upstream_why[64] = {};
    // Current-lineage checks (Astra 20cddb8 H1b point 3): presents whose hand-off did not come
    // from the upstream, and of those, frames whose already-redirected HUD was laid on anyway.
    uint64_t lineage_miss = 0, lineage_recovered = 0;
    uint64_t world_depth = 0; // draws left in the picture because they are depth-tested on the scene depth
    uint32_t traces = 0;      // frame-structure traces taken (hud_trace lines in the log)
    uint64_t draw_transfer_frames = 0, draw_protect_frames = 0;
    uint64_t transfer_verified=0,transfer_changed=0,clean_verified=0,protection_verified=0;
    uint32_t last_draw_method=0; // completed GPU receipt: 1 native, 2 current-picture protection, 3 changed transfer
    uint64_t canvas_candidates=0,canvas_probes=0,canvas_verified=0,canvas_changed=0,canvas_no_state=0;
    uint64_t canvas_source_snapshots=0,canvas_source_probes=0;
    uint64_t handoff_snapshot_missing=0,handoff_no_source=0,handoff_inferred=0;
    uint64_t native_recorded=0,native_submitted=0,native_verified=0,native_rejected=0;
    uint64_t native_allocated=0,native_reused=0;
    uint64_t native_resource_changed=0,native_finishing_draws=0;
};
Stats GetStats();
void ReportPictureResult(bool identity,bool clean,uint32_t tested=0,uint32_t matched=0,float error=0); // completed GPU receipt, status only
void ReportCanvasProbe(uint64_t source,uint64_t generation,bool identity,uint32_t tested=0,uint32_t matched=0,float error=0);
void ReportNativePictureResult(bool identity,uint32_t tested=0,uint32_t matched=0,float error=0);
std::string StatusLine();
// Finished frame-structure traces (hud_trace JSON lines, CRLF), taken once; empty when none.
std::string TakeTrace();

// Pure helpers (tests).
DXGI_FORMAT TypelessOf(DXGI_FORMAT f);      // the layer's resource format; UNKNOWN = not supported
float GreyFor(DXGI_FORMAT view);            // N1's background as the shader reads it through `view`
bool SceneShaped(bool blend_known, uint32_t blend_mask, uint64_t count, uint64_t instances);
bool CoversTarget(const float* viewport6, const int32_t* scissor4, uint32_t w, uint32_t h); // the whole w x h target
void ScaleViewport(const float in[6], float sx, float sy, float out[6]);
void ScaleScissor(const int32_t in[4], float sx, float sy, uint32_t ow, uint32_t oh, int32_t out[4]);
// Finds the one picture-sized SRV the actual shaders can read. Unused table
// entries do not establish lineage. This identifies a dependency, not proof
// that a draw preserves its colour, spatial mapping or HUD composition.
bool PictureSource(const na::observe::NativeOperation&, uint64_t destination,
                   uint32_t width, uint32_t height, uint64_t& source);

} // namespace na::j2::native_hud
