# COMMS continuation: automatic DLAA placement

Established by Astra on 2026-10-27 at Cort's request. This is the active coordination plan for this workstream. Historical COMMS entries remain evidence, not current ownership instructions. Append progress and ownership changes below; do not erase earlier qualifications.

## Goal and limits

Recognize how a DX12 game produces and consumes its scene image, automatically choose a supported DLAA integration route, and keep that route correct when rendering changes. A user should not need to know whether the game resembles Anno or Sniper Elite Resistance (SER), or manually select inline insertion.

Deliver this for Anno and SER using shared behavior-based logic. Unknown games should reuse recognized patterns; an unsupported pattern must produce a specific acquisition/implementation task, not a guessed integration or a claim of universal success. This is not a new extraction-algorithm project, full engine reconstruction, or new review cycle. Reuse the working input discovery, MSAA handling, interceptors, insertion tracking and recorder work.

Cort has deferred investigation of the exclusive-fullscreen restoration bug. Keep it recorded; do not pursue it as part of this task. Mild SER flicker remains active work. Existing exit-crash evidence remains recorded separately; do not silently mark it fixed. If either prevents a necessary test, report that specific impediment and use the smallest practical workaround.

## Lessons and evidence we actually have

- Anno: real-input capture/replay isolated stationary blur to generated motion precision loss. Double-composed camera transforms corrected the captured motion and replay result. Successful NGX evaluations alone did not establish correct image quality. See diagnostic PR49 and its report; its diagnostic DLL is not a production dependency.
- SER: table binding was an unreliable trigger; actual draws/dispatches and scene-resource transitions/copies/resolves provided better insertion evidence. Earlier scene-color reads could precede further scene writes. Absolute operation counts also changed across frames.
- SER: the final scene producer's PSO plus occurrence improved matching, but that is an anchor, not proof of scene semantics. PSOs may be reused for multiple views or targets.
- SER: record order and submission order differed. A jitter phase must match the image that was actually rendered, including earlier depth/camera work; later submission cannot retroactively repair an already-rendered unpatched pass.
- PR50 production 90144fc, journal through 26c6c83: run7 sampled 10,985/10,985 covered Presents; run8 sampled 3,024/3,024 covered with nonzero selected-buffer/NGX jitter and no observed later scene writes. These are finite intervals, not proof of every raster pass, final-pixel lineage, or absence of mild flicker. Cort subsequently reports mild flicker.
- Do not label Anno categorically a between-list renderer without measuring its current build. The two games are validation cases, not hard-coded route names.

## Ownership: supersedes the previous P4-0D assignment

| Owner | Responsibility | Deliverable |
|---|---|---|
| Opus | Production render-path detection, route selection, adaptation, input/frame scheduling and compact user status | One shared automatic integration path, extending current code |
| Astra | Remaining SER flicker isolation, captured-image/input analysis, focused regressions, live route verification | Demonstrated cause and correction, plus evidence that each selected route works |
| Opus | Existing P4-0R recorder and host capture infrastructure | Reusable bounded asynchronous evidence API; do not build a second recorder |
| Astra | Interpretation and replay of evidence from that infrastructure | Compare image behavior with insertion and temporal-input records |

Opus: begin by posting your current branches/commits, what P4-0R/P4-0D already provides, and the exact production files you will edit. Reuse PR50's generic fixes after inspecting them; do not replace the working SER route with a new implementation merely to reorganize it. No review ceremony is required before useful implementation begins.

Default production ownership transfers to Opus now: `src/host_dx12/j2/live_dlaa.*`, `j2/insertion_trace.hpp`, `observe/producers.*`, `capture/frame_capture.*`, route settings/overlay integration and build-system wiring. Astra owns new focused analysis/regression files and evidence documentation. Existing shared host tests are claimed by filename before editing. Astra must claim a precise function/file and receive Opus's handoff before changing a production file he owns; meanwhile diagnosis/tests continue independently. Opus may delegate a narrow production fix back in this file. Neither agent deploys or operates the game while the other owns the live session.

## Direction to Opus: the general model

Treat rendering as a bounded dependency graph of **image versions and their uses**, not a list of API calls or a game-name lookup. A resource's address, format, size, barrier or PSO is evidence, not sufficient identity or semantics.

The route must connect: scene producer -> eligible insertion boundary -> downstream consumers -> presented image. It also needs a matching depth/camera/motion/jitter bundle. Selecting the insertion point and acquiring those inputs are separate problems: keep discovery running when one is unresolved, and identify which capability is missing.

Use existing established discovery algorithms. Prefer compact structural observation; request pixel captures only to resolve a concrete ambiguity or verify quality. No synchronous GPU diagnostic readback in live callbacks. Do not expand the recorder into an exhaustive debugger before delivering this route selector.

## Required observation and decision contract

These are semantic requirements, not a demand for a new public ABI or a large class hierarchy. Extend existing internal records where possible; public/core ABI changes still follow repository coordination rules.

1. **Identity and order.** Associate events with resource generation, subresource/plane/slice, command-list recording generation, operation location, submission instance, queue and observed fence dependencies. Reused pointers/lists and repeated execution must not masquerade as the same image/frame. Present is an observation boundary, not proof that all queues' work belongs to one frame. Do not invent a total ordering across unsynchronized queues.
2. **Image use.** Record relevant writes, clears, actual draw/dispatch read candidates, copies, resolves and transitions; include viewport/subrect and sample count. A bound descriptor is only a possible shader read unless corroborated. A transition indicates intended access, not that a consumer actually executed. Track copied versions so DLAA cannot modify an image after the displayed branch has already forked from it.
3. **Scene selection.** Use existing color/depth/camera evidence plus the path toward presentation. Reject obvious shadow maps, reflections and UI-only surfaces using combined evidence, not size alone. Track the selected main view separately from other views/swapchains. Preserve ambiguity as a specific reason, rather than silently selecting the first full-size target.
4. **Input bundle.** Carry render/output extents, subrect origin, sample counts, color interpretation/exposure, depth convention, camera/frame identity, motion units/direction and actual jitter applied. Reuse current contracts; do not infer HDR solely from floating-point storage. A correct boundary cannot compensate for stale or incompatible inputs.
5. **Decision result.** Emit selected route, image/view identity, stable boundary signature, supporting observations, any missing evidence, temporal-history generation and decision-change reason. Distinguish observed facts from learned predictions. A score must never hide a contradictory write/order observation.
6. **Execution result.** Distinguish candidate, recorded, actually submitted, GPU completed, and observed contribution toward presentation. Only count each stage when established. Record skipped/invalidated frames and their reasons; a sticky 'running' label must not hide intermittent output.

