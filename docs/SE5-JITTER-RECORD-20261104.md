# SE5 jitter: problem record and fix history (2026-11-04)

Written by Opus (Claude) for Astra/Sol at Cort's request. This is a record of facts, measurements and what Cort saw. It deliberately does not recommend a solution. Where something is my interpretation rather than a measurement, it is marked **(interpretation)**. Where a claim of mine later proved wrong, it is marked **(withdrawn)**.

Branch: `claude/se5-motion-pkg` (Cort's test packages). Game: Sniper Elite 5, DX12, render-small upscaling (Quality/Balanced/Performance) plus in-frame DLAA. DLSS preset Automatic (L for DLAA/Quality, M for Balanced/Performance).

## 1. The problem as Cort sees it

| Build | Cort's observation (his words, condensed) |
|---|---|
| 0ce11a8 (combined, 09:30Z) | Quality "vastly improved" with jitter on. But an odd shake on buildings/foliage and on the HUD; in the main menu part of the text shakes, split by an invisible vertical divider. Jitter off removes the shake, image worse. |
| 5d5b0f5 (Sol's calibration) | Jitter on starts sharp, then blurs over time. Off looks better. On again: sharp at first. |
| be67bb1 | "Your jitter is exactly the same." Jitter no longer makes the image better, gets blurrier over time. HUD clearly upscaled. |
| be67bb1, later | DLAA shows vibration from jitter; Quality/Balanced/Performance do not. After mode changes DLAA went very dark ("exposure all the way down"), stayed dark; Quality restored colour. |
| e9ca885 / 3d90845 / 7ed5d30 | 7ed5d30: no flicker/shake. DLAA: no visible difference jitter on vs off. DLSS modes: some textures sharper with jitter, but hard edges jagged with jitter, perfectly antialiased without. "This wasn't a problem before." DLAA stayed bright. |
| 32e25dd (viewport jitter) | "Violent back and forth shaking", DLAA through Performance. Many captures taken. |

## 2. Core measured fact (all builds up to 7ed5d30)

**Our injected jitter never reached SE5's rasterised picture.**

- Method: per capture set, regress the frame-to-frame shift of a still patch against the declared jitter delta, lag -2..+3 frames.
- 0ce11a8, 5d5b0f5, be67bb1 still sets: slope of `scene_color` (early, pre-post capture plane added by Sol) and of the DLSS input colour within ±0.003 at every lag. Consecutive |dColor| 0.59/255 in fully jittered still set 012612.
- 5d5b0f5 run p43844: scene_color slope 0.000 (r 0.09), DLSS input 0.000 (r 0.11), also in set 030849 before the calibration lock, with raster deltas up to 0.5 px.
- Sol's independent check: captured hardware depth in still examples shifts ~0-0.005 px while declared jitter changes ~0.6-0.8 px; moving set 012627#6 depth follows ~3 px motion like RGB.
- Where we wrote: J1 (`begin_jitter`, live_dlaa.cpp) replaces the camera ViewProjection (and inverse) in bound constant buffers with jittered copies. In SE5 the camera blocks found are projection-only (eye 0, no view) in all 16 sets of the 0ce11a8 run.
- Coverage: J1's raster jitter was nonzero only on frames where the camera was freshly found (camera age 0). camera_held is 60-67 % of frames in all builds. 5d5b0f5: 40/324 captured frames jittered (12 %). 7ed5d30 run p51716: 9,014 of 25,588 DLSS frames (35 %). Halton phases skip (o8512 -> o8515). jitter_writes/frame 1.29 (0..8.8 per 5 s block); two write modes seen (1-3 vs 7.6-9.4 writes/frame), neither moves the picture.

**(interpretation)** So in every build up to 7ed5d30, "jitter on" changed only what DLSS was told, never what the game drew.

## 3. Chronology of changes and results

### 3.1 0ce11a8: declared raster jitter on unjittered colour
- What it did: DLSS was told J1's Halton sample; the colour did not carry it.
- Measured: DLSS output (and HUD, composited after) shifted by the declared offset each frame. Witness at the time said unshifted (8.67M vs 1418) yet the jitter was declared anyway.
- Cort: much better quality, plus shake (incl. split main-menu shake).
- **(interpretation)** The shake = DLSS moving its output by the declared offset the colour never had.

### 3.2 5d5b0f5: Sol's calibration (06f2dd2 / c430c32 / 5d5b0f5)
- What it did: measure whether the colour carries raster jitter, per stage (flow, final); lock a convention; declare only what the colour carries.
- Measured p43844: Unknown -> Unjittered after 165 frames / 2 windows, changes=1, never flips (last window tested 28.4M, unshifted 25.6M, shifted 204k). After the lock DLSS is declared 0 on every frame.
- Cort: sharp at first, blurs over time.
- **(interpretation)** "Sharp then blur" = the jittered start before the lock, then DLSS reset transient (~1.3 s) and declared 0. 124 clustered restart_resume resets in that run were not traced.

### 3.3 86506ed: calibration verdict memory (Opus)
- Per-stage remembered convention, saved per game ([j2] jitter_conv_flow/final). Cort's ini now holds 2/2. Overturned by two contrary windows.
- No visible effect expected or reported.

### 3.4 be67bb1 (HUD + fullscreen fixes picked from Sol)
- Jitter behaviour unchanged from 5d5b0f5. Sol's HUD paths never engaged in p57140 (hud_frames=0, candidates=0; reported COMMS 913e389).
- DLAA dark (p27200 set 040904): DLSS raw output pins highlights above 1.0 near 1.0 (mean 1.22 -> 0.75). Nov 3 DLAA was 1:1. Cause unproven.

### 3.5 278303f (reverted by 1b073f2): DLAA declares the measured convention
- Reverted when Cort blamed the declared-0 behaviour for the blur.

### 3.6 39a28a3 / 14c87af: fresh DLAA feature after render-small, dlss_create log
- On returning from render-small to DLAA, the DLAA DLSS feature is released and recreated (`dlaa_fresh_features`).
- 7ed5d30 run: DLAA stayed bright (Cort; dlss_create seq=4 fresh on return; one run only). The dark cause itself is still not proven.

### 3.7 e9ca885: render-small and DLAA declare the raster jitter again
- Return to 0ce11a8 declaration behaviour (calibration measures/logs only).

### 3.8 deca816 / 3d90845 / 7ed5d30: jitter shift
- What it did: before DLSS, the colour is resampled by the declared jitter (Catmull-Rom, 9 bilinear fetches, clamped to 3x3 min/max), so the picture stops moving by the declared offset. Sign first wrong (centre + J, deca816), fixed to centre - J per the EvalDesc contract (3d90845). 7ed5d30 fixed a shutdown crash.
- Runs only where declared jitter is nonzero: ~35 % of frames in p51716.
- Cort: no flicker/shake; DLAA no difference on/off; DLSS modes jagged hard edges with jitter.

### 3.9 Offline real-NGX harness (PC)
- **Frozen-frame arms (withdrawn):** repeating one still frame made every arm blocky, including declared 0. Conclusions drawn from those arms ("declared jitter cannot give clean edges", Laplacian numbers 26.4 / 22.0 / 23.4 etc.) are withdrawn. The sign check direction (c- vs c+) still stands.
- **Real captured sequence (012612 loop, 120 frames, preset L):**

| Arm | Laplacian last 30 | PSNR vs captured 0ce11a8 dlssout | |dOut| | Visual |
|---|---|---|---|---|
| a2 declared raster, colour unmoved (0ce11a8) | 20.21 | 29.08 dB | 4.617 | crisp foliage, clean edges, closest to the game |
| b2 declared 0 | 19.95 | 27.19 dB | 2.957 | softer |
| c2 declared + colour shifted (7ed5d30) | 18.82 | 27.96 dB | 3.863 | softest, not jagged |

  Second set 031303: differences much smaller (a2 9.37 / b2 8.52 / c2 9.11; PSNR 28.17 / 28.23 / 28.39).
  Caveats: PSNR is against captures from a run that declared jitter, which favours a2. 012612 has ordinal gaps and the loop wraps; output shift 0.34 px even with declared 0, so the sequence is not truly still. Laplacian counts stair-steps as sharpness and is not a quality measure. The harness cannot test the viewport path (it needs the game's draws). Cort's jagged edges were not reproduced with real frames.

### 3.10 3e66d93 / 32e25dd: viewport jitter (Opus)
- What it does: in the D3D12 command-list hooks (producers.cpp `vpj_before_draw`, before DrawInstanced / DrawIndexedInstanced / ExecuteIndirect / ExecuteBundle), when the bound DSV is the live scene depth and the bound PSO has depth test enabled, the game's recorded viewports are re-set with TopLeftX/Y += (x, y) render pixels; otherwise the game's viewports are restored. One Halton sample per frame, advanced at OnPresent while DLAA/upscaling ran in the last 2 frames (`vpj_advance`). Declared value = the sample recorded by the scene list's own draws (`ListSceneViewportJitter`), else that frame's table entry. J1 matrix writes and jitter_shift are bypassed; the camera tracker gets the game's camera with injected 0.
- Convention assumed: +x right, +y down; content moves by +(x, y); declared jitter = (x, y), from camera_tracker.hpp's `jitter_px` convention and the J1 projection jitter. `[j2] viewport_jitter=-1` negates the declaration only (the viewport offset stays).
- 3e66d93: 23 J1 tests failed because the mode bypassed J1; 32e25dd made the in-code default 0 and the ini Load default 1. 189 passed / 4 skipped / 0 failed. No automated test covers the viewport path.
- Cort: violent back-and-forth shaking in DLAA through Performance.
- Measurements from that run: see section 4.

## 4. 32e25dd run measurements

Run p44832 (32e25dd), log `C:\Users\Cort\NeuralAliasAbundance\local\j2-se5-dlaa\status-44832.log`, results `Documents\NA-se5-results-20261104-054356`, evidence copies `Documents\NA-se5-jitter-evidence-20261104`. Cort: violent back-and-forth shaking, DLAA through Performance.

**The viewport path engaged.** Last `viewport_jitter` line: `mode=1 frames=4021 declared_list=3891 declared_table=123 declared_none=880 other_frame=0 scene_draws=2046678 sets=107814 restores=4017 unknown_viewports=0`.

**For the first time the scene moves with the jitter.** Tile-median shift vs declared jitter delta, lag 0 (PC's measure: s is the offset where p(x+s)=c(x), so a negative slope means the content moved by +declared):

| Set | Mode | scene_color | DLSS input colour | depth | DLSS output | presented | Notes |
|---|---|---|---|---|---|---|---|
| 054326 | Performance 1720x706 | -0.993 (r -1.00) | -0.805 (r -1.00) | -0.997 (r -0.99) | -1.446 (r -0.92) | | mean shift 0.943 px |
| 054307 | DLAA | | -0.995 (r -1.00) | -0.942 | -0.788 (r -0.96) | -0.547 (r -0.95) | |
| 054225 | Quality | -0.995 | -0.869 | -0.850 | -1.010 | | |
| 054219 | | -0.974 | | | -0.714 | | |
| 054330 | jitter off | 0.000 | 0.000 | 0.000 | 0.000 | | |

Lags ±1 show +0.6 to +0.8 only because Halton deltas anticorrelate; lag 0 is the peak by |r|.

So the input to DLSS now carries the jitter (slope ≈ -1 on scene colour and depth), and **the DLSS output also moves with the jitter** (slope -0.7 to -1.4), which is the shake Cort saw. In earlier builds the input never moved and the output moved by the declared offset.

Other lines in that run: `jitter_witness last_window{tested=16189995 unshifted=4224278 shifted=2745849} unseen_now=0`; final_colour convention=2 epoch=2. declared_none=880 of 4021 frames got no sample in the declaration.

(The PC's full table, |dOut| comparisons against 0ce11a8 / 7ed5d30, and the evidence file list will be appended here.)

## 5. Other open items touching jitter

- Flow stage: Cort's ini remembers flow convention 2; flow encode `Frame.jitter` takes `cam.jitter_px`.
- Clustered restart_resume / camera_cut resets (untraced).
- Toggling jitter triggers a DLSS history reset (`g.resume_reset` on jitter_on change).
- Dark DLAA root cause unproven (fresh-feature mitigation only).
- Sol's HUD paths did not engage in p57140.

## 6. Where things are
- Code: `src/host_dx12/j2/live_dlaa.cpp` (begin_jitter ~1075, vpj_advance/vpj_declared after jitter_phases, record() jitter handling ~2185-2250, jitter shift `kJitterShiftHlsl`), `src/host_dx12/observe/producers.cpp` (vpj_* before `struct ListHook`), `src/providers/camera/camera_tracker.hpp` (jitter convention).
- Settings: `[j2] jitter_shift` (1/-1/0), `[j2] viewport_jitter` (1/-1/0), `[j2] jitter_conv_flow/final`.
- Earlier notes: /mnt/project-files/notes/se5-motion-5d5b0f5-test-notes.md, se5-motion-be67bb1-test-notes.md. COMMS entries 7aab0bb, 913e389, e8f47b7, e68b775, 167939f.
- PC evidence: offline harness `ngxarms` scratchpad (real-sequence-012612.png, 012612loop.csv, 031303loop.csv, then-now-012612.png); evidence folder for the 32e25dd run listed in section 4.
