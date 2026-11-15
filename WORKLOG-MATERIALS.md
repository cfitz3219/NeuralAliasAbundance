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
