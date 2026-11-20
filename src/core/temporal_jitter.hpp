#pragma once
#include <array>
#include <cstdint>
#include <stdexcept>

namespace neural_alias {
// Independent sampling math, applied by the verified Alien camera adapter.
// The adapter owns sequence advancement/reset and must select the main scene camera.
struct JitterSample { double x_pixels, y_pixels; };
// The overlay owns this session-only transform. NVIDIA's F9/F10 hotkeys
// have no reported state we can reliably return to zero.
struct JitterDiagnostic {
    struct Scale { float x, y; };
    struct Configuration { float x, y; const char* name; const char* description; };
    inline static constexpr Configuration scales[] = {
        {1, 1, "Normal jitter", "DLAA gets the same tiny shift the game drew. Use this for playing."},
        {.5f, .5f, "Half the distance", "DLAA is told the picture moved only half as far as it did."},
        {.5f, -.5f, "Half the distance, up and down reversed", "DLAA gets half the real movement. Up counts as down and down counts as up."},
        {-.5f, .5f, "Half the distance, left and right reversed", "DLAA gets half the real movement. Left counts as right and right counts as left."},
        {-.5f, -.5f, "Half the distance, opposite direction", "If the picture moved up and right, DLAA is told it moved down and left, half as far."},
        {2, 2, "Twice the distance", "DLAA is told the picture moved twice as far as it did. This can make distant edges shake."},
        {2, -2, "Twice the distance, up and down reversed", "DLAA gets twice the real movement. Up counts as down and down counts as up."},
        {-2, 2, "Twice the distance, left and right reversed", "DLAA gets twice the real movement. Left counts as right and right counts as left."},
        {-2, -2, "Twice the distance, opposite direction", "If the picture moved up and right, DLAA is told it moved down and left, twice as far."},
        {1, -1, "Reverse up and down", "DLAA reads an upward shift as downward and a downward shift as upward. Left and right stay normal."},
        {-1, 1, "Reverse left and right", "DLAA reads a leftward shift as rightward and a rightward shift as leftward. Up and down stay normal."},
        {-1, -1, "Reverse every direction", "DLAA gets the right distance in the opposite direction. A shift up and right becomes down and left."},
        {.5f, 1, "Smaller sideways steps", "DLAA gets half the real left or right movement. Up and down stay normal."},
        {1, .5f, "Smaller up and down steps", "DLAA gets half the real up or down movement. Left and right stay normal."},
        {-.5f, 1, "Smaller sideways steps in reverse", "DLAA reads a shift to the right as a half-sized shift to the left, and vice versa. Up and down stay normal."},
        {1, -.5f, "Smaller up and down steps in reverse", "DLAA reads a shift up as a half-sized shift down, and vice versa. Left and right stay normal."},
        {2, 1, "Larger sideways steps", "DLAA gets twice the real left or right movement. Up and down stay normal."},
        {1, 2, "Larger up and down steps", "DLAA gets twice the real up or down movement. Left and right stay normal."},
        {-2, 1, "Larger sideways steps in reverse", "DLAA reads a shift to the right as a shift twice as far to the left, and vice versa. Up and down stay normal."},
        {1, -2, "Larger up and down steps in reverse", "DLAA reads a shift up as a shift twice as far down, and vice versa. Left and right stay normal."}
    };
    static constexpr uint32_t count = sizeof(scales) / sizeof(scales[0]);
    uint32_t value = 0; // first count = normal axes, next count = swapped
    constexpr uint32_t Config() const { return value % count; }
    constexpr bool Swapped() const { return value >= count; }
    constexpr uint32_t WithConfig(uint32_t config) const { return (Swapped() ? count : 0) + config % count; }
    constexpr uint32_t ToggleAxes() const { return Config() + (Swapped() ? 0 : count); }
    constexpr Scale Declared(float x, float y) const {
        const auto scale = scales[Config()];
        const float sx = x * scale.x, sy = y * scale.y;
        return Swapped() ? Scale{sy, sx} : Scale{sx, sy};
    }
};
enum class JitterMode : uint8_t { Unset, Unshifted, Shifted };

// A recorded frame can still carry the previous setting. The history changes
// mode only on an evaluated frame whose raster agrees with the new request.
constexpr JitterMode JitterHistoryForFrame(JitterMode previous, bool requested, bool raster_shifted,
                                          JitterDiagnostic diagnostic = {}) {
    if (requested != raster_shifted) return previous;
    // Each declared transform has its own shifted history on this view.
    return raster_shifted ? static_cast<JitterMode>(static_cast<unsigned>(JitterMode::Shifted) + diagnostic.value)
                          : JitterMode::Unshifted;
}
constexpr bool JitterHistoryChanged(JitterMode previous, JitterMode next) {
    return previous != JitterMode::Unset && next != JitterMode::Unset && previous != next;
}
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
// Martin Roberts' R2 low-discrepancy sequence (plastic constant), on the same cycle as
// temporal_jitter (index wraps at period, so every consumer that assumes a repeating
// pattern keeps working): x = frac(0.5 + n*a1) - 0.5, y = frac(0.5 + n*a2) - 0.5.
inline JitterSample temporal_jitter_r2(uint32_t phase, uint32_t period) {
    if(period<2 || period>4096) throw std::invalid_argument("jitter period must be 2..4096");
    const double a1=0.7548776662466927, a2=0.5698402909980532;
    const double n=double(phase%period+1);
    const auto frac=[](double v){ return v-double(static_cast<int64_t>(v)); };
    return {frac(0.5+n*a1)-0.5,frac(0.5+n*a2)-0.5};
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
