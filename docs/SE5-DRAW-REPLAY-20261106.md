# Sniper Elite 5 object motion (TOX2-inspired draw replay)

## Installed behavior

The installed `dxgi.dll` SHA-256 is
`16BF72DD15C6FE0CDD8BBA7E4BD0FA42B433ABCF9C9E2951A3EA07B4D17A8FD5`.
It matches the additive install manifest for `3b16d38`. The user INI
has `[j2] draw_replay=1`. The saved run ending at frame 122723
(`NA-se5-results-20261106-062011/status-20404.log`) reports 31 pipeline
build failures, zero successful builds, zero replays and zero frames
supplied. Inline DLAA itself ran. The switch therefore had no replay
motion to compare in this run. The later staged `9952c4d` retains the
same replay code and was the base for the repair.

## Corrections

- Stream output requires a root signature that permits it. Preserve the
  exact serialized signature for each original or sampler variant and
  create a private copy with stream output enabled. Use a vertex-only
  pipeline with raster outputs disabled. Replay the graphics bindings
  on the private signature and restore the signature, bindings and
  pipeline the game was using. Log the DX12 error and shader hash for
  rejected pipeline variants.
- Capture on a single observed inline scene recording per frame. Initialize
  the counters in that recording. Consume current data on that same list,
  use submitted history only, and retain the recording's resources and
  descriptor slots until retirement. Skip unsupported stages, visible
  shader writes, predicated draws and a list with its own stream output.
- Match one draw in each frame by signature and geometry buffers. Drop
  duplicates and mismatched extents. Rasterize using the current
  viewport jitter and original scissor, then remove that jitter from
  the current screen coordinate before computing previous minus current.
  Require agreement with the visible depth. Native game motion retains
  priority. Camera and optical flow continue to cover unqualified pixels.
- Skip the old always-running replay probe when the feature is off.
  The overlay distinguishes waiting, pipeline rejection and motion made
  available to DLSS. Keep the default off and preserve the user choice.

## What the automated test covers

The DX12 fixture uses the actual draw hook, stream-output pipeline,
recording lease and replay rasterizer. It draws a triangle that moves one
pixel between frames while the viewport jitter changes. The pixel
readback checks the signed motion, depth mask and scissor. It also checks
the Off path, rejection of an unrelated recording, a discarded frame
and ambiguous draw matches. No NVIDIA model or game is involved in the
pixel readback. The scene is deliberately small.

## What the in-game comparison measures

Compare the same moving menu character with the camera held still. Keep
DLAA mode, model, optical flow, sharpening and the frame cap fixed.
Allow a few seconds after switching each setting, then use Capture DLAA
frames and Results. The provenance plane in each capture shows which
pixels used Tier 1. Compare those pixels to the corresponding area with
replay off. Compare sweatshirt edge trails over several frames and check
frame times from the same run. An active overlay row alone establishes
availability. If the target region has no replay coverage, the result
answers a coverage question and cannot tune its strength. Replay feeds
measured motion directly and has no strength slider.

## Hardware receipt

The final replay fixture measured +0.999578 px horizontally and
+0.000476 px vertically on the RTX 5070 Ti, against +1 and 0 expected.
The mask passed on the object and rejected the background and scissor
exterior. WARP produced +0.999578 and +0.000475. The DX12 debug layer
was enabled in both cases and reported no errors.
