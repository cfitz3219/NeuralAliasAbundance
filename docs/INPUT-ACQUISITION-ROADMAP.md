# Broad DX12 input acquisition

2026-10-23 — Codex proposal for joint review. This is an engineering roadmap,
not implemented coverage or a promise that every DX12 game can be supported.

## Goal and limits

Build one injector with reusable observation, acquisition and validation layers,
then engine/game profiles that supply the semantics DirectX does not describe.
Aim to reduce the work needed per title. Measure coverage against an explicit
game/build/feature matrix instead of counting algorithms or successful DLL loads.

In-process hooks can observe resources, commands, shaders and accessible game
state even when a game exposes no public integration API. They cannot guarantee
recovery of an input that was never calculated or preserved. For example, depth
and camera matrices recover static-scene camera motion, but not previous bone
poses for arbitrary animated objects. Two images can also admit multiple depth
and motion explanations. Estimates remain estimates, even when a neural model
produces plausible results. Full SR requires controlling actual scene rendering
resolution; shrinking a finished image does not save its original rendering cost.

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

Start with the recovered motion-arbitration design instead of building another
competing fallback: trustworthy engine vectors first, camera reprojection next,
then qualified camera-relative optical-flow residuals. The surviving contract
explicitly rejects a residual-only fallback when camera motion is unavailable.
Do not silently change it to make unsupported frames look supported. Preserve
per-pixel provenance and invalid masks; a zero vector is not proof of no motion.
Any standalone raw-flow fallback is a separate proposal with its own tests.

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
routes. A claim about **any** DX12 game is not an achievable acceptance test;
expand demonstrated coverage and keep unsupported cases explicit.
