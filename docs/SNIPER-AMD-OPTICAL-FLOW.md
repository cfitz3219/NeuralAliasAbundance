# Same-frame AMD optical flow for Sniper's inline DLAA route

Owner: Astra, at Cort's direction. Fable retains Anno/NVOFA. Reuses Opus aa68134's unmodified AMD shader import and DXC build step, pinned to FidelityFX FSR3 3.0.3 a0632abf1350bb64c098573d84c42f053f053a6e, MIT. No second solver or FidelityFX runtime DLL. Source/license notices remain in third_party/ffx-opticalflow.

## Data path

J2 resolves/copies its selected scene view and depth before DLAA as before. On the inline route, with Moving-object motion enabled, the same command list then encodes the scene using the existing fixed transform, runs AMD's optical-flow passes, expands the block flow with depth-aware validity, subtracts the matching camera motion and jitter exactly once, validates the estimate against the image, and supplies the residual to the existing motion arbiter. The arbiter adds accepted object motion to camera motion. DLAA and the game's later post-processing follow in that same list; existing RestoreListState restores the game bindings. No queue split, external optical-engine fence, one-frame-late prediction or CPU/GPU wait.

Between-list NVIDIA flow remains the preferred existing path. AMD also records in J2's own list when no NVIDIA ticket exists, including resolved MSAA colour. This does not qualify all possible games or change Anno's NVOFA matching rules. The existing camera-only DLAA continues if compute flow is unavailable/busy or a pair cannot be qualified.

Important correction to the draft plan: AMD's R16G16_SINT vectors are INTEGRAL PIXELS, not NVOFA S10.5. The existing expander's /32 is cancelled by vector_scale32, while its spatial block size remains8. Actual shaders on the RTX5070Ti reproduced both -4 and +4 pixel translations exactly; no inferred sign or units.

## Explicit pairs and lifetime

PairFlow records AMD's prepare-luma/SPD/histogram/divergence passes for both supplied images, then AMD search/filter/scale from coarse to fine. Both pyramids and temporary resources are initialized for every pair, rather than advancing hidden CPU-side history on Record. This costs an extra preparation of the previous image but makes discarded and replayed command lists deterministic. Shader algorithms remain unmodified. AMD's stream-start six-frame warmup is replaced by having a fully initialized explicit pair; histogram seeding is not treated as a scene cut. Current-pair scene-change bits reject the residual on the GPU.

Encoded images become history ONLY on actual J2 submission. Frame number, preceding DLAA frame, route generation, size, device, queue and jitter belong to that pair. Reset/cut/resize or a missing matching history image seeds camera-only history. Previous images must have a Ready recording lease on the consuming queue.

Resources, descriptor slots and both images are retained using the existing native::RecordingLease, through Reset/destruction AND every GPU submission/replay. A GPU-complete but still executable list cannot have its resources overwritten. Each previous image additionally retains the consumer lease. Busy slots skip flow rather than stall. New-size contexts are independently allocated; old contexts stay in their recording payloads. Working passes start and finish in defined states, so discarding commands does not poison CPU-side state assumptions. Telemetry is copied asynchronously and read only after recording retirement; discarded commands cannot publish unwritten counters. No synchronous diagnostic GPU readback occurs in game callbacks.

## Controls, builds and tests

Existing Moving-object motion switch selects the available scheduling path automatically; no Sniper executable hash, offsets or shader hashes. [j2] flow defaults0; safe startup clears it. Inline status names AMD compute and shows recorded/submitted pairs, busy skips, accepted-pixel count and GPU duration. j2compute appears in the periodic log. DXC is needed at build time (Windows SDK detected); SM6.2/wave support is checked on the device. Without the shader build this capability is unavailable, while camera-only DLAA remains.

Automated evidence includes:

- Real AMD shaders: +/-4px, stationary images, discarded/re-recorded pair, correct direction/units.
- Real AMD -> depth expansion -> residual -> arbiter: moving textured object over stationary background, all9216 sampled object pixels correct, zero distant-background motion errors. At512x256 about0.10ms, and1920x1080 about0.42ms on this RTX5070Ti in the synthetic workload. This is added flow preparation/fusion time, not full game or DLAA time, and not a game performance promise.
- Recording lifetime: completed-but-executable slot refused; reset/generation/missing history/cut/abandoned recording cannot become stale history.
- J2 inline fixture with larger array/mipped scene, resolved depth, fake DLSS runtime: AMD pair submitted before evaluation, original post-processing runs after it, state restored, device survives. This is actual shader orchestration with a stand-in DLSS model, not Sniper visual acceptance.

Cort retains both Sniper live checks: enable Moving-object motion, inspect stationary scenery and moving characters while standing still and panning; compare trails/shimmer, check GPU time and j2compute log, then toggle off/on. No game launch, control or deployment by Astra. Prebuilt reversible test installers are prepared separately using the existing Sniper add-only transaction/manifest and retaining the MIT notice. Native game DLLs are never replaced.

References: AMD pinned host schedule sdk/src/components/opticalflow/ffx_opticalflow.cpp and vendored pass entry points; [AMD optical-flow documentation](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/optical-flow/). Fine geometry, transparency, occlusion and block-boundary quality remain estimation limits, not proof that motion is exact for every pixel.

