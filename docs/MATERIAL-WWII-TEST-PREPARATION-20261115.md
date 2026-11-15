# WWII material test preparation

Cort accepted recipe v4's doubled fine-detail footprint. Keep that size. The
first game preparation uses PBRify RPLKSRd V3 colour and Normal V3, existing
authored normals, the approved fine/medium residual recipe and 65% strength.
This is a local game-material experiment, not a claim of universal material
classification or demonstrated in-game image quality.

## Actual source evidence

WWII a492f64 captured 48 immutable source DDS files in each of two sessions:
`j2-20261114-215609-s2_sp64_ship-p41164` and
`j2-20261114-215729-s2_sp64_ship-p41164`, under LocalAppData/NeuralAlias/captures.
Each index identifies complete source bytes using `texture-subresources-v1`.
No game textures, predictions or proprietary outputs enter Git.

Initial pairs are 1024-square opaque repeating terrain: gravel, colour
`0bc41ec6e2…` / normal `323abe2d3f…`, observed set `4413f042`; earth,
colour `9c0afa1914…` / normal `1a650f3a20…`, set `c2192f56`. Matching texture
features were inspected in decoded maps. Shared bind sets alone are not proof;
the spatial evidence and full source digests are retained in local selection
metadata. Neither the prep tool nor the runtime infers semantics from filenames.

WWII's normal view is BC5_SNORM (84). Preserve it as 84 with
`tangent_xy_snorm_positive_z`, not an unsigned texture relabelled as signed.
The producer uses Microsoft's DirectXTex to decode to R32G32_FLOAT and encode
signed float X/Y back to BC5_SNORM. Tests include negative components, all mip
levels, identities and actual compressed cache files. sRGB aliases are decoded
as stored values before explicitly applying the colour transfer function.
Reference: <https://github.com/microsoft/DirectXTex/wiki/Texconv> and
<https://github.com/microsoft/DirectXTex/blob/main/DirectXTex/BC4BC5.cpp>.

## Deliberate first-test scope

Publish colour and normal together. Leave packed BC1 material textures (including
the game's gloss/roughness interpretation) untouched: no established channel
mapping was in the supplied capture. Internal flat roughness arrays satisfy the
existing math API only and are never encoded, published or bound. This first
package therefore tests PBRify colour plus learned surface normals; it does not
claim the Lab's learned roughness is integrated. Negative normal signs, texture
coordinates and original authored grooves are preserved.

The models run offline. Runtime memory is the prepared BC7/BC5 mip chains; there
is no in-game model execution or additional surface shading pass. Larger maps
need Fable's per-shader plain-sampling qualification. The cache preserves source
shape, digest, signed encoding and complete mip history. Partial streamed ranges,
alpha-tested atlases, unclassified colour/data and packed maps are outside this
first preparation adapter, not silently converted into compatible sources.

## Verification and delivery state

47 Python tests passed, including real DirectXTex compression and signed float
roundtrips. The approved synthetic Lab remains intact. Both actual WWII units
are generated (4096-square, full mips, four compressed maps, 85.33 MiB total).
The local preparation receipt, selection and output keys are under
`local/wwii-materials`; no proprietary input/output is committed.

Astra merged Fable's cf09c93, then completed the runtime half after announcing
ownership in COMMS. Pixel-shader creation attaches its plain-sampling mask to
the shader. Shader changes and live enable/disable refresh bound textures on
the next draw, including when the game has not rebound its SRVs. Integer loads,
explicit mip levels and other size-dependent reads keep original maps. Signed
normal manifests require matching 84 source/output and the signed encoding.
The existing loader, unit publication, serial checks and GPU ownership remain.
Experimental > Material enhancement is persistent, off by default; status
separates prepared entries, loaded units and replacement binding counts.

Six focused C++/GPU tests pass: manifest, cache, shader texture usage, native11
provider, native11 DLAA and automatic native11 DLAA. The latter two also run
the exact prepared DDS bytes through real DX11 hooks on the RTX GPU. All four
replacement samples match direct prepared-map samples exactly; off restores
the original pixels exactly; changing to an integer-load shader restores them
without an SRV rebind. Measured normal sample changes are 0.0625/0.09375 and
gravel colour 0.015625 (fixture output encoding), not quality scores. Earth
colour's chosen sample quantizes identically; binding is independently checked.
These are synthetic-scene regressions using actual source/prepared textures and
the NVIDIA DLAA runtime, not live WWII quality/performance evidence.

Initial test failures exposed two fixture assumptions: it had relied on the
old default-on material policy, and it treated a loader-ready count as proof
that the following Present had published the unit. Explicit enable and waiting
for all four actual published replacements resolved those, without weakening
the production shader rules. Earlier parallel MSVC child processes failed;
the single-worker production build succeeded.

Scratch installation tests passed previous-build upgrade, payload hashes,
owned-entry uninstall, preservation of byte-identical preexisting cache and
exact original game-file restoration. Because WWII was running, only private
scratch script copies bypassed the process check; delivered scripts retain it.
The real game installation and running game were not touched.

No model runs per frame. Current acquisition requires captured textures;
automatic whole-game extraction and background model inference are not built.
The first package is two terrain materials, not a whole-game enhancement.
