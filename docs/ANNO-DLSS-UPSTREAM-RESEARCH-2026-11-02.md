# Anno DLSS: upstream methods and reusable parts

Research by Astra, 2 November 2026 Pacific, at Cort's request. Research only:
no rendering changes, game launch, installation, or third-party binary execution.
Fable retains the Anno implementation. NeuralAlias's existing backend, acquired
inputs, reconstruction, frame ownership and overlay remain the starting point.

## Practical conclusion

There is useful implementation code to reuse. The closest new DX12 reference is
**EldenRingDLSS**, particularly its treatment of the maps and passes after an early
upscale. The strongest broad alternative is **a genuinely lower-resolution game
with separately sized presentation**, implemented by DLSS5 ReShade AIO and preceded
by established window/proxy upscalers. Neither requires replacing our core.

I did not find a verified ready-made Anno 1800 DLSS-SR mod. That is a search
result, not a proof none exists. Many popular "DLSS 5 anywhere" tools primarily
inject Neural Rendering; that alone does not make the game render fewer pixels.

## What is failing here now

Read shared COMMS and current Anno evidence, not just the older Layer2 failure.
The installed manifest names cf65722. Last available status-22116 sample:
12,369 scene frames, 58 missed, zero early placements, zero no-state/no-camera/
DLSS failures. Projection-window counters show 7,063 windowed frames, 55 misses,
eight engagements, 183 repeats, and 31,035 capped camera blocks. The final state
is disengaged, Quality requested but render/output both 3440x1440, with a note
that too many windowed frames presented without DLSS. These are counters, not a
new image-quality measurement. They establish that successful NGX evaluation
alone is not our unresolved task.

Fable's newer **fab5928** is not that installed sample. Its source expands the
scan, keeps the window engaged, and repeats the previous result rather than
exposing a raw corner on some failures. Repeating can conceal a raw/processed
flash; it does not reconstruct the missing current picture. Keep fresh outputs,
repeats and CPU cost distinct when assessing it. No new runtime verdict is made
without its actual run.

## 1. Closest DX12 reference: EldenRingDLSS

