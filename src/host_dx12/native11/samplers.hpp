// Native DX11 route: DLAA texture detail (mip bias) and anisotropic filtering on
// the game's own D3D11 samplers, live from the existing menu state (no restart).
//
// The game's sampler states are recorded at CreateSamplerState. When the policy
// is on, each eligible sampler bound with PSSetSamplers / CSSetSamplers is
// swapped for a variant (same state, policy bias / AF) created on demand and
// kept for the process; when it is off, binds pass through untouched. Rules:
// sampler_policy.hpp (the DX12 host's rules on D3D11's fields).
#pragma once

#include <cstdint>

namespace na::native11 {

// The menu's wanted bias target (0 = game's own) and AF level (0 / 4 / 8 / 16), read on each bind.
using SamplerBiasFn = float (*)();
using SamplerAfFn = uint32_t (*)();

// Hooks the game's device (ID3D11Device*) CreateSamplerState and its contexts' PS/CS sampler binds.
// Idempotent. Install it when the device is created, before the game makes its samplers.
bool InstallSamplers(void* device, SamplerBiasFn bias, SamplerAfFn af);

struct SamplerStats {
    uint64_t created = 0;          // game CreateSamplerState calls seen
    uint64_t eligible_bias = 0;    // of the distinct game samplers: material samplers (bias rule)
    uint64_t eligible_af = 0;      // of the distinct game samplers: AF rule
    uint64_t variants = 0;         // variant sampler states we created
    uint64_t variant_failures = 0;
    uint64_t binds = 0;            // sampler slots bound while the policy was on
    uint64_t swapped = 0;          // of those, bound as our variant
    uint64_t unknown = 0;          // bound samplers we never saw created (passed through)
    float bias = 0.0f;             // policy applied at the last bind
    uint32_t af = 0;
    bool installed = false;
};
SamplerStats GetSamplerStats();
int FormatSamplerStatus(char* out, unsigned long n); // "native11_samplers{...}"

} // namespace na::native11
