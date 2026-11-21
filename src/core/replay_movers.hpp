#pragma once
// Which draws draw replay keeps replaying (Cort 2026-11-17: "Yes on one"; his Sniper Elite
// Resistance runs showed replay expanding about 3 million vertices every frame for a tenth of the
// screen, and heavy areas turning choppy).
// Replay earns its cost only on things that move by themselves: a still scene under a moving
// camera is already described by the screen's own motion (the camera's reprojection, or the fitted
// screen motion when there is no camera). So the replay pass counts, per replayed draw, the
// sampled pixels where the draw is the visible surface and, of those, the ones whose replayed
// motion is apart from the screen's motion there. This file is the rule on those counts, with no
// game in it:
//   - a frame where enough of a draw's visible samples are apart is a moving frame for that draw;
//   - a moving frame adds `up` to the draw's score, a visible still frame takes 1 away, and a frame
//     where the draw was replayed but not seen takes 1 away only every `unseen_every` frames
//     (not being seen is not evidence of being still);
//   - a draw becomes a mover at `promote` (three moving frames close together) and stays one until
//     its score is back at 0 (`top` visible still frames at most: half a minute at 60 fps);
//   - a frame with no usable screen motion to compare with teaches nothing (the caller does not
//     report it).
// The admission (host_dx12/observe/producers.cpp) replays known movers first and spends a small
// rotating share of the vertex budget (core/replay_budget.hpp) on every other draw, which is how a
// draw is first seen moving, and how a mover that was let go is found again.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace na::replay_movers {

struct Params {
    uint32_t apart_min = 2;        // sampled pixels apart from the screen's motion a moving frame needs
    uint32_t apart_share_pct = 5;  // and their share of the draw's sampled visible pixels, in percent
    uint32_t up = 300;             // what a moving frame adds
    uint32_t promote = 900;        // the score of a mover
    uint32_t top = 1800;           // the score's ceiling
    uint32_t unseen_every = 4;     // replayed-but-unseen frames that cost one point
    size_t capacity = 4096;        // draws remembered; the least recently reported go first, movers last
};

enum class Seen : uint32_t { Unseen, Still, Moving };

inline Seen Judge(uint32_t visible, uint32_t apart, const Params& p)
{
    if (!visible) return Seen::Unseen;
    if (apart >= p.apart_min && uint64_t(apart) * 100 >= uint64_t(visible) * p.apart_share_pct) return Seen::Moving;
    return Seen::Still;
}

struct State {
    uint32_t score = 0, unseen = 0;
    bool mover = false;
    uint64_t reported = 0; // the caller's frame number of the last report
};

enum class Change : uint32_t { None, Promoted, LetGo };

// One frame's report for one draw.
inline Change Step(State& s, Seen seen, const Params& p)
{
    switch (seen) {
    case Seen::Moving:
        s.unseen = 0;
        s.score = s.score + p.up > p.top ? p.top : s.score + p.up;
        break;
    case Seen::Still:
        s.unseen = 0;
        if (s.score) --s.score;
        break;
    case Seen::Unseen:
        if (++s.unseen >= (p.unseen_every ? p.unseen_every : 1)) { s.unseen = 0; if (s.score) --s.score; }
        break;
    }
    if (!s.mover && s.score >= p.promote) { s.mover = true; return Change::Promoted; }
    if (s.mover && !s.score) { s.mover = false; return Change::LetGo; }
    return Change::None;
}

// The draws remembered, by the identity replay matches on. Not thread safe: the caller locks.
class Table {
public:
    explicit Table(const Params& p = Params()) : p_(p) {}
    bool IsMover(uint64_t sig, uint64_t vb) const
    {
        const auto it = map_.find(Key{sig, vb});
        return it != map_.end() && it->second.mover;
    }
    Change Report(uint64_t sig, uint64_t vb, uint32_t visible, uint32_t apart, uint64_t frame)
    {
        const Seen seen = Judge(visible, apart, p_);
        const Key key{sig, vb};
        auto it = map_.find(key);
        if (it == map_.end()) {
            if (seen != Seen::Moving) return Change::None; // nothing to remember about a draw never seen moving
            if (map_.size() >= p_.capacity) Trim();
            it = map_.emplace(key, State()).first;
        }
        State& s = it->second;
        s.reported = frame;
        const Change c = Step(s, seen, p_);
        if (c == Change::Promoted) { ++movers_; ++promoted_; }
        if (c == Change::LetGo) { --movers_; ++let_go_; }
        if (!s.score) map_.erase(it); // still again: forgotten, the rotating share will try it later
        return c;
    }
    void Clear() { map_.clear(); movers_ = 0; }
    size_t Movers() const { return movers_; }
    size_t Suspects() const { return map_.size() - movers_; } // seen moving, not three times yet
    uint64_t Promoted() const { return promoted_; }
    uint64_t LetGo() const { return let_go_; }

private:
    struct Key {
        uint64_t sig, vb;
        bool operator==(const Key& o) const { return sig == o.sig && vb == o.vb; }
    };
    struct Hash {
        size_t operator()(const Key& k) const { return static_cast<size_t>(k.sig ^ (k.vb * 0x9e3779b97f4a7c15ull)); }
    };
    // Full: drop the quarter reported longest ago, draws that are not movers before movers.
    void Trim()
    {
        std::vector<std::pair<uint64_t, Key>> order; // (mover, age) packed so that a plain sort puts the first to go first
        order.reserve(map_.size());
        uint64_t newest = 0;
        for (const auto& e : map_) if (e.second.reported > newest) newest = e.second.reported;
        for (const auto& e : map_) {
            const uint64_t age = newest - e.second.reported;
            const uint64_t keep = (e.second.mover ? (1ull << 62) : 0) + ((1ull << 62) - 1 - (age < (1ull << 62) - 1 ? age : (1ull << 62) - 1));
            order.push_back({keep, e.first});
        }
        const size_t drop = map_.size() / 4 ? map_.size() / 4 : 1;
        std::partial_sort(order.begin(), order.begin() + drop, order.end(),
                          [](const std::pair<uint64_t, Key>& a, const std::pair<uint64_t, Key>& b) { return a.first < b.first; });
        for (size_t i = 0; i < drop; ++i) {
            const auto it = map_.find(order[i].second);
            if (it == map_.end()) continue;
            if (it->second.mover) --movers_;
            map_.erase(it);
        }
    }
    Params p_;
    std::unordered_map<Key, State, Hash> map_;
    size_t movers_ = 0;
    uint64_t promoted_ = 0, let_go_ = 0;
};

} // namespace na::replay_movers
