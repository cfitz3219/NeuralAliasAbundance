// Standalone review reproductions. Does not alter or deploy the runtime.
#include "host_dx12/j2/route_selector.hpp"
#include <cstdio>

int main() {
    using namespace na::j2;
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
        failures += !ok;
    };
    FrameObservation scene;
    scene.scene = true;
    scene.boundary_inside_list = true;
    scene.inline_signature = 17;
    RouteSelector selector;
    for (int i = 0; i < 3; ++i) selector.OnFrame(scene);
    scene.executed = true;
    scene.later_scene_write = true;
    selector.OnFrame(scene);
    // apply_route_locked currently dispatches from route alone, ignoring state.
    check(selector.Current().route != Route::Inline,
          "contradicted inline route is not executable through the current route-only dispatch");

    FrameAggregator graph;
    ListSummary first;
    first.renders_scene = true;
    first.color_writes = 3;
    first.reads_after_last_write = 1;
    first.reads = 1;
    first.boundary_signature = 17;
    graph.AddList(first);
    ListSummary clear;
    clear.color_writes = 1;
    clear.clears_color = true; // clear after scene, no matching depth binding
    graph.AddList(clear);
    check(!graph.Observation().boundary_inside_list,
          "late non-scene clear invalidates the old inline boundary");

    RouteSelector unknown;
    FrameObservation no_consumer;
    no_consumer.scene = true;
    for (int i = 0; i < 3; ++i) unknown.OnFrame(no_consumer);
    check(unknown.Current().route == Route::None,
          "unknown consumer is not proof of a between-list insertion opportunity");

    RouteSelector interceptor;
    FrameObservation intercepted;
    intercepted.interceptor = true; // explicit interface, no heuristic scene found
    for (int i = 0; i < 3; ++i) interceptor.OnFrame(intercepted);
    check(interceptor.Current().route == Route::Interceptor,
          "explicit interceptor ownership does not depend on heuristic scene discovery");
    return failures ? 1 : 0;
}
