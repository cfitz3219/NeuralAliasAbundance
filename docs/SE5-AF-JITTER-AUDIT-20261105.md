# SE5 anisotropic filtering and jitter audit — 5 November 2026

Cort assigned AF and jitter/sharpness to Astra, HUD to Opus, and intermittent
effect disengagement the lowest priority. No game launch, input control,
installation or game DLL change was performed. Opus retains package ownership;
Cort retains visual testing. This work reuses the existing sampler variants.

## Installed evidence

The installed `bin/dxgi.dll` was SHA256
`42C9C16EAAB7939562CE3A4C84CB3A28B26E36C0F7F2D7FFEC0C5CC0DE8388FB`,
matching package/source f207b0aa3ddd0a14b758c04b6c7fa70d16cd1d62. The update CMD
selected that package. Runs33036 and21260 are actual game evidence, unlike
the automated model/host fixtures. Raw evidence and analysis scripts are ignored
under `local/se5-af-jitter-audit-20261105` in the main checkout. The later run is
also preserved in `Documents/NA-se5-results-20261105-021701`.

## AF: reproduced failure and bounded repair

The AF override incorrectly reused `sampler_wants_bias`, the negative-mip-bias
eligibility heuristic. That heuristic excludes ordinary linear CLAMP materials
to avoid biasing screen-space mip chains. Addressing alone does not determine an
anisotropic footprint. The shared predicate therefore prevented AF from
improving legitimate clamped material textures.

The standalone actual-DX12 test `j2_anno_shape_test --af-pixels clamp` reproduced
the old failure: choosing16x still returned the original flat gray mip rather
than the stripes. The WRAP version worked. This proves a coverage defect; it
does not prove every material in SE5 uses the excluded filter/address combination.

The repair separates AF eligibility from mip-bias eligibility. Standard linear
minification, including CLAMP, may receive AF; comparison, min/max reduction,
point-minification and no-mip lookups keep their filtering. Negative mip bias
retains its previous material heuristic. Existing native anisotropy is never
lowered. Host-created samplers remain outside both overrides and native inventory.

The test creates the original root signature and pipeline BEFORE selection,
uploads a real1024x64 mip chain and executes sampling with16:1 gradients.
It switches0→16→0→4→8→16→0, reads GPU pixels, and checks a1:1-gradient control
in the other half of the same output. On the RTX5070Ti:

| Addressing | 16x oblique mean absolute change | 8x change | 1:1 control change | Return to original |
|---|---:|---:|---:|---|
| WRAP | 0.500000 | 0.249996 | 0.000000 | Exact |
| CLAMP | 0.500000 | 0.257809 | 0.000000 | Exact |

The4x result remains gray for this intentionally16:1 footprint; that is expected,
not an ignored selection. Both test modes report zero failures. These are actual
hardware filter results, with no fake NGX model. Test utilities alone wait/read
back; no synchronous GPU diagnostic readback was added to a live callback.

A separate source-confirmed delay was visible in run33036: requested16x at
01:56:18 became active01:57:08. The background worker was asked for one mip
level but compiled pipelines for EVERY prebuilt root level (1111pipelines became
4444twins). It now compiles only the requested level. The pixel test asserts
one pipeline compilation for first16x activation despite three root levels
already being prepared. Later requested levels remain buildable on demand.
Compilation stays off the game's creation callbacks and below normal priority.
This removes redundant work; it does not establish an instant first activation
in the game. Cached static variants switch at recording Reset. Existing dynamic
sampler descriptors are not retrospectively rewritten by this increment.

The AF static counter previously included changes caused ONLY by mip bias.
It now counts AF changes separately. Original sampler inventory distinguishes
native16x requests from our increases. UI descriptions no longer promise that
every screen-space filter is excluded, nor a fixed preparation time. The1:1
control above checks one ordinary screen-space footprint, not every possible
effect shader or explicit-LOD lookup.

## Jitter: measured alignment, not a speculative rewrite

