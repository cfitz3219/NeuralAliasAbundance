# Sniper: evidence for reversing the global consumer-wait rule

## What the saved pictures establish

Cort reports a7fce1f is worse and asks for a fresh comparison, including reversing
changes that made the result worse. Saved012353/PID17796 confirms that actual
DLL/source pin, not a stale installer. No game was launched or controlled during
this investigation.

All66 nonempty capture directories were inventoried;42 contain Sniper images,
1370 saved frames in total. The remaining directories include Anno and a separate
unidentified diagnostic. A contact sheet identifies them visually. Shared capture
storage means a file collected by the SE5 results script is NOT automatically an
SE5 image. In particular,002651/002703 and010808 show Anno. Do not use those as
Sniper passes. Existing historical qualifications are retained, not retroactively
erased. Offline evidence: local/se5-perspective-20261103.

Latest a7fce1f gameplay F8 sets012258/012329/012337 contain90 evaluations:

| Insertion recorded in metadata | Final DLAA indicator visible | Absent |
|---|---:|---:|
| Scene-end barrier, before Close |53|0|
| Close recovery |0|28|
| Exact-consumer trigger |0|8|
| Legacy late trigger |0|1|

Raw DLSS outputs show the indicator90/90. All90 report zero later writes. Thus
successful evaluation plus zero later writes does not establish that the result
contributed to the finished picture. The independent40-consecutive-Presents
capture012326 has the indicator28/40 and20 visible/absent transitions. Source
colour1441/depth1439/subresource0 are identical throughout all90, ruling out
alternating selected-resource IDs in these captures. This does not rule out
different downstream sources or an unobserved read.

The first F8 set has jitter on and18/30 finals with DLAA. The two jitter-off sets
have16/30 and19/30. Turning jitter off therefore does not solve this delivery
failure. Optical flow is off with zero submissions/attempts. On stationary
jitter-off012329, median raw-input and raw-output brightness/detail are nearly
identical between the visible/absent classes; final horizontal edge energy is
0.02882 versus0.02007. This is an actual picture difference, not only a missing
text label. Camera jitter can amplify the visible difference between processed
and raw frames, and camera-only motion/foliage quality remain separate questions.

Earlier saved Sniper F8 sets181141/181159/181203/205530/205535/205541/210510/
210515 show the indicator30/30 each. Those are selected successful evaluations,
not consecutive coverage of every Present; do not call them a full stability
pass. Earlier consecutive Sniper sets have real omissions too. Different scenes,
models and settings prevent treating these captures as a controlled FPS or
image-quality benchmark. The source/field comparison below identifies a concrete
regression mechanism rather than claiming every older defect was absent.

## What went wrong in our policy

4ce21f6 introduced HasRecentExactEntry(frame). It checks all accepted exact
consumers globally, without qualifying the final scene writer. A recent consumer
after writer A can suppress a qualified barrier after writer B. If B's actual
downstream read is unobserved, later matching/Close records DLAA after the game
already carried the picture onward. That produces a success counter with no
effect on the final image. a7fce1f's descriptor index fixes a genuine bounded-table
blind spot, but cannot reveal an operation the observer does not inspect.

A blanket removal of the wait restored the split-consumer case without needing
Close, but produced four late writes in the existing optional-tail regression.
That version was not packaged. The targeted reversal retains waiting only when
the CURRENT producer has its OWN three unambiguous final-consumer observations,
with the same one-frame freshness. A different producer does not borrow aggregate
hits or suppress an established earlier opportunity. Actual observed consumer
matching still supports previously unseen final objects; it is the decision to
wait that needs producer-specific evidence.

No camera/lens/raster ownership, descriptor index, optical-flow or UI work is
rolled back. No new route, guessed resource state, predicated NGX, GPU wait or live
readback is introduced. NVIDIA's manual-hooking restoration example was checked
against our restore code; this change does not assume an unproven graphics-state
failure or rewrite that subsystem:
https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideManualHooking.md#70-restoring-command-listbuffer-state

## Actual GPU regression, not only counters

New --inline-indirect-handoff extends the existing production-path DX12 fixture.
Two final scene producers alternate. One uses a direct post draw; the other uses
an actual ExecuteIndirect DRAW. The observer does not identify the shader read
inside that indirect operation. The GPU still consumes the scene before Close.
The fixture retains copied/null descriptors, range offsets and real downstream
pixel readback; synchronization/readback are confined to this test executable.

Before:8/17 checked downstream pictures receive the replacement,18/24 evaluations,
nine useless Close recoveries. The missed pixels contain raw half-float
3266/34cc/3666(.2/.3/.4). Corrected:17/17 pictures, the SAME18/24 evaluations,
zero Close recoveries. Both runs have six initial qualification frames. Existing
finite/unbounded table tests retain their one-cold-frame expectation; the new
scenario is not falsely credited with23/24.

The fixture uses the existing stand-in NGX model and actual DX12 shader/copy
execution. It establishes insertion ordering/contribution, not neural image
quality. Existing optional-tail, split-consumer, changing-object, raster/pan and
descriptor-range regressions are retained. CPU tests distinguish aggregate hits
from current-producer evidence, freshness, contradiction and unknown producers.

The live captures prove the late-path/output correlation. They do not record
whether Sniper's missed first read is indirect. Therefore the exact hidden
operation and sole live causality are not claimed established. This corrects the
source-proven global-wait defect that reproduces the observed pattern; Cort's
next game test qualifies the resulting actual picture.

## Final test and delivery receipt

Full-suite and package results are appended after completion. Current installed
game and prior camfix packages remain intact during engineering verification.
