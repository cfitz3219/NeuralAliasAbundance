# XeGTAO control and the player trial

Cort’s quality policy uses measured whole-frame FPS. Above 50 use Ultra, 40 through 50 use Medium, above 15 and below 40 use Low, and at or below 15 skip the pass and clear its history. The next healthy frame selects its own tier. Exactly 40 is Medium because Low starts below 40. The FPS rule governs performance. Missing resources or an uncomputable projection are separate execution conditions, logged as such. An existing game AO setting or dark paint alone supplies no reason to cut strength.

## What to program

Start with Fable’s four groups. Compute one projected reach from the lens and a representative scene depth. Keep the radius in the same units as that depth. Intel multiplies radius by its bias correction, so calibrating both independently against the same reach can yield multiple equally good answers. Hold the multiplier near its calibrated value while selecting reach. Measure the displayed addition to decide where the image needs relief. Spend spare GPU time comparing nearby cleanup options only after the cost of that extra work is measured. The exhaustive comparison belongs in offline validation. Where several values look equivalent, retain the current one. [Fable’s slider proposal](https://github.com/cfitz3219/NeuralAliasAbundance/blob/cb34a4a/research/camera-independent-lighting/REFEREE-SLIDER-PLAN.md)

| Dial | Intel reference or NeuralAlias policy | How to grade a change |
| --- | --- | --- |
| Quality | Intel defaults to High. Cort’s FPS tiers take priority. | Feed measured frame intervals across each boundary, verify dispatch at Ultra/Medium/Low, off at 15, then recovery. |
| Radius | Intel starts at 0.5 scene units. NeuralAlias takes a share of measured scene depth. | Check shadow reach on the same object as viewing distance changes. Compare with independent geometry or a calibrated frame when available. |
| Radius multiplier | Intel starts at 1.457, typical range 0.3–3. | Hold base radius fixed when auditioning the correction. Log the product. |
| Falloff | Intel starts at 0.615. Increasing the fraction starts attenuation earlier inside the radius. | Measure excess dark bands beyond contact, with equivalent coverage close to the receiver. |
| Sample distribution | Intel starts at 2. A power of 1 spreads taps evenly along a slice. Higher powers favor the center. | Score small grooves and broad contacts separately. |
| Depth mip offset | The source defaults to 3.3 and suggests 2–6. The README describes 3.15 from its experiment. Higher selects finer depth. | Compare thin-detail loss, temporal crawl and GPU cost on the current hardware. |
| Thin-object compensation | Intel starts at zero and suggests 0–0.7. Depth silhouettes conceal thickness. | Accept stronger compensation if it reduces fringe around wires and preserves contact beneath supports. |
| Denoising | Intel defaults to one spatial pass (Sharp), with 0–3 available. | Score flat-surface grain and lost contact detail at the same quality. |
| Final power | Intel starts at 2.2, typical range 0.5–5. Powers above one deepen partially occluded pixels. | Compare deep and partial contacts separately with amount held fixed. |
| Amount | NeuralAlias controls the mix with the original. | Compare before and candidate-after on supported contacts, near-black pixels and the worst tile. Back off where added shading overwhelms the combined picture. |
| Temporal stability | NeuralAlias adds motion-guided accumulation after DLAA. | Compare the same moving surfaces and newly exposed pixels. Bad motion clears history while spatial AO can continue. |

Intel calibrated combinations of controls on multiple views against ray-traced AO. That is the geometry reference for controlled fixtures. Cort’s blind votes supply an appearance reference on actual games. A budget is a ceiling, so the controller has no obligation to darken a scene until it reaches the limit. Intel’s settings and ranges here come from the revision vendored by NeuralAlias. [Intel implementation notes](https://github.com/GameTechDev/XeGTAO/blob/a5b1686c7ea37788eeb3576b5be47f7c03db532c/README.md) · [Settings header](https://github.com/GameTechDev/XeGTAO/blob/a5b1686c7ea37788eeb3576b5be47f7c03db532c/Source/Rendering/Shaders/XeGTAO.h) · [Shader](https://github.com/GameTechDev/XeGTAO/blob/a5b1686c7ea37788eeb3576b5be47f7c03db532c/Source/Rendering/Shaders/XeGTAO.hlsli)

## Player test and engineering checks

Open the generated `XeGTAO-trial.html`. Compare Original, Intel reference, A and B at the same exposure. The Intel reference uses Intel’s tuning, matching the comparison’s base radius and sample quality, with the common final shadow guard. Vote once per comparison and save the JSON. Seven still comparisons change one dial at a time, then two moving comparisons isolate spatial filtering and history. Radius multiplier tests effective reach with radius fixed. FPS changes are tested by the fixture. The two recordings’ original engine AO settings are unknown. The trial judges their rendered pictures. Engine-AO-on versus off remains a separate paired test. Estimated attendance is three to five minutes. The HTML embeds its pictures and works offline.

Feed the chosen brackets, ties and weak/overdone votes back into preference calibration. Preserve each recording as a unit when splitting calibration from evaluation. On a new game compare the controller against fixed Intel settings and a bounded offline search at identical quality. Separately log per-control targets, values, alternate values tried, actual before/after shadow darkening, worst tile, near-black change, contrast, GPU cost and time since a cut. Proposed metrics from Fable’s table are comparison scores until validated across games. A miss on a preference score does not disable rendering. Live reduced-copy auditions and shared DX11/DX12 automation still need implementation and timing. A fixed 50-degree lens and a half-budget cap remain Fable’s hypotheses, not a verified input or an extra shutdown rule.

The offline XeGTAO pass now retains full requested mix through weak existing shading, protects near-black pixels, and weights existing darkness only when the predicted tile exceeds the audition ceiling. It trims the brightest-to-darkest span correction by reducing AO gain when broad contrast goes out of bounds. The existing study’s 0.24 mean and 0.82 local stops are proposed audition ceilings. A dark original, a worse-looking candidate or a past calibration miss does not flip the AO enable switch.

## Matched projection replay

Use the shipping Intel shader in the existing isolated D3D12 fixture. With settings held constant and no budget or history applied, compare a blind lens against the older 50-degree Resistance matrix. Score affected pixels above 0.001 stops. Signed placement precision and recall must each reach 99%. At least 99% of affected pixels must be within the larger of 1% of reference strength and 0.01 stops. Report strict relative strength as a diagnostic. This tests equivalence of two projections, not whether the player likes the effect.

| Resistance turn / frame | Signed precision | Signed recall | Strength with 0.01-stop allowance |
| --- | ---: | ---: | ---: |
| 1 / 0 | 99.95% | 99.91% | 98.17% |
| 1 / 8 | 99.89% | 99.90% | 98.40% |
| 2 / 0 | 99.88% | 99.87% | 95.30% |
| 2 / 8 | 99.92% | 99.95% | 95.98% |

Full capture resolution was 3440 × 1440. The pan zoom is assumed equal to the older reference session and the blind fits use Fable’s published rounded values. All four placement comparisons passed. All four strength comparisons fell short. Rescaling the depth unit and radius together gave almost the same scores. Fable’s four NumPy results passed his version of the effect. Intel’s scores here apply to this exact shader and these controls. Compare F-only, offset-only and evaluation precision before attributing the difference to a specific scalar.

Run from `research/contact-shadows` with the game closed, the built `xegtao_test.exe`, NumPy and Pillow. All paths below are supplied by the operator.

```text
python quality_trial.py THREE_KINGDOMS_CAPTURE three_kingdoms.json FIXTURE fps.json
python projection_trial.py TURN1 REFERENCE_META 49.991 .000046 FIXTURE turn1.json
python projection_trial.py TURN2 REFERENCE_META 49.987 .000067 FIXTURE turn2.json
python build_ao_review.py RESISTANCE_STILL THREE_KINGDOMS_STILL TURN2 FIXTURE XeGTAO-trial.html --indices 5 13 --lens 49.987 .000067
```

The visual trial uses half-size replay, a fixed exposure for each scene, and a nine-frame slow-motion camera pan. Temporal candidates use the preceding recorded frame to seed each evaluation. Long trails, DLSS 5 interaction, native-resolution player preference, measured in-game FPS and parity across both live APIs remain ungraded.
