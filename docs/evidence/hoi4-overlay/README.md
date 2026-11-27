# Overlay jitter evidence (jitter/scene-overlays)

Output of the DX11 overlay fixture (`tests/host/native11/provider_test.cpp --overlay-jitter*`), 1280x720.
The fixture draws thin lines and small labels with the depth test off, over depth-tested terrain, before the
interface. Each picture is a 3x crop with three panels: before (jitter/depth-tested-only), after (jitter/scene-overlays),
and a 16x supersampled reference of the same frame.

| Level | Edges the jitter never moves (before → after) | Mean error on lines and labels vs reference (before → after) |
|---|---|---|
| DLAA | 91.3 % → 0.21 % | 0.098 → 0.084 |
| Quality | 91.2 % → 0.02 % | 0.132 → 0.085 |
| Performance | 90.9 % → 0.00 % | 0.135 → 0.080 |

These pictures come from our fixture only. No crops of game frames are kept in the repository.
