#pragma once
// J2 automatic route selection (COMMS-Continuation step 2, Opus).
//
// Decides, per selected view, WHERE live DLAA is integrated:
//   Between   - after the frame's last scene-writing command list, as J2's own list
//               (Anno-style: the scene completes at a list boundary);
//   Inline    - inside the last scene-writing recording, at the observed start of
//               its downstream use of the colour (SER-style: post-processing shares
//               the scene's list; PR50's trigger performs the insertion);
//   Interceptor - a supported upscaler call already serves this view (existing
//               handlers; J2 must not also evaluate it);
//   None      - no scene observed / no stable structure yet, with a named reason.
//
// Pure CPU logic: no D3D12, no game objects, no clocks. J2 feeds it compact
// per-list summaries in SUBMISSION order (FrameAggregator) and closes each frame at
// Present (RouteSelector::OnFrame). Decisions change only at frame boundaries and
// carry a history generation so output of an old route never feeds a new one.
//
// Evidence semantics (Astra's corrections, 2026-10-27):
//   * native sub-pixel camera jitter is recorded as evidence only, never "TAA";
//   * "no later scene write after our insertion" is PLACEMENT CONSISTENCY, not proof
//     that the output reached the presented image: contribution stays UNKNOWN unless
//     downstream lineage to the presented buffer was actually observed.
#include <cstdint>
#include <cstring>

namespace na::j2 {

enum class Route : uint8_t { None = 0, Between = 1, Inline = 2, Interceptor = 3 };
enum class RouteState : uint8_t { Observe = 0, Active = 1 };
enum class RouteMode : uint8_t { Auto = 0, ForceBetween = 1, ForceInline = 2 };

inline const char* RouteName(Route r)
{
    switch (r) {
    case Route::Between: return "between";
    case Route::Inline: return "inline";
    case Route::Interceptor: return "interceptor";
    default: return "none";
    }
}

// One command-list recording's use of the selected scene colour, as observed in that
// recording (producers' InsertionTrace + render-target history).
struct ListSummary {
    bool renders_scene = false;  // bound the selected colour with the selected depth (and drew/cleared)
    uint32_t color_writes = 0;   // writes to the selected colour in this recording
    uint32_t reads_after_last_write = 0; // downstream uses of the colour after its LAST write in this recording
    uint32_t reads = 0;          // all observed uses of the colour (reads) in this recording
    bool clears_color = false;   // the recording starts the image over
    uint64_t boundary_signature = 0; // stable identity of the in-recording boundary (writer PSO, occurrence, consumer)
};

// Per-frame structural observation, built from the frame's list summaries.
struct FrameObservation {
    bool scene = false;            // the selected colour + depth were drawn this frame
    bool interceptor = false;      // a supported upscaler call served the view this frame
    uint32_t scene_lists = 0;      // recordings that wrote the scene
    bool boundary_inside_list = false; // the last scene-writing recording uses the colour after its last write
    bool consumer_in_later_list = false; // a later recording reads the colour after the last scene write
    uint64_t inline_signature = 0; // boundary_signature of that last scene-writing recording
    // Inputs (missing-evidence reporting; they do not choose the route).
    bool camera_missed = false, depth_state_missed = false;
    bool native_jitter = false;    // evidence only
    // Outcome of executing the CURRENT route this frame.
    bool executed = false;           // our DLAA was submitted for this frame
    bool later_scene_write = false;  // the selected version was written again after our insertion
    bool lineage_to_present = false; // modified version observed reaching the presented image
};

// Builds a FrameObservation from list summaries in submission order. A later scene
// write invalidates an earlier read (a mid-frame refraction/reflection read is not
// the scene's consumer); a recording that clears the colour restarts the image.
class FrameAggregator {
public:
    void Reset() { obs_ = {}; consumer_after_last_write_ = false; }
    void AddList(const ListSummary& s)
    {
        if (s.renders_scene && s.color_writes) {
            if (s.clears_color) obs_.scene_lists = 0; // the image starts over
            obs_.scene = true;
            ++obs_.scene_lists;
            obs_.boundary_inside_list = s.reads_after_last_write > 0;
            obs_.inline_signature = s.boundary_signature;
            consumer_after_last_write_ = false;
            return;
        }
        if (obs_.scene && s.reads && !s.color_writes) consumer_after_last_write_ = true;
        if (obs_.scene && s.color_writes && !s.renders_scene) {
            // Written outside the scene pass (e.g. a full-screen composite into it):
            // any consumer seen before this point read an earlier version.
            consumer_after_last_write_ = false;
            if (s.clears_color) {
                // The image was restarted outside the depth-paired scene pass: the
                // earlier list's in-list boundary no longer describes the final image
                // (Astra R2). Ordinary composites into it keep the association.
                obs_.boundary_inside_list = false;
                obs_.inline_signature = 0;
            }
        }
    }
    FrameObservation& Observation()
    {
        obs_.consumer_in_later_list = consumer_after_last_write_;
        return obs_;
    }

private:
    FrameObservation obs_;
    bool consumer_after_last_write_ = false;
};

struct RouteDecision {
    Route route = Route::None;
    RouteState state = RouteState::Observe;
    uint64_t signature = 0;          // structural identity of the active route
    uint32_t history_generation = 0; // bumps whenever the route (or its signature class) changes
    uint32_t matches = 0;            // consecutive frames proposing the same route + signature
    uint32_t observed = 0;           // scene frames observed in the current window
    bool override_active = false;
    bool contribution_established = false; // lineage to Present observed in the last executed frame
    const char* reason = "Observing the frame structure.";
    const char* missing = "";
    const char* change = "";
    // Counters.
    uint64_t scene_frames = 0, executed_frames = 0, placement_consistent = 0, contradictions = 0;
    uint64_t route_changes = 0, unstable_windows = 0, native_jitter_frames = 0;
    uint64_t camera_misses = 0, depth_state_misses = 0;
};

class RouteSelector {
public:
    static constexpr uint32_t kMatchesToAccept = 3;  // engineering defaults (COMMS-Continuation)
    static constexpr uint32_t kWindowFrames = 120;

