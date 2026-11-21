# Camera-independent DLAA: corrected runtime and precision verification

2026-11-16, Astra. Fable source: `c18ab226` on `claude/project-thread-axt148` (fixes in `1d107a0f`). Follow-up to `CAMERA-INDEPENDENT-RUNTIME-AUDIT-20261116.md`. User authorizes automated GPU work; game testing remains with Cort. No game was launched, installed or modified.

## Decision

Fable's R1/R4/R5 corrections pass their supplied CPU/GPU reproductions. His precision diagnosis is confirmed: quantizing the jitter-stabilized pictures to eight bits discards useful evidence for small movements. Use **R10G10B10A2_UNORM when supported**, rather than doubling storage with RGBA16. On this GPU it retains about 94% of RGBA16's measured slow-motion improvement for the original 32-bit-per-pixel storage and approximately the original processing cost. This is an improvement to motion estimation, not a claim of a measured visual improvement in a game.

The separate existing 1080p camera-fit case still fails. The new counters identify its rejection before image validation, at the flow-energy fit. Do not weaken the image validation threshold to address it.

## Fable corrections verified

Release x64, MSVC 19.51, RTX 5070 Ti, actual GPU. Baseline c18ab226: **14/14 selected CTests passed in 116.13 seconds**.

- R1: unknown/skipped depth evidence interrupts a pending opposite streak while keeping the established direction; the CPU regression passes.
- R4: private command-list reset now resets the recording identity too. The DX12 camera-off test proceeds beyond the prior six-slot stall. Camera disappearance delivers 10/10 frames (four held-camera, six measured), with no missing-motion period in this case.
- R5: abandoned, reset, recorded-ahead, replayed and reverse-submission-order depth evidence all pass the new recording-proof fixture. Never-submitted evidence no longer establishes a convention in those cases.
- Native DX11 cold startup with camera discovery off: **46/46 acquired frames delivered, 45 actual viewport jitter samples**, zero camera-supplied frames, one initialization reset. Both direct and companion paths use real NVIDIA NGX, not a substitute model.
- Existing provider, J2 live, total-motion/arbiter, camera-fit and flow tests also pass. J2 integration tests use fake NGX on the real GPU; distinguish them from the real-NGX native DX11 tests.

The recording-proof fixture does not cover inline insertion or multiple queues. It is not evidence that every possible submission graph is qualified.

Final capability-selected packed10 runtime: **the same 14/14 CTests passed in 110.38 seconds**, including both real-NGX cold starts with the same 46/46 delivery. The separate debug run passes with **zero D3D12 errors**. Its first execution found eight descriptor-clear errors in Astra's shader-audit fixture (a shader-visible descriptor was supplied as the CPU-readable clear descriptor), not in the runtime. Fixed the fixture to use a matching non-shader-visible descriptor, then reran it successfully; numerical results are unchanged. The final 1080p execution passes all new precision/cut assertions and fails only the already recorded R3 camera-fit assertion.

Hardware selects DXGI format 24 (packed10), verified in the completed frame stats. The unsupported-device fallback is source-checked, not claimed as a hardware run on an unsupported adapter.

## Precision experiment

Same source, gates, shaders and source pictures; change only stabilized-image storage and its matching views. Existing AMD search -> subpixel refinement -> motion arbiter; no camera (matrices poisoned with NaNs), alternating raster jitter, 8-bit source images. Errors below are the mean length of vector error in pixels over the interior of a 1920x1080 synthetic scene. They are not percentages of correct game pixels.

| Actual movement | RGBA8 error | Packed10 error | RGBA16 error |
|---|---:|---:|---:|
| Stationary | 0.0002 | 0.0010 | 0.0009 |
| 1/8 pixel pan | 0.0823 | 0.0624 | 0.0610 |
| 1/4 pixel pan | 0.1248 | 0.0958 | 0.0939 |
| Reversed 1/4 pixel pan | 0.1105 | 0.0879 | 0.0868 |
| 3 pixel pan | 0.0138 | 0.0234 | 0.0227 |

Mean x for true -0.125/-0.250/+0.250: RGBA8 -0.0716/-0.1808/+0.1916; packed10 -0.0946/-0.2055/+0.2088; RGBA16 -0.0958/-0.2067/+0.2093. Slow-pan error falls approximately 20-24% with packed10. Residual underestimation remains; no gain multiplier or looser admission rule was introduced. The small increase in still/3-pixel error is disclosed and bounded by added regressions.

