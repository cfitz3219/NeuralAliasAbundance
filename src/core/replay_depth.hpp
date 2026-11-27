#pragma once
// Draw replay, both APIs (observe/producers.cpp on DX12, native11/draw_replay.cpp on DX11): which draws
// can be replayed by their depth use. A draw is replayed when it writes the scene depth, or when it is
// opaque geometry tested EQUAL / LESS_EQUAL / GREATER_EQUAL against a depth already written (a depth
// pre-pass: engines draw their G-buffer pass with depth writes off). Blended or other-test draws stay out.
//
// ONE rule for the DX12 recipe kept at pipeline creation, the DX12 per-draw gate and the DX11 per-draw
// gate. Before 2026-11-27 the DX12 recipe was kept only for depth-writing pipelines while the gate
// (a42171b, 2026-11-08) also admitted the depth-tested ones: those draws died as no_recipe (Sniper Elite
// Resistance: 12.39 M admitted, 12.14 M no recipe, about 710 a frame).
// The comparison numbering is the one D3D11 and D3D12 share (EQUAL 3, LESS_EQUAL 4, GREATER_EQUAL 7).
#include <cstdint>

namespace na::replay {

enum class DepthUse : uint8_t { No, Written, Matched };

constexpr DepthUse DepthUseOf(bool depth_enable, bool depth_write, bool blended, uint32_t depth_func)
{
    if (!depth_enable) return DepthUse::No;
    if (depth_write) return DepthUse::Written;
    if (blended) return DepthUse::No;
    return depth_func == 3 || depth_func == 4 || depth_func == 7 ? DepthUse::Matched : DepthUse::No;
}

} // namespace na::replay
