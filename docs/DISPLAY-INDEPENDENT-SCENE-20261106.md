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
