# Production evidence acquisition — first slice

`SampleAcquisition` is a single-worker component backed by `HostSampleApi`.
It arms at most four whole-subresource requests, polls once per worker pump,
copies only completed READY data into owned memory, then releases host leases.
It never waits, maps GPU memory or runs work in the observation callback.
Stop/resync must call `Clear`/`Scope`; the backend outlives the collector.
Outputs retain the **returned** session, epoch, queue and queue ticket, resource,
subresource and format. A request does not identify the eventual submission.
Callers must also bound retained outputs; the four-lease limit is not an output
history budget. Numeric decode currently supports D32_FLOAT/R32_FLOAT only,
including padded rows with an unpadded final row. Typeless/packed formats need
explicit view interpretation before a decoder can be added.

`AcquireDepthFrame` requires a separately established camera/frame association
for the exact returned queue/ticket. It refuses request-time or stale camera
identity. The fixture knows that association because it owns the submissions;
the live host must establish it. CPU matrix shape matches or APP_SYNC_UNKNOWN
snapshots cannot set `qualified` by themselves.

`CheckTemporalDepth` compares adjacent depth versions using camera reprojection,
without fixture world positions or a supplied visibility mask. It checks scope,
adjacency, queue ordering, extents, finite values, invertibility, resets, sky,
overlap and discontinuous interpolation footprints. Initial evidence thresholds
are 16 checked pixels, 75% eligible coverage, 95% agreement and camera-induced
motion on at least 25% of checked pixels. Stationary/jitter-only pairs remain
uninformative. Reverse-Z is represented by the actual projection, not guessed.

Agreement maps describe a **static-surface hypothesis**, not proven visibility.
They are independent of candidate motion vectors but depend on the proposed
depth and qualified camera. Two self-consistent wrong buffers can still agree;
agreement cannot establish semantic depth, whole-image MV validity or trust.
Moving geometry, sky, transparency, dynamic resolution, cross-queue pairing and
large disocclusions require more evidence. This module does not enable DLSS.

R1 remains open for generic camera/frame association, independent visibility
corroboration and pre-HUD color evidence. In particular, C9b samples the end of
a submission: it cannot recover a pre-HUD version overwritten later in the same
submission. H2 pass ordering alone cannot certify that end-of-submission bytes
contain no HUD. Acquire an independently retained source version or add an
explicitly reviewed pass-boundary mechanism; do not relabel final color.

Claude owns the off-by-default read-only host activation/worker integration.
Astra owns these consumers, fixture accuracy evidence and the Anno report.
No game deployment or live activation is part of this checkpoint.

## Completion path for diagnostic confirmation

`classifier/production_checks.hpp` now combines metadata and content checks:
`ConfirmDepth` ties the main-scene write to temporal depth evidence;
`ConfirmSceneColor` checks a preserved source actually consumed by the final
composition against final color outside bounded later overlay draws; and
`ConfirmMotion` derives the visibility estimate from depth/camera agreement
before evaluating candidate vectors. The latter requires no oracle stable mask.
Color requires no foreground/alpha oracle. H2 must establish source/version,
presentation lineage, complete write coverage and overlay bounds; missing
evidence returns Unavailable rather than inventing those facts.

`Confirmation::Supported` means a diagnostic candidate supported by that
observed model, never a trusted DLSS input. The three APIs return explicit
reasons/coverage and accept observable data rather than fixture-only truth.
Unknown camera association and overwritten scene sources remain legitimate
per-game unknowns to report. A5's fixed fixture-v2 acceptance measurement is in
docs/A5-ACCEPTANCE.md; generic host acquisition accuracy is a separate metric.
