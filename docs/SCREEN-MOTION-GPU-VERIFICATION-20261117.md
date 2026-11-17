# Screen motion and replay fallback: independent GPU verification

Runtime: Fable `644b10558205cc9588a61af5de1493014c89b0fa`, on `claude/project-thread-axt148` (PR145). Auditor: Astra. Hardware: NVIDIA GeForce RTX 5070 Ti. Release x64, SDK-enabled MSVC build. This increment adds independent tests and records their results; it changes no production code.

## Decision and scope

The GPU verifies both intended repairs: a partial game/replay field no longer erases measured background motion, and flat/unmeasurable pixels use the fitted screen movement instead of an arbitrary search result. The supplied repeating-pattern and strict camera-fit cases still pass. Approve these repairs for Fable's integration, with the measured cost below explicitly disclosed.

These are executed GPU fixtures, including real NGX delivery checks, not a claim that Cort's Resistance image is now free of shimmer. No game was launched, controlled, installed or changed. Partial-layer jitter, changes between viewport and matrix jitter, and replay's separate performance cost remain outside this repair. The flat fallback estimates the screen's movement; it does not recover independently moving, invisible detail in an untextured object.

## Source checks

- `src/host_dx12/j2/live_dlaa.cpp` keeps `tier.total` and `tier.total_valid` in `Inputs::estimated_*` before native/replay writes `tier1_*`. The later source selection no longer overwrites the fallback. The intentional no-motion diagnostic also clears the new flag.
- `src/host_dx12/native_d3d11/live_dlaa.cpp` uses the same separate estimated slot, including its frame evidence's `flow_ran` field.
- `src/core/motion_arbiter_gpu.cpp` checks extent and validity, binds the additional SRVs, and chooses native/replay, then estimated, then camera, per pixel. Nonfinite vectors and malformed masks cannot masquerade as valid tier-1 data.
- `compute_flow.cpp` stores the fit head in bytes 112 through 191 of a **192-byte** readback. The handoff's phrase "192 words" was a units typo, not an allocation defect.
- All waits and explicit readbacks introduced here are in test utilities. Production submission, resource retirement and collection code is unchanged.

## Replay holes and the estimated fallback

The test dispatches the actual arbiter over 8,192 pixels. Eight repeating categories cover valid replay, absent replay, a NaN replay vector, a NaN replay mask, an out-of-range replay mask, absent estimated validity, infinite estimated motion and both sources invalid. Measured fallback validity includes both 1 and 0.5. It runs all eight combinations of replay on/off, camera present/absent and 32/36-byte counters.

**Result: zero vector or provenance errors in all eight combinations.** With replay on, 1,024 pixels retain ENGINE motion and 4,096 retain ESTIMATED motion. Replay off yields 5,120 ESTIMATED pixels. Remaining pixels use the valid camera or stay invalid, as appropriate. The ninth counter is checked only in the 36-byte variant; the original eight-counter layout passes the debug layer too. Output finiteness is explicitly asserted.

This reproduces the concrete collision identified from Cort's report. It verifies per-pixel arbitration and both host wiring sites; it does not measure the quality or coverage of replay-generated vectors in a game.

## The screen fit agrees with its reference

Controlled samples exercise all eleven terms with varying depth, a moving corner outlier, all-far depth and insufficient samples. The unchanged production `fit_accum` and `fit_solve` programs run 12 solves plus final validation. They are compared with `screen::Fit` on exactly the same samples.

| Controlled case | Samples GPU / CPU | Agreeing GPU / CPU | Nearness limit GPU / CPU | Largest coefficient difference |
|---|---:|---:|---:|---:|
| Varying depth | 2,048 / 2,048 | 2,048 / 2,048 | 0.087338 / 0.087338 | about 0.0000002 |
| Independent corner | 2,048 / 2,048 | 1,898 / 1,898 | 0.087338 / 0.087338 | about 0.0000002 |
| All depth at far | 2,048 / 2,048 | 2,048 / 2,048 | 0 / 0 | below printed precision |
| Too few samples | 64 / 64 | 0 / 0 | 0 / 0 | correctly invalid |

The next test generates two mostly flat pictures with known eleven-term movement, supplies matching raw proposals on textured pixels, injects deliberately wrong proposals into flat areas, and runs the actual `fit_sample -> fit_accum/solve -> measure` chain. The true coefficients are not passed to the shaders. This isolates image confirmation and fitting; the separate complete-production test below also runs the real search. GPU block decisions/vectors are compared with the CPU image sampler, then GPU samples are fed to the CPU fit to isolate solver precision.

| Image case | Fitted samples / agreement | Flat pixels checked | Mean flat-motion truth error | Wrong flat validity |
|---|---:|---:|---:|---:|
| Varying depth | 1,024 / 1,024 | 103,936 | 0.04160 px | 0 |
| Independent corner object | 1,024 / 944 | 96,454 | 0.04142 px | 0 |
| All-far depth | 1,024 / 1,024 | 103,936 | 0.08428 px | 0 |
| Nonzero jitter (+0.375, -0.25) | 999 / 999 | 99,820 | 0.00683 px | 0 |
| Conventional depth, far = 1 | 1,024 / 1,024 | 103,936 | 0.04160 px | 0 |

All checked flat pixels carry validity 0.5. Removing the model makes those flat pixels exactly zero/unknown; they never inherit the injected `(17,-13)` search noise. The independently moving object's measurable interior has 2,292 of 2,348 pixels (97.6%) within 0.75 px of its own movement, so the fit does not simply replace the object's vectors with background motion.

