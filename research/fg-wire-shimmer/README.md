# Resistance 2x frame generation and the ripple around wires

On November 26 Cort reported rings extending into the surface around thin lines, like heat rising off a road. The 02:33 Resistance results identify build `864d0823`. The first frame of the saved bunker sequence was present 19,253. Cort enabled 2x at 20,416. The last active sample counted 5,898 generated frames shown, eleven dropped and zero late. The saved frames supply the real scene for an offline reconstruction.

## Measured result

The prototype reduced broad luminance error roughly 47% in a strip 32-96 display pixels above and below the yellow cable. Each measurement compares with a real middle frame withheld from the generator. The five middle frames came from the first nine consecutive real frames of this capture. The two groups interleave, sharing source material. Five samples describe one scene.

| 32-96 pixels from the cable | Original 2x | With correction |
| --- | ---: | ---: |
| Mean absolute luminance error after a 9x9 spatial average | 1.375 | 0.730 |
| Pixels with a channel error above 12 display-proxy levels | 19.37% | 10.46% |
| Fine-detail contrast relative to the real middle frame | 84.9% | 89.9% |

This region is measured by vertical distance from the visible cable. The scorer uses signed, unclipped gamma 2.2 on linear RGB, scaled by 255. Its units are a comparison proxy. They are not nits or a perceptual visibility threshold. The cable and several other thin lines are visible against the textured ceiling. These measurements do not identify every live ring.

## How the shader works

`fg_static_midpoint.hlsl` runs on the generated world picture before the HUD paste. When nearby motion is below a display pixel and the two real HUD-free images agree about the local brightness pattern, the shader blends toward their linear-light midpoint. The weight falls to zero as either test fails. A moving edge can veto the correction through its neighbouring motion samples. The shader preserves the generated alpha and the signed FP16 range. It needs no game names, camera or player setting.

The RGB transfer used by the agreement test assumes linear scene colour, as in the tested FP16 path. Qualify SDR and other transfer functions separately. Restrict a first live trial to 2x and a verified pair of consecutive, same-size HUD-free frames. Invalidate the older image after a cut, pause, HUD reacquisition, resize, mode switch, missing guide or generation reset. Run the unmodified pipeline until the pair is valid. The shader expects motion in render pixels and scales it to display pixels. The live owner must decode the actual motion scale and subrect before dispatch. Preserve resource ownership until the queue fence passes. Hardware must support typed UAV reads for the output format.

## GPU trial

The D3D12 replay used Cort's RTX 5070 Ti, 3440x1440 linear RGBA16F, the camera-free generator, folded 1720x720 guides, and `nvngx_dlssg.dll` 310.9.16.0 from the tested build. The bunker trial used the game's saved pre-HUD sheet, reconstructed per channel using the paired real frame, and the production T2 HUD paste. Other scenes used the complete back buffer and a world-only scoring region. The two-frame motion input composed the recorded current-to-previous vectors and converted them to folded pixels. Consecutive present ordinals were checked for every scored triple. Holding out a frame doubles the interval between generator inputs.

| World sample | Scored middle frames | Broad error before | Broad error after |
| --- | ---: | ---: | ---: |
| Bunker cable strip, first group | 3 | 1.385 | 0.742 |
| Bunker cable strip, second group | 2 | 1.360 | 0.712 |
| Earlier still scene | 3 | 0.859 | 0.793 |
| Earlier turn | 3 | 4.559 | 4.559 |
| Second moving sequence | 2 | 2.385 | 2.385 |
| Fast flick | 3 | 13.449 | 13.447 |

GPU timestamps around the correction pass gave **0.171 ms median** across 16 dispatches, range **0.163-0.370 ms**. Previous-frame retention, any additional live copy, and presentation latency are outside these timestamps. The CPU makes no readback in the proposed live path. The GPU replay waited between samples to read its output.

Increasing motion-vector scale degraded fine detail in the near-still bunker. Changing only the jitter declaration to clip-space units produced the same scores on the two second-group frames. An unrestricted motion-compensated blend helped some stationary areas and introduced error during turns. The confidence tests keep it out of those regions. Fast-motion shimmer remains open.

## Recheck the numbers

The saved November 26 inputs are in `j2-20261126-023013-sniperresistance_dx12-p32724`. Use real indices 0, 2, 4, 6, 8 as input on one replay and 1, 3, 5, 7 on the second. Score withheld indices 3, 5, 7 and 4, 6 respectively. The first pair on each replay warms the generator. Read back generated output in tightly packed RGBA16F before any display clamping. The reference game capture uses its recorded row pitch.

Run `python measure.py CAPTURE TRUTH_INDEX ORIGINAL_FP16 CORRECTED_FP16 --bunker-wire-band` with NumPy installed. Other scenes accept `--roi X Y RIGHT BOTTOM`. Compare individual triples and average their results. A replay or capture must supply both generated images.

The next discriminating live capture is a tagged previous-real, generated, next-real sequence from the presenter's final output. Record the frame IDs and present result for each copy. The game-side capture currently used in this study only supplies the source pictures. A stationary view followed by a pan across the same cable distinguishes this correction's two operating cases.
