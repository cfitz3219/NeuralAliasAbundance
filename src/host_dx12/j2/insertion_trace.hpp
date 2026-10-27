#pragma once
#include <cstdint>
#include <vector>

namespace na::j2 {
// CPU-only recording evidence; never retains game objects or waits for the GPU.
// A read binding is an insertion candidate, not proof of an AA/tonemap shader.
struct InsertionEvent {
    uint64_t resource = 0, destination = 0, pipeline = 0;
    bool write = false;
};
struct InsertionDecision {
    uint32_t candidates = 0, final_candidate = 0, writes = 0;
    uint64_t destination = 0, pipeline = 0;
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
        for (const auto& event : events_) {
            if (event.resource != color) continue;
            if (event.write) {
                ++result.writes;
                result.final_candidate = 0; // later geometry invalidates earlier reads
                result.destination = result.pipeline = 0;
            } else if (result.writes && event.destination != color) {
                ++result.candidates;
                // First downstream read after the LAST scene write, not last read:
                // later reads can already be HUD/compositing on processed content.
                if (!result.final_candidate) {
                    result.final_candidate = result.candidates;
                    result.destination = event.destination;
                    result.pipeline = event.pipeline;
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
