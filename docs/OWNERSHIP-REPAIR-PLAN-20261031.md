# NeuralAlias ownership repairs — implementation plan for Cort's audit

Revision 1 — 2026-10-31. Author: Astra (Codex).

**Current status: review received; Cort authorized only approved/corrected implementation. See revision 2 below for the controlling staged scope.** Original revision-1 proposal follows for provenance. The interrupted implementation turn performed source reads only. No runtime source, build, installer, game file or running game was changed by that turn.

Baseline: source78df593 (documentation after e6b3ac4); rendering behavior b71ba0b. Companion findings: docs/BROAD-NATIVE-DLSS-INSTALLER-AUDIT-20261031.md. Preserve the current user-confirmed Ghost package as the comparison baseline.

## 1. Intended result, in plain language

For each game image, our existing code must agree on who supplies its buffer, who processes it, which queue submits it and who delivers it to the screen. A helper must not change the renderer's preferences or crash-recovery record. A failed menu must not take down unrelated runtime services.

This first milestone repairs ownership. It does not claim completion of every broad-installer finding. We retain NeuralAlias's own host, overlay, DLSS/NR implementation, controls, NVIDIA scheduler integration and reversible installer. The approved UI and Poise remain. We reuse existing code and established Windows/D3D12 synchronization patterns; no wholesale external host, presenter or backend replacement.

**User involvement:** engineering and automated regression work are unattended after plan approval. Cort retains game installation/testing. No game launch/control/deployment or native DLL replacement. No implementation or launcher update before the requested plan audit.

## 2. Boundaries of this milestone

### Included

1. Resolve Streamline/NeuralAlias-presenter buffer ownership consistently across buffer acquisition, index, Present, resize and release.
2. Replace process-wide/device-wide NR route decisions with qualified stream ownership and submitted-work retirement.
3. Separate helper/read-only initialization from renderer-owned settings/session writes, including concurrent processes and crash recovery.
4. Separate runtime/bootstrap ownership from optional menu/input initialization to the minimum needed by these fixes.
5. Add targeted tests of the failing combinations and combine them with existing regressions.
6. Prepare a reproducible candidate package only after the audit permits implementation and automated checks pass; retain the known-working package and rollback.

### Deferred and independently tracked

- Full native FG live-request/confirmation redesign, including games that set options once and temporary Off policy.
- Broad legacy SL-tag normalization, motion/jitter convention conversion and full enhanced-barrier coverage except the minimal identity observation needed below.
- More engine quality-setter families, general game-library UI, DX11/Vulkan runtime integration and new extraction algorithms.
- HDR/composition changes, NR model behavior, sharpening, allocation-pool optimization and cursor repair. The frozen cursor is still a reported issue; this plan does not call it fixed by an ownership change.
- Opus PR72's FG wording is a separate small change. Do not silently merge its inference of an old interface from a zero maximum.

The deferred items remain visible, not declared solved by this milestone. Any required identity observation discovered during the design review must be named and bounded before coding it.

## 3. Existing machinery to extend

| Concern | Existing implementation retained | Planned scope |
|---|---|---|
| Chain/queue identity | overlay/BoundaryRoutes, PresentQueues; slb::PresentEvent proxy_id/epoch; actual creation/ResizeBuffers1 hooks | One coherent choice consulted by all affected swapchain methods |
| Native presentation | overlay/presenter.cpp and NVIDIA's existing native FG | Mutually exclusive ownership for a given chain, without replacing either implementation |
| NR processing | native_fg::Record/RecordFrame, FgContract, FgIdentity/FgFrameCache | Same algorithms; choose the correct stream before calling them |
| GPU lifetimes | RecordingLease, TrackUse, native::Submission, recording reset/destruction/fence receipts | Keep payloads and route generations alive until their real uses end |
| Input capture | sl_consumer's snapshots, NGX input snapshots and existing observer/variant registry | Attach explicit origin/stream evidence; no newest-frame ownership guess |
| Runtime startup | host_boot init worker, hook transactions, module watch and early factory gate | Core readiness independent of menu readiness; no new loader framework |
| Settings/session | settings.cpp, host_boot/session.cpp and existing preference preservation | Separate renderer authority, persistence authority and recovery journal |
| Installation | Existing additive transaction, hashes, receipts and rollback | Deliver tested runtime without disturbing the saved baseline |

