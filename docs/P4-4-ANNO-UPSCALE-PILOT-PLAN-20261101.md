# P4-4 pilot: DLSS upscaling on the J2 route, Anno first (plan, 2026-11-01)

Status: plan for review (Astra/Sol). No code yet. Owner of the J2 side: Fable/Opus. Shared files
(observation hooks, overlay, settings) need ownership agreed in COMMS before edits, as P4-4 says.

## Goal and constraints

- Cort (2026-11-01): DLSS upscaling must work in games that have no upscaler of their own. A game's
  own FSR/scaler option is not the plan (it may be used where it exists, never required).
- Pilot: Anno 1800, one reduced size only (DLSS Quality at 3440x1440, size taken from NGX's optimal
  settings query, not hard-coded), output 3440x1440. Off by default; J2 DLAA stays the default.
- Success is judged by matched pictures and GPU time, not by "it runs". A smaller raster area does
  not guarantee higher fps (Sol, COMMS a285862).

## Method (as reviewed by Sol, COMMS a285862)

Keep every game allocation at full size. During the main scene passes only, the game draws into a
render-size region (top-left) of those allocations. J2 then evaluates DLSS with an input subrect
(base 0,0, render size; the backend already passes subrect bases and sizes,
dlss_backend.cpp 678-687) into its own output, and copies the full-size result back as today
(separate color_in / output / copy-back; input and output never alias). Everything after J2's
insertion point (post-FX, HUD) sees a normal full-size image.

Sol's corrections, all adopted:
1. Map the producers AND readers of scene colour and depth both BEFORE and AFTER the J2 insertion
   point. Full-screen lighting, AO, reflections and compute may consume the scene before J2.
2. RSSetViewports/RSSetScissorRects change rasterizer mapping only. They do not change compute
   dispatch sizes, UVs computed in shaders, screen-size constants, history addressing or tiled
   light grids. Each such consumer needs its own treatment, or the game stays on DLAA.
3. Touch only the identified main-scene passes. Shadows, other views (reflection probes, minimap)
   and the HUD stay untouched.
4. Carry the active render extent explicitly through J2: camera/jitter, motion generation, depth
   sampling, optical flow and DLSS. Today they all use full size (live_dlaa.cpp 651/681/762,
   1049-1053, 1122-1142; render = max_render = output and Quality::DLAA at 1165-1167). Use the
   queried supported DLSS dimensions.
5. Restore full-size state at the chosen boundary, reusing J2's existing state, lifetime and reset
   handling.

## Stage M: scene map (read-only, first)

A diagnostic recording, off by default, built on the existing observation hooks (producers.cpp
already sees OMSetRenderTargets, RSSetViewports, Dispatch, descriptor tables, clears and copies)
and J2's insertion trace. For a bounded number of frames it writes a text log, one line per
operation in submission order across the frame's lists:

- list, queue, order; op kind (draw, dispatch, copy, clear, barrier); pipeline and root hash
- bound render targets / depth target: object ids, extents, formats; viewport and scissor
- every bound SRV/UAV whose resource is view-sized (same extent as the scene), with its object id,
  marked when it is the selected scene colour or depth
- dispatch group counts
- screen-size constants: in each bound constant buffer, 4-float groups that match (W, H, 1/W, 1/H),
  (1/W, 1/H) or (W, H) for the scene extent, with resource and offset (same kind of scan J2 already
  does for camera blocks)
- the J2 insertion point, the main-camera binding, and Present

Offline classification of that log (PowerShell or C#, since Python is not installed on Cort's PC):
main-scene passes; pre-J2 full-screen consumers; post-J2 consumers; shadows and other views
(different extents or camera); HUD.

Cort's part: one diagnostic run (install the package, one minute still camera plus one pan, exit).
The package saves its own log; nothing needs to be read off the screen.

### Decision gate D1 (reported to Cort and Sol before any shrinking code)

For every view-sized pass found: how it would follow the render extent (viewport/scissor;
screen-size constant patch; dispatch size; or "cannot follow"). Any "cannot follow" pass that is
needed for the main picture means no-go for this method in Anno (stays on DLAA), with the map as
evidence. Otherwise the stage P estimate is given from the actual list.

## Stage P: pilot at one size (only after D1)

Game side, scene passes only: viewport and scissor scaled to the render extent; the screen-size
constants found in Stage M patched for those passes; dispatch sizes adjusted where the map shows
they follow the screen size. Restored at J2's insertion boundary.

J2 side, render extent carried explicitly:
- camera tracker and jitter projection in render pixels; jitter phase count per NVIDIA's guidance
  for the ratio; mip bias log2(render/display) - 1 through the existing j2_mip_bias
- depth copy and arbiter motion over the render rectangle
- optical flow source size = render extent (encode, expansion and acceptance already take w/h)
- DLSS evaluation with input subrect = render extent, output = display extent, then the existing
  copy-back
- history reset on switching size or mode

Menu: a J2 quality selector (DLAA / Quality pilot), off by default, logged in the j2 status line.

Tests before any game run: a synthetic fixture renders a scene into the corner with a full-screen
pass that reads a screen-size constant; checks the viewport override, the constant patch, jitter
and motion at render extent and the DLSS stand-in output size, plus restore after the boundary.

## Stage E: evidence

Matched ON/OFF captures at the same spot (still camera, pan, moving ships), GPU time from the
existing timestamps, the j2 log. Cort judges the picture.

## Not in this plan

No new extraction solver, no renderer replacement, no reliance on the game's own scaler, no
change to the DLAA default. Estimates: Stage M about 1-2 days; Stage P only after D1.
