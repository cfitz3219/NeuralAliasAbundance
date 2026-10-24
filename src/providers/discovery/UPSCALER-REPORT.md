# A4 upscaler observation report

`upscaler_report.hpp` classifies a supplied, coherent snapshot of SR observations.
It does not authorize a backend evaluation, infer support from a DLL, or identify
the currently selected game setting. Counters mean **observed during this process**;
there are no timestamps or frame identities in the current host snapshot, so the
report explicitly leaves present activity and input qualification unestablished.
A new process/snapshot starts with no inherited evidence.

Each provider (DLSS, FSR, XeSS) supplies successful **SR-specific** creation and
evaluation counts split by game, injector, or unknown origin. Game-origin success
establishes an observed integration; multiple integrations stay visible. Injector
success never establishes native support. Failed calls, libraries and missing
observers have separate descriptions. Failed calls do not erase earlier successful
history. The model has no runtime feature-enabling side effects.

## Current NGX bridge

`ReadUpscalerReport()` uses the existing host's non-destructive Stats snapshot.
Only `sr_creates` establishes successful SR calls. The aggregate `evaluates` and
`create_failures` counters include other features and are deliberately not used.
Module presence/hooks establish observation context only. Existing stats do not
separate game/injector origin, so these creations report **SOURCE UNKNOWN**.
FSR/XeSS report **NOT MONITORED** until C7 producers supply their actual observations.
No call-record ring is drained or competing with the existing recorder.

C6/C7 should provide SR-specific, origin-qualified snapshots to the portable
classifier. Do not classify an injector call as native because its stack also
contains a game frame. Producer lifetime/reset, origin and per-frame validity
must be explicit before displaying stronger claims.

## Host wiring requested from Claude (not yet applied)

The new bridge is currently compiled and exercised by the Windows CPU test.
For the actual runtime, add this source to `host_dx12` in the root CMake:

```cmake
target_sources(host_dx12 PRIVATE src/providers/discovery/upscaler_menu.cpp)
```

In `src/host_dx12/overlay/route_a_panel.cpp`, include
`providers/discovery/upscaler_menu.hpp` and call
`na::discovery::RegisterUpscalerReport()` after the existing registrations in
`RegisterRouteAMenu()`, after its `menu::Clear()`. Check the return value in the
host's diagnostic path; the registry is bounded to 16 sections. Registration is
idempotent and works again after Clear. The Details page renders the summary and
three provider rows with their evidence reasons. Existing game-picture controls
are unaffected. This is additive provider code, not an observation ABI change.

A4's visible-in-game acceptance remains pending this host wiring and the Gate 2
session. C7 game observation is a separate prerequisite for real FSR/XeSS detection;
synthetic unit-test inputs prove classification rules, not interception coverage.
