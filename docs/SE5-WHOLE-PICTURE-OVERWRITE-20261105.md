# SE5: whole game picture overwrote enhanced output

2026-11-05 Pacific. Astra. Cort reports worse stability, clarified as rapid
DLAA on/off. No game launch/control/deployment or driver/profile change by Astra.

## Actual installed run

Manifest: ef71b722865e111f13d006360e6f1fd1f4f8d88c, including PR111.
DLL SHA256:266212AD09E6A7CF0B56D3C9FD3285C712426DCC4457D795AD1AE19207C78213.
PID41768, captures j2-20261105-230156 and j2-20261105-230159, both suffixed
sniper5_dx12-p41768. Decoded pictures show the main-menu 3D background;
these are not mission/gameplay evidence. Originals remain unchanged.

All53 comparable captured frames have IDENTICAL RGB in hud_n0 and hud_n1:
mean difference0. In45/53 frames the final dlssout also equals hud_n0 at
EVERY RGB pixel:100% exact, MAE0. The two backgrounds have produced the
same whole game picture, which is then restored over DLSS. The other frames
use a different whole-picture presentation. Second-session indices20/28
match bilinear sampling of the low-resolution hud_full within1 UNORM code
at100% of pixels (MAE0.0001237/0.0001245), corroborating the protected branch.
Derived comparisons, C# source, previews and raw receipts are ignored under
main/local/se5-stability-20261105. No capture is committed.

At23:03:55:13163 scene frames,411 missed scene hooks; Present recovered most,
with13153 DLSS and66 stretches/13219 attempts. Thus inline misses are NOT
equivalent to pictures shown without DLSS. Depth-state/busy/NGX failures were
zero in the examined summary; contextual-entry recovery was zero. Acquisition
misses are a separate remaining limitation, not the proved output overwrite.

## Cause and repair

Existing HUD composition fits a+scene*b, with b=(native1-native0)/grey.
With identical layers, b=0 and the fit can match the game picture for ANY
scene. Identity was mistaken for separation. The clean branch replaces the
enhanced result with a; the protected branch replaces it with bilinear raw
colour. NGX can evaluate successfully throughout these visible changes.
This is Astra's existing compositor defect, not an NVIDIA error.

Reuse the EXISTING verify/prepare/compose GPU passes and reserved proof bytes.
For reconstruction packets, verify counts nonzero scene dependence between
the native counterfactuals at the input grid. Following passes consume that
same-frame result after the existing UAV ordering barrier. A zero-slope
whole-picture model cannot authorize scene substitution OR reactive masking
OR native restoration: ordinary DLSS receives the original complete colour,
keeps actual motion, has no all-picture reactive mask, and retains its enhanced
output. Candidate HUD count remains visible but separated pixels=0, so the
existing four-method policy advances/stops normally.

This decision occurs BEFORE current-frame composition. Asynchronous policy
failure alone would still permit bad trial packets. Opus's90%-area exclusion
is replaced by direct scene-dependence evidence; coverage remains diagnostic.
A valid translucent full-screen veil is not rejected merely for its area.
Partial opaque glyphs remain valid when the scene transmits elsewhere.
Probe-only/independent-native-replay descriptor dependencies remain unchanged.
No new pass, allocation, readback, GPU/CPU wait, HUD algorithm, library, adapter
or user toggle. Existing jitter/flow/mip/AF/GPU services remain intact.

Also retain strict boundary matching instead of a9f7cd2's writer-only rescue.
Its first-failure counter did not establish later predicates, and the added
branch still fell through to another writer rejection. A different first
intermediate writer is not terminal-scene proof. Existing qualified-entry,
context and split-list paths remain; no replacement heuristic is added.

## Reproduction and final source qualification

New actual D3D12 regression: complete-picture counterfactuals, distinct enhanced
output, clean/protected routing, with/without low-size model validation.
Unchanged a9f7cd2 replaces ALL512 enhanced pixels in each of four cases:
0/512 retained,12 assertions fail. Repaired:512/512 retained in all four,
all128 input pixels use complete colour, mask0, actual4px motion retained,
separated pixels0. An intermediate receipt-only assertion caught incomplete
candidate coverage reporting; corrected before final qualification, failed
logs preserved. No suppressed device errors or rendering disable.

Complete Release build PASS. Final25 affected CPU/RTX/default cases PASS,
0 skipped/failed,50.79s. Covers native HUD/opaque/partial/HDR composition,
recovery/init, live mip pixels/warmup, split-entry direct/indirect, jitter,
UI and actual NGX entry integration. Forced-WARP6/6 PASS17.01s. No fresh219-case
full-suite claim. Evidence final-tests.log, warp-tests.log in the directory above.

Pixel tests exercise actual GPU composition with a deliberately distinct
enhanced picture, not NVIDIA reconstruction quality. Capture comparison proves
the installed-build overwrite. Corrected live stability remains Cort's test.
This is not a claim that Sniper HUD separation now works. Regression host-stub
DLLs are not install payloads. Sol/Claude retains the established combined
production-adapter package/CMD lane and reversible deployment.
