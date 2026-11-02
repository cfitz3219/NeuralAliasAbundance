# Layer 2 integration status

Layer 2 supplies reduced scene inputs for games without a usable engine render-scale control. Anno is the first target. Opus owns the shared J2 upscale core, its physical-input interface and the native operation recorder. Astra owns the paired allocations and scoped binding/dimension/fallback work here. Existing native-DLSS adapters remain intact.

## Implemented

- `SceneGroup` prepares an immutable group of native and genuinely smaller companion allocations. The game's original committed/placed allocation and heap layout stay intact. Per-member dimensions include identified half/quarter dependencies. Publication is atomic; allocation alone does not enable redirection.
- Explicit viewport/scissor and known compute-thread layouts scale the selected operation. Typed semantic constant patches edit owned bytes, rather than search-and-replace matching numbers in shared game buffers.
- `GpuConstants` copies the latest source CB on the GPU and patches only identified extent fields. Game/J1 camera or jitter changes made after command recording remain visible. Both root CBVs and descriptor-table CBVs can use the owned result.
- `BindingHook` connects prepared plans to the existing shader-operation Bindings lane. It binds private RTV/DSV and typed SRV/UAV/CBV descriptors, preserves the native sampler heap/tables and rebinds all native resource tables together. It restores native roots/arguments/heaps/PSO and raster state after the operation. It adds no independent draw hook.
- `ListShaderState` reads the existing observer shadow. Raster restoration retains CPU copies of native RTV/DSV descriptors, so legal reuse of the original CPU slots cannot change the restored targets.
- The prepared plan/resources are retained through the existing executable-recording/submission service, including legal replay and Reset while previous GPU work is pending. This is independent of native FG being enabled. With no SDK-backed retention service, the new scope remains unavailable.
- `ColorRestore` supplies a prepared linear spatial promotion into full-size native colour; `DepthRestore` supplies a real sampled depth draw, with stencil preserved. Neither is an illegal scaling copy. The native HUD can then draw at native pixel coordinates. These are fallback helpers, not DLSS substitutes.

## Runtime contract

Prepare allocations, typed views, constants and PSOs on the setup worker under `HostWorkScope`. Feed actual recognized per-operation view/root/layout metadata; the binding layer does not invent scene semantics from matching dimensions. The private shader heap is populated from typed descriptions, never copied from a shader-visible source heap.

The provider admits one coherent scene group, its queue ordering and the operations that use it. Private members enter and leave these operations in COMMON; explicit barriers cover the actual selected subresources. Queue ownership and initialized/history contents remain the group's responsibility. A retained pointer does not serialize simultaneous writes on different queues.

An acquired plan that cannot bind calls `BindingProvider::Refused` before recording its private commands. For a partially redirected scene, the owner must promote the needed current-frame resources and restore native state before forwarding that consumer. Returning to an untouched native allocation alone does not supply the current picture. The same distinction applies to J2's forthcoming `ReplacementUnavailable` result.

Install Reset and submission tracking during host setup. Each injected recording must pass through the existing `nr::native::Submission` wrapper around its actual Execute. `SetExecuteForwarder` is a helper/sample forwarding hook, not the wrapper for ordinary game submissions. Prepare plans per the group's real queue/recording ownership; mutable GPU constants cannot be raced across independent queues.

## Automated evidence

Real DX12 tests exercise allocation and patching, raster target restoration after legal CPU descriptor-slot reuse, GPU-time camera markers and depth/colour promotion. The new scoped-binding fixture uses the actual producer hooks and shader forwarding:

- Native32x16 and reduced21x11 dispatches with CBVs in both a descriptor table and a root binding, plus separate UAV/SRV tables and a native sampler table.
- An incomplete resource-table map is refused before private commands; the original dispatch executes. A complete map renders all expected pixels; the following native dispatch sees its original bindings/dimensions.
- A marker uploaded after recording reaches both private and native consumers. No source dimensions are changed.
- The prepared payload remains alive after a completed submission while its recording is executable, and after Reset while a later submission is held behind a test-only GPU gate. It retires after that GPU work completes.
- Raster colour AND depth render to real smaller resources. Spatial promotion fills the native output, then a9pixel native HUD draw uses native constants/targets. Every colour/depth pixel is checked; the DX12 validation layer reports no errors.

Correction to earlier journal/interface wording: submitting the same direct command list twice in one Execute is not a legal replay test. The debug layer caught this. Both the constants fixture and new scope fixture now replay only after the previous execution completes. An initial new fixture also omitted the production submission wrapper; that caused premature retirement in the test and has been corrected. Neither failure was a measured game defect.

These shaders are controlled DX12 fixtures, not DLSS or game-quality evidence. Full-suite totals and source receipts are recorded in WORKLOG/COMMS after the run.

