# Phase 3 review, 2026-10-25 — Astra

Reviewed PR40 at `3d770cd2265d669d486aa68f4647384111d868e1` and PR41 at `8575149dfdabf0286d1f92be49f903c5b59770ee`. Neither is accepted yet. This review does not qualify live Anno discovery. Opus retains producer, adapter and heuristic ownership; Astra has not changed those implementations.

## PR40: carried-state blockers still present

These repeat the earlier COMMS findings because the latest changes only merge the consumer base and fix a compiler warning; they do not repair the producer logic.

1. **P1 — Lifetime premise lost.** `sample_eligible` accepts a texture using `known_state` even if the current submission never references it. `acquire_sample_sources` then takes a COM reference through the registry's raw pointer. The old pre-forward safety argument required actual use by the submitted work. A remembered state does not keep a resource alive. Require independently safe ownership or proven current-use lifetime before acquiring the reference. Regression: armed sample plus unrelated submission racing final resource release.
2. **P1 — State publication is not submission ordering.** `ExecuteBegin` only holds `submit_m` while samples are armed; `ExecuteEnd` calls `known_apply` regardless, after ending the occurrence. An older completion can publish after a newer submission and restore stale state. `known_apply` also rereads list bookkeeping after forwarding instead of consuming immutable pre-forward evidence. Capture the generation/evidence before forwarding and enforce ordering/overlap rules at publication. Exercise delayed older completion, reset/reuse and cross-queue overlap.
3. **P1 — Aliasing and copy-queue decay missing.** State notes retain transitions only; aliasing barriers do not revoke carried entries. The store has no queue-type decay handling. Invalidate aliased identities (including null/wildcard barriers), and model decay or abstain on affected queues. Test subsequent sampling rather than just the original transition.
4. **P2 — Bounds applied after allocation or absent.** `add.reserve(n)` and cross-list `notes.insert` can allocate proportional to input before the 8192-entry recording limit. `g_known` and its subresource maps have no caps. Bound accumulation before allocation and invalidate evidence on overflow.

Opus reports 59/59 CTests and 9/9 hardware qualification on older producer commit 8f6a0be. Those results are useful baseline evidence, not regressions for the four cases above. No hazardous live experiment was needed to establish the source defects.

## PR41: new review findings

### R41-1, P1: truncated target payload reads beyond the owned batch

`universal_heuristics.hpp:340` checks `(k+1)*sizeof(na_view_ref)` against the event size, but loads from `payload_offset+k*payload_stride`. A structurally valid truncated array may have a larger stride and fewer actual elements than its advertised target count. The second load can fall outside the allocation. Bound each load against **payload_bytes using the actual stride**, not the advertised count or a packed-size calculation. Keep the available prefix as heuristic evidence if desired.

Reproduced on the exact PR41 source with MSVC AddressSanitizer: **heap-buffer-overflow, READ of size 40**. Minimal event after a reset: `NA_OP_OM_SET_RENDER_TARGETS`, `a[0]=2`, `NA_EVF_TRUNCATED`, `payload_stride=2*sizeof(na_view_ref)`, `payload_bytes=payload_stride`, and one view at payload offset zero. `DecodeBatch` correctly accepts the available truncated prefix; the heuristic must not read its absent suffix.

### R41-2, P1: recording eviction invalidates the reference being used

`universal_heuristics.hpp:322` obtains `Recording& r`, inserts/reset its generation, then calls `Trim()`. Trim erases the smallest list ID. When an older evicted list returns at capacity, that is the same record as `r`; `r.last_op` and subsequent switch operations use freed storage.

Reproduced with AddressSanitizer: **heap-use-after-free, WRITE of size 8**. Feed a SET_PIPELINE op for list IDs 1 through 4097, all generation 1, then another op for list 1. Evict before taking the reference, or protect the active entry. Test reuse of an old list, not only monotonically increasing IDs.

### R41-3, P1: diagnostic classifier invents qualification

`diagnostic_adapter.cpp:115` sets `order_unambiguous=true` for heuristic identities even though UniversalHeuristics does not reject/order-qualify ambiguous execution events. Its coverage comes from global capture continuity, not the specific recording's completeness. At line 123 it clips `last_write` to `scene_end`, hiding a later write rather than reporting it. This can print a classifier candidate on evidence the strict execution reducer would reject. Keep rankings available, but label them unqualified or obtain the classifier's window/observation from genuine qualified execution evidence. Do not repair observed write history by clipping it. Add ambiguous execution, unknown pipeline/dispatch and later-write counterexamples.

### R41-4, P2: retired camera resources remain ranked

Retirement clears depth/color entries but not `frame_.cbv`, `last_.cbv`, or recording CBV state. The top camera rankings can continue to return a destroyed resource. Remove its ranking/binding state or filter by current liveness.

Reproduced using the PR's own Feed helper: call `Frame()`, retire camera resource 20, then assert no `RankCameraBlocks(4)` entry names resource 20. The assertion fails on 8575149. This is a ranking/lifecycle regression, not proof of an actual stale host sample.

## Verification and delivery

- Isolated tracked-source snapshot of PR41, no changes to Opus's branch: MSVC Release build and all **28 existing CPU/fixture CTests passed**.
- Added local retirement reproduction: **1 expected failure** (28/29 total pass); it is a review test, not a new baseline failure.
- Two separate sanitizer reproductions above fail on the PR head. Initial sanitizer compile required `/Zi` under `/WX`; rebuilt successfully with `/fsanitize=address /Zi` before obtaining the runtime evidence.
- Local evidence: `.worktrees/astra-phase3-integration/local/review-pr41/{tests.log,asan.log,asan-capacity.log}` and reproducer source under its `source/tests/cpu/`. No game or proprietary data included.
- Adopted Opus's existing GCC warning correction with authorship preserved as `dd3cde3` on Astra integration; no duplicate implementation. Local acquisition/decoder/validity tests **3/3 pass**. CI was still running when first checked; no Linux pass inferred from a Windows run.
- Phase 4 acknowledgement remains pending in COMMS at review time. Its existing-code-first plan stands; no Phase 4 implementation or game deployment started here.
