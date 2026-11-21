# Observer cost audit — 2026-11-17

Astra, on `astra/observer-bounded-work`, based on Fable's `4ac8c4a0`.
Cort requested a wider efficiency investigation beyond replay and the earlier
0.11 ms GPU reductions. No game launch, control, installation or settings change.
Fable retains replay and combined-package ownership.

## Changes and evidence

| Work | Finding and correction | Measured or structural result |
|---|---|---|
| Saturated resource-use tracking | `add_use_locked` sorted/uniqued the whole 4,096-entry buffer on every subsequent use when the distinct set remained full. Compact once, then use sorted membership and bounded insertion until Reset. | Real DX12 recording benchmark, 4,096 distinct resources used twice: **49.3658 → 3.0198 ms** median, **50.3729 → 3.4675 ms** p95. Bypass controls .9449/.9358 ms. About 94% less time in this complete observed test workload. |
| Repeated material-table membership | Root table binds repeatedly walked an unchanged SRV range and linearly searched the same historical texture IDs. Remember 16 exact heap/range/revision results per recording; writes, copies, heap identities and Reset remain observable. CBV records are not skipped. | Initial 6,000-draw, four-worker, eight-SRV/table comparison: observer plus HUD **1.3415 → 1.1428 ms**, CPU **5.2188 → 4.4821 ms**. This is one no-game comparison, not a reliable universal percentage: later repeats have substantial wall-time variation. |
| Submission snapshot copying | The callback copied as many as 4,096 binding records while it could visit at most the remaining 64-record budget plus one overflow receipt. Copy only that prefix and stop visiting lists once the overflow receipt is emitted. | Maximum single-list temporary copy falls from 4,096 to 65 records (about 192 KiB to 3 KiB on x64). Test with 30 + 4,096 + 4,096 bindings produces exactly 30 + 34 normal snapshots, one budget receipt and no third-list snapshots. No isolated frame-time claim. |
| Camera-candidate lookup | `CameraRank` allocated and stable-sorted all candidates for every snapshot, including missing or nonqualifying object constants. Look up the queried block first, then count candidates ahead of it, stopping once it falls outside the requested top count. Map order preserves stable ties. | Same-process CPU ABBA, 65,536 queries over 256 constructed bindings: old **154.26 / 161.55 ms**, new **3.50 / 3.43 ms**, equal checksums. Per 64 queries: .151–.158 → .00335–.00342 ms, about **98% less** CPU for this lookup workload. It runs on the background consumer, so this is not additive critical-path frame time. |

The 49 ms defect is conditional on a large distinct resource set in one command
recording. The saved Sniper log does not give that set size. This fixes a proven
expensive code path; it does **not** establish that Sniper loses 46 ms there on
every frame. Draw count and distinct referenced-resource count are different.

The tracking change retains all 4,096 distinct IDs. Previously the compaction
path also marked the recording incomplete when a duplicate arrived at a full
set. Now an omitted *new* ID marks it incomplete; duplicates do not. Actual
overflow remains bounded and does not evict known IDs. Small lists retain their
original append path. No lock or GPU lifetime protection was removed.

## Verification

- Release producer/backend and `NeuralAliasDXGI` builds pass. The discovery
  adapter is also rebuilt and exercised through `na_diagnostic_adapter`.
- Final **21 CTests pass, zero failures/skips**, 66.19 seconds. Covers resource
  tracking, descriptor mutations/copies/reset, snapshot count limits and
  provenance, map/unmap race, carried resource states, shader operation hooks,
  viewport jitter, automatic scene placement and its split/indirect/replay/
  render-ahead variants, and actual discovery adapter wiring.
- Camera ranking includes 13 CPU cases. The added case compares all requested
  ranks against the old sorted result for ties, low counts, missing bindings,
  different offsets, and top limits 0/1/4/8/512.
- `observe_hotpath_bench --uses 4096` uses real resources but does not submit the
  recorded barriers to the GPU. `observe_bookkeeping_test` independently checks
  descending insertion, full capacity, duplicate retention, a 4,097th ID, Reset,
  and repeated small sets. These are engineering tests, not gameplay evidence.
- Test-development corrections: the new snapshot test initially treated the
  existing void `WaitIdle` helper as bool; corrected and rebuilt. The initial
  run following that build failure used the prior executable and is not the
  new budget case's evidence. `snapshot-regressions.log` and the final 21-test
  run execute the corrected case. One attempted build target name was wrong;
  the actual target `na_cpu_universal_heuristics` subsequently built and passed.

Receipts: `C:/Users/Cort/NA-camera-runtime-audit/local/observer-cost`.
Primary timing receipts: `uses-before.log`, `uses-after.log`, `before-span8.log`,
`after-span8.log`, `rank-benchmark.log`. Baseline and changed statically linked
benchmark executables are retained as `before.exe` and `after.exe`. Baseline is
4ac8c4a0 plus the identical new benchmark code. Resource tracking comparisons
completed at 03:11/03:14 local; ranking at 03:17:39.

Additional `abba-*`, `mutated-*`, `isolated-*`, `final-before-*` and
`final-after-*` files are retained. Some later runs overlapped other test work;
the last repeats also overlapped Cort's game (launcher 03:19:08, DX12 process
03:19:41). Astra stopped the benchmark processes when the game was noticed.
Those repeats are not clean performance acceptance and do not support an exact
steady-state FPS claim. Correctness tests passed, but their elapsed runtime is
not a performance result. No further GPU load or game action was initiated.

## Other inspected areas and decisions

- Hook transport: the earlier batched wake-up and per-ring producer admission
  already exist. Do not remove the generation/quiescence protocol for a guessed
  gain. It prevents emission into stopped or replacement sessions.
- Per-list and pipeline lookup: generation-checked caches already exist.
  Removing registry/binding protection would not be equivalent behavior.
- Recording leases: `Retired()` is an atomic read; `Maintain()` uses a try-lock
  and bounded collection. No evidence here warrants an ownership rewrite.
- Settings polling: existing per-frame watch checks atomic fields and emits
  changed values. It is not doing an INI rewrite every frame.
- Sampler variant worker: waits for requests/missing variants, and compiles at
  lower thread priority. Its object scan is outside normal draw callbacks.
  Compilation spikes remain distinguishable from steady-state tracking cost.
- A negative scene-read cache needs explicit derived-resource invalidation.
  Simply caching "not relevant" could miss a newly discovered scene copy.
  That tempting shortcut was not applied.

These fixes are reusable shared observer/discovery changes, with no game names,
fixed addresses, reduced input quality, disabled effects or weaker evidence
requirements. Native DX11's separate recording implementation is unchanged.
Fable's bounded replay work remains the larger already-observed live feature
cost. This audit does not claim the entire program is now optimally efficient
or that the user's noticeable FPS/latency target has been demonstrated.
