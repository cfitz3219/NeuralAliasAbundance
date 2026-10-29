# Poise 0.1 — font and preview only

Cort selected the Poise visual concept and authorized creating a usable font and preview, **without integration**. This isolated design folder is not referenced by the application, build system or installer. Fonts have not been installed into Windows.

## Deliverables

- `fonts/Poise-Light.ttf`, `Poise-Regular.ttf`, `Poise-Medium.ttf`: installable static TrueType faces, weights 300/400/500.
- Matching WOFF2 files for the local browser specimen.
- `preview.html`: offline, editable specimen with actual 12/14/16/18px text. Open in a browser; no server, network or installation needed. The Go button is intentionally inert.
- `Poise-specimen.png`: actual font rendering, not an image-generated approximation.
- `build.py`, upstream source, OFL license and SHA-256 manifest.

## Design and honest authorship

This is a **modified Jost derivative**, not a wholly original alphabet and not an exact tracing of the generated concept image. Jost's clean geometry is the foundation; the adaptation preserves a light display voice with more substantial controls and counters.

Changes: the capital A crossbar is lowered 12 units (1000-unit em); lowercase l gets a small curved foot to distinguish it from I; its advance increases 22 units; nonzero advances receive 6 units of extra breathing room. All three faces receive consistent changes. Composite accented glyphs inherit changed bases where they reference them. Original kerning/layout data remain. Edited outlines have hinting removed rather than retaining invalid upstream instructions. Family/style/version names identify Poise, while copyright and license information remain.

Original design: Owen Earl / The Jost Project Authors. Font adaptation and specimen: Astra for NeuralAlias, 2026-10-29. Original source and derivatives remain SIL OFL 1.1; see `OFL.txt`. No upstream endorsement is implied.

Source downloaded from `https://raw.githubusercontent.com/google/fonts/main/ofl/jost/Jost%5Bwght%5D.ttf`; SHA-256 `6343b70971000b04c5d401c96ae08ce371086135e999d5e1e1413039c0213076`. The exact source bytes are included, so rebuilding does not depend on the moving URL. Initial attempt to obtain the metadata-listed upstream commit path returned 404; Google Fonts source was used instead. License obtained from Google Fonts `ofl/jost/OFL.txt` on 2026-10-29.

## Validation and limits

All six outputs reopen/decompile with fontTools; family names, static weight status and sample-character coverage are checked. The three TTFs render the specimen through Pillow/FreeType; the specimen is visually inspected. 535 glyphs per face, inherited from upstream; this is not universal language coverage. No italics or bold face is supplied. At small sizes, use Regular/Medium; Light is for headings. Native Windows/DirectWrite and in-game legibility are not yet qualified, and no claim is made that this completes professional font production/hinting. O/0 remain visually similar; the preview exposes them for evaluation.

Rebuild with Python and `fonttools==4.68.0`, `brotli==1.2.1`, `pillow==13.0.0`: `python build.py`. The source hash is verified first. No renderer/game regression tests are relevant because no runtime code changed.
