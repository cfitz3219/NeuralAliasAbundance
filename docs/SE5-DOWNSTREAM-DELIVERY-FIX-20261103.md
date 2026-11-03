# Sniper downstream-delivery correction

## Reproduced defect

Cort's 53295cb captures show successful, stable DLSS outputs whose NVIDIA
indicator intermittently disappears from the actual presented picture. See
SE5-FINAL-PICTURE-FLICKER-20261103.md for the measured live evidence.

Following the output-ownership lead uncovered a concrete blind spot in the
current observer: before_shader_use inspected at most64 SRVs in each descriptor
range. A valid scene-colour SRV beyond that prefix was invisible. When a recent
known consumer caused J2 to defer the preceding barrier, the unobserved actual
post draw consumed the original image. The Close recovery then evaluated DLAA
AFTER the draw had carried the image onward. This counted as successful DLAA
with no subsequent selected-colour writes, although it did not affect the
downstream image. Merely relaxing the one-evaluation rule or changing jitter
would not correct that ordering.

The negative control uses actual DX12 draws and readback with the existing fake
NGX runtime: a96-entry table moves the scene from entry0 to72 on alternate late
frames. Shipped observer code produces23 successful evaluations out of24frames
(one cold), zero input-state errors, but only10/17 checked downstream pictures
receive the replacement. The seven failures contain the exact raw half-float
RGB3266/34cc/3666 (.2/.3/.4), while Close recovers7 nominal evaluations. This
reproduces the observed discrepancy between an evaluation counter and delivery.

## Correction

Reuse the existing descriptor metadata: shader-visible heaps now maintain a
sparse resource-ID-to-SRV-slot index as descriptors are created, copied and
overwritten. Before an actual shader use, query the selected resource's indexed
slots within the full bound range, in addition to the existing small role-data
prefix. Respect the bound heap, GPU table base, explicit range offset, range
length and actual heap extent, including unbounded ranges. No full-heap scan per
draw, extra GPU dispatch, readback, wait, resource equivalence or frame-admission
relaxation is introduced. The actual post draw can now trigger DLAA before it
consumes the image; Close remains available for the separate-list case it was
intended to recover.

The initial corrected reproduction delivers17/17 downstream pictures with the
same23/24 evaluation count and0Close recoveries. The registered GPU regressions
extend it to entries0/72/95, direct descriptor writes and descriptor copies, a
GPU table base17 and range offset11, null-overwrite invalidation and both finite
and unbounded root ranges. They verify each checked downstream pixel, exact
source-change telemetry, zero late writes and zero camera/state failures.

Normal existing pan, changing final geometry, optional later scene segment,
split-consumer, raster-sample and auto-route checks are retained. Their initial
nine-case focused run passes24.87s. Final broad qualification is recorded below.

## Capture evidence and limits

The existing status line adds wide_scene_reads, counting shader uses whose
selected-colour SRV lies beyond the prefix. Existing F8 metadata adds placement
and at_close, identifying the insertion trigger and Close recovery per captured
evaluation. This can distinguish the previously unobservable late recovery in
the next user capture. No extra raw plane or continuous diagnostic readback was
added. Capture workers, fences and retention are unchanged.

This fixes a source-proven, GPU-reproduced delivery defect. Existing saved game
captures do not contain descriptor indices, so they cannot independently prove
that this is the sole cause of Sniper's live flicker. No new game run, deployment
or live visual pass is claimed. The fixture uses a stand-in DLSS model; it proves
command ordering and actual downstream contribution, not DLSS image quality.
This is descriptor-table support, not a claim of complete SM6.6 direct-heap or
late-after-record descriptor coverage.

Microsoft's descriptor-range contract permits finite and unbounded ranges and
nonzero offsets;64 is not an API limit:
https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_descriptor_range
The dynamic-indexing fixture follows the documented HLSL5.1 mechanism:
https://learn.microsoft.com/en-us/windows/win32/direct3d12/dynamic-indexing-using-hlsl-5-1

Local receipts/scripts: C:/Users/Cort/NeuralAliasAbundance/local/se5-delivery-fix-20261103.
Negative-control logs retain the original minimal96-entry/zero-base case; final
regressions include the extended copy/offset/unbounded cases described above.

## Final automated qualification and package

Runtimea7fce1f87dd56b0b8c55abc9a45c1c97a174ec2e: full rebuilt Release and
173registered CTest cases complete in237.52s,168pass/5skip/0fail. Both expanded
finite/unbounded cases deliver17/17 checked pictures with23/24evaluations,
0Close recoveries and0late/camera/state errors. Existing pan, optional-tail,
split-consumer, raster, real-NGX, observer, overlay and input cases pass. Skips
remain the opt-in NR probe, three unsupported16xMSAA cases and upscaler-jitter.
Detailed per-case output is preserved from CTest's LastTest.log without reruns.

Documents/NA-SE5-camfix-a7fce1f is published through the usual
NA-se5-camfix-update/results/rollback CMDs. DLL SHA256:
57A568AA2E7FF65F1AE7CFB488B023903C9D052FBEA101FBA40EE4885F9D5421.
The same reversible installer is reused; result summaries retain profile and
wide-read counters and explicitly distinguish evaluation from delivery.
Temporary-folder WindowsPowerShell019->candidate->019->original roundtrip
passes, including raster mode, retained sharpness and native-file preservation.
All payload hashes pass. Launchers were backed up before-a7fce1f-20261103-011524;
019known-good,6fbaseline and all earlier packages remain available.

The actual installed game DLL stays53295cb/41C3805A... before and after this
publication. No game launch, control or deployment occurred. Cort performs the
live picture test; this receipt does not promote the fixture to a game pass.
PR81 contains the correction and updated scope/validation description; no
additional review cycle is required by this delivery.
