# Complete shared GPU allocator and background preparation integration

Cort authorized the decided D3D12MA + NVRHI + Taskflow combination on 5 November 2026, requiring one Opus submission after the whole increment is complete. This extends the previously delivered NVRHI/effects work; earlier receipts saying D3D12MA/Taskflow were proposals are historical. No game launch, control, installation or native game DLL replacement. Opus owns the combined additive package/CMD; Cort owns game tests.

## What changed

The programme now shares one backing allocator for our native and NVRHI scratch resources and one small preparation executor instead of detached per-module preparation threads. Input discovery, jitter/motion mathematics, HUD algorithms, DLSS/NR/FG frame identity, queue selection and our visible controls remain intact.

Reused Opus4225e39's D3D12MA foundation rather than introducing another allocator. Existing NVRHI code/leases/fences remain the submission architecture. D3D12MA handles backing memory, NVRHI handles its eligible GPU recording/bindings, and Taskflow handles independent background CPU work.

| Service | Connected production consumers |
|---|---|
| Shared D3D12MA factory | Existing HUD layers; J2 scene/output/RCAS/jitter texture factory; standalone NR colour/depth/motion/output factory; Donut colour/blur/histogram/exposure/sRGB targets; XeGTAO scratch/sRGB targets; same backing for NVRHI imports |
| Taskflow preparation | Donut shader/PSO preparation, XeGTAO shader/PSO preparation, NIS sharpening shader compilation, NIS scaling shader compilation, DLSS-file identification hashing |
| Existing NVRHI | Donut owned compute packets; J2 render-small/pre-HUD owned lists; standalone NR owned list; imported shared effect resources. Inline game-list recording stays native. |

This is actual production connection, not merely linking unused libraries. Existing special allocations not routed through these factories remain unchanged; no claim that every allocation in the programme was rewritten. Permanent observation/capture loops keep their established owner threads because placing blocking loops on the preparation pool would consume its workers indefinitely.

## Ownership, queue completion and memory reuse

The physical path is D3D12MA memory allocation -> native placed resource -> optional NVRHI handle. A private-data IUnknown holder keeps the memory-only allocation attached to the native resource. It creates no allocation/resource reference cycle: AllocateMemory owns the heap range, while CreateAliasingResource supplies the independent resource. Native COM references and NVRHI import/binding references therefore both retain the allocation. Native recordings still need their existing RecordingLease/fence ownership because raw D3D12 command lists do not themselves retain arbitrary resources. NVRHI-owned command work is submitted through NVRHI and retained on its completion path. CPU task completion is never treated as GPU completion.

No simultaneously live resources intentionally share allocation ranges. A range is returned only when the last resource/lease/submitted NVRHI reference releases it; reuse consequently occurs across ExecuteCommandLists invocations after prior GPU work is retired. This follows Microsoft's documented advanced placed-resource model, where the Execute boundary provides memory coherence. No temporal aliasing inside a live submission, data inheritance, guessed completion or new GPU wait/readback is introduced. Render/depth targets still start with the caller's full clear/copy; compute outputs and counters are fully written before reads. Histories preserve existing first-frame/reset rules.

The wrapper now supports up to eight actual device contexts (one allocator per device), matching adapters by the device's LUID rather than accepting only the first device forever. Tier1 heap-category flags are supplied when needed; unserviceable requests use the original committed-resource API. Preferred blocks are lazy64MiB, not an upfront reservation. Whole-adapter budget is not multiplied by device count. The pinned existing D3D12MA v3.0.1 source is unchanged.

Corrected the failed-private-data path to release the placed resource before freeing its backing allocation. Holder destruction also uses HostWorkScope on the actual releasing thread. The allocator registry is process-lifetime/bounded so late submitted-resource destruction cannot touch a destroyed singleton. Empty cached heaps may remain reserved; live allocation count and reserved/used bytes are distinct diagnostics.

## Background preparation

One shared Taskflow4.1 executor has exactly two below-normal Windows workers, without fixed CPU affinity or hardware_concurrency sizing. At most32 jobs are admitted (running plus waiting); full/stopping admission defers optional preparation rather than waiting in a render hook. Graphics creation and captured resource destruction run inside HostWorkScope on the actual worker. This replaces the five detached preparation/hash job sites listed above, not an added pool alongside those old threads.

Jobs retain their own immutable inputs/program owners. A model-DLL hash result is discarded if its path became stale while queued. NIS/Donut/XeGTAO retain their existing ready/error publication and live-frame readiness behaviour. The rendering thread reads readiness and proceeds with its existing fallback; it does not await compilation or task completion.

Start happens on the existing host initialization worker; isolated component tests can start the service lazily. Explicit overlay Stop drains bounded jobs and joins the executor before teardown; timeout retains the pool for retry. Concurrent Stop is serialized, and a worker cannot join/wait for itself. Process-detach notification only sets an atomic stop flag: no loader-lock joins, waits or static executor destructor. Restart after an ordinary Stop is supported; process-exit stop forbids new work.

