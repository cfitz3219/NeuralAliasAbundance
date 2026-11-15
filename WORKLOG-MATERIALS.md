# Material enhancement implementation journal

## 2026-11-14 Pacific — approved combined plan and first offline candidate

User authorized implementation after Fable's response, then reminded Astra to
respond in COMMS. Read central COMMS, MASTER-PLAN, CLAUDE, standing collaboration
rules and the proposal. Replied directly on main (39f4020) accepting the split,
with bounded corrections for source-byte lifetime, observed streamed mip ranges,
draw counts versus coverage, deferred recordings and GPU retirement. Fable owns
runtime binding/publication and independent colour-upscale baseline; Astra owns
coordinated detail, shared cache/record, mip filtering and common comparison.

New branch astra/material-preparation from 2c2e761 preserves shared 502f6bf and
WWII fixes without editing game code. Shared header and manifest contract
committed/pushed as e8cbd44; COMMS handoff 7ada0cb lets Fable proceed immediately.
No work in his runtime files, new game hook, game-file change or deployment.

Implemented `tools/material_lab`: six original procedural wood/masonry/fabric
inputs, explicit family/conventions, fixed-seed jointly derived colour/normal/
roughness detail, linear-light colour mips and a normal-moment roughness
approximation. Texture dimensions are preserved. Families are declared, not
automatically classified; three held-out fixtures reserve parameter checks,
not a trained model evaluation. This candidate is mathematics, not AI.

Preparation snapshots source bytes once for both decoding and identity; a test
updates the file mid-preparation and verifies the generated set still names the
bytes actually decoded. Immutable recipe cache records every map's source and
interpretation, BC format, complete mip layout and digest. Microsoft texconv
2026.5.8.1, may2026 release, SHA256
dcfdec10244e02cf5037fba089c55fb7e1326b1c8181742d77d15fa5cb5eef06, compresses already
filtered mips on CPU. Its MIT notice stays alongside the local binary; no
downloaded binary or source assets committed. Download helper verified locally.

Generated all six 256x256 original/coordinated sets. Each three-map set uses
218,520 bytes of BC payload with nine mip levels; these are small fixtures, not
runtime budget/performance evidence. First six-material preparation/demo took
25.61 seconds before static reference rendering was added. Subsequent preparation
used existing cache entries without rewriting them. Normal-map error on these
six BC5 sets: mean 0.751–0.949 degrees; 99th percentile 2.051–2.258 degrees.

The self-contained WebGL viewer uses the compressed-and-decoded cache, shared
camera/light, per-map isolation and actual mip levels. Colour is uploaded as
sRGB so filtering happens in linear light; normal/roughness stay linear. Texture
caching keys on the complete mip-array identity, not just the top-level image.
Until Fable supplies his baseline, the middle panel says colour-only control,
not AI. Baseline import verifies content plus source interpretation. Added a
separate offline reference renderer and six PNG comparisons; inspected oak and
sandstone visibly. New surface-light response remains present in normal-only
views. This is a synthetic visual demonstration, not a game quality verdict.

Validation: 20 offline tests PASS, including real BC7/BC5/BC4 encoder round trips,
exact strength-zero preservation, source mutation and mip behaviour. Portable
C++17 record compiles and runs under MSVC /W4 /WX. During test development one
test block was misplaced (undefined test-local variable), then corrected. A
too-tight 1-degree mean compression assertion failed at 1.117 degrees on the
32x32 fixture; declared the lossy BC5 angular budget as 2 degrees mean / 5 degrees
99th percentile. The production algorithm was not changed to satisfy that
assertion. Removed a redundant test of dictionary merging. Initial/final logs
are in local/material-tests-initial.txt and local/material-tests.txt.

Browser automation rejected file: URLs by policy; no alternate browser route or
policy workaround used. Interactive WebGL execution is therefore not claimed
verified. Source checks and offline decoded-payload renders are the available
evidence. No user action requested to unblock implementation; static PNGs can
be inspected independently and the HTML is provided for normal user opening.

Remaining: Fable's plain-upscale candidate, actual source-material census and
sampled-binding fixture/game delivery; qualitative choice with Cort. No trained
generator, atlas handling, arbitrary shader packing, runtime overhead result,
universal compatibility or completed in-game material enhancement is claimed.

Fable then replied with 7e4c41b/e4c6ca7, having fast-forwarded to shared e8cbd44.
Answered his source-field question with pushed 4f10137: optional resource format
and slice provenance, preserving older v1 defaults without rewriting hashed
recipes. Reviewed his actual texture module and posted COMMS f55ee3e: retain
reverse identity for old bound versions, mirror original context ownership,
and hold an explicit reference lease between registry lookup and the real bind.
Each finding includes a failing sequence and a concrete correction for Fable's
owned files. Confirmed API reference ownership in Microsoft's PSSet/Get docs.
No edits to Fable's module or game deployment. Node's JavaScript syntax check on
the viewer passes; that does not claim browser/WebGL execution was tested.

