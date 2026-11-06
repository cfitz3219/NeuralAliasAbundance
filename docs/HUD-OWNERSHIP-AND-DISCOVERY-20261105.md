# HUD ownership, mixed canvases and automatic source discovery

Astra's HUD correction starts at Opus15f544f on `astra/se5-unblend-review`. Cort assigned HUD priority over further anisotropic-filter investigation. Opus acknowledged runtime ownership in main COMMS; he owns the combined installer. Cort retains game launch, installation and visual testing. No game files, settings, driver profiles or launchers were changed during this work.

This is a correction to the existing machinery. It uses the existing observer, exact operation snapshots, Layer-2 private bindings/constants, HUD `Picture` GPU comparison and recording/submission leases. DLSS, jitter, motion/flow, NR and the GPU infrastructure remain in place. There are no game names, executable hashes or fixed game addresses in the new decisions.

## Findings established before changing rendering

1. **Wrong target association reproduced on real GPU pixels.** The old test drew another veil into an unrelated target AFTER copying the real canvas into the proxy. That other target never reached the proxy, but the old retarget rule still returned its layers. Adding an actual scene assertion failed: RTX recovered0.245403 versus true0.200000; WARP0.242520. The old pointer/count assertions had passed. Original expected failures are retained under main `local/opus-af-watch/hud-review-retarget-probe*`.
2. **Opaque output does not imply isolated HUD.** SRC_ALPHA/INV_SRC_ALPHA with shader output alpha1 also produces identical black/grey results. A shader copying a complete scene-plus-HUD canvas puts the world in both layers. Changing only the PSO's DestBlend classification cannot separate it.
3. **The actual Sniper trace contains a mixed batch.** In p23896's trace ending at Present61535, render-sized target13597 has a54-vertex blended batch sampling render-sized13595 plus small material/font resources. A subsequent final draw has no source in the bound-table trace. That trace is not an exhaustive shader-use inventory; it does not prove there is no texture dependency. These IDs are evidence only and are not in the implementation.
4. **The old inversion leaves opaque text in DLSS.** At zero background coefficient, dividing cannot recover the covered scene. The implementation kept the complete picture there, so opaque HUD could still be reconstructed and sharpened.
5. **Viewport bounds do not prove geometry coverage.** Three/six vertices can draw small opaque geometry with a full-target viewport and scissor. The old shortcut erased all earlier layers and skipped the small draw.
6. **CPU recording order differs from queue execution order.** A future recording into the same resources can replace the CPU's apparent latest canvas before the current recording executes.
7. **A reusable recording exposed a one-shot snapshot-state bug.** First-recorded SnapshotScene/SnapshotFull assumed their destinations initially entered COPY_DEST. Re-executing the closed command list encountered NPSR instead. DX12 validation reported the invalid copies/barriers. Snapshot resources now always enter/leave NPSR, including their first execution.

The installed p23896 evidence predates L0/L1 recording and the new code. Its opaque-only match rate was approximately24%; it does not establish which individual combination above caused every live pixel. Opus confirmed there is no newer exact13620/13622 draw capture available. The new automated reproductions establish actual code defects independently of that missing capture.

## Runtime decisions

| Observed render behaviour | Handling |
|---|---|
| Scene and ordinary UI share a target | Capture the real pre-HUD scene, replay the actual UI geometry into render/native counterfactuals, validate against this completed picture. |
| A whole-image CopyResource/CopyTextureRegion carries the canvas onward | Freeze its layers and scene at that copy occurrence. Later writes to the source cannot change the handoff. Copy chains and late destination HUD inherit the original pre-HUD scene. |
| An opaque final draw samples a known HUD canvas | Preserve the source's coefficient/colour domain and snapshot it at consumption. GPU comparison checks the actual final pixels, including transformations or partial coverage. |
| One UI batch samples the world as well as drawing UI | Rebind the real reflected world SRV to the corresponding private background/layer in all four replays, using existing immutable descriptor and constant plans. The unchanged game draw still executes once. |
| Final source dependency cannot be obtained by the replay route | Probe current submitted canvases using the existing GPU picture comparison. Two predominantly identical completed pictures learn an association. Every enhanced frame still performs current pixel comparison; historical evidence only chooses a candidate. |
| Camera/depth scene geometry, followed by ordinary scene postprocessing | Keep scene draws on their existing path. Descriptor snapshots occur after camera/depth exclusion. A picture chain descending from observed scene work stays scene until a terminal HUD contribution begins. |

No target is promoted merely because it was recorded last or has a plausible size. The source probe is asynchronous, uses the presenter's existing completion fence and reads16 bytes after completion. It neither waits for the GPU nor reads images back in a live game callback. Only canvases newly submitted since the output's previous Present enter independent discovery; retained old resources do not crowd out the current candidates.

## Actual scene and composition

