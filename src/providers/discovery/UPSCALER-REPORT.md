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

## Host producer mapping

`ReadUpscalerReport()` reads non-destructive NGX, FSR, XeSS and DLSS-backend
snapshots. These are independent cumulative snapshots, not an atomic frame record.
NGX `sr_creates` remains SOURCE UNKNOWN until origin instrumentation is available;
generic NGX evaluations and failures span feature types and are not used.
C6's own `features_created` / `evaluates` are injector-origin DLSS evidence.
Unknown-origin evidence outranks injector-only evidence in that row.
An injector-only row does not inherit a "native observer unavailable" warning:
C6 supplies that evidence independently of NGX hooks. Aggregate NGX creates still
cannot be separated into native and injector calls without producer-side origin
instrumentation; subtracting C6 counters would not establish that provenance.

FSR `upscale_contexts` and XeSS `contexts_initialized` prove successful game
creation/initialization. Hook/module counters describe observation availability
and library presence. `upscale_dispatches` / `executes` are game **requests**:
they increment before native success is known and include replaced calls. They
produce GAME REQUESTS when no stronger evidence exists and do not establish a
successful native route. Neither `fallbacks` nor requests minus `replaced` proves
native success. Replacements belong to the DLSS row, counted once through C6.
The report never drains observation records or changes replacement settings.

## Runtime integration and validation

The A4 branch includes C7 at 13a8a59 and must integrate after C6/C7. Their open
review findings still require author fixes; this dependency is not approval.
`upscaler_menu.cpp` is compiled into `host_dx12`. `RegisterRouteAMenu()` registers
the report after Clear and existing sections, reporting registration failure to
the Windows debug output. The Details page renders the three rows and reasons.

Windows CPU tests link the actual host implementation, exercise startup twice,
verify exactly one report section, map synthetic producer snapshots, and draw the
report with ImGui. Portable tests verify conservative evidence classification.
These checks do not establish live-game interception or on-screen rendering;
that acceptance remains part of the Gate 2 Windows/game session.
