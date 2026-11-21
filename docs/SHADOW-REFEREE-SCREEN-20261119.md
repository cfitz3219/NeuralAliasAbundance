# What reached the screen in the November 18 Shadow of War run

Cort saw a slow sway on K that subsided on M and severe flicker on reflections with either preset. The runtime was 2e062ef. The results folder is `Documents/NA-ShadowOfWar-results-20261118-233811-184`. The six original capture folders are under `%LOCALAPPDATA%/NeuralAlias/captures`, all with suffix `ShadowOfWar-p33800`. The times below identify the folders.

## Confirming DLAA delivery

All 180 saved evaluations have a matching display ordinal. A 3440 × 1433 scene appears inside a 3440 × 1440 display. A separate search for the displacement that best aligned the fortress edges picked four pixels down and zero sideways in each capture. The Referee had stretched the scene to the edges of the display.

The shared runtime now compares scaling with native-pixel centering when the size difference is small enough to be padding. Both possible origins of an odd margin are tried. The decision uses features common to the input and output, so favoring a particular DLAA result cannot select the mapping. At least three regions must favor the same correction after conflicting regions are counted against it. The production replay uses the same sampling and vote. The table counts frames on which the compared patches favor the DLAA output.

| Capture | Preset | Scaled comparison | With registration |
|---|---|---:|---:|
| 233533 | K | 0 | 29 |
| 233543 | K | 2 | 30 |
| 233659 | K | 0 | 29 |
| 233713 | M | 0 | 29 |
| 233718 | M | 2 | 28 |
| 233727 | M | 1 | 28 |

Each comparison uses the same probe positions for both columns. Saved frames are selected successful evaluations. The seven remaining frames give insufficient or contrary evidence for an output verdict. These totals cannot measure the whole session's frame coverage or stability. The 75 logged Referee preview choices all held the current settings. Native search issued no confirmed takeover during the run.

A separate eligibility bug counted 2,244 scene frames inside a runtime DLAA pause as a delivery outage. The change records the evaluation pause on the closed native frame. An already submitted frame still counts if paused afterward. After resume, a genuine missed submission again counts.

## Where reflection flicker becomes visible

The two closest pause-menu comparisons are 233659 (K) and 233713 (M), with the same menu composition, camera discovery off and sharpness at 50. We measured a 320 × 195 region at scene coordinates (1350, 535), offset four rows on the display. Only consecutive presented frames were used to measure changes. The real runtime captures its normal Referee model-output sample before the cut guard and sharpening. The saved DLAA plane in these six captures is after the cut guard and before sharpening. We reconstructed the production sharpening from saved color and depth separately.

| Preset | Median frame change at saved DLAA output | After reconstructed sharpening | With depth frozen in sharpening | Actual screen |
|---|---:|---:|---:|---:|
| K | 0.015 | 0.022 | 0.021 | 0.068 |
| M | 0.044 | 0.058 | 0.057 | 0.122 |

Values are differences in log2(0.02 + luminance). Display grading can change this scale. To check for a separate signal, a per-pixel model learned the local relationship between reconstructed RGB and displayed light in one chronological half and predicted the other half; the halves were then reversed. Adding the matching pre-DLAA scene color halved the squared prediction error on 449 of 1,481 fluctuating points on K and 460 of 2,101 on M. Adding local depth instead did this on 13 and 5. Supplying the wrong frame's color did it on 23 and 3. The other four captures also favored matching scene color over local depth. Files for reproducing this comparison are in `Documents/NA-strobe-7933efc/sow-2e062ef-final-screen`.

The leading explanation is that late lighting or composition contains a signal related to the original scene color. Sharpening contributes, but recreating it or freezing its depth does not reproduce the displayed changes. The same correlation could arise if two passes use a common specular source. The saved planes cannot identify a texture binding or name the game draw responsible. The distant-light control corrects selected depth reads; it has no mechanism to correct a later color contribution.

## Ownership and acceptance

- Astra owns final-screen registration, native receipt accounting, and the reflection after the image is returned to the game. Reuse the existing reader tracking and screen challenge. Compare the same reflected feature at handback, at the suspected reader's output and on the screen. Require the candidate to preserve the shown DLAA scene and reduce excess variation on the reflected detail while preserving genuine changes visible in the scene. Keep ordinary delivery running during a challenge. A local artifact alone does not authorize switching the whole route.
- Fable accepted the K/M analysis in COMMS. His deliverable is a model-input correction if substantiated by registered scene motion, local vectors, jitter, exposure and actual rendered output. Keep each user-selected preset. Fable's independent measurements live in `Documents/NA-strobe-7933efc/sow-2e062ef-sway`.
- Automated verification covered the CPU Referee, RTX screen recognition, real native DLAA, adaptive screen placement, WARP controls, independent images, and genuinely scaled output. No live game was launched. The visible reflection and slow sway remain open until their respective fixes reach a displayed image.
