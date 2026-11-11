#pragma once
// VARIANT: camera11/split
//
// The split-camera vote, with no D3D11 in it, so recorded draws can be replayed
// through the same rule the live provider uses.
//
// A scene draw votes when it binds two small constant ranges together: a world
// range that witnesses the rebased origin (wwii::DecodeWorldOrigins) and a
// 64-byte view-projection range. Draws that agree on both form one vote. At the
// scene boundary the vote with the most draws wins, if its projection is a
// camera-relative scene projection and it covers enough of the scene.
//
// The provider owns the hooks, the uploads and the locks. This file owns the key
// and the choice, and nothing else.

#include "host_dx12/adapters/wwii/scene_camera.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace na::native11 {

// Projection bytes exactly as bound, and the world origins they were drawn with.
using SplitKey = std::pair<std::array<uint8_t, 64>, wwii::WorldOrigins>;

// The range of an upload a binding exposes: `first` and `count` are in 16-byte
// constants, count 0 means "to the end" (a plain bind).
inline bool SplitBoundRange(size_t size, uint32_t first, uint32_t count, size_t& start, size_t& length)
{
    start = size_t(first) * 16;
    if (start >= size) return false;
    const size_t left = size - start;
    length = count ? std::min(left, size_t(count) * 16) : left;
    return true;
}

// The identity of one draw's camera evidence. False when the draw is not a vote.
// Pass a fresh key; it is only meaningful when this returns true.
inline bool MakeSplitKey(const uint8_t* world, size_t world_size, uint32_t world_first, uint32_t world_count,
                         const uint8_t* projection, size_t projection_size, uint32_t projection_first,
                         uint32_t projection_count, SplitKey& key)
{
    size_t world_start = 0, world_length = 0, projection_start = 0, projection_length = 0;
    if (!SplitBoundRange(world_size, world_first, world_count, world_start, world_length) ||
        !SplitBoundRange(projection_size, projection_first, projection_count, projection_start, projection_length))
        return false;
    if (projection_length != key.first.size() ||
        !wwii::DecodeWorldOrigins(world + world_start, world_length, key.second)) return false;
    std::memcpy(key.first.data(), projection + projection_start, key.first.size());
    return true;
}

// Why a frame's votes did or did not give a camera. Logged, so a run says WHY.
enum class SplitVerdict : uint8_t {
    Chosen,
    NoVotes,      // no scene draw bound a world range and a 64-byte projection together
    NoProjection, // votes exist, none holds a camera-relative scene projection for this extent
    FewDraws,     // the leading vote has fewer than 2 draws
    NoMajority,   // the leading vote does not hold more than half of the qualified draws
    SmallShare,   // the leading vote covers less than 1/8 of the frame's scene draws
};
inline const char* SplitVerdictName(SplitVerdict v)
{
    switch (v) {
    case SplitVerdict::Chosen: return "chosen";
    case SplitVerdict::NoVotes: return "no_votes";
    case SplitVerdict::NoProjection: return "no_projection";
    case SplitVerdict::FewDraws: return "few_draws";
    case SplitVerdict::NoMajority: return "no_majority";
    case SplitVerdict::SmallShare: return "small_share";
    }
    return "?";
}

// Chooses among `votes` (an ordered map keyed by SplitKey; `draws_of` reads a
// vote's draw count). `best` is the leading qualified vote and `best_vp` its
// projection whenever one exists, also when the verdict is a rejection.
// Ties keep the first vote in key order.
template <class Votes, class DrawsOf>
SplitVerdict ChooseSplitCamera(const Votes& votes, DrawsOf&& draws_of, uint32_t width, uint32_t height,
                               uint64_t scene_draws, typename Votes::const_iterator& best, camera::Matrix& best_vp)
{
    best = votes.end();
    uint32_t qualified = 0;
    for (auto it = votes.begin(); it != votes.end(); ++it) {
        const std::vector<uint8_t> bound(it->first.first.begin(), it->first.first.end());
        const auto vp = camera::LoadMatrix(bound, 0, camera::Layout::RowMajor);
        if (!wwii::IsRelativeSceneProjection(vp, width, height)) continue;
        const uint32_t draws = draws_of(it->second);
        qualified += draws;
        if (best == votes.end() || draws > draws_of(best->second)) {
            best = it;
            best_vp = vp;
        }
    }
    if (best == votes.end()) return votes.empty() ? SplitVerdict::NoVotes : SplitVerdict::NoProjection;
    // The world candidate must dominate qualified votes and cover a real
    // share of the scene. A small object drawn twice is not a world origin.
    const uint32_t draws = draws_of(best->second);
    if (draws < 2) return SplitVerdict::FewDraws;
    if (draws <= qualified / 2) return SplitVerdict::NoMajority;
    if (uint64_t(draws) * 8 < scene_draws) return SplitVerdict::SmallShare;
    return SplitVerdict::Chosen;
}

} // namespace na::native11
