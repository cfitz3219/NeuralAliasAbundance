# Universal DX12 audit — 6 November 2026

This is an engineering audit of the combined shipping source, not a claim that
every DX12 game has been qualified. Cort authorized source cleanup, efficiency
repairs and automated tests, retained game testing, and explicitly required that
today's work be preserved. No game was launched, controlled, installed into or
modified during this audit. No subagents were used.

Baseline: `8edb0bc`, the existing SE5 native-scale package. Cleanup increment:
`f377eba`. The combined candidate also preserves Opus's production work through
`0f0640e`, merged as `4e5ae8f`, including his draw-replay/motion changes, used-SRV
check, native retry/thread fixes and diagnostics. The original package remains
identifiable. Coordination is append-only on main COMMS.

## What I changed

### Observation delivery: remove repeated copying and allocation

`host_observe.cpp` gathered events into a vector, delivered its first 64 KiB,
then erased that prefix. Every erase moved the entire remaining backlog. A
3 MiB backlog split into 48 batches therefore incurred roughly 70 MiB of
unnecessary shifting in addition to the useful copies. Under sustained load
this competed with recording threads and delayed the discovery consumer.

The worker now advances a cursor and clears the gathered vector after consuming
it. It retains payload/batch capacity, writes loss markers directly into the
payload, and removes the unused old shared-byte-counter field. Per-thread rings,
epoch barriers, validity publication, loss accounting, sink reentrancy and final
shutdown delivery are unchanged. No new lock or allocation enters the producer
hook. A new real-transport test queues 3 MiB across 16 producers per burst, with
a resync between bursts, and checks all 96 event identities, producer order,
epoch order, zero losses and exactly one final delivery.

### AMD optical flow: reuse fixed bindings

`ffx_flow.cpp` recreated 64 null descriptors for every pass, followed by the
pass's real bindings. The explicit-pair graph has 28 passes, so this produced
almost 1,900 view creations per pair even though scratch resources and most
bindings were unchanged.

The fixed tables are now prepared once in non-shader-visible CPU storage.
Existing executable-recording heaps receive copies on first use; completed
heaps can reuse the same fixed tables. A private-data stamp includes a unique
PairFlow instance identity and initialized slot bits, so a destroyed/replaced
heap or a different scratch owner cannot inherit another owner's bindings.
Only the two current/previous image SRVs vary on warm reuse. Descriptor heaps
and input resources still follow the existing recording lease and GPU fences.
AMD's search shaders, flow units, acceptance rules and GPU pass sequence were
not changed by this optimization.

Microsoft requires descriptor-copy sources to be non-shader-visible; the new
templates obey that rule. See [CopyDescriptorsSimple](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-copydescriptorssimple).

The expanded GPU test covers forward, reverse and static pairs, fresh/reused
recording heaps, a different PairFlow owner reusing that heap, return to the old
owner, and discarded recordings. Default tests enable the DX12 debug layer and
reject error messages. The optional CPU recording benchmark deliberately
discards commands and has no GPU wait in its timed region. On this RTX 5070 Ti,
the initial 100-sample comparison was 617.51 us mean / 784.80 us p95 before,
528.99 us mean / 582.10 us p95 after. That is about 14% less CPU recording time
in this component, not a game FPS measurement. GPU flow cost remains.

An initial new-test crash was in my conditional use of WRL ComPtr's overloaded
address operator. The faulting instruction and fixture were checked; the test
now uses `std::addressof` for the optional ComPtr pointer. It was not evidence
of a pre-existing game/runtime crash and is not counted as a product fix.

### Delete the abandoned NIS sharpening stage

Removed `nis_sharpen.cpp/.hpp`, the `RecordNisForTests` wrapper/global/shutdown
code, and its test/target. The header already described this as reference/test
code. No live slider called it: RCAS replaced the old stacked sharpening stage.
This removes 501 lines in those three deleted files plus their wiring, shader
source text and obsolete test surface. This is a source/maintenance cleanup, not
a claimed live GPU performance gain from a pass that was already unused.

The separate NIS spatial scaler and its upstream notices remain: native-picture
and HUD paths still use them. RCAS, XeGTAO, Donut, optical flow, native scale,
camera/jitter, motion, NR/FG and today's installer machinery are retained.

### Native producer identity across Present

The native bridge saved a producer camera/depth-state payload at Close, stamped
it with the CPU Present counter, and rejected it if a preceding picture's
Present occurred before the separate upscale list was recorded. This is a real
render-ahead ordering, not evidence that the scene inputs disappeared.

