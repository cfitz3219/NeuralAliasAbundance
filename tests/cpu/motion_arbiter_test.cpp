// reconstructed 2026-10 — independent numerical truth and per-pixel provenance.
#include "test_support.hpp"
#include "core/motion_arbiter.hpp"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>
using namespace na::motion;
namespace {
Mat4 identity() { Mat4 m{}; m.m[0] = m.m[5] = m.m[10] = m.m[15] = 1; return m; }
Tier2Input camera(const float* depth, uint32_t w = 1, uint32_t h = 1) {
    Tier2Input t{}; t.valid = true; t.depth = {depth, w, h, w};
    t.inv_view_proj_current = identity(); t.view_proj_previous = identity(); return t;
}
// Perspective camera (D3D, row-vector Mat4 = transpose of the column-vector matrix),
// looking down +Z after a yaw, at `pos`. Standard depth, near .1, far 1000.
Mat4 cam_row(double yaw, double px) {
    const double n = .1, f = 1000, sy = 1.0, sx = 1.0;
    const double P[16] = {sx,0,0,0, 0,sy,0,0, 0,0,f/(f-n),-f*n/(f-n), 0,0,1,0};
    const double c = std::cos(yaw), s = std::sin(yaw);
    double V[16] = {c,0,-s,0, 0,1,0,0, s,0,c,0, 0,0,0,1};
    const double pos[3] = {px, 0, 0};
    for (int r = 0; r < 3; ++r) V[r*4+3] = -(V[r*4]*pos[0] + V[r*4+1]*pos[1] + V[r*4+2]*pos[2]);
    double M[16]{};
    for (int r = 0; r < 4; ++r) for (int k = 0; k < 4; ++k) for (int q = 0; q < 4; ++q) M[r*4+k] += P[r*4+q]*V[q*4+k];
    Mat4 o{}; for (int r = 0; r < 4; ++r) for (int k = 0; k < 4; ++k) o.m[k*4+r] = static_cast<float>(M[r*4+k]);
    return o;
}
Mat4 inverse(const Mat4& m) {
    double a[4][8]{};
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) { a[r][c] = m.m[r*4+c]; a[r][c+4] = r == c; }
    for (int c = 0; c < 4; ++c) {
        int p = c; for (int r = c+1; r < 4; ++r) if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
        for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[p][k]);
        const double d = a[c][c]; for (double& v : a[c]) v /= d;
        for (int r = 0; r < 4; ++r) if (r != c) { const double f = a[r][c]; for (int k = 0; k < 8; ++k) a[r][k] -= f*a[c][k]; }
    }
    Mat4 o{}; for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) o.m[r*4+c] = static_cast<float>(a[r][c+4]);
    return o;
}
// Double-precision row-vector camera far from the origin (like Anno's world).
using D16 = std::array<double, 16>;
D16 cam_row_d(double yaw, double px, double pz) {
    const double n = .1, f = 1000;
    const double P[16] = {1,0,0,0, 0,1,0,0, 0,0,f/(f-n),-f*n/(f-n), 0,0,1,0};
    const double c = std::cos(yaw), s = std::sin(yaw);
    double V[16] = {c,0,-s,0, 0,1,0,0, s,0,c,0, 0,0,0,1};
    const double pos[3] = {px, 40, pz};
    for (int r = 0; r < 3; ++r) V[r*4+3] = -(V[r*4]*pos[0] + V[r*4+1]*pos[1] + V[r*4+2]*pos[2]);
    D16 M{}, o{};
    for (int r = 0; r < 4; ++r) for (int k = 0; k < 4; ++k) for (int q = 0; q < 4; ++q) M[r*4+k] += P[r*4+q]*V[q*4+k];
    for (int r = 0; r < 4; ++r) for (int k = 0; k < 4; ++k) o[k*4+r] = M[r*4+k]; // row-vector
    return o;
}
D16 inverse_d(const D16& m) {
    double a[4][8]{};
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) { a[r][c] = m[r*4+c]; a[r][c+4] = r == c; }
    for (int c = 0; c < 4; ++c) {
        int p = c; for (int r = c+1; r < 4; ++r) if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
        for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[p][k]);
        const double d = a[c][c]; for (double& v : a[c]) v /= d;
        for (int r = 0; r < 4; ++r) if (r != c) { const double f = a[r][c]; for (int k = 0; k < 8; ++k) a[r][k] -= f*a[c][k]; }
    }
    D16 o{}; for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) o[r*4+c] = a[r][c+4];
    return o;
}
Mat4 f4(const D16& d) { Mat4 o{}; for (int i = 0; i < 16; ++i) o.m[i] = static_cast<float>(d[i]); return o; }
D16 mul_d(const D16& a, const D16& b) { D16 o{}; for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) for (int k = 0; k < 4; ++k) o[r*4+c] += a[r*4+k]*b[k*4+c]; return o; }
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
        for (float z : {-1.f, 1.1f, std::numeric_limits<float>::quiet_NaN()})
            CHECK(!ReprojectPixel(t, 1, 1, 0, 0, z, &x, &y, &off));
        t.depth_inverted = true; t.depth_clear = 0;
        t.view_proj_previous.m[8] = 2; // x depends on RAW depth; a numeric depth flip would give .75.
        CHECK(ReprojectPixel(t, 1, 1, 0, 0, depth, &x, &y, &off)); NEAR(x, .25, 1e-6);
        t.view_proj_previous.m[12] = 4;
        CHECK(ReprojectPixel(t, 1, 1, 0, 0, depth, &x, &y, &off)); CHECK(off);
        t.view_proj_previous.m[15] = -1; CHECK(!ReprojectPixel(t, 1, 1, 0, 0, depth, &x, &y, &off));
    }},
    {"sky (clear depth) reprojects at the far plane: rotation moves it, translation barely does", [] {
        float depth = 1.f, x = 0, y = 0; bool off = false; auto t = camera(&depth);
        t.inv_view_proj_current = inverse(cam_row(0, 0));
        t.view_proj_previous = cam_row(0.01, 0); // yawed 0.01 rad: ~0.01 * focal (64 px) = ~0.64 px... at 128 px wide
        CHECK(ReprojectPixel(t, 128, 128, 64, 64, depth, &x, &y, &off));
        CHECK(std::fabs(x) > 0.5f && std::fabs(y) < 0.05f);
        t.view_proj_previous = cam_row(0, 1.0); // moved 1 unit sideways: the sky is ~1000 units away
        CHECK(ReprojectPixel(t, 128, 128, 64, 64, depth, &x, &y, &off));
        CHECK(std::fabs(x) < 0.1f && std::fabs(y) < 0.05f);
        t.depth_inverted = true; t.depth_clear = 0; // reverse-Z clear value is also sky
        CHECK(ReprojectPixel(t, 128, 128, 64, 64, 0.f, &x, &y, &off));
    }},
    {"a still camera far from the origin: composed reprojection gives zero motion (Anno blur)", [] {
        // Anno-like: camera thousands of units from the origin, still. Two separate
        // float matrices chained through world space give sub-pixel false motion;
        // one matrix composed in double gives none.
        const D16 vp = cam_row_d(0.7, 4200.0, -6100.0), inv = inverse_d(vp);
        float depth = .9995f, x = 0, y = 0; bool off = false;
        auto t = camera(&depth);
        float worst_sep = 0, worst_comp = 0;
        for (uint32_t py = 100; py < 1440; py += 331)
            for (uint32_t px = 50; px < 3440; px += 617) {
                t.inv_view_proj_current = f4(inv);
                t.view_proj_previous = f4(vp);
                if (ReprojectPixel(t, 3440, 1440, px, py, depth, &x, &y, &off)) worst_sep = std::max(worst_sep, std::max(std::fabs(x), std::fabs(y)));
                D16 id{}; id[0] = id[5] = id[10] = id[15] = 1;
                t.inv_view_proj_current = f4(mul_d(inv, vp));
                t.view_proj_previous = f4(id);
                CHECK(ReprojectPixel(t, 3440, 1440, px, py, depth, &x, &y, &off));
                worst_comp = std::max(worst_comp, std::max(std::fabs(x), std::fabs(y)));
            }
        std::printf("still camera: worst false motion separate %.5f px, composed %.5f px\n", worst_sep, worst_comp);
        CHECK(worst_comp < 0.002f);
        // And it still moves correctly: yaw by 0.002 rad between frames.
        const D16 prev = cam_row_d(0.702, 4200.0, -6100.0);
        D16 id{}; id[0] = id[5] = id[10] = id[15] = 1;
        t.inv_view_proj_current = f4(mul_d(inv, prev));
        t.view_proj_previous = f4(id);
        CHECK(ReprojectPixel(t, 3440, 1440, 1720, 720, depth, &x, &y, &off));
        CHECK(std::fabs(x) > 1.0f);
    }},
    {"engine priority and per-pixel validity fallbacks", [] {
        float engine[]{4,5, 6,7, 8,9}, valid[]{1,0,std::numeric_limits<float>::quiet_NaN()};
        float depths[]{.5f,.5f,std::numeric_limits<float>::quiet_NaN()} /* no usable depth (clear depth is sky now) */, residual[]{2,0, 2,0, 2,0}, output[6]{}; uint8_t provenance[3]{};
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
        depth=std::numeric_limits<float>::quiet_NaN(); // unusable depth (the clear value is sky: reprojected)
        CHECK(DeriveResidual({raw,1,1,2},c,current,previous,residual,valid,1,1));
        NEAR(valid[0],0,0); NEAR(residual[0],0,0);
        CHECK(!DeriveResidual({raw,2,1,4},c,current,previous,residual,valid,1,1));
    }},
}); }
