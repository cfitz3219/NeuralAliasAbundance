# Live DLSS quality control: PBZ and broader coverage

Astra, 2026-10-31. Assessment at76d9a9f; no runtime edits, game launch, deployment or live test in this investigation.

## PBZ evidence

Reviewed existing AppData/Local/NeuralAlias/PBZ/{observe.log,status.log,unreal_probe.log}, archived local/pbz-nr-flicker-7133085/status.log, current Unreal screen_percentage.cpp/.hpp, route_a_panel.cpp and NGX creation observations.

- Probe actually found r.ScreenPercentage's TAutoConsoleVariable at executable RVA0xdf4a9f8, with IConsoleVariable pointer and backing values33.3/33.3. Neighboring object87bytes later contained50/50: the existing narrow locator is deliberate, not a universal ABI guarantee.
- The captured status has6 optimal queries of1000x1000. Actual native DLSS creations later include1146x480 UltraPerformance,2295x961 Quality,1996x836 Balanced and1720x720 Performance, with3440x1440 output. No increasing query counter accompanies the later sizes. Both SR(feature1) and RR(feature13) occur in this evidence.
- One-pixel output nudges also appear with unchanged1146x480 input and changed quality labels. This directly demonstrates why a feature rebuild/quality enum alone is insufficient.
- These captures support engine-controlled live ratios. They do not prove every menu operation goes through a particular setter or that our newer adapter has successfully changed the game: captures predate that implementation.

## Current implementation: plausible mechanism, unfinished reliability

1. screen_percentage.cpp192 writes both shadowed floats directly with InterlockedExchange from its worker. This bypasses IConsoleVariable's setter, priority rules, change callbacks/sinks and engine-thread propagation. Atomic scalar writes do not reproduce that operation. Existing COMMS language about using the same setting must not be read as proof of using the same update pathway.
2. run_request225 uses the right kind of control, but has no main-thread dispatch or standard setter invocation. A selected fixed mode must also deliberately resolve ownership with dynamic resolution and game settings, rather than repeatedly force memory or immediately concede on a rewrite.
3. confirmation274-286 reads last_sr_in fields, which ngx_observe updates at successful feature creation, not current frame evaluations. Global create-count growth is not exact request/viewport attribution. A dynamic-resolution change within an existing feature may be missed; creation success alone is not rendered success. Reuse and extend the existing observed-evaluation monitor, rather than add a second system; support SR and RR and valid no-recreation transitions.
4. request lifetime currently has no cancel check when master/follow-game/mode changes while the worker is applying. Add one per-request generation and make hand-back behavior explicit. No dedicated Unreal adapter test was found in current tests; build/CI passes don't establish these behaviors.

## Bounded next implementation

Keep the locator, UI, startup ratio table, standard NGX interception and renderer observations. Retain the discovered IConsoleVariable object, resolve its actual setter/ABI and a main-thread scheduling point, then use the engine to propagate the setting. Establish fixed-mode versus DRS ownership with normal setting priority and explicit hand-back. Observe resulting SR/RR input extents on the matching real frames; never report a raw write or rebuild as success. Test cancellation, game overwrites, DRS, SR/RR and no-recreation transitions with an engine fixture, then one combined user-owned PBZ run.

Estimate: several focused engineering hours to roughly1-2working days for the engine setter/thread binding and regressions, contingent on identifying the shipping binary's interface. This is not a runtime rewrite and is not a promise of in-game completion before user testing. No user attendance during engineering; live acceptance remains Cort's.

## Broadening without one complete implementation per title

Use one request/confirmation contract, backed by reusable mechanisms:
- NGX/Streamline query interception when engines refresh their standard resolution query live.
- Unreal console/game-settings dispatch for UE titles, accounting for supported version/layout and dynamic-resolution behavior.
- Native deferred graphics-settings adapters for custom engines (Ghost demonstrates the pattern; TLOU already has one). Share behavior/discovery only when independently verified across engine versions; do not generalize Ghost addresses to Nixxes titles blindly.

Choose a mechanism from observed integration and engine capabilities, preserve explicit resource ownership and native FG transitions, and confirm actual input dimensions. This reduces work to engine families and exceptions. It does not establish a numeric 'most games' coverage claim or guarantee every DLSS title: shipping engine differences and restricted injection still exist. No restart is accepted as successful live Apply.

Primary references:
- Epic Console Variables C++: https://dev.epicgames.com/documentation/unreal-engine/console-variables-cplusplus-in-unreal-engine?lang=en-US (console manager, callbacks/sinks, thread context and set priority).
- Epic Dynamic Resolution: https://dev.epicgames.com/documentation/en-us/unreal-engine/dynamic-resolution-in-unreal-engine (resolution owner/heuristic).
- NVIDIA Streamline DLSS guide: https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS.md (query result versus application's viewport update).
