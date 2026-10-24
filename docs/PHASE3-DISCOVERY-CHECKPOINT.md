# Phase 3 discovery checkpoint — 2026-10-24

Cort requested autonomous continuation. This checkpoint takes independent discovery
work through A8 while live acquisition is developed. No phase/gate completion is
claimed, no game was deployed or controlled, and no frozen core/ABI change was made
by Astra. All PRs are drafts for integration/review.

| Task | PR / tested task head | Implemented | Outstanding |
|---|---|---|---|
| A5 | #21 b53ad53 | Metadata ranking, independent depth/color checks, 396 anonymous corpus decisions | H2 ingestion, qualified async texture acquisition, automatic scene/HUD boundaries and semantic accuracy |
| A6 | #22 e635290 | Bounded matrix hypotheses, layouts/depth conventions, baseline offset and discontinuity checks, 216 hypotheses/72 snapshots | H3 ingestion, independently qualified camera/jitter and broader conventions |
| A7 | #23 4341900 | Reprojection checks, jitter/scale conventions, visibility/ambiguity safeguards,160 corpus hypotheses | Live candidate discovery and independent visibility/exposure handling |
| A8 | #24 da205b6 | Bounded profile codec, atomic persistence, strict invalidation and reuse tests | Actual hashes/confirmed-layout provenance and runtime rebinding |

A5 corpus:396 expected decisions match, zero false Consistent results within supplied
independent fixture geometry/composition evidence. This is not >=95% end-to-end
universal discovery. A6 deliberately retains object-transform decoys as hypotheses.
A7:30 correct Consistent,90 wrong-sign/scale Contradicted,40 low-resolution Unavailable.
Initial motion test wrongly required a positive result with one stable pixel; fixed
the expectation and retained the sparse case. A8 second-launch reuse is a synthetic
cache-path test, not game-launch timing. None returns a trusted runtime input.

Consumer self-review corrected acceptance of UINT64_MAX unknown markers in identity,
version and ordering evidence. New regressions and task suites pass. Initial task-head
CI passed for A5/A6/A7 before this correction; final-head CI must be checked separately.

## Combined Windows build

Integration code snapshot fd85049 combines the four task branches and Claude's C8a
through3b6315f. Full Release build succeeded with VS18/MSVC19.51.36260.0, optional local
NVIDIA SDKs enabled. All19 selected na.* CPU/fixture/menu CTests passed, zero skips.
No full GPU/device-loss suite was run for this CPU integration checkpoint.
Evidence stays private: local/phase3-integration/build-final.log and
local/phase3-integration/cpu-ctest-final.log, plus per-task build/a5-cpu througha8-cpu.

## Host-observation test failure and diagnosis

Earlier combined snapshot5f04162 with C8a aaa1308 built successfully and passed18
na.* tests, but host_observe_producers_test failed on RTX5070Ti. Redirected output
reported0xc0000409. A terminal rerun exposed failing CreateCommandAllocator at test
line143. Windows' existing crash dump plus linker-map stack-address inspection
identified abort/terminate in the g_host static destructor, consistent with a fatal
test return leaving the worker joinable. This was not a full debugger stack unwind.

The triggering test call used a default SRV for the UNKNOWN-format buffer at line135.
A private diagnostic copy changed only that call to an explicit R32_UINT/BUFFER SRV,
1024 elements and default component mapping. Linked against the SAME producer,
transport and hooking libraries, it passed on RTX:26 hooks,0 optional missing,
12 live/13 created/1 retired,5 writes/1 copy,0 unresolved/0 sentinel failures,
1 execution,0 assertions. This confirms the isolated test fix, not qualification of
all producer synchronization/lifetime paths. The source owner received the exact fix
and RAII-stop request in COMMS; no Claude-owned tracked file was edited by Astra.