Taskflow's70 unmodified header files and MIT licence are pinned to45366fe5bc4f2f8ec9aa590b40c504e296886865 (v4.1.0), with raw-upstream SHA256SUMS and -text preservation. There is no Taskflow configure-time network download and no CUDA dependency. Only host_background compiles asC++20; the core remainsC++17.

## Controls and packaging

XeGTAO remains Enhancements. Lighting and the NVRHI option remain Experimental, retaining live sliders, tooltips, modal X-close and existing saved choices. All image-effect defaults and the NVRHI runtime option remain off. The allocator and preparation executor are infrastructure, not extra image effects or user tuning sliders. Experimental now reports scratch memory used/reserved and preparation activity; status logs include allocator and worker counts.

Both libraries are static/header dependencies; no new runtime DLL needs installation. Keep the established additive installer and native game files. Include third_party/taskflow/LICENSE, third_party/d3d12ma/LICENSE.txt, existing NVRHI/DirectX-Headers licences and the existing effect notices. Preserve settings on upgrade. Source-pick/merge instructions and final qualification results are appended only after the complete automated run.

## Automated evidence

- background_work_test blocks both workers (offline only), fills32 slots, checks bounded rejection without waiting, two below-normal workers, actual-worker observer exclusion, timeout/retry, no self-join, exception recovery, drain/join/restart and process-exit rejection.
- gpu_services_test now holds actual GPU consumption behind a test fence, drops caller bindings/texture references, proves D3D12MA allocations remain live through native/NVRHI submission, then signals/completes/collects and checks retirement plus exact compute pixels. No production gate/wait is added.
- mem_alloc_test checks real placed textures, first-use clear, native resource lifetime, actual-device contexts, repeated heap reuse and equal64-texture creation/destruction batches. On RTX5070Ti, five-pair median CPUallocation time was672.10us pooled vs20786.10us committed with an8MiB cached heap. This measures allocation churn only; no steady-frame FPS claim or timing pass threshold.
- Initial seven component/service/production checks passed17.06s. Complete production Release build and full218 suite are being qualified; final results follow below. Real DX12 fixture integrations use deterministic NGX/NR stand-ins where documented, not claimed NVIDIA model/game quality.

Primary references: Microsoft CreatePlacedResource remarks (advanced model, initialization, Execute boundaries): https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createplacedresource ; AMD allocation/ownership API: https://gpuopen-librariesandsdks.github.io/D3D12MemoryAllocator/html/quick_start.html ; pinned Taskflow worker/executor API in third_party/taskflow/taskflow/core. Existing NVRHI receipt/pin: docs/NVRHI-INTEGRATION-20261105.md.


## Final qualification and delivery

Engineering increment COMPLETE. Runtimeef4d729a7dc71116db9ba8e57b5d28dfa683f4c1 has a complete Release build and full218-case result: **213 passed,5 skipped,0 failures in440.79s**. Existing opt-in NR probe, unsupported16xMSAA trio and real-runtime upscaler-jitter probe are the five skips. All new allocator/Taskflow/NVRHI integration cases execute and pass.

Final forcedWARP7/7PASS12.78s covers background workers, shared memory, pooled NVRHI submission/retirement, AO, Donut, productionNR and productionJ2. NA_BUILD_NVRHIOFF configuration/build plus background/memory/nativeDonut3/3PASS6.96s confirms independence of the native path. Generated projects confirm host_background stdcpp20 and abundance_core stdcpp17. No runtime code change followed qualification; final followup corrects raw-upstream checksum/EOL preservation and records this receipt.

GitHub runtime CI42037757934 succeeds on all three jobs: Windows without NVIDIA SDKs, Windows public NVIDIA SDK headers, Linux portable core. https://github.com/cfitz3219/NeuralAliasAbundance/actions/runs/42037757934 . Source pins/licence verified;70header files +licence match raw upstream git blobs, with the corresponding LF SHA256SUMS. No GPU wait/readback was added to live hooks.

Ignored evidence lives in main local/experimental-20261105: stack-full-build.log, stack-full-tests.log, stack-warp-tests.log, stack-off-{configure,build,tests}.log, allocation-measurement.log. The controlled allocation benchmark establishes lower creation cost, not a game FPS result. Renderer/NGX/NR fixture evidence does not establish game picture quality. No game test performed, as Cort requested.

Opus receives the COMPLETE stack source only now, with this receipt and exact source commits. Build using the package branch's existing production adapter configuration; the isolated regression build uses the host stub for tests. Keep your latest HUD/AF/Breadcrumbs changes, include all notices and retain the established additive package/CMD. No extra algorithm work or review stage is required for this increment. Do not reset the production branch to this test checkout or deploy into a running game.
