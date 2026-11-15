# Material Lab: three additional AI models

Cort requested PBRify RPLKSRd V3 and both Real-HAT variants with a selector for
direct comparison. All three ran on Cort's RTX 5070 Ti against the six existing
256-square source PNGs. Eighteen new colour results are prepared at 1024 square.
Real-ESRGAN and Lanczos remain as the fourth and fifth choices. No game runtime
or installation changed.

## Delivered

Existing desktop shortcut: **NeuralAlias Material Lab**. Its target remains
Chrome opening `C:\Users\Cort\OneDrive\Documents\NeuralAlias-Material-Lab-20261114\Material-Lab.html`.
The new HTML is 59,008,574 bytes, SHA-256
`3a7890e3f85535e225519d8fe4321f2719b5a7d49988a4cbd7ea9d64ebb91ff5`.
Previous HTML preserved as `Material-Lab-before-model-selector.html`.

AI model selects the middle panel without moving the camera or resetting the
light. Pause Move the light for fixed-light comparisons. Complete material or
Colour detail only shows the model differences; the other isolation modes
explicitly explain why colour changes are hidden. Original normal/roughness
are retained for every AI model. The right panel remains coordinated mathematical
detail, not an AI upscaler. Six `Models-*.png` sheets show original + all five
upscales with identical light and camera; older `Preview-*.png` remain too.

## Model provenance

| Choice | Author weights | SHA-256 |
|---|---|---|
| PBRify | `4x-PBRify_RPLKSRd_V3.pth` | `1325a08842ebfc18aea90e71594aa9b9e82b9c7e16321dccf5b757884a22daf1` |
| Real-HAT fidelity | `Real_HAT_GAN_SRx4.pth` | `f5b1e3bbbb05147ca2beefcc715279cb647d7976cbda67d62ea7e6e20d5ffcc7` |
| Real-HAT sharper | `Real_HAT_GAN_sharper.pth` | `5800b67136006eb8cab3b4ed7c8d73b6a195bb18e6cc709b674f9aa069c00271` |

PBRify from [the author's release](https://github.com/Kim2091/Kim2091-Models/releases/tag/4x-PBRify_RPLKSRd_V3).
HAT from the [official README's](https://github.com/XPixelGroup/HAT)
[Drive folder](https://drive.google.com/drive/folders/1HpmReFfoUqUbnAOQ7rvOeNU3uf_m69w0?usp=sharing),
file IDs `1Ma12vCWT27P9M99-s2RXnynKN-OQsBrv` and
`1EioFq5-mKmv1uqta_Byd9cgXp9SU3zjj`. Download URLs, hashes, author notices and
inference receipts are copied into the Lab's `Model information` folder. No
model weights are committed or placed in a game.

Preparation uses Spandrel 0.4.2, PyTorch 2.11.0+cu128, torchvision 0.26.0+cu128,
Python 3.12.14. Models are restricted weights-only loaded, float32, RGB, 4x,
whole-image inference. Spandrel identifies architectures PLKSR and HAT.
Existing Fable preparation is reused: 16-source-texel periodic padding/crop,
original alpha, linear-light mips, BC7 sRGB, immutable validated cache.
Cache recipes include weights SHA and inference versions/settings.

Local reproducibility: `C:\Users\Cort\NA-material-preparation\local\model-comparisons`
contains the catalogue, three indices, receipts, cache and verification script.
Source PNGs are the existing `local/material-demo/sources`; they were not
regenerated. `tools/material_lab/README.md` gives the repeatable commands.

## Evidence and limits

- 31 Python preparation/intake tests pass with real BC compression.
- Real model inference completed for all 18 new results; each has finite output
  and the requested size, validated source identity and cache payload.
- All 654 embedded mip images decode with correct descending dimensions.
  Each material's five comparison choices have distinct decoded colour pixels.
- The shipped JavaScript selection controller passes using the actual generated
  gallery data: five choices bind their own pixels, originals and normal maps
  stay unchanged, and camera/light/material-selection state is retained.
  This is a mocked DOM/GL controller test, not browser pixel-render evidence.
- Offline rendered timber and linen sheets inspected. PBRify retains a more
  distinct weave on the linen fixture; HAT fidelity smooths it into bands.
  HAT sharper adds strong fine vertical markings on painted timber. These are
  observations of these synthetic inputs, not a universal model ranking.
- Packaged HTML hash equals the verified source, and the desktop shortcut
  points to that exact file. No actual game material improvement is claimed.

The local dependency setup initially failed because two installers overlapped
in one isolated environment. Both exited; reinstalling the pinned CUDA packages
sequentially and then Spandrel repaired it. CUDA arithmetic and `pip check`
passed before inference. No global Python environment or game files were altered.
