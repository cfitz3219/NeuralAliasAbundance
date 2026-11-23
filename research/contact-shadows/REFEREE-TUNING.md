# Choosing the shadow and curvature controls

Cort approved the stronger lighting study as the starting look. The offline Referee now observes the scene, proposes individual settings and renders competing choices before accepting corrections. The approved lighting is the reference for appearance. Physical correctness, the earlier 99% camera-independence gate and RTX game performance require separate evidence.

## What the Referee uses

The inputs are registered linear DLAA color, oriented depth, and usable current-to-previous motion. The optional Intel comparison also needs a fresh qualified projection to convert projected reach into scene units. The custom contact and curvature path continues without it. Game names never select settings.

| Control | How the proposal and selection work |
| --- | --- |
| XeGTAO quality | Use Ultra at or above 50 FPS. Below 50 FPS select Low immediately, a drop of three levels. Return to Ultra at 50. Use the selected view's whole-frame present interval. GPU effect duration is a separate budget. Missing timing starts the offline audition at Ultra and is recorded as unknown. |
| Radius and multiplier | Estimate a projected reach from foreground thinness, convert it to scene units with measured depth and focal length, then trim for atmospheric haze. Audition both values and log their combined result. |
| Falloff | Test shorter and longer fading near the sampled contact. Reject extra shading along detached edges when another variant better matches supported recesses. |
| Sample distribution | Concentrate more samples close to the receiver on scenes with narrow foreground features. Audition alternate concentrations. |
| Thin-object compensation | Estimate narrow foreground features from two-sided depth differences. Test weaker and stronger compensation in the real Intel shader. The score penalizes fringe shading and loss of useful contact. |
| Depth mip offset | Test finer depth on detail-rich surfaces. The actual shader uses log2(sample distance) minus this offset. Increasing the offset selects finer levels for otherwise identical taps. This is the depth hierarchy, separate from material texture filtering. |
| Denoising | Compare Off and available spatial filter levels around the proposal. Smooth discrete denoiser transitions independently of the FPS quality switch. |
| Amount and final power | Adjust overall visibility blend and shadow response separately. Thin-edge spill, already-dark pixels, visible coverage and agreement on affected surfaces contribute to the objective. |
| Temporal stability | Set zero on cuts, gaps and invalid motion. Otherwise compare history weights on actual consecutive GPU evaluations, checking stable surface pixels and newly revealed areas separately. Longer continuous histories and fast camera turns still need player captures. |
| Curved lighting | Use the same proposal, audition and validation process for screen reach, shadow depth, outward lift, scale mix, edge rejection, haze suppression and the existing-shadow guard. Near-black protection, regional budgets and broad contrast remain enforced. The adaptive curvature pass uses four directions at 50 FPS and above and two below 50 while the effect is active. The existing whole-frame time allowance can suspend the effect. |

## Decision process

Use the current scene's thin-feature coverage, local luminance variation, distant haze evidence, existing darkening, eligible depth coverage and motion. The analytic rule maps these to bounded controls. Search neighboring settings offline, holding the input frame and exposure fixed. A tolerance around the approved look gives the search room to reduce fringe shading and excessive darkening. The objective penalizes missing the approved visible coverage as well as exceeding it.

Fit spatial settings on the first frame and test the proposed correction on other frames before retaining it. The analytic setting wins a tie. For temporal corrections, reserve additional valid pairs and discard a correction if they fail to support it. The last frame is held out from spatial and temporal selection. Separate recordings can be evaluated with a frozen calibration. Continuous settings move toward their target over time. Invalid history clears immediately. Per-session corrections are evidence from an offline replay, never game-specific substitutions in the renderer.

The current proof runs the actual Intel shader in a separate D3D12 test process. A paired export seeds GPU history on the preceding frame and uses real registered vectors and jitter on the next one. The custom contact and curvature estimator runs separately in NumPy. The Intel audition measures its own contribution against the accepted contact look. Adding stock Intel output on top of the accepted contact layer would double-darken some recesses. Production routing, per-view GPU reductions and fused budgets remain with the live integration after the motion review. No in-game parameter exploration or synchronous renderer readback is needed for this proof.

## Reproduction

From research/contact-shadows, with NumPy and Pillow available:

    python -m unittest test_referee_tuning test_form_lighting test_contact_shadow
    python tune_replay.py <30-frame-capture> <report.json> --gpu-fixture <xegtao_test.exe>
    python tune_replay.py <turn-capture> <turn.json> --gpu-fixture <xegtao_test.exe> --calibration <still.json>
    python quality_trial.py <camera-backed-capture> <report.json> <xegtao_test.exe> <threshold.json>
    python form_study.py proofs <capture> <output-folder> --referee-calibration <report.json>

Comparisons assume 60 Hz for converting archive motion to speed if no frame interval is available. This assumption never fabricates an observed FPS threshold crossing. The threshold trial instead supplies explicit intervals, including 20.00 and 20.01 milliseconds, and compares the resulting real GPU shading after a drop and recovery.

## Player recordings

Use the existing Resistance recorder. Stand still for 15 seconds, then make two separate 15-second turns in opposite directions from the same spot, including a little up/down movement. Frame thin foreground cover against more distant objects so both shimmer and revealed surfaces are visible. Each button hides the overlay. The same three captures cover every candidate. Slide belongs only to a flat strategy map. These capture-only runs test motion and camera-independent geometry. A later instrumented live run is needed to measure GPU cost on the RTX 5070 Ti.
