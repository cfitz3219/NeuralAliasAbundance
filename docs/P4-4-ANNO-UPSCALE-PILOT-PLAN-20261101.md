# P4-4 pilot: DLSS upscaling on the J2 route, Anno first (plan rev 2, 2026-11-01)

Status: plan for review (Astra/Sol). No code yet. Owner of the J2 side: Fable/Opus. Shared files
(observation hooks, overlay, settings) need ownership agreed in COMMS before edits, as P4-4 says.

Rev 2 (same day): Cort supplied a second proposal (two stages: J2 upscaling from a smaller scene
pair; shrinking the scene textures at creation instead of drawing into a corner). Its critique of
the corner approach matches Sol's correction 2 and is adopted; the corner method is kept only for
engines already built for dynamic resolution. Stage 1 below is that proposal's first stage; Stage
P2 is its second, with the safety rules it lacks.

## Goal and constraints

- Cort (2026-11-01): DLSS upscaling must work in games that have no upscaler of their own. A game's
  own FSR/scaler option is not the plan (it may be used where it exists, never required).
- Pilot: Anno 1800, one reduced size only (DLSS Quality at 3440x1440, size taken from NGX's optimal
  settings query, not hard-coded), output 3440x1440. Off by default; J2 DLAA stays the default.
- Success is judged by matched pictures and GPU time, not by "it runs". A smaller raster area does
  not guarantee higher fps (Sol, COMMS a285862).

## Why no single method is universal

| | corner (subrect in full buffers) | game's own scale | shrink scene textures at creation |
|---|---|---|---|
| full-screen quads sampling UV 0..1 | wrong pixels | fine | fine |
| compute dispatches | ignore the viewport | fine | extra threads, otherwise fine |
| screen-size constants, pixel-addressed loads | wrong | fine | still wrong (must be found and patched) |
| which textures are the scene | known from J2 | n/a | decided at creation, before any frame evidence |
| copies between scene and other textures | fine | fine | size mismatch = device removed (crash) |
| full-size DLSS result into later passes | copy-back as today | replaces the game's final upscale | needs descriptor redirection |
| depth/motion read later at full size | fine | n/a | needs full-size copies |
| games covered | DRS-built engines only | games with a slider | the rest, where the map allows |

All three depend on knowing the game's pass structure, so the map comes first in every case.

## Stage 1: J2 upscales from a smaller scene pair (prerequisite for everything)

Teach J2 render != output: detect a scene colour + depth pair at W_in x H_in below the swapchain
size, evaluate DLSS W_in x H_in -> W_out x H_out (the backend already takes separate sizes and input
subrects, dlss_backend.cpp 678-687; J2 sets them equal today, live_dlaa.cpp 1165-1167), carry the
render extent explicitly through camera/jitter, motion, depth, optical flow and the mip bias
(log2(render/display) - 1), and write the result where the game's own spatial upscale would have
written. In games with a resolution/render-scale slider this alone gives DLSS upscaling; the
insertion logic must learn that final-upscale spot (post-FX in such games usually runs at the small
size, so DLSS sees a post-processed image there; quality is judged in Stage E).

Open check (Cort, menus only): do Anno 1800, Sniper Elite 5 and Sniper Elite Resistance have a
resolution-scale slider? Anno's only known scale control is its FSR option (engine.ini
FSRUpscaling). Stage 1 is built regardless, because Stage P2 needs it too.

## Stage M: scene map (read-only, before any shrinking code)

A diagnostic recording, off by default, built on the existing observation hooks (producers.cpp
already sees resource creation, OMSetRenderTargets, RSSetViewports, Dispatch, descriptor tables,
clears and copies) and J2's insertion trace. For a bounded number of frames it writes a text log,
one line per operation in submission order across the frame's lists:

- list, queue, order; op kind (draw, dispatch, copy, clear, barrier); pipeline and root hash
- bound render targets / depth target: object ids, extents, formats, creation order and flags
  (heap type, placed/committed, allow-RT/DS/UAV); viewport and scissor
- every bound SRV/UAV whose resource is view-sized (same extent as the scene), with its object id,
  marked when it is the selected scene colour or depth
- every copy/resolve touching a view-sized texture (source, destination, extents)
- dispatch group counts
- screen-size constants: in each bound constant buffer, 4-float groups that match (W, H, 1/W, 1/H),
  (1/W, 1/H) or (W, H) for the scene extent, with resource and offset (same kind of scan J2 already
  does for camera blocks)
- the J2 insertion point, the main-camera binding, and Present

Offline classification of that log (PowerShell or C#, since Python is not installed on Cort's PC):
main-scene passes; pre-J2 full-screen consumers; post-J2 consumers; copies in and out of the scene
set; shadows and other views (different extents or camera); HUD; and, for Stage P2, a creation-time
fingerprint (order, format, flags, size class) of every texture in the scene set.

Cort's part: one diagnostic run (install the package, one minute still camera plus one pan, exit).
The package saves its own log; nothing needs to be read off the screen.

### Decision gate D1 (reported to Cort and Sol before any shrinking code)

For every view-sized pass and copy found: how it would follow the render extent (texture shrunk;
screen-size constant patched; dispatch size adjusted; copy partner also shrunk; or "cannot
follow"). Any "cannot follow" pass or copy that the main picture needs means no-go for Anno
(stays on DLAA), with the map as evidence. Otherwise the Stage P2 estimate is given from the list.

## Stage P2: shrink the scene set at creation (only after D1, games without a slider)

- Creation hooks (CreateCommittedResource / CreatePlacedResource, swapchain buffers excluded)
  shrink only textures matching the Stage M fingerprint whitelist for this game; applied on the
  next launch or on a resize, since the scene set is created before frame evidence exists.
- Safety rules: never shrink a texture that the map shows copied to or from a full-size texture
  unless the partner is shrunk too; never shrink UI/composition targets; refuse (stay DLAA) if the
  creation stream does not match the fingerprint; placed resources keep valid heap offsets.
- Viewports and scissors are scaled only while a shrunk target is bound.
- Screen-size constants found in Stage M are patched for the passes that bind shrunk targets;
  dispatch sizes adjusted where the map shows they follow the screen size.
- The first full-size consumer after the scene has its scene-colour descriptor redirected to J2's
  full-size DLSS output (the observation layer already resolves descriptor writes); depth gets a
  full-size nearest copy if a later pass reads it at full size.
- Restore and reset through J2's existing state and lifetime handling.

## Stage E: evidence

Matched ON/OFF captures at the same spot (still camera, pan, moving ships), GPU time from the
existing timestamps, the j2 log. Cort judges the picture.

## Not in this plan

No new extraction solver, no renderer replacement, no reliance on the game's own scaler for the
general case, no change to the DLAA default. Estimates: Stage 1 about 2-3 days; Stage M about 1-2
days; Stage P2 only after D1.
