# Sniper depth provenance and camera evidence

## Concrete failure and repair

The actual 2ce5f17/status-18644 run has one observed game direct queue but many `ambiguous_queue_order` resets. Near the captured sequence it had over 25,000 learned depth substitutions. The outer game submission was still being observed when our DLSS/timing completion calls signalled private fences. Those calls were counted as nested game operations. They marked the **outer** occurrence ambiguous and erased its correctly observed depth transitions.

An offline real-DX12 regression reproduces both sides:

- Original unscoped timing notification: one ambiguous outer occurrence; carried depth becomes unknown.
- Existing small `submission_tracking.hpp` helper around private notifications: zero ambiguous occurrences; exact carried depth survives.
- A private host Wait also preserves the enclosing occurrence after the same host-work exclusion as Signal.
- A genuine nested game Signal remains ambiguous; it is not silently exempted.
- The same queue may consume its carried state. Another queue without ordering provenance is refused.

The helper is reused from Astra's earlier 9853a49. Only the private-completion scope and Wait exclusion are brought forward; none of that rejected build's placement/aging/flow/jitter changes are imported. Subscriber callbacks and actual game submission remain outside the private scopes. Every real Signal/Wait still executes. No new CPU/GPU wait is added.

The old learned-depth fallback is removed. Previously, 30 matching historical observations allowed an unknown frame to copy depth using an old assumed state. Preserving current provenance replaces that guess. The historical `depth_assumed` field stays zero for old Results parsers. In-recording transitions still have priority, and inherited inline state now requires the owning queue. A genuinely unknown state still needs acquisition; this repair does not claim to establish unobserved cross-queue dependencies.

## Evidence improvements

`j2_input_sources` counts **submitted evaluations**, distinguishing depth from local, inherited, provider-supplied and submitted-prefix evidence. Camera counts distinguish fresh reads, same-frame J1 reuse, still holds, moving holds, extrapolation and saved same-frame input, with last/max age. Existing camera selection, continuity thresholds and hold limits are unchanged. Last-loss evidence identifies resource/ring offset and learned-slot bounds, shape and inverse failures. A new full-flow fallback remains Opus's shared work.

Entry rejection logging distinguishes new/unidentified, learning and seen-mid-scene keys and saves the actual pipeline/root/destination/operation/barrier identity in the existing bounded last-miss packet. This corrects the uninformative generic “no candidate” reason; the placement admission/learning policy is unchanged. It will distinguish Opus's recurring approximately 40-second bursts without guessing another placement rule. The current logs cannot establish their cause. No new jitter or transparent-pass bypass is included.

Claude's b033474 flow-setting log addition is included unchanged, so the last recorded acceptance setting is available alongside the existing sample age.

## Actual game evidence already available

Saved capture `j2-20261103-032738`, PID 18644, is from the installed **previous 2ce5f17 build**. All 30 evaluations use compute entry PSO648/root8479461187647717055. Reusing the existing offline NGX-indicator classifier gives scene0/30, raw NGX output30/30 and final30/30. This supports delivery in those actual saved samples. They are sampled evaluations, not all presents, and do not establish foliage/jitter/flow quality or eliminate the reported periodic drops. Report: `local/se5-depth-evidence-20261103.json` and its CPU-only script.

## Verification scope

`depth_private_sync_test` uses real GPU timestamp completion, real queue Signal/Wait and actual depth transitions. It checks both the broken unscoped control and fixed production helper. Its first development version reported success but crashed during CRT teardown: it omitted the DLL's existing process-exit flag while persistent timing COM objects outlived the observer registry. The fixture now follows the actual DLL teardown contract and must exit cleanly; the failed exit is retained in the local journal.

The downstream direct/indirect/native-NGX fixtures now retain depth across every tested frame without a scene-list clear supplying fresh local state. They must deliver all17 checked downstream images, use inherited depth, make zero depth guesses and account for each submitted input source exactly once. Native NGX followed by the existing depth-debug pixel witness measures restoration/contribution, not neural reconstruction quality.

Build/test/installer receipts are appended to WORKLOG and COMMS. Cort owns game testing; no game deployment or control by Astra. The original baseline, previous 2ce package and their rollback paths remain available.
