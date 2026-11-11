// DX11 camera pick: finds the camera ViewProjection in a constant buffer's
// bytes, with the same rule as the DX12 host (live_dlaa.cpp, find_camera): a
// ViewProjection whose inverse sits in the same block (engines that rebuild
// positions from depth upload both; object constants rarely do). Uses the
// shared na::camera::FindMatrices, so there is one definition of "a camera".
// Once found, the offset is learned and checked first next frame.
//
// Portable C++17 (Linux-tested).
#pragma once

#include "providers/camera/matrix_candidates.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

namespace na::native11 {

struct CameraPick {
    bool found = false;
    uint64_t offset = 0;          // byte offset of the ViewProjection in the block
    int64_t inverse_delta = 0;    // inverse's offset minus the VP's
    na::camera::Layout layout = na::camera::Layout::RowMajor;
    na::camera::Matrix vp{};      // column-vector convention, as FindMatrices normalises
    bool reversed = false;        // reverse-Z
    bool camera_relative = false; // the game adapter proved this pose without an inverse pair
    bool raster_jitter_qualified = true; // adapter can limit a pose to motion reconstruction
    // Admitted by the shared no-inverse rules (providers/camera/camera_admission.hpp): the game
    // uploads no inverse, so the matrix's own shape at the learned place is the re-read's proof.
    bool derived_inverse = false;
};

// Re-reads a learned camera at `learned` (same layout); true if it still is a
// perspective ViewProjection with its inverse at the same distance.
// A camera learned without an inverse is re-read only from the binding it was
// learned at (`learned_binding`): an inverse pair proves itself in any block, a
// lone matrix at the same offset of another block proves nothing.
inline bool CheckLearned(const std::vector<uint8_t>& bytes, const CameraPick& learned, CameraPick* out,
                         bool learned_binding = true)
{
    if (!learned.found || learned.camera_relative || learned.offset + 64 > bytes.size()) return false;
    const auto m = na::camera::LoadMatrix(bytes, static_cast<size_t>(learned.offset), learned.layout);
    na::camera::Candidate c{};
    if (!na::camera::ViewProjection(m, c)) return false;
    if (learned.derived_inverse) {
        if (!learned_binding) return false;
        *out = learned;
        out->vp = m;
        out->reversed = c.reversed;
        return true;
    }
    const int64_t io = static_cast<int64_t>(learned.offset) + learned.inverse_delta;
    if (io < 0 || static_cast<uint64_t>(io) + 64 > bytes.size()) return false;
    const auto inv = na::camera::LoadMatrix(bytes, static_cast<size_t>(io), learned.layout);
    if (!na::camera::IsInversePair(m, inv)) return false;
    *out = learned;
    out->vp = m;
    out->reversed = c.reversed;
    return true;
}

inline CameraPick FindCamera(const std::vector<uint8_t>& bytes, uint64_t identity)
{
    CameraPick r;
    if (bytes.size() < 128) return r;
    na::camera::Snapshot s{};
    s.session = s.epoch = 1;
    s.submission = s.content_version = identity;
    s.resource = identity;
    s.base_offset = 0;
    s.complete = s.lifetime_valid = true;
    s.sync = na::camera::Sync::AppSyncUnknown; // read on the CPU at the game's own upload: scope known
    s.bytes = bytes;
    const auto scan = na::camera::FindMatrices(s);
    for (const auto& c : scan.candidates) {
        if (c.kind != na::camera::Kind::ViewProjection || !c.has_inverse_pair) continue;
        r.found = true;
        r.offset = c.byte_offset;
        r.inverse_delta = static_cast<int64_t>(c.inverse_byte_offset) - static_cast<int64_t>(c.byte_offset);
        r.layout = c.layout;
        r.vp = c.matrix;
        r.reversed = c.reversed;
        return r;
    }
    return r;
}

// Writes a column-vector matrix back into the block in the block's own layout
// (for jitter injection at the game's upload).
inline void StoreMatrix(uint8_t* bytes, size_t size, uint64_t offset, na::camera::Layout layout, const na::camera::Matrix& m)
{
    if (offset + 64 > size) return;
    float f[16];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            const float v = static_cast<float>(m[r * 4 + c]);
            if (layout == na::camera::Layout::RowMajor) f[r * 4 + c] = v;
            else f[c * 4 + r] = v;
        }
    std::memcpy(bytes + offset, f, 64);
}

} // namespace na::native11
