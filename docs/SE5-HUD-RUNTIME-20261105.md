# Shared HUD repair, 5 November 2026

Base is the actually installed 66840fe. Cort's test failed: no submitted native replay or final native HUD composition. Do not describe this base as HUD fixed.

## Completed first increment (not yet a user package)

Completed handoffs are immutable packets published in actual queue submission order. Later CPU recording cannot erase an earlier submitted output. Source recordings may be submitted separately; their existing RecordingLease ordering and queue-specific readable resource states replace the old same-Execute-array requirement. A batch must still actually carry the final output to PRESENT; an unrelated batch cannot license replay.

The original exact identity verdict remains available, but native/clean pixel selection now uses a local agreement map in the existing GPU proof allocation. A changed cursor/corner preserves the original there without discarding native HUD elsewhere. The bilinear footprint must agree. Same colour tolerance, no synchronous callback readback or extra full-screen pass. Completed receipts report matched/tested pixels and maximum colour error.

Additional reproduced shared failure: the eight live-snapshot slots were occupied by already-retired recordings merely because cached canvas owners retained immutable snapshots. Retired packets now leave the live-recording pool even while other owners retain their data; their Pictures cannot be overwritten while those owners exist.

Focused actual-DX12 j2_native_hud_test passes after the repairs, including separate Execute calls, unsubmitted future recording before an earlier submission, delayed Present, changed processing, alpha/HDR, original queries and unsafe-buffer fallback. Expanded split test failed on the original compiled runtime at missing native_picture/native_check. Its first draft wrongly called a combined AfterSubmission after the separate submissions; that artificial permission was removed before recording the failure. An additional render-ahead test exposed the retired-snapshot pool exhaustion and now passes. Logs are ignored local/se5-hud-failure-66840fe-20261104. Model evaluation in this fixture is fake; pixel operations and DX12 state are real. Full combined suite remains pending.

## Remaining work

Earlier offscreen HUD input replay is in progress. The preserved real trace has a same-sized UI canvas with roughly163 blended draws, including glyph-atlas reads, before the final picture. It must not be required to equal the complete scene: UI-only textures and whole-picture canvases are distinct. Reuse native glyph replay and actual finishing-shader bindings, keeping world reconstruction separate from transparent UI rendering. Finish this lane and full tests before handing the combined changes to Opus. Cort explicitly assigns CMD packaging to Opus and game testing to himself. Do not update/install a partial candidate. Performance bottleneck repair follows HUD; Opus's latest lock work is recorded in main COMMS and must be integrated/reviewed without duplication.
