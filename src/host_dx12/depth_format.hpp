// Shared depth-format rule (Argon audit #2, 2026-10). Frame Generation, Neural
// Rendering and live motion copy the game's depth PLANE 0 into an R32_FLOAT
// texture with CopyTextureRegion. That copy is only valid when plane 0 is a
// 32-bit float layout. 24-bit (D24_UNORM_S8_UINT family) and 16-bit (D16_UNORM
// family) depth would be an invalid copy (undefined behaviour, typically device
// removal), so callers must check here first and decline with a plain reason.
// Converting 24/16-bit depth (compute pass reading an R24_UNORM_X8 / R16_UNORM
// SRV into R32_FLOAT) is planned work, see MASTER-PLAN "Audit notes".
#pragma once

#include <dxgiformat.h>

namespace na::depth {

inline bool PlaneZeroCopiesToR32F(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:      // D32S8: plane 0 is 32-bit float
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        return true;
    default:
        return false;                        // D24S8, D16 and anything unknown: not copyable as R32F
    }
}

// Short name for user-facing reasons ("24-bit", "16-bit", "unknown").
inline const char* DepthKind(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
        return "24-bit";
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_UNORM:
        return "16-bit";
    default:
        return PlaneZeroCopiesToR32F(f) ? "32-bit" : "unrecognised";
    }
}

} // namespace na::depth
