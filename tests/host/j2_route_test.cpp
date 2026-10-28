// J2 automatic route selection (route_selector.hpp), pure CPU logic.
// Synthetic frame structures, each of which an older rule got wrong or must not:
//   - between-list scene (consumer in a later list)          -> Between
//   - scene + post-processing in ONE recording               -> Inline
//   - mid-frame read, more scene writes, final consumer later -> Between (the
//     first-candidate/first-read rule chose the mid-frame read)
//   - mid-frame read list, then a scene list that post-processes itself -> Inline
//   - variable optional passes / inline signature changes     -> same route, same history
//   - a contradiction (later scene write after our insertion) -> relearn, counted,
//     not placement-consistent
//   - alternating structures                                   -> stays None with a reason
//   - native camera jitter                                     -> evidence only, never None/"TAA"
//   - a structure from neither game (three scene lists, compute consumer in a fourth)
//   - overrides; route changes bump the history generation exactly once
// Self-contained (no D3D12): also compiles with a host compiler for quick checks.
#include "host_dx12/j2/route_selector.hpp"

#include <cstdio>
#include <vector>

namespace {
int g_failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_failures; } } while (0)
using namespace na::j2;

ListSummary scene_list(uint32_t writes = 10) { ListSummary s; s.renders_scene = true; s.color_writes = writes; s.reads = 0; return s; }
ListSummary scene_and_post(uint64_t sig) { ListSummary s = scene_list(); s.reads = 3; s.reads_after_last_write = 2; s.boundary_signature = sig; return s; }
ListSummary reader() { ListSummary s; s.reads = 1; return s; }
ListSummary mid_read_scene_list() { ListSummary s = scene_list(); s.reads = 1; s.reads_after_last_write = 0; return s; } // read, then wrote again

FrameObservation frame(const std::vector<ListSummary>& lists)
{
    FrameAggregator a;
    a.Reset();
    for (const auto& l : lists) a.AddList(l);
    return a.Observation();
}
} // namespace

