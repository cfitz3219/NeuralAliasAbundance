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

## Follow-up: native-FG event chain and deeper failures

Cort requested continuing beyond the first findings. This appendix preserves them and adds a lifecycle audit against the same production source (`5d6deaf`; documentation HEAD `bf144b0`). Three additional failures were reproduced in standalone harnesses using production functions. These are implementation defects, not three independently proven explanations of the PBZ capture. No runtime repair or game operation occurred.

### What actually happens, in order

| Stage | Current behavior | Consequence to examine |
|---|---|---|
| 1. Initialization | The additive DXGI package hooks native DXGI vtables; NGX FG creation registers an NR context. Streamline owns its game-facing proxy and FG presentation scheduler. | Our low-level Present callback is not necessarily the application's one-real-frame boundary. A global FG-context count is not a per-swapchain scheduling state. |
| 2. Game rendering and tagging | The engine renders/upscales, produces HUD-free/full color and guides, and tags resources for Streamline. A short-lived tag can be copied when tagged. | The image later passed to NGX may already be a private copy. Original tag state is valid at its declared command-buffer point, not indefinitely. |
| 3. Game calls Streamline Present | Streamline's public proxy implementation invokes plugin before-hooks before forwarding to its underlying swapchain, unless a plugin takes over that call. | Native FG may schedule/copy images before our underlying-DXGI Present hook. That hook cannot alone identify a fresh real frame. |
| 4. Native FG calls NGX | `EvaluateFeatureImpl` runs `native::Before`; NR reconstructs from the HUD-free input, composes the pair, copies into FG color, sometimes substitutes an owned HUD-free image, then calls the original FG implementation. | Mutation is inside native FG processing. Neither the local state tracker nor a successful NR return proves exclusive write access or real-frame delivery. |
| 5. More generated frames | Later inner calls reuse the cached NR packet. The NR preference is checked again before cache/identity resolution. | A setting change can split a single generated-frame group; missing/repeated IDs remain a separate earlier finding. |
| 6. GPU execution | Our recording wrapper forwards command lists and signals a completion receipt. Native FG continues its own generation, copies and pacing. | NR completion, FG completion and actual presentation are different events. Counters cannot substitute for an image-delivery witness. |
| 7. Native presentation and Home | The underlying Present hook skips Present-NR while any native context exists, then may draw Home on the selected queue. | Home has its own queue/lifetime defects. An unavailable draw slot intentionally skips a draw; this can hide Home for that output without changing NR's preference. |
| 8. Suspension/rebuild | FG release immediately removes NR route ownership. Present-NR can become eligible; resize runs `Maintain`, then forwards. | Old scheduled images and closed recordings need their own retirement. Context removal is not proof that those images/resources have finished. |

The intended composition is one enhanced real-frame source feeding both real presentation and native FG, while native FG retains scheduling ownership. This does not mean adding another generator or suppressing frames until an arbitrary confidence score is met.

### 11. Home completion fence mixes independent queue timelines — reproduced

`overlay/overlay.cpp:1903-1994`: all eight allocator/ImGui slots use one `g_state.fence`. Draws signal increasing values on whichever presentation queue was selected. A signal on queue B can overtake an older draw blocked on queue A. Comparing the shared fence with A's slot value then falsely permits resetting A's allocator and replacing its descriptors/vertex data. The same fence also protects atlas updates and old-chain retirement.

Standalone WARP reproduction through production `DrawMenuForTests`/`render_locked`: block A, draw once on A, draw seven times on B, drain B using an independent fence, and request the ninth draw on B. Production records it into A's still-busy ring slot. Witness: `gate=1`, `draws before=21 after=22 initial=13`; assertion that the draw must defer fails. A is then explicitly released and both queues drained. This establishes unsafe admission, not a reproduced device removal. The ordinary prior queue-switch test drained one queue before switching, so it missed this interleaving.

Microsoft explicitly recommends one progress timeline per fence; increasing numbers alone do not order independent queues. Repair requires queue-scoped completion ownership (or an established shared ordering), including atlas/resource retirement. Increasing the ring size only delays the bug. The older PBZ trace does not prove this exact interleaving occurred.

### 12. Completed NR recordings retain a backbuffer across resize — reproduced

`nr/native_fg.cpp:137-225,312-317` retains packets and their game-resource references until both GPU completion and command-list Reset/destruction. Releasing the native FG context does not retire a still-valid recording. `overlay.cpp:2313-2345` calls `Maintain` before resize, but it cannot clear that recording.

