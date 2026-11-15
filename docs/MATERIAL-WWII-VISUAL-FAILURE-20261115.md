# WWII material pilot: no visible benefit

## User result and scope

Cort tested e06f539: the control became active only at the original capture
location and switching it produced **no visible difference**. Visual acceptance
failed. Do not promote this to an enhanced-game claim based on loader/bind counts.

This cache contains two captured terrain pairs, not a whole-game pack. Identity
matching uses texture content, not coordinates or a hardcoded location. The same
material can match elsewhere; unrelated materials remain original.

## Existing evidence

Results: `C:\Users\Cort\Documents\NA-wwii-results-20261114-232439-572`.
Installed e06f539 DLL SHA256:
`BFBAEBA42768292DDC3926EE8C966541D043BF3D69B889BA2068738C29CCEB10`.

- PID41280: 251 textures hashed, no matching views/maps, no replacements.
- PID33868: 483 textures hashed; four matching views; four maps/two units loaded;
  zero map failures. 89,479,184 loaded bytes and 701,904 replacement **bindings**.
- Source views retired at exit, explaining the final replaced=0 after earlier
  replaced=4. Do not confuse resource retirement with failure to publish.
- The user repeatedly changed the switch; the enabled flag follows those changes.
- No new frame captures belong to these two PIDs. The earlier 21:56/21:57 source
  captures predate the material installation and cannot prove this run's A/B.

## Two independent questions

1. Does the actual drawing shader reference the substituted slot at Draw time?
2. If so, how much does it affect the final visible material?

The old `swapped` count answers neither. It counts PSSetShaderResources binds.
The compatibility parser intentionally permits unused slots (safe because no
instruction reads them). Also, the next shader can restore an incompatible
larger replacement before Draw. A bind can therefore be real without a shader
ever sampling the prepared material. This is a confirmed measurement gap, **not
proof that it caused this particular game result**.

The new `draw_refs{read,unread,unknown,deferred}` counts distinguish static shader
references after final policy refresh. They use existing creation-time shader
analysis and context shadows: no draw-time lookup, parsing, GPU readback, locks
or allocations. `deferred` counts recorded draws separately. The overlay no
longer presents the large binding total as the main progress figure.

Even a referenced shader instruction is not a final-pixel proof: branches,
clipping, depth, material blending, lighting and later passes can suppress its
contribution. Keep that qualification in the UI, logs and handoffs.

## Prepared-pixel measurements

Offline DirectXTex signed BC5 decode and explicit sRGB decoding compared the
actual installed DDS maps, not synthetic substitutes. For the same UV footprint,
original 1024-wide mip L is compared with replacement 4096-wide mip L+2.

| Effective width | Gravel colour mean difference (0–255) | Earth colour mean difference (0–255) | Gravel normal mean angle | Earth normal mean angle |
|---|---:|---:|---:|---:|
| 1024 | 2.20 | 7.41 | 7.81° | 8.28° |
| 256 | 4.08 | 5.36 | 5.47° | 4.89° |
| 128 | 3.32 | 4.22 | 3.45° | 3.32° |
| 64 | 2.73 | 3.37 | 1.84° | 2.10° |

Raw results and exact local script:
`C:\Users\Cort\NA-material-preparation\local\wwii-materials\mip-comparison.json`
and `inspect_mips.py`. These quantify differences, not quality or game pixel
coverage. The finer normal changes survive compression and filtering, with
expected attenuation; total erasure by mips is not supported. Gravel colour
alone is a subtle change. The source normals remain the base.

The game's original packed gloss/roughness is still bound. The Lab's learned
roughness response is therefore absent from this pilot. Do not equate the full
Lab demonstration with the colour-plus-normal game integration.

## Verification and next action

The existing real-D3D11 fixture now tests ignored bound slots, shader changes
without texture rebinds, incompatible shader restoration before Draw, and a
nonzero sampling footprint against the actual prepared mip chain. The earlier
constant-UV/zero-gradient probe forced the finest mip; its success was too
narrow to substantiate distance visibility.

Do not change the approved v4/2x/65% material recipe or swap AI models just to
make a stronger-looking toggle. First resolve actual game shader usage and
visible contribution. Fable has the evidence and a request to check any existing
terrain shader/packing evidence; Astra owns the consumption instrumentation.
No new game run is requested by this note. The installer stays on e06f539;
instrumentation is an engineering increment, not a claimed visual repair.