CPU and hardware bilinear arithmetic are not claimed bit-identical. Non-jittered block sampling matches exactly; the jittered case has one decision difference and three vector differences above 0.03 px among 3,072 grid locations. Across 168,960 interior pixels, validity differences are 1 to 50 and vector differences above 0.03 px are 217 to 615, depending on the case. These stay below the declared 1% comparison bound. CPU/GPU fitted coefficients differ by at most approximately 0.0000005. These comparison tolerances were not weakened to obtain a pass.

## Complete production path and GPU cost

An additional test runs `compute_flow::Record`, including the real AMD search, encoding, candidate processing, screen fit and measurement, on fixed alternating image pairs. Two thirds of each image are flat; motion is a three-pixel translation. No camera matrices are supplied. Runs alternate fit OFF, ON, ON, OFF at each resolution, with six warm-up frames followed by twenty timestamp samples per block.

The final pair's actual full-resolution outputs are read back:

- 1920x1080: **634,352** interior flat pixels per block, zero bad pixels. ON mean motion error **0.015572 px**, all validity 0.5; OFF all zero/unknown.
- 3440x1440: **1,506,464** interior flat pixels per block, zero bad pixels. ON mean motion error **0.016150 px**, all validity 0.5; OFF all zero/unknown.

Timing covers the whole flow stage, not just the solver and not total game frame time. The output readback is recorded after that stage's end timestamp. It is a test-only check.

| Resolution | OFF block medians | ON block medians | Added cost, mean of paired medians |
|---|---|---|---:|
| 1920x1080, initial run | 0.5867 / 0.5855 ms | 1.7984 / 1.7961 ms | 1.211 ms |
| 1920x1080, output-checked run | 0.5868 / 0.5892 ms | 1.7985 / 1.8003 ms | 1.211 ms |
| 1920x1080, explicit-finiteness rerun | 0.5903 / 0.5883 ms | 1.8052 / 1.7601 ms | 1.193 ms |
| 3440x1440, initial run | 1.2662 / 1.2700 ms | 3.5452 / 3.5483 ms | 2.279 ms |
| 3440x1440, output-checked run | 1.2700 / 1.2714 ms | 3.6387 / 3.7161 ms | 2.407 ms |
| 3440x1440, explicit-finiteness rerun | 1.2731 / 1.2726 ms | 3.4057 / 3.5581 ms | 2.209 ms |

All ON warm pairs have a valid model, 12 steps, 7,826/18,773 samples and 7,458/17,841 agreeing samples respectively. The spread between runs is disclosed rather than choosing only the fastest result. This repair is useful but not free; these numbers do not predict an exact game FPS change.

Before reducing robust iterations, Fable can parallelize the 81 independent reductions in `fit_solve` instead of executing 81 sequential reductions with group barriers. That is a source-level optimization opportunity, not a measured attribution of the entire cost. Preserve sample admission, outlier handling, finiteness and final validation when measuring a revised implementation. Astra has not started competing runtime optimization.

## Regressions and receipts

The existing 16 ambiguity cases pass at 512x256 and 1920x1080, both normally and with the D3D12 debug layer. Debug error count is zero at each size. At 1080p, the tested repeated 6-, reversed 6-, 12- and 6.5-pixel pans now report 100% of sampled interior vectors within one pixel, with zero far (>64 px) errors. Independent-region motion remains measured: 98.24% within one pixel for the moving region at 1080p; whole-image correctness 99.80%. These are the exact fixtures, not a universal periodic-texture claim.

The original default and 1080p tests, including the unchanged strict 0.35-pixel camera-fit assertion, pass. Camera-disabled DX12 delivery and submitted-recording depth proof regressions pass. CPU total-motion, screen-motion, camera-admission and motion-arbiter tests pass, as does compilation of all twelve production flow shader entry points.

Across the baseline, regression and final delivery selections, **18 distinct CTests pass, none fail and none skip**. The final eight-test delivery selection passes in 104.05 seconds, covering the new screen test, flow-resource retention, DX12 live fixture and five native DX11 real-NGX paths (ordinary, automatic, camera-off cold start, companion, and camera-off companion). Both camera-off cold starts deliver **46 of 46 acquired frames**, with 45 raster-jitter samples, zero busy/rejected frames and one initial history reset. The companion executable was rebuilt as a dependency, not borrowed from a prior package. These delivery counts are synthetic-host results, not Sniper's frame coverage.

Receipts remain in ignored `local/screen-fit-verification/` in `C:\Users\Cort\NA-camera-runtime-audit`: `build-baseline.log`, `baseline-tests.log`, `regressions.log`, `screen-fit-final-debug.log`, `screen-fit-timing.log`, `screen-fit-final-timing.log`, `ambiguity-debug.log`, `delivery-tests.log`, and `delivery-details.log`. `screen-fit-finite-timing.log` repeats the output check after strengthening explicit nonfinite assertions. No SDK, captured game data or runtime DLL is committed.

Reproduce using the SDK-enabled Release build:

```powershell
cmake --build local/audit-build --config Release --target j2_compute_flow_test native11_dlaa_test j2_no_camera_test j2_live_test j2_flow_retention_test --parallel 2
local/audit-build/tests/host/Release/j2_compute_flow_test.exe --screen-fit --debug
local/audit-build/tests/host/Release/j2_compute_flow_test.exe --screen-benchmark
local/audit-build/tests/host/Release/j2_compute_flow_test.exe --ambiguity --debug
ctest --test-dir local/audit-build -C Release -R 'host_j2_compute_flow|host_j2_screen_motion|host_j2_no_camera|host_j2_flow_retention|host_j2_live_test|host_native11_dlaa' --output-on-failure
```

The audit branch contains no installer or game modification. Fable retains runtime and combined-package ownership.
