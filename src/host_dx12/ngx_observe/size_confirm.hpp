// A66/A67 (Astra's A65 and A66 reviews): request-scoped render-size confirmation.
//
// One "Apply changes" request pins the requested render size immutably at
// admission, BEFORE the window is touched, so a DLSS creation the game
// performs from inside the resize itself (or on a worker before the window is
// back) is observed by the request. The caller marks the restoration
// boundary; only evaluations after it count. The monitor then requires
// `need` CONSECUTIVE successful evaluations of the eligible feature (the
// feature the game created since the request was armed, identified by a
// known non-zero handle), each at exactly the requested size with coherent
// inputs. Any evaluation at another size, with inputs that do not fit, with
// an unknown handle, or that failed, resets the streak. A replacing creation
// restarts the streak but never the request's own deadlines. Deadlines are
// enforced before every event, including creations, and a finished request
// is never revived.
//
// Pure state machine (no D3D, no time source of its own): callers feed
// events plus a millisecond tick. Exercised by tests/size_confirm_test.cpp.
#pragma once

#include <cmath>
#include <cstdint>

namespace na::ngx::confirm {

// A66 (Astra's A65 review, R2): scalar input checks for one evaluation.
// `missing` is the "never provided" sentinel ReadEvalParams leaves in place
// (1e38f). Jitter must be present, finite and within one pixel; the motion
// scale must be present, finite and non-zero on both axes. std::isfinite
// rejects +inf, -inf and NaN alike. A NEGATIVE finite scale is a legitimate
// coordinate convention and passes.
inline bool ScalarInputsCoherent(float jitter_x, float jitter_y, float mv_scale_x, float mv_scale_y, float missing = 1e38f)
{
    auto present_finite = [missing](float v) { return v != missing && std::isfinite(v); };
    return present_finite(jitter_x) && present_finite(jitter_y) &&
           std::fabs(jitter_x) <= 1.0f && std::fabs(jitter_y) <= 1.0f &&
           present_finite(mv_scale_x) && present_finite(mv_scale_y) &&
           mv_scale_x != 0.f && mv_scale_y != 0.f;
}

// A68 (Astra's A67 review, 2): subrect coverage in 64-bit on both axes, so a
// malformed base near 2^32 cannot wrap into a passing value. Unknown base
// (0xFFFFFFFF) or a missing resource is NOT coverage.
inline bool CoversSubrect(uint64_t res_w, uint64_t res_h, uint32_t bx, uint32_t by, uint32_t w, uint32_t h)
{
    if (bx == 0xFFFFFFFFu || by == 0xFFFFFFFFu || res_w == 0 || res_h == 0 || w == 0 || h == 0) return false;
    return res_w >= static_cast<uint64_t>(bx) + w && res_h >= static_cast<uint64_t>(by) + h;
}

// A68 (Astra's A67 review, 1): creation flags bound to the feature they were
// created with, keyed by the handle the game uses, so a feature created
// between two evaluations cannot change the MVLowRes reading of another.
// Bounded (8 live features); a re-created handle value overwrites its entry.
class FeatureFlags {
public:
    static constexpr uint32_t kUnknown = 0xFFFFFFFFu;
    void Set(uint64_t handle, uint32_t flags, uint64_t core = 0)
    {
        if (!handle) return;
        for (Entry& e : e_) if (e.handle == handle && e.core == core) { e.flags = flags; return; }
        for (Entry& e : e_) if (e.handle == 0) { e = {handle, flags, core}; return; }
        e_[next_++ % kEntries] = {handle, flags, core}; // full: overwrite round-robin
    }
    uint32_t Get(uint64_t handle, uint64_t core = 0) const
    {
        if (!handle) return kUnknown;
        for (const Entry& e : e_) if (e.handle == handle && e.core == core) return e.flags;
        return kUnknown;
    }
    void Erase(uint64_t handle, uint64_t core = 0)
    {
        for (Entry& e : e_) if (e.handle == handle && e.core == core) e = {};
    }
    void EraseCore(uint64_t core) { for (Entry& e : e_) if (e.core == core) e = {}; }

private:
    static constexpr uint32_t kEntries = 8;
    struct Entry { uint64_t handle = 0; uint32_t flags = kUnknown; uint64_t core = 0; };
    Entry e_[kEntries]{};
    uint32_t next_ = 0;
};

enum class State : uint32_t {
    None = 0,
    Armed = 1,      // request admitted; creations are observed; the window is not back yet
    Counting = 2,   // window restored; evaluations of the eligible feature are counted
    Confirmed = 3,
    Failed = 4,
};
enum class Reason : uint32_t {
    None = 0,
    NoCreation = 1,        // no DLSS feature was created for this request in time
    DrawnAtOtherSize = 2,  // evaluations came, but at a size other than the requested one
    Incoherent = 3,        // evaluations at the requested size had inputs that did not fit / could not be verified
    TooFewEvaluations = 4, // the streak never reached `need` before the deadline
    Superseded = 5,        // a newer request replaced this one
    Cancelled = 6,         // the caller withdrew it (settings edited, nudge failed)
    UnknownFeature = 7,    // the creation(s) carried no usable handle, so nothing could be attributed
    NoRestoration = 8,     // the window was never reported restored within the request's lifetime
};

struct Snapshot {
    uint64_t id;                 // request generation (0 = never armed)
    uint32_t state, reason;      // State / Reason
    uint32_t req_w, req_h;       // immutable requested render size
    uint32_t need, streak;       // consecutive matching coherent evaluations required / current
    uint64_t evals;              // evaluations of the eligible feature seen while counting (incl. failed)
    uint64_t matching;           // ... successful, at the requested size, with coherent inputs
    uint64_t mismatched;         // ... at another size
    uint64_t incoherent;         // ... at the requested size with inputs that did not fit or could not be verified
    uint64_t failed;             // ... whose evaluate call failed or could not be read
    uint64_t resets;             // ... with the history-reset flag raised
    uint64_t unknown_handle;     // evaluations with no handle identity (break the streak, never match)
    uint64_t other_feature;      // evaluations of a different known feature (ignored for the streak)
    uint64_t before_restore;     // evaluations seen before the restoration boundary (not counted)
    uint64_t creations;          // DLSS features created since the request was armed
    uint32_t created_w, created_h; // size of the latest creation (0 = none yet)
    uint32_t last_w, last_h;     // size of the last counted evaluation (0 = none yet)
    uint64_t armed_tick, restored_tick, created_tick, done_tick;
};

struct EvalObs {
    uint32_t w, h;    // the evaluation's render size (subrect if present)
    bool coherent;    // inputs fit that size (caller's structural checks all verified)
    bool reset;       // history-reset flag raised
    bool ok;          // the evaluate call succeeded and its parameters were readable
    uint64_t handle;  // the feature handle the game evaluated (0 = unknown)
};

class Monitor {
public:
    // Arms a new request; anything running is superseded. `create_timeout_ms`
    // runs from the restoration boundary (a creation seen before it counts);
    // `confirm_timeout_ms` runs from the eligible creation; `overall_timeout_ms`
    // runs from arming and bounds everything, replacements included.
    uint64_t Arm(uint32_t w, uint32_t h, uint32_t need, uint64_t now, uint64_t create_timeout_ms, uint64_t confirm_timeout_ms,
                 uint64_t overall_timeout_ms)
    {
        if (Active()) finish(State::Failed, Reason::Superseded, now);
        Snapshot n{};
        n.id = ++last_id_;
        n.state = static_cast<uint32_t>(State::Armed);
        n.req_w = w;
        n.req_h = h;
        n.need = need ? need : 1;
        n.armed_tick = now;
        s_ = n;
        handle_ = 0;
        eligible_ = false;
        create_timeout_ = create_timeout_ms;
        confirm_timeout_ = confirm_timeout_ms;
        overall_deadline_ = now + overall_timeout_ms;
        return n.id;
    }
    // Withdraws the running request (no effect on a finished one).
    void Cancel(uint64_t now)
    {
        if (Active()) finish(State::Failed, Reason::Cancelled, now);
    }
    // The window is back at its size; evaluations from now on count.
    void MarkRestored(uint64_t now)
    {
        Tick(now);
        if (state() != State::Armed) return;
        s_.restored_tick = now;
        s_.state = static_cast<uint32_t>(State::Counting);
        s_.streak = 0;
    }
    // A successful DLSS super-resolution creation by the game. `handle` 0 =
    // unknown identity: recorded, but nothing can be attributed to it.
    void OnCreate(uint32_t w, uint32_t h, uint64_t handle, uint64_t now)
    {
        Tick(now);
        if (!Active()) return;
        ++s_.creations;
        s_.created_w = w;
        s_.created_h = h;
        s_.created_tick = now;
        handle_ = handle;
        eligible_ = handle != 0;
        s_.streak = 0; // a replaced feature starts its own streak; deadlines are unchanged
    }
    // One DLSS evaluation, successful or not.
    void OnEvaluate(const EvalObs& e, uint64_t now)
    {
        Tick(now);
        if (!Active()) return;
        if (state() == State::Armed) {
            ++s_.before_restore;
            return;
        }
        if (e.handle == 0) { // unknown identity: never a match, and it breaks the streak
            ++s_.unknown_handle;
            s_.streak = 0;
            return;
        }
        if (!eligible_ || e.handle != handle_) {
            ++s_.other_feature;
            return;
        }
        ++s_.evals;
        if (!e.ok) {
            ++s_.failed;
            s_.streak = 0;
            return;
        }
        s_.last_w = e.w;
        s_.last_h = e.h;
        if (e.reset) ++s_.resets;
        if (e.w != s_.req_w || e.h != s_.req_h) {
            ++s_.mismatched;
            s_.streak = 0;
            return;
        }
        if (!e.coherent) {
            ++s_.incoherent;
            s_.streak = 0;
            return;
        }
        ++s_.matching;
        if (++s_.streak >= s_.need) finish(State::Confirmed, Reason::None, now);
    }
    // Deadlines; safe to call from any poller. Runs before every event.
    void Tick(uint64_t now)
    {
        if (!Active()) return;
        if (now >= overall_deadline_) {
            finish(State::Failed, state() == State::Armed ? Reason::NoRestoration : failure_reason(), now);
            return;
        }
        if (state() != State::Counting) return;
        if (!eligible_) {
            // No usable creation yet (or an unattributable one): creation deadline from restoration.
            if (now - s_.restored_tick >= create_timeout_)
                finish(State::Failed, s_.creations ? Reason::UnknownFeature : Reason::NoCreation, now);
            return;
        }
        uint64_t since = s_.created_tick > s_.restored_tick ? s_.created_tick : s_.restored_tick;
        if (now - since >= confirm_timeout_) finish(State::Failed, failure_reason(), now);
    }
    bool Active() const { return state() == State::Armed || state() == State::Counting; }
    State state() const { return static_cast<State>(s_.state); }
    const Snapshot& snapshot() const { return s_; }

private:
    Reason failure_reason() const
    {
        if (!eligible_) return s_.creations ? Reason::UnknownFeature : Reason::NoCreation;
        if (s_.matching == 0 && s_.mismatched > 0) return Reason::DrawnAtOtherSize;
        if (s_.incoherent > 0 && s_.incoherent >= s_.mismatched) return Reason::Incoherent;
        if (s_.mismatched > 0) return Reason::DrawnAtOtherSize;
        return Reason::TooFewEvaluations;
    }
    void finish(State st, Reason r, uint64_t now)
    {
        s_.state = static_cast<uint32_t>(st);
        s_.reason = static_cast<uint32_t>(r);
        s_.done_tick = now;
    }
    Snapshot s_{};
    uint64_t last_id_ = 0;
    uint64_t handle_ = 0;
    bool eligible_ = false;
    uint64_t create_timeout_ = 0, confirm_timeout_ = 0, overall_deadline_ = 0;
};

} // namespace na::ngx::confirm
