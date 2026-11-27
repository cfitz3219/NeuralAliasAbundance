# Missing-camera projection for adaptive XeGTAO: offline solve on the Resistance turns

2026-11-23, Fable/Opus thread "Make XeGTAO work without the camera", answering Astra's 09:37Z request. Offline only: no runtime change, no build.

## 1. What the shader needs

`xegtao::Projection{sx, sy, dx, dy, a, b}` (`src/host_dx12/effects/xegtao.cpp`, `FromViewProjection`): two focal scales, the centre offset, and depth = a + b / distance.
- `sy` = F (focal in half-heights), `sx` = F * height / width (square pixels), `dx = dy = 0` (centred; raster jitter is passed separately).
- `a` = -A for reversed depth (1/distance proportional to d + A), 1 + A for normal depth.
- `b` only sets the unit of distance. It cannot be seen in any picture (scale ambiguity) and is needed only because the radius setting is in world units. Either take it from the renderer's own number or express the radius in units of b.

So the unknowns are F and A, plus b for the radius.

## 2. Blind result: `dense_lens.py` on Cort's two turns

Input: colour, depth and `raster_jitter_px` of frames 0, 2, 4, 6, 8 of each turn. No matrix and no delivered motion is read. Dense optical flow gives 8,000 to 26,000 two-way-checked matches per pair; F, A and one rigid pose per pair are fitted continuously (exact rotation, not the small-rotation polynomial). Reference, revealed only for scoring: the 21 older Resistance matrices, which are a pure projection (identity view): sx 0.897701, sy 2.144508 (50.000 degrees), a = -0.00005, b = 0.100005.

| Fit | Turn 1 vfov | focal error | A | Turn 2 vfov | focal error | A |
|---|---|---|---|---|---|---|
| six pairs together | 49.991 | +0.02 % | 0.000046 | 49.987 | +0.03 % | 0.000067 |
| pair 0-2 alone | 50.004 | -0.01 % | 0.000039 | 49.959 | +0.09 % | 0.000098 |
| pair 2-4 alone | 50.029 | -0.07 % | 0.000040 | 49.973 | +0.06 % | 0.000087 |
| pair 4-6 alone | 49.992 | +0.02 % | 0.000043 | 49.975 | +0.06 % | 0.000076 |
| pair 6-8 alone | 49.990 | +0.02 % | 0.000045 | 49.989 | +0.03 % | 0.000075 |
| pair 0-4 alone | 50.001 | 0.00 % | 0.000043 | 49.992 | +0.02 % | 0.000066 |
| pair 4-8 alone | 49.982 | +0.04 % | 0.000048 | 49.993 | +0.02 % | 0.000058 |

14 of 14 fits pass the 1 % focal target (worst 0.09 %). Turn 1 against Turn 2: 0.01 % apart. Median reprojection residual 0.07 to 0.13 px. Offset: reference 0.000050, absolute error at most 0.000048 on a single pair, 0.000004 and 0.000017 on the joint fits. The reference comes from an older session, so "same zoom" is inferred from the agreement, not recorded.

**Raster jitter matters and its sign is minus.** Position = pixel centre - `raster_jitter_px`. Ignoring jitter, adjacent pairs alternate -0.46 / +0.54 / -0.32 / +0.24 %; with the wrong sign -1.01 / +1.02 / -0.69 / +0.45 %; with the right sign +0.09 / +0.06 / +0.06 / +0.03 % (Turn 2).

**Why the earlier estimates were coarser.** `image_depth_lens.py` searches a 0.25 degree grid (one step is 0.5 % of focal), with 79 matches on Turn 1 and no jitter correction. The 11-term readout I proposed at 04:35Z assumes small rotations; these turns are 1.8 to 5 degrees between captured frames (50 to 220 px), far outside it. Use the exact rigid model for recordings like these.

**Declines:** a still or a pure slide has no rotation to fit. Rule proposed: a pair counts only with at least 2,000 matches and at least 1 degree of fitted rotation, and a lens is reported only when two independent pairs agree within 0.5 %. Not yet run on the Still recording (zero shift gives zero matches of use).

## 3. Lighting scores with only the projection source changed

Test AO: my numpy horizon AO in XeGTAO's formulation (not Intel's shader), radius 0.5, quarter size, same settings on both sides, b from the reference on both sides. Activity over 0.001 stops.

