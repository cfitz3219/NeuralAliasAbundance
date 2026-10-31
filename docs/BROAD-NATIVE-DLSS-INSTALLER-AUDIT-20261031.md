# Broad native-DLSS installer audit — 2026-10-31

Author: Astra (Codex). Production source reviewed: e6b3ac44b045759727c7d8fde5e09b165fb633a6; shipped rendering DLL: b71ba0baf65017afef5ee824aa7a7284094d5f70. Also inspected Opus cb364199e1fb32de9f8dd28707ab73707eb48db1 (PR72, UI text only, not integrated by this audit), current COMMS, prior Route A audit, and preserved Ghost FG evidence. This is a source/contract/coverage audit of the native-DLSS route, installer and shared services, not a claim to have verified every line, every interleaving or every graphics API. No runtime changes, deployment or game testing in this audit.

## Assessment

We have reusable working components and a real Ghost success. We do not yet have a general drop-in product. The central weakness is that startup, presentation, NR and controls do not consistently share one answer to: which process, device, view, real frame, command queue and presentation owner does this operation belong to? Several answers are still inferred independently, globally, or only after an optional service succeeds. That makes valid components fragile when combined.

The remedy is a bounded consolidation of the existing native route: reuse its events, queue bindings, recording leases, mode controller and installer transactions. It is not a new graphics analyzer, new DLSS algorithm, or another universal discovery project. Native-DLSS games already expose valuable labeled inputs; bindless scene rendering does not inherently hide those API inputs. They do not necessarily expose every extra input needed for post-tonemap, pre-UI NR, or an engine resolution-setting API.

The immediate broad release target should be supported Windows x64 DX12 native-DLSS integrations. DX11 and Vulkan are separate graphics backends: the current D3D12 NGX hooks and DXGI queues cannot be advertised as covering them merely because those games also say DLSS.

## What is actually established

- Cort confirms Ghost opens Home, runs NR, toggles NR off/on, and changes models/presets. This is genuine integrated user evidence.
- Latest preserved Ghost status reports 7,153 NR evaluations/submissions, zero recorded skips, source=1 (native NGX FG). It does not establish frame-tagged SL NR source=2. The matching boundary log records legacy slSetTag calls (for_frame=0), with association 0/5171/0 (unique/ambiguous/none).
- Opus's FG findings are supported by the preserved status: sent=0/1 and 1/1 both occur for requests Off/On. Thus 'it remained x2' is not evidence that Off/On failed. The UI mixes the game's original setting with the forwarded setting. NVIDIA's Streamline v2.4.0 header explicitly requires numFramesToGenerate=1: an older host integration can limit output to 2x even with a newer feature DLL.
- The two-queue ACCESS_DENIED defect and partial hook startup were reproduced and repaired. New tests exercise the production overlay on WARP/RTX with separate game/native queues and extra native Presents. These are meaningful improvements.
- Existing automatic quality-control attempts, actual-dimension confirmation, native engine-thread setters, submitted GPU lifetimes, subrect handling, reversible file transactions, guarded ABI reads and per-PID logs should all be retained.
- Latest broad automated evidence was 72 PASS/1 optional actual-model SKIP; final focused checks 13 PASS. Font/installer fixtures subsequently passed. These are previous completed test results, not new tests run during this audit. They do not prove broad game or SDK-version coverage.

## Findings that explain fragility

### F1. The installer is still two game recipes, not a reusable installer

Evidence: scripts/deploy-pbz.ps1:4-19 accepts PBZ/Tsushima and embeds local paths, executable names, layout, settings scope and driver-profile policy. Its preflight verifies package hashes, the NR DLL's NVIDIA signature and a chosen executable path; it does not discover a game's active graphics API, loaded upscaler interface, supported SL family, relevant child process, or separately supported feature set. PBZ's plugin NR search path is another hardcoded layout at line174.

Effect: a copied command file can install the wrong recipe or valid files in a directory the actual renderer never loads. Compatibility decisions happen late, inside the game. Multiple Documents package folders, mutable launcher pointers and chains of repair-from receipts also make stale-build tests easy. The Poise package records runtime and installer commits separately, but deployment/status exposes mainly source_commit, so the full release identity is not consistently visible.

