# Display-independent scene integration — engineering journal

## 2026-11-06 Pacific: failed-window-path evidence

Cort tested source `1aa2bee`; Quality made no visible change in borderless.
The installed DLL was `616FF6D76C6F5456E5DAA4B62F154259C9E0A3E95DD08C912E5BF173CAC6C472`.
Run `p42480` requested 2293x960, 1720x720 and 2000x837; actual colour/proxy
dimensions remained 3440x1440. Zero monitor/display-mode rewrites occurred.

Read-only examination of the installed executable clarifies what the query
counters mean. The two frequently polled client-rectangle sites belong to
mouse-coordinate processing. One paired window/monitor site belongs to cursor
clipping. They do not establish that a render-size request reached the renderer.
The actual WM_SIZE branch skips resizing for the borderless display-mode value.
WM_DISPLAYCHANGE refreshes monitor identity; the same monitor does not cause
the resize body to run. No executable offsets, modified game code, writes to
game settings, or game execution are part of this examination.

These facts rule out repeating the same notification and interpreting query
counts as acceptance. They do not establish that every other engine ignores
the notifications, nor qualify an alternative in the actual game.

## Approach under integration

Reuse the existing smaller scene-resource group, scoped descriptor/constant
bindings, physical-input provider, J2 reconstruction and GPU recording leases.
These change actual render resources without changing the OS window, monitor,
DXGI fullscreen state or physical output extent. Do not substitute cropping or
downsampling a full-resolution render. Do not resurrect projection-window or
unconfirmed settings-memory writes.

This is unfinished integration. The provider cannot simply be enabled alongside
the current presentation path: clear/copy ownership, indirect commands, source
extent/jitter and actual submitted output ownership all need to agree.

## Increment 1: independent clear/copy owners

The automatic scene provider and HUD listener previously registered into one
global texture-operation slot. Whichever registered last removed the other.
Added two explicit lanes, Bindings and Effects, using the existing shader-lane
snapshot and reverse-unwind design. The compatibility setter owns Effects;
automatic scene transfers own Bindings. Registration does not destroy callbacks
under the registration lock. In-flight operations hold their exact snapshot;
replacement still suppresses the native call exactly once and stops entering
later lanes.

Extended the real DX12 automatic-scene regression with an Effects witness
registered before the scene provider, self-unregistration inside Before,
matching After, and later Effects registration without losing scene transfers.
Existing reduced-resource, full-output, mode-change, stencil, material,
unsupported-plan and recording-replay pixel assertions remain active.

Validation: complete affected targets built in Release. Nine affected CTests
passed, zero skipped or failed, 19.53 seconds on this PC. GPU commands and pixel
checks are real; the automatic-scene model callback is fake. This is not live
Sniper fullscreen or image-quality evidence. Source is not an installer payload;
the existing build uses the regression adapter. No game launch/control,
deployment or installed configuration change occurred.

Remaining: finish source-route selection and presentation integration, including
indirect/state semantics and actual frame/extent delivery, then qualify and
prepare one reversible production package. Do not describe this increment as
the completed fullscreen fix.

## Increment 2: draw ownership, jitter and the reconstruction boundary

Implemented on the shared1aa2bee base, retaining the existing core/backend.
A command-signature creation observer recognizes draw-only indirect signatures
which inherit their root/resource/IA bindings. GPU argument/count buffers remain
native, with no diagnostic readback. Unknown or binding-changing signatures,
bundles and render passes keep the existing native fallback. The new creation
hook is optional; its absence does not disable ordinary observation.

The new raster-pixel test exposed a real forwarding issue: the DX12 wrapping
layer entered the draw path again through a DIFFERENT command-list object with
no game root bindings. Its jitter handler could reapply the original full-size
viewport over the private reduced viewport. Native draw/dispatch/indirect/bundle
forwarding now uses the existing thread-local HostWorkScope, while the outer
game command is observed once after forwarding. No new lock, allocation, GPU
pass or wait is added by this guard.