Saved native producers now carry an exact recording identity, a monotonically
unique source serial and the existing recording lease. The producer retains
its source resources. A discarded, unsubmitted recording cannot qualify;
submission commits consumption of that serial, so another Present alone
cannot make an old producer fresh again. Merged recordings that already own
reconstruction do not publish a second unused producer. The bridge retains
Opus's actual-used-SRV requirement and imports state only from its matching
producer payload; contrary current state still wins.

The GPU fixture now closes the scene before the preceding Present and records
the separate upscale afterward, for both an unsubmitted producer included in
the eventual batch and an already-submitted producer. Reintroducing the old
Present equality check makes both separate graphics and compute regressions
fail. Restoring the new path passes, retains later HUD pixels, and returns to
DLAA. These are real DX12 commands with a fake NGX model; they do not establish
live-game picture quality.

### Native restoration and current confirmation

Pausing during an inhibited native size update used to restore only the staged
cache, clear ownership, and never retry the still-reduced renderer. Restoration
now remains pending, reconciles the staged cache with the committed scale, and
retries on the game's native update thread, using Opus's bounded retry policy.
Both committed dimensions and the game's scale/upscaler cache must agree before
the status reports restoration. The original effective native scale is retained
when a native upscaler overrides the scalar. If the user changes that native
upscaler to a new, unrecognized effective ratio during control, the code reports
pending until an actual commit can be distinguished; it does not invent a ratio
or call a cache write success. Tests cover inhibited restoration with both zero
and nonzero native-upscaler selections.

The green native-DLSS confirmation is recomputed from each fresh submitted
evaluation. A missed, repeated or dimension-mismatched evaluation clears the
current confirmation instead of leaving an old success on screen indefinitely.
This changes reporting, not the user's enable setting.

### Complete a missing public DXGI forwarder

