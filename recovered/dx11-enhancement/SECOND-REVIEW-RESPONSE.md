# Response to the second source review — 22 September 2026

Please distinguish confirmed code behavior, evidence-backed interface expectations,
and predictions. This is still an experimental integration with unverified live
quality. Neither the positive architectural claims nor the catastrophic predictions
in the review are established by the available tests.

## Jitter

The two helpers perform different operations. `depth_addon.cpp::live_submit`
extracts jitter for DLAA. `dlaa_helper.cpp` assigns it to `InJitterOffsetX/Y`.
After successful DLAA and game post-processing, `alien_neural.cpp` constructs a
different metadata record for the final-picture neural pass. Its jitter fields
remain zero; the neural helper does not receive the original camera-jitter record.
The common struct's existence does not mean every backend consumes every field.

Forwarding the original sampling offset to a downstream reconstructed picture
is not automatically valid. No cited NR contract establishes that requirement.
Our raw motion/depth alignment with the final picture still needs temporal testing;
that is a real uncertainty, not proof of guaranteed shimmering.

## Color and exposure

The categorical claim that this NR runtime requires raw linear HDR conflicts with
the inspected integration reference, which explicitly describes finished sRGB input:
[OptiScaler menu source](https://github.com/Dagherbou/OptiScaler_DLSSNR/blob/973761621353b99bee3dc7d4bb27b117fef2644f/OptiScaler/dlssnr/DlssNr_Menu.cpp).
That is community implementation evidence, not an official NVIDIA numeric contract.
[NVIDIA research](https://research.nvidia.com/labs/adlr/DLSS5/) describes current
rendered color, engine motion, temporal state and artistic controls at inference;
it does not establish the review's asserted linear-HDR/exposure parameter contract.

The live path copies already tone-mapped SDR color, preserving encoded values
through a float-format conversion. The synthetic Reinhard/sRGB step belongs only
to the isolated replay preparation. Exposure/pre-exposure in this live NR metadata
are defaults of1, not captured dynamic exposure that was collected then discarded.
The displayed image already contains the game's exposure response. This does not
prove correct brightness or temporal adaptation in every scene; it means blindly
applying another exposure multiplier is not a justified repair.

We did identify an inherited `IsHDR` generic DLSS creation flag in our helper.
An isolated helper variant removed only that flag. Both saved1920x1200 frames
remained RGB-bit-identical to the original output, and both8-bit format tests
passed. This narrows the concern for this runtime/test input; it does not prove
the flag is universally ignored. Installed code was not changed during this audit.
Evidence: `docs/evidence/2026-09-22-neural-sdr-flag-test.log` in the project.

## Parameter and model evidence

A read-only ASCII string scan of the exact pinned DLL finds
`DLSSNR.Hint.Render.Preset`. It does not find the proposed jitter names
`DLSSNR.JitterOffsetX/Y`, `Jitter.Offset.X/Y`, or the ordinary DLSS exposure names
`DLSS.Pre.Exposure` and `DLSS.Exposure.Scale`.
String presence does not prove a parameter is used, and absence does not prove
there is no equivalent interface. It does undermine presenting invented/string-tag
model selection or those missing-key requirements as established facts.
Evidence: `docs/evidence/2026-09-22-neural-parameter-strings.json`.

Model requests0..3 producing identical bytes is an observation. Whether the hint
falls back, is ignored, selects equivalent content, or needs a different contract
is unresolved. We did not verify separate weights. STYLE is a separate control
whose values0/1/2 did produce different output. Do not conflate those controls.

## Valid risks, overstated outcomes

HUD/text inclusion, missing reliable engine camera-cut detection, uncertain final
color/motion/depth alignment, undocumented API reliance and unknown live performance
are valid outstanding issues. Severe/unplayable ghosting, guaranteed shimmer and
instant driver-update breakage have not been observed or demonstrated.

Reset handling includes first frame, nonconsecutive frame numbers, elapsed gaps
over250ms, display changes, neural settings/helper recreation, and DLAA setting
changes. That is more than timeout/resize, but still does not detect every cut.
HUD separation and reliable cut detection remain engineering work; they were not
accepted as proven harmless. Our own metadata layout is controlled by our two
processes; a driver cannot itself change that struct, though the external API can
be incompatible. The installer pins the DLL's hash but not future driver behavior.

Passing copy/fence tests does not prove no cross-API deadlock is possible. Parent
process waits govern CPU/helper lifetime; they do not guarantee a GPU/driver can
never hang. Our bounded waits and process cleanup reduce risk, not eliminate it.

Next useful review work is a concrete source-referenced synchronization defect or
a cited NR contract, plus in-game temporal/UI/performance observations. Changing
color space or adding guessed parameters based on confident prose alone would
not be a validated fix. No new game launch or gameplay observation in this audit.
