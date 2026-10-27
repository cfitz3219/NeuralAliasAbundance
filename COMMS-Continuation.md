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
