# Making NeuralAlias adapt across games

Author: Astra. Date: 2026-11-16. Status: source audit and implementation proposal, no runtime changes.

## Scope and conclusion

Cort asked why adding games repeatedly needs rewiring, whether a small AI could choose correctly, and what would make the existing program a coherent adaptive product. Preserve the core and the two product routes. Do not reinstate parked Route B upscaling as part of this work.

Source baseline inspected: `claude/project-thread-3euhfs` at `b36d261488985b96576339e163a31666496d9390`, the current integration branch when inspected. Central coordination baseline: `origin/main` at `9ccf3bb`. The staged Rome II campaign package uses the separately game-tested `2f539a98` binaries. These are different evidence scopes.

This is an architecture/call-path audit with a whole-source inventory (308 files under src), targeted source reads, caller searches and upstream research. It is **not** a line-by-line correctness audit of every source file, a new GPU benchmark, or certification that all games work. No game was launched, controlled, installed or changed. No rendering fix is included.

**Conclusion:** an adaptive program is achievable by completing and joining the machinery already here. There are several existing learners, format handlers, state owners and input providers. They do not yet share a complete description of the image's journey or a common record of what worked. A small trained model can help rank uncertain candidates after the observations are reliable. It cannot repair missing observation, invent an unavailable execution point, or make resource ordering correct by guessing.

## What to preserve

- Two product routes: reuse a game's native temporal inputs; discover and supply inputs when the game does not provide a supported native integration. DX11 and DX12 are providers, not separate products. A non-DLSS game with a compatible FSR/XeSS interface should reuse its inputs before heuristic discovery.
- Existing NGX/Streamline/FSR/XeSS hooks and the common `dlss::EvalDesc` backend. The different input APIs have different conventions, so translation stays at their boundaries.
- Native DX11 rendering with shared GPU resources to the DX12 effects backend, including the x64 companion for 32-bit hosts. Do not return to translating the entire game's renderer.
- Existing camera admission, inverse/positioned-twin/relative-camera rules, jitter paths, motion reconstruction, optical-flow integration, scene-entry and consumer learners, MSAA implementation, captured image analysis, installers and rollback.
- Current pooling, submitted-resource ownership, D3D12MA and background scheduling. NVRHI exists but `gpu/services.cpp:83` currently enables it only for explicit tests because the recorded comparison favored the native path. A common library is not automatically a speed improvement.
- `docs/VARIANTS.md`, the fixtures, saved evidence and GameCheck. GameCheck stays a separate assessment application. It must not quietly become a runtime decision maker or turn unknown evidence into a passing result.

## Findings tied to source

### A1. Adaptation is real, but local to individual paths

`j2/route_selector.hpp` already learns inline versus between-list placement, honors interception ownership, tolerates short observation gaps and relearns after contradictions. `j2/post_entry.hpp` and `consumer_profile.hpp` learn boundary/consumer patterns. `native11/frame_model.hpp` selects a scene with three-frame hysteresis. `providers/camera/camera_admission.hpp` and `relative_camera.hpp` contain shared camera rules. `adapters/mode_control.hpp` tries available quality-control methods, verifies observed render size and persists the working layer.

Therefore a proposal to build a new global learner from nothing would duplicate working code. Keep these mechanisms, give their observations and outcomes a common vocabulary, and extract policy incrementally behind the existing entry points. Quality selection already demonstrates the desired try/check/remember pattern.

### A2. The live scene model cannot describe every rendering pattern

The DX11 model's `Kind` at `native11/frame_model.hpp:113` contains Draw, ClearDepth, Read and CbUpdate. Its Event records `rt0`, depth and RT count, rather than all writes and their corresponding reads. The hook table in `native11/provider.cpp:2437` observes CS SRV bindings but does not model Dispatch and CS UAV output as scene-producing operations.

