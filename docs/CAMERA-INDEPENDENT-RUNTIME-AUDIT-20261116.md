# Camera-independent runtime: independent hardware audit

2026-11-16 — Astra. Audited Fable's `3141125d` runtime at `77afc711`, PR145.
Audit branch: `astra/camera-independent-runtime-audit`. No production edits, packaging, deployment or game testing.

## Verdict

The first native-DX11 implementation works on the actual NVIDIA GPU. It can start with camera discovery disabled, obtain total image motion, generate raster jitter, and deliver real NGX DLAA. This is a meaningful removal of the camera dependency, not an identity-matrix substitute. The existing camera-backed test paths also continue to pass at their standard fixture size.

I approve this implementation direction and the demonstrated DX11 path. There is one small depth-state correction to make (R1), a measured slow-pan quality limitation (R2), and an additional camera-fit test failure at 1080p to keep visible (R3). This is not approval of an already-complete universal renderer: DX12 integration, backward scene selection and partial-layer jitter remain the explicitly outstanding work. None requires another speculative user capture to act on this audit.

## R1 — P2: opposite depth proofs are not required to be consecutive

Owning code: `src/host_dx12/native11/depth_direction.hpp:131-148`. The header, `kFlipFrames` comment and COMMS handoff all promise three opposite proofs **in a row**. `Decide()` increments `against_` for opposite evidence but does not clear it when evidence is uninformative; `EndFrame()` only clears the observations.

Executed reproduction with the real header:

1. Prove LESS with clear 1 and 400 LESS_EQUAL writes.
2. Prove GREATER with clear 0 and 400 GREATER_EQUAL writes: direction stays LESS, flips 0.
3. An empty/uninformative frame: retain LESS.
4. Another GREATER frame, then another empty frame: still LESS.
5. A third isolated GREATER frame: direction changes to GREATER, flips 1.

The added temporary regression failed exactly at step 5 (10 existing cases pass, this case fails). Receipt: `local/audit-build/depth-repro.log`. This proves a contract mismatch; it does not prove this was the cause of any user's flicker.

**Correction for Fable:** keep `proven_` across unknown frames, but clear the pending opposite streak across an unknown or skipped frame. Cover both an unknown `Decide()` and a frame that reaches `EndFrame()` without a decision. Add the interrupted-proof sequence above and retain the existing uninterrupted-three-proof test. Do not clear the proven direction and thereby introduce unnecessary DLAA misses.

## R2 — measured quality limitation: slow pans are attenuated

This is beyond the CPU reference agreeing with its shader: known scene translations were rendered into input pictures, put through the real AMD search, total-motion refinement and GPU arbiter, then read back. Unused camera matrices and jitter inputs were NaNs, and the camera tier was disabled. All interior vectors used estimated provenance, none camera/engine provenance.

512x256 fixture, alternating executed raster jitter:

| True current-to-previous displacement | Mean measured X | Mean measured Y | Mean vector error |
| --- | --- | --- | --- |
| stationary | 0.0000 | approximately 0 | 0.0002 px |
| -0.125 px | -0.0714 | -0.0102 | 0.0823 px |
| -0.250 px | -0.1807 | -0.0190 | 0.1258 px |
| +0.250 px reversal | +0.1909 | +0.0173 | 0.1107 px |
| -3 px | -2.9991 | +0.0003 | 0.0140 px |

At 1920x1080 the corresponding errors are 0.0002, 0.0823, 0.1248, 0.1105 and 0.0138 pixels. The total-motion checks pass at both sizes. The slow-pan limitation therefore occurs on a smooth textured fixture too, not only aliased hard edges. It is not a sign inversion, camera leakage, jitter-only motion or complete rejection of movement. A 100% estimated-pixel count here does not mean 100% accuracy.

**Concrete next improvement, without replacing the flow engine:** preserve the current stationary/reversal fixtures while checking where the remaining subpixel error enters the existing stabilized-picture/refinement path. The shader matches its CPU reference, so another wrapper/sign fix is not supported by these results. Compare the same controlled images before and after the RGBA8 stabilizing encode to separate quantization/resampling error from the two-step refinement/zero-versus-moving selection. Keep proven camera motion where it helps. Do not amplify all flow or loosen thresholds globally: the still-image error is already near zero. This is a quality follow-up, not grounds for rejecting a working first increment.

## R3 — recorded failure in the camera-fitted path at 1080p

The extended `j2_compute_flow_test.exe 1920` run fails its **existing** confirmed-raster camera-reconstruction assertion: the fixture expects a -6 px pan, but measures a mean error of 6.0000 px. Stationary error remains 0.0001 px. The new camera-independent checks and cut checks subsequently pass.

To distinguish an audit-test regression, I restored the entire original test source from `77afc711`, rebuilt it against that runtime and reran the same command. It fails identically, at the original line 150 (`moving/mn < .35`). Restored the audit source afterwards. Logs: `flow-audit-1080p.log`, `flow-baseline-1080p.log`, `flow-baseline-build.log`.

This establishes a failure in the delivered baseline's larger camera-fit fixture, not its introduction date, nor failure in an actual game. The 512x256 version and separate `camera_fit_gpu_test` pass. **Fable follow-up:** examine the fit admission/sample coverage at 1080p using this existing fixture; report whether it is a fixture assumption or the fit path returning no correction. Do not change the new total-flow vector sign or suppress this assertion to obtain green results. The default Optional mode still uses a found camera, so retain this case while the adaptive supplier selection is built.

