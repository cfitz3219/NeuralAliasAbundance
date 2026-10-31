# PBZ: NR before native Frame Generation

## Implemented path

The native NGX Frame Generation evaluate hook now calls NR **before** forwarding
the game's FG evaluation, on the same open command list. The NR feature and its
parameters belong to that NGX core. The full picture and HUD-free picture are
updated together; the original FG invocation, its output targets, and the game's
native DLSS files are preserved. The Present-time NR pass is suppressed while
this native FG context owns the route. There is no new cross-queue wait.

`DLSSG.BackbufferFrameID`, including zero, identifies each real picture. The
bounded cache runs NR once and supplies its cached pair on subsequent MFG calls.
`MultiFrameIndex` and SR/RR CPU call counts are not substituted for identity.
Missing identity is reported explicitly. GPU ordering follows the existing
native FG feature's temporal evaluation order; this is not permission to execute
calls on that same feature concurrently on unrelated queues.

Each recording owns its descriptors and resource references. Actual
ExecuteCommandLists forwarding publishes completion fences; Reset/destruction
invalidates recording tokens. A cached slot cannot be recycled while any
recorded or submitted consumer still holds it. Discarded, unsubmitted work can
retry its identity. Failed evaluations retain any partially recorded work until
retirement, then use a new feature. Retired packets release references to game
images while keeping their reusable private output allocations. Feature size,
format, device and model changes retain old allocations through their consumers.
NGX shutdown orphans its own objects so later packet destruction cannot call a
shut-down core. Internal NGX submissions bypass the outer NR recording mutex.

## Image and guide conventions

The route consumes independent color, HUD-free, depth and motion subrects.
Guide regions can remain at render resolution. FG motion scales normalize
motion; multiplying those scales by display width/height supplies NR's
display-pixel convention. Explicit camera-excluded or jittered vectors and
distortion fields are not silently interpreted as qualified guides.

NR output is RGBA32F, following NVIDIA's reference renderer. SDR is composed
directly. HDR10 PQ and scRGB use the existing SDR working-domain transform and
apply the NR change as a bounded ratio on the original HDR light values. The
result is converted back to the original encoding before composition. This HDR
adaptation is our integration policy, not a claim of NVIDIA-certified HDR NR.

The initial connected route uses the paired equality mask: where the original
full and HUD-free RGB agree, both receive the enhanced image; elsewhere both
retain their originals. Original alpha and allocation padding are preserved.
This deliberately avoids guessing a PQ UI blend domain. The independently
tested alpha compositor remains available but is not selected automatically.

Required native inputs are plain, matching color formats, DIRECT-list FG
evaluations, and supplied frame identity, HDR mode and motion scales. Missing
or unsupported inputs produce a reason, never a persisted NR setting change.
Native FG release ends ownership of this route. No universal game compatibility
or live PBZ picture-quality result is implied by the fixture results below.

## Automated evidence

Fresh full Release build and 20 focused tests pass (2026-10-29). The connected
DX12 test uses a deterministic compute stand-in for the NR model and real RTX
execution/debug-layer validation. It checks full/HUD-free copy-back before a
simulated FG consumer, MFG deduplication, identity zero, poisoned subrect padding,
render-resolution guide scales, discarded recordings, cache recycling beyond
capacity, size changes, off/on, failed-evaluation retry, PQ/scRGB identity
round-trips, core shutdown/recreation, internal-submit reentry, and feature
retention behind a GPU gate. No D3D12 debug errors are accepted.

Separate tests cover the previous queue dependency freeze, repeated input
recordings, runtime ownership/shutdown, partial initialization retry, retired
device migration, settings provenance, and Home draw-slot backlog on WARP/RTX.
The new route reports completed GPU timestamps through the existing menu cost
display. These tests do not execute NVIDIA's model or prove PBZ's visual quality.

## User test and receipts

With PBZ closed, use the prepared `Documents/NA-pbz-update.cmd`. Native Ultra
Performance remains selected through the game's settings. NR controls remain
in Home. The package is additive and retains the existing install/rollback
mechanism, repairing from the actually installed 68fc32a package.

