# Fine material detail: 2x size trial

Cort approves the normal-distance improvement and requests doubling the fine
detail's size before choosing the final look. Recipe v4 changes only the fine
preparation scale: infer at half width/height (512x512 for these 1024 outputs),
then resample its slope/roughness residual to the existing output extent.

This doubles the model's texel-scale footprint in material coordinates. It is
not a literal zoom/crop of the old predicted image: the model sees the same
whole material at half size, so individual predicted bumps can differ. This
keeps the detail aligned with the material rather than moving a crack or grain
to a different part of the source. No camera-dependent scaling is involved.

The following are unchanged: original maps and source identities, PBRify colour
bytes, medium prediction files, medium filter/settings, strength gains, total
detail bounds, original-direction protection, output dimensions and byte budget,
mip filtering and rendering shader. Combination/capping can change the final
medium-plus-fine result even though the medium layer itself is unchanged.

Validation: 43 Python tests pass with real compression. Added regressions prove
an eight-texel feature has twice the spacing after half-size preparation without
doubling slope strength, and stays aligned under a material UV shift. The
actual-gallery controller passes. Actual RTX shader checks on six materials at
1x/2x pass exact-zero and increasing 65/100% strength. Same-light GPU comparisons
at 1x/4x show previous/new fine sizes; wood and cloth inspected. Exact medium
prediction paths, source identity, colour bytes, output budgets and shaders were
compared with v3. No game installation or game validation in this trial.

Current index: `local/learned-surface/combined-v4.json` and adjacent receipt.
Gallery: `local/combined-material-gallery-v4`.
GPU comparison images/metrics: `local/material-models/fine-size-v4`.
Slider checks: `local/material-models/gpu-slider-v4`.

Existing Documents Lab updated in place, with v3 saved as
`Material-Lab-before-fine-size-2x.html`. Its right-panel caption identifies the
2x size trial. Default view remains 1x and strength65%. Fine-size-*.png compares
PBRify alone / previous size / 2x fine size, with unchanged medium detail.
Delivered HTML SHA256:
`8af46a7475118afaaa37063d224461013bbeb28d050f2f00b574663869684d7e`.

No new texture layer or shader work: this remains offline preparation into the
same normal/roughness maps. This is a look-development trial awaiting Cort's
size preference, not a claim that the full in-game material feature is complete.