Execution also requires 32/64-lane waves, matching AMD's pinned search shader's 64-thread reductions and MaxWaves2. WaveOps alone does not establish this: Microsoft's software renderer accepted all PSOs but returned zero translated motion on CI. The runtime now rejects unsupported wave ranges before allocating the dispatcher. Numerical execution tests carry the `gpu` label; the ordinary shader/PSO test still runs on CI and checks the unsupported-WARP refusal. NVIDIA's 32-lane hardware path remains covered by the actual numerical GPU tests on this PC. AMD shader source is unchanged.

## Delivery receipt

Runtime source: `a450adfcb1a54692e0087f16f5b036b59b3f0559`, PR74 stacked on PR73. The integration includes Fable's newer Anno changes through c4a6e44. Anno's NVOFA implementation was preserved from that branch; AMD's encode/accept shader helper is an independently retained snapshot of the existing NeuralAlias helper, not a replacement for Fable's temporal filter. The earlier description of shared helpers does not mean the Anno filter was ported into AMD. Live jitter/edge quality remains part of Cort's game check.

Final Release build passed with real local NGX/NVOF and embedded AMD DXIL. The clean, subsequent full CTest run completed in 246.32 seconds: 152 passed, 0 failed, 5 skipped out of 157. Skips: opt-in NR model probe, three unsupported 16x-MSAA cases, optional upscaler jitter fixture. All four AMD/inline tests ran and passed. Earlier runs during integration are not substituted for this final run. Evidence: `local/amd-flow-reference/final-build.log`, `final-tests.log`, `hd-compute-test.log`, and the focused logs in that folder.

Prebuilt, reversible candidates (prepared, not deployed):

- SER: `C:/Users/Cort/Documents/NA-SER-AMD-flow-a450adf`, updater `C:/Users/Cort/Documents/NA-ser-flow-update.cmd`, rollback `C:/Users/Cort/Documents/NA-ser-flow-rollback.cmd`.
- SE5: `C:/Users/Cort/Documents/NA-SE5-AMD-flow-a450adf`, updater `C:/Users/Cort/Documents/NA-se5-flow-update.cmd`, rollback `C:/Users/Cort/Documents/NA-se5-flow-rollback.cmd`.

Both contain DLL SHA256 `CB83E99F8289A05FF0DA38F1A6C915B147AA4E16BE54D0C9DD8C35AC93E105DB`. Existing ordinary DLAA commands remain unchanged. Close the respective game before updating. Flow starts off; enable Home > Moving-object motion (test). The package reuses the existing Sniper installation receipt, original-file snapshot and settings carry-over; payload validation happens before retiring an existing candidate. No Git checkout/build occurs in the updater. The MIT notice accompanies the installed shader code. Native game DLLs are never replaced.

Windows PowerShell 5.1 installer fixtures passed separately for SER and SE5: additive install, notice, default-off, corrupt replacement leaving the previous install intact, valid receipt upgrade, rollback restoring the original directory, and refusal to replace pre-existing native DLSS. These tests use fake game directories. Neither Sniper has been launched, controlled, installed into or visually qualified by this task. AMD shader correctness on the synthetic cases and successful inline orchestration are measured; actual Sniper picture quality and GPU cost are still the user's live tests.

This delivery supersedes the staged dab9462 packages, which were never installed. The final capability correction and full suite above apply to a450adf. The synthetic HD timing was measured on dab9462 with the same shader/dispatch math; the new admission check is also exercised by the final RTX tests.

Runtime CI receipt: GitHub run 41263196753 completed SUCCESS on Linux portable/CPU, Windows no-SDK and Windows public-SDK at a450adf. Hardware numerical evidence remains the separate local RTX run above; CI's software renderer is not presented as optical-flow accuracy evidence.

## Familiar-command correction after the first user run

Cort reported a stable Sniper run but no visible flow UI. The recent 19:33-19:36 Resistance run used installed cf3855f (October27), DLL B7B0E56BF05A894E8322FD786FD8AF47D84F188B47DD458A256F4F2952393C2A; it contains no AMD flow. SE5 had a450adf installed but no run log. The report is useful for the old Resistance baseline, not AMD-flow acceptance. Leaving familiar DLAA commands on old scripts caused this delivery ambiguity.

Corrected the package generator to publish both ordinary DLAA and flow update/rollback commands to the same verified candidate, after validating BOTH packages and preserving all prior launcher bytes. Existing PS1 build templates remain source templates, while the normal commands call prebuilt package deploy.ps1 directly. Two command-publication tests passed (matching routes/backups; corrupt second package preserves every old command). No runtime/solver/UI-code change or rebuild was needed: a450adf already has Home > Picture > Live DLAA > Moving-object motion (test), and an AMD compute status with pairs, submissions, accepted pixels and GPU cost.

Current `C:/Users/Cort/Documents/NA-ser-dlaa.cmd` and `NA-ser-flow-update.cmd` both install SER a450adf; the SE5 pair likewise selects its a450adf package. Matching rollback commands share the existing receipt. All eight prior launchers backed up under `C:/Users/Cort/Documents/NA-Sniper-launchers-before-a450adf-20261031-194042-511194`. Runtime hashes unchanged. Installer commands were prepared only; actual installed SER/SE5 DLL hashes remained unchanged. Close Resistance, run its normal command, then enable Moving-object motion in Home. No subsequent AMD game result is claimed.