The next game run must show `nr_route=pre_native_fg`, real-frame-ID availability,
and `nr_pre_fg evaluated/reused/submissions` increasing. The Home status should
say NR is active before native Frame Generation and show its GPU cost. The
user tests picture/Home flicker, off/on and graphics-quality changes. If a
required input is absent the logged reason identifies it; automated results
must not be presented as that live confirmation.

## Primary references and provenance

- NVIDIA Streamline DLSS-G Programming Guide, version 2.14.16:
  https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md
- NVIDIA vk_gltf_renderer NR implementation (evaluateNr/updateSizeNr):
  https://github.com/nvpro-samples/vk_gltf_renderer/blob/main/src/dlss.cpp
- Its denoising documentation:
  https://github.com/nvpro-samples/vk_gltf_renderer/blob/main/docs/denoising.md
- Locally pinned NVIDIA NGX `nvsdk_ngx_defs_dlssg.h` for exact FG parameter names.

The old comments' NR-INSERTION-DESIGN.md and NR-TLOU-I-REPLAY.md were not found
in fetched repository history; they are not evidence for this implementation.

## Final peer-review corrections (2026-10-29)

Opus reviewed 2ec66f0/c38f7a1 and accepted the connected placement, PQ ordering,
real-frame deduplication, Present suppression, timing, shutdown ownership and
additive installer. His remaining items have these dispositions:

1. Unclean-session startup is implemented. A per-profile session marker survives
   force termination, nonzero exit and watchdog freezes. Clean ExitProcess(0)
   removes it unless the watchdog already recorded failure. Next launch starts
   rendering overrides off, leaves Home available and preserves saved choices.
   Explicit re-enablement saves normally. Driver FG overrides are restored using
   the existing per-game backup; restoration failures are visible in Home.
   This only detects sessions that ran marker-aware builds; it cannot reconstruct
   a crash before the marker existed. The controlled child-process test verifies
   actual hooked clean exit, forced termination and watchdog-marked exit.
2. Submission no longer acquires the native context mutex. Only lists carrying
   an NR recording token acquire their own recording lock. Shared recording
   leases retain resources across forwarding. Ordinary game/NGX helper submits
   do not wait on model creation. A helper-thread submit during fake model
   creation is the regression, not a same-thread TLS-only test. Timestamp reads
   wait for recording retirement as well as GPU completion, preventing reuse
   races while a list can still be resubmitted.
3. Input states come from observed legacy barriers on the actual command list.
   Color, HUDless, depth and motion are normalized for NR and restored to their
   exact observed states. Split begins, alias changes, Reset and enhanced
   barriers invalidate old evidence. Unobserved/carried states and enhanced-only
   inputs wait with a visible reason; no arbitrary state is substituted. The RTX
   fixture now uses combined shader-read states and actual barrier detours,
   without manually feeding observations. The debug layer reports no errors.
4. Status now records real-frame identity, state evidence, and whether native FG
   Backbuffer/OutputReal are tagged DXGI swapchain resources. These markers retain
   no swapchain references. This is diagnostic evidence, not a claim that pointer
   inequality proves missing real frames: Streamline can copy internal resources.
   The pinned NGX header explicitly describes OutputReal as optional. No guessed
   Present copy of the latest NR frame was added. Actual PBZ real/generated-frame
   continuity remains part of Cort's live test; if neither resource is a swapchain
   image, copy lineage or a capture must establish how the real frame is delivered.

Validation: Release build; 22 focused CPU/DX12/RTX/WARP tests passed (8.65 seconds),
including settings preservation, real process-exit hook, helper-thread submission,
paired output/subrect/HDR/cache tests and Home backlog. Final no-manual-observation
native hook regression also passed (0.47 seconds). Additive installer/rollback
fixture passed. These use a fake NR model and do not prove NVIDIA model quality
or PBZ visual continuity. No game launch, deployment or native DLL change.

## Live-result correction: optional identity and compute FG (2026-10-29)

