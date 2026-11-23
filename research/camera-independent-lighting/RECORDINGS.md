# Camera-independent lighting recordings

Use one unpaused gameplay scene at fixed zoom, preferably with foreground cover and textured distant scenery. Each click closes the overlay. Begin the movement immediately and continue for 15 seconds. The recorder waits three seconds and then takes 30 evaluations. Keep the previous captures when repeating an incomplete condition.

| Recording | Sniper Elite Resistance and Sniper Elite 4 | Three Kingdoms and Pharaoh on a flat strategy map |
| --- | --- | --- |
| Still | Stand still with the view fixed. | Hold the view still. |
| Turn 1 | Stand in place and look diagonally across the scene. Include a small vertical component. | Rotate the view across the map. Include tilt if the controls permit it. |
| Turn 2 | From the starting view, make another independent diagonal sweep in the other direction. | Return to the original view and rotate again in the other direction. |
| Slide | Inapplicable. | Move sideways across the map at fixed heading, pitch and zoom. |

The shooter package displays three buttons. The flat-map package adds Slide. There is one set of captures for all offline methods. Use actual gameplay for SE4. On a map that locks tilt, record the normal turn and let the report identify the missing axis.

Run RESULTS after quitting. Read lighting-tests.json in the timestamped results folder. It groups recordings by executable, build, PID and process start. Each condition needs its own complete 30 frames. Copies of the original raw planes stay in LocalAppData. A prior build's pan remains available as an unlabeled historical capture. A second recording is independent of the first, although both turn fits may use the same map slide for the depth offset.

The readout samples the delivered motion and depth on a grid and robustly fits the eleven-term screen motion polynomial. It assumes a centered perspective with square pixels. The present offline implementation handles registered, unjittered vectors. The later GPU coefficient feed, startup handover, remembered lens, FOV changes and GPU cost still need independent runtime validation. The 50-degree start is a diagnostic comparator in this test. The recorder does not apply it to the picture.

The report keeps a rotation-only focal estimate separate when the two axes agree but translation cannot constrain the depth offset. The lens checks are 1% relative focal error and 10% relative offset error with independent reference, 2% agreement between axes, and 1% focal repeatability. At an effectively zero reference offset, report absolute error and leave the relative verdict unscored. A reference-free repeat measures consistency only. The actual lighting acceptance still requires 99% signed precision, 99% signed recall and 99% of affected pixels within 1% strength per layer per frame. The 0.005-stop tolerance is printed alongside the strict strength score when registered reference and predicted layers are supplied to calibration.py --score. The capture report leaves shadow accuracy unscored until those layers have been evaluated.

The optical-flow model comes from Longuet-Higgins and Prazdny, *The interpretation of a moving retinal image* (1980). The polynomial coordinates and coefficient order match src/core/screen_motion.hpp. The captured field can already include motion reconstruction, so a good fit alone does not qualify the lighting.
