# Stronger contact shadows and curved lighting

Cort rejected the first contact-shadow study by eye. Its largest frame mean was 0.000236 stops over four captures. Staying under a ceiling tells us nothing about whether a player can see an improvement.

This is a separate offline candidate on the same PR. The original peer-matching study remains reproducible. Nothing in a game package changes. Fable's NA1 is off in every comparison here.

## How the two layers work

At two screen-relative radii, compare directed device depth on opposite sides of each pixel. Opposing differences cancel the slope of a flat surface. Midpoint samples reject isolated depth steps, and the immediate neighborhood protects depth outlines. A signed bend that is consistent across several directions supports a lift or a darkening. Both scales respond to the frame's depth and tone range and registered on-screen motion. Valid timing inputs can reduce directional sampling and suspend the pass at the time limit. This measures visible shape in the screen image. World-space normals and physical light positions remain unmeasured.

Contact shadows can only darken a recess. An already-dark neighborhood reduces that request. Curved lighting can brighten the outward or darken the inward side, using the existing local light to reinforce its direction. These terms both see the original picture. They use the larger darkening where they coincide. Lower local contrast reduces distant relief to preserve atmospheric haze. The fog still imposes a limit on how prominent distant forms should become.

The provisional audition ceilings are **0.24 stops of mean darkening per frame and 0.82 stops per aligned local tile**, compared to the first study's 0.010 and 0.040. Cort owns the final settings. The broad-contrast ratio stays within 0.92–1.10. Near-black pixels are protected. Both layers at zero give exact identity. The instantaneous scene and tile budgets apply after combining the negative requests. The spatial pass uses no accumulated previous image.

## What Cort can see

We fixed frame zero in advance for all four scenes. Each proof was evaluated directly at the original 3440 × 1440. There are four PNGs per scene, with a common exposure and display transform. The viewer embeds high-quality copies with an instant A/B, 100% zoom, contact alone, curved lighting alone, and both together. The static crops show the same central half of both pictures. The animated WebP changes lighting on a still frame every 1.8 seconds. Local output is under local/lighting-strength-study/.

| Capture | Scene pixels changed at least 0.25 stops by contact alone | By both layers | Whole displayed frame with luminance difference at least 8 of 255, both layers |
| --- | ---: | ---: | ---: |
| Sniper Elite Resistance | 11.9% | 31.7% | 24.5% |
| Three Kingdoms | 28.6% | 45.9% | 44.0% |
| Pharaoh | 33.0% | 40.1% | 28.5% |
| Sniper Elite 4, menu backdrop | 27.8% | 53.8% | 46.4% |

The last column compares saved, display-mapped PNGs using Rec. 709 luma coefficients on 8-bit encoded channels. It includes all pixels, even sky. It measures the size of the displayed difference. It cannot predict preference.

## Verification

A separate run evaluated all four 30-frame bursts at 860 × 360. Across 120 frames, the largest mean darkening was 0.234311 stops and largest tile was 0.820000 stops (float precision). Near-black share was unchanged on every frame. Broad contrast ranged from 1.00468 to 1.09998. The fixed first-frame native exports were also checked for these budgets. Ten Python regressions passed across the earlier and new producers. They include constant depth, a painted flat surface, a sloped plane, an isolated slab, near black, sky, timing shutdown, motion response, separate dials, exact off and depth direction.

The earlier dilute result failed the visual brief. This stronger look is an audition. Foliage on the strategy map can read as individually lit clumps, and highly curved bright objects can look strongly modelled. Independent camera-reference scores, convincing player-turn evidence, stable live behavior, GPU cost, and live API parity are still open. The SE4 backdrop illustrates parked cars and nearby stonework. It is not moving-play evidence.

## Reproduce

From research/contact-shadows, with NumPy and Pillow available:

    python -m unittest test_form_lighting test_contact_shadow
    python form_study.py replay <capture-folder> <output-folder> --count 30 --step 4
    python form_study.py proofs <capture-folder> <output-folder> --frame 0
    python compare_forms.py <output-folder> "<scene title>"

The source stays on the shared lighting branch. The installer and recorder remain at d9106460. No attendance is needed to view the proofs. A real player-turn evaluation reuses the existing Resistance still and two turn recordings when available.
