// Native DX11 viewport jitter: the offset rule and the scene-draw gate.
#include "test_support.hpp"
#include "host_dx12/native11/viewport_jitter.hpp"

using na::native11::JitterViewports;
using na::native11::ViewportRect;
using na::native11::WantsViewportJitter;

int main()
{
    return test::run({
        {"a full-target scene viewport is widened half a pixel each side, then moved by the sample", [] {
            ViewportRect v{0, 0, 3440, 1440};
            JitterViewports(&v, 1, 0.25f, -0.375f, 3440, 1440);
            CHECK(v.x == -0.25f && v.y == -0.875f);
            CHECK(v.w == 3441.f && v.h == 1441.f);
            // Centre moved by exactly the sample: the declared jitter is the applied one.
            CHECK(v.x + v.w * 0.5f == 1720.f + 0.25f);
            CHECK(v.y + v.h * 0.5f == 720.f - 0.375f);
        }},
        {"a partial viewport is only moved", [] {
            ViewportRect v{100, 50, 800, 600};
            JitterViewports(&v, 1, -0.5f, 0.5f, 3440, 1440);
            CHECK(v.x == 99.5f && v.y == 50.5f && v.w == 800.f && v.h == 600.f);
        }},
        {"several viewports all move by the same sample, none widened", [] {
            ViewportRect v[2]{{0, 0, 3440, 1440}, {0, 0, 1720, 720}};
            JitterViewports(v, 2, 0.125f, 0.25f, 3440, 1440);
            CHECK(v[0].x == 0.125f && v[0].w == 3440.f && v[1].y == 0.25f && v[1].w == 1720.f);
        }},
        {"only depth-tested draws into the scene depth are jittered", [] {
            CHECK(WantsViewportJitter(true, 7, 7, true, 1));
            CHECK(!WantsViewportJitter(false, 7, 7, true, 1)); // no sample this frame
            CHECK(!WantsViewportJitter(true, 8, 7, true, 1));  // shadow map / other depth
            CHECK(!WantsViewportJitter(true, 0, 7, true, 1));  // post pass, no depth
            CHECK(!WantsViewportJitter(true, 7, 7, false, 1)); // full-screen pass with depth test off
            CHECK(!WantsViewportJitter(true, 7, 7, true, 0));  // game viewports unknown
            CHECK(!WantsViewportJitter(true, 0, 0, true, 1));  // no scene selected
        }},
    });
}
