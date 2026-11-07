# Sniper DLAA stability repair, 2026-11-07

Astra owns native DLAA. Fable owns Quality. The combined source includes Fable's
7c3a577 plus Astra's boundary, reader-recovery, retired-selection and Home-capture
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
