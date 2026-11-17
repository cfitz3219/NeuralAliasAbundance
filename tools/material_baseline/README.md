# Material baseline: plain colour upscale

Owner: Fable. The comparison arm the coordinated candidate in `tools/material_lab`
(owner: Astra) has to beat: each source's base colour is enlarged by an existing,
unmodified image upscaler; normals and roughness are left as the originals. It
writes ordinary `docs/MATERIAL-CACHE-V1.md` entries with one map (`base_color`),
its own algorithm id, and the source identity copied exactly from the source
bytes, so `python -m tools.material_lab demo --baseline-index <index.json>`
accepts it.

Nothing here runs in a game. No upscaler program or model is in the repository;
the program's and the model files' SHA-256 go into each entry's recipe.

```powershell
python -m tools.material_baseline --sources local/material-demo/sources `
  --cache local/material-baseline/cache --index local/material-baseline/baselines.json `
  --texconv local/tools/texconv.exe `
  --upscaler <folder>/realesrgan-ncnn-vulkan.exe --model realesrgan-x4plus --scale 4
```

`--upscaler lanczos` uses Pillow's Lanczos filter instead of a program: a control
with no learned model, and a way to exercise the tool without one.

Tiling sources are padded by wrapping before the upscale and cropped after it,
so the result still tiles. Opacity is the source's, resized; the upscaler sees
colour only. Mips are filtered in linear light by the lab's own `mip_chain`.