For an affine contribution, P = L0 + m*S and m = (L1-L0)/grey. The real S is captured BEFORE HUD, including under opaque pixels. The current-picture GPU verification checks both final-picture identity and that equation, including intermediate UNORM clipping and finite values. Native N0/N1 are then composed over the reconstructed scene.

Where the measured model does not hold, the existing GPU map protects those current original pixels. Valid regions continue to use the clean scene and native HUD. A stale30-frame diagnostic cannot switch the entire effect off and on. Final-picture agreement has a separate count from blend-model agreement, so a locally non-affine material does not revoke a correct source association.

The reflected source's sRGB meaning is explicit and independent of the output view. The clean reconstruction input is converted into the output's colour encoding. The coefficient check uses the source's actual sampling domain. The original final alpha is preserved. No guessed global gamma correction or tolerance increase is used.

## Submitted ownership and reuse

Each closed recording freezes its own owner. A later recording creates a separate owner and inherits prior contributions only when it is continuing them. Submission packets publish/invalidate resource associations in actual ExecuteCommandLists order and operation order. Recorded-ahead but unsubmitted data cannot replace a submitted picture. Repeated execution republishes that recording's own snapshot.

Recording leases retain private resources through discard, Reset and completion of every submission. Present owners additionally prevent pool reuse. Snapshot copies are state-balanced on every execution. Another Present of an already prepared picture forks its mutable descriptors/output resources through a small retained reader pool; it does not mutate an earlier in-flight consumer. Size, format and actual device changes reset source learning. No native game resource is replaced, and no synchronous production GPU wait is introduced.

## Automated evidence and limits

Isolated native-backend tests pass on RTX5070Ti and forced WARP. The expanded test exercises the actual GPU implementation and compares pixels:

- Original wrong-target reproduction now reconstructs0.200000 exactly.
- Clean scene sent to reconstruction includes every opaque HUD pixel, maximum channel error0.000391.
- Native composition matches a direct native raster reference within1/255.
- Source overwritten after copy, two-hop copy chain, late HUD and ownership beyond three Presents pass; recovered-scene error0.002353 RTX /0.003481 WARP.
- Mixed world/HUD batch plus opaque final draw has0/255 equation error in the fixture.
- Actual mixed-canvas reconstruction input error0.000391 for UNORM and0.000187 for an sRGB source sampled into UNORM.
- Query-protected final draw is discovered by completed GPU pixels and selected after two matching pictures, without duplicating its query-visible draw.
- Render-ahead, separate submissions and repeated reversed queue execution retain the exact submitted scene; DX12 validation reports no errors after the snapshot-state repair.
- A small opaque triangle with full-target viewport/scissor preserves the preceding contribution.
- Non-affine UNORM clipping protects the affected pixels while leaving the correctly separated scene active elsewhere.
- Repeat-Present snapshot forks retain identical clean input in independent output resources.
- Existing native HUD regressions, including DXIL, stencil/depth, draw handoff, HDR world/UI composition, separate lists/submissions, render-ahead and resize cases, pass on RTX and WARP.

These tests use controlled scene images and reconstruction stand-ins where applicable. They establish routing, pixels, states and lifetime; they do not establish live Sniper quality or game FPS. The complete NVRHI-on Release build and full CTest receipt follow below after completion.

Coverage is broader but not proven for every renderer. This new counterfactual route handles ordinary side-effect-free, single-sample, base-level2D compatible-colour targets and full-image copy geometry. A HUD producer that itself uses direct heap indexing/UAV side effects, a different HDR-to-SDR composition, partial-copy/subrect mapping or unsupported blending still needs its corresponding existing/native replay path or further acquisition. Hidden FINAL dependencies are covered by GPU discovery; that is distinct from replaying a bindless HUD producer. Arbitrary nonlinear postprocessing cannot be inverted from two samples. The actual frame stays intact where current proof does not establish the model. No universal success claim is based on these fixtures.

## Integration

Runtime increments:42ca3dd (target ownership/current picture), e42c5b7 (mixed canvases/hidden discovery),996b250 (submitted order/reusable recordings). Use the final branch HEAD and qualification receipt, including the follow-up device/source refresh correction. Do not reset Opus's package branch to this checkout; merge onto his latestAF/UI/Breadcrumbs source. Keep the established additive installer, saved settings, rollback and native DLL policy. Opus owns the combined CMD; Cort owns game testing. No extra review cycle is requested.