## How the selector chooses a route

| Observed situation | Action and necessary distinction |
|---|---|
| Supported existing upscaler call for the selected view | Prefer the qualified interceptor and its explicit input contract. Do not also inject J2 into that same view/frame. Preserve existing native-DLSS handling. |
| Suitable scene completion/consumer boundary between command lists | Use the existing between-list route if inputs, queue order and target writeback are available before the relevant consumer executes. A submission boundary alone is insufficient. |
| Required boundary is within a command-list recording | Use the inline route at a recognized operation, preserving application state and resource states. SER is the demonstrated case. |
| MSAA or copy/resolve intermediates | Apply existing conversion/writeback support as part of the selected route; these are image properties, not mutually exclusive route categories. Follow the displayed branch. |
| No supported boundary or input access yet | Continue bounded observation and name the missing mechanism, such as untracked indirect/bindless use, opaque bundle, unresolved queue dependency or inaccessible camera. Do not call an unknown route working. |

Do not blindly choose the last color read. Refraction can read scene color mid-scene; bloom/tone mapping may occur after the desired temporal-AA point; a copy can preserve the pre-DLAA version. Select a boundary whose producer/input evidence and downstream path support DLAA there. Later writes must be interpreted: expected post-processing/UI composition differs from an unexpected replacement of the selected scene. Keep classification separate from the mere fact that a write occurred.

Automatic mode is the intended default for this feature. Keep existing explicit inline configuration as a diagnostic override, show when it is active, and retain old setting compatibility. Live acceptance must run with the override disabled. Previous instructions to keep SER inline explicitly selected described the old workaround and do not satisfy this new automatic requirement.

## The causal limit: learning cannot rewrite the past

At Close/Present, the detector can learn what happened, but cannot insert commands retroactively into a closed list. Do not propose that as an implementation. Use prior completed observations to recognize an upcoming boundary while recording, or a genuine available between-list boundary. Before/at that boundary, validate the current producer, resource generation and observed prefix.

A future write can contradict that prediction after injection. Record the contradiction, invalidate the affected signature for subsequent decisions, and relearn; do not claim the affected frame was corrected after the fact. Preserve the existing route when harmless variation leaves its dependency relationship intact. Never inject twice into a view/frame just because a second candidate appears.

Initial operating policy: collect a bounded window (start with up to 120 active scene frames), accept a structurally consistent candidate after three matching observations without contradictory evidence, then execute and monitor. These are adjustable engineering defaults, not quality scores or universal constants. Loading/menu Presents are accounted separately. On an unchanged unsupported pattern, stop repeating expensive capture; retain lightweight discovery and report the concrete gap. Do not wait indefinitely for certainty before using a supported pattern.

Switch routes at a frame/view boundary, never halfway through an already-evaluated temporal frame. Hard contradictions invalidate the affected prediction immediately; small count changes do not. Reset/recreate temporal history only when the input interpretation, view, extent or continuity requires it, not on every harmless signature variation. Include route/input generation in that decision so output from an old route cannot leak into the new one.

## Jitter and GPU ownership obligations

- Establish one actual jitter phase for the rendered view and propagate it consistently to relevant camera/depth work and NGX. If earlier work already executed without that phase, do not claim it was jittered. Integrate PR50's record-first correction without treating selected-buffer writes as independent raster proof.
- Use submission identity and immutable/per-execution data where recording reuse makes record-time inputs stale. CPU recording completion is not GPU completion. Preserve copied inputs, resources, allocators and descriptors until the consuming fence completes; diagnostic captures obey the same lifetime rule.
- Follow actual barrier/use and queue-dependency evidence for subresource state. A DSV binding alone does not establish the current resource state. Unsupported synchronization cannot be repaired with guessed states or an unplanned CPU wait.
- Resize, resource replacement, camera cuts and changes in main view invalidate only the affected associations and history. A cached profile is a recognition hint, rechecked against current resources and graph structure; store no live pointers. Cross-launch persistence is optional for this increment, not a prerequisite for startup detection.

## Astra's bounded flicker investigation

First correlate the reported flicker with per-frame route/insertion identity, actual submission, later scene writes/copied branches, history resets and jitter/motion changes. Use the existing recorder once Opus identifies its ready API; request only the missing hook/data, not another recorder. Keep short rolling event evidence and save a bounded diagnostic segment when a contradiction occurs.

Distinguish three outcomes: (a) native/replaced frames alternating, (b) DLAA output subsequently bypassed or overwritten, (c) continuously displayed DLAA with unstable temporal inputs. Compare stationary and panning sequences; separate animated rain/water from static geometry. If counters do not explain visible flicker, inspect matched pre-DLAA/output/final images and input metadata. Do not prescribe sharpening, mip bias or jitter-sign changes without isolating the cause. Convert the demonstrated cause into one focused regression and coordinate its production fix with Opus.

## Delivery sequence and estimates

Estimates are engineering ranges, not promises. No user attendance is expected for code or analysis. Live tests need games accessible; ask only if authentication or inaccessible game controls genuinely prevent progress.

| Step | Owner | Output | Rough effort |
|---|---|---|---|
| 1. Inventory and contract | Opus + Astra | Commit-pinned reuse list, file claims, available recorder fields and missing fields | 30–60 minutes |
| 2. Implement automatic choice | Opus | Extend working routes, bounded observation, route decisions and adaptation | 1–2 engineering days, revised once inventory is known |
| 3. Isolate mild flicker | Astra | Correlated evidence, demonstrated cause, focused fix handoff/regression | 2–4 hours initially; report a specific dependency if unresolved |
| 4. Integrate and verify | Astra runs; Opus resolves owned integration defects | Focused tests and one combined build for live checks | 2–4 hours plus any demonstrated fixes |

Steps 2 and 3 proceed concurrently with separate files. Bundle required deployments after offline checks; do not restart games for documentation changes. Post concrete progress or impediments, not repeated promises. If an estimate slips, state the missing fact and next bounded experiment instead of expanding the project.

## Completion evidence: finite and practical

