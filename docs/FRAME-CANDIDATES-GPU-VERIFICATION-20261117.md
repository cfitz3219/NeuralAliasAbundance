# Shared frame candidates: GPU verification

Runtime verified: Fable `0c0eacd9703b9fe0a87f06e9d8c0660deff2ba1b`, integrated with Astra's prior regressions at `c35747aa` on `claude/project-thread-axt148`. Auditor: Astra. Hardware: NVIDIA GeForce RTX 5070 Ti, Release x64, MSVC 19.51, SDK-enabled build. Fable owns production; this increment changes tests and documentation only.

## Decision

The implementation fixes the reproduced 1080p camera-fit failure and the large false motion on the supplied repeating-picture cases **on the GPU**. The existing strict camera-fit bound is restored, and the actual-output ambiguity test is now registered with CTest. The correction belongs before both consumers, as implemented; restricting it to camera-free frames would leave the camera-fit failure unresolved.

This is hardware-fixture verification. No game was launched, modified, installed or controlled. It does not claim every repeating image is resolvable, or that game image quality and frame rate have been measured.

## Actual output, not the CPU prototype

The existing fixture runs the production encoding, AMD search, new candidate correction, depth-aware expansion and total-motion measurement with no usable camera matrices. Ground truth only grades outputs. The older test-side CPU candidate experiments are still printed for comparison but cannot satisfy any GPU assertion.

At 1920x1080, a sampled pixel is counted correct when its vector error is below one pixel. Far error means above 64 pixels. These are interior samples, excluding image edges and a band around the independent-region boundary, as in the earlier audit.

| Case | Before correction: correct | Actual GPU after: correct | Far error after | Mean vector error after |
|---|---:|---:|---:|---:|
| Repeating picture, 6-pixel pan | 11.97% | 99.97% | 0.00% | 0.002 px |
| Repeating picture, reversed 6-pixel pan | 7.11% | 99.88% | 0.00% | 0.007 px |
| Repeating picture, 12-pixel pan | 51.97% | 99.96% | 0.00% | 0.004 px |
| Independent central region, whole picture | 96.42% | 99.92% | 0.00% | 0.004 px |
| Independent central region, region only | 69.43% | 99.54% | — | — |
| Still / 3-pixel pan / non-repeating 6-pixel pan | 100% | 100% | 0.00% | 0.000 px rounded |

The particular alarming case is resolved: previously 46.65% of all sampled pixels in the 6-pixel pan were over 64 pixels wrong **and tagged measured**. After correction, that count is zero. The reversed pan no longer follows the false -622-pixel peak. The shared pass offers several locally tested vectors instead of trusting the strongest vote.

Added fractional-pan check: repeating 6.5-pixel pan is 98.53% correct at 1080p, mean error 0.182 pixels, with zero far/marked-measured-far samples. At 512x256 it is 96.55%, mean 0.215 pixels. This case still has finite local refinement error; passing the ambiguity test does not mean exact motion everywhere.

All original fourteen image/size cases plus the two fractional cases pass. No assertion was relaxed. Still, ordinary 3-pixel and non-repeating cases report **zero changed blocks**, not merely similar final percentages.

## Camera fit uses the correction too

Confirmed-raster repeated-picture case at 1080p:

- Prior final moving error: 6.0000 pixels because the fitted transform was rejected.
- Current final moving error: **0.0004 pixels**.
- Raw-flow fit energy: **3.1869 -> 0.0011**; the original 0.7 acceptance ratio is unchanged.
- Held-out image check: **15,507 / 15,507 cells improve**, all nine screen regions improve.
- GPU fitted transform independently reconstructed before admission: mean (-5.9999, 0.0001), error 0.0002 pixels.
- Non-repeating control remains 0.0008 pixels final moving error.

The test no longer permits the temporary 6.5-pixel known-limit bound. Both pictures at both sizes must satisfy the original **0.35-pixel moving-error bound**. Confirmed still motion remains below 0.08 pixels. The deliberately unconfirmed-jitter negative control remains a negative control; this pass does not repair false raster provenance.

## GPU plumbing and integration

The expanded ambiguity test was also run with the D3D12 debug layer: **zero validation errors at both sizes**. Candidate stats now checked in the fixture include completed-pair count, exact frame identity, offered/changed ordering and no changed blocks on the still/ordinary controls. This exercises the newly added descriptor table, vote texture atomics, eight candidate-selection rounds, corrected grid and readback words 64..103 on actual hardware.

Observed 1080p candidate counters:

