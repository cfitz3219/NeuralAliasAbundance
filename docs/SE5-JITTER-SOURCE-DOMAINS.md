# Flow-stage jitter/source repair, 2026-11-04

This increment fixes the sampling-domain error identified in the SE5 captures. It does not claim a new game result. Cort retains game testing. HUD separation is a separate increment and is not required to consume this one.

## Evidence and cause

In the stationary wall from capture 012612, source displacement was approximately zero, but submitted motion included roughly 0.6 to 1 pixel and the output moved with it. Reconstructing the unfitted camera from captured matrices and depth gave approximately zero motion; every pixel in that patch had camera-only provenance. The fitted camera, rather than an accepted object residual, introduced the false displacement. Moving flow-off examples also showed real 1 to 5 pixel motion with zero supplied vectors, so simply disabling flow would lose useful motion reconstruction.

The source image did not exhibit the jitter it was declared to contain. Shifting that image to remove nonexistent jitter manufactured motion. The early flow source and final NGX colour source must be qualified separately: postprocessing can change the sampling convention.

## Implementation

- Keep expected raster samples separate from measured colour-stage convention. No global raster-jitter disable or sign flip.
- The early stage keeps a fixed-colour-transform raw-grid pair beside the existing stabilised AMD pair. The encoder writes both in its existing dispatch. This avoids calibrating against images already resampled by the assumption under test.
- A small GPU probe distinguishes raster, unjittered and opposite-sign source conventions. It uses source generation, extent and format, two decisive 60-completed-pair windows, and existing recording retirement. It copies 16 bytes of counts asynchronously; it does not wait in callbacks or create a queue.
- Moving/unexplained textured pixels remain in the evidence denominator. A small stationary HUD cannot qualify a moving world as unjittered by being the only retained evidence.
- Once a source convention changes, the flow pair is seeded once on its new grid. Previously encoded and newly encoded images cannot become a false-motion pair. The final NGX history resets once for its independent colour convention.
- Camera-fit and residual hypotheses use the effective encode shift. Residual derivation does not subtract the nominal raster delta again after encoding has stabilised the pair.
- Captures include the exact pre-flow `scene_color`, expected raster jitter, actual final NGX declaration, effective flow encode offset and each source convention/epoch. The status log includes the flow convention and epoch.

`compute_flow::Frame.jitter` remains nominal input. `Ticket.frame.jitter` is the effective encode shift used by that recording. `raster_jitter`/`raster_sample` keep the independent expected raster sample; `source_generation` carries the selected source generation. Consumers must not replace one stage's convention with the other's.

## Automated verification

- Full Release build and complete suite: 199 registered cases, 194 passed, five skipped, zero failed, 316.89 seconds. Skips: optional NR runtime probe, three unavailable 16xMSAA shapes, and the existing upscaler jitter case. This source precedes the last isolated evidence-denominator refinement.
- Final rebuilt targeted set after that refinement: five passed, zero failed, 8.46 seconds: CPU jitter-domain contract, GPU source probe, actual AMD compute-flow pipeline, GPU camera fit and native-HUD/witness integration.
- The GPU probe distinguishes six combinations of source jitter and an independent encode offset. An additional moving-world/fixed-overlay case refuses to let the overlay qualify the whole source.
- The actual AMD pipeline runs 156 stationary frames with alternating nominal jitter, learns the unjittered source, uses zero artificial shift thereafter, and records exactly two history-only frames: initial seed and the single convention changeover. Two subsequent genuinely moving frames retain valid accepted object motion.
- Corrected Opus's phantom-fit test fixture: the bad input flow must stay identical when the extra fitter hypothesis is disabled. Previously clearing the hypothesis also cleared the test's manufactured input, so the before/after comparison was testing different data. No runtime threshold was weakened.

Local evidence: `local/se5-flow-jitter-20261104/flow-final-{build,tests}.log` and `flow-eligibility-{build,tests}.log`. SDKs, game files and captures remain untracked.

## Integration and limits

Opus's motion repair 69e88aa is included locally as cherry-pick 06f5ab9. Do not pick it twice. Take d98ffa4, 5b9b4bb and the following early-flow commit onto a branch that already has 69e88aa; this does not require importing this branch's unrelated paused sizing ancestry. Preserve Opus's later debug and model-choice work when resolving shared-file context.

Calibration honestly describes the selected colour source; it cannot create genuine fresh raster samples where they were never written. The next normal capture now includes the early colour needed to locate where those samples were lost. No pre-tonemap HDR image is substituted directly into final SDR/HUD composition. Depth-grid/encoded-grid correspondence remains part of the coordinated motion work, particularly at silhouettes. This increment does not claim HUD separation, native per-object vector discovery, zero startup acquisition time, or a measured game-quality improvement.

The extra raw-grid images use four bytes per pixel per retained image (about 69 MB for eight 2293x941 images, about 265 MB for eight 4K images), with actual recording leases controlling reuse. The probe examines one in sixteen pixels; extra full-image work is one raw-grid colour write in the existing encode dispatch. Game-time GPU cost has not been measured.
