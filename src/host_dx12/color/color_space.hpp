// A60: one answer to "what kind of picture is in this swap chain?" shared by
// Neural Rendering, motion analysis, Frame Generation and the presenter.
//
// DX12 games with DLSS present in one of these encodings:
//   SDR      8-bit (R8G8B8A8 / B8G8R8A8, sRGB display-encoded) or, rarely,
//            10-bit with the SDR colour space (G22 P709);
//   HDR10    R10G10B10A2 with the PQ colour space (G2084 P2020);
//   scRGB    R16G16B16A16_FLOAT with the linear colour space (G10 P709),
//            1.0 = 80 nits, values above 1 and below 0 allowed.
// The colour space the game set on its chain (IDXGISwapChain3::SetColorSpace1)
// decides; when the game never set one (DXGI_COLOR_SPACE_CUSTOM) the format
// decides: fp16 is taken as scRGB (an SDR fp16 chain is practically unheard
// of), 10-bit is taken as SDR (treating an SDR 10-bit picture as PQ would be
// far worse than the reverse), 8-bit is SDR.
#pragma once

#include <dxgi1_4.h>

#include <cstdint>

namespace na::color {

enum class Encoding : uint32_t {
    SDR = 0,     // display-encoded (sRGB-like), any 8-bit or 10-bit UNORM chain
    HDR10 = 1,   // PQ, R10G10B10A2
    scRGB = 2,   // linear fp16, 1.0 = 80 nits
};

inline Encoding Classify(DXGI_FORMAT format, DXGI_COLOR_SPACE_TYPE color_space)
{
    bool fp16 = format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    bool ten = format == DXGI_FORMAT_R10G10B10A2_UNORM;
    switch (color_space) {
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
        return ten ? Encoding::HDR10 : (fp16 ? Encoding::scRGB : Encoding::SDR); // PQ needs 10-bit; fp16 with PQ set is treated as scRGB
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
        return fp16 ? Encoding::scRGB : Encoding::SDR;
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
    case DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709:
        return Encoding::SDR;
    default: // never set (CUSTOM) or something exotic: the format decides
        return fp16 ? Encoding::scRGB : Encoding::SDR;
    }
}

inline bool IsHdr(Encoding e) { return e != Encoding::SDR; }

// How the values STORED in a picture of `format` are encoded, for passes that read and write the
// presented picture itself (Frame Generation's still-area steadying and HUD paste). `hdr` is the
// presenter's answer from Classify. FP16 is always linear (scRGB scale): the display compositor
// reads FP16 chains as linear light whatever colour space is set, and this keeps the FP16 path
// exactly as it was proven. 10-bit is PQ when the chain is HDR, otherwise display-encoded SDR.
// 8-bit (and anything else) is display-encoded SDR.
inline Encoding ForStoredPicture(DXGI_FORMAT format, bool hdr)
{
    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R16G16B16A16_TYPELESS) return Encoding::scRGB;
    if ((format == DXGI_FORMAT_R10G10B10A2_UNORM || format == DXGI_FORMAT_R10G10B10A2_TYPELESS) && hdr) return Encoding::HDR10;
    return Encoding::SDR;
}

// Shader-side mode value shared by the passes (0 SDR, 1 PQ, 2 scRGB).
inline uint32_t ShaderMode(Encoding e) { return static_cast<uint32_t>(e); }

// The format a shader reads and writes a stored picture through: the plain UNORM/FLOAT member of
// its family (typeless and _SRGB chains read their stored values, which ForStoredPicture describes).
inline DXGI_FORMAT ViewFormat(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return f;
    }
}

inline const char* Name(Encoding e)
{
    switch (e) {
    case Encoding::HDR10: return "HDR10";
    case Encoding::scRGB: return "scRGB";
    default: return "SDR";
    }
}

} // namespace na::color
