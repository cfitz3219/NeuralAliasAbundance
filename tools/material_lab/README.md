# Material Lab


## Current: 2x fine-detail size trial (recipe v4)

Cort requested larger fine detail after approving the normal-distance result.
Fine inference now runs at half size; the existing medium layer and output
texture budget stay unchanged. Current index/gallery end in `combined-v4.json`
and `combined-material-gallery-v4`. Previous v3 remains available. See
`docs/MATERIAL-FINE-SIZE-TRIAL-20261114.md` for the change and evidence.

## Normal-distance detail update (recipe v3)

The current Lab bakes fine AND medium-size learned detail into the same maps.
The same surface models also run on quarter-size, linear-filtered colour;
material coordinates, original surface foundation, strength bounds and mip
filtering are retained. No new runtime texture or shader pass. The viewer now
starts at 1x. See `docs/MATERIAL-DISTANCE-DETAIL-20261114.md` for the evidence,
cost comparison and exact limits. Earlier sections below record prior versions.

Current paths: `local/learned-surface/combined-v3.json` and
`local/combined-material-gallery-v3`. Use these with the existing prepare/compare
commands. Test distance preservation against the previous delivered gallery:

```powershell
python -m tools.material_lab.tests.gpu_distance_check --previous local/combined-material-gallery-v2/Material-Lab.html --current local/combined-material-gallery-v3/Material-Lab.html --output local/material-models/gpu-distance-v3
```


## Surface slider correction (2026-11-14 Pacific, v2)

The combined candidate now uses the `learned_surface.VERSION == '2'` recipe.
Original maps remain the base. Instead of suppressing model detail at all edges,
it removes only the added component that would flatten a non-flat authored
normal. Broad prediction offsets still do not replace authored material values.
A wider residual band and documented fixed relief range retain visible detail.
0% remains exactly PBRify alone. Prepared prediction arrays are reused; the model
weights, PBRify colour and original textures are unchanged.

The **Surface detail** slider automatically pauses the moving light and returns
from Colour detail only to Complete material. The previous UI allowed a surface
control to change while its surface maps were deliberately hidden. **Detail zoom**
starts at 2x and magnifies the same region in all panels; 1x shows the whole tile.
It uses ordinary derivative-based mip selection, not sharpened or forced mips.

New output index: `local/learned-surface/combined-v2.json`; generated gallery:
`local/combined-material-gallery-v2`. Use these paths with the commands below.
Details, measured signal losses and before/after GPU evidence are in
`docs/MATERIAL-SURFACE-SLIDER-FIX-20261114.md`.

The optional actual-GPU check requires `moderngl==5.12.0` in the preparation
environment; this dependency is never linked into the game runtime:

```powershell
python tools/material_lab/tests/gpu_surface_check.py local/combined-material-gallery-v2/Material-Lab.html --output local/material-models/gpu-slider-v2
node tools/material_lab/tests/model_selector_test.cjs local/combined-material-gallery-v2/Material-Lab.html
```

The GPU test renders the shipped shader with the prepared compressed textures
on a standalone OpenGL context. The only shader adaptation is the GLSL version
declaration. This tests shader pixels, not the user's browser UI or a game.

## Current comparison: PBRify colour + learned surface detail

The default prepared Lab now compares **Original**, **PBRify alone**, and
**Combined result**. PBRify RPLKSRd V3 supplies identical colour to the two enhanced
panels. Author-provided PBRify NormalV3 and RoughnessV2 models supply fine detail
for the combined panel, with original normal/roughness maps retained as its base.
There is no procedural noise in this new candidate. Surface detail defaults to
65%; 0% uses the original surface textures directly and exactly matches PBRify
alone. The prior five-model comparison remains at `AI-model-comparison.html` in
the user package, linked from the new page.

These models run offline through the existing PyTorch/Spandrel environment.
The browser only displays prepared maps. Dependencies, weights and prepared
assets stay local; the game DLL is unchanged. See
[model provenance and combination details](../../docs/MATERIAL-LEARNED-SURFACE-20261114.md).

```powershell
python -m tools.material_lab.surface_runner --sources local/material-demo/sources --colour-index local/model-comparisons/pbrify-v3.json --cache local/learned-surface/cache --predictions local/learned-surface/predictions --index local/learned-surface/combined.json --texconv local/tools/texconv.exe --normal-model local/material-models/1x-PBRify_NormalV3.pth --rough-model local/material-models/1x-PBRify_RoughnessV2.pth
python -m tools.material_lab compare --comparison local/material-demo/comparison.json --output local/combined-material-gallery --baseline-index local/model-comparisons/pbrify-v3.json --combined-index local/learned-surface/combined.json
```

The surface runner requires declared periodic positive-Z tangent materials and
the paired 1x SPAN models. It reuses existing source identities, BC compression,
mip generation and immutable publication. It never overwrites authored sources.
Generated HTML embeds all needed maps; when packaging it, also preserve the
earlier five-model gallery as `AI-model-comparison.html` for the comparison link.

