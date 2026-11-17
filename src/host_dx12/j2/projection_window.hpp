#pragma once
#include <cstdint>
namespace na::j2 {
// P4-4 projection window, GPU cost: while J2's projection window is engaged, every draw
// recorded with the scene depth bound gets its scissor clipped to the top-left corner
// the scene is drawn into. Geometry the windowed camera places outside the corner (beyond
// the game's own view) is then never shaded. Draws without the scene depth (shadow maps,
// half-size effects, post-processing, the HUD) are untouched. The game's own scissor is
// restored as soon as the scene depth is unbound. Acts on render-target / scissor changes
// (not per draw); installed once at start-up; a no-op unless the window is engaged.
void InstallProjectionWindowScissor();
uint64_t ProjectionWindowScissored();       // game scissor sets clipped to the corner
uint64_t ProjectionWindowScissorRestored(); // game scissors put back after the scene depth was unbound
}