RTX 5070 Ti standalone test used a real DXGI flip-discard swapchain and production NR with the deterministic test model. The GPU was completely idle, the native context was released, and all test-owned backbuffer references were dropped. The closed command list remained alive. Results:

| Same sequence | Resize before list Reset | Resize after list Reset and Maintain |
|---|---|---|
| NR off | `S_OK` | `S_OK` |
| NR on | `0x887A0001` (`DXGI_ERROR_INVALID_CALL`) | `S_OK` |

The control establishes that NR adds the blocking references. This is a concrete graphics-rebuild failure mechanism; it does not establish that PBZ's recorded lockup came from this call. Nor should repair simply release resources at first completion: a still-executable list may be resubmitted. The stream/rebuild lifecycle must explicitly retire affected recordings once they can no longer execute, retaining already-submitted work through its receipt. A retry loop or another arbitrary wait does not resolve the ownership problem.

### 13. One MFG group can receive inconsistent NR inputs — reproduced

`native_fg.cpp:354` reads `nr_enabled` before resolving the real-frame identity or reusing its packet. With the unknown-state HUD-free substitution path, the first call receives enhanced full color and our enhanced HUD-free replacement. `InputOverride` restores the game's original HUD-free pointer after each call. Turning NR off before the next inner call returns before reapplying the cached packet: full color is already modified, but later FG calls receive the original HUD-free image.

RTX fake-model harness kept one native frame ID and three inner calls, toggling only the NR setting after the first. Witness: `owned_hudless=1,0,0`; the last HUD-free readback also fails the enhanced-image expectation. No native NVIDIA FG model was evaluated in this harness. This proves an inconsistent input group, not its exact visible artifact. It requires a transition and cannot by itself explain Cort's continuous flicker with untouched settings.

Repair: latch a frame's enhancement decision through all its consumers, apply setting changes at the next real-frame boundary, and coordinate appearance/history transitions. This also applies to failure/recovery policy, without pretending all failures can safely reuse old imagery.

### 14. Readable FG inputs do not establish exclusive permission to overwrite them — contract gap

`InputStates` uses local transition observations and the NGX read contract; `CopyRegion` then writes full color and, when known, HUD-free color. A native pipeline may legally have several concurrent readers of a read-shared resource. A local barrier is not a fence joining those readers. Adding writes changes that contract. Even narrowing a combined read state must be justified at the point of use.

There is no demonstrated exclusive producer/consumer handoff for these writes in the current hook. This is an ordering gap, not proof that PBZ actually has a competing reader. It reinforces moving enhancement before native copying/scheduling, with owned snapshots where required. Do not respond by adding blind cross-queue waits: the captured dependency chain already demonstrates why a wait before the original Present can block the very work needed to satisfy it.

### 15. Temporal transitions and original-FG outcomes are not coordinated — code-confirmed gaps

NR resets its own history on settings/frame discontinuity (`native_fg.cpp:447-469`) but never propagates an NR appearance transition to native FG's history. FG's previous original image and a new enhanced image may straddle a transition. This requires a deliberate policy; it does not justify resetting FG on every tiny slider change or every frame. No live ghosting/reset artifact is claimed from source inspection alone.

`EvaluateFeatureImpl:559-575` marks/caches NR work before calling native FG, then only logs/returns FG's result. There is no completion/failure notification back into NR's frame transaction. `ReleaseFeatureImpl:795-827` removes the native route and handle classification before learning whether the original release succeeded. In a failure/retry case this can leave routing/cache/history inconsistent with the actual feature. The captured releases used for the earlier route-switch finding succeeded; this failed-release case is additional, not a reinterpretation of that evidence. A failed CPU call also does not authorize freeing resources for commands already recorded/submitted.

### 16. Route switching does not drain or transfer the stream — earlier finding expanded

`OwnsRoute` is only `contexts_count != 0`; `Release` erases that context even when `Recording` still holds queued NR work. `nr_pass.cpp:835-836` then permits the independent Present pipeline. The two NR engines have separate histories and different input association/composition. There is no per-stream epoch, completion handoff, or knowledge that a pending native presentation is generated versus real. Conversely, a retained but inactive FG context can suppress Present NR indefinitely. These are code-confirmed transition gaps; actual double-processing of a particular PBZ image has not been demonstrated.

NVIDIA's guide makes runtime FG suspension during menus/reconfiguration normal. That supports treating it as a lifecycle event, not as evidence that Cort changed a setting. The exact upstream reason for any individual captured release remains unknown.

### Opus's new proposal, checked against primary code

