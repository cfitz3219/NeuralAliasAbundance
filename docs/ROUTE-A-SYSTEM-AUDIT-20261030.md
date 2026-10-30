# Route A system audit — 2026-10-30

Author: Astra. Code reviewed: `5d6deaf`, `astra/pbz-rebuild-queues` (PR59).
Scope: the native-DLSS/TLOU route, including PBZ's use of that route. No Route B changes, game launch, deployment, or runtime repairs in this audit. Remote branches refreshed: no newer integrated Unreal control implementation was found. This is a control/data-flow and contract audit, not a claim that every line or execution interleaving has been verified.

## Conclusion

The pieces are not connected into one coherent engine-to-screen integration. Intercepting an NGX call is useful, but it does not itself control engine resolution, own native FG's scheduler, or prove that the enhanced real frame reaches presentation. We have working low-level components and significant missing integration. Previous standalone successes were too easy to mistake for evidence that the complete route worked.

The highest priorities are real/generated image delivery, the NR composition mask, actual engine mode control, and GPU-completion-based feature retirement. These are specific repairs to existing components; they do not require reinventing DLSS or rebuilding discovery.

## Findings and evidence

### 1. Picture-mode control stops below the engine that owns the resolution — confirmed

`ngx_observe.cpp:optimal_settings_common` changes answers to future optimal-resolution queries. `overlay.cpp`'s general Apply path nudges the window by one pixel. Neither requires an engine to discard a cached resolution percentage or query again. `adapters/tlou1/apply.cpp` does contain a real engine-rebuild adapter, but it is pinned to one TLOU executable hash. `adapters/unreal/cvar_probe.cpp` is read-only discovery, not an Apply implementation.

PBZ's earlier captures show 1000x1000 reference queries and an unchanged 1146x480 render size after attempted changes. This explains rebuilding without changing quality mode. NVIDIA's SR guide explicitly has the application apply the returned render dimensions to its viewport; an NGX feature recreation is not that operation.

Repair: finish the existing Unreal apply adapter on the appropriate engine thread, including the engine's update/notification path. Keep the generic query override for games that consume it. Confirm the next SR inputs' actual dimensions; do not equate window resize or CreateFeature success with a mode change. Preset selection is a separate operation and must not be diagnosed solely from resolution.

### 2. The FG UI controls our generator, not the game's generator — confirmed

`route_a_panel.cpp:DrawFrameGenerationRow` writes `fg_enabled`, `fg_mode`, and `fg_multiplier`, used by `fg/frame_gen.cpp` and our proxy presenter. Availability is `present::Enabled()`. Native FG detection changes the hint; it does not turn these into native Streamline option controls. FG preset requests use a driver profile, another distinct control path.

The legacy Streamline settings wrappers in `sl_proxy.cpp` log and forward. There is no implemented native `slDLSSGSetOptions` ownership/control path. Therefore native FG continuing to run does not establish that our multiplier selector controls it, and enabling a second presenter is not a solution.

Repair: retain native FG as presentation owner where the game already has it. Use that owner's supported options/control boundary for native controls. Keep our generator for the separate no-native-FG case. Do not stack schedulers.

### 3. Modern Streamline frame association is unfinished for our own FG — confirmed

`sl_proxy.cpp:slSetConstants` can decode V2 camera constants, but `slEvaluateFeature` creates `EvaluationScope` only for V1. Its V2 branch simply forwards, with a comment explicitly acknowledging absent viewport scope. `frame_gen.cpp:Generate` requires a matching camera/frame and also latches when the input queue differs from the output queue. These are actual unimplemented associations, not evidence that FG cannot work in modern games.

Repair: complete V2 frame-token/viewport association and a submitted-input dependency path before presenting our own FG as a modern-Streamline feature. This is secondary to fixing NR with the game's already-working FG.

### 4. NR composition uses an unstable exact-equality mask — confirmed behavior, live causality not yet measured

`native_fg.cpp` always calls composition with `UiComposition::EqualPixels`. `fg_composition.cpp` applies the enhanced result only if **all original full-color RGB components exactly equal HUD-free RGB**. Otherwise both images retain their original scene values. A pixel can therefore switch between enhanced and original due to a one-step color difference while NR evaluation and submission counters remain perfect. A translucent fullscreen UI layer can also remove enhancement across a large area.

