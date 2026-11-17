// Native DX11 viewport jitter: the offset rule and the scene-draw gate.
#include "test_support.hpp"
#include "host_dx12/native11/viewport_jitter.hpp"
#include "providers/camera/camera_tracker.hpp"
#include <cmath>

using na::native11::JitterViewports;
using na::native11::ViewportRect;
using na::native11::WantsViewportJitter;
using na::native11::RasterViewProjection;

namespace {
using Vec = std::array<double, 4>;
Vec Transform(const na::camera::Matrix& m, const Vec& p)
{
    Vec q{};
    for (unsigned r = 0; r < 4; ++r)
        for (unsigned c = 0; c < 4; ++c) q[r] += m[r * 4 + c] * p[c];
    return q;
}
Vec Screen(const na::camera::Matrix& m, const Vec& p, const ViewportRect& v)
{
    const auto q = Transform(m, p);
    return {v.x + (q[0] / q[3] + 1) * v.w / 2,
            v.y + (1 - q[1] / q[3]) * v.h / 2, q[2] / q[3], 1};
}
}

int main()
{
    return test::run({
        {"a full-target scene viewport is widened half a pixel each side, then moved by the sample", [] {
            ViewportRect v{0, 0, 3440, 1440};
            CHECK(JitterViewports(&v, 1, 0.25f, -0.375f, 3440, 1440));
            CHECK(v.x == -0.25f && v.y == -0.875f);
            CHECK(v.w == 3441.f && v.h == 1441.f);
            // Centre moved by exactly the sample: the declared jitter is the applied one.
            CHECK(v.x + v.w * 0.5f == 1720.f + 0.25f);
            CHECK(v.y + v.h * 0.5f == 720.f - 0.375f);
        }},
        {"a partial viewport is only moved", [] {
            ViewportRect v{100, 50, 800, 600};
            CHECK(!JitterViewports(&v, 1, -0.5f, 0.5f, 3440, 1440));
            CHECK(v.x == 99.5f && v.y == 50.5f && v.w == 800.f && v.h == 600.f);
        }},
        {"several viewports all move by the same sample, none widened", [] {
            ViewportRect v[2]{{0, 0, 3440, 1440}, {0, 0, 1720, 720}};
            CHECK(!JitterViewports(v, 2, 0.125f, 0.25f, 3440, 1440));
            CHECK(v[0].x == 0.125f && v[0].w == 3440.f && v[1].y == 0.25f && v[1].w == 1720.f);
        }},
        {"raster projection matches off-centre pixels and reconstructs their depth", [] {
            for (const auto size : {std::array<uint32_t, 2>{64, 36}, {3440, 1440}}) {
                const auto w = size[0], h = size[1];
                for (bool reverse : {false, true}) {
                    const na::camera::Matrix vp{1.2,0,.03,0, 0,2.1,-.02,0,
                        0,0,reverse ? 0.0 : 1.0,reverse ? .25 : -.25, 0,0,1,0};
                    const auto adjusted = RasterViewProjection(vp, true, w, h);
                    CHECK(RasterViewProjection(vp, false, w, h) == vp);
                    na::camera::Matrix inv{};
                    CHECK(na::camera::Invert(adjusted, inv));
                    for (unsigned i = 0; i < 16; ++i) {
                        const auto j = na::native11::DlaaJitterSample(i, true);
                        const float dx = float(j.x_pixels), dy = float(j.y_pixels);
                        ViewportRect shifted{0,0,float(w),float(h)};
                        CHECK(JitterViewports(&shifted, 1, dx, dy, w, h));
                        // Compute the raster position directly from the game's original
                        // projection and actual viewport, independently of the correction.
                        for (const Vec p : {Vec{-3,1,5,1}, Vec{2,-1,5,1}}) {
                            const auto drawn = Screen(vp, p, shifted);
                            const auto predicted = Screen(adjusted, p, {dx,dy,float(w),float(h)});
                            NEAR(drawn[0], predicted[0], 2e-4);
                            NEAR(drawn[1], predicted[1], 2e-4);
                            NEAR(drawn[2], predicted[2], 1e-12);
                            const Vec ndc{2*(drawn[0]-dx)/w-1, 1-2*(drawn[1]-dy)/h, drawn[2], 1};
                            const auto world = Transform(inv, ndc);
                            for (unsigned c = 0; c < 3; ++c) NEAR(world[c]/world[3], p[c], 2e-6);
                            // The old camera was detectably wrong away from screen centre.
                            const auto old = Screen(vp, p, {dx,dy,float(w),float(h)});
                            CHECK(std::fabs(old[0] - drawn[0]) > .2);
                        }
                    }
                }
            }
        }},
        {"tracked reprojection handles camera movement and jitter on/off footprints", [] {
            constexpr uint32_t w = 3440, h = 1440;
            const na::camera::Matrix base{1.2,0,0,0, 0,2.1,0,0, 0,0,0,.25, 0,0,1,0};
            na::camera::CameraTracker tracker;
            const double zero[2]{};
            na::camera::Matrix previous{};
            for (uint32_t frame = 0; frame < 20; ++frame) {
                auto vp = base;
                vp[3] = -.02 * frame; // known sideways camera translation
                const bool expanded = frame >= 2 && frame < 18;
                const auto effective = RasterViewProjection(vp, expanded, w, h);
                const auto cam = tracker.Update(effective, frame + 1, w, h, zero);
                CHECK(cam.valid);
                const Vec point{2,-1,5,1};
                const auto now = Screen(effective, point, {0,0,float(w),float(h)});
                if (frame) {
                    CHECK(!cam.camera_cut);
                    const auto old = Screen(previous, point, {0,0,float(w),float(h)});
                    const auto reproj = na::camera::TransposeMatrix(na::camera::ComposeReprojectionRow(
                        cam.inv_view_proj_current_row, cam.view_proj_previous_row));
                    const auto q = Transform(reproj, {2*now[0]/w-1, 1-2*now[1]/h, now[2], 1});
                    NEAR((q[0]/q[3]+1)*w/2, old[0], 1e-8);
                    NEAR((1-q[1]/q[3])*h/2, old[1], 1e-8);
                }
                previous = effective;
            }
        }},
        {"native R2 has sixteen distinct bounded samples and Halton still has eight", [] {
            for (unsigned i = 0; i < 16; ++i) {
                const auto a = na::native11::DlaaJitterSample(i, true);
                const auto wrap = na::native11::DlaaJitterSample(i + 16, true);
                CHECK(a.x_pixels >= -.5 && a.x_pixels < .5 && a.y_pixels >= -.5 && a.y_pixels < .5);
                CHECK(a.x_pixels == wrap.x_pixels && a.y_pixels == wrap.y_pixels);
                for (unsigned k = 0; k < i; ++k) {
                    const auto b = na::native11::DlaaJitterSample(k, true);
                    CHECK(a.x_pixels != b.x_pixels || a.y_pixels != b.y_pixels);
                }
                const auto halton = na::native11::DlaaJitterSample(i, false);
                const auto old = neural_alias::temporal_jitter(i, 8);
                CHECK(halton.x_pixels == old.x_pixels && halton.y_pixels == old.y_pixels);
            }
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
