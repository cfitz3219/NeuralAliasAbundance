# Sniper CPU-overhead investigation and repair

The 8bbd661 package receipt below is historical and superseded. CI subsequently
reproduced a HUD writer-identity defect, now corrected in d4145c1; the final receipt
at the end of this document governs the next package. The update CMD was returned
to the preceding 55fcd11 package during that correction.

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

## Final engineering and package receipt

Tested runtime: `8bbd66194680ac6067b76d89b4df9cf6bcbab290`,
`astra/se5-cpu-hotpath`, [PR 97](https://github.com/cfitz3219/NeuralAliasAbundance/pull/97).
Complete Release build passed. Complete final confirmation suite: **195 passed,
five skipped, zero failed, 326.20 seconds**. Skips are the optional NR runtime
probe, three unsupported 16x-MSAA cases and the optional upscaler-jitter runtime.
No rendering edits followed that run.

An earlier same-source complete run had three intermittent failures: a tuple
index TypeError in the unchanged Python reference checker, a temporary-DLL
CopyFile failure in the Streamline forwarding test, and the native-HUD fixture's
missing expected fallback layer at its deliberately unsafe-VB frame (followed by
exit failure). All three passed an immediate unchanged rerun and ten executions
each on the same binaries. The complete confirmation then passed. Their causes
are **not established or claimed fixed**; the original failure and repeated-run
logs remain in the evidence directory. There was no relaxed pixel comparison,
disabled test or runtime validator change to produce the passing confirmation.

Prepared package: `C:\Users\Cort\Documents\NA-se5-motion-8bbd661`.
DLL SHA256: `E24C38751A8C159CCA102B1FC4B2FB352EE761C02CEA00DE5D3DA0D89D4E58D3`.
It includes the previous 55fcd11 HUD work and the existing jitter repair. Opus's
separate optional anisotropic-filtering change is not silently added to this
measured package. The production PR is cleanly mergeable with that separate work.

The established additive deployment/rollback logic is reused. A temporary-folder
Windows PowerShell fixture passed install, corrupt-candidate preservation,
upgrade, saved menu options, new diagnostic defaults, results export, rollback
and native-file refusal. Payload hashes were verified. Ordinary-play defaults
are `profile_hooks=0`, `size_probe=0`, `capture_auto=0`; explicit F8/menu capture
and functional resolution/fullscreen handling remain. The latter two diagnostic
defaults are installer changes, not credited with the measured benchmark gain.

The existing `NA-se5-dlss-update.cmd` and results CMD select this package.
`NA-se5-cpu-previous.cmd` selects the preceding prepared 55fcd11 HUD package;
the existing `NA-se5-hud-previous.cmd` selects c1b3ed2, the actually installed
previous version. Original-installation rollback remains available through the
existing receipt. Old command pointers are backed up in the evidence directory.
Astra did not launch, control or deploy to the game. Its installed c1b3ed2 DLL
remained `03A63FBFB017726BE156A0793DF6C3118FA4A29294E5203E44D92524BFC401FE`.

Next performance acquisition, coordinated with Opus, is a function-level game CPU
sample to account for work outside the existing hook envelopes. That is distinct
from the proven redundant tracking removed here. A lean path for irrelevant work
can build on this head while preserving early binding state and promoting a list
before its first relevant scene/HUD operation; classifying a pooled list solely
at Reset from its previous frame would lose required state. No new review cycle
or game-specific addresses are introduced.

## CI reproduction and writer-identity correction

Public-header Windows CI run 41979107842 reproduced the HUD missing-layer failure;
Linux and ordinary Windows jobs passed. The repaired local full pass therefore
did not justify dismissing that HUD failure as a transient. It was reproduced on
this PC's software GPU using `NA_TEST_FORCE_WARP=1` in the test helper.

Temporary source/Closing diagnostics proved that old canvas 216, last used at
frame 28, and the actual current canvas 357, used at frame 38, matched the same
reused command-list address and local recording number 10. The old canvas won the
probe-order comparison. Its state was absent from the new command list, so Closing
returned without snapshotting canvas 357. The finishing list then correctly
declined an unowned source picture. Pool exhaustion was ruled out: five of eight
slots were occupied. No capacity increase or state guess was the remedy.

Runtime d4145c1 matches the writer's existing unique object ID, pointer and recording
number in both Closing and NativeBoundary. DrawInfo carries the ID already found
under its existing lock; actual canvas draws do not acquire another identity lock.
The inactive/non-HUD fast paths remain. No jitter/flow math, acceptance threshold,
GPU lease or fallback-pixel assertion changed. Temporary capture instrumentation
is removed. Ten repaired WARP runs passed. The only new test-control option forces
the existing software-GPU fallback for reproduction; normal hardware preference
is unchanged when the option is absent.

The final runtime's four-thread recording check was 3.9769 ms wall / 17.9843 ms
process CPU with HUD, versus the three original controls' median 6.9341 / 29.2653.
This single final-source check is consistent with the main cleanup's paired
measurements; it is not presented as another three-pair median or game FPS.
Frozen final Release/full-200 confirmation and final package follow.

## Current final receipt: d4145c1

This supersedes the 8bbd661 package and provisional receipts above.
Runtime `d4145c1d2e9933363854934dda81fec41f20f155`: complete Release build,
complete 200-case run **195 passed / five skipped / zero failures, 372.60 s**,
and ten forced-WARP native-HUD runs passed. Skips remain the same optional NR,
unsupported 16x MSAA and optional upscaler-jitter cases. All three GitHub jobs
also passed on this runtime, including the previously failing public-header
software-GPU job: [run 41980899607](https://github.com/cfitz3219/NeuralAliasAbundance/actions/runs/41980899607).
No runtime source changed after these checks.

Final independent package: `C:\Users\Cort\Documents\NA-se5-motion-d4145c1`.
DLL SHA256: `FE0298D8A7E7C2F636ADCFB16328CCC1850C36E0239A022D72470D0BA45642BC`.
The isolated additive-install fixture passed again for this exact package:
corrupt-package preservation, upgrade, saved choices, diagnostic defaults,
results export, rollback and native-file refusal. All payload hashes verified.
The existing SE5 DLSS update/results CMDs now select this package; the prior
55fcd11 pointer and original receipt rollback remain available. The game itself
remains installed with c1b3ed2 / `03A63FBF...C401FE`; Astra did not deploy or launch.

Opus's independent lean-overhead version remains separate for comparison at
Cort's direction. Comparing versions should keep functional settings and HUD
processing equivalent; passing the benchmark alone does not establish game FPS.

On Cort's HUD-scanner question: removing its checkbox has negligible cost.
Removing the functional scanner/replay would save work but lose native HUD
separation. The final benchmark's inactive HUD-hook difference is 0.2789 ms wall
per 6,000 draws (3.9769 minus 3.6980); it does not measure active HUD detection,
replay or GPU work. Detailed periodic draw tracing is a separate diagnostic path
and can be made opt-in independently. It is not silently removed from this
already-qualified runtime in response to a feasibility question.
