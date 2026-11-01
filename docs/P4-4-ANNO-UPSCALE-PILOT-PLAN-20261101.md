# P4-4: DLSS upscaling on the J2 route (plan rev 5, 2026-11-01)

Status: plan for review (Astra/Sol), iterated on COMMS until solid (Cort). No code yet. Owner of
the J2 side: Fable/Opus. Shared files (observation hooks, overlay, settings) need ownership agreed
in COMMS before edits, as P4-4 says.

## Cort's binding direction (2026-11-01)

- **No FSR route.** A game's FSR (or any vendor upscaler) is never the mechanism: it limits the
  program to games that ship one.
- **The game's own resolution / render-scale slider is the main route.** Nearly all games since
  2018 have one (Argon). It makes the game render its 3D scene smaller with the HUD at full size, so
  the game itself creates the smaller scene textures; no texture shrinking by us.
- **No "learn on the first launch, apply on the second".**
- **One click (Cort, 22:20Z):** the user picks DLAA / Quality / Balanced / Performance in our
  menu and it just works; the user never touches the game's slider. We query DLSS's render size
  and set the game's render-scale value ourselves (live through an engine setter where an adapter
  exists, else the game's settings file). A game that only accepts steps (55/60/65) gets the
  nearest accepted value; DLSS is told the real size (any size works) and the menu shows it.
- **Sniper (found 2026-11-01, read-only):** both Sniper Elite 5 and Resistance keep
  `RenderScale` (float, 0.5-2.0, continuous) in `%LOCALAPPDATA%\<game>\GraphicsOptions.ini`.
  Pilot mechanism: write the DLSS ratio (e.g. 0.666667) there at process start, before the game
  reads the file, so it applies on the first launch; a mid-game change applies on the next launch
  until a live engine setter is found. To verify in the first run: the game keeps the value.
- **Coverage:** the DLSS path never depends on where the setting lives (J2 detects the real
  reduced extent; any slider step works). One-click needs a per-engine profile (file, key,
  format): Unreal screen percentage (existing), Rebellion's GraphicsOptions.ini RenderScale
  (Sniper verified; other Rebellion titles likely, unverified). No profile -> the menu asks the user
  to set the game's slider once to ~65-70%. No auto-scan confirmed on a later launch (that is
  learn-then-apply, ruled out).

History: rev 1 (corner/subrect), rev 2 (creation-time shrink), rev 3 (game's FSR as the Anno pilot)
and rev 4 (no game scaler at all) are superseded; see COMMS a285862 to 8ed6614.

## Goal and constraints

- DLSS Super Resolution (render extent < output extent) in DX12 games without DLSS, HUD at output
  size, DLAA unchanged as the default and the fallback for every case below.
- Pilot: one game with a resolution slider, one reduced size (DLSS Quality, render size from NGX's
  optimal-settings query), output at the swapchain size. Off by default.
- Reuse J2 detection, state, lifetimes and the existing NGX backend. No new solver, no renderer
  replacement.
- Success is judged by matched pictures and GPU time, not by "it runs".

## Route ladder

1. **R1, the game's resolution slider (main route).** The slider is set to the DLSS render size
   (by the user from our menu's guidance first; setting it for them is a later convenience). J2
   finds the reduced scene colour + depth, evaluates DLSS into a distinct full-size output, and
   replaces the game's own spatial stretch to its output target.
2. **R2, our engine adapters** (existing Unreal screen-percentage and Ghost adapters).
3. **R3, no slider (next after R1, Anno as test case):** Argon's layer 2, shrink the 3D-scene
   textures at creation (whole texture, never a corner, so 0..1 UVs stay right) with matching
   viewports. Open pieces: recognise scene textures on the first launch with no learning run
   (built-in per-game profile from a recording, format combinations, or create both sizes and pick
   in frame 1); never shrink a texture copied to/from a full-size one; fix stored screen-size
   constants and dispatch sizes; redirect later passes to the DLSS output. Stage M maps come first.
4. Otherwise DLAA. Subrect/corner rendering is rejected.

## Stage 1: J2 upscales from a smaller scene (prerequisite, fixtures only)

Teach J2 render != output: evaluate DLSS W_in x H_in -> W_out x H_out (the backend already takes
separate sizes and input subrects, dlss_backend.cpp 678-687; J2 sets them equal today,
live_dlaa.cpp 1165-1167, and copies back into the same scene resource, 1261-1280), into a distinct
full-size output target, carrying the active render extent explicitly through camera/jitter (phase
count from the ratio), motion units, depth sampling, optical flow and the mip bias
(log2(render/display) - 1). Active extent and allocation size are tracked separately (a game may
render a smaller region inside a larger allocation, and colour and depth may differ). Built and
tested on fixtures; no game setting changed.

## Stage M: map of a slider-reduced frame (read-only)

Cort sets the game's resolution slider to the DLSS Quality size and does one diagnostic run on the
current DLAA build. A text recorder, off by default, built on the existing observation hooks and
J2's insertion trace, logs per operation in submission order for a bounded number of frames:

- list, queue, order; op kind (draw, dispatch, copy, clear, barrier); pipeline and root hash
- bound render targets / depth target with extents and formats; viewport and scissor
- every bound SRV/UAV that is scene-sized or output-sized, marking the selected scene colour/depth
- every copy/resolve touching those textures; dispatch group counts
- the J2 insertion point, the main-camera binding, and Present

From it: (a) the reduced scene colour and depth and their active extents; (b) the operation that
stretches the scene to output size and its destination; (c) where AA, tonemapping and exposure run
relative to that stretch; (d) the HUD boundary after it. Pair scene/depth with the actual stretch
destination (size alone also matches bloom or reflection targets). Offline classification in
PowerShell or C# (no Python on Cort's PC). The package saves its own log.

## Stage P: pilot at one size

- J2 inserts DLSS at the stretch: DLSS output written into the stretch's destination, the game's
  stretch (and any sharpening of it) must not reprocess the result. Mechanism (skip the game's
  dispatch/draw vs overwrite after it) to be agreed with Astra (open Q1; Fable prefers skip).
- If the stretch runs after AA/tonemap, the pilot takes that LDR input with honest colour flags and
  is judged in Stage E; Stage M records whether an earlier HDR point at render size is reachable.
- Menu: J2 quality selector (DLAA / Quality pilot), the slider value to use, logged in the j2 line.
- History reset on switching size or mode; every failure falls back to DLAA or the game's own path.

## Stage E: evidence

Matched captures at the same spot (DLAA at native, the game's own stretch at the reduced size, DLSS
at the reduced size), GPU time and fps, ghosting on movers. Cort judges the picture.

## Open with Astra

- Q1 skip vs overwrite the game's stretch.
- Q3 ownership: Fable Stage 1 and the Stage M recorder (bounded, off by default, in producers.cpp,
  if Astra agrees); Astra placement/insertion (Stage P) as before.
- Q4 anything to reorder.
- Which game is the pilot: Anno has no resolution slider separate from its FSR option (Cort,
  2026-11-01), so Anno is not the pilot. Proposed pilot: Sniper Elite 5 or Resistance (J2 inline
  route, reported resolution-scale setting; Cort to confirm). Anno stays on DLAA until R2 or later
  research gives it a route.

## Estimates

Stage 1 about 2-3 days; Stage M recorder about 1-2 days; Stage P after the map.