    void SetMode(RouteMode m)
    {
        if (m == mode_) return;
        mode_ = m;
        if (m == RouteMode::Auto) {
            d_.override_active = false;
            d_.state = RouteState::Observe;
            d_.matches = d_.observed = 0;
            d_.reason = "Override removed: observing the frame structure.";
            return;
        }
        set_route(m == RouteMode::ForceInline ? Route::Inline : Route::Between, 0, "override");
        d_.override_active = true;
        d_.state = RouteState::Active;
        d_.reason = m == RouteMode::ForceInline ? "Inline insertion forced by [j2] route (diagnostic override)."
                                                : "Between-list insertion forced by [j2] route (diagnostic override).";
    }
    RouteMode Mode() const { return mode_; }
    const RouteDecision& Current() const { return d_; }
    void Reset()
    {
        const RouteMode m = mode_;
        *this = RouteSelector{};
        mode_ = RouteMode::Auto;
        SetMode(m);
    }

    // Close one frame. The returned decision applies to the NEXT frame.
    const RouteDecision& OnFrame(const FrameObservation& o)
    {
        if (o.interceptor && mode_ == RouteMode::Auto) {
            // A supported upscaler handler owns the view: explicit ownership, honoured at
            // once and without heuristic scene discovery or voting, so J2 never runs a
            // duplicate (Astra R4).
            if (d_.route != Route::Interceptor) set_route(Route::Interceptor, kInterceptorSignature, "upscaler call owns the view");
            d_.state = RouteState::Active;
            d_.matches = d_.observed = 0;
            last_proposal_ = Route::Interceptor;
            last_sig_ = kInterceptorSignature;
            d_.reason = "A supported upscaler call serves this view.";
            d_.missing = "";
            return d_;
        }
        if (!o.scene) return d_; // menus/loading: not a scene frame, nothing learned
        ++d_.scene_frames;
        if (o.native_jitter) ++d_.native_jitter_frames;
        if (o.camera_missed) ++d_.camera_misses;
        if (o.depth_state_missed) ++d_.depth_state_misses;
        d_.missing = o.camera_missed ? "camera not acquired this frame"
                   : o.depth_state_missed ? "depth state not established this frame"
                   : (!o.boundary_inside_list && !o.consumer_in_later_list && !o.interceptor)
                       ? "consumer of the scene colour not observed (bindless, indirect or untracked read)"
                       : "";
        // Outcome of the route that ran this frame.
        bool contradiction = false;
        if (o.executed) {
            ++d_.executed_frames;
            d_.contribution_established = o.lineage_to_present;
            if (o.later_scene_write) { ++d_.contradictions; contradiction = true; }
            else ++d_.placement_consistent;
        }
        if (mode_ != RouteMode::Auto) return d_; // overrides never relearn

        // Positive boundary evidence only (Astra R3): a usable in-list boundary, or a
        // later list that reads the final scene version. No observed consumer is NOT a
        // list boundary (same-list bindless post-processing would be invisible).
        Route proposal = Route::None;
        uint64_t sig = 0;
        const char* why = "No usable boundary observed: the scene's consumer is not visible.";
        if (o.boundary_inside_list) {
            proposal = Route::Inline;
            sig = o.inline_signature ? o.inline_signature : kInlineSignature;
            why = "Post-processing is recorded in the scene's own list: insertion inside that list.";
        } else if (o.consumer_in_later_list) {
            proposal = Route::Between;
            sig = kBetweenSignature;
            why = "The scene completes at a command-list boundary; a later list reads it.";
        }

        if (proposal == last_proposal_ && sig == last_sig_) ++d_.matches;
        else { d_.matches = 1; last_proposal_ = proposal; last_sig_ = sig; }
        ++d_.observed;

        if (d_.state == RouteState::Active && contradiction) {
            // A hard contradiction invalidates the active placement: stop executing it
            // (route none, new history generation) and relearn from scratch (Astra R1).
            // Already-recorded GPU work keeps its own lifetime in J2's ring.
            set_route(Route::None, 0, "contradiction: the scene was written again after our insertion");
            d_.state = RouteState::Observe;
            d_.observed = 1;
            d_.matches = 1;
            d_.reason = "Relearning after a contradiction.";
            return d_;
        }
        if (proposal == Route::None) {
            // Nothing to accept. An active route tolerates a few such frames (a missed
            // observation); otherwise keep observing and report what is missing.
            if (d_.state == RouteState::Active) {
                if (d_.matches < kMatchesToAccept) return d_;
                set_route(Route::None, 0, "boundary no longer observed");
                d_.state = RouteState::Observe;
                d_.observed = 0;
                d_.reason = why;
                return d_;
            }
            if (d_.observed >= kWindowFrames) {
                ++d_.unstable_windows;
                d_.observed = 0;
                d_.reason = "No usable boundary within 120 scene frames; still observing.";
            } else d_.reason = why;
            return d_;
        }
        if (d_.state == RouteState::Active) {
            if (proposal == d_.route) {
                // Harmless variation (another optional pass, a different producer
                // occurrence) keeps the dependency relationship: same route, same
                // history. The exact boundary is matched per frame by the inline trigger.
                d_.reason = why;
                d_.signature = sig;
                return d_;
            }
            if (d_.matches < kMatchesToAccept) return d_; // tolerate a few odd frames
            set_route(proposal, sig, "structure changed");
            d_.reason = why;
            return d_;
        }
        // Observe.
        if (d_.matches >= kMatchesToAccept) {
            set_route(proposal, sig, d_.route == Route::None ? "learned" : "relearned");
            d_.state = RouteState::Active;
            d_.reason = why;
            d_.observed = 0;
            return d_;
        }
        if (d_.observed >= kWindowFrames) {
            ++d_.unstable_windows;
            d_.observed = 0;
            if (d_.route != Route::None) set_route(Route::None, 0, "no stable structure within the window");
            d_.reason = "The frame structure did not settle within 120 scene frames; still observing.";
            d_.missing = "a stable scene-to-consumer boundary";
        }
        return d_;
    }

private:
    static constexpr uint64_t kBetweenSignature = 0xB1, kInterceptorSignature = 0x1C, kInlineSignature = 0x11;
    void set_route(Route r, uint64_t sig, const char* change)
    {
        const bool differs = r != d_.route;
        if (differs) {
            ++d_.history_generation;
            if (d_.route != Route::None || r != Route::None) ++d_.route_changes;
        }
        d_.route = r;
        d_.signature = sig;
        d_.change = change;
    }
    RouteMode mode_ = RouteMode::Auto;
    RouteDecision d_;
    Route last_proposal_ = Route::None;
    uint64_t last_sig_ = 0;
};

} // namespace na::j2
