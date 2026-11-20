// Automatic anisotropic filtering, one rule for DX11 and DX12 (Cort 2026-11-20: "We should have
// it automatic. If the game has it, we turn it off.").
//
// No menu control. [j2] anisotropic_filter is a hidden setting: -1 = automatic (default),
// 0 = off (the game's own filtering everywhere), 16 = force 16x on world samplers.
//
// A sampler is a "world" sampler (3D surfaces: terrain, walls, roads, foliage) only when all four
// hold; anything else is never touched by any mode:
//   1. not a comparison sampler (hardware PCF shadow maps) and not a min/max reduction (depth
//      pyramids): standard reduction only;
//   2. not point filtered: minification and magnification both linear (or already anisotropic);
//      point filtering is deliberate for lookup tables, noise, pixel art and data;
//   3. has mips: MaxLOD > 0 (single-mip samplers are screen, UI or compute passes);
//   4. tiles: WRAP or MIRROR (incl. MIRROR_ONCE) on U or V. Post-processing, shadow cascades
//      and HUD use CLAMP or BORDER.
//
// Automatic: the detector watches the game's own sampler descriptions as it creates them (before
// any change of ours; each distinct description counts once, so a game re-creating one sampler
// every frame does not outvote its materials). When at least half of the distinct world samplers
// are already 8x or more, the game has its own anisotropic filtering and we change nothing.
// Otherwise world samplers below 8x are raised to 16x and the game's own 8x/16x ones are left as
// they are. The decision moves only on clear evidence (hysteresis: it flips to "the game has it"
// at 60 % and back at 40 %) and every change is reported with the counts behind it.
//
// D3D11_FILTER and D3D12_FILTER share their encodings, as do the address modes, so the rule works
// on plain numbers. Portable C++17 (Linux-tested); the API layers copy the fields in and out.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <unordered_set>

namespace na::af {

// [j2] anisotropic_filter.
enum class Mode : int32_t { Auto = -1, Off = 0, Force = 16 };
inline Mode ModeFromSetting(int32_t v) { return v < 0 ? Mode::Auto : v == 0 ? Mode::Off : Mode::Force; }
inline const char* ModeName(Mode m) { return m == Mode::Auto ? "auto" : m == Mode::Off ? "off" : "force16"; }

// What is applied to world samplers right now. The values are also the DX12 variant index.
enum Action : uint32_t {
    kKeep = 0,    // the game's own filtering
    kAuto16 = 1,  // world samplers below 8x -> 16x; the game's 8x/16x stay as they are
    kForce16 = 2, // world samplers below 16x -> 16x
};
inline const char* ActionName(uint32_t a) { return a == kAuto16 ? "raise_below_8x_to_16x" : a == kForce16 ? "force_16x" : "game_own"; }
// The level a world sampler ends up at, for readouts (0 = the game's own).
inline uint32_t ActionLevel(uint32_t a) { return a == kKeep ? 0u : 16u; }

struct Facts {
    uint32_t filter = 0;                   // D3D11_FILTER / D3D12_FILTER
    uint32_t address_u = 1, address_v = 1; // TEXTURE_ADDRESS_MODE (1 = WRAP)
    uint32_t max_anisotropy = 1;
    float max_lod = 3.402823466e+38f;
    float mip_lod_bias = 0.0f;
};

namespace bits {
constexpr uint32_t kAnisotropicBit = 0x40;
constexpr uint32_t kAnisotropic = 0x55; // FILTER_ANISOTROPIC
inline uint32_t Reduction(uint32_t f) { return (f >> 7) & 3u; } // 0 standard, 1 comparison, 2 min, 3 max
inline uint32_t MinFilter(uint32_t f) { return (f >> 4) & 3u; } // 1 = linear
inline uint32_t MagFilter(uint32_t f) { return (f >> 2) & 3u; } // 1 = linear
inline bool Anisotropic(uint32_t f) { return (f & kAnisotropicBit) != 0; }
inline bool Tiles(uint32_t mode) { return mode == 1 || mode == 2 || mode == 5; } // WRAP, MIRROR, MIRROR_ONCE
} // namespace bits

// Why a sampler is (not) a world sampler; the first failing rule wins.
enum class Kind : uint8_t { World = 0, Comparison, MinMax, Point, NoMips, NotTiled, Count };
inline const char* KindName(Kind k)
{
    static const char* names[] = {"world", "comparison", "minmax", "point", "no_mips", "clamp_or_border"};
    return names[static_cast<int>(k) < static_cast<int>(Kind::Count) ? static_cast<int>(k) : 0];
}

inline Kind Classify(const Facts& s)
{
    using namespace bits;
    const uint32_t r = Reduction(s.filter);
    if (r == 1) return Kind::Comparison;
    if (r != 0) return Kind::MinMax;
    if (!Anisotropic(s.filter) && (MinFilter(s.filter) != 1 || MagFilter(s.filter) != 1)) return Kind::Point;
    if (!(s.max_lod > 0.0f)) return Kind::NoMips;
    if (!Tiles(s.address_u) && !Tiles(s.address_v)) return Kind::NotTiled;
    return Kind::World;
}
inline bool IsWorld(const Facts& s) { return Classify(s) == Kind::World; }
// The game's own filtering counts as anisotropic filtering at 8x or more.
inline bool GameFiltered(const Facts& s) { return bits::Anisotropic(s.filter) && s.max_anisotropy >= 8; }

// The sampler as `action` wants it; true when anything changed. Only world samplers change.
inline bool Apply(Facts& s, uint32_t action)
{
    if (action == kKeep || !IsWorld(s)) return false;
    if (action == kAuto16 && GameFiltered(s)) return false; // the game's own 8x/16x stay
    if (bits::Anisotropic(s.filter) && s.max_anisotropy >= 16) return false;
    s.filter = bits::kAnisotropic;
    s.max_anisotropy = 16;
    return true;
}

class Detector {
public:
    enum class State : uint8_t { Pending = 0, GameHasAf, GameLacksAf };
    static const char* StateName(State s)
    {
        return s == State::GameHasAf ? "game_has_af" : s == State::GameLacksAf ? "game_lacks_af" : "pending";
    }
    struct Snapshot {
        Mode mode = Mode::Auto;
        State state = State::Pending;
        uint32_t action = kKeep;
        uint64_t observed = 0;           // sampler descriptions the game created (all, repeats included)
        uint64_t distinct = 0;           // distinct descriptions
        uint64_t world = 0;              // distinct world samplers
        uint64_t world_af8 = 0;          // ...of them already 8x or more by the game
        uint64_t rejected[static_cast<int>(Kind::Count)] = {}; // distinct non-world samplers, by first failing rule
        uint64_t changes = 0;            // decisions changed (state or mode)
        uint32_t last_filter = 0, last_aniso = 0, last_u = 0, last_v = 0; // the world sampler that moved the decision last
    };
    using OnChange = void (*)(uint32_t action);
    using OnReport = void (*)(const char* line); // every decision report, as it is made

