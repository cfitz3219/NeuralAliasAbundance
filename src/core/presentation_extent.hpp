#pragma once

#include <cstdint>

namespace na::presentation {

// A native picture fills one display axis, with the remaining pixels used as bars.
// This only qualifies a candidate. Its placement still needs screen evidence.
constexpr bool CenteredNativeRegion(uint32_t w, uint32_t h, uint32_t screen_w, uint32_t screen_h)
{
    if (!w || !h || w > screen_w || h > screen_h || (w == screen_w && h == screen_h)) return false;
    return (w == screen_w && uint64_t(h) * 2 >= screen_h) ||
           (h == screen_h && uint64_t(w) * 2 >= screen_w);
}

} // namespace na::presentation
