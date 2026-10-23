// MotionArbiter â€” the Cascading Priority Motion Pipeline (proposal M-001, rev2
// after Route B's M-001-REVIEW).
//
// Purpose: produce a DLSS-grade per-pixel motion field for games that never
// generated motion vectors, by arbitrating three sources of decreasing trust:
//
//   Tier 1  Engine ground truth  â€” motion vectors injected via DXIL patching
//                                  of the game's own shaders (exact where valid).
//   Tier 2  Analytical camera    â€” reprojection of the depth buffer through the
//                                  previous and current camera matrices
//                                  (exact for static geometry; nothing for
//                                  moving objects).
//   Tier 3  Residual optical flowâ€” image-space flow AFTER camera motion has
//                                  been removed, masked by depth (so only
//                                  plausible moving-object regions contribute);
//                                  noisy, hence snapped/clamped = ESTIMATED.
//
// Arbitration per pixel:
//   Tier 1 valid at pixel       -> V = V_engine                 provenance ENGINE
//   else Tier 2 valid at pixel  -> V = V_cam [+ residual]       CAMERA / CAMERA_RESIDUAL
//   else                        -> V = 0                         INVALID
// Residual is applied ONLY where a valid camera vector exists at that pixel:
// camera-subtracted flow has no meaning as a total vector, so there is no
// residual-only fallback (review finding 5).
//
// Conventions (binding for every tier and for the output):
//   - Fields are row-major, render-resolution, 2 floats per pixel, in PIXELS.
//   - v = p_previous - p_current (where this pixel's content was one frame
//     ago), +x right, +y down; the value NGX consumes with MV scale (1,1).
//   - Matrices: row-major storage, ROW-VECTOR algebra (p' = p * M), DirectX.
//     Suppliers using column-vector algebra pass the transpose (Route B does).
//   - Matrices are UNJITTERED, but the raster depth samples were taken with
//     the current frame's raster jitter applied. So the sample at raster
//     position p_raster corresponds to unjittered-projection position
//     p_raster - jitter_current; the reprojection unprojects THERE and
//     returns v = p_prev_unjittered - (p_raster - jitter_current). Previous
//     jitter is never added to the vector (it belongs to a previous-sample
//     lookup, not to motion). (review findings 1 / rev2-R1)
//   - Depth is the RAW post-projection z/w in [0,1] and is fed through the
//     supplier's ACTUAL inverse view-projection; `depth_inverted` is metadata
//     (which end is near / which value is "clear"), never a numeric flip.
//     (review finding 2)
//   - Validity is per pixel: a pixel is invalid at a tier when its input is
//     absent, non-finite, out of range, at the clear depth, or reprojects
//     behind the camera. Invalid pixels produce v = 0 and provenance INVALID;
//     consumers must not read a zero as "stationary". (findings 3, 6)
//
// Supplier prerequisites the arbiter does NOT check (documented, enforced by
// the adapter): the two frames form a consecutive pair in one session/epoch,
// no resize/reset between them, all fields share the render subrect origin
// and extent. GPU-port validation tolerance: |Î”v| â‰¤ 1e-3 px per component;
// bit-exactness is NOT claimed for float math across CPU/GPU.
#pragma once

#include <cstdint>

