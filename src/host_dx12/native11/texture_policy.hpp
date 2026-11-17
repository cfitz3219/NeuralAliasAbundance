// Native DX11 material textures: which of the game's textures can take a prepared replacement, and
// why not when they cannot. Decided from what the texture and its view are, never from a game name
// or a register number. A texture passing here is a CANDIDATE: its role in a material (colour,
// normal, roughness, packed data) still has to be shown by how the game's shaders use it.
//
// Portable C++17 (Linux-tested); the D3D11 layer copies the fields in.
#pragma once

#include <cstdint>

namespace na::native11 {

struct MaterialTextureFacts {
    bool texture2d = false;     // the view is of a 2D texture (not a buffer, 1D, 3D)
    bool cube = false;          // D3D11_RESOURCE_MISC_TEXTURECUBE, or a cube view
    uint32_t width = 0, height = 0, mips = 0, array_size = 1, samples = 1;
    uint32_t format = 0;        // the texture's own DXGI format (may be typeless)
    uint32_t view_format = 0;   // the view's typed DXGI format
    uint32_t view_first_mip = 0, view_mips = 0; // the view's mip range, resolved (never ~0)
    uint32_t bind_flags = 0;    // D3D11_BIND_*
    uint32_t usage = 0;         // D3D11_USAGE: 0 default, 1 immutable, 2 dynamic, 3 staging
};

enum class TextureVerdict : uint8_t {
    Candidate,       // may take a replacement once its content and role are known
    NotTexture2D,    // a buffer, 1D or 3D view
    Cube,            // environment and sky maps: six faces, not a surface material
    Array,           // several slices (atlas arrays, terrain layers): no single-slice contract yet
    Multisampled,
    RenderedTo,      // the game draws or computes into it: its content is made per frame
    Rewritten,       // dynamic or staging usage: the game rewrites it (video, UI, streaming scratch)
    TooSmall,        // under 64 texels on a side: lookup tables, noise, icons
    NoMips,          // one level only: screen-space data, lookup or UI art, not a mip-filtered surface
    PartialView,     // the view shows only part of the mip chain (streaming window): range not final
    DataFormat,      // a format surfaces are not stored in (float, integer, depth, single 8/16-bit)
    kCount
};

inline const char* Name(TextureVerdict v)
{
    switch (v) {
    case TextureVerdict::Candidate: return "candidate";
    case TextureVerdict::NotTexture2D: return "not_2d";
    case TextureVerdict::Cube: return "cube";
    case TextureVerdict::Array: return "array";
    case TextureVerdict::Multisampled: return "multisampled";
    case TextureVerdict::RenderedTo: return "rendered_to";
    case TextureVerdict::Rewritten: return "rewritten";
    case TextureVerdict::TooSmall: return "too_small";
    case TextureVerdict::NoMips: return "no_mips";
    case TextureVerdict::PartialView: return "partial_view";
    case TextureVerdict::DataFormat: return "data_format";
    default: return "?";
    }
}

// What a candidate's format can hold. Says nothing yet about what the game stores in it.
enum class TextureClass : uint8_t {
    None,
    ColourSrgb,   // an sRGB view: authored colour
    FourChannel,  // BC1/2/3/7 or 8-bit RGBA/BGRA read as stored: colour, or packed masks, or a normal
    TwoChannel,   // BC5: almost always a tangent normal's X and Y
    OneChannel,   // BC4: a single mask (roughness, height, occlusion)
};

inline const char* Name(TextureClass c)
{
    switch (c) {
    case TextureClass::ColourSrgb: return "colour_srgb";
    case TextureClass::FourChannel: return "four_channel";
    case TextureClass::TwoChannel: return "two_channel";
    case TextureClass::OneChannel: return "one_channel";
    default: return "none";
    }
}

// DXGI_FORMAT values used below (dxgiformat.h).
inline TextureClass ClassOf(uint32_t view_format)
{
    switch (view_format) {
    case 29:  // R8G8B8A8_UNORM_SRGB
    case 72:  // BC1_UNORM_SRGB
    case 75:  // BC2_UNORM_SRGB
    case 78:  // BC3_UNORM_SRGB
    case 91:  // B8G8R8A8_UNORM_SRGB
    case 93:  // B8G8R8X8_UNORM_SRGB
    case 99:  // BC7_UNORM_SRGB
        return TextureClass::ColourSrgb;
    case 28:  // R8G8B8A8_UNORM
    case 71:  // BC1_UNORM
    case 74:  // BC2_UNORM
    case 77:  // BC3_UNORM
    case 87:  // B8G8R8A8_UNORM
    case 88:  // B8G8R8X8_UNORM
    case 98:  // BC7_UNORM
        return TextureClass::FourChannel;
    case 83:  // BC5_UNORM
    case 84:  // BC5_SNORM
        return TextureClass::TwoChannel;
    case 80:  // BC4_UNORM
    case 81:  // BC4_SNORM
        return TextureClass::OneChannel;
    default:
        return TextureClass::None;
    }
}

inline constexpr uint32_t kBindRenderTarget = 0x20, kBindDepthStencil = 0x40, kBindUnorderedAccess = 0x80;
inline constexpr uint32_t kMinMaterialSide = 64;

// The first reason that applies, in the order a reader would ask them.
inline TextureVerdict Judge(const MaterialTextureFacts& t)
{
    if (!t.texture2d) return TextureVerdict::NotTexture2D;
    if (t.cube) return TextureVerdict::Cube;
    if (t.array_size > 1) return TextureVerdict::Array;
    if (t.samples > 1) return TextureVerdict::Multisampled;
    if (t.bind_flags & (kBindRenderTarget | kBindDepthStencil | kBindUnorderedAccess)) return TextureVerdict::RenderedTo;
    if (t.usage >= 2) return TextureVerdict::Rewritten;
    if (ClassOf(t.view_format) == TextureClass::None) return TextureVerdict::DataFormat;
    if (t.width < kMinMaterialSide || t.height < kMinMaterialSide) return TextureVerdict::TooSmall;
    if (t.mips < 2) return TextureVerdict::NoMips;
    if (t.view_first_mip != 0 || t.view_mips != t.mips) return TextureVerdict::PartialView;
    return TextureVerdict::Candidate;
}

// Bytes of a full mip chain at `width` x `height` in `view_format` (block formats: 4x4 blocks).
inline uint64_t ChainBytes(uint32_t width, uint32_t height, uint32_t mips, uint32_t view_format)
{
    uint32_t block_bytes = 0, texel_bytes = 0;
    switch (view_format) {
    case 71: case 72: case 80: case 81: block_bytes = 8; break;              // BC1, BC4
    case 74: case 75: case 77: case 78: case 83: case 84: case 98: case 99: block_bytes = 16; break; // BC2, BC3, BC5, BC7
    default: texel_bytes = 4; break;
    }
    uint64_t total = 0;
    for (uint32_t m = 0; m < mips; ++m) {
        const uint64_t w = (width >> m) ? (width >> m) : 1, h = (height >> m) ? (height >> m) : 1;
        total += block_bytes ? ((w + 3) / 4) * ((h + 3) / 4) * block_bytes : w * h * texel_bytes;
    }
    return total;
}

} // namespace na::native11
