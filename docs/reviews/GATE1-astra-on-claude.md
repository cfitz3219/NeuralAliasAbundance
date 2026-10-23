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
