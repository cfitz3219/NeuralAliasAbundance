# Real raster jitter without invented camera motion

## What failed

Cort reports severe back-and-forth shaking with jitter enabled in build32e25dd,
from DLAA through Performance. Disabling jitter stops it. This is different from
the older matrix-writer failure: the new viewport writer really moves the scene.

The independent inputs are not interchangeable:

* R is the physical viewport offset used while rasterising this recording.
* E is the sampling offset used to stabilise an image before optical flow.
* M is real current-to-previous motion, with deliberate jitter excluded.
* The NGX jitter argument describes R in render pixels. It is not camera motion.

An old learned Unjittered verdict survived the change from an ineffective matrix
writer to an effective viewport writer. compute_flow therefore used E=0 while
R was nonzero. AMD saw the deliberate raster movement. The projection-only
camera reconstruction fitted that movement as camera motion. We sent that fitted
movement as supposedly jitter-free M, AND sent R to DLSS. The duplicate movement
is the concrete defect addressed here. Increasing sharpening or changing the
Halton sign cannot repair it.

## Evidence from the actual game

Read Opus's complete record on claude/se5-motion-pkg at c822b3f, including the
withdrawn frozen-frame conclusions. Independently read results
`Documents/NA-se5-results-20261104-054356`, PID44832/build32e25dd, and all five
0542xx/0543xx capture sets. Local analysis and primary NVIDIA guide are under
`local/se5-jitter-repair-20261104` (not committed game assets).

Across18 consecutive pairs in stationary menu set054326, masonry patches in
the EARLY scene_colour follow about0.90-0.99 of the recorded jitter delta. Their
submitted motion follows about0.69-0.86 of that delta despite an unchanged
projection-only camera. Final output still follows about0.60-0.87. These are
regional phase-correlation measurements, not a new engine-camera measurement.
The final colour also contains unjittered menu patches; averaging those with
world patches hides the distinction. The jitter-off set054330 has a final-image
95th-percentile measured displacement of0.011 input pixels across the selected
regions, versus1.156 with jitter in054326. Moving clouds/planes remain in the
captures and are not asserted stationary.

The production-chain GPU reproduction uses the ACTUAL AMD engine, expansion,
camera fitter and arbiter. Identical analytic raster samples, a deliberately
remembered Unjittered verdict, and a projection-only tracked camera produce:

| Input handling | Stationary false motion | Real 6-pixel movement: average vector error |
|---|---:|---:|
| Old cache wins over actual raster writes |0.9911px|1.3263px|
| This repair, exact raster evidence |0.0000px|0.0043px|

This is measured GPU execution, not the NVIDIA model or gameplay. The separate
real-NGX test confirms the existing sign convention: genuinely jittered zone
plate samples score17.68dB against high-resolution truth, versus11.25dB for
bilinear and8.75dB with the opposite sign. It rules out an unsupported sign flip
as our repair. It does not score Sniper foliage or establish a game-quality pass.

## Exact changes

1. `compute_flow::Frame.raster_confirmed` means this source's scene recording
   contains actual viewport writes. Only that provenance bypasses an older
   learned colour convention. It sets E=R immediately for this early image.
   Matrix/unknown sources keep the existing independent image calibration.
   The persisted verdict is not globally overwritten or used to disable jitter.
2. Pairing records that provenance in each retained image. Switching between
   confirmed and inferred acquisition reseeds the pair instead of comparing
   incompatible encodings as real motion. Existing recording leases still own
   images, descriptors and fitted correction through actual submission.
3. Flow expansion converts encoded-grid locations BACK to the raw depth grid.
   For a raw pixel p, grid lookup is at p-E; a grid neighbour's depth comes from
   its centre+E. This fixes the same coordinate mismatch at silhouettes without
   another GPU pass. Default-zero offsets preserve existing NVOFA callers.
4. Camera fitting still reconstructs from raw depth at p and unjittered position
   p-R. Its image validation samples the encoded pair at p-E. Optional
   `FitImages.raster_to_image_px` supplies -E; existing callers default to zero.
   P4-JITTER-1 was acknowledged by Opus in main COMMS d460853.
5. Image acceptance uses that same encoded position. Temporal residual history
   uses the previous RAW raster position, including the known offset change.
   For confirmed, correctly stabilised images, the competing phantom-jitter
   hypothesis is zero: do not misclassify genuine motion matching a Halton step.
