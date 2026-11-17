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
    // Alternate between two proven endings of the same scene. A middle-class
    // consumer can use the remembered ending, but never an unknown writer,
    // another target, or another occurrence of the same writer.
    na::j2::LearnedInsertion previous{1, 2, {}};
    previous.decision.final_candidate = previous.decision.final_transition = 2;
    previous.decision.boundary_writer = 100;
    previous.decision.writer_occurrence = 1;
    previous.decision.destination = 3;
    previous.decision.pipeline = 20;
    auto remembered = previous;
    remembered.decision.boundary_writer = 200;
    auto current = remembered.decision;
    check(na::j2::FindBoundary(previous, &remembered, 1, 1, 2, current) == &remembered,
          "previously delivered alternate ending wins over previous frame");
    current.boundary_writer = 300;
    check(!na::j2::FindBoundary(previous, &remembered, 1, 1, 2, current), "unknown writer cannot use a remembered ending");
    current = remembered.decision;
    ++current.writer_occurrence;
    check(!na::j2::FindBoundary(previous, &remembered, 1, 1, 2, current), "another writer occurrence remains ineligible");
    current = remembered.decision;
    check(!na::j2::FindBoundary(previous, &remembered, 1, 1, 9, current), "another depth cannot use the ending");
    {
        // A scene ending placed after its known last list (scene-end/known-ending): what the
        // lists already present in the same submission say about it.
        using na::j2::ColorEvent; using na::j2::Op;
        const auto ev = [](bool write, Op op) { ColorEvent c; c.write = write; c.op = op; return c; };
        check(na::j2::SceneTailVerdict({}) == 0, "an empty later list does not move the ending");
        check(na::j2::SceneTailVerdict({ev(false, Op::GraphicsRead), ev(false, Op::ComputeRead)}) == 0,
              "a reader still being learned does not move the ending");
        check(na::j2::SceneTailVerdict({ev(false, Op::GraphicsRead), ev(true, Op::DrawWrite)}) == 1,
              "a later scene draw in the same submission defers the ending");
        check(na::j2::SceneTailVerdict({ev(false, Op::CopyRead), ev(true, Op::DrawWrite)}) == -1,
              "a draw after the picture was copied onward is reuse, not continuation");
        check(na::j2::SceneTailVerdict({ev(true, Op::ClearWrite), ev(true, Op::CopyWrite)}) == 0,
              "clears and copies into the colour are not scene continuation");
        // Only delivered endings count for it: an empty previous-frame slot never matches.
        check(!na::j2::FindBoundary({}, nullptr, 0, 1, 2, current), "no delivered ending, no placement");
    }
    return failures ? 1 : 0;
}
