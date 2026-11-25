# NA1 enhancer and its Referee governor

Status: first build, 2026-11-23. Owner: Claude (NA1, recombine, Referee measurement and control, binding).
Astra owns shared depth and surface directions, the XeGTAO adaptation, curved-surface lighting and the
existing-shadow guard; her parts plug in through the lighting-delta inputs described in section 6.

Cort's direction (2026-11-22): no fixed-strength look. "We need a way for referee to see the numbers and
decide if its strong enough. Or even, which of the algorithms need to be strengthend, which need to be
weakened." A dark scene is still enhanced, with the settings altered so its feel is kept.

This file is the contract. The processor reference in `src/core/na1_reference.hpp` and the shader in
`src/host_dx12/effects/na1.hlsl` are kept in step with it by hand, and a card-equals-processor test proves it.

## 1. Where it runs

After DLSS, before sharpening (RCAS), on the DLSS output, on a Direct3D 12 command list:

| Path | Insertion | Picture | Depth |
| --- | --- | --- | --- |
| DX12 added DLAA (`j2/live_dlaa.cpp`, `record()`) | after `na::dlss::Evaluate`, the Referee copies and the cut show; before the debug-view/RCAS block | `g.output` | `g.depth32` |
| DX11 added DLAA (`native_d3d11/live_dlaa.cpp`, `EvaluateFrame`) | same place relative to DLSS and RCAS, on the private D3D12 list (the `after` list on the 32-bit companion route) | `slot.output` | `shared_depth` |
| Route A (game's own DLSS) | not in this build; logged as "not built for this route yet" | | |

One implementation serves DX11 and DX12 because the DX11 route already evaluates on a D3D12 device over shared
textures. NA1 writes its result into its own scratch texture; the caller copies it over the DLSS output, so when
NA1 records nothing the plain DLAA picture goes on unchanged. Repeat frames reuse the finished output.

NA1 does not run, and says why, when: the master switch is off; shaders are still being prepared or failed;
the colour format is not one of `R16G16B16A16_FLOAT`, `R11G11B10_FLOAT`, `R32G32B32A32_FLOAT`,
`R8G8B8A8_UNORM` (sRGB views carried as stored bytes count as 8-bit sRGB); depth size differs from the picture
(upscaling, render-small, Present-time source); the debug view or smear test is active; DLSS did not run;
the frame pool is busy; the Referee has suspended it for time.

## 2. The picture maths

All in log2 luminance of the linear picture. `lum = dot(rgb, (0.2126, 0.7152, 0.0722))`, `y = log2(max(lum,0) + 1e-3)`.
8-bit pictures are sRGB-decoded first and encoded at the end. Non-finite input channels are treated as 0.

Depth key (no camera needed): raw device depth `d`; `q = reversed ? d : 1 - d`, clamped to >= 0. `q` is
proportional to 1/distance. Sky is `q < 1e-5`. The edge stop between two pixels is
`rel(a,b) = 2 (qa - qb) / (qa + qb + 1e-6)`, weight `exp(-0.5 (rel / 0.04)^2)`.

### 2.1 Passes

1. **Prep** (full size): writes `yq` (`R32G32_FLOAT`) = (y, q).
2. **Blur level 1** (full size, radius 3, offsets 1,2,3): two 1D passes, x then y.
   For centre c: `num = p_c`, `den = 1`; for each offset o and both signs:
   `w = exp(-0.5 (o / (0.5 r))^2) * exp(-0.5 (rel(q_n, q_c) / 0.04)^2)`, `num += w p_n`, `den += w`.
   Coordinates clamp to the picture. Result `b1` (`R32_FLOAT`).
3. **Down 1** (to half size, `ceil(w/2) x ceil(h/2)`): for each half texel take the 2x2 block (clamped);
   `q* = max q` of the block; `p = sum(w_i b1_i) / sum(w_i)` with `w_i = exp(-0.5 (rel(q_i, q*) / 0.04)^2)`.
   Writes `yq2` = (p, q*).
4. **Blur level 2** (half size, radius 6, offsets 1..6): as level 1 on `yq2`. Result `b2`.
5. **Down 2** (to quarter size, from `b2` and the q of `yq2`): as Down 1. Writes `yq4`.
6. **Blur level 3** (quarter size, radius 12, offsets 2,4,..,12): as level 1 on `yq4`. Result `b3`.
7. **Compose** (full size): see 2.2. Writes the result colour.
8. **Measure** (only on sample frames): see section 3.

### 2.2 Compose, per pixel

`u2 = up(b2)`, `u3 = up(b3)`: depth-aware upsampling. For a coarse level of factor F (2 or 4):
`t = (x + 0.5) / F - 0.5` per axis, `i0 = floor(t)`, `f = t - i0`; the four texels `i0, i0+1` (clamped) get
`w = bilinear * (exp(-0.5 (rel(q_coarse, q) / 0.04)^2) + 1e-3)`; result `sum(w v) / sum(w)`.

```
D1 = y - b1          D2 = b1 - u2          D3 = u2 - u3          BASE = u3
shown = Curve(y)                                   (section 4)
pool  = w_dark * 0.4 * sat((shown - p85) / (p97 - p85 + 1e-9))
for i in 1..3:  g_i = 1 + c_i * texture,  c = (0.5, 0.4, 0.25)
                e_i = D_i * (g_i - 1) / (1 + (|D_i| / 0.6)^2)
                if D_i > 0: e_i *= 1 + pool
add = e_1 + e_2 + e_3
if not sky: add += -0.35 * light * (BASE - mid)
add += delta_a * dial_a + delta_b * dial_b        (lighting deltas from the other parts, in stops; 0 when absent)
hi = sat((shown - 0.55) / 0.40)
lo = sat((m0/16 - shown) / (m0/16 - m0/64))
add = add > 0 ? add * (1 - hi)^1.5 : add * (1 - lo)
rgb = rgb_in * 2^add
if colour > 0:
    yl = lum(rgb);  ref = yl * REF          REF = (1 - w_dark) * (1,1,1) + w_dark * tint
    dist = length((rgb - ref) / (yl + 1e-5))
    rgb = max(ref + (1 + 0.6 * colour * (1 - sat(dist / 1.2))) * (rgb - ref), 0)
rgb *= 2^key
```

Exactness rule: when `add == 0` and `colour == 0` and `key == 0` the pixel is written back bit for bit
(no decode/encode round trip). Alpha always passes through bit for bit. With every dial at zero and no
deltas, `Prepare` returns nothing and no pass runs.

Dials: `texture`, `light`, `colour` in [0, 1.5]; `key` in [-0.5, 0.5] stops; `dial_a`, `dial_b` in [0, 1.5].
Scene values from the Referee: `mid`, `m0`, `w_dark`, `p85`, `p97`, `tint[3]`, the display curve.

## 3. What the Referee sees

On a sample frame (every 250 ms, never more than one in flight) one extra dispatch reduces the picture before
and after NA1 to a 128 x 72 grid of cells, 24 floats per cell, copied to a readback buffer in the same list and
read when that recording has retired. Nothing waits.

Cell (cx, cy) covers pixels `[floor(cx W / 128), floor((cx+1) W / 128))` by the same in y, sampled at stride 2
from the cell's first pixel. For each sampled pixel, with b = before, a = after (both linear):

| Index | Sum over | Value |
| --- | --- | --- |
| 0 | all | 1 (count) |
| 1, 2 | all | y_b, y_a |
| 3, 4 | all | shown_b = Curve(y_b), shown_a = Curve(y_a) |
| 5, 6 | all | near black b, a: `max(rgb) * (shown + 1e-5) / (lum + 1e-3) <= 0.000492` (8/255 at gamma 2.2) |
| 7, 8 | all | pure white b, a: the same display value `>= 0.9957` |
| 9 | solid | 1 (count); solid = not sky |
| 10 | solid | y_b |
| 11, 12, 13 | solid | r, g, b of the before picture |
| 14, 15 | solid | colour strength b, a: `(max - min) / (max + 1e-4)` |
| 16, 17 | solid | variety b, a: `length(rgb / (lum + 1e-5) - REF)` |
| 18, 19 | solid | fine_b, fine_b^2; `fine = y - mean(y at (x±2, y), (x, y±2))`, clamped coordinates |
| 20, 21 | solid | fine_a, fine_a^2 |
| 22 | band | 1 (count); band = sky pixel with a solid pixel among the four at ±8 and none among the four at ±3 |
| 23 | band | `|y_a - y_b - key|` |

From the cells (`core/enhancer_stats.hpp`):

- **texture** = sqrt(var(fine_a) / var(fine_b)) over all cells.
- **broad** = std(T_a - G(T_a)) / std(T_b - G(T_b)) with T the cell-mean y thumbnail, G a Gaussian of sigma
  3 cells, over cells that are more than half solid.
- **colour**, **variety**: mean over solid pixels. **near black**, **pure white**: percent of all pixels.
- **glow**: mean of column 23 over the band (0 when the band has fewer than 200 samples).
- **key shift** = log2(sum shown_a / sum shown_b).
- Scene feel, from the before picture: `m0` = mean shown; `key = -log2 m0`;
  `w_dark = clip((key - 2.6) / 1.4, 0, 1)`; `range` = p95 - p5 of solid cells' mean y;
  `w_range = clip((range - 4) / 1, 0, 1)`; `mid` = median of solid cells' mean y; `tint` = per-channel median
  over solid cells of mean rgb / its luminance, scaled so `dot(tint, lum weights) = 1`;
  `p85`, `p97` = percentiles of the cells' mean shown.
- **GPU cost** in ms from a timestamp pair around NA1's passes.

## 4. Screen brightness (the display curve)

`Curve(y)` estimates how bright a scene value is on the screen, 0..1 linear.

- 8-bit sRGB picture: the picture is already display-referred, `shown = clamp(2^y - 1e-3, 0, 1)`.
- Float picture: a 16-knot table over y (`x0`, `dx`, `cy[16]`, linear between knots, clamped at the ends),
  fitted from the existing Referee's paired thumbnails of DLSS output and the presented screen
  (`core/enhancer_screen_curve.hpp`: bin by output y, median screen value per bin, made non-decreasing).
  Only when the screen is SDR and the Referee is sampling. The screen thumbnail contains the HUD; the median
  per bin tolerates it.
- No fit yet, or an HDR screen: an assumed curve, `shown = t / (1 + t)`, `t = 0.18 * 2^(y - mean y)`.
  With an assumed curve the scene is treated as ordinary (`w_dark = 0`), and the log says so.

## 5. The control rule (`core/enhancer_governor.hpp`)

Each sample gives before and after numbers from the same frame, so the game's own baseline is always current.

Goals (unchanged from the design; Cort keeps them as they are):

| Number | Goal | Rest band | Hard limit |
| --- | --- | --- | --- |
| Texture | 1.10x | 1.08x to 1.12x | none |
| Colour (variety around REF) | before x 1.12, never past colour strength 0.60 | goal ± 15% of the gap | none |
| Near black | before x (1 - 0.30 (1 - w_dark) w_range), never below 2% | goal ± 15% of the gap | rise of more than 0.5 points when light is not wanted |
| Pure white | none | | rise of more than 0.3 points |
| Edge glow | none | | 0.03 stops |
| Broad contrast | none | | 0.92x to 1.10x |
| Key shift | 0 | ± 0.01 stops | none |

The game's own picture is always acceptable: a ceiling or floor only limits what NA1 adds. A number already
past its ceiling leaves that dial at zero.

One check of one dial (measured m, before m0, goal g, band [lo, hi]):

- inside the band: hold ("in band");
- dial below 0.05, or response `|m - m0|` under 1% of m0: fixed step of 0.05 up (or down); at the top with no
  response, report "no response" and return to zero until the scene changes;
- otherwise move 80% of the way to `dial * (g - m0) / (m - m0)`, by at most `max(25% of the dial, 0.05)`.

A broken hard limit cuts the responsible dial by `max(20%, 0.05)` instead. Broad contrast under 0.92 cuts the
light dial by `max(15%, 0.05)`. The key dial moves by `-0.9 * key shift`, at most 0.1 stops a check.
Applied dials glide toward the governed values a little each frame so no step is visible.

Time allowance: when the median frame time is over 22.2 ms (45 fps) and the enhancer's parts together cost more
than 0.5 ms, the costliest part is switched off and the reason logged; it is tried again after 30 seconds once
the frame time is under 18.2 ms (55 fps). NA1's own passes over 3 ms on average are suspended the same way.

Scene values are smoothed over about two seconds and snap on a camera cut or history reset.

## 6. The contract for the other parts (Astra's)

Each part gives the Referee: one dial (0 = off, picture unchanged, live), its own change as a per-pixel layer
in stops, its numbers, its GPU cost. In code:

```cpp
// na::na1::Inputs
struct Delta { ID3D12Resource* layer; DXGI_FORMAT format; float gpu_ms; const char* name; };
Delta delta[2];   // R16_FLOAT or R32_FLOAT, picture size, NON_PIXEL_SHADER_RESOURCE, stops, 0 = no change
```

NA1 multiplies each layer by its dial and adds it to the lighting term before white/black protection, so
texture is never darkened. The governor holds the summed darkening under the shared budget (Astra's proposed
0.010 stops mean, 0.040 worst tile; set by Cort's eye) by cutting the larger contributor first.
A part that cannot trust its inputs passes no layer.

## 7. Switches, logs, proof

- Setting `[na1] enabled` (default off), one row on the Enhancements page. Off means the program is exactly as
  before. No restart.
- Log events: `na1` (reason changes: why it is not running), `na1_governor` (each change of a dial with its
  why, the numbers before and after, the scene feel, GPU ms; with its own sample counter),
  `na1_curve` (fitted / assumed and why).
- Captures gain an extra plane `na1_out` when NA1 ran.
- Proof without a game: `tests/cpu/na1_reference_test.cpp`, `tests/cpu/enhancer_governor_test.cpp`,
  `tests/cpu/enhancer_stats_test.cpp`, `tests/host/na1_test.cpp` (card equals processor, zero is bit-exact,
  alpha kept; runs on the software device in CI), and `na_na1_replay`, which runs the processor reference
  and the governor over a saved capture and writes before/after pictures.
- Not proven by CI: speed, behaviour in motion, real games. Those need Cort's PC.
