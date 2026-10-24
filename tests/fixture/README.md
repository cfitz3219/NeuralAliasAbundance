# A3: deterministic reference fixture v2

This offline reference renderer emits linear scene-color frames, a separate HUD
composite, and exact analytical truth for its defined layered scene. It is the
portable input/oracle for C6 and Phase 3 acquisition algorithms. It does not
replace the existing D3D12 host fixture or claim to test GPU resource discovery.

Requires Python 3.8+ (standard library only). Run from the repository root:

```sh
python tests/fixture/scene.py --output /tmp/na-fixture --frames 12
python tests/fixture/check.py --truth /tmp/na-fixture/frame-003.json --candidate candidate.json
```

On Windows choose an output directory under `local/`, for example
`--output local/fixture-v2`. Do not commit generated frames or local captures.
CMake registers `na.fixture.reference` through `tests/cpu/CMakeLists.txt` in both
root and standalone CPU builds. Standalone fixture validation:

```sh
cmake -S tests/fixture -B build-fixture
ctest --test-dir build-fixture --output-on-failure --no-tests=error
```

## Scene and conventions

The scene is a perspective pinhole camera viewing layered world-space surfaces.
It pans in X/Y each frame and jumps on frame 6. A rigid foreground rectangle
moves independently; another rectangle deforms by time-varying shear. Visibility
changes reveal/disocclude the textured background. A moving alpha-blended layer
has its own material correspondence; a white screen-space HUD is composited last.
Frames 0 and 6 reset history. Every surface's material coordinates persist, which
provides analytical previous positions rather than an optical-flow estimate.

All arrays are row-major image order, index `y*width+x`. JSON numbers hold the
reference values; PPM files are clamped 8-bit previews (linear values, not an sRGB
encoding and not the numerical oracle). Current frame metadata includes:

- camera position, row-major view/projection matrices acting on column vectors,
  left-handed +Z, focal/principal point, near/far, and current/previous pixel jitter;
- pixel-center sampling at `(x+.5-jitter_x,y+.5-jitter_y)`;
- motion in current-to-previous render pixels, +Y down, excluding jitter;
- forward D3D depth in [0,1], using nearest **opaque** surface, with explicit
  foreground depth/motion/alpha for the transparent layer;
- linear SDR RGB scene color, with transparency blended before the separate HUD.

`frame-NNN.json` schema `neuralalias.fixture.v2` contains `planes`:

| Plane | Components | Meaning |
|---|---|---|
| scene_color / composite_color | RGB | Scene before / after HUD |
| depth / motion | 1 / XY | Opaque-surface normalized depth and material motion |
| surface_id | integer | 1 background, 2 moving rigid object, 3 deforming object |
| history_valid | bool | Correspondence is on-screen and visible in previous frame; false on reset |
| disocclusion | bool | Prior opaque correspondence occluded or outside the viewport; false on reset |
| hud | bool | Pixels touched by screen-space HUD |
| transparent_alpha | 1 | 0 absent, 0.35 foreground blend |
| transparent_depth / transparent_motion | 1 / XY | Transparent material's own correspondence; absent depth is 1 |
| transparent_history_valid | bool | Transparent correspondence on-screen; false when absent/reset |

Visibility uses continuous previous-frame material positions, not a nearest-pixel
raster lookup. A newly exposed opaque pixel still has geometrical motion but no
usable temporal sample; `history_valid` records that distinction. Transparent
pixels contain two motions, so consumers must use the alpha/validity data rather
than describe opaque motion as exact motion of the blended color. Cuts zero both
motion planes and invalidate history. The baseline intentionally uses planar
geometry, fixed orientation, linear SDR and a simple alpha layer; it does not yet
cover rotating cameras/sky, skinned 3D meshes, HDR exposure changes or GPU passes.
Those are additional controlled cases, not claimed coverage in this version.

## Candidate checking

A candidate JSON uses the same schema, frame/extents, reset, conventions and camera
metadata, plus `scene_color`, `depth`, `motion`, `history_valid`,
`transparent_motion`, `transparent_depth`, `transparent_history_valid` planes.
Other reference masks are optional in a candidate; they are not scored. The checker
compares camera parameters and reports checked/error counts and maximum error per
plane. Motion error is scored only where **reference** validity permits history;
candidate validity is itself checked everywhere, so marking all pixels invalid
cannot hide bad motion. Invalid-history motion values must still be finite.

Default absolute tolerance is 1e-4 (pixels for motion, normalized depth, linear
color and camera units); adjust explicitly for a quantized backend. Exit codes:
0 accepted, 1 numerical/validity mismatch, 2 malformed or incompatible input.
The checker is an offline strict oracle, not an automatic live confidence policy.

Ten tests cover independent camera/object/deformation formulas, projection and
depth inversion, cuts/disocclusions, layered motion/HUD, odd extents/determinism,
file round-trip and CLI failure codes. Wrong sign/scale, inverted depth, HUD leakage,
wrong jitter, all-invalid masks, NaNs and truncated planes must fail qualification.
