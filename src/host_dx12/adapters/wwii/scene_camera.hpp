#pragma once

#include "providers/camera/camera_tracker.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace na::wwii {

struct SceneCamera {
    camera::Matrix vp{};
    std::array<double, 3> origin{};
    uint32_t vp_offset = 0, vp_duplicate = 0;
    bool raster_jitter_qualified = false;
};

// Campaign shader constant layouts observed at the scene draw.
// Require scene extent and the observed redundant pose fields. p49040 has
// rounded copies of the world origin, and shader variants reuse the adjacent
// float4s (400/960 or 560) for unrelated data. Those are not pose witnesses.
inline bool DecodeSceneCamera(const std::vector<uint8_t>& bytes, uint32_t declared_size,
                              uint32_t width, uint32_t height, SceneCamera& out,
                              const char** why = nullptr)
{
    // `why` (diagnostics): the first check that failed on the layout whose size matched.
    const char* failed = "size";
    struct Layout {
        uint32_t size, vp, duplicate, extent;
        std::array<uint32_t, 3> origin;
        uint32_t origin_count;
        bool native_variant;
    };
    constexpr Layout layouts[]{
        {1280, 64, 320, 704, {384, 944, 0}, 2, false},
        // Native campaign p30648: the copy at 64 is unrelated. Two matching
        // matrices and two nonzero origin witnesses still establish the pose.
        {1280, 208, 320, 704, {800, 928, 0}, 2, true},
        // Native p47656: a second shader form copies the matrix at 192 instead and keeps the
        // origin at 928 only (800 then holds a plane, w != 0). 36 min of play ran without DLAA
        // while it was rejected as vp_repeat.
        {1280, 192, 320, 704, {928, 800, 0}, 2, true},
        {1536, 192, 0, 816, {480, 512, 528}, 3, false},
    };
    bool matched_projection_copy = false;
    for (const auto& layout : layouts) {
        if (declared_size != layout.size) continue;
        if (layout.native_variant && matched_projection_copy) continue;
        if (bytes.size() < 976 || !width || !height) { failed = "short_or_no_extent"; continue; }
        std::array<float, 4> extent{}, origin{};
        std::memcpy(extent.data(), bytes.data() + layout.extent, sizeof(extent));
        if (extent[0] != float(width) || extent[1] != float(height) ||
            !(std::abs(extent[2] * width - 1.0f) < 0.002f) ||
            !(std::abs(extent[3] * height - 1.0f) < 0.002f)) { failed = "extent"; continue; }
        if (layout.duplicate && std::memcmp(bytes.data() + layout.vp, bytes.data() + layout.duplicate, 64)) {
            failed = "vp_repeat";
            continue;
        }
        matched_projection_copy = true;
        bool agrees = true;
        if (layout.native_variant) {
            // Native forms: each origin slot is either a live origin (finite, w == 0, not all
            // zero), blank, or unrelated data (w != 0). p37752 left 928 blank for 30 s, p47656
            // put a plane at 800 while 928 held the origin. Every live origin must agree and
            // at least one must exist.
            uint32_t live = 0;
            for (uint32_t i = 0; i < layout.origin_count; ++i) {
                std::array<float, 4> slot{};
                std::memcpy(slot.data(), bytes.data() + layout.origin[i], sizeof(slot));
                if (!std::isfinite(slot[0]) || !std::isfinite(slot[1]) || !std::isfinite(slot[2]) || slot[3] != 0.f) continue;
                if (slot[0] == 0.f && slot[1] == 0.f && slot[2] == 0.f) continue;
                if (!live++) { origin = slot; continue; }
                for (size_t axis = 0; axis < 3; ++axis) {
                    const double scale = std::max(1.0, std::max(std::abs(double(origin[axis])), std::abs(double(slot[axis]))));
                    agrees &= std::abs(double(origin[axis]) - slot[axis]) <= 2.0 * std::numeric_limits<float>::epsilon() * scale;
                }
            }
            if (!live) { failed = "empty_origin"; continue; }
        } else {
            std::memcpy(origin.data(), bytes.data() + layout.origin[0], sizeof(origin));
            if (!std::isfinite(origin[0]) || !std::isfinite(origin[1]) || !std::isfinite(origin[2]) || origin[3] != 0.f) { failed = "origin"; continue; }
            for (uint32_t i = 1; i < layout.origin_count; ++i) {
                std::array<float, 4> other{};
                std::memcpy(other.data(), bytes.data() + layout.origin[i], sizeof(other));
                agrees &= other[3] == 0.f;
                for (size_t axis = 0; axis < 3; ++axis) {
                    const double scale = std::max(1.0, std::max(std::abs(double(origin[axis])), std::abs(double(other[axis]))));
                    agrees &= std::isfinite(other[axis]) &&
                        std::abs(double(origin[axis]) - other[axis]) <= 2.0 * std::numeric_limits<float>::epsilon() * scale;
                }
            }
        }
        if (!agrees) { failed = "origin_witness"; continue; }

        const auto vp = camera::LoadMatrix(bytes, layout.vp, camera::Layout::ColumnMajor);
        camera::VpGeometry geometry{};
        camera::Candidate candidate{};
        if (!camera::ViewProjection(vp, candidate) || !candidate.reversed ||
            !camera::DecomposeViewProjection(vp, geometry) ||
            std::abs(geometry.sy / geometry.sx - double(width) / height) > 0.02 ||
            std::abs(geometry.position[0]) > 1e-5 || std::abs(geometry.position[1]) > 1e-5 ||
            std::abs(geometry.position[2]) > 1e-5) { failed = "vp_shape"; continue; }
        // Native reads establish motion, but do not establish which of its
        // matrices the vertex shader uses to place raster samples.
        out = {vp, {origin[0], origin[1], origin[2]}, layout.vp, layout.duplicate, !layout.native_variant};
        if (why) *why = "ok";
        return true;
    }
    if (why) *why = failed;
    return false;
}

struct SceneCameraReceipt {
    uint64_t frame = 0, color = 0, depth = 0;
    uint32_t writes = 0;
    SceneCamera camera{};

    bool Matches(uint64_t f, uint64_t c, uint64_t d) const {
        return writes && frame == f && color == c && depth == d;
    }
};

} // namespace na::wwii
