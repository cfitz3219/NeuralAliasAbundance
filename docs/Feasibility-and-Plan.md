# NeuralAliasAbundance: feasibility and implementation proposal

Prepared September 22, 2026. Planning only; implementation awaits the user's go-ahead.

## Follow-up: narrower DX12/native-DLSS starting point

User subsequently proposes targeting DX12 games that already implement DLSS, intercepting their existing inputs and adding neural rendering. This is a better bounded starting point than arbitrary DX11 support. The main simplification comes from existing DLSS integration, not DX12 alone. A DX12 game without those inputs retains much of the discovery problem.

Recommended first target: one owned, mod-compatible x64 DX12 game with working DLSS Super Resolution. Initially preserve native SR and its quality settings; observe the actual NGX/Streamline evaluation calls, validate frame metadata and resource lifetimes, and add NR at a verified compatible scene stage. Do not infer working support from DLL presence alone. A clean SR input/output is not automatically the correct color stage for the NR runtime: tone mapping, exposure, UI and later effects still need qualification. Frame Generation should initially be disabled until the NR ordering and real/generated-frame relationship are verified.

An in-process DX12 implementation can avoid Alien's cross-architecture transport entirely while reusing evaluator logic. It still needs correct command-list/resource-state handling, descriptors, synchronization and restoration of game state. Retain the existing helper as a reference/test backend rather than forcing it into the new hot path.

Rechecked primary sources: OptiScaler documents interception of existing upscaler calls; NVIDIA's Streamline DLSS guide documents tagged resources and frame constants; NVIDIA's DLSS5 research describes its conditioning inputs but does not itself establish the runtime API contract or redistribution rights. These sources support feasibility, not a promise that SR interception alone guarantees correct NR insertion.

Revised planning estimates for this narrower experiment: 2–4 hours to inspect a selected title and confirm the interception route; roughly20–50engineeringhours total for a first playable single-game NR prototype if the current NR runtime is usable in that pipeline; approximately60–120hours total for a small tested multi-game release, excluding unforeseen NR API/image-quality problems. These are provisional estimates pending the first game inspection. No implementation, new project directory or game changes authorized by this discussion. The broader DX11 plan below remains an alternative, not a simultaneous implementation commitment.

## Recommendation

Build a separate, standalone DX11 enhancement framework with its own loader, settings window, diagnostics and optional rendering backends. Reuse the verified parts of NeuralAlias, but make Alien: Isolation an optional adapter rather than a requirement.

Current direction (2026-10-23): build a universal DX12 injector through automatic
input discovery, convention testing, reconstruction and synthesis. The September
DX11 architecture below is historical background. Semantic labels accelerate
acquisition; when absent, the injector tests candidates against geometric,
photometric and temporal evidence. See [the current acquisition roadmap](INPUT-ACQUISITION-ROADMAP.md).

Track implemented features and measured game coverage as milestones toward that
goal. Engine adapters can accelerate progress while automatic discovery and
profiles generalize the same capabilities across games.

## What standalone means

The planned package has a normal program for configuration and installation, a small DLL loaded into the selected game, an in-game overlay, and an optional 64-bit rendering helper. A DLL is a library that runs inside another program. The helper is a separate process used where architecture or backend requirements justify it.

The manager and our own graphics test application run without Alien or ReShade. Enhancing another game's rendering still requires the runtime to load into that game. A separate desktop window alone cannot reliably obtain all its internal graphics data.

Proposed source location after approval: `outputs/NeuralAliasAbundance/`, alongside—not inside—the existing `dx11-enhancement` project. It gets independent source files, build outputs, configuration, documentation and version history. No editable shared links to the working Alien installation. Preserve a provenance manifest identifying the source snapshot and reused files. Exclude private captures, machine-specific deployment records, obsolete binaries and unlicensed redistributables from the new public source tree.

## What we can reuse

| Existing component | Treatment in the new project |
|---|---|
| DX12 DLAA/DLSS evaluator and combined neural evaluation | Extract behind a backend interface; retain regression tests |
| 32-to-64-bit texture transport, GPU fences and frame identity checks | Reuse after removing deployment assumptions; keep lifetime and cancellation tests |
| CPU constant-buffer upload cache | Reuse; replace ReShade event sources with native DX11 hooks |
| Jitter sequence and temporal history utilities | Reuse the mathematics; camera insertion remains provider-specific |
| GPU timing and bridge diagnostics | Reuse with standalone logging and UI |
| Alien shader identities, camera layout, render-size patches and pass selection | Keep only in an optional Alien adapter |
| ReShade registration, event callbacks, settings and shader-control interface | Replace with our own host, overlay and configuration |
| Experimental neural runtime integration | Isolate as optional research functionality; requalify inputs, controls and distribution rights |

