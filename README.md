# NeuralAliasAbundance

> **Recovery status (2026-10-23):** source recovered; full build and runtime not
> yet revalidated. The original milestone README below is historical. Read
> [Recovery notes](docs/RECOVERY-NOTES.md) and the
> [independent review / Claude handoff](docs/CLAUDE-HANDOFF.md) for current gaps,
> proposed ownership and validation. The source contains later features that the
> text below does not describe; their presence is not a claim of working support.

Standalone DX12 enhancement framework under development. No ReShade installation is required to build or run this project.

**Milestone 1 is implemented: independent project, DX12 diagnostic scene, settings preview and limited DXGI forwarding prototype. Game enhancement is not implemented yet.**

## Run the current tools

- `build/Release/NeuralAliasAbundance.exe`: opens the standalone settings preview. Both input routes are described. Effect controls are disabled because no game/backend is connected. Frame Generation is explicitly marked not implemented.
- `build/Release/NeuralAliasAbundance.exe --status`: reports available DX12 hardware and actual implementation status.
- `build/Release/abundance_dx12_fixture.exe --output build/evidence`: runs hardware GPU validation and writes scene/depth/motion/HUD preview images and a result file.

The settings preview changes explanatory text only. It does not persist settings, install a mod or alter a game. It is a native Windows management window, not yet an in-game overlay.

## Build and verify

Run `./build.ps1` in PowerShell. The script finds the existing Visual Studio CMake tools, builds x64 Release with warnings treated as errors, and runs CTest. The current toolchain is VS18/MSVC19.51 and Windows SDK10.0.26100.0. No dependency download is needed for this milestone. Hardware GPU tests need a working DX12 adapter; they never silently substitute software rendering.

## Intended architecture

Two providers feed a shared frame contract:

1. **Existing upscaler:** compatible evaluation calls must actually be observed and their inputs validated.
2. **Engine/game adapter:** acquire and interpret game resources when no compatible upscaler integration exists. Optical flow may later supplement motion; it does not replace depth/camera/UI work.

Unknown inputs keep features inactive. Camera cuts, provider changes, resource generations and discontinuities reset history. Backends have separate qualification gates; input metadata acceptance is not an assertion that GPU contents are valid. Frame Generation has a reserved feature identity and is always unavailable at present.

The initial target chosen by the user is **Anno 1800 in DX12 mode**, without native DLSS. Read [its inspection report](docs/ANNO-1800.md). Native-upscaler interception remains in the architecture but has not been implemented or tested against a game.

## Code layout

- `src/core`: frame contract, route selection and copied independent utilities.
- `src/runtime`: limited DXGI forwarding proof of concept.
- `src/app`: hardware report and Windows settings preview.
- `shaders`: deterministic synthetic layered scene.
- `tests`: CPU contract tests, actual DX12 compute/readback reference tests and DLL/device/swapchain tests.
- `reference`: preserved complete previous source-review archive, not linked or loaded by the new project.
- `docs/source-provenance.json`: SHA256 inventory of copied sources/tests and reference archive.

## Important implementation boundaries

`NeuralAliasDXGI.dll` forwards only the explicitly listed exports and is loaded explicitly by our test. It is deliberately not named `dxgi.dll` in the build output. It has no draw hooks, shader inspection, NGX interception or overlay. Do not install this milestone DLL into a game. The forwarding test creates a genuine DX12 device and swapchain, tests resize/presentation eligibility and unloads after COM resources retire. This is not general injection qualification.

The GPU fixture is a compute-generated layered scene with known depth and motion, not a full perspective 3D renderer. It checks moving foreground, stationary background, jitter offsets, camera-cut output and HUD separation across two extents. CPU waits/readbacks are intentional offline measurement operations and are not a proposed live rendering architecture.

DLAA/SR, neural rendering, optical flow, a persistent manager, full injection and in-game overlay remain future stages. The existing Alien installation is untouched and remains separate. Universal DX12 DLSS injection is the project goal; automatic discovery and synthesis are described in [the acquisition roadmap](docs/INPUT-ACQUISITION-ROADMAP.md). Measured performance and compatibility are recorded per tested build.

See [HANDOFF](HANDOFF.md), [WORKLOG](WORKLOG.md) and [provenance/credits](docs/PROVENANCE.md).
