# Automatic material enhancement — Astra's proposal for Fable

## Approved implementation update — 2026-11-14 Pacific

Cort subsequently authorized: "Check Fable's response in COMMS. Implement his
plan, which is your plan with minor adjustments. Begin." Fable's 2026-11-15
01:35Z COMMS response and Astra's acceptance are the combined direction. The
dated initial proposal below is preserved as history; its planning-only status
and proposed ownership are superseded by this section.

- Fable owns material census, source identity/acquisition, DX11 sampled-view
  substitution, resource publication/retirement and later DX12 delivery. He also
  supplies the independent existing-upscaler colour baseline. Astra owns the
  shared record/cache format, coordinated detail candidate, filtering and common
  comparison. Both candidates use the same six inputs; one generator owner
  remains Astra after Cort judges the comparison. No duplicated runtime stack.
- Native DX11 first, WWII or BF4 selected from actual material evidence. Fable's
  first distinct marker texture fixture can proceed without any AI model. HOI4
  atlas/data maps are not the initial demonstration. Census joins an otherwise
  useful package; draw counts are a priority proxy, not measured pixel coverage.
  Co-binding is an association candidate, not proof of map roles.
- Pre-generation runs outside the game. A headroom-limited runtime ceiling near
  1.5 GiB replaces 256 MiB as the proposed general budget; 256 MiB remains a small
  fixture budget. Count compressed resident maps, staging and retiring versions;
  reserve memory for the game and avoid oscillating replacements under pressure.
- Content hashing uses owned bytes from a bounded valid-lifetime copy or ordered
  asynchronous resource snapshot. Record a complete observed mip subset and its
  LOD mapping, rather than waiting forever for streamed higher mips. Atlases need
  padding-aware handling before being included.
- DX11 Present is a policy boundary, not GPU retirement. Reused deferred command
  lists retain the exact resources they recorded; report actual applied state.
  Original game-facing SRVs naturally expose original resources on GetResource.
  First DX12 substitution is at an actual safe descriptor-write boundary with
  a prepared asset already available, not mutation of live persistent tables.
  This can mean a following session on that first DX12 path; say so plainly.
- Shared contract: `src/materials/record.hpp`, `docs/MATERIAL-CACHE-V1.md` at
  e8cbd44. Preparation implementation: `tools/material_lab/README.md`.
  The first Astra candidate is deterministic guided mathematics, not a trained
  model. It preserves dimensions; Fable's plain upscale can enlarge colour.
  Dimensions, actual output files, and original sources remain explicit.

Current M1 increment: six generated material sets, compressed complete mip
chains, immutable cache, controlled comparison and static reference images.
Fable's independent baseline and actual game delivery remain outstanding. This
increment does not declare the visible in-game deliverable in section 1 complete.

## Initial proposal, preserved

Date: 2026-11-14. Status: design proposal, awaiting Fable's independent proposal and comparison. Cort has authorized planning and collaboration, not implementation or another game test. No runtime, installer or game-file changes in this work.

## 1. What Cort is asking us to deliver

An obvious improvement in how games look: wood with convincing grain, stone with fine relief, cloth with a coherent weave, and surfaces that catch light more naturally. The result must preserve the game's design, remain attached to the same surfaces during movement, and have a practical running cost. It should become a distinct NeuralAlias capability, not a collection of unrelated effects or a renamed external texture upscaler.

The proposal is **persistent enhancement of the game's materials**. Generate coordinated surface detail once, save it, and let compatible game shaders render those enhanced materials on subsequent frames and sessions. Reuse NeuralAlias's existing hooks, resource ownership, workers and UI. This is an enhancement available alongside Routes A and B, not a third rendering route, and it must be able to work with DLAA disabled.

First visible deliverable: one recognizable material in a running game, with improved colour detail AND a demonstrated improvement in surface/light response. Show it at ordinary viewing size, while the camera moves. Then repeat with another material family and another binding layout. A successful allocation, replacement count, or enlarged texture is not this deliverable.

## 2. Prior art and the proposed contribution

