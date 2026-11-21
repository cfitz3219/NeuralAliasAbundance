# Unused camera-discovery work — 2026-11-17

Owner: Astra. Base: Fable's integration `87f4a64`, including PR156 and movers-first replay. Cort asked for another performance pass beyond the earlier bookkeeping/GPU cuts. Fable keeps replay, integration and the next referee work. No game settings, installation or game controls changed.

## Finding and the path to the cost

`[j2] camera=0` promises that the game's camera is never searched. J2's live path honors that, but the independent discovery consumer did not:

1. `ExecuteBegin` called `snapshot_submission` on every submitted batch. It copied up to 64 constant bindings / 64 KiB from mapped buffers into observation events, including when nobody should be searching for a camera.
2. `diagnostic_adapter::Sink` continued ranking/scanning those snapshots for matrices and requesting separate GPU-timeline buffer samples. These scans log hypotheses; they do not supply the current J2 frame's camera or motion.
3. `UniversalHeuristics` continued per-draw root-CBV tallying solely for camera ranking. Its depth/colour scoring has no dependency on those tallies.

The active Resistance process PID38380 confirms the setting: status line15973 reports `camera_setting=0`, camera supplier0 / measured81659; line15978 reports the saved camera setting0. A read of its discovery summary during this audit had 71,849 presents, 17,077,173 snapshot records, 6,654,728 captured snapshots and 10,214,236 GPU-written statuses. The last category is NOT a CPU copy: do not describe all17million records as successful memory reads. Matrix scans and GPU camera hypotheses continued. The source explains why. The log is live and later lines may increase the totals.

Evidence locations (local only):

- `C:/Users/Cort/NeuralAliasAbundance/local/j2-ser-dlaa/status-38380.log`
- `C:/Users/Cort/NeuralAliasAbundance/local/j2-ser-dlaa/discovery.jsonl`
- Previous PID10376 status also ends at camera0, with229598 measured-camera-free frames.

The snapshot copies are made inside the queue submission hook, before forwarding the game's submission. They are not all background-only. Some read upload memory: Microsoft's [Map guidance](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12resource-map) warns against CPU reads from upload/write-combined resources because of their cost. That supports removing unused reads, not a specific FPS estimate on Cort's machine.

## Implemented change

- The existing `na_observe_skip_kinds` facility now suppresses `NA_EV_SNAPSHOT` when the adapter observes camera0. The producer checks it BEFORE collecting bindings, reading buffers, assigning event sequence numbers or filling the transport ring. Default external consumers retain the original behavior unless they request suppression.
- The adapter honors camera0 at startup and on subsequent batches. It ignores already-queued camera snapshots, releases pending camera-buffer sample leases through the existing asynchronous lifetime API, clears camera-only plans/retry entries, and stops new camera sample requests. Texture/depth/colour sampling and scene selection remain intact.
- The heuristic reducer stops camera-only binding/ranking bookkeeping and removes old camera rankings. Camera policy survives a session/epoch reset. Re-enabling discovery starts learning again from subsequent observations; it does not claim an old suppressed recording supplied fresh camera evidence.
- Stage-M descriptor-event suppression composes with the snapshot mask instead of overwriting it when a frame-map session ends.
- A small `camera_discovery` log receipt records policy changes. No new menu toggle, forced camera setting, reduced motion precision or model change.

The independent J2 camera reader, viewport jitter, submitted depth evidence, motion synthesis, DLSS evaluation and replay admission are unchanged. DX11 rendering is unchanged; this fix is in the DX12 observation consumer. It contains no executable-name or engine-specific condition.

## Verification

Receipts: `C:/Users/Cort/NA-camera-runtime-audit/local/discovery-work-trim/`.

- Eight selected CTests passed, zero failed/skipped, 33.35s. Tests used `NA_TEST_FORCE_WARP=1`, including actual adapter/transport interaction and tiny128x128 software-rendered scenes. Cort was playing Sniper; no RTX rendering tests or performance benchmark was run alongside it.
- Snapshot submission regression uses the same recorded list in on/off/on order. It observes64 / 0 / 64 snapshot events and64 / 0 / 64 actual guarded-copy calls; thread-event numbering stays contiguous. No GPU commands are executed by that particular test.
- Adapter camera-off test retains depth and scene-colour discovery, matched execution samples and scalar depth decoding, but produces neither matrix scans nor GPU camera hypotheses. A live on/off/on test produces no such hypotheses during the disabled interval and resumes matrix scans afterward.
- Sample acquisition's11 CPU cases include cancelling only buffer leases, retaining the texture lease, avoiding reads/double releases, and restarting in the same scope.
- The14 heuristic cases include identical depth/colour scores and draw counts with camera ranking on/off; toggling it off also removes cached camera evidence. Existing Anno-style colour/depth pairing and loading-screen rules pass.
- Initial test link failed because the discovery adapter's new settings dependency was not linked into its standalone fixture. Added the actual `host_settings` library to that target's dependency list; rebuilt and reran. No stub was substituted.
- Release `NeuralAliasDXGI.dll` built successfully with `NA_GAME_ADAPTER_LIB=na_discovery_diagnostics`; the production link includes the real consumer and `host_settings`. This DLL was not installed in a game.

The reduction in copies/scans is proved. The in-game milliseconds/FPS benefit is not measured. It depends on submission/binding volume and mapped-memory behavior; this report does not add it to unrelated GPU savings or promise that it closes the remaining frame-time gap.

## Broader areas inspected, without unsupported rewrites

| Area | Evidence and disposition |
| --- | --- |
| Descriptor batching | Saved PID10376 has348,061,964 deferred writes in341,106,905 applied batches, about1.02 writes/batch. `RegistryLock` flushes pending descriptors even for registry reads unrelated to descriptor contents. This is a genuine lost batching opportunity worth targeting within existing ownership/visibility rules. The counters alone do not establish which earlier flushes can be removed or the render-critical cost. Do not bypass cross-thread descriptor publication indiscriminately. |
| Transport wakeups | The quarter-full condition currently wakes on every following event above that threshold, and overflow notifications repeat while a loss mark is pending. A coalescing opportunity exists under backlog. No scheduling change in this patch: event delivery latency and stop/resync ordering need to remain intact. |
| Scene-read filtering | Saved PID10376 skipped1,610,310,064 descriptor walks and performed2,568,383, with the filter ready and no overflow. The filter is working; removing it would increase work. |
| Flow input pyramids/full-image passes | The explicit previous/current pair and submitted recording lifetime explain the repeated image work. Reusing a pyramid purely by CPU frame number would be incorrect. Prior measured whole AMD stage is about1.5–1.7ms, not an unexplained15ms saving. No half-resolution or image-quality tradeoff introduced. |
| NR retirement and readback | `Maintain` uses a try-lock; completed-flow collection checks recording retirement before mapping. No evidence of a hidden blocking GPU wait on that steady path. Existing submission lifetimes retained. |
| Main DLSS evaluation | Previously measured native3440x1440 presetM evaluation3.541ms remains intentional. Cort rejected changing the model to gain FPS. |
| Replay | Fable owns movers-first selection; his measured-game predecessor was the known10–12ms lever. This patch neither duplicates it nor claims its saving. |

This is another bounded improvement to the current machinery, not a claim the entire codebase is maximally optimized. Preserve Fable's next referee priority rather than building another performance framework.
