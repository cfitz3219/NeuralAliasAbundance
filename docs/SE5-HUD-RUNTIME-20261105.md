# Shared HUD repair, 5 November 2026

Base is the actually installed 66840fe. Cort's test failed: no submitted native replay or final native HUD composition. Do not describe this base as HUD fixed.

## Completed first increment (not yet a user package)

Completed handoffs are immutable packets published in actual queue submission order. Later CPU recording cannot erase an earlier submitted output. Source recordings may be submitted separately; their existing RecordingLease ordering and queue-specific readable resource states replace the old same-Execute-array requirement. A batch must still actually carry the final output to PRESENT; an unrelated batch cannot license replay.

The original exact identity verdict remains available, but native/clean pixel selection now uses a local agreement map in the existing GPU proof allocation. A changed cursor/corner preserves the original there without discarding native HUD elsewhere. The bilinear footprint must agree. Same colour tolerance, no synchronous callback readback or extra full-screen pass. Completed receipts report matched/tested pixels and maximum colour error.

Additional reproduced shared failure: the eight live-snapshot slots were occupied by already-retired recordings merely because cached canvas owners retained immutable snapshots. Retired packets now leave the live-recording pool even while other owners retain their data; their Pictures cannot be overwritten while those owners exist.

Focused actual-DX12 j2_native_hud_test passes after the repairs, including separate Execute calls, unsubmitted future recording before an earlier submission, delayed Present, changed processing, alpha/HDR, original queries and unsafe-buffer fallback. Expanded split test failed on the original compiled runtime at missing native_picture/native_check. Its first draft wrongly called a combined AfterSubmission after the separate submissions; that artificial permission was removed before recording the failure. An additional render-ahead test exposed the retired-snapshot pool exhaustion and now passes. Logs are ignored local/se5-hud-failure-66840fe-20261104. Model evaluation in this fixture is fake; pixel operations and DX12 state are real. Full combined suite remains pending.

## Remaining work at the first checkpoint

Earlier offscreen HUD input replay is in progress. The preserved real trace has a same-sized UI canvas with roughly163 blended draws, including glyph-atlas reads, before the final picture. It must not be required to equal the complete scene: UI-only textures and whole-picture canvases are distinct. Reuse native glyph replay and actual finishing-shader bindings, keeping world reconstruction separate from transparent UI rendering. Finish this lane and full tests before handing the combined changes to Opus. Cort explicitly assigns CMD packaging to Opus and game testing to himself. Do not update/install a partial candidate. Performance bottleneck repair follows HUD; Opus's latest lock work is recorded in main COMMS and must be integrated/reviewed without duplication.

## Completed engineering handoff (supersedes the open checkpoint above)

Runtime commits in order, based on the installed 66840fe:

- a5928ab: submitted immutable packets, separately submitted sources, local agreement and retired probe-pool repair.
- 534cb56: separate UI-only/world reconstruction and actual native finishing replay.
- fcbb570: current-frame HUD status and partial-proof accounting; no rendering-math change.

The UI-only path follows the actual reflected compositor dependencies. It requires a uniquely identified, same-extent HDR world SRV (RGBA16F, RGBA32F or R11G11B10F) alongside the observed blended UI canvas. It snapshots the world at consumption and sends THAT scene to the existing production PreHudUpscale callback. The UI canvas is redrawn at native size independently. Both native inputs replace their corresponding bindings in the original finishing shader; colour processing is retained. Original-size replay remains the independent check. No resource ID, game hash, executable address, or guessed final-buffer location is hardcoded.

Captured resources, source recording leases, submitted queue identity and readable end states remain required. Later CPU recording cannot replace a submitted packet. Existing jitter and flow maths/settings are untouched. Native/check/compositor work remains on the existing command and lifetime machinery; no synchronous GPU readback or new callback wait. Local proof uses the existing allocation and verification pass; changed areas retain the game's image while agreeing areas retain native UI. Whole-picture identity remains a separate diagnostic fact.

The overlay no longer treats any historical HUD frame as proof that native HUD is active now. Partial local agreement is reported separately from a wholly rejected native picture. The sharpness tooltip says that fallback HUD embedded in the scene may still be sharpened.

### Validation and limitations

Complete Release build plus all 200 CTest cases on 534cb56: **195 passed, five skipped, zero failed**, 261.77 seconds. Skips: optional NR runtime, three unsupported MSAA16 cases, optional upscaler jitter. After the final status-only fcbb570 patch: complete Release rebuild and eight affected tests **8/8 passed**, 14.49 seconds (actual raster jitter, AMD/fit, camera fit GPU, scoped bindings, production PreHudUpscale, native HUD, presenter ownership, fullscreen/resize). This is not a claim that a second 200-case run occurred after fcbb570.

Expanded real-DX12 regressions reproduce and cover separate Execute calls, unsubmitted future recording before earlier submission, delayed Present, local changed regions, transparent UI plus separate HDR world, native geometry edges, changed finishing shaders, alpha/HDR, original queries and unsafe-buffer fallback. Production callback asserts that fake NGX receives the separate world scene, not UI. Model evaluation is fake in those fixtures; actual resources, state, GPU pixels and finishing shaders are real. The glyph-like geometry surrogate does not prove the game's font atlas resolution or actual live text quality. Debug layer reported zero errors.

Logs: ignored `local/se5-hud-failure-66840fe-20261104/{final-release-build,final-release-tests,status-final-build,status-final-tests}.log`. Earlier baseline reproduction and incremental logs are retained there. The installed game DLL and CMD were not changed. Arbitrary bindless/compute compositors are not demonstrated by these finite raster tests; discovery remains active and the original complete picture is retained when replay cannot be established.

### Exact packaging instruction to Opus

Pick a5928ab, 534cb56, fcbb570 in that order onto your current production/overhead branch (which already contains base66840fe). The later receipt-only documentation commit is optional. Do not import unrelated Layer2 ancestry or re-pick the jitter repair. Preserve your own AF/mip and descriptor-lock commits; resolve WORKLOG by retaining both append-only histories. Build/test the combined source, then use the established additive installer to update the existing SE5 CMD. Cort owns game installation/testing. No extra review cycle requested. Do not call actual game HUD quality confirmed before his run.

Astra now continues the independently assigned frame-rate investigation: verify your descriptor-lock theory against raw evidence and inspect your reader/lifetime coverage plus other possible submission stalls. This HUD handoff does not assert any recovered game FPS.
