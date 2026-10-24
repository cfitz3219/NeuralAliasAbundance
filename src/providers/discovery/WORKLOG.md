# Discovery reconstruction journal

## A2 — 2026-10-23 — consumer contract and host requirements

After A1's implementation was pushed, its Windows validation and PR creation
remained blocked. Reported that in COMMS on main (`137b170`) and proceeded to
independent A2 under the master plan's blocked-task exception. Created
`astra/a2-discovery-contract` from updated main rather than modifying A1's branch.

Read frozen ABI 0.1 and surviving overlay execution/discovery, variant-registry,
adapter bootstrap and swapchain lifetime contracts. Wrote CONTRACT.md specifying
consumer framing, bounded ownership, inventory versus execution versus content
evidence, epochs/loss/resync, descriptor/queue ambiguity and callback lifetime.
Explicitly recorded that B-003/B-004 are not reconstructible agreements, while
B-005…B-007 have partial surviving source references only. No lost agreement or
approval was invented.

Defined A2-R01 (C7 upscaler evidence), A2-R02 (C8 pass/binding/state observations
and async resource samples), and A2-R03 (C9 upload/snapshot readiness and leases).
These request a subsequent host proposal, not an approved ABI amendment. Listed
negative cases for future consumers and clarified that content classifiers need
actual qualified samples, not only metadata. No runtime code, frozen interface,
root build file, Claude-owned source or game file changed. This is construction
of the assigned specification, not a pre-gate peer review.

Validation: checked referenced source paths/symbols, the local Markdown link and
diff whitespace. No executable test is appropriate for this documentation-only
task, and no application behavior is claimed from it. Requirements and delivery
status are posted to Claude only through COMMS on main.

## 2026-10-24 — A5 started: metadata candidate ranking

Cort directed "Start Phase 3" after Gate 2 closure on both sides. Read current task queue, COMMS, AGENTS, CLAUDE, collaboration rules, handoff, journal, recovery notes and A2 contract. Updated main to 68c88c8 and created isolated astra/a5-signal-classifier worktree. No delegation, frozen ABI/core changes or game operations. Argon remains deferred.

Implemented first A5 slice in providers/classifier/signal_candidates.hpp: worker-side bounded metadata ranking for depth/pre-HUD scene color, explicit candidate/unavailable confidence and reasons/acquisition tasks. Validates session/epoch/submission/queue/list/recording, live resource/content identity, external work, complete loss-free unambiguous coverage, render extent and content write window. Ranks format/write role, boundary position and clear/HUD observations. Never selects or qualifies a resource. Equal scores remain ambiguous; duplicate identities invalidated; >256 observations rejected visibly. Scope deliberately excludes content/camera/HUD verification and live host connection.

Fresh standalone CPU Release build with VS18/MSVC19.51.36260.0 passed /W4 /WX. CTest 11/11 passed with zero skips, including the new classifier regression executable (11 internal cases) and existing fixture v2 reference tests (10 internal cases). Build: primary checkout build/a5-cpu, test evidence build/a5-cpu/Testing/Temporary/LastTest.log. Initial and final builds succeeded; no test failures. git diff --check passed. No GPU/device-removal test was needed for this pure metadata module.

This is A5 in progress, not its >=95% fixture classification acceptance: current tests exercise metadata abstention and ranking, not content classification accuracy. Next: fixture candidate corpus/independent content verification and acknowledged C8/H2 ingestion; qualified async texture snapshots and camera evidence remain acquisition dependencies. Scene/HUD boundaries are required observations, not magically inferred here. Full A5 planning estimate remains 4–8 working days; this slice needs no user attendance. Gate 2 remains closed.

## 2026-10-24 — A5 content corroboration and anonymous fixture corpus

Cort instructed autonomous continuation without recurring audit-status mentions. Read Claude's C8-H2 proposal and acknowledged additive 0.2 event families with required descriptor-range/completeness, fingerprints and record-time-version qualifications in COMMS/COLLABORATION (343a8f8). No frozen ABI edits by Astra. Merged current main coordination into A5 branch.

Added portable content_checks.hpp. Depth corroboration uses independently known world geometry plus camera, checking raster position and normalized forward/reverse depth; candidate-derived geometry would be circular and is explicitly prohibited. Color corroboration uses independent known linear source-over compositions, rejecting malformed/stale evidence and detecting HUD contamination. Opaque pixels remain unresolved until another same-scene-version witness exposes them. Results are consistency with supplied evidence, not live trust/feature activation.

Added 6 C++ negative-path cases and anonymous C++/Python fixture transport. 396 candidate decisions across 36 base scenes plus 8 opaque-HUD/wrong-camera cases matched expected verdicts. Includes shadow/clear/wrong-depth-convention, scene/composite/postprocess/history decoys, shuffled anonymous IDs, camera cuts and three extents. Full CPU/fixture Release suite 13/13 passed, zero skips; no initial failures. Explicit second controlled translucent-HUD probe provides content visibility absent from baseline opaque HUD; this is synthetic evidence, not a claim of live host acquisition. No end-to-end A5 accuracy claim.

Checkpoint complete; A5 live integration remains dependent on C8/C9 and asynchronous texture sampling. Proceeding with independent A6 camera candidate scanning under the task-queue dependency exception, with no attendance needed. No game operations or GPU diagnostic waits.
