# Discovery consumer contract — reconstructed 2026-10

Task A2. This document reconstructs a working specification from surviving
source. It does **not** recreate the missing B-003…B-007 agreements or claim that
their original text, acceptance records, or test evidence survived. No host ABI,
production implementation, or shared core is changed here.

## 1. Authority and evidence

The frozen wire contract is [`host_observe.h`](../../host_dx12/host_observe.h),
ABI **0.1**. Its layout and meanings take precedence over conjectures about lost
documents. Source comments specify intended behavior; runtime coverage still
requires tests. A header declaring an event is not proof that all game calls
producing that event are hooked.

| Surviving evidence | What can be reconstructed | What remains unknown |
|---|---|---|
| ABI 0.1 header and host transport | Batch framing, loss, epoch/status and event vocabulary | Live producer coverage for a given game and interface version |
| B-003/B-004 | No complete agreement recovered; old worklog mentions shared menu coordination | Original requirements, acknowledgments and tests; do not invent them |
| `overlay/overlay.hpp`, `host_boot/game_adapter.h` (B-005 references) | Adapter startup phases, execution/discovery callbacks and callback lifetime | Any lost consumer implementations or stronger guarantees |
| `hooking/variant_registry.hpp`, `hooking/vtable_hook.hpp` (B-005/B-006 references) | Per-slot implementation coverage, bounded discovery, quiescence rules | Proof that a previously recorded command list was fully observed |
| `overlay/presenter.hpp` and backbuffer lifetime comments (B-007 references) | Swapchain replacement/lifetime precautions | Original review text and validation against all swapchain paths |

Read source references as context, not new authorization to deploy, mutate game
memory or enable a feature. Claude owns host producers and runtime behavior;
Codex owns discovery consumers. Missing agreements require fresh numbered
proposals and acknowledgment through COLLABORATION/COMMS before shared changes.

## 2. Separate three kinds of evidence

1. **Inventory:** which objects are known alive with which recorded properties.
2. **Execution history:** what bindings/work were recorded and submitted, under
   which proven ordering and coverage. A successful resync does not recover past
   history.
3. **Contents and semantics:** which bytes/textures were captured, when they were
   ready, and evidence that they mean scene color, depth, motion or camera.

An inventory-complete flag does not validate an input texture. Present is a
correlation boundary, not proof of frame ownership or GPU completion. Resource
final release is not the last GPU use. A plausible matrix or image is a candidate,
not a qualified input.

## 3. ABI 0.1 batch consumption

- Consume only the bytes valid during the serialized worker-thread sink callback;
  copy records into bounded consumer-owned storage if retaining them. H1 records
  contain opaque IDs, not retained GPU resource pointers.
- Validate major version, minimum header size, alignment, exact event count and
  exact payload consumption before publishing any derived result. Walk events
  by `size_bytes`, not a compile-time struct size. Read via bounded copies when
  alignment is not established.
- A known event smaller than its known structure, an event outside the payload,
  invalid size/alignment, or an inline array outside the event is malformed.
  Count the loss, discard that batch's candidate updates, and invalidate affected
  deductions. Validate offsets/strides/counts with overflow-safe arithmetic
  before dereferencing; a large count must never wrap into a small range.
- Unknown optional event kinds can be skipped using their declared sizes.
  Unknown **foundational** events make inventory-derived deductions incomplete.
  Appended minor-version fields may be skipped according to the frozen contract;
  a different major version requires an explicit decoder.
- `na_list_ref` is frozen. Inline arrays are located by their declared offset and
  stride, which may include extension space. Never assume they immediately follow
  the known struct.
- Batches may represent an older epoch while a status query reports a newer one.
  Keep their identities distinct; do not stamp buffered records with query-time
  state. All events in a batch must match its epoch.

The sink may query status, but may not call stop/resync. Schedule lifecycle work
on a control thread. Stop's FINAL delivery and no-more-callbacks guarantee are
the boundary for releasing sink storage. The host's batch allocation is never
owned by the consumer.

## 4. State, loss and recovery

Use `(session_id, epoch, object_id)` for epoch-scoped records. Keep command lists
keyed by `(list_id, recording_generation)` within that scope. Pointers, vtable
addresses, dimensions or frame counters alone are not stable identities.

