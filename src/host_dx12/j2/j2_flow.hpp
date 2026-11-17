// P4-0b: moving-object motion for J2 (live DLAA) from NVIDIA hardware optical
// flow, on the between-lists route only (plan: docs/P4-0B-OPTICAL-FLOW-PLAN-20261031.md,
// rev 2 with Astra/Sol's corrections, COMMS 6a15578).
//
// One frame, in GPU order on J2's queue:
//   game lists ... last scene list
//   PRE list (ours): copy the scene colour's subresource, encode it to BGRA8
//                    with ONE fixed transform (linear float: Reinhard + sRGB;
//                    UNORM: as stored), into an input ring slot. The encode is
//                    JITTER-STABILISED: texel q is sampled at q + jitter of its
//                    frame, so both images of a pair sit in the unjittered frame
//                    and DLAA's sub-pixel shake never reaches the engine (Sol's
//                    idea B, COMMS 6cb1720; the Anno post shimmer was the engine
//                    reading that shake on thin edges as 1-4 px motion).
//   Signal(ready)  -> NVOFA (its own engine) computes current -> previous flow
//                     and a per-block matching cost, then signals flow_done.
//                     Temporal hints are on only when the engine's previous job
//                     was the preceding pair of the same chain (idea A).
//   Wait(flow_done)   (GPU wait on J2's queue; the CPU never waits)
//   J2's DLAA list: depth copy, then RecordFusion (expand the grid depth-aware,
//                   subtract this pair's camera motion once (no jitter term: the
//                   images were stabilised), accept a residual only where the
//                   engine's cost, an image check and the previous pair agree),
//                   the arbiter (camera + accepted residual), DLSS
//   ... the rest of the game's batch
//
// Pairing: the previous image is used only when it is the frame J2's camera
// history pairs with (the last submitted DLAA frame), in the same size, format
// and route generation, with no cut, resize or reset in between. Otherwise the
// frame only encodes (so the next one can pair) and motion stays camera-only.
//
// Failure rules: every failure leaves DLAA running on camera motion. A failed
// NVOFA call enqueues no queue wait. A ticket that is not submitted is
// abandoned: the ready value is still signalled so the engine never waits
// forever, and the image is never used as a previous frame. A watchdog thread
// releases the queue (CPU-signals flow_done) and latches flow off if the engine
// stops answering. Ring slots are reused only after every GPU use has retired;
// a busy slot skips flow. A size change retires the old set only after all its
// work completed (no CPU waits), then rebuilds.
//
// Not covered here (deferred by the review): the inline route, MSAA colour.
#pragma once

#include <d3d12.h>
#include <cstdint>

// Forward declaration only: core/motion_arbiter.hpp and motion/live_flow.hpp both
// define na::motion::Output/Stats, so this header (included by the menu) must not pull them in.
namespace na::motion::gpu { struct Inputs; }