Reduced viewport bindings carry the exact recording's applied jitter in render
pixels. They preserve that applied sample so a repeated draw does not overwrite
the private viewport or re-jitter it. Actual viewport changes update the binding
serial. The real raster witness checks positive and negative offsets, a cached
second draw, a later frame's competing requested sample, an intervening native
pass, return to the smaller scene and live jitter Off. Final measured coordinates
match the smaller raster exactly; native-pass coordinates remain unchanged.
Initial fixture compilation hit Windows' small macro and a missing snapshot
header/setup; those fixture errors were corrected. The subsequent valid raster
failure was traced to the second command-list entry above. A fixture cache also
needed the same native-boundary/recording invalidation as the production owner.

A second actual-DX12 regression reproduced an output-loss defect. An engine keeps
all original buffers at64x32, renders through the private provider, submits J2,
then records a four-pixel native UI draw. Previously that later draw could stay
in the smaller-scene group and cause its raw image to be promoted over the
reconstruction:2048/2048 final pixels failed for Quality and Performance. A
provided source is now sealed at its J2 outcome, so later native shader and
texture operations cannot re-enter the old source. Native dependencies are
published before those consumers. The same test now has0wrong pixels through
Quality, Performance and native DLAA; original resources remain64x32 throughout.
This uses a fake model with real DX12 allocations, commands and pixel readbacks.

The completed-frame active/reconstructed evidence now survives its Present
boundary, and an empty subsequent Present clears it. Admission failures report
which recording, root/table, typed view or retired resource prevented the
snapshot. A native compute writer invalidates reduced provenance instead of
allowing J2 to consume an older private scene. Coupled raster targets use the
identified dependency group.

Startup seam: [present] scene_resources=1 selects this source provider; use
render_small=0, [j2] window=0. It keeps native presentation, disables a second
Present DLAA and does not install virtual window/display resizing. Existing
[layer2] enabled=1 remains supported. The overlay reports actual J2 input/output
sizes when they match the reconstructed source, rather than treating preparation
or allocation as applied Quality.

Before the final diagnostic additions: complete Release build and24affected
CTest cases PASS,0skips/failures,84.74s. This includes the new raster/source tests,
existing split/indirect/real NGX entry, HUD, scoped bindings and presentation.
A fresh full build/suite is running on the final increment; do not call it passed
until its receipt is appended. No game launch/control, deployment or installed
settings/native DLL changes. Opus accepted the independent presenter/output test
and reversible package lane in main COMMS; his presenter trace found the
full-size output path already suitable, and his output test is separate from
our provider test. Final combined source/package is still pending.

Reference: Microsoft's Indirect Drawing contract distinguishes inherited state
from command signatures that alter bindings:
https://learn.microsoft.com/en-us/windows/win32/direct3d12/indirect-drawing .
No upstream implementation was copied and no new dependency was introduced.

## Render-ahead ownership repair and combined presentation witness

Integrated Opus92c7cb6 as2fe8fcb (authorship preserved). Its real-DX12 presentation test checks unchanged SDR/10bit/float pictures through windowed/borderless chains, with no second evaluation or resize. Combined forcedWARP8/8PASS13.33s. Prior exact4b18 fullRelease222cases:217PASS5SKIP0FAIL364.31s; skipped NRruntimeprobe,3unsupportedMSAA16cases,optionalupscalerjitter. No game/physical exclusive-mode evidence inferred.

A new finite render-ahead reproduction failed before the repair: next scene records before the previous Present, then submits after it. The old global Present seal refused the actual scene, while resetting current at Present allowed its later UI list back into private rendering. Four frames each had2048/2048incorrect final pixels and missed physical evaluations (8assertions). Before log local/se5-followup-20261106/ahead-before.log. This failure was not detected by the original serial fixture.