## 4. Rules every change must obey

1. Exactly one active presentation owner per chain generation. Buffer substitution and image delivery are inseparable decisions.
2. NR ownership concerns a known rendering stream/view and generation, not all work on a device. Registering an FG handle or seeing tags creates a candidate, not automatic authority.
3. At most one NR evaluation per qualified real image. Generated/retry Presents must not become fresh NR frames.
4. A frame's identity and GPU completion are separate facts. Completion alone does not prove matching picture/guides.
5. Selected ownership survives normal FG suspension, retry and toggles. Missing one frame does not trigger alternating routes.
6. Ownership changes at a defined boundary. Recording/submission receipts retain old generation payloads; CPU Release, a timer or an empty vector is not GPU retirement.
7. Raw pointers are borrowed only within a valid callback/lifetime. Persistent IDs include a generation; address reuse is not stream reuse. COM aliases are normalized appropriately.
8. Unknown association never permits writing on an arbitrary queue or processing an unrelated image. Unsupported/ambiguous cases get a specific per-stream reason, without disabling proven unrelated streams.
9. No new synchronous GPU readback, CPU GPU-completion wait, file I/O or blocking installation in live rendering callbacks. Workers perform process/session file work; test-only readback/waits are explicitly scoped.
10. Runtime settings snapshots can be process-local. Persistent settings and recovery records have explicit owners. A helper cannot save or delete a renderer's state.

## 5. Work package O1 — presentation owner consistency

**Owner after audit:** Astra. **Estimate:** about0.5-1working day including regressions.

### Defect

Overlay native Present bypasses our presenter for a Streamline-owned base, but GetBuffer/GetIndex can still substitute our presenter's resources. This can make the game draw into a buffer whose delivery path no longer runs. Current shipped proxy=0 avoids that combination; the defect is latent and must be reproduced deliberately.

### Implementation design

- Extend the existing per-chain record, with an explicit presentation choice: ordinary native path, Streamline game-facing path, or NeuralAlias presenter path. Include generation/base/proxy identity and existing exact queue binding. No new process-global routing latch.
- Selection must happen **before the first buffer substitution**, not merely when Present arrives. Factory/upgrade hooks associate the actual returned chain; GetBuffer/Index consult that association.
- When Streamline already owns a chain, our presenter must decline buffer/index substitution for that associated base. The menu remains on the verified game-facing image, before the original SL Present.
- If our presenter already supplied buffers before Streamline discovery, do not switch away at that Present and strand those buffers. Keep delivering the current generation through its existing owner. Transition only after controlled resize/recreation has dropped the old acquired buffers and the presenter has retired its work through existing shutdown/resize machinery. If that transition cannot be established, keep the coherent existing owner and report the deferred switch.
- GetBuffer, GetCurrentBackBufferIndex, Present/Present1, latency management, BeforeResize/ResizeBuffers1 and final release must all read/update the same choice. An unrelated chain retains its own route.
- A failed resize preserves the old generation. Successful resize commits the next generation. Base replacement behind the same proxy refreshes identity; final release removes only that chain's record.

### Files and tests

Primary: overlay/overlay.cpp, overlay/boundary_fallback.hpp, overlay/presenter.cpp/.hpp; small sl_boundary association seam if needed, agreed with Opus before edit.

Extend overlay_test and presenter lifecycle fixtures with actual DX12 resources on distinct queues. Test both event orders: SL association before GetBuffer; presenter buffer acquired before SL association. Verify output pixels from the buffer actually acquired, not only Present/draw counters. Include extra native/generated Presents, TEST/retry, resize failure/success, queue replacement, two chains and same-address new generation. Existing WARP and RTX tests must still pass.

