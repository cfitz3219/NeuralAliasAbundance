# PostEntryPSO specification: assessment and implementation handoff

2026-10-27, Astra. Reference: Cort's supplied `p4_0d_universal_auto_router_postentrypso_specification.md` in Downloads. The document is design input, not independent proof of a solved live defect. Cort directs us to pause the prior capture-first correction approach, assess this design, and prepare its valid parts.

## Decision

Adopt retrospective learning of the actual post-entry consumer as the preferred inline anchor. It directly addresses sensitivity to variable intermediate readers and is a worthwhile replacement for ordinal-driven matching when the consumer is identifiable. Keep the existing capture capability for eventual verification; no new capture-first investigation or game deployment in this preparation step.

The document's claimed SER root cause remains plausible, not established. Run8 used producer PSO plus occurrence when available, not just transition ordinal. The 220/366 gap co-occurred with rejected candidates, but the old log did not record which matching predicate failed. The interval was not proven continuously visible gameplay. Neither culling as the cause nor an invariant SER post-entry PSO has been measured. Implement/test the proposed improvement without relabelling those hypotheses as facts.

## Necessary corrections (keep the design, correct the implementation)

1. **Hook ABI:** Windows SDK10.0.26100.0 `ID3D12GraphicsCommandList4Vtbl` places BeginRenderPass at68 and EndRenderPass at69.62/63 are wrong. Use the repository's existing offsetof slot assertions and query List4 availability before installing extension hooks. Standalone `tests/offline/renderpass_abi_test.cpp` verifies the actual SDK ABI.
2. **Prediction, not an invariant:** post-processing PSOs can vary with view/mode/settings and one PSO can serve multiple purposes. Neither 100% execution nor >95% game coverage is established. Match actual selected-resource use, consumer operation kind, live PSO identity/generation, root signature and view/subresource. A unique PSO in a completed trace is a learned predictor, not proof that the next trace cannot use it earlier.
3. **No retroactive repair:** Close-time learning cannot fix that recording. Update the next not-yet-recorded matching view's prediction; parallel recordings may already exist, so even 'next frame' needs qualification. A cold first frame needs observation. Test 2/2 coverage on TWO measured frames after a separate learning frame. On a previously unseen mode change, allow the explicitly recorded miss and verify adaptation; remove 'same-frame safety net / zero uncovered frames' wording.
4. **Close versus submit:** Close publishes an immutable candidate keyed by list recording generation. It does not establish GPU frame order or the final scene. Promote the candidate using submission/dependency order, not whichever worker thread closes last. A recording never submitted must not replace the active learned boundary.
5. **Between-list placement:** a producer and consumer in different lists can be in ONE ExecuteCommandLists batch. Appending DLAA after the entire batch is too late. Insert before the actual consumer using the existing qualified ordering machinery; if batch splitting is needed, preserve list/queue dependencies and account for resource-state decay. Do not infer a queue switch from a list split. Unrelated other-target draws are not by themselves proof that a scene boundary is unusable.
6. **Native-upscaler ownership:** lifetime counters >0 are not current-view ownership. Creation counts are not successful evaluation, and the injector's own NGX calls must not trigger stand-down. Use external evaluation events associated with the relevant view/frame/context; handle enable/disable without leaving J2 permanently suppressed. This completes the existing R4 integration task rather than adding another global counter heuristic.
7. **Copy/shared-PSO fallback:** copies have no consumer PSO. Distinguish copy/resolve operation and source/destination version/subresource; do not treat PSO0 as a unique anchor. Producer+consumer pairs can also repeat mid-scene. The fallback ordinal remains a fallback with known ambiguity, not a universal cure. Record why a fallback was chosen and reject a demonstrated ambiguous earlier match.
8. **Render passes:** observe Begin/End, beginning clears, ending resolves/discards and suspend/resume semantics needed by the selected image. Block this DLAA implementation's copy/dispatch insertion inside an open render pass. EndRenderPass does not restore old RTV/DSV bindings: Microsoft's contract leaves them undefined; clear current shadow bindings while retaining historical evidence. Don't merely toggle `in_render_pass=false` and keep stale attachments. Restoration must cover the supported trigger's graphics/compute state.
9. **Scope of taxonomy:** native interface / between-list / inline / unconfirmed are useful implementation routes, not an exhaustive theorem about DX12 engines. Cross-queue dependencies, multiple views, reused lists, UAV-produced scene color and unobserved bindless/indirect reads still affect what can be inferred. Preserve current explicit unknowns; don't implement every missing engine pattern before delivering this bounded change.