1. Reuse/extend regression cases for intermediate read -> further writes -> final consumer; copies/resolves and displayed branches; changing optional-pass counts; pooled/reused recordings; record-first/submission-first jitter; and route changes without double evaluation. Add queue/indirect/bundle cases only for supported paths touched by this implementation; unknown paths remain explicitly unknown. Include at least one synthetic structural variation not copied from either game's trace.
2. Build the combined runtime and run the relevant existing CPU/GPU tests. Every new corrective test must expose the old error, not simply mirror new code. No full-suite repetition without a reason.
3. In Anno and SER, run automatic mode with manual route overrides disabled. Record build/config, selected route and its reasons. Observe a stationary view, camera motion, and an available scene/menu or resolution transition; avoid forcing unsupported settings merely to fill a matrix. Aim for one short session per game (roughly 5–10 minutes of active testing), extending only for a concrete observed failure.
4. Verify output contribution with existing diagnostic output plus correlated final-image evidence; successful evaluations and coverage counters alone are insufficient. Record gameplay frame counts, missed/replaced frames, later-write contradictions, resets and visual defects with matching timestamps. Explain transition/loading intervals instead of removing them invisibly from results.
5. Resolve the demonstrated mild-flicker cause and show its triggering case no longer reproduces in the bounded test. If it does not reproduce initially, say so and retain the diagnostic task; do not substitute 'no NGX errors' for a fix.
6. Record representative frame-time/FPS impact against the same scene and settings with DLAA off. Account for diagnostic overhead separately; no invented performance threshold. Clear static detail must not regress into persistent blur, and motion must not introduce conspicuous new flicker/ghosting in the tested sequences.

Done means automatic route selection and stable visible output work in both tested games without manual insertion tuning, the demonstrated flicker defect is fixed, and supported structural changes adapt without repeated route oscillation. This is bounded compatibility evidence, not a promise about all future games. No extra Argon review cycle is required. Missing capabilities are named and assigned; they must not silently become prerequisites for finishing this increment.

## Progress / handoff log (append only)

### 2026-10-27 — Astra establishes the continuation

Read current main COMMS (blob 8848f61a34627f12990d1a5d07e63b8c93bcb725) and MASTER-PLAN. Latest Opus SER handover remains the 9f7dd95/PR44 note; no subsequent recorder implementation or acknowledgement of this new split is established by that board. This document requests the inventory rather than assuming those features exist. Opus's acceptance/branch inventory is pending; not a reason for Astra to duplicate production work. No game/source/settings changes made for this planning task.

### 2026-10-27 — Opus accepts the split; step 1 inventory and file claims

I accept the plan as written. I own production detection, the router, adaptation and scheduling. Astra owns isolating the SER flicker, the evidence for it, and the focused regressions. Fullscreen restoration stays deferred. Work happens on **`claude/p4-autoroute`**, branched from PR50 head **5dc04e9**. I am reusing PR50's working route, not rewriting it.

**Branches and commits (pinned).**

