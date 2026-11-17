// VARIANT: flow/frame-candidates
// The block search's answers, reconsidered against the frame's own most common answers, before
// anything reads them. This is the CPU reference of the `cand_*` programs in
// host_dx12/j2/flow_shaders.hpp (kCandidateShader); the two are kept in step by hand: same
// candidates, same samples, same order of decisions, same constants.
//
// Why (Astra's audit R3 and its limit 2, 2026-11-17): the search works down a pyramid and looks only
// 8 pixels around what the coarser level said. On a picture that nearly repeats (a fence, tiles, a
// formation, a wave pattern) a coarse level can lock onto the repeat, and the fine level then never
// compares the true vector at all. On the RTX card, 1920x1080, a 6 pixel pan of the four-sine test
// picture left 12 % of the pixels right; 47 % were wrong by more than 64 pixels and still passed
// as measured. The camera fit refused the same frame (most samples were vectors no camera explains).
//
// What it does: every block that the search did get right votes for its vector. The few vectors
// with the most votes are offered to every block, and a block takes one only when it explains the
// two pictures clearly better than the block's own vector. So the fine comparison the search never
// made is made, for a handful of vectors, at full resolution.
//
// What it keeps (Astra's conditions):
//  - A block the search called still stays still. Stillness is the per-pixel measure's business.
//  - A block's own vector stays unless a candidate is clearly better ON THE PICTURES. Votes only
//    nominate; they never decide. A moving object keeps its vector: the background's candidate does
//    not explain the object's block.
//  - The most common vector is not trusted for being common. On the card the reverse pan's
//    strongest peak was a wrong (-622, 0) on 20,003 blocks against the true vector's 2,740.
//  - A candidate closer than the search's own reach (8 pixels) to the block's vector is not
//    offered: the search compared those itself, with its own rule.
//  - On a scene cut the search's field is all zero, so nothing votes and nothing changes.
//  - No camera is read, and no vector is invented: a block ends with its own vector or with one
//    that other blocks of this same pair measured.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace na::motion::candidates {

struct Vector {
    int x = 0, y = 0;
};

// Row-major luma in [0,1]; both pictures of the pair, in the unjittered frame the search was given.
struct Picture {
    const float* luma = nullptr;
    uint32_t width = 0, height = 0;
};

constexpr int kWindow = 127;       // vectors up to this many pixels on each axis can vote (8 bits each in the packed key)
constexpr int kReach = 8;          // the search's own reach at full resolution: nearer candidates are not offered
constexpr uint32_t kCount = 8;     // candidates kept
constexpr float kRatio = 0.8f;     // a candidate must leave at most this share of the own vector's error ...
constexpr float kGain = 0.001f;    // ... and be better by at least this much mean |luma| (one step of the 10-bit pictures)
constexpr uint32_t kBlock = 8;     // pixels per block, each axis

// A vector needs this many blocks behind it to be offered: 1 % of the frame's blocks, 8 at least.
constexpr uint32_t MinimumSupport(uint32_t blocks) { return (std::max)(8u, blocks / 100u); }

constexpr bool Votes(Vector v) { return (v.x != 0 || v.y != 0) && v.x >= -kWindow && v.x <= kWindow && v.y >= -kWindow && v.y <= kWindow; }
// Support (capped at 65535) above the vector's key: the order candidates are ranked in, and unique per vector.
constexpr uint32_t Pack(Vector v, uint32_t support) { return ((std::min)(support, 65535u) << 16) | (uint32_t(v.y + 128) << 8) | uint32_t(v.x + 128); }
constexpr Vector Unpack(uint32_t packed) { return Vector{int(packed & 255u) - 128, int((packed >> 8) & 255u) - 128}; }

struct Result {
    uint32_t top[kCount]{}; // packed, strongest first; 0 = none
    uint32_t found = 0;     // candidates
    uint32_t offered = 0;   // blocks that had at least one candidate to compare
    uint32_t changed = 0;   // blocks that took a candidate
};

// The shader's SampleLevel with a clamp sampler: bilinear, texel centres at +0.5.
inline float Sample(const Picture& im, float x, float y)
{
    const float fx = std::clamp(x - 0.5f, 0.0f, float(im.width - 1));
    const float fy = std::clamp(y - 0.5f, 0.0f, float(im.height - 1));
    const uint32_t x0 = static_cast<uint32_t>(fx), y0 = static_cast<uint32_t>(fy);
    const uint32_t x1 = (std::min)(x0 + 1, im.width - 1), y1 = (std::min)(y0 + 1, im.height - 1);
    const float tx = fx - float(x0), ty = fy - float(y0);
    const float* r0 = im.luma + size_t(y0) * im.width;
    const float* r1 = im.luma + size_t(y1) * im.width;
    return (r0[x0] * (1 - tx) + r0[x1] * tx) * (1 - ty) + (r1[x0] * (1 - tx) + r1[x1] * tx) * ty;
}