The existing project already supplies its own jitter and no longer requires Alias Isolation. It still requires ReShade as the host. Existing tests establish behavior for our current adapter and test scenes, not arbitrary games. The earlier neural-noise complaint remains unresolved as an image-quality qualification.

## Architecture to build

1. **Loader and DX11 host.** Begin with a local system proxy such as `dxgi.dll`, forwarding the genuine Windows functions while observing device and swapchain creation. A swapchain is the set of images a game presents to the screen. Add other loading routes only when a tested game's startup requires them. Never overwrite an unrelated proxy automatically.
2. **Native event tracking.** Replace ReShade's notifications with hooks into the relevant DirectX calls. MinHook can redirect functions, but does not itself understand resource lifetimes, contexts or rendering passes. We must handle interface versions, object destruction, immediate/deferred contexts, draw/dispatch calls, buffer updates, command-list execution, resize and presentation.
3. **Frame description.** Each data provider supplies a versioned description of color, depth, motion, dimensions, color encoding, jitter, reset state, frame identity and resource lifetime. Optional fields are explicitly unknown or unnecessary under the selected feature contract; they are never silently fabricated.
4. **Rendering backends.** Keep the existing combined helper path first. Preserve the x86-to-x64 bridge for older games. Evaluate an in-process x64 backend later where it reduces overhead; do not force every modern game through IPC merely because Alien needed it. IPC means communication between separate programs.
5. **Standalone controls.** Use full Dear ImGui sources and its Windows/DX11 rendering backend. Add persistent per-game settings, shortcuts, capability explanations, reset-to-default and exportable diagnostic reports. Ordinary users should not edit shader preprocessor definitions.
6. **Manager and profiles.** Detect executable architecture, select a tested profile, install with a manifest and restore only files we own. Profiles record supported game builds and feature status. No global injection service is required for the initial release.

DX12 inside the helper is not the same as supporting DX12 games. Native DX12 game interception needs its own host and resource-state tracking and belongs in a later milestone. The shared interfaces should allow it without advertising it as implemented.

## How to obtain the rendering data

Use these providers in descending order of confidence, independently for each feature:

| Provider | Where useful | Main limitation |
|---|---|---|
| Existing upscaler interface interception | Games already passing buffers through a supported DLSS/FSR/XeSS/Streamline interface | APIs and versions vary; the game must actually use that interface |
| Engine or game adapter | Titles with identifiable camera, depth, motion and clean scene passes | Discovery and validation work; updates can change signatures |
| Diagnostic-assisted resource discovery | Finding likely buffers and shader stages in an unfamiliar game | A plausible image is evidence to investigate, not proof of correct semantics |
| Experimental optical flow | Estimating screen-space movement between images when engine vectors are unavailable | Does not provide camera jitter, depth, clean HUD separation or engine resolution control |

An `nvngx.dll` proxy is one interception route, not a universal source of engine buffers. A game that never calls that interface will not suddenly provide depth and motion to it. OptiScaler's documented scope likewise starts with existing upscaler integrations. [OptiScaler](https://github.com/OptiScaler/OptiScaler)

For DLAA/SR we must validate the selected feature's inputs and conventions. NVIDIA documents depth, motion, input/output color, matching frame data, motion scaling, jitter and exposure handling; documented auto-exposure is an available policy, not permission to invent exposure data. SR additionally needs a genuine smaller internal rendering resolution. Merely shrinking a finished screenshot does not save the game's rendering work. [NVIDIA DLSS integration guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS.md)

Our implementation must also establish the following for each applicable adapter:

- Where to inject subpixel camera offsets without moving UI, shadows or unrelated cameras, and how those offsets affect existing motion vectors.
- How to replace or cooperate with the game's existing temporal anti-aliasing rather than accidentally stacking incompatible passes.
- How to detect cuts, loading, camera changes and resource recreation. Matrix discontinuities are useful heuristics, not guaranteed semantic camera-cut flags.
- Where the clean scene exists before HUD composition, with its actual SDR/HDR encoding. Cleared depth is not a universal UI mask: UI can avoid depth writes and sky can have cleared depth.
- How to resize the scene while retaining correct display-size UI and post-processing, then restore original rendering when disabled.

