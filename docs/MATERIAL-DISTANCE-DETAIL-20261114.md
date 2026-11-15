# Material detail at normal viewing distance

2026-11-14 Pacific. Offline Material Lab recipe v3.

## User observation and cause

Cort likes the learned relief at 4x but cannot see enough of it at 1x/2x. The
previous one-scale prediction concentrated useful detail into tiny output texels.
Normal minification averages those variations when the material occupies fewer
pixels. A larger strength alone does not supply missing medium-size structure.
The v2 numerical slider checks demonstrated an effect, but did not establish
that it was sufficiently visible for Cort at normal distance.

The existing mip generator already carries normal first-moment variation into
roughness. It was not missing entirely, and was not replaced in this increment.
Normal filtering must account for nonlinear lighting, not just average encoded
RGB normals: [NVIDIA's Toksvig paper](https://download.nvidia.com/developer/Papers/Mipmapping_Normal_Maps.pdf)
and [Han et al., Frequency Domain Normal Map Filtering](https://www.cs.columbia.edu/cg/normalmap/).
Our existing moment approximation is not an exact implementation of either
paper's complete shading model.

## Implemented change

Reuse the identical PBRify colour, NormalV3/RoughnessV2 weights and fine predictions.
Also run those two surface models on the same colour reduced by four in each
axis, using linear-light box filtering and the existing periodic inference
padding. For these six materials this is 256x256 inference beside the retained
1024x1024 prediction. Twelve additional small offline inferences; no new model.

Keep a medium-sized band: Gaussian sigma 0.75 coarse texels minus the broad
sigma-six-source-texel component. Upsample this band with periodic bilinear
sampling in the SAME material coordinates. Add its slopes/roughness to the
existing fine residual, using fixed gains 3/2. The total addition still has the
same slope-length cap 0.35 and roughness-delta cap +/-0.12. The authored-direction
constraint and protected masks apply to the combined addition. Original colour,
normals and roughness remain the base; the PBRify colour DDS is byte-identical.

This is a fixed material-space preparation. No camera-dependent enlargement,
negative mip bias, forced fine mip or new render-time branch. Existing mip
filtering and the EXACT old fragment/vertex shaders remain unchanged. The two
detail scales are baked into the same BC5 normal and BC4 roughness textures.
The fine prediction is retained, although the final bounded combination need
not be identical to v2 at close range.

The Lab starts at 1x instead of 2x. Its 65% strength default, exact-zero original
surface sampling, light pause on adjustment and same-region shared zoom remain.
Original / PBRify alone / Combined result remains the comparison. Samples and
colour-model comparison history are unchanged.

## Rendered evidence

The actual generated HTML shader and embedded BC-decoded mip chains are rendered
on the RTX 5070 Ti using the existing standalone ModernGL test (only GLSL version
declaration adapted). Same light, exposure, camera and 65% strength. This is
offline GPU evidence, not browser automation or game integration evidence.

At 1x, 512-square panels, mean absolute sRGB8 channel difference from PBRify alone:

| Material | Previous fine-only | Fine + medium | New 95th percentile |
|---|---:|---:|---:|
| Oak boards | 0.94 | 4.68 | 12 |
| Painted timber | 0.75 | 2.78 | 7 |
| Warm sandstone | 1.66 | 4.81 | 14 |
| Cool slate | 0.76 | 2.70 | 10 |
| Brown linen | 2.59 | 7.35 | 16 |
| Olive canvas | 4.34 | 7.62 | 16 |

These measure effect strength, not a quality multiplier. Inspected GPU comparisons
show more visible grain/relief at whole-tile size and retained fine texture at 4x.
Slate remains more subtle than cloth/wood; lighting and material content matter.

- 41 Python tests pass with real BC compression. New cases cover medium detail
  surviving minification, constant predictions introducing no broad tilt,
  original direction/masks, bounded combined strength and exact zero.
- Actual-gallery and legacy model-selector controller tests pass.
- Six materials x three zooms x two panel sizes (256/512) pass actual GPU checks:
  zero exactly equals PBRify alone; 65/100% increase effect; 1x gains over v2.
- Three fractional mip boundaries per material/size pass: maximum output change
  is no more than two 8-bit channel levels across +/-0.001 LOD. No discrete
  distance-dependent processing switches were added. This is not a guarantee
  that every game/material will be free of shimmer.
- Tests assert byte-identical original/baseline payloads, identical shaders,
  prepared dimensions and payload sizes between v2 and v3.

Evidence under `local/material-models/gpu-distance-v3`: JSON and six three-column,
three-zoom comparison PNGs. Previous fine-only sits between PBRify alone and the
new result. Indices/receipts: `local/learned-surface/combined-v3.json` and adjacent
receipt. Prepared gallery: `local/combined-material-gallery-v3`.

## GPU/storage cost

Relative to v2, there are no extra textures, shader instructions or samples in
the Lab's rendering pass. Identical DDS payload budget per material: colour
1,398,128 bytes, normal 1,398,128, roughness 699,064; total 3,495,320 bytes
(3.33 MiB including complete mips, excluding DDS headers/allocation overhead).
PNG sizes in the HTML can change because their contents compress differently.

This is NOT free relative to original 256x256 source maps. Their normal/roughness
payload is about 0.125 MiB; the 1024x1024 enhanced pair is about 2 MiB. Extra
texture storage/cache traffic remains a runtime consideration. The Lab also
samples original and enhanced maps for its comparison slider; that UI cost
should not be mistaken for a required game-shader pass. Fable owns runtime
binding/association and must measure actual game timing. Model inference is
offline, not an AI computation on every frame. Retain high mips for the close-up
detail Cort likes; this increment makes their content useful at more distances
without allocating yet another detail layer.

## Delivery and limits

Existing Documents Lab updated in place, previous default retained as
`Material-Lab-before-distance-detail.html`. Existing desktop shortcut and
one-click launcher unchanged. Hash of delivered default HTML:
`6dcc35ddf16382509e58033fbd938cf396b0438d4b55b966be2d1aa0e3363dfa`.

No runtime, game installation, source texture, model weights or colour output
changed. These are six synthetic materials with declared periodic positive-Z
tangent normals and explicit roughness; no claim of semantic understanding of
all game materials. Learned relief is an adjustable estimate. The original
surface remains the foundation, not proof that every added bump is physical.
