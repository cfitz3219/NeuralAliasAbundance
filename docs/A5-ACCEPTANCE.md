# A5 fixture acceptance — combined production classifier

The declared fixture-v2 corpus meets the >=95% criterion: **264/264 correct
candidate decisions (100%), 63 true positives, 201 true negatives, zero false
positives and zero false negatives**. `Supported` is a diagnostic corroborated
candidate; it does not authorize effect submission or claim universal semantics.

This measures `Describe` metadata gating followed by `ConfirmDepth` or
`ConfirmPreHudComposite` in `src/providers/classifier/production_checks.hpp`, not only
the older oracle-assisted content checks. The scorer provides no class label to
the C++ classifier. Candidate IDs and ordering are randomized deterministically.

Corpus: fixture-v2 frames1–11 at64x48,96x64 and160x96, eight candidates per case:
main depth, preserved scene color, shadow depth, clear depth, wrong depth
convention, HUD final color, stale history and unrelated postprocess color.
Here the fixture's "preserved scene color" is exactly the display-space source
of final HUD composition: the fixture has no intervening tone-map/upscale chain.
The checker is therefore named `ConfirmPreHudComposite`. Its positive result
does not qualify render-resolution, pre-post-processing color for DLSS. That
separate semantic input still requires render-pass and post-process lineage;
the 264-case score does not measure that acquisition or qualification.
Camera cuts require depth abstention; scene-color evidence remains current.
Observations contain producing version/write order, the composition source
identity and overlay scissor bounds from the fixture's render graph. Content
inputs are completed depth/color versions plus cameras. No per-pixel world
positions, HUD foreground/alpha or oracle visibility mask are supplied.

Reproduce with `na.fixture.production_classifier`, or run
`tests/fixture/test_production_classifier.py --runner <na_production_classifier_corpus.exe>
--report <output.json>`. The scorer enforces >=95% overall accuracy, zero false
support and detection of every positive in this corpus. Local full decision
record: `local/phase3-integration/a5-acceptance.json` (generated, not game data).
Additional targeted tests cover corrupted source pixels, overwritten/stale
sources, unresolved presentation lineage, unknown writes, malformed/full-screen
overlay bounds and wrong motion direction using depth-derived visibility.

Limits: metadata acquisition is supplied by the fixture's known render graph;
this is not an end-to-end accuracy measurement of H2 capture in arbitrary games.
The production checker requires the same observable identities/coverage from
the host. Missing bindings, unqualified cameras, same-resource overwrites,
unknown tone mapping or insufficient overlap return Unavailable with a reason.
Pre-overlay agreement is conditional on complete observed composition/write
lineage; it is not proof against undiscovered earlier overlays. These limits
belong in the game report and review, not silently counted as supported inputs.
