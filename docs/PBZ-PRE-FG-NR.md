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
