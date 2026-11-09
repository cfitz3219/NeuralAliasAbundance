#pragma once

#include "providers/camera/camera_tracker.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace na::wwii {

struct SceneCamera {
    camera::Matrix vp{};
    std::array<double, 3> origin{};
};

// Campaign shader constant layouts observed in the translated scene pass.
// Require the scene extent and the redundant copies, not a process address
// or an arbitrary smooth float3. The other view blocks share the same lens
// and sometimes the same VP but lack these repeated world-origin fields.
inline bool DecodeSceneCamera(const std::vector<uint8_t>& bytes, uint32_t declared_size,
                              uint32_t width, uint32_t height, SceneCamera& out)
{
    struct Layout {
        uint32_t size, vp, extent;
        std::array<uint32_t, 4> origin;
    };
    constexpr Layout layouts[]{
        {1280, 64, 704, {384, 400, 944, 960}},
        {1536, 192, 816, {480, 512, 528, 560}},
    };
    for (const auto& layout : layouts) {
        if (declared_size != layout.size || bytes.size() < 976 || !width || !height) continue;
        std::array<float, 4> extent{}, origin{};
        std::memcpy(extent.data(), bytes.data() + layout.extent, sizeof(extent));
        if (extent[0] != float(width) || extent[1] != float(height) ||
            !(std::abs(extent[2] * width - 1.0f) < 0.002f) ||
            !(std::abs(extent[3] * height - 1.0f) < 0.002f)) continue;
        if (layout.size == 1280 &&
            (std::memcmp(bytes.data() + layout.vp, bytes.data() + 208, 64) ||
             std::memcmp(bytes.data() + layout.vp, bytes.data() + 320, 64))) continue;
        std::memcpy(origin.data(), bytes.data() + layout.origin[0], sizeof(origin));
        if (!std::isfinite(origin[0]) || !std::isfinite(origin[1]) || !std::isfinite(origin[2]) || origin[3] != 0.f) continue;
        bool agrees = true;
        for (const auto offset : layout.origin)
            agrees &= std::memcmp(bytes.data() + offset, origin.data(), sizeof(origin)) == 0;
        if (!agrees) continue;

        const auto vp = camera::LoadMatrix(bytes, layout.vp, camera::Layout::ColumnMajor);
        camera::VpGeometry geometry{};
        camera::Candidate candidate{};
        if (!camera::ViewProjection(vp, candidate) || !candidate.reversed ||
            !camera::DecomposeViewProjection(vp, geometry) ||
            std::abs(geometry.sy / geometry.sx - double(width) / height) > 0.02 ||
            std::abs(geometry.position[0]) > 1e-5 || std::abs(geometry.position[1]) > 1e-5 ||
            std::abs(geometry.position[2]) > 1e-5) continue;
        out = {vp, {origin[0], origin[1], origin[2]}};
        return true;
    }
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