## Earlier procedural candidate (retained for comparison)

Owner: Astra. Runtime acquisition and binding: Fable. This is the offline M1
prototype from the approved material-enhancement plan, not an injected effect.

The preparer uses one deterministic fine-detail field for colour, tangent-space
normals and roughness. It preserves source dimensions and opacity. Wood follows
the source's dominant direction; masonry and fabric use different detail spectra.
Families are declared, not discovered by AI. No model, training or model weights
are included. The six original procedural examples are fixtures, not game assets.
"Held-out" labels reserve examples from tuning; they do not imply a trained model.

## Run

Python 3.11+ with NumPy and Pillow. The tested environment is Python 3.14.7,
NumPy 2.5.4, Pillow 13.0.0 on Windows. Use an isolated environment if needed:

```powershell
python -m pip install -r tools/material_lab/requirements.txt
powershell -NoProfile -File tools/material_lab/Get-Texconv.ps1
python -m tools.material_lab demo --output local/material-demo --texconv local/tools/texconv.exe
```

`Get-Texconv.ps1` downloads Microsoft's pinned texconv release and its MIT
notice into `local/tools`. It verifies the release's SHA256. No downloaded
binary or generated content belongs in Git. Compression uses CPU-only texconv,
one process per already-filtered mip, and never launches a game.

Open `local/material-demo/Material-Lab.html` in a browser with WebGL 2. It is
self-contained, including all compressed-and-decoded mip levels, and has no
network requests. `Preview-*.png` provides static reference comparisons without
a browser. The static renderer is a controlled GGX comparison, not a substitute
for a game's material shader.

The developer template is `viewer.html.in`, not the gallery. The former
`tools/material_lab/viewer.html` address is now a simple launcher for the default
generated demo, with still-picture links; it must not be packaged as the gallery.
Ship the generated `Material-Lab.html`. The 2026-11-14 user package has a desktop
shortcut named **NeuralAlias Material Lab** pointing at that prepared file.

Controls: material, moving light, view angle, flat/curved surface, map-isolation
comparison and distance. All panels use identical light/camera/exposure. The
distance control selects the prepared mip levels; it does not regenerate maps.

Until Fable supplies his baseline, the middle panel is explicitly **colour-only
control**, using this candidate's colour with the original normal/roughness.
It is not labelled an AI upscale. Import his baseline with a JSON object:

```json
{"oak_boards":"C:/materials/fable-cache/<entry-key>"}
```

Then append `--baseline-index path/to/baselines.json` to `demo`. Entries must
match the same source-map content and material id. Colour-only entries inherit
original normal/roughness. Missing cases keep the labelled control. Both sides
should use the exact `sources/` files, not regenerate with different versions.

## Cache and source contract

`docs/MATERIAL-CACHE-V1.md` and `src/materials/record.hpp` are the shared handoff.
An immutable entry contains a manifest and BC7 sRGB colour, BC5 XY normals and
BC4 roughness DDS files, each with the complete mip chain. The key includes
source hashes/interpretations, settings, seed, generator and compressor version.
Existing entries are validated and reused without rewriting their files.

Colour mips are filtered in linear light. Normal first moments carry unresolved
variance into an approximate perceptual-roughness filter without repeatedly
adding the same variance at each level. This approximation is for the declared
GGX-like convention; arbitrary game gloss/specular encodings need an adapter.
The BC files' actual decoded content supplies the preview, not pre-compression
arrays. Normal/roughness never use sRGB filtering.

For another declared source folder, supply `base_color.png` (sRGB RGBA),
`normal.png` (positive-Z unit tangent XYZ encoded as UNORM), `roughness.png`
(linear perceptual roughness) and `source.json` following a generated example:

```powershell
python -m tools.material_lab prepare path/to/source --cache local/material-cache --texconv local/tools/texconv.exe --strength 0.7
python -m tools.material_lab validate local/material-cache/<entry-key>
```

This first candidate accepts periodic power-of-two textures with matching map
sizes. No atlas, alpha-tested foliage, arbitrary packed maps, automatically
recognized channel roles or in-game texture identification is claimed. Runtime
mapping and any channel repacking remain Fable's work. Synthetic sources remain
marked `fixture: true`. Original game files are never modified.

## Verify

```powershell
$env:NA_TEXCONV=(Resolve-Path local/tools/texconv.exe).Path
python -m unittest discover -s tools/material_lab/tests -v
```

Tests cover exact strength-zero preservation, deterministic detail, protected
regions, colour-space filtering, normal/mip behaviour, real BC compression,
cache reuse, source mutation, baseline inheritance and malformed/partial entries.
Without `NA_TEXCONV`, the compression tests explicitly skip. These are offline
functional checks, not game compatibility or visual-benefit proof.

## Licensing and provenance

