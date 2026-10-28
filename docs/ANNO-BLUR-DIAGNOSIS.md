# Anno stationary blur diagnosis — 26 October 2026

This is diagnostic evidence, not a rendering fix or a general DLAA quality pass.

## Demonstrated cause

The generated motion input blurs stationary geometry. An eight-frame real Anno capture, replayed through the same NVIDIA runtime at native3440x1440, reproduces the live blur. Replacing only the motion texture with zeros restores fine roof tiles and railings. Reversing the jitter sign or replacing jitter metadata with zeros does not restore them. Zero motion is a stationary control, not a solution for a moving camera or moving objects.

Both input and DLAA output were copied before game post-processing. The input has clear detail; the output is already soft. A downstream SMAA pass is therefore not required to produce this failure.

## Measurements

- Source under test: deployed Opus2315633 plus capture-only6ae9bd6. No AA coexistence changes or rendering correction included.
- Native3440x1440; single-sample color/depth; AA Off; eight frames each with injected jitter ON/OFF.
- ECC registration calibrated using a known(+0.5,-0.25)pixel translation. Roof example jitter delta(-0.25,-0.5556), measured color(-0.2736,-0.5505). Cliff and second building follow the same convention. Depth follows the shift too. These regions contradict a simple reversed/missing raster jitter explanation.
- Jitter-OFF roof movement stays under0.001pixel; generated roof motion is approximately(-0.08643,+0.02945)pixel. It should be close to zero for that stationary surface.
- Manual replay:64 evaluations per variant, equal dimensions, HDR/auto-exposure matching live J2. Eight captured frames repeat; this is valid for static-region isolation, not moving-water quality evaluation.
- Roof log-color mean absolute error between original replay and live output:0.002325. Difference from original replay to zero-motion replay:0.022009. Corresponding cliff values0.001052 and0.009252.
- Descriptive roof horizontal-gradient magnitude: original0.02252; zero-motion0.02854; reversed-jitter0.01987; zero-jitter0.02202. These values are not acceptance thresholds and sharpening/aliasing can also raise a gradient metric; the controlled input substitution plus visible recovered structure is the evidence.
- NVIDIA DLL SHA256D00B5B0AB29C3CA101B70C3A5ECB3A01B7EF33766A3383A28A18623083E6123E matches live and replay.

## Scope and limitations

The exact arithmetic origin of the motion error needs camera-matrix evidence. Separate float inverse/current and previous projections are a suspect because the shader reconstructs world coordinates then projects them again, but that mechanism is not established by these captures alone. Opus retains production ownership. No blanket motion threshold, sign flip, preset change or sharpening fix was applied.

The saved SMAA1 flag does not establish actual executed SMAA. A targeted flag comparison is being completed separately. An external RenderDoc1.46 attempt crashed before gameplay, so no shader-pass capture was obtained.

## Evidence and diagnostic tooling

Local evidence is in `local/blur-isolation/`: jitter-on/off raw planes and metadata, registration.txt, replay-*.raw, replay-check.txt, motion-cause-comparison.png and scripts. Captures, DLLs and game data are intentionally not committed. Diagnostic branch adds manual `blur_replay`, asynchronous capture calls and camera sidecars; it must not be merged as a production fix.

The first capture session completed both batches, then Windows recorded dxgi.dll c0000409 during requested Save-and-Exit(PID34776,17:13:21). The instrumentation is not release-qualified. Capture worker shutdown is a possible explanation, not yet proved. Restoration of the original DLL is required.

## Follow-up: numerical mechanism and SMAA comparison

Captured the actual inverse-current, previous and current matrices in a second eight-frame session(1b615c6 instrumentation). On the same roof, actual GPU motion median(-0.085999,+0.020203)px; separate float matrix calculation gives(-0.085938,+0.020244); double calculation gives(+0.000730,-0.001247). Composing inverse-current times previous in double, then casting the combined transform to float, gives(+0.000732,-0.001241). Individual-pixel CPU/GPU results differ because of arithmetic evaluation; this is not a bitwise GPU reproduction. The large stationary motion bias is consistent with loss of precision in the separate transforms.

Crucially, recomputing motion from those captured matrices using the double-composed transform and replaying the same captured color/depth/jitter restores roof tiles and railings, without forcing motion to zero. Original and composed replays each execute64 real NVIDIA evaluations. Roof gradient0.02285 versus0.02876(descriptive only). This supports the numerical mechanism, not merely a zero-motion workaround. Local matrix-result.txt, smaa-analysis.py, smaa-replay-{original,composed}.raw and precision-cause-comparison.png contain the evidence. Production implementation and moving-camera qualification remain Opus's task.

Verified Custom/SMAA changed1->0 with exactly one byte different from the original config; AA stayed0. This did not remove the blur. At matching jitter, roof pre-NGX gradient0.03437 vs0.03455, NGX output0.02254 vs0.02269, finished frame0.02271 vs0.02290. This flag test does not identify whether an SMAA shader actually executes; no shader trace was obtained. It does show the saved flag is not a remedy for the observed failure. The causal replay already isolates the loss before later processing.

Setup correction: initial config edit selected the unused Low preset due to matching the QualitySetting value instead of the Custom object. It was caught, the unused launch exited at the title menu, and the corrected one-byte config verified before the comparison. No result was attributed to that invalid setup.

## Restoration

Both gameplay capture sessions completed their files and then crashed during requested Save-and-Exit: PID34776 at17:13:21 and PID41684 at17:19:25, dxgi.dll c0000409. No capture::Shutdown caller was found; capture-worker teardown is a suspect. The diagnostic branch is evidence tooling, not suitable for production merge.

After the process exited, original dxgi.dll restored and hash verified7753F5A969E3EF12FE83624FFD508708BFAB6B25657A4541598723E5BEA44D97. Exact original engine.ini restored, hash91683D816F06083CEC3C3B016784A8E57EB06C61217184486DAA9C7EA02B2BC5. Camera jitter restoredON before first exit and defaultON during final capture. No rendering fix remains deployed. Game is closed.
