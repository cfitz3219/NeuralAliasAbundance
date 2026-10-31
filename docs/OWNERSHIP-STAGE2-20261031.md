# Ownership repair stage 2: renderer preferences and recovery

Cort directed completion of the remaining approved work. This is O3 after the separately committed first-stage review corrections24daf95. It follows Fable's light per-process journal design; it does not replace NeuralAlias's core or implement the conditional O2 NR router.

## Result

Non-rendering helpers can read defaults and attach observation hooks, but cannot create recovery journals, persist settings, delete a renderer's journal or activate saved effects. Renderer evidence is a real non-TEST presentation excluding the host dummy, or an external NGX evaluate on a known real D3D12 device. Callbacks publish an atomic flag and signal an event; they do not perform journal or preference I/O.

The existing initialization thread becomes the settings/session control worker. Once rendering is observed it reloads untouched preferences, elects one shared writer and handles recovery before saved effects are allowed. Other qualified renderer processes use their own settings/journal and can run effects, but cannot overwrite shared preferences. Election and clean close/delete use a short mutex keyed to the settings path; per-process journal handles distinguish a live renderer from an abandoned file. No executable-name helper heuristic, owner UUID, new service or complex lease/migration framework was introduced.

Each qualified renderer owns only `<user ini>.session.<pid>`. Windows closes the journal handle on death. A live renderer's journal is not a previous crash. Only the elected renderer consumes abandoned recovery records, and a helper cannot consume a legacy `.session` marker. PID reuse is distinguished by the held journal handle rather than PID existence. Clean close/delete is serialized with the next election, preventing a healthy exit from briefly appearing as a crash.

If journal creation or the clean-exit hook is unavailable, the renderer remains process-local and cannot save shared preferences. It does not manufacture an unerasable crash marker. Known abandoned recovery is still applied locally. The original renderer and menu continue; status reports writer/clean-exit readiness. No relative journal is created in a game directory if the user settings path cannot be resolved.

## Preferences and feature activation

Settings Save requests are coalesced onto the existing worker; the menu distinguishes queued, saved and failed/read-only results. Unchanged fields are refreshed from the current user file at renderer election; locally edited fields are retained. This prevents an old bootstrap snapshot from replacing newer settings. The existing recovery overrides still turn effects off temporarily without saving the forced Off as the user's choice. Explicit NR Off is now also recognized as user intent, including when recovery already forced its current value to zero.

Saved SR/model/native-FG, NR, J2, motion, Reflex and FSR/XeSS replacement remain provisional until renderer recovery is known. Observations/forwarding continue. Driver-profile writes require shared writer permission. Native FG requests and the game's own calls pass through before activation. Saved model/quality requests use the existing reapply/mode-control paths; the overlay's native alignment and observed Unreal ratios were moved to shared helpers, avoiding a second sizing calculation. These requests retain the existing rendered-size confirmation; merely queuing one is not a successful game quality change. Unknown output size is not invented.

The activation does not promise completion in one frame: the worker performs bounded control work while the original game continues rendering. O2's NR ownership decisions, image processing and real/generated-frame associations remain the existing implementation.

## Remaining first-stage findings addressed here

The reviewed input-hook correction24daf95 is retained. A further inspection found that failed hook cleanup could leave a valid trampoline, then overwrite it with the patched export. The input path now preserves the published original; a real failed-commit/removal injection verifies retained trampoline identity while Home still draws. Optional factory-batch failure also no longer marks the independent proxy-Present group terminal. Present-group dead-hook policy is preserved.

Fable's stage1 review was closed in COMMS c422a03; no additional review cycle was requested. Both the first-stage source/runtime snapshot and b71ba0b remain preserved independently.

## Evidence

Full Release build PASS. Final CTest:140 selected,135 PASS,5 SKIP,0 FAIL in80.95s. Skips are the optional real-NR runtime probe, three unsupported16xMSAA fixtures and the optional upscaler-jitter integration. WARP/RTX, SL, NGX, NR, settings/session, overlay and quality-control regressions pass.

`session_ownership_test` uses actual same-executable child processes with isolated settings and named synchronization events. It covers helper-first/renderer-first, helper Save/failure/exit, simultaneous renderers, a second renderer's local choices, stale bootstrap reload plus local edit preservation, forced death, failure followed by clean ExitProcess, temporary NR Off preservation, explicit NR Off, PID-reuse files, legacy recovery, read-only preferences, journal creation failure, unavailable clean-exit observation, overlapping clean exit/election, and recovery while another local renderer remains active. Public-SDK builds also prove the real NGX create-override function leaves parameters unchanged before renderer qualification. No game, GPU model or NVIDIA profile is invoked by this child-process test.

Bootstrap fixtures exercise the production host init worker and actual WARP menu. Cursor cleanup fixture verifies the published original after failed removal. Existing stand-in-model GPU fixtures qualify orchestration/lifetimes, not game image quality.

Windows PowerShell5.1 additive install/rollback and Ghost upgrade-wrapper fixtures PASS. Evidence: local/ownership-stage2-20261031 (not committed). Initial compile/link adjustments and earlier intermediate tests are retained there; the final build/test result above applies to the delivered runtime. No game was launched, controlled, installed into, or had a native DLSS/FG/Streamline DLL changed.

## Scope and delivery

The corrected first-stage snapshot and this O3 candidate are separate packages; the current updater points to the newer tested O3 build. Actual installed baseline receipts are verified rather than inferred from launcher filenames. The NVIDIA NR payload and Poise fonts are reused unchanged. Existing additive repair can reset NeuralAlias preferences during an upgrade; installer preference migration is outside this reviewed runtime/session stage. The new runtime prevents helper/recovery writes from erasing wishes after installation.

O2 remains conditional as directed by the review: Ghost's existing receipt has aggregate tag shapes/counters and ambiguous associations, not original per-evaluate resource/frame/recording-generation identity. It cannot replay-prove the binder. Keep the working NR route; no speculative global-claim deletion, F/F-1 pointer guess or new routing path was implemented. The proposed narrow observation remains a later qualification task, requiring actual evidence. No game success or universal-installer completion is inferred from these automated results.

User attendance: none for engineering or automated tests. Cort retains the live Ghost/PBZ checks. Known-working Ghostb71ba0b and the pre-O3 stage remain available as comparisons; no automatic deployment occurs.
