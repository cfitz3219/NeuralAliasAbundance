# Sniper DLAA stability repair, 2026-11-07

Astra owns native DLAA. Fable owns Quality. The combined source includes Fable's
b66b1ea (including 7c3a577 and 557c1d6) plus Astra's boundary, reader-recovery, retired-selection and Home-capture
changes. The additive installer preserves the user's preferences. Cort owns game
testing. No game launch, game control, deployment or native game-file changes were
performed while preparing this repair.

## Saved evidence

`Documents/NA-se5-results-20261107-053559/status-44372.log` contains 397 inactive
DLAA transitions in the native-only interval, frames 2200–17034. 392 have an
intermediate-entry veto in the last rejection text. 370 have the same completed
boundary tuple as the previous frame. Another 23 match an earlier delivered
tuple. A completed tuple does not establish which callback ran at recording time.
These numbers describe transitions, not the total number of missed frames.

The older p40268 discovery evidence also shows a destroyed depth texture's votes
delaying its established replacement. It is a separate acquisition interruption.
Successful-evaluation captures are not used to count missed presents.

## Repairs

* Carry the exact matched remembered boundary through final validation. Previously
  a match to an earlier successful frame was rechecked against the immediately
  preceding, different frame and rejected. Writer occurrence, resource identity,
  destination and consumer checks remain. Late-written deliveries do not enter
  the successful-boundary memory.
* Keep the qualified actual-reader path available after the entry profile learns.
  Previously it was unconditionally disabled, so an intermediate operation taking
  the entry slot could strand the real reader later in the same recording. This
  path now admits the first actual read after the write when its submitted-frame
  consumer profile qualifies it. It does not enable the rejected barrier fallback.
* Exclude destroyed depth textures throughout their remaining vote history. The
  first repair excluded one only on the retirement publication, allowing the next
  publication to select it again. Retirement ids now persist within the existing
  16-frame window. Replacement still requires established evidence. Empty frames
  still count. This operates at publication time, with no draw-hook or GPU cost.
* Preserve the previously completed Home-capture repair. Present captures can
  include the Home overlay and record whether its draw was submitted. Existing
  DLSS and XeGTAO plane captures remain separate.

## Regression evidence

The new intermediate-entry GPU fixture alternates scene writers and places a
known intermediate shader before the terminal scene reader. Restoring the old
unconditional rejection produces 0/17 delivered downstream frames and 20 failing
assertions. The repaired version delivers all 17, with exactly one evaluation per
frame, no evaluation at the intermediate operation and no later scene writes.

A second GPU fixture alternates two previously delivered writer boundaries while
the terminal shader shares a known intermediate identity. It checks all 17
downstream images and one evaluation per frame, including remembered-boundary
recovery before the contextual entry profile learns the new role. An initial
counter assertion incorrectly required memory recovery on every frame even after
the contextual route qualified. The final test requires both remembered variants
to be exercised and every frame to deliver.

The repeated-retirement CPU test fails on the one-publication repair and passes
with persistent exclusion, including two destroyed textures in the same window.
Existing depth-selection tests still pass. The initial combined entry, real-NGX
and AO regression selection passed 10/10. Final combined receipts follow below.

The downstream tests use actual DX12 commands and a fake neural output marker.
A separate fixture evaluates NVIDIA's actual runtime. Neither substitutes for
Cort's visual confirmation of uninterrupted DLAA in Sniper. No new capture is a
prerequisite to delivering the engineering repair.

## Combined verification and Quality ownership

The broad integration run had 242 passes, four unsupported/optional skips and one
outdated observer assertion in 247 cases. That assertion expected a preexisting
resource to remain unknown after its view was created. The established view
registration deliberately discovers the live object. The corrected test verifies
its actual descriptor identity and still requires unknown creation method, initial
state and placement, with PREEXISTING/METHOD_UNOBSERVED provenance. No runtime
change was made for this test failure.

The subsequent combined rebuild passed all 50 affected tests in 141.73 seconds,
including that observer case, discovery, real NGX, inline/split/indirect and
decoy/boundary placement, native Quality, AO and Home overlay. Five isolated
installer checks passed for additive install, retained settings, corrupt-package
rejection, exact rollback and preservation of a foreign DLL.

Fable then posted b66b1ea while packaging was in progress. It is retained in the
final combined source and its affected tests are rerun. Its Quality-only warmup
counts preliminary admission. It does not include every possible refusal inside
record(), and frames with no learned-step attempt do not reset its counter. These
limits were sent to Fable in COMMS. It is not described as proof of uninterrupted
Quality delivery. Native DLAA does not use that gate. The native DLAA repair makes
the qualified reader and correct frame boundary available directly.

## Final package receipt

Final runtime source e96502a5e44926e19d36d791395cb11a297c9f88, including Fable b66b1ea. All27 affected GPU integration tests passed in90.03seconds after the final merge, including both new native DLAA regressions and the real NVIDIA evaluation. The preceding50-test combined qualification passed as recorded above. Final additive package Documents/NA-se5-motion-e96502a has19 hash-verified payloads. Core DLL SHA256 7F81F088E3E927245B9CE5A2895441F1F06DDC8F4BC44CE9205CE441D87690B9. Five isolated installer checks passed again for the exact final package.

The update, results, rollback and legacy DLAA CMDs all point to this package. NA-se5-dlss-previous.cmd retains b66b1ea. Original launchers are backed up under local/se5-dlaa-solid-20261107. No actual game deployment/control occurred. No new game capture was requested. Cort tests native DLAA by selecting DLAA in Picture > Render mode. Quality remains Fable's work with the limits above. Raw build/regression logs remain in the source worktree's build directory; package and installer receipts are in local/se5-dlaa-solid-20261107. The earlier a1d0666 candidate was never published to a launcher.