**Done:** no configuration can retain our buffer substitution while bypassing its delivery; a transition cannot discard an already-acquired image; one chain's owner cannot suppress another.

## 6. Work package O2 — NR stream ownership and handover

**Owner after audit:** Astra; Opus reviews the identity bridge. **Estimate:** about2-4working days once the bridge below is settled; report a concrete unresolved association instead of guessing.

### Defect

sl_consumer publishes a device claim before queue/constants/input qualification. OwnsRoute uses any SL claim or native context count. Fallback collection and native-FG processing then suppress unrelated work. Conversely, simply deleting those checks can run NR twice or process generated frames.

### Proposed state kept in the existing Stream/Context records

- **Candidate:** observed metadata/feature; owns no image and suppresses no unrelated route.
- **Selected:** a qualified stream route; retains exclusivity through normal input gaps, FG suspension and user NR off/on.
- **Draining:** the generation is ending; no new NR work admitted under it; recorded/executable/submitted uses still retain resources.
- **Ended:** its recording and presentation ownership is relinquished; a reused address receives a new generation.

NR enabled/disabled and per-frame input readiness are separate from ownership state. Turning NR off must not invite another path to enhance the same image. A waiting view must not erase the successful status of another view.

### Required identity bridge — explicit review question, not an assumed solution

A game-facing route key can use native device identity + SL runtime + proxy_id + epoch + viewport. A native NGX FG context is currently identified by NGX core + feature handle; that is not automatically the same key. An SR context has its own core/feature handle. The implementation must bind those namespaces through actual evidence.

Proposed binding order:
1. A directly observed game-facing buffer or guide-resource identity with its frame/recording generation and accepted SL viewport/tag record.
2. If native FG consumes a preservation copy, reuse existing copy/submit observation to trace the relevant recorded copy to that same source and submission generation. Observe only the already tracked candidate resources; no whole-scene graph or pixels/descriptor-name heuristic.
3. Where neither proves a relation, keep the association explicitly unknown. Do not assign a view from 'same device', pointer proximity, latest callback or similar dimensions. Do not enable a new competing route for an image that could already be owned.

**R1 for Opus:** verify whether current hooks/metadata already provide this binding on the supported native paths. If not, specify the minimum extra copy/evaluation/viewport event and exact publication point. Account for asynchronous FG workers, multiple views, reused buffers, current-vs-prior frame and API versions without frame-based tags. We will revise this section before O2 implementation; it is not acceptable to hide the missing bridge behind another global claim.

The conservative unknown case is not completion of universal scoping. Acceptance requires independent streams to continue when their independence is proven, and ambiguous ones to state exactly which association is missing. Known-working Ghost source1 must remain available when source2 cannot qualify.

### Code changes after the bridge is reviewed

- Replace OwnsAny/OwnsDevice as permission decisions with scoped lookup against a presentation stream/image or native feature association. Coarse counters may remain diagnostics only.
- Publish selection only after queue/device, exact frame/viewport, required guides and convention evidence qualify the route. Use a reservation during record/submission so another path cannot race it; commit selection only on successful submission and abandon a failed reservation without consuming the image serial.
- Scope flights and queue handovers to the affected stream; another stream's in-flight packet must not make this one wait.
- Keep producer snapshots on demand for candidate routes when harmless/needed; stop the current global 'any FG context means no SR input capture' behavior. New capture work must remain bounded and preserve existing lifetimes.
- Use existing RecordingLease and native packet receipts to carry old-generation work through reset, resubmit, release and queue replacement. Do not destroy a context's payload just because its public owner was removed.
- Move/restrict last-consumed identity updates to successful record/submission. Never choose a fallback by newest completed input unless matching color/view/frame is independently proven.
- Match runtime/device when setting/shutting down runtime entry points; one core shutdown must not clear unrelated owned streams. Native game ReleaseFeature still forwards exactly once; errors are not converted to fake success.

### Files and acceptance

