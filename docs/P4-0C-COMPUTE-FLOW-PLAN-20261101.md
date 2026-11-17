# P4-0c: AMD FidelityFX compute optical flow for J2 (inline route, and fallback)

Status: rev 1, 2026-11-01, Opus. Cort: "Begin the engineering work for AMD's optical flow so it's ready
for Sniper (and any other game setups you can anticipate)." Follows P4-0b (NVIDIA flow, between-lists,
PR #73), and Astra's review point 6 (COMMS 6a15578): inline needs same-frame flow, and the cleaner
candidate is AMD's compute flow, which Cort has already authorized reusing.

## Why compute flow
NVIDIA's optical-flow engine is a separate unit driven by fences. It can only run between
ExecuteCommandLists batches. Sniper Elite Resistance / SE5 record the scene and its post-processing
in ONE command list, and J2 inserts DLAA inside it (inline route), so there is no gap for NVOFA.
AMD's FidelityFX optical flow (FSR 3, MIT) is plain compute. It can be recorded into the same list,
right before DLSS. That makes the flow same-frame on the inline route too.

## Reuse (no new solver)
- Source: GPUOpen FidelityFX-SDK `release-FSR3-3.0.3` (a0632ab), MIT, vendored unmodified under
  `third_party/ffx-opticalflow/`: the 10 `gpu/opticalflow/*.h` headers, the 7 DX12 pass `.hlsl` entry files,
  `ffx_core*`/`ffx_common_types.h`, SPD headers (luminance pyramid), and the licence.
- Passes, in AMD's order: prepare luma, luminance pyramid (SPD, 7 levels), scene-change histogram,
  scene-change divergence, then per level from coarse to fine (7 levels): search (v5), filter, and scale
  to the next level. Output: one int16 vector per 8x8 block (S10.5 pixels), the same as our NVOFA grid
  at grid 8, so FlowExpand, DeriveResidual and the acceptance pass (cost-free: texture gate, image check,
  1 px snap, validity) are reused unchanged. AMD's scene-change output resets pairing.
- Our own small dispatcher replaces AMD's backend: each pass has an embedded root signature
  (`FFX_OPTICALFLOW_EMBED_ROOTSIG`: a UAV table and an SRV table of 32, 8 root constants, two static
  samplers), so we bind two descriptor tables and constants per dispatch. No FidelityFX runtime DLL.

## Shader compilation
The passes use Shader Model 6 wave intrinsics, so FXC (D3DCompile) cannot build them. They are compiled
at build time with DXC from the Windows SDK (`-T cs_6_2 -E CS -DFFX_GPU=1 -DFFX_HLSL=1
-DFFX_OPTICALFLOW_EMBED_ROOTSIG=1`, HDR permutation 0/1), and the DXIL is embedded as generated headers in
the build folder (never committed). Without DXC the feature compiles out (stub: unavailable), like NVOF.
Devices without SM 6.2 or wave ops report unavailable at run time.

## Engine selection (automatic, per frame, no per-game code)
| Route J2 picked | NVOFA available | Engine |
|---|---|---|
| between lists | yes | NVIDIA (P4-0b, batch split) |
| between lists | no / failed / latched | AMD compute, recorded in the pre list |
| inline | any | AMD compute, recorded in the game's list before DLSS |
| any failure | | camera-only, exactly as today |
A route or engine change resets pairing (no mixing of two engines' frames). Status line and the `j2flow`
log line gain `engine=nvidia|compute|none`.

## Other setups planned for
- MSAA scene colour: run the flow on J2's resolved colour (J2 already resolves for DLSS).
- HDR / float colour: AMD's prepare-luma has an HDR permutation; we feed J2's fixed encode
  (Reinhard + sRGB) so both engines see the same image.
- Sub-rect / dynamic resolution: sized to J2's render size, rebuilt with deferred retirement on resize.
- Cuts: AMD's scene-change detection and J2's cut flag both reset pairing.
- Cost: compute flow competes with the game for shader time. A half-size option (AMD runs on our
  downscaled encode) is the first lever; measured, then defaulted.

## Increments
1. Vendor + DXC build step + stub (CI stays green without DXC).
2. `src/host_dx12/motion/ffx_flow.*`: resources (two luma pyramids, two flow pyramids, SCD buffers),
   PSOs from embedded DXIL, `Record(list, colour, reset)` → 8x8 grid. WARP/real test: a moving textured
   object over a still background (sign, units), a scene cut.
3. j2_flow: an in-list engine kind beside the fence engine (no split, no fences, no watchdog needed).
   Between-lists fallback when NVOFA is unavailable.
4. Inline route: Prepare/RecordFusion inside the game's list before DLSS; state restoration as J2 inline
   already does (we only touch J2's own copies).
5. Status/log fields, menu, package for Cort's Sniper check.
Cort owns all game testing. Astra reviews at the gate; the inline hunks in live_dlaa.cpp are posted to
COMMS before they are edited.
