# Sniper temporal alignment repair and Opus review, 2026-11-03 Pacific

Astra's source repair starts at installed `07e38251d161a22bedc41838c7f1f308242ef7a3`. It reuses the camera-component fixes from Astra `2ffc23d` (preserved in PR83) and extends the existing projection matching. No game-specific addresses, shader hashes, new camera solver or motion-scale multiplier are introduced.

## Reproduced errors and corrections

J1 rejected a buffer without the combined view/projection even when it contained the exact projection or inverse used by another pass. Its cheap probe examined one offset, incorrectly excluding the whole buffer on a miss. CBV deduplication retained the first view's size, so a short view could hide a larger camera view at the same address. Projection matching supported only the camera block's storage layout.

The correction reads each bounded observed range once (maximum1024bytes per block), retains its largest view, and patches recognized camera components in either row/column storage. If a VP-only depth submission starts the sample, a later separately uploaded pure projection can be identified using the existing Projection and P-inverse-times-VP-is-rigid-view predicates before that projection is consumed. The loose VP fallback retains exact injected-sample provenance. Derived bases refresh when the projection is established later in the same frame. Existing write-ledger normalization and sample ownership remain in use.

Four regressions exercise the production observer/J1/J2 path on real DX12 resources with a fake NGX model. A GPU shader reads the projection actually available to its consumer and projects a point; its pixel shift must match NGX. Queue waits/readbacks are test-only.

| Case | Before correction | After correction |
| --- | --- | --- |
| Standalone projection in a different buffer/offset | GPU(0,0), NGX(-0.25,+0.166667) | GPU matches NGX |
| Short CBV followed by larger view at same address | Camera/evaluation absent | Camera and evaluation present |
| Projection consumed before main camera block arrives | Early GPU(0,0), later NGX nonzero | Early GPU matches NGX |
| Separately uploaded transposed projection | GPU(0,0), NGX nonzero | GPU matches NGX |

First two failures are against unchanged07e runtime with the new tests; latter two fail against the initial2ffc23d-only port. Logs: `local/se5-quality-20261103/jitter-{before,after}-tests.log` and `jitter-edge-{before,after}-tests.log`. No captures or SDK binaries committed.

Full Release build/suite:193cases,188PASS,5SKIP,0FAIL,268.50seconds. Skips: optional NR runtime probe; unsupported16xMSAA shape/resolve/depth32; upscaler-jitter fixture without its configured runtime. Existing native-NGX reconstruction, MSAA, post-entry, depth-state, overlay and lifetime tests ran. This is not a game visual pass.

Optional `dlss_real_test <DLL> --inspect-inputs` measures whether actual NGX changes known fractional motion input, with waits/readback confined to the test. On RTX5070Ti:0of262144float components changed, maximum error0. SDK overwriting motion is unsupported by this probe; it does not prove camera correctness or image quality.

Bounded reads replace an invalid exclusion shortcut. Existing jitter_cpu_us/max counters expose submit cost. Game CPU-performance for this repair is unmeasured; do not attribute the old shortcut's timing to this candidate.

## Latest game evidence:05dc62b, PID47592

`Documents/NA-se5-results-20261103-230657/manifest.json` identifies source05dc62bd61589a29fb0c2635c1c72631b926ac94 and DLL SHA25691AD9A850C007488FCA78786A7C87920890264E818EA4471F126A2802DA6579D. This run contains neither this repair nor later44b81fc/302138a HUD changes.

All ten J2 sets,300evaluated frames, were decoded. All300declare zero jitter. Sizes are1720x706,2000x820,2293x941 to3440x1411;9samples reset history. First seven sets are largely stationary; last three pan. In230634,27consecutive pairs show the stone-wall patch moving about10.9render pixels horizontally while median supplied motion remains0. Some regions have useful vectors: left-tree motion about4.8pixels with median vector4.0. These image-based estimates have repetitive-texture/mixed-depth limitations. Zeroing jitter alone has not repaired the moving-image/vector mismatch.

Counters:47308DLSS plus6955plain stretched fallbacks out of54263render-small upscale attempts,12.82%fallback. Present source449success/6700refusals; last reason "no selected scene depth and colour". Final AMD acceptance0is one still pair; a preceding moving sample accepts951805of2157713pixels (44.11%), recording0.461ms. Do not call flow uniformly0% or stalled. Summary's held share99.8% uses an inappropriate denominator:5929held/46859J2frames is12.65%;491extrapolations are a subset. No independent full-native Present capture was saved; evaluation counters do not establish final scanout contribution.

Witness reports15unseen decisions/14periodic retests. Its code sets resume_reset when decisions change and globally suppresses J1 while unseen. It is an active rendering policy, not just measurement. Preserve measurement, remove periodic automatic off/on while integrating the actual camera-component repair. Post-processing, reused pictures or a wrong movement field can fail this witness even if a raster pass was jittered. Zero-jitter windows cannot test this repair's subpixel coverage.

Decoded pictures/metadata/estimates: `local/se5-latest-47592-20261103`. Saved present planes are reduced proxies, not independent native-output/scanout witnesses.

## Opus proposal review

H1b's upstream-location idea usefully extends existing HUD machinery.44b81fc is a first one-hop increment, not completion of the approved arbitrary-chain/version-aware service. Corrections before its automatic redirection is ready:

- One same-sized bound SRV and matching format do not prove transparent transfer. Tone mapping, sharpening and blur can share them. Distinguish processing edges from proven whole-subresource copies; retain actual views/rects/content lineage.
- ListCurrentGraphicsSrvs reintroduces the known64-descriptor prefix blind spot without incomplete-result evidence. Reuse the existing sparse bound-range observer.
- TakeFrame keeps an upstream alive when u->redirected>0 even if final handoff no longer uses it: redirection validates its own stale ownership.120-present grace is not current-frame delivery proof. Preserve the original complete picture during uncertain/changeover frames; layer handoff must follow actual presented lineage. Do not remove HUD before fallback can recover it.
- Both replacement searches initialize to g_proxy[0] even when the loop excludes that entry. It can remain selected and overwrite the presented target/protected upstream. Start null and choose only eligible slots.
- Candidate counts do not reset on no-source frames, so "three consecutive" can bridge gaps. Track submitted versions and contradictions; the learned source currently lacks queue/recording-generation/content-version identity.

Bilateral-guided colour transfer is useful established photometric processing, but its residual is not a HUD classifier. Motion mismatch, disocclusion, bloom/DOF and filtering can all make the fit fail. Replacing those pixels with the small final image risks reintroducing moving foliage artifacts. Retain as a separate possible photometric extension after alignment, not a substitute for H1b/H2 and downstream delivery.0.3ms is a target, not a measurement.

Primary references: [NVIDIA DLSS integration checklist](https://developer.nvidia.com/blog/how-to-integrate-nvidia-dlss-4-into-your-game-with-nvidia-streamline/), [Streamline DLSS-G composition guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md), [original bilateral-guided-upsampling abstract](https://research.google/pubs/bilateral-guided-upsampling/).

## Handoff

Opus retains I1 continuity, H1b/display integration and motion-source investigation. Integrate Astra's camera-component patch, rather than reimplementing it. Cort requested review of this combined evidence/patch: check projection admission, both layouts, early-consumption regression, write ledger and submit cost. These fixes do not substitute for remaining motion/source/delivery repairs. No installer publication or visual-pass claim; Cort retains game testing. No game launch/control/deployment or native-file changes by Astra.