This supersedes the earlier mandatory BackbufferFrameID / DIRECT-list contract.
Cort reported MissingIdentity in focus, apparent engagement after losing focus,
and worse rapid flicker. Copied live status/observe/startup logs to
local/pbz-native-identity-20261029. Installed283e1a0 hash was verified. Across43399
FG evaluations frame_id_calls remained0; preFG NR evaluated0. Direct and compute
FG lists appeared, with route switching to Present and old NR evals8764. Thus
this was not the intended preFG path visually failing: it never evaluated NR.

Two implementation defects corrected: optional native identity incorrectly made
mandatory, and COMPUTE lists excluded from route ownership/evaluation. The native
route now registers on feature creation and handles both DIRECT and COMPUTE.
NVIDIA's nvsdk_ngx_defs_dlssg.h marks BackbufferFrameID optional; its
nvsdk_ngx_params_dlssg.h defaults multiFrameCount and multiFrameIndex to1/1.
Without an explicit ID, legacy calls each describe a real-frame pair; explicit
MFG index1 starts a pair, subsequent inner indices reuse that pair's result.
Malformed or incomplete inner sequences are not treated as a new frame. No SR
call count, window focus or global generated-frame Present count is used. An ID
source change resets the cache/history while outstanding recordings retain old
resources. NR is not started halfway through an unenhanced MFG group.

Primary source: https://github.com/NVIDIA/DLSS/blob/main/include/nvsdk_ngx_params_dlssg.h
and corresponding defs header. The legacy fallback cannot distinguish an
application retry that supplies no ID and resubmits the same picture as a new
FG invocation; explicit native IDs remain preferred. This is not a global
render-frame identity or permission to overlap calls on one FG feature.

Release and22focused tests pass9.30s. New RTX/debug-layer coverage: missing ID
with3inner frames on direct and compute queues, legacy missing count/index on
compute, route ownership before first evaluate, native ID0, malformed sequences.
NVIDIA model/actual PBZ display continuity still require Cort's test. No game
launch/control/deployment. Opus owns resolution/model selection; Astra changed
only the native route registration line in ngx_observe's FG creation branch.

## Opus PR60 integration review (2026-10-29)
Integrated012beda/aa88e76. Accepted actual-display target computation, preset-only
Apply for PBZ's engine-owned size, preset logging and read-only driver override
inspection. Corrected findings: only relabel quality when actual creation input
matches the selected target; don't treat a display-size query as consumption;
require driver override enable plus selection; use current global profile API
and effective game-profile inheritance, not base-profile substitution; don't
claim a configured override proves the active model or identical appearance;
preserve existing engine-adapter controls. Added missing preset logging for the
in-place Apply path, which bypasses CreateFeatureImpl.

Accepted Opus's compute-state recommendation: validate observed states against
compute-legal bits before recording any input transition. COMMON/PRESENT is legal;
RT/depth/pixel-only states cannot be transitioned by this compute list.

Legacy duplicate NR concern is real, but his global-Present deduplication rule
is not adopted: multiple frames can be queued and generated Presents can
interleave. A burst of calls alone does not prove duplicated picture content.
No claim that this retry ambiguity is solved; image-write or explicit frame-tag
provenance is needed to distinguish it. No guessed cache reuse was introduced.

Full Release build +24focused tests pass9.61s; after final logging/adapter changes,
6affected tests pass1.57s. No live game test, deployment or driver-profile writes.

## 2026-10-29 - Argon Route A implementation disposition

Integrated Opus PR62 f88bce6, corrected before release. Public D3D12 GetCapabilityParameters, GetParameters and AllocateParameters now wrap existing SR/RR optimal callbacks. Empty allocations stay empty. Failed acquisition leaves returned parameters untouched. Permanent per-original thunks preserve forwarding across multiple runtime callbacks and repeated acquisition (32 distinct targets; exhaustion preserves the runtime callback). Path counters appear in dlss_control.

Removed the proposed private PopulateParameters_Impl hook: its declaration is absent from the pinned public SDK, and our core-module filter excludes feature snippets where this export is found. Allocation followed by later population is therefore NOT claimed covered. No guessed vtable[11] hook or incorrect Streamline struct prefix. PBZ's additive package does not replace sl.interposer; a proxy-only optimal-settings wrapper would not cover it. Actual display-size queries and consumed creation sizes still determine mode availability. No query is not evidence that PBZ calculates sizes independently.