## Remaining integration at ca61235 (historical; update below)

The Layer 2 route is not automatically enabled in Anno. It still needs the same-launch native operation/dependency map, actual clear/copy/resolve and history routing for that group, and the shared J2 logical-to-physical input/output interface. Prepared binding plans and fallback helpers must be connected to that recognized closure. Typeless colour conversion and arbitrary bindless shader resource accesses are not supplied by the current typed-view helper.

Once those pieces are connected and the automated regressions pass, prepare the reversible user test package. Cort retains game launch, deployment and visual/performance acceptance. No game or installer is changed by this increment.

## API basis

- [Microsoft: setting descriptor heaps](https://learn.microsoft.com/en-us/windows/win32/direct3d12/setting-descriptor-heaps): a changed heap invalidates descriptor-table state, and SetDescriptorHeaps supplies all active heaps together.
- [Microsoft: copying descriptors](https://learn.microsoft.com/en-us/windows/win32/direct3d12/copying-descriptors): CPU descriptor-copy sources must be non-shader-visible.
- [Microsoft: ExecuteCommandLists](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-executecommandlists): the previous execution of a reused command list must have completed.

## November 1 continuation: resource operations and frame ownership

Merged the shared runtime through d6dc848 at 745acd1, preserving the Layer 2 raster/binding hooks, both build/test additions and both append-only journals. The shared J2 `PhysicalInputProvider` is now present in this branch.

`TextureOperationHook` extends the existing command-list detours for RTV/DSV clears, whole-resource copies, texture-region copies and resolves. It adds no second API hook. A callback snapshot survives unregistration until After; native calls forward once or are completely replaced. Host operations never recurse into the callback. `PreparedTransfer` supplies private views, correctly remaps supported mip/slice indices, scales exactly representable boxes/rectangles, and balances private COMMON transitions. It retains resources/descriptors through executable recordings and all submitted uses. A copy with an outside-group endpoint or unrepresentable region is refused before private commands; the route's existing Refused callback owns promotion before forwarding.

`SceneFrame` owns each admitted frame's prepared fallback and history work. It seeds identified colour histories from the previous full-size native image, with separate explicit states for the seed and finish boundaries. The owner marks actual written dependencies and marks the scene ready. Its J2 input carries the exact reduced colour/depth, full-size destination, states, extent and one group generation. A redirected but incomplete frame returns Unavailable. Finishing a failed/unavailable replacement restores the current colour and written dependencies, including depth, to their native size; finishing a successful replacement preserves the DLSS colour. Raster state is restored before the original full-size consumer/HUD continues.

`PhysicalInputBridge` binds a frame to its actual list identity and recording generation using the existing RecordingLease. Inline J2 lookup uses that exact recording. `SubmissionScope` selects the same snapshot around a between-lists J2 call. Reset, stale identity, unrelated logical IDs and missing submission scope cannot reuse the previous frame. No second GPU-fence service is introduced.

Both binding and transfer providers receive `Applied` only after the private operation has actually been recorded. The native map owner marks SceneFrame's written dependencies there; it must not mark them in Acquire, which can still be refused before any commands. The expanded fixture now obtains its colour/depth written state through that real callback rather than marking it manually.

New/extended actual GPU tests cover the transfer hooks, partial-copy output, a 2x MSAA resolve, lifetime after completion versus Reset, history seed/copy-back, complete colour/depth promotion, native HUD pixels and preserved enhanced colour. The bridge's provided/unavailable/failure and inline/between/reset cases exercise the actual callback contract. The successful-colour fixture explicitly simulates an evaluated output; it is not a real neural-model image-quality result. Full-suite receipts are appended in WORKLOG and COMMS.

The implemented transfer set is typed RTV/DSV clears and copies/resolves, not arbitrary UAV clears, ExecuteIndirect or render-pass rewriting. Frame promotion currently supports plain resolved members, typed colour and identified D32/R32 depth. The admitting map must exclude unresolved operations or promote before their native consumer; arbitrary bindless/typeless conversion is not supplied by this increment. No claim of complete coverage is inferred from the fixtures.

### Integration input still needed from the game/recorder lane

No usable same-launch native Anno dependency/binding map has been supplied. The existing Stage M PassMap diagnostic merges operations and approximates table reads with 16 slots; it is not itself an executable per-operation plan or proof of semantic dimension fields. The actual recognized map must select the complete group, typed views, dimension fields, current states, initialization/history and consumer boundaries; bind its frame through PhysicalInputBridge, use the scoped binding/transfer providers, and call Finish before each required native consumer. Those runtime admission/provider connections and actual Anno qualification are still pending, rather than secretly enabled from matching dimensions. No game deployment, installer publication or game launch occurred in this continuation; Cort retains game testing and the SE5 installer hold is preserved.

### Final automated receipt and core handoff

Source 3454fb5 builds Release and passes the full local suite: 164 passed, zero failed, five skipped out of 169 (237.66s). Skips are the opt-in NR probe, three unsupported 16x MSAA cases and the optional upscaler-jitter fixture.

GitHub's no-SDK Windows job exposed the older colour gradient test's overly tight fixed 0.001 tolerance on WARP. A forced local WARP reproduction measured a maximum green error of 0.00122547. The Direct3D [texture-addressing specification](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm) guarantees at least eight fractional subtexel bits; for this gradient that gives per-axis error bounds of `1/(256*(source_size-1))` plus floating-point allowance. The test now uses those derived bounds, still verifies every pixel and preserves exact HUD/HDR/alpha checks. The runtime linear filter was unchanged. A forced-WARP regression is now registered.

Final narrow refinements also make SceneFrame seed/finish and PreparedTransfer recording self-isolate in HostWorkScope. Callers cannot accidentally feed private draw/clear state back into the native observer. A direct public transfer call must leave the callback count unchanged; frame seed/finish tests now call without external guards. Final Release runtime and five focused tests pass (6.08s), including real shared J2 provided/unavailable/wrong-identity integration. These refinements are not attributed to the preceding full-suite source.

Source 8510b9c subsequently passes all three GitHub jobs (Linux, Windows without SDKs, Windows with public NVIDIA SDK headers), in both push/PR runs. The final provider Applied notification refinement then passes the same five focused tests in 6.73s; its source receipt is in WORKLOG/COMMS.

Opus was also notified of an early extent handoff to address before reduced-route activation: J1 currently patches using the preceding `g.rw/g.rh`, while J2 updates those dimensions later in record. A native/reduced size switch needs the selected group's extent before the first camera patch, or an explicitly unjittered transition frame. A later history reset does not rescale an already rendered sample. This is a source-order finding, not a measured Anno artifact; shared J1/core changes remain Opus's lane.

## Automatic integration continuation (November 1 Pacific)
The opt-in `scale/auto_scene` owner now uses the real producer shadow directly rather than turning the diagnostic PassMap into executable instructions. Exact typed descriptor creation/copy metadata, finite root layouts, current targets and compiler-reflected dimension fields supply `NativeBindingSpec`. Observed paired scene operations select the primary MRT/depth group; copy lineage only becomes a group dependency when scene work binds it. Prepared views, GPU-time constant patches and transfer plans are built on the existing session worker and adopted at a coherent frame boundary. Current executable recordings retain their selected resources and plans.

Native-current checkpoints at original command-list Close and native consumers preserve usable full-size data even if J2 does not evaluate. Per-recording pending writes are distinct. Native/private copy and clear operations both execute; the owner acknowledges their mirrored write instead of unnecessarily spatially promoting a read-only copied companion. Reduced inputs remain available to J2 through an exact-source-recording provider lookup. The shared backend, insertion selector, GPU retention, camera/jitter and flow services remain the existing ones.

Actual GPU tests now cover same-launch recognition and publication, reflected smaller scene rendering, copied colour/depth used by a water-like draw, all native pixels, visible test-model output through real J2, model-failure fallback, live quality switching and the captured D32S8 family. These controlled shaders/test NGX callbacks do not prove an Anno image-quality or GPU-time result. Current engineering/qualification is still in progress; the new route has not been deployed to Anno. Cort requires Opus's final verification before any installer CMD is published.
### Automatic-owner review corrections (2026-11-01 Pacific)
Known draws now use allocation-free shadow keys and shared-lock plan lookups; exact descriptor/resource snapshots are cold worker requests. Compiler-used resource/field metadata permits native material-table passthrough when no private resource/table CB patch is required. Changes to those materials do not create new plans or interrupt reduced rendering. A shader using private inputs still gets complete finite rebased tables and explicit null unused descriptors.
Scene selections have two retained contexts and switch at Present after stability; returning to a cached context does not reallocate. Preparation is incremental with cached negative results, evidence-based retries and bounded age sweeps. Unsupported work stays steadily native. New structural operations still need worker preparation and a coherent native learning interval; this is not a promise of instant support for arbitrary unseen shaders, bindless, bundles, indirect or render-pass work. GPU/recording ownership continues through existing leases.
The material, failure-cache/recovery, stencil, inverse-vector, split-list, native-boundary and scoped-binding cases pass on the RTX GPU. They prove exact fixture pixels and test-model plumbing, not real Anno DLSS quality or reduced GPU frame time. Opus verification/workload measurement must precede a built installer; Cort owns all game tests. Cumulative state/build/failure/control-worker timing telemetry is in the existing status log.