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
roundtrips. The approved synthetic Lab remains intact. Actual WWII map generation
is underway; runtime signed loader and per-shader larger-map integration are
coordinated with Fable in COMMS. A test package is not ready until those actual
prepared maps pass loading/binding checks and additive upgrade/rollback checks.
No game deployment or game launch has been performed by Astra.