// Mean |luma| difference between block (bx, by) of the current picture and the previous picture
// moved by (vx, vy).
inline float BlockError(const Picture& current, const Picture& previous, uint32_t bx, uint32_t by, float vx, float vy)
{
    float e = 0, n = 0;
    for (uint32_t j = 0; j < kBlock; ++j)
        for (uint32_t i = 0; i < kBlock; ++i) {
            const uint32_t x = bx * kBlock + i, y = by * kBlock + j;
            if (x >= current.width || y >= current.height) continue;
            e += std::fabs(current.luma[size_t(y) * current.width + x] - Sample(previous, float(x) + 0.5f + vx, float(y) + 0.5f + vy));
            n += 1;
        }
    return e / (std::max)(n, 1.0f);
}

// A whole-pixel vector judged fairly: motion is rarely a whole number of pixels, so the vector is
// tried at itself and at the eight half-pixel places around it, and the best of the nine counts.
inline float BestOfNine(const Picture& current, const Picture& previous, uint32_t bx, uint32_t by, Vector v)
{
    float least = 1e9f;
    for (int sj = -1; sj <= 1; ++sj)
        for (int si = -1; si <= 1; ++si) least = (std::min)(least, BlockError(current, previous, bx, by, float(v.x) + 0.5f * float(si), float(v.y) + 0.5f * float(sj)));
    return least;
}

// The strongest vectors of the grid (cand_count + cand_pick).
inline Result Pick(const Vector* grid, uint32_t gw, uint32_t gh)
{
    std::vector<uint32_t> votes(size_t(256) * 256, 0);
    const size_t blocks = size_t(gw) * gh;
    for (size_t i = 0; i < blocks; ++i)
        if (Votes(grid[i])) ++votes[size_t(grid[i].y + 128) * 256 + size_t(grid[i].x + 128)];
    const uint32_t minimum = MinimumSupport(uint32_t(blocks));
    Result r;
    for (uint32_t round = 0; round < kCount; ++round) {
        uint32_t best = 0;
        for (size_t i = 0; i < blocks; ++i) {
            if (!Votes(grid[i])) continue;
            const uint32_t n = votes[size_t(grid[i].y + 128) * 256 + size_t(grid[i].x + 128)];
            if (n < minimum) continue;
            const uint32_t packed = Pack(grid[i], n);
            bool taken = false;
            for (uint32_t j = 0; j < round; ++j) taken = taken || r.top[j] == packed;
            if (!taken) best = (std::max)(best, packed);
        }
        if (!best) break;
        r.top[round] = best;
        ++r.found;
    }
    return r;
}

// One block's decision (cand_apply). `offered`: the block had a candidate to compare.
inline Vector Decide(const Result& top, Vector own, const Picture& current, const Picture& previous, uint32_t bx, uint32_t by, bool* offered = nullptr)
{
    if (offered) *offered = false;
    if (own.x == 0 && own.y == 0) return own;
    const int lo_x = int(bx * kBlock), lo_y = int(by * kBlock);
    const int hi_x = (std::min)(lo_x + int(kBlock), int(current.width)), hi_y = (std::min)(lo_y + int(kBlock), int(current.height));
    float least = 1e9f;
    Vector best = own;
    bool found = false;
    for (uint32_t k = 0; k < kCount && top.top[k]; ++k) {
        const Vector c = Unpack(top.top[k]);
        if ((std::max)(std::abs(c.x - own.x), std::abs(c.y - own.y)) <= kReach) continue;
        if (lo_x + c.x < 0 || lo_y + c.y < 0 || hi_x + c.x > int(current.width) || hi_y + c.y > int(current.height)) continue; // the whole block must come from inside the picture
        const float e = BlockError(current, previous, bx, by, float(c.x), float(c.y));
        if (e < least) { least = e; best = c; found = true; }
    }
    if (!found) return own;
    if (offered) *offered = true;
    // Exact shortcut (Astra, COMMS dc69b00a): the own vector's best of nine is never above its
    // whole-pixel error, and no error is below zero, so under kGain no candidate can be better by kGain.
    if (BlockError(current, previous, bx, by, float(own.x), float(own.y)) < kGain) return own;
    const float e_own = BestOfNine(current, previous, bx, by, own), e_best = BestOfNine(current, previous, bx, by, best);
    return e_best <= kRatio * e_own && e_own - e_best >= kGain ? best : own;
}

// The whole pass. `out` may be `grid`.
inline Result Reconsider(const Vector* grid, uint32_t gw, uint32_t gh, const Picture& current, const Picture& previous, Vector* out)
{
    Result r = Pick(grid, gw, gh);
    for (uint32_t by = 0; by < gh; ++by)
        for (uint32_t bx = 0; bx < gw; ++bx) {
            const Vector own = grid[size_t(by) * gw + bx];
            bool offered = false;
            const Vector chosen = Decide(r, own, current, previous, bx, by, &offered);
            if (offered) ++r.offered;
            if (chosen.x != own.x || chosen.y != own.y) ++r.changed;
            out[size_t(by) * gw + bx] = chosen;
        }
    return r;
}

} // namespace na::motion::candidates
