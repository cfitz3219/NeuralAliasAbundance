#pragma once
#include <array>
#include <cstdint>

namespace na::latency {
// Observations only: incoming markers do not prove correct engine placement.
// No inferred frame IDs or reconstruction of missing phases.
struct NativeCall {
    uint64_t count = 0, successes = 0, device = 0, frame = 0;
    uint64_t caller = 0, begin_qpc = 0, end_qpc = 0;
    uint32_t thread = 0;
    int32_t result = -1;
};
struct NativeAudit {
    static constexpr uint32_t Sleep = 13, Unknown = 14, Slots = 15;
    std::array<NativeCall, Slots> calls{};
    void Record(uint32_t kind, NativeCall call) {
        auto& slot = calls[kind < Slots ? kind : Unknown];
        call.count = slot.count + 1;
        call.successes = slot.successes + (call.result == 0 ? 1 : 0);
        slot = call;
    }
};
}
