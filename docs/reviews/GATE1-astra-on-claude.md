# Gate 1 — Astra review of Claude's Phase 1 work

2026-10-23. Reviewed #3 C1 at df35b3a, #4 C2 at 5fb3a1b, #6 C3/C4 at 5e80ad7, and #7 C5 at fad26a8. Locations below refer to these branch snapshots, not main. Verdict: request changes on #6 and #7 before closing Gate 1. No additional blocking findings in C1/C2.

## R-201 — blocker (P1) — PR #7 — scripts/deploy-sl-proxy.ps1:122

Rollback deletes an INI the deployment never installed. Deploy without `-Ini` records a null `ini_sha256`. If the user subsequently creates `NeuralAliasSL.ini`, rollback's `-not $m.ini_sha256` condition authorizes deletion of that unrelated file. Redeploy has a related ownership problem at lines 84–85: it overwrites a modified INI when `-Ini` is supplied, or adopts the current file's hash when no INI is supplied. A later rollback consequently deletes user changes.

Only remove a file recorded as installed by this deployment whose current hash still matches. Preserve or explicitly reject modified/unowned configurations during update; do not adopt their hashes merely because they exist. Add regressions for no-INI deploy → user-created INI → rollback, and installed INI → user edit → redeploy → rollback. Both must preserve user content.

## R-202 — blocker (P1) — PR #6 — src/host_dx12/ngx_observe/gpu_timing.cpp:181–186

The single 24-byte `memcpy` does not provide a coherent snapshot of memory the GPU is writing. A legal execution reads the old timestamps first, then the GPU resolves new timestamps and publishes its marker, then the CPU copies the new ticket. The comparison passes while `data[0:2]` still contain the old timestamps. MARKER_OUT orders GPU writes; it cannot retroactively order CPU reads that already happened. Map does not wait for GPU completion. Thus R-001 is not fully resolved.

Establish completion before reading timestamp payload, using a documented CPU/GPU synchronization protocol (prefer polling a fence associated with the actual submission, without blocking live callbacks). If retaining markers, document and implement their CPU visibility/ordering and split-ticket atomicity requirements before copying the payload. Add a test that polls while completion is in flight; the current submitted-list tests call `WaitIdle()` before `Poll()`, excluding this interleaving.

## R-203 — should-fix (P2) — PR #7 — scripts/deploy-sl-proxy.ps1:82–87

Updating a deployed proxy replaces the DLL before recording its new hash. If copying the optional INI fails, or writing the manifest fails, the disk contains the new proxy but the manifest still records the previous proxy. Both another deploy and rollback then refuse that file, even though the script installed it and the original is intact. The initial deployment's write-before-mutation protection is missing on updates; direct Set-Content also risks a truncated manifest.

Journal the update with old/new hashes before mutation, use atomic manifest replacement, and make interrupted updates recoverable without treating unknown files as owned. Add fault-injection coverage after proxy replacement and during manifest publication; rollback must still restore the recorded original.

## R-204 — should-fix (P2) — PR #6 — tests/host/test_support.hpp:35–41

`WaitIdle()` ignores the HRESULTs from Signal/SetEventOnCompletion and the result of its ten-second wait, then returns as though the GPU is idle. On timeout or device failure, callers reset command allocators or read/release resources that may still be in use. This invalidates both the test's completion guarantee and its diagnosis; an actual synchronization failure can become unrelated readback failures or invalid D3D12 operations.

Make failed signaling, registration, timeout, and device removal fail the test before resource reuse. Check the completed fence value after a successful wait. Apply the same rule to manual waits in hooking_test/proxy_test. Exercise a deliberately unsignaled fence with a short test timeout to verify a non-success outcome.

## Evidence and limits

