# Optional XeGTAO enhancement — 5 November 2026

Cort requested an optional, general effect using existing machinery, automated
testing only, with game testing retained by him. XeGTAO defaults OFF.

## Controls

Enhancements → XeGTAO: enable checkbox and Settings button. The modal has an X;
outside clicks do not dismiss it. Every setting is a live slider, with a one-line
tooltip when hovering over the title's actual text. Enable is also available
inside the window. Changes are saved per game. Reset restores Intel defaults
without forcing the effect on. No game or model restart is requested.

| Setting | Intel default |
|---|---:|
| Quality | High (2), 3 slices × 3 samples per side |
| Denoising | 1 (Sharp) |
| Radius | 0.5 world/view units |
| Radius multiplier | 1.457 |
| Falloff | 0.615 |
| Sample distribution | 2.0 |
| Thin-object compensation | 0.0 |
| Shadow strength / final power | 2.2 |
| Depth mip offset | 3.3 |

Values come from the pinned upstream header, rather than the README's older
mip-offset example. Low/Medium/High/Ultra sampling matches Intel's sample entry
points. Up to three denoiser passes use Intel's ping-pong/final-pass scaling.
The wrapper's blend is 100%, so these defaults apply Intel's result unchanged.
Spatial noise is fixed; no new temporal noise or game-camera jitter is injected.

## Reuse and placement

Intel's core, prefilter and denoiser are unmodified at upstream
`a5b1686c7ea37788eeb3576b5be47f7c03db532c`; attribution/license in
`third_party/xegtao`. Our glue extracts the actual depth subrect, reconstructs
normals with Intel's function, computes AO, and composites into scene colour.
DXC compilation happens at build time, using existing SDK discovery. GPU PSO
preparation is asynchronous. Sliders change constants or denoise pass count;
they do not compile pipelines. Disabled/zero amount records no AO work and
allocates no AO textures. Previously prepared resources can remain cached.

J2's existing scene/depth/camera acquisition supplies inputs. AO runs after the
motion/flow input has been captured. Native-resolution DLAA receives the shaded
scene; render-small writes it back to the selected scene subresource before
later game postprocessing/HUD. Existing MSAA writeback handles multisampled
colour. The at-Present acquisition fallback does not apply AO over a finished
HUD-bearing image. No new HUD detector or game-specific adapter was added.

Native SR uses the same pass before the game's own NGX evaluation. A small
scope temporarily replaces ONLY Color and restores it immediately afterward.
The full allocation and original colour bases are preserved; depth uses its own
base. Native game input is never overwritten, so repeated evaluations cannot
stack our AO on that input. Existing native recording leases retain the frame,
descriptors, copied depth and programs. The FG evaluation branch and injector's
own evaluations bypass this native-SR hook, avoiding duplicate AO there.

The existing camera cache now receives accepted SL2 constants. An exact
SL2 token/unique-viewport EvaluationScope reaches synchronous NGX calls; no
latest-global-frame guess. Legacy SL proxy scope remains unchanged. Games
without a matched camera or supported depth/colour surface retain ordinary DLSS
and report the optional enhancement unavailable. No executable/build hashes.

Shared scratch is scoped to a KNOWN queue, consumed immediately in the same
ordered work. Unknown native queues use independent scratch, rather than the
overlay's guessed queue. Descriptor ownership remains per executable recording.
J2 uses its existing fence ring; inline/native work uses existing RecordingLease.
No synchronous GPU readback or queue wait was added to callbacks. HDR values and
alpha are preserved. sRGB receives decode/composition/encode and a compatible
sRGB result resource. Input states are restored; split/enhanced-invalidated
states are not guessed. NGX's named-input read contract can supply an otherwise
unobserved NPSR entry state. No core ABI or frame-identity subsystem replacement.

This is extra AO on an already lit scene, not a replacement for a game's native
ambient-lighting calculation. Disable native AO where possible to avoid stacking.
Depth-derived normals do not include normal-map detail, and screen-space AO has
its usual visibility/edge approximations. We have not measured game GPU cost or
claimed universal game compatibility. Existing jitter, flow and HUD algorithms
were not rewritten. Game files, native NVIDIA DLLs and installed CMDs untouched.

## Automated evidence

Complete Release build passed, including production NeuralAliasDXGI.dll.
Baseline 202-case run: 196 passed, 5 skipped, one test-fixture failure in
310.94s. The new fake-SL evaluation test called its export directly, allowing
compiler assumptions across the patched function; calling through GetProcAddress
like the other hook tests fixes the test. Production scope did not change for
that correction. It and the other final affected checks pass.

After final sRGB/format/jitter-value refinements: 10 affected tests passed in
28.60s. They include actual GPU XeGTAO, persistence, SL2 boundary, overlay,
native HUD, J2 scene, MSAA, inline/post-entry and automatic placement cases.
Then all THREE AO-enabled production J2 paths passed in 11.01s: between-lists,
inline, and MSAA. No runtime edit followed these checks. This is not falsely
reported as one full 205-case run after all edits.

The GPU test demonstrates local contact shading, forward/reverse depth parity,
HDR clear sky/alpha, separate native subrects, odd sizes and resize, all four
denoise choices, sRGB roundtrip and zero D3D12 debug errors. It also exercises
NativeScope with fake NGX parameters and actual GPU work: Color is replaced in
scope, original bases/full extent stay intact, pointer is restored, original
game colour remains byte-for-byte unchanged, retained output survives scope,
and Off records no additional work. Model/engine fixtures are not live game
visual validation. Stock Intel AO's flat-plane baseline is compared at matching
locations rather than assuming its approximate integral equals 1 everywhere.

Raw logs: ignored main-checkout `local/xegtao-20261105`, especially
`full-build.log`, `full-test.log`, `final-build.log`, `final-test.log`,
`integration-build.log`, `integration-test.log`, and `boundary-test.log`.
Opus owns combined packaging/CMD and must include the upstream MIT license.