Primary: nr/sl_consumer.cpp/.hpp, nr/native_fg.cpp/.hpp, nr/nr_pass.cpp/.hpp, NGX observation metadata at exact create/evaluate/release points, narrow SL observation seam. Keep FrameContract/core ABI unchanged unless a separately visible proposal is truly needed; no unannounced core ABI expansion.

Extend native-FG, SL-consumer/NR queue and frame-delivery fixtures. Require: two views on one device; one with missing guide/queue/constants; native FG on one view and SR-only on another; duplicate/retry and MFG inner calls; NR toggle during pending work; FG suspend/recreate; two independent queues; failed record/submit; delayed GPU completion; reset/discard/resubmit; separate NGX cores and one-core shutdown. Check input/output pixels and per-stream successful submission counts. The other view must keep improving, not merely avoid crashing.

**Done:** one incomplete candidate cannot disable a different qualified stream; one real image cannot be enhanced by two routes; selected streams keep consistent ownership during suspension; old payloads retire according to submitted uses; unknown associations remain explicit rather than misreported as successful coverage.

## 7. Work package O3 — process, preferences and recovery ownership

**Owner after audit:** Astra. **Estimate:** about1-2working days including multi-process tests.

### Defect

Every process loading our host can read/write the same game scope. Any existing session marker is considered an earlier crash. One process can remove another's marker. A PID suffix on logs does not fix this.

### Proposed process policy

- Bootstrap may read game defaults/preferences and install observation hooks. Loading our DLL or creating the overlay's dummy device does not establish renderer authority.
- Renderer qualification requires actual application rendering evidence, excluding our injector/dummy calls: native DLSS evaluation on its real device, associated with the real non-TEST presentation stream/window. Keep multiple views in that renderer under the same process authority.
- Qualification queues work to the existing init/control worker. A worker claims a per-user/per-installation scope persistence lease using an OS-held file handle/lock, which is released by Windows on process death. Do not use PID existence as the sole authority or a mutex whose ownership silently ends when a short-lived initialization thread exits.
- Read prior recovery evidence only after acquiring the lease. Record an owner instance ID, PID/creation-time diagnostics and schema version; write atomically. A live competing instance is not an unclean previous run. Unqualified helpers may neither mark failure nor save/reset choices.
- Apply recovery overrides for the elected renderer before it begins our feature work. Observation and the menu can initialize; effect dispatch that needs process authority remains provisional until the worker publishes the result. No disk wait is inserted into a render callback.
- A second independently qualified renderer using the same installation can use process-local settings and its own session journal, but must not overwrite the primary scope's preferences or recovery record. Keep the initial policy read-only for shared persistence; no automatic mid-frame ownership stealing.
- Only the owner may save shared settings or delete its journal on clean exit. Forced termination leaves recovery evidence. A helper exiting cleanly leaves the renderer's journal unchanged. Safe-start overrides remain distinct from the user's saved wishes.
- On authority acquisition, merge/reload current persisted preferences before writing, while respecting explicit local user edits already queued. A stale bootstrap snapshot must not replace newer choices. Legacy marker files with no live lease still trigger the existing conservative recovery behavior; migration is recorded rather than silently deleting historical failure evidence.

**R2 for Opus:** check qualification/startup ordering against saved SR/model/native-FG requests, especially before first Present. Specify how the existing control worker is awakened and which features remain provisional. Avoid enabling saved effects before recovery is known or leaving a healthy renderer waiting indefinitely. Confirm behavior for two real renderer instances, helper-first and renderer-first startup, and same-exe helpers.

### Files and acceptance

Primary: settings.cpp/.hpp, host_boot/session.cpp/.hpp, host_boot.cpp and a small renderer-evidence callback from the existing NGX/SL/host observation path. Use Windows file-lock/process-handle primitives, not a new background service.

