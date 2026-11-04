# HUD through draw handoffs, 2026-11-04

This extends the existing native-HUD machinery for games that finish their picture with a draw rather than a copy. It contains no SE5 executable hash, shader hash, fixed game address or title-specific branch. Cort retains game testing; source/fixture results are not a new game-quality receipt.

## The two methods

1. **Separate scene and native HUD.** At the first admitted HUD draw, snapshot the scene. Replay the actual game HUD draws into the existing native-size black/grey layers, using the game's shader, blending, viewport, scissor and mirrored depth/stencil. Also forward the original HUD draw once, preserving the complete game picture. At the actual final handoff, snapshot that complete upstream image and attach the scene snapshot and native layers to the destination back-buffer identity.
2. **Protect actual drawn coverage.** When some draws cannot be represented as a clean affine native layer (for example, clipped additive blending), retain the complete game colour as NGX input. Derive coverage from the black/grey replay's actual pixels, zero motion and provide NGX's current-colour bias there, then preserve the current original composite under that coverage after reconstruction. The mask follows the actual shader/discard/stencil/blend result, rather than a bounding rectangle or a learned static-image region. Native pixels belonging to an input pixel are conservatively included so thin glyphs cannot disappear between sample centres.

The second method preserves a low-resolution composite region, including its background, and does not claim to recreate native text that was already rasterised at low resolution. Unsupported HUD draws stay in the original picture; discovery is not reported as universal qualification.

## A draw is not assumed to be a copy

The existing operation snapshot identifies the picture-sized SRV actually read by the shader. Unused descriptor-table slots cannot establish lineage. This covers the t17-versus-stale-t0 case that a 16-descriptor prefix missed. It identifies a dependency; it does not certify the draw's colour/UV transformation.

For each captured occurrence, the GPU compares the saved complete source with the actual final picture before selecting the NGX input. Matching within format precision permits the clean/protection branch. A tone-map, blur, UV change or additional output UI that changes those pixels leaves the original complete final image as input. No later asynchronous CPU verdict is used to justify deleting an earlier draw. No arbitrary HDR image is substituted for an SDR postprocessed image.

For a verified transfer and complete affine replay, NGX receives the scene snapshot; normal reconstruction and enhancement run, then the actual native HUD layers are composed. For partial/non-affine replay, the coverage method applies. If the transfer is not verified, both selection and composition retain the complete original picture. Layout changes therefore do not lose a previously redirected HUD.

Common opaque, alpha-over and multiplicative RGB blends can use the clean branch. MIN/MAX, logic blending, destination-alpha coupling and clipped additive blending use the protection branch. Shader UAV writes (including root UAVs), stream-output shaders and active game queries are excluded from new replay. Query begin/end tracking reuses the existing variant-aware list hook; no second graphics hook is installed. Cached PSO metadata supplies replay eligibility, avoiding a descriptor snapshot/reflection walk for every HUD draw. Candidate transfer source snapshots remain bounded to actual final-picture draws.

## Frame identity and ownership

The captured source occurrence ends at its handoff, not at a later CPU Present. Its immutable picture/layer packet is attached to that destination buffer. Render-ahead can overwrite the shared upstream resource for another picture without changing this packet. Every source recording retains its snapshot/layer ownership; selection requires submitted queue-ready leases. Present retains the packet through its own existing fence.

Layer and picture pools reuse only released/retired ownership. They discard inactive old extents after resolution changes instead of retaining every historical size. Completed Present owners are released promptly rather than held for an entire ring rotation. There is no GPU wait, new queue or synchronous diagnostic image readback in the callbacks.

The verification, input selection and composition run on the existing Present command list. GPU proof is copied to a 16-byte readback for status only; the CPU maps it after the existing Present fence proves completion. `native_hud ... draw{... gpu_verified=... changed=... clean=... protected=...}` distinguishes actual completed GPU results from candidate counts. These counters do not control the rendering decision.

## Capture and colour handling

F8 records the actual NGX input, not an earlier unselected image. Extra planes include `hud_n0`, `hud_n1`, `hud_scene`, `hud_full` and `hud_draw_mask`. Extra-plane capacity is twelve so these can coexist with scene colour, provenance, camera and flow planes. The flow-stage jitter repair remains a separate completed increment.

Selected input uses RGBA16F while preserving the source's encoding; the existing explicit NGX HDR declaration still controls HDR interpretation. The layer view controls sRGB decoding/composition. HDR highlights above one and output alpha are retained. The wider sharpening/Clarity work remains Opus's lane; it should precede native HUD composition and use this coverage where UI remains in the input.

## Automated evidence

The actual DX12 test exercises both GPU branches, a changed processing transfer, incomplete replay, exact used-SRV reflection, ordinary copy/native-HUD behaviour, clipped additive fallback and an active query followed by recovery. It also runs two different pictures through the same source resource before presenting the first; the first scene snapshot and native layer stay attached to their own destination. Reuse requires fresh current scene/full snapshots. Float-HDR composition checks values above one and alpha. The DirectX debug layer reports zero error messages in the focused test.

Final runtime source eb422f3: full Release build passed; complete 199-case suite finished with 194 passed, five skipped and zero failed in 297.25 seconds. Skips are the optional NR runtime probe, three unsupported 16xMSAA cases and the existing upscaler jitter case. Logs are local-only under `local/se5-flow-jitter-20261104/hud-release-{build,tests}.log`. No game launch/control/deployment or independent installer change is part of this increment. Actual SE5 handoff acceptance, visual text quality and game-time GPU cost await Cort's test.

## Practical limits

This qualifies a same-size, same-family picture handoff that preserves its pixels. A processing/warping handoff is preserved intact rather than treated as an identity transfer; carrying native HUD through an arbitrary nonlinear or spatial postprocess is not claimed. Compute/indirect UI and unknown shader reflection continue through the game. Extra work includes retained scene/full copies, native HUD replay and three small GPU stages; pools bound reuse to actual in-flight/executable recordings, not a fixed number of Presents. Game-time cost has not been measured.
