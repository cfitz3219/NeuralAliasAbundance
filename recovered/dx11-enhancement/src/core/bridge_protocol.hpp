#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace dx11_enhancement {
// Pointer-free wire layout shared by x86 and x64. Input keyed-mutex ownership
// protects this single slot too; the producer cannot overwrite an in-flight frame.
struct BridgeFrame {
    uint32_t version = 1;
    uint32_t sequence = 0;
    uint64_t source_frame = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    float jitter_x = 0;
    float jitter_y = 0;
    float motion_scale_x = 1;
    float motion_scale_y = 1;
    float exposure = 1;
    float pre_exposure = 1;
    float frame_ms = 16.6667f;
    uint32_t reset = 0;
};
static_assert(std::is_standard_layout_v<BridgeFrame> && std::is_trivially_copyable_v<BridgeFrame>);
static_assert(sizeof(BridgeFrame) == 56 && offsetof(BridgeFrame, source_frame) == 8);
static_assert(offsetof(BridgeFrame, jitter_x) == 24 && offsetof(BridgeFrame, reset) == 52);
}