NVIDIA's optical-flow API provides image-motion estimation and exposes capability queries. We need an actual initialization and format/grid capability probe on this RTX 5070 Ti before selecting a path. Driver DLL exports alone do not prove working support. Then test vector direction, scale, occlusions, transparencies, camera motion and HUD contamination against known reference motion. Success would qualify an experimental fallback, not equivalence to engine vectors. [NVOFA programming guide](https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-programming-guide/index.html)

## Feature tiers and user controls

The overlay should distinguish: host loaded; data identified; data validated; feature producing current frames. Unsupported features stay unavailable with a short reason.

- **Basic host:** settings, diagnostics, timing and optional ordinary image processing. No promise of temporal reconstruction.
- **Validated reconstruction:** DLAA plus supported SR modes, runtime-supported model presets, texture sampling adjustment where applicable, reset control and input/output resolution display.
- **Experimental motion fallback:** separately labeled optical-flow mode after validation, with provider identity visible.
- **Experimental neural rendering:** separate switch, measured cost and only controls whose behavior is verified for the loaded runtime. No decorative model selections that produce identical output.

NVIDIA's published DLSS 5 research describes conditioning on the rendered frame, engine motion, temporal state and artistic controls. That is not a complete public integration contract for our DLL. The research does not justify transferring every DLSS SR assumption to NR. Verify the exact NR API, supported inputs and runtime terms separately before calling this an officially supported integration. [NVIDIA DLSS 5 research](https://research.nvidia.com/labs/adlr/DLSS5/)

## Similar projects and lessons

