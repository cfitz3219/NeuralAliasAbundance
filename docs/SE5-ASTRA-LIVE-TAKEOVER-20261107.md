# Sniper stability takeover — 2026-11-07

Cort authorized Astra to install reversibly, control the game and repair/test the
remaining work after Fable's handoff. Fable handed off `6e0630d`; the requested
four-minute watch ended at that handoff. Runtime and package ownership transferred
to Astra in COMMS. No new user diagnostic run is required.

## Changes and distinguishing evidence

| Repair | Evidence and result |
|---|---|
| Accept the current unique, correctly sized late-bound input at the established shader/output boundary | Own `6e0630d` run had 380 unlearned-input misses. The first repair removed the repeated input-variant rejection; the DX12 fixture replaces the input after route learning and checks the delivered pixels. |
| Acquire original depth from the actual ordered submission prefix when a private snapshot is absent | Baseline had 149 missing-snapshot refusals. Subsequent runs have no repeated snapshot rejection; the fixture uses inherited depth state and checks restoration. Explicitly unavailable state is never replaced with an assumed state. |
| Canonicalize original/snapshot depth storage only for supported single-sample 32-bit depth | Retains Fable's history-allocation fix while avoiding equivalence with MSAA/other depth formats. No repeated history reset in the settled live sections. |
| Invalidate obsolete learned dimensions immediately when the requested size changes | Baseline live Performance-to-Quality change rejected 734 operations against old dimensions. Later runs apply Quality/Performance/DLAA without that maintenance delay. |
| Scale Home drawing to the actual framebuffer while keeping input in client coordinates | Baseline rendered a small menu viewport inside a much larger backbuffer; scaled coordinate interaction proved the mismatch. Offscreen pixel regression and subsequent game interaction confirm aligned drawing/clicks. |
| Retain the exact producer qualified before forwarding the upscale dispatch | Another thread can Close a future scene between Before and After. The old repick loses all 20 forced-race deliveries; retained producer passes. Snapshot storage stays reserved while the candidate/scene still owns its reader-fence reference. |
| Refresh the existing trace immediately after a changed input/layout eliminates the old tail boundary | `801c3f1` missed 736 gameplay frames after loading, then recovered at periodic tracing. `6f73365` recovered after three. Removing only refresh from the GPU regression causes 66 failing assertions; repaired output reaches the final converted screen. Persistent unsupported layouts retry at most every 120 presents. |
| Select/consume same-queue producers by submission order, retaining original array order separately from lock order | A future CPU Close before Before can hide the actual submitted scene. New GPU regression forces this 20 times. Restoring the old selection function produces 86 assertions, including missed delivery and final pixels. This is distinct from the Before/After race. |
| Keep recent input variants in the existing four-entry cache | The old cache permanently retained the first four, preventing later replacements from being remembered. No additional scan or unbounded cache. |
| Keep recording Reset tracking independent of optional enhanced-barrier registration | A failure to register the latter must not let a reused recording keep its old submission receipt. Legacy barrier observation is still enabled only when enhanced invalidation is registered. |

The ordering change reuses `RecordingLease` and the existing serialized observed
Execute scope. It adds a retained same-queue order receipt, not another submission
system, GPU wait or diagnostic readback. The completed foreign-queue route retains
its existing qualification; same-queue numbers are not used to invent cross-queue
ordering. Microsoft distinguishes successive Execute calls from a batch whose
lists may overlap internally; the game's resource dependencies/barriers still
govern execution within that batch. See [ExecuteCommandLists documentation](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-executecommandlists).

## Actual game runs

All runs below use NVIDIA's real DLSS runtime. Pixel-placement regressions use a
fake neural marker and are reported separately.

| Source / process | Result |
|---|---|
| `6e0630d` / p40052 | Baseline 7247/7783 matched deliveries; 536 drops. 380 late input, 149 missing snapshot, 6 producer reselection, 1 missing-size input. Actual NGX errors zero. |
| `7db4fb1` / p11748 | 3905/3985 matched deliveries; 77 tail-boundary misses (76 during loading), one mode input miss and two initial camera admissions. Native DLAA then roughly 6600 frames without ongoing misses; two resize reacquisitions. |
| `801c3f1` / p39580 | 1729 continuous menu frames, 736 loading-layout misses, then 2159 settled gameplay deliveries. Quality 1280x960 to 1920x1440; screen probe 64/64. |
| `6f73365` / p7724 | Loading misses reduced to three; one additional tail miss during mode change. Final sampled counter 5630 submitted, 5631 recorded and 5635 matched (one recorded operation in flight), four gate drops, longest continuous segment 2555. Initial prelearned colour-state admission once and 16 learning/size/scene gates remain separately counted. No NGX or ordering failures. |