Required: parameterize the existing transaction with a discovered executable/layout and one release manifest. Preserve file receipts/rollback. Cache scan facts and recognize API/loader families; distinguish observed runtime support from file-presence hints. Declare DLSS control, NR and native FG independently. Keep native DLLs intact by default and detect another existing proxy rather than replacing it. This is the foundation underneath the approved library UI; it does not require building that UI now.

### F2. Bootstrap is still timing-sensitive and too dependent on the overlay

Evidence: host_boot.cpp:259/292 starts an init worker and allows game factory creation to wait at most3s. That is bounded and sensible for avoiding startup deadlock, but expiration is not proof that early objects were captured. sl_boundary.cpp:1007 sets module_seen before successful attachment and declines all later install_for_module attempts for that module, so a failed installation is not retried by that path. The shared wait fixed ordinary contention; it does not turn a timeout/partial install into a recoverable attach state.

More seriously, host_boot.cpp:222 returns when overlay::Start fails; engine adapters and the NGX logger below it never start. Queue interception, recording/reset observation and presentation-original publication also reside inside overlay::Start (overlay.cpp:2740-2877). Hiding or failing the menu should not remove the machinery on which DLSS controls/NR depend.

Required: retain the init worker and existing hook transactions; give core attachment explicit waiting/attached/failed-retry states and late-object acquisition. Run the renderer services and control adapters independently of menu availability. Keep menu/input initialization optional. Do not add unbounded waits or attempt hook installation under a render callback/loader lock.

### F3. Helper processes still share game settings and the crash marker

Evidence: settings.cpp:36-56 selects one file per configured scope. BeginSession:272 treats any existing scope marker as an unclean previous run; the marker contains no live-owner PID. EndSession:292 can delete that same marker. Ghost evidence contains two host PIDs sharing a scope, one reporting no SL activity. Per-PID logs fix diagnostic mixing, not settings/session ownership.

Effect: a helper starting alongside the renderer can be classified as crash recovery, save stale settings, or remove a marker belonging to the renderer. This is a concrete ownership flaw; it is not proof that every earlier unexpected setting change had this cause.

Required: select the actual rendering process using renderer/device evidence and restrict game preference/session writes to its owner. Make live concurrent ownership distinguishable from a dead prior session. Preserve the original user preference separately from a temporary recovery override. Avoid a long manually maintained helper-name blacklist as the primary solution.

### F4. Presentation ownership is only partly unified

Evidence: the new BoundaryRoutes correctly selects a game-facing proxy and suppresses its associated native Presents. But overlay.cpp:2305-2338 suppresses presenter_present for a boundary-owned base while DetourGetBuffer still allows presenter::HandleGetBuffer to substitute our presenter buffers (presenter.cpp:720). With our presenter enabled, the game's buffer substitution can survive while the code that delivers those buffers to the screen is bypassed. Opus independently reported this. Both current installers use proxy=0, so it is a latent configuration defect rather than an explanation for the current successful Ghost run.

Required: one presentation owner per chain, consulted by GetBuffer, GetIndex, Present/Present1, resize and release together. Native FG and NeuralAlias's own presenter must not partly own the same stream. Reuse BoundaryRoutes/PresentQueues and existing presenter records. Test actual final pixels when deliberately enabling the conflicting configuration.

Late capture of the exact SL game queue is another coverage gap: factory RTTI/export hooks plus the early gate improve it, but a missed creation with no later ResizeBuffers1 can still withhold the menu. Private SL class RTTI/IIDs and a dummy native swapchain implementation are not a universal attachment API. Prefer supported interface boundaries/returned actual objects where available and retain version-qualified fallbacks. Never restore 'last direct queue' guessing.

### F5. NR route ownership is too broad, and a candidate can claim it before working

Evidence: sl_consumer.cpp:137-167 creates a Stream and publishes its device into claimed before checking for a usable queue, constants, required guides or supported motion. OwnsDevice:258 is device-wide. native_fg.cpp:296 uses contexts_count OR any SL claim for OwnsRoute. nr_pass.cpp:585 stops gathering fallback SR inputs when any route is owned; OnPresent:838 also suppresses by device/route.

