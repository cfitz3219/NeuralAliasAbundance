// The relative camera: for games that render relative to the camera.
//
// Such a game uploads a view-projection with the camera position removed, so
// the matrix gives direction and lens but not where the camera is. The position
// is witnessed by the world transforms of unrotated, unscaled objects: each is
// a pure translation of (object position - camera position), and for static
// world geometry authored in place that is exactly minus the camera position.
// Many draws share it; a prop with its own offset does not.
//
// A scene draw votes when it binds both: the projection and a world range that
// holds such a transform. Draws that agree on both form one vote. At the scene
// boundary the vote with the most draws wins if it covers enough of the scene.
//
// WHERE the two sit (which binding, which offset, which storage) is a
// RelativeLayout. It is learned from what the draws bind (RelativeLearner), or
// seeded by an adapter that already knows one game's places; either way every
// draw is checked by content, so a layout that does not fit simply gives no vote.
//
// No graphics API in here. A provider reports bound ranges; this file owns the
// key, the choice and the learning.
//
// History: the vote was written for Call of Duty WWII on native DX11 at fixed
// places (world: slot 0, 160 bytes, transforms at 0 and 96; projection: slot 1,
// 64 bytes). Those places are now the seed in adapters/wwii, and the rule is
// here for every game (2026-11-11).
//
// Portable C++17 (Linux-tested).
#pragma once
#include "providers/camera/camera_admission.hpp"

#include <array>
#include <cstring>
#include <map>
#include <tuple>
#include <utility>

