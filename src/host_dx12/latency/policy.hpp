#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace na::latency {
// Explicit user choice overrides only the low-latency/boost bits. Preserve
// the game's limiter and marker configuration. -1 leaves ownership with game.
template<class Params> Params ModeRequest(Params p, int mode) {
    if (mode >= 0) { p.bLowLatencyMode = mode == 1 || mode == 2; p.bLowLatencyBoost = mode == 2; }
    return p;
}
inline bool NativeDuration(uint64_t start, uint64_t end, float& ms) {
    if (!start || end <= start || end - start > 1000000) return false;
    ms = static_cast<float>(end - start) / 1000.f;
    return true;
}
struct Distribution { uint32_t count = 0; float mean = 0, p95 = 0, max = 0; };
struct Samples {
    std::array<float, 256> data{};
    uint32_t next = 0, count = 0;
    void Add(float v) { if (!std::isfinite(v) || v < 0) return; data[next++ % data.size()] = v; if (count < data.size()) ++count; }
    Distribution Read() const {
        Distribution d; d.count = count; if (!count) return d;
        auto sorted = data; std::sort(sorted.begin(), sorted.begin() + count);
        for (uint32_t i = 0; i < count; ++i) d.mean += sorted[i];
        d.mean /= static_cast<float>(count);
        d.p95 = sorted[(count * 95 + 99) / 100 - 1]; d.max = sorted[count - 1]; return d;
    }
};
}
