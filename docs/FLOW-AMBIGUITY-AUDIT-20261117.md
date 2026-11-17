# Repeating-picture motion ambiguity: GPU audit and handoff

2026-11-17 UTC, Astra. Fable owns production and candidate implementation; Astra owns GPU tests and independent candidate evaluation. Cort retains game testing. Starting source: Fable `67fc050b` on `claude/project-thread-axt148`, which includes packed10 via PR147. No production code or game files changed in this audit.

## Verdict on Fable's explanation

The hardware results support it. The 1080p failure is an ambiguous correspondence problem upstream of the camera fit, not a general inability to fit a camera at 1080p. Repeated texture can also fool the camera-free estimator into reporting a very wrong vector as measured motion. Its image-error check can accept the wrong repetition. This is now reproduced on an RTX 5070 Ti, not just Fable's CPU model.

Two qualifications: 6 pixels per frame need not be an extreme camera speed, and success on one non-repeating synthetic picture does not establish success on every ordinary game picture. The tests establish the mechanism and give a useful correction experiment; they do not measure how often it occurs in a game.

## Camera-fit verification

Built Release x64 MSVC 19.51 with the existing SDK-enabled audit build. Both Fable flow CTests pass (512x256 and 1920x1080), 14.44 seconds combined.

| Confirmed raster case | Stationary error | Moving error | Last fit energy | Held-out image evidence |
|---|---:|---:|---|---|
| Waves, 512x256 | 0.0000 px | 0.0041 px | 3.1818 -> 0.0120 | 961/961 better |
| Non-repeating, 512x256 | 0.0000 px | 0.0035 px | 3.1785 -> 0.0168 | 606/612 better |
| Waves, 1920x1080 | 0.0001 px | 6.0000 px | 7.9561 -> 7.2460 | Not reached |
| Non-repeating, 1920x1080 | 0.0001 px | 0.0008 px | 3.1864 -> 0.0022 | 9743/9801 better |

Fable changed the repeating-picture large-case assertion from accuracy below .35 px to a labelled known-limit bound below 6.5 px. I accept adding the non-repeating control to isolate the fit. **The changed bound does not fix or qualify the repeating-picture case.** A mean below 6.5 also does not prove every pixel is left unchanged. Our new optional ambiguity regression grades actual vector distributions and stays red on the current runtime.

## Actual camera-free GPU results

`j2_compute_flow_test.exe --ambiguity` uses the real AMD search, packed10 stabilization and total-motion shader, with no camera and no jitter. It reads the produced motion and validity on the GPU and separately reads the same pair's raw integer search. Readbacks and waits are test-only. The existing test still covers alternating jitter and fractional pans separately.

We grade every fourth interior pixel, excluding a 24-pixel screen border and a 12-pixel margin around the mixed-region boundary. "Correct" means vector error below 1 pixel; "far" means above 64 pixels. Measured-far and approximate-far are percentages of **all sampled pixels**, not only of valid vectors. The mixed case is a separately translating texture region inside a panning background, not a claim to simulate all moving-object geometry or occlusions.

| 1080p case | GPU correct | GPU far wrong | Marked measured but far wrong | Mean vector error |
|---|---:|---:|---:|---:|
| Waves, stationary | 100.00% | 0.00% | 0.00% | 0.000 px |
| Waves, 3 px | 100.00% | 0.00% | 0.00% | 0.000 px |
| Waves, 6 px | 11.97% | 72.68% | 46.65% | 69.649 px |
| Waves, reverse 6 px | 7.11% | 83.59% | 64.31% | 265.976 px |
| Waves, 12 px | 51.97% | 24.46% | 14.11% | 25.493 px |
| Non-repeating, 6 px | 100.00% | 0.00% | 0.00% | 0.000 px |
| Mixed independent region | 96.42% overall | 2.25% | 1.41% | 2.202 px |

The mixed region itself is only **69.43% correct**, hidden by the good background in the whole-frame percentage. At 512x256 all single-motion cases above are 100% correct; the mixed case is 99.59% overall, 99.82% on its independent region.

## Candidate experiments and corrections to the proposed remedy