[Repository](https://github.com/masonpetrosky/EldenRingDLSS), inspected commit
`1fddbb6ce895dd497c7d9d1297f1d682c5ad7a26`. Project code is MIT licensed.

This mod obtains lower scene rendering through Elden Ring's hidden engine
resolution scale. Its early DLSS output initially appeared cropped because later
passes still used the smaller region. Its investigation then addressed the later
command lists and their resolution-dependent processing. The author's report
records 9.04 ms at 67% versus 10.97 ms DLAA and 9.38 ms game TAA at one tested spot;
these are their measurements, not ours or an Anno prediction.
[Investigation and results](https://github.com/masonpetrosky/EldenRingDLSS/blob/1fddbb6ce895dd497c7d9d1297f1d682c5ad7a26/docs/recon.md#stage-3-round-4-where-to-upscale-2026-10-10).

Concrete source to compare:

- [`StretchPostInputs`](https://github.com/masonpetrosky/EldenRingDLSS/blob/1fddbb6ce895dd497c7d9d1297f1d682c5ad7a26/src/addon.cpp#L1326)
  handles scene depth, depth copy, motion, emissive and half-depth readers.
- [`Stretch::Run`](https://github.com/masonpetrosky/EldenRingDLSS/blob/1fddbb6ce895dd497c7d9d1297f1d682c5ad7a26/src/stretch.cpp)
  provides actual DX12 copy/expansion work, including depth output.
- [`CorrectPostPassImpl`](https://github.com/masonpetrosky/EldenRingDLSS/blob/1fddbb6ce895dd497c7d9d1297f1d682c5ad7a26/src/addon.cpp#L1645)
  shows viewport/scissor and constant corrections after the upscale.
- [`RenderScale::Find/Set`](https://github.com/masonpetrosky/EldenRingDLSS/blob/1fddbb6ce895dd497c7d9d1297f1d682c5ad7a26/src/render_scale.cpp)
  shows the engine-specific sizing adapter separately from DLSS execution.

**Our application:** use this as a comparison for Anno's later camera/guide-map
handoff. We already have private resources, guide promotion, depth conversion,
scoped constants and submitted lifetimes. Reuse those implementations rather than
adding another resource framework. Determine which actual Anno readers need the
handoff and correct those. Do not copy Elden Ring's shader names, memory offsets,
raw mapped-buffer edits or predictions about command-list order into generic code.
Its SDK integration is useful reference, not independent verification of Anno.

## 2. Broad alternative: smaller game, separately sized output

[DLSS5 ReShade AIO](https://github.com/kibblerz/DLSS5-Reshade-AIO), inspected
`09301f5528e619e8b9ec17c257d167e2985f53b0`. Original project code is Apache-2.0;
preserve its LICENSE, NOTICE and third-party notices if reusing code.

Its documentation describes a smaller actual game backbuffer feeding a native
output, with standalone SR available separately from NR. It requires the game
really to create the smaller backbuffer; a lower working size after a native
frame was rendered does not save the game's native shading work.
[Setup and limitations](https://github.com/kibblerz/DLSS5-Reshade-AIO/blob/09301f5528e619e8b9ec17c257d167e2985f53b0/README.md).

Source inspected in `addon/src/nr-standalone.cpp`:

- `SetSrEvaluationContract`, ~5403: different input and output extents.
- Coordinate conversions, ~7178: reduced game client versus visible output.
- `ApplyCompositionHostTransform`, ~8190: composition host sizing.
- ~9821–9919: chooses game HWND or detached host, builds a native-size composition
  swapchain, and attaches its visual. This is real presentation code, not a plan.
- Source explicitly sets SR jitter metadata to zero at ~5409. That contract is
  not a quality solution to import into our jittered input path.
[Pinned source](https://github.com/kibblerz/DLSS5-Reshade-AIO/blob/09301f5528e619e8b9ec17c257d167e2985f53b0/addon/src/nr-standalone.cpp#L9821).

**Our application:** retain NeuralAlias's acquisition, motion, real jitter,
backend and overlay. Let Anno render its own complete lower-resolution frame;
present reconstructed output at monitor resolution. That removes the need to
window every engine matrix or resize every intermediate ourselves. Borrow the
presentation and coordinate-handling pieces only, keeping our ownership model.

The full-frame version also begins with a lower-resolution game HUD, and later
processing of a finished image differs from normal early DLSS placement. Native
HUD separation or an earlier insertion with corrected post processing is further
work, not something the second window supplies automatically. Our own overlay
can remain native-sized. AIO's issue reports include cursor/window and resize
problems; source availability does not mean proven drop-in Anno compatibility.
[Issue 3](https://github.com/kibblerz/DLSS5-Reshade-AIO/issues/3),
[issue 5](https://github.com/kibblerz/DLSS5-Reshade-AIO/issues/5).

This is worth a bounded prototype if the current projection route keeps growing;
it is not authorization to begin another implementation alongside Fable's work.
Its first useful comparison is one lower window size, no NR/FG, real guide/jitter
matching, still/pan pictures and end-to-end GPU cost against native and unmodified
low resolution. No separate universal detector is needed before that experiment.

## 3. Established precedents for independent presentation

**3Dmigoto** documents two upscaling modes: a replacement game backbuffer, or a
second swapchain presented at another resolution. It is DX11-era infrastructure,
not an Anno DX12 or temporal-input solution. It is valuable evidence that the
separate presentation pattern predates the current NR tools.
[Device/upscaling configuration](https://github.com/bo3b/3Dmigoto/blob/master/Dependencies/d3dx.ini).

**Magpie** (`241e0a447d1c45fc95781f9cd4b5a9198e738a3b`) supplies general window
scaling infrastructure. Its README describes spatial filters including FSR and
Anime4K, not an extracted Anno temporal contract. Use as a window/input/presentation
reference, not as proof DLSS inputs or native HUD extraction are solved.
[Magpie](https://github.com/Blinue/Magpie).

Microsoft explicitly supports composing a DX12 queue's swapchain into a window.
That API gives us a standard presentation primitive, not an engine resizer.
[CreateSwapChainForComposition](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforcomposition).

## 4. Native engine controls: PureDark, Luma and existing Anno FSR

Read PureDark's public Skyrim-Upscaler at
`fa057bb088cf399e1112c1eaba714590c881e462`, cloned read-only under
`local/anno-dlss-research-20261102`. `DRS::SetDRS` writes the engine's current width/
height scale fields; `UpscalerHooks.cpp` hooks the game's TAA/jitter operations.
That is a concrete successful architectural pattern: use engine sizing and replace
the reconstruction boundary. The Skyrim offsets and SKSE interfaces do not apply
to Anno. Public project license is MIT.
[DRS.cpp](https://github.com/PureDark/Skyrim-Upscaler/blob/fa057bb088cf399e1112c1eaba714590c881e462/src/DRS.cpp).

Luma Prey similarly has native DRS plus a known temporal pass. Its source restores
native-size processing after an early upscale. Luma's generic Unreal path, however,
sets `sr_render_resolution_scale = 1.0f` and says `DLAA only` in the inspected
snapshot. Do not infer an automatic lower-resolution controller from its generic
DLAA support. Source for the advertised Witcher 2 entry inspected here has
`ENABLE_NGX 0` and no DLSS scale implementation in that file; the wiki listing
alone is insufficient to call it a working reusable SR reference.
[Prey](https://github.com/Filoppi/Luma-Framework/blob/6fce340570a4bb6b6704a4ca9e44bb7bfc8cf4ff/Source/Games/Prey/main.cpp),
[Unreal](https://github.com/Filoppi/Luma-Framework/blob/6fce340570a4bb6b6704a4ca9e44bb7bfc8cf4ff/Source/Games/Unreal%20Engine/main.cpp),
[Witcher 2 inspected file](https://github.com/Filoppi/Luma-Framework/blob/6fce340570a4bb6b6704a4ca9e44bb7bfc8cf4ff/Source/Games/The%20Witcher%202/main.cpp).
Luma uses custom MIT terms with attribution and author permission for commercial
reuse. Do not describe it as plain MIT.

Anno's developers explicitly document engine-managed lower rendering through
FSR1 in update11.1. Using that sizing path with our replacement is an established
fallback to investigate, not a discovery made today or a standard FSR2/3 DLL swap.
Cort previously excluded relying on the game's scaler. That restriction has not
been silently reversed, and no game setting was changed in this research.
[Anno update11.1](https://www.anno-union.com/updates/anno-1800-game-update-11-1/).

## 5. A genuinely different performance lever: VRS

DX12 variable-rate shading can shade a group of pixels once while retaining the
same render-target dimensions. Microsoft exposes per-draw 2x2 shading through
`RSSetShadingRate`; no new small scene buffer is required. NVIDIA also publishes
VRS reference material, and vrperfkit demonstrates injected VRS in DX11 VR.
[Microsoft API and capabilities](https://learn.microsoft.com/en-us/windows/win32/direct3d12/vrs),
[NVIDIA overview](https://developer.nvidia.com/vrworks/graphics/variablerateshading),
[vrperfkit scope](https://github.com/fholger/vrperfkit).

**Our inference:** selective VRS plus existing native-size DLAA is a much smaller
performance experiment than rewriting Anno's rendering dimensions. Existing scene
pass identification could scope it, with full-rate UI and other sensitive passes.
It is NOT DLSS Quality/Performance upscaling. It reduces pixel shading rather than
all geometry/compute/rendering work, may hurt material detail, and cannot solve a
CPU-limited scene or the current projection-window bugs. Anno benefit and hardware
capability must be measured; no speedup is claimed. Keep this as a small optional
experiment, not a new workstream before fixing the requested SR behavior.

## 6. Useful smaller pieces and methods that do not close our gap

- **Shader patching/cache:** Luma has actual DXBC instruction helpers and per-shader
  patch callbacks. 3Dmigoto's ShaderRegex is another longstanding way to describe
  shader transformations. Reusing a known shader's recipe can avoid repeatedly
  rediscovering its constants on every frame. Neither provides semantic knowledge
  of every Anno matrix automatically; DXIL and shader-stage coverage are separate.
  Keep a measured, cached known-pass optimization separate from inventing a new
  universal shader translator.
  [Luma helpers](https://github.com/Filoppi/Luma-Framework/blob/6fce340570a4bb6b6704a4ca9e44bb7bfc8cf4ff/Source/Core/includes/shader_patching.h).
- **ReScaleFrame** (`f2772157af96ede1fb6ac9f33ad5ede9841b9b9b`) separates game
  knowledge from provider/presentation ownership. Read its current status before
  interpreting historical plans: it explicitly reports unfinished integrations
  and a secondary-window teardown defect. Root license is GPL3, with separately
  licensed SDK parts. Useful architecture/UI ownership reference, not an Anno
  turnkey or reason to replace our orchestrator.
  [Current status](https://github.com/KillerPixelCrew/ReScaleFrame/blob/f2772157af96ede1fb6ac9f33ad5ede9841b9b9b/docs/current-status.md).
- **OptiScaler, DLSSTweaks, DLSS Enabler and most LukeFZ-style replacement paths**
  start with an existing suitable upscaler/engine contract. OptiScaler's own README
  names DLSS2+/FSR2+/XeSS inputs; that does not include Anno FSR1. Useful for our
  existing-upscaler route; it does not replace the missing generic resolution
  control. [OptiScaler's own description](https://github.com/optiscaler/OptiScaler).
- **CheekyFoveatedDLSS** reduces the DLSS workload in native-DLSS games; its stated
  requirements already include DLSS. It is a backend-cost reference, not the
  absent Anno render-size controller.
  [Requirements](https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS).
- **Feeder proxy proposal:** distinguish implemented code from proposals. The
  inspected PLAN-PROXY-SWAPCHAIN explicitly calls itself design/spike only. The
  actual AIO presentation code is the stronger implementation reference.
  [Plan's status](https://github.com/nicolanonmechanical6684/DLSS5-Feeder/blob/main/PLAN-PROXY-SWAPCHAIN.md).
- **TransparentSR research** concerns splitting rendering/upscaling through shared
  input resources. Its indexed text describes changes in render engines; I did
  not verify a generic Anno injection implementation or public drop-in library.
  The web reader could not fetch the full11.6MB PDF. Do not claim it solves our
  acquisition or resolution control.
  [Paper](https://www.cs.sfu.ca/~mhefeeda/Papers/mmsys25_transparentSR.pdf).

## NVIDIA requirements that matter to these choices

The current guide favors early pre-tonemap placement, matching real raster jitter
and motion conventions, and suggests nearest expansion for G-buffer data needed
by later effects. These support our current early SR work and the Elden Ring
handoff reference. They also explain why an attractive late-output demo alone
does not qualify temporal quality. Streamline standardizes DLSS's inputs; it
does not expose a universal control over every game's internal scene resolution.
[DLSS guide,31March2026](https://raw.githubusercontent.com/NVIDIA/DLSS/main/doc/DLSS_Programming_Guide_Release.pdf),
[NVIDIA integration checklist](https://developer.nvidia.com/rtx/streamline/get-started).

Developer-forum reports include a custom Unity integration fixed by correcting
MVLowRes, vector scale/sign and jitter scale. Another shaking report was resolved
by the author recognizing the vectors were already unjittered. These are useful
isolation patterns, not instructions to flip our conventions without evidence.
[Unity case](https://forums.developer.nvidia.com/t/unity-custom-srp-dlssv3-issues/291381),
[jitter-flag case](https://forums.developer.nvidia.com/t/motion-vector-flag-nvsdk-ngx-dlss-feature-flags-mvjittered-seems-to-work-abnormally/294576).

## Recommended finite next decision

Novelty/provenance: AIO and Luma Prey were already named in earlier COMMS. This
search verifies their source and limits more deeply; they are not presented as
newly invented discoveries. EldenRingDLSS is the strongest new close-match source;
VRS is a distinct additional performance idea. ReScaleFrame and the older
3Dmigoto presentation modes add independent references, not a ready Anno package.

1. Fable should compare the current Anno handoff with `StretchPostInputs` and
   `CorrectPostPassImpl`, using our existing Layer2/private-constant primitives
   where needed. One observed post-DLSS reader chain, not every hypothetical game
   effect. Preserve fresh-frame accounting in fab5928; repeat fallback is visible
   only as a repeat, not a fresh DLSS success.
2. If the packed-camera route continues requiring more matrix discovery, the
   strongest broad fallback is genuine lower game resolution plus a separately
   sized native presenter. Use AIO's presentation/coordinate code selectively and
   keep our temporal inputs/backend. Treat late placement and game-HUD quality
   as explicit prototype tradeoffs; do not promise them already solved.
3. Keep engine adapters where native sizing is available. Do not start another
   general rendering framework, new optical-flow solver or NR/FG integration to
   get the first reduced-resolution Anno image working. VRS is an optional
   separate performance idea, not the completion criterion for DLSS SR.

This report is source/API research. Author-reported successful game runs are
labeled as such; no upstream binaries were run, no new Anno result or benchmark
was produced, and there is no claim of universal compatibility.
