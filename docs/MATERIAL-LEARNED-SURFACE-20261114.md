# PBRify with authored-surface-preserving learned detail

2026-11-14 Pacific. Cort asked Astra to pair PBRify with an existing material
model, preserve original surface detail, coordinate with Fable, and update the
Lab for a direct PBRify-only versus combined comparison. This increment is an
offline preparation/viewer change. Fable retains game acquisition, association
and runtime loading. No game installation changes.

## Model choice and provenance

PBRify's own normal and roughness models are an appropriate pairing with the
colour model Cort preferred. They estimate material response from the upscaled
colour; they do not classify potholes or prove what a dark marking represents.
The author's repository supplies the trained weights under CC0. NVIDIA's RTX
Remix ComfyUI reference workflow also uses these two models.

- Author: <https://github.com/Kim2091/PBRify_Remix>
- Model/source notes: <https://github.com/Kim2091/PBRify_Remix/blob/main/EXTRA_INFO.md>
- Licence: <https://github.com/Kim2091/PBRify_Remix/blob/main/LICENSE>
- Exact downloaded release: <https://github.com/Kim2091/PBRify_Remix/releases/download/v1.7.1/PBRify_Remix-1.7.1.zip>
- Reference workflow: <https://github.com/NVIDIAGameWorks/ComfyUI-RTX-Remix/blob/main/workflows/restapi_pbrify.json>

| Purpose | Exact file | SHA256 |
|---|---|---|
| Normal inference | `1x-PBRify_NormalV3.pth` | `b0a18270da765f02eaae3c228203bee0677fc28b2854a562515e4aae9c61223b` |
| Roughness inference | `1x-PBRify_RoughnessV2.pth` | `7003c39041af64cdb77d5120bd560a2878538fbb14a07657d8fd12aac5773679` |

Colour remains PBRify RPLKSRd V3, already prepared and preferred by Cort; it was
not silently changed to V4. Its provenance is in
`MATERIAL-MODEL-COMPARISON-20261114.md`. Both new models load as 1x SPAN, three
input/output channels. Real float32 inference ran on the RTX 5070 Ti with
PyTorch 2.11.0+cu128 and Spandrel 0.4.2. Input is the decoded 1024-square PBRify
colour, periodic padding 16 texels, whole-image inference, no tiles. The existing
model adapter quantizes output to 8 bits before map preparation. Normal XYZ is
decoded and normalized; roughness RGB is averaged to one linear channel.

The author's chain calls its normal output `normal_dx`. The Lab's tangent +Y
follows increasing texture V (down), matching that output. It is not flipped.
This does not qualify unknown game normal encodings: association and handedness
must remain explicit in Fable's runtime path.

## What is preserved, and what is added

1. Read and identify the original source through the existing cache. Keep the
   original normal and roughness as the base; do not replace them with a full AI
   estimate. Inputs are not regenerated or rewritten.
2. Run the two learned models on the same PBRify colour used in the middle panel.
   Remove broad prediction components with a periodic Gaussian low-pass (sigma
   1.5 original texels). Only the residual fine detail is added.
3. Blend normals using slopes (`xy / z`), not encoded RGB averaging. This uses
   standard partial-derivative normal blending, explained in
   [Blending in Detail](https://blog.selfshadow.com/publications/blending-in-detail/).
   Cap added slope length at 0.22. Cap roughness addition at +/-0.12 before
   retaining the original 0..1 range. No weak prediction is amplified to meet a
   target contrast or RMS. There is no height displacement or random noise.
4. Reduce additions near original normal, roughness and colour edges. This is
   an edge-protection heuristic, not a semantic recognizer or learned confidence
   score. An explicit protected-region mask can suppress additions exactly.
5. Reuse the existing linear-colour, normal-variance-aware mip pipeline and
   BC5/BC4 compression. Copy PBRify's BC7 colour DDS byte-for-byte into the
   combined entry. Both panels therefore have identical colour at every mip.

The original structure remains the foundation, but nonzero detail intentionally
changes surface normals/roughness. This is not a promise that every authored
pixel remains unchanged after resampling/compression. At the Lab's zero slider,
the shader samples the original textures directly, so PBRify alone and combined
are pixel-equivalent even with different original/enhanced map dimensions.
The slider interpolates slopes and roughness live, with the same light/camera.
Default 65% is a restrained comparison setting, not a measured universal optimum.

## Actual outputs and scope

Six existing synthetic materials were used: oak boards, painted timber, cool
slate, warm sandstone, brown linen and olive canvas. Twelve real surface-model
inferences completed. This set contains no captured game materials. Original
maps are 256 square; prepared colour/surface maps are 1024 square with complete
mip chains. The baseline still uses original normal/roughness textures.

The first observed still renders show restrained extra light-response detail,
not a dramatic transformation. Preserved grooves and weave are visible in both
enhanced arms. The purpose of the comparison is to assess whether the added
response is preferable; superior game quality is not inferred from tests.
Learned predictions can confuse baked illumination or painted marks with relief;
fine-only combination limits that problem but does not solve semantic ambiguity.

The full-strength combined cache is offline comparison content. Do not feed its
1024-square maps blindly to a runtime loader qualified only for same-size
replacement. Fable's role/format/extent qualifications and game-side integration
remain separate. The browser's 65% control is a viewer control, not a runtime
material-setting implementation.

## Reproduction and evidence locations

Workspace: `C:\Users\Cort\NA-material-preparation`.

- Runner and regeneration commands: `tools/material_lab/README.md`.
- Model weights/notices: `local/material-models` (not tracked).
- Actual learned prediction arrays: `local/learned-surface/predictions`.
- Six output entries: `local/learned-surface/combined.json`.
- Model/version/timing receipt: `local/learned-surface/combined.receipt.json`.
- Generated gallery, six PNGs: `local/combined-material-gallery`.
- Regression log: `local/material-models/surface-tests.log`.
- Actual-output verification: `local/material-models/combined-verification.json`.
- Actual-gallery controller log: `local/material-models/combined-controller-test.log`.

37 Python regressions pass with real texconv compression. Added cases cover
zero preservation, bounded additions, protected regions, constant estimates,
periodic shifts, invalid predictions and zero-slider rendered equality at
multiple lights/angles. The actual-gallery JavaScript controller passes selection,
shared colour, retained original maps, fixed camera/light and strength control
checks with a mock DOM/GL. All 360 embedded mip images decode; actual-cache
zero-strength renders match exactly in all 18 material/light/angle comparisons.
Each of the six nonzero-strength outputs differs from its colour-only control.
Offline reference renders were
visually inspected. These checks are not interactive browser GPU or game tests.

Existing Documents package is updated in place; the desktop shortcut remains.
The old five-model page is preserved as `AI-model-comparison.html` and is linked
from the combined page. `Preview-*.png` contains the new comparison; `Models-*.png`
retains the earlier five-model comparison. Author notices and receipts accompany
the package. No model weights, generated assets or third-party libraries enter Git.

Delivered combined HTML SHA256:
`7eed792a81467bc11e0298096b16e94cd29b35136f50ed76e4ad389e4234d3db`.
Preserved five-model HTML SHA256:
`3a7890e3f85535e225519d8fe4321f2719b5a7d49988a4cbd7ea9d64ebb91ff5`.
