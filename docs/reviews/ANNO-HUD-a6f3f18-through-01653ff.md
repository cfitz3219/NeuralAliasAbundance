# Anno HUD review: tested a6f3f18 and follow-ups through01653ff

Reviewer: Astra. Scope clarified by Opus's553fd7c COMMS request, after Cort
corrected the initially supplied build. Reviewed range be1927a^..01653ff on
claude/native-present. Cort's saved225048 manifest identifies packagea6f3f18,
DLLC74A6619C08C6E02706908438771306426E022A97384CF00132C860511C2D099.
01653ff is the later staged revision, not claimed installed/tested by Cort.
The native-resolution HUD draw-pass proposal is NOT implemented in this range.
Line references below use the01653ff snapshot; the cited mask bodies are also
present in the tested a6f3f18 implementation.

## Verdict

Disabling the mask by default in3e3f9f6 is the right immediate correction for
the observed scenery replacement. Keep the existing scene/DLSS machinery and
NIS component. The reviewed code is an experimental pixel replacement plus a
new diagnostic probe, not actual HUD exclusion or native-resolution HUD drawing.
Do not present it as a finished separator. The concrete corrections below are
finite; no new general renderer, broad review gate or game launch is requested.

## Findings

### HUD-R1: HUD is still included in the DLSS input (functional gap)

live_dlaa.cpp2940 assigns e.color=frame, the finished render-size frame containing
HUD. The dilate pass only zeros motion
where evidence is high; composite around2569/2998 blends NIS's copy of that same
whole frame over the DLSS output. It does not obtain a HUD-free scene or a separate
UI color/alpha layer. In particular, UI edges below the blend threshold remain
in the temporal result. The menu claim at route_a_panel.cpp979, "Keeps ... out of
DLSS", is stronger than the implementation.

This directly explains the observed vibration: if scenery is labelled HUD, the
composite replaces its temporally resolved pixels with spatially scaled current
raw jittered pixels. Opus's reported pink scenery/wobbling live run is consistent
with this exact dataflow; no new camera fault need be invented to explain it.

Correction: keep the approximation explicitly experimental/off. For actual
separation, capture the scene before UI composition or use an existing UI layer,
then compose/rerender identified UI draws after DLSS. Draw at output resolution
to recover native text rasterization; magnifying an already low-resolution HUD
cannot recreate all missing detail. Reuse the observer/PSO metadata, existing
scene selection, native presenter and submitted-resource ownership.

### HUD-R2: old HUD evidence can survive indefinitely after an element disappears

live_dlaa.cpp2529 gates EVERY evidence update, including its negative evidence,
on current 3x3 contrast>0.1. If a previously classified HUD element disappears
and reveals flat scenery, the evidence remains1 even when the pixel difference
is large. The dilate pass then continues zeroing genuine scene motion and the
composite continues replacing those scene pixels. Gaps/toggles reset prev_valid
(3047), not the stored evidence. Camera-cut/feature reset is likewise not used to
clear this evidence in the reviewed path.

Reproduction: scalar replay of the exact shader update with prior evidence1,
current local contrast0, disappearance difference0.8, and camera motion2px leaves
evidence1 after120frames. This is arithmetic/source evidence, not a GPU/game test.

Correction if retaining the opt-in fallback: apply contradictory/expiry evidence
outside the contrast gate; clear/reinitialize evidence after relevant gaps and
resets. Do not continue consuming a mask from the old image state. Include a
HUD-disappears-over-flat-background case in the actual classifier test.

### HUD-R3: probe deduplication loses both command-list identity and UI clip changes

live_dlaa.cpp2819 stores only recording generation. producers.cpp5126 returns
generation+1 scoped to ONE command-list object, so different lists can both have
generation1. The2830 key (generation,RT,DS) merges them. The same key ignores
scissor_serial, rectangle count and every rectangle coordinate, so a new HUD
clip on the same target is silently discarded. It also saves only the first
rectangle's dimensions, losing its screen position.

Compiled reproduction executes the UNCHANGED production probe body with stub
state/resource getters: two distinct lists/generation1 collapse to one record;
a subsequent100x50 UI clip is lost and the log retains1280x720. This is a real
CPU execution of the body, not an actual D3D12 command-list/game run.

Correction: key per-recording observations by (list object ID,generation,operation
index), retain real scissor state changes/full rectangles and viewport, and include
the actual draw's pipeline/depth-enable/blend data when identifying UI. An attached
DSV is not proof that the shader tests/writes depth. Existing observer state has
the required list, pipeline and raster information; extend it rather than build
a second state tracker.

### HUD-R4: the probe reports a recording-time prefix as a presented-frame sequence

hud_probe appends callbacks immediately on recording threads. hud_probe_present
(2835) groups that vector by the wall-clock arrival of PresentUpscale. Nothing
checks whether a recording was closed/submitted, joins submission order or excludes
discarded/future work. A compiled exact-body reproduction records a list and never
submits it; that list still appears in the presented proxy's log. Recording order
can differ from execution order. Thus this log cannot yet select the actual HUD
boundary or prove that no scene drawing follows it.

The128-record prefix limit at2829 also discards the late HUD tail silently.
Reproduction:128 earlier target changes omit a final UI bind9999. Only formatting
is sampled once per120presents; collection/global locking occurs every frame.
Moreover the vector closes only inside PresentUpscale, which presenter.cpp1010
calls only while scaled, so a preceding unscaled/DLAA period can fill the vector
with stale startup data before the first scaled sample.

Correction: retain bounded observations per (list,generation), join only actual
submitted recordings through the existing submission evidence, close samples at
the actual frame boundary, and mark truncation. Preserve the late tail or select
the presented-image chain before bounding. Actual sampling can avoid collecting
unsampled frames. This is a data-fidelity correction to the read-only probe,
not a claim that it currently alters rendering or causes the observed shake.

## Follow-up and evidence limits

- 3e3f9f6 genuinely disables the failing mask/NIS replacement by default. It does
  not implement a different HUD separator. Keep that distinction in status/UI.
- 01653ff stops injected jitter after three consecutive stretched frames. That
  addresses showing raw jitter on prolonged spatial fallback; it does not acquire
  missing scene inputs, remove HUD from DLSS, or establish successful recovery in
  Cort's game. The stated Balanced startup gap remains open.
- Saved225048 ends85242DLSS evaluations plus12980spatial stretches,0DLSS refusals,
 0presenter-kept-old,16349HUD-NIS composites and final HUD-NIS setting0. These are
 cumulative totals, not a claim that every blurry frame was a stretch or that the
 HUD caused the separate Quality->Balanced crash. The latter is not diagnosed by
 adding a crash logger/retry alone.
- j2_hud_nis_test compiles HUD kernels and executes NIS on a two-tone image. It
  never dispatches the HUD classifier/dilation/composite and cannot establish
  correct HUD detection. Add focused actual-kernel cases for HUD over a moving
  scene, scene-only input, and HUD disappearance/reset; no large new test system.
- I found no active callback ownership collision: host_boot makes projection
  window and render-small mutually exclusive. Do not report the single listener
  as a demonstrated current conflict.

No runtime/installer changes and no game action by Astra. Local reproducible
artifacts: local/native-hud-review-20261102 (exact-body CPU probe executable,
source hash, output and classifier arithmetic replay). Review asks for the above
corrections and accurate feature scope, not another independent audit cycle.