Effect: one incomplete view can reserve an entire device and disable another viable route/view. 'SL seen' or 'native FG handle exists' is not sufficient to establish which real frames this path will enhance. Sticky ownership is needed to prevent duplicate NR during FG suspension, but device-wide stickiness confuses that requirement with successful per-stream ownership.

Required: extend the existing Stream records with scoped candidate/selected/draining ownership tied to runtime, device, view and presentation epoch. Publish ownership at the appropriate qualified acquisition point, maintain it through genuine suspension/in-flight work, and transition routes at explicit boundaries. Do not switch every missed frame or erase a selected owner by a timeout. A stream that cannot acquire its required inputs must not disable unrelated streams.

### F6. Native DLSS inputs are standardized; our normalization coverage is narrower

Evidence: sl_boundary.cpp:283-307 gives legacy slSetTag no frame identity; sl_consumer.cpp:82 rejects events without a frame token; Consume requires Unique association. Constants require camera-inclusive, unjittered motion at line167. NVIDIA's constants API explicitly represents other motion conventions. Modern frame-based tags, legacy persistent tags, local evaluate tags and differing guide lifetimes are valid integration choices, not four different game bugs.

The no-FG Present path still selects the newest completed guide snapshot (nr_pass.cpp:889-911) rather than a proven view/frame-matched final color. Completion prevents unsafe reuse but does not establish image identity. Its selected input is consumed before later processing can fail. Existing native FG identity/cache/lease work should be reused to close that route, not duplicated.

fg_states.cpp:11-14 invalidates on enhanced barriers instead of decoding their layouts; it mostly observes current-list legacy transitions. This is honest incomplete coverage, not support for all modern D3D12 state conventions. An NGX read-input contract does not give permission to write arbitrary native resources.

Required: a few interface-family adapters that normalize tags, frame identity, extents, motion/jitter conventions, resource lifetime and actual queue/layout into the existing NR contract. Support legacy SL tags using real constants/evaluate/Present identity rather than arbitrary timing. Where normalization requires absent camera data, acquire it through the available interface and keep the unsupported case explicit. Extend the existing state tracker for enhanced layouts where the chosen insertion point needs it; do not build a second render graph.

Native SR does not necessarily expose display-resolution HUD-free post-tonemap color. Missing optional NR-specific guides should affect NR availability, not basic DLSS preset control or the overlay.

### F7. Live quality control breadth is currently overestimated

Evidence: mode_control.cpp tries Streamline, Unreal, native settings adapters and window rebuild, and verifies actual dimensions. This is a good base. NVIDIA's guide explicitly assigns returned render sizes to the application's viewport; changing an optimal-resolution answer cannot force every engine to use it again.

Unreal locator.hpp:12-22 recognizes nine normalized function-body signatures; line37 still matches their hashes. Ghost locator.hpp:56/119 also requires recognized normalized function shapes. TLOU apply.cpp:19/208 still pins one executable hash. Removing exact-file admission from Unreal/Ghost supports relocation/unrelated file changes; it does not make the setters generic across compiler optimization, UE versions or other Nixxes ports. The evidence currently qualifies a supported binary-interface shape, not most Unreal games.

Required: keep the single request/confirmation controller, query steering and tested setters. Broaden by documented engine interfaces where available and a small versioned set of verified setter contracts where not. Keep code-shape signatures as supported fallbacks, not a claim of engine-wide support. Gate ownership of dynamic resolution correctly and confirm SR/RR input dimensions. Avoid pursuing a nonexistent NVIDIA API that directly controls every engine's internal render resolution.

### F8. Native FG controls are requests, not a complete live control loop

Evidence: SetFgRequest:1257 only updates an atomic request; forwarding happens on the game's next DetourDlssgSetOptions:770 call. This works in Ghost, which calls it frequently, but not reliably in a title that configures once. State and request bookkeeping are global rather than per viewport. The override can also turn the game's temporary Off request back On during a loading/menu/resize suspension. NVIDIA requires coordinated options/Present ordering and appropriate suspension.

read_fg_state/query_fg_state:733-768 use bounded versioned bytes, but missing fields, zero-filled extensions, an older integration and an actual reported system limit must remain distinguishable. A zero maximum alone is not proof of a particular Streamline version. Opus PR72's reordered sent-status is useful; its inference of a pre2.7 integration from max_frames==0 should not become generic detection. The public v2.4 header does contain numFramesToGenerate but requires1; it is inaccurate to say that field is absent.

