# SE5: DLAA evaluates, but its result intermittently disappears from the final picture

2026-11-03 Pacific. Read-only analysis of Cort's saved frames. Runtime:
53295cb7d73de7d0c377d6a9ae88a6c3ed51a134, DLL
41C3805A10C58272B9DF843D958B0F5C4E732C002AE9555712EF706560965618.
No game launch, control, deployment, settings changes or runtime edits.

## Finding

Cort's incessant on/off flicker is confirmed in the actual final pictures. A
successful NGX evaluation is not sufficient evidence of delivery. The NVIDIA
debug indicator exists in every inspected raw DLSS output but is absent from
many corresponding final pictures. Static stone detail in those final pictures
also follows the original raster jitter, while the saved DLSS output is stable.
This is a real visual contribution failure, not merely a changing status label,
and not an optical-flow comparison.

## Evidence and scope

Saved result directory:
`C:/Users/Cort/Documents/NA-se5-results-20261103-003218`.
Its manifest pins 53295cb and the DLL above; status log PID12060 identifies the
selected scene colour1441/depth1439, 3440x1440, inline route, presetK11.
The three Sniper capture sets inspected are under
`C:/Users/Cort/AppData/Local/NeuralAlias/captures/`.

| Capture | Saved pictures | Indicator in final picture | Visible/absent transitions |
| --- | ---: | ---: | ---: |
| j2-20261103-003156 | 30 evaluations | 18/30 | 15 across saved neighbours |
| j2-20261103-003200 | 30 evaluations | 10/30 | 12 across saved neighbours |
| presents-20261103-003151 | 40 consecutive Presents | 15/40 | 25 of39 consecutive boundaries |

The F8 collections have capture-writer gaps; do not treat their neighbour
transitions as a continuous frame-rate measurement. The separate 40-Present
collection is consecutive. Its metadata records DLAA active throughout and one
additional processed frame at each of the39 subsequent pictures. Nevertheless,
the NVIDIA indicator disappears in25 of40 final pictures. These are sample
counts, not an estimated percentage for all gameplay.

Detection uses a horizontal-edge template of the actual NVIDIA text in the raw
DLSS output, at x8..599/y1325..1437. Correlation with final pictures has two
widely separated clusters: approximately0.979..0.981 when visible, and
-0.001..0.012 when absent. Threshold0.3 separates them; the visible/absent
classification was also inspected directly in crops. The indicator remains at
the same position and is unobscured by the game's menu.

Concrete matched frames in j2-20261103-003156:

- #0/P1805, #1/P1806 and #2/P1807: raw DLSS output contains NVIDIA text;
  the final picture has none.
- #3/P1808: text is present in both.
- #4/P1809 through #6/P1811: text disappears from the final picture again.
- #7/P1812: it returns. This visually reproduces Cort's description.

Independent image evidence uses a static wall region x2600..3111/y160..671.
Between P1806 andP1807, the image-only phase-correlation shift toward the
previous frame is approximately(-0.819,+0.884)px in the original scene and
(-0.813,+0.884)px in the final picture. Raw DLSS output is approximately
(-0.001,0.000)px. Thus the final image's fine detail follows the jittered input,
although DLSS has produced a stable image. This is supporting image analysis,
not a GPU command-order trace or an engine motion measurement.

Across saved consecutive pairs in the first F8 set, median absolute luminance
change in the static arch ROI is0.01818 before DLSS and0.000377 after DLSS.
The arch region of the final image contains a game-menu panel, so its numerical
change is not used as proof about world detail. The wall and unobscured NVIDIA
indicator provide the cleaner witnesses.

## What the existing counters establish

Final status reports2211 evaluations covering2217 observed scene frames,
6misses,0late writes,0NGX failures,0busy skips,0history resets and one startup
cut. During the captured late interval the miss count stays constant. Flow is
off, with0AMD attempts/pairs. Successful output exists on all60 F8 evaluations.
These facts eliminate an optical-flow execution problem and repeated NGX
refusal/history reset as explanations for this particular captured flicker.
They do not establish that the output reached the screen.

The three-percent brightness jumps and changing texture contrast in final
images therefore cannot be dismissed by the nominal99.7% evaluation coverage.
The registry indicator was a useful visual witness, as Cort reported.

## Repair target and remaining uncertainty

The failure lies after the recorded raw DLSS output and before the final
presented picture: sharpening/write-back, the actual downstream source, and
recording/submission ordering are the relevant chain. The existing F8 output
capture is BEFORE RCAS and the write-back. It does not itself distinguish a
write-back failure from a downstream pass consuming an earlier/cached source.
The zero-late-write counter concerns observed writes to the selected colour;
it cannot prove that the final compositor consumed the updated version.

The latest Close recovery records1052 of2211 submitted evaluations, but the
capture identity does not record whether each evaluation was a Close recovery.
Do not claim that Close is the exact cause from a similar overall count. Nor
should more camera holding, motion filtering or relaxed placement checks be
called a fix for this evidence. Keep the repair focused on actual output
delivery with the NVIDIA indicator/source-image witness as its acceptance
criterion, reusing the current observer/capture services.

The latest candidate is NOT visually qualified. No runtime change or refreshed
installer is supplied by this analysis.

## Local reproducibility

Offline scripts, JSON metrics and visual crops are in
`C:/Users/Cort/NeuralAliasAbundance/local/se5-flicker-captures-20261103/`:
`inspect_frames.py`, `measure_patches.py`, `indicator.py`,
`measure_indicator.py`, their metrics/logs, `indicator-comparison.png` and
`wall-comparison.png`. They read row-pitched original raw planes with their
declared DXGI formats. Display crops use one fixed exposure mapping; numerical
measurements use the original linear data. No synchronous game readback or
new live instrumentation was added. Captures/artifacts are deliberately not
committed to the repository.