`AfterLayers` at `frame_model.hpp:665` extends most-drawn selection by finding a later floating-point picture drawn with the selected or second depth. This is a useful behavior-based rule and enabled existing games. It still leaves compute-only lighting, different-sized depth/color, floating-point material layers, and non-floating-point lit intermediates outside that particular inference. This is a demonstrated scope gap in the model, not proof those games all fail through every route.

Correction: allow those existing hooks/models to represent compute writes, all relevant targets, copies/resolves and resource versions. Rank candidates by their downstream contribution to the presented view. Retain current draw-count/format/depth heuristics as cheap candidate ranking. Do not equate a texture format with its semantic role.

### A3. Screen contribution is not wired into the common DX12 route outcome

`j2/route_selector.hpp:68` declares `lineage_to_present`. On executed frames it copies this to `contribution_established` at line 200. At `j2/live_dlaa.cpp:7908-7956`, the caller fills execution, later scene writes, missing camera/depth and interception, but not this lineage field. A source-wide search found no production assignment of `lineage_to_present`.

This does **not** mean the output never reaches the screen. It means this route outcome cannot learn from positive screen-contribution evidence. It also does not justify setting the field true whenever NGX succeeds.

Existing code to reuse: `trace_analyze` at `j2/live_dlaa.cpp:951` already walks writers in submission order backward from the presented target for the native-upscale trace. It handles resource versions and distinguishes several incomplete/inferred cases. Reuse the dependency representation and bounded traversal. Do not enable its entire diagnostic dump or reduced-resolution mutation path during ordinary DLAA.

Acceptance: a completely overwritten result must not count as delivered to the final picture. Tone mapping, blending and HUD composition may transform pixels, so final-image byte equality is not the general test. Distinguish dependency-based contribution from pixel-confirmed visual quality. Use existing captures to corroborate the former where available.

### A4. Some observation streams are explicitly approximate

`providers/discovery/pass_map.hpp:32` scans the first 16 descriptors of each bound table and labels reads approximate. That diagnostic pass map is not a full shader execution trace. The newer DX12 trace has shader reflection and late-descriptor handling, but also tracks untraced/inferred operations. Neither deserves an unconditional complete-graph label.

With Shader Model 6.6, shader-selected heap indices can bypass root descriptor-table mappings. Microsoft documents that mechanism. A learned model cannot recover an unobserved dynamic index from a texture's dimensions.

Correction: retain known/possible/unknown access distinctions. Use reflection and real descriptor-range information where available. Limit deeper instrumentation to a few ambiguous candidates and an explicit acquisition budget. When an existing native upscaler supplies the contract, use it without requiring reverse-engineering of the preceding bindless renderer.

### A5. Ordinary format differences are not handled uniformly

DX12 has `j2/msaa_resolve.hpp:15`, including separate color resolve and nearest-covered-depth handling. Native DX11's `EvaluateFrame` at `native_d3d11/live_dlaa.cpp:293` refuses multisampled inputs, and specifically refuses BGRA and R10G10B10A2 scene color. `TextureFacts` already carries active rectangles, typed views, mip and slice, so a useful description is present.

Correction: a shared table of supported preparation operations should choose resolve, channel conversion, depth extraction and subresource handling from observed facts. Reuse the existing DX12 resolve/math where applicable, but implement and test the correct DX11 sharing/copy/restore operation. An array or MSAA resource cannot simply be reinterpreted. HDR transfer function and exposure cannot be inferred from channel bit depth alone. Preserve the game's representation when copying the result back.

This is a finite compatibility improvement. It makes several new games use an existing capability without a new game-specific patch.

### A6. The original core contract is not the runtime's universal interface

`core/frame_contract.hpp` holds the early portable Frame/Continuity model. The `dlss::FromFrame` conversion exists at `dlss_backend.cpp:72`, but source caller searches found no runtime calls to it. Real routes build `EvalDesc` directly, with substantial validation and lifetime handling in those routes. Native DX11 has its richer `SceneFrame` at `native11/input_provider.hpp:79`.

This is not evidence that validation is absent. It is evidence that passing the old frame-contract test does not establish every live path's semantics. Do not force modern paths through the older, less expressive struct.