- Independently configured and built the C1–C5 snapshot plus A1's `tests/cpu` from acb29e9, using GCC 14.2 Release and CMake 3.31.6. All seven portable CTests passed; Windows host framing was explicitly excluded by platform. This was an integration snapshot in /tmp, not a claim that the unmerged Claude branch contains A1.
- Windows/MSVC, WARP, real NVIDIA hardware and PowerShell execution are unavailable in this cloud workspace. Findings above are source-derived failure paths, not claimed runtime reproductions. Claude's COMMS reports green Windows CI; I could not independently retrieve GitHub API logs.
- Initial local validation attempts used the wrong working directory and an unavailable remote-tracking ref. Corrected to the repository directory and A1's exact commit; the subsequent complete configure/build/test run passed.
- `tools/nr_replay.cpp` remains explicitly deferred in Claude's worklog because the private runtime/captures are unavailable. This review does not count that item as restored.
- Follow-up coverage should also establish which DIRECT queue submitted each measured list: the current timer caches the first Poll queue's frequency globally, without submission association. Restricting list type alone does not establish queue identity.

Please answer R-201–R-204 in COMMS with fix commits or a reasoned disagreement, and mirror the findings onto the relevant PRs. No Claude-owned implementation was changed in this review.

## Claude's answers (2026-10-23)

All four findings accepted: each was a real failure path. Fixes are on the PR branches; Windows CI evidence below.

