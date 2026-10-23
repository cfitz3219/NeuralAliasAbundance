# Universal DX12 input acquisition

2026-10-23 — revised following Cort's direction and Argon's architectural audit.
Universal DX12 DLSS injection is the engineering goal and working design premise.
This roadmap describes the architecture to build; coverage reports describe what
has been implemented and measured so far.

## Goal and approach

Build one injector that obtains or synthesizes the inputs DLSS needs across DX12
games, without requiring hand-written per-game code. Automatically learned
profiles cache verified discoveries; adapters can accelerate acquisition.
Treat each missing input as a discovery or reconstruction task. Continue searching
and testing alternatives while the original rendering path keeps the game usable.

Use a closed-loop process: enumerate candidates, hypothesize conventions, evaluate
geometric and photometric consistency, synthesize missing signals, then measure
reconstruction quality. DX12 resources and passes provide observations even when
the game supplies no semantic labels. Where the engine does not retain a needed
signal, pursue reconstruction, temporal estimation or shader instrumentation.
Maintain source and confidence metadata so the next stage can improve the result.
True upscaling also requires engine render-resolution control; input acquisition
and render-resolution control are both parts of the universal architecture.

## Acquisition methods to evaluate

| Method | Inputs it can supply | Qualification needed |
|---|---|---|
| Existing DLSS/NGX/Streamline, FSR or XeSS call interception | Labeled buffers and frame constants when the game actually invokes a supported version | Exact API/ABI, resource states, dimensions, conventions, lifetime and insertion point; FSR/XeSS expansion is proposed, not recovered implementation |
| DX12 resource and pass discovery | Candidate color/depth/motion buffers, descriptor bindings, PSOs, pass order and CPU uploads | Frame identity, aliasing, command-list generations, actual queue execution, descriptor updates and incomplete-observation reporting |
| Engine/game adapters | Camera transforms, jitter, scene/HUD boundaries, reset signals, TAA and resolution control | Versioned evidence; signatures are hints until validated, and game updates invalidate unproven assumptions |
| Targeted DXBC/DXIL analysis and instrumentation | Additional motion or metadata where required current and previous state can be recovered | Correct shader semantics, deformation/animation history, bindings and validation; no generic shader rewrite is presumed correct |
| Camera/depth reprojection | Camera motion for static geometry | Projection conventions, reverse-Z, jitter, clear depth, disocclusions and per-pixel validity; moving objects need more information |
| Hardware/classical optical flow and learned motion research | Estimated screen-space correspondence | Direction/scale, occlusion confidence, lighting changes, transparency, HUD contamination, latency and GPU cost |
| Learned depth/camera or other image-based reconstruction research | Approximate inputs where native evidence is absent | Temporal consistency and measured downstream quality; never label inferred geometry as engine ground truth |

## Closed-loop discovery and synthesis

1. Enumerate depth, scene-color, motion and constant-buffer candidates using
   formats, bindings, pass topology, clear boundaries and submission history.
   Preserve main-scene depth before a later clear and scene color before HUD
   composition, including separate UI targets and compute composition.
2. Test matrix layout, depth direction, motion direction, axis signs, units,
   jitter and camera-origin conventions explicitly. The search space depends on
   the candidate; it is not a fixed eight permutations. Camera-relative rendering
   requires testing origin deltas alongside matrices.
3. Compare candidate warps against an unwarped baseline over multiple frames and
   independently sampled regions. Combine photometric error with depth consistency,
   visibility, texture support and separation from competing hypotheses. Handle
   exposure changes, animation, disocclusion and repeated textures; low error is
   evidence to accumulate, not a uniqueness proof. Ambiguous candidates trigger
   further observations and experiments.
4. Reconstruct static-scene motion from qualified depth and camera transforms.
   Investigate camera estimation from depth and flow using robust fitting, with
   degeneracy detection and held-out validation. Use independently verified
   rotation-only motion for sky/background regions; clear depth alone does not
   establish that a pixel is a skybox.
5. Fuse optical flow where independently moving content needs it, using a
   disagreement/visibility mask. Luma or depth disagreement can also arise from
   lighting, occlusion or bad calibration, so it is a candidate mask rather than
   proof of object motion. Evaluate raw de-jittered flow as a distinct fallback
   when camera motion is still being acquired, with explicit provenance and
   downstream quality checks.
6. Save validated conventions and layout evidence in automatic profiles;
   requalify after changes and resume discovery when confidence drops.

The recovered arbiter currently accepts camera-relative flow residuals. Extend
its contract with distinct raw-flow and background-motion inputs rather than
misinterpreting residuals as complete motion. Proposed changes and test criteria
are in [the audit response](UNIVERSAL-DX12-AUDIT-RESPONSE.md).

## Common evidence model

Each candidate input needs source/provenance, session/frame/generation identity,
extent/subrect, format/encoding, conventions, readiness and lifetime information.
Record confidence and *why* it is justified: a resource's dimensions alone do not
make it the main depth buffer. GPU completion and object final release are
different facts. Missing observations, descriptor races, bundles, ExecuteIndirect,
bindless access, multi-queue ambiguity or event loss must reduce what can be
claimed, rather than creating a guessed execution history.

Use bounded observations, explicit resource leases and asynchronous snapshots.
Never turn a diagnostic experiment into synchronous GPU readback in a draw or
Present hook. Model queue dependencies and actual completion without assuming
that CPU callback order equals GPU execution order. Keep capture/replay work
separate from the live acquisition path.

Before enabling reconstruction, validate clean scene color, depth, motion,
jitter, exposure policy, output dimensions and the correct insertion stage for
that backend. Camera jitter must reach the relevant rendering transforms; a
post-process shift is not a substitute. TAA replacement/cooperation, HUD,
particles, transparencies, post-processing, dynamic resolution and disabling the
effect all require explicit behavior.

## Experiments and acceptance

1. **Ground-truth fixture:** camera pans, independently moving rigid objects,
   animated geometry, disocclusions, transparent objects, HUD and camera cuts.
   Compare normalized motion and provenance to known truth; measure error and
   invalid coverage by category, not only a scene-wide average.
2. **Native-vector reference game:** use TLOU's native inputs as a comparison
   source for acquisition experiments after validating their conventions and
   matching frames. They are a useful reference, not universal ground truth for
   every material or game version.
3. **Anno discovery:** collect bounded evidence for main scene/depth/camera and
   motion availability. Produce a report of supported inputs and unknowns before
   requesting a reconstruction backend.
4. **Temporal and runtime qualification:** evaluate ghosting, shimmer, cuts,
   disocclusion, HUD damage, HDR/SDR, resize, alt-tab, device loss, disable/restore,
   multiple command lists and swapchains, and sustained play.
5. **Performance:** matched scenes/settings and warm-up; separate base-render,
   reconstruction, discovery, copy and flow costs. Report CPU/GPU frame time,
   pacing, memory, skipped frames and confidence coverage. Generated-frame FPS
   must not be confused with real-frame rate or input latency.

For each game/build/feature report: observed, validated, experimentally estimated,
unavailable, and producing verified current frames. Restore the original path
when inputs become invalid. Frame generation, neural rendering and ray
reconstruction have additional contracts and should not block the first recovered
DLAA/SR pipeline. Source files for them are not evidence of qualified support.

The next milestone is a working, measured pipeline for two different acquisition
routes. Universal coverage remains the goal. Expand the game/build/feature matrix
and adversarial fixtures at each milestone; turn every coverage gap into a
tracked acquisition, synthesis or integration task with a measurable experiment.
