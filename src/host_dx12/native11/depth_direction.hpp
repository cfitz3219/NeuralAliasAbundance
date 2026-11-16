// Which end of the scene depth is near, learned from how the game itself writes that depth.
//
// VARIANT: depth/direction-from-depth-writes
// Without a camera there is no projection to read the depth convention from, and DLSS, the flow
// expansion and the sharpening pass all need to know which value means "nothing drawn here". The
// evidence is the selected depth's own scene writes (Astra's correction 6 of 2026-11-16):
//  - a draw that tests the depth with LESS / LESS_EQUAL and WRITES it keeps the smaller value, so
//    smaller is nearer; GREATER / GREATER_EQUAL and a write says larger is nearer;
//  - the value the depth is cleared to before those draws is the far end (1 for smaller-is-nearer,
//    0 for larger-is-nearer).
// What is NOT conclusive on its own, and is never taken as proof:
//  - draws that only compare EQUAL (a second pass over a depth laid down earlier), and draws that
//    test without writing (marks, glass, outlines): they say nothing about which value wins;
//  - a clear value alone;
//  - writes that disagree with each other, unless the clear agrees with the side that makes up
//    most of them;
//  - writes that all agree while the clear says the opposite.
// A direction, once proven for a depth buffer, is kept across frames that prove nothing (a menu,
// a frame of EQUAL-only draws). It changes only when the opposite is proven on kFlipFrames frames
// in a row, and it is forgotten when another depth buffer is selected. Until one is proven nothing
// is assumed: the caller reports why and does not guess. The depth values themselves are never
// touched (raw z/w goes on as the game wrote it).
//
// Portable C++17 (Linux-tested, tests/cpu/depth_direction_test.cpp). One rule for both hosts: the
// DX11 provider fills it from its draw events, the DX12 host (j2/live_dlaa.cpp) from the sums the
// observe layer counts while asked (observe/producers.hpp DepthWriteEvidence).
#pragma once

#include <cstdint>

namespace na::native11 {

enum class DepthCompare : uint8_t { None = 0, NearIsLess = 1, NearIsGreater = 2 };

// D3D11_COMPARISON_FUNC values (the same numbers in D3D12 and in Vulkan's order + 1).
constexpr uint8_t kCompareLess = 2, kCompareEqual = 3, kCompareLessEqual = 4, kCompareGreater = 5,
                  kCompareGreaterEqual = 7;

// Which way a comparison says the depth runs when the draw also writes it.
constexpr DepthCompare CompareOf(uint8_t func)
{
    return func == kCompareLess || func == kCompareLessEqual ? DepthCompare::NearIsLess
         : func == kCompareGreater || func == kCompareGreaterEqual ? DepthCompare::NearIsGreater
         : DepthCompare::None;
}

// One frame's evidence for the selected depth, up to the point the scene is handed over.
struct DepthEvidence {
    uint32_t less = 0, greater = 0; // depth-tested, depth-WRITING draws by the way their comparison runs
    uint32_t equal = 0;             // such draws comparing EQUAL (prove nothing)
    uint32_t other = 0;             // such draws with ALWAYS / NEVER / NOT_EQUAL (prove nothing)
    uint32_t clears = 0;            // clears of the depth
    float clear = 0;                // the last clear's value
    bool clear_mixed = false;       // cleared to different values within the frame
};

enum class DepthVerdict : uint8_t {
    Proven,        // this frame's writes prove the direction
    Kept,          // this frame proves nothing; the direction proven earlier is kept
    NoWrites,      // unproven: no depth-writing draw with a directional comparison (EQUAL-only, or nothing)
    ClearOnly,     // unproven: only a clear value
    Conflict,      // unproven: writes disagree and the clear does not settle it
    ClearDisagrees // unproven: the writes agree with each other and the clear says the opposite
};

inline const char* DepthVerdictName(DepthVerdict v)
{
    switch (v) {
    case DepthVerdict::Proven: return "proven";
    case DepthVerdict::Kept: return "kept";
    case DepthVerdict::NoWrites: return "no_directional_writes";
    case DepthVerdict::ClearOnly: return "clear_only";
    case DepthVerdict::Conflict: return "conflicting_writes";
    case DepthVerdict::ClearDisagrees: return "clear_disagrees";
    }
    return "?";
}

// What one frame's evidence proves by itself (None = nothing), and why not.
inline DepthCompare ProveDepthDirection(const DepthEvidence& e, DepthVerdict* why = nullptr)
{
    const auto say = [&](DepthVerdict v, DepthCompare d) { if (why) *why = v; return d; };
    DepthCompare clear = DepthCompare::None;
    if (e.clears && !e.clear_mixed) {
        if (e.clear == 1.0f) clear = DepthCompare::NearIsLess;
        else if (e.clear == 0.0f) clear = DepthCompare::NearIsGreater;
    }
    if (!e.less && !e.greater) return say(e.clears ? DepthVerdict::ClearOnly : DepthVerdict::NoWrites, DepthCompare::None);
    if (e.less && e.greater) {
        // Writes disagree: only a clear that sides with at least three quarters of them settles it.
        const uint64_t all = uint64_t(e.less) + e.greater;
        if (clear == DepthCompare::NearIsLess && uint64_t(e.less) * 4 >= all * 3) return say(DepthVerdict::Proven, clear);
        if (clear == DepthCompare::NearIsGreater && uint64_t(e.greater) * 4 >= all * 3) return say(DepthVerdict::Proven, clear);
        return say(DepthVerdict::Conflict, DepthCompare::None);
    }
    const DepthCompare writes = e.less ? DepthCompare::NearIsLess : DepthCompare::NearIsGreater;
    if (clear != DepthCompare::None && clear != writes) return say(DepthVerdict::ClearDisagrees, DepthCompare::None);
    return say(DepthVerdict::Proven, writes);
}

class DepthDirection {
public:
    static constexpr uint32_t kFlipFrames = 3; // the opposite direction, proven this many frames in a row, replaces the kept one

