#pragma once
// Referee: is the 3D picture still right when the game draws it small?
//
// A game's own draws are sent to smaller pictures (native11/world_raster.hpp). Most draws do not care.
// Some read a full-size picture of the frame (water bending what is behind it, soft particle edges,
// screen-space shading) and find their place in it from the pixel they are drawing; drawn small, they
// can read the wrong place. Nothing in the draw calls says which kind a shader is with certainty, so
// the Referee LOOKS: it has the game draw two frames at full size and makes them small afterwards (the
// comparison pictures), then one frame drawn small, and compares coarse grids of the three. Two frames
// drawn the same way differ only by what moved; the frame drawn small may differ by no more than that.
// A picture that fails twice in a row moves to the next way of giving draws their full-size inputs,
// and when every way has failed the world keeps its full size (the caller falls back and says why).
// A failed world is looked at again after a rest that grows each time: the scene that could not be drawn
// small (a menu background, one stretch of water) may be followed by one that can.
//
// Pure logic, no graphics calls: tests/cpu/referee_world_check_test.cpp.
#include <array>
#include <cstdint>
#include <cstdlib>

namespace na::referee {

constexpr uint32_t kWorldGridW = 64, kWorldGridH = 32, kWorldCells = kWorldGridW * kWorldGridH;
using WorldGrid = std::array<uint8_t, kWorldCells>; // brightness of each cell, 0..255

// How a draw that reads a full-size picture of the frame is given that picture while the world is small.
enum class WorldInputs : uint8_t {
    ByShader, // a shader that uses its pixel position gets the corner copy, any other the picture as it is
    AsDrawn,  // every shader gets the picture as it is (right for shaders that address it 0..1)
    Corner,   // every shader gets a full-size copy with the picture made small in its top-left corner
              // (right for shaders that address it by pixel position)
    kCount
};
constexpr const char* Name(WorldInputs p)
{
    return p == WorldInputs::ByShader ? "by-shader" : p == WorldInputs::AsDrawn ? "as-drawn" : p == WorldInputs::Corner ? "corner" : "?";
}

struct WorldMismatch { float fraction = 0; float mean = 0; };
// Cells whose brightness differs by more than kWorldCellStep count as different.
constexpr int kWorldCellStep = 20;
inline WorldMismatch CompareWorld(const WorldGrid& a, const WorldGrid& b)
{
    uint32_t differ = 0, sum = 0;
    for (uint32_t i = 0; i < kWorldCells; ++i) {
        const int d = std::abs(int(a[i]) - int(b[i]));
        sum += uint32_t(d);
        if (d > kWorldCellStep) ++differ;
    }
    return {float(differ) / float(kWorldCells), float(sum) / float(kWorldCells) / 255.f};
}

class WorldCheck {
public:
    enum class Frame : uint8_t { Small, Reference };
    enum class Verdict : uint8_t { None, Passed, PassedUnseen, Retry, NextInputs, Failed };
    // More than this share of cells differing between the two comparison pictures: the view is moving too
    // fast to judge anything; look again.
    static constexpr float kTooMuchMotion = 0.20f;
    // The small frame may differ from the comparison picture in this share of cells plus twice what the
    // two comparison pictures differ in.
    static constexpr float kAllowed = 0.05f;
    static constexpr uint32_t kStrikes = 2;      // failures in a row before the inputs are judged wrong
    static constexpr uint32_t kUnseenTries = 6;  // looks spoiled by motion before the small picture is let through unjudged
    static constexpr uint32_t kAwaitFrames = 40; // frames a look may wait for its pictures to come back
    // Frames a failed world rests at full size before it is looked at again; four times longer after each failure.
    static constexpr uint32_t kFirstRest = 1800, kLongestRest = 108000;

    // Starts a look with the current way of giving inputs (a new launch of the route, or a shader that reads
    // a full-size picture and has not been looked at yet).
    void Begin()
    {
        if (phase_ == Phase::Failed) return;
        phase_ = Phase::Ref0;
        tags_ = {};
        have_ = {};
        waited_ = 0;
    }
    // Another scene or another level: everything is to be looked at again, from the first way.
    void Reset() { *this = {}; }
    // One frame at full size after every way failed. True when the rest is over: everything is to be looked
    // at again from the first way, and the next rest (should it fail again) is four times longer.
    bool Rest()
    {
        if (phase_ != Phase::Failed || ++rested_ < rest_) return false;
        const uint32_t next = rest_ >= kLongestRest / 4 ? kLongestRest : rest_ * 4;
        *this = {};
        rest_ = next;
        return true;
    }
    uint32_t RestFrames() const { return rest_; }

