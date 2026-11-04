// Bridge frame record (v3): what the game side tells the DLSS side about one
// frame. Rebuilt from the recovered Alien bridge (recovered/dx11-enhancement,
// bridge_protocol.hpp v1) and widened for real upscaling.
//
// It knows nothing about any game. The game side fills it from whatever it
// found (depth, camera, jitter); the DLSS side checks it and runs DLSS.
//
// Wire layout: plain fixed-size fields, no pointers, identical in 32-bit and
// 64-bit builds, so a 32-bit game can talk to the 64-bit helper through
// shared memory. Portable C++17: also compiled and tested on Linux.
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace na::bridge {

constexpr uint32_t kRecordVersion = 3; // v3: camera motion built in the helper
constexpr uint32_t kMaxExtent = 8192; // per side, render or output

// Same numeric values as NVSDK_NGX_PerfQuality_Value (and na::dlss::Quality).
enum class Mode : uint32_t {
    Performance = 0,
    Balanced = 1,
    Quality = 2,
    UltraPerformance = 3,
    UltraQuality = 4,
    DLAA = 5,
};
const char* ModeName(Mode m);

// Which DLSS model setting best matches render size -> output size. DLSS uses
// the exact render size it is given; the mode only picks its model. Equal
// sizes -> DLAA.
Mode ModeForScale(uint32_t render_w, uint32_t render_h, uint32_t out_w, uint32_t out_h);

enum Flags : uint32_t {
    kReset = 1u << 0,          // history is invalid (cut, load, resize)
    kHdr = 1u << 1,            // colour is linear HDR
    kDepthInverted = 1u << 2,  // reversed-Z
    kMvJittered = 1u << 3,     // motion includes the jitter delta
    kMvDisplayRes = 1u << 4,   // motion is at output resolution
    kAutoExposure = 1u << 5,   // let DLSS work out exposure
    // v3: no motion texture from the game; the helper builds camera motion from
    // depth and `reproject` with our motion arbiter (as J2 does in DX12).
    kCameraMotion = 1u << 6,
    kKnownFlags = (1u << 7) - 1,
};

struct FrameRecord {
    uint32_t version = kRecordVersion;
    uint32_t sequence = 0;      // bridge-assigned, +1 per submitted frame
    uint64_t source_frame = 0;  // the game's own frame counter (for logs)
    // Valid region of colour/depth/motion, top-left of the shared textures.
    uint32_t render_w = 0, render_h = 0;
    uint32_t output_w = 0, output_h = 0;
    // Jitter in render pixels; colour sample was taken at (pixel centre - jitter).
    float jitter_x = 0.f, jitter_y = 0.f;
    // Multiply stored motion by this to get pixels of the motion texture.
    float mv_scale_x = 1.f, mv_scale_y = 1.f;
    float pre_exposure = 1.f;
    float exposure_scale = 1.f;
    float frame_ms = 16.6667f;
    uint32_t mode = static_cast<uint32_t>(Mode::DLAA);
    uint32_t preset = 0;  // DLSS render preset (10 J ... 13 M); 0 = NVIDIA default
    uint32_t flags = 0;
    // kCameraMotion only: current clip -> previous clip in ONE matrix (row-major
    // storage, row vectors: p_prev = p_cur * M), composed in double on the game
    // side (na::camera::ComposeReprojectionRow), and the depth of an empty pixel.
    float reproject[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float depth_clear = 1.f;
    uint32_t reserved = 0;
};
static_assert(std::is_standard_layout<FrameRecord>::value && std::is_trivially_copyable<FrameRecord>::value,
              "FrameRecord crosses processes as raw bytes");
static_assert(sizeof(FrameRecord) == 144, "FrameRecord layout must match in 32-bit and 64-bit builds");
static_assert(offsetof(FrameRecord, source_frame) == 8 && offsetof(FrameRecord, jitter_x) == 32 &&
                  offsetof(FrameRecord, flags) == 68 && offsetof(FrameRecord, reproject) == 72 &&
                  offsetof(FrameRecord, depth_clear) == 136,
              "FrameRecord field offsets are part of the wire format");

// What the helper reports back (written only by the helper).
enum class HelperState : uint32_t {
    Starting = 0,
    Ready = 1,    // shared objects opened, DLSS feature can be created
    Failed = 2,   // see reason; the game side stops using the helper
    Stopped = 3,
};

struct HelperStatus {
    uint32_t state = static_cast<uint32_t>(HelperState::Starting);
    uint32_t dlss_available = 0;
    uint64_t last_submitted = 0;  // highest sequence+1 whose GPU work was queued
    int32_t ngx_result = 0;
    uint32_t motion_frames = 0;   // frames whose camera motion the helper built (kCameraMotion)
    char reason[128] = {};
};
static_assert(std::is_standard_layout<HelperStatus>::value && std::is_trivially_copyable<HelperStatus>::value,
              "HelperStatus crosses processes as raw bytes");
static_assert(sizeof(HelperStatus) == 152, "HelperStatus layout must match in 32-bit and 64-bit builds");

// The whole shared-memory block for one bridge session.
struct SharedBlock {
    FrameRecord frame;
    HelperStatus status;
};
static_assert(sizeof(SharedBlock) == 296, "SharedBlock layout must match in 32-bit and 64-bit builds");

// Sizes the session was created with (shared textures are this big).
struct SessionLimits {
    uint32_t max_render_w = 0, max_render_h = 0;
    uint32_t output_w = 0, output_h = 0;
};

// nullptr when the record is usable for `limits` and `expected_sequence`,
// otherwise a plain-language reason. Never trusts anything it cannot check.
const char* Validate(const FrameRecord& f, const SessionLimits& limits, uint32_t expected_sequence);

// nullptr when limits are usable.
const char* ValidateLimits(const SessionLimits& limits);

} // namespace na::bridge