Retained already-implemented overlay draw-slot fencing, completion admission without cross-queue waits, and pre-native-FG NR. Rejected reusing real-frame depth/MV on generated images: stale guides do not match those images. Two buffers alone do not prove GPU lifetime safety. Retained distinct SR and RR preset domains. Legacy same-picture FG retry identity ambiguity remains open; this change does not claim to resolve it or live flicker.

Validation: full Windows Release build; 13 focused CPU/DX12 tests PASS (5.95s), final 3 NGX tests PASS (0.88s) after adding cross-original SR forwarding witness; additive installer/rollback fixture PASS. Actual hooked-export test covers SR/RR, all three public acquisition paths, empty parameters, failure, idempotence and retained callbacks after parameter destruction. Existing NR direct/compute GPU and overlay tests use controlled fixtures; no PBZ quality proof.

Source authority: pinned NVIDIA nvsdk_ngx.h public parameter acquisition declarations, nvsdk_ngx_params.h typed virtual Get/Set, nvsdk_ngx_defs_dlssd.h RR preset domain. Argon's supplied universal_dlss_route_a_architecture_fixes.md is reviewed design input, not an authoritative ABI definition.

## 2026-10-29 - Correct NR input-state fallback using NGX read contract

Cort assigned Astra the PR65 correction. Did not merge the same-state inference. NVIDIA's actual DLSS-FG Programming Guide310.7.0 section6 (PDF page18; individual input contracts pages97-99) requires all FG inputs to be compute-readable at EvaluateFeature. Source: https://raw.githubusercontent.com/NVIDIA/DLSS/main/doc/DLSS-FG%20Programming%20Guide.pdf . This establishes permission to read; it does not establish identical exact state bits across textures.

Implementation: keep observing/restoring exact known states. NoTransition on nonswapchain HUDLess/depth/motion can use that native NGX read contract, without transitions on those unknown-state resources. Full-color input still needs observed state because we copy enhanced real-picture pixels back. Split, aliasing, enhanced-layout invalidation and uncovered-list evidence still refuse. Unknown exact state remains0xffffffff in logs; states_ready and read_contract_mask distinguish usable inputs from known exactstates.

When HUDLess is unobserved, compose enhanced HUD-free pixels into an owned texture with the same allocation size/format/subrect coordinates as original. Only the FG call's HUDLess parameter is temporarily rebound. RAII restores the original pointer when original EvaluateFeature returns. Original HUD texture is never written/transitioned. Packet/recording ownership keeps replacement alive through GPU completion and repeated MFG calls, including context release under backlog. No game DLL replacements, new GPU waits, or rendering-state guesses. This is a rendering correction, not just diagnostic logging.

Evidence: new RTX5070Ti test reproduces differing real states (color/HUD0xc0, guides0x40) while only color is observed. NR evaluates, FG reads enhanced owned HUD image, original HUD remains byte-correct, parameters restored, no debug-layer errors. Compute/no-ID MFG and HDR PQ/scRGB round-trip with subrect/extent changes also pass. FullRelease+21focused tests PASS8.38s. First build hit test-variable shadow warning and was corrected. Initial HDR extension incorrectly expected a linear SDR additive delta through HDR conversion (four pixel assertions failed, no barrier errors); use zero-delta HDR round-trip oracle as existing HDR tests do, retain nonzero output witnesses in SDR cases. Tests use fake model, not actual PBZ quality. Existing legacy duplicate-FG retry ambiguity remains separate.

### 2026-10-29 — Astra: diagnostic 6a37d67 capture received; freeze still unresolved

Cort reported force-quitting about five seconds after the visible freeze. The new watchdog dump actually completed at 19:19:59, with a 10,015 ms no-present interval; the 4,096-event trace contains all 3,336 events and zero drops. No repeat capture is needed merely because of the reported five seconds. Evidence is preserved at root local/pbz-nr-queue-freeze-20261029, including freeze-20261029-191959.dmp and debugger-unwound stacks.txt.