The composition helper supports alpha-based reconstruction, and the contract reader collects UI/alpha, but the production caller does not select those modes. The actual captured PBZ run reports zero UI/alpha calls, so simply wiring the existing alpha option will not solve that run. NVIDIA specifies matching non-UI scene content; equality is a possible fallback under that condition, not a general guarantee of stable coverage.

Repair: acquire actual UI coverage or a scene insertion point before UI composition; use provided alpha where available. Measure equality-mask coverage in the failing path before attributing the whole flicker to it. Do not replace this with an arbitrary tolerance and claim correctness.

### 5. Modifying FG input does not prove the displayed real image changed — critical integration gap

`native_fg.cpp` writes the composed full image back to `DLSSG.Backbuffer`, and writes/rebinds HUDLess. It reads OutputReal mainly for status; it does not establish the relation between that input copy and the real image selected by the external presenter. The installed SDK describes OutputReal as optional, giving FG an opportunity to draw debug text on the real frame. Its presence is not a guarantee that FG always copies the modified Backbuffer to that destination.

The captured flickering run had periods of zero NR skips. Real/generated delivery mismatch remains a stronger explanation than claiming NR was toggled off. If native presentation keeps an earlier real-frame copy, enhanced interpolated frames can alternate with an unenhanced real frame. This is a plausible mechanism, not yet a pixel-proven cause in PBZ.

Repair: establish one enhanced real-frame image before the native scheduler takes the copies used for both interpolation and real presentation. Reuse native scheduling, tags and resources. Do not add a second late Present enhancement or blindly copy over a resource whose ownership is unknown.

### 6. Native-FG route ownership is a global handle count — confirmed

`native::OwnsRoute()` is `contexts_count != 0`. `nr_pass.cpp` suppresses input capture and Present NR while that is true, independent of viewport, swapchain, whether a context is evaluating, or whether it successfully enhances a frame. Release removes the context before forwarding the original release result. A retained inactive FG context can suppress NR; a release can immediately switch to a different NR pipeline while native presentation work is still draining.

The captured game did release its FG handle without the user changing settings. That explains the route switch in our code, not why the engine released it. Do not label it a user toggle.

Repair: associate route ownership with the actual view/presentation stream and its submitted lifetime. Handle suspend/recreate/drain explicitly using existing context/receipt machinery. A timeout alone is not a substitute.

### 7. Present fallback chooses latest completed guides, not a proven matching picture — confirmed gap

`nr_pass.cpp:OnPresent` selects the highest completed input serial, accepts same-queue ordering, and skips incomplete cross-queue inputs. This avoids a known dangerous wait but does not prove that those guides belong to the presented color. The code also consumes the selected serial before later allocation/processing can fail. The existing capture shows intermittent skips in this phase.

Repair: carry view/frame identity from SR through the real-image submission and consume only after a successful record. Integrate at an already-ordered real-frame boundary where possible. Do not reuse stale output or insert a blind queue wait to conceal skips.

### 8. Preset replacement uses CPU-call-count retirement — confirmed unsafe assumption

`ngx_observe.cpp` retires replaced SR handles at `g_evaluates + 8`. That counter includes calls other than the old feature's GPU work, including native FG. There is no completion fence in this retirement path. The old-feature handle map, retirement list and last creation snapshot are also global rather than keyed consistently by runtime/device/view. Retirement calls the current `CoreSlot`'s release function.

Eight CPU calls can occur while the GPU is still using an old feature. Multiple runtime instances can also route release through the wrong owner. These are credible freeze/device-failure mechanisms; the existing PBZ freeze trace does not establish that this particular path caused its stall.

Repair: retain owner runtime/device and per-feature creation data, retire on the actual submitted GPU completion, and serialize mapping publication/release. Reuse the fence/recording machinery already built for NR instead of inventing another delay.

### 9. Optional identity and state coverage remain incomplete — confirmed limitations

