#pragma once
// Coarse shading on D3D11 (the fold's shading saving, core/fold_shading.hpp, on the native DX11 route).
// D3D11 has no shading rate of its own; NVIDIA's driver interface (NVAPI) offers the same hardware
// feature for D3D11 on its cards. Measured on an RTX 5070 Ti (tests/host/vrs11_test.cpp):
//  - set on a context, it applies to every following draw until switched off: the game's
//    RSSetViewports, ClearState and render-target changes do not clear it, so it is switched off
//    explicitly before every draw that is not a scene draw;
//  - set inside a deferred recording, it is recorded into the command list; a deferred context starts
//    every recording at full rate again (FinishCommandList resets it);
//  - set on the immediate context, it also applies to a command list run there, so it is switched off
//    before every ExecuteCommandList;
//  - a coarse block is shaded at the block's centre, exactly as D3D12's coarse shading: the fold keeps
//    the same (last) pixel of each block on both routes.
#include <d3d11.h>
#include <cstdint>

namespace na::native11::coarse {

// Whether `device` can shade in blocks; `why` (plain words) when it cannot. Checked once per device.
bool Available(ID3D11Device* device, const char** why = nullptr);
// Shading rate on context `c` from its next draw: 0 = one per pixel, 4 = 2x1 blocks, 5 = 2x2 blocks
// (D3D12_SHADING_RATE numbering), for `viewports` viewports of a target up to w x h pixels.
bool Apply(ID3D11DeviceContext* c, ID3D11Device* device, uint32_t rate, uint32_t viewports, uint32_t w, uint32_t h);
void Release(); // the device has gone

} // namespace na::native11::coarse