Measured: NR evaluated six real frames, reused five inner frames, submitted eleven recordings and retired seven. All NR and original FG CPU calls returned. FG worker and pacer threads are idle in their work-condition waits, not stuck inside our NR context lock. Two NR receipts are incomplete. FG queue 0x455aef430 waits for fence 0x1d8cabf30 value 21942, observed completed 21941 (event 3226); this wait is encountered during a copy/presentation submission after an NR evaluation submission had completed. Later FG work and the game's render queue depend on progress behind this wait. No Signal of this fence is in the captured queue trace. Its owner and CPU/driver signal path are not yet identified. Calling it definitively a backbuffer-ready fence was an inference, not proven ownership.

Actual NVIDIA runtime isolation extended: HDR R10G10B10A2, 3440x1440, 32 real frames, 64 FG evaluations, eight-list backlog, absent BackbufferFrameID, and 96 real DXGI flip-discard presentations now complete with clean NGX shutdown. This separate test window does not load PBZ or reproduce Streamline's asynchronous scheduler. It narrows the issue; it does not prove a game fix. Test evidence probe-hdr.log and probe-present.log.

Considered owned full-color replacement plus downstream-copy routing, saved experiment locally, then reverted it without packaging: capture does not establish that input copy-back causes this stall, nor that PBZ uses the proposed exact copy route. No speculative rendering change shipped. Added four raw caller addresses to the existing bounded queue Wait/Signal records, with module resolution only on the background watchdog. Needed to attribute the unsatisfied wait to our code, DXGI/driver or native FG scheduling; a longer old-format capture cannot recover this missing information. Production rendering behavior remains unchanged. Full Release build passed; 18 selected automated tests passed and opt-in runtime probe skipped in CTest (separately executed successfully with real NVIDIA libraries). No game control, deployment or native DLL replacement. Freeze is NOT fixed; next live evidence requires the attributed trace, not an unsupported claim of repair.

### 2026-10-29 - Astra: native FG release explains route selection; two confirmed defects corrected
After Cort closed PBZ, observe.log became readable. Native FG handle0x159F4A4920 was evaluated successfully on thread49196, then successfully released on game/render thread31276 atQPC1081277669640; another native FG incarnation had been created atQPC1076283388519. Earlier19:46 route switch likewise follows a native FG release, not a user NR toggle or an expiry timer. The logged release API lacks the upstream call stack/options: it does not establish WHY the engine requested that release. Cort explicitly did not change game settings. Do not blame him or assert focus as fact.

Confirmed status defect: native GetStatus returned zeroed inactive/evaluation counters when Before held its mutex. Captured19:52:28 and19:53:13 rows show active0/evals0 Updating Neural Rendering between active rows. Fixed by retaining the last complete snapshot during contention, without waiting on the processing mutex. Deterministic RTX test polls status from another thread while the fake model is inside Evaluate; it must retain active/nonzero counters. This corrects a false UI indication, not proof of an image correction.

Confirmed queue-tracking defect: swapchain map recorded only creation queues. ResizeBuffers1 permits a new queue per back buffer and rotates presentation through them; our detour forwarded the change but left NR/menu using the obsolete map. Added small owned PresentQueues binding, successful-resize update, current-buffer selection and retained queue lifetime across callback. Failed resize preserves binding; creation on reused address resets it. Unit cases check failed update and alternating queues. Real overlay test changes the same swapchain to a second direct queue via ResizeBuffers1 and verifies the selected presentation queue while drawing. WARP andRTX bothPASS. Source: https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiswapchain3-resizebuffers1 . Old live trace does not record ResizeBuffers1 arguments, so this is a confirmed implementation flaw, not a proven sole cause of the reported PBZ flicker.

Full Release buildPASS;18 focused testsPASS/1opt-in runtime probeSKIP,11.36s. No game launch/deployment. Earlier native-FG phase had zero recorded NR skips despite reported visible flicker; real/generated pixel delivery and equality-mask coverage still lack image evidence. Later Present phase has incomplete-input skips without allocator/missing counters, consistent with cross-queue completion admission; do not fix this by adding an unproven GPU Wait or displaying stale output. Existing freeze remains unproven fixed. Updated candidate contains real queue/status fixes but is not qualified as resolving all visible flicker.