namespace na::j2::flow {

using ExecuteFn = void(__stdcall*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

// Engine seam: NVOFA by default; tests inject a fake with the same contract
// (Execute waits on wait_fence >= wait_value, signals signal_fence = signal_value).
class Engine {
public:
    virtual ~Engine() = default;
    virtual bool Available(ID3D12Device* device, char* error, size_t cap) = 0;
    virtual bool Init(ID3D12Device* device, uint32_t w, uint32_t h, uint32_t grid, uint32_t perf, bool cost) = 0;
    virtual bool CostEnabled() const = 0;
    virtual uint32_t OutWidth() const = 0;
    virtual uint32_t OutHeight() const = 0;
    virtual bool RegisterInput(ID3D12Resource* r, ID3D12Fence* f, uint64_t wait, uint64_t signal, void** out) = 0;
    virtual bool RegisterOutput(ID3D12Resource* r, ID3D12Fence* f, uint64_t wait, uint64_t signal, void** out) = 0;
    virtual bool RegisterCost(ID3D12Resource* r, ID3D12Fence* f, uint64_t wait, uint64_t signal, void** out) = 0;
    virtual bool Unregister(void* h) = 0;
    virtual uint32_t Outstanding() const = 0;
    // `hints`: the engine may use its previous result as temporal hints. True only when that
    // result was the immediately preceding pair of the same chain (prev image = that pair's
    // current image, same generation, nothing abandoned in between); false on the first pair
    // after a cut, gap, resize, restart, abandoned job or switch-on.
    virtual bool Execute(void* cur, void* prev, void* grid, void* cost, ID3D12Fence* wait_fence, uint64_t wait_value,
                         ID3D12Fence* signal_fence, uint64_t signal_value, bool hints) = 0;
    virtual bool Shutdown() = 0;
    virtual const char* LastError() const = 0;
};
void SetEngineForTests(Engine* engine); // nullptr = NVIDIA hardware optical flow
void SetWatchdogMsForTests(uint32_t ms); // default 1000

// The menu switch ([j2] flow=1 sets it at start). Off by default.
void SetEnabled(bool on);
bool Enabled();
// Pair frames whose scene list is followed by more game lists in the same submission
// (the batch is then split there: the game's lists still run once, in order). On by
// default: Anno's batches always continue after the scene. Off: such frames stay camera-only.
void SetAllowSplit(bool on);
bool AllowSplit();

struct Source {
    ID3D12Resource* color = nullptr; // the game's scene colour (not MSAA)
    uint32_t subresource = 0;
    uint32_t state = 0;              // its state where the pre list runs (restored after)
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; // typed format of J2's colour copies
    uint32_t width = 0, height = 0;  // the subresource's size (J2's render size)
    uint64_t frame = 0;              // J2 frame of this image
    uint64_t previous_frame = 0;     // the frame J2's camera history pairs with (0 = none)
    uint32_t generation = 0;         // J2 route/history generation
    bool reset = false;              // cut, resize, resume or tracker restart: no pair
    bool allow_pair = true;          // false: the batch cannot be split here
    float jitter_px[2] = {0, 0};     // this frame's raster jitter (render pixels)
    uint32_t slot = 0;               // J2 ring slot (descriptor block)
};

struct Ticket {
    bool valid = false;     // a pre list exists: Submit or Abandon it
    bool pair = false;      // the engine was given this pair; Submit makes the queue wait for it
    bool consumed = false;  // RecordFusion recorded tier-3 inputs from it
    ID3D12CommandList* pre = nullptr;
    uint64_t ready_value = 0, flow_value = 0;
    uint32_t cur = 0, prev = 0, grid = 0, pre_slot = 0, slot = 0;
    uint32_t epoch = 0;     // resource set the indices belong to
    float jitter_prev[2] = {0, 0};
    uint64_t frame = 0;
    uint32_t generation = 0;
    float jitter_cur[2] = {0, 0};
};

// Between-lists route, under J2's lock, before J2 records its own list. Never waits.
Ticket Prepare(ID3D12Device* device, ID3D12CommandQueue* queue, const Source& src);

struct Tier3 {
    ID3D12Resource* residual = nullptr; // RG32F, NON_PIXEL_SHADER_RESOURCE
    ID3D12Resource* mask = nullptr;     // R32F 0/1 (accepted), NON_PIXEL_SHADER_RESOURCE
    ID3D12Resource* valid = nullptr;    // R32F, NON_PIXEL_SHADER_RESOURCE
};
// In J2's DLAA list once `depth32` (R32F raw depth, J2's size) is NON_PIXEL_SHADER_RESOURCE.
// `camera` = J2's tier-2 arbiter inputs for this frame (same pair). True: *out is usable.
bool RecordFusion(ID3D12GraphicsCommandList* list, Ticket& t, ID3D12Resource* depth32,
                  const na::motion::gpu::Inputs& camera, Tier3* out);

// Forwards `batch` (game lists, then t.pre right before J2's list at `ours`, then the
// rest). With a consumed pair: [0, ours) / Signal(ready) / Wait(flow_done) / [ours, n).
// Otherwise one call, then Signal(ready).
void Submit(ID3D12CommandQueue* queue, Ticket& t, ID3D12CommandList* const* batch, UINT count, UINT ours, ExecuteFn forward);
// After the batch was forwarded and J2 signalled `j2_fence` = `value` behind its list.
void Committed(Ticket& t, ID3D12Fence* j2_fence, uint64_t value);
// The ticket's lists were not submitted (J2 forwarded the game's batch untouched).
void Abandon(ID3D12CommandQueue* queue, Ticket& t);
// Releases everything (device loss / J2 shutdown). The engine is torn down in the
// documented order; nvofapi64.dll stays loaded.
void Shutdown();

struct Stats {
    uint32_t enabled = 0, available = 0, active = 0, latched = 0, cost = 0;
    uint32_t width = 0, height = 0, engine_w = 0, engine_h = 0, grid = 0, perf = 0, downscale = 0;
    uint64_t pairs = 0, encode_only = 0, consumed = 0, abandoned = 0;
    uint64_t hinted = 0;          // pairs the engine ran with temporal hints (continuous chain)
    uint64_t skipped_busy = 0, skipped_split = 0, skipped_msaa = 0, skipped_reset = 0, skipped_resize = 0;
    uint64_t engine_failed = 0, watchdog_trips = 0;
    // Last read-back frame (pixels): accepted residual, and why others were not.
    uint64_t px_accepted = 0, px_invalid = 0, px_small = 0, px_cost = 0, px_offscreen = 0, px_image = 0; // px_image includes flat pixels
    uint64_t px_flicker = 0;      // passed every check but has no agreeing residual from the previous pair yet (jitter flicker)
    float mean_cost = 0;          // over pixels that reached the cost check
    float ms_last = 0, ms_avg = 0; // GPU time from the pre list's start to the end of fusion
    char reason[160] = {};
};
Stats GetStats();

} // namespace na::j2::flow
