# Vulkan hooks

The Vulkan twin of the DX11 hooks (docs/DX11-HOOKS.md). It is a Vulkan layer,
`VK_LAYER_NEURALALIAS_host` (`NeuralAliasVulkan.dll` + `NeuralAliasVulkan.json`),
that "listens" to a Vulkan game and decides where DLSS goes. It uses the same rules
as DX11, in the same code: `host_dx11/frame_model.hpp` (scene depth, scene colour,
camera block, DLSS point) and `host_dx11/camera_pick.hpp` (ViewProjection + inverse).

Nothing here knows any game. Every rule is about what the game *does*.

## Why DLSS runs inside the game here

For DX11, the picture goes to the DX12 helper and comes back. That cannot work
for Vulkan. A Vulkan game records the scene and the tonemap into **one command
buffer** and submits it in one go, so there is no moment between them when
another process could take the picture. The layer instead records DLSS into the
game's own command buffer, at the right spot, through NVIDIA's Vulkan DLSS (NGX
Vulkan). This works like the DX11 in-process backend (`bridge/dx11_dlss_inproc.cpp`):
same presets and flags, and the same `FrameRecord`. NGX is given the *next*
layer's function pointers, so its commands never come back through our hooks.

## How it is loaded

`scripts/deploy-vulkan.ps1 -GameExe <game.exe>`:

- copies the layer and `nvngx_dlss.dll` to `%LOCALAPPDATA%\NeuralAlias\vulkan\layer`
  (nothing goes into the game folder except the ini);
- registers it for this Windows user as an implicit layer
  (`HKCU\Software\Khronos\Vulkan\ImplicitLayers`);
- writes `NeuralAliasHost.ini` beside the game's exe.

Every Vulkan program then loads the layer, but it stays passive unless the ini
beside that program's exe turns it on. When it is off, the layer returns the
driver's own functions, so nothing of ours sits in the way. Setting the environment
variable `DISABLE_NEURALALIAS_VK=1` keeps the loader from loading the layer at all.
`-Action Rollback` removes the files, the registration and the ini.

```ini
[vulkan]
enabled=1        ; turn the layer on for this game
jitter=0         ; 1 = inject sub-pixel camera jitter (and mip bias); off for the first run
mip_bias=-1.0    ; added to new mip-mapped samplers when jitter=1
preset=12        ; DLSS preset (12 = L, same as DX11)
copy_test=0      ; 1 = no DLSS, a copy instead (checks the plumbing only)
log=             ; default %LOCALAPPDATA%\NeuralAlias\vulkan\NeuralAliasVulkan-<pid>.log
```

## What happens each frame

| Step | Rule | Where |
|---|---|---|
| Device | The first device that makes a swap chain. | `CreateSwapchainKHR_h` |
| Images | Each render target gets a "kind" (format, size, samples, mips, layers, attachment use). Engines rotate 2–3 copies of the scene target, and all copies share one kind, so the selection holds steady. When DLSS runs, it works on the copy actually being drawn. Colour targets get copy access and depth targets get sampling access at creation (only where the format allows it). | `CreateImage_h` |
| Scene depth / colour | Same as DX11 (ReShade's depth rule, then the colour drawn most with it), on the kinds. | `frame_model.hpp` |
| Camera | The vertex-stage uniform buffer, or the vertex push constants, bound for the most scene draws. Per-object blocks are left out, same as DX11. Their bytes are read at `vkQueueSubmit`, from the game's mapped memory or from the staging buffer it copied in. | `CameraNow` |
| A "read" | What Vulkan makes a game say before it reads a colour target: a barrier that makes colour writes visible to reads, a render pass that ends with the image in a read layout, or a copy, blit or resolve from it. | `OnRead` |
| DLSS point | The first read of the scene colour after its last scene draw, learned each frame as a *tag*: where that read sits in its command buffer (how many reads and passes came before it, whether scene draws did, and what kind of read it is). In the next frame, DLSS is recorded right before the read with that tag. A tag that names two reads is not used (counted as "not unique"). | `frame_model.hpp` tags |
| Recording DLSS | Copy the colour out. Build camera motion from depth (`shaders/camera_motion.comp`, the Vulkan twin of the arbiter's tier 2). Run DLSS. Write the result back. Put the colour and depth back in the layouts they were in, and rebind the game's compute pipeline, sets and push constants. | `RecordInsert` |
| At submit | Command buffers are replayed into the model in GPU order, the same way as DX11 deferred lists. The camera-motion matrix is composed in double, exactly as for DX11, and written into this frame's slot before the GPU runs it. | `Replay` |
| Clip space | Both kinds are handled: Vulkan's own (+Y down, positive viewport) and D3D-style (negative viewport height). Motion vectors and jitter are checked in both. | `y_down` |
| Jitter (optional) | Written into the game's camera block in mapped memory at submit, before the GPU can see it. Only the camera whose aspect matches the screen gets it. Mip bias goes on new mip-mapped samplers. | `InjectJitter` |
| Secondary command buffers | Draws inside them take the primary's targets at `vkCmdExecuteCommands`. DLSS is never placed inside a secondary. | `Replay` |

## Threads and locks

Games record command buffers and update descriptors from many threads at once,
so the hooks on those paths are kept off shared locks:

- Recording state is per command buffer, found through a per-thread cache.
- The device lookup on every call uses a per-thread cache too (no lock unless a
  device or instance was just created or destroyed).
- Descriptor sets live in 64 separately locked shards. An update or a bind locks
  only its set's shard. Updates that write no uniform buffer (images, samplers,
  storage buffers: most of them) take no lock at all.
- A bind reuses per-thread scratch space, so it allocates no memory.
- The frame model itself is only touched at `vkQueueSubmit`.

The layer's added cost per call fell by about a third with this (local bench on
lavapipe, 1 and 4 threads, about 300 ns down to about 200 ns per call).