Extend session_exit_test and settings tests with child processes and barriers/events. Cover helper-first and renderer-first overlap; two processes simultaneously requesting authority; helper saving/exiting; renderer force-kill then relaunch; clean exit; genuine failed session; same PID/address reuse represented by a new instance token; read-only failure; second qualified renderer; existing legacy marker. Assert exact persisted NR/model/mode choices and which process can remove which journal. Also verify session test setup is isolated under a unique test scope.

**Done:** helpers cannot change game preferences or manufacture/erase renderer recovery; temporary safe-start Off never persists as a user's Off; simultaneous live instances are distinguished from previous crashes.

## 8. Work package O4 — core attachment independent of menu

**Owner after audit:** Astra, shared seams checked with Opus. **Estimate:** about1-2working days; overlaps O1/O3.

- Split the existing overlay::Start responsibilities minimally: core queue/chain/reset/submission attachment remains the existing implementation and is callable without successful ImGui/input setup. Menu/input readiness is a separate result.
- Host boot must start eligible adapters and the NGX logger even if the menu cannot initialize. Preserve the early factory gate as bounded; log expiration and continue late attachment, rather than calling the absence of early capture success.
- Change SL attachment bookkeeping from 'module_seen means never retry' into worker-owned attempt/attached/retryable-failure state, preserving one installer per module. No retry may re-enable dead hook targets or invalidate originals retained after a partial commit. Existing MinHook dead-target policy remains authoritative.
- Preserve already-published working hooks on a retry; retry only unfinished interfaces and log which capability remains unavailable. Installation waits remain worker-only.
- Avoid renaming/rearranging unrelated modules. This is a separation of readiness and lifecycle responsibilities, not replacement of the host.

Acceptance: inject menu creation/input failure while actual queue/NGX/adapter events continue; race module workers; expire early gate; recover from a retryable acquisition failure; ensure fatal partial-commit/dead targets do not retry as fresh; discovery and logging remain bounded/available. Preserve existing boot normal/recovery, wrapper COM dispatch, two-queue and user-input tests.

## 9. Coordination and change boundaries

**Astra proposed implementation ownership:** O1-O4, production worktree/PR59, automation and eventual combined package. **Opus requested role now:** read-only design reviewer; no parallel edits or package refresh. Review can recommend a bounded implementation split later, but a new assignment must name exact files and seams before either agent edits shared files.

Preserve Opus's separate FG UI work. Do not silently broaden this into native FG controls/HDR/engine-locator replacement. Do not start implementing a response to every review idea automatically; reconcile the one review into this plan and make substantive scope/design changes visible to Cort.

Commit independently reviewable increments after meaningful regression success; never overwrite the working Ghost package. No new subagents are proposed. Communication stays in COMMS.

## 10. Verification and delivery sequence after audit

1. Freeze/record source commit, actual installed receipts, runtime/installer hashes and working Ghost behavior. Read only; do not change the game.
2. Add each targeted failing regression, show the existing failure where deterministic, implement the bounded fix, and rerun its focused suite.
3. Sequence: O1 presenter consistency; settle R1/R2; O3 process/session authority; O2 NR scoping; complete O4 separation/retry as dependencies require. Do not use this order to leave a partially published ownership transition in a deliverable.
4. Build full Release successfully before running tests, with and without public SDK headers through existing CI. Run CPU, hook, SL, NGX, overlay, NR, session, settings and live-control regressions. Extend CI only for the small current installer fixtures needed by the candidate; no new testing framework.
5. GPU fixtures run WARP and RTX where applicable. Test-only GPU waits/readbacks are permitted; production rendering callbacks keep none. Injected/fake NR proves orchestration, not NVIDIA model quality or native scheduler behavior.
6. Prepare one candidate from the exact tested build, with code/assets/installer identity, previous receipt discovery, hash validation and reversible additive upgrade. Do not replace native game DLLs. Preserve the existing package/commands until the candidate is ready; no font-only or unrelated preference reset as a side effect.
7. Hand candidate to Cort with a short live checklist: Ghost Home and existing NR toggle/model/mode behavior; native FG on/off and resize/quality-change; then one distinct PBZ integration session. These are user-owned final game checks, not automatic game control or a new review cycle.
8. Record automated pass/skip/failure separately from actual game observations. Any remaining unknown association or unsupported branch is listed plainly. Do not call universal ownership fixed based on one passing game or a count of successful NGX calls.

