# Sniper HUD transition and independent canvas discovery

## Live evidence, before implementation

Cort tested be67bb1, PID57140, results `NA-se5-results-20261104-040050`.
Both draw-handoff methods were present but processed zero HUD frames. All
47 status samples have zero candidates, redirected draws and compositions.
The backend tests did not establish live admission and do not override this.

The ten raw `hud_trace` frames place the long blended draw sequence on an
intermediate SDR/typeless colour image before a final triangle into a small
proxy backbuffer. At traced Present300, image13566 receives its last
camera-associated draw at operation451, then53 non-camera draws at452..504;
the output draw into13546 is505. The example size is2293x941. After a size
change, the corresponding busy image is14074 at1720x706. These IDs are local
evidence, never runtime configuration.

This identifies a candidate scene/HUD transition, not a proved GPU boundary.
The trace is in CPU recording order, omits pipeline IDs, and some frames have
no camera marker or stale camera bindings. The last no-camera draw alone
cannot establish a universal UI boundary. A clean pre-HUD insertion must keep
HUD raster dimensions native, use that picture's depth/motion/camera, and
preserve all later processing. Moving a Present-time NGX call earlier without
those conditions would not implement the requested path.

## The shared failure and the implemented independent entrance

The two existing draw methods learn an upstream canvas only after the final
output draw exposes exactly one picture-sized used SRV. Missing reflection,
unresolved state or several inputs all prevent either method from running.
The old status collapsed these failures into an unexplained ambiguous source.

The new discovery entrance observes actual colour-target writes, not a presumed
descriptor slot. Same-size/format-family images ending in blended 2D writes
become probe candidates. A probe snapshots the candidate at the output draw,
then compares that immutable snapshot with the exact final picture on the GPU.
It neither alters the pictures nor licences HUD removal. Two completed matching
probes admit the canvas; the existing clean/protection decision still verifies
EVERY actual picture before changing its input or composition.

The probe and source texture are retained through the actual game recording;
Present retains the packet through its existing fence. There is no queue,
GPU wait or synchronous readback added to game callbacks. Completed16-byte
results use the existing Present fence. Source output extent is taken from the
actual destination, not a different old swapchain with the same render extent.
Candidate dimensions are cached at target discovery, not fetched per draw.

An opaque full-target write now replaces previous handoff evidence rather than
preserving an earlier ambiguous source forever. Partial-screen triangles do not
receive that whole-picture privilege. Counters distinguish candidate canvases,
scheduled/matching/changed probes, absent local texture state, unavailable
operation snapshots, unidentified sources and pixel-admitted handoffs.

## Pre-HUD upscale and actual native HUD draws

The fourth path is now implemented. It saves the scene before the terminal HUD
sequence while preserving every original game draw. It records the actual game
HUD shaders, indexed geometry, constants, texture bindings, blending and clipping
into a private list. The existing J2 upscaler reconstructs the saved scene into
a native picture, then that private list draws the HUD onto it at native size.
This uses the existing NGX view, guide acquisition, queue and ring fences; it
does not create a second upscaler backend or synchronously read back the image.

The same real HUD commands are also drawn at the original resolution over the
saved scene. Before showing the native result, a GPU comparison checks BOTH
that this original-size redraw matches the game's full composition (including
alpha) and that the final handoff still matches that exact composition. If
either fails, the original final game picture is retained. This is distinct
from black/grey affine recomposition and from a low-resolution protection mask.

The extra draw list executes after the exact original Execute batch, before
its application completion fence. Every source recording must belong to that
batch, with the same object ID and generation. Existing producer queue ordering
also serialises Signal/Wait, so an application fence cannot jump between the
batch and the host callback. Every HUD texture/root buffer/vertex/index/predicate
read is tracked; changes to a non-readable or unknown final subresource state
decline this delayed redraw and retain the older route. No guessed resource
barrier is issued. Direct depth/stencil-tested HUD remains with the established
mirrored-depth path; this new replay does not pretend to reproduce it.

Late camera/world or screen-position full-screen processing restarts capture
at the next terminal HUD sequence. The game original is never removed first.
Resources, descriptors and command storage are pooled, reused only after all
recording and Present owners retire, and bounded to eight native packets.
Inactive extents are pruned. Restarting within an unsubmitted recording reuses
its packet rather than allocating another full-size image. Test telemetry
shows two native allocations and eight reuses across the covered cases.

The pre-upscale and final-presentation phases use separate descriptor pages,
because both may be recorded while the earlier one is still executing. Alpha
is seeded from the saved scene before the original destination-alpha blending;
DLSS RGB and HDR highlights are preserved. Final F8 capture saves the ACTUAL
composed output plus scene/full/native/check images, marks `hud_presentation`,
and carries the original evaluation's camera/jitter identity. It does not label
later-frame depth or motion as belonging to this already-rendered picture.

## Automated evidence and remaining live observation