[Microsoft's default-SRV contract](https://learn.microsoft.com/en-us/windows/desktop/api/d3d12/nf-d3d12-id3d12device-createshaderresourceview)
requires a suitable inherited format. Private diagnostic source/script/CMake are under
local/phase3-integration. Original failure tool output is preserved in the chat and
COMMS; the shared CTest LastTest.log was later replaced by the successful19-test run.

Additional C8a source-level concerns posted to Claude: noncanonical interface-pointer
identity, failed-sentinel attachment behavior, concurrent first-registration/queue-state
lifetime and sync/execute overlap accounting. These are not claimed fixed or reproduced
by the descriptor experiment. H2/H3 remain unavailable at this checkpoint, so live
classifier integration and A9 Anno diagnostics cannot yet provide trustworthy results.

## Final CI confirmation

Final task-head CI completed successfully on all three jobs (Linux GCC, Windows MSVC,
Windows with public SDK headers): A5 b53ad53 run40089229958, A6 e635290 run40089230956,
A7 4341900 run40089233177, A8 da205b6 run40089173392. These runs cover individual task
branches, not the integrated C8a experiment. Read-only git merge-tree checks confirmed
all four task branches merge cleanly against mainc78a50e. No PR was merged.

## 2026-10-24 — newer host change and real RTX decoder check

Claude61ab0b3 arrived during checkpoint publication. Integration831d085 includes it; full Release build passes. Private explicit-SRV diagnostic passes RTX (26hooks,0missing,no shared slots). Adding PR25 DecodeBatch to that private test's Sink produces one failure: initial capability report session=0 while its batch has the actual nonzero session. The producer snapshot initializes no session field. Decoder correctly rejects that entire batch. Raw evidence local/phase3-integration/decoder-rtx-diagnostic.log; private diagnostic only, no host-owned committed edits. Sent exact trigger and fix request to Claude. The earlier20/20 CPU results remain valid, but do not cover this newly demonstrated real-producer mismatch.

Reviewed61ab's duplicate-hook-target guard: skipping the second install leaves the first global detour active at the same target, so incompatible second-slot calls can still reach it. Reported need for preflight/rollback and capability degradation. Not reproduced on this RTX (shared slots empty); distinct from the actual session mismatch. H2/H3 and existing lifetime/order findings remain open.

## Validity integration / latest host9655c85

Combined9cd8a76 merges Claude9655c85 and consumer1311ea0. Full Release builds;21/21 na.* discovery CPU/fixture/menu CTests pass (local/phase3-integration/build-validity.log and ctest-validity.log). CaptureValidity adds conservative authoritative-loss, inventory-boundary, capability and epoch checks; standalone12/12 pass. No runtime registration or qualified signal selection.

Committed RTX producer test still exits0xc0000409 after checkpoint4 with25hooks. Private explicit-SRV diagnostic plus actual decoder gets through all producer assertions but rejects initial cap.session0 versus nonzero batch.session. Logs ctest-host-9655.log and decoder-rtx-9655.log under local/phase3-integration. This new test confirms allocator-hook removal did not fix the previously isolated descriptor defect or session mismatch. Earlier source-level findings remain open; no host-owned source changed. New-head CI pending at this entry.

## Both reproduced defects fixed; real consumer round trip passes

Cort authorized direct correction and coordination. Fix1a81e1b (PR26; docs0701da3) corrects the buffer SRV, drains observation on early test returns and populates capability session from coherent status. Committed producer test now asserts startup/resync session equality and epoch progression. Combined df18a4e full Release and22/22 selected tests passed on RTX5070Ti, including the formerly failing producer test.

Added the actual producer-to-DecodeBatch-to-CaptureValidity test from PR25. Combined e08d2de builds and both hardware and explicit WARP variants pass, zero skips. It checks H2 PASS remains unavailable, resync scope and FINAL invalidation. No game operations. Remaining shared-target/lifetime/order findings and H2/H3/texture acquisition are not claimed complete. Exact fix commit delivered to Claude in main COMMS to avoid duplication. This supersedes earlier reproduced descriptor/session failures only for the corrected integration snapshot.

## Latest entry-hook integration and pending ordering reproduction

Merged Claude238f508 at1407803. The merge needed a semantic correction: retain the unobserved buffer with its explicit BUFFER SRV instead of combining Claude's new texture with that buffer descriptor. Session/RAII regression fixes remain. Full Release succeeds and default non-GPU suite40/40 passes at6fa529a, zero skips, including RTX producer and RTX/WARP consumers. Evidence local/phase3-integration/{build-entry-hooks.log,ctest-entry-hooks-full.log}.

Separately, new queue_overlap_probe (PR25 a692599) FAILS3 assertions on this producer. It opens a controlled host-owned Execute observation window and performs real Signal/Wait calls inside; execute1/signal2/wait3 all carry flags0 with no ambiguity range. This is the previously reported R3 sync-overlap gap now reproduced, not a regression in the fixed descriptor/session issues and not a GPU-order measurement. No rendering submitted. Diagnostic target builds; known-failing CTest is opt-in via NA_ENABLE_PENDING_CAPTURE_TESTS until Claude's producer fix arrives. Default40/40 and CI must not be presented as covering this pending test. Evidence queue-overlap-committed.log. Sent exact repro/ownership through main COMMS.

H2/H3 additions remain unpublished. Producer canonical identity, sentinel-attachment failure, registration/QueueState lifetime, unified queue overlap, entry-patch rollback/module lifetime/new-vtable coverage remain Claude's lane. Astra keeps consumer/regression integration. No game deployment or Phase3 closure.

## 2026-10-24 — queue fixed; H2/H3 structural integration

User-authorized queue fix3aa7745 (PR29) unifies Execute/Signal/Wait ambiguity finalization and retains queue state across the occurrence. Default regression passes; earlier pending/known-failing status is superseded on this branch. Portable concurrent/nested/delayed-publication cases pass. GCC test formatting correction152cf47 addresses initial Linux CI failure.

H2 b736b6a and H3 29e8647 are integrated with bounded structural consumers and real fixture wrappers. Full Release and46/46 default non-GPU-labelled tests pass, zero skips; log local/phase3-integration/ctest-h3-full.log. H2 incomplete tables/operation details and all APP_SYNC_UNKNOWN H3 snapshots remain incomplete evidence. C9-H3-REVIEW-1 identifies underlying Unmap/copy lifetime race, suppressed binding associations and provisional snapshot ordering; sent to Claude for producer fixes. No live classification, Phase3 closure or deployment claimed. Latest CI pending at publication.
