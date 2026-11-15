# Material Lab: first coordinated-detail candidate

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
