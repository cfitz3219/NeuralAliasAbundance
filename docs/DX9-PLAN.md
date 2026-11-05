# DX9 plan

How NeuralAlias reaches DirectX 9 games, using the same game-agnostic rules as
DX11 and Vulkan (`host_dx11/frame_model.hpp`, `camera_pick.hpp`) and the same
64-bit DX12 DLSS helper as the DX11 bridge (`na_bridge_helper64.exe`).
Nothing here knows any game.

## The two hard facts

1. **Most DX9 games are 32-bit, and DLSS (NGX) is 64-bit only.** So DLSS cannot
   run inside a 32-bit game. It has to run in our 64-bit helper, and the picture
   has to cross processes. That is the job the DX11 bridge already does.
2. **A plain DX9 device cannot share textures or fences with another process,
   and its depth buffer (D24S8) cannot be read.** Something has to give the
   picture and depth a shareable, readable form.

## The route: D3D9On12 (built into Windows)

Windows 10/11 ships **D3D9On12**, Microsoft's DX9-on-DX12 layer (`d3d9on12.dll`,
open source at microsoft/D3D9On12). An app gets it with `Direct3DCreate9On12`,
and can hand it **its own D3D12 device and queue**. Its interop API
(`IDirect3DDevice9On12::UnwrapUnderlyingResource` / `ReturnUnderlyingResource`)
gives the **real D3D12 resource behind any DX9 surface**, with fence-based
hand-back. ([spec](https://microsoft.github.io/DirectX-Specs/d3d/TranslationLayerResourceInterop.html),
[announcement](https://devblogs.microsoft.com/directx/coming-to-directx-12-d3d9on12-and-d3d11on12-resource-interop-apis/))

So, inside the game:

```
game (DX9 calls) -> our d3d9.dll proxy -> D3D9On12 on OUR D3D12 device/queue
                                   |
            at the DLSS point:     v
   unwrap scene colour + depth (D3D12 resources, depth readable)
   -> copy into NT-shared D3D12 textures, signal a shared D3D12 fence
   -> na_bridge_helper64.exe (unchanged: same shared-handle protocol as DX11)
   -> GPU waits for the helper's output fence -> copy result back
   -> ReturnUnderlyingResource with our fence -> the game reads it (tonemap)
```

Why this route:

- **All-in-one.** It is an OS component. Nothing third-party has to be downloaded,
  and no game file is changed (the proxy sits beside the exe like our dxgi.dll).
- **It solves both hard facts at once.** D3D12 resources share across processes
  and across 32/64-bit with NT handles, and D3D12 fences share too. The depth
  buffer becomes a normal D3D12 resource we can read.
- **The helper does not change.** It already opens NT-shared textures and fences,
  and builds camera motion from depth.

Its known costs (from users who forced it on, [Vogons](https://vogons.org/viewtopic.php?p=1054082)):

- **Slower than native DX9.** One Mass Effect scene ran at about 130 fps against
  about 200 native. DX9 games are old and light on today's GPUs, so this is
  usually fine. The probe (D0) measures it.
- **No exclusive fullscreen.** We switch the game to borderless windowed at
  device creation.
- **Some games misbehave on it**, mostly very old DirectDraw/DX7 games, which we
  don't target. D3D8/9 games do much better.

Fallback for a game D3D9On12 cannot run: **dgVoodoo2** (DX9 to DX11) plus our
existing DX11 hooks built 32-bit. It needs no DX9-specific code from us, and is
what DLSS5-Feeder does. It is an optional download, never bundled.

Routes looked at and dropped:

- **Upgrading the game to D3D9Ex for its shared handles.** Ex refuses the MANAGED
  memory pool that nearly every DX9 game uses, and has no shared fences.
- **DXVK** (DX9 to Vulkan). It is the fastest translator, but our Vulkan layer runs
  DLSS inside the process (impossible at 32-bit). A Vulkan game also can't hand
  the picture to another process mid-frame (see VULKAN-HOOKS.md).
- **CPU readback.** Too slow, adds latency, and still leaves depth unreadable.

## The "ears" in DX9 terms

The rules are the same as in the frame model; only the events are new.

| What | DX9 calls we watch | Notes |
|---|---|---|
| Targets | `SetRenderTarget(0)`, `SetDepthStencilSurface` | Surface ids; size/format/samples from `GetDesc`. |
| Draws | `DrawPrimitive`, `DrawIndexedPrimitive`, `...UP` | Depth test from `SetRenderState(D3DRS_ZENABLE)`. |
| Reads | `SetTexture` of the scene target's texture, `StretchRect`/`GetRenderTargetData` from it | `StretchRect` is how DX9 games resolve MSAA, which counts as a read. |
| Depth clear | `Clear` with `D3DCLEAR_ZBUFFER` | |
| Camera (shaders) | `SetVertexShaderConstantF` | The 256 float4 registers act as one "camera block". FindCamera scans it as it was at the scene draws; the offset is register × 16. |
| Camera (fixed function) | `SetTransform(VIEW / PROJECTION)` | Given directly, no search needed. |
| Frame | `Present`, `IDirect3DSwapChain9::Present`, `Reset` | `Reset` = resize/Alt-Tab: rebuild. |

**Jitter, the universal way: patch the vertex shaders.** Many DX9 games upload one
combined World×View×Projection per object, with no separate camera matrix to
rewrite. So at `CreateVertexShader` we rewrite the shader bytecode, a small, fully
documented token format. The final position is computed into a spare register,
and then `oPos.xy += jitter × oPos.w` is added from a constant register the shader
doesn't use. This works for every shader-based game, whatever its matrices look
like. Fixed-function games get it in `SetTransform(PROJECTION)`. Mip bias goes
through `D3DSAMP_MIPMAPLODBIAS`. DLSS is told exactly the jitter we added, as
for Vulkan.

**Motion vectors.** When a camera (VP + inverse, or fixed-function view and
projection) is found, motion comes from depth, as for DX11. When it isn't
(per-object WVP only), the helper uses optical flow. We already have the NVIDIA
optical-flow code (`host_dx12/motion/live_flow.hpp`), and its ~4 ms is
affordable for old games.

**DLSS point.** Same rule (before the first read of the finished scene colour)
for games with post-processing, which is most DX9 games from about 2007 on.
Older games draw the scene straight into the back buffer and then the HUD on
top. That needs one new rule: "before the first draw into the scene colour with
depth test off, after its last scene draw", falling back to Present. This rule
also helps DX11.

## Steps

| Step | What | Proves it |
|---|---|---|
| **D0** | **Probe on CI** (WARP has D3D12): create a DX9 device through D3D9On12 on our own D3D12 device, draw into an RGBA16F/RGBA8 target with D24S8 depth (and an MSAA surface), unwrap colour and depth, copy to an NT-shared texture, check pixels and depth values, return. 32-bit and 64-bit. Times the round trip. | That unwrapping depth works. This is the one fact the spec doesn't state. |
| D1 | 32-bit build of the game side (MinHook, frame model, camera pick, frame record) + a Win32 CI job. | Builds and the portable tests pass at 32-bit. |
| D2 | `d3d9.dll` proxy: off unless `[dx9] enabled=1`, otherwise system d3d9. On: `Direct3DCreate9`/`Ex` → D3D9On12 on our device and queue; borderless windowed; per-run log. | A synthetic DX9 game runs unchanged through it. |
| D3 | DX9 ears (table above) → frame model; back-buffer/HUD rule. | Synthetic game: scene, depth, camera at the right register, DLSS point before tonemap/HUD. |
| D4 | Transport: game-side D3D12 bridge (unwrap → copy, D24S8→R32F → shared textures/fence → existing helper → copy back → return). | Copy test (red/blue swap) seen at the tonemap and not before, as for DX11/Vulkan. |
| D5 | Jitter by shader patch + fixed function; mip bias. | The picture moves by exactly the jitter DLSS is told. |
| D6 | Motion: camera motion; optical-flow fallback in the helper. | Within 0.001 px on the synthetic camera; flow path flagged in the log. |
| D7 | `deploy-dx9.ps1` (install/rollback/collect, refuses an existing d3d9.dll), docs. | Then a real game on Cort's PC, when he says so. |

Order matters: **D0 first.** If unwrapping the depth surface doesn't work, the
fallback is to make the game's depth surface a readable format at creation
(the INTZ trick, which D3D9On12 may or may not support), and D0 checks that
too.

## Risks, plainly

- D3D9On12's speed and compatibility vary by game. D0 measures speed; games it
  can't run go to the dgVoodoo2 fallback.
- Depth unwrap is not documented (checked first, D0).
- Games with only per-object matrices get optical-flow motion, which is weaker
  than camera motion on fast pans.
- Scene-in-back-buffer games need the new HUD rule; until then they get DLSS at
  Present with the HUD inside it.
- MSAA scene targets: the resolve is the read; depth MSAA needs a sample-0 copy.