## Concrete production work for Opus

Keep ownership of `producers.*`, `insertion_trace.hpp`, `live_dlaa.*`, route selector, upscaler handlers and build wiring. Current fc4381b already includes PR50's record-first jitter and capture: no duplicate merge of old90144fc is needed.

1. Extend existing ordered trace with typed SceneWrite/ColorConsumed/ColorToRead/OtherTargetWrite events, operation index, selected resource/subresource, consumer PSO/root signature and graphics/compute/copy kind. Retain all relevant color writes, even without selected depth bound, so a late clear/composite cannot be silently ignored. Preserve the observed-vs-possible-read distinction.
2. Learn first observed consumer after final scene production, and whether that consumer PSO also appears among earlier scene-color readers. Learn actual consumer identity separately from barrier records. Current barrier candidates use UINT64_MAX as a pipeline sentinel; that must never become a PostEntryPSO. Copies carry operation identity, not the currently bound unrelated PSO.
3. Preferred match: selected image/view + compatible root/operation kind + unique learned consumer PSO. Secondary match: producer+consumer with demonstrated distinguishing context. Explicit fallback for copy/shared consumers. Match before forwarding the actual consuming draw/dispatch/copy, restore application state, retain one-evaluation/frame behavior and existing jitter scheduling.
4. Promote evidence in submission order. Keep bounded matching and next-recording adaptation; do not add a16-frame histogram wait to a known new consumer signature. Count cold learning, signature changes, ambiguous matches and actual matched mode separately.
5. Add correct List4 render-pass hooks and the scoped external-upscaler ownership event. Verify interface support and state semantics using SDK assertions/tests. No per-game hashes/config route requirement in the final automatic path.

## Astra's focused regression preparation

I own new `tests/offline/renderpass_abi_test.cpp` and the forthcoming consumer-anchor regressions; I will not edit Opus's existing `j2_anno_shape_test.cpp` concurrently. Agree the small observation/signature API before adding tests that call it; no dummy passing implementation.

| Case | Sequence and required result |
|---|---|
| Variable readers (primary) | Learning frame:3 water/refraction reads interleaved with scene writes, then distinct post PSO. Measured frames:3 then0 mid-scene readers, same final consumer. Both measured frames inject once before final consumer,0later scene writes. Real consuming draws, not binding-only surrogates. |
| Variable final producer | Final geometry PSO changes while post consumer stays the same. Unique-consumer route still matches; it must not accidentally retain the old producer requirement. |
| Shared consumer | Same consumer PSO occurs mid-scene and post-scene. Never classify unique; require distinguishing producer/context or declared fallback. Include repeated producer+consumer pair to expose ambiguity. |
| Copy-only | No shader PSO. Preserve ordered source/destination subresource identity and use explicit copy fallback, never UniquePostEntryPso. |
| Mode change | New post PSO cannot be retroactively injected. Learn from completed/submitted evidence and match a subsequent recording, accounting for the change frame honestly. |
| Parallel recording | Close order differs from submit order; abandoned/reset generation cannot override submitted-view learning. |
| Render pass | Correct SDK slots, selected attachments/clears observed, no injection inside pass, bindings undefined after End, insertion after a legal boundary. |
| Routing | Synthetic Anno-like split producer/consumer selects Between; SER-like same-list actual post consumer selects Inline. Same-batch producer+consumer verifies insertion between them, not after both. Current external FSR/XeSS/DLSS event owns its view; own NGX/old counters do not. |

These are targeted tests of this change, not proof of the games' live render graphs. After the primary case passes, use one combined SER build to verify coverage and whether the reported flicker disappears; expand only for a demonstrated remaining defect. Do not claim the entire symptom is fixed from synthetic tests alone.

## Sources checked

- Installed SDK: `C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/um/d3d12.h`, List4 vtable; executable offsetof checks in the test above.
- [Microsoft: Direct3D12 render passes](https://learn.microsoft.com/en-us/windows/win32/direct3d12/direct3d-12-render-passes): render-pass restrictions, attachment bindings and undefined bindings after End.
- [Microsoft: executing and synchronizing command lists](https://learn.microsoft.com/en-us/windows/win32/direct3d12/executing-and-synchronizing-command-lists): recording/submission and queue dependency model.
- Current source fc4381b and historical run8 report `docs/SER-FLICKER-CONTINUATION.md`.
