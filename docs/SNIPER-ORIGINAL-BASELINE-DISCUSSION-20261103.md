# Sniper restart: original baseline, discussion before implementation

Cort rejects0403a0b as unchanged and directs a full return to the original
starting build. His next instruction is to post the problem, proposed fix and
failed approaches in COMMS, exchange responses with Claude, and agree on a
likely fix before applying another rendering change.

## Restored starting point

The initial Sniper handoff and Cort's subsequent explicit rollback identify
6f6b806ccb7b276a65410e4952f0c34ee5911aec as the pre-repair baseline. This branch
starts directly there. No source, tests, build settings, shaders or scripts from
the later repair branches are mixed into it. Later commits remain archived.

The standard Documents/NA-se5-camfix-update/results/rollback CMDs now select
Documents/NA-SE5-camfix-original-6f6b806. Its original saved DLL hash is
F40312ACDD98182A59C3795C97A0CA072FAFAA28AE7E6BD9C2EAD4EE561BB38C.
This is a byte-for-byte copy of the preserved historical package, not a rebuild.
Native NGX runtime and attribution files are also unchanged.

The copied installer restores original presetL, sharpness100, mip bias-1,
flowOn, automatic routing and the original matrix-jitter behavior. This prevents
recent experimental menu settings carrying into the baseline. Only this
packaging reset and an explanatory note are new; rendering is original. Prior
settings are archived by the existing reversible installer. The older menu
layout returns as part of restoring the whole build.

All package hashes and a temporary-folder0403->exact6f->original-file rollback
pass, including original settings and preservation of a native fixture file.
Evidence: main workspace local/se5-original-reset-20261103/restoration.json and
roundtrip.log. The installed game still contains0403 until Cort runs the updater;
Astra did not deploy, launch or control the game. The actual rejected DLL and
run7472 logs are separately archived there. No fresh rendering fix is applied.

## What the latest run changes in the diagnosis

Actual installed0403/F692958F and manifest0403 are verified. Run7472 reports
1712evaluations/1717observed scene frames, no state/camera/NGX/busy failures,
808Close recoveries and999exact waits. Capturej2-20261103-021203 has30complete
records: raw output marker30/30, final15/30. Fourteen ordinary barrier placements
deliver; all14Close placements lose the picture; the two exact-consumer
placements split one delivered/one absent. Selected colour/depth IDs remain
1441/1439. The producer-scoped wait correction was therefore insufficient in
the real game, despite fixing the controlled alternating-producer fixture.

The prior90-framea7f comparison and independent40-Presents capture establish
real final-image alternation, including with jitterOff. They do not prove the
exact hidden GPU operation. Current raw-output captures precede RCAS and
copy-back. A fault in sharpening/descriptor lifetime, copy-back destination/state,
or a downstream read of an earlier/different image still needs separating.

## Discussion and proposed direction

Full chronological failed-change analysis and the proposed discriminator are
posted to main COMMS.md in d9c0579, section "requested diagnostic discussion
FIRST; no further fix implementation". That packet covers predicated NGX,
consumer-only admission, raster/sample changes, camera/flow changes, exact-read
priority, Close recovery, full-range descriptor matching and producer-scoped
waiting. Automated fixture success is explicitly separated from actual game
improvement. Anno images in shared capture folders are excluded from Sniper
qualification.

Proposed first experiment, pending discussion: reuse the existing asynchronous
capture service for a few samples that distinguish post-RCAS output from the
scene subresource immediately after copy-back, with submitted recording/Present
identity. Do not build another recorder, add continuous readbacks or wait for
the GPU in live callbacks. Claude is asked to challenge whether this separates
the causes and inspect the write-back/state-restoration/first-read source path.

The proposed fix is conditional on that evidence: repair the one demonstrated
source/order/lifetime defect while retaining the original earlier opportunity.
Do not globally wait longer, count late Close work as delivery, add evaluations,
or extend jitter coverage as a substitute. If sharpening or copy-back is faulty,
fix it there instead of moving placement. No implementation or instrumentation
has been applied; further exchanges occur in COMMS before a likely fix is chosen.

This restores a starting point with known residual flicker. It is not a claim
that the original build was flicker-free or that a new repair is qualified.
