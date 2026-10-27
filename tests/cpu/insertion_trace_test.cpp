#include "host_dx12/j2/insertion_trace.hpp"
#include <cstdio>

int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* what) { if (!ok) { std::printf("FAIL: %s\n", what); ++failures; } };
    na::j2::InsertionTrace trace;
    trace.Add({1, 0, 0, true}); // scene
    trace.Add({1, 2, 10, false}); // intermediate refraction
    trace.Add({1, 0, 0, true}); // more scene: invalidates first read
    check(trace.Analyze(1).final_candidate == 0, "an intermediate read is not final");
    trace.Add({1, 3, 20, false}); // postprocessing
    trace.Add({1, 4, 30, false}); // subsequent compositor must not move insertion later
    auto d = trace.Analyze(1);
    check(d.final_candidate == 2 && d.candidates == 3 && d.pipeline == 20 && d.destination == 3,
          "first read after final scene write selected");
    check(trace.Analyze(2).final_candidate == 0, "selection change does not inherit old evidence");
    trace.Add({1, 0, 0, true}); // late restart/clear
    check(trace.Analyze(1).final_candidate == 0, "late clear invalidates candidate");
    trace.Reset();
    trace.Add({1, 2, 10, false});
    check(trace.Analyze(1).candidates == 0, "reads before scene writes are not candidates");
    trace.Add({1, 0, 0, true});
    trace.Add({1, 1, 10, false});
    check(trace.Analyze(1).candidates == 0, "scene self bindings not postprocessing");
    trace.Add({1, 2, 10, false});
    trace.Add({1, 2, 10, false});
    check(trace.Analyze(1).final_candidate == 1, "repeated bindings do not choose a later read");
    for (int i = 0; i < 8192; ++i) trace.Add({9, 0, 0, true});
    check(trace.Analyze(1).final_candidate == 0, "truncation cannot invent finality");
    trace.Reset();
    trace.Add({1, 0, 0, true}); trace.Add({1, 2, 10, false});
    check(trace.Analyze(1).final_candidate == 1, "new recording recovers from overflow");
    uint32_t transition = 0;
    for (int extra_reads : {0, 5}) {
        trace.Reset();
        trace.Add({1, 0, 0, true});
        trace.Add({1, 0, UINT64_MAX, false});
        for (int i = 0; i < extra_reads; ++i) trace.Add({1, 2, 10, false});
        trace.Add({1, 0, 0, true});
        trace.Add({1, 0, UINT64_MAX, false});
        const auto final = trace.Analyze(1);
        if (!transition) transition = final.final_transition;
        check(final.final_transition == transition && transition == 2,
              "variable shader reads do not change final transition identity");
    }
    for (int optional_passes : {0, 1, 4}) {
        trace.Reset();
        for (int i = 0; i < optional_passes; ++i) {
            trace.Add({1, 0, 100, true});
            trace.Add({1, 0, UINT64_MAX, false});
        }
        trace.Add({1, 0, 200, true});
        trace.Add({1, 0, UINT64_MAX, false});
        auto final = trace.Analyze(1);
        check(final.boundary_writer == 200 && final.writer_occurrence == 1,
              "optional intermediate passes do not shift final producer identity");
        trace.Add({1, 0, 200, true});
        trace.Add({1, 0, UINT64_MAX, false});
        final = trace.Analyze(1);
        check(final.boundary_writer == 200 && final.writer_occurrence == 2,
              "repeated producer boundaries remain distinguishable");
    }
    return failures ? 1 : 0;
}
