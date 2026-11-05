# DX11 hooks

The part that "listens" to a DX11 game and decides where DLSS goes. The DLSS
side is unchanged: the bridge (docs/DX11-BRIDGE.md) carries the picture to the
DX12 helper, which runs our existing DX12 DLSS code. The helper also builds the
camera motion vectors with our existing motion arbiter, the same way J2 does in DX12.

Nothing here knows any game. Every rule is about what the game *does*: which
depth buffer gets the most geometry, which constant buffer is bound for the
scene, and when the finished scene is first read.

## How it is loaded

It uses our existing `dxgi.dll` (NeuralAliasDXGI.dll) next to the game's exe.
`NeuralAliasHost.ini` beside it turns it on:

```ini
[dx11]
enabled=1        ; start the DX11 host
jitter=0         ; 1 = inject sub-pixel camera jitter (and mip bias); off for the first run
mip_bias=-1.0    ; applied to new mip-mapped samplers when jitter=1
preset=12        ; DLSS preset (12 = L, Cort's choice for DLAA)
cpu_wait=0       ; 1 = the CPU waits for the helper (slower, can never stall the GPU)
copy_test=0      ; 1 = no DLSS, the helper copies the picture (checks the plumbing only)
flow=1           ; optical-flow object motion in the helper (AMD FidelityFX); changes live
flow_loosen=50   ; flow acceptance looseness 0..100 (0 = the strict Anno-run gates); changes live
log=             ; log file; default %LOCALAPPDATA%\NeuralAlias\dx11\NeuralAliasDX11-<pid>.log
```

`na_bridge_helper64.exe` and `nvngx_dlss.dll` go next to the dll.

## What happens each frame

| Step | Rule | Where |
|---|---|---|
| Device | The first swap chain that presents with a D3D11 device. A new swap chain on the same device is adopted. | `OnPresent` |
| Scene depth | ReShade's generic-depth rule: the depth buffer with the most vertices, aspect within 0.1 of the screen, 0.5–1.85× its size. Skips frames with one depth and ≤ 8 draws (loading screens). | `frame_model.hpp` |
| Scene colour | The render target drawn most with that depth, at the same size. | `frame_model.hpp` |
| Camera | The vertex-shader constant buffer bound for the most scene draws. Buffers re-uploaded for more than a quarter of those draws are skipped, because those hold per-object data. In it, a ViewProjection whose inverse is in the same block (shared `na::camera::FindMatrices`, the same rule as J2). The offset is learned, then re-checked every frame. | `camera_pick.hpp` |
| DLSS point | Right before the game first *reads* the scene colour after its last scene draw (the tonemap or post pass). That is before the HUD, and before the HDR/SDR conversion. The point is learned again every frame. If a scene draw comes after DLSS ran, it is logged as a "contradiction". | `frame_model.hpp` |
| Deferred contexts | Command lists are recorded per context and replayed into the model when the immediate context executes them. DLSS can run before an immediate read, or before a list whose first scene read is the target. If the scene and its read are in one list, it is logged as "unplaceable" and never forced. | `dx11_host.cpp` |
| DLSS | DLAA at the scene colour's size, in place. Camera motion is built in the helper from depth + one reprojection matrix composed in double precision. HDR is on for float colour, reversed-Z comes from the camera, and the history resets on camera cuts. | `FireBeforeRead` |
| Jitter (optional) | The main camera's ViewProjection (and its inverse) are rewritten in the game's own upload, before the GPU sees them. Only the camera whose aspect matches the screen is changed, so shadow and reflection cameras keep theirs. The camera tracker is told the exact sample. | `JitterUpload` |

Changing the selection takes 3 frames in a row, so one odd frame doesn't flip it.

## The log

One file per run. It records the device, the selection (sizes and formats), where the
camera was found, every skip reason (once each time it changes), and a summary
line every 60 frames for the first 600 frames, then every 600. The summary has
draws, scene draws, the learned read, and fired, delivered, repeated,
not-ready and failed counts, plus no-camera, missed, unplaceable,
contradictions, camera share, lists and jitter writes.

## Tests

- `dx11_frame_model` (portable, Linux CI too): selection, the point before the tonemap
  (not a mid-scene copy), loading screens, hysteresis, deferred lists, the
  contradiction check, camera versions, and the camera pick in both matrix layouts.
- `dx11_host_synthetic_game` (Windows CI): a small DX11 "game" under the real hooks,
  with a shadow pass, an FP16 scene with D24S8 depth, a camera constant buffer, a
  tonemap read, HUD and Present. It checks the selection, the camera at its byte
  offset, DLSS before the tonemap (helper in copy-test mode, motion pass really
  runs), jitter seen by the GPU in rows 0/1 only, mip bias, deferred contexts, and
  a resize.

## Not done yet (honest list)

- Not run in a real game yet.
- Render-small upscaling (Quality/Balanced/Performance), RCAS and the in-game menu.
- MSAA scene colour (refused with a reason).
- Scene colour alpha: the bridge writes alpha = 1. A game that keeps data in the
  scene's alpha channel could show it.
- 32-bit games (the code is 32-bit clean; no 32-bit build yet).
- A game that swaps whole context states itself (`SwapDeviceContextState`) is
  not tracked.