| Project | Useful lesson | Foundation decision |
|---|---|---|
| [Luma Framework](https://github.com/Filoppi/Luma-Framework) | Per-game shader work, resource analysis and reusable mod organization | Uses ReShade add-ons; not a direct route to removing that requirement |
| [OptiScaler](https://github.com/OptiScaler/OptiScaler) | Intercept known upscaler inputs and translate between backends | Useful provider design; does not solve arbitrary legacy-engine data discovery |
| [Special K](https://github.com/SpecialKO/SpecialK) | Independent injection payloads and separation from a frontend | Useful host/manager architecture reference; no need to absorb an entire mature project |
| [MinHook](https://github.com/TsudaKageyu/minhook) | Small x86/x64 function-hooking library | Candidate dependency, with pinned version and notices |
| [Dear ImGui](https://github.com/ocornut/imgui) | Embedded native settings/debug UI | Candidate dependency; replace current ReShade-facing headers with a complete integration |
| [RTX Remix](https://docs.omniverse.nvidia.com/kit/docs/rtx_remix/1.2.4/docs/introduction/intro-compatibility.html) | Separation of runtime, content and compatibility work | Official compatibility centers on DX8/9 fixed-function games; not a drop-in DX11 renderer |

The recommended foundation is our own small host plus reusable NeuralAlias components. Source reuse requires a file-level license review: Luma has a custom MIT-style license with attribution and commercial-use conditions; OptiScaler and Special K identify GPL-3.0 licensing. MinHook uses BSD licensing and ImGui MIT. Studying architecture and copying implementation are different decisions. Preserve notices and determine the new project's compatible license before importing code. [Luma license](https://raw.githubusercontent.com/Filoppi/Luma-Framework/main/LICENSE.md), [OptiScaler license](https://raw.githubusercontent.com/OptiScaler/OptiScaler/master/LICENSE), [Special K repository](https://github.com/SpecialKO/SpecialK), [MinHook](https://github.com/TsudaKageyu/minhook), [ImGui license](https://github.com/ocornut/imgui/blob/master/LICENSE.txt)

True Remix-style replacement of lighting/materials with a new ray-traced renderer requires scene geometry, materials and lights, not merely a final image and motion vectors. Keep this as a separate research roadmap. Neither DLSS SR nor adding an injection DLL automatically provides path tracing.

## Requirements and what I will need

| Requirement | Current position / action after approval |
|---|---|
| Windows C++ toolchain, CMake, Windows SDK, x86/x64 builds | Existing project has successful builds; reuse and record exact installed versions in the new manifest |
| Existing code and test evidence | Available locally; freeze and inventory an independent source snapshot |
| MinHook and full Dear ImGui | Fetch official pinned sources and preserve licenses; not installed for this proposal |
| NVIDIA DLSS development/runtime files | Audit existing files, official terms and supported interfaces; do not publish a mirror-downloaded DLL merely because it is signed |
| Optical Flow SDK | Obtain official SDK after approval if needed, check terms and run a real capability probe |
| Debugging and GPU profiling | Start with our own test host, D3D debug layer and timestamp measurements; add a graphics debugger/profiler only when a concrete investigation needs it |
| Test games | Later select two or three owned DX11 games from different engines, preferably including one with native upscaler inputs and one legacy title; no new purchase required to start |
| Test time on your RTX 5070 Ti | Automated synthetic tests first; short, bundled in-game visual checks later |
| Publication materials | Reproducible build instructions, dependency notices, compatibility table, limitations, uninstall instructions and sanitized evidence |

No new hardware is necessary to begin. Additional testers/hardware will eventually be necessary to support claims beyond this machine. Initial development targets games that allow local mod loading; protected titles may reject injection and are not a promised compatibility tier.

## Performance and reliability requirements

Preserve the corrected architecture: capture CPU uploads before submission rather than synchronously reading GPU buffers mid-draw; use GPU fences and explicit ownership; keep source-frame identity and generation checks; combine dependent reconstruction/NR work where suitable. Never return an old frame merely to make the FPS counter look better.

Avoid deliberate game-thread completion waits, but do not claim that GPU dependencies or submission cost disappear. A current-frame effect must finish before its result can be consumed. In the existing fixed-scene measurements, NR still added roughly 10 ms of GPU work. Removing ReShade will not automatically remove that cost, and those measurements are not a universal forecast.

Test resize, alt-tab, fullscreen changes, device loss, multiple swapchains, deferred contexts, resource reuse, formats, MSAA, reverse depth, sRGB/HDR, startup/shutdown, helper failure and disabling effects. Restore the game's graphics state after injected work. Guard against recursive hooks and avoid initialization under Windows' DLL loader lock. Forward system exports correctly for each architecture.

Benchmark bypass overhead, CPU frame time, GPU feature time, memory use, frame pacing and missed outputs. Compare identical scenes with effects off/on, include warm-up and changing motion, and keep quantitative transport correctness separate from visual quality. Set performance budgets from the first standalone baseline instead of promising an unmeasured FPS gain.

## Milestones and rough effort

These are engineering-work estimates, not guaranteed elapsed completion times. Unknown engine layouts can push an adapter outside its range. We can work in independently reviewable stages.

| Stage | Deliverable and acceptance test | Estimate |
|---|---|---|
| 1. Independent snapshot and test application | New project builds without ReShade/Alien; synthetic scene supplies known depth, motion, jitter and UI | 4–8 hours |
| 2. Standalone host and overlay | x86/x64 proxy forwards correctly; settings, resize, teardown and clean bypass pass in our test application | 16–32 hours |
| 3. Core/backend extraction | Existing reconstruction and transport tests run through the new host; no ReShade headers/runtime dependency | 16–32 hours |
| 4. First non-Alien game | A selected game's provider supplies validated data; DLAA/SR and HUD behavior pass where supported | 24–60 hours |
| 5. Broader input-provider support | Add a selected native-upscaler interface and at least one additional engine/profile | 24–60 hours |
| 6. Optical-flow feasibility experiment | Capability probe and reference tests yield an explicit usable/not-usable result for the selected fallback | 16–40 hours |
| 7. Release hardening and manager | Installer/rollback, compatibility matrix, error handling, documentation and sustained testing | 32–80 hours |

Stages 1–4 total about **60–132 hours** for a standalone framework and a first qualified non-Alien integration. All listed stages total about **132–312 hours**, with continued per-game compatibility work afterward. Think weeks of development and testing rather than one long session. This includes neither a general DX12 host nor a new path-traced renderer. The earlier 20–40 hour discussion concerned a narrower Alien-only host migration.

NR API qualification and image-quality repair remain a separate uncertain workstream; they must not block delivery of a working standalone DLAA/SR core. Optical-flow research may fail its quality bar even if the API works, and that is a legitimate milestone result.

## First work after your go-ahead

Create the separate project and source-provenance inventory, then build a small moving 3D test scene with known inputs and a HUD. Establish the standalone proxy and overlay against that scene before installing anything into another game. This gives us controlled tests without repeatedly restarting Alien and proves independence early.

The current Alien mod remains its own working project. No new project folder, source migration, dependency installation or game deployment has been performed for this proposal. Only this report and the existing action/handoff documentation are updated.
