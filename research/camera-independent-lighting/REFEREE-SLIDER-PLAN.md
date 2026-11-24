# Referee control of every XeGTAO slider: plan and pass marks

2026-11-24, Fable/Opus (XeGTAO camera thread), sent to Astra at Cort's request (00:08Z "Send it to Astra"). A plan, not code. It builds on `research/contact-shadows/REFEREE-TUNING.md` (Astra's offline rules for the same controls) and on the live governor in the enhancer thread (bounded steps, shadow budget, the game's own picture always acceptable). The numbers marked "proposed" are first values for Cort to change.

## Principle

Each slider answers to one measured number. The Referee sets the slider from that number, measures the result, and corrects in bounded steps. Nothing is set once and trusted. Game names never select a value.

## The eleven sliders in four groups

| Group | Sliders (`xegtao_*`) | Measured input | Rule | Pass mark (proposed) |
|---|---|---|---|---|
| **Size** | `radius_x1000`, `radius_multiplier`, `falloff`, `sample_distribution`, `depth_mip_offset` | Lens state and scalars (F, A; b when the renderer gives it), depth spread of the scene, thin-feature coverage | Computed, not searched. Radius is held as a share of the scene's own depth (in units of b), so one value means the same reach in every game; falloff, distribution and mip offset follow from projected reach in pixels and detail density | Reach within 15 % of the offline-best reach on at least 90 % of held-out frames; changes no faster than 5 % per second while the scene is unchanged |
| **Strength** | `amount`, `final_power` | Existing darkness, added darkening as frame mean and worst 1/16-height tile, near-black share, broad contrast | Bounded steps toward the shared shadow budget; the existing-shadow guard subtracts what the game already shaded | Budget met within 12 checks from zero, never exceeded by more than 10 %, near-black and broad-contrast limits kept on every frame |
| **Cleanup** | `thin_occluder`, `denoise` | Fringe shading around thin foreground (two-sided depth differences), grain on flat receivers | Audition the neighbouring value on a quarter-size copy every N frames inside the time allowance; keep it only if the measured spill or grain drops and contact on supported recesses is not lost | Spill and grain within 5 % of the offline best; no visible step when the denoise level changes |
| **Speed and steadiness** | `quality`, `temporal` | Whole-frame present interval, cuts, gaps, motion validity, turn speed | Astra's rule: Ultra at or above 50 fps, Low below, back at 50. History zero on cuts and invalid motion, shortened as measured turn speed rises | fps floor kept; after a cut no carried shading; newly revealed areas clear within 3 frames |

## What makes it accurate

1. **Closed loop.** Every change is followed by a measurement of the stops layer the pass produced, and a correction.
2. **Try before committing.** Cleanup values are auditioned on a reduced copy, not switched blind.
3. **Proof against the best answer.** On saved recordings, search each slider exhaustively offline with the frame and exposure fixed; score what the Referee chose alone against that best on held-out frames. Overall pass (proposed): appearance loss within 5 % of the exhaustive best on held-out frames; in motion the 95th percentile of frame-to-frame change of the layer no larger than with fixed settings; identical decisions on DX11 and DX12 from the same inputs.
4. **Lens dependence stated.** The Size group reads the lens state. `remembered` or `measured`: use it. `guess`: use the 50 degree start, and the Strength group caps at half budget until the lens is measured (proposed), so a wrong reach cannot over-darken. No lens-shaped input at all: Size stays at defaults and the log says so.

## Log line (about once a second)

`referee_ao{lens= reach= reach_why= amount= power= budget_mean= budget_tile= thin= denoise= quality= temporal= fps= changed= why=}`

## Open decisions

- **Radius unit.** A picture cannot give a game's world unit (b). Proposal: radius as a share of scene depth, as above. With renderer scalars present, b is known and the two forms can be compared.
- **Budget values.** REFEREE-TUNING's audition caps (0.24 mean, 0.82 local) against the earlier 0.010 / 0.040: Cort sets them by eye.
- **Audition cost.** The reduced-copy audition needs a measured GPU time on the RTX 5070 Ti before it is allowed live.
- **Split.** Astra: per-slider rules and measurements (existing work). Enhancer thread: live binding, budget and time allowance. This thread: lens state and reach input.
