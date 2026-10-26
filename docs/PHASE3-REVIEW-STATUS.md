# Phase 3 review status — 2026-10-24

This supersedes the e55c191 snapshot-only review handoff and the older outstanding-A5/A9 table in GATE3-READINESS.md. Review the current PR27 head, not the earlier packets without its delta.

| Deliverable | Evidence and status |
|---|---|
| Production-obtainable R1 check implementations | `production_checks.hpp`: temporal depth/camera corroboration, preserved-source composition checks, and depth-derived visibility for motion. CPU regressions and fixture exercise the conditional APIs without supplied world-position/HUD-alpha/visibility oracles. Live callers still must acquire the required evidence. |
| A5 fixture acceptance |264/264 correct on the declared fixture-v2 corpus,63TP/201TN/0FP/0FN. See A5-ACCEPTANCE.md for scope. This is not measured arbitrary-game acquisition accuracy. |
| Read-only live consumer | Explicit INI opt-in, existing game-adapter extension, worker-side owned decoding, bounded asynchronous requests, unavailable inputs never promoted. Real Anno transport/overlay and local activation/reader tests exercised. |
| A9 | Actual Anno DX12 rendered-session report and follow-up resource/timeout report delivered in A9-ANNO-DIAGNOSTICS.md. All diagnostic deployments restored; final one-launch verification completed with exit0. |
| Process teardown correction | New reproducer failed0xc0000409 before correction and passes after; separate DLL process-detach regression passes. Normal in-game exit verified twice (30b4256 and9e1b140), both exit code0. Driver queue re-entry startup fail-fast also reproduced and fixed with regression. |
| Review | Latest consumer/host-lifecycle delta requested from Claude in COMMS; no external approval claimed. |

The requested implementation, fixture measurement and Anno report are present for review. **Gate3 is not closed and Anno compatibility is not qualified.** Latest live session completed12 texture samples, logged16,419 actual Presents and scanned captured constant buffers. Qualified camera/frame association, preserved pre-HUD lineage and complete capture remain unavailable. The reproduced startup failure was same-thread queue-mutex re-entry and is corrected; see the appended A9 evidence. These are concrete review findings, not evidence to reinterpret as passing inputs. The agreed subsequent review/fix/recheck and additional-game test steps remain required before gate closure.

Build with `NA_GAME_ADAPTER_LIB=na_discovery_diagnostics` for this optional diagnostic consumer. Default builds keep the stub; `[discovery] enabled=1` and `read_only=1` plus an explicit log path are required. Disabling discovery leaves no subscription. No automatic deployment or renderer replacement is part of this change.

Final local validation: full Release build and61/61 non-GPU-labelled CTests passed,zero skips/failures,including explicit active-EXE and active-DLL process-exit regressions. Logs are retained under local/phase3-integration. Source9e1b140 passed all3 CI jobs40158744541. The mixed-status adapter regression also passed. No tests ran concurrently with the final game session.



## 2026-10-25 — Final review accepted; ready for Argon

This supersedes the pending-peer-check status above. Claude accepted90f165b's
R9–R13 fixes and cleared the handoff conditional on acceptance of PR39. Astra
reviewed and accepted52cde25: queue-local ticket checks reject interference up to
the copy submission, while later FIFO-ordered operations do not invalidate its
bytes. The pre-copy cross-queue regression remains ARMED; the post-copy nested
Signal regression correctly remains READY. This replaces the overly broad
global invalidation in9085418. CI40170612165 has all3 jobs successful.

The P3 SampleFormat family note remains a documented unsupported-plan boundary:
resource formats19/20 and44/45 are supported with explicit41/46 plane returns;
other view-family identifiers are not silently decoded. No blocking finding or
peer disagreement remains. Exact final source, local test result, both review
records and hashes are in the regenerated Argon package. Audit status is
**ready for Argon**, not delivered/audited, and Gate3 is not closed.
