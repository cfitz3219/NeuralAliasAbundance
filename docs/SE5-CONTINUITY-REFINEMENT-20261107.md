# Sniper continuity refinement after 8503d19

## Evidence and scope

Cort reported that the combined build was substantially better, with a few
remaining flickers and complete Quality dropouts. One flicker was in the menu;
another may have coincided with XeGTAO settings. Evidence is the saved
`Documents/NA-se5-results-20261107-170815`, process 2740, source 8503d19.

The old recurring three-on/two-off loop is absent from this run. Only six
remembered-boundary rescues and five retirements occurred. Preserve abf70fd,
the viewport-edge repair, and Opus's TOX2 changes.

Quality reached a 15,189-delivery uninterrupted streak. Its remaining failures
include 3,269 refusals to append reconstruction after a recording that already
contains later work. The upscale remains the same operation, but its two
render-size inputs alternate. `input_last_draw` changes between 1 and 0 during
the interruptions. Last-drawn-image prediction consequently becomes unavailable
or selects an unsuitable input. The final image cannot be repaired at the end
of that recording because the HUD/composition has already read it.

This is not evidence that XeGTAO itself disables DLSS. Its slider activity and
the Quality dropouts must not be equated. Neither shader IDs nor game-specific
resource addresses are used by the repair.

## Quality repair: sample the actual GPU binding

The existing execution-resolved trace now retains the input's root-table
parameter and descriptor offset. Two agreeing traces qualify that coordinate
alongside the existing shader, dimensions, output, view and scene-lineage checks.

For late-bound compute upscale steps, the input copy reads that SAME descriptor
from the game's heap during GPU execution, immediately after the game's upscale
and before our reconstruction. This avoids freezing a stale CPU descriptor and
avoids guessing which ping-pong render target was last drawn. The heap is read,
never modified. Existing state restoration returns the list to the game.

A small compute copy writes a private byte-address buffer through a root UAV,
then copies its placed footprint to our existing DLSS color input. Using a root
buffer UAV allows the game's resource heap to stay bound. The implemented
formats are RGBA8 UNORM and RGBA16_FLOAT; other formats retain their existing
route. The buffer is allocated/reused with the existing six completion-qualified
slots. The recording lease retains the heap, root signature, pipeline, scratch
buffer and ordinary reconstruction resources. No GPU readback/wait is added to
game callbacks, and no game descriptor or native DLL is replaced.

This follows Microsoft's descriptor lifetime model: volatile table contents may
be filled after recording but must be ready and preserved while submitted work
uses them. References:

- [Root Signature 1.1](https://learn.microsoft.com/en-us/windows/win32/direct3d12/root-signature-version-1-1)
- [Advanced use of descriptor tables](https://learn.microsoft.com/en-us/windows/win32/direct3d12/advanced-use-of-descriptor-tables)

The source texture is only read through its existing shader-readable binding.
The metadata template's state is neither guessed nor transitioned. Depth,
producer ordering, destination state and recording lifetime checks still apply.

## DLAA repair: retain a recoverable scene handoff

The separate-consumer path previously threw away a valid scene handoff when its
local camera scan failed. That prevented `record()` from using its existing
current-J1/recent-camera recovery path. It now retains that handoff with an
explicit `has_camera=false`; the common validator decides whether recovery is
possible. Existing age limits, reset restrictions, depth/state checks and
prediction rules are unchanged. No jitter is claimed for an unjittered held
camera.

## Reproductions and validation

- New late-input fixture draws both same-size images but deliberately upscales
  the earlier one. Descriptor contents are dummy textures at record time and
  are filled before submission. It checks the actual pixels entering NGX,
  final output, and the later native HUD. Restoring 8503d19 runtime produces
  205 failed assertions. Repaired RGBA8 case passes; RGBA16_FLOAT variant also
  passes. Rotating-input and merged-layout regressions pass.
- New separate-consumer camera-gap fixture removes the camera data on two
  otherwise valid frames. Previous code delivers 15/17 settled final images,
  with six failed assertions. Repair delivers 17/17 with no state/camera errors
  or late scene writes. It also requires zero declared jitter on the unreadable
  frames, rather than comparing nonexistent matrices.
- These tests use real DX12 execution/readback and a fake NGX output marker.
  They prove routing, input pixels and downstream composition, not NVIDIA image
  quality in the game. Full-suite and package receipts are appended below when
  completed. Earlier manual F16 test output from a locked/relink-failed binary
  was discarded; the accepted F16 output explicitly reports input/output format10.

The saved run also contains initial acquisition and occasional producer-order
admissions. These are not silently relabeled as successful. No universal
zero-miss or new live-game acceptance claim follows from this automated repair.

Local detailed logs: `C:\Users\Cort\NeuralAliasAbundance\local\se5-continuity-20261107`.

### Final resource-lifecycle pass

The late-input scratch buffer is released with its completion-qualified slot at
J2 shutdown; submitted recording leases keep their own references. This avoids
reusing a buffer from a previous device when a recreated device has equal-sized
images. Reading the descriptor increment now holds the existing device mutex,
so shutdown cannot null the device during that access. No game-path GPU wait is
introduced. This is a lifecycle completion of the new copy, separate from the
captured interruption causes.