| Case | Blocks offered alternatives | Blocks changed |
|---|---:|---:|
| Still / ordinary pan / non-repeating pan | 0 | 0 |
| Repeating 6 px | 29,523 | 27,007 |
| Reversed 6 px | 31,222 | 29,253 |
| Repeating 12 px | 29,576 | 14,862 |
| Repeating 6.5 px | 29,530 | 27,110 |
| Mixed independent region | 32,163 | 924 |

Final Release suite: **17/17 passed, zero failed or skipped, 148.79 seconds**. This includes strict small/1080p camera-fit/total-flow tests, the sixteen-case ambiguity fixture, flow retention, direct camera-fit shader tests, compilation of all flow entry points, DX12 camera-off and exact-submission/depth proofs, four CPU motion/depth/candidate tests, and five real-NGX native-DX11 DLAA variants.

Both actual-NGX camera-off cold starts (direct and 64-bit companion) deliver **46/46 acquired frames**, schedule 45 production viewport-jitter samples, have zero busy/rejected frames, and show one initial input reset. No camera discovery warmup or repeated resets are introduced. This verifies synthetic end-to-end delivery, not live-game picture quality.

## Measured GPU cost

The new optional `--candidate-benchmark` uses the existing `SetFrameCandidates` test seam. Each case uses the same two pictures alternating direction, six warmup frames, twenty measured GPU timestamp samples, and an off/on/on/off order. Each state is thus measured twice. The table averages the two run medians. These timestamps cover the existing whole flow stage, **not DLSS or total game frame time**. CPU setup and synchronous fixture readbacks are outside the GPU interval; none are added to game callbacks.

| 1080p pair | Candidates off | Candidates on | Added stage cost |
|---|---:|---:|---:|
| Still | 0.541 ms | 0.555 ms | 0.014 ms |
| Non-repeating 6 px | 0.595 ms | 0.617 ms | 0.022 ms |
| Repeating 6 px, alternating direction | 0.613 ms | 1.857 ms | 1.244 ms |
| Repeating 6.5 px, alternating direction | 0.611 ms | 1.702 ms | 1.091 ms |
| Independent-region pair, alternating direction | 0.597 ms | 1.922 ms | 1.325 ms |

Normal single-motion cases have little extra cost. Ambiguous/multiple-motion cases cost materially more: even blocks with already-correct local motion compare a distant offered candidate. Mixed-region measurements show this clearly: 32,163 offered blocks, only 924 changed in the final pair. This is disclosed performance evidence, not a reason to revert the verified correction.

Sent Fable an exact optimization opportunity without duplicating his production work: if the own integer `BlockError < kGain`, skip scoring alternatives. `BestOfNine(own) <= integer_error`, and every alternative error is non-negative, so the required absolute improvement is mathematically impossible. Strict less-than preserves the equal-to-margin case. This is a proposed cost reduction, **not implemented or benchmarked here**; offered-count semantics must stay explicit if adopted. Preserve shader group-barrier participation when adding such a shortcut; skip work inside the block rather than returning individual lanes before `GroupMemoryBarrierWithGroupSync`.

## Scope that remains

- Exact repeated patterns with equal image error remain ambiguous; this implementation retains the local answer.
- Candidates must have at least 1% block support (eight blocks minimum), and each component must be within +/-127 pixels. Small independent motions and faster pans outside those bounds are not newly qualified by these tests.
- An own zero vector is deliberately kept. This prevents changing the measured still/subpixel controls; it is not proof zero is always correct.
- Candidates requiring pixels outside the image are not applied to that block. Interior regression results are not an edge/occlusion qualification.
- This audit does not change motion confidence, jitter, the model, camera requirements or game placement.

These are implementation limits, separate from the now-corrected concrete regressions. No new user capture is required to establish the reported fixes.

## Reproduction and receipts

Worktree `C:\Users\Cort\NA-camera-runtime-audit`, branch `astra/frame-candidates-gpu-verification`, build `local/audit-build`. Ignored receipts under `local/candidate-verification/`:

- `build.log`, `baseline-tests.log`, `ambiguity.log`, `ambiguity-debug.log`: unchanged Fable runtime and prior tests.
- `expanded-build.log`, `cpu-build.log`: rebuilt fixtures and affected integration executables.
- `expanded-debug.log`: sixteen actual GPU cases, candidate counters, debug validation.
- `timings.log`: twenty ABBA timing blocks, 400 measured timestamps.
- `final-tests.log`: final strict regression suite.

Commands after building the named existing targets:

```powershell
j2_compute_flow_test.exe
j2_compute_flow_test.exe 1920
j2_compute_flow_test.exe --ambiguity
j2_compute_flow_test.exe --ambiguity --debug
j2_compute_flow_test.exe --candidate-benchmark
```

No SDK, DLL or capture data is committed. Production code remains Fable's implementation unchanged.