### 2026-10-30 — Game-facing Streamline NR consumer implemented

Astra replaced the missing common-source integration with `nr/sl_consumer.cpp`.
It preserves volatile tagged HUD-free/depth/motion/UI inputs at their tag command-list point and defers immutable tags until Present, as required by NVIDIA's tag-state contract. Accepted results are joined by exact frame token and viewport. The game-facing proxy's real image is enhanced once before original Present. The enhanced HUD-free image is re-tagged through original `slSetTagForFrame` with eOnlyValidNow on the same owned command list; SL records its preservation copy before that list is submitted. Native FG then receives this copy while real presentation receives the enhanced game image. There is no NR evaluation of interpolated images, no stale previous-frame output replay, and no injected GPU Wait.

The submission queue is the GAME's creation/ResizeBuffers1 queue, captured above SL, not SL's private physical-presentation queue. Factory RTTI and export discovery now run even when swap-chain RTTI was found. Queues are canonicalized through SL's base-interface query and retained per buffer. The public NVIDIA guide section18 explicitly describes the separate asynchronous presenting queue and offscreen game buffers. Unknown bindings remain explicitly unavailable rather than submitting NR on a guessed queue.

The adapter uses explicit states, actual backbuffer and HUD subrects, per-proxy serial/resize epoch, constants for that exact token/viewport, and existing HDR normalization/restoration. Owned snapshots require actual producer submission and same-queue order or completed cross-queue work. Native/base-Present fallback is suppressed for a claimed SL device across MFG, temporary FG suspension and preference toggles. Failed Present resets subsequent history; retry does not re-enhance; proxy destruction releases stream ownership.

UI alpha/UIColorAlpha uses the existing opacity composition. Without opacity, both streams use the enhanced scene and the real image preserves the original additive residual B-H. This removes the per-pixel equality gate that could alternate enhancement with quantization/dither. It is an explicit approximation for opaque/translucent UI when opacity is unavailable: exact UI separation cannot be recovered uniquely from B and H alone. Game HUD quality still needs the user's visual test; no claim of mathematically exact missing-alpha composition.

Automated evidence: expanded RTX fixture runs24real frames through the production consumer, original SL retag trampoline, real DXGI buffers and a deterministic NR shader. It overwrites volatile originals after tagging, verifies both real output and FG's preserved input, checks no duplicate NR on retries/lower FG calls, toggles NR, resizes the actual swapchain, uses backbuffer subrects and UI alpha, and exercises immutable state declarations valid at Present. This caught a completed receipt pinning old backbuffers: leases now retain only completion evidence after retirement, not the old recording payload. Producer leases separately test delayed submissions, same/different queues and discarded lists. Boundary tests verify original-only re-tagging and exact game-factory/per-buffer queue binding. These are plumbing/pixel-witness tests, not a live NVIDIA NR perceptual result or a PBZ gameplay test.

Primary references consulted:
- https://raw.githubusercontent.com/NVIDIA-RTX/Streamline/main/docs/ProgrammingGuideDLSS_G.md (sections5.2,15.1,18)
- https://raw.githubusercontent.com/NVIDIA-RTX/Streamline/main/source/core/sl.api/sl.cpp (`slGetNewFrameToken` explicit-index contract)
- https://raw.githubusercontent.com/NVIDIA-RTX/Streamline/main/source/core/sl.interposer/dxgi/dxgiFactory.cpp
- https://raw.githubusercontent.com/NVIDIA-RTX/Streamline/main/source/core/sl.interposer/dxgi/dxgiSwapchain.cpp
- https://raw.githubusercontent.com/nvpro-samples/vk_gltf_renderer/master/docs/denoising.md (post-tonemap NR, normalized display-domain input)

No game launched or deployed. Earlier measured freezes/flicker remain historical evidence, not retrospectively relabeled as proven fixed by fixture success. The integration is ready for user-owned gameplay validation after the final build/package receipts below.
