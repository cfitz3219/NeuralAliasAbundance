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

## 2 November, 18:12 user results: image regression confirmed

Cort reports the new SE5 picture is much worse. Installed DLL and results
manifest both identify ea7534d / D648D72D596B834852A95A79F4153043832C8C0629276D03ACFADE8F8C59CB63.
Results: Documents/NA-se5-results-20261102-181231, status-33188.log.
Archive, previews and offline numerical analyses: local/se5-regression-20261102.

Depth-state failures, busy-slot failures and DLSS evaluation failures are zero.
The route counts 15,810 processed scene frames of 15,821 (99.9305%), 11 misses
and five early placements. Those counters do not establish motion correctness.

The three actual SE5 capture sets j2-20261102-181141, -181159 and -181203 contain
90 evaluated gameplay frames. All have nonzero jitter metadata, no reset and no
observed later scene write. Sampled supplied motion is effectively zero in all
90: maximum magnitude 0.000173 px; maximum 95th percentile 0.0001221 px. Flow was
Off throughout the retained run: zero AMD attempts/submissions. The image itself
is moving. Offline local phase correlation on consecutive captured Presents,
with their jitter difference removed, measures about 8.5 px displacement on the
static wall in 181203 frames0->1. That region's supplied motion has median0 and
95th percentile0.000063 px. Other building/ground patches also move multiple
pixels. This is supporting image evidence, not a recovered engine camera;
capture gaps are excluded from consecutive-frame comparisons.

The concrete defect exposed is **unrepresented background motion in DLSS's
input**, despite successful evaluation. The live camera path calls ordinary
CameraTracker::Update; the existing UpdateCameraRelative origin-composition
helper has no live caller here. Incomplete camera-relative transforms or selection
of an unmoving projection-like block could cause this. These captures do not
store selected camera bytes/matrices, so they do not distinguish those causes.
Do not claim the origin hypothesis proven or choose a float3 by guesswork.

My default-Off recommendation was premature: the synthetic AMD test proves the
shader can recover known camera/object motion, not that SE5's acquired camera
describes all world motion. Flow-On masking this defect in the successful run
is plausible, not a controlled comparison. Retain Cort's requested default
until a deliberate correction is agreed; no live setting was changed here.

Correction to preliminary commentary: full-run held-camera counts mix menus/
loading and gameplay. At18:09:08 the held counter was already5,081 of the final
5,108; only27 additional holds occur through18:12:08. The32% whole-run ratio is
not evidence of32% gameplay camera loss. Global capture175558 is Anno and was
excluded after image inspection. Retain019d6f4 as the successful comparison.

Next work is actual acquired-camera identity/current-previous motion. No boundary
relaxation, preset change or new resolution-control route follows from this
evidence. No source fix, game action, installer update or new game-success claim
was made during this results investigation.

### Subsequent matrix evidence identifies the classifier defect

The discovery log does retain GPU-sampled matrix hypotheses, separate from F8's
image metadata. Resource67/ticket4411 records one camera block containing view
at+0, projection at+64, full VP at+128, inverse projection at+256 and inverse VP
at+320. The projection's vertical focal scale is negative (clip Y flip); the full
VP contains camera translation. This resolves the earlier evidence limitation
for that sampled block without guessing a camera-origin field.

Projection() incorrectly required positive focal axes. Thus FindMatrices failed
to recognize the signed lens, classified it as a combined ViewProjection, found
its inverse192bytes later, and offered it before the actual moving VP. That
matches the run's learned64/inverse_delta192 and effectively zero motion. A replay
of the actual saved matrices through the old compiled scanner selects offset64;
the corrected scanner selects128. Raw matrices/replay inputs stay local.

Corrected signed-axis projection classification and exclusion of inverse lenses
from combined-camera candidates. The inverse-lens check first tests its sparse
shape so ordinary scan windows do not incur another full inversion. Learned-slot
validation now applies the same lens exclusion, allowing relearning instead of
retaining the false camera. No game name, fixed camera offset or guessed origin
enters the production correction.

Two new generic CPU regressions fail on the old source and pass after correction:
signed-axis lenses/inverses in both memory layouts, and lens/inverse preceding
the true translated/rotated VP. CPU scanner13/13pass. Existing DX12 pan fixture
now also includes the misleading signed lens/inverse ahead of the real camera,
with flowOff and an explicit learned-camera offset assertion. Full rebuilt suite
results follow below; this does not yet establish corrected in-game quality.

### Correction delivery

Runtime source bd5fd0403cc1f5a2f85fd7c30e7a500fbd083e1c: full Release build and
167 registered tests complete in186.00s;162pass,5skip,0fail. The extended DX12 pan
fixture selects the true VP with the signed-lens decoy present and flowOff;
26processed frames/27(one cold),88intermediate deferrals,0miss recoveries and
0depth-state failures. The captured-matrix replay and the13CPU scanner cases
also pass. NGX fixture evidence is distinct from actual game image quality.

