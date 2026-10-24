# A7 motion evidence — offline checkpoint

motion_checks.hpp tests candidate two-channel vectors by bilinear reprojection of
completed adjacent color samples, with explicit direction/scale and jitter convention.
Frames/samples retain session/epoch/frame identity and lifetime/completion evidence.
Independent stable visibility masks exclude disocclusion, changing transparency and
unavailable samples. Candidate-derived masks must never be used to hide errors.

Reports include checked/excluded/out-of-bounds counts, maximum and mean color error,
zero-motion baseline error and magnitude. Incorrect correspondences cannot be diluted
by a low mean. Sending an independently valid pixel off screen contradicts a candidate;
unavailable interpolation coverage keeps the result unavailable. Static/flat cases
remain ambiguous. Consistent means agreement with these sampled images, not proven
motion semantics: repeated textures, optical flow alternatives and masked regions
can be indistinguishable. No selected resource or DLSS activation is returned.

8 C++ cases cover correct/wrong sign/scale/channel order, both jitter conventions,
static/flat/empty masks, out-of-bounds evasion, one corrupted vector, stale epochs,
cuts, gaps, completion/lifetime, malformed/NaN data and lost visibility.
The fixture-v2 corpus evaluates160 hypotheses:30 correct warps at larger extents
agree,90 wrong-sign/scale cases contradict;40 hypotheses at32x24 remain unavailable
because too few stable pixels survive. The independent fixture mask excludes raster
interpolation across material/checker/glass boundaries. This is an oracle-assisted
content check, not a live visibility classifier or complete A7 acceptance.

Next: observed resource/format candidates from C8, qualified async images and
independent visibility; broader HDR/exposure and temporal consistency checks. Current
scope assumes matching extents and stable linear color, no unknown tone mapping.
