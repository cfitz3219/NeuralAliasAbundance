# WWII DX11On12 startup ownership repair

Runtime: `7be0838ad7f91d1f12b803a3ea6253b53cb0ff95`, PR128.

## Finding

The failed 23609e3 launches p37712 and p40720 did load the corrected queue/depth build. This was not another stale installer run. Both started two translated-device requests, with the first waiting 25 seconds and falling back. Windows reported a CRT abort during the game main thread's exit callbacks. That terminal abort alone does not identify the earlier reason the game exited; there was no demonstrated NVIDIA device removal or DLSS evaluation.

The p37712 dump contains the runtime's `g_init_thread` value, 19424. That is exactly the thread recorded as the first timed-out device request. Relinking the existing objects with a linker map established the global's address, and comparing the entire `.text` section against the shipped DLL established that the map described the same code. Thus the first intercepted device belonged to **our graphics bootstrap**, not a second game renderer. Redirecting that nested helper to a worker that creates another D3D12 device produces the observed circular startup dependency. The internal driver lock itself was not captured before timeout.

## Correction

Reuse `InternalFactoryScope` around host initialization and overlay core setup. The D3D11 bridge now forwards calls made in this thread-local scope unchanged. Actual game device requests still go through D3D11On12. No process name, address fingerprint, security change, or game-specific timeout workaround is involved. Existing queue recovery, D24 staging, menu composition, and effects are retained.

## Automated evidence

- Release DLL and affected fixture built successfully.
- Six bootstrap, menu failure/recovery-related, optional input and cursor regressions passed: 16.36 seconds.
- New hardware case creates two nested native helper devices, then an actual translated device outside the scope; checks ID3D11On12Device identity and registered queue, followed by overlay composition.
- Removing only the new admission check makes that test fail four identity/queue assertions. Restoring it passes in 3.51 seconds. This is an exercised behavioral regression, not a source-text test.

## Installation and observed game run

Candidate package: `C:\Users\Cort\Documents\NA-wwii-on12-7be0838`.
DLL SHA256: `915F3C9981CD0F2211D33E4CBDC21B9432890A008C751D6CA2D3DAF1C0B7810E`.

The first packaging command used a wrong build output directory and retained the old copied DLL. Hash comparison caught that before any game launch. Corrected the source to `build\Release\NeuralAliasDXGI.dll`, regenerated all nine payload hashes, redeployed through the existing additive rollback installer, and compared the installed DLL to the actual build. No run used the mislabeled intermediate package. The existing four Install/Update/Results/Rollback CMDs now all target 7be0838; their prior versions are preserved.

Live p37000, started from Steam while Cort was using the PC, reached the main menu and the graphics settings. Core/menu initialization completed **before** the game device request. There was one translated request, completed in **266 ms**, no timeout and no native fallback. By the 3,374-present sample, every frame recovered its correct queue; the menu had completed 53 composites with zero failures. The observed game remained responsive through a switch to fullscreen. Cort was controlling the game; Astra stopped input attempts when the desktop tool detected that activity and monitored read-only.

This proves the previous startup delay is removed in this run and that the corrected build progresses beyond the formerly failing startup. It does not, by itself, prove campaign DLAA, image quality, or all DX11 games. This run's menu has no scene depth/color selection yet; that is not a DLSS failure result from a playable mission. Do not call an initialized overlay or bridge a qualified DLAA image.

Evidence: main `local/wwii-startup-crash-20261108` (original dumps, text-identical linker map, startup analysis, builds and positive/negative tests), `local/j2-wwii-on12` (p37000), and `Documents/NA-wwii-results-20261108-225921` (preserved failing run). Original game files and the separate ReShade backup remain preserved.