Estimated combined engineering scope: roughly4-8working days if existing observations resolve R1/R2; extra identity-observation work requires an explicit revised estimate before implementation. This covers ownership, not all remaining installer/engine/convention work. No user attendance during code/automated tests. Plan review and Cort's requested audit happen first.

## 11. Reviewer checklist (Opus)

Please respond in COMMS with PLAN-REVIEW, the reviewed plan revision/commit and a verdict: ready for Cort audit, or needs revision. Rank concrete findings and cite code where the proposed contract fails.

- R1: exact SL-view/native-FG/SR identity bridge, including shadow copies and async workers; no hidden same-device assumption.
- R2: renderer qualification/recovery authority ordering before any saved effect, with helpers and second real instances.
- O1: first GetBuffer versus later Present ownership, late SL discovery, failed resize and already-acquired presenter buffers.
- O2: reservation/commit ordering, failed recording, executable list resubmit, queue replacement, disabled NR, FG suspension and two views.
- O3: OS lease lifetime, crash journal owner, clean exit/force-kill, persistence writes and legacy marker migration.
- O4: exact minimum core/menu split and no retry of dead MinHook targets.
- Lock ordering: no registry/consumer/settings lock held across foreign code or waits, no reverse dependencies with submission callbacks.
- Regression set: can the tests fail the current code and detect wrong final-image delivery, rather than mirror the proposed flags?
- Scope: identify unnecessary machinery and a smaller established solution wherever possible. Core/UI must remain NeuralAlias.

No runtime changes are requested from the reviewer during this review. Do not invent a passing review on an unresolved association; state the specific missing fact and the smallest way to obtain it.

## 12. References and provenance

- Existing audit and current source files cited above are the implementation evidence; old repaired bugs are historical context, not presumed still present.
- Windows file-lock ownership/lifetime: https://learn.microsoft.com/en-us/windows/win32/fileio/locking-and-unlocking-byte-ranges-in-files ; named-object semantics: https://learn.microsoft.com/en-us/windows/win32/sync/using-named-objects . These are established primitives, not an external runtime dependency.
- NVIDIA native/proxy interface contract: https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideManualHooking.md .
- NVIDIA native FG lifetime/Present contract: https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md .
- Existing RecordingLease, PresentQueues and frame identity code supplies the reusable mechanisms; no upstream code or new license dependency has been introduced by this planning document.

## Review and audit log

- Revision1 authored by Astra; Opus review requested through COMMS. No peer review received at publication.
- Cort's audit pending. No implementation approval inferred from elapsed time or lack of a reply.


## Revision 2 — disposition of PLAN-REVIEW 72d3aaf

Cort authorized implementation after the review recorded in COMMS at f3d58d1. That entry is by Claude/Opus, with Fable attribution in its commits. The separately named project-file writeup was not accessible in this checkout/Downloads; no additional approval is inferred from it. The explicit COMMS review controls this revision.

