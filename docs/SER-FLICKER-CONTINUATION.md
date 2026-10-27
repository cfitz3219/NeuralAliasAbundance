# SER remaining flicker: investigation checkpoint

2026-10-27, Astra. Evidence is from the existing run8 log, not a new game run.

## Finding

The earlier clean 3,024-frame interval does not describe the rest of the session. Between sampled timestamps 04:12:45 and 04:16:25, counters advance by 12,772 Presents and 11,369 covered Presents: a gap of 1,403. Also observed: 17 later writes, 222 camera-finding failures, 383 state failures and 26 history cuts. These are different counters with different denominators; failure attempts are not missed-frame counts. Do not add them to explain the gap.

The interval includes overlay/settings interaction and possible game transitions. The log does not establish continuous gameplay, visibility, camera movement or exact timing of Cort's perceived mild flicker. Therefore this is evidence of renewed intermittent coverage and input/state problems, not proof that all 1,403 gaps were visible defects.

| Sample interval | Presents / covered | Other evidence | Interpretation |
|---|---|---|---|
| 04:14:45–04:14:50 | 385 / 280 | 351 depth-state failures, 4 cuts, 4 later writes; overlay closed at both endpoints | Depth-state acquisition recurred; last-miss text explicitly names depth state. Not just an image-quality/sharpening issue. |
| 04:15:00–04:15:05 | 366 / 146 | No camera/state failures or later writes; 1 cut | Input failure counters alone do not explain all missing coverage. Need matched selection/route/frame events. |
| 04:15:45–04:15:50 | 304 / 304 | 1 later write, 2 camera-finding failures | Successful coverage does not guarantee placement correctness; failed candidate attempts can coexist with eventual frame coverage. A later write is not necessarily a full overwrite. |
| 04:16:15–04:16:20 | 360 / 346 | 54 camera-finding failures, no new cuts or later writes | Multiple candidate failures can occur per frame. Need successful candidate identity and actual input provenance. |

Sharpness changes from 0 to 50 during the later session; preset is 11 in the listed intervals. Preserve that context in image comparisons. Neither change proves the cause of flicker. Menu endpoint samples do not show the game's own pause/loading state or activity between samples.

## Reproduction of analysis

Run `tools/analyze-j2-status.ps1 -LogPath <status.log> -CsvPath <intervals.csv>` offline. It preserves cumulative-counter resets as separate epochs, menus/settings at endpoints, selected image IDs, missing fields and non-atomic counter skew. The source log is append-only across several runs: the five detected counter epochs are not a claimed count of launches. Some older logs lack coverage fields.

Local input: `C:/Users/Cort/NeuralAliasAbundance/local/ser-insertion-20261027/status.log`. Output: sibling `intervals.csv`. No game files/captures are committed. `tests/offline/j2_status_analysis_test.ps1` passes deltas, restart, disabled interval, menu/settings, missing optional fields, prose contamination, incomplete block and skew cases.

## Concrete source/capture limitations handed to Opus

At 90144fc, inline `record()` obtains state from the recording's observed states or the submitted-state store. Later depth read-only/no-local-write cases can still miss; the live last-miss message proves unknown depth state, not why that state is unavailable. Do not infer writable state from a DSV binding to suppress the failure. Correlate the actual depth use, preceding barriers and submission dependencies.

Current `frame_capture` is native-NGX-only, has one mutable current frame and resets its shared command allocator at each captured Present without checking GPU completion. It does not yet provide safe consecutive J2 input/output/final-image association. Opus owns the minimal production capture extension; Astra requested submission-bound identity, jitter/scales/reset and paired final image, with retained per-submission ownership. No unsafe capture deployment or duplicate recorder was made.

Opus accepted production ownership on 5828ba1, branch claude/p4-autoroute. His inventory confirms P4-0R and the complete P4-0D were planned, not already implemented. Two implementation corrections requested: native jitter is evidence of temporal sampling, not proof of TAA; no-later-write evidence is not equivalent to displayed contribution without downstream lineage. Those are implementation clarifications, not new acceptance gates.

## Next discriminator

Use Opus's per-Present evidence to partition missed output by selection loss, unmatched boundary, depth state, camera acquisition and recorded-but-unsubmitted work. Then inspect paired images for any flicker during continuously covered frames. Fix the demonstrated production cause under a narrow file handoff. The game is currently closed; no launch/deployment occurred for this checkpoint. Fullscreen restoration remains deferred. Remaining mild flicker is unresolved.

## Follow-up: boundary matching is a separate candidate cause

04:15:00–04:15:05:366 Presents,146 covered,220gap;1,724 inline_waited with zero camera/state/selection/busy/NGX/refusal increments and no later writes. Last missed frame68640 reports learned transition3, actual1, but a stale running reason. Since production matching can use producer+occurrence rather than transition ordinal, this does not prove the exact predicate that failed. It does show why successful input acquisition alone will not fix every missing frame. The automatic router currently reuses this matcher, so within-route matching/adaptation needs its own evidence. Requested per-rejection predicate and recording/frame identity from Opus.

New router f6587cc builds and passes3/3 existing route/automatic-host tests on RTX (4.42s). The four additional standalone review cases fail and have been handed to Opus: contradiction still executable, stale boundary after non-scene clear, absent consumer promoted to Between, interceptor ownership ignored without heuristic scene. These were introduced after run8 and do not explain the historical visual flicker. Capture not landed; no deployment or live-flicker fix claimed.
