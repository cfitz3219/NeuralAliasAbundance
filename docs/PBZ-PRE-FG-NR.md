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
