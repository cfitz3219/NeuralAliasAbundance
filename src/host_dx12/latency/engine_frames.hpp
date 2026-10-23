#pragma once
#include <array>
#include <cstdint>

namespace na::latency {
// IDs come from an engine adapter, never a Present counter. Different frames
// may overlap; every individual frame must traverse the complete phase order.
enum class Phase : uint8_t { Simulation, Input, SimulationEnd, Render, RenderEnd, Present, Presented };
struct EngineFrames {
    struct Frame { uint64_t id = 0; Phase phase = Phase::Simulation; bool valid = false; };
    std::array<Frame, 128> frames{};
    uint64_t newest = 0, complete = 0, rejected = 0;
    bool Accept(uint64_t id, Phase phase) {
        if (!id) { ++rejected; return false; }
        auto& f = frames[id % frames.size()];
        if (phase == Phase::Simulation) {
            if (id <= newest) { ++rejected; return false; }
            newest = id; f = {id, phase, true}; return true;
        }
        if (!f.valid || f.id != id || static_cast<unsigned>(phase) != static_cast<unsigned>(f.phase) + 1) {
            ++rejected;
            if (f.id == id) f.valid = false;
            return false;
        }
        f.phase = phase;
        if (phase == Phase::Presented) ++complete;
        return true;
    }
};
}
