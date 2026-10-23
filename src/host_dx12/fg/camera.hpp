#pragma once
#include <cstdint>
#include <cmath>
#include <cstddef>

namespace na::fg {
// TLOU 1.1.5 / SL 1.4.7 layout verified at the pinned executable's
// 0x1f28d38 producer. This is not a claim about arbitrary Streamline versions.
struct Camera {
    float view_to_clip[16], clip_to_view[16], clip_to_lens[16];
    float clip_to_previous[16], previous_to_clip[16];
    float jitter[2], motion_scale[2], pinhole[2];
    float position[3], up[3], right[3], forward[3];
    float near_plane, far_plane, fov, aspect, invalid_motion;
    uint8_t inverted, camera_motion, motion_3d, reset, not_game, ortho, dilated, jittered;
    const void* extension;
};
static_assert(sizeof(Camera) == 432 && offsetof(Camera, jitter) == 320 && offsetof(Camera, inverted) == 412);
inline bool InversePair(const float* a, const float* b) {
    for (unsigned y=0; y<4; ++y) for (unsigned x=0; x<4; ++x) {
        double v=0; for(unsigned k=0;k<4;++k) {
            if (!std::isfinite(a[y*4+k]) || !std::isfinite(b[k*4+x])) return false;
            v += double(a[y*4+k])*b[k*4+x];
        }
        if (std::abs(v-(x==y?1.0:0.0)) > 0.02) return false;
    }
    return true;
}
inline bool ValidCamera(const Camera& c) {
    for (unsigned i=0; i<3; ++i)
        if (!std::isfinite(c.position[i]) || !std::isfinite(c.up[i]) ||
            !std::isfinite(c.right[i]) || !std::isfinite(c.forward[i])) return false;
    if (!std::isfinite(c.near_plane) || !std::isfinite(c.far_plane) ||
        !std::isfinite(c.fov) || !std::isfinite(c.aspect)) return false;
    return InversePair(c.view_to_clip,c.clip_to_view) && InversePair(c.clip_to_previous,c.previous_to_clip) &&
        std::isfinite(c.jitter[0]) && std::isfinite(c.jitter[1]) &&
        std::isfinite(c.pinhole[0]) && std::isfinite(c.pinhole[1]) && std::isfinite(c.invalid_motion) &&
        c.inverted<=1 && c.camera_motion<=1 && c.reset<=1 && c.ortho<=1 && c.dilated<=1 && c.not_game<=1;
}
void RecordCamera(uint64_t engine_frame, uint32_t viewport, const Camera&);
bool FindCamera(uint64_t engine_frame, uint32_t viewport, Camera&);
struct CameraStats { uint64_t recorded=0, rejected=0, matched=0, missed=0; };
CameraStats GetCameraStats();
// The synchronous SL evaluate -> NGX call chain, not the latest global frame.
struct EvaluationScope {
    EvaluationScope(uint32_t frame, uint32_t viewport);
    ~EvaluationScope();
    uint64_t previous_frame; uint32_t previous_view;
};
uint64_t EvaluationFrame();
uint32_t EvaluationViewport();
}
