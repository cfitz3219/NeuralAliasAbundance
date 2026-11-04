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
- **Game state:** the bridge swaps in its own context state for the frame (`SwapDeviceContextState`) and swaps the game's back afterwards. In-process DLSS runs inside that swap too, so NGX's own state changes stay with us.
- **Inputs:** each input can be a shader view, or the raw texture when the game made it without shader access (back buffers, most depth buffers). Raw textures are copied into readable ones, with the right view for typeless and depth formats (D16, D24S8, D32, D32S8). Multisampled colour is resolved. Motion is two channels, at render or output size.
- **sRGB:** if the colour view decodes sRGB and the target view does not, or the other way round, the result is converted so brightness stays the same.
- **Size changes and device loss:** `Process()` returns NotReady and `NeedsRestart()` turns true. The caller stops and starts the bridge at the new size. An old-size picture is never drawn.

## Not done yet

- The DX11 hooks that find colour, depth, motion, the camera and the HUD point in a running game.
- A 32-bit build of the game-side library (the code is 32-bit clean; CMake only builds x64 today).
- Neural rendering in the helper, which the recovered Alien `neural_helper.cpp` shows how to do.
- Timing on real hardware. The round trips have only run in CI so far.

## What could break it (review against Far Cry 5 / New Dawn, 2026-11-04)

Far Cry 5 and New Dawn use Ubisoft's Dunia engine: DX11, 64-bit, rendered on
several threads with deferred contexts, HDR output, and an exclusive-fullscreen
mode. I used them as the example because they hit most of the ways a DX11 game
differs from our test. Nothing below was run in those games. Each risk is
reasoned from how DX11 and the engine work, and the patched ones are covered by
the CI round trip.

### Fixed in the bridge

| # | What would go wrong | Why (Far Cry 5 example) | Fix | Test |
|---|---|---|---|---|
| R1 | Bridge fails to start, or the game's draws come out wrong after ours | Games that create their device with `SINGLETHREADED` can't make deferred contexts, and the first version used one. NGX D3D11 also changes bound state on the game's context. | Own context state via `SwapDeviceContextState`, made single-threaded when the device is. DLSS runs inside it. | Case 2 (single-threaded device; SRV, RTV, blend factor, viewport survive) |
| R2 | Black or missing picture | The back buffer and the D24S8 depth buffer usually have no shader access. Depth is typeless or a depth format. | Raw texture inputs are copied into readable textures with the right view format. MSAA colour is resolved. | Case 2 (raw RGBA8 typeless, D24S8, RG16 typeless), case 3 (4x MSAA) |
| R3 | Washed-out or too-dark picture | The game reads its scene through a non-sRGB view and writes through an sRGB one, or the reverse. | Output converts between sRGB and linear when the two views differ. | Case 2 (sRGB target) |
| R4 | Stretched or garbage picture after Alt-Tab, a resize or a resolution change, or a crash on device loss | Exclusive fullscreen calls `ResizeBuffers` on Alt-Tab. A driver reset removes the device. | NotReady + `NeedsRestart()`. A removed device is never touched. | Case 3 (resize, then restart at the new size) |
| R5 | Random crashes | Hooks can fire from the render thread and the present thread. | One lock around Start, Stop, Process and the getters. | (by construction) |
| R6 | DLSS fails to start | NGX writes logs and a cache. Game folders under Program Files are often read-only. | Default data folder is `%LOCALAPPDATA%\NeuralAlias\bridge`. | (by construction) |
| R7 | Corrupted frames or a crash | Dunia records on deferred contexts. A hook might call us with one, or another game thread might use the immediate context while we work. | Deferred contexts are refused per call (NotReady). D3D11's own multithread lock is held while we work if the game turned it on. | Case 1 (deferred context refused) |
| R8 | Helper removes its own device, which kills the session | The helper reset a command allocator after a 1-second timeout even if the GPU still used it (for example, the game froze on a loading screen). | It never resets on a timeout. It waits up to 10 s, then reports Failed, and it checks for device loss every frame. | (by construction) |
| R9 | Restart fails right after a resize | The new session could reuse the old session's object names while the old helper was still exiting. | Session names include a counter. | Case 3 (restart) |

### Still open (for the hooks, or needing a real run)

- **The hooks must call `Process()` on the immediate context, after the game runs its command lists.** In Dunia, the scene is finished only when the immediate context runs the deferred command lists. The hook point is the immediate context's draw or copy that starts the HUD, or Present at the latest. This is the main design question for the DX11 hooks.
- **Game temporal AA.** Far Cry 5 has its own TAA and temporal filtering (a half-resolution reconstruction). For DLSS, the game's TAA should be off and temporal filtering off, or the picture smears twice. This is a game setting, and the hooks can only detect it, not change it.
- **HDR.** With HDR on, the back buffer is R10G10B10A2 in PQ (or FP16 scRGB). DLSS must run on the scene before tone mapping (FP16, linear), not on the PQ back buffer. The hooks must pick the stage. The bridge itself carries any format.
- **GPU wait when the helper dies mid-frame.** In the default `Gpu` wait mode, the game's GPU waits on the helper's fence. If the helper crashes after queuing its work but before the GPU runs it, Windows normally still finishes or cancels that work, but this is not proven. If a freeze is ever seen, `WaitMode::CpuDeadline` cannot hang, at some cost in speed.
- **During a restart (resize), the game's own image shows** until the helper is ready again (DLSS start-up can take about a second). The bridge never shows an old-size picture, so this is the only time it does not repeat a frame.
- **Overlays.** The Ubisoft Connect, Steam, RivaTuner and Discord overlays also hook Present. They draw after us, so they should be fine, but they are not tested.
- **Laptops with two GPUs.** The helper opens the game's adapter by LUID, so it follows the game. Not tested.

