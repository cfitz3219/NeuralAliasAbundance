#include "core/motion_arbiter.hpp"

#include <cmath>

namespace na::motion {
namespace {

inline bool finite2(float a, float b) { return std::isfinite(a) && std::isfinite(b); }
inline bool field_ok(const Field2& f, uint32_t w, uint32_t h)
{
    return f.xy && f.width == w && f.height == h && f.stride_floats >= w * 2;
}
inline bool field_ok(const Field1& f, uint32_t w, uint32_t h)
{
    return f.v && f.width == w && f.height == h && f.stride_floats >= w;
}
inline float at1(const Field1& f, uint32_t x, uint32_t y)
{
    return f.v[static_cast<size_t>(y) * f.stride_floats + x];
}
inline const float* at2(const Field2& f, uint32_t x, uint32_t y)
{
    return f.xy + static_cast<size_t>(y) * f.stride_floats + x * 2;
}
// Validity-mask domain (rev2-R2): finite and in (0,1] authorizes; exactly 0
// means "not written"; anything else is malformed and never authorizes.
enum class MaskState { Valid, NotWritten, Malformed };
inline MaskState mask_state(float m)
{
    if (!std::isfinite(m) || m < 0.0f || m > 1.0f) return MaskState::Malformed;
    return m > 0.0f ? MaskState::Valid : MaskState::NotWritten;
}

inline void mul(const float* p, const Mat4& m, float* out)
{
    for (int c = 0; c < 4; ++c)
        out[c] = p[0] * m.m[0 * 4 + c] + p[1] * m.m[1 * 4 + c] + p[2] * m.m[2 * 4 + c] + p[3] * m.m[3 * 4 + c];
}

// Snap/clamp; returns true if a non-zero residual remains. Sets `filtered`
// when either approximation altered the value.
inline bool shape_residual(float& rx, float& ry, const Config& cfg, Stats* s, bool& filtered)
{
    float mag = std::sqrt(rx * rx + ry * ry);
    if (mag < cfg.residual_snap_px) {
        if (mag != 0.0f) filtered = true;
        rx = ry = 0.0f;
        ++s->pixels_residual_snapped;
        return false;
    }
    if (mag > cfg.residual_max_px) {
        float k = cfg.residual_max_px / mag;
        rx *= k;
        ry *= k;
        ++s->pixels_residual_clamped;
        filtered = true;
    }
    return true;
}

} // namespace

bool ValidateConfig(const Config& cfg)
{
    return std::isfinite(cfg.residual_snap_px) && std::isfinite(cfg.residual_max_px) &&
           cfg.residual_snap_px >= 0.0f && cfg.residual_max_px >= cfg.residual_snap_px;
}

bool ReprojectPixel(const Tier2Input& t2, uint32_t width, uint32_t height, uint32_t x, uint32_t y,
                    float depth, float* vx, float* vy, bool* offscreen)
{
    if (!vx || !vy || width == 0 || height == 0 || x >= width || y >= height) return false;
    if (!std::isfinite(depth) || depth < 0.0f || depth > 1.0f || depth == t2.depth_clear) return false;
    if (!finite2(t2.jitter_current_px[0], t2.jitter_current_px[1])) return false;
    // The raster sample at this pixel center was taken under the current
    // frame's jitter: in the UNJITTERED projection it sits at
    // p_raster - jitter_current. Unproject there (rev2-R1); raw depth goes
    // straight through the supplier's actual inverse matrix.
    const float px = static_cast<float>(x) + 0.5f - t2.jitter_current_px[0];
    const float py = static_cast<float>(y) + 0.5f - t2.jitter_current_px[1];
    float ndc[4] = {px / width * 2.0f - 1.0f, 1.0f - py / height * 2.0f, depth, 1.0f};
    float world[4];
    mul(ndc, t2.inv_view_proj_current, world);
    if (!std::isfinite(world[3]) || std::fabs(world[3]) < 1e-20f) return false;
    for (float& c : world) c /= world[3];
    world[3] = 1.0f;
    float clip[4];
    mul(world, t2.view_proj_previous, clip);
    if (!std::isfinite(clip[3]) || clip[3] <= 0.0f) return false; // behind the previous camera
    float nx = clip[0] / clip[3], ny = clip[1] / clip[3];
    if (!finite2(nx, ny)) return false;
    float ppx = (nx + 1.0f) * 0.5f * width, ppy = (1.0f - ny) * 0.5f * height;
    *vx = ppx - px;
    *vy = ppy - py;
    if (offscreen) *offscreen = ppx < 0.0f || ppy < 0.0f || ppx >= width || ppy >= height;
    return finite2(*vx, *vy);
}

bool DeriveResidual(const Field2& raw_flow, const Tier2Input& t2, const float jitter_current_px[2],
                    const float jitter_previous_px[2], float* residual_out, float* valid_out,
                    uint32_t width, uint32_t height)
{
    if (!residual_out || !valid_out || !jitter_current_px || !jitter_previous_px ||
        !field_ok(raw_flow, width, height) || !t2.valid || !field_ok(t2.depth, width, height))
        return false;
    const float jdx = jitter_previous_px[0] - jitter_current_px[0];
    const float jdy = jitter_previous_px[1] - jitter_current_px[1];
    if (!finite2(jdx, jdy)) return false;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const float* f = at2(raw_flow, x, y);
            float cx = 0, cy = 0;
            bool ok = ReprojectPixel(t2, width, height, x, y, at1(t2.depth, x, y), &cx, &cy, nullptr) &&
                      finite2(f[0], f[1]);
            size_t i = static_cast<size_t>(y) * width + x;
            residual_out[i * 2] = ok ? f[0] - cx - jdx : 0.0f;
            residual_out[i * 2 + 1] = ok ? f[1] - cy - jdy : 0.0f;
            valid_out[i] = ok ? 1.0f : 0.0f;
        }
    }
    return true;
}

