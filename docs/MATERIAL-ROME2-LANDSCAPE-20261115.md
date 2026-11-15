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