Primary contracts used: [Microsoft colour blend description](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_render_target_blend_desc), [command-list execution and synchronization](https://learn.microsoft.com/en-us/windows/win32/direct3d12/executing-and-synchronizing-command-lists). Implementation evidence above comes from our actual source and GPU reproductions.

## Final engineering receipt

Runtime HEAD836849b includes the final source/device refresh and current HUD status correction. PR108 targets Opus's `claude/se5-overhead-argon`; a read-only merge-tree check with his ed65e11 succeeds without conflicts. Integration should merge the complete branch, retaining his later AF wording. No further runtime changes follow this receipt.

- Complete NVRHI-on Release build: PASS. Full213-case suite: **208 passed,5 skipped,0 failed,351.38s**. This broad run includes the ownership/mixed-canvas/submission/snapshot repairs and device/source-refresh changes. The five existing skips are the opt-in NR runtime probe, unsupported16xMSAA trio and real-runtime upscaler-jitter probe.
- After the final current-picture-only handoff and HUD hint correction: complete Release rebuild PASS; six affected HUD/Anno/NVRHI/overlay/shared-service cases PASS16.42s; actual route-A menu case PASS0.12s.
- Final forced-WARP HUD/native-HUD/NVRHI/shared-service/menu cases: **5/5 passed,0 skipped,0 failed,9.26s**.
- Earlier native-backend diagnostic configuration and both RTX/WARP HUD paths passed before enabling NVRHI. The source correction does not require NVRHI to own the game's lists.

All raw receipts are ignored under main `local/opus-af-watch`: hud-nvrhi-config.log, hud-full-{build,tests}.log, hud-final-{build,tests,menu,warp}.log, hud-submit-{test,warp,native,native-warp}.log and earlier deliberately failing reproductions. Final tests use the real D3D12 debug layer and report no resource-state errors. A final actual-input check also verifies sRGB-to-UNORM source handling, not just returned pointers/counters.

The overlay now describes current source discovery and the last completed image check. Its historical unblend statistics remain diagnostics in the log and cannot authorize an unverified legacy handoff. Where the real scene/full snapshot is unavailable, normal rendering and discovery continue.

This is source engineering qualification and an Opus integration handoff. It is not a claim that the game was installed, that a new CMD is staged already, that Sniper's HUD is visually accepted, or that all DX12 renderers have been tested. Cort retains that live test; Opus retains combined build/package ownership.

## Main-menu regression found in Cort's f48642c run; corrected a56a9e8

Cort reported a main-menu lockup after installing Opus's f48642c merge of this HUD work. Opus supplied both p40248/p34828 runs and DRED. Installed hashCDDAD19E...DFC8366A matches that package, and its ancestry includes836849b. This was the new build, not a stale installer. Both processes lost their D3D12 device with0x887A0001; DLSS, flow and NR evaluations remained0. DRED had no page-fault address and breadcrumbs were disabled. The device had ample memory budget. The later `could not retain recording` status does not establish the first failing operation.

**Confirmed cause:** TakeFrame attaches typeless L0/L1 validation resources to the Picture. In a main menu without qualified depth/motion, reconstruction's Prepare does not run. Prepare was also the only code assigning layer_view. If that Picture then participates in source discovery, Probe's common bind function created SRVs for the retained L0/L1 using FormatUNKNOWN. Typeless allocations require a typed view. The void CreateShaderResourceView call immediately removed the device with DXGI_ERROR_INVALID_CALL. The discovery shader does not need those coefficients at all when its model flag is off.

A test-only addition to the unchanged HUD runtime reproduced the exact call and error on RTX and forced WARP, before any invalid command list was submitted. The debug layer explicitly reports FormatUNKNOWN/R8G8B8A8_TYPELESS, followed by RemoveDevice with0x887A0001. The previous tests probed before attaching the coefficients or prepared reconstruction first; they did not exercise this actual menu order. The earlier passing qualification remains recorded above, but did not establish that startup order as working.

**Repair:** bind only the inputs/outputs used by each picture entry point. Unused slots receive valid typed null descriptors. Discovery no longer binds retained render-target coefficients, native layers or reconstruction UAVs. Typeless frame allocation formats map to their typed storage views. Repeated completed probes reset their consumed receipt, and an already prepared discovery consumer forks through the existing snapshot pool before its descriptors can be reused. HUD discovery/replay stays enabled; no game-specific exception, feature disable or new rendering system was added.

Corrected typed and typeless main-menu probes both match2048/2048 actual GPU pixels with a healthy device. Repeated/forked probes return fresh receipts. Complete Release rebuild plus9 affectedRTX HUD/nativeHUD/J2/Anno/NVRHI/menu/service/effect cases PASS21.60s. Final expanded RTX HUD test PASS, and forcedWARP4/4 affectedcases PASS9.95s. No GPU validation errors. The complete213suite above predates this repair; no new full-suite pass is claimed. This narrow API-binding repair has the targeted production/pixel checks listed here.

Ignored evidence: main `local/hud-menu-lockup-20261105/repro-{rtx,warp}.log`, fix-{all-build,tests,warp,final-rtx}.log. Original game evidence remains Documents/NA-se5-results-20261105-185044 and local/j2-se5-dlaa/NeuralAlias-dred-{40248,34828}.txt. Sourcea56a9e8, PR109 into Opus's package branch; pick that one runtime commit ontof48642c, retaining his AF/package changes. Opus stages the combined additive CMD; Astra did not launch, install, restore or change game files/settings. No further game run or expanded capture was needed to find this cause. Live confirmation of the corrected package remains Cort's test.