p7724 exercised Quality 1280x960 and Performance 960x720 to 1920x1440 exclusive,
native DLAA, then Quality 682x512 to 1024x768 borderless and 685x495 to 1028x743
windowed. Home remained interactive and its draw counters reported no errors.
The display/game selected these sizes despite the earlier stored 3440x1440
preference; this is not a new qualification of 3440x1440 output.

Game HUD remains absent from the captured scene/DLSS output and appears in the
final picture, consistent with the previous pixel verification report. NVIDIA's
development diagnostic text is already in DLSS output and is not game HUD.

Static p7724 AO captures show temporal mean adjacent change 0.008715 in Quality
and 0.008051 in DLAA; edge changes 0.005270 and 0.005079 respectively, below the
interior changes. These figures do not establish moving-camera AO quality. The
brief movement in these sessions is not a full pan/shimmer acceptance.

## Receipts and preservation

Local evidence: main checkout `local/se5-astra-live-20261107`. The original installed
DLL, manifest, game INI, global preferences and full game profile were backed up
under `before-6e0630d` before testing. Existing additive deployment/rollback is used.
The first gameplay interaction fired one shot; later movement was brief. Original
saves remain backed up; no claim that testing produced no autosaves or progress
changes. The game showed a corrupt-checkpoint notice on the first own launch;
this has not been attributed to these repairs or silently "repaired".

Saved results are under Documents, respectively `NA-se5-results-20261107-072737`,
`075050`, `075949`, and `081302`. Game capture directories carry their process ids.
The worktree `build/astra-*` logs preserve initial failures and negative controls.

First ordering-test run: two fixture failures, an illegal duplicate submission in
one array and a Close counter omitted from the before-Before variant. Corrected
both without relaxing final-image assertions. A subsequent run exposed the short
direct-pass fixtures not installing production Reset tracking; they reused a
receipt across resets. Those variants now install the same tracking as production.
Full rebuilt validation and final package/game results are appended when complete.

The broad run also exposed an older sizing-fixture assumption: under the current
1024-pixel desktop at 125% scaling its client was 823 logical pixels wide while
the monitor was 819. Production correctly clamped four pixels, contradicting the
fixture's exact unchanged-window assertion. The fixture now starts inside the
monitor and retains the exact assertion; it passes. A diagnostic hypothesis about
the test's caller scope did not fix this and was completely reverted. No production
window-sizing behavior was changed for that failure. Logs: `astra-size-*`.

## Qualification boundary

This work addresses the observed Sniper dropout chains and their reproduced
recording races. Loading/size/scene acquisition can still leave initial frames
unenhanced. It does not claim every game, render-ahead pattern or different-queue
producer topology has been live qualified. The pre-existing unlearned scene-pass
route's closed/unsubmitted allowance remains separate from the learned Quality
route; its post-submit order check cannot cancel embedded commands. This is an
explicit remaining architecture limitation, not a claim that it caused the
settled p7724 picture to flicker. No new warmup blackout or after-HUD insertion
has been added.

## Completed automated qualification

Complete Release build succeeded. The 253-case broad run took 455.01 seconds:
248 passed, four optional/unsupported cases skipped, and the older sizing fixture
failed as explained above. Its corrected exact round-trip test passed separately
in 0.81 seconds. Thus all executed cases are resolved; this is not represented as
one all-green broad run. The four skips are the optional NR runtime probe and
three unsupported MSAA16 cases. Actual NVIDIA DLSS evaluation, NR lifetime and
composition, native Quality ordering/pixel/HUD fixtures, XeGTAO, overlay and NVRHI
tests passed. Logs: `build/astra-order-full-{build,tests}.log` and
`build/astra-size-fixture-{build,test}.log`.

The negative control changed only producer selection back to the older function
(adapting its consumed-record field to the new receipt). All 20 forced future-Close
frames missed, with 86 total assertions including image checks. Repaired code was
restored before the complete build and broad run, which passed that test. The
counterexample did not ship in a package or run in the game.