| Motion work at 1080p | RGBA8 | Packed10 | RGBA16 |
|---|---:|---:|---:|
| Reported GPU interval across six pairs | 0.562-0.636 ms | 0.567-0.639 ms | 0.719-0.852 ms |
| Bytes per stabilized texel | 4 | 4 | 8 |
| Additional memory for eight warmed images | 0 | 0 | 63.3 MiB |

GPU times are the existing flow timestamp interval, not total DLAA cost or game frame time. These are short repeated fixture measurements, not a claim of a statistically resolved 0.005 ms change. RGBA8 and RGBA16 were each repeated twice with close results. At 3440x1440 the eight-image RGBA16 increase would be about 151.2 MiB. Eight is the warm allocation, not an absolute maximum when recordings remain in flight.

## Implemented change

- Select packed10 once at context creation after `CheckFeatureSupport` confirms 2D texture, shader load, filtered sample, typed UAV view and typed UAV store. Unsupported or failed query uses the previous RGBA8 format. No per-frame feature query.
- All stabilized-image allocations, encode UAVs and motion validation SRVs use that context's format. Raw calibration/jitter-probe images remain RGBA8. Motion reads RGB and the encoded alpha is always one, so packed10's two alpha bits lose no used signal.
- Fix `CameraFit::Record` to use each supplied typed image's actual format. Previously both image SRVs were unconditionally RGBA8; merely changing the allocation would make this view invalid. This correction also supports the RGBA16 experiment.
- Report the actual stabilized format in internal stats. Extend the existing end-to-end flow regression to pin slow-pan improvements and bound still/ordinary-motion errors when packed10 is selected. No extra user setting.
- Add an optional `--debug` run of that existing fixture to check D3D12 validation errors from the new views and resources.

Device checks follow Microsoft's [format support structure](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_feature_data_format_support), [resource/sample flags](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_format_support1) and [typed UAV store flag](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_format_support2). The optional format does not introduce a new minimum hardware requirement.

## R3: separate camera-fit result

`j2_compute_flow_test.exe 1920` still fails the confirmed-raster camera-fit assertion on a 6-pixel synthetic pan, with all three storage formats. The measured-total path, cut handling and precision cases in that same execution pass.

Baseline confirmed-raster fit at 1080p: 11 collected, zero accepted/flow-passed; final samples 12995 -> 12995, energy 7.9533 -> 7.2417, alternate hypothesis 7.9026. The required energy reduction is not reached (7.2417/7.9533 is about 0.91, versus a required ratio below 0.7). Image-validation cells and regions are zero because execution has not reached that gate. At 512x256, 1017 samples give 3.1823 -> 0.0130 and all 963 held-out cells improve.

Packed10 gives 12936 samples, energy 7.9561 -> 7.2460, alternate 7.9055: materially the same rejection. This rules out the proposed storage precision as its remedy. The raw search field versus fitted camera model remains Fable's next discriminating inspection; a periodic-fixture search ambiguity is a hypothesis, not an established cause. Assertion retained, failure not relabelled as a pass. It does not gate the new camera-independent total-motion path.

## Reproduction and receipts

Worktree: `C:\Users\Cort\NA-camera-runtime-audit`, branch `astra/camera-precision-audit`; build `local/audit-build`, SDKs under `C:\Users\Cort\Documents\NeuralAliasAbundance-SDKs`. No SDKs, runtime DLLs or captures committed.

- `local/precision-audit/baseline-build.log`, `baseline-tests.log`, `baseline-tests-detail.log`
- `r8-1080p-1.log`, `r8-1080p-2.log`, `r8-small.log`, `r8-binary-hash.txt`
- `r16-build.log`, `r16-1080p-1.log`, `r16-1080p-2.log`, `r16-small.log`, `r16-binary-hash.txt`
- `r10-build.log`, `r10-1080p.log`, `r10-small.log`, `r10-binary-hash.txt`
- Final capability-selected build and suite: `final-build.log`, `final-tests.log`, `final-tests-detail.log`
- Debug validation: `final-debug-build.log`, initial `final-debug.log`, corrected `final-debug-corrected.log`; final large run `final-1080p.log`, `final-binary-hash.txt`

Build the existing `j2_no_camera_test`, `j2_live_test`, `j2_compute_flow_test`, `camera_fit_gpu_test`, `native11_dlaa_test`, `native11_provider_test` and CPU depth-rule targets in Release. Run the two no-camera modes, J2 live/flow, camera-fit, provider, five native11 DLAA variants (direct/automatic/companion/two cold starts) and CPU total-motion/depth-rule/arbiter. Run the flow executable with `1920` for the separately reported large case, and `--debug` for descriptor/resource validation.

No new game capture is needed to distinguish these results. Real-game visibility, performance and compatibility with every scene remain separate from these hardware fixtures.
