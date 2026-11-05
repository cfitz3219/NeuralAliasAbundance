# DX9 → DLSS (Claude)

DLSS (DLAA) for DirectX 9 games, 32-bit and 64-bit, with no per-game code.
Plan and reasoning: [DX9-PLAN.md](DX9-PLAN.md). This page says what was built,
how to turn it on, and what it does not do yet.

## How it works

```
game (DX9, 32 or 64-bit)
  │  d3d9.dll = NeuralAliasD3D9.dll (our proxy)
  ▼
D3D9On12, on OUR D3D12 device and queue          src/host_dx9/d3d9_proxy.cpp
  │  device hooks → "ears" → frame model           src/host_dx9/dx9_hooks.cpp, dx9_ears.hpp
  │     scene colour + depth, camera, DLSS point
  │  vertex shaders patched for jitter             src/host_dx9/vs_patch.hpp
  ▼  at the DLSS point, on the game's thread
unwrap colour + depth (D3D12 resources) → copy into shared textures
  │                                                src/host_dx9/dx9_transport.cpp, src/bridge/dx12_game_bridge.cpp
  ▼
na_bridge_helper64.exe (unchanged, the same helper DX11 and Vulkan use)
  │  camera motion from depth + reprojection, DLSS
  ▼
result copied back into the scene colour → handed back to DX9 with our fence
  → the game's tonemap / HUD carries on as normal
```

| Part | What it does |
|---|---|
| Proxy | Off unless `[dx9] enabled=1`. Sends `Direct3DCreate9(Ex)` through D3D9On12 on our own D3D12 device (then 9On12's own device, then plain d3d9 as a last resort, which gets no DLSS). Fullscreen requests become borderless windowed, also across Reset. Log per run. |
| Ears | Scene colour = the target that gets the most depth-tested draws; depth only counts with `ZENABLE` on. Camera: the ViewProjection (+ its inverse) in the vertex-shader constants, found by the same matrix scan as DX11, ignoring registers that change for most draws (per-object matrices); fixed-function games use View × Projection. |
| DLSS point | Before the first read of the finished scene colour (the tonemap); for games that draw the scene straight into the back buffer, before the first HUD draw after the last scene draw; else at Present. |
| Jitter | Every vertex shader is rewritten at creation: its position output goes through a spare temp, then `oPos.xy += cJ.xy × oPos.w`, with `cJ` the device's last constant register (a shader that already reads it is left alone). Before each draw `cJ` is set to the jitter (scene draws) or zero (everything else), only when it changes. Fixed-function scene draws get a jittered projection the same way. The game's matrices are never touched, so the camera stays exact. Jitter only runs while DLSS is delivering, so the picture never shakes without it. Mip bias −1 is added to the game's own on all samplers while jittering. |
| Motion | Camera motion: the helper reprojects the depth with current → previous ViewProjection. Games where no camera is found get no motion (see "Not yet"). |

## Install (test)

Package: `NeuralAliasD3D9-x86.dll` (from the CI artifact `windows-release-x86`),
`NeuralAliasD3D9-x64.dll` (`NeuralAliasD3D9.dll` from `windows-release`),
`na_bridge_helper64.exe`, `nvngx_dlss.dll`, `scripts/deploy-dx9.ps1`.

```
.\deploy-dx9.ps1 -GameExe "C:\Games\Something\game.exe"      # picks 32/64-bit from the exe
.\deploy-dx9.ps1 -Action Collect                                # after playing: logs into .\logs
.\deploy-dx9.ps1 -Action Rollback                               # removes exactly what was added
```

It refuses to install over an existing `d3d9.dll` (ReShade, dgVoodoo, DXVK…)
or `NeuralAliasHost.ini`, and never changes a game file.

### NeuralAliasHost.ini `[dx9]`

| Key | Default | Meaning |
|---|---|---|
| `enabled` | 0 | 1 = on. Off means pure forwarding to the system d3d9. |
| `borderless` | 1 | Fullscreen → borderless windowed (needed for 9On12 + our presentation). |
| `own_device` | 1 | 9On12 on our D3D12 device first. |
| `dlss` | 1 | Run DLSS at the point found. |
| `preset` | 0 | DLSS render preset (0 = NVIDIA default). |
| `jitter` | 1 | 0 off (shaders not even patched), 1 while DLSS delivers, 2 always (test only). |
| `jitter_phases` | 8 | Halton sequence length. |
| `mip_bias` | −1 | Added while jittering (−3…0). |
| `copy_test` | 0 | 1 = no DLSS; the helper copies and the way back swaps red/blue (pipeline test). |
| `log` | | Log path; default `%LOCALAPPDATA%\NeuralAlias\dx9\NeuralAliasDX9-<pid>.log`. |

Settings are read at start-up for now. There is no in-game overlay for DX9 yet.

## Tests (CI, Windows x64 and Win32)

- `dx9_d3d9on12_probe`: colour and D24S8 depth readable through D3D9On12, 32 and 64-bit.
- `dx9_ears` (portable): scene, depth, camera register, read point, HUD rule, fixed function, loading screens.
- `dx9_vs_patch` (portable): vs_1_1 / vs_2_0 / vs_3_0 shaders run in a small VM before and after the patch: position moves by exactly jitter × w, every other output unchanged, refusals.
- `dx9_proxy_off/on/copy/jitter`: a small DX9 game (post-processed, then back-buffer) through the real proxy.
  - `copy`: the helper's result reaches the tonemap with red/blue swapped, the HUD is untouched, camera motion frames are sent.
  - `jitter`: a colour-gradient quad is drawn as part of the scene in a 32-bit float target; over 12 frames `16 r + jx` must stay constant, i.e. the picture moves by exactly the jitter DLSS is told (within 0.02 px).

## Not yet

- **Optical-flow motion** for games where no camera matrix is found (only
  per-object matrices): those get zero motion today, so DLSS can ghost on
  moving objects and camera pans. Camera motion covers games that upload a
  ViewProjection, which the scan finds.
- No overlay / live settings for DX9 (start-up ini only).
- The jitter scale uses the scene target's size; a game that renders the scene
  into a smaller viewport of a bigger target would get slightly wrong jitter.
- Not run on a real game yet.
