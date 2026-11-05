# Shared NVRHI infrastructure and Experimental effects — 5 November 2026

## Delivered scope

Cort explicitly authorized the reviewed infrastructure integration followed by the effects, with XeGTAO in Enhancements and everything else discussed in Experimental. Both effect families and the NVRHI backend default off. Existing explicit saved choices remain remembered. The existing X-close settings windows retain live sliders and title-hover explanations. Falcor fitted filmic is an alternative tone curve inside Lighting, not a third stacked effect. No game launch, installation or native DLL replacement.

New src/host_dx12/gpu/services owns a pinned optional NVRHI DX12 backend, imported texture/buffer compute packets, owned native-list export, exact-queue submission, nonblocking collection and retained external payloads. Production consumers:

- Donut and XeGTAO scratch texture creation uses the shared resource factory when an actual DIRECT queue is available. Native fallback remains supported.
- Donut compute dispatch uses NVRHI shader/pipeline/layout/immutable binding packets when recorded on an active NVRHI-owned list. Game inline lists use the original native compute path, restoring the complete native binding set on fallback.
- J2 render-small/pre-HUD upscale uses an NVRHI-owned list when enabled, preserving the original native HUD replay after the upscale and before the existing completion fence. Original game command-list batches are not split by migrating J2-between.
- Standalone NR OnPresent uses an NVRHI-owned list and submission. Inline NR/native FG recording inside game lists remains native.

GPU algorithms, camera/jitter/flow, native upscaler interception, input discovery, core frame scheduling and ImGui rendering stay intact. NVRHI command allocators retire on NVRHI submissions; native external payloads retain the existing recording/fence owners. Native resource barriers remain authoritative across raw SDK/passes; automatic NVRHI barriers are disabled for these interop lists and its binding cache is cleared before re-entry. No permanent state is assigned to imported game resources. No live callback waits/readbacks added.

The shared factory is an integration seam for future D3D12MA; NVRHI createTexture currently creates committed backing resources. This increment does not claim D3D12MA pooling or Taskflow integration.

## Boundaries and lifetime

Pinned NVRHI cannot import already-open game lists. It exports its own native list, which must be submitted via NVRHI exactly once; native game lists remain replayable under their existing machinery. SubmitMixed prevalidates closed/single-use/exact queue and one matching owned list before forwarding any native group. Native groups retain order and are forwarded once. NVRHI work is scoped out of observer bookkeeping on each actual calling thread; original D3D calls execute.

There are at most eight cached device/queue contexts, retained for process lifetime because the pinned NVRHI Device destructor waits for idle. Toggle/resize never destroys a device and never waits for it. Texture/compute packets are not globally cached indefinitely; existing scratch/ring owners and NVRHI fence GC retire them. An empty active-recording registry also survives static destruction so other translation units can release exported recordings safely at DLL exit. The integration regression reproduced an exit AV after all pixel checks passed before this correction and passes after it.

Only our render-small owned+HUD batch is split around NVRHI submission; J2-between's original mixed game batch is intentionally retained verbatim. This is supported interop adoption, not a claim that every eligible auxiliary consumer has been rewritten. Native compatibility paths are intentional, not fake NVRHI execution.

## Build and packaging

NA_BUILD_NVRHI defaults ON at build time; experimental_nvrhi defaults zero at runtime. OFF builds retain native services with no NVRHI dependency. cmake/nvrhi.cmake pins NVRHI bf0bbd9b666a7297492beb1999a4ddc0dfded143 and DirectX-Headers d873b344dc540898868697245f100c2a67fe68d9. CI/fresh configure fetches those exact sources; local validation used an unmodified exact-pin override.

Package third_party/nvrhi/LICENSE.txt and DirectX-Headers-LICENSE.txt with the compiled backend, plus existing Donut MIT/Falcor BSD/XeGTAO notices. No NVIDIA model/SDK files committed. Opus owns one combined additive package and existing CMD; Cort retains game tests.

## Automated evidence and limits

Real RTX5070Ti service test executes imported source -> NVRHI compute -> native readback in queue order with native lists on both sides. It checks exact pixel results, external-owner retirement, observer exclusion, discard/reopen and rejection of repeat submission. Donut --nvrhi executes the real effect shaders including two-direction Bloom, tone curves, adaptation and native inline fallback; warm bindings/allocations remain cached. NR --nvrhi and J2 --nvrhi exercise production owned-list paths with deterministic fake NGX/NR models and actual DX12/HUD replay. They do not measure NVIDIA model quality or game performance.

Initial focused nine tests passed 19.08s. Actual service/Donut/NR NVRHI tests also pass on forced WARP, 3/3 in3.84s. The subsequently added J2 production test found the static teardown defect described above; corrected test passes2.90s. Native-only build of host_gpu_services + settings persistence passes with NA_BUILD_NVRHI=OFF. Full final Release/214-case suite result is appended below after completion. Logs remain ignored under main local/experimental-20261105. No whole-game FPS, visual benefit or universal compatibility is inferred.
