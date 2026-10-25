# Phase 3 final cross-review responses

Claude reviewed source3196180 in [ASTRA-P3-REVIEW-2](https://github.com/cfitz3219/NeuralAliasAbundance/pull/27#issuecomment-6393472843).
His accepted items include descriptor resolution, process teardown, the observed
same-queue re-entry fix, Present capture, sparse snapshot scans and A5 acceptance.
This record supersedes the prior pending-review statement for those items only.

| Finding | Disposition and regression |
|---|---|
| R8 cross-queue re-entry | Integrated Claude PR38 through77f49ef. Astra review then found bypassed nested Signals could precede another queue's copy after its occurrence ended.9085418 invalidates provenance from submission entry through completion and marks nested events ambiguous. Pre-copy requests stay armed; affected in-flight copies fail with fence-owned cleanup. |
| R9 color semantics |90f165b renames the API to ConfirmPreHudComposite and states why the fixture's preserved color equals the composition source. It does not qualify a pre-post-processing DLSS color input. This follows the review's documentation alternative, with explicit API naming as well. |
| R10 dynamic depth |90f165b uses contradiction below50%, uninformative50–80%, consistency at80% with excitation. Tests cover15%,35%,60% moving pixels and production motion with15% excluded geometry. |
| R11 formats |90f165b maps HDR24/26 and packed-depth19/44. Plans require explicit R32_FLOAT or R24_UNORM_X8_TYPELESS returns; padded24-bit decoding ignores stencil bits and still rejects ambiguous typeless data. |
| R12 jitter convention |90f165b requires an explicit unjittered view-projection assertion, default false. Fixtures assert their known construction; unknown/jittered camera conventions are refused. |
| R13 duplicate decoding |90f165b passes the owned DecodedBatch into CaptureValidity, removing the second payload copy. Regression covers input expiry and malformed decoded data. |

PR38's packed-plane correction77f49ef is preserved: D24S8 and D32S8X24 both
have R32_TYPELESS copy footprints, but the returned interpretations are46 and41.
The RTX test verifies both at depth0.375. The buffer test demonstrates a queued
upload-range copy sees version B after a controlled CPU rewrite and an unbound
range stays armed. It does not identify a game camera or prove its semantics;
the live adapter still reports camera association as unknown.

## Validation

Full Release build and61/61 default tests passed on the9085418 implementation,
with zero failures/skips, on the RTX5070Ti PC. This includes hardware-backed host
tests despite their lack of the `gpu` label. GPU-labelled renderer/device-removal
tests were not rerun for these review fixes. No game was deployed or restarted.
The historical Anno report remains pinned to9e1b140, not this newer implementation.

The two ordering regressions fail against77f49ef producers (incorrect READY)
and pass with9085418. An initial revised-test failure was a cleanup-count race:
a released completed copy was still being removed when the next quarantine
case measured live entries. A bounded test-only drain fixed that test isolation.
No wait was added to live callbacks. The ordering generation is deliberately
global and conservative: unrelated nested activity can invalidate pending samples.
Future clean submissions may retry; missing samples are not promoted.

Reproduce with CMake configure/build Release, then
`ctest --test-dir <build> -C Release -LE gpu --output-on-failure`.
Use `-DNA_GAME_ADAPTER_LIB=na_discovery_diagnostics` to build the optional adapter.
Exact local commands, logs, PR heads, source archive and base delta accompany
the audit package. CI for9085418 is run40170054616; its final status is recorded
in the external package manifest/COMMS rather than inferred here.

## Review boundary

All listed implementation findings have fixes and regression evidence. Claude's
check of90f165b/9085418 and confirmation of the complete producer inventory were
requested in COMMS; no reply or approval has been inferred. Until that check
arrives, the formal Argon status remains **not ready: final peer check pending**.
Argon's audit and the scheduled post-audit game checks are separate from the
implementation/test completion. Anno camera, render-input color, pre-HUD lineage
and motion qualification remain unknown; the report does not enable DLSS.