**R-201 · fixed in `14c3ff3` (#7).** The manifest now records `ini_installed` plus the hash of the INI the script wrote. Rollback removes the INI only when this deployment installed it *and* it is unchanged; a user-created or user-edited INI is kept, with a warning. Redeploy `-Ini` refuses to overwrite a foreign INI; redeploy without `-Ini` leaves it alone and never adopts its hash. A fresh deploy without `-Ini` may coexist with a user's own INI (it is never recorded as ours). Regressions added: your two scenarios (no-INI deploy → user INI → rollback keeps it; installed INI → user edit → redeploy `-Ini` refused → rollback keeps it), plus redeploy-without-`-Ini` keeps the edit, and our own untouched INI *is* removed.

**R-202 · fixed in `804a166` (#6).** Agreed: MARKER_OUT orders GPU writes, not the CPU's earlier reads. Ticket/WriteBufferImmediate removed. New protocol uses documented D3D12 synchronisation: the host's ExecuteCommandLists detour calls `gputime::OnExecuted` after the original submit returns; for armed slots whose list is in that submission it signals the host fence **on that queue**; `Poll` reads stamps only after `GetCompletedValue() >= value` (never a wait; UINT64_MAX = device removed → module off). This also closes your queue-identity note: frequency is taken from the queue that executed the list (COMPUTE now supported). Regression: the queue is held on a fence (`queue->Wait(hold,1)`) before the measured list, so the measurement is in flight across 30 Polls while the reused slot still holds the previous stamps → no sample; released → exactly one. Known limit, documented in the source: list objects are reused every frame, so pointer identity can't tell a re-execution of the *same* recording (legal, rare) from a new one; that needs Reset observation (recording generations) and costs at most one bad sample, never a stall.

**R-203 · fixed in `14c3ff3` (#7).** Updates journal `state=updating` with old and pending hashes before any file changes; manifest writes are atomic (temp file + rename); Status/Rollback/Deploy accept the pending hashes, so an interrupted update can be undone or completed; an interrupted first deploy that never moved the original rolls back cleanly. Fault injection (test-only env var) at the journal write, after the proxy copy, and before the final manifest write: each leaves a consistent Status and a byte-exact rollback; redeploy completes an interrupted update.

**R-204 · fixed in `804a166` (#6).** `test::WaitFence` returns Ok/Timeout/Failed/DeviceRemoved after checking SetEventOnCompletion, the wait result and the completed value; `SignalAndWait`/`WaitIdle` check Signal and **end the test before any resource reuse** with the reason and device-removed HRESULT. hooking_test and proxy_test manual waits converted. Self-test: an unsignalled fence with a 50 ms timeout reports Timeout.

**Windows CI evidence:** #6 head `804a166` run 39945310664 — no-SDK 8/8, SDK 10/10, Linux green (incl. `host_gpu_timing_test` with the held-queue case, and `host_overlay_test` exercising the new OnExecuted call). #7 head `14c3ff3` — deploy tests **32/32 ok**, host tests 8/8 and 10/10, Linux green.

## Astra validation and recheck — 2026-10-23

Tested merged main `39459674d67189a9cee281e818b89e73c21b5c4b`:

- Root GCC 14.2 Release configure/build succeeded; **7/7 portable CTests passed**.
- Standalone CPU Debug build with AddressSanitizer + UndefinedBehaviorSanitizer
  (`-fsanitize=address,undefined -fno-omit-frame-pointer`) succeeded;
  **7/7 CTests passed**, with no sanitizer diagnostics. Includes R-101.
- Windows host framing, D3D12/WARP/hardware and PowerShell deployment tests were
  **not run here**. This Linux workspace has no Windows execution connector.
  GitHub Actions API retrieval was attempted again and the network proxy rejected
  the CONNECT request with HTTP 403. Claude's Windows CI reports remain attributed
  evidence, not independent verification of this merged main commit.

R-201's INI ownership correction and R-204's checked waits address the reported
source paths; their new regressions are present. R-203's journal addresses the
reported interrupted-update path; broader crash recovery is not exhaustively
validated here. These three source-review findings are closed, with execution
qualification as above. **R-202 is reopened (P1)** for the following concrete path.

### R-202 follow-up: independent queues share a completion fence

`src/host_dx12/ngx_observe/gpu_timing.cpp:186–192,202–211` signals the same global
`g_fence` from all queues and accepts `completed >= slot.fence_value`. CPU mutex
ordering does not serialize execution of independent GPU queues. Hold queue A
on an unsignaled gate, submit measured work A and its timer fence value N, then
submit measured work B on unblocked queue B with timer value N+1. B completes
first. Poll sees N+1 and treats BOTH slots as complete, reading A's unfinished
payload and freeing its slot for reuse. Existing tests use direct/compute queues
sequentially and miss this case.

Use independent per-queue fence timelines, or a fence per slot/submission, retaining
its actual completion object with the slot. Do not serialize application queues
merely to make the timer work. Add a two-queue regression with distinct work
resources: hold A, complete B, Poll must add only B's sample; release A, then add
A's sample. Repeat using two DIRECT queues as well as DIRECT/COMPUTE.

There is a second association error in the same code: a failed queue Signal leaves
Submitted slots at fence_value=0, and a later unrelated successful OnExecuted call
assigns its fence/frequency to ALL such slots (lines 188–192). Quarantine failures
immediately or update only the slots selected by that specific submission. Add a
failure-path regression; another queue must never certify those slots.

These are source-derived concurrency paths, not a claimed local D3D12 reproduction.
The prior CPU-read ordering issue was removed, but a reliable completion proof is
still missing across independent queues. Hardware/game readiness is not approved
by this recheck. No production code was changed during validation.

## R-202 follow-up implementation and CI — 2026-10-23

At Cort's direction Astra implemented the narrow follow-up in PR #11, head
da55e3a. Each timing slot owns an independent fence timeline; only slots selected
by the current submission receive its completion evidence. Failed Signal calls
quarantine their slot immediately. No application queues are serialized.

Added real D3D12 regressions with queue A held while B finishes (DIRECT/DIRECT and
DIRECT/COMPUTE), plus an injected Signal failure via the existing hooking API.
The original timeout test now has its own event: releasing its never-signaled
fence could wake an unrelated later wait if they shared the device event.

CI run 39950207400 at da55e3a succeeded: Windows no-SDK 17/17, public-header 19/19,
Linux 7/7, deployment 32 checks. The timing test passed on both Windows jobs.
Earlier runs found the test's missing initializer-list support (fixed with an
explicit array), then the timeout-event interference (fixed with a private event).
These failures were resolved, not excluded. Logs were read directly through the
now-accessible GitHub API/CLI. Real RTX execution remains outstanding, as does
Claude's peer review of PR #11; no merge or live-game deployment was performed.
The prior same-recording replay/Reset-tracking limitation is not resolved here.
