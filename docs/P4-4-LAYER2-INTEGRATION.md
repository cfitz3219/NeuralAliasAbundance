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

## Remaining integration

The Layer 2 route is not automatically enabled in Anno. It still needs the same-launch native operation/dependency map, actual clear/copy/resolve and history routing for that group, and the shared J2 logical-to-physical input/output interface. Prepared binding plans and fallback helpers must be connected to that recognized closure. Typeless colour conversion and arbitrary bindless shader resource accesses are not supplied by the current typed-view helper.

Once those pieces are connected and the automated regressions pass, prepare the reversible user test package. Cort retains game launch, deployment and visual/performance acceptance. No game or installer is changed by this increment.

## API basis

- [Microsoft: setting descriptor heaps](https://learn.microsoft.com/en-us/windows/win32/direct3d12/setting-descriptor-heaps): a changed heap invalidates descriptor-table state, and SetDescriptorHeaps supplies all active heaps together.
- [Microsoft: copying descriptors](https://learn.microsoft.com/en-us/windows/win32/direct3d12/copying-descriptors): CPU descriptor-copy sources must be non-shader-visible.
- [Microsoft: ExecuteCommandLists](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-executecommandlists): the previous execution of a reused command list must have completed.
