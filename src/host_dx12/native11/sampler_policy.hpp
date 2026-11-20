// Native DX11 sampler policy: DLAA texture detail (mip bias) and anisotropic
// filtering for a game rendering on its own D3D11 device. The rules are the
// DX12 host's (observe/producers.cpp: sampler_wants_bias, sampler_wants_aniso,
// biased, aniso_to), restated on D3D11's sampler fields, whose filter and
// address encodings are numerically the same as D3D12's:
//  - bias: standard reduction, anisotropic or linear minification, tiling
//    (WRAP / MIRROR / MIRROR_ONCE) on U or V, and mips (MaxLOD > 0). Screen-space
//    passes clamp and stay untouched. Cort's total rule: a non-zero target
//    replaces the game's own bias (native -0.5 or 0 -> -1); 0 = the original.
//  - AF: core/anisotropic_auto.hpp, the one rule both APIs share (world samplers
//    only: not comparison, not point, mips, WRAP/MIRROR; automatic by default).
//
// Portable C++17 (Linux-tested); the D3D11 layer copies the fields in and out.
#pragma once

#include "core/anisotropic_auto.hpp"

#include <cstdint>

namespace na::native11 {

struct SamplerFacts {
    uint32_t filter = 0;        // D3D11_FILTER
    uint32_t address_u = 1, address_v = 1; // D3D11_TEXTURE_ADDRESS_MODE (1 = WRAP)
    uint32_t max_anisotropy = 1;
    float mip_lod_bias = 0.0f;
    float max_lod = 3.402823466e+38f;
};

namespace sampler_bits {
constexpr uint32_t kAnisotropicBit = 0x40;   // D3D11_ANISOTROPIC_FILTERING_BIT
constexpr uint32_t kAnisotropic = 0x55;      // D3D11_FILTER_ANISOTROPIC
inline uint32_t Reduction(uint32_t f) { return (f >> 7) & 3u; } // 0 = standard
inline uint32_t MinFilter(uint32_t f) { return (f >> 4) & 3u; } // 1 = linear
inline bool Anisotropic(uint32_t f) { return (f & kAnisotropicBit) != 0; }
inline bool Tiles(uint32_t mode) { return mode == 1 || mode == 2 || mode == 5; } // WRAP, MIRROR, MIRROR_ONCE
} // namespace sampler_bits

inline bool WantsBias(const SamplerFacts& s)
{
    using namespace sampler_bits;
    if (Reduction(s.filter) != 0) return false;
    const bool aniso = Anisotropic(s.filter);
    const bool linear_min = MinFilter(s.filter) == 1;
    const bool surface = aniso || (linear_min && (Tiles(s.address_u) || Tiles(s.address_v)));
    return surface && s.max_lod > 0.0f;
}

inline af::Facts AfFacts(const SamplerFacts& s)
{
    af::Facts f;
    f.filter = s.filter;
    f.address_u = s.address_u;
    f.address_v = s.address_v;
    f.max_anisotropy = s.max_anisotropy;
    f.max_lod = s.max_lod;
    f.mip_lod_bias = s.mip_lod_bias;
    return f;
}
inline bool WantsAniso(const SamplerFacts& s) { return af::IsWorld(AfFacts(s)); }

inline float ClampLod(float v) { return v < -16.0f ? -16.0f : (v > 15.99f ? 15.99f : v); }

// The sampler as the policy wants it; returns true when anything changed.
// `bias_target` 0 keeps the game's bias; `af_action` (af::Action) kKeep keeps the game's filtering.
inline bool ApplyPolicy(SamplerFacts& s, float bias_target, uint32_t af_action)
{
    bool changed = false;
    if (bias_target != 0.0f && WantsBias(s)) {
        const float b = ClampLod(bias_target);
        if (b != s.mip_lod_bias) { s.mip_lod_bias = b; changed = true; }
    }
    af::Facts f = AfFacts(s);
    if (af::Apply(f, af_action)) {
        s.filter = f.filter;
        s.max_anisotropy = f.max_anisotropy;
        changed = true;
    }
    return changed;
}

} // namespace na::native11
