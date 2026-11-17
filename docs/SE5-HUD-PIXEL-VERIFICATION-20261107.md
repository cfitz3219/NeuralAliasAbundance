# Sniper HUD: verification from saved pixels

Astra, 2026-11-07. Read-only analysis; no game launch, deployment, runtime edits or new capture request.

## Finding

The inspected ammunition numbers, rifle/grenade icons, minimap, aiming marker and menu labels are absent from the captured DLSS input and output and present in the final screen image. Their native-resolution rendering is supported by actual pixel comparisons, not only full-size viewport metadata. The game's HUD follows DLSS and RCAS in this route. This verifies those elements in the saved runs; it is not an inventory of all HUD elements, scope views, subtitles, menus and world markers in every game state.

The NVIDIA development-runtime diagnostic text is an exception: it appears inside `dlssout` before the game HUD. RCAS can sharpen that diagnostic readout. It must not be used as evidence that game text goes through RCAS.

## Exact saved evidence

Capture root: `C:/Users/Cort/AppData/Local/NeuralAlias/captures/`.

- Quality gameplay: `j2-20261107-054019-sniper5_dx12-p27252`, 11 paired evaluations, 2293x960 input to 3440x1440 output. Representative `00005-meta.json`, frame7472, present_closed_frame7472, sharpening50, evaluation_succeeded true. Results manifest `Documents/NA-se5-results-20261107-054247/manifest.json` identifies runtime11e3365.
- Native-resolution DLAA reference: `j2-20261107-053412-sniper5_dx12-p44372/00015-meta.json`, frame13976, present_closed_frame13976, 3440x1440, sharpening50. This is a native-DLAA reference, not an unmodified-game reference.
- Menu Quality: `j2-20261107-060416-sniper5_dx12-p22652`, 30 frames, sharpening50; representative frame452.
- Menu Performance: `j2-20261107-060454-sniper5_dx12-p22652`, 30 frames, sharpening50; representative frame4801.
- Menu Performance: `j2-20261107-060529-sniper5_dx12-p22652`, 30 frames, sharpening76; representative frame8779.
- Settings menus at Performance and Ultra Performance: `j2-20261107-061019-sniper5_dx12-p22652` and `j2-20261107-061033-sniper5_dx12-p22652`; representative frames39932 and41085, sharpening54, input1720x720 and1146x480 respectively. Text is absent before game composition and visible in final output. These are visually checked, not included in the quantitative three-menu comparison.
- p22652 results manifest `Documents/NA-se5-results-20261107-061218/manifest.json` identifies b66b1ea. No e96502a game run is claimed here.

21 capture sessions were surveyed through representative input/output/present contact sheets. Empty capture directories were excluded. The 11 gameplay and 90 menu frames above received quantitative comparisons. Do not call every image in all21 sessions exhaustively verified.

## Pixel measurements

Read each raw image using its own recorded width, height, format and row pitch. RGBA8 input/output is normalized by255; FP16 present is converted from linear to sRGB for display. Cropped HUD and menu outputs retain the native pixel grid. Only smaller *scene input* images are nearest-neighbor expanded for placement in the comparison sheet.

Foreground geometry uses min(R,G,B)>0.9 in displayed RGB; compare binary mask intersection/union at the same screen coordinates. This is an outline comparison, not exact RGBA equality or a full antialiasing/halo metric. The translucent background and edge antialiasing legitimately affect brightness. No image alignment/resizing was applied to the final HUD masks.

| Element | Native-coordinate rectangle (exclusive end) | Result |
| --- | --- | --- |
| Ammunition numeral5 | 2850,1250 to2915,1325 | 98.02–98.45% mask agreement across11 Quality frames versus native-DLAA frame13976 |
| Rifle icon | 2965,1250 to3205,1300 | 100% mask agreement across all11 Quality frames |
| Grenade icon | 3210,1245 to3300,1315 | 99.829% mask agreement across all11 Quality frames |
| SERVICE RECORD / LOADOUT AND CUSTOMIZATION / SHOOTING RANGE | 600,800 to990,1010 | 99.735–100% agreement across90 frames, including Quality/Performance and sharpening50/76, relative to Quality frame452 |

The accompanying crops show the actual detail: game HUD is missing from the scene input and DLSS output; it is visible, with fine native edges, in the final image. Minimap content rotates with the camera, so it was checked for stage membership and visual detail, not incorrectly compared for identical pixels across different views. The aiming marker is also added after DLSS.

## RCAS ordering checked against code and captures

In both captured revisions11e3365 andb66b1ea, `record()` captures `g.output` before RCAS, then sharpens that private scene output, then writes it back to the scene/upscale destination. NativeScaleHook inserts this work after the native scaler, before the following game draws. The deferred route requires the scaler recording to have no subsequent work and submits before the next game list. The old render-small sharpening path is separate; the p22652 run reports it absent. There is no later J2 RCAS call on the final presented image in the native-scale route.

Current e96502a retains this ordering. Thus the post-DLSS snapshot alone is not proof about RCAS; the source ordering and observed menu50/76 comparison supply the additional evidence. Translucent HUD backgrounds can still show changes in the sharpened world behind them. That does not mean glyphs were sharpened.

The saved p22652 frame35252 draw dump independently shows scaler operation828 reading1720x720 #13988 and writing3440x1440 #13990, followed by full-size blends and final operation992 reading that canvas through sRGB into the FP16 backbuffer. It is a separate sampled frame, not falsely identified as the exact captured pixel frame. Earlier p27252 frame6422 contains the equivalent native canvas/HUD/final path.

## Artifacts and limits

Analysis scripts, raw-metadata inventory, quantitative rows and PNG comparison crops are local under `C:/Users/Cort/NeuralAliasAbundance/local/se5-hud-pixels-20261107/`: `inspect.py`, `detail.py`, `measure.py`, `inventory.json`, `measurements.json`, `ammo-comparison.png`, `minimap-comparison.png`, `menu-text-comparison.png`. Game images remain local and are not committed.

This is positive evidence for preserving the inspected game HUD through the native-upscale route. It does not establish uninterrupted Quality delivery, qualify the legacy render-small HUD extraction, prove texture atlases contain unlimited detail, or certify every possible HUD element. No controlled same-frame RCAS-off/on pair exists in these selected captures, so do not claim byte-identical output under every sharpening setting. No new user test is required to support the findings above.