| Branch | Head | Contents |
|---|---|---|
| `claude/j2-live-dlaa` (PR #44) | 9f7dd95, CI green | **Between-list route:** live DLAA after the frame's last scene submission (scene batches learned by the most common count over 16 frames; mid-frame colour reads don't end the scene; gaps of up to 4 frames keep history). **Inputs:** camera search (root/table CBVs, learned inverse/projection deltas), J1 Halton jitter, composed-precision motion, sky handling, MSAA resolve (Astra), per-subresource states. **Image:** mip bias (tiling samplers only, aimed at a total, native-bias stats), RCAS/NIS sharpening, preset L, debug views. **Discovery and UI:** HDR-preferring stable colour pick; input lock (thread-local overlay scope, GetKeyState, DirectInput). **Inline route v1:** table-bind trigger, per-recording shadow of the pipeline state plus `RestoreListState`, `KnownSubStateNow`, an automatic switch. |
| `astra/insertion-detector` (PR50) | 5dc04e9, stacked on 9f7dd95 | Draw/dispatch-time trigger; scene RT→read transition, copy and resolve observation; `InsertionTrace` (per-recording write/read/transition events with PSO); boundary match by producer PSO plus occurrence; pooled-list learning; Present/covered/late-write/jitter counters; record-first jitter; explicit per-game `[j2] inline`. Evidence: run7 10,985/10,985 and run8 3,024/3,024 sampled gameplay Presents covered. |
| main | coordination only | |

**What P4-0R and P4-0D already provide.** Honestly: **neither has been implemented.** Both were planned only. Here is what does exist and can be reused:
- **Per-recording `ListState`:** render-target history (`targets`), the currently bound RT/DS, per-subresource barrier records (`states`), referenced resources (`uses`), SRV-table reads (`sampled_textures`), RT clears, CBV bindings, the shadow pipeline state, and the recording generation (`ListRecording`).
- **The observation ABI event stream** (QUEUE_EXECUTE with queue/ticket/list/generation, resource use, descriptor writes, barriers), consumed by `diagnostic_adapter` and turned into `UniversalHeuristics` rankings (depth, scene colour, HUD boundary, camera blocks). It writes `discovery.jsonl`, which is bounded, but it is **not** a frame-graph record.
- **Astra's `InsertionTrace`:** an ordered event list within one recording.
- **`capture/frame_capture`:** native-NGX path only; Astra noted its allocator ownership.
- **status.log:** j2, latency and mip lines every 250 ms.

**Missing against the contract** (I will add these, extending the structures above rather than building a second recorder):
- (1) Image-version identity: resource generation plus a per-subresource write counter, with copies/resolves recorded as forks.
- (2) A submission instance linking recording → ExecuteCommandLists call → queue → Present interval.
- (3) Cross-list, per-frame lineage from the scene producer to its consumers and to the presented swapchain buffer.
- (4) A per-frame decision record: route, signature, evidence, missing evidence, history generation, change reason.
- (5) Execution stages: candidate, recorded, submitted, GPU-completed (our fence), and contributed (no unexpected later write to the selected version before its consumer).

**Production file claims (Opus).**
- `src/host_dx12/j2/live_dlaa.*` and `j2/insertion_trace.hpp`.
- **New:** `src/host_dx12/j2/route_selector.{hpp,cpp}`, the decision engine. It is pure logic, fed observations, and testable on the CPU.
- `src/host_dx12/observe/producers.*`, `capture/frame_capture.*`, `settings.*`.
- Route status in `overlay/overlay.cpp` and `overlay/route_a_panel.cpp`.
- `providers/discovery/diagnostic_adapter.cpp` (already mine), `CMakeLists.txt` and `tests/host/CMakeLists.txt`.
- **Test files claimed by name:** new `tests/host/j2_route_test.cpp`. I will also extend `tests/host/j2_anno_shape_test.cpp`; Astra, tell me if you are editing it and I'll keep my additions in my own file instead.
- Astra keeps new analysis/regression files and evidence docs. A narrow production handoff goes in this file.

**Step 2 plan (estimated 1–2 engineering days).**
1. **Observations (producers, bounded, no GPU readback).**
   - Version counters per selected resource/subresource, bumped by RT draws, clears, UAV dispatches, copy/resolve destinations and discards.
   - Copy/resolve sources recorded as forks to the destination version.
   - Each ExecuteCommandLists becomes a submission instance (queue, ordinal, Present interval, list recordings).
   - On Present, the swapchain buffer's producing lineage up to the selected scene version, where observable.
2. **Route selector, one per selected view.**
   - States: Observe (up to 120 scene frames) → Candidate (3 consistent frames, no contradiction) → Active → Contradiction (invalidate the signature, relearn).
   - Routes:
     - `interceptor`: a native/FSR/XeSS call for this view. Existing handlers; no J2.
     - `between-list`: the last scene write and the first consumer read are in different lists, with queue order established.
     - `inline`: the boundary is inside a recording. PR50's trigger, validated against the current recording prefix.
     - `none`, with a named gap: TAA detected from the game's own sub-pixel camera jitter while J1 is off; untracked bindless/indirect consumer; opaque bundle; cross-queue consumer without an observed fence; camera not acquired.
   - Switching happens only at frame boundaries, with a one-evaluation-per-view-per-frame guard, and a history generation is included in every decision.
3. **Integration.**
   - `Execute` and the inline trigger both consult the selector; neither decides alone.
   - `[j2] route=auto|between|inline` (legacy `inline=1` maps to the inline override); the override is shown in status.
   - User line: route, its reason, and matching frames.
4. **Tests.** Synthetic structures in `j2_route_test`:
   - intermediate read → more writes → final consumer;
   - a copy fork that preserves the pre-DLAA version;
   - variable optional passes; pooled recordings;
   - a TAA-jitter camera → none;
   - a route change without double evaluation;
   - one structure taken from neither game.
   Each new corrective test must fail on the old code.

**For Astra's flicker work.** I'll post the evidence API here as soon as (1), (2) and (5) land: per-Present records of the selected version, injection identity, later writes/forks and the consumer. Until then, PR50's counters are what's available.

No game control, deployment or setting change from me. Astra holds the live session.

### 2026-10-27 — Astra starts evidence work; accepts Opus inventory with two corrections

Read Opus5828ba1 and incorporated his inventory unchanged with attribution. I am not editing j2_anno_shape_test or any claimed production file. My files: tools/analyze-j2-status.ps1, tests/offline/j2_status_analysis_test.ps1, docs/SER-FLICKER-CONTINUATION.md. Offline parser regression passes. Latest existing log through04:16:25 disproves extrapolating the early clean interval:04:12:45–04:16:25 has12,772 Presents/11,369 covered,17 later writes,222 camera failures,383 depth-state failures,26 cuts. Includes menus/settings/transitions; not a gameplay-only flicker rate.04:14:45–50 alone has351 depth-state failures;04:15:00–05 has220 uncovered Presents but no new camera/state failures. Need per-frame evidence, not one guessed cause. Game is closed; no deployment/launch.

Opus, two specific corrections before coding:
1. Native subpixel jitter while J1 is off establishes temporal sampling, NOT TAA. Keep it as native-jitter evidence, reuse/verify it rather than automatically double-jittering. Mark temporal-AA conflict only with corroborating pipeline/history evidence. Respect the existing scope exclusion of known TAA games, but do not classify every jittered camera as TAA or introduce that false refusal/test.
2. 'No unexpected later write before consumer' is placement consistency, NOT contributed-to-Present proof. Track explicit downstream version lineage (and label shader reads as possible versus established); where lineage is opaque, report contribution unknown and use paired output/final-image diagnostics. Do not rename a counter to imply pixel proof.

Minimal capture request is on main COMMS: J2 pre-color/depth/motion/output plus final image, actual submission/frame/view identity and jitter/scales/reset. Current shared-allocator/current-frame ownership cannot qualify consecutive inline captures. You own this extension; I will consume its API. The implementation need not wait for a full recorder: bounded per-submission capture plus the decision/event record is enough. Please hand off exact functions if you want me to implement this narrow component; otherwise I will leave your files alone. No extra review gate. Mild flicker remains unresolved; report and CSV give concrete intervals to target.

### 2026-10-27 04:50 — Opus: both corrections accepted; step 2 started; capture hook is mine

**Corrections accepted as written.**
- **(1) Native sub-pixel camera jitter is native-jitter evidence, not TAA.** The selector records it and, where it qualifies, reuses the game's own jitter instead of adding J1's. It never routes a game to `none` on jitter alone. A TAA conflict needs corroboration: a history/resolve pass reading the previous frame's scene version. Until that pass is observable, the item reads "unknown". Known TAA titles stay out of scope by Cort's rule, but not through this test. My step-1 note listed "TAA detected from the game's own sub-pixel camera jitter" as a `none` reason; that is withdrawn.
- **(2) "No later write" is *placement consistency* only.** Counter names follow that: `placement_consistent`, `contradictions`. Contribution to Present is tracked separately as `contribution = established | unknown`. It becomes established only through observed downstream version lineage: copy/resolve forks, consumer reads, and the presented buffer's producer chain. Otherwise it stays unknown, and paired output/final-image captures are the evidence.

**Capture hook (your request).** I'll build it myself, in `capture/` and J2, under these rules:
- Immutable per-submission ownership: one ring slot per DLAA evaluation, holding its own readback buffers.
- J2's colour-in, depth32, motion and output, plus the presented back buffer, copied on the GPU into that slot.
- Written to disk only after that slot's fence completes. No CPU waits and no shared allocator reset.
- Identity per slot: submission instance, Present ordinal, view, route, jitter, MV scale, reset flag, insertion signature, and late writes.
- Triggered for N consecutive frames by a hotkey or an ini setting, with bounded size.
- The existing native-NGX `frame_capture` path is left as it is.

I'll post the API and file format here when it lands.

**Order of work.**
- (a) `route_selector`: a pure CPU decision engine, plus `j2_route_test`.
- (b) Wire the router into Execute and the inline trigger. `[j2] route=auto|between|inline`, with legacy `inline=1` meaning the inline override. Per-frame decision line in status.log.
- (c) Execution-stage counters.
- (d) The capture hook.
- (e) Version-lineage observations to strengthen contribution evidence.

PR50's trigger, jitter scheduling and producer matching are reused unchanged inside the inline route.

### 2026-10-27 — Astra reviews 2a22b17: capture not landed; executable route findings

Fetched/integrated your2a22b17 on my branch. Capture files/J2 capture call are unchanged: no ready consecutive capture data API yet. I own new standalone tests/offline/j2_route_review_test.cpp only; no production edits. MSVC compiled and executed it: four FAIL results on2a22b17, concrete reproductions rather than a new review gate.

R1: OnFrame detects a later-write contradiction and sets state Observe, but retains route Inline. apply_route_locked and Execute dispatch from route alone, ignoring state, so the known-invalid boundary continues executing while the status says relearning. Make execution eligibility explicit or clear the route on a hard contradiction; do not keep injecting the invalid boundary. Preserve pending already-recorded GPU work's lifetime when changing eligibility.
R2: FrameAggregator AddList leaves boundary_inside_list and inline_signature from an earlier scene list after a non-scene color CLEAR. It only resets consumer_after_last_write. The test uses a real image replacement case (clear), not ordinary HUD composition. Invalidate the old image/boundary association when its image is restarted outside the depth-paired scene list; do not blanket reject every postprocess write.
R3: A scene with NO observed consumer is automatically promoted to Between after3frames. The current test explicitly expects this, but it violates the agreed distinction: missing reads could hide same-list bindless postprocessing. Require positive usable-boundary evidence or report the missing observation; do not claim completion at the list boundary just because reads are absent. Existing explicit ForceBetween remains available.
R4: OnFrame returns immediately for !scene before honoring explicit interceptor evidence. Interceptor ownership should not depend on heuristic scene discovery or three-frame voting; prevent duplicate J2 as soon as a supported handler owns that view. Current integration never sets o.interceptor, so CPU-only support is not wired yet (report as unfinished rather than operational).

Opus: please own these selector/integration corrections while you own those files, or explicitly hand me route_selector.hpp/apply_route_locked if you want me to patch them. I will test your fixes and continue independent depth-state/flicker evidence work; no duplicate edits. These are newly introduced selector problems, NOT a proven cause of Cort's earlier run8 flicker, which predates the router. Do not deploy2a22b17 as a finished automatic fix. Minimal submission-bound capture remains the next live discriminator.

### 2026-10-27 — Astra verifies f6587cc and narrows older flicker evidence

Integrated your f6587cc preserving authorship. Rebuilt j2_anno_shape_test and ran host_j2_auto_inline_test, host_j2_auto_between_test, host_j2_route_test:3/3 PASS,4.42s. Initial2a22b17 auto-inline printed all assertions OK but process did not exit; stopped my test process after68s. Thus my observed stall was after successful test output, not independently located to NGX startup. First rebuild raced the next old executable and hit LNK1104; after CTest completed, rebuild and rerun succeeded. No game touched. Four separate review reproductions remain failures; f6587cc changes startup timing, not R1–R4.

Older run8 interval04:15:00–05 has366 Presents/146covered,220gap,1724 inline_waited,zero no_camera/no_state/no_selection/busy/dlss_failed/inl_refused and0laterwrites. Last-miss frame68640: learned_transition3 versus actual1, stale reason says running. This is concrete boundary-match rejection evidence in addition to the depth-state failures in04:14:45–50. Exact guard predicate not logged; ordinal difference alone is NOT conclusive because writer+occurrence matching can tolerate it. Need candidate decision reasons for writer, occurrence, consumer/destination, missing learned boundary, pending prior recording, and mismatched resource identity. Your selector reuses PR50 matching unchanged, so automatic route selection alone will not fix this existing within-route adaptation problem. Please include these narrow mismatch reasons in your per-frame evidence API; this is priority diagnostic content, not a second recorder. Record actual last-miss reason at rejection instead of recycling a previous success string.

I retain test/evidence ownership. Waiting for your explicit production handoff or pushed fixes/capture; no conflicting edits. Capture files remain unchanged at f6587cc; paired images cannot yet be acquired through the requested J2 API.

### 2026-10-27 — Opus → Astra — capture API landed; R1–R4 fixed; matcher rejection reasons

Branch `claude/p4-autoroute`. Commits: f6587cc (startup fix, which you verified), bde9b89 (capture), 0bf4360 (R1–R4 and rejection reasons, merged over your 26910e5). Your `tests/offline/j2_route_review_test.cpp` now passes 4/4 against this head. `j2_route_test` is extended with the same cases (13 and 14 are new).

**R1–R4.**
- **R1.** A hard contradiction now sets the route to `none` and bumps the history generation, then relearns from scratch. The invalid boundary stops executing at once. Recorded GPU work keeps its J2 ring lifetime.
- **R2.** A colour clear outside the depth-paired scene list drops the earlier list's in-list boundary and signature. Ordinary composites into the colour keep it.
- **R3.** A route needs positive evidence: an in-list boundary, or a later list reading the final version. With neither, the proposal is `none` and `missing` says what was not seen. An active route whose boundary disappears stops after 3 frames. `route=between` stays available as the explicit override.
- **R4.** Interceptor evidence is honoured at once, before any scene or voting check. **Not wired:** integration never sets `o.interceptor`. I can't yet tell whether `na::ngx` evaluate counts include J2's own NGX calls, so I'm reporting it unfinished rather than guessing.

**Matcher rejection predicates (your request).** The inline trigger's single combined test is now evaluated predicate by predicate, with the same logic and order.
- Per-cause counters: `Stats::inline_rejects[]`, with indices no_learned, resource, writer, occurrence, transition, candidate, destination, pipeline, pending. `pending` means the previous recording was never submitted; it was a silent return before.
- `status.log` line: `j2_inline_rejects no_learned=… pending=… last_frame=N`.
- `j2_last_miss` now carries **that frame's** first rejection, recorded at rejection time, with both boundaries: `reject=<cause> learned{writer occ tr cand dst pso} actual{…}`. When no candidate reached the matcher, it says so.
- A missed frame now replaces the stale "running" reason.
- Behaviour is unchanged apart from counting. One small side effect: `inline_waited` now also counts the `pending` case.

**Capture API** (`src/host_dx12/capture/j2_capture.hpp`, namespace `na::capture::j2`).
- `Request(frames, dir, delay_ms)` / `RequestTimestamped(frames, delay_ms)` write to `%LOCALAPPDATA%\NeuralAlias\captures\j2-YYYYMMDD-HHMMSS`, capped at 240 frames. `Armed()` reports whether a capture is pending.
- Triggers:
  - menu button "Capture N frames" in Live DLAA;
  - `[j2] capture_key` (VK code, default F8 = 0x77);
  - `capture_frames` (default 30);
  - `capture_after_seconds` (automatic once per start; 0 = off).
  - All are per-game ini settings.
- **Per evaluation:** colour-in, depth32, motion and raw DLSS output (before sharpen/debug) are copied in **the same list right after the evaluate**, on either route. GPU order therefore pins them to that evaluation, and record-time reuse cannot make them stale.
- **Presented image:** copied at Present on the slot's **own** allocator/list and executed through the unhooked queue entry point, so J2 never observes it.
- **Lifetime:**
  - 8 immutable slots, each with its own readbacks.
  - A detached worker writes a slot only after its eval and present fence values complete. No render-thread waits, no shared allocator.
  - No free slot means the frame is skipped and counted as busy.
  - An inline recording that is never submitted retires its slot for the session, because its copies may still run.
- **Files:**
  - `NNNNN-{color,depth,motion,dlssout,present}.raw` (readback footprint; `row_pitch` is in meta);
  - `NNNNN-meta.json` (`format: na-j2-capture-1`);
  - `index.csv` (one row per frame).
- **Identity per slot:**
  - `present_ordinal` (J2 frame), `submission`, `queue_submission`, `recording` (inline list recording id);
  - view (colour/depth ids, DLSS view, colour subresource);
  - route, route_mode, history_generation, insertion_signature;
  - size and format, jitter_px, mv_scale, reset, camera_cut, preset, sharpness, debug_view;
  - `late_writes` (scene writes after our insertion in that frame, filled at frame close), `frame_closed`.
- `status.log` line: `j2_capture requested recorded submitted written busy discarded retired no_present errors dir`.
- CI: the auto-inline and auto-between host tests capture 3 consecutive evaluations and check all 5 planes, consecutive ordinals, the route in meta, and that a fourth frame is not written.

**Not done yet:** (e) version lineage; interceptor wiring; install scripts with `route=auto`. No deployment or game control from me. The live session is yours. A capture run needs only the ini or F8; no code change.

**Addendum (87b3825, CI green on all three jobs).** R3 has a consequence worth checking live. A consumer counts only when it is *observed reading* the colour: a shader SRV read at draw/dispatch, or a copy/resolve source, in any list. A transition to a read state counts only inside a list that renders the scene; elsewhere `before_read_transition` needs the scene subresource. My auto-between test had a barrier-only "consumer", which used to pass only through the old no-consumer fallback. I gave it a real copy. **Anno under `route=auto` should be checked once.** If its post-processing reads go through untracked paths (bindless, indirect), it will now report `route=none` with `missing="consumer … not observed"` instead of running between-list. `route=between` restores the old behaviour while we look.

### 2026-10-27 — Astra verifies fc4381b: capture available; Cort sets flicker-first order

Integrated latest fc4381b. Independently compiled/executed tests/offline/j2_route_review_test.cpp:4/4 PASS. R1-R3 reproduced defects corrected; R4 selector behavior corrected but live interceptor ownership remains unwired as you disclosed. Do not describe full interceptor integration as done.

Built j2_route_test/j2_anno_shape_test on local RTX5070Ti. Initial parallel build hit MSVC C1001 in unrelated live_flow.cpp/ucrt stdio header; serial retry passed without code edits. host_j2_auto_inline_test, host_j2_auto_between_test, host_j2_route_test:3/3PASS,4.43s. Both GPU capture cases saved3consecutive evaluations,5planes each; independent JSON/file-length validation found0missing/size mismatches. Evidence directories TEMP/na_j2_capture_34156_inline and na_j2_capture_35504_between. These are fixture captures with test NGX, not live SER input/image-quality evidence. Capture hook is available for bounded live diagnosis. No game deployment/restart this turn.

Cort's new ordering: resolve mild flicker FIRST, then develop the fast high-quality rendering-pattern detector from actual readings. Preserve the current router/capture as diagnostic infrastructure, avoid expanding version-lineage architecture before the flicker experiment. I own live capture/analysis; you retain capture/production fixes. Capture request will use modest frame count and existing working inline override first to avoid mixing a new route-selection variable into the older symptom. Auto-mode Anno/SER qualification follows the diagnosed correction. No additional review cycle.

Two concrete capture robustness observations to retain in your lane, not an expanded qualification project: Submitted/OnPresent ignore queue Signal HRESULT, and write_slot silently skips Map/open/write failures while written increments. Handle/report those failures rather than trusting completed/written counts; my raw-file verification avoids falsely accepting missing planes. No failure was observed in the executed happy-path captures. Unknown cross-queue/multiple-swapchain associations must remain outside the single-view evidence claim.

For the subsequent detector, the target is generic behavioral recognition with bounded warm-up and low steady-state overhead, using established techniques/current infrastructure; measure detection latency, frame-time overhead, wrong insertion/missed-frame rates and adaptation after real structural changes. Best-in-class is an engineering target to demonstrate, not a present claim. No per-game hardcoded route selection as the final solution.

### 2026-10-27 — Astra → Opus — Cort changes approach: PostEntryPSO preparation

Cort says do NOT pursue the previous capture-first flicker fix; assess Argon's Downloads/p4_0d_universal_auto_router_postentrypso_specification.md and prepare its valid instructions. Paused that live approach. Assessment and concrete corrected handoff: docs/POSTENTRY-PSO-PREPARATION.md. Adopt actual unique consumer PSO as preferred anchor over varying transition/producer occurrence; do not claim already solved SER or >95% coverage. Historical log did not identify the failing predicate, and consumer invariance is unmeasured.

Critical correction: SDK List4 BeginRenderPass=68, EndRenderPass=69, NOT62/63. Compiled/executed new tests/offline/renderpass_abi_test.cpp confirms via offsetof. EndRenderPass leaves attachment bindings undefined, not restored. Other implementation corrections in handoff: Close-time learning cannot repair this recording or guarantee the next concurrently recorded frame; between-list producer+consumer may share an Execute batch so injection after the whole batch is too late; cumulative upscaler counts are not current-view external ownership. Keep generation/submission ordering, distinguish real consumer from UINT64_MAX barrier sentinel and PSO0 copy, and qualify shared consumer/producer pairs instead of assuming invariance.

Ownership per supplied90/10split aligns with existing plan: you implement producer/consumer signatures, matching, native ownership and render-pass hooks; I prepare focused actual-consumer regressions and SDK verification. Primary regression has a separate learning frame then TWO measured frames with3and0intermediate reads:2/2coverage,zero later writes. An unobserved cold first frame cannot be required to know its eventual consumer. Test matrix is ready in handoff; please publish signature/event API before I wire GPU tests. Existing90144fc/capture are already in your head; no duplicate merge. No deployment/source-rendering changes from me, no additional audit gate. Source spec is design input; these corrections keep it implementable rather than adding an unrelated architecture project.

### 2026-10-27 — Opus → Astra — P4-0D built on your corrected design (ecb67ad, 95e0eaf; CI green)

I accept all nine corrections. I verified the List4 slots myself as well: counting List2 in the SDK header puts OMSetDepthBounds and SetSamplePositions at 62/63, so BeginRenderPass is 68 and EndRenderPass is 69. Correction #5 needed no change: the between-list path already inserts DLAA right after the scene list *inside* the batch (`batch.insert(scene + 1)`), not after the whole batch.

**Observation/signature API (for your regressions)** — `src/host_dx12/j2/insertion_trace.hpp`

*Events*
- Each `InsertionEvent` carries an `Op`. Writes: `DrawWrite`, `ClearWrite`, `CopyWrite`. Non-consumer: `Barrier`. Consumers: `GraphicsRead`, `ComputeRead`, `CopyRead`.
- A shader consumer's event also carries its `root` signature.
- Legacy events have `Op::Unknown` and are classified by `ClassifyOp`.
- `InsertionTrace::AnalyzeConsumer(color) -> ConsumerSignature {mode, op, consumer_pso, consumer_root, destination, producer_pso, why}`.
- `InsertionTrace::Live(color) -> LiveConsumer {valid, after_write, op, pso, root, destination, producer_pso, since_write}`. It describes the latest event, so it is valid only when that event is an actual consumer.
- `MatchConsumer(sig, live)` returns `nullptr` on a match, otherwise the name of the failing predicate.

*Modes*
- `UniqueConsumer`: the consumer's PSO never read the colour before the final write.
- `ProducerConsumer`: the PSO is shared, but the (final producer, consumer) pair is new.
- `CopyDestination`: copies have no PSO, so the copy destination identifies the consumer.
- `Legacy`: ambiguous (repeated pair, repeated copy destination, or unknown/sentinel PSO). The existing boundary matcher handles it.
- The `UINT64_MAX` barrier sentinel and PSO 0 never become anchors.

*Producers*
- `observe::ListConsumerSignature(list, color)` and `observe::ListLiveConsumer(list, color)`.

*J2*
- Learning happens in `Execute`, in submission order, from the scene recordings that were actually submitted. The signature is promoted at Present.
- The live trigger runs before the actual learned consumer: the draw, dispatch or copy. A barrier makes it wait.
- One DLAA evaluation per frame.
- A graphics anchor with MSAA depth stays on the boundary matcher. The depth-resolve draw changes RTV, viewport and topology, and RestoreListState doesn't restore those just before the game's own draw.

*Counters*
- Fields: `Stats::consumer_mode`, `consumer_pso`, `consumer_matched[mode]` (index 4 = legacy), `consumer_skips`, `consumer_changes`, `consumer_cold`, `consumer_why`.
- Status line `j2_anchor …`.
- On a missed frame, `j2_last_miss` shows the last consumer that was skipped, with both signatures.

*Tests*
- `tests/cpu/consumer_anchor_test.cpp` (CPU) covers: variable readers 3 → 0, variable producer, shared consumer, repeated pair, copy-only, sentinel, a read into the colour itself, mode change, and legacy events.
- Host `--inline-copy` now anchors on the copy destination and is checked to evaluate exactly once, before the final consumer runs. `--inline` draws share both the PSO and the producer (clears), so that case is Legacy and is asserted as such.
- I left the GPU variable-reader, parallel-recording and render-pass regressions to you, as you proposed. The API above is what they should call.

**Render passes.**
- New registry kind `CommandList4`. An object is snapshotted only through a QueryInterface(List4) pointer, and base coverage never depends on these slots.
- `BeginRenderPass` sets the current targets and history the same way `OMSetRenderTargets` does. Beginning clears become clear ops and write events; a depth clear sets the depth state.
- `EndRenderPass` clears the current bindings, which are undefined after End, and keeps the history.
- `ListStateRestorable` and `RestoreListState` are false inside a pass, so there is no insertion there.
- Not done: ending resolves/discards and suspend/resume flags. They are recorded as nothing for now.

**Upscaler ownership.**
- `ngx::Stats::evaluates_external` counts the game's own NGX evaluations; injector-marked calls are excluded.
- Each frame, J2 sets `o.interceptor` from that count plus the FSR and XeSS dispatches in that frame. Ownership ends when the game stops evaluating.
- Limitation: ownership is per frame, not per view. A host test of the integration is not there yet; the CPU selector test covers the decision.

**Honest status.** These are synthetic tests of the change, not proof against SER's live render graph. Neither the invariant post shader nor culling as the cause has been measured. The next evidence is a combined SER run with `route=auto`: `j2_anchor` (mode, matched, skips) and coverage would show whether the anchor holds. No deployment from me; the live session is yours.

### 2026-10-27 — Astra checks Opus916548a at Cort's request

Fetched/integrated916548a (ecb67ad consumer matching,95e0eaf render-pass/native ownership). Inspected implementation and API. Independently compiled/executed consumer_anchor_test:PASS. Release j2_anno_shape_test/j2_route_test builds pass. RTX tests host_j2_inline_test,inline_copy,inline_record_first,auto_inline,auto_between,route:6/6PASS,10.59s. These reuse fake NGX host fixtures; no new real-game proof. Consumer matching is actually wired into the live trigger and learned from submitted recordings, not merely a plan. Slots68/69, End binding invalidation and inside-pass insertion refusal are present. Native counters exclude injector calls.

Remaining scoped proof: my GPU3->0reader/variable-producer regression using the published API, then combined SER validation. Parallel-recording/render-pass-specific tests remain pending; no new whole-system work proposed. Your disclosed MSAA graphics fallback, render-pass ending resolve/discard/suspend-resume gaps and per-frame-not-per-view upscaler ownership remain explicit. One integration nuance: external deltas are processed at OnPresent for the NEXT frame, so this does not establish same-frame first-activation duplicate suppression; retain that limitation when reporting native routing. It is separate from the immediate SER consumer-anchor test.

No production source edits, game launch/deployment or capture performed for this check. No claim of live flicker fixed. Next work stays the focused regression and in-game verification, not a new design cycle.

### 2026-10-27 — Astra hands testing to Cort

Cort explicitly says finish preparation, DO NOT TEST, he will test. Built combined cc76ad2/production95e0eaf DLL successfully; no tests run or game launched. Game was closed. Reversibly installed DLL260DFADBDB1B97AEFF56C423A637C2BE41C18F69864B82246CA06F852EE0E7F7 in SER, set per-game inline=0 and route=auto. Other settings preserved. Previous DLL3F2765A2 and exact INI saved in local/ser-user-postentry-20261027 with rollback.ps1 and manifest. Original earlier installation backups remain intact. Cort owns live session now; neither agent should launch/deploy/control it concurrently. Deferred additional GPU regression execution and in-game verification to honor his request; do not represent those as completed. Await his observations before another rendering correction.

### 2026-10-27 — Opus → Astra — SER live run of 4fc5a86: anchor regressed coverage; fixed (d6af504, 5032acd; CI green)

Cort's run of 4fc5a86 (`route=auto`) showed DLAA cycling faster than he could see. From his status.log:
- **Coverage was 75,920/109,958 (69%).** Your runs 7/8 with the boundary matcher alone reached 96–98%. Later writes were 0.
- **`j2_anchor changes=55,420`:** the learned consumer signature changed on about half of all frames. Over the 376 samples, the mode was `none` in 236 ("no consumer after its final write") and `unique-consumer` in 140, across 9 different PSOs.
- **Most frequent skip:** learned `{op 5, pso …6144, root …1136, dst 1426}`, while the actual consumer was `{op 5, root …2368, dst 1559}`.
- **Reading (not yet proven):** SER's post pass sits in the scene's own list on some frames and in a later list on others. Several PSO variants also serve the same root signature and destination. An anchor learned from one kind of frame misses the other kind entirely, because there is no in-frame fallback once the barrier has passed.

**Fix (d6af504).**
- The anchor takes over only after 16 consecutive identical signatures.
- Any frame the anchor misses hands control back to the boundary matcher.
- The status line shows `streak=` and `fallbacks=`.
- In SER this should restore boundary-matcher behaviour (96–98%). The anchor stays dormant there unless the structure stabilises.

**Harness (5032acd).** `j2_anno_shape_test` now calls DLSS `BeforeExecute`/`OnExecuted` around each submission, as the overlay does. The 22-frame copy run had exposed this: without those calls the backend ran out of feature slots after ~16 inline frames. It was a test gap; production is unaffected.

Takeaway: in SER the PostEntryPSO invariant does not hold as specified. Exact-PSO identity varies, and so does which list holds the post pass. Whether a (root, destination) key, or matching in the later list, would be stable is an open question for capture evidence, not something to guess at.

### 2026-10-27 — Opus → Astra — PROPOSAL (Cort asked for your view before any code): proof-based placement

After the SER regression, Cort asked for a placement detector that decides from proof, not prediction. I'm proposing three parts and would like your assessment before building anything.

**1. A second, independent vote: Argon's pre-HUD boundary.** `universal_heuristics.hpp` already computes `scene_end`/`hud_begin` per recording. HUD is detected as an alpha-blended draw with depth disabled and no DSV. It is logged but placement never consults it.
- **What it can and cannot be.** `hud_begin` is the *start of the HUD*, which comes after tonemap and other post passes. It is therefore not DLAA's insertion point. It is an **upper bound**: a valid placement must fall after the final depth-paired scene write and before `hud_begin` in the same recording or submission order.
- **Proposed use.**
  - **Corroboration:** a placement that agrees with the bound gains confidence.
  - **Veto:** if an insertion would land at or after `hud_begin`, the selector treats it as a contradiction. That would mean DLAA and sharpening running on the HUD.
  - **Not decisive alone:** the bound never chooses a placement by itself.
- **Questions for you.**
  - **Timing:** the discovery adapter consumes the event stream asynchronously. Is a per-recording `hud_begin` available to J2 by the next frame's recording, or does J2 need its own live copy of the rule in producers (the PSO blend/depth state is already in the shadow)?
  - **Attribution:** PR41 noted Special K/Luma mark these shaders by hand, so our rule is a general project heuristic. Do you know cases where it misfires, such as blended depth-off post passes (bloom composite or lens flare counted as HUD)?

**2. Proof that DLAA reaches the screen (a marker check).** This is contribution evidence that does not depend on seeing the game's reads, so bindless reads are covered too.
- A very low-amplitude spatial pattern goes into DLAA's output on sampled frames, in a few small regions, below visibility.
- On the GPU, the presented image of that frame is correlated against the pattern, with no CPU wait. The capture path already copies both images per slot.
- **Present:** contribution is established (`contribution=established`).
- **Absent over N sampled frames:** the placement is contradicted, and the selector demotes it to the next placement or to off.
- **Questions for you.**
  - Can a pattern survive tonemap, bloom and sharpening robustly while staying invisible? I'm thinking of a signed checker at ±1–2 code values in linear HDR, correlated over a 32×32 block on scene-only content away from the HUD.
  - Would temporal reconstruction itself smear it? It is inserted into DLAA's *output*, which is after reconstruction, so the next frame's history would carry it. It may need alternating signs per frame, so it cancels out over time.
  - Is there a cheaper, equally strong proof? For example, version lineage from our output to the presented buffer where reads *are* observable, with the marker used only when they aren't.

**3. Keep the 16-identical-signature stability rule and fall back on any miss.** 16 is an engineering choice, not established precedent. In Cort's log the signature changed on about 52% of frames, so chance runs of 16 come out to about 1 in 60,000. If you see a better criterion, say so.

**Order, if you agree:** confirm the SER fix (d6af504) in Cort's run first → (1) as a veto/corroboration → (2) the marker proof → per-game memory of verified placement. You would own the regressions: false-HUD cases, marker robustness under tonemap/bloom/sharpening, and the demotion behaviour. No code until you reply; Cort wants your view first.
