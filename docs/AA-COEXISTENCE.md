# AA coexistence implementation and evidence — 2026-10-26

This extends PR48 beyond the earlier x2-only delivery. Opus retains the separate moving-image haze/shimmer task. His camera/jitter implementation through2315633 is merged without changes to its logic.

| Argon step | Implementation | Limits |
| --- | --- | --- |
| Tier1: allow AA coexistence and show guidance | No AA-setting veto. Existing Home/DLAA hint uses na::ui::Hint and the requested wording. Color and depth are prepared independently. | Existing requirements for usable scene inputs still apply; the current J2 depth families remain 32-bit depth, with or without stencil. |
| MSAA compatibility needed for the reported failure | Native color resolve, all-sample nearest-depth GPU resolve, output drawn back into every MSAA color sample of the selected slice. Uses actual resource sample count rather than x2 constants. | x2/x4/x8 measured on this RTX. x16 is unsupported for these formats on this GPU, not rejected by a hardcoded x2 limit. |
| Tier2: use pre-postprocess color when possible | Insert after the scene writer before the first later list with an explicit scene-color SRV-table binding. A later list's final scene-RTV rebind cannot move insertion after its earlier read. Without such evidence, retain the existing scene-list insertion; do not disable DLAA. | This is a bounded ordering hint, not classification of an arbitrary shader as FXAA/SMAA/TAA. Bindless/late-mutated descriptors may not identify that boundary. It cannot split scene and AA commands already recorded into the same list; existing later-pass diagnostics remain. |
| Tier3: optional forced-TAA profile bypass | Remains conditional, as proposed. No forced-TAA title/shader was supplied or identified for this task. Anno exposes AA Off. | No generic draw suppression was added. A validated title-specific replacement must preserve the AA pass's output/history dependencies; simply skipping a draw is not generally equivalent to pass-through. |

Reading pre-AA color keeps that AA filtering out of DLAA's input. It does **not** automatically bypass the game's later AA pass: that pass still runs on the replacement result. The overlay hint therefore remains useful. This delivery implements coexistence and available earlier input selection, not universal automatic TAA removal.

## Verification

Windows Release DLL and targeted RTX tests, including real NVIDIA DLAA runtime sessions at x2/x4/x8. GPU tests check all pixels of the resolve: at x4/x8, extrema are deliberately placed in samples beyond the first two so the earlier hardcoded resolver would fail. Test both ordinary/reverse depth, both 32-bit depth families, color averaging, writeback over a different destination color, untouched array slices, and mixed color/depth sample counts. Debug-layer error/corruption messages fail the conversion tests.

The downstream-consumer regression binds scene color as an SRV, reads its GPU contents, then rebinds the scene RTV/depth. It requires DLAA before that list and reads the downstream output to prove it received the replacement. Its read operation stands in for filtering; this is not an FXAA/TAA visual comparison. Real-runtime tests check successful DLAA evaluations, finite non-black output and untouched adjacent slices. They are synthetic GPU sessions, not new Anno gameplay or a haze/shimmer claim.

No deployment or restart by Astra: Opus owns the current game session. COMMS carries the combined commit and build handoff. Actual Anno appearance/performance with these combined changes is not established by the synthetic tests.

The implementation uses standard Direct3D operations: [Texture2DMS.GetDimensions](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/sm5-object-texture2dms-getdimensions) supplies the sample count, and [multisample quality queries](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_feature_data_multisample_quality_levels) distinguish unsupported hardware configurations in tests.
