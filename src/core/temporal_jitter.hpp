#pragma once
#include <array>
#include <cstdint>
#include <stdexcept>

namespace neural_alias {
// Independent sampling math, applied by the verified Alien camera adapter.
// The adapter owns sequence advancement/reset and must select the main scene camera.
struct JitterSample { double x_pixels, y_pixels; };
inline double radical_inverse(uint32_t index, uint32_t base) {
    double result=0, weight=1;
    while(index) { weight/=base; result+=double(index%base)*weight; index/=base; }
    return result;
}
inline JitterSample temporal_jitter(uint32_t phase, uint32_t period) {
    if(period<2 || period>4096) throw std::invalid_argument("jitter period must be 2..4096");
    const uint32_t index=phase%period+1;
    return {radical_inverse(index,2)-0.5,radical_inverse(index,3)-0.5};
}
// Explicit convention: row-major storage, column-vector multiplication, D3D viewport.
// Pixel X points right and pixel Y down. Translate clip X/Y by W so displacement
// is constant after perspective division. Preserve clip Z/W, including reverse-Z.
// Caller must start from an UNJITTERED matrix, not apply this repeatedly to a result.
inline std::array<double,16> jitter_projection(const std::array<double,16>& unjittered,
    JitterSample sample,uint32_t width,uint32_t height) {
    if(!width || !height) throw std::invalid_argument("zero render extent");
    auto result=unjittered;
    const double dx=2*sample.x_pixels/double(width),dy=-2*sample.y_pixels/double(height);
    for(unsigned col=0;col<4;++col) {
        result[col]+=dx*unjittered[12+col];
        result[4+col]+=dy*unjittered[12+col];
    }
    return result;
}
} // namespace neural_alias
