# Diagnostic build and test

Current usage is in [DLAA-CONTROLS.md](DLAA-CONTROLS.md). The normal build hides
legacy diagnostics (`DX11_DIAGNOSTIC_UI=OFF`). Set that CMake option ON only for
developer previews/captures. The smoke test now also compiles the DLAA control
effect, checks technique/global toggle state and hot-reloads a preset definition.
Install current DLAA through `scripts/install-dlaa-trial.ps1` after building the
Win32 addon and the x64 helper with the privately provisioned NVIDIA SDK.
The loading-only v0.1.0 account below is historical, including its statement that
DLAA is not implemented. See HANDOFF for current build/SDK and deployment state.

For the current v0.2.0 depth preview, read [DEPTH-VIEW.md](DEPTH-VIEW.md).
The original v0.1.0 scope below describes the loading-only baseline.

This first build is a read-only DX11 diagnostic add-on for ReShade 6.8.0 with
add-on support (API 20). It logs GPU information, image dimensions, frame callbacks
and a bounded list of textures created with depth/stencil usage.

**It does not yet identify scene depth, inspect motion vectors, visualize buffers,
or improve image quality. DLAA and DLSS are not implemented.** Depth-usage flags
also match shadow maps and other textures; candidates are explicitly unverified.

## Build and test

Install Visual Studio Community 2026 with Desktop development with C++, MSVC
x86/x64 tools, Windows SDK 10.0.26100.0 and C++ CMake tools for Windows.
Run these from the project folder in PowerShell:

```powershell
./scripts/build.ps1
./scripts/test-smoke.ps1 -ReShadeDll 'C:/path/to/Alien Isolation/dxgi.dll'
```

The build produces `build/Win32/RelWithDebInfo/dx11_diagnostic.addon32`. It uses
the static C++ runtime. Win32 means 32-bit, matching Alien: Isolation. The optional
x64 build target is for future hosts, not this game's executable.

The test copies your existing 32-bit ReShade runtime into a unique local folder.
A hidden DX11 window creates a device and depth texture, presents 121 frames,
resizes, and releases its resources. The test script verifies the actual ReShade
log for add-on registration, resource and frame callbacks, resize and cleanup.
It does not use game assets or change the game installation.

## Install and inspect

Close the game, then run:

```powershell
./scripts/install-diagnostic.ps1 -GameDirectory 'C:/path/to/Alien Isolation'
```

The script adds only `dx11_diagnostic.addon32` and refuses to overwrite a copy.
It records hashes in the ignored `local/deployment.json`. Start through Steam.
Look in the game's `ReShade.log` for `DX11Diag`, GPU identification, image
dimensions and frame 120. It also appears in ReShade's add-on list; it has no
overlay of its own yet. It observes current rendering, including existing mods.

To uninstall, close the game and remove only `dx11_diagnostic.addon32`. Leave
`dxgi.dll`, `d3d11.dll`, Alias Isolation, other add-ons and presets alone.

## Implementation scope

- `src/core` contains reusable diagnostics reached through ReShade events.
- `src/adapters` contains game identification, currently only the AI.exe name.
- `tests` contains the real DX11/ReShade integration host.
- `third_party` contains pinned ReShade headers and license notices.

This loading test uses ReShade directly instead of importing Luma's full core.
Luma remains a candidate for later analysis and visualization. No Luma or Alias
code is copied into this build. This is an independent project, not an NVIDIA
RTX Remix product.

Next: validate Alien-specific shader/pass detection, safely capture scene depth
and velocity before reuse, and add visualization. A separate 64-bit DLSS helper
remains an unproven later experiment.
