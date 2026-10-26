# A6 camera candidates — offline checkpoint

matrix_candidates.hpp scans float32 CPU snapshots (4-byte aligned, at most 1 MiB)
for perspective projection and rigid-view matrix hypotheses in row/column storage.
At most 256 hypotheses per kind are returned. Overflow discards that kind and
sets its explicit overflow flag, while scanning continues for the other kind.
Snapshots retain session/epoch/submission/resource/content identity and synchronization
status. App-sync-unknown stays unknown: copying CPU bytes does not prove GPU values.
No wire ABI or host memory access is introduced.

Perspective coverage: conventional D3D normalized depth, left/right handed,
forward/reverse depth, finite/infinite far, off-axis offsets. Rigid views require
orthonormal axes with determinant +1 or -1, recorded as handedness. A handedness
change is a discontinuity. Unsupported shapes remain undiscovered;
this includes packed 3x4, oblique projection, arbitrary combined view-projection,
non-float32 or scaled coordinate conventions. Those are acquisition work, not
claims of absence. Object transforms/shadow cameras may have identical structure.
All matches remain hypotheses; there is no selected/qualified camera field.

MeasureOffset compares a candidate to a separately established unjittered projection
of the same camera. Without that independent baseline the result cannot be called
jitter; principal-point offsets may be intentional. CompareViews reports a thresholded
translation/rotation discontinuity only for caller-proven adjacent frames with
matching session/epoch/resource/offset/layout. It is not a proven camera cut or a
hidden temporal classifier. Game-specific units and timing require calibrated
thresholds. Camera reset/resize or gaps must invalidate the caller's lineage.

Windows Release tests: 7 negative/structural cases plus 216 embedded hypotheses in
72 fixture snapshots (12 camera frames, 3 extents, 2 layouts). Matrices are embedded
at randomized locations in noise; candidate output carries no truth labels. Tests
check near/far, inverse-view position and injected projection offsets, deliberately
retaining camera-shaped object-transform decoys as ambiguous. No live H3 snapshot,
semantic camera selection or end-to-end A6 acceptance is claimed.

Next: C9 snapshot ingestion when available, independent scene/depth corroboration
with A5, more camera conventions, and calibrated temporal jitter/cut inference.
