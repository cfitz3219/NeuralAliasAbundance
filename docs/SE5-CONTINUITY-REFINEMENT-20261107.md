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

### Completed automated validation

- The broad 255-case suite finished with 251 PASS, 4 SKIP, 0 FAIL (498.96 s).
  The optional NR runtime probe and three unsupported 16x MSAA cases skipped.
  This run preceded the final combined rebuild and latest fixture registrations;
  do not describe it as an immutable full-suite run of the final package.
- Combined 2523220 (including Opus a42171b unchanged): complete Release build;
  all 99 affected tests finished, 96 PASS, 3 unsupported MSAA16 SKIP, 0 FAIL
  (280.30 s), including five real-NVIDIA cases and the new input/camera cases.
- Final runtime 1ebb7b7d243dac63ffff8bb6759f4a0762a75a66 adds only the cleanup/device
  access completion above. Complete Release build passed, then all five direct
  input-format, camera-gap and stale-boundary regressions passed (16.33 s).
- `git diff --check` passed. Source is pushed on astra/se5-quality-continuity,
  PR126 into claude/se5-tox2-gates. No new live-game acceptance is claimed.

### Package and handoff

Prepared Documents/NA-se5-motion-1ebb7b7 from Opus's a42171b additive installer,
with final runtime 1ebb7b7. DLL SHA256:
`B3390C50E447E40530BBD576BB8D7290B600A0451C013C70D9962A2DD8404659`.
All 22 payload hashes and five isolated installer checks passed: additive install,
menu-setting preservation, corrupt-package rejection, exact rollback and foreign
DLL preservation. The usual DLAA, DLSS-update and DLAA-fix-update CMDs, plus their
Results/Rollback CMDs, now agree on this package. Previous launchers are preserved
under main local/se5-continuity-20261107/public-cmd-before. Dedicated TOX2/play
launchers are unchanged. No game installation or launch occurred this turn.

Main COMMS/HANDOFF receipt 61a0a71 gives Opus the complete source/package chain;
installation remains with him. CI was still running when handed off. Prior game
acceptance of preceding repairs is not acceptance of this new package. The
package contains this report through the completed automated validation section;
this post-packaging receipt lives in source to avoid changing validated payloads.
