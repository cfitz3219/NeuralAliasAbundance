# Surface slider: actual signal loss and correction

2026-11-14 Pacific. Cort reported that Surface detail appeared to do nothing.
The earlier numerical-nonzero tests were insufficient to claim a useful control.
This correction is offline only and keeps Fable's runtime work separate.

## What the evidence identified

The original six model predictions were retained and measured at each stage.
The models were not re-downloaded or rerun with different inputs.

- **UI:** Colour detail only intentionally binds original normal/roughness, but
  the Surface detail slider was still active. It changed a uniform while both
  endpoints sampled the same maps. That mode is a definite no-effect case,
  independent of the model strength. We cannot establish which view Cort had
  selected when he reported this.
- **Protection:** on painted timber, normal slope RMS fell from 0.02330 after
  clipping to 0.00476 after protection (80% removed). On slate it fell from
  0.01355 to 0.00386 (72% removed). This blanket edge suppression also removed
  model detail that agreed with authored surface structure.
- **Filtering:** roughness variation was cut to about 39–53% of its original RMS
  on the woods/stones, before protection. Removing a global material offset is
  appropriate, but the previous 1.5-source-texel Gaussian removed useful
  intermediate-scale variation along with the broad estimate.
- **Display:** cloth retained considerably more normal detail. The older
  128-pixel reference measurements minified it heavily. A real 512-pixel GPU
  render showed cloth responding more strongly than wood. The Lab had a
  distance/mip-bias control but no way to magnify the material region.
- **Not an unplugged shader:** actual GPU rendering of the shipped shader with
  original prepared maps responds to strength. With a static light and 512-square
  panels, wood/stone average changes were only 0.22–0.36 sRGB8 levels at 100%.
  Source wiring alone was not enough to call the effect useful.

Artifacts under `C:\Users\Cort\NA-material-preparation\local\material-models`:
`surface-loss-stages.json`, `gpu-old-metrics.json`, `variant-metrics.json`, and
`gpu-slider-v2/gpu-surface-check.json`. Earlier arrays/cache remain intact.

## Correction

Preparation recipe v2 retains original normal and roughness as the base, and
copies the exact PBRify colour DDS unchanged. No noise or new colour contrast is
added. The combination continues to use standard partial-derivative normal
blending, not normal-map RGB averaging:
<https://blog.selfshadow.com/publications/blending-in-detail/>.

The Gaussian residual band is now six source texels instead of 1.5. Learned
normal relief has a fixed 3x maximum scale, roughness variation 2x, with maximum
added slope 0.35 and roughness delta +/-0.12. These are explicit artistic strength
settings, not a claim of physically measured material properties. There is no
per-material normalization to force weak predictions to a target contrast.

Blanket edge suppression is replaced with a directional constraint: where the
original slope magnitude exceeds 0.04, remove the part of the learned addition
that opposes it. Reinforcing and perpendicular detail survives. Thus a strong
authored groove cannot be flattened or reversed by the learned addition in the
uncompressed preparation. An explicit protected-region mask still preserves
those pixels exactly. At nonzero strength the appearance intentionally changes;
this is not a semantic guarantee for every painted marking or every material.

The 0% viewer endpoint samples the original maps directly. It does not substitute
resampled/compressed approximations for them. Learned material estimates remain
subject to baked-lighting ambiguity; the author's own notes explain that limit:
<https://github.com/Kim2091/PBRify_Remix/blob/main/EXTRA_INFO.md>.

UI changes:

- Moving Surface detail pauses the animated light at its current position.
- If Colour detail only was selected, moving Surface detail returns to Complete
  material and binds the actual enhanced surface maps.
- Detail zoom (1–4x, default 2x) magnifies the same UV region in every panel.
  Normal mip filtering is retained. A benefit is also measured at 1x; the zoom
  is not the only reason the new slider responds.
- The original five-model colour comparison remains linked and unchanged.

## Actual rendered evidence

Standalone GPU: NVIDIA GeForce RTX 5070 Ti. The test reads the shader from the
generated HTML, changing only GLSL ES 300 to desktop 330. It decodes the exact
embedded BC-derived maps, uploads their prepared mip chains and uses the same
sRGB/linear sampling, uniforms, UVs and lighting. This is an actual GPU pixel
test, not the existing mock-GL controller test. It is not browser automation or
a game run.

At 100% strength, same 512-square panel and grazing light, **1x whole-tile view**:

| Material | Old mean pixel change | New mean pixel change | New 95th percentile |
|---|---:|---:|---:|
| Oak boards | 0.36 | 1.43 | 4 |
| Painted timber | 0.29 | 1.10 | 3 |
| Warm sandstone | 0.34 | 2.45 | 11 |
| Cool slate | 0.22 | 1.13 | 6 |
| Brown linen | 1.13 | 4.00 | 10 |
| Olive canvas | 2.25 | 6.71 | 16 |

Units are sRGB8 channel levels over the rendered surface, excluding background.
These show a stronger signal, not a universal perceptual-quality score. Actual
0%/100% linen and timber comparisons were inspected: cloth has clearly stronger
fine light response; timber remains subtler. No dramatic gain is claimed on a
weak model prediction.

All six materials pass at both 1x and 2x: zero equals PBRify alone byte-for-byte
in the GPU image; the 65% and 100% images differ, with increasing average change;
full strength changes over 3% of visible pixels by more than two levels, with a
95th percentile of at least three. These are concrete regressions for this
fixed comparison set, not a definition of visual quality for arbitrary games.

38 Python tests pass with real compression, including retained authored slope,
exact zero, bounded change, masks and mip behavior. Actual-gallery and legacy
controller tests pass, including colour-only mode recovery, shared zoom and
light pausing. A temporary edit helper initially misread UTF-8 as Windows-1252;
the controller caught the unapplied pause change. Rebuilt the template from its
UTF-8 Git source, reapplied the edits, regenerated and reran the checks before
packaging. That failed intermediate page was never delivered.

Generated output: `local/combined-material-gallery-v2`. Index:
`local/learned-surface/combined-v2.json`. User package remains the existing
OneDrive Documents Lab; previous default is backed up as
`Material-Lab-before-surface-slider-fix.html`. `Slider-*.png` contains actual-GPU
endpoint comparisons. The desktop shortcut remains unchanged.

Delivered HTML SHA256:
`c3dedca0f873a7e7110a239b830bd537c3acf804e646b7aa9655cda966982aed`.