    bool Looking() const { return phase_ != Phase::Unseen && phase_ != Phase::Steady && phase_ != Phase::Failed; }
    bool Steady() const { return phase_ == Phase::Steady; }
    bool Failed() const { return phase_ == Phase::Failed; }
    bool Unjudged() const { return unjudged_; } // let through without a verdict (motion): the caller looks again later
    WorldInputs Inputs() const { return inputs_; }
    uint32_t Looks() const { return looks_; }
    float LastFraction() const { return last_.fraction; }
    float LastMotion() const { return base_.fraction; }

    // How the coming frame's world is to be drawn. Only a frame drawn small can be wrong, so every frame of a
    // look other than the one being judged is drawn at full size and made small afterwards.
    Frame Next() const { return phase_ == Phase::Steady || phase_ == Phase::Test ? Frame::Small : Frame::Reference; }
    // Whether the coming frame's small picture is to be sampled (and its tag handed to Drawn).
    bool WantsSample() const { return phase_ == Phase::Ref0 || phase_ == Phase::Ref1 || phase_ == Phase::Test; }

    // The frame that just ended: how its world was really drawn, and the tag its small picture was sampled
    // under (0: not delivered small, or not sampled). policy_matters: some draw of it read a full-size picture.
    void Drawn(Frame as, uint64_t tag, bool policy_matters)
    {
        if (phase_ == Phase::Await && ++waited_ > kAwaitFrames) { Begin(); return; }
        if (!tag) return;
        if (phase_ == Phase::Ref0 && as == Frame::Reference) { tags_[0] = tag; phase_ = Phase::Ref1; }
        else if (phase_ == Phase::Ref1 && as == Frame::Reference) { tags_[1] = tag; phase_ = Phase::Test; }
        else if (phase_ == Phase::Test && as == Frame::Small) { tags_[2] = tag; policy_matters_ = policy_matters; phase_ = Phase::Await; waited_ = 0; }
    }

    // A sampled picture came back.
    Verdict Sampled(uint64_t tag, const WorldGrid& cells)
    {
        if (!tag || !Looking()) return Verdict::None;
        for (int i = 0; i < 3; ++i)
            if (tags_[i] == tag) { grids_[i] = cells; have_[i] = true; }
        if (phase_ != Phase::Await || !have_[0] || !have_[1] || !have_[2]) return Verdict::None;
        ++looks_;
        base_ = CompareWorld(grids_[0], grids_[1]);
        last_ = CompareWorld(grids_[1], grids_[2]);
        if (base_.fraction > kTooMuchMotion) {
            if (++unseen_ >= kUnseenTries) {
                unseen_ = 0;
                unjudged_ = true;
                phase_ = Phase::Steady;
                return Verdict::PassedUnseen;
            }
            Begin();
            return Verdict::Retry;
        }
        unseen_ = 0;
        if (last_.fraction <= kAllowed + 2.f * base_.fraction) {
            strikes_ = 0;
            unjudged_ = false;
            phase_ = Phase::Steady;
            return Verdict::Passed;
        }
        if (++strikes_ < kStrikes) { Begin(); return Verdict::Retry; }
        strikes_ = 0;
        // No draw read a full-size picture: the other ways would draw the very same frame.
        const auto next = static_cast<WorldInputs>(static_cast<uint8_t>(inputs_) + 1);
        if (!policy_matters_ || next == WorldInputs::kCount) {
            phase_ = Phase::Failed;
            return Verdict::Failed;
        }
        inputs_ = next;
        Begin();
        return Verdict::NextInputs;
    }

private:
    enum class Phase : uint8_t { Unseen, Ref0, Ref1, Test, Await, Steady, Failed };
    Phase phase_ = Phase::Unseen;
    WorldInputs inputs_ = WorldInputs::ByShader;
    std::array<uint64_t, 3> tags_{};
    std::array<bool, 3> have_{};
    std::array<WorldGrid, 3> grids_{};
    WorldMismatch base_{}, last_{};
    uint32_t strikes_ = 0, unseen_ = 0, waited_ = 0, looks_ = 0, rested_ = 0, rest_ = kFirstRest;
    bool policy_matters_ = false, unjudged_ = false;
};

// When redirecting may start, and when it must stop: the world's draws are first only COUNTED for a few
// frames (a rehearsal: which of them could be drawn small, which could not and why). Drawing small starts
// after enough clean frames in a row and goes back to counting when a frame is not clean; each fall asks
// for a longer clean run, so a game that keeps slipping does not flip between two ways of drawing.
class WorldRehearsal {
public:
    static constexpr uint32_t kFirst = 8, kLongest = 3600;
    void Frame(bool clean)
    {
        if (clean) { if (clean_ < kLongest) ++clean_; return; }
        if (Live()) need_ = need_ >= kLongest / 8 ? kLongest : need_ * 8;
        clean_ = 0;
    }
    bool Live() const { return clean_ >= need_; }
    uint32_t Clean() const { return clean_; }
    uint32_t Needed() const { return need_; }
    void Reset() { *this = {}; }

private:
    uint32_t clean_ = 0, need_ = kFirst;
};

} // namespace na::referee
