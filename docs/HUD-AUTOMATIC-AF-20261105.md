# Automatic HUD routes, verified filtering, and flicker review

Written by Astra for Cort's 5 November 2026 instruction. Base: Opus's package branch through70e9f13, including PR109's main-menu descriptor repair. No game launch, game control, installation or driver-setting write by Astra. Cort retains live testing. Opus retains the combined additive package/CMD.

## Four finite HUD routes

The automatic policy reuses the existing native replay and counterfactual layer machinery. These are four acquisition routes with two shared composition implementations, not four independently rebuilt renderers:

1. **Native draw replay:** the established pre-HUD/native-check route. Original game draws remain intact during automatic trials. Its original-size replay must agree with the actual final image; unsupported tail transforms/UI-only reconstructions advance to the next route. Its shadow replay now seeds the scene without evaluating NGX, consuming guides, resetting DLSS history, or recording a fake stretched presentation. Actual DLSS still runs once at Present.
2. **Direct target / whole-image copy:** capture the pre-HUD scene and native/render-size layers on the actual presented target, or transfer immutable snapshots along an exact whole-image copy. Unrelated equal-size targets cannot take ownership.
3. **Reflected shader handoff:** follow the actual image input of the final draw, including a batch containing both world colour and UI. Reuse the private descriptor/constant substitution to remove the world contribution from the counterfactual UI layers.
4. **Pixel-discovered canvas:** when final CPU-side dependency information is unavailable, compare frozen submitted canvas pictures with the current final image on the GPU. Existing asynchronous discovery requires two matching pictures to associate a source. Each subsequent frame still receives its own pixel check.

The policy does not contain a game name, executable hash, fixed address or per-game profile. Its context is the actual device, queue, input/output extents and format. Rotating back-buffer identities do not restart discovery. A real change of that context starts a new finite trial. A menu, absent reconstruction inputs or a repeated/generated presentation cannot consume the eligible-scene budget.

Each method gets at most90 eligible scene frames without successful evidence. Three positive completed GPU receipts establish the selected method. A receipt must contain nonempty HUD coverage and show that at least98% of its identified HUD pixels were removed from the reconstruction input through the verified scene/layer model. An empty image or matching world picture cannot qualify it. Eight failing nonempty HUD receipts advance to the next route; one transient mismatch does not flap a selected route. Epoch/trial tickets reject late results from another method or context. After the fourth route fails, **only HUD separation stops** until the context changes. DLSS continues using the ordinary complete image, including its HUD.

The current-frame shader check occurs before input selection and composition. Incorrect regions retain the ordinary enhanced DLSS result. The old compositor instead copied the raw game image over DLSS when its model failed, an actual mechanism for apparent on/off flicker. This correction removes that bypass. Failure to allocate a repeated-picture fork likewise drops HUD separation for that frame and continues ordinary DLSS instead of returning failure to the presenter. Diagnostic probes remain asynchronous and read only after the existing GPU completion fence; no new synchronous diagnostic readbacks or waits are added to game callbacks.

Native automatic trials do not run the legacy speculative redirect path or mirror its queries. Unsupported direct draws are observed/forwarded while the next method remains available. Source-recording leases and Present owners retain pictures, descriptors and replay resources until actual GPU retirement.

All manual HUD switches, pink HUD views and HUD NIS tuning are removed from the overlay. The temporal pixel-mask fallback is not counted as separation and is not silently enabled after the four methods fail. The overlay gives a simple trying/verified/unavailable status. The existing status log appends `hud_auto` with each method's eligible frames, receipts, passes, failures, actual HUD coverage and separated coverage.

These checks establish routing/model agreement and actual native-size replay. They do not assert that a game's low-resolution font atlas contains detail it never supplied, that an unsupported shader can be replayed, or that a fixture proves live-game visual acceptance.

## 16x filtering

The earlier driver blocker was real in its earlier measurement: both the global and game profiles selected USER/NONE_LINEAR and a completely independent literal16x D3D12 sampler produced flat gray. On this run, a fresh read-only DRS query reports selector0/APP on both global and sniper5_dx12.exe. Astra made no driver writes. The literal native control now preserves the stripe pattern (contrast0.500000).

