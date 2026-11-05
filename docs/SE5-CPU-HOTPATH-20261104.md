# Sniper CPU-overhead investigation and repair

Cort reports 165 fps and 85% GPU use without NeuralAlias, versus roughly 35 fps,
25-30% GPU use and 80% CPU use with it. Astra owns this independent investigation;
Opus has held overlapping observer/HUD edits. Game testing remains Cort's.

## Finding

The CPU command-tracking path has substantial overhead even without a DLSS,
neural-rendering or optical-flow evaluation. Reducing image quality is not the
first remedy for this evidence. The late installed c1b3ed2 log (`status-46688.log`)
has about 180-198 thousand indexed draws and 145-193 thousand graphics root-table
changes each second. Their hook envelopes represent about 60-66% of measured
command-list hook time. Those counters include the underlying driver and can
overlap across threads: they are not exclusive CPU time or an attribution of
the entire reported 130 fps loss.

A separate real-DX12 benchmark reproduces the tracking cost on this PC. It records
6,000 indexed draws and 6,000 table binds per synthetic frame, with 16 material
tables and 64 distinct texture descriptors per table. Persistent workers avoid
repeated thread startup and cold thread-local caches. The timed section neither
submits GPU work nor waits for it. It does not evaluate DLSS or another model.
Its sink counts events; it does not run the game's full observation consumer.

Single-thread bare DirectX recording took 0.0755 ms; the previous observer took
9.1574 ms, or 10.9197 ms with native-HUD hooks installed. The repaired equivalents
took 3.6922 ms and 3.7253 ms. These are CPU command-recording measurements, not
game frame times or DLSS image-quality results.

Three alternating before/after pairs with four recording workers gave:

| Trial | Before wall ms | After wall ms | Before process CPU ms | After process CPU ms |
| --- | ---: | ---: | ---: | ---: |
| 1 | 7.0502 | 4.6835 | 30.4798 | 19.3545 |
| 2 | 6.9341 | 4.4762 | 29.2653 | 16.3702 |
| 3 | 6.1999 | 4.4419 | 23.5989 | 16.6420 |
| Median | 6.9341 | 4.4762 | 29.2653 | 16.6420 |

These rows include the observer and native-HUD hooks. Median wall time fell
35.4%; process CPU time fell 43.1%. Process CPU includes the asynchronous observer
worker and is summed across this process's threads. Wall time is elapsed recording
time. Scheduling and CPU frequency vary; the ranges are retained rather than
selecting the best trial. Four-thread native-only recording was 0.0412 ms wall /
0.1671 ms process CPU in its separate control process. There is still significant
tracking overhead after the repair.

## Work removed or made less frequent

1. **Always-on diagnostic timing.** The previous build times command-list hooks;
   Opus's d1d9bf4 adds useful nested attribution but would time several scopes per
   call. Profiling now requires `[observe] profile_hooks=1` and defaults off.
   `hook_cost disabled` is explicit. This does not disable observation itself.
2. **Repeated locks for one operation.** Root-table bookkeeping used separate
   registry/recording lock entries for event identity, binding shadow and table
   contents. These now share one registry/recording critical section. The direct
   draw's event identity and depth/order notes are similarly fused. Emission and
   external callbacks remain outside these locks.
3. **Texture rescans after unrelated changes.** A texture-read cache no longer
   becomes stale just because a per-object CBV, constant or pipeline changed.
   Root layouts, texture/UAV table changes, bound heaps, descriptor/resource
   revisions and selected scene source still invalidate it. Graphics and compute
   caches remain separate. Full binding state and operation-key tracking remain.
4. **All-heap searches on normal table binds.** The existing generation-checked
   metadata for the recording's currently bound heaps is tried first. The old
   registry search is retained as fallback. Game-sized heap registries therefore
   need not be walked for an ordinary already-bound table.
5. **Scans after a collection is already full.** The existing per-recording
   sampled-texture hint stores at most 256 distinct resources. Once it is full,
   further table entries cannot add information; those scans stop. The cap and
   retained first resources are unchanged. Actual shader-read checks still run.
6. **Duplicate depth facts.** Thousands of adjacent identical depth-write facts
   added no information and could consume the room needed for a later genuinely
   different depth target. Those adjacent duplicates are coalesced. Every draw's
   event/order remains; actual transitions and different subresources remain.
