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