## 2026-11-14 Pacific — blank preview handoff corrected

Cort reported the lab could only be opened by pasting a path, then showed three
empty panels with no material buttons. Browser inventory (read only, no local
file navigation) established the exact open URL was the developer source
`C:/Users/Cort/NA-material-preparation/tools/material_lab/viewer.html`, not the
generated Documents gallery. That template still contained __MATERIAL_DATA__;
its JSON.parse failed before material buttons or rendering could start. This
identifies the screenshot's failure without blaming WebGL or the generator.

Renamed the internal template to viewer.html.in. The already-open source address
now serves a static launcher linking to the actual prepared gallery and three
existing PNG comparisons. Moved initialization errors above the panels and put
JSON parsing inside the startup error handler. Regenerated the real gallery and
copied it to the existing Documents package. Added a Windows desktop shortcut,
NeuralAlias Material Lab, directly targeting installed Chrome with the correct
prepared HTML. Read back its target/arguments; checked the file and identical
source/package hashes. No shortcut execution or browser file-navigation bypass.

Verified six materials, 324 embedded mip images, no unexpanded placeholder and
all launcher file links. Added a regression that decodes the generated viewer's
embedded PNG mip chains from real compressed cache inputs. This checks delivery
of images, not just script syntax. No material math, cache schema, game hooks or
game installation changed. Interactive WebGL rendering remains separately
unverified by automation; the user's screenshot was of the unpopulated template.

## 2026-11-14 Pacific — Fable handoff integrated: AI comparison and runtime-source intake

Cort said Fable had supplied what was needed. Read central COMMS and pulled the
actual handoffs: e3b220b's Real-ESRGAN/Lanczos indices, abf3394's shared serializer,
8b7bfbc's Windows narrowing correction and 4525b4e's actual DDS/index source dump.
Fast-forwarded onto that shared history rather than copying runtime modules.
No edits to Fable's hooks, producer, loader, or model executable.

The Lab now compares the actual six Real-ESRGAN x4plus results (1024-square colour)
with the same 256-square original sources and our coordinated candidate. Added
an optional Lanczos middle-panel selection, explicit model/dimensions and original
normal/roughness wording. A compare command updates existing entries without
regenerating source PNGs. All twelve external entries passed original source and
interpretation matching. Corrected offline reference renders: they previously
sampled only the highest mip and thus minified the larger AI map unfairly. Each
map now uses its own UV-derived trilinear mip footprint. The existing WebGL shader
already used implicit derivatives; it did not need a common integer LOD fix.

Visually inspected the updated brown-linen and painted-timber BC-decoded sheets.
Linen's AI colour result smooths the weave into diagonal bands; painted timber's
AI colour has extra fine vertical markings. Coordinated normal changes produce
larger lighting differences, especially under grazing light. These are synthetic
examples, not proof of superior real-game quality or a trained neural generator.
Six panels and the common viewer remain a comparison, not a selected final look.

Added runtime_source.py using Fable's texsub.digest unchanged. It consumes his
actual 4525b4e index.jsonl + DDS format directly and the earlier proposed raw-row
format for portable fixtures. Both yield the same GPU-content identity and cache
entry, with source view/range retained. Files are read once for hashing/decoding;
padding never enters identity. Roles and packing must be explicitly associated.
No automatic semantic material identification or capture hook is invented here.
Documented exact producer/consumer contract in MATERIAL-SOURCE-CAPTURE-V1.md.
Generated local/runtime-capture-example from our own oak fixture and prepared it
through the actual DDS/index intake into the immutable compressed cache.

Validation: 30 offline tests PASS including the shared digest vectors, padded
rows, altered texels, partial views, BGRA/BGRX, invalid layouts, raw-vs-DDS cache
identity, actual compression and different-resolution trilinear comparisons.
All 456 embedded images in the six-case generated gallery decode; the script
passes Node syntax check. C++ shared-vector test first failed MSVC /W4 /WX at
abf3394, reported to Fable; took his existing 8b7bfbc fix and the same nine cases
then passed on Windows. Reported separate malformed-header division/shift issue
for his owned serializer. Interactive browser execution remains unverified by
our automation; static reference renders were inspected. No game launch, install,
SDK/runtime replacement, in-game quality or performance claim.

## 2026-11-14 Pacific — five-choice AI comparison delivered

Cort requested all three shortlisted AI models and a direct selector. Claimed
this offline lane in central COMMS (61454f5), leaving Fable's runtime capture and
loader lane untouched. Based on his 6db3a8d shared history, added an optional
inference callback to his baseline preparer, a Spandrel model runner, strict
same-source model catalogue, selector and fixed-light contact sheets. Existing
Real-ESRGAN and Lanczos stay available. Camera/light and original normal/roughness
are preserved across model changes. Colour-hidden isolation modes explain it.

