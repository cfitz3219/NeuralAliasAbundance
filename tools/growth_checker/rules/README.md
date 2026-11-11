# Growth checker judgment rules

These rules run on saved results. They return a verdict and the reason it was
reached. They do not edit evidence, accepted floors, or game settings.

## A1. Compare runs and maintain a floor

| Evidence | Judgment |
|---|---|
| Confirmed case, scene, render mode, other case settings and test environment match. Both builds are identified by their own evidence and each run has at least three measured gameplay intervals. | Comparable |
| Scene, settings, build attribution or gameplay boundaries are unconfirmed. Evidence is from automation, there are too few samples, sessions restart or time goes unlogged. | Comparable with limits. A directly recorded failure remains visible. The comparison cannot earn a positive gameplay verdict or change a floor. |
| Case, route, API, provider, scene, setting, environment or evidence kind differs. The log is reused or no scored run exists. | Not comparable |

Whole-play FPS tolerates the greater of 1 FPS or 3%. Median frame time tolerates
the greater of 0.25 ms or 3%. Coverage tolerates 1 percentage point. A sampled
gap has a margin of one interval at the coarser sample rate. These are initial
comparison margins, not estimates of the hardware's run-to-run variance.

| Floor evidence | Action |
|---|---|
| No accepted starting floor or the floor belongs to another case. | Needs owner |
| The run is limited, misses a required budget, loses delivery coverage while gaining FPS, or has an unmeasured or regressed floor value. | Keep |
| A qualified independent run improves an existing coverage, continuity or whole-play performance floor beyond the margin. | Propose |
| The next scheduled run is independent, produced by the same build, and meets the proposed value. | Confirm. The caller can retain the old floor in history. |

The comparison accepts optional explicit observations `scene_id`,
`test_environment` and `play_intervals` (true when warm-up, menus, intentional
Off and cuts are accounted for in the measurements). These need a run-card
reader. Existing results lack them and stay limited. A proposal's `awaits`
must identify the scheduled confirmation evidence ID. Report any
`floor_block` reason returned by the comparison in the floor decision.
Conditional FPS, raw counters and sampled gap estimates cannot raise a floor.

## A2. Flag misleading results

| Guard | Trigger and effect |
|---|---|
| Reused or wrong-build evidence | A producer mismatch, rejected provenance, or the same evidence ID/status hash blocks comparison credit. Re-scoring a log is an analysis update. |
| Fewer scenes recognised | A smaller scene share warrants a warning. Falling delivery across all presents also blocks improvement credit. |
| Speed with lost coverage | Whole-play FPS rises as delivery per scene or present falls. Blocks improvement credit. |
| Conditional FPS | Conditional FPS improves without whole-play FPS, or excludes substantial non-delivery time. Show the two separately. |
| Missing budget measure | A missing configured limit measurement remains unknown. |
| Counter reset | Multiple sessions. Count only within each session and retain the comparison limit. |
| Sampled gap | Display the gap as an estimate over sampled opportunities. A change between sampled and direct measurement prevents a like-for-like gap claim. |

## A3. Credit reuse

| Claim | Required observation |
|---|---|
| Zero-change first contact | Attributed successful live result plus a `manifest.first_contact` record with `zero_change: true`, producer `build` and a `source`. A first saved log alone earns no credit. |
| Independent engine or pattern | The same build uses the same attributed shared variant in two distinct games with different declared engines or different observed pattern values. Adapter-assisted or unattributed uses are counted separately. |
| Tested combination | At least two rendering facts recorded for the same successful run. This is a run-level observation. Mixed per-axis histories do not establish a single pattern on that axis. |
| Adapter hint | A shared rule verified a layout supplied by an adapter. |
| Title-specific dependency in use | A variant reports positive frames. Count a saved run once and use the largest count among overlapping counters for the same variant. Pass the selected build's runs for current counts. |

Route B success needs delivered frames and the owner's picture confirmation.
Route A needs a verified render-size change. A discovered pattern, adapter
credit or verified combination describes the saved games and modes tested.

## A4. Resolve uncertainty

| Evidence | Judgment |
|---|---|
| Evidence for a job names different builds, or live results for the same build conflict. | Unknown. Keep both observations. |
| Only a replay, fixture or synthetic result exists. | Show its result and leave the gameplay job untested. |
| A previously verified job now fails, an accepted floor is breached or a misleading gain blocks a comparable result. | Receded |
| A required job fails with no established comparison. | Failing |
| Matched evidence, all applicable jobs verified, no meaningful loss and a measured gain or newly verified job. | Grew |
| Matched evidence, all applicable jobs verified and measured performance/coverage hold. | Held |
| Missing evidence, unresolved trade-off, or unknown affected-case scope. | Not proven |

A recorded regression or failure takes priority in the build summary.
Positive build summaries require gameplay results for the affected cases.
An omitted case keeps the original build on its previous evidence. Use
`job_state` with pieces scoped to the candidate build, with earlier build
evidence displayed separately. No game can inherit a live result from a replay.