The actual NeuralAlias static-root/PSO replacement test also passes on the RTX5070Ti for WRAP and CLAMP. It keeps the same game root/PSO and texture, switches filtering live through0/4/8/16/0, and reads the real GPU result:

| Measured area | Off | 8x | 16x | Off again |
|---|---:|---:|---:|---:|
| Oblique detail contrast, WRAP | 0.001961 | 0.249996 | 0.500000 | 0.001961 |
| Oblique detail contrast, CLAMP | 0.001961 | 0.257809 | 0.500000 | 0.001961 |
| Change in isotropic control | 0 | 0 | 0 | 0 |

The final pass returns exactly to the original image. This is actual pixel evidence, not merely a sampler creation or binding count. Optional `NA_TEST_AF_CAPTURE` saves each standalone GPU result as PGM. The test-only readback does not run in game callbacks.

The AF overlay no longer disables the selector forever from the cached startup driver value. A profile read is diagnostic evidence and cannot prove a particular GPU sampling result. Current USER overrides receive a warning; CONDITIONAL is no longer mislabeled as an unconditional USER override; failed reads cannot claim known settings. Source location is included in driver status and CLI output. The existing explicit per-executable apply/restore action remains reversible; no automatic/global profile writes or extra vendor SDK are added. Counters now say recorded pipeline bindings instead of pretending those counts prove screen coverage.

Off restores the game's own sampling, which can itself already be16x. AF restores detail for elongated/minified footprints; it does not sharpen magnified or face-on pixels. SE5's observed material path uses static samplers. Existing CreateSampler/CreateSampler2 creation interception remains; this change does not claim that an already-created dynamic sampler heap or every pipeline-library path was rewritten for arbitrary live changes.

Primary driver-setting meanings/provenance: https://docs.nvidia.com/nvapi/NvApiDriverSettings_8h.html and https://docs.nvidia.com/nvapi/struct__NVDRS__SETTING__V1.html . Sampling reference: https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-to-samplegrad .

## Independent flicker review

Opus9f7873a correctly addressed a separate defect where the HUD Off switch handed control to the old native route. Removing the switches and making exhaustion observational removes that crossover as an automatic routing policy. The current Picture compositor's raw-image fallback is separately repaired above.

The actual b734513/p34536 result still has reconstruction misses: `NA-se5-results-20261105-200304/run-summary.txt` reports a3856-frame capture with1084 consecutive stretched frames, followed by good intervals, and another2190-frame capture with19 stretches. All say no current depth/motion. Final raw counters: scene114078, missed5198, middle-entry rejections226490, boundary rescues2. Opusd80901e adds rescue-failure diagnostics, not a completed fix for these bursts.

The reviewed source's PostEntryProfile identifies only the consumer PSO/root/destination/extent/format/compute class and permanently marks it Middle if any occurrence was mid-scene. A terminal occurrence can therefore collide with an earlier occurrence of the same pipeline. Its boundary rescue still depends on recording-local writer occurrence, which can change when two lists merge. COMMS requests a contextual producer/entry match using existing submission-ordered frame evidence, plus alternating split/merged/optional-late-scene tests, zero early placements and one evaluation per true scene. Do not erase the Middle evidence or globally relax occurrence matching. Opus retains this placement lane; final source must be reviewed when he posts it. No claim of game flicker closure from this HUD/AF increment.

Raw HUD logs also correct a misleading earlier interpretation: Picture GPU receipts DO exist while HUD is enabled (for example clean513). `check_frames=0` is the old unblend diagnostic; `composited=0` was also a counter gap because the new Picture composition branch never incremented `small_hud_layer`. That accounting is corrected. Neither old counter was proof that no GPU composition ran. Historical handoff wording is preserved with this explicit correction.

## CPU/GPU infrastructure already integrated

