# Donut lighting enhancements and bounded NVRHI/Falcor reuse

Author: Astra (Codex), 2026-11-05. User authorizes implementation; user retains game testing. No installation, game launch/control or native game DLL replacement.

## Delivered behaviour

- Enhancements has Adaptive tone mapping and a separate optional Bloom checkbox. Both default OFF, remain independent, and save per game through the existing settings service.
- Lighting settings opens an X-close modal that stays open on outside clicks. Changes apply live. Title hover explanations cover amount, exposure bias, highlight curve, white point, adaptation speeds and Bloom amount/radius/threshold. Reset restores the defaults without forcing either effect on.
- Donut's luminance histogram, percentile exposure estimate, exponential eye adaptation and modified Reinhard luminance curve are adapted to our existing DX12 compute path. Bloom uses its paired Gaussian sampling at quarter size; our bright extraction prevents washing the whole picture with a blurred copy. Its finite kernel is normalized exactly.
- Falcor's **unmodified extracted `toneMapAces` function** is the optional Filmic curve. It replaces the Donut curve within this effect, rather than stacking another tone mapper. This is the fitted approximation used by Falcor, not a complete ACES colour-management pipeline.
- Existing J2 between/inline/MSAA and render-small paths process the selected scene input after motion acquisition and before their own DLSS evaluation. Native NGX SR/RR uses a scoped replacement of Color around the original evaluation. Nested XeGTAO/Donut overrides unwind in reverse order and restore the original parameter object.
- The original game colour is never overwritten by the native adapter. Full allocation, untouched texels and colour subrect origins are retained. This is before native FG; generated pictures are not processed as new SR frames. Our injector's NGX calls and native-FG evaluation branch bypass the native adapter.

This is an **additional scene-linear creative grade**, preserving the input's encoding and alpha. It is not a replacement for the game's final tone mapper and does not convert linear HDR scene data into an SDR backbuffer. We keep the game's final display transform. Optional tone-curve changes deliberately alter the scene's brightness/contrast before that transform. HDR-range values remain possible with the Donut curve; the Filmic choice compresses positive components into its normalized range by design. sRGB-like LDR data is decoded and re-encoded, not filtered as linear HDR. The native adapter uses the supplied HDR creation flag when present; absent flags on 8-bit inputs use the ordinary LDR convention. Unknown PQ/unsupported resource shapes are not interpreted as scene-linear data.

HUD protection uses the existing selected world input and later HUD composition; no second HUD detector is introduced. Where that existing path falls back to a HUD-bearing image, these effects do not independently recover a missing HUD layer. No universal HUD or game-quality claim follows from this feature.

## Defaults and performance scope

Donut defaults retained: histogram percentiles 0.8/0.95, adapted luminance 0.02–0.5, speeds 1/0.5, exposure bias -0.5 stops, white point 3. Wrapper controls: amount 100%, Bloom amount 0.1, sigma 16 input pixels, threshold 1 scene-linear unit; both effects OFF. An SDR scene with no values above the Bloom threshold needs a lower threshold to show glow. Sampling is one histogram sample per 4x4 area, 256 bins with Donut's 6-bit weights. The effect adds no CPU GPU-readback or wait.

On J2's known ordered queue, exposure history is GPU-owned and reacts over real elapsed recording time; size/stream changes and camera reset clear its starting state. Scratch sharing requires Record calls to execute in that queue's order and the returned colour to be consumed before the next Record. J2 records inside its existing serialized submission path.

Native recording-time calls do not provide a proven execution queue. They therefore use independent retained scratch and **immediate histogram adaptation**. The UI reports this. The gradual adaptation-speed controls apply to the ordered J2 path; native immediate adaptation is not falsely advertised as gradual history. This avoids guessing an overlay queue or introducing cross-queue exposure races. Private input processing does not accumulate another grade onto the game's original colour if it evaluates SR more than once.

Cold PSOs prepare on a background worker with existing build-time DXC shaders. Until ready, ordinary rendering continues. Both effects OFF returns before GPU resource/PSO work. Bloom OFF allocates no blur textures and dispatches no blur. No in-game GPU cost/FPS claim has been measured.

## Specific NVRHI improvement, without replacing our core

