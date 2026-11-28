#pragma once
// Scale hunt: the portable parts of the Referee's search for a game's render-scale command
// (Cort 2026-11-28). Three pieces, no Windows or graphics API in here:
//
//   1. Memory matchers. What a render scale, or the render size it produces, looks like in a
//      block of the game's memory: the world's width and height side by side (whole numbers or
//      floats), a fraction equal to world/output, or a whole percent.
//   2. Board. Every search method reports what it found to one board. The same address found by
//      several methods, or seen following the world through a size change, scores higher. The
//      Referee decides from the board, not from any single method.
//   3. Response judge. After a command: did the game respond (and to what size: games may snap to
//      their own steps), did it ignore us, did it revert, and how often must the command be
//      repeated (once / on each revert / on a learned period / every frame).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

namespace na::scale::hunt {

// ---------------------------------------------------------------- 1. memory matchers
enum class Kind : uint8_t {
    SizePairI32, // world width then height, 32-bit whole numbers
    SizePairF32, // world width then height, floats
    ScaleF32,    // world/output as a float
    ScaleF64,    // world/output as a double
    PercentI32,  // world/output * 100 as a whole number
    PercentF32,  // world/output * 100 as a float
};
inline const char* KindName(Kind k)
{
    switch (k) {
    case Kind::SizePairI32: return "size pair (whole numbers)";
    case Kind::SizePairF32: return "size pair (floats)";
    case Kind::ScaleF32: return "scale (float)";
    case Kind::ScaleF64: return "scale (double)";
    case Kind::PercentI32: return "percent (whole number)";
    default: return "percent (float)";
    }
}
inline bool IsSizePair(Kind k) { return k == Kind::SizePairI32 || k == Kind::SizePairF32; }

struct Target {
    uint32_t world_w = 0, world_h = 0, out_w = 0, out_h = 0;
    double Ratio() const { return out_w ? double(world_w) / double(out_w) : 0.0; }
    bool Valid() const { return world_w >= 64 && world_h >= 64 && out_w >= world_w / 4 && out_h; }
    // At full size a scale of 1.0 and a percent of 100 are everywhere: not searched in bulk.
    bool Distinct() const { return Valid() && std::fabs(Ratio() - 1.0) > 0.02; }
};
struct Hit { uint64_t address = 0; Kind kind = Kind::ScaleF32; };

// Does the memory at p (n readable bytes) hold this kind of value for this target?
inline bool Holds(const uint8_t* p, size_t n, Kind k, const Target& t)
{
    const double r = t.Ratio();
    switch (k) {
    case Kind::SizePairI32: {
        if (n < 8) return false;
        uint32_t v[2]; std::memcpy(v, p, 8);
        return v[0] == t.world_w && v[1] == t.world_h;
    }
    case Kind::SizePairF32: {
        if (n < 8) return false;
        float v[2]; std::memcpy(v, p, 8);
        return v[0] == float(t.world_w) && v[1] == float(t.world_h);
    }
    case Kind::ScaleF32: {
        if (n < 4) return false;
        float v; std::memcpy(&v, p, 4);
        return std::isfinite(v) && std::fabs(double(v) - r) <= 0.004;
    }
    case Kind::ScaleF64: {
        if (n < 8) return false;
        double v; std::memcpy(&v, p, 8);
        return std::isfinite(v) && std::fabs(v - r) <= 0.004;
    }
    case Kind::PercentI32: {
        if (n < 4) return false;
        int32_t v; std::memcpy(&v, p, 4);
        return std::fabs(double(v) - r * 100.0) <= 0.6;
    }
    default: {
        if (n < 4) return false;
        float v; std::memcpy(&v, p, 4);
        return std::isfinite(v) && std::fabs(double(v) - r * 100.0) <= 0.4;
    }
    }
}

struct SweepOptions {
    bool size_pairs = true;
    bool scales = true;    // only honoured when the target is Distinct()
    size_t max_hits = 4096; // per block
};
// One block of game memory, 4-byte aligned. Appends hits; returns how many were appended.
inline size_t SweepBlock(const uint8_t* bytes, size_t n, uint64_t address, const Target& t, const SweepOptions& o, std::vector<Hit>& out)
{
    if (!t.Valid() || n < 8) return 0;
    const bool scales = o.scales && t.Distinct();
    const size_t before = out.size();
    const uint32_t wi = t.world_w;
    uint32_t wf; { const float f = float(t.world_w); std::memcpy(&wf, &f, 4); }
    const double r = t.Ratio();
    // Cheap first tests on the raw 32-bit word; the full test only on a near miss.
    uint32_t lo, hi, plo, phi; // float bit ranges for r and r*100 (positive floats order as integers)
    { const float a = float(r - 0.004), b = float(r + 0.004); std::memcpy(&lo, &a, 4); std::memcpy(&hi, &b, 4); }
    { const float a = float(r * 100 - 0.4), b = float(r * 100 + 0.4); std::memcpy(&plo, &a, 4); std::memcpy(&phi, &b, 4); }
    const uint32_t pct = uint32_t(std::lround(r * 100.0));
    for (size_t i = 0; i + 4 <= n; i += 4) {
        uint32_t w; std::memcpy(&w, bytes + i, 4);
        const size_t left = n - i;
        if (o.size_pairs) {
            if (w == wi && Holds(bytes + i, left, Kind::SizePairI32, t)) out.push_back({address + i, Kind::SizePairI32});
            else if (w == wf && Holds(bytes + i, left, Kind::SizePairF32, t)) out.push_back({address + i, Kind::SizePairF32});
        }
        if (scales) {
            if (w >= lo && w <= hi) out.push_back({address + i, Kind::ScaleF32});
            else if (w >= plo && w <= phi) out.push_back({address + i, Kind::PercentF32});
            else if (w == pct && pct != 0) out.push_back({address + i, Kind::PercentI32});
            if ((i & 7) == 0 && left >= 8 && Holds(bytes + i, left, Kind::ScaleF64, t)) out.push_back({address + i, Kind::ScaleF64});
        }
        if (out.size() - before >= o.max_hits) break;
    }
    return out.size() - before;
}

// ---------------------------------------------------------------- 2. the board
enum Method : uint32_t {
    kCallChain = 1u << 0,   // read from the game code that sets up the world
    kCreation = 1u << 1,    // read from the game code that creates the world's pictures
    kMemory = 1u << 2,      // found by value in the game's memory
    kNamed = 1u << 3,       // beside a setting name such as "render scale" in the game's code
    kPointerChain = 1u << 4 // reached by following pointers from the game's code
};
inline int MethodCount(uint32_t methods)
{
    int n = 0;
    for (uint32_t m = methods; m; m &= m - 1) ++n;
    return n;
}
struct Entry {
    uint64_t address = 0;
    Kind kind = Kind::ScaleF32;
    uint32_t methods = 0;
    uint32_t followed = 0;  // size changes after which it held the new value
    uint32_t missed = 0;    // size changes after which it did not
    uint32_t noisy = 0;     // times it changed while the world size did not
    bool matches_now = false;
    int Score() const
    {
        // Following the world through a change is the strongest evidence; agreement between
        // independent methods next; a value that changes on its own (a timer) counts against.
        int s = 40 * int((std::min<uint32_t>)(followed, 3)) + 12 * MethodCount(methods) + (matches_now ? 6 : 0);
        if (methods & kNamed) s += 10;
        if (!IsSizePair(kind)) s += 4; // the scale itself is the command; a size is its result
        s -= 25 * int((std::min<uint32_t>)(missed, 3)) + 8 * int((std::min<uint32_t>)(noisy, 5));
        return s;
    }
    bool Confirmed() const { return followed >= 1 && missed == 0 && noisy <= 1; }
};
class Board {
public:
    explicit Board(size_t capacity = 200000) : capacity_(capacity) {}
    // A method reports an address. Returns false when the board is full and it was dropped.
    bool Report(uint64_t address, Kind kind, uint32_t method, bool matches_now)
    {
        const Key k{address, kind};
        auto it = entries_.find(k);
        if (it == entries_.end()) {
            if (entries_.size() >= capacity_) { ++dropped_; return false; }
            Entry e; e.address = address; e.kind = kind;
            it = entries_.emplace(k, e).first;
        }
        it->second.methods |= method;
        it->second.matches_now = matches_now;
        return true;
    }
    // The world size changed and settled at `now`. holds(address, kind, target) re-reads memory.
    template <class HoldsFn> void SizeChanged(const Target& now, HoldsFn&& holds)
    {
        ++changes_;
        for (auto it = entries_.begin(); it != entries_.end();) {
            Entry& e = it->second;
            const bool ok = holds(e.address, e.kind, now);
            e.matches_now = ok;
            if (ok) ++e.followed; else ++e.missed;
            // Memory-only finds that miss a change are chance matches: forget them.
            if (!ok && e.methods == kMemory) it = entries_.erase(it); else ++it;
        }
    }
    // Between size changes: a candidate that changed on its own is noise.
    template <class HoldsFn> void Steady(const Target& now, HoldsFn&& holds)
    {
        for (auto& kv : entries_) {
            const bool ok = holds(kv.second.address, kv.second.kind, now);
            if (kv.second.matches_now && !ok) ++kv.second.noisy;
            kv.second.matches_now = ok;
        }
    }
    std::vector<Entry> Ranked(size_t top) const
    {
        std::vector<Entry> v;
        v.reserve(entries_.size());
        for (const auto& kv : entries_) v.push_back(kv.second);
        const size_t n = (std::min)(top, v.size());
        std::partial_sort(v.begin(), v.begin() + std::ptrdiff_t(n), v.end(), [](const Entry& a, const Entry& b) {
            return a.Score() != b.Score() ? a.Score() > b.Score() : a.address < b.address;
        });
        v.resize(n);
        return v;
    }
    size_t Size() const { return entries_.size(); }
    size_t Dropped() const { return dropped_; }
    uint32_t Changes() const { return changes_; }
    size_t ConfirmedCount() const
    {
        size_t n = 0;
        for (const auto& kv : entries_) if (kv.second.Confirmed()) ++n;
        return n;
    }

private:
    struct Key {
        uint64_t address; Kind kind;
        bool operator<(const Key& o) const { return address != o.address ? address < o.address : kind < o.kind; }
    };
    std::map<Key, Entry> entries_;
    size_t capacity_, dropped_ = 0;
    uint32_t changes_ = 0;
};

// ---------------------------------------------------------------- 3. the response judge
enum class Hold : uint8_t {
    Once,       // one command; the game keeps it
    OnRevert,   // the game sometimes puts its own value back: command again when it does
    Periodic,   // it reverts on a rhythm: command again shortly before each revert
    EveryFrame, // it reverts at once: command every frame
};
inline const char* HoldName(Hold h)
{
    switch (h) {
    case Hold::Once: return "once";
    case Hold::OnRevert: return "again on each revert";
    case Hold::Periodic: return "on a learned period";
    default: return "every frame";
    }
}
enum class Verdict : uint8_t {
    Idle,       // nothing asked
    Waiting,    // commanded; the game has not answered yet
    Responded,  // the world is at the asked size
    Snapped,    // the world moved to the game's own nearest step, and stays there
    Ignored,    // deadline passed with no change: this command does not work
    Reverted,   // it had responded and went back (this frame)
    Refused,    // too many reverts even when commanded every frame
};
inline const char* VerdictName(Verdict v)
{
    switch (v) {
    case Verdict::Idle: return "idle";
    case Verdict::Waiting: return "waiting";
    case Verdict::Responded: return "responded";
    case Verdict::Snapped: return "responded at the game's own step";
    case Verdict::Ignored: return "ignored";
    case Verdict::Reverted: return "reverted";
    default: return "refused";
    }
}
struct Frame {
    uint64_t index = 0;      // presented frame number
    bool world_seen = false; // false in menus/loading: such frames prove nothing either way
    uint32_t world_w = 0, world_h = 0, out_w = 0, out_h = 0;
};
class ResponseJudge {
public:
    struct Limits {
        uint32_t respond_frames = 180;  // world frames to wait for an answer
        uint32_t settle_frames = 3;     // the same size this many frames in a row = settled
        uint32_t revert_frames = 2;     // off target this many frames in a row = reverted
        double tolerance = 0.012;       // of the output width (rounding and alignment)
        double snap_window = 0.08;      // a settled size this close to the ask is the game's step
        uint32_t max_every_frame_reverts = 30;
    };
    ResponseJudge() = default;
    explicit ResponseJudge(const Limits& l) : lim_(l) {}