Added `DXGIDisableVBlankVirtualization` to the proxy's public exports. A Windows
11 application importing it previously could fail before injection began. The
implementation forwards to system DXGI only when the application calls it;
older systems return a missing-function HRESULT. NeuralAlias does not call it
to change refresh behavior. Existing proxy loading/factory/present/resize tests
also check the export. See [Microsoft's function contract](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_6/nf-dxgi1_6-dxgidisablevblankvirtualization).

### Correct the generic frame converter's colour contract

The `FromFrame` helper rejected all sRGB SDR inputs as unsupported, while sending
linear SDR through NGX's LDR mode. NVIDIA's guide explicitly accepts gamma/sRGB
LDR, and warns against linear colour in LDR processing. The converter now accepts
sRGB SDR with IsHDR clear, and selects linear processing for both linear HDR and
linear SDR. Unknown encoding remains unknown. Regressions cover all three known
encodings and unknown encoding, without changing the frozen FrameContract.
See [NVIDIA's DLSS programming guide, colour input requirements](https://raw.githubusercontent.com/NVIDIA/DLSS/main/doc/DLSS_Programming_Guide_Release.pdf).
There are currently no shipping call sites of this helper outside its tests;
J2/interceptors build EvalDesc directly. This fixes the reusable conversion API,
not a claimed explanation for the current game's softness or a live quality win.

## What still prevents an arbitrary-game promise

The shared DLSS backend is reusable. The hard part is acquiring the correct
inputs, changing the actual native render size, and returning the enhanced image
at the right point. Those are separate compatibility conditions. None requires
replacing our core; a universal installer needs to select the supported route
and describe the actual result honestly.

| Area | Concrete limitation in this build | Practical next improvement using existing machinery |
|---|---|---|
| Native DLSS quality control | An NGX/Streamline answer changes actual resolution only when the engine consumes it. Cached sizes and engine-owned tables still need a native settings adapter. Unreal/Ghost/Sniper locators are reusable families, not proof for every engine. The old TLOU adapter remains exact-build-qualified. | Extend the existing named-settings/native dispatch adapters when a game supplies no live generic query. Confirm actual submitted input/output; keep unsupported-control reasons distinct from native DLSS working. |
| Non-DLSS J2 discovery | CPU-visible camera blocks, selected scene/depth, placement and temporal association must be discoverable. GPU-written matrices, procedural/ray-traced scenes, unusual view families and opaque indirect state can defeat that evidence. | Reuse native FSR/XeSS/Streamline inputs first. Add missing acquisition to the current observer/provider contract rather than guess buffer meaning. |
| New DX12 work types | Full observer tracks legacy barriers, classic draw/dispatch and render-pass attachments. It does not implement a complete enhanced-layout state model, mesh/ray dispatch observation, arbitrary bundles or GPU-mutated indirect bindings. NR invalidates legacy state on enhanced barriers; that is not full enhanced-barrier support. | Add modern creation/layout/work coverage to the existing observer and state model as explicit increments. A legacy state flag is not an enhanced layout. |
| Bindless rendering | Bounded table inspection is not proof of which index a shader selected from a very large heap. Reflection identifies declared ranges but cannot recover every runtime index. J2's selected-scene read heuristic scans at most 64 descriptors per range. | Prefer labeled native upscaler contracts. For discovery routes, extend used-resource evidence at the existing shader/binding seam; do not scan every heap on every draw. Bindless alone does not invalidate a game's native DLSS route. |
| Depth formats | Several reconstruction/NR/FG copies require a 32-bit float plane. `depth_format.hpp` explicitly refuses D16/D24 as direct R32 copies. | Add typed R16_UNORM/R24_UNORM_X8 SRV conversion into the existing R32 depth workspace. This is a missing conversion, not a reason to replace the backend or weaken a copy check. |
| Extents and views | XeSS replacement explicitly refuses nonzero bases. The native-scale bridge currently requires exact-size, single-sample, single-slice scene input and a compatible full-size destination. Generic J2 supports more MSAA/mip/slice cases, but that does not make every consumer support them. | Extend existing EvalDesc/subrect/copy/resolve contracts end to end; preserve selected subresource and active extent separately from allocation/output size. |
| Motion quality | Camera reprojection explains static scenery, not independently moving people, vehicles, foliage or transparency. Native motion has strongest semantics; optical flow is an optional approximate supplement with real GPU cost. | Validate native motion conventions first, then measure the incremental image-quality benefit/cost of flow. Do not assume a low acceptance percentage means a stall or that relaxing all rejection rules improves the image. |
| Render ordering | The fixed counter mismatch is covered, but one latest native producer payload is not a general multi-view scheduler. Multiple producers sharing a target before their consumers, or replayed closed lists with mutated camera memory, need stronger per-submission association. | Extend existing recording/submission identity and queue ordering rather than infer identity from Presents or texture pointer equality. |
| HUD and colour stage | The four automatic HUD methods are fallbacks with finite evidence; some games draw an inseparable whole-picture composite. Colour encoding/stage cannot always be inferred from texture format. | Keep the existing pre-HUD/explicit tags/source snapshots; qualify actual separated pixels. Preserve scene/output colour-space contracts and avoid calling an entire final picture a verified HUD layer. |
| Native FG/NR | Pre-FG NR needs the correct real-frame pair, supported full/HUD-free composition, state/layout, subrects and queue ownership. Resource names alone do not establish every title's FG protocol. | Continue using the existing native NGX/Streamline boundaries and exact frame contracts. Qualify new protocol/version/composition variants with the current recorder and GPU fixtures. |
| API/build variation | FSR interception is the supported exported FidelityFX API; it is not an interceptor for every statically linked FSR2/3 fork. XeSS and Streamline decoders accept known ABI layouts and pass unknown versions through. Embedded/pipeline-library root signatures also limit sampler replacement. | Add observed ABI/provider variants in existing adapters, with primary-header layouts and small regressions. Do not equate DLL presence with successful reconstruction. |
| Multi-device/capacity | The DLSS service owns one attached device and bounded records/views; GPU services and presenter have bounded context/chain pools. Multiple independent device/view graphs can exhaust those limits. | Measure the actual required graph, then key/retire contexts using existing lifetimes. Do not remove bounds or retain every historical resource forever. |
| Installation | The additive proxy must actually be loaded early. A conflicting proxy filename, alternate executable, late loading or protected process can prevent coverage. | Keep reversible manifests, early-load receipts and clear route diagnostics. No native game DLL replacement or protection bypass is part of this audit. |

Enhanced/legacy barrier interoperation requires a real layout transition, not
casting flags. See [Microsoft's enhanced-barrier specification](https://microsoft.github.io/DirectX-Specs/d3d/D3D12EnhancedBarriers.html).
NVIDIA also assigns render-size selection to the integrating application via
its optimal-settings query: [Streamline programming guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuide.md).
The full/HUD-free real-frame requirements come from its
[DLSS-G integration guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md).

## Efficiency review and retained code

| Subsystem examined | Decision / remaining cost |
|---|---|
| Loader, bootstrap, host session, settings and ownership | Preserve deferred setup, renderer election, background persistence, recovery and optional producer activation. Native-DLSS installs need not run the full J2 discovery route. A second independent renderer or all-route startup is not an optimization. |
| Hook registry, descriptors, roots/PSOs, CB snapshots and transport | Keep current striped descriptor locks, VA interval index, per-recording caches, immutable variants, skipped event kinds and per-thread rings. The current 6,000-draw synthetic benchmark still shows measurable observer CPU cost; no claim that all hook overhead is gone. Large global revision invalidation and broad table inspection remain candidates for measured, scoped refinement. |
| Discovery/classifier/camera/profiles | Keep established heuristics and verification as separate evidence. These are used by non-DLSS paths even when absent from one Sniper session. Do not delete them as "dead" merely because native DLSS supplies the same inputs in another game. |
| J2 placement, Layer2/scene resources, native scale, HUD and presentation | Preserve today's alternate paths. Native scale disables competing size owners by configuration; the other routes are still valid cross-game fallbacks and fixtures. Windowed/borderless/fullscreen share input/output contracts, but physical game-mode acceptance remains a live test. |
| DLSS/NGX/Streamline/FSR/XeSS | Preserve backend-owned features, view histories, cross-queue ordering and GPU-completion retirement. Locks around mutable feature state cannot safely be removed just because a synthetic single-thread case is faster. Native DLLs remain untouched. |
| NVRHI | Retained and qualified as an alternate owned recording service. Automatic production selection currently chooses the already-pooled native backend because the measured NVRHI wrapper added imports/state/fence work without removing it. The removed UI switch has not returned. Do not force NVRHI on to claim a framework is being used. |
| D3D12MA | Active common allocator for owned native/NVRHI resources. Allocation ownership follows the resource, existing GPU leases keep it alive, and committed allocation remains a fallback. Its bounded process-lifetime device registry is deliberate; destroying it on live resize/toggle would reintroduce unsafe lifetime/idle waits. Not every legacy/third-party workspace has been converted to it. |
| Taskflow | Active bounded two-worker preparation/hash/compile service. Keep below-normal priority, queue bounds, worker-safe shutdown and the existing no-join-under-loader-lock rule. Not a job scheduler for every draw or a substitute for GPU ordering. |
| Optical flow, jitter, camera fit and native-motion selection | Retained current algorithms and today's changes. Flow descriptor preparation optimized; GPU search/pyramid/fit passes and asynchronous telemetry still cost time. Halton/R2 arithmetic is negligible beside intercepting and recording work; no speculative sequence rewrite. |
| XeGTAO, Donut tone mapping/Bloom, RCAS/NIS scaler | Retain optional defaults, cached immutable binding packets, scratch reuse and per-recording owners. These are real GPU passes when enabled. Bloom does not make the program cheaper than turning Bloom off. No additional enhancement/framework added. |
| Capture, status/event log, profiling, usage sampler, DRED/watchdog | Disk writes and capture readbacks are on workers; no new synchronous diagnostic GPU readback in game callbacks. Active captures can still consume major GPU bandwidth/storage and skip busy slots. Profiling is opt-in. Failure evidence and device-removal diagnostics are useful, not obsolete rendering code. |
| Build, SDK options, tests and additive installer | Full shipping source built with current SDKs/NVRHI and production discovery adapter. SDK absence must remain a visible unavailable feature, never a fake pass. Test stubs are not install payload. Existing prepare/hash/rollback machinery is reused. |

## Validation receipts

Raw evidence is local, never committed: `local/universal-dx12-audit-20261106` in
the main workspace. `flow-baseline.log`, `flow-after.log`, `baseline-hotpath.log`,
`old-present-guard-tests.log`, `full-tests.log`, `native-review-tests.log` and
the final combined logs preserve successes and failures separately.

The cleanup snapshot's complete Release suite: **227 passed, 5 skipped, zero
failed, 232 cases, 372.19 seconds**. The three native correctness fixes combined
with Opus's work: **7 affected tests passed, zero skipped/failed, 12.33 seconds**.
Reintroducing the old Present equality guard: **both separate native graphics
and compute tests fail**, as expected; the guard was restored to the corrected
source immediately afterward. The five full-suite skips were the unavailable NR
runtime probe, three unsupported 16x-MSAA cases, and the external upscaler jitter
runtime test. Final combined qualification and handoff are appended below after
completion; earlier receipts are not promoted to final-source evidence.

The changes make the program smaller and remove specific wasted work and
ordering defects. They do not prove that no further optimization exists, restore
130 game FPS by assertion, or qualify all engines/window modes without Cort's
tests. The remaining compatibility work is concrete acquisition/adapter/state
coverage, not a reason to discard the core or build another framework.
