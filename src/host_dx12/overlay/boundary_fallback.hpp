#pragma once
#include <cstdint>

namespace na::overlay {

// Decides when the overlay must draw from Streamline's game-facing Present
// (the SL proxy hook in sl_boundary) instead of the native DXGI Present detour.
//
// Normally a game Present into SL's proxy reaches the native swap chain's
// Present, where the overlay draws. Ghost of Tsushima (f46adb7, 2026-10-31)
// presents thousands of frames through the proxy while the native detour never
// runs, so the menu had no frame to draw on. The policy watches the native
// detour's game-present counter at each real boundary Present: when it has not
// advanced across kMissesToEngage consecutive boundary frames, the boundary
// becomes the overlay's frame source for the rest of the session (sticky, so
// the menu never alternates between the two paths or draws twice).
//
// Caller serializes calls.
class BoundaryFallback {
public:
    static constexpr uint32_t kMissesToEngage = 3;

    // One real (non-TEST, non-retry) game Present seen at the boundary, before
    // its original runs. native_presents: the native detour's game-present
    // count right now. Returns true when the overlay draws this frame here.
    bool OnBoundaryPresent(uint64_t native_presents)
    {
        if (engaged_) return true;
        if (have_last_ && native_presents == last_native_) {
            if (++misses_ >= kMissesToEngage) engaged_ = true;
        } else {
            misses_ = 0;
        }
        last_native_ = native_presents;
        have_last_ = true;
        return engaged_;
    }
    bool Engaged() const { return engaged_; }
    uint32_t Misses() const { return misses_; }
    void Reset() { *this = BoundaryFallback{}; }

private:
    uint64_t last_native_ = 0;
    uint32_t misses_ = 0;
    bool have_last_ = false;
    bool engaged_ = false;
};

} // namespace na::overlay
