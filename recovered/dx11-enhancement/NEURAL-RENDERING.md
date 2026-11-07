# DLSS 5 feasibility — 21 September 2026

The isolated and cross-architecture neural-rendering tests work on this RTX5070Ti.
v0.11.1 is installed with an initially unchecked experimental ReShade technique.
Live gameplay validation is pending. The existing DLAA helper and settings remain
unchanged; the addon now also supports the separate neural helper.

## Runtime and provenance

Downloaded `nvngx_dlssnr.dll` version310.8.0.0 from the community mirror used by
DLSS5-Autopilot. This is not a download hosted by NVIDIA. The unmodified Windows
binary has a **Valid NVIDIA Corporation Authenticode signature**; the archive hash
matches GitHub's published release digest. Signature validation establishes
publisher/integrity, not redistribution permission or game compatibility.

- [Source release](https://github.com/RankFTW/rhi-repo/releases/tag/dlssnr-310.8.0)
- Archive SHA256: `388C0A7912E15EC911B9C9E11A692142B11FE387DDF2B637D8C358138FFFB3AC`
- DLL SHA256: `E16BCF15E16E13F527491CDF7845B2FE6521A738D8F7C9C721866A8496E1FC8E`
- DLL architecture: x64 (PE machine0x8664), size165840496bytes.
- Private location: `local/neural-runtime-310.8.0/`; provenance in `source.json`.
- Evidence: `docs/evidence/2026-09-21-neural-runtime-identity.json` and
  `2026-09-21-neural-runtime-exports.log`.

The SDK used for compilation remains the existing pinned NVIDIA SDK. Its public
headers do not declare a complete neural-rendering contract. Feature18 and the
`DLSSNR.*` parameter names were researched in community source. They are treated
as an experimental, version-specific interface. Our probe uses the normal NGX SDK
and our existing custom project GUID. No patched runtime, other game's application
identity, caller-identity shim, ReShade interposer or downloaded installer is used.

## What passed

`neural_runtime_probe` is a separate x64 command-line test, not a game addon.
Both DX11 and DX12 can create ordinary DLAA through this test (positive controls).
With the signed neural runtime, DX11 reports `DLSSNR.Available=0` and creation
returns0xBAD00007. DX12 reports availability1 and successfully creates feature18.
Exports bearing a DX11 name did not establish functional DX11 support.

The DX12 test then processes the two previously captured1920x1200 frames from
`local/replay-88730/prepared`. Inputs: RGBA16F color, R32F depth, RG16F engine motion;
output RGBA16F. It supplies per-resource rectangles, non-inverted depth, motion
scale(-width,-height), and a reset on the first frame only. Defaults requested:
model0, style0, intensity1, local structure1, local tone0, skin structure1,
automatic mask1, UI correction1. These settings are not a recommended final look.

Captured linear color is converted through Reinhard tone mapping and sRGB encoding
for this experiment. **That is an approximation, not Alien's final color pipeline.**
The captured input also predates temporal AA. These frames demonstrate API/image
feasibility, not a finished appearance or correct production temporal contract.

| Frame | Invalid RGB values | Unwritten sentinel values | Mean absolute RGB change | GPU command bracket |
| --- | --- | --- | --- | --- |
| 0 | 0 | 0 | 0.0128057 | 4.946ms |
| 1 | 0 | 0 | 0.0127044 | 4.743ms |

GPU timestamps bracket the neural evaluation commands; upload/readback is outside
the bracket. CPU evaluation calls took1.668ms and0.364ms. These are two samples,
including startup, not a steady-state benchmark or total in-game latency. Do not
extrapolate an Alien frame rate or a cost at the user's larger display resolution.
Both outputs are finite/nontrivial and differ from their input. Visual inspection
shows subtle surface/shading changes; this is not evidence of a dramatic remaster.
Text fidelity and motion stability need longer, properly placed live tests.

Raw private results: `local/neural-image-test`. Reviewable PNG comparison and
full-size images: sibling output folder `../neural-rendering-preview/`.
Evidence: `2026-09-21-neural-create-probes.log`, `2026-09-21-neural-image-probe.log`.

## Reproducing the isolated test

Build the x64 `neural_runtime_probe` target with the already configured private
`NGX_SDK_ROOT`. Command from the project root, with paths resolved to absolute:

```powershell
./build/x64/RelWithDebInfo/neural_runtime_probe.exe dx12 `
  "$PWD/local/neural-runtime-310.8.0" "$PWD/local/neural-image-test" nr `
  "$PWD/local/replay-88730/prepared"
```

Omit the final input directory for creation-only testing. Use `dx11` to repeat the
unavailable path. Use `dlaa` with the normal SDK runtime directory for the positive
control. No new game capture, game restart or settings change is needed.

## Experimental live trial (v0.11.0)

`core/neural_transport.hpp`, `neural_bridge.cpp`, and `neural_helper.cpp` implement
NT-shared DX11/DX12 textures and fences on the same adapter. Seventy-two changing
texture roundtrips were byte-exact. Actual NR bridge replay matches the standalone
DX12 RGB result bit-for-bit on two saved1920x1200 frames, while preserving input
alpha and tested viewport/SRV state. RGBA8/BGRA8 output tests also pass, including
all256 alpha levels. This is transfer correctness, not proof of motion quality.
CPU output waits in the repeated two-frame test:8.738/7.528ms; not GPU/gameFPS.
Evidence: neural-transport-test, neural-bridge-test, neural-bridge-sdr-test logs.

The Alien adapter runs immediately before ReShade effects on the final SDR
UNORM picture, with this-frame successful DLAA, raw depth and engine motion.
Game HUD/text are included. No explicit UI mask is supplied. Automatic skin masking
does not protect the HUD. UI correction is disabled because separate UI inputs
are absent. See [controls and audit](NEURAL-CONTROLS.md) for the seven new settings.
Only single-mip/array/sample RGBA8/BGRA8 views are accepted; matching image extents
are mandatory. No sRGB decode/encode is added. First/gap/>250ms/display resets
clear history. Camera cuts within continuous gameplay and post-processing versus
motion/depth alignment remain unverified. No assertion of a correct scene-only
stage. Model0/style0, intensity1, local structure1, local tone0, skin structure1.

Launch game, enable DLAA in ReShade Home, then **Neural rendering (experimental)**.
Check the existing DLAA status panel for neural active/starting/error. Uncheck
neural to return to DLAA alone without restart. Failures latch only neural off;
uncheck/check to retry. Existing global effects switch disables both. Initial
install does not modify the preset; later checkbox choice is saved by ReShade.
Helper startup polls without waiting on each frame; output deadline100ms, startup
15seconds. A failed frame can hitch before fallback. Higher-resolution performance,
long temporal behavior, camera cuts, HUD/text and visual quality need live testing.
No new captures are requested for the initial trial. Report active status, visible
changes, ghosting, text quality, and frame rate with the checkbox off versus on.

Install script `scripts/install-neural-trial.ps1` checks runtime hash/signature and
owned file hashes, backs up previous addon/config/log, leaves SDK binaries private,
and adds only own neural control/config plus updated addon. Current source and
matching known-good0.10 addon/helper checkpoint retained under ignored local/.
Unchecking neural is immediate rollback; binary rollback while game is closed can
restore the0.10 addon and deployment record from local/backup-e2a4bb6fdade4ce9b3fc459ae8f2d2e4.
DLAA helper was not rebuilt or changed in this milestone.

## Research references

Original code for the probe is in `tests/neural_runtime_probe.cpp` and
`tests/neural_image_probe.hpp`. Community sources were inspected for interface
facts; their implementations have not been copied into our runtime:

- [DLSS5-Autopilot](https://github.com/Kizzuwatnaa/DLSS5-Autopilot/tree/669486769d03e83b2d6c0edcaa7309486ed711f8): runtime-source discovery.
- [DLSS5-Feeder](https://github.com/jlrouzies-fr/DLSS5-Feeder/tree/cb24507e93a93b5a8d2740e82717af6adaecf190):32-bit host/helper design research.
- [OptiScaler_DLSSNR](https://github.com/Dagherbou/OptiScaler_DLSSNR/tree/973761621353b99bee3dc7d4bb27b117fef2644f): feature18 creation/evaluation parameter research.
- [DLSS5-Reshade-AIO](https://github.com/kibblerz/DLSS5-Reshade-AIO/tree/09301f5528e619e8b9ec17c257d167e2985f53b0): independent lab/interface research; its caller-identity bridge was not used.
- [NVIDIA research](https://research.nvidia.com/labs/adlr/DLSS5/): model overview; distinct from the undocumented community integration contract.

Downloaded binaries and captured game imagery stay out of the public source
repository. A public distribution needs its own verified runtime sourcing and
applicable licensing review; the signed DLL is not bundled by this project.
