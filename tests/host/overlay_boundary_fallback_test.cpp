// Overlay frame source policy (Ghost of Tsushima, 2026-10-31): the overlay
// draws from Streamline's proxy Present only after the native DXGI Present
// detour has stayed silent across three consecutive proxy frames, and then
// stays there (never both, never alternating).
#include "test_support.hpp"
#include "host_dx12/overlay/boundary_fallback.hpp"

int main()
{
    using na::overlay::BoundaryFallback;

    // Normal game: each proxy Present reaches the native detour before the
    // next one, so the count advances between boundary frames: never engages.
    {
        BoundaryFallback p;
        uint64_t native = 0;
        for (int i = 0; i < 1000; ++i) {
            CHECK(!p.OnBoundaryPresent(native));
            ++native; // the native detour ran inside this frame's original Present
        }
        CHECK(!p.Engaged());
    }
    // Native detour bypassed from the first frame (Ghost): the first call only
    // records, then three silent intervals engage on the frame that saw the third.
    {
        BoundaryFallback p;
        CHECK(!p.OnBoundaryPresent(0));
        CHECK(!p.OnBoundaryPresent(0) && p.Misses() == 1);
        CHECK(!p.OnBoundaryPresent(0) && p.Misses() == 2);
        CHECK(p.OnBoundaryPresent(0) && p.Engaged());
        CHECK(p.OnBoundaryPresent(0));
        // Sticky: a native Present appearing later does not switch back.
        CHECK(p.OnBoundaryPresent(50));
    }
    // A native count that stalls briefly (loading, a frame-generation pacing
    // hiccup) and then resumes resets the miss run; the boundary does not engage.
    {
        BoundaryFallback p;
        CHECK(!p.OnBoundaryPresent(10));
        CHECK(!p.OnBoundaryPresent(10)); // miss 1
        CHECK(!p.OnBoundaryPresent(10)); // miss 2
        CHECK(!p.OnBoundaryPresent(11) && p.Misses() == 0);
        CHECK(!p.OnBoundaryPresent(12));
        CHECK(!p.Engaged());
    }
    // Native detour that worked and then went silent (bypass after a chain
    // change) engages the same way.
    {
        BoundaryFallback p;
        CHECK(!p.OnBoundaryPresent(1));
        CHECK(!p.OnBoundaryPresent(2));
        CHECK(!p.OnBoundaryPresent(2));
        CHECK(!p.OnBoundaryPresent(2));
        CHECK(p.OnBoundaryPresent(2));
        p.Reset();
        CHECK(!p.Engaged() && p.Misses() == 0);
        CHECK(!p.OnBoundaryPresent(2));
    }
    return test::finish("overlay_boundary_fallback_test");
}
