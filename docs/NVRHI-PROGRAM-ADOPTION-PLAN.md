# NVRHI across NeuralAlias: proposed adoption plan

Author: Astra. 2026-11-05. Revision 1, for Opus review and Cort/Argon audit. Planning only; no runtime changes authorized by this document. Existing Donut implementation is 6984089; its binding-cache changes borrowed a pattern, not the NVRHI library.

## Objective

Use NVRHI as extensively as practical for GPU work NeuralAlias owns, while preserving our interception, discovery, frame identity, reconstruction, DLSS/NR/FG integrations and user interface. The intended result is one shared GPU services layer used throughout the product, not an isolated enhancement dependency.

Concrete benefits to establish: less duplicated GPU infrastructure; fewer allocation/descriptor updates in warm frames; reliable toggle/resize/resource retirement; consistent queue/state handling; easier addition of effects and future graphics backends. These are hypotheses to measure, not promised FPS gains or automatic game compatibility.

Primary references: [NVRHI pinned source](https://github.com/NVIDIA-RTX/NVRHI/tree/bf0bbd9b666a7297492beb1999a4ddc0dfded143), [programming guide](https://github.com/NVIDIA-RTX/NVRHI/blob/bf0bbd9b666a7297492beb1999a4ddc0dfded143/doc/ProgrammingGuide.md), [Donut](https://github.com/NVIDIA-RTX/Donut). Verify APIs against this pin during the first stage; update deliberately if required. NVRHI is MIT licensed; preserve notices.

## What remains our core

Keep DXGI/NGX/Streamline/FSR/XeSS hooks, game-facing adapters, live quality controls, input discovery, camera/jitter reconstruction, flow fusion, HUD detection/composition decisions, real/generated-frame identity and model/backend selection. Preserve shader algorithms unless an independently justified change is approved. Retain the visible overlay and installation/rollback behaviour.

NVRHI would execute and manage our own GPU stages below these decisions. It cannot observe arbitrary game commands automatically, identify a HUD, obtain missing motion, fix incorrect jitter, or supply DX11 interception simply by being linked. Existing observer data supplies the entry/exit contracts.

## Adoption map: maximum useful coverage

| Existing area | Proposed use | Expected contribution |
|---|---|---|
| Shared texture/buffer allocation | NVRHI resource handles and a common bounded reuse policy | Remove independent allocation code; account for cached/in-flight memory and trim idle allocations after resize/disable. Pooling policy remains explicit; handles alone are not a pool. |
| Descriptor heaps/views/bindings | Immutable binding layouts/sets, cached by actual resource/view identity | Reduce repeated descriptor work and unify binding invalidation across effects. Do not replace interception of descriptors created by the game. |
| Per-frame parameters | Push constants for small payloads; volatile constant buffers for larger ones | Avoid overwriting data still referenced by unfinished GPU work. Set version capacity from real recording pressure, not an arbitrary frame count. |
| Owned command recording | NVRHI pipelines/dispatches, resource tracking and native interop where supported | Share the same transition, binding and recording implementation across features. Inline insertion is a separate qualification case. |
| GPU lifetime and retirement | Resource references combined with existing submission receipts/fences | Reliable toggle, resize, shutdown and repeated-list handling. Exactly one service releases submitted payloads; no competing retirement authorities. |
| Donut/XeGTAO | First real NVRHI consumers, then upstream Donut passes where suitable | Exercise compute/graphics, histogram history, scratch reuse and resizing. Preserve the distinction between scene grading and final display tone mapping. |
| RCAS/NIS, colour conversion, depth preparation | Move dispatch/binding/resource plumbing into the shared layer | Keep existing mathematics and remove duplicate host infrastructure. |
| AMD/NVIDIA optical flow and motion synthesis | Wrap owned textures and preparatory/fusion passes; keep vendor entry points native | Consistent resource reuse and interop. Do not rewrite the algorithms or assume NVRHI understands vendor work. |
| DLSS/DLAA and NR | Native texture/list handoff around vendor evaluation, shared scratch and retirement | Consolidate staging, format/subrect handling and output delivery without replacing NGX/NR integration. |
| Frame Generation and presentation | Adopt shared GPU utilities inside existing real/generated scheduling and composition | Consistent composition/resource management. Preserve scheduler, swapchain ownership, real-frame identity and generated-frame deduplication. |
| HUD composition and Home overlay | Adopt owned rendering utilities after earlier paths qualify | Reuse GPU services while keeping HUD detection and input/font/UI behaviour intact. Avoid two independent ImGui backends. |
| Capture, timing and developer tests | Common asynchronous staging/timestamp utilities where supported | Better feature-cost evidence and consistent readback retirement. No synchronous diagnostic readbacks in live callbacks. |

## Required seam, before migrating callers

Define a small internal GPU context carrying device, proven queue or explicit unknown, recording identity/generation, view and real-frame identity, texture/subresource/format/encoding/extents, entry states and required exit states. Existing services supply these facts; do not invent another discovery/frame tracker. Preserve the frozen core ABI unless a separately numbered shared-interface change is agreed.

For host-owned lists, NVRHI may manage their recording and submission through the established forwarder. For inline game-owned lists, first prove the pinned API can safely record into/adopt that list and preserve/restores its necessary state. Merely exposing NVRHI's own native list does not prove the reverse operation is supported.

If inline adoption is unavailable, retain that native recording seam and share allocations, views, shaders and retention where supported. A separately submitted list cannot replace inline work when that changes ordering. Do not force a whole-renderer conversion to obtain superficial coverage.

Importing game resources requires compatible views and valid entry states; export native handles for NGX/vendor calls. Bracket external GPU work with correct state reconciliation. Enhanced/split barriers, mip/array/depth-plane state and aliasing remain explicit. Do not let both trackers independently assume ownership of the same transitions. Restore required states and bindings at the game boundary.

Queue identity is an execution fact, not the last observed overlay queue. Cross-queue transfers need a real dependency. Unknown queues use independent scratch or existing qualified native handling. Replayed/executable command lists retain their resources through every actual submission and retirement, not one completion or a Present count.

## Staged implementation and ownership proposal

1. **Inventory and interoperability spike — Astra, Opus reviews boundaries (roughly 1–2 engineering days).** Map duplicated allocation/binding/upload/retirement code in effects, J2, dlss_backend, nr_pass/native_fg, flow and presenter. Record actual costs/memory and which lists/queues each owns. Compile a minimal device/resource import and native SDK handoff. Exercise owned and game-owned-list cases, restores, external barriers and repeated submission. Deliver a capability table of proven direct adoption versus retained native seams. Stop speculative rewrites if this exposes a limitation; finish useful shared services instead.
2. **Shared services plus first real consumers — Astra (roughly 2–4 days).** Add a thin NVRHI-backed internal GPU service, then move Donut and XeGTAO onto it. Include allocation/binding caching, constants, bounded memory, shutdown and resize. Remove superseded local machinery only after equivalent checks pass. This must use the library, not merely imitate another pattern.
3. **Broad effect/motion migration — proposed Opus: RCAS/NIS, conversions, depth and AMD flow dispatch plumbing; Astra: interop/retirement and NVIDIA flow seam (roughly 3–5 days combined).** Preserve numerical results and flow acceptance policy. Publish exact per-file ownership in COMMS before edits. Reuse existing tests and consumers; no duplicate service per agent.
4. **Native DLSS/NR and composition — proposed Astra: vendor handoff/NR lifetimes; Opus: J2/HUD/presenter consumers (roughly 3–5 days combined).** Consolidate shared staging and dispatch services while preserving model calls, frame identity, placement and scheduling. Qualify native FG interaction and repeated evaluations separately. Explicitly decide which inline native seams stay.
5. **Overlay/capture/timing and consolidation — split after inventory (roughly 1–3 days).** Adopt remaining suitable GPU services. Delete duplicate allocators/binding/upload code after all consumers have moved. Produce one coverage/memory/cost report listing retained native seams and their technical reasons.

These are preliminary engineering ranges, not a deadline; some stages overlap. Opus should revise them after the interoperability spike. No user attendance is required for implementation and automated checks. Cort retains scheduled live game acceptance. Do not start implementation just because this plan is posted: obtain Opus's review and let Cort audit the revised plan first.

## Evidence and acceptance

- Capture baseline versus candidate CPU frame time, GPU time per stage, allocation/descriptor counts and peak/cached/in-flight memory on identical controlled workloads. Separate summed thread work from frame critical-path time. Report cold and warm behaviour and disabled-feature overhead. No invented percentage target or universal FPS claim.
- Preserve existing numerical/pixel tests for motion, jitter, RCAS, depth, subrects, HDR/alpha, HUD composition and vendor inputs. Test actual GPU output where relevant, not only resource creation.
- Stress recording ahead, delayed completion, discard/reset before submission, repeated submissions, two queues with actual dependencies, resize/quality changes, toggle cycles and device loss/shutdown. No resource reuse before completion, no game-image overwrite where only a private override is intended, no new callback waits/readbacks.
- Test external SDK work changing state between NVRHI stages and exact game-state restoration. Keep fake-model/fixture proof distinct from real model and real-game evidence.
- Each stage builds and runs affected automated checks before integration. Run the full required suite at integration milestones. Package through the existing reversible additive installer; no native game DLL replacement. Existing working path remains recoverable during migration, then remove duplicate implementations once the new path qualifies.
- Final success means the shared services support the major GPU consumers above, with one clear ownership model and demonstrated maintenance/performance/stability benefits. An isolated Donut wrapper does not meet this plan. A particular NVRHI component that adds overhead or cannot preserve ordering may remain native, with evidence and a documented alternative; maximum useful coverage is the objective, not library coverage at any cost.

## Questions for Opus's review

Please review this exact revision and post corrections in COMMS: (1) Does the pinned API support our two command-list ownership modes? Cite the exact mechanism or limitation. (2) Which existing abstractions already provide these services and should become the integration seam? (3) Where could tracker/retirement duplication introduce races? (4) Which consumers give the largest concrete benefit first, and which are unsuitable? (5) Correct ownership, estimates and missing acceptance cases. (6) Challenge any proposal that is just a superficial wrapper, changes image algorithms unnecessarily or replaces the core. Return a bounded corrected plan for Cort/Argon, not implementation started in parallel.