| Trigger | Consumer action |
|---|---|
| New session | Discard old identities, mappings, candidates and temporal history |
| EPOCH_BEGIN | Start a new incomplete inventory; prior execution history is permanently unknown |
| INVENTORY_COMPLETE with complete=1 | Permit inventory use only for that session/epoch and observed capability scope, if no relevant loss occurred |
| complete=0, foundational loss, unknown foundational event | Keep inventory-derived inference invalid; request resync from a control thread |
| Detail loss | Invalidate affected pass/content/candidate evidence; do not mislabel it a complete capture |
| More records arriving after loss | Do not clear invalidity merely because traffic resumed |
| Resync succeeds | Restore current liveness only, never old descriptor versions, submissions, cameras or frame history |
| Resize/cut/provider change/recording gap | Clear corresponding temporal evidence and require fresh qualification |

Status counters are authoritative even when the event channel is saturated.
Track session/epoch and counter deltas coherently. A status counter increase
without a delivered loss mark still matters. Empty snapshot completion with no
registered producer is not discovery of an entire game. Capability reports and
coverage gaps remain mandatory.

## 5. Ordering and object facts

- Record-time descriptor writes/copies do not prove shader-visible contents at
  execution. Preserve heap/slot/version and the evidence scope. A later rewrite
  can invalidate a binding inference.
- Successful Reset increments recording generation. Failed Reset or unobserved
  creation/reset does not establish a usable recording. Mark gaps until a fully
  observed Reset-through-Close sequence is submitted through a covered path.
- Queue tickets identify occurrences. They prove same-queue order only outside
  overlapping hook windows and all retroactive ORDER_AMBIGUITY_RANGE records.
  Revoke dependent deductions when a later ambiguity range arrives.
- H1 game-fence waits have UNKNOWN/AMBIGUOUS provenance with zero source IDs.
  They prove no cross-queue completion edge. Threshold waits can be satisfied by
  unobserved CPU/external signals. Successful Signal/Wait submission is not GPU
  completion; failed calls establish no dependency.
- Bundles, unsupported interface methods, pre-existing objects and uncovered
  implementation variants produce explicit gaps, never synthetic events.
- Swapchain-buffer association requires actual observed GetBuffer evidence.
  Matching size/format is insufficient. Failed resize does not advance a proven
  buffer generation; chain replacement invalidates old associations.

## 6. The surviving in-process adapter seam is separate from the wire ABI

`na_adapter_prepare()` registers menu sections before overlay startup;
`na_adapter_start()` registers discovery/execution consumers after startup.
`na_adapter_stop()` requires a quiescent test/controlled shutdown, not unsafe
cleanup from PROCESS_DETACH. The link-time adapter interface is not ABI 0.1.

`SubscribeExecute` supplies the actual queue and list array, an occurrence ticket
and uncovered-list count. `begin` runs before the original call; a submission
receipt may be published only in `end`, after forwarding returns. Neither is a
GPU-completion callback. Pointers/arrays are callback-scoped; no CPU waits,
unbounded allocation, file I/O or synchronous GPU readback in these callbacks.

Discovery callbacks run on the discovery worker. Queued objects are AddRef'd
until callback return. Retaining one needs separately established ownership;
this does not grant access to its resources' future GPU contents. Overflow and
late subscription must remain visible. An object being fully covered *now* does
not establish coverage of its earlier recording generation.

The variant registry keys `(kind, slot, concrete function)` rather than vtable
address. Unknown/incomplete coverage, cross-slot conflict, thunk exhaustion,
queue overflow, scope ambiguity and unsupported bundles are acquisition gaps.
If stop/unsubscribe cannot prove quiescence, retain state and retry; do not free
callback data or re-register over active consumers. Avoid retaining real
swapchain backbuffers in a way that prevents ResizeBuffers or replacement.

## 7. Data requested from Claude for C7/C8/C9

These are **requirements for a new proposal**, not new accepted ABI structs.
Use existing H1 fields where sufficient. An additive H2/H3 minor-version proposal
must preserve all existing layouts and meanings. Changed semantics require the
major-version process in the frozen header.

### A2-R01 — C7: native-upscaler observations for A4