Repair: source ownership first follows the exact bound recording, including a sealed older recording. In a new recording, an observed whole-view reset of BOTH selected colour and depth can begin the next scene before Present; a single/partial UI clear cannot. Reuse the prepared transfer for the first clear's exact copied values, then forward the second native clear once with its normal private mirror. Borrowed rectangle/resource pointers are never retained. A recorded but not yet submitted source survives the previous picture's Present; submitted/sealed ownership remains unchanged. New physical counters require an actual submission, not merely recorded commands. No GPU readback, wait, camera arithmetic or game-specific offset is added.

After repair, four consecutive render-ahead frames each perform the smaller-input evaluation, retain the reconstructed full-size output plus native four-pixel UI, and report0incorrect pixels. Existing serial/liveQuality/Performance/DLAA and emptyPresent evidence checks remain. FullRelease rebuildPASS;18affectedtestsPASS38.43s. Final forcedWARP indirect/postnative/renderahead3/3PASS5.76s. Model is fake; GPU/state/recording/submission/pixels are real.

Applicable platform contracts: Microsoft ExecuteIndirect inherits state that the signature does not modify, while affected bindings reset after it; only inherited-state draw signatures are admitted here (https://learn.microsoft.com/en-us/windows/win32/direct3d12/indirect-drawing). DXGI resize requires releasing real backbuffer references (https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-resizebuffers). The package uses full-size game-facing proxies; their lifetime must not be confused with retaining physical DXGI buffers. Opus owns the presentation/resize witness and additive package, Astra the source repair. Production payload must use na_discovery_diagnostics, never the regression adapter stub.

## Final engineering / package receipt

Runtime source96b76343c94a818218df1bcc5fa5e9a650b48698, PR113. Complete final local224-case run:219PASS5SKIP0FAIL392.63s. Production adapter/NVRHION build separately passed16checks40.89s, including actual NVIDIA NGX, all automatic-scene variants, jitter raster pixels, presenter output and resize. ForcedWARP source checks3/3PASS5.76s; combined presenter/source8/8PASS13.33s before the final source increment. The retained-old-game-buffer extension passes1.48s in the production build: actual format resize succeeds with the old full-size game-facing proxy externally retained, the new buffer identity differs, and the old proxy remains usable. No presenter/runtime repair was required for that case.

Production DLL SHA256:0E6558784A9974A92207E6517F66D22979416921199C94D8B6FF991F9D6ECCA2. Candidate C:/Users/Cort/Documents/NA-se5-motion-96b7634,16payloadhashesverified. Explicit launcher C:/Users/Cort/Documents/NA-se5-scene-update.cmd; previousbuild NA-se5-scene-previous.cmd (1aa2bee); original-install restoration NA-se5-scene-rollback.cmd. Existing NA-se5-dlss-results.cmd reads the shared result path. The ordinary NA-se5-dlss-update.cmd remains Opus's separately staged menu-motion diagnostic; the new source candidate is intentionally labelled separately and does not contain3d8a1fa/bb733ea camera/velocity changes.

Isolated installer checks5PASS0FAIL: additive install and exact scene_resources configuration; update preserving menu choices; tampered package refused before changing an existing installation; exact original fake-game rollback; foreign dxgi.dll preserved. Tests rewrote only a copy of the installer to a validated temporary fixture directory under local/se5-followup-20261106 and used an inert fake executable. Original running-game guard remains in the actual installer. Initial harness LF-vs-CRLF regex and inherited intentional-failure exit code were corrected; final receipts and raw fixture evidence are retained. No actual game deployment, game launch or game file modification occurred. Installed receipt still1aa2bee at final package verification.

Cort test: close game, run the explicitly named scene updater, choose Quality then Performance in Home/Picture/Render mode, and compare actual render-vs-output sizes in the desired window mode. The full display stays native; smaller source dimensions and a successful reconstruction identify a pass. A source/admission failure reports its stage in layer2 status/log; native-size fallback is not Quality success. Native/Off remains the existing native-resolution DLAA route. Physical exclusive fullscreen, real gameplay image quality, performance and full per-engine dependency coverage are not claimed by automated fake-model fixtures. Opus retains his separately assigned camera/object-motion investigation and was given the exact source/build/package/config and non-overlap instructions through COMMS.