`fg_identity.hpp` uses native frame ID when available. Without it, each index-1 call starts a new serial; duplicate index-1 evaluations cannot be distinguished from a new real frame. A repeated first call can therefore run temporal NR again. PBZ's capture reports no native frame-ID calls. `fg_frame_cache.hpp` warns that cached reuse requires submission ordering; the caller relies on native FG ordering rather than recording an independently demonstrated dependency.

`fg_states.cpp` tracks legacy transitions on the current list. Enhanced barriers invalidate the tracked state instead of being decoded; unobserved full-color state still blocks copy-back. These can cause skips in valid native pipelines. The NGX input-read contract is not permission to invent a writable resource state.

Repair: obtain real-frame identity from the native presentation/Streamline frame boundary, preserve deduplication there, and acquire actual color state/layout at that boundary. Keep the useful existing cache and lifetime tracking.

### 10. HDR conversion is custom approximation; cost remains display-sized

Pre-FG NR infers encoding from HDR flag plus texture format. It maps HDR to clamped SDR, then applies a bounded brightness ratio back to the HDR original (`nr_shaders.hpp`). That is our approximation, not NVIDIA's reference HDR integration. It needs image validation with nonzero NR changes; a zero-delta round-trip test proves only preservation when enhancement is absent. The source sample confirms post-tonemap display-resolution NR with correctly scaled guide data. Ultra Performance lowers SR input size but does not lower this NR output workload.

This is an image-quality/performance limitation, not the first explanation for route switching. Fix frame delivery and composition before tuning HDR or sharpening.

## What the tests actually establish

Reran the existing current `build/pbz-queues-20261029` focused suite: 18 passed, actual-model opt-in probe skipped, 4.57 seconds. No new rendering code was tested or changed. An initial run against older `pbz-clean-20261029` ran only three tests and is not used as current-suite evidence.

Reviewed `nr_runtime_probe.cpp`: it loads the NGX core directly and explicitly copies generated output and the mutated input into its own DXGI chain. It does not load/reproduce the game's Streamline scheduler. Thus it assumes the very delivery link in finding 5 that needs qualification. Existing composition tests correctly validate the chosen equality fallback but do not establish that its coverage is stable in the game.

Minimum useful regression additions during repair: (1) native-like independent real-frame copy plus generated delivery, (2) changing full/HUD-free equality with stable scene, (3) suspend/release/recreate with queued presentations, (4) duplicate first inner-frame call, (5) delayed GPU completion during preset replacement. Tests should demonstrate these failures, not merely assert successful NGX return codes.

## Focused repair order

1. Preserve native FG ownership; establish enhanced real-frame delivery and stable UI composition together.
2. Repair per-stream frame identity/routing and GPU lifetime defects; verify suspend/rebuild behavior.
3. Finish existing engine-resolution control and connect native FG controls to the correct owner. Model presets remain separate from resolution.
4. Validate actual rendered/generated images, then address remaining HDR quality and NR cost.

No new universal discovery project, arbitrary disabling checks, or additional external review cycle is proposed. Automated repairs need no user attendance; final live quality verification remains the user's test. A defensible repair-duration estimate depends on identifying the actual native real-frame copy boundary; this audit does not pretend that an unresolved integration is a one-line fix.

## Primary references

- NVIDIA Streamline SR guide: https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS.md (application applies viewport size, options and preset distinction).
- NVIDIA Streamline FG guide: https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md (frame constants, tagged-resource lifetime, HUD-free/UI composition and suspension).
- NVIDIA sample NR documentation: https://github.com/nvpro-samples/vk_gltf_renderer/blob/main/docs/denoising.md and source https://github.com/nvpro-samples/vk_gltf_renderer/blob/main/src/dlss.cpp (post-tonemap display-resolution NR, guide scaling).
- Installed NVIDIA SDK `nvsdk_ngx_defs_dlssg.h` and `nvsdk_ngx_params_dlssg.h` (OutputReal optional, UI equation, motion normalization, inner-frame indices). SDK sources not committed.

The attempted public `Streamline/source/plugins/sl.dlss_g/dlss_g.cpp` URL returned 404; it is not evidence of scheduler internals. No claim to have inspected unavailable native FG implementation code.
