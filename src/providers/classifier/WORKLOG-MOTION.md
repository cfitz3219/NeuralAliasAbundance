# A7 journal

## 2026-10-24 — reprojection evidence checkpoint

Continued independent work while C8/C9 acquisition is pending. Own branch
astra/a7-motion-discovery from main343a8f8. Added portable bilinear motion corroboration
with explicit jitter/scale, independent visibility, scoped adjacent-frame identity,
strict per-pixel contradiction checks and no promotion on flat/static imagery.
No host/core/ABI or game changes.

Fresh MSVC Release build succeeded. Initial8-case analytic tests passed. First
fixture corpus failed:32x24 had only one stable pixel after filtering checker edges,
but the test incorrectly demanded a positive result. Corrected expected behavior:
<16 checks must remain unavailable, retained the low-resolution case as a regression,
and added larger64x48,96x64,128x96 extents. Final12/12 CTests passed, zero skips.
160 fixture hypotheses:30 correct Consistent,90 incorrect Contradicted,40 low-resolution
Unavailable. No runtime visibility/semantic-confidence claim. Evidence under primary
build/a7-cpu/Testing/Temporary/LastTest.log. A7 remains in progress pending capture and
resource-candidate integration. Full A7 estimate2–4 working days; no attendance needed
for this checkpoint.