Provide provider/API/version identity; actual observed create/evaluate result;
frame/feature identity; input/output/subrect dimensions; resource identities;
motion/jitter conventions; exposure policy and format/encoding when known.
Represent missing keys individually. Distinguish module present, API called,
inputs validated, and current successful evaluations. A DLL name alone cannot
produce an active route. Include last-seen age, reset and release/recreation so
the overlay cannot display stale support. Supply evidence-only observations;
route ranking and user-facing interpretation belong to the consumer.

### A2-R02 — C8/H2: pass and resource observations for A5/A7

Required groups:

- PSO/shader identity from observed creation, content hashes and available shader
  stage/reflection metadata; root-signature identity and binding layout.
- Per-command-list recording operation index, generation, reset/close outcome,
  PSO/root signature, graphics/compute root bindings, descriptor-heap identity,
  RTV/DSV/SRV/UAV view identity, format, subresource/range and descriptor version.
- Draw/dispatch/copy/resolve/clear order and viewport/scissor/subrect where
  observed; barrier/state information including aliasing and known coverage of
  enhanced/split barriers. Expose unknown state instead of fabricating it.
- Root constants copied by value with exact range and binding identity. Root
  descriptors/CBV ranges must resolve to observed resource identity plus byte
  offset, or remain unknown; do not use a bare GPU virtual address as identity.
- Submission association with the actual queue and the same ambiguity/gap rules
  as H1. Flag bundles, ExecuteIndirect, bindless indexing, unobserved methods,
  GPU-written descriptors/data, descriptor races and self-generated host work.

Metadata alone cannot validate value distributions or HUD absence. A5/A7 also
need filtered, asynchronous **resource content samples** with explicit source
subresource, snapshot frame/epoch, format/row layout and completion/lifetime
evidence. Until a qualified snapshot path exists, report metadata candidates
only; do not claim a content-based classifier is ready.

### A2-R03 — C9/H3: content snapshots for A6 and qualification

For each bounded constant/upload snapshot, provide buffer identity/generation,
byte offset/length, binding range and stage, recording/submission association,
copy phase, snapshot ID, captured bytes, and validity/truncation/loss status.
Snapshot at the documented submission boundary from memory whose lifetime and
readability are established. Persistent mappings and application writes may
still race: mark **app-sync-unknown**, never claim an atomic or immutable camera
snapshot without evidence. A CPU snapshot of a GPU-written buffer is not its
execution-time contents.

For GPU texture samples, provide an asynchronous lease/token with source identity,
captured extent/subrect, format/plane, queue/completion association, readiness,
expiry/cancellation, ownership and explicit release. Producer and consumer must
agree who keeps the copy alive through GPU use and how device loss is reported.
Do not expose a live pointer plus a guessed Present delay as a completed sample.
Define finite byte/event/lease budgets and visible drop behavior. Skipping a busy
capture is preferable to blocking the game thread; the skipped frame remains
unknown rather than reusing stale contents under a new frame ID.

For future J1, observing a matrix does not authorize patching every occurrence of
its bytes. Mutation needs a separate contract covering main-camera identification,
aliasing/shared allocations, copy-on-write/rebinding where needed, synchronization,
shadow/UI exclusions, and restoration. C9 is an observation facility.

## 8. Candidate outputs and qualification

Consumer records should retain evidence references, identity scope, last-seen
frame, format/extent/subrect, conventions, confidence rationale, validity and
rejection reasons. A score is a ranking, not a calibrated probability unless
measured as such. Distinguish absent, unobserved, ambiguous, stale, estimated and
validated. Invalid motion must not become a valid zero/stationary vector.

The classifier may hypothesize roles; only separate qualification promotes them.
Qualification should compare matched fixture frames, include deliberately similar
decoy buffers, and measure precision plus discovery coverage/abstention across
held-out scenes. "No false positives" is an observed result on a defined test
set, not a universal guarantee. Transparent/multilayer pixels and disocclusions
need explicit truth/validity definitions rather than invented unique motion.

Minimum consumer regressions for subsequent implementation: malformed sizes and
arrays; unknown foundational kinds; loss without marks; delayed old-epoch batches;
counter/session changes; incomplete snapshots; retroactive ordering ambiguity;
descriptor rewrites; object/recording reuse; snapshot cancellation/expiry;
truncation and stale content; swapchain replacement; and invalid confidence
promotion. A1's host tests cover transport behavior, not this not-yet-built
consumer. A2 adds this specification only.
