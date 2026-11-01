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
