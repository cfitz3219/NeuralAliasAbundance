# A5 signal discovery — first implementation

`signal_candidates.hpp` implements portable metadata ranking for depth and
pre-HUD scene color. This is the first A5 slice, not A5 acceptance or live
discovery. It has no host connection, GPU resource ownership or DLSS submission.
The structures are consumer-local C++ types, not additions to frozen ABI 0.1.

Use `Rank` on the discovery worker with at most 256 observations for one fully
observed command-list recording/submission window. Larger sets return an explicit
capacity failure, not a potentially misleading truncated ranking. Callers must
retain that failure and acquire a narrower window. Do not use in live callbacks:
the diagnostic result allocates strings and vectors.

Input identities and content versions must come from actual observations. The
scene/HUD boundaries are **inputs requiring independent evidence**, not inferred
by this implementation. Neither Present nor matching dimensions supplies them.
Only intact resource/subresource content versions at the scene boundary are
eligible. Late writes require an earlier preserved snapshot; setting a prior
last-write index while reading the later contents would violate this contract.

Every observation receives reasons and one of two confidence states:

- `Unavailable`: insufficient, mismatched, lost, ambiguous or incompatible
  metadata. Score zero.
- `Candidate`: plausible acquisition target. The integer acquisition score is
  a heuristic ordering, not a probability or content confidence. Even a unique
  highest score supplies no selected/qualified resource.

Extent/write role, format, write position, depth clear endpoints and a known HUD
boundary contribute to ranking. Unknown clear/HUD facts add no score. Equal top
scores report ambiguity; duplicate content identities invalidate all duplicates
until the producer normalizes them. Distinct subresources/content versions are
distinct candidates. No state survives a call: callers must rebuild on loss,
resync, resize and scene changes instead of retaining old results.

Remaining A5 work:

1. C8 H2 ingestion after an acknowledged host proposal, preserving A2-R02
   recording, coverage, order and descriptor-version semantics.
2. Completed asynchronous content snapshots with lifetime/subresource/frame
   evidence. H2 metadata and H3 upload bytes alone do not provide texture samples.
3. Content distributions, independent camera/depth consistency and HUD exclusion
   verification. Unknowns remain acquisition tasks, not a permanent stop to discovery.
4. Fixture v2 candidate corpus including shadow maps, history, UI/composite,
   postprocess and ambiguous decoys. Truth labels belong only in the scorer, not
   classifier inputs. Measure >=95% correct classification and no confidently
   wrong selections; current metadata tests do not claim this acceptance result.
5. Integrate diagnostics, then qualify Anno and a second DLSS-less game at Gate 3.

Validation: standalone `tests/cpu` CMake build supports these regressions without
running device-removal/GPU tests. `na.cpu.signal_candidates` covers stale scope,
loss, ambiguity, overwrites, extent/format mismatches, unknown values, duplicate
identity, capacity failure, color HUD uncertainty and ranking without promotion.
Existing A3 reference tests remain the oracle's own tests, not A5 accuracy tests.
