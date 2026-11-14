# WWII stationary tremor: evidence and bounded repair, 2026-11-14

Cort reported trembling on stationary objects with jitter both on and off, after testing a11c167. This repair retains preset M, the expanded-raster coordinate correction and 16-sample R2. It changes the shared optical-flow acceptance rule, not the camera path or the game's render settings. Game testing belongs to Cort.

## Saved game evidence

- Capture: `%LOCALAPPDATA%\NeuralAlias\captures\j2-20261114-024549-s2_sp64_ship-p23460`, 30 saved evaluations. Frames 123733–123743 are consecutive; the remaining saver gaps are not evidence of DLAA misses.
- Status: `C:\Users\Cort\NeuralAliasAbundance\local\j2-wwii-on12\status-23460.log`. During the stationary portion preceding the capture, camera delivery is continuous and the camera-cut count does not increase. At 02:45:24, :29 and :39 the log records zero jitter sent to DLSS. The toggle was not simply ignored.
- Every saved evaluation has reset=0, preset=13 (M), full 3440x1440 input/output, reverse depth, pixel motion scale (1,1), and unjittered-motion flag. The eye remains at (-79810.6016, -3259.38818, -318.447113) during the first eleven frames, with small orientation changes.
- Reconstructing motion independently from the recorded double matrices and depth gives a median difference of about 0.0001 pixel from camera-only regions of the saved motion texture. Reprojecting static timber depth into the preceding frame gives median normalized-depth errors around 1e-7. These results do not support another large-coordinate precision repair or changing the jitter amplitude/sign.
- The fused texture contains additional motion on static timber: 0.08–1.69% of a sampled timber region has residual magnitude above 0.26 pixel across the first ten consecutive pairs. Most additional motion lies on the weapon, where independent movement is expected. The correction must preserve that use of flow.
- A 3x3 image comparison shows some extra vectors align stationary beam pixels worse than camera-only. Qualification: the old native capture records the *DLAA input after optional AO/tone mapping*, whereas flow reads the scene before those effects. Those error counts are supporting evidence, not an exact replay of the acceptance shader. Likewise the old native metadata left flow.ran, flow.loosen and raster_jitter at default zero despite fused motion being present. They cannot be used as evidence flow was off.

Local analysis scripts and numerical outputs are retained in `C:\Users\Cort\NA-hoi4-boundary\local\wwii-tremor`. Plane decoding uses each recorded DXGI format and row pitch; the capture's final row has no trailing alignment padding. Image previews use a viewing transform only. The numerical motion/depth analyses use the original float data. Texture-patch translation estimates on nearly planar/low-contrast timber were ambiguous and were not used to alter the camera.

## Reproduced defect and change

Both flow acceptance shaders used:

`e_flow <= image_abs && e_flow <= max(.02, image_ratio * e_camera)`

Consequently e_camera=0 and e_flow=.015 admitted false movement. Temporal agreement could not reject a consistent wrong vector. This is particularly relevant to faint texture details and directionally ambiguous edges; it is not a failure to calculate Halton or R2.

The new rule requires flow to actually improve the image and meet the existing ratio:

`e_flow <= image_abs && e_flow < e_camera && e_flow <= image_ratio * e_camera`

The camera answer wins ties. The looseness slider retains its 0.5–1.0 ratio range, absolute-error range, texture/temporal settings and small-motion threshold. No global camera smoothing, increased motion cutoff, forced flow disable, game-name check or model substitution. AMD and NVIDIA flow paths use the same corrected policy. The change removes an absolute-error bypass; it does not claim to make all optical-flow estimates correct.

Native DX11 armed captures now fill the existing flow/raster/depth/cut metadata from the actual evaluation and include `source_color` when optional effects make the DLAA input differ from the source used by flow. This is capture-only; no new synchronous GPU readbacks or per-frame polling.

## Discriminating automated proof

`tests/host/j2_flow_test.cpp` adds a low-contrast, stationary surface and a persistent false one-pixel flow. It executes production encode, expansion, temporal acceptance and the motion arbiter on the RTX device. Before the fix, **340 sampled stationary points receive one-pixel motion**. After the fix, **zero do**. The same surface genuinely moving one pixel per frame retains **360 correct moving points**, unchanged by the correction. This demonstrates that the improvement does not come from disabling all small motion.

`tests/host/dlss_real_test.cpp --stationary` additionally runs actual NVIDIA preset-M DLAA for 64 frames per case with fixed pixels, fixed depth, zero jitter and continuous history. Alternating erroneous one-pixel vectors produce mean successive-frame change 0.007588648; correct zero vectors produce 0.000465106 (about 16.3 times less). This is a controlled synthetic pixel measurement, not a claim that WWII itself improved by that factor. The separate gate regression connects the false vector to the production defect; the real model test establishes that such a vector can cause jitter-independent image movement.

## Limits and separate findings

The supplied capture plus reproducer establish a real source of false stationary motion. They do not prove that every visible tremor, light shimmer or AO artifact has this one cause. The scene colour already has temporal shading changes before DLAA; this patch does not erase real lighting changes or apply a generic blur. Existing saved evidence cannot isolate pre-AO colour from post-AO colour in this capture. The separate pine-branch AO report is not silently treated as solved.

The later live status also records bursts of missing camera origin, including 02:58–03:00. Those are distinct from the continuously delivered stationary capture; no speculative camera-admission relaxation is included here. The jitter-off behaviour and capture metadata correction are documented so a future investigation does not rediscover an already disproven explanation.

## Verification and delivery

See the staged package's `AUTOMATED-TESTS.txt`, `AUTOMATED-TEST-DETAILS.txt`, `MOTION-REGRESSION-BEFORE.txt`, `MOTION-REGRESSION-AFTER.txt`, `STATIONARY-MODEL-TEST.txt` and `INSTALLER-CHECKS.txt`. Preserve the previous a11c167 package and the earlier fc0e6bd recovery package. Stage a new additive WWII package only after the focused suite passes, and update the existing Documents CMDs to it. Do not install or launch the game.

Completed focused suite: 12/12 pass, no skips, 64.34 seconds. Real AMD confirmed-raster fixture: stationary motion 0.0000 px, moving-camera error .0043 px. Real NGX preservation check: 0/262144 motion components modified by evaluation, maximum error 0. Variant checker: 21 rows, zero errors. The production motion gate is tested with a fake flow engine and real GPU processing; the AMD path is separately exercised with the real AMD engine, and the temporal pixel consequence uses the real NVIDIA model. Neither constitutes a new WWII visual run.
