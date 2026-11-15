# Material source capture v1

This is the offline consumer contract for Fable's capture producer. It reuses
`texture-subresources-v1` in `src/materials/texture_subresources_v1.hpp` and
`tools/material_baseline/texsub.py`. It adds no runtime hook or second digest.

**Primary producer format:** Fable's **4525b4e** now writes `index.jsonl` and
`<content_sha256>.dds`. The preparer reads that format directly; Fable does not
need to implement the earlier proposed raw-file container below. The raw-file
reader remains useful for shared golden-vector/padding fixtures.

For each qualified role in `source.json`, use the existing dump index and exact
digest instead of `capture`:

```json
"base_color": {
  "index": "dump/index.jsonl",
  "content_sha256": "<exact 64-character digest from the index>",
  "packing": "rgba_srgb"
}
```

The reader checks the DDS header against the index's view extent, format, slice
count and mip count, re-hashes all tightly packed levels with the shared
serializer, and decodes the same owned payload. A complete file proves the
declared range was exported; it does not prove a material role. `class`, `slot`
and `set` in the producer's census remain hints, not automatic role assignments.
The test imports an equivalent raw fixture and DDS dump into the same cache
entry, proving that the container does not replace GPU source identity.

Fable owns acquisition and observed role/packing qualification. Astra owns
`tools/material_lab/runtime_source.py` and preparation. A generated raw capture
fixture can establish byte compatibility, but is not evidence of acquisition
from a real game or successful replacement in its shader.

## Files and publication

A material directory contains `source.json` and one capture directory per role.
Publish raw files and capture descriptions completely before publishing the
material's `source.json`. Published capture versions are immutable. A material
association refers to one qualified version of each role; nearby bind slots or
matching filenames alone do not establish that association.

Example `source.json` (a generated fixture, not a game claim):

```json
{
  "material_id": "oak_boards",
  "family": "wood",
  "seed": 12,
  "fixture": true,
  "address": "wrap",
  "normal_convention": "tangent_xyz_positive_z",
  "roughness_convention": "perceptual_roughness",
  "runtime_maps": {
    "base_color": {"capture": "base_color/capture.json", "packing": "rgba_srgb"},
    "normal": {"capture": "normal/capture.json", "packing": "xyz_unorm_positive_z"},
    "roughness": {"capture": "roughness/capture.json", "packing": "perceptual_r"}
  }
}
```

The top-level normal convention describes the preparer's decoded representation.
Each map's `packing` describes its captured bytes. No role is inferred. Initial
generator families remain explicit wood, masonry or fabric. It requires matching
power-of-two map extents of at least 8 texels and periodic addressing. Packed
metalness/AO, normal handedness changes, gloss-to-roughness conversions, transparent
materials and automatic material classification are not implemented by this intake.

Each `capture.json` contains the eight agreed header integers plus the expected
runtime digest, observation status and raw-file list. This complete example is
Fable's two-mip 2x2 identity vector; its dimensions are for testing the digest,
below the generator's minimum size:

```json
{
  "identity_domain": "texture-subresources-v1",
  "resource_format": 28, "view_format": 28,
  "width": 2, "height": 2,
  "first_mip": 0, "mip_count": 2,
  "first_array_slice": 0, "array_size": 1,
  "complete_observed_range": true,
  "content_sha256": "881dba1cbcd2dae281ca92e0a9c73a8991d0659c5ce5eada825a461706aa4ec1",
  "levels": [
    {"slice": 0, "mip": 0, "file": "mip0.bin", "row_pitch": 8},
    {"slice": 0, "mip": 1, "file": "mip1.bin", "row_pitch": 4}
  ]
}
```

`mip0.bin` is bytes 0 through 15; `mip1.bin` is bytes 16 through 19.
Files contain exactly `row_pitch * rows` bytes, including any row padding.
They stay inside their capture directory. Levels occur in the agreed slice/mip
order. The consumer excludes padding and calls the shared serializer; only the
same owned byte snapshot is decoded. Changing padding leaves identity unchanged;
changing a texel fails against the expected digest.

Width/height are resource mip-zero dimensions. Partial views retain their real
`first_mip`, `mip_count` and slice; `complete_observed_range=true` means all levels
of THAT range were captured, not that the full original allocation was observed.
The preparer supports a single slice; the shared digest itself also covers arrays.

## Decoding and preparation

- Colour: typed sRGB RGBA/BGRA/BGRX/BC1/2/3/7 views require `rgba_srgb`; linear
  UNORM versions require `rgba_linear`. Both become linear colour in preparation.
  A shader that manually changes colour encoding needs its actual interpretation
  qualified before replacement; this intake does not assume that conversion.
- Normals: `xyz_unorm_positive_z` on RGBA8_UNORM, or
  `xy_unorm_positive_z` on RGBA8_UNORM/BC5_UNORM. Packed negative-Z or signed-normal
  variants are not silently reinterpreted. XY is reconstructed and normalized.
- Roughness: `perceptual_r/g/b/a` on RGBA8_UNORM; `perceptual_r` on BC4_UNORM.
  R8_UNORM decoding is ready but requires format 61 in the shared serializer.

Run the existing preparer:

```powershell
python -m tools.material_lab prepare <material-folder> --cache <cache-folder> --texconv <texconv.exe>
```

Its manifest uses the exact `texture-subresources-v1` source digest and observed
range, not a hash of an exported PNG. The recipe also retains source packing and
decoder version. Output remains BC7 sRGB colour, BC5 normals, BC4 roughness, with
coordinated mips. This is a NEW prepared representation: the runtime loader must
still check the shader's view/channel semantics and the original observed mip
range before substituting it. The output's new dimensions never overwrite the
source resource's recorded dimensions.

The old PNG fixtures remain `file-bytes`; they are never compared with a runtime
digest as though those identities were interchangeable.

## Evidence

`tools/material_lab/tests/test_runtime_source.py` consumes all four single-slice
shared golden vectors through actual raw files (including padded rows), checks
the fifth array vector against the shared reference, preserves partial views,
checks BGRA/BGRX interpretation, rejects altered content/incomplete captures and
prepares a three-map raw fixture through actual BC compression and cache reuse.
No source capture from a game is implied by those tests.
