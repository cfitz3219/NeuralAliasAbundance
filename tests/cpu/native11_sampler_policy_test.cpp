// Native DX11 sampler policy (mip bias + AF): same eligibility and total-target
// rules as the DX12 host, on D3D11's encodings.
#include "test_support.hpp"
#include "host_dx12/native11/sampler_policy.hpp"

using na::native11::ApplyPolicy;
using na::native11::SamplerFacts;

namespace {
constexpr uint32_t kMinMagMipLinear = 0x15, kMinMagMipPoint = 0x00, kAniso = 0x55, kCmpLinear = 0x95, kMinLinearMagMipPoint = 0x10;
constexpr uint32_t kWrap = 1, kClamp = 3;
SamplerFacts S(uint32_t filter, uint32_t u, uint32_t v, float bias = 0.0f, uint32_t aniso = 1, float max_lod = 1000.0f)
{
    SamplerFacts s;
    s.filter = filter;
    s.address_u = u;
    s.address_v = v;
    s.mip_lod_bias = bias;
    s.max_anisotropy = aniso;
    s.max_lod = max_lod;
    return s;
}
} // namespace

int main()
{
    return test::run({
        {"a tiling trilinear material sampler gets the total target bias and 16x AF", [] {
            auto s = S(kMinMagMipLinear, kWrap, kWrap, -0.5f);
            CHECK(ApplyPolicy(s, -1.0f, 16));
            CHECK(s.mip_lod_bias == -1.0f);
            CHECK(s.filter == kAniso && s.max_anisotropy == 16);
        }},
        {"a clamped linear screen-space sampler gets AF but never the bias", [] {
            auto s = S(kMinMagMipLinear, kClamp, kClamp, 0.0f);
            CHECK(ApplyPolicy(s, -1.0f, 8));
            CHECK(s.mip_lod_bias == 0.0f);
            CHECK(s.filter == kAniso && s.max_anisotropy == 8);
        }},
        {"point, comparison and mip-less samplers are untouched", [] {
            auto p = S(kMinMagMipPoint, kWrap, kWrap);
            CHECK(!ApplyPolicy(p, -1.0f, 16));
            auto c = S(kCmpLinear, kWrap, kWrap);
            CHECK(!ApplyPolicy(c, -1.0f, 16));
            auto m = S(kMinMagMipLinear, kWrap, kWrap, 0.0f, 1, 0.0f);
            CHECK(!ApplyPolicy(m, -1.0f, 16));
        }},
        {"the game's own higher anisotropy is never lowered; target 0 keeps its bias", [] {
            auto s = S(kAniso, kWrap, kWrap, -0.25f, 16);
            CHECK(!ApplyPolicy(s, 0.0f, 8));
            CHECK(s.max_anisotropy == 16 && s.mip_lod_bias == -0.25f);
        }},
        {"linear minification with point mips still counts as a material (bias moves the mip)", [] {
            auto s = S(kMinLinearMagMipPoint, kWrap, kClamp);
            CHECK(ApplyPolicy(s, -1.0f, 0));
            CHECK(s.mip_lod_bias == -1.0f && s.filter == kMinLinearMagMipPoint);
        }},
    });
}
