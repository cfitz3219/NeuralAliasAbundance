#pragma once

#include <dxgiformat.h>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace na::screen_marker {

inline constexpr float kAt[3][2]{{.30f, .62f}, {.70f, .62f}, {.50f, .38f}};
inline constexpr float kLow = .2f, kHigh = .8f;
inline constexpr uint32_t kSession = 8;
// VARIANT: referee/independent-region-code
// Each region visits high and low twice on each challenge parity. A common
// every-other-frame change in the game cannot stand in for these three codes.
inline constexpr uint32_t kPatterns[kSession]{0, 7, 5, 2, 3, 4, 6, 1};

struct Vote {
    uint32_t follow[3]{}, invert[3]{}, flat[3]{}, steps[3]{};

    void Add(uint32_t before, const float* previous, uint32_t after, const float* current)
    {
        for (int k = 0; k < 3; ++k) {
            const uint32_t bit = 1u << k;
            if (!((before ^ after) & bit)) continue;
            ++steps[k];
            const float direction = (after & bit) ? 1.f : -1.f;
            const float delta = current[k] - previous[k];
            if (!std::isfinite(delta)) continue;
            if (std::fabs(delta) < .02f) ++flat[k];
            else if (delta * direction > 0) ++follow[k];
            else ++invert[k];
        }
    }

    enum class Result { Unknown, Reached, Discarded };
    Result ResultOf() const
    {
        uint32_t reach = 0, still = 0;
        for (int k = 0; k < 3; ++k) {
            if (steps[k] < 4) continue;
            if (follow[k] == steps[k] || invert[k] == steps[k]) ++reach;
            if (flat[k] * 4 >= steps[k] * 3) ++still;
        }
        return reach >= 2 ? Result::Reached : still == 3 ? Result::Discarded : Result::Unknown;
    }
};

inline float Luma(const uint8_t* p,DXGI_FORMAT f){
    const auto half=[](uint16_t h){const uint32_t e=(h>>10)&31,m=h&1023;const float v=e==0?std::ldexp(float(m),-24):std::ldexp(float(m|1024),int(e)-25);return (h&0x8000)?-v:v;};
    float r=0,g2=0,b=0;
    switch(f){
    case DXGI_FORMAT_R16G16B16A16_FLOAT:case DXGI_FORMAT_R16G16B16A16_TYPELESS:{uint16_t t[4];std::memcpy(t,p,8);r=half(t[0]);g2=half(t[1]);b=half(t[2]);break;}
    case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:case DXGI_FORMAT_R8G8B8A8_TYPELESS:r=p[0]/255.f;g2=p[1]/255.f;b=p[2]/255.f;break;
    case DXGI_FORMAT_B8G8R8A8_UNORM:case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:case DXGI_FORMAT_B8G8R8A8_TYPELESS:b=p[0]/255.f;g2=p[1]/255.f;r=p[2]/255.f;break;
    case DXGI_FORMAT_R10G10B10A2_UNORM:case DXGI_FORMAT_R10G10B10A2_TYPELESS:{uint32_t v;std::memcpy(&v,p,4);r=(v&1023)/1023.f;g2=((v>>10)&1023)/1023.f;b=((v>>20)&1023)/1023.f;break;}
    default:return -1.f;
    }
    return .2126f*r+.7152f*g2+.0722f*b;
}
} // namespace na::screen_marker
