# Gate 2 integrated completion

Cort asked Astra to take over Opus's outstanding tasks on 2026-10-24.
`astra/g2-completion` integrates C6 d9fee42, C7 ba1bf6f, C7b 70e6f99,
A3 1973c43 and A4 6272344. PR #17 is the combined candidate for main;
the earlier PRs remain the historical task/review records. No gate closure or
independent approval of Astra's takeover changes is implied.

## Findings and resulting behavior

| Finding | Resolution and regression |
|---|---|
| R-301/R-305 | Every feature remains owned; lost completion proof taints it. Tainted features cannot be reused. Exhausted completion capacity now refuses **before** recording NGX commands. Backend tests check refusal without a runtime call and exactly-once release at idle shutdown. |
| R-303 | Submitted predecessors provide GPU fence dependencies across queues. Unsubmitted work on a different list receives an independent reset feature before NGX records commands, so reversed submission cannot race one feature's mutable history. Same-list evaluations remain ordered by the list. Regression submits two independent histories in reverse order, holding one queue, alongside the existing cross-queue wait test. |
| R-308 | Preserved Opus's registration-before-lookup watch implementation and deterministic late-load test. |
| R-309 | Preserved native history ownership updates only after successful FSR/XeSS calls, with failing-native-call regressions. |
| R-310 | Preserved complete Streamline V2 constants/token/viewport objects; genuine nonzero runtime result is forwarded without storing a camera. |
| ARGON-G2-001 | Preserved maximum-size allocation and stable quality mapping. Isolated the DRS scenario from earlier intentionally abandoned views/features rather than raising limits or weakening assertions. |
| ARGON-G2-002 | XeSS regression now starts with an **unattached** backend and a context first seen at Init. The initializer verifies the recovered command-list device and successful replacement. |
| ARGON-G2-003 | Capture, validate, transition and restore exposure as well as color/depth/motion/output. Reject conflicting aliased roles and compute-list states requiring graphics bits before emitting barriers. Deduplicate identical resource roles. Translate FFX generic read to its actual copy-source plus compute-read bits. Real texture/list submissions cover success and failed evaluation; D3D12 debug assertions check exposure at evaluate entry and after restoration when the debug layer is available. |
| ARGON-G2-004 | Both producers use backend EnsureOn. Attach/InitOwn/Shutdown share its lifecycle serialization. A retained device identity prevents pointer-reuse confusion; simulated removal/concurrent recovery proves one initializer establishes the replacement generation. Physical device loss remains a hardware qualification. |
| ARGON-G2-005 | Destroy snapshots/removes metadata under the interceptor lock and releases the backend view afterward. Backend error stats are read outside that lock. Deterministic two-thread tests hold the backend during an evaluate callback while another context's destroy reaches ReleaseView, then read interceptor stats. A reintroduced inverse lock order times out the test. |
| ARGON-G2-006 | Preserved successful XeSS re-init forcing the next DLSS reset. |
| ARGON-G2-007 | Combined tree includes latest C7 and A4 changes, preserving the peer fixes. |
| ARGON-G2-008 | Explicit thread-local call provenance separates marked injector creations from unmarked/unknown creations. A4 consumes the unknown counter directly, never subtracts unrelated totals, and counts C6 results once. Marked calls bypass game overrides/capture paths. Tests exercise scope nesting/unwinding, real observer detours, backend marker emission and the report bridge. |
| ARGON-G2-009 | Preserved motion-scale documentation and display-resolution normalized FromFrame regression. |
| ARGON-G2-010 | Preserved A3's explicit opaque history-validity versus alpha-blended boundary qualification. |

## Validation and remaining qualification

Local GCC Release build and 9/9 CTests passed. Initial integrated Windows SDK run
[40083082539](https://github.com/cfitz3219/NeuralAliasAbundance/actions/runs/40083082539)
passed 28/28 non-GPU-labelled tests at 2880ef8 with NGX/DLSS enabled. Final-head
CI evidence belongs in COMMS.md and the PR description. SDK CI uses fake NGX and
D3D12/WARP; it does not prove RTX inference or game compatibility.

Separate unsubmitted lists can no longer share temporal state speculatively;
record-ahead isolation resets history and may increase feature allocations.
Capacity exhaustion falls back to the game's upscaler. Performance, visual impact,
real runtime quality/dynamic-resolution bounds, FSR/XeSS jitter conventions and the
scheduled RTX/game session remain qualification tasks. Callers must obey D3D12
list/resource lifetimes and submit each tracked recording once; resetting an
unsubmitted list or replaying it is not a supported tracking contract.

Argon's received audit was partial, and quota exhaustion is not approval. The
new takeover fixes require independent delta review when that session resumes.