namespace na::camera {

// ---------------------------------------------------------------- the witness
// How an identity-basis world transform is stored.
enum class WitnessForm : uint8_t {
    Column4x4, // translation in the 4th column (floats 3, 7, 11), last row 0 0 0 1
    Row4x4,    // translation in the 4th row (floats 12, 13, 14), last column 0 0 0 1
    Column3x4, // three rows only, translation in the 4th column
};
struct WitnessPlace {
    uint32_t offset = 0;
    WitnessForm form = WitnessForm::Column4x4;
};
inline bool operator==(const WitnessPlace& a, const WitnessPlace& b) { return a.offset == b.offset && a.form == b.form; }
inline size_t WitnessBytes(WitnessForm form) { return form == WitnessForm::Column3x4 ? 48 : 64; }

// The camera position this transform witnesses (minus its translation). False
// when the bytes at `place` are not an identity-basis transform.
inline bool ReadWitness(const uint8_t* bytes, size_t size, const WitnessPlace& place, double* origin)
{
    const size_t length = WitnessBytes(place.form);
    if (!bytes || place.offset > size || size - place.offset < length) return false;
    std::array<float, 16> m{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    std::memcpy(m.data(), bytes + place.offset, length);
    for (size_t axis = 0; axis < 3; ++axis) {
        const size_t translation = place.form == WitnessForm::Row4x4 ? 12 + axis : axis * 4 + 3;
        if (!std::isfinite(m[translation])) return false;
        origin[axis] = -double(m[translation]);
        m[translation] = 0.f;
    }
    constexpr std::array<float, 16> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    return m == identity;
}

// ---------------------------------------------------------------- the layout
struct RelativeLayout {
    // The per-object range: its binding as the shaders see it. A bound length of 0 means any
    // length (every draw is checked by content); a seed may pin the one length it knows.
    uint64_t world_binding = 0;
    uint32_t world_size = 0;
    uint32_t witnesses = 0;    // 1 or 2 places; the first is this frame's, the second last frame's
    WitnessPlace witness[2]{};
    // The projection range.
    uint64_t projection_binding = 0;
    uint32_t projection_size = 0;
    uint32_t projection_offset = 0;
    Layout projection_layout = Layout::RowMajor;
    // Other places of the block that held the very same matrix when the layout was learned. They
    // must keep holding it: a place that was only equal while the camera stood still (last frame's
    // projection) then shows itself, and the draw gives no vote instead of a camera one frame late.
    uint32_t projection_copies = 0;
    uint32_t projection_copy[3]{};
    bool reversed = true;      // the depth direction it had when learned; a game does not flip it
    bool valid() const { return witnesses != 0 && projection_size >= 64; }
};

inline Matrix LoadMatrixAt(const uint8_t* bytes, Layout layout)
{
    Matrix m{};
    for (size_t r = 0; r < 4; ++r)
        for (size_t col = 0; col < 4; ++col) {
            float v{};
            std::memcpy(&v, bytes + (layout == Layout::RowMajor ? r * 4 + col : col * 4 + r) * 4, 4);
            m[r * 4 + col] = v;
        }
    return m;
}

// A camera-relative projection that fills the scene picture.
inline bool IsRelativeSceneProjection(const Matrix& vp, uint32_t width, uint32_t height, bool& reversed)
{
    Candidate c{};
    if (!ViewProjection(vp, c) || !MatchesSceneAspect(vp, width, height) || !IsCameraRelative(vp)) return false;
    reversed = c.reversed;
    return true;
}

// ---------------------------------------------------------------- the vote
// Projection bytes exactly as bound at the layout's place, and the positions
// witnessed with them (this frame's, then last frame's when the layout has both).
using RelativeOrigins = std::array<double, 6>;
using RelativeKey = std::pair<std::array<uint8_t, 64>, RelativeOrigins>;

// The identity of one draw's camera evidence. False when the draw is not a vote.
// `world` and `projection` are the bound ranges (not the whole buffers).
inline bool MakeRelativeKey(const RelativeLayout& layout, const uint8_t* world, size_t world_size,
                            const uint8_t* projection, size_t projection_size, RelativeKey& key)
{
    if (!layout.valid() || (layout.world_size && world_size != layout.world_size) ||
        projection_size != layout.projection_size || layout.projection_offset > projection_size ||
        projection_size - layout.projection_offset < 64) return false;
    for (uint32_t i = 0; i < layout.projection_copies && i < 3; ++i)
        if (layout.projection_copy[i] > projection_size || projection_size - layout.projection_copy[i] < 64 ||
            std::memcmp(projection + layout.projection_offset, projection + layout.projection_copy[i], 64) != 0) return false;
    key.second = {};
    for (uint32_t i = 0; i < layout.witnesses && i < 2; ++i)
        if (!ReadWitness(world, world_size, layout.witness[i], key.second.data() + 3 * i)) return false;
    if (key.second[0] == 0 && key.second[1] == 0 && key.second[2] == 0) return false; // no translation: witnesses nothing
    std::memcpy(key.first.data(), projection + layout.projection_offset, key.first.size());
    return true;
}

// Why a frame's votes did or did not give a camera. Logged, so a run says WHY.
enum class RelativeVerdict : uint8_t {
    Chosen,
    NoVotes,      // no scene draw bound a witnessing world range and the projection range together
    NoProjection, // votes exist, none holds a camera-relative scene projection for this extent
    FewDraws,     // the leading vote has fewer than 2 draws
    NoMajority,   // the leading vote does not hold more than half of the qualified draws
    SmallShare,   // the leading vote covers less than 1/8 of the frame's scene draws
};
inline const char* RelativeVerdictName(RelativeVerdict v)
{
    switch (v) {
    case RelativeVerdict::Chosen: return "chosen";
    case RelativeVerdict::NoVotes: return "no_votes";
    case RelativeVerdict::NoProjection: return "no_projection";
    case RelativeVerdict::FewDraws: return "few_draws";
    case RelativeVerdict::NoMajority: return "no_majority";
    case RelativeVerdict::SmallShare: return "small_share";
    }
    return "?";
}

// Chooses among `votes` (an ordered map keyed by RelativeKey; `draws_of` reads a
// vote's draw count). `best` is the leading qualified vote and `best_vp` its
// projection whenever one exists, also when the verdict is a rejection.
// Ties keep the first vote in key order.
template <class Votes, class DrawsOf>
RelativeVerdict ChooseRelativeCamera(const RelativeLayout& layout, const Votes& votes, DrawsOf&& draws_of,
                                     uint32_t width, uint32_t height, uint64_t scene_draws,
                                     typename Votes::const_iterator& best, Matrix& best_vp)
{
    best = votes.end();
    uint32_t qualified = 0;
    for (auto it = votes.begin(); it != votes.end(); ++it) {
        const Matrix vp = LoadMatrixAt(it->first.first.data(), layout.projection_layout);
        bool reversed = false;
        if (!IsRelativeSceneProjection(vp, width, height, reversed) || reversed != layout.reversed) continue;
        const uint32_t draws = draws_of(it->second);
        qualified += draws;
        if (best == votes.end() || draws > draws_of(best->second)) {
            best = it;
            best_vp = vp;
        }
    }
    if (best == votes.end()) return votes.empty() ? RelativeVerdict::NoVotes : RelativeVerdict::NoProjection;
    // The world candidate must dominate qualified votes and cover a real
    // share of the scene. A small object drawn twice is not a world origin.
    const uint32_t draws = draws_of(best->second);
    if (draws < 2) return RelativeVerdict::FewDraws;
    if (draws <= qualified / 2) return RelativeVerdict::NoMajority;
    if (uint64_t(draws) * 8 < scene_draws) return RelativeVerdict::SmallShare;
    return RelativeVerdict::Chosen;
}

// ---------------------------------------------------------------- learning
// Constant registers are 16 bytes, so matrices start at 16-byte steps.
constexpr size_t kProjectionScanBytes = 4096; // of a block, searched for the projection
constexpr size_t kWitnessScanBytes = 512;     // a per-object range larger than this is not searched

// The camera-relative scene projections in a block: how many different matrices,
// where the first one sits, and which other places hold that same matrix. A block
// can hold this frame's projection beside last frame's or a second view's; only a
// count of one says which is the camera.
inline uint32_t FindRelativeProjections(const uint8_t* bytes, size_t size, uint32_t width, uint32_t height,
                                        RelativeLayout& found)
{
    if (!bytes) return 0;
    uint32_t different = 0;
    found.projection_copies = 0;
    const size_t end = size < kProjectionScanBytes ? size : kProjectionScanBytes;
    for (size_t at = 0; at + 64 <= end; at += 16)
        for (const Layout l : {Layout::RowMajor, Layout::ColumnMajor}) {
            bool reversed = false;
            if (!IsRelativeSceneProjection(LoadMatrixAt(bytes + at, l), width, height, reversed)) continue;
            if (!different) {
                found.projection_offset = static_cast<uint32_t>(at);
                found.projection_layout = l;
                found.reversed = reversed;
                different = 1;
            } else if (l != found.projection_layout || std::memcmp(bytes + at, bytes + found.projection_offset, 64) != 0) {
                ++different;
            } else if (found.projection_copies < 3) {
                found.projection_copy[found.projection_copies++] = static_cast<uint32_t>(at);
            } else {
                ++different; // more copies than a layout can watch: not a block to learn from
            }
            break; // one reading per place
        }
    return different;
}

// The identity-basis transforms in a per-object range, lowest offset first, at
// most two. The first must carry a translation (that is what makes its storage
// form certain); a second one is read in the same form. Returns how many.
inline uint32_t FindWitnessPlaces(const uint8_t* bytes, size_t size, WitnessPlace* places)
{
    if (!bytes || size > kWitnessScanBytes) return 0;
    uint32_t found = 0;
    for (size_t at = 0; at + 48 <= size && found < 2;) {
        bool hit = false;
        for (const WitnessForm form : {WitnessForm::Column4x4, WitnessForm::Row4x4, WitnessForm::Column3x4}) {
            if (found && form != places[0].form) continue;
            const WitnessPlace place{static_cast<uint32_t>(at), form};
            double origin[3]{};
            if (!ReadWitness(bytes, size, place, origin)) continue;
            if (!found && origin[0] == 0 && origin[1] == 0 && origin[2] == 0) continue;
            places[found++] = place;
            at += WitnessBytes(form);
            hit = true;
            break;
        }
        if (!hit) at += 16;
    }
    return found;
}

// Learns a RelativeLayout over two scene frames:
//  frame 1, at its boundary: FindProjection looks for a camera-relative scene
//    projection in the ranked blocks. Finding one says this is a camera-relative
//    game and arms the tally.
//  frame 2, per scene draw that binds that projection range: NoteDraw looks for
//    identity-basis transforms in the draw's small vertex ranges and tallies
//    where they sit.
//  frame 2, at its boundary: Finish adopts the places most draws agree on, if
//    they cover the scene the way a vote must.
// The per-draw search runs only while armed, so a learned game pays nothing.
class RelativeLearner {
public:
    bool armed() const { return armed_; }
    // Draws take part in the tally only when they bind this range.
    uint64_t projection_binding() const { return layout_.projection_binding; }
    uint32_t projection_size() const { return layout_.projection_size; }

    // `blocks`: the ranges the scene's draws bound, best-ranked first; slot.binding and size identify each.
    // Only a block that holds exactly one such projection (copies aside) is taken: there is no doubt
    // which matrix is this frame's camera. `ambiguous()` says blocks with several were passed over.
    bool FindProjection(const std::vector<AdmissionBlock>& blocks, uint32_t width, uint32_t height)
    {
        tally_.clear();
        armed_ = ambiguous_ = false;
        for (const auto& b : blocks) {
            if (b.size < 64) continue;
            RelativeLayout l;
            const uint32_t n = FindRelativeProjections(b.bytes, b.size, width, height, l);
            if (n > 1) ambiguous_ = true;
            if (n != 1) continue;
            l.projection_binding = b.slot.binding;
            l.projection_size = static_cast<uint32_t>(b.size);
            layout_ = l;
            armed_ = true;
            return true;
        }
        return false;
    }
    bool ambiguous() const { return ambiguous_; }
    void NoteDraw(uint64_t world_binding, const uint8_t* bytes, size_t size)
    {
        if (!armed_ || world_binding == layout_.projection_binding) return;
        WitnessPlace places[2]{};
        const uint32_t n = FindWitnessPlaces(bytes, size, places);
        if (!n) return;
        ++tally_[Signature{world_binding, n, places[0].offset, places[1].offset, static_cast<uint8_t>(places[0].form)}];
    }
    // Ends the tally. True with the learned layout when one signature has at least 2 draws
    // and an eighth of the scene's draws (the vote's own share rule).
    bool Finish(uint64_t scene_draws, RelativeLayout& out)
    {
        const bool was_armed = armed_;
        armed_ = false;
        const Signature* best = nullptr;
        uint32_t best_draws = 0;
        for (const auto& kv : tally_)
            if (kv.second > best_draws) { best = &kv.first; best_draws = kv.second; }
        bool learned = false;
        if (was_armed && best && best_draws >= 2 && uint64_t(best_draws) * 8 >= scene_draws) {
            out = layout_;
            out.world_binding = std::get<0>(*best);
            out.world_size = 0; // any length under that binding: each draw is checked by content
            out.witnesses = std::get<1>(*best);
            const auto form = static_cast<WitnessForm>(std::get<4>(*best));
            out.witness[0] = {std::get<2>(*best), form};
            out.witness[1] = {std::get<3>(*best), form};
            learned = true;
        }
        tally_.clear();
        return learned;
    }

private:
    // world binding, places, first offset, second offset, storage form
    using Signature = std::tuple<uint64_t, uint32_t, uint32_t, uint32_t, uint8_t>;
    bool armed_ = false, ambiguous_ = false;
    RelativeLayout layout_{};
    std::map<Signature, uint32_t> tally_;
};

} // namespace na::camera