Reference: [NVRHI](https://github.com/NVIDIA-RTX/NVRHI/tree/bf0bbd9b666a7297492beb1999a4ddc0dfded143), particularly immutable binding sets and versioned/in-flight resources.

Our existing XeGTAO packet already cached descriptor heaps, but rewrote all its bindings on every Prepare. It now caches the immutable binding contents as well, invalidating on actual colour/depth resource, scratch, format or denoiser-output changes. Constants such as jitter, quality and strength still update every recording. Packet reuse remains conditional on its existing owners releasing it; an executable or submitted recording cannot have its bindings overwritten. The same pattern is used in Donut. GPU tests verify warm frames perform no further allocation/binding rebuild, and a changed binding invalidates the cache.

This borrows the architecture; no NVRHI source or backend was linked/copied. Existing RecordingLease, slot fences, queue ownership and hook plumbing remain authoritative. No descriptor observer, HUD extractor, camera/jitter or optical-flow algorithm was replaced. No game-FPS reduction is asserted from the cache counters alone.

## Provenance and packaging

- Donut pin **52efbe37d7f550ea1c5c7ee02b75404ec7c72c09**, MIT. Unmodified original shader/header references and licence are in `third_party/donut`; our explicitly adapted implementation is `effects/donut.hlsl`.
- Falcor pin **759aad033ff610fb0d82c74f7e0a508d0096d5f2**, core BSD-3-Clause. `third_party/falcor/filmic.hlsli` contains the original copyright and selected function. No restricted RTX dependency was copied.
- Generated DXIL headers are build artifacts. A missing DXC builds an unavailable optional effect rather than adding a runtime compiler dependency.
- Opus owns the combined additive package/CMD. Include `third_party/donut/LICENSE.txt` and `third_party/falcor/LICENSE.md` with attribution, alongside the existing XeGTAO/AMD notices. No NVIDIA model DLL or game file is included in this source change.

## Automated evidence

Logs: ignored `local/donut-20261105/`. No game testing. The production J2 integrations use real DX12 and a fake NGX model; native adapter validation uses fake parameter objects, not an actual game/model result.

Initial complete Release build passed after picking Opus's existing reader-slot padding/test fixes through cb2809a. New component + between/inline/MSAA integrations: **4/4 pass, 17.23 seconds**. Actual component test also passes forced WARP with zero D3D12 errors. Tests cover glow spreading, unaffected distant colour, original input preservation, alpha, finite output, filmic alternative, warm resource/binding reuse, gradual exposure response/reset, native subrect padding, rejected malformed options, sRGB round-trip, scoped native Color restoration and output lifetime after scope exit.

Full-suite and final affected receipts are appended below after completion. No full post-final-edit or live-game pass is claimed by these initial checks.

### Final receipt

- Full 210-case suite on the initial effect build at the cb2809a base: **204 pass, 5 skip, 1 fail, 451.33 seconds**. The failure was the pre-existing reader-slot test's 2,000-write/10-second progress bound (1,924 writes, zero torn writes/overlap), not an enhancement failure. Initial shader compile rejected vector ternary syntax; corrected to DXC's supported select operation before these successful effect checks. The first baseline build also hit Opus's already-fixed intentional-padding warning; his existing fix was incorporated, not duplicated.
- Final LDR encoding, settings persistence and existing XeGTAO binding cache: complete Release rebuild and **12/12 affected passes, 39.92 seconds**.
- Incorporated Opus's latest 82bd49b base, including his writer-parking and test changes, then completed another entire Release build: **pass**. Final combined checks: **13/13 pass, 61.20 seconds** (reader-slot lock, Donut, XeGTAO, settings, menu/overlay, native HUD, both effects' between/inline/MSAA production paths).
- Additional final observer/queue/snapshot/scoped-binding checks: **4/4 pass, 13.28 seconds**.
- Both final component executables also pass forced WARP, with zero reported D3D12 errors. No runtime code edits after these checks. The initial full run was before the final refinements/base update; this is not presented as a single all-210 post-final-edit pass.

Engineering implementation and automated checks are complete. The combined package/CMD remains Opus's assigned handoff action; game appearance, performance, interactions with a game's own exposure/Bloom and current HUD separation remain Cort's live test. No extra review cycle is requested.
