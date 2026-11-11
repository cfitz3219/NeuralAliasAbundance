#pragma once
// VARIANT: camera/relative
//
// The native DX11 side of the relative-camera vote. The rule (the key, the
// choice, the learning) is providers/camera/relative_camera.hpp and has no
// graphics API in it. This file only adds what is DX11's own: how a binding's
// first constant and count select a range of an upload.
//
// The names below predate the move (the vote began as the "split camera" at the
// places Call of Duty WWII uses). They are kept so the provider and the replay
// tests read as before; with no layout given they use the WWII seed.

#include "host_dx12/adapters/wwii/scene_camera.hpp"
#include "providers/camera/relative_camera.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace na::native11 {

using SplitKey = camera::RelativeKey;
using SplitVerdict = camera::RelativeVerdict;
inline const char* SplitVerdictName(SplitVerdict v) { return camera::RelativeVerdictName(v); }

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

// The identity of one draw's camera evidence under `layout`. False when the draw is not a vote.
// Pass a fresh key; it is only meaningful when this returns true.
inline bool MakeSplitKey(const camera::RelativeLayout& layout, const uint8_t* world, size_t world_size,
                         uint32_t world_first, uint32_t world_count, const uint8_t* projection,
                         size_t projection_size, uint32_t projection_first, uint32_t projection_count, SplitKey& key)
{
    size_t world_start = 0, world_length = 0, projection_start = 0, projection_length = 0;
    if (!SplitBoundRange(world_size, world_first, world_count, world_start, world_length) ||
        !SplitBoundRange(projection_size, projection_first, projection_count, projection_start, projection_length))
        return false;
    return camera::MakeRelativeKey(layout, world + world_start, world_length, projection + projection_start,
                                   projection_length, key);
}
inline bool MakeSplitKey(const uint8_t* world, size_t world_size, uint32_t world_first, uint32_t world_count,
                         const uint8_t* projection, size_t projection_size, uint32_t projection_first,
                         uint32_t projection_count, SplitKey& key)
{
    return MakeSplitKey(wwii::RelativeSeedLayout(), world, world_size, world_first, world_count, projection,
                        projection_size, projection_first, projection_count, key);
}

template <class Votes, class DrawsOf>
SplitVerdict ChooseSplitCamera(const camera::RelativeLayout& layout, const Votes& votes, DrawsOf&& draws_of,
                               uint32_t width, uint32_t height, uint64_t scene_draws,
                               typename Votes::const_iterator& best, camera::Matrix& best_vp)
{
    return camera::ChooseRelativeCamera(layout, votes, draws_of, width, height, scene_draws, best, best_vp);
}
template <class Votes, class DrawsOf>
SplitVerdict ChooseSplitCamera(const Votes& votes, DrawsOf&& draws_of, uint32_t width, uint32_t height,
                               uint64_t scene_draws, typename Votes::const_iterator& best, camera::Matrix& best_vp)
{
    return camera::ChooseRelativeCamera(wwii::RelativeSeedLayout(), votes, draws_of, width, height, scene_draws, best,
                                        best_vp);
}

} // namespace na::native11