6. The first eligible scene draw pins a viewport sample for its command-list
   recording. A Present on another thread cannot change the sample halfway
   through that recording. Reset starts a fresh recording/sample. Non-scene
   draws restore the game's original viewport.
7. Declaration uses actual recording metadata qualified by selected depth.
   The old per-frame REQUEST table is no longer treated as draw evidence.
   Present-time acquisition can instead use the sample from that scene/depth's
   actual submitted recordings in the same closed frame. Its status counter is
   `declared_submitted`, distinct from `declared_list` and `declared_none`.

No second image-resampling "jitter fix" is stacked on the viewport writer.
Existing matrix/resampling diagnostic routes remain bypassed while viewport
jitter is active. No arbitrary new acceptance percentage, smoothing accumulator,
game address, shader hash, queue, callback GPU wait or synchronous readback.

## Automated tests

* Extended `host_j2_compute_flow_test`: the bad path must reproduce false motion;
  the repaired path must eliminate it and retain genuine camera movement. It
  also preserves the existing unjittered-source calibration/object-motion tests.
* New `host_viewport_jitter_test`: actual interpolated raster coordinates prove
  the offset/sign, sample pinning across a simulated concurrent Present, Reset
  selecting a new sample, selected-depth provenance, and restoration on an
  ordinary depth-disabled post/HUD draw. It uses real D3D12 rendering/readback
  inside a test, not inside a live callback.
* Existing GPU camera-fit cases and real NVIDIA DLSS reconstruction pass.
* Final complete Release build/CTest receipt is appended to WORKLOG and COMMS
  after it finishes; do not infer completion from the focused numbers above.

## Integration instructions for Opus

Pick the focused repair commit announced READY in COMMS onto
claude/se5-motion-pkg. It starts at32e25dd and does not replace your core or pull
in the paused HUD prototype. Your c822b3f documentation can stay. Resolve by
preserving these contracts, not by reverting to another guessed sign:

* Keep `viewport_jitter=1`; do not enable jitter_shift on top of it. -1 is only
  the pre-existing deliberately wrong-sign diagnostic, not the recommended mode.
* Do not clear user settings to hide the bug: cached flow/final=2 is an explicit
  regression case. Confirmed early raster writes win locally even with it saved.
* Keep exact R in depth/motion reconstruction and in NGX. Use E only for the
  encoded image/grid mapping. Do not subtract R twice from motion or subtract
  a Halton delta blindly from fitted camera motion after the fact.
* Do not remove the real-motion case to make a stationary case pass. Do not
  replace the metric with Laplacian sharpness (which also counts jagged edges).
* Preserve frame/recording ownership. A frame-table request alone is not proof
  that rasterisation used it. The submitted fallback is evidence from actual
  recorded draws with matching scene depth, not a timer-based assumption.

Run the combined automated suite, stage one additive package with HUD when
that separate handoff is READY, and leave installation/game testing to Cort.

## Honest limits and next live observation

Engineering evidence establishes a real cause and its production-pipeline
regression repair. Cort still verifies the next game run. Useful evidence is
early flow encoding matching the actual viewport samples, near-zero stationary
world motion, stable reconstructed edges and preserved real movement. Compare
world and HUD separately. The final present composite mixes jittered world with
unjittered UI; fixing motion does not magically turn that UI into a jittered
world sample. The separate HUD separation integration is therefore still needed.

This does not prove viewport injection covers every depth-disabled/compute
world pass in every engine. Older clustered history resets and the previously
dark DLAA report are not newly declared explained by this duplication. No reset
policy was loosened without evidence. Current p44832 has14 restarts while the
older p43844 report had124; they are different sessions. No game was launched,
controlled, modified or deployed during this repair.

## Primary NVIDIA reference

DLSS Programming Guide,31March2026, sections3.6.2 and3.7: render actual fresh
sub-pixel samples, provide the current render-pixel offset, and distinguish
jitter-containing vectors with the appropriate feature flag. This project
continues to supply jitter-FREE motion rather than relabel its broken fit as
jittered motion: https://raw.githubusercontent.com/NVIDIA/DLSS/main/doc/DLSS_Programming_Guide_Release.pdf