- Preparation and fixture code: newly written for NeuralAlias, under the project
  licence. The generated synthetic fixture images are marked CC0-1.0 for reuse
  in the shared comparison. They contain no game assets or third-party textures.
- DirectXTex/texconv: Microsoft, MIT; the download helper retains its notice.
- NumPy: BSD-3-Clause. Pillow: HPND. Install through their normal packages.
- Rendering uses standard microfacet lighting for comparison. This is not a
  claim that NeuralAlias invented microfacet rendering or AI material generation.
- No borrowed GPL implementation, model weights or proprietary SDK is included.
# Fable baseline and runtime capture handoff

To update the existing gallery without regenerating any inputs:

```powershell
python -m tools.material_lab compare --comparison local/material-demo/comparison.json --output local/material-demo --baseline-index C:/Users/Cort/NA-material-baseline/baselines.json --conventional-index C:/Users/Cort/NA-material-baseline/baselines-lanczos.json
```

The middle panel switches between the supplied Real-ESRGAN colour upscale and
Lanczos. Both retain original normals and roughness. Captions report actual
colour size and model; source identities must match. The right panel is the
coordinated mathematical candidate, not Real-ESRGAN applied to all maps.
Offline PNG comparisons now sample each map's real mip chain at its screen
footprint; a 1024-pixel colour map does not force 256-pixel normals to mip 2.

For raw runtime-source preparation, see [the shared capture contract](../../docs/MATERIAL-SOURCE-CAPTURE-V1.md).
The same `prepare` command accepts explicit `runtime_maps` in `source.json`,
reusing Fable's source digest and retaining the original view/mip identity.

## Multiple AI models in the same Lab

The **AI model** selector changes only the middle panel. Original (left) and
coordinated mathematical detail (right) stay available as references. Pause
**Move the light** for a fixed-light A/B comparison; material, view, distance and
light phase stay unchanged when a model is selected. Use **Complete material**
or **Colour detail only** to see colour-upscaler differences. Normal/roughness
isolation deliberately hides colour changes. Every AI comparison retains the
original normal and roughness maps. Results are prepared once, not inferred in
the browser or a game.

Use the existing source PNGs, not newly regenerated fixtures. The optional
PyTorch/Spandrel runner shares `material_baseline.prepare`, including source
identity, 16-source-texel periodic padding, alpha preservation, linear-light
mips, compression, validation and immutable cache publication:

```powershell
python -m tools.material_lab.model_runner --sources local/material-demo/sources --cache local/model-comparisons/cache --index local/model-comparisons/pbrify-v3.json --texconv local/tools/texconv.exe --weights local/model-downloads/4x-PBRify_RPLKSRd_V3.pth --model-id pbrify-v3
```

The runner uses restricted weights-only loading, float32 whole-image inference,
and records the model SHA-256, inference-library versions, architecture and
padding in the cache recipe. It validates output size and finite values. CUDA
is the default; `--device cpu` is explicit. Dependencies are local preparation
tools and never part of the game DLL. Install compatible CUDA PyTorch and
torchvision first, then Spandrel in the same isolated environment; never run
two package installers in that environment concurrently.

The model catalogue is a JSON list, for example:

```json
[
  {"id":"pbrify-v3","label":"PBRify · game textures","description":"PBRify RPLKSRd V3 colour upscale.","index":"pbrify-v3.json"},
  {"id":"hat-fidelity","label":"Real-HAT · fidelity","description":"Real-HAT GAN SRx4 colour upscale.","index":"hat-fidelity.json"},
  {"id":"hat-sharper","label":"Real-HAT · sharper","description":"Real-HAT GAN sharper colour upscale.","index":"hat-sharper.json"}
]
```

Index paths are relative to the catalogue; cache-entry paths in each index are
relative to that index (absolute paths also work). Every model must have every
listed material, with matching source identity and interpretation. Legacy
Real-ESRGAN and Lanczos comparisons remain available automatically.

```powershell
python -m tools.material_lab compare --comparison local/material-demo/comparison.json --output local/material-model-gallery --models-index local/model-comparisons/models.json
node tools/material_lab/tests/model_selector_test.cjs local/material-model-gallery/Material-Lab.html
```

The JavaScript test runs the real selection controller with a mocked DOM/GL;
it checks changed model images, retained original normal/roughness, fixed camera
and lighting, material switching and isolated-map views. It is not a browser
pixel-render test. Actual model runs have separate `.receipt.json` records.

Author sources: [PBRify RPLKSRd V3](https://github.com/Kim2091/Kim2091-Models/releases/tag/4x-PBRify_RPLKSRd_V3)
(CC0), [HAT](https://github.com/XPixelGroup/HAT) (Apache-2.0 repository;
official weights linked by its README), and [Spandrel](https://github.com/chaiNNer-org/spandrel).
Keep the downloaded authors' notices beside the local models. No weights are
vendored. These six procedural examples compare behaviours; they do not select
a universal winner for real game materials.
