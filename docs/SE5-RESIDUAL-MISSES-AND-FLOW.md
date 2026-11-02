# SE5 residual frames and optical-flow readings, 2026-11-02

Cort owns all game testing. This increment starts from the user-tested runtime
019d6f4, not the rejected experimental placement stack. That package and its
ED88CB675C0829EF867D4D402B2A278BD584DC68A280639B19369CE3448433CD DLL remain intact.
Fable/Opus owns Anno; this work does not change its projection-window code.

## Remaining-frame repairs

The archived live run has 25,208 scene frames, 25,147 DLAA submissions, 61 misses
and 27 early placements. Its final three minutes have three misses in 5,272
scene frames, with no early placements or history resets. Those three misses
increase both the depth-failure and boundary-recovery counters. The latter is
not evidence that the boundary detector caused them: Close recovers any deferred
role when the frame did not run, including frames whose input qualification failed.
The first sampled startup has 22 scene misses; 17 are depth-attributed after
DLAA had first run. Do not equate that startup count with all 23 depth misses.

Two defects were reproduced and corrected:

1. **Our private completion signals invalidated game state.** DetourExecute opened
   the game observation scope, forwarded through J2, then called DLSS and GPU
   timer completion tracking without identifying their queue operations as ours.
   The observer saw these Signal calls as nested game work, declared the parent
   occurrence ambiguous, and cleared carried depth state. The live run's 25,286
   nested operations and 82,596 ambiguous occurrences support tracing this path;
   the deterministic fixture establishes its actual failure mechanism.
   BeforeGameSubmission/AfterGameSubmission now narrowly identify private
   completion synchronization with the existing HostWorkScope. The actual game
   forward and subscribers remain outside those scopes. D_Wait now honors that
   marker as D_Signal already did. Genuine nested game Wait still receives the
   existing ambiguity treatment; the regression checks both cases.
2. **An active mixed-role boundary could forget its less frequent role.** If one
   list/writer/occurrence was terminal occasionally and intermediate for more
   than 120 frames, Judge aged out its terminal observation and began deferring
   it again. The reverse could requalify an intermediate point as a final end.
   Mixed-role facts now remain ambiguous while the cache entry is active. Unused
   entries still retire after 120 frames; a selected-resource change resets the
   cache. Pure intermediate and terminal qualification thresholds are unchanged.

The CPU regression fails in both aging directions before the correction. The
real DX12 pan fixture reproduces spurious nested/ambiguous observations before
the completion fix. Afterward it retains the exact depth state, reports zero
depth failures and no synthetic nested queue operations, while preserving
26/27 processed frames (one initial learning frame), 88 intermediate deferrals,
zero miss recoveries and no new early placement. NGX is a fake model in this
fixture; rasterization, copies, resources, queue operations and fences are real.

Miss diagnostics now preserve an actual recording/input failure instead of
overwriting it with the generic boundary-mismatch explanation at Present.
No depth threshold, camera hold limit, NGX placement or GPU lifetime is relaxed.
Automated tests cannot establish that every future SE5 frame is now covered.

## What optical flow does here

Camera/depth reprojection already supplies motion for the world as the camera
moves, including when the optical-flow toggle is Off. Optical flow estimates
image displacement; we subtract that camera contribution and qualify the
remaining object-motion correction. A low correction percentage therefore does
not mean the camera movement is missing or that only that percentage was processed.

Existing acceptance rules remain unchanged: valid finite inputs, at least 0.5 px
of residual, in-bounds prior position, sufficient image contrast, a good image
match relative to camera-only reprojection, and agreement with the previous
qualified pair. The first moving pair waits for temporal agreement. Unaccepted
pixels retain camera motion; they do not disable DLAA for the frame.

The 166 periodic completed pixel samples in status-46360 show accepted correction
coverage from 0.0027% to 45.2525%, median 0.019%. They are periodic latest-frame
samples, not every-frame percentiles or an accuracy score. In the last sample:

| Classification | Pixels | Share of picture |
| --- | ---: | ---: |
| Accepted correction | 950 | 0.019% |
| Residual below 0.5 px | 4,502,791 | 90.899% |
| Invalid reprojection/flow | 317,530 | 6.410% |
| Image/contrast check | 84,878 | 1.713% |
| Waiting for temporal agreement | 47,451 | 0.958% |

The engine submitted 25,074 frames, with no busy or unavailable failures. Its
periodically sampled GPU cost had median 0.867 ms at 3440x1440. This is flow cost,
not all DLAA cost, a controlled On/Off performance comparison, or a quality score.

The real AMD shader regression deliberately distinguishes these two cases:

| Input | Measured result at 512x256 |
| --- | --- |
| Object moves 8 px; camera stationary | 9,216/9,216 tested object pixels have the correct -8 px final vector; zero background errors |
| Textured world moves 8 px because camera moves | 127,984/131,072 pixels (97.64%) show estimated image motion; zero object corrections are needed |

The overlay now labels **image motion**, **object corrections**, and **below
0.5 px** separately, with the existing submitted/paired/skipped counts and sample
frame age. The log adds image_moving. This uses a spare counter in the existing
GPU pass and existing asynchronous readback; no extra pass or synchronous live
readback is added. Rejection thresholds and rendered motion remain unchanged.

Turning flow Off may look similar when camera motion explains most of the image,
or independent movement covers little of it. It is still useful for independently
moving people, vehicles and animation when their image evidence can be tracked.
Lack of obvious smearing is not a measurement of every object's vector accuracy.
Thin or low-contrast objects are harder: AMD provides one estimate per 8x8 block,
then our existing depth-aware expansion and checks qualify individual pixels.
We do not increase acceptance merely to display a larger percentage.

## Primary references

- [AMD FidelityFX Optical Flow](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/optical-flow/): current/previous image matching, 8x8 block vectors, integer-pixel search, scene-change output.
- [NVIDIA DLSS programming guide](https://raw.githubusercontent.com/NVIDIA/DLSS/main/doc/DLSS_Programming_Guide_Release.pdf): screen-space current-to-previous vectors must account for camera and dynamic-object motion.

These sources support the input requirements and algorithm limits. The split
into reconstructed camera motion and accepted residual motion is NeuralAlias's
existing integration, verified here with its numerical fixtures. No claim of a
new successful game test or universal optical-flow accuracy is made.