    void SetOnReport(OnReport f)
    {
        std::lock_guard<std::mutex> l(mu_);
        on_report_ = f;
    }

    void SetOnChange(OnChange f)
    {
        std::lock_guard<std::mutex> l(mu_);
        on_change_ = f;
    }
    // Returns true when the action changed.
    bool SetMode(Mode m)
    {
        uint32_t act = 0;
        OnChange cb = nullptr;
        {
            std::lock_guard<std::mutex> l(mu_);
            if (s_.mode == m) return false;
            const uint32_t before = s_.action;
            s_.mode = m;
            s_.action = action_locked();
            ++s_.changes;
            report_locked("mode");
            if (s_.action == before) return false;
            act = s_.action;
            cb = on_change_;
        }
        if (cb) cb(act);
        return true;
    }
    // One sampler the game created, as the game described it. Returns true when the action changed.
    bool Observe(const Facts& s)
    {
        const Kind k = Classify(s);
        uint32_t act = 0;
        OnChange cb = nullptr;
        {
            std::lock_guard<std::mutex> l(mu_);
            ++s_.observed;
            // Past the cap the decision stands on what was seen (thousands of distinct descriptions).
            if (seen_.size() >= kMaxDistinct || !seen_.insert(Key(s)).second) return false;
            ++s_.distinct;
            if (k != Kind::World) {
                ++s_.rejected[static_cast<int>(k)];
                return false;
            }
            ++s_.world;
            if (GameFiltered(s)) ++s_.world_af8;
            const State before_state = s_.state;
            const uint32_t before = s_.action;
            s_.state = next_state_locked();
            s_.action = action_locked();
            if (s_.state == before_state) return false;
            s_.last_filter = s.filter; s_.last_aniso = s.max_anisotropy; s_.last_u = s.address_u; s_.last_v = s.address_v;
            ++s_.changes;
            report_locked("evidence");
            if (s_.action == before) return false;
            act = s_.action;
            cb = on_change_;
        }
        if (cb) cb(act);
        return true;
    }
    uint32_t Current() const
    {
        std::lock_guard<std::mutex> l(mu_);
        return s_.action;
    }
    Snapshot Get() const
    {
        std::lock_guard<std::mutex> l(mu_);
        return s_;
    }
    // The last decision report: what was detected and why (one line, no newline).
    void LastReport(char* out, size_t n) const
    {
        std::lock_guard<std::mutex> l(mu_);
        std::snprintf(out, n, "%s", report_);
    }
    uint64_t ReportCount() const
    {
        std::lock_guard<std::mutex> l(mu_);
        return reports_;
    }
    // Writes the current state as one log line (no newline).
    static int Format(const Snapshot& s, char* out, size_t n)
    {
        const auto r = [&](Kind k) { return static_cast<unsigned long long>(s.rejected[static_cast<int>(k)]); };
        return std::snprintf(out, n,
                             "af_auto mode=%s state=%s action=%s world=%llu world_8x_or_more=%llu world_below_8x=%llu "
                             "distinct=%llu observed=%llu left_alone{comparison=%llu minmax=%llu point=%llu no_mips=%llu clamp_or_border=%llu} changes=%llu",
                             ModeName(s.mode), StateName(s.state), ActionName(s.action), static_cast<unsigned long long>(s.world),
                             static_cast<unsigned long long>(s.world_af8), static_cast<unsigned long long>(s.world - s.world_af8),
                             static_cast<unsigned long long>(s.distinct), static_cast<unsigned long long>(s.observed), r(Kind::Comparison),
                             r(Kind::MinMax), r(Kind::Point), r(Kind::NoMips), r(Kind::NotTiled), static_cast<unsigned long long>(s.changes));
    }

private:
    static constexpr size_t kMaxDistinct = 4096;
    static uint64_t Key(const Facts& s)
    {
        uint32_t lod = 0, bias = 0;
        std::memcpy(&lod, &s.max_lod, 4);
        std::memcpy(&bias, &s.mip_lod_bias, 4);
        uint64_t h = 14695981039346656037ull;
        for (const uint32_t v : {s.filter, s.address_u, s.address_v, s.max_anisotropy, lod, bias}) h = (h ^ v) * 1099511628211ull;
        return h;
    }
    State next_state_locked() const
    {
        if (!s_.world) return State::Pending;
        // Shares in percent; the first world sampler decides at 50 %, later flips need 60 % / 40 %.
        const uint64_t pct = s_.world_af8 * 100 / s_.world;
        switch (s_.state) {
        case State::Pending: return pct >= 50 ? State::GameHasAf : State::GameLacksAf;
        case State::GameHasAf: return pct < 40 ? State::GameLacksAf : State::GameHasAf;
        case State::GameLacksAf: return pct >= 60 ? State::GameHasAf : State::GameLacksAf;
        }
        return s_.state;
    }
    uint32_t action_locked() const
    {
        switch (s_.mode) {
        case Mode::Off: return kKeep;
        case Mode::Force: return kForce16;
        case Mode::Auto: return s_.state == State::GameLacksAf ? kAuto16 : kKeep;
        }
        return kKeep;
    }
    void report_locked(const char* cause)
    {
        char state[400];
        Format(s_, state, sizeof state);
        const char* why = "";
        if (s_.mode == Mode::Off) why = "setting is 0: the game's own filtering everywhere";
        else if (s_.mode == Mode::Force) why = "setting is 16: world samplers below 16x are raised";
        else if (s_.state == State::GameHasAf) why = "the game already filters at least half of its world samplers at 8x or more: ours is off";
        else if (s_.state == State::GameLacksAf) why = "most world samplers are below 8x: they are raised to 16x, the game's own 8x/16x stay";
        else why = "no world sampler seen yet: nothing is changed";
        std::snprintf(report_, sizeof report_, "decision cause=%s %s last_world{filter=0x%X max_aniso=%u address=%u/%u} why=\"%s\"", cause, state,
                      s_.last_filter, s_.last_aniso, s_.last_u, s_.last_v, why);
        ++reports_;
        if (on_report_) on_report_(report_);
    }

    mutable std::mutex mu_;
    Snapshot s_;
    std::unordered_set<uint64_t> seen_;
    OnChange on_change_ = nullptr;
    OnReport on_report_ = nullptr;
    char report_[640] = {};
    uint64_t reports_ = 0;
};

// The process's one detector (both APIs feed and read it).
inline Detector& Global()
{
    static Detector* d = new Detector(); // never destroyed: game threads may create samplers at exit
    return *d;
}

} // namespace na::af