These experiments refine candidates on the **CPU reference**, using actual GPU output/search candidates and the source pixels. They are not a GPU implementation of the correction. Ground truth only grades the result; it never supplies or selects a candidate.

1. Exact modal search vector as an extra locally competing hypothesis.
2. Four most frequent search vectors, choose a frame candidate by error across distributed 3x3 image patches, then compete locally.
3. The same four vectors retained as separate locally competing hypotheses.

The local vector is preserved unless an alternative is an informative measured result and its patch error is below 80% of the current choice. Zero/stillness remains in the reference evaluation. This 80% is an experimental comparison rule, not an approved general product constant or a relaxation of existing admission.

For 6 px waves, the true (-6,0) is the largest exact bin (5291 blocks); the incorrect cluster has more votes collectively but splits across (-7,-94), (-6,-96), (-6,-95), etc. Both the mode and photo-ranked candidate increase correctness from 11.97% to **98.91%**. This confirms Fable's idea has value.

**Counterexample to bare modal voting:** reverse 6 px produces (-622,0) on 20003 blocks; true (6,0) has 2740. A bare modal candidate does nothing (7.11% correct). The distributed image score picks (6,0) from the alternatives and reaches **98.84%**. The 12 px case improves from 51.97% to **99.40%** with either selection. Thus a popularity vote alone is insufficient; image comparison distinguishes these near repetitions.

A single frame winner preserves but does not repair the independently moving region: 69.43% remains 69.43%. Its correct motion is the second search peak (6,0), while the background peak is (-3,0). Retaining several locally competing hypotheses is the focused next step; do not overwrite every pixel with a frame-wide answer.

The completed four-candidate local experiment raises the independent region to **99.64% correct** (whole picture 99.94%). It gives 98.91% / 98.83% / 99.40% on the 6 px / reverse 6 px / 12 px wave cases, and preserves 100% on the still, 3 px and non-repeating controls. This supports a bounded set of alternatives rather than a single frame winner. It is still test-side CPU work, so it does not establish added production GPU cost or a shipped fix.

## Integration recommendation to Fable

- Keep production ownership with Fable; reuse the existing GPU search and image comparison. No new camera dependency, CPU diagnostic roundtrip, frame-history guess or game-specific rule.
- Use a bounded set of supported candidates with spatial and image evidence. Local/object motion and stillness continue to compete. Unsupported/indistinguishable alternatives are not independently proven motion.
- The 1080p camera-fit issue is not fixed if the correction is added only to `f.total`'s `measure`: the fit consumes expanded search **before** that branch. Prefer correcting the shared search evidence before both consumers. If using the proposed support-aware camera-fit comparison instead, retain the image, jitter and spatial coverage controls; do not simply raise the global .7 energy threshold.
- The new regression currently fails on the GPU cases above. It must judge corrected production output, never the successful CPU candidate experiment. Preserve the existing fractional-motion, stationary-jitter, reversal and scene-cut tests, and measure added GPU time.

AMD's [Optical Flow description](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/optical-flow/) describes an 8x8 block search with a luminance pyramid, iterative refinement and median filtering. That explains why a mistaken coarse match can persist into the fine search. A reverse-direction consistency check is an established confidence test, but by itself can also agree on the wrong repeated correspondence; it is not a substitute for testing the competing matches here.

## Receipts and scope

Worktree: `C:\Users\Cort\NA-camera-runtime-audit`; branch `astra/flow-ambiguity-audit`. Existing `local/audit-build` and SDKs reused. Logs under `local/ambiguity-audit`: `baseline-build.log`, `baseline-tests.log`, `audit-build.log`, `ambiguity.log`, `ambiguity-regression.log`, `ambiguity-extended.log`, `ambiguity-candidates.log`.

Build `j2_compute_flow_test` Release, run its default/`1920` cases for the camera fit, and `--ambiguity` for the actual total-motion defect and candidate comparisons. The optional ambiguity run intentionally fails current production; it is not registered as a passing CTest or disguised as expected success. No installer was made, no game was started, and no game quality claim is inferred from these fixtures.