Downloaded PBRify V3 and both HAT weights from author links. Ran all three on the
six existing sources, without regenerating them: 18 actual RTX 5070 Ti results,
1024-square colour. Same wrap padding, source identity, alpha, linear mips and
BC7 compression. Weights/libraries/cache remain local, not in game or git.
Initial environment install failed due to overlapping pip processes; repaired
sequentially with pinned CUDA torch/vision, then installed Spandrel. CUDA and
pip dependency check passed. Exact hashes, provenance and limitations are in
docs/MATERIAL-MODEL-COMPARISON-20261114.md.

31 offline regressions PASS with real compression. Generated-gallery validation
PASS: six cases, five selections, 654 decoded mips, all five colour outputs
pixel-distinct per case. Node test of the actual controller and generated data
PASS: selected pixels, retained originals/normals, fixed camera/light, retained
model across materials and isolated-view explanation. Mock DOM/GL is not a
browser renderer. Inspected actual BC-decoded timber and linen contact sheets.
PBRify retains a much stronger linen weave; HAT variants differ, with sharper
adding timber markings. No universal winner or game-benefit claim.

Updated the EXISTING OneDrive Documents Lab and verified its HTML hash matches
3a7890e3f85535e225519d8fe4321f2719b5a7d49988a4cbd7ea9d64ebb91ff5. Existing desktop
Chrome shortcut still points there. Preserved previous HTML, added six model
contact sheets, updated Read Me and copied author notices/provenance/receipts.
No game launch, control, deployment or runtime edits in this increment.

## 2026-11-14 Pacific — PBRify plus trained surface models, original maps retained

Cort chose PBRify and requested a suitable existing material model, preservation
of original surface detail and a direct combined comparison. Claimed the offline
lane in COMMS 427aea9, leaving runtime acquisition/association/loader with Fable.
Selected the author's CC0 PBRify NormalV3 and RoughnessV2 SPAN models, also used by
NVIDIA's RTX Remix reference workflow. Downloaded the official 1.7.1 release and
verified weights/hashes; colour stays the existing preferred RPLKSRd V3. Actual
inference on all six unchanged Lab sources completed (twelve surface estimates)
on RTX 5070 Ti. Models/environment/predictions remain local.

Implemented fine residuals over original normal/roughness, bounded slope/roughness
additions and original-edge suppression. No procedural noise or replacement of
the entire authored surface. Exact PBRify colour DDS is shared byte-for-byte.
Reused original identity/cache, mip filtering, BC compression and publication.
The author's DirectX normal orientation matches the Lab's +V tangent direction;
no Y flip. Protection is a heuristic, not semantic recognition. Full details and
repeatable commands are in docs/MATERIAL-LEARNED-SURFACE-20261114.md.

Default page now shows Original / PBRify alone / Combined result, with a live
surface slider starting at 65%. At 0 it directly samples original surface maps,
not a resampled approximation, keeping the two arms identical. Old five-model
gallery is preserved as AI-model-comparison.html with a link from the new page.
Existing desktop shortcut and Documents entry point retained. New HTML SHA256
7eed792a81467bc11e0298096b16e94cd29b35136f50ed76e4ad389e4234d3db.

Validation: 37 Python tests PASS with actual texconv, no skips; actual-gallery
controller and legacy five-choice controller PASS using mock DOM/GL. All 360
embedded mip images decode. Six output colour DDS files exactly match PBRify;
18 actual-cache reference renders at zero strength exactly match the middle arm.
All six nonzero-strength outputs differ. Inspected linen/slate static renders:
surface changes are restrained, not a dramatic transformation. No claimed
superiority from tests. No interactive browser GPU render or real-game result.
The published surface maps are 1024 square; Fable's same-size-only runtime loader
must not accept them blindly. Its extent/format/role work remains separate.
No game runtime, game files, game testing or installation changed.

## 2026-11-14 Pacific — Surface slider visible-result correction

Cort reports Surface detail does nothing; explicitly requested tracing the loss,
preserving original material and updating the Lab. Claimed the offline lane in
central COMMS 940b8e1, also answered Fable's partial-mip question. Retained actual
PBRify predictions, model weights, colour output and original source files.
Measured filtering, clipping, protection and actual GPU output independently.
Found definite colour-only UI no-effect case; blanket edge suppression removes
80% of painted timber's remaining normal detail and 72% of slate's; narrow band
removes much roughness variation. Fine cloth detail minifies in a whole-tile view.