The actual DX12 test strips final-shader reflection with D3DStripShader. The
previous single-SRV entrance cannot identify that transfer. The new entrance
learns the actual image by GPU comparison and delivers both the clean/native
and clipped-additive protection paths. Changed processing is rejected, active
queries are forwarded, and two render-ahead pictures preserve independent
scene/layer snapshots. Standalone comparison tests reject even one changed
edge pixel and verify that both original textures stay byte-identical.

The focused host tests pass on the RTX5070Ti. Expanded native replay cases use
actual indexed geometry and stripped HUD shaders, additive clipping, query-frame
forwarding, independent render-ahead frames, changed-processing fallback, a
vertex buffer switched to COPY_DEST before delayed redraw, HDR/alpha, and
in-flight descriptor pages. The original Anno/direct-backbuffer and both older
draw-transfer paths still pass. Executable UAV writes remain excluded even
after optional reflection data is stripped. The shader fallback uses Microsoft's
D3DDisassemble for standard SM4/5 DXBC; it does not claim DXIL reflection.

The separate J2 live test calls the PRODUCTION PreHudUpscale callback after
actual deferred scene-depth/motion acquisition. It checks one evaluation,
correct native dimensions, actual appended replay execution and refusal to
consume the same guides twice. Its reconstruction model is fake. Together
with the actual-draw test this establishes engineering wiring and composition,
not that Sniper's HUD is visually correct or that the stripped-shader case was
the exact live failure. Full combined validation receipt follows in WORKLOG
and COMMS. No game launch/control/deployment has occurred.

## Four requested paths: preserve their real distinctions

1. Existing direct-backbuffer native HUD handling, used by the Anno route.
2. Intermediate scene snapshot plus clean native HUD replay/composition.
3. Actual draw-coverage protection when a clean native layer is unavailable.
4. Saved pre-HUD scene through the existing upscaler, followed by actual native
   HUD redraw and an independent original-size redraw comparison.

Path3 protects an existing composite; it does not recreate native text. Paths2
and3 previously shared the failing entrance, repaired by independent observed
canvas discovery. Path4 uses actual native draws rather than another mask or
another name for path2. No per-game addresses, texture IDs or shader hashes.

These cover different rendering patterns. They do not guarantee every engine:
arbitrary colour-changing processing between the canvas and final output,
unbounded bindless bindings, indirect/compute UI without replayable state, and
UI shaders whose native screen-position reads need additional dimension
adaptation remain outside this new native redraw. A GPU-verified unchanged
handoff is required for this scene substitution; changed processing retains the
complete game picture. This is a finite reusable implementation for the observed
shape, not a promise that four method names prove universal HUD discovery.

## Exact integration for Opus

Jitter runtime6bc5731 is independently READY and has been merged into this HUD
branch for combined validation. Do not cherry-pick that merge or import paused
Layer2 sizing ancestry. On your jitter-integrated claude/se5-motion-pkg, pick:
e4868ee (independent GPU canvas entrance), ca28b7a (pre-HUD replay checkpoint),
then the final completion commit announced READY in COMMS. ca28b7a is only a
checkpoint until paired with that final commit. Preserve your current jitter,
flow, sharpening, preset and fullscreen fixes when resolving shared files.

The source scene and final image must stay immutable per captured occurrence;
do not replace their owner with a global latest image or expire it by a count
of Presents. Keep both descriptor pages, alpha seeding, both GPU comparisons,
same-batch source identity and resource-state qualification. Keep normal draws
forwarding until GPU proof; do not move the destructive HUD redirect into this
discovery path. Pool only fully retired owners. No per-game profile is needed.

After combined tests, stage one additive package and point the existing CMD at
it. Cort retains installation/game testing. The overlay identifies the last
COMPLETED method; logs distinguish canvas probes from actual native submission,
verified/redraw-rejected output, resource-state rejection and pool allocations.
First live confirmation must show nonzero canvas/probe/admission/native counters,
not merely an enabled checkbox. F8 then distinguishes the saved scene, original
composite and actual selected native output. No extra review cycle requested.

## Primary references

- NVIDIA's integration order places UI after DLSS and display-resolution post
  processing: https://raw.githubusercontent.com/NVIDIA/DLSS/main/doc/DLSS_Programming_Guide_Release.pdf
- NVIDIA explanation: https://developer.nvidia.com/blog/nvidia-dlss-natively-supported-in-unity-2021-2
- Microsoft shader reflection can be removed independently of executable code:
  https://learn.microsoft.com/en-us/windows/win32/api/d3dcompiler/ne-d3dcompiler-d3dcompiler_strip_flags

Final receipt: source6ba04ef complete200case suite195PASS5SKIP0FAIL263.68s. Subsequent inactive discovery-profile cache repair passed the expanded GPU canvas replacement test and final seven affected regressions14.69s after a complete Release rebuild. Final commit is named in COMMS. Profile eviction releases learning slots; GPU data still follows recording/Present owners. No game test/deployment by Astra.