    // The Referee asks for a scale (0 = stop asking). Starts a new command.
    void Ask(double scale, uint64_t frame)
    {
        wanted_ = scale; achieved_ = 0; verdict_ = scale > 0 ? Verdict::Waiting : Verdict::Idle;
        waited_ = on_ = off_ = 0; before_ = 0; commanded_ = false;
        hold_ = Hold::Once; reverts_ = 0; bounces_ = 0; period_ = 0; last_command_ = frame; responded_at_ = 0; streak_reverts_ = 0;
    }
    // Should the command be issued on this frame? (The caller issues it, then calls Commanded.)
    bool NeedsCommand(uint64_t frame) const
    {
        if (!(wanted_ > 0) || verdict_ == Verdict::Ignored || verdict_ == Verdict::Refused) return false;
        if (!commanded_) return true;
        switch (hold_) {
        case Hold::EveryFrame: return true;
        case Hold::Periodic: return period_ && frame - last_command_ >= period_;
        case Hold::OnRevert: return verdict_ == Verdict::Reverted;
        default: return verdict_ == Verdict::Reverted;
        }
    }
    void Commanded(uint64_t frame) { commanded_ = true; last_command_ = frame; }

    Verdict Observe(const Frame& f)
    {
        if (!(wanted_ > 0) || verdict_ == Verdict::Ignored || verdict_ == Verdict::Refused) return verdict_;
        if (!f.world_seen || !f.out_w) return verdict_; // a menu or loading screen: no evidence
        const double ratio = double(f.world_w) / double(f.out_w);
        if (!before_) before_ = ratio; // where the world was when we asked
        const double goal = achieved_ > 0 ? achieved_ : wanted_;
        const bool on_target = std::fabs(ratio - goal) <= lim_.tolerance;
        if (verdict_ == Verdict::Reverted) verdict_ = Verdict::Waiting; // a revert is reported for one frame
        if (verdict_ == Verdict::Waiting) {
            ++waited_;
            if (on_target) {
                if (++on_ >= lim_.settle_frames) Settle(f.index, achieved_ > 0 ? last_kind_ : Verdict::Responded, goal);
            } else {
                // Reached the target and fell straight off it: the game is putting its own value
                // back faster than it can settle. The first time: command again. Twice: command every frame.
                if (on_ > 0 && hold_ != Hold::EveryFrame) {
                    ++reverts_; waited_ = 0; on_ = 0;
                    if (++bounces_ >= 2) hold_ = Hold::EveryFrame;
                    else if (hold_ == Hold::Once) hold_ = Hold::OnRevert;
                    verdict_ = Verdict::Reverted; // one frame: the caller commands again
                    return verdict_;
                }
                on_ = 0;
                // Moved, but to somewhere else, and stays: the game's own step.
                const bool moved = std::fabs(ratio - before_) > lim_.tolerance;
                const bool close_by = std::fabs(ratio - wanted_) <= lim_.snap_window;
                if (achieved_ <= 0 && moved && close_by && std::fabs(ratio - snap_ratio_) <= lim_.tolerance / 2) {
                    if (++snap_ >= lim_.settle_frames * 4) Settle(f.index, Verdict::Snapped, ratio);
                } else { snap_ = 0; snap_ratio_ = ratio; }
                if (verdict_ == Verdict::Waiting && waited_ >= lim_.respond_frames) {
                    // Never answered. After a revert under EveryFrame this is a refusal.
                    verdict_ = responded_at_ ? Verdict::Refused : Verdict::Ignored;
                }
            }
            return verdict_;
        }
        // Responded or Snapped: watch for the game putting its own value back.
        if (on_target) { off_ = 0; return verdict_; }
        if (++off_ < lim_.revert_frames) return verdict_;
        Revert(f.index);
        return verdict_;
    }
    Verdict State() const { return verdict_; }
    Hold HoldPolicy() const { return hold_; }
    double Wanted() const { return wanted_; }
    double Achieved() const { return achieved_; } // the scale the game really gave (its own step when snapped)
    uint32_t Reverts() const { return reverts_; }
    uint64_t Period() const { return period_; }   // frames between commands under Hold::Periodic

private:
    void Settle(uint64_t frame, Verdict kind, double ratio)
    {
        verdict_ = last_kind_ = kind; achieved_ = ratio; responded_at_ = frame; on_ = off_ = snap_ = 0; waited_ = 0;
    }
    void Revert(uint64_t frame)
    {
        ++reverts_;
        const uint64_t held = frame > last_command_ ? frame - last_command_ : 0;
        // How long it held after our last command decides how often to repeat.
        if (held <= lim_.revert_frames + lim_.settle_frames + 2) {
            if (hold_ == Hold::EveryFrame && ++streak_reverts_ >= lim_.max_every_frame_reverts) { verdict_ = Verdict::Refused; return; }
            hold_ = Hold::EveryFrame;
        } else if (hold_ == Hold::Once) {
            hold_ = Hold::OnRevert; shortest_hold_ = held;
        } else if (hold_ != Hold::EveryFrame) {
            // Two or more reverts with a similar hold time: a rhythm. Command before it.
            const bool similar = shortest_hold_ && held <= shortest_hold_ * 3 / 2 + 2 && held * 3 / 2 + 2 >= shortest_hold_;
            shortest_hold_ = shortest_hold_ ? (std::min)(shortest_hold_, held) : held;
            if (similar || hold_ == Hold::Periodic) {
                hold_ = Hold::Periodic;
                period_ = (std::max<uint64_t>)(1, shortest_hold_ / 2);
            }
        }
        verdict_ = Verdict::Reverted; off_ = on_ = 0; waited_ = 0;
    }
    Limits lim_{};
    double wanted_ = 0, achieved_ = 0, before_ = 0, snap_ratio_ = 0;
    Verdict verdict_ = Verdict::Idle, last_kind_ = Verdict::Responded;
    Hold hold_ = Hold::Once;
    uint64_t last_command_ = 0, responded_at_ = 0, period_ = 0, shortest_hold_ = 0;
    uint32_t waited_ = 0, on_ = 0, off_ = 0, snap_ = 0, reverts_ = 0, streak_reverts_ = 0, bounces_ = 0;
    bool commanded_ = false;
};

} // namespace na::scale::hunt
