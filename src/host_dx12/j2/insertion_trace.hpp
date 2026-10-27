#pragma once
#include <cstdint>
#include <vector>
#include <map>

namespace na::j2 {
// CPU-only recording evidence; never retains game objects or waits for the GPU.
// A read binding is an insertion candidate, not proof of an AA/tonemap shader.
struct InsertionEvent {
    uint64_t resource = 0, destination = 0, pipeline = 0;
    bool write = false;
};
struct InsertionDecision {
    uint32_t candidates = 0, final_candidate = 0, writes = 0;
    uint32_t all_reads = 0;
    uint32_t transitions = 0, final_transition = 0;
    uint64_t destination = 0, pipeline = 0;
    uint64_t boundary_writer = 0;
    uint32_t writer_occurrence = 0;
};
class InsertionTrace {
public:
    void Reset() { events_.clear(); complete_ = true; }
    void Add(InsertionEvent event) {
        if (events_.size() == 8192) { complete_ = false; return; }
        events_.push_back(event);
    }
    InsertionDecision Analyze(uint64_t color) const {
        InsertionDecision result;
        if (!complete_ || !color) return result;
        uint64_t writer = 0;
        std::map<uint64_t, uint32_t> writer_reads;
        for (const auto& event : events_) {
            if (event.resource != color) continue;
            if (!event.write) ++result.all_reads;
            if (!event.write && event.pipeline == UINT64_MAX) ++result.transitions;
            if (event.write) {
                writer = event.pipeline;
                ++result.writes;
                result.final_candidate = 0; // later geometry invalidates earlier reads
                result.final_transition = 0;
                result.destination = result.pipeline = 0;
                result.boundary_writer = 0;
                result.writer_occurrence = 0;
            } else if (result.writes && event.destination != color) {
                const uint32_t occurrence = event.pipeline == UINT64_MAX ? ++writer_reads[writer] : 0;
                ++result.candidates;
                // First downstream read after the LAST scene write, not last read:
                // later reads can already be HUD/compositing on processed content.
                if (!result.final_candidate) {
                    result.final_candidate = result.candidates;
                    result.final_transition = event.pipeline == UINT64_MAX ? result.transitions : 0;
                    result.destination = event.destination;
                    result.pipeline = event.pipeline;
                    result.boundary_writer = writer;
                    result.writer_occurrence = occurrence;
                }
            }
        }
        return result;
    }
private:
    std::vector<InsertionEvent> events_;
    bool complete_ = true;
};
} // namespace na::j2