Correction: extract a common description from the fields actually used now. Include view and real-frame identity, resource incarnation and subresource, current/previous frame pairing, active rectangle, camera/depth conventions, jitter actually applied, motion source and units, color/exposure, placement evidence and submitted-work lifetime. Keep API-specific execution owners. Backend availability, input validity, successful submission and screen contribution remain distinct outcomes.

### A7. Existing tests are valuable but are not generalization evidence by themselves

`tests/host/native11/frame_model_test.cpp` drives the live model with synthetic game-shaped events. There are provider GPU fixtures and recorded-shape camera/boundary regressions. `scripts/check_variants.py` checks register fields, source markers and referenced file existence. It does not run every listed proof or establish that the right image was modified.

Correction: feed both API providers the same semantic renderer cases, varying addresses, formats, optional passes, draw counts, rotating buffers, resolution, deferred-list placement and queues. Label true scene/HUD boundaries and false candidates. Leave entire renderer families out of any trained selector's training set. Measure wrong-target selection, missed delivery, alternating delivery, longest outage, unnecessary history resets and overhead separately. A single coverage percentage cannot certify stability.

GameCheck can show these results and keep prior runs. Existing shader/image captures remain private local evidence. Do not publish game assets in test fixtures.

### A8. Persist the successful recipe, not stale pointers

Quality control already persists its learned layer. The inspected scene/consumer/camera learners are principally live process state. `profiles/layout_cache.hpp` also already implements bounded, versioned, atomically saved hints, but the caller search found no production integration. Its current schema is executable-hash keyed, permits five signal bindings and demands an exact layout match. There is no unified live saved recipe covering scene source, camera, boundary, preparation and evidence across the two providers. Extend that cache rather than add another persistence mechanism.

Correction: store behavior/structure signatures, schema version, successful strategy and outcomes. Rebind current resources on load. Shader/layout fingerprints can help identify a match, but an exact executable hash should not be the sole admission condition. Device replacement, resolution/format changes, altered shader layout or contradictory dependencies invalidate the affected part. Keep separate learned states for different views/menu/gameplay layouts so ordinary transitions do not repeatedly erase all learning.

### A9. Useful confirmation code exists, but needs real evidence suppliers

`providers/classifier/production_checks.hpp` offers `ConfirmDepth` and `ConfirmPreHudComposite`; source searches found no live callers. `content_checks.hpp` includes checks against independently known world points and composition witnesses. These are valuable reference checks, but an unknown game does not supply those fixture witnesses automatically.

`ConfirmPreHudComposite` explicitly checks a preserved display-space composition source, not the earlier pre-post-processing DLSS input. `temporal_depth.hpp` requires qualified, unjittered camera data and classifies uninformative evidence separately. Those restrictions must survive integration. Camera and depth that agree with each other do not independently prove both are the correct main scene.

Existing live example to generalize: `j2/game_motion_decide.hpp` ranks 12 motion-unit/sign hypotheses by whether warping the previous picture predicts the current one better than zero motion. It already embodies measure/compare/retain/reject. Extend that principle while preserving actual frame identity, occlusion handling and opportunities to say the evidence is uninformative.

## Response to Fable's concurrent plan

Read Fable's 2026-11-16 08:2xZ COMMS entry after completing the first source pass (central main `d55e205`). His watch-only referee, saved-capture replay, measured voting, shared frame description and learned memory are aligned with this audit. This is coordination, not another implementation approval gate. No implementation work is assigned in this report.

Corrections and additions for the plan:

1. Establish a minimal common evidence record before the watch-only measurements. Otherwise they start with two incompatible definitions of frame, source version and convention and need another rewrite at stage four.
2. Do not plug the October checks in as a universal verifier. Preserve their assumptions above. Check picture dependency through post-processing first; restrict direct pixel comparisons to corresponding spaces and observed operations. Correlation by itself does not show DLAA changed the screen.
3. Camera/depth reprojection and jitter need informative samples. Moving objects, occlusions, fog and exposure changes cannot all vote a correct camera down. Static/flat scenes may not identify motion scale. Test wrong-but-plausible candidates as well as the right one, and do not fit and certify a candidate on the same evidence.
4. Add the native DX11 compute-write observation and ordinary format preparation gaps to the plan. A referee alone will identify a problem and still lack an action that can solve it.
5. Use the existing DX12 backward writer traversal for contribution and the existing layout cache for persistence. The cache needs schema evolution and structural revalidation, not its current exact-executable key as a cross-game admission rule.
6. The broad claim that live paths never measure their picks is too strong. Game-motion prediction and several local admission/continuity checks do. The missing element is a shared complete feedback loop, not an absence of all measurement. Likewise a tiny decision tree can explain a choice; explainability does not require ruling out all machine learning. Start deterministic because that is the fastest verifiable integration, and judge a later ranker on withheld renderer cases.
7. Pharaoh can be one fresh integration case, not the sole proof. Existing Anno, both Snipers, WWII, BF4, HOI4 and Rome II shapes must remain covered in replay. Keep native-DLSS ownership cases too. Do not demand a user restart for each internal finding; use the saved evidence and automated fixtures first.

## The smallest useful unification

The logical loop is Observe -> propose candidates -> prepare compatible inputs -> verify -> run -> monitor -> repair the affected choice -> remember. These are responsibilities of the current program, not a demand for eight new subsystems.

1. **Normalize facts and outcomes at the existing provider/backend seams.** Preserve both API executors. Add the missing read/write semantics to the native model, and connect the existing DX12 lineage analysis to an outcome with explicit evidence quality.
2. **Select and compose existing strategies.** A strategy says what it requires, what it can produce and why it failed. Discovery can retry a different camera while retaining the working color and insertion point. A format mismatch selects a converter. A known scene-layout transition selects a remembered strategy. Required missing data remains an acquisition task rather than a permanent off switch. Never fill a new frame with stale scene data merely to make a delivery counter green.
3. **Standardize format preparation and history/lifetime rules.** Normalize input conventions once, convert only when needed, retain submitted work to its real completion, and change temporal generations only for real discontinuities. Restore the game's state exactly.
4. **Replay cross-game cases and retain learned recipes.** First run the existing rules through this seam without changing their output. Then introduce one shared improvement at a time and replay the full set. Do not simultaneously replace camera selection, jitter and insertion logic.
5. **Evaluate a small ranker only after the above produces reliable labels.** It should earn inclusion by selecting better than the existing rules on withheld game/renderer cases.

No wholesale replacement of the hook layer, DLSS backend, companion, installers, existing effects or known-good rendering methods. No new consumer-facing route or second settings system. No cloud inference dependency.

## A small AI is a reasonable candidate

Use a small decision-tree ensemble or compact classifier over existing observations, if it improves selection. Suitable inputs include relative sizes, formats, draw/dispatch patterns, depth relationships, actual resource dependencies, scene-to-present distance, camera/motion agreement and limited asynchronous image statistics. Candidate outputs are a ranked scene/depth/camera/boundary strategy and confidence.

Run it when discovery is uncertain or structure changes, rather than on every draw. The fast path is a learned recipe plus inexpensive consistency checks. Start with deterministic rules through exactly the same interface so a trained model can be compared and removed without disturbing execution.

The model does not author shaders or arbitrary memory writes during gameplay, select resource states by probability, or label its own guesses as training truth. It selects among implemented capabilities. Entire unseen mechanisms still need a new general capability once, but a different combination of known mechanisms should not need a game patch.

A larger model is potentially more useful offline: examining the existing structured capture and shader evidence, suggesting a rule, then generating a replay case for engineering review. RenoDX now exposes this sort of inspection through an MCP devkit. That is a separate developer aid, not a dependency embedded in the shipping DLL.

## Primary upstream research and what is worth reusing