7. **HUD work on non-HUD draws.** My finishing-draw `After` callback now checks
   the existing bound HUD destination before acquiring draw-state/HUD locks.
   Qualified replay and finishing snapshots still run.
8. **Jitter's inactive latch.** Once jitter had ever been enabled, its hook used
   to keep acquiring the list locks after it was turned off. A count of unfinished
   recordings with pinned samples permits a lock-free return only when there is
   no current request and no pinned recording. A pinned recording still completes
   with its own exact sample and restores post/HUD viewports. Reset and destruction
   retire that count. There is no borrowed list-pointer cache or new COM owner.

The paired measurements combine these changes; no independent percentage is
claimed for each bullet. Opus's nested profile helped locate repeated bookkeeping,
while this independent benchmark and game log provide separate evidence.

## Work retained

Selected scene/depth identification, camera-constant acquisition, insertion
ordering, complete binding restoration, recording/frame identity and submitted
GPU lifetimes are functional requirements. Disabling those to win the benchmark
would invalidate the picture path. The working jitter/flow maths, acceptance
policy, DLSS modes and sharpening are unchanged. Optical-flow GPU time in the
saved run was about 0.52 ms; that does not explain the reproduced CPU tracking
cost. This investigation is not evidence that every remaining game bottleneck
has been isolated.

The approach follows NVIDIA's recommendation to reduce unnecessary CPU work in
the rendering critical path, and preserves Microsoft's distinction between
recording command lists and submitting them:
[NVIDIA CPU guidance](https://developer.nvidia.com/blog/advanced-api-performance-cpus/),
[Microsoft command lists and queues](https://learn.microsoft.com/en-us/windows/win32/direct3d12/command-queues-and-command-lists).
The actual performance numbers above are local measurements, not vendor claims.

## Verification and reproducibility

Evidence directory: `local/se5-cpu-hotpath-20261104` in the main checkout (not
committed). It contains old/new benchmark logs, the old control binary, build/test
logs and the eventual package receipt. Benchmark source is
`tests/host/observe_hotpath_bench.cpp`; build its opt-in `observe_hotpath_bench`
target in Release, then run with `--threads 4 --span 64 --seconds 3`.
`--bare` is the separate no-producers process. `--profile` deliberately enables
the diagnostic timing; `--latch-jitter` exercises a previous request now turned
off. The saved old binary predates these last two flags and is not a valid
old control for them. No performance assertions are added to CTest/CI.

The original exact wire-stream regression still requires the same 14 operations,
identities, payloads and order. An additional session verifies changed CBVs retain
the right selected-source read, a table switch changes it, selected-source changes
invalidate the cached verdict, descriptor rewrites invalidate it, and a new depth
target is still known after 5,000 repeated draws. This is recording-time evidence,
not a claim of GPU pixels after a descriptor rewrite. The initial test extension
held its sink mutex across the new session and was stopped; the fixture now
releases that mutex. Its depth assertion also needed explicit typed mip/slice
evidence and a fully observed Reset. Those fixture failures are preserved in the
logs; runtime qualification was not weakened to make them pass.

The viewport raster test now records one half on a joined second thread, disables
the global jitter request, and records the other half on the first thread. Actual
GPU pixels must retain the recording's original sample on both halves and retain
unshifted post/HUD viewports. The existing next-frame sample and Reset case remains.

First full Release/200-case suite: 195 passed, five skipped, no failures. The final
full build/suite and package receipt will be appended after the frozen runtime
finishes verification. Tests use real DX12 work, with fake-model callbacks where
already indicated by the individual fixture; they do not prove the game's visual
result or recovered game fps. Existing HUD finite limits remain documented in
`SE5-HUD-LIVE-HANDOFF-20261104.md`.

## Game comparison

The prepared update will reuse the established additive installer and rollback;
it will not be installed or launched by Astra. Cort's next comparison should use
the same scene, resolution, game settings and our selected effects. Ordinary play
has hook profiling disabled. Capture saving and F8 are unnecessary for the first
fps comparison and add separate diagnostic work. The user-reported 165-to-35 fps
loss remains an open live result until that comparison, even though redundant CPU
work has been reproduced and reduced automatically.