Required: queue a per-viewport user request, retain a version-qualified copy of relevant options and apply pending changes at a supported ordered presentation/settings boundary. Retain native scheduling and the game's temporary suspension/recreation rules. Read back supported status and distinguish requested, forwarded, accepted and observed presentation behavior. Driver preset C is a separate request with separate confirmation; never equate it to a native multiplier. If the loaded host interface cannot request MFG, report that capability accurately. A newer model DLL does not alone change the host API.

### F9. NR image composition and HDR remain coverage/quality work

Evidence: native_fg.cpp:576 selects supplied alpha, additive residual on the SL route, or exact equality on the native fallback. Alpha composition has a defined equation. Additive residual preserves B-H but does not recover unknown opacity; equality only enhances matching pixels. Neither fallback guarantees correct translucent UI in every title. Ghost's reported result does not prove that every fallback/route has been tested.

NR shaders adapt HDR through SDR and a clamped enhancement ratio. NVIDIA's sample explicitly runs NR on post-tonemap LDR display-resolution imagery. The HDR adapter is our algorithm and needs image tests with a nonzero enhancement delta, highlights, scRGB/PQ and UI; a zero-change roundtrip is insufficient. Resource format alone is not the complete color encoding. Current SL consumer obtains color space through the base binding; that can be unknown with late attachment.

Copy allocates a new committed texture per preserved guide, and Consume allocates an allocator/list per candidate frame. This creates avoidable allocation pressure and stutter on top of display-sized NR cost. Retain the submitted lifetimes; reuse completed work objects and correctly sized images from the existing pools where possible.

Required: use supplied UI/alpha and color-space metadata, keep one output pipeline for full and HUD-free images before FG, qualify the fallback composition modes explicitly, and reuse retired buffers/allocators. The new overlay now precedes native interpolation, so menu appearance during MFG needs its own image check.

### F10. Testing and release evidence have followed individual repairs more than integrations

The historical test failures were not that no tests existed. Many modeled the expected topology: one known queue, one view, one interface version, one always-refreshed settings callback. They proved the component under those assumptions. Ghost's different proxy/native queue ordering exposed the assumptions. New two-queue/bootstrap tests improve this considerably.

CI compiles with and without SDK headers and runs many tests, but fake NGX models do not reproduce NVIDIA's external FG scheduler. The current workflow's deployment step runs the older deploy-sl-proxy fixture, not the current PBZ/Ghost/font transaction fixtures. Package preparation and launcher mutation still happen through local scripts, outside a single repeatable release command. A very large mixed host library and monolithic overlay/NGX modules also make it easy to change shared behavior while reasoning about one title only.

Required: keep existing tests, add a small topology matrix and run the actual current installer fixtures in CI. Compare output pixels and per-real-frame delivery, not just successful call counts. Generate one immutable versioned release with runtime/installer/assets/capability schema identity, and make its updater select the installed receipt itself while migrating user choices. A font update should not need to reset NR preferences. No new external review cycle is required.

## Focused completion sequence

These are repairs/extensions to existing pieces. Estimates are engineering ranges for bounded completion, not a promise that literally every title is covered.

| Order | Deliverable | Reuse | Rough effort | Completion evidence |
|---|---|---|---|---|
| 1 | Remove competing presentation ownership and overlay-as-core dependency; scope session/settings writes to actual renderer | BoundaryRoutes, PresentQueues, host_boot, session | 1-2 working days | Deliberate conflicting-presenter, helper/renderer and menu-failure fixtures keep correct image delivery and controls |
| 2 | Scope NR ownership and normalize older/newer SL/direct-NGX input families, including needed enhanced layouts | Stream, PresentEvent, FgContract, RecordingLease, states | 2-5 days | Legacy/frame-based/no-proxy cases, two views, same/different queues, resize/FG suspend/recreate; no duplicate or unrelated-view suppression |
| 3 | Complete native FG request/observe loop and capability reporting | Existing SetOptions/GetState hooks and UI | 1-2 days | A game-like fixture which sets once can change live; temporary Off during resize is respected; old SDK cannot claim MFG; per-view state is correct |
| 4 | General installer/release identity, detect API/loader family, preserve preferences through upgrade | Existing deploy transaction, receipts and library-sync design | 1-2 days | Arbitrary fixture executable/layout, existing-proxy coexistence handling, repeatable current-release upgrade/rollback and original-file hashes |
| 5 | Broaden the supported resolution setter families and image/convention coverage | mode_control, engine setters, composition/color helpers | 2-4 days initially | At least another independently built UE interface and a direct-NGX/custom-engine case; real dimensions plus HDR/UI pixel tests |