1. **First candidate: O4 + O1 + cursor + FG wording.** Core queue/submission/discovery readiness is separate from optional input/ImGui readiness; independent engine/logger startup continues after a menu failure. Streamline retries unfinished installation groups after acquisition contention, with completed originals preserved and preparation/commit failures terminal. External presentation ownership is observed before new buffer substitution; existing proxy images retain their delivery until successful resize. Failed resize restores exact images/index, and unretired presenter work prevents native resize. Streamline observation never changes its private queue while handing over retained images. No route-table lock spans presenter/COM/consumer calls. Cached GetBuffer access uses a separate short buffer publication lock, avoiding the consumer/presenter lock inversion.
2. **Cursor is included now.** Balance every ShowCursor increment, including the final visible one; force the menu arrow against competing SetCursor calls while Home is open; forward application cursor requests while closed. This fixes reproducible cursor ownership/count defects. Cort's actual frozen-cursor appearance still requires his live confirmation.
3. **FG wording is included now.** Integrate reviewed cd012b1, read the loaded interposer's real version and distinguish forwarding from observed generation. Ghost's 2.4.0 interface requires one generated frame (2x); a zero/absent limit alone does not establish its version. This stage adds no FG scheduler or live multiplier redesign.
4. **O3 is a separate subsequent candidate.** Replace the proposed OS lease/instance-ID/migration design with the review's simpler rendering-qualified per-process marker first. Loading our DLL/dummy/menu does not qualify. No session/settings changes are included in the first candidate.
5. **O2 remains conditional.** Written R1 evidence/answer is still absent in COMMS. Before any NR scoping change, time-box the source=1/2 and unique/ambiguous/none association evidence. Preserve current NR behavior if no exact bridge can be established. No second peer-review cycle and no approval by silence.
6. **Delivery/testing order supersedes section 10.** Deliver this first candidate separately, retaining b71ba0b and existing launcher/package files as the known-working comparison; then O3, then O2 only after R1. The review's proposal to launch PBZ now is superseded by Cort retaining game installation/testing. Automated CPU/DX12 and installer fixture tests are authorized. No game launch/control/deployment/native DLL replacement.

No whole host/backend replacement, new analyzer, extraction algorithms or new UI design is authorized by this revision. NeuralAlias's existing core, NR algorithms, frame/resource lifetime machinery, overlay controls, Poise assets and additive installer remain in place.

### First-stage evidence and limitations

Evidence is recorded in `docs/OWNERSHIP-STAGE1-20261031.md` and local/ownership-stage1-20261031 (untracked test logs). Test-only GPU readbacks/waits qualify orchestration/image delivery, not native game FG scheduling or model quality. Initial engineering failures and corrections remain recorded in WORKLOG; they were never deployed.


### Part 2 received after the first-stage build — d9d6bbb

The reviewer has now posted R1/R2 answers. They refine the staged approval; they do not qualify O2 automatically. A bounded read of existing Ghost sl_boundary/status evidence confirms source1 and `assoc=0/5171/0` (ambiguous), but the SL evidence file stores aggregate shapes/counters, not per-evaluate resource/frame/generation records. The proposed context/view binding therefore cannot be replay-proven from that receipt. Preserve current NR behavior and do not start O2 routing changes from these counters.

R1's proposed narrow evaluate publication is a useful next observation seam. Resource equality must also match the accepted recording/submission generation: allowing either F or F-1, or the latest legacy tag set, cannot by itself disambiguate pooled textures reused between frames. Original tag identities must be captured before our InputOverride/retag replaces them. Async evaluate timing must be observed rather than assumed to run on the game's Present thread. Without exact evidence, keep the context unbound; no device/dimensions/latest-callback fallback.

R2's rendering-evidence ordering is useful, with two corrections for the later O3 stage: a worker wake does not guarantee readiness within one frame, and per-process journal filenames alone do not protect shared settings Save/recovery writes. Preserve the review's light first implementation while making renderer permission to persist explicit. No OS lease/instance-ID framework is being smuggled into the first stage. No second review cycle is requested.

### Remaining approved work completed — O3, 2026-10-31

Cort directed completion after the first-stage handoff. O3 now implements the corrected light renderer-qualified per-process journals, one shared writer, helper/read-only behavior, queued preference saving, fresh untouched preferences, recovery-before-saved-effects and clean-exit handling. See OWNERSHIP-STAGE2-20261031.md for exact policy, evidence and failure behavior. A short election/clean-close mutex plus held per-process journal handles are used; no owner UUID, new worker service or elaborate lease/migration framework. Fable closed first-stage review at COMMS c422a03. All unconditional approved ownership work is implemented; O2 remains intentionally unchanged under its unmet R1 condition. No new review cycle or user game run was requested/performed.