Read main COMMS `f0eb51e`. Agree with moving to the game-facing Streamline boundary and keeping native scheduling. Source `DXGISwapChain::Present/Present1` confirms before-hook ordering; `GetBuffer` and `GetCurrentBackBufferIndex` also have plugin hooks. This is useful existing infrastructure to work with, not a reason to build another renderer. Corrections:

- `output_real=0` and a non-swapchain FG input do **not** prove that the real frame bypasses that resource. The native scheduler could copy from it. Alternating enhanced generated/original real images remains a strong hypothesis requiring pixel provenance.
- Hooking only `slUpgradeInterface` is incomplete: NVIDIA's `DXGIFactory::CreateSwapChain*` can create a swapchain proxy directly via `setupSwapchainProxy`. Discover the returned game-facing interface on either creation path, including replacement chains.
- Game-thread execution alone does not solve GPU ordering. Tag state belongs to a specific command-buffer point; short-lived tags may already have been copied, reused or destroyed by Present. Preserve frame token/viewport, declared lifetime, subrect and actual producer order. A COM reference preserves an object, not its old pixel contents.
- One proxy Present call is not an unconditional new rendered frame: handle test/retry calls and associate with the application's frame token. It is much better than counting native generated Presents, but identity still needs explicit association.
- The overlay fence and resize-reference defects above are separate from hook placement. They must be repaired as ownership issues even with the improved boundary.

For eventual user testing, Opus's small marker/readback proposal could distinguish real/generated delivery. It must be explicitly diagnostic, applied once to the matched frame pair, read asynchronously, and associated with presentation identity; dropped capture frames must not be counted as missing NR. No marker, capture hook, or game modification was installed during this audit.

### Checks that did not support another claimed cause

The PBZ evidence has `[present] proxy=0`. Our optional presenter is not shown stealing native FG's frame-latency waitable signals: its query is explicitly non-waiting, and the overlay's waitable hook forwards the handle. Native Reflex Sleep/marker observers forward the original call once; no extra per-Present Reflex Sleep was found. These mechanisms should not be blamed merely because NVIDIA documents pacing hazards. Successful-reset tracking invokes invalidation after successful Reset, not after a failed Reset. Also, NGX Create forwarding uses the per-core original; the old global alias is not evidence of a new create-routing error.

### Reproduction receipts and limits

Local evidence: `local/route-a-audit-20261030/overlay-multiqueue.patch`, `overlay-multiqueue.log`, `nr-lifecycle-audit.patch`, `nr-resize-retention.log`, `nr-mfg-toggle.log` (workspace root, not the worktree). These are deliberate failure reproductions against unchanged production functions. They are not green quality tests. Tests use WARP/RTX with a deterministic stand-in for NR where noted. No debug-layer error/device-removal claim is made for the optional reproductions.

Temporary test source was restored byte-for-byte and both modified executables rebuilt. Restored baseline `host_overlay_test`, `host_overlay_hardware_test`, `host_nr_native_fg_test`: **3/3 PASS, 4.00 seconds**. An initially mistyped CTest name matched no tests; it is not counted. The temporary harness initially used deprecated `getenv` under warnings-as-errors; corrected to the Windows API. Its HUD-free readback state was corrected for the original-resource case before the final MFG receipt. No production source, installer or deployed payload changed.

Repair priority is now concrete: preserve one enhanced real-frame source through native scheduling; correct queue-specific overlay completion and rebuild retirement; make per-real-frame decisions and lifecycle transitions coherent; then qualify both output streams and their history. Engineering harness work needs no user attendance. Live quality testing remains Cort's. The previously captured unsatisfied native fence still has no proven owner/causal repair.

Additional primary sources checked:

- Microsoft multi-engine synchronization (independent timelines and fence races): https://learn.microsoft.com/en-us/windows/win32/direct3d12/user-mode-heap-synchronization
- Microsoft ResizeBuffers (outstanding backbuffer references): https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-resizebuffers
- NVIDIA public swapchain proxy (before hooks, buffer/index hooks): https://github.com/NVIDIA-RTX/Streamline/blob/main/source/core/sl.interposer/dxgi/dxgiSwapchain.cpp
- NVIDIA factory proxy (automatic proxy creation): https://github.com/NVIDIA-RTX/Streamline/blob/main/source/core/sl.interposer/dxgi/dxgiFactory.cpp
- NVIDIA Streamline general guide (tag state/lifetime and copies): https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuide.md

The public proxy is not the closed FG scheduler; its source establishes interface ordering, not PBZ's internal copy topology. Initial URLs omitting the `dxgi/` subdirectory returned 404; the corrected sources above were read.
