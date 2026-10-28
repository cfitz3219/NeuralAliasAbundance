# Pull-request consolidation — 27 October 2026

Cort requested a review of every PR, retaining useful work and removing duplicate or obsolete open requests. This is repository consolidation, not game qualification. No local tests, game launches, installations or rendering changes were performed for this task.

## Inventory and disposition

All 51 existing PRs were inventoried. 32 were already merged, 11 already closed without merge, and 8 remained open.

| PR | Disposition | Preservation evidence |
| --- | --- | --- |
| 51 | Retain as the combined implementation | Head reviewed for consolidation: `9ad127c6513a5ac4900d6621da29096570561e59`. Includes current live DLAA, routing, capture, placement refinements and live mip-bias teardown correction. |
| 44 | Superseded by 51 | Exact head `9f7dd955` is an ancestor of 51. |
| 45 | Superseded by 51 | Exact head `81d15db7` is an ancestor of 51. |
| 46 | Superseded by 51 | Exact head `31de80eb` is an ancestor of 51. |
| 47 | Superseded by 51 | Exact head `bf237c6f` is an ancestor of 51. |
| 48 | Superseded by 51 | Exact head `8ca1c087` is an ancestor of 51. |
| 50 | Superseded by 51 | Exact head `8754c238` is an ancestor of 51, including Astra's response to Fable. |
| 49 | Close diagnostic-only PR; retain evidence | Copy `docs/ANNO-BLUR-DIAGNOSIS.md` unchanged from `3b62c780`. Do not merge temporary instrumentation or its build wiring: exit failures are documented. Keep the diagnostic branch/history for replay tools and provenance. |

Already closed unmerged PRs 5, 21–26 and 28–30 are ancestors of 51. PR8 is explicitly a CI-only integration experiment; its remaining commits are integration merges and `git cherry` reports no unique non-merge patch against 51. It stays closed. Already merged PRs remain unchanged.

GitHub PRs are closed, not erased. No branch deletion is needed to remove duplicate requests from the open queue, and retaining branches avoids disrupting the agents' active worktrees or losing diagnostic provenance.

## Limits

Retention does not declare SER flicker resolved, universal compatibility achieved, or all known runtime issues closed. Existing CI results are inspected, not rerun manually. Cort owns live testing. See COMMS-Continuation and PLACEMENT-DESIGN-RESPONSE for remaining placement qualifications.