int main()
{
    // 1. Anno-like: scene in two lists, post-processing reads it in a later list.
    {
        RouteSelector rs;
        const auto o = frame({scene_list(), scene_list(), reader()});
        CHECK(o.scene && o.scene_lists == 2 && o.consumer_in_later_list && !o.boundary_inside_list);
        for (int i = 0; i < 2; ++i) rs.OnFrame(o);
        CHECK(rs.Current().route == Route::None && rs.Current().state == RouteState::Observe); // not before 3 matches
        rs.OnFrame(o);
        CHECK(rs.Current().route == Route::Between && rs.Current().state == RouteState::Active);
        CHECK(rs.Current().history_generation == 1);
        CHECK(rs.Current().missing[0] == 0);
    }
    // 2. SER-like: the last scene list post-processes itself.
    {
        RouteSelector rs;
        const auto o = frame({scene_list(), scene_list(), scene_and_post(0x55)});
        CHECK(o.boundary_inside_list && o.inline_signature == 0x55);
        for (int i = 0; i < 3; ++i) rs.OnFrame(o);
        CHECK(rs.Current().route == Route::Inline && rs.Current().state == RouteState::Active);
    }
    // 3. Mid-frame read (refraction) then more scene writes, final consumer in a later
    //    list: the boundary is after the LAST write, not at the first read.
    {
        const auto o = frame({scene_list(), reader(), scene_list(), reader()});
        CHECK(o.consumer_in_later_list && !o.boundary_inside_list && o.scene_lists == 2);
        const auto o2 = frame({scene_list(), reader(), scene_list()}); // the later write invalidates the earlier reader
        CHECK(!o2.consumer_in_later_list);
        RouteSelector rs;
        for (int i = 0; i < 3; ++i) rs.OnFrame(o2);
        CHECK(rs.Current().route == Route::None); // no consumer observed is not a boundary (R3)
        CHECK(rs.Current().missing[0] != 0);
        for (int i = 0; i < 3; ++i) rs.OnFrame(o);
        CHECK(rs.Current().route == Route::Between);
    }
    // 4. A mid-read scene list, then a scene list that post-processes itself -> Inline.
    {
        const auto o = frame({mid_read_scene_list(), scene_and_post(7)});
        CHECK(o.boundary_inside_list && o.inline_signature == 7);
        // ...but a scene list that only read mid-scene is not an inline boundary.
        const auto o2 = frame({scene_and_post(7), mid_read_scene_list()});
        CHECK(!o2.boundary_inside_list);
    }
    // 5. Variable optional passes: the inline signature varies; route and history do not.
    {
        RouteSelector rs;
        for (int i = 0; i < 3; ++i) rs.OnFrame(frame({scene_and_post(1)}));
        CHECK(rs.Current().route == Route::Inline);
        const uint32_t gen = rs.Current().history_generation;
        for (uint64_t s = 2; s < 10; ++s) {
            FrameObservation o = frame({scene_list(), scene_and_post(s)});
            o.executed = true;
            rs.OnFrame(o);
        }
        CHECK(rs.Current().route == Route::Inline && rs.Current().history_generation == gen);
        CHECK(rs.Current().placement_consistent == 8 && rs.Current().contradictions == 0);
        CHECK(!rs.Current().contribution_established); // no lineage observed: unknown, not assumed
    }
    // 6. Contradiction: the scene was written again after our insertion.
    {
        RouteSelector rs;
        for (int i = 0; i < 3; ++i) rs.OnFrame(frame({scene_and_post(1)}));
        FrameObservation bad = frame({scene_and_post(1)});
        bad.executed = true;
        bad.later_scene_write = true;
        const uint32_t gen0 = rs.Current().history_generation;
        rs.OnFrame(bad);
        // One contradiction: a misplaced frame, tolerated (no route drop, no history reset).
        CHECK(rs.Current().contradictions == 1 && rs.Current().route == Route::Inline && rs.Current().history_generation == gen0);
        rs.OnFrame(bad);
        CHECK(rs.Current().route == Route::Inline);
        rs.OnFrame(bad); // the third within the window: the route itself is wrong
        CHECK(rs.Current().contradictions == 3 && rs.Current().placement_consistent == 0);
        CHECK(rs.Current().state == RouteState::Observe); // relearning
        CHECK(rs.Current().route == Route::None);          // and NOT still executing the invalid boundary (R1)
        CHECK(rs.Current().history_generation == gen0 + 1);
        // The structure is now actually between-list: it is accepted after 3 matches, once.
        const auto between = frame({scene_list(), reader()});
        const uint32_t gen = rs.Current().history_generation;
        rs.OnFrame(between);
        rs.OnFrame(between);
        CHECK(rs.Current().route == Route::None); // not yet: learning
        rs.OnFrame(between);
        CHECK(rs.Current().route == Route::Between && rs.Current().history_generation == gen + 1);
    }
    // 7. Alternating structures never settle: no route, a named reason, no oscillation.
    {
        RouteSelector rs;
        const auto a = frame({scene_list(), reader()});
        const auto b = frame({scene_and_post(3)});
        for (int i = 0; i < 130; ++i) rs.OnFrame(i % 2 ? a : b);
        CHECK(rs.Current().route == Route::None && rs.Current().unstable_windows == 1);
        CHECK(rs.Current().route_changes == 0);
    }
    // 8. Native jitter is evidence only.
    {
        RouteSelector rs;
        FrameObservation o = frame({scene_list(), reader()});
        o.native_jitter = true;
        for (int i = 0; i < 3; ++i) rs.OnFrame(o);
        CHECK(rs.Current().route == Route::Between && rs.Current().native_jitter_frames == 3);
    }
    // 9. Neither game: three scene lists, the consumer is a compute list (reads, no writes).
    {
        RouteSelector rs;
        const auto o = frame({scene_list(4), scene_list(4), scene_list(4), reader()});
        for (int i = 0; i < 3; ++i) rs.OnFrame(o);
        CHECK(rs.Current().route == Route::Between && o.scene_lists == 3);
        // A colour-clearing list restarts the image (a second view rendered from scratch).
        ListSummary restart = scene_list();
        restart.clears_color = true;
        const auto o2 = frame({scene_list(), scene_list(), restart, reader()});
        CHECK(o2.scene_lists == 1);
    }
    // 10. Menus/loading frames (no scene) are not learned from.
    {
        RouteSelector rs;
        for (int i = 0; i < 50; ++i) rs.OnFrame(frame({reader()}));
        CHECK(rs.Current().scene_frames == 0 && rs.Current().route == Route::None);
    }
    // 11. Overrides: fixed, shown, never relearned; removing the override observes again.
    {
        RouteSelector rs;
        rs.SetMode(RouteMode::ForceInline);
        CHECK(rs.Current().route == Route::Inline && rs.Current().override_active);
        for (int i = 0; i < 10; ++i) rs.OnFrame(frame({scene_list(), reader()}));
        CHECK(rs.Current().route == Route::Inline);
        rs.SetMode(RouteMode::Auto);
        CHECK(!rs.Current().override_active && rs.Current().state == RouteState::Observe);
        for (int i = 0; i < 3; ++i) rs.OnFrame(frame({scene_list(), reader()}));
        CHECK(rs.Current().route == Route::Between);
    }
    // 12. Interceptor: a supported upscaler call owns the view, at once, even when no
    //     scene was discovered heuristically (R4).
    {
        RouteSelector rs;
        FrameObservation o;
        o.interceptor = true;
        rs.OnFrame(o);
        CHECK(rs.Current().route == Route::Interceptor && rs.Current().state == RouteState::Active);
        // Ownership ends: J2 relearns from structure, with a new history generation.
        const uint32_t gen = rs.Current().history_generation;
        const auto inl = frame({scene_and_post(9)});
        for (int i = 0; i < 3; ++i) rs.OnFrame(inl);
        CHECK(rs.Current().route == Route::Inline && rs.Current().history_generation == gen + 1);
    }
    // 13. An image restarted (colour clear) outside the depth-paired scene list drops the
    //     earlier list's in-list boundary (R2); an ordinary composite does not.
    {
        ListSummary clear;
        clear.color_writes = 1;
        clear.clears_color = true;
        const auto o = frame({scene_and_post(17), clear});
        CHECK(!o.boundary_inside_list && o.inline_signature == 0);
        ListSummary composite;
        composite.color_writes = 1;
        const auto o2 = frame({scene_and_post(17), composite});
        CHECK(o2.boundary_inside_list && o2.inline_signature == 17);
    }
    // 15. Isolated contradictions spread over time never drop the route (SER flicker).
    {
        RouteSelector rs;
        for (int i = 0; i < 3; ++i) rs.OnFrame(frame({scene_and_post(1)}));
        for (int k = 0; k < 10; ++k) {
            FrameObservation bad = frame({scene_and_post(1)});
            bad.executed = true;
            bad.later_scene_write = true;
            rs.OnFrame(bad);
            for (int i = 0; i < 100; ++i) { FrameObservation ok = frame({scene_and_post(1)}); ok.executed = true; rs.OnFrame(ok); }
        }
        CHECK(rs.Current().route == Route::Inline && rs.Current().route_changes == 1 && rs.Current().contradictions == 10);
    }
    // 14. An active route whose boundary disappears (no consumer observed any more)
    //     tolerates a couple of frames, then stops instead of running blind.
    {
        RouteSelector rs;
        for (int i = 0; i < 3; ++i) rs.OnFrame(frame({scene_list(), reader()}));
        CHECK(rs.Current().route == Route::Between);
        rs.OnFrame(frame({scene_list()}));
        rs.OnFrame(frame({scene_list()}));
        CHECK(rs.Current().route == Route::Between);
        rs.OnFrame(frame({scene_list()}));
        CHECK(rs.Current().route == Route::None && rs.Current().missing[0] != 0);
    }
    if (g_failures) { std::printf("j2_route_test: FAILED (%d failures)\n", g_failures); return 1; }
    std::printf("j2_route_test: OK\n");
    return 0;
}
