# C9b texture sampling review — 2026-10-24

Candidate Claude67b7744 (PR32), experimental combined merge92e5aad in PR27. Review status: changes required before live consumer use. No production consumer calls sample_request; no game deployment.

Full Release and48/48 local non-GPU-labelled CTests pass, zero skips. RTX5070Ti sample fixture returns correct color64x32/pitch256 and depth32x16 values,0 failures. Logs local/phase3-integration/{build-c9b.log,ctest-c9b-sample.log,ctest-c9b-full.log}. Happy-path execution does not establish adversarial lifetime, ordering or lease safety. Findings below are source review, not deliberately induced GPU failures.

- **C9B-R1 P1** — producers.cpp2825: repeat release of an in-flight token frees GPU objects. First release changes status to CANCELLED/reap; second takes the non-IN_FLIGHT branch. Preserve deferred retirement across every release until fence completion. Regression: stalled fence, twice release, verify objects retained.
- **C9B-R2 P1** — producers.cpp2340–2441: source is not retained through copy completion; raw registry pointer acquisition also needs a lifetime protocol. Signal failure after submitting the host list frees its resources immediately. Retain source and all submitted objects through reliable completion or device removal, including failure quarantine.
- **C9B-R3 P1** — producers.cpp2673/2713: occurrence ends before the copy; other original queue submissions can intervene. Global sample mutex does not serialize original queue calls. State and boundary provenance become stale. Require an ordered original-plus-copy submission protocol and explicit overlap handling, without GPU callback waits.
- **C9B-R4 P1** — producers.cpp1758: truncated barrier arrays,8192-note cap, pending BEGIN_ONLY transitions and skipped unknown lists do not invalidate old state evidence. Track coverage/split/decay eligibility and refuse unproven states.
- **C9B-R5 P1** — producers.cpp2298/2811: READY pointer is returned without a read lease; another service/poll can expire/unmap it during consumer reads. Use owned bytes or an acquired lease that expiry cannot revoke.
- **C9B-R6 P1** — producers.cpp2359–2421: missing subresource range/plane validation, incomplete planar rejection, BC subrect alignment, and non-null box on depth copies. Reject unsupported shapes before submission and use exact footprints. Microsoft's [CopyTextureRegion contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-copytextureregion) requires a null source box for depth-stencil copies. Passing values on this RTX does not establish API validity.

Additional gaps: tokens have no session/epoch scope; observation stop/resync leaves requests active; cancelled in-flight allocations are excluded from the8-sample live cap; completed readback Map can occur during service_samples in a live submission callback. Move completion/readback service into an explicitly owned worker and bound all retained allocations.

Sent exact findings and ownership to Claude through main COMMS, C9B-REVIEW-1. Producer fixes stay with Claude to avoid duplication. Consumer adaptation waits for a stable, safe ownership/state contract. No interface approval or Phase3 closure. Prior PR31 corrections and same-address binding test remain preserved.

## Recheck — 688c0ec / C9B-REVIEW-2

Claude PR33 (5a299fc implementation,688c0ec warning fix) integrated by fast-forward. Full Release and48/48 local non-GPU-labelled tests pass, zero skips. Logs local/phase3-integration/{build-c9b-fixes.log,ctest-c9b-fixes-full.log}. Existing repeated-binding test is preserved. The original R1 repeat-cancellation and R5 borrowed-pointer defects are corrected in inspected paths. Retention after acquisition, failed-signal quarantine, queue locking, coverage gating, format/plane validation and owned reads are present. The updated fixture exercises stalled double-release/source retention, truncated/COMMON evidence, invalid subresource/BC rectangle, expiry and delayed scope invalidation.

Changes still required before live use (source-level findings, not reproduced failures):

- Worker startup2387–2397: detach makes joinable false, so each accepted request starts a permanent2ms worker. A stable singleton guard and explicit shutdown/static-lifetime protocol are required. Module pinning is not static-data lifetime management.
- Source acquisition2450: raw registry QI occurs after forwarding game submission; retaining afterward does not establish safe acquisition during concurrent destruction. Define a lifetime protocol and test release at acquisition, not only after the host already owns the source. Exact triggering possibilities depend on application synchronization/queue coverage; no crash was induced.
- Scope: service_samples/poll/read do not revalidate session/epoch at use. They can act on stale ARMED/READY entries until worker maintenance runs. Add immediate stop/resync/read/service tests, not only WaitDone after stop.
- Typeless depth: depth_format checks typed depth formats only. Partial R32_TYPELESS/R16_TYPELESS resources with ALLOW_DEPTH_STENCIL are not rejected; depth restrictions must consider resource flags. Add refusal tests before GPU submission.

