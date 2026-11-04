# DX11 bridge

Runs DLSS (DLAA, or real upscaling with Quality/Balanced/Performance) for a DX11
game. It works with any game, because it only moves pictures and a frame record.
Finding depth, the camera and the HUD point belongs to the hooks, which are a
separate step.

Rebuilt from the recovered Alien: Isolation bridge (`recovered/dx11-enhancement/`,
PR #90): NT-handle shared textures, shared D3D11 fences, a shared-memory frame
record and a hidden helper process.

## Pieces

| File | What it does |
|---|---|
| `src/bridge/frame_record.{hpp,cpp}` | Frame record v2: render size, output size, jitter, motion scale, exposure, mode, preset, flags. Same bytes in 32-bit and 64-bit builds. Portable and tested on Linux. |
| `src/bridge/win_shared.hpp` | Object names, handles, shared memory, events. |
| `src/bridge/dx11_bridge.{hpp,cpp}` | The game side. `Start()` picks a backend, and `Process()` runs one frame. |
| `src/bridge/dx11_dlss_inproc.*` | In-process backend: NGX D3D11 on the game's own device (64-bit, needs the NGX library). |
| `src/bridge/dx12_helper_main.cpp` | `na_bridge_helper64.exe`: opens the shared objects and runs our existing DX12 DLSS backend (`na::dlss`). |
| `tests/bridge/` | Record test (portable) and a DX11 → helper → DX11 round trip (`--copy-test`, any GPU or WARP). |

## Behaviour

- **Backends:** `Auto` tries in-process first, then the helper. 32-bit builds always use the helper. Neural rendering will need the helper, because DLSS NR is not available on D3D11 (Alien notes, 2026-09-21).
- **Sizes:** colour and depth go up to the session's maximum render size, and the output is the screen size. Dynamic resolution below the maximum needs no restart.
- **Waiting:** by default the game's CPU waits only until the helper has *queued* its work, and the game's GPU waits for the result, with no CPU stall on the GPU. `WaitMode::CpuDeadline` is the original Alien behaviour (CPU waits for the result, with a deadline).
- **Misses:** a late result never falls back to DLAA or the game's own image. The last good result is shown again (Cort's rule, 2026-11-03).
- **Game state:** all bridge drawing goes through a deferred context, replayed with the game's state restored.
- **Inputs:** colour can be any colour format. Depth can be an R32F, R24X8 or R16 view (red channel is used). Motion is a two-channel float view at render or output size. They are copied into the bridge's own textures, so typeless game formats are fine.

## Not done yet

- The DX11 hooks that find colour, depth, motion, the camera and the HUD point in a running game.
- A 32-bit build of the game-side library (the code is 32-bit clean; CMake only builds x64 today).
- Neural rendering in the helper, which the recovered Alien `neural_helper.cpp` shows how to do.
- Timing on real hardware. The round trip has only run in CI so far.
