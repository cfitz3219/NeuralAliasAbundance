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

## Automated evidence and what remains

The actual DX12 test strips final-shader reflection with D3DStripShader. The
previous single-SRV entrance cannot identify that transfer. The new entrance
learns the actual image by GPU comparison and delivers both the clean/native
and clipped-additive protection paths. Changed processing is rejected, active
queries are forwarded, and two render-ahead pictures preserve independent
scene/layer snapshots. Standalone comparison tests reject even one changed
edge pixel and verify that both original textures stay byte-identical.

The focused host test passes on the RTX5070Ti with zero debug-layer errors.
This is a controlled reproduction, not evidence the exact cause in Sniper was
stripped reflection or that Sniper's UI is now visually correct. Full combined
validation and a separate pre-HUD/native-canvas path remain outstanding here.
No game launch, deployment or installer update has occurred in this increment.

## Four requested paths: preserve their real distinctions

1. Existing direct-backbuffer native HUD handling, used by the Anno route.
2. Intermediate scene snapshot plus clean native HUD replay/composition.
3. Actual draw-coverage protection when a clean native layer is unavailable.
4. Requested pre-HUD upscale with subsequent ordinary native HUD rendering.

Path3 protects an existing composite; it does not recreate native text. Paths2
and3 previously shared the failing entrance. Path4 is not implemented by merely
adding another mask, renaming path2 or counting this discovery probe as a new
separator. It still needs implementation and qualification.

## Primary references

- NVIDIA's integration order places UI after DLSS and display-resolution post
  processing: https://raw.githubusercontent.com/NVIDIA/DLSS/main/doc/DLSS_Programming_Guide_Release.pdf
- NVIDIA explanation: https://developer.nvidia.com/blog/nvidia-dlss-natively-supported-in-unity-2021-2
- Microsoft shader reflection can be removed independently of executable code:
  https://learn.microsoft.com/en-us/windows/win32/api/d3dcompiler/ne-d3dcompiler-d3dcompiler_strip_flags