## The log

One file per run. It records the device, the swap chain, the selection, where the
camera was found, the layer's own resources, every skip reason (logged once each
time it changes), and a summary line every 60 frames for the first 600 frames,
then every 600 frames. The summary has: draws, scene draws, colour reads, whether
a point is learned, then inserted, delivered, refused, no-camera, failed, missed,
contradictions, not-unique, camera share and jitter writes.

## Tests

`vk_layer_synthetic_game_*` runs on Linux CI on Mesa lavapipe, a CPU Vulkan driver.
A small Vulkan game runs under the real layer: a shadow pass, an RGBA16F scene with
D32 depth, a camera block with the ViewProjection at byte 64, per-object blocks
with dynamic offsets, a mid-scene copy, the tonemap read, a HUD and a present.
The copy test swaps red and blue, so the test can see which read got the
processed picture.

- `plain`: checks the selection, the camera at byte 64, DLSS before the tonemap
  and not at the mid-scene copy, that draws are counted once, a resize, and the log.
- `jitter`: checks that jitter reaches the GPU in rows 0/1 only, that the picture
  moves by exactly the jitter DLSS is told, and the mip bias.
- `motion`: the camera moves; the camera motion vectors are within 0.001 px of
  the true motion.
- `flip`: the same with a D3D-style projection and a negative viewport.
- `dynamic`: Vulkan 1.3 dynamic rendering, with half the scene drawn from a
  secondary command buffer.

With `NA_VK_TEST_VALIDATION=1` the test also runs the Khronos validation layer,
synchronization checks included, *below* our layer, so it checks the commands we
add too. It reports nothing about the layer's commands. (Its remaining messages
are the test game's own leaks at exit and a render-pass dependency in the test.)
The NGX Vulkan code is compile-checked on the Windows SDK CI job. It has not run
on an RTX GPU yet.

## Not done yet (honest list)

- Not run in a real game yet, and DLSS itself (NGX Vulkan) not yet run on an RTX GPU.
- Render-small upscaling (Quality/Balanced/Performance), RCAS and the in-game menu.
- DLSS's reset on a camera cut lands one frame late, because DLSS is recorded
  before the frame's camera is known (the camera motion itself is not late).
- No jitter for cameras in push constants or `vkCmdUpdateBuffer`. Those are found,
  but jitter is written only into mapped memory.
- MSAA scene colour and depth are refused with a reason.
- Not handled yet: descriptor buffers (`VK_EXT_descriptor_buffer`), push
  descriptors, and buffer device addresses (cameras read through a pointer).
  A game that uses only these for its camera gets "no camera".
- Scene targets made before the layer saw them (layer loaded late) lack copy
  access, so they are refused with a reason.
- A game that rotates more than 16 depth buffers is refused.
