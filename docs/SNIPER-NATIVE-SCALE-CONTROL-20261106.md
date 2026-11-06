# Native engine render-scale control — 2026-11-06

This increment implements the controller following the trace documented in
SNIPER-NATIVE-RENDER-SCALE-TRACE-20261106.md. The existing scale fractions,
engine-owned apply pattern, observer, J2 camera/motion/evaluation, destination
copy, command-state restoration, recording leases and additive installer are
reused. There is no new rendering backend or window-size virtualization.

## What changed

`scale/native_locator.hpp` reuses Ghost's mapped PE/HDE reader. Named settings,
parser quantizer, pending/cache mirrors, two dimension calculations and the
normal settings-tick call derive the context and function pointers. The C++
locator is checked read-only against both installed Sniper executables. SE5's
helper and Resistance's inlined settings apply are recognized, including
chained unwind metadata, without build hashes, fixed RVAs or numeric scans.

`scale/native_control.cpp` queues menu intent; native functions execute only
from the game's own settings-update detour once a real scene has been observed.
The normal tick runs first. Exact desired scale is then passed through the
complete native settings apply, after the pending-setting 5% normalization.
Only the pending scalar and, where present, native-upscaler choice are
temporarily overridden. Pending values are restored before the native save
code sees them. Other graphics settings remain under the original apply code.
An inhibited native update is retried through that same native path. Pausing
restores the game's requested scale/upscaler; selecting DLAA requests full size.

The native setting alone is not reported as DLSS success. Confirmation requires
both exact committed scene dimensions, agreement with the requested fraction
within native integer rounding (one pixel), matching full-output dimensions,
and a newer submitted J2 evaluation. UI and `native_scale` log rows distinguish
waiting/applied/confirmed. No backend is called on the UI/settings thread.

J2's existing reconstruction lane recognizes the actual operation reading the
selected reduced scene into a compatible native-size target. Graphics and
compute, merged and separate lists are supported. The native operation is
forwarded first; existing J2 `record()`/`UpscaleTarget` reconstructs into that
destination immediately afterward, before later post effects/HUD. Full graphics
viewport/scissor, depth-disabled/nonblended single target and exact resource
dimensions/format are checked. Ambiguous or incompatible destinations remain
the game's output while discovery continues. There is no second Present model.

For a separate producer recording, the existing `SceneInput` payload carries
the current frame's camera/sample and explicit colour/depth end states. It is
used only for the exact selected colour/depth and positively observed consumer
relationship; an unavailable/contradictory state in the current recording wins.
This does not substitute previous-frame guessed states. The existing J1 ledger
establishes the sample before the scene recording closes; no rendering, GPU
wait or GPU readback occurs at Close.

Native input/output resources and working images are retained with the existing
recording lease, including partial failed attempts. Slots and size changes wait
for GPU completion. A completed but still executable/closed game recording does
not require a future Reset to allow a new picture size. `RecordingLease::Completed`
checks actual latest submission completion, including concurrent re-submission
and discard handling. J2 commits history only through its original real-submit
path; no record-time evaluation counter is treated as submitted success.

The existing discovery scale admission is configured down to one third for
native control (including Ultra Performance), while aspect/shadow filtering
remains. Native mode prewarms the existing mip variants and selects the existing
automatic totals. Window, monitor and swap-chain dimensions are not changed.
Native control excludes marker-read hooks, render-small, projection-window and
the alternative Layer2 scene provider so scaling is applied once.

The current ordinary installer's motion/camera line through 71f27d1 is merged
with the existing 8fa9ad1 source/presenter wiring. Later experimental flow-snap
and draw-replay-probe changes are not silently included.

## Validation and qualification

- The controller CPU regression covers called/inlined and relocated locators,
  all five modes, exact scale past quantization, owner-thread dispatch,
  preserved pending settings, pause/restore, inhibited apply retry and refusal
  of stale/wrong-size/small-output-only DLSS evidence.
- Actual installed SE5 and Resistance executable inspection uses the SAME C++
  locator that ships in the DLL, not the earlier Python analysis as a proxy.
- Real DX12/fake-model bridge regressions exercise graphics/compute and
  merged/separate lists. The operation itself discovers the destination; the
  test never calls SetUpscaleTarget or a private inline callback. Each variant
  reconstructs 64x32 into 96x48 and verifies the far corner and a later native
  green HUD marker, then returns to DLAA while a completed post list stays closed.
- Initial separate-list testing failed on colour-state acquisition. The exact
  producer state payload corrected that failure; the initial failure remains in
  the ignored evidence logs. Integer-boundary CPU testing also exposed the need
  to confirm actual committed sizes rather than require a separately rounded
  nominal width. The closed-list return case checks the completion/retirement
  distinction. One merged motion API needed its existing InterloperFilter
  receipt argument; the integrated call carries and commits that receipt.

Final production Release build passes. The affected run covered 95 cases:
88 passed, three unsupported 16x MSAA hardware cases skipped, and four new
return-to-DLAA checks initially failed. Those checks exposed that the old J2
fixture's forward bypassed the mandatory production `nr::Execute` seam. It now
uses that actual seam, including with NR off, to publish real recording-lease
completion. Final eight affected/adjacent tests pass (four native bridge cases,
ordinary J2, NVRHI J2, native-FG retention and game motion), 11.92 seconds. Across
the two runs, 94 unique checks pass, three skip, no unresolved failure. The
earlier failing run is preserved; this is not represented as a clean first run
or a fresh complete-suite pass. The native settings hook also explicitly owns
its shared hook-library initialization before the early boot transaction.

The native bridge's return case passes while the completed old post recording
remains closed, and each of the four layouts produces three full-size outputs
with the later HUD marker intact. Installer receipts are recorded in COMMS.
Automated fake-model evidence is not a live game image-quality,
performance or all-window-mode pass. The user retains game launch/deployment
and visual testing. Discovery of this native settings relationship is qualified
on these two Rebellion executables; it is not a claim every game shares that ABI.

## Package/use

Run the updated `NA-se5-dlss-update.cmd` with SE5 closed. The usual DLAA and
scene update aliases point to the same native package. In Home > Picture,
choose Render mode; retain the preferred windowed/borderless/fullscreen setting.
Read the actual scene/output dimensions and confirmed status. DLAA returns to
full native scene size. Results uses the existing capture/log transaction.

The installer is the existing additive transaction, with original backups,
payload hashes, game-running guard and rollback. No existing game DLL is
replaced. Previous launchers/package remain available; the native-previous CMD
restores the previous ordinary updater, and rollback restores the installation
recorded before NeuralAlias. Preparing/publishing the CMD does not deploy it.