Cross-thread queue interleaving remains untested deterministically. The current fixture does not visibly inject host Signal failure or planar rejection despite broad coverage claims. Findings and non-overlapping producer ownership posted in COMMS. No live sample-request consumer or deployment enabled; additive interface remains experimental. This section supersedes the initial six-finding disposition only where explicitly noted.

## Recheck — 629ad9b / PR34

Integrated Claude PR34 by fast-forward. Pre-submit source references replace post-forward acquisition; scope checks run at acquire/service/poll/read; ALLOW_DEPTH_STENCIL enforces whole typeless-depth copies. Worker count now has a stable started guard and the fixture asserts one worker. These address the acquisition/scope/typeless findings in the reviewed paths. No blanket concurrency or teardown qualification.

Worker lifecycle remains changes-required:
- **Reproduced:** after the worker reaches empty-store wait, request publication never notifies it. New diagnostic tests/cpu/sample_worker_wakeup_test.cpp confirms the first20ms request times out, releases it, allows idle, then observes the second20ms request still ARMED after~1s. RTX5070Ti,1 failed assertion, exactly one worker. No GPU submissions. Log local/phase3-integration/worker-wakeup.log. Build target na_sample_worker_wakeup; optionally register with NA_ENABLE_PENDING_SAMPLE_TESTS=ON. Default passing tests explicitly exclude this known failure until correction.
- **Source review:** WorkerQuiesce abandons waiting after100ms. Timeout does not establish that a live tick has exited; an immortal sample mutex/vector does not protect other host statics it accesses. Require actual quiescence or a demonstrated lifetime design for all touched state; distinguish already-killed OS threads from a still-running slow tick.

Exact reproduction and ownership sent to Claude. No producer edits by Astra. The paused-worker scope tests and typeless-copy tests are retained. No live consumer enabled; promote pending wakeup test when corrected. Prior four-finding status is narrowed to these worker concerns for this snapshot; other previously documented untested concurrency/failure paths remain qualifications.

## Follow-up — 47b1208 / PR35

Claude published PR35 during this turn. Integrated immediately, superseding the preceding pending wakeup status. Request publication now notifies the worker; na.host.sample_worker_wakeup is mandatory (pending option removed) and passes on RTX. Teardown now waits on a worker thread handle instead of giving up after100ms. Full Release/default49/49 local CTests pass, zero skips, including process exit of both sample fixtures. Logs local/phase3-integration/{build-review3.log,ctest-review3-full.log}.

The reproduced wakeup defect is corrected. Normal handle-backed teardown is source-reviewed and exercised by ordinary fixture process exit. Remaining failure-path qualification sent to Claude: DuplicateHandle failure still detaches the thread without retaining a handle, so WorkerQuiesce returns immediately; add an explicit safe failure path. This failure was not injected. Previously tracked deterministic queue interleaving and Signal-failure tests remain separate qualifications. No live consumer/deployment. Earlier four-finding and known-failing-default-exclusion entries are historical, superseded for this head only.

## Follow-up — c15881d / PR36

Reviewed and integrated Claude's worker ownership correction: no DuplicateHandle and no detach; an immortal std::thread retains its original native handle for shutdown. Thread creation failure refuses the request before publication and allows retry. The injected regression reports request refused, live0, workers0, then subsequent samples succeed. This addresses the reported handle-duplication gap in the inspected path; actual OS thread exhaustion was not induced.

Full Release build passes. This time CTest included all GPU-labelled tests:54 registered,53 passed,1 skipped,0 failed (54.31s). All49 non-GPU-labelled tests pass, including mandatory worker wakeup and sample failure/retry. Real DLSS and device-loss tests pass. host_upscaler_jitter_test skips because FSR/XeSS runtime DLLs were not found in its configured directory; it is not counted as passed. Raw logs local/phase3-integration/{build-review4.log,ctest-review4-full.log}. No game deployment or live sampling consumer enablement. Deterministic cross-thread ordering, failed-Signal quarantine and plane rejection remain unexercised qualifications coordinated with Claude. Prior handle-gap finding is superseded for this head.