Some tasks overlap. Roughly one to three focused working weeks for this hardening/breadth milestone is a planning range; the supported-engine interface evidence is the largest variable. Universal all-game coverage has no honest fixed-date guarantee. Engineering and automated fixtures need no user attendance. Keep live testing to short user-owned sessions spanning distinct integration families, with a concrete purpose for each, rather than repeating the same title after every small edit.

A sensible bounded initial matrix is direct-NGX SR without native FG; older SL persistent tags with native 2x FG (Ghost); modern SL frame-based tags/MFG (PBZ is a candidate pending current-build validation); and manual/late-attach or multiple-view presentation. These are integration families, not one custom algorithm per game. Resolution behavior is an independent axis: live query, cached query/engine percentage, custom settings, or DRS.

## What to preserve and what not to spend time rebuilding

Preserve: original game installation, the working Ghost package, NVIDIA's native FG scheduler, NGX API interception, guarded version decoding, per-buffer creation/resize queues, shared hook transactions, recording/submission leases, actual-size confirmation, asynchronous diagnostics, color/guide conversion kernels and the approved UI work.

Do not add a general draw/dispatch analyzer to solve native-route attachment; do not replace native SL/FG wholesale to obtain a different API version by default; do not recreate temporal algorithms; do not add arbitrary last-queue or latest-image guesses; do not create a new review gate; do not rewrite every module before shipping the fixes above.

The busy-cursor report remains unresolved. Current code sets IDC_ARROW on menu open/WM_SETCURSOR but does not intercept another game-side SetCursor; competing cursor ownership is a plausible lead, not a proven diagnosis. Treat it as an overlay input defect, not evidence of GPU deadlock from its appearance alone.

## Primary references checked

- NVIDIA Streamline general guide: https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuide.md — initialization, tagged lifetimes, viewport/device and thread contracts.
- NVIDIA manual hooking guide: https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideManualHooking.md — native/proxy interface handling and documented interception boundaries.
- NVIDIA DLSS SR guide: https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS.md — application sets render viewport from optimal settings.
- NVIDIA DLSS-G guide: https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md — ordered SetOptions/Present, suspension, resource lifetime and composition.
- NVIDIA current DLSS-G headers: https://github.com/NVIDIA-RTX/Streamline/blob/main/include/sl_dlss_g.h — generated-count meaning, supported maxima and states.
- NVIDIA Streamline2.4 header: https://github.com/NVIDIA-RTX/Streamline/blob/v2.4.0/include/sl_dlss_g.h — numFramesToGenerate exists but must equal1.
- NVIDIA constants: https://github.com/NVIDIA-RTX/Streamline/blob/main/include/sl_consts.h — motion/jitter/camera conventions are represented explicitly.
- NVIDIA sample NR: https://github.com/nvpro-samples/vk_gltf_renderer/blob/main/docs/denoising.md — display-resolution, post-tonemap LDR NR. This Vulkan sample is a reference contract, not proof of our DX12 game integration.
- Microsoft DX12 swapchains: https://learn.microsoft.com/en-us/windows/win32/direct3d12/swap-chains — creation queue/presentation ownership.
- Microsoft enhanced barriers: https://github.com/microsoft/DirectX-Specs/blob/master/d3d/D3D12EnhancedBarriers.md — layouts/access/queue compatibility; not arbitrary legacy-state substitution.
- OptiScaler official README/config: https://github.com/optiscaler/OptiScaler and https://github.com/optiscaler/OptiScaler/blob/master/OptiScaler.ini — established API-family interception, independent FG inputs/outputs, process filters, compatibility knowledge and quality/DRS overrides. These are useful architecture precedents, not evidence that OptiScaler guarantees every game or universally controls engine resolution. No upstream implementation was copied.