Documents/NA-SE5-camfix-bd5fd04 is prepared, DLL SHA256
4527331127D4B1012FCA13681EBCDBD25A9945545C840305A053498B1010E212.
The existing camfix update/results/rollback CMDs now select it. Their previous
bytes are preserved in Documents/NA-se5-camfix-launchers-before-bd5fd04-20261102-182701.
The known-good019 command and historical6f command are unchanged. Windows
PowerShell install->candidate->019->original roundtrip passes in an isolated
directory, retaining raster mode, user sharpness and native-file contents.

The actual installed DLL remains ea7534d/D648D72D..., unchanged by Astra. Flow
remains defaultOff as Cort requested. This fixes a reproduced false-camera
selection; Cort's next game run determines actual picture improvement. No game
launch/control/deployment or live image-quality success is claimed.

## Latest stability investigation: saved 212919 (2026-11-02 Pacific)

Cort distinguishes foliage shimmer from actual DLAA dropouts: the NVIDIA
registry indicator disappears with the latter. This is evidence of a picture
problem, not grounds for changing only the Home status.

The saved manifest and installed payload identify ebfd468, not a stale package.
The learned camera is now offset128; state, busy and NGX failure counters stay0.
There are874 missed **observed scene frames**,218 early placements and35 route
changes in the long run. A further39676 no-candidate Presents includes intervals
with no tracked selected-scene work at all. These cannot honestly be called
39676 proven missed gameplay frames, or assumed idle without further evidence.
The previous results script's98.7% held share is a ratio of held-camera events
to camera failures, not scene coverage or picture quality.

Five capture sets contain150 successful evaluations, all with zero late writes.
Their background is mostly stationary; near-zero reconstructed camera motion
is consequently not proof of another camera-selection failure. Successful
evaluation captures do not contain the missing evaluations. The recorded-list
generation is469 behind the Present ordinal throughout those sets, which does
not support duplicate Present calls as the bulk explanation there.

Two production-path defects were reproduced and corrected:

1. **A recording could change raster phase halfway through its geometry.**
   Present/another worker can advance the global suggestion while the list is
   still open. Previously every draw replaced the recording's sample; NGX only
   received the last one. The new actual-DX12 regression changes the suggestion
   between two halves of one shared edge. Old code changes93 GPU pixels and
   reports the wrong sample; corrected code changes0. The first actual sample
   now belongs to the recording until Reset, including a temporary failure to
   obtain a later suggestion. A separate-list saved camera carries the actual
   recorded raster sample instead of the global suggestion.

2. **Different consumer shaders were collapsed into one ambiguous role class.**
   With that class unavailable, placement fell back to the previous frame's
   final scene writer. The log explicitly contains different-writer/occurrence
   rejections. The existing profile now learns an additional exact consumer
   identity with the same three completed-frame observations and temporary-pass
   rejection. Structural matching remains available for shader variants. The
   DX12 regression gives middle/post consumers identical structural inputs and
   uses a new final geometry pipeline every frame. Old code processes0/24;
   corrected code processes21/24 (three learning frames), with0 early writes,
   camera failures or depth-state failures. This is real command-list/GPU work
   using a fake NGX model, not a neural image-quality demonstration.

   A second case keeps the final writer stable, then introduces optional drawing
   after its previously final barrier. Simply adding precise matching was not
   enough: the earlier barrier still won, giving4 late writes. Recent qualified
   exact consumers now take priority over that prediction; the same24-frame
   run processes23frames(one cold) with0 late writes. The preference requires
   the actual post consumer to have appeared in the immediately previous frame;
   a missing or contradicted consumer does not permanently suppress barrier-only
   routing. A read before later texture clear/reuse also cannot suppress a
   barrier-only completion. The existing pan fixture caught that distinction;
   the corrected eligibility preserves its26/27(one cold). MSAA retains its
   existing barrier timing because its depth conversion changes graphics state.
   These priority, reuse and expiry cases are exercised in the regressions.

The exact identities use the observer's monotonic, retirement-aware PSO IDs,
not allocation addresses that can be recycled. Pipeline-library/preexisting
objects receive an unknown-description identity on first use; their state and
shader properties are not invented. A replaced lifetime does not inherit the
old consumer's approval. Late writes demote the exact trigger too. The profile
remains bounded (512 entries including the new qualifications); existing class
counters retain their structural-class meaning. `j2_profile matched{...exact=}`
reports actual uses of the precise tier.

No trace cap or camera acceptance threshold was relaxed, no state/evaluation
failure was hidden, and no duplicate NGX evaluation or previous-picture replay
was introduced. Flow stays Off by default and visible under Enhancements.
These reproductions establish defects and corrected behavior in their exact
cases. They do not establish that all live dropouts/shimmer are gone; Cort's
game test remains the picture qualification. Depthless/compute/indirect scene
structures and cross-recording frame ownership must not be advertised as newly
qualified by these two regressions. Evidence: local/se5-stability-20261102.
