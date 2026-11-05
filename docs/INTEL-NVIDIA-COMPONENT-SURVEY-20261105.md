# Intel and NVIDIA components worth considering for NeuralAlias

Author: Astra (Codex), 2026-11-05. Research only; no runtime, installer or game changes.

## What was examined

The public [Intel GameTechDev catalogue](https://github.com/GameTechDev) returned 69 repositories, including its website and organization configuration. I examined the catalogue metadata, available project READMEs and, where those were absent, the source-tree contents. This is a feasibility survey of every catalogue entry, not a line-by-line audit or a claim that any unintegrated effect has been tested in our injector. NVIDIA coverage is a targeted selection relevant to the same goals, not all NVIDIA repositories.

Raw inventory and downloaded references are retained locally under `local/vendor-survey-20261105/`. Intel descriptions below are paraphrased. Recommendations are my assessment against our existing DLSS/DLAA, motion/flow, RCAS, mip/AF, HUD, resolution and XeGTAO machinery.

## Best opportunities

| Project | What the player or developer gains | What I recommend borrowing |
|---|---|---|
| [NVIDIA Donut](https://github.com/NVIDIA-RTX/Donut) | A ready source of bloom, tone mapping, SSAO and temporal-AA implementations. | Individual post-processing shaders and their supporting math. Keep NeuralAlias's renderer, ownership and insertion system. Bloom could be an optional visible effect; tone mapping needs a known colour space and placement so it does not remap an already mapped picture twice. Donut is MIT licensed. |
| [Intel PresentMon](https://github.com/GameTechDev/PresentMon) | Better evidence about frame pacing, CPU/GPU duration and display delivery across GPU vendors. | Use its existing capture tools or supported data interface for measurement before writing another timing system. It measures problems; it does not remove overhead automatically. Avoid a second presentation hook just to display FPS. |
| [Intel MetricsGui](https://github.com/GameTechDev/MetricsGui) | Readable performance histories inside an ImGui interface. | Small graph controls supplied with measurements from our existing timing system. This is a presentation library, not the source of the GPU measurements. |
| [Intel TAA](https://github.com/GameTechDev/TAA) | Concrete reference for rejecting stale history, sampling sharp history and handling newly exposed surfaces. | Study depth-based rejection, variance clipping, bicubic history sampling and precision choices for temporal work we own. DLSS's internal history algorithm is not exposed, so we cannot simply transplant these improvements inside DLSS. Do not add another TAA pass after DLAA by default. |
| [NVIDIA NVRHI](https://github.com/NVIDIA-RTX/NVRHI) | Established resource-state, upload-buffer and submitted-resource-lifetime patterns. | Small, measured ideas for our own backend. MIT licensed. A wholesale backend migration is unnecessary; it also cannot automatically understand work that the game records outside it. |
| [Intel CMAA2](https://github.com/GameTechDev/CMAA2) | A spatial anti-aliasing alternative that does not depend on motion or temporal history. | A possible optional fallback when temporal AA is unavailable or unwanted; also useful edge classification. It is not a replacement for obtaining correct DLSS inputs, and its edge map cannot by itself identify HUD text. |
| [Intel DynamicCheckerboardRendering](https://github.com/GameTechDev/DynamicCheckerboardRendering) and [DynamicResolutionRendering](https://github.com/GameTechDev/DynamicResolutionRendering) | Examples of coordinating smaller scene rendering with later full-size processing. | Subrect conventions, resource sizing and dynamic-resolution control ideas. They operate inside their own renderers; they do not solve locating and changing arbitrary game passes for us. |
| [Intel VALAR](https://github.com/GameTechDev/VALAR), [VALAR-API](https://github.com/GameTechDev/VALAR-API), [VRS-DoF](https://github.com/GameTechDev/VRS-DoF) | Potential reduction of pixel-shading work where the image is less sensitive to it. | Later, borrow the luminance/motion sensitivity classifier for selected world passes. Preserve fine geometry, UI and unsuitable passes. VRS reduces GPU shading, so it does not repair our CPU interception bottleneck. |

XeGTAO is already integrated on `codex/xegtao-enhancement`, commit `0e23732`, and handed to Opus for the combined package. It is not a new proposal here. RCAS, AMD optical flow and NIS also already exist in our source; this survey is not permission to stack duplicate filters.

## NVIDIA: genuine open source and other public SDK source

| Project | Source/licence distinction | Fit for NeuralAlias |
|---|---|---|
| [NVIDIA Image Scaling](https://github.com/NVIDIAGameWorks/NVIDIAImageScaling) | MIT. Spatial scaler and adaptive directional sharpening shader source. | Already in our source. Its directional edge/contrast logic is worth comparing with RCAS for selective improvements or a choice of sharpening methods. Do not revive the rejected NIS-on-RCAS stack. Its documented colour-space/placement requirements matter. |
| [Donut](https://github.com/NVIDIA-RTX/Donut) | MIT, with separate dependencies. | Best new source of practical individual rendering passes. Includes bloom, adaptive tone mapping, temporal AA and SSAO. Use components, not its whole scene framework. |
| [NVRHI](https://github.com/NVIDIA-RTX/NVRHI) | MIT. Backend abstraction for DX11, DX12 and Vulkan. | Useful engineering reference for barriers, deferred destruction, resource bindings and parallel recording. It does not turn our existing DX12 game interception into a DX11 injector merely by linking it. |
| [Falcor](https://github.com/NVIDIAGameWorks/Falcor) | Core uses a permissive BSD-style licence; linked RTX SDKs have their own terms. | Larger research collection with modular rendering and post-processing. Useful individual passes and test references. Donut is the more focused first stop for a small enhancement. No replacement of our core is proposed. |
| [NRD](https://github.com/NVIDIA-RTX/NRD) | Shader/library source is public; the current [licence](https://github.com/NVIDIA-RTX/NRD/blob/master/LICENSE.txt) is NVIDIA's RTX SDK licence, not MIT. | Sophisticated denoising, reprojection, history validation and edge-aware filtering reference. Its normal/roughness/depth/motion and noisy-lighting input contracts make it different from a generic final-image cleaner. Applying the full denoiser to finished game colour would not be a justified DLSS-blur fix. Individual mechanisms may help temporal effects we own. |
| [HBAO+](https://github.com/NVIDIAGameWorks/HBAOPlus) | Public source under NVIDIA's Source Code License (1-Way Commercial), rather than MIT. | Real AO alternative with DX11/DX12 entry points, depth/projection inputs and optional normals. Cache-friendly interleaving and depth-edge behaviour are useful references. It substantially overlaps our new XeGTAO; do not stack both by default. |
| [Spatiotemporal Blue Noise](https://github.com/NVIDIA-RTX/STBN) | Public generator and textures. [Licence](https://github.com/NVIDIA-RTX/STBN/blob/main/License.txt) contains separate non-commercial and commercial terms; it is not a plain MIT package. | Useful sample distributions for stochastic effects. Changing noise every frame only helps if the downstream accumulation is appropriate; it can otherwise add visible sparkle. This does not justify replacing our DLSS projection jitter sequence. |
| [Streamline](https://github.com/NVIDIA-RTX/Streamline) | Public integration framework and much plugin source. NVIDIA-specific licence; the DLSS-G plugin remains prebuilt. | Already part of our integration/reference machinery. Useful official implementation patterns for frame/resource tagging and feature interaction. The open integration code does not expose the DLSS neural implementation. |
| [RTX Texture Filtering](https://github.com/NVIDIA-RTX/RTXTF) | Public SDK source under its supplied terms. | Interesting stochastic material filtering, accumulated across frames. It changes texture sampling within material shading; it is not a more powerful version of our sampler-only AF switch or a finished-image sharpening pass. Possible later research, not a small add-on. |
| [RTXDI](https://github.com/NVIDIA-RTX/RTXDI) and [RTXGI](https://github.com/NVIDIA-RTX/RTXGI) | Public SDK sources with NVIDIA-specific terms and dependencies. | Real lighting technology, but it requires renderer/scene/lighting integration beyond colour/depth post-processing. ReSTIR sampling ideas are valuable; adding genuine replacement lighting is a separate project, not an enhancement toggle we can honestly promise now. |

Public availability and open-source licensing are different questions. The clearest permissive NVIDIA starting points here are Donut, NVRHI, NIS and Falcor's own core. This is a record of the licences observed for the current sources, not a legal opinion or approval to copy arbitrary dependencies.

## Every Intel catalogue entry

“Borrow” means a useful portion or reference. “Tool” means useful for development/measurement. “Later” means plausible engineering work with additional inputs. “Skip” means it does not address our current product goals. Archived status does not invalidate an algorithm; it does mean maintenance becomes ours if adopted.

| Repository | Assessment for our project |
|---|---|
| [.github](https://github.com/GameTechDev/.github) | Skip: organization configuration, not a graphics effect. |
| [64-bit-Typed-Atomics-Extension](https://github.com/GameTechDev/64-bit-Typed-Atomics-Extension) | Skip now: Intel-specific shader extension example; not a general NVIDIA enhancement. |
| [AdaptiveSync](https://github.com/GameTechDev/AdaptiveSync) | Borrow later: presentation/VRR capability information; not an algorithm that fixes DLSS quality or CPU overhead. The README says its extra library is not required just to use Intel adaptive sync. |
| [AOIT-Update](https://github.com/GameTechDev/AOIT-Update) | Later: transparency-order rendering technique. Requires participation in transparent-object rendering, not just the final image. Cannot generally reconstruct all transparent layers after they were blended. |
| [ASSAO](https://github.com/GameTechDev/ASSAO) | Borrow only: older AO implementation; upstream explicitly points to XeGTAO as its successor. No need for a second default AO effect. |
| [asteroids_d3d12](https://github.com/GameTechDev/asteroids_d3d12) | Borrow: command recording and resource-management patterns from a DX12 renderer. Engine-side performance examples, not a ready injector optimization. |
| [ChatHeads](https://github.com/GameTechDev/ChatHeads) | Skip: RealSense webcam segmentation/social-video sample. |
| [CloudsGPUPro6](https://github.com/GameTechDev/CloudsGPUPro6) | Later/research: cloud rendering with Intel pixel ordering. Needs its own cloud representation and rendering; does not remove trails from existing game clouds. Sample media have separate restrictions. |
| [CloudySky](https://github.com/GameTechDev/CloudySky) | Later/research: another cloud-rendering sample with similar engine/input limitations. Not a general sky repair. |
| [ClusteredShadingAndroid](https://github.com/GameTechDev/ClusteredShadingAndroid) | Skip now: clustered lights in an Android renderer; needs scene lighting/materials. |
| [CMAA2](https://github.com/GameTechDev/CMAA2) | Borrow/optional effect: non-temporal AA fallback and edge classification, implemented with DX11/DX12 compute. Avoid stacking automatically over DLAA. |
| [CmdThrottlePolicy](https://github.com/GameTechDev/CmdThrottlePolicy) | Tool/reference for Intel driver extension only. It is not a portable remedy for CPU-side descriptor contention. |
| [CPU_Capability_Tester](https://github.com/GameTechDev/CPU_Capability_Tester) | Borrow selectively: CPU-capability detection for safe SIMD dispatch if needed. Existing system detection should be reused first. |
| [D3D12VariableRateShading](https://github.com/GameTechDev/D3D12VariableRateShading) | Later/reference: basic per-draw VRS. Need per-pass suitability and hardware support; no universal FPS gain claim. |
| [d3dext-sdk](https://github.com/GameTechDev/d3dext-sdk) | Later for Intel support: graphics extensions including useful capability/diagnostic interfaces. Not a vendor-neutral effect library. |
| [DeferredCoarsePixelShading](https://github.com/GameTechDev/DeferredCoarsePixelShading) | Later/research: reduced shading frequency inside a deferred renderer; substantially overlaps the VRS performance idea. Requires renderer participation. |
| [DX12-Multi-Adapter](https://github.com/GameTechDev/DX12-Multi-Adapter) | Skip now: split-frame multi-GPU renderer. Transfers and ownership make this a separate project, not free spare-iGPU performance. |
| [dxvk-igdext](https://github.com/GameTechDev/dxvk-igdext) | Skip for our Windows DX12 path: Intel extension forwarding for Wine/DXVK. |
| [DynamicCheckerboardRendering](https://github.com/GameTechDev/DynamicCheckerboardRendering) | Borrow: coordinated resolution/subrect and reconstruction examples. Its checkerboard renderer is not a drop-in substitute for our DLSS pipeline. |
| [DynamicResolutionRendering](https://github.com/GameTechDev/DynamicResolutionRendering) | Borrow: older engine-owned dynamic-resolution sample and timing/control ideas. Does not discover arbitrary game draw dependencies for us. |
| [EZSIMD](https://github.com/GameTechDev/EZSIMD) | Borrow selectively: SIMD-related utility/reference code. Only pursue a measured numerical CPU hotspot; not a fix for waiting on locks. |
| [FaceMapping](https://github.com/GameTechDev/FaceMapping) | Skip: RealSense face scans and mesh mapping. |
| [FaceMapping2](https://github.com/GameTechDev/FaceMapping2) | Skip: improved RealSense face/mesh sample. |
| [FaceTracking](https://github.com/GameTechDev/FaceTracking) | Skip: face tracking/animation sample. |
| [FlipModelD3D12](https://github.com/GameTechDev/FlipModelD3D12) | Tool/reference: swapchain/presentation demonstration. Useful for understanding fullscreen/borderless behaviour, but not evidence of a fix in our installed package. |
| [gametechdev.github.io](https://github.com/GameTechDev/gametechdev.github.io) | Index/documentation, not an effect. |
| [gpudetect](https://github.com/GameTechDev/gpudetect) | Borrow: device/capability enumeration and quality categorization. Prefer current capability queries over old device-ID performance tables. |
| [GrassInstancing](https://github.com/GameTechDev/GrassInstancing) | Skip: rendering grass geometry efficiently requires owning that geometry/rendering path. It cannot repair existing leaves by processing the finished frame. |
| [GTS-GamesTaskScheduler](https://github.com/GameTechDev/GTS-GamesTaskScheduler) | Borrow/reference: task scheduling and reduced worker overhead. Our lock/contention evidence does not justify replacing all threading with another scheduler. |
| [HybridDetect](https://github.com/GameTechDev/HybridDetect) | Borrow selectively: core topology information, potentially helpful for background shader compilation/capture workers. Measure before forcing affinity on game threads. |
| [InstantAccess_Tiling](https://github.com/GameTechDev/InstantAccess_Tiling) | Old texture-access/tiling renderer sample. Low priority for an RTX DX12 injector; it does not give access to higher-detail source assets the game never loaded. |
| [Intel-Texture-Works-Plugin](https://github.com/GameTechDev/Intel-Texture-Works-Plugin) | Tool: Photoshop DDS texture-compression workflow. Could help author our own assets; not a live image enhancer. |
| [IntelAIGamingAssistantLibrary](https://github.com/GameTechDev/IntelAIGamingAssistantLibrary) | Skip: local AI gameplay assistant, screenshot queries and retrieval. It is not a temporal upscaler or low-overhead render-graph detector. |
| [IntelShaderAnalyzer](https://github.com/GameTechDev/IntelShaderAnalyzer) | Tool for Intel shader ISA/performance work. For our RTX shader bottlenecks use the corresponding NVIDIA tools, not Intel ISA as a performance prediction. |
| [IntroductionToVulkan](https://github.com/GameTechDev/IntroductionToVulkan) | Reference for a future Vulkan backend. Not directly useful as an enhancement in the current DX12 injector. |
| [ISPC-DirectX-Graphics-Samples](https://github.com/GameTechDev/ISPC-DirectX-Graphics-Samples) | Borrow: vectorized CPU computation examples. Useful only when measured work is arithmetic rather than synchronization or memory tracking. |
| [ISPCTextureCompressor](https://github.com/GameTechDev/ISPCTextureCompressor) | Tool: fast asset compression including BC formats. Potential build/capture-storage utility; does not add texture detail to game assets. |
| [LightScattering](https://github.com/GameTechDev/LightScattering) | Later/research: light-shaft sampling, lookup tables and ray-march acceleration. Full effect needs actual lights/shadow visibility; image/depth alone does not supply all of that. |
| [MaskedOcclusionCulling](https://github.com/GameTechDev/MaskedOcclusionCulling) | Skip for injection now: fast CPU geometry visibility testing. Needs geometry/occluders and game draw-list integration; cannot safely omit arbitrary intercepted draws from depth guesses. |
| [MetricsDiscoveryHelper](https://github.com/GameTechDev/MetricsDiscoveryHelper) | Tool: Intel hardware-counter collection. Useful for Intel GPU support; PresentMon is the broader first choice for frame timing. |
| [MetricsGui](https://github.com/GameTechDev/MetricsGui) | Borrow: small ImGui graph controls. Reuse current timing counters; no second instrumentation system required. |
| [Multi-Adapter-Particles](https://github.com/GameTechDev/Multi-Adapter-Particles) | Skip now: custom iGPU/dGPU particle simulation split; engine-owned work and inter-device transfers. |
| [NNERuntimeOpenVINO](https://github.com/GameTechDev/NNERuntimeOpenVINO) | Skip now: Unreal developer plugin for neural inference, not an automatic DLSS/NR replacement for shipped games. |
| [OcclusionCulling](https://github.com/GameTechDev/OcclusionCulling) | Skip for injection now: older CPU scene-visibility renderer, with MaskedOcclusionCulling comparison. Same missing geometry/ownership requirements. |
| [OpenGL-ES-3.0-Deferred-Rendering](https://github.com/GameTechDev/OpenGL-ES-3.0-Deferred-Rendering) | Skip now: mobile/OpenGL renderer tutorial. |
| [OpenGLESTessellation](https://github.com/GameTechDev/OpenGLESTessellation) | Skip: requires changing geometry/tessellation; not a generic post-process improvement. |
| [OutdoorLightScattering](https://github.com/GameTechDev/OutdoorLightScattering) | Later/research: atmosphere and sun scattering with cascaded shadow maps. Sampling acceleration is reusable; correct world atmosphere replacement needs much more than our acquired camera/depth. |
| [PracticalVulkan](https://github.com/GameTechDev/PracticalVulkan) | Future-backend reference; no direct current DX12 enhancement. |
| [PresentMon](https://github.com/GameTechDev/PresentMon) | High-value tool: cross-vendor frame delivery and timing evidence. Prefer using its existing implementation over recreating system-level timing. |
| [RCRaceland](https://github.com/GameTechDev/RCRaceland) | Skip now: UE4.19/Substance engine sample about CPU use and richer scenes. Not a general binary-game add-on. |
| [SamplerFeedbackStreaming](https://github.com/GameTechDev/SamplerFeedbackStreaming) | Later/reference: explicit virtual-texture streaming via sampler feedback and DirectStorage. Requires texture-file/tile mapping and material/resource control; cannot transparently replace every game's streamer. |
| [SPIRV-Cross](https://github.com/GameTechDev/SPIRV-Cross) | Tool/reference: experimental SPIR-V-to-ISPC backend. Not an automatic way to move arbitrary game shaders to the CPU faster. |
| [stardust_vulkan](https://github.com/GameTechDev/stardust_vulkan) | Borrow/reference for future backend: efficient parallel command generation. A different API/sample is not a drop-in fix for our DX12 hook contention. |
| [TAA](https://github.com/GameTechDev/TAA) | High-value reference: rejection, clipping, history reconstruction and precision. Borrow owned temporal-filter mechanisms where applicable; don't double-accumulate after DLSS. |
| [TextureSetNeuralCompressionSample](https://github.com/GameTechDev/TextureSetNeuralCompressionSample) | Later/research: neural texture compression/cooperative vectors. Needs encoded source assets and material sampling changes; not a live texture-sharpness toggle. |
| [UAVOverlapSample](https://github.com/GameTechDev/UAVOverlapSample) | Intel-specific scheduling/extension sample. Do not remove required barriers from unknown game work; not a portable safe-overlap switch. |
| [UE4RealSensePlugin](https://github.com/GameTechDev/UE4RealSensePlugin) | Skip: camera/sensor integration for game developers. |
| [UE4_GPA_Plugin](https://github.com/GameTechDev/UE4_GPA_Plugin) | Tool for profiling an owned Unreal project. Does not supply engine-side instrumentation automatically in arbitrary shipped binaries. |
| [UnityPerformanceSandbox](https://github.com/GameTechDev/UnityPerformanceSandbox) | Learning fixture for Unity/Intel profiling, not runtime code we need to import. |
| [UnrealCapabilityDetect](https://github.com/GameTechDev/UnrealCapabilityDetect) | Borrow/reference for installer capability messaging; requires project/plugin integration for its intended use. Existing generic DXGI/system queries should remain primary. |
| [VALAR](https://github.com/GameTechDev/VALAR) | Later: motion/luminance-based VRS classifier, useful potential GPU optimization with pass exclusions. |
| [VALAR-API](https://github.com/GameTechDev/VALAR-API) | Later: packaged VRS control/classification interface; check actual device capabilities. Still needs integration with appropriate world passes. |
| [VRS-DoF](https://github.com/GameTechDev/VRS-DoF) | Borrow/reference: selective VRS based on blur/material sensitivity and quality comparisons. Valuable lesson: applying coarse shading indiscriminately can reduce both quality and performance. |
| [Windows-Desktop-Sensors](https://github.com/GameTechDev/Windows-Desktop-Sensors) | Skip: Windows device sensors, unrelated to DLSS inputs or enhancement effects. |
| [XeGTAO](https://github.com/GameTechDev/XeGTAO) | Already reused: upstream AO, depth pyramid, normals-from-depth and denoising. Our enhancement is optional/off by default; game quality/performance still belongs to Cort's test. |
| [XeSS-VALAR-Demo](https://github.com/GameTechDev/XeSS-VALAR-Demo) | Borrow/reference: coordinating temporal upscaling and selective coarse shading. It is an owned MiniEngine demo, not a universal injection module. |
| [XeSSInspector](https://github.com/GameTechDev/XeSSInspector) | Tool/reference: buffer visualizations, timing, frame dumps and state overrides for a game already integrating XeSS. Repository availability does not establish a reusable source-code licence for the tool. |
| [XeSSUnityPlugin](https://github.com/GameTechDev/XeSSUnityPlugin) | Borrow/reference: engine integration and matching the upscaler's inputs. Plugin source is available, but an engine plugin cannot simply be dropped into every compiled Unity game. |
| [XeSSUnrealPlugin](https://github.com/GameTechDev/XeSSUnrealPlugin) | Borrow/reference: Unreal resolution/upscaler integration and feature configuration. Useful for understanding engine behaviour; does not remove the need to locate/qualify an existing compiled game's interface. |

## Recommended order, without adding new scope

1. Keep the completed XeGTAO handoff and our existing AF/RCAS/flow machinery. Test the combined package rather than adding another AO or AA stack.
2. For visibly new enhancements, examine one Donut pass at a time. Bloom is an intelligible candidate when the insertion point and HDR inputs are available; expose optional controls rather than overwrite the game's presentation by default.
3. For cleaner existing output, compare owned history/filter operations against Intel TAA and appropriate NVIDIA NRD reference mechanisms. Name the specific defect and verify a small change; do not run a second temporal filter over everything.
4. For performance, use PresentMon plus our existing GPU timing and hook measurements. Borrow MetricsGui only if it improves the current graphs. NVRHI/GTS/SIMD ideas should address an observed bottleneck, not trigger a rewrite.
5. Consider selective VRS later when the application is GPU shading bound and world-pass classification is reliable. It is not the answer to CPU-side serialization.

Nothing in this survey authorizes implementation of these additional candidates, changes the ongoing ownership split or establishes in-game compatibility. No tests were necessary for this documentation-only change; none of the surveyed unintegrated packages was built or game-tested here.