| Frame | Projection source | precision | recall | strength within 1 % | within 1 % or 0.005 stops |
|---|---|---|---|---|---|
| Turn 1 f0 | blind lens | 0.9997 | 0.9998 | 0.941 | 0.998 |
| Turn 1 f8 | blind lens | 0.9998 | 0.9997 | 0.905 | 0.998 |
| Turn 2 f0 | blind lens | 0.9995 | 0.9997 | 0.828 | 0.971 |
| Turn 2 f8 | blind lens | 0.9997 | 0.9997 | 0.853 | 0.973 |
| Turn 2 f0 | exact F, A = 0 | 0.9993 | 0.9991 | 0.734 | 0.944 |
| Turn 2 f0 | F 1 % off, exact A | 0.9970 | 0.9999 | 0.363 | 0.945 |
| Turn 2 f0 | F 2 % off, exact A | 0.9941 | 0.9998 | 0.186 | 0.871 |

Placement passes 99 % on 4 of 4 frames from the blind lens. The strict strength gate fails on 4 of 4. It is not met even by a focal 1 % off with the exact offset (36 %), nor by the exact focal with A = 0 instead of 0.00005 (73 %). **The strict 1 % strength gate therefore needs the renderer's own scalars, to about 0.1 % in F and about 1e-6 in A. No picture measurement can deliver that; a focal target of 1 % and the strict strength gate are not the same requirement.**

## 4. Proposal: two independent witnesses

1. **Renderer scalars (exact).** Accept a lens from any matrix the existing scan sees, without asking it to be a camera: `DecomposeViewProjection` already yields sx, sy, dx, dy, and a, b follow from the depth column. Lens-only admission: sx / sy equals height / width of the bound viewport (0.1 %), centre within the raster jitter, the sign of b agrees with the observed depth direction, the same lens persists over frames. No position, no motion proof, and a held matrix is fine because a lens does not change frame to frame. Resistance's is a pure projection with an identity view, which a camera test rejects but a lens test accepts.
2. **Blind picture lens (section 2).** Independent of every matrix. Runs when the player turns; remembered per game.

Use the renderer's scalars when the blind lens confirms them within 0.5 % (that also gives b and passes the strict gate by construction). With no scalars, use the blind lens: placement passes, strength follows the Referee dial, radius in units of b. With neither, AO stays at zero and says which witness is missing. Zoom or scope: the blind lens disagrees with the remembered one, so re-admit.

## 5. Path table (frames independently graded / passed / unavailable)

| Path | Source | Graded | Passed | Unavailable | Note |
|---|---|---|---|---|---|
| DX12, Route B, reversed (Resistance turns, 2 x 30) | blind lens, 12 single-pair + 2 joint fits over frames 0-8 | 14 fits | 14 (focal 1 %) | frames 9-29 not yet fitted (non-adjacent, larger turns) | reference from older session |
| DX12, Route B, reversed (Resistance, older 90) | renderer scalars | 21 fresh | 21 lens-consistent (50.000) | 33 held (acceptable to a lens rule, not yet counted), 6 aspect failures rejected, 30 none | no motion, blind lens declines |
| DX12, Route B, reversed (Resistance Still, 30) | none | 0 | 0 | 30 | no motion, no matrix (recorder ran with the camera off) |
| DX11, Route B, normal (Three Kingdoms, 210) | renderer scalars | 180 | not rescored here | 30 | all bursts still: blind lens declines; needs one turn recording to grade the blind lens against a true camera in the same session |
| DX12, Route B, normal (Sniper Elite 4, 60) | none | 0 | 0 | 60 | menu scene, still, no matrix |
| DX11, Route B, normal (Pharaoh, 30) | none | 0 | 0 | 30 | still, no matrix |
| DX11, Route B, reversed | no capture | 0 | 0 | all | |
| Route A, DX11 and DX12, normal and reversed | no capture | 0 | 0 | all | |

## 6. Missing measurements and how to get them

- **Same-session truth for the blind lens:** one Three Kingdoms turn recording (camera found on 180/210 there), and Resistance turns recorded with the camera setting on "use when found" so the projection is in the metadata of the same frames.
- **Whether SE4 and Pharaoh expose any lens-shaped matrix:** rerun the scan with the lens-only admission and log every candidate's sx, sy, a, b and why it was refused.
- **Normal-depth offset from pictures:** Three Kingdoms has A = 0.002 (near 1, far 500), 40 times Resistance's; needs a turn with some walking.
- **Intel's shader as the matched reference** in place of my test AO, same projection swap.
- **Runtime form:** this fit took about 8 s per pair in Python. It is needed once per game and can run off the frame path; not designed yet.