## What was verified

Final registered regression run: **11/11 passed**, 104.13 seconds, including seven GPU-labelled tests. The two additional failing investigations R1/R3 are reported separately below and are not hidden by this green standard-suite result.

- **Cold start, actual NGX, direct:** 48 synthetic DX11 frames; 46 acquired, 46 delivered, 45 with nonzero production-scheduled viewport jitter; no test-supplied next jitter, no prior camera warmup, no discovered camera, no busy/rejected frames, one initial scene reset. `camera_setting=0`, camera supplier 0, measured supplier 46.
- **Cold start, actual NGX, companion:** same 46/46 delivery and 45 raster samples; companion started 1, available 1, failed 0, frames 46, refused 0. This is the actual consumer's separate-process/two-list path on a 64-bit synthetic host, not a live 32-bit game.
- **Live transitions:** the existing provider/real-DLAA tests switch Required -> Off -> Optional -> Required; camera-backed and camera-free delivery checks pass. The automatic API route and companion tests pass too.
- **Shader/reference:** eight controlled cases, 8,960 interior pixels each. Zero validity-mask disagreements. Three of 71,680 vector comparisons exceed 0.03 px (maximum 0.044899); the others stay within it. Hardware bilinear precision is not CPU bit-exact; the test reports all discrepancies and allows at most 0.5% threshold-adjacent outliers. Includes stationary/wrong raw search, 1/8 and 1/4 pixel, reversal, 3.4 px, flat unknown/approximate and scene cut.
- **Actual flow-to-arbiter:** known translations and alternating raster jitter; poisoned unused camera inputs; estimated provenance only; stationary jitter stays stationary. Existing residual and camera-fit cases at the default size also pass.
- **Cut handling:** actual AMD scene-cut detection and production `cut_show`: the detected cut displays current-picture value 250 instead of synthetic temporal-output witness 47; next submitted reset restores 47; subsequent frame does not ask another reset. This validates the GPU cut-protection protocol, not the artistic quality of real DLSS around a game cut. Real NGX delivery is verified separately above.
- **Cost:** reported flow recording GPU time approximately 0.12-0.13 ms at 512x256 and 0.57-0.64 ms at 1920x1080 on RTX5070Ti for these inputs. This is the flow path's timestamp interval, not total DLAA cost or a game FPS prediction; no 3440x1440 extrapolation.

## Source audit

`native_d3d11/live_dlaa.cpp` separates submitted flow picture identity, camera tracker identity and delivered image identity. Scene continuity no longer includes camera constant-buffer identity. Missing camera chooses total motion with the camera tier disabled; the old residual path is not handed to DLSS as total motion. Optional-camera routes and their existing post effects are preserved; camera-dependent ambient shading is explicitly unavailable without projection information. Raster facts come from the executed jitter path, not a fabricated camera record.

`j2/compute_flow.cpp` retains per-recording resources through submitted leases, seeds only after submission, collects only retired readback slots, and carries cut protection on the GPU until a reset is recorded. The direct and companion paths submit the shared images back into their expected state even if evaluation fails; unsuccessful evaluation is not copied to the game. These source checks and fixtures are not proof against every possible render-ahead/failure interleaving. No new synchronous diagnostic readback has been added to a live callback; waits/readbacks in this change are test-only.

## Reproduction and artifacts

Worktree: `C:\Users\Cort\NA-camera-runtime-audit`. Build directory: `local/audit-build` (ignored). MSVC19.51, Release x64, SDKs at `C:\Users\Cort\Documents\NeuralAliasAbundance-SDKs`; real NGX library and release runtime enabled. Device: NVIDIA RTX5070Ti.

```powershell
cmake --build local/audit-build --config Release --target native11_dlaa_test native11_provider_test j2_compute_flow_test na_cpu_total_motion na_cpu_depth_direction na_cpu_motion_arbiter camera_fit_gpu_test --parallel 2
ctest --test-dir local/audit-build -C Release -R '^(na.cpu.(total_motion|depth_direction|motion_arbiter)|host_(native11_dlaa(_automatic|_companion|_camera_off_start|_camera_off_start_companion)?|native11_provider|camera_fit_gpu|j2_compute_flow)_test)$' --output-on-failure -j1
# Additional larger fixture (known camera-fit failure described above):
local/audit-build/tests/host/Release/j2_compute_flow_test.exe 1920
```

New cold-start CTests are registered in `tests/host/CMakeLists.txt`; no root CMake changes. Tests extend the existing fixtures rather than add a second runtime implementation.

Receipts in `local/audit-build`: `audit-final-build.log`, `audit-final-tests.log`, `audit-final-tests-detail.log`, `flow-audit-detail.log`, `flow-audit-1080p.log`, `flow-baseline-1080p.log`, `cold-direct.log`, `cold-companion.log`, `depth-repro.log`. The first parallel-8 build hit MSVC child-process/communication failures; rebuilding unchanged source at parallel-2 succeeded. The depth-interruption repro was temporary and its source restored; it is not silently included as a passing check. No game run is being claimed and no capture request is needed for R1-R3.