| Project | Verified mechanism | Useful to NeuralAlias | Scope/licensing note |
|---|---|---|---|
| [OptiScaler](https://github.com/optiscaler/OptiScaler) | Existing temporal-upscaler inputs translated to selected outputs, scaling/DRS controls and backend-specific behavior | Compare our input translation and mode-control coverage against its mature paths; prioritize existing interfaces over reconstruction | It requires supported temporal inputs. [README](https://raw.githubusercontent.com/optiscaler/OptiScaler/master/README.md) even records XeSS integrations lacking depth and custom FSR interfaces. [GPL-3.0](https://raw.githubusercontent.com/optiscaler/OptiScaler/master/LICENSE) for copying code |
| [ReShade generic depth](https://github.com/crosire/reshade/blob/main/examples/09-depth/generic_depth_addon.cpp) | Draw/vertex/indirect statistics, aspect tests, depth preservation around clears | Existing ranking source already acknowledged in our frame model. Reuse and compare missing clear/preservation behavior rather than invent another depth selector | BSD-3-Clause in this source. Finds depth, not complete temporal semantics |
| [Luma Framework](https://github.com/Filoppi/Luma-Framework) | DX11 draw/dispatch/state inspection, shader and rendering-pass modifications, game modules | Reference for closing native DX11 observation gaps and making short snapshots informative | [Custom MIT license](https://raw.githubusercontent.com/Filoppi/Luma-Framework/main/LICENSE.md) requires attribution and author permission for commercial use. No wholesale dependency required |
| [RenoDX devkit](https://github.com/clshortfuse/renodx/blob/main/docs/DEVKIT_MCP.md) | Structured frame/shader/resource inspection, named-pipe/MCP bridge, workflow backward from final frame | Particularly useful example for machine-readable evidence and offline AI-assisted diagnosis | [MIT](https://raw.githubusercontent.com/clshortfuse/renodx/main/LICENSE). Runtime still needs its own observations and decisions |
| [DLSS5-Feeder](https://github.com/jlrouzies-fr/DLSS5-Feeder) | Synthetic DLAA contract, D3D11/D3D12 sharing, x64 helper, current built-in flow estimator and external providers | Compare transfer/companion ownership and failure reporting with ours; useful fallback reference | MIT repository code, separate dependency licenses. README explicitly says estimated motion has temporal limitations and HUD is processed. Do not mistake broad transport compatibility for perfect scene understanding |
| [NVIDIA Streamline](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS.md) | Explicit resource tags, matched frame token and viewport, conventions and integration point | Ground the common frame description and translation tests in the actual API contract | Describes what the host supplies, not automatic recognition of an arbitrary engine |
| [NVRHI](https://github.com/NVIDIA-RTX/NVRHI) | Resource/state/lifetime and command recording abstractions | Retain our measured integration, use only where it replaces duplicated work | Does not infer which texture is the scene; no reason to enable extra tracking over existing tracking |
| [RenderDoc scripting](https://github.com/baldurk/renderdoc/blob/v1.x/docs/python_api/index.rst) | Scriptable graphics inspection and replay | Optional offline reference and corpus tooling when capture is already supported | Not a prerequisite for Cort to run more PIX-style sessions and not a replacement for normal injection |

Microsoft's [dynamic resources specification](https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_6_DynamicResources.html) explains why a bound descriptor table alone cannot describe all bindless access. This is an observation limitation to represent explicitly, not a reason to reject all bindless games.

The inspected projects offer strong pieces. I did not find a documented drop-in component among them that discovers all required temporal inputs and an ideal pre-HUD insertion point for every renderer. There is also no basis here to promise that a small AI will do so without the missing observations.

## Success and sequencing

First deliverable: shared evidence/outcomes and an observe-only selector fed by both providers, replaying the already solved renderer shapes. Expected effort is several focused engineering days, not a new engine rewrite; it is an estimate, not a completed implementation. No user attendance needed for source work or automated regressions.

Second deliverable: promote only the decisions that match the known-good cases and fix the known observation/preparation gaps. Prove unrelated views and existing strategies retain their choices, and account for the incremental CPU/GPU cost. Use the existing hook cache and worker machinery; expensive reflection at shader creation, graph updates at submission, deeper scans only while learning. GPU probes are asynchronous and bounded. No new FPS or overhead number is claimed from this audit.

Live success means an unseen combination of already supported patterns works without a source edit, surviving menus, camera changes, resize and window-mode transitions, with output actually reaching the picture. A game can remain rendering while acquisition seeks missing data. Normal play must not keep alternating between incompatible strategies.

Final user testing should be on one coordinated package that preserves the known-good package and rollback. Automated event replay establishes decision behavior, not final game image quality. Cort's visible result remains the latter evidence. No additional review gate is proposed.

## Audit checks and limitations

- Searched runtime callers of FromFrame and lineage_to_present, checked actual provider hook registration, scene-selection code, live input preparation, route state machine and existing trace traversal.
- Inspected native quality-controller persistence, shared resource ownership, GPU services and motion contract; inventoried regression/replay locations.
- Consulted the primary upstream documents linked above. No upstream code copied.
- Attempt to run `python scripts/check_variants.py` did not execute because `python` resolves to the Windows Store alias on this shell. No test pass is claimed. This documentation-only audit does not alter runtime behavior.
- Complete implementation, performance measurement and game validation remain future work. This document records concrete starting points, not a claim that the adaptation has already been implemented.

## Addendum: Fable's camera-optional proposal versus automatic calibration

2026-11-16. Cort asks which proposal is better, whether they are the same, or whether to combine them. Reviewed Fable's 08:26Z COMMS proposal at main 6c5da81 against production source b36d261, NVIDIA's NGX D3D evaluation structures and programming guide, and AMD's optical-flow documentation. This is an architectural audit, not an implementation or a game-quality verdict.

**Verdict: a focused hybrid, starting with Fable's camera-optional motion path.** His proposal removes a concrete dependency that currently stops otherwise usable frames. Automatic calibration supplies selection, observation and remembered choices. The backward image walk is already common to both proposals. Shader analysis/instrumentation is a targeted escalation for unresolved inputs, not a prerequisite for first contact or a replacement for our core.

### A. Camera independence is sound; the existing fitter is not camera-independent

The standard NGX SR/DLAA evaluation structures take color, depth, motion, jitter and associated dimensions/options without requiring a camera matrix. This supports removing camera discovery as an unconditional gate in our direct NGX path. It does not waive Streamline common constants or establish requirements for NR/FG. See [NVIDIA's D3D evaluation structures](https://github.com/NVIDIA/DLSS/blob/main/include/nvsdk_ngx_helpers_d3d.h) and [Streamline common constants](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS.md#60-provide-common-constants).

Current dependencies are concrete:

- `native_d3d11/live_dlaa.cpp::EvaluateFrame` calls `Pose` and returns "Waiting for the current scene camera" before starting its effects path. Its flow runs only after that check and produces a camera-relative residual.
- `core/camera_fit_basis.hpp::MakeFitBasis` consumes composed reprojection, current inverse VP, previous VP and eye position. `camera_fit_gpu.hpp` describes a six-parameter correction from that basis. Near/far are not separate parameters: the projection/depth mapping is already represented by the matrices.
- `j2/live_dlaa.cpp` supplies those camera inputs to the M3 fit. Its `CarriesNoView` trigger does not prove that no camera was found. An axis-aligned real view can also satisfy the diagonal-pattern test. Missing camera admission and a projection-like admitted matrix are different cases.
- `j2/compute_flow.cpp` already computes expanded total image flow internally, then derives a residual against camera reprojection. `motion_arbiter_gpu.hpp::DispatchDeriveResidual` and tier-three arbitration assume that camera term exists. Removing the admission check alone cannot create a valid camera-free route.

**Smallest useful change:** expose measured total current-to-previous image motion with its validity and exact pair/coordinate identity, alongside the existing residual output. Route it independently of a camera into the existing arbiter/backend. Reuse AMD flow, image normalization, depth-aware expansion, GPU ownership and the existing image checks. Native game motion, when independently verified, is another camera-free supplier. Never send a residual as total motion or feed an invented identity camera to satisfy an interface.

Total 2D image motion does not require world position or metric near/far. It still requires the right image pair, jitter treatment, direction/scale, active extent and scene depth/clear convention for DLAA and our expansion. A future relative-camera fit could use a valid projection without recovering absolute world pose. Estimating all missing projection/depth parameters is additional inverse-rendering work; it is unnecessary for the first direct-flow implementation.

AMD's algorithm returns one estimate per 8x8 block. Our depth-aware expansion can separate some boundaries but cannot recover independent motion that was never resolved inside a block. Thin foliage, repeated textures, newly revealed regions, rapid view changes, fog/particles and changing lighting are important limitations. Broad textured strategy-map motion is a reasonable candidate, not inherently a failure case. Keep valid game/camera motion where it predicts better; choose per-pixel suppliers using existing validity rather than globally replacing all camera motion. Source: [AMD FidelityFX Optical Flow](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/optical-flow/).

### B. The backward walk is the correct foundation, not a complete boundary classifier

It can unify the three proposed scene timing variants by describing actual contributing resource versions and operations. It does not, alone, distinguish a finished lit scene from a normal buffer that contributes to lighting, or identify HUD semantics among several later blends. A dependency edge establishes possible contribution, not the desired DLAA insertion point or a pixel-level contribution amount.

Reuse `j2/live_dlaa.cpp::trace_analyze` and connect actual evidence to the router. Native DX11 needs Dispatch/CS UAV read/write observations to represent compute-produced pictures; its current FrameModel only records Draw/ClearDepth/Read/CbUpdate. Do not treat PassMap's approximate first-16 descriptor entries as a complete graph. Track resource incarnation, subresource, write version, active rectangle and submitted execution order, including reused/deferred command lists and relevant cross-queue dependencies. Scope the walk to the active presented view and its ancestors; do not rebuild a full renderer graph every frame.

No proposed backward selector exists yet, so there is no honest measured list of eight games it gets wrong. Known discriminating cases are already available: BF4's normal/material layer versus lit picture; Rome II's layered scene/second-depth composition; HOI4's scene/HUD sharing; Sniper's actual upscale and later HUD boundary. Anno and WWII remain regression cases, not games declared automatically solved. Preserve the native-DLSS ownership route for Ghost/PBZ.

### C. The displayed-picture fallback needs two corrections

**SDR is supported.** NVIDIA explicitly supports appropriately encoded LDR input; display encoding is not itself disqualifying. Set color mode from actual encoding. Do not assume linear LDR, scRGB and HDR display encoding are interchangeable or infer the transfer function solely from texture bit depth. See section 3.1 of the [DLSS Programming Guide](https://raw.githubusercontent.com/NVIDIA/DLSS/main/doc/DLSS_Programming_Guide_Release.pdf).

**Jitter is the larger problem.** `native11/viewport_jitter.hpp::WantsViewportJitter` selects draws by scene depth and depth testing. It does not cover all layers or compute-based scene work. Fable's COMMS account of the earlier Rome II capture reports approximately 15% of the input following the declared jitter. That is evidence against assuming this fallback is already generally available, not a new measurement made in this audit. A final picture can also have undergone TAA, de-jittering, distortion or other coordinate-changing postprocessing. Declared jitter, depth and motion must describe the image actually submitted. Shifting the completed image is not equivalent to obtaining new raster samples from the game.

Use the existing jitter-domain probe and saved image pairs to determine the applicable convention. Prefer an earlier compatible scene boundary when the final picture has already lost jitter. HUD included in temporal processing can shimmer/ghost because it does not share scene depth or jitter. It can be an explicitly lower-quality fallback where the contract holds, not a guaranteed universal floor.

### D. Combine independent capabilities instead of a rigid four-rung order

HUD separation, usable camera motion and object motion are independent. A renderer may offer exact game motion with no readable camera, or a clean pre-HUD image while motion must be measured. Treat them as separately selected inputs, with a stable remembered configuration for each scene/view layout. Camera motion is not automatically better than measured motion for moving objects, and unreliable game motion must not win merely because it is native.

Use the existing history/route generation mechanisms: normalize motion providers into the same convention; retain a working selection through uninformative observations; switch on sustained discriminating evidence. A change of coordinate space, input stage or actual frame continuity needs an appropriate history transition. Do not reset every frame, oscillate between alternatives on single scores, or repeat an old picture and count it as fresh delivery. A lost camera can then switch to a compatible motion supplier while color/insertion remain intact. No alternate supplier can correct missing scene depth or invalid resource ownership by assertion.

The practical order is: camera-independent total motion; submitted image dependency selection with the missing DX11 observations; shared measured selection/persistence; targeted shader analysis for unresolved cases. Reuse the existing richer native SceneFrame/EvalDesc seams rather than routing through the older, narrower core::Frame. Broader native MSAA/BGRA/10-bit preparation remains ordinary reuse work identified earlier, independent of whether camera discovery succeeds.

### Small, decisive automated proofs using existing machinery

1. Extend `tests/host/j2_compute_flow_test.cpp` and `tests/cpu/motion_arbiter_test.cpp` with genuinely absent camera inputs. Use known textured subpixel motion, separate foreground/background motion, disocclusion and jitter-only stationary images. Verify final total vectors and actual temporal output, not merely successful dispatch. Existing tests initialize a camera and therefore cannot establish this property. Reuse `camera_fit_gpu_test.cpp` for the separate projection-known fit; do not call it camera-free proof. A null-camera path that produces no field, doubles jitter or moves a still image falsifies the proposed implementation directly.
2. Reuse `tests/host/native11/frame_model_test.cpp` and the saved BF4/HOI4/Rome II evidence for picture selection. Add explicit compute-to-composite, same-target overwrite, late binding and second-depth cases with known contributing output. A missing recorded edge must report an observation gap, not a successful match. Saved frame logs can answer only the edges they actually recorded; they cannot recreate absent shader accesses. Use the existing scene/jitter GPU fixtures for mixed shifted/unshifted layers. This determines whether the selector and fallback choose compatible inputs without another user run.

These are proposed implementation regressions, not tests run by this audit. An SE5 camera-hidden trial can later demonstrate real-game quality, but switching off a camera admission flag while retaining identity matrices would not answer the camera-free question. No user run requested now; no code/build/installation changed. A live visual result and measured cost are still required before calling the new fallback a delivered compatibility improvement.

### Cort's clarification: eliminate the camera dependency in base DLAA

After the comparison Cort says: "But I want to essentially eliminate the dependancy not reduce it". The proposal must therefore make base DLAA independent of camera discovery, not merely add another camera recovery method. Discovered camera information is an optional improvement and must not gate startup, motion production, continued delivery or history management in the independent path. Direct-NGX DLAA is the scope; other features retain their actual input requirements.

The acceptance case must disable camera discovery and remove matrices, position and camera-derived motion/jitter/reset signals. With compatible scene/depth and actual raster-jitter evidence, the path must produce current-to-previous TOTAL motion from verified native motion or measured image flow, reach the displayed picture, and maintain coherent history without placeholder cameras. Preserve a complete working configuration while independently evaluating optional improvements. Image/depth matching, jitter coverage and resource preparation must acquire their evidence independently rather than indirectly calling a camera-qualified validator. Lack of informative flow is not permission to invent valid motion or use stale frames. Measure image quality and cost; delivery alone is not success.

This also supports Cort's broader goal of avoiding manual per-game rewiring: make selection depend on observed rendering relationships and save reusable structural recipes, not executable names or fixed memory addresses. A new supported combination must work without a source edit. Eliminating camera dependency is a concrete architectural requirement; it does not alone prove that every renderer's depth, final-image composition or shader-driven effects are already understood. Those acquisition capabilities remain part of the proposed shared automatic integration. Clarification only; no runtime work started by this amendment.