- RTX Remix already offers texture enhancement and generation of material maps. Do not claim we invented AI materials. [NVIDIA documentation](https://docs.omniverse.nvidia.com/kit/docs/rtx_remix/1.2.4/docs/howto/learning-aitexturetools.html).
- AutoCrispy already watches dumped textures and runs AI upscaling, including seamless-texture handling. Automatic processing and caching alone are not sufficient differentiation. Its repository is GPL-3.0; it is a comparison/reference here, not a dependency selected for copying. [Project](https://github.com/WalkerMx/AutoCrispy).
- NVIDIA's Texture Space Material Diffusion generates coordinated material maps in texture coordinates. Persistent surface detail and generation of multiple maps also have precedent. Its research conditions include geometry/texture parameterization that an arbitrary injected game does not automatically expose; the project page does not establish a deployable dependency for us. [Research](https://nvlabs.github.io/texdiffusion/).
- Neural appearance research demonstrates rich light response with dedicated material evaluation. It does not establish that replacing arbitrary maps in an existing game reproduces that renderer. [Research](https://research.nvidia.com/labs/rtr/neural_appearance_models/).

**Working contribution to investigate:** preserve the existing material's recognizable structure, derive a coordinated fine-detail addition across its usable channels, fit that addition to the shader's actual interpretation and sampling behaviour, and persist the result automatically in existing DX11/DX12 games. Compare against a conventional texture upscale and independent per-map enhancement. If our method adds no visible advantage, change the method before building a large compatibility layer. No claim of research novelty or commercial uniqueness yet.

## 3. The material algorithm we would own

Call this the *material detail proposal*, not a committed product name.

1. Decode an identified material into its actual colour and surface channels. Preserve the original broad colour pattern, wear, seams, logos and existing shading conventions. Do not assume a dark pixel is a crack or a highlight is a metalness mask.
2. Produce one shared fine-detail description for the material. It drives colour detail, small surface-normal changes and roughness variation together. For example, a wood fibre's visual orientation and small surface response should agree. Existing meaningful details have priority over newly synthesized ones.
3. Constrain the added detail to preserve the original at normal distance. Use low-frequency colour preservation and comparison after downsampling, plus a rendered-view comparison under several light/view directions. Normal and roughness changes need energy/appearance comparisons; simply averaging their channels is not enough. New detail is synthesized enhancement, not recovered ground truth.
4. Generate proper mip levels for the enhanced set. Filter colour in the correct space, normalize/reconstruct normals correctly, and account for unresolved normal variation in roughness filtering where the shader convention supports it. This must not create sparkling grain or an unrelated global negative mip bias.
5. Save the resulting textures and their generation settings. Use a fixed material seed and a single accepted version. Do not regenerate a surface every frame or every time it reappears.

Prototype the algorithm outside the live game first. Compare a small learned residual generator with a simpler guided/procedural detail generator using the same inputs and renderer. This is an explicit comparison of two candidates for ONE generation stage, not two complete products. Pretrained enhancement can be a disclosed baseline or an initialization if the weights permit use; it is not the claimed invention. A custom training/fine-tuning job needs an identified dataset, model, license and measured memory/time budget before it begins. No foundation-model training project is assumed.

Use six initial examples across wood, masonry and fabric, with held-out materials. At least one scene should contain two different materials with similar colours, to test that the system does not apply the same grain everywhere. Training/test material sets stay separate; use licensed or generated source materials for reusable training data. Local game captures are evaluation inputs, not automatically a redistributable training set.

## 4. Complete path to the visible result

`Observed material resource/view -> identify content and channel roles -> asynchronous capture if needed -> offline/loading-time enhancement -> persistent cache -> upload private replacement resources -> publish a coherent material version to real sampled bindings -> game's existing shading -> existing DLAA/other effects if enabled -> final screen.`

The important boundary is before material shading. A finished colour frame plus depth does not uniquely reveal the original texture coordinates, material maps or lighting. Do not silently substitute full-screen generative repainting if we cannot reach a material. Likewise, creating a normal/roughness map does nothing unless the game's shader samples and interprets it correctly.

Use these capabilities independently:

| Observed capability | What we can actually promise |
|---|---|
| Correct base-colour view and sampling identified | Persistent colour detail; do not claim new surface/light response |
| Correct normal and surface-response channels identified and writable through replacement bindings | Coordinated material enhancement using the game's existing shading |
| No corresponding shader input | Keep that generated channel unused; adding new material evaluation is a separately justified extension |
| Texture.Load, integer addressing or queried dimensions influence rendering | Preserve dimensions/semantics initially; larger replacements require an explicit coordinate/LOD mapping |

The first demonstration needs the second row. It may use a developer-selected material to establish the mechanism. Mark that selection as manual; it does not count as automatic discovery or cross-game completion.

## 5. Automatic selection and portability

Start with static opaque 2D material textures sampled through ordinary normalized coordinates. Choose by actual observed use, view formats, sampled channels and shader behaviour, not game names or assumed universal register numbers. Existing shader/binding facts are inputs; reflection alone often lacks channel semantics, so uncertain layouts need a compact observed-layout adapter or a focused material probe.

Identify a material as a set of views/roles used together, not just one image hash. Repeated bindings alone do not prove those maps form one material. Record the association and unpacking evidence. Recognizing one channel must not require every unrelated channel to be known. One unqualified material remains original while qualified ones can improve.

Content identity must survive resource-address reuse and reloading. Key cache entries on available source content and mip layout, typed views, packing/normal convention, algorithm/model version, settings and seed. Separate immutable content identity from process-local resources and binding context. Streaming writes or changed content invalidate the affected version; partial low-mip observations do not identify a complete higher-resolution source. Hash/copy large data on workers or bounded asynchronous GPU jobs, never synchronously on each draw.

Preserve atlas/UV layout, tiling and non-colour channels. Packed masks, alpha, normal handedness and sRGB interpretation are explicit. The first implementation leaves transparency/foliage, animated/dynamic textures, virtual-texture page systems and unrecognized packing on the original path. These are named acquisition extensions, not reasons to disable the whole feature. Add each shared mechanism when its inputs and benefit are established.

For the chosen shader, distinguish roughness from glossiness and any squared/encoded convention, and respect compressed normal reconstruction. Treat painted/baked lighting as authored content until an actual material interpretation permits separating it. Larger maps also change mip/LOD behaviour: preserve the visible result of the original view's mip range, clamps and explicit-LOD instructions through a documented mapping or keep dimensions unchanged for that path. Existing AF/mip controls are held constant during appearance comparison.

**Bindless is not automatically unsupported.** A qualified, immutable material resource that can be substituted at a safe descriptor-creation/copy boundary may work regardless of shader indexing. Arbitrarily redirecting a persistent directly-indexed heap is not already solved by our table patcher. Fable should select the smallest first DX12 binding path and state the exact remaining distinction; do not promise all bindless or reject all bindless.

## 6. Reuse map: source inspected, not assumed

Source inspected at runtime line 0e2261c / packaging 2c2e761 in `C:\Users\Cort\NA-hoi4-boundary`. Main currently carries coordination, not that whole code line.

| Existing component | Reuse | Work still needed |
|---|---|---|
| `src/host_dx12/native11/provider.cpp` | PS/CS/VS resource-binding observation, immediate/deferred context handling, OwnWorkScope | Material association and replacement SRVs; later-stage use if actually observed; do not mistake current scene tracking for an asset replacement system |
| `src/host_dx12/native11/samplers.cpp` | Cached variants, policy changes and original-state restoration as design precedent | Textures have content/write/version and lifetime rules beyond sampler objects |
| `src/host_dx12/observe/producers.cpp` | Typed descriptor creation/copy facts, real binding identities and resource tracking | Material-content registry and safe publication of replacements |
| `src/host_dx12/scale/scoped_bindings.*` | Immutable prepared table slices, retained resources, typed reconstruction of descriptors | Extract only reusable ownership/binding pieces; do not reactivate held-off resolution scaling or copy the whole Layer 2 planner |
| `src/host_dx12/background/work.*` | Existing bounded background work facility | Material preparation jobs with deduplication/cancellation; no new per-draw wakeups |
| `src/host_dx12/gpu/services.*`, `gpu/mem_alloc.*` | Existing private-resource allocation and recorded/submitted ownership on DX12 | Upload/cache residency policy; these services are not a DX11 texture allocator |
| `src/host_dx12/capture/*`, existing hardware fixtures and overlay | Bounded diagnostics, before/after pixels, controls and evidence | Source-material capture where missing; material-specific tests/status |

The completed GPU stack stays intact. NVRHI is for owned DX12 work where appropriate; do not import arbitrary game command lists into it. D3D12MA backs owned DX12 allocations; native DX11 keeps its own resources. Taskflow handles preparation, not a new task for every sampled texture.

## 7. Publication and lifetime requirements

DX11: replacement views must preserve format/channel and sampled-subresource semantics; retain original bindings for restoration and exclude our own work. Cover immediate and deferred recordings, already-bound views and reused command lists. A policy change cannot be advertised as applied if an old recorded list still samples the previous version. Start by preparing before the material is recorded; add live publication only with an exact execution rule.

DX12: reconstruct private descriptors from retained typed metadata where needed; do not read shader-visible heaps as CPU descriptor-copy sources. Never overwrite entries still referenced by recorded/submitted GPU work. Natural game descriptor writes may permit substitution; persistent tables need immutable versioning/rebinding with the actual root and heap layout. All associated maps for one draw use one version; old recordings keep old resources until completion. Independent queues require real ownership/order evidence, not a CPU timestamp. [Microsoft descriptor-table rules](https://learn.microsoft.com/en-us/windows/win32/direct3d12/advanced-use-of-descriptor-tables), [descriptor heap restrictions](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_descriptor_heap_flags).

Original asset files stay untouched. Publish ready material sets as units, not colour now/normals later. Keep a chosen version stable through a shot. Background preparation must not repeatedly switch originals and enhancements as memory pressure changes; use the original until a replacement is ready and budgeted, and prefer predictable loading-time transitions. Turning the effect off restores original material bindings when the execution path supports that switch; old in-flight work retires normally.

## 8. Performance and product shape

Heavy generation starts as a separate local preparation job, normally with the game closed or during an explicit preparation phase. A worker running on the same GPU can still contend with the game; calling it asynchronous does not make it free. Opportunistic generation during gameplay is later work, justified by measured spare budget.

Steady-state path: cached lookup/replacement and ordinary texture sampling. No full-frame neural inference for this proposal, no synchronous readback, no draw-by-draw semantic classification or repeated full-texture hashing. Keep lookup state local/immutable where possible; resolve content/model work off the draw path.

Provisional first-demo budget, for Fable to challenge: warm incremental CPU <=0.2 ms/frame and GPU <=0.5 ms/frame on the recorded test setup; report whole-frame p50/p95/p99 and VRAM as well. These are design targets, not estimates already achieved. Start with a bounded 256 MiB extra GPU texture budget and a configurable disk cache; count retained originals, all replacement maps, mip chains, staging and in-flight generations. Doubling width and height quadruples texels. Do not hide that cost behind compressed-file sizes or average FPS. Generation memory/time is reported separately.

Eventual overlay: one Material Enhancement switch, a restrained detail-strength control and clear states such as Preparing, Active on X materials, or Original material retained. Report actually sampled replacements, not merely generated files. Keep the detailed authoring controls in a preparation tool until the behaviour is established. No installer/UI project starts ahead of the visible prototype.

## 9. Stages, decisions and effort

These are rough focused engineering estimates after agreement, not promises of unattended calendar completion. Revise after the first assets/binding path are known. AI model availability/training and unfamiliar shader packing are the main uncertainty. No runtime implementation begins from this planning request alone.

| Stage | Deliverable and decision | Proposed owner | Rough effort / Cort attendance |
|---|---|---|---|
| M0: agree one design | Fable compares his proposal against this one, selects the first material path, generator experiment, cost target and file ownership | Both; Fable replies in COMMS | One focused review exchange, roughly 1–3 hours of analysis; no game run |
| M1: show the visual idea | Six material examples in a controlled viewer; original vs conventional upscale vs our coordinated detail, static/moving camera and changing light. Select the generator by visible results | Astra | 1–3 working days if suitable models/data are available; Cort can judge a short clip, no installation |
| M2: reach actual game shading | One prepared enhanced material set reaches a real sampled binding, and normal/roughness changes visibly affect the final image. Original files and existing features preserved | Fable | 1–3 working days for first bounded binding path; one combined user session only after the fixture proves recorder and replacement |
| M3: automate and persist | Observed material selection, stable identity, preparation/cache, reset/reload and coherent publication. One owner per shared file | Fable runtime; Astra generator/cache format | 2–4 working days after M2; reuse that game's evidence |
| M4: demonstrate transfer | Same generator and material contract on a second renderer/binding layout, including the other API; list any small adapter | Fable integration, Astra quality/regressions | 1–3 working days if ordinary texture bindings; Cort tests the combined candidate |

M1 and bounded M2 fixture work can proceed concurrently after approval. If generation looks ordinary, focus the algorithm before building M3. If delivery is unproven, use a tiny known replacement to distinguish wrong binding from weak enhancement before asking for a game run. If normal/roughness maps cannot be used, select an eligible material rather than quietly downgrading the promised demonstration to a sharpened colour texture. General shader rewriting, complete relighting, displacement geometry and a full material-authoring application are not prerequisites for this first deliverable.

## 10. Evidence that answers the right questions

- **Appearance:** A/B at ordinary viewing size and in motion, with identical exposure, camera, resolution, DLAA, mip/AF, RCAS and effects settings. Include grazing light to show surface response. A normal-map-only and colour-only comparison separates actual material improvement from extra sharpening. Cort decides whether the result achieves the intended visual impact.
- **Faithfulness:** Existing pattern, lettering, colours, edges and material identity remain; detailed additions do not invent damage patterns or metalness indiscriminately. Evaluate held-out examples as well as selected showcase material.
- **Consistency:** Same accepted texture hash/version after turn-away/return, mip transitions, load/reload and another session. Check visible texture crawl/specular sparkle as well as identity: fixed textures alone do not guarantee stable filtering.
- **Delivery:** Trace actual sampled resource/view through the shaded scene to final pixels. A debug fixture's distinct material marker proves the observation and replacement before the prettier generated material is used. Do not replay an arbitrary live game frame twice or assume DLAA capture includes upstream material textures.
- **Regression/cost:** Existing Route A and B rendering unchanged when disabled; no replacement of HUD, lookup/data textures or unrelated packed channels; resource lifetime/reset/streaming tests on the selected paths; measure warmed gameplay with capture writer idle. Counters describe scope, not visual success.

For a requested game session, package the A/B controls and capture together. The run must distinguish no replacement, wrong role/packing, genuine-but-weak enhancement, and enhancement lost in later processing. Validate those observations in the fixture first. Use Cort's existing game-testing authority; no new permission inferred from this plan.

## 11. Proposed collaboration split and Fable's requested response

One final design and one implementation line. Fable owns the first binding/renderer feasibility decision and runtime resource/publication work. Astra owns the coordinated-detail algorithm experiment, fidelity/mip rules and appearance/cost comparisons. Agree the material record and cache format together before either writes production code. Nobody edits the same shared hook file concurrently. This split is proposed, not an assignment already accepted by Fable.

Fable, please post your independent proposal and then answer these points by accepting, modifying or replacing them with a concrete alternative:

1. What part would make Cort say "wow" and what part is materially distinct from plain AI upscaling/RTX Remix? Identify overlap rather than merely calling the system novel.
2. Which first game's actual material channels and binding path can we use? Existing evidence may select it; a resource of a plausible format is not proof of a material role. WWII is convenient but not mandated.
3. Does a shared fine-detail description across colour/normal/roughness look like the best generator target, or is there a simpler way with equal visual gain? Name baseline and comparison.
4. What existing source mechanism handles the first DX11 publication and first DX12 publication, and what must be added? Include reused recordings and persistent/bindless tables where relevant.
5. Do you prefer pre-generation/loading-time preparation for the first build? Identify any concrete advantage of live generation that repays its contention and complexity.
6. Are the performance/memory targets and first-demo scope sensible? Offer revised values and a reason rather than making the feature unavailable everywhere.
7. What would you change in the proposed ownership, stages or model/data assumptions? Reply with one combined recommendation after comparing our proposals.

Astra will review the response against the visible goal, the source and these alternatives, and fold accepted corrections into this document with a short decision record. Do not build two competing full stacks, add an outside review round, or treat a request for review as agreement already received.

## Decision record

- 2026-11-14: Cort requested Astra's plan in COMMS and independently asked Fable for a proposal; both are to collaborate on the best combined direction.
- 2026-11-14: Initial proposal prepared after source reuse inspection and primary-source comparison. No material-specific Fable proposal was present in central COMMS at the initial read. All performance, compatibility, novelty and implementation claims above remain proposal/targets unless explicitly identified as existing source behaviour.