    // Evidence, in the order the game's draws execute. `func` is the draw's depth comparison when it
    // tests AND writes the depth, 0 otherwise (frame_model.hpp Event::depth_func).
    void NoteWrite(uint8_t func)
    {
        if (!func) return;
        const DepthCompare d = CompareOf(func);
        if (d == DepthCompare::NearIsLess) ++frame_.less;
        else if (d == DepthCompare::NearIsGreater) ++frame_.greater;
        else if (func == kCompareEqual) ++frame_.equal;
        else ++frame_.other;
    }
    void NoteClear(float value)
    {
        if (frame_.clears && frame_.clear != value) frame_.clear_mixed = true;
        frame_.clear = value;
        ++frame_.clears;
    }
    // The same evidence, already summed by its gatherer (the DX12 host: observe's DepthWriteEvidence
    // counts the draws as they are recorded; the DX11 provider notes them one by one above).
    void NoteSummed(const DepthEvidence& e)
    {
        frame_.less += e.less; frame_.greater += e.greater; frame_.equal += e.equal; frame_.other += e.other;
        if (!e.clears) return;
        if (e.clear_mixed || (frame_.clears && frame_.clear != e.clear)) frame_.clear_mixed = true;
        frame_.clear = e.clear;
        frame_.clears += e.clears;
    }
    // Once per frame, when the scene is handed over. `depth` names the selected depth buffer (its
    // lifetime id): another buffer starts from nothing. Returns the direction to use, None while
    // none has been proven for this buffer.
    DepthCompare Decide(uint64_t depth, DepthVerdict* verdict = nullptr)
    {
        if (depth != depth_) {
            depth_ = depth;
            proven_ = DepthCompare::None;
            against_ = 0;
        }
        DepthVerdict why = DepthVerdict::NoWrites;
        const DepthCompare now = ProveDepthDirection(frame_, &why);
        last_ = frame_;
        if (now != DepthCompare::None) {
            if (proven_ == DepthCompare::None) proven_ = now;
            else if (now == proven_) against_ = 0;
            else if (++against_ >= kFlipFrames) {
                proven_ = now;
                against_ = 0;
                ++flips_;
            } else why = DepthVerdict::Kept; // the opposite, not yet for long enough
        } else if (proven_ != DepthCompare::None) {
            why = DepthVerdict::Kept;
        }
        if (verdict) *verdict = why;
        return proven_;
    }
    // The frame is over (Present): the next frame's evidence starts empty. What is proven stays.
    void EndFrame() { frame_ = {}; }
    void Reset() { *this = {}; }

    DepthCompare Proven() const { return proven_; }
    const DepthEvidence& Frame() const { return frame_; } // gathered so far this frame
    const DepthEvidence& Last() const { return last_; }   // what the last Decide saw
    uint64_t Flips() const { return flips_; }

private:
    DepthEvidence frame_{}, last_{};
    DepthCompare proven_ = DepthCompare::None;
    uint64_t depth_ = 0, flips_ = 0;
    uint32_t against_ = 0;
};

} // namespace na::native11