namespace na::motion {

enum class Tier : uint32_t { None = 0, EngineDxil = 1, CameraReprojection = 2, ResidualOpticalFlow = 3 };

struct Field2 {          // 2 floats per pixel
    const float* xy = nullptr;
    uint32_t width = 0, height = 0;
    uint32_t stride_floats = 0; // floats per row (>= width * 2)
};
struct Field1 {          // 1 float per pixel
    const float* v = nullptr;
    uint32_t width = 0, height = 0;
    uint32_t stride_floats = 0; // floats per row (>= width)
};
struct Mat4 { float m[16]; }; // row-major, row vectors

struct Tier1Input {
    bool valid = false;
    Field2 motion;           // engine motion, arbiter convention
    Field1 valid_mask;       // per-pixel: finite value in (0,1] = written by
                             // the engine; 0 = not written; anything else
                             // (NaN/Inf/<0/>1) = malformed -> INVALID; null = all
};

struct Tier2Input {
    bool valid = false;
    Field1 depth;            // raw z/w in [0,1]
    bool depth_inverted = false;
    float depth_clear = 1.0f;    // value meaning "no geometry" (0 if inverted)
    Mat4 inv_view_proj_current;  // clip -> world, current camera, unjittered
    Mat4 view_proj_previous;     // world -> clip, previous camera, unjittered
    float jitter_current_px[2] = {0, 0}; // raster jitter of the CURRENT frame,
                                          // pixels, same sign the game applied
                                          // to its projection (see header note)
};

struct Tier3Input {
    bool valid = false;
    Field2 residual;         // image flow with camera motion already removed
    Field1 mask;             // in [0,1]: 0 = ignore; null = 1 everywhere
    Field1 valid_mask;       // per-pixel validity, same domain rule as Tier 1's:
                             // finite (0,1] valid, 0 invalid, else malformed
};

struct Config {
    float residual_snap_px = 0.25f;  // |residual| below this -> 0 (approximation!)
    float residual_max_px = 64.0f;   // |residual| above this -> scaled to this
};

// Per-pixel provenance written alongside the vectors.
enum Provenance : uint8_t {
    NA_MOTION_INVALID = 0,          // no valid source; vector is 0, not "still"
    NA_MOTION_ENGINE = 1,           // Tier 1
    NA_MOTION_CAMERA = 2,           // Tier 2 only (static geometry assumption)
    NA_MOTION_CAMERA_RESIDUAL = 3,  // Tier 2 + Tier 3 (estimated)
};
enum ProvenanceFlags : uint8_t {
    NA_MOTION_F_FILTERED = 0x10,    // residual was snapped or clamped
    NA_MOTION_F_OFFSCREEN = 0x20,   // previous position lies outside the frame
};

struct Output {
    float* xy = nullptr;             // caller-owned, width*height*2
    uint8_t* provenance = nullptr;   // caller-owned, width*height (Provenance | flags)
    uint32_t width = 0, height = 0;
};

struct Stats {
    Tier tier_used = Tier::None;
    uint64_t pixels_engine = 0;
    uint64_t pixels_camera = 0;
    uint64_t pixels_residual_added = 0;
    uint64_t pixels_residual_snapped = 0;
    uint64_t pixels_residual_clamped = 0;
    uint64_t pixels_offscreen = 0;
    uint64_t pixels_invalid = 0;     // no valid tier at that pixel
    uint64_t pixels_rejected = 0;    // inputs rejected (non-finite/out of contract)
};

// Validates config (finite, non-negative, max >= snap). Arbitrate rejects an
// invalid config.
bool ValidateConfig(const Config& cfg);

// Runs the arbitration for one frame. A tier whose extent mismatches `out`
// is rejected as a whole. Returns false only if `out` or `cfg` is unusable.
bool Arbitrate(const Tier1Input& t1, const Tier2Input& t2, const Tier3Input& t3,
               const Config& cfg, Output& out, Stats* stats);

// Analytical reprojection for one pixel: v = p_prev - p_cur in pixels using
// unjittered pixel centers. Fails (returns false) for depth outside [0,1] or
// at the clear value, non-finite math, or a previous position behind the
// camera (w <= 0). `offscreen` (optional) reports a previous position outside
// the frame; the vector is still returned for such pixels.
bool ReprojectPixel(const Tier2Input& t2, uint32_t width, uint32_t height,
                    uint32_t x, uint32_t y, float depth, float* vx, float* vy,
                    bool* offscreen);

// For suppliers with RAW image flow measured on JITTERED frames:
//   residual = raw_flow - V_cam - (jitter_prev - jitter_cur)
// Writes residual and a per-pixel validity (1 where the camera vector was
// valid, else 0 with residual 0). Returns false on contract violations.
bool DeriveResidual(const Field2& raw_flow, const Tier2Input& t2,
                    const float jitter_current_px[2], const float jitter_previous_px[2],
                    float* residual_out, float* valid_out, uint32_t width, uint32_t height);

} // namespace na::motion
