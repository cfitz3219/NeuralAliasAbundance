// reconstructed 2026-10 — independent numerical truth and per-pixel provenance.
#include "test_support.hpp"
#include "core/motion_arbiter.hpp"
#include <limits>
using namespace na::motion;
namespace {
Mat4 identity() { Mat4 m{}; m.m[0] = m.m[5] = m.m[10] = m.m[15] = 1; return m; }
Tier2Input camera(const float* depth, uint32_t w = 1, uint32_t h = 1) {
    Tier2Input t{}; t.valid = true; t.depth = {depth, w, h, w};
    t.inv_view_proj_current = identity(); t.view_proj_previous = identity(); return t;
}
}
int main() { return test::run({
    {"camera reprojection direction and raster jitter", [] {
        float depth = .5f, x = 99, y = 99; bool off = true; auto t = camera(&depth);
        t.jitter_current_px[0] = .25f; t.jitter_current_px[1] = -.25f;
        CHECK(ReprojectPixel(t, 4, 4, 1, 1, depth, &x, &y, &off)); NEAR(x, 0, 1e-6); NEAR(y, 0, 1e-6);
        t.view_proj_previous.m[12] = .5f; t.view_proj_previous.m[13] = .5f;
        CHECK(ReprojectPixel(t, 4, 4, 1, 1, depth, &x, &y, &off)); NEAR(x, 1, 1e-6); NEAR(y, -1, 1e-6);
        CHECK(!off);
    }},
    {"invalid depth, reverse Z metadata and offscreen flags", [] {
        float depth = .25f, x = 0, y = 0; bool off = false; auto t = camera(&depth);
        for (float z : {-1.f, 1.f, 1.1f, std::numeric_limits<float>::quiet_NaN()})
            CHECK(!ReprojectPixel(t, 1, 1, 0, 0, z, &x, &y, &off));
        t.depth_inverted = true; t.depth_clear = 0;
        t.view_proj_previous.m[8] = 2; // x depends on RAW depth; a numeric depth flip would give .75.
        CHECK(ReprojectPixel(t, 1, 1, 0, 0, depth, &x, &y, &off)); NEAR(x, .25, 1e-6);
        CHECK(!ReprojectPixel(t, 1, 1, 0, 0, 0, &x, &y, &off));
        t.view_proj_previous.m[12] = 4;
        CHECK(ReprojectPixel(t, 1, 1, 0, 0, depth, &x, &y, &off)); CHECK(off);
        t.view_proj_previous.m[15] = -1; CHECK(!ReprojectPixel(t, 1, 1, 0, 0, depth, &x, &y, &off));
    }},
    {"engine priority and per-pixel validity fallbacks", [] {
        float engine[]{4,5, 6,7, 8,9}, valid[]{1,0,std::numeric_limits<float>::quiet_NaN()};
        float depths[]{.5f,.5f,1.f}, residual[]{2,0, 2,0, 2,0}, output[6]{}; uint8_t provenance[3]{};
        Tier1Input t1{true, {engine,3,1,6}, {valid,3,1,3}};
        Tier3Input t3{}; t3.valid = true; t3.residual = {residual,3,1,6};
        Output o{output, provenance, 3,1}; Stats s;
        CHECK(Arbitrate(t1, camera(depths,3), t3, {}, o, &s));
        CHECK(provenance[0] == NA_MOTION_ENGINE && provenance[1] == NA_MOTION_CAMERA_RESIDUAL && provenance[2] == NA_MOTION_INVALID);
        NEAR(output[0],4,0); NEAR(output[1],5,0); NEAR(output[2],2,1e-6); NEAR(output[4],0,0);
        CHECK(s.pixels_engine == 1 && s.pixels_camera == 1 && s.pixels_invalid == 1 && s.pixels_residual_added == 1);
    }},
    {"residual alone never becomes full motion", [] {
        float r[]{3,4}, out[]{99,99}; uint8_t provenance[]{255}; Tier3Input t{}; t.valid = true; t.residual = {r,1,1,2};
        Output o{out,provenance,1,1}; Stats s;
        CHECK(Arbitrate({}, {}, t, {}, o, &s)); CHECK(provenance[0] == NA_MOTION_INVALID && s.pixels_invalid == 1);
        NEAR(out[0],0,0); NEAR(out[1],0,0);
    }},
    {"snap/clamp preserve provenance and bounded residual", [] {
        float depth=.5f, r[]{.1f,0}, out[2]{}; uint8_t p[1]{}; Output o{out,p,1,1}; Stats s;
        Tier3Input t{}; t.valid=true; t.residual={r,1,1,2};
        CHECK(Arbitrate({}, camera(&depth), t, {.25f, 2}, o, &s));
        CHECK(p[0] == (NA_MOTION_CAMERA | NA_MOTION_F_FILTERED) && s.pixels_residual_snapped == 1);
        r[0]=3; r[1]=4; CHECK(Arbitrate({}, camera(&depth), t, {.25f, 2}, o, &s));
        NEAR(out[0],1.2,1e-6); NEAR(out[1],1.6,1e-6);
        CHECK(p[0] == (NA_MOTION_CAMERA_RESIDUAL | NA_MOTION_F_FILTERED) && s.pixels_residual_clamped == 1);
    }},
    {"malformed masks, extents and configuration", [] {
        float depth=.5f, r[]{1,2}, mask=2, out[2]{}; uint8_t p[1]{}; Output o{out,p,1,1}; Stats s;
        Tier3Input t{}; t.valid=true; t.residual={r,1,1,2}; t.mask={&mask,1,1,1};
        CHECK(Arbitrate({},camera(&depth),t,{},o,&s)); CHECK(p[0] == NA_MOTION_CAMERA && s.pixels_rejected == 1);
        t.mask={}; t.valid_mask={&mask,1,1,1}; CHECK(Arbitrate({},camera(&depth),t,{},o,&s)); CHECK(p[0] == NA_MOTION_CAMERA);
        t.residual.width=2; CHECK(Arbitrate({},camera(&depth),t,{},o,&s)); CHECK(s.pixels_rejected == 1);
        CHECK(!Arbitrate({}, {}, {}, {-1,2}, o, &s)); CHECK(!Arbitrate({}, {}, {}, {3,2}, o, &s));
        CHECK(!ValidateConfig({0,std::numeric_limits<float>::infinity()}));
        o.xy=nullptr; CHECK(!Arbitrate({}, {}, {}, {}, o, &s));
    }},
    {"residual derivation removes camera AND jitter difference", [] {
        float depth=.5f, raw[]{1.5f,-.75f}, residual[2]{}, valid[1]{};
        auto c=camera(&depth); c.view_proj_previous.m[12]=2; // +1 pixel camera motion at width 1.
        float current[]{.25f,-.25f}, previous[]{-.25f,0}; c.jitter_current_px[0]=current[0]; c.jitter_current_px[1]=current[1];
        CHECK(DeriveResidual({raw,1,1,2},c,current,previous,residual,valid,1,1));
        NEAR(residual[0],1,1e-6); NEAR(residual[1],-1,1e-6); NEAR(valid[0],1,0);
        depth=1; CHECK(DeriveResidual({raw,1,1,2},c,current,previous,residual,valid,1,1));
        NEAR(valid[0],0,0); NEAR(residual[0],0,0);
        CHECK(!DeriveResidual({raw,2,1,4},c,current,previous,residual,valid,1,1));
    }},
}); }