Recipe v2 keeps original maps and adds wider-band learned relief at a documented
fixed strength, with opposing-to-authored slope removed instead of all-edge
suppression. Still bounded, no random detail, no per-material automatic gain.
0% samples originals exactly; same colour DDS in both enhanced arms. Added shared
detail zoom, automatic light pause and return from colour-only when adjusting
surface strength. No other model, runtime path or game installation changes.

Used actual shipped GLSL and embedded maps on RTX 5070 Ti via optional standalone
ModernGL, not browser automation. All six cases at 1x/2x: 0% exactly matches the
colour-only image; 65/100% yield increasing signal. Whole-tile mean pixel changes
rise about 3–7x. Inspected actual GPU linen/timber endpoints; fabric visibly
changes, wood remains subtler. 38 Python tests with real compression pass; actual
gallery and legacy controller checks pass. Temporary UTF-8 helper failure caught
before delivery, restored original UTF-8 source and regenerated successfully.
Detailed evidence and thresholds: docs/MATERIAL-SURFACE-SLIDER-FIX-20261114.md.

Updated existing Documents Lab with previous default backed up, six actual-GPU
Slider PNGs, six updated static previews and current evidence/Read Me. New HTML
hash c3dedca0f873a7e7110a239b830bd537c3acf804e646b7aa9655cda966982aed. No claim
of browser UI automation, game-material quality, or in-game replacement testing.


## 2026-11-14 Pacific — visible material detail at ordinary viewing distance

Cort confirmed v2 looks good at 4x but loses its useful detail at 1x/2x and raised
GPU cost. Claimed offline preparation in central COMMS 60400fa; Fable keeps
runtime association/loader work. Inspected actual one-scale predictions, existing
normal-variance mip filtering and rendered v2. The old signal was concentrated
at fine texel scale. Do not mislabel existing filtering as absent, or equate
nonzero pixels with a satisfying visible result.

Added medium-scale inference using the SAME NormalV3/RoughnessV2 weights on
quarter-size linear-filtered PBRify colour. Twelve small offline inferences;
fine predictions reused exactly. Fixed-coordinate, band-limited medium detail
joins fine detail before the existing total cap and original-direction/mask
preservation. No source/material-colour change, new game code, new shader, new
texture sample, or extra output allocation. Existing mip approximation remains.
Recipe v3 immutable caches retain v1/v2. Viewer starts at whole-tile 1x.

41 Python tests with real BC compression pass, actual-gallery/legacy controller
checks pass, real RTX shader tests pass across six materials/three zooms/two
panel sizes plus three mip boundaries. Mean 1x pixel effect at 65% is 1.8–5.0x
v2 on 512-square panels; effect strength, not a perceptual score. Inspected wood,
stone and cloth at 1x/4x: medium relief survives normal distance and fine texture
remains. Exact zero and identical colour/originals/shaders/output byte budgets
are checked. Existing enhanced surface maps still cost more than originals:
2 MiB versus ~0.125 MiB per normal/roughness pair. This revision adds no further
map bytes or rendering samples; actual game timing is unmeasured.

Existing Documents Lab updated with old default saved as
Material-Lab-before-distance-detail.html; previous model comparison retained.
GPU Distance PNGs and detailed report supplied. HTML SHA256
6dcc35ddf16382509e58033fbd938cf396b0438d4b55b966be2d1aa0e3363dfa.
Full procedure/evidence: docs/MATERIAL-DISTANCE-DETAIL-20261114.md.
No game deployment or browser UI automation occurred. Tiny detail must still
filter naturally at distance; not all added medium relief is guaranteed to be
physically correct for an arbitrary material.


## 2026-11-14 Pacific — requested 2x fine-detail size trial

Cort confirms v3 is better, asks to double fine detail size before final selection.
Coordinated lane in COMMS dc9f4f4. Recipe v4 uses existing cached-inference path at
half size for fine normal/roughness predictions, resampling residuals back into
same UVs/output maps. Medium predictions remain byte-identical; gains and total
bounds unchanged. No texture crop or camera-dependent pattern. This doubles
model texel footprint, not every individual AI-predicted feature exactly.

43 Python tests pass with actual compression, including doubled feature spacing
and material-space alignment. Actual-gallery controller and six-material actual
RTX slider checks pass; previous/new 1x/4x comparisons rendered, wood/cloth
inspected. Verified identical original identity, PBRify colour bytes, medium
prediction paths, shader source and output sizes/bytes. No game tests/deployment.

Updated existing Lab with prior v3 backed up as
Material-Lab-before-fine-size-2x.html and explicit 2x-trial caption. Fine-size PNGs
show previous/current at identical light and strength. HTML hash
84f85850b2362158fb668a441c01d637ef5332b4421ce4b56aaf86a667461a49.
Fable's separate WWII capture handoff noted; signed BC5 and non-sRGB colour
qualification are not silently widened as part of this visual size trial.