Independent phase correlation of consecutive actual captured frames, using
static masonry regions, compared image displacement with declared jitter delta:

* Run33036, seven native-resolution capture sets: input slope0.94–1.03;
  output slope0.001–0.008. Stationary motion median was zero. Both Halton and
  R2 were present among these captures.
* Run21260, real gameplay at2293x941→3440x1411,39high-confidence region pairs:
  early HDR scene slope1.0092, final DLSS-input slope1.0086,
  DLSS-output slope0.0005 (output displacement converted to render pixels).

Thus in these samples the physical raster moves by the declared render-pixel
offset and DLSS removes it. This argues against a global sign, scale, UV-unit
or sequence-delivery error. It does NOT establish alignment of every light,
transparent contribution or screen-space effect. Several zero-jitter native
evaluations remain separately unexplained; saved-frame gaps are not proof of
missed evaluation. Inline capture camera fields left at default zero must not
be treated as evidence of an absent camera.

Source audit: viewport jitter uses signed subpixel offsets within±0.5 render
pixels and preserves the first eligible sample per recording. Depth-tested
draws on the selected scene depth are shifted. Depth-disabled passes retain
their viewports, avoiding double shifts of ordinary postprocessing. This can
leave mixed contributors if a depth-disabled world/light pass needs the same
sample, or if screen-space world reconstruction assumes an unshifted camera.
That is a concrete hypothesis to investigate, not an established SE5 cause.
Do not simply shift all fullscreen passes or revive the old ineffective matrix
writer: that can shift the same image twice or move the HUD.

Current counts: DLAA16, Balanced24 at58%, Performance32 at50%; the helper uses
`ceil(8*(output_width/render_width)^2)`, capped64. Actual2293/3440 Quality yields
19 because integer width rounding puts the ratio just above1.5. Native-DLSS
interception preserves the game's own sequence. The cap would undercut NVIDIA's
72phase recommendation at3x Ultra Performance; current render-small choices
stop at Performance, so that is not today's Quality/Performance defect.

No jitter amplitude, sequence, motion convention, preset or native game setting
was changed by this AF increment. Small texture-detail differences are not alone
proof of malformed jitter: input mip content and filtering must be checked too.
The submitted screenshot's light shaking remains unqualified by these static
masonry measurements. Do not claim it fixed.

## Primary-source corrections to the supplied jitter advice

NVIDIA's31March2026 guide, sections3.6–3.7, recommends Halton and says training
uses it; it ALSO says other patterns should function correctly, though untested.
Using R2 therefore does not inherently break the model. Jitter parameters are
render-pixel offsets, not normalized UVs. The recommended scaled phase formula
is8*S*S:2x linear upscaling needs32, not8. Jittered and unjittered motion are both
supported if `MVJittered` matches; our reconstructed vectors intend to exclude
jitter and the flag is clear. The mip baseline in current guide3.5 includes
`NativeBias + log2(render/display) - 1`; more-negative bias can itself shimmer.

The linked2023 forum contains a user's unanswered observation, not proof of
the most common root cause. The2026 reply proposing `IsDebug` is explicitly
challenged by its original poster because that flag is absent from published
headers. Use the documented development-DLL registry utilities and guide3.18
debug controls rather than inventing a feature bit. No production/development
DLL swap in either game was performed.

References:

* https://github.com/NVIDIA/DLSS/blob/main/doc/DLSS_Programming_Guide_Release.pdf
* https://raw.githubusercontent.com/NVIDIA-RTX/Streamline/main/docs/ProgrammingGuideDLSS.md
* https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_sampler_desc
* https://forums.developer.nvidia.com/t/jitter-issue-resolved-with-jitter-config-1/262909
* https://forums.developer.nvidia.com/t/dlss-jitter-debug/361140

## Qualification

The complete Release build passed. Both standalone AF hardware modes passed
after the final runtime edits. Full200case CTest results follow after completion;
do not treat the tests as a live SE5 visual or performance confirmation.
