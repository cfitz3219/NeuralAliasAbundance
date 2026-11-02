#pragma once
#include <array>
#include <stdexcept>

namespace neural_alias {
// P4-4 projection window. Same convention as jitter_projection: row-major storage,
// column vectors, D3D viewport (pixel y down), clip row 3 = w. Maps the matrix's clip
// space so its NDC [-1,1]^2 lands in the TOP-LEFT sx x sy fraction of the viewport:
// x' = sx*x + (sx-1)*w, y' = sy*y + (1-sy)*w. Z and W are untouched (depth, reverse-Z and
// perspective are preserved). Apply after jitter: a jitter of j render pixels at the
// window's extent stays j native pixels, because one render pixel is one native pixel.
inline std::array<double, 16> window_projection(const std::array<double, 16>& m, double sx, double sy)
{
    if (!(sx > 0 && sx <= 1 && sy > 0 && sy <= 1)) throw std::invalid_argument("window fraction must be in (0,1]");
    auto r = m;
    for (unsigned c = 0; c < 4; ++c) {
        r[c] = sx * m[c] + (sx - 1.0) * m[12 + c];
        r[4 + c] = sy * m[4 + c] + (1.0 - sy) * m[12 + c];
    }
    return r;
}
} // namespace neural_alias
