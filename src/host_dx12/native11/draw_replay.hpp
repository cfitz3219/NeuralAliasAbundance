// Draw replay on the native DX11 route (Cort 2026-11-20: "make replay available on DX11. It should be
// almost identically implemented"). The DX12 producer (observe/producers.cpp, "Draw replay") replays
// every qualifying scene draw with a stream-output twin of its pipeline; this is the same producer
// for the game's own D3D11 device:
//   - the game's vertex shaders are kept at CreateVertexShader (their bytecode), and a twin is made
//     from that bytecode with CreateGeometryShaderWithStreamOutput (SV_Position, 16 bytes, no
//     rasterised stream), on a worker thread as in DX12;
//   - right after a qualifying scene draw on the immediate context, the same draw runs again with
//     the twin bound as the geometry stage and this frame's position buffer as the stream-output
//     target at the draw's own offset, then the game's (empty) geometry stage and stream-output
//     targets are put back. Nothing is rasterised;
//   - the same gates as DX12 where D3D11 has the same thing, the same draw identity (shader,
//     counts, buffers), the same vertex budget and movers table (core/replay_budget.hpp Choose,
//     core/replay_movers.hpp), one table per producer;
//   - at the frame's hand-over the records are sealed and the positions packed into a texture the
//     DX12 effects device shares (Pack); the DX12 side copies them into a buffer and runs the very
//     same rasteriser (j2/replay_motion.hpp, the overload that takes a frame).
// Generic: no game data. The rest of the route (sharing, matching, tier 1) is native_d3d11/live_dlaa.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11UnorderedAccessView;

namespace na::native11::draw_replay {

// One replayed draw: the same fields as na::observe::ReplayRecord (check/token are always 0 here:
// every draw is written on the immediate context before the hand-over).
struct Record {
    uint64_t sig = 0, vb = 0;
    uint32_t first = 0, verts = 0, width = 0, height = 0;
    int32_t scissor[4]{};
};

// Positions a frame can hold (float4 each). The packed texture is kPackWidth texels wide.
constexpr uint32_t kCapacity = 4096u * 384u, kPackWidth = 4096u;
constexpr uint32_t kVerticesPerRow = kPackWidth / 4u;
constexpr uint32_t PackRows(uint64_t verts) { return static_cast<uint32_t>((verts + kVerticesPerRow - 1) / kVerticesPerRow); }
constexpr uint32_t kPackRows = PackRows(kCapacity);

// Hooks the device's CreateVertexShader to keep each vertex shader's bytecode. Idempotent.
bool Install(void* device);
// The CreateVertexShader hook's body: tests call it directly with a shader they made.
void NoteVertexShader(void* shader, const void* bytecode, size_t length);

// What the provider knows about a draw (its own Ctx), handed in so this module needs no hooks of its own.
struct DrawFacts {
    uint64_t frame = 0;          // the provider's frame (advances at Present)
    bool deferred = false;       // recorded on a deferred context: not replayed (immediate only, as DX12 replays only in a recording J2 reads)
    bool scene_depth = false;    // the bound depth is the selected scene depth, with depth test on
    bool depth_read_only = false; // bound through a read-only depth view: the draw cannot write it
    bool query_active = false;   // a raster query is open on this context (a second draw would count in it)
    // The game's own viewports (before our jitter offset) and the depth target's size.
    const float* viewport = nullptr; // TopLeftX, TopLeftY, Width, Height, MinDepth, MaxDepth of viewport 0
    uint32_t viewports = 0;
};
struct DrawArgs {
    bool indexed = false, indirect = false;
    uint32_t count = 0, instances = 1, start = 0, start_instance = 0;
    int32_t base_vertex = 0;
};

// Live settings, called once a frame by the consumer. `on` false: no draw is replayed (nothing else changes).
void Configure(bool on, uint64_t budget, bool movers);
bool Active();
// After the game's draw has been forwarded (immediate context, the game's thread).
void AfterDraw(ID3D11DeviceContext* context, const DrawFacts& facts, const DrawArgs& args);

// The frame's hand-over: its records (empty when nothing was replayed), and its vertex count. The
// frame is closed: later draws of it are not replayed.
struct Sealed { uint64_t frame = 0, verts = 0; std::vector<Record> draws; };
Sealed Seal(uint64_t frame);
// Records on `context` (the immediate one) the copy of the sealed frame's positions into `target`
// (RGBA8_UNORM, kPackWidth x at least PackRows(verts)): one texel carries the four bytes of one
// float component. A row-major copy restores the original float4 bytes without a decoder. Must
// come after the frame's last replayed draw and before that context's next scene draws.
bool Pack(ID3D11DeviceContext* context, ID3D11UnorderedAccessView* target, uint64_t verts);

// What the rasteriser learned about the draws it drew (the DX12 pass's learn sink).
struct Observation { uint64_t sig = 0, vb = 0; uint32_t visible = 0, apart = 0; };
void Learn(const Observation* seen, size_t count);

struct Stats {
    uint64_t noted = 0, built = 0, build_failed = 0, replayed = 0, vertices = 0, twin_queued = 0, twin_building = 0, twin_failed = 0;
    uint64_t no_shader = 0, overflow = 0, sealed = 0, packed = 0, pack_failed = 0;
    uint64_t not_sampled = 0, draw_too_large = 0, frame_full = 0, budget = 0;
    uint32_t budget_threshold = 0;
    bool on = false, movers_on = false;
    uint64_t movers = 0, suspects = 0, promoted = 0, let_go = 0, mover_draws = 0, mover_verts = 0, movers_left_out = 0, probe_verts = 0, reports = 0;
    char gates[320] = {}; // why scene draws were not replayed: "name=count ..." (only non-zero)
};
Stats GetStats();

} // namespace na::native11::draw_replay
