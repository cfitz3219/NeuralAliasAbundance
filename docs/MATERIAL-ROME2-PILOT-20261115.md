# Rome II material pilot

Two original Rome II materials are ready for offline comparison. Open the
existing Material Lab and select **Compare Rome II materials**. The three
panels show the original compressed colour and normals, PBRify colour with
the original normals, and the same PBRify colour with added surface relief.
The Lab uses a shared light and gloss setting. Rome II uses its own lighting,
gloss, specular maps, masks and decals.

| Material | Witness | Source pack | Source and prepared size |
|---|---|---|---|
| Roman legionary shield | Legionary variant references the early scutum model, which names the two overrides | `models3_rome2.pack` | 512² to 1024² |
| Greek stone pavement | `greek_pavement_high_centre_piece01_destruct01.rigid_model_v2` references the pavement maps | `models2.pack` | 1024² to 2048² |

RPFM v4.7.4 inspected the archive indexes and extracted the DDS files. The
scutum has empty placeholder entries in `models_rome2.pack`. The populated
copies in `models.pack` and `models3_rome2.pack` are byte identical. The
`rome2-pilot.json` selection records the exact paths and extracted folders.
The two downloaded Workshop packs `Rome HD - colors version DEI.pack` and
`@@@@@orbis_terrarum_2_main.pack` contain overrides for the shield and pavement,
respectively. Their activation state was not established. For a comparison
against the original assets, use a preset where these overlaps are inactive.

`python -m tools.material_lab.rome2` consumes the extracted files and runs the
same PBRify V3 colour and NormalV3 models. The v4 fine and medium relief is
prepared at the existing physical scale, 65% baked strength. The largest
prepared mip is omitted for a 2× output size. The legacy DDS formats are
retained: DXT1 colour and DXT5 normals with X in alpha and Y in green.
The source's authored normal field provides the baseline. The Lab's 100% slider
setting displays the baked result. The preview decodes the actual compressed
mip data. The process performs inference offline.

## Package and checks

The local Mod pack `neuralalias_rome2_materials_pilot.pack` contains four DDS
overrides. It measures 10,487,088 bytes. SHA-256:
`50843D25419CEF87381EE1E59D88BEE45C8D700AB7353909952E659A81A75556`.
RPFM reopened the package and extracted all four files. Each extracted file's
SHA-256 matches the prepared input. The compressed texture payload is 10 MiB
instead of the originals' 2.5 MiB for these four files.

The 47 existing preparation regressions and the Lab controller tests passed.
The published Lab was opened in Edge and verified through its existing entry
point, both Rome II samples, the live slider and the return link. An RTX
5070 Ti rendered both sets with the Lab shader at 1× and 2×. Surface detail at
zero exactly matched PBRify alone. Both maps showed a measurable relief change
at 1×. These results characterize the Lab. The first game run will verify
the engine's loaded assets and visible effect at battle zoom.

Stage: `C:\Users\Cort\NA-material-preparation\local\rome2-materials\prepared`.
This includes the exact input/output hashes in `receipt.json`, the Lab page,
checked DDS contents and GPU test data. Game assets and generated packages stay
local. The eventual combined installer should place the pack under `data` and
record its hash for rollback alongside Fable's runtime files. The material
component is available now. The combined install needs Fable's tested Win32
capture and x64 DLAA companion package. A native Mod pack is loaded on game
start. Use the Lab for immediate texture A/B so the first combined launch can
check the materials in the game and compare DLAA live if the runtime supports it.
