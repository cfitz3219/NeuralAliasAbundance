# Queue ordering correction journal

## 2026-10-24 — user-authorized R3 fix

Cort directed Astra to fix queue ordering; ownership posted to Claude in COMMS before editing. Patch3aa7745 unifies Execute, Signal and Wait in QueueOccurrences. A short per-queue mutex makes ticket allocation, active-window overlap generation and exit decisions atomic relative to one another. It never spans the original D3D12 call, emission or a GPU wait. Every event is published only after finalization, so delayed sink delivery cannot expose a provisional clean flag; retroactive ranges are unnecessary for these producer paths. Each internal token must be ended exactly once. Failed sync HRESULTs are retained and do not create dependencies; this patch does not correlate game-fence waits across queues.

Tokens retain shared queue bookkeeping and the entry-time object ID until End, avoiding raw state lifetime/re-lookup hazards. queue_state first-use registration is serialized, so simultaneous lookups cannot replace one another's state/sentinel. This is not a general canonical COM identity, sentinel attachment or all-object registration fix; those remain separately tracked. No frozen wire layout change.

The formerly failing real overlap probe now passes and is enabled by default; pending switch removed. Six portable algorithm regressions cover isolated/nested/crossing windows, delayed publication and concurrent boundaries. The kind-independent tracker is shared by all three real hook paths. Actual failed D3D12 calls and device-loss recovery were not induced in this test.

Pre-H2 combined Release5/5 focused tests passed. After merging Claude H2+b736b6a (conflict resolved preserving list discovery and new finalization),6/6 targeted tests pass including real H2 fixture decoding. Full combined Release44/44 default non-GPU-labelled CTests pass, zero skips. Logs local/phase3-integration/build-queue-fix.log, ctest-queue-fix.log, build-h2-final.log, ctest-h2-full.log. No game deployment. Exact patch delivered to Claude for cherry-pick; H2 consumer additions live in combined PR27/consumer PR25.
