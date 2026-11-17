# Sniper placement repair on the original 6f6b806 baseline

## What changed

The final mesh and its draw count change as the camera pans. They are poor identifiers for where post-processing begins. This repair learns the first actual graphics/compute operation after a selected-colour handoff: pipeline object ID, root layout, destination and size. It observes draws, dispatches and indirect operations even when the colour descriptor is unresolved. A table setting is not treated as an operation or proof of a read.

Only complete **submitted frame sequences**, across all observed Execute calls, train this small profile. Three terminal observations qualify a key. Seeing that key before later scene writes disqualifies it. Learning is structural evidence, not a claim of final-screen delivery. Resetting the scene selection clears it. Storage is bounded at 512 keys.

Once qualified, this operation triggers the existing single inline evaluation immediately before that operation. The original matcher remains the warm-up route. There is no Close recovery, global wait for a later consumer, predicated NGX evaluation, broad descriptor scan, new jitter scheme or command-list split. Existing ownership, camera, resource states, GPU retirement and history handling remain in force. MSAA depth keeps the original boundary path because its graphics resolve has additional raster-state effects. Real selected-colour copies keep their existing consumer path and cancel a pending shader entry.

The regular F8 metadata includes the entry pipeline/root/destination, recording, operation index and barrier ordinal, plus process ID. `j2_entry` reports learned key count and actual entry matches. An unresolved descriptor still does not become a synthetic resource-read event in the original analyzer.

## Automated evidence

The new GPU fixture uses 24 different final mesh PSOs, an intermediate post read, and a separate final post PSO selected **after** its descriptor table is bound. Its real scene SRV is descriptor 72, outside the original 64-entry prefix. It checks the actual downstream render target on frames 8 through 24. A second mode makes the final draw indirect. CPU tests cover middle/terminal ambiguity, later submitted writes, incomplete traces, duplicate observations, root/target/size changes, direct/indirect matching and bounded storage.

The real-NGX mode calls NVIDIA's runtime and then uses the existing depth-debug image as a known post-NGX pixel witness. That proves after-transition recording, restoration and downstream delivery in this fixture; it does **not** measure neural reconstruction quality. Both builds use the original package's identical `nvngx_dlss.dll` hash D00B5B0AB29C3CA101B70C3A5ECB3A01B7EF33766A3383A28A18623083E6123E.

- Unmodified 6f6b806 runtime, compiled separately with the same delivery fixture: **0/17** downstream frames, 0 evaluations. The only source difference in that control worktree is the test file; the added entry-stat assertion is omitted because the old runtime has no such statistic.
- Repaired runtime: **17/17** downstream frames in direct, indirect and real-NGX modes; no later scene writes, missing state/camera or device removal in those tests.
- Local evidence: `local/se5-entry-original-control.log`, `local/se5-post-entry-targeted.log`; build/test logs retain separate source paths for control and candidate.

## Limits and next test

This is a predictor learned from observed operations. New pipelines require learning; a pipeline used both mid-scene and at the end remains ambiguous. Unobserved writes cannot be ruled out by zero observed contradictions. The fixture establishes the concrete regression and after-transition validity, not elimination of all live Sniper flicker. Cort handles game installation and visual testing. Compare actual captured final images and entry identities if the live result differs.

Claude's separately authored flow policy is combined **after** this independent placement build passes, at Cort's request. Its acceptance slider changes residual-motion policy; it is not part of the placement proof and does not guarantee a visible improvement on aircraft. No claims of game verification follow from this document.
