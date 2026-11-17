// DLAA for a 32-bit game. NVIDIA ships DLSS for 64-bit programs only, so the in-game half (32-bit,
// inside the game) hands each prepared frame to a 64-bit companion program that runs the very same
// DLSS backend. Pictures never cross as bytes: both sides open the same GPU textures and fences.
// This header is the whole contract between the two halves. Every field has a fixed width so a
// 32-bit and a 64-bit build agree on the layout; handles travel as 64-bit numbers that the in-game
// half has already duplicated into the companion.
#pragma once
#include <cstdint>

namespace na::dlaa_remote {

constexpr uint32_t kMagic = 0x41414c44; // "DLAA"
constexpr uint32_t kVersion = 1;
constexpr uint32_t kSlots = 3; // the in-game half's frame slots

enum class Kind : uint32_t { None = 0, Hello = 1, Bind = 2, Frame = 3, Bye = 4 };

// Start the companion's device on the game's adapter and open the two fences.
// `ready` is signalled by the in-game queue when a frame's inputs are written;
// `done` is signalled by the companion's queue when its output is written.
struct Hello {
    uint32_t adapter_luid_low;
    int32_t adapter_luid_high;
    uint64_t ready_fence;
    uint64_t done_fence;
    uint32_t copy_for_tests; // 1 = copy colour to output instead of DLSS (machines without DLSS)
    uint32_t reserved;
    char16_t data_path[260]; // writable folder for the DLSS runtime's own files
};

// The four textures of one frame slot, all the same size, created by the in-game half.
struct Bind {
    uint32_t slot;
    uint32_t width, height;
    uint32_t reserved;
    uint64_t color, depth, motion, output;
};

// One frame. Mirrors the scalar part of dlss::EvalDesc.
struct Frame {
    uint32_t slot;
    uint32_t preset;
    uint64_t value; // wait for `ready` to reach it, then signal `done` with it
    uint64_t view_id;
    float jitter_x, jitter_y;
    float mv_scale_x, mv_scale_y;
    float pre_exposure, exposure_scale;
    uint32_t quality;
    uint32_t flags; // FrameFlags
};
enum FrameFlags : uint32_t {
    kReset = 1u << 0, kHdr = 1u << 1, kDepthInverted = 1u << 2,
    kMvJittered = 1u << 3, kAutoExposure = 1u << 4,
};

struct Request {
    Kind kind;
    uint32_t reserved;
    union {
        Hello hello;
        Bind bind;
        Frame frame;
    };
};

struct Reply {
    uint32_t ok;        // 1 = done as asked
    int32_t hresult;    // first failing call, 0 when ok
    uint32_t available; // Hello: DLSS can run on this device
    uint32_t micros;    // time the companion spent on this request
    char reason[160];   // plain language when !ok or !available
};

// The shared block: one request and its reply at a time. The in-game half waits for each reply,
// so nothing queues up and a companion that stops answering is noticed on the very next frame.
struct Block {
    uint32_t magic, version;
    uint32_t companion_pid;
    uint32_t reserved;
    Request request;
    Reply reply;
};

static_assert(sizeof(Hello) == 32 + 520, "Hello layout");
static_assert(sizeof(Bind) == 48, "Bind layout");
static_assert(sizeof(Frame) == 56, "Frame layout");
static_assert(sizeof(Request) == 8 + sizeof(Hello), "Request layout");
static_assert(sizeof(Reply) == 176, "Reply layout");
static_assert(sizeof(Block) == 16 + sizeof(Request) + sizeof(Reply), "Block layout");

} // namespace na::dlaa_remote