See docs/GPU-STACK-INTEGRATION-20261105.md for complete source and receipts. D3D12MA supplies pooled backing memory to the shared native/NVRHI scratch factory, HUD layers, J2 scene/output/RCAS/jitter resources, standaloneNR and effect resources. Taskflow uses two below-normal workers and32 bounded jobs for shader/PSO preparation and DLSS-file hashing, replacing five detached preparation sites. NVRHI supplies the optional owned GPU command/binding/submission backend for J2 render-small, standaloneNR and Donut compute; inline game-list work remains native. Its runtime Experimental option defaults off, while shared allocation/background infrastructure is active independently.

Allocation benchmark:64 scratch textures, five-pair median672.10us pooled versus20786.10us committed. This measures creation/destruction churn, not game FPS. Lifetime tests hold GPU consumption behind a test fence, release caller references and prove allocations stay alive until actual completion. Background tests prove bounded work, observer exclusion, shutdown/restart and no loader-lock wait. These integrations retain the core discovery, motion, jitter, reconstruction and model interfaces. They do not by themselves remove per-draw interception/replay costs.

Opus has removed adaptive tone mapping at Cort's direction; preserve his Bloom-only UI/persistence changes. Do not reintroduce that effect while packaging infrastructure. XeGTAO stays Enhancements. No additional implementation or review cycle for the libraries is proposed here.

## Qualification receipt

Initial complete Release build and eight affected RTX checks pass. The expanded production callback test proves zero NGX evaluations for an automatic shadow replay, then one ordinary DLSS evaluation after all four HUD methods exhaust; the effect remains enabled. The GPU layer test covers clean opaque text, native reference composition, wrong-target isolation, mixed source replacement, hidden-source discovery, queued/repeated submissions, clipping, and preservation of a deliberately different enhanced output when HUD pixels fail validation. Policy checks cover four-attempt exhaustion, nonempty coverage, delayed results, pause, resize and non-flapping isolated failure.

The broad suite and final narrow follow-ups are recorded below on completion. No game visual or FPS result is claimed from these automated checks. Regression build uses the host-stub adapter; Opus must build the established production adapter for the user's combined additive package.

### Final source qualification — 5 November 2026 Pacific
Complete NVRHI-on Release build PASS. Broad213-case run:208PASS,5SKIP,0FAIL,405.11s. That broad run preceded the final small status/provenance, native automatic non-destructive guard/check and test refinements. Final complete rebuild plus8affectedRTXchecks PASS20.81s; finalforcedWARP5/5PASS11.83s covers nativeHUD/counterfactualHUD/productionJ2/NVRHI/menu. Separate final actual native automatic route proves512HUDpixels/512separated; negative replay-alpha and empty-HUD cases execute. Productioncallback proves shadowreplay zeroNGX and ordinary DLSS still oneevaluation afterexhaustion. No runtime changes after final checks.
Final RTXactual replacement pixel modes WRAP andCLAMP bothPASS (0/4/8/16/0 includingexactOffrestoration);14rawPGMimages andaf-final-comparison.png saved under mainlocal/opus-af-watch/af-pixels-final. Fresh read-onlydriverprofilelogaf-final-profiles.log reportsAPP. Testevidence logs: hud-auto-full-tests.log, hud-auto-final-{build,tests,warp}.log, hud-native-auto-rtx.log, af-final-{profiles,wrap,clamp}.log. All localpaths ignored.
Initial test-only capture export lacked the algorithm header and the new direct callback invocation needed its explicit context argument; corrected before successful fullbuild. An expanded native automatic test initially skipped validation atframe3 because it inherited the older render-ahead fixture's continue, then reached a discovery-only assertion despite this resolved route; fixed the test's automatic case, exercised actualframe3 and reranRTX/WARP. Do not count that failed intermediate check as a passing runtime result.
No game launch, gamefiles, settings/profile writes, installer deployment or gamequality/FPS claim. Broad qualification uses realDX12 with fakeNGX where documented; rendererfixture success does not replace Cort's game test. The independentflickerreview remains a concrete findings handoff to Opus; no final contextualfix has been posted as of this receipt. Opus must build the established production adapter rather than package these host-stub regressionDLLs. No extra reviewcycle requested.