bool Arbitrate(const Tier1Input& t1, const Tier2Input& t2, const Tier3Input& t3, const Config& cfg,
               Output& out, Stats* stats)
{
    if (!out.xy || !out.provenance || out.width == 0 || out.height == 0 || !ValidateConfig(cfg)) return false;
    const uint32_t w = out.width, h = out.height;
    Stats local{};
    Stats* s = stats ? stats : &local;
    *s = Stats{};

    bool use1 = t1.valid && field_ok(t1.motion, w, h) && (!t1.valid_mask.v || field_ok(t1.valid_mask, w, h));
    bool use2 = t2.valid && field_ok(t2.depth, w, h);
    bool use3 = t3.valid && field_ok(t3.residual, w, h) && (!t3.mask.v || field_ok(t3.mask, w, h)) &&
                (!t3.valid_mask.v || field_ok(t3.valid_mask, w, h));
    const uint64_t all = static_cast<uint64_t>(w) * h;
    if (t1.valid && !use1) s->pixels_rejected += all;
    if (t2.valid && !use2) s->pixels_rejected += all;
    if (t3.valid && !use3) s->pixels_rejected += all;

    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            float* o = out.xy + i * 2;
            uint8_t& prov = out.provenance[i];
            o[0] = o[1] = 0.0f;
            prov = NA_MOTION_INVALID;

            if (use1) {
                MaskState ms = t1.valid_mask.v ? mask_state(at1(t1.valid_mask, x, y)) : MaskState::Valid;
                if (ms == MaskState::Malformed) ++s->pixels_rejected;
                if (ms == MaskState::Valid) {
                    const float* e = at2(t1.motion, x, y);
                    if (finite2(e[0], e[1])) {
                        o[0] = e[0];
                        o[1] = e[1];
                        prov = NA_MOTION_ENGINE;
                        ++s->pixels_engine;
                        continue;
                    }
                    ++s->pixels_rejected;
                }
            }

            bool have_cam = false, offscreen = false;
            float cx = 0, cy = 0;
            if (use2) {
                have_cam = ReprojectPixel(t2, w, h, x, y, at1(t2.depth, x, y), &cx, &cy, &offscreen);
                if (!have_cam) ++s->pixels_rejected;
            }
            if (!have_cam) {
                ++s->pixels_invalid;
                continue; // residual is meaningless without its camera vector
            }

            float rx = 0, ry = 0;
            bool have_res = false, filtered = false;
            if (use3) {
                MaskState vs = t3.valid_mask.v ? mask_state(at1(t3.valid_mask, x, y)) : MaskState::Valid;
                if (vs == MaskState::Malformed) ++s->pixels_rejected;
                bool rvalid = vs == MaskState::Valid;
                float m = t3.mask.v ? at1(t3.mask, x, y) : 1.0f;
                if (rvalid && std::isfinite(m) && m >= 0.0f && m <= 1.0f) {
                    if (m > 0.0f) {
                        const float* r = at2(t3.residual, x, y);
                        if (finite2(r[0], r[1])) {
                            rx = r[0] * m;
                            ry = r[1] * m;
                            have_res = shape_residual(rx, ry, cfg, s, filtered);
                        } else {
                            ++s->pixels_rejected;
                        }
                    }
                } else if (rvalid) {
                    ++s->pixels_rejected; // out-of-contract mask rejects the evidence
                }
            }

            o[0] = cx + rx;
            o[1] = cy + ry;
            prov = have_res ? NA_MOTION_CAMERA_RESIDUAL : NA_MOTION_CAMERA;
            if (filtered) prov |= NA_MOTION_F_FILTERED;
            if (offscreen) {
                prov |= NA_MOTION_F_OFFSCREEN;
                ++s->pixels_offscreen;
            }
            ++s->pixels_camera;
            if (have_res) ++s->pixels_residual_added;
        }
    }
    if (s->pixels_engine) s->tier_used = Tier::EngineDxil;
    else if (s->pixels_residual_added) s->tier_used = Tier::ResidualOpticalFlow;
    else if (s->pixels_camera) s->tier_used = Tier::CameraReprojection;
    else s->tier_used = Tier::None;
    return true;
}

} // namespace na::motion
