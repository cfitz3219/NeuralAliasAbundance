# Astra Phase 3 implementation and evidence handoff

This maps the completed Astra deliverables to the Phase 3 acceptance criteria in
MASTER-PLAN.md. It does not close the review gate or claim qualified Anno DLSS
inputs. A9 explicitly requires a diagnostic report of findings and unknowns;
reconstruction and output replacement belong to Phase 4.

| Item | Implementation and acceptance evidence |
|---|---|
| A5 signal classifier | `src/providers/classifier/signal_candidates.hpp` and `production_checks.hpp`. The declared fixture-v2 corpus passes 264/264 decisions, 63 true positives and 201 true negatives, with no false support. `docs/A5-ACCEPTANCE.md` states the supplied observation/acquisition limits. |
| A6 camera/jitter finder | `src/providers/camera/` structural candidates, independent-reference jitter and continuity/cut checks. `na.fixture.camera_candidates` checks matrices, view position, near/far and projection offsets across 72 snapshots. `na.cpu.matrix_candidates` covers missing lineage, jumps and handedness discontinuity. Structural object-transform decoys remain ambiguous. |
| A7 motion finder | `src/providers/classifier/motion_checks.hpp` and production checks. `na.fixture.motion_content`, `na.cpu.motion_checks` and `na.cpu.production_checks` cover correct/wrong conventions, sparse/flat ambiguity and depth/camera-derived visibility. No image-agreement-only semantic trust. |
| A8 profiles | `profiles/layout_cache.hpp`, exercised by `na.cpu.layout_cache`: the second fixture launch reuses matching hints; executable/layout/classifier mismatches invalidate them. Stored evidence is not a stale runtime resource identity or effect authorization. |
| A9 Anno report | `docs/A9-ANNO-DIAGNOSTICS.md`: real DX12 gameplay, overlay, sample results, Present events, camera-scan outcomes, errors, limitations and verified rollback. Two corrected builds exited normally with code 0. |
| R1 production check implementation | `ConfirmDepth`, `ConfirmPreHudComposite` and `ConfirmMotion` accept observable scoped samples, camera evidence, composition lineage and draw bounds. The production fixture tests do not supply world-position, per-pixel HUD-alpha or oracle visibility inputs to these checks. Missing acquisition evidence remains unavailable. |
| Read-only report integration | Existing game-adapter extension; explicit opt-in and bounded worker-side processing. Per-resource depth/color reports now call the same A5 `Describe` implementation and expose its reasons instead of a generic placeholder. Real-transport regression proves inventory-only textures cannot become supported inputs. |

## Validation and deployment boundary

The last game-tested runtime is source `9e1b140`: local full Release/default
61/61 tests passed, and all three CI jobs in run 40158744541 passed. The final
single-launch session logged 16,419 Presents and 12 completed texture samples;
normal exit returned 0 and the installation was restored. The later report-only
classifier wiring was built and checked with the real transport adapter test,
production checks and the 264-case acceptance corpus. It was not redeployed to
Anno. No further game launch is needed merely to repeat the recorded checks.

The live evidence does **not** qualify an Anno camera, packed-depth input,
preserved pre-HUD source or motion signal. It also contains detail loss and
incomplete swapchain creation provenance. These are reported compatibility and
acquisition findings, not passing inputs. No Anno profile is saved as trusted and
no DLSS/reconstruction path is enabled by discovery.

## Remaining gate work

Claude must review the final consumer and host-fix delta; no approval is inferred
from an older review. Independent audit, finding resolution and the scheduled
additional-game gate session remain gate requirements. COMMS records the request
for execution-time camera/packed-depth acquisition work and ownership. Those
items must not be relabeled complete or concealed by the completion of A9.

The review package contains the exact tracked source archive, delta from the
previous review head and SHA-256 manifest. It excludes SDK binaries, game files,
raw captures and local debugger dependencies. Historical reports remain intact.
