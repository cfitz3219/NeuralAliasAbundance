# Rome II landscape test pack

The landscape pack covers the four Mediterranean summer battle grass ground
variants and the campaign sea colour in Grand Campaign and Imperator Augustus.
It can be added to Fable's native DX11 package as one owned component. In the
Rome II launcher, enable the landscape pack and use a clean preset. The
downloaded Orbis Terrarum II and Orbis Aquarum packs overlap these paths.

The ocean grade uses the vanilla maps from `terrain2_rome2.pack`. It brings out
the broader transition from shallow turquoise to deeper offshore blue. It
uses the existing coastline pattern as a guide and changes only RGB in affected
BC3 blocks. The original header, 2032×1440 extent, all eleven mip layouts, and
every alpha block are identical. Rome II continues to render waves, foam,
reflections and ship wakes from its existing assets. See the Lab's **Compare
campaign ocean** page for a fixed-size comparison of the actual encoded maps.

Reproduce the sea colour files with:

```text
python -m tools.material_lab.rome2_sea ORIGINAL_DDS OUTPUT_DDS PATH_TO_TEXCONV
```

Grass uses the approved stronger relief parameters from `a547c30`. Run the
existing Rome II preparation on variants 0 through 3 of the following paths:

```text
terrain/textures/battle/rome_med/grass/{0..3}_su_diffuse.dds
terrain/textures/battle/rome_med/grass/{0..3}_su_normal.dds
```

The source pairs are from `terrain2.pack`. For the runtime pack we used the
baked 1024×1024 mip and every smaller level verbatim. Each file keeps its
original legacy header and extent, so it can fit the game's existing terrain
tiles. The first variant stays derived from the prepared map Cort approved.
The original blade assets are separate from this ground material.

On an RTX 5070 Ti, the native-size export differed from the approved first
variant's high-resolution preview by 0.0244/255 mean at Lab Distance 3. At that
distance, each shipped grass variant still changed the rendered sample by a
mean of 3.36 to 5.69/255 from its added relief. In the 508×360 sea mip, about
79% of water pixels (original alpha ≤ 5) changed by more than eight channel
values in at least one RGB channel. All sea alpha blocks, all ten archived
texture hashes, and the complete grass mip chains passed verification. These
checks characterize prepared assets and the Lab. Game-map visibility remains
for the combined in-game session.

The staged `neuralalias_rome2_landscape_pilot.pack` is 16,196,011 bytes:

```text
BD3E9C5A015EAE898D8AB3399E8DDDB4DF713564CCF5C6FB40BEC3C976086EF0
```

The staging folder is
`C:\Users\Cort\NA-material-preparation\local\rome2-materials\landscape\combined-materials`.
Use `package-components.json` for the exact name, hash and relative destination
in Fable's combined manifest. The first shared test should cover a coastal
Grand Campaign view and a Mediterranean summer land battle during one game
launch. Start with sharpness disabled and test DLAA live once the companion is
available. The current hooks-only package does not provide DLAA.

## Campaign map pack for the combined DLAA test

Cort made the campaign map the priority. Use
`neuralalias_rome2_campaign_pilot.pack` for this test. The Mod pack contains
eight summer campaign ground materials from `terrain2.pack`, with four
diffuse and alpha/green normal pairs per family. The two sea maps above
carry forward byte for byte.

```text
terrain/textures/campaign/default/grass_a/{0..3}_su_{diffuse,normal}.dds
terrain/textures/campaign/default/grass_b/{0..3}_su_{diffuse,normal}.dds
terrain/campaigns/main_rome_map/lf_sea_colour.dds
terrain/campaigns/main_emperor/lf_sea_colour.dds
```

Select the grass pairs with `broad_relief` and `native_extent` both true
when running `tools.material_lab.rome2`. The export keeps their original
1024×1024 layouts, legacy DXT encodings, headers, and eleven mip levels.
The grass uses the same PBRify models and relief settings Cort approved.
It changes the ground surface. Grass blade geometry, other land materials
and winter tiles keep their existing game assets.

Re-extraction verified all 18 packed asset paths and hashes. The Lab
shows the actual compressed grass A and B variant 0 from this pack, and
links to the campaign ocean. At 1× and Lab Distance 3 on the RTX 5070 Ti,
the added relief changed the eight samples by a mean of 1.85–3.88/255
across four fixed light angles. The zero-relief comparison matches the
same colour with original surface relief. The Lab setting is a coarse
mip comparison. In-game visibility is still to be established.

Pack size is 24,586,327 bytes. SHA-256:

```text
E2A6F35CAF526ABF0DC29C27A95F30A5C1C87DB24FB0EF5EC3689AE0A655AC52
```

Staging is at
`C:\Users\Cort\NA-material-preparation\local\rome2-materials\landscape\campaign-component`.
Use both entries from `package-components.json`. Test install and rollback
with the 2f539a9 runtime in a scratch folder preserved the original
executable and an unrelated pack byte for byte. Fable owns staging the
user-facing combined CMD. Enable the mod pack in the Rome II launcher, then
view a coastline with inland grass in Grand Campaign or Imperator Augustus.
A clean launcher preset avoids the grass and sea overlaps from Orbis
Terrarum II and the sea overlap from Orbis Aquarum while retaining both mods.

## Rocky ground candidate

The next campaign surface is the base summer rocky ground family
`terrain/textures/campaign/default/rock_b` from the installed vanilla
`terrain2.pack`. The four variants show weathered, fractured and gravelly
surfaces in the source mips. The adjacent `rock` family includes much flatter
and partially neutral maps. The add-on targets `rock_b` only.

Extract the `rock_b` directory from `terrain2.pack` with RPFM. Reuse the
grass pipeline's broad slope calculation on the existing rock normal map at
65% of its full slope adjustment. Carry only the added broad detail into
each smaller mip, on top of the game's authored normal at that mip. Export
four native 1024×1024 DXT5 normal textures with their original headers,
eleven mip levels, and alpha/green encoding. The original rock colour
textures supply both sides of the Lab comparison.

```text
python -m tools.material_lab.rome2_rock ROCK_B_SOURCE_DIR OVERRIDE_ROCK_B_DIR PATH_TO_TEXCONV
```

On the RTX Lab renderer at 1×, the prepared normal maps changed 70.9% of
the tile pixels by more than two channel values at Distance 3, and 50.1%
at Distance 4, averaged over four variants and four fixed lights. The
default preview shows the packed strength. Zero strength pixel-matches
the original. The Lab offers weathered, fractured and gravelly examples.
Variants 2 and 3 have byte-identical original colour and normals. These
measurements describe the Lab's coarse mip view. In-game visibility is
pending.

Verified the four exact packed paths and hashes on RPFM re-extraction.
Pack `neuralalias_rome2_campaign_rock.pack` is 5,593,720 bytes and uses the
existing additive installer component contract.

```text
338711A25B79612A678F3F9C4760384D2DD2572268FA314DA7173D18AB492191
```

Staging and `package-components.json` are in
`C:/Users/Cort/NA-material-preparation/local/rome2-materials/landscape/campaign-rock/authored-component`.
The rock pack is an optional addition to the combined campaign test.

The installed Orbis Terrarum II archive overrides all eight paths with
different files. Orbis Aquarum and the four other indexed Workshop packs
have zero overlaps with the rock component. Use the same temporary clean
preset planned for grass and ocean when comparing rock. The test view can be
an exposed ridge beside the coast during the same campaign launch.
