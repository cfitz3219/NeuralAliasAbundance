# Picture-conditioned contact shadows

Cort rejected the visual strength of this first experiment. The stronger contact and curved-lighting candidate, native-resolution pictures, revised test limits and results are documented in [Visible lighting](VISIBLE-LIGHTING.md).

Offline experiment for the post-DLAA lighting layer. The existing Intel XeGTAO renderer supplies an independent visual comparison. The candidate estimates its own field from the registered input picture and directed device depth. It requires neither an engine camera nor an assumed field of view. It does not recover metric depth or surface normals for the separate curved-lighting pass.

## What determines a pixel's shade

1. Sample the center, midpoints and endpoints along opposing directions at two screen-relative distances. Both sides must approach the viewer continuously, remain in the viewport and have similar chromaticity to the center. This excludes a lone jump across an object outline.
2. Measure how much darker the contact is than the darker of its two sides, in log2 linear luminance. Export the strongest supported dip as the shared existing-shadow guard. Dark paint can produce the same evidence. A second consumer can request the guard with the contact strength at zero.
3. Group comparable geometry by contact strength and surrounding luminance. Estimate a target dip from the opposite checkerboard of screen tiles. Each qualifying tile contributes one vote, with at least three donor tiles required. Peers can be elsewhere in the picture. This infers what the game tends to do with similar contacts, even without a camera matrix.
4. Request only the positive difference between the inferred target and the existing dip. Blend close and wide estimates of a single contact. Clamp spatial and reprojected estimates against the current request. A dial of zero gives an exact zero layer. A picture with uniformly unshaded geometry can supply no positive examples and produces zero.
5. The output is a signed stop texture, zero or negative. For offline display only, multiply linear color by exp2(delta). Fable's runtime consumer takes a unit-dial proposal in NA1 delta[2] and applies its dial once on the lighting layer. Preserve a single shared guard. For the combined test, measure overlapping darkening from curvature and contact, and apply only the larger justified addition on the same crease before testing the joint budget. This integration is pending.

## Referee

Every evaluation updates the effect from valid scene coverage, texture activity, screen motion and available timing feedback. The controls are strength, near and far reach, the blend of scales, number and weighting of directions, continuity and material tolerance, donor quantiles and maximum donor contribution, spatial filtering, and temporal weight. A time-based transition smooths continuous changes. Timing pressure drops four directions to two with hysteresis. The hard frame or effect budget shuts the pass off. The caller owns the allowable cost and regional and average darkening caps. Fixed geometry-validity tests and hard picture-protection limits stay in force at every strength.

Motion uses current-to-previous pixel offsets. Reuse requires a consecutive frame in the same epoch, matching depth and lighting, and a plausible on-screen displacement. Under 50% usable vector coverage, the motion reading is unknown and the pass uses conservative spatial processing. This happened on seven Resistance frames. Replay assumes 60 Hz where a measured frame interval is absent. The 45-frame controlled test rotates the camera about its vertical axis with perspective-correct depth and independently reprojects each pixel. A stationary control configuration runs on the same frames.

## Offline evidence

30 archived frames per scene. The candidate replay sampled 3440 × 1440 inputs at 860 × 360. Each row gives the largest measurement across the 30 frames. The SE4 burst shows a menu. Game names identify these evidence sets only.

| Scene | Largest frame mean, stops | Largest tile mean, stops | Genuine Intel reference panel |
| --- | ---: | ---: | --- |
| Sniper Elite Resistance | 0.000157 | 0.005055 | Frame 17 |
| Three Kingdoms | 0.000236 | 0.008884 | Frame 25 |
| Pharaoh | 0.000053 | 0.002095 | No qualified camera |
| Sniper Elite 4 | 0.000116 | 0.003296 | No qualified camera |

Provisional caps were 0.010 stops for the frame and 0.040 for an aligned square tile roughly one sixteenth of the picture height. Every near-black share was unchanged, with the threshold at linear luminance 0.005. The broad contrast ratio across the 120 frames was 0.99990–1.00042. The additions are deliberately small in these captures. These numbers show restraint. They cannot establish visual preference.

The equal-geometry fixture repeats the same groove profile. At the centers of the unshaded grooves it adds 0.15662 stops on average. At already-shaded centers it adds zero. When the synthetic view rotates, the near reach falls from 2.67% to 1.73% of image height and previous-frame weight falls from 0.665 to 0.187. When the view stops they recover to 2.56% and 0.565. Frozen controls stay fixed throughout. All six candidate regression cases and the four existing XeGTAO CTest cases passed.

The Intel comparison runs the repository's pinned shader on white linear color and captured depth with a qualified, fresh view projection, High quality, 0.5 scene-unit radius, one spatial denoise and temporal accumulation off. Both comparison masks have the same scene and tile ceilings. Their actual mean strengths can differ. Registration, held-out reference, timestamped GPU cost, live DX11/DX12 equivalence, and a moving-player A/B are still needed before integration. The picture-conditioned target has no reference label for true physical occlusion.

## Reproduce

Use Python with NumPy and Pillow. From this directory:

    python test_contact_shadow.py
    python controlled_study.py <output>
    python study.py <J2-capture> <output/scene> --count 30 --step 4 --intel-exe <xegtao_test.exe>
    python inspect_crops.py <J2-capture> <output/scene>

Add --frozen-from <still-output/metrics.json> to the turn replay to export the same scene under the qualified still settings, signed per-frame metrics and a frozen layer on the selected frame.

The Intel executable is optional for camera-free cases. The replay emits measurements per frame, selected floating-point layers, a full-frame comparison and an enlarged crop. The inspection tool shows the strongest 80 × 80 crop beside a labelled magnitude image in stops. Captured game data and images stay local.
