#include "host_dx12/diag/event_log.hpp"
#include "host_dx12/gpu/mem_alloc.hpp"
#include "host_dx12/gpu/services.hpp"
#include "host_dx12/diag/gpu_breadcrumbs.hpp"
#include "host_dx12/j2/live_dlaa.hpp"
#include "host_dx12/j2/camera_interloper.hpp"
#include "host_dx12/j2/game_motion.hpp"
#include "host_dx12/j2/replay_motion.hpp"
#include <unordered_map>
#include "host_dx12/capture/capture_session.hpp"
#include "host_dx12/j2/msaa_resolve.hpp"
#include "host_dx12/j2/nis_scaler.hpp"
#include "host_dx12/j2/native_hud.hpp"
#include "host_dx12/j2/hud_unblend.hpp"
#include "host_dx12/j2/hud_picture.hpp"
#include "core/jitter_domain.hpp"
#include "host_dx12/j2/route_selector.hpp"
#include "host_dx12/j2/consumer_profile.hpp"
#include "host_dx12/j2/post_entry.hpp"
#include "host_dx12/j2/jitter_writes.hpp"
#include "host_dx12/j2/j2_flow.hpp"
#include "host_dx12/j2/compute_flow.hpp"
#include "host_dx12/capture/j2_capture.hpp"
#include "host_dx12/ngx_observe/ngx_observe.hpp"
#include "host_dx12/upscalers/ffx_intercept.hpp"
#include "host_dx12/upscalers/xess_intercept.hpp"

#include "core/motion_arbiter_gpu.hpp"
#include "core/temporal_jitter.hpp"
#include "core/projection_window.hpp"
#include "host_dx12/depth_format.hpp"
#include "host_dx12/dlss/dlss_backend.hpp"
#include "host_dx12/nr/native_fg.hpp"
#include "host_dx12/observe/producers.hpp"
#include "host_dx12/observe/texture_operation.hpp"
#include "host_dx12/observe/operation_snapshot.hpp"
#include "host_dx12/settings.hpp"
#include "host_dx12/effects/xegtao.hpp"
#include "host_dx12/effects/donut.hpp"
#include "host_dx12/overlay/presenter.hpp"
#include "host_dx12/scale/settings_read_hook.hpp"
#include "providers/camera/camera_tracker.hpp"

#include <windows.h>
#include <wrl/client.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <atomic>
#include <set>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>
#include <string>

using Microsoft::WRL::ComPtr;

namespace na::j2 {
namespace {

constexpr uint32_t kRing = 6;            // frames in flight before a slot is reused (never waited on)
constexpr uint32_t kBlockBytes = 512;    // root CBVs have no size: read this much (the producer's own bound)
constexpr size_t kMaxBlocks = 256;       // unique constant blocks examined per frame (until the camera slot is learned)
constexpr uint32_t kMaxBlockRead = 1024; // bytes read from one constant buffer
constexpr uint64_t kViewId = 0x4A32;     // DLSS view for this source ("J2")

struct Slot {
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    UINT64 fence_value = 0;
    na::nr::native::RecordingLease native_scale_use;
    std::shared_ptr<na::donut::Frame> grade;
    std::shared_ptr<na::xegtao::Frame> ambient; // existing slot fence owns AO descriptors/resources
    std::shared_ptr<const PhysicalInput> physical; // Layer2 payload used by this slot's list: kept
                                                   // until the slot is reused (its fence completed)
};

struct PendingHistory {
    na::camera::CameraTracker tracker;
    // The interloper rule after this frame's camera, committed with the tracker: an attempt that
    // never submits (failed DLSS, failed recording) must not teach it a jump or a return.
    na::j2::InterloperFilter interlopers;
    bool has_interlopers = false;
    bool cut = false;
    uint32_t capture = 0; // J2 evidence capture token for this evaluation (0 = none)
    na::j2::compute_flow::Ticket compute_flow;
    uint32_t depth_source=0, camera_source=0;
    uint64_t camera_age=0;
    bool keep_reset = false; // a repeated frame: DLSS did not run, a pending history reset still applies
    uint64_t selection_version = 0; // live scene selection this evaluation used (same epoch = same version)
};

// CPU metadata of this frame's last submitted scene. No pooled command-list
// pointer survives Execute: games reset those lists before the consumer arrives.
struct SceneInput {
    uint64_t frame = 0, color = 0, depth = 0, producer = 0;
    uint32_t color_sub = 0, depth_sub = 0;
    uint32_t color_state=0,depth_state=0;
    bool native_states=false; // exact current-frame producer end states, not historical guesses
    na::camera::Matrix vp{};
    bool reversed = false;
    na::j2::InterloperFilter interlopers; // the rule after judging vp (when judged)
    bool judged = false;
    double jitter[2] = {};
    bool injected = false; // the scene's camera block held J1's (windowed) camera
    // I1.4 (COMMS a066d76): the Present-time source. vp/jitter are only a camera when
    // has_camera (J1's applied sample for that frame); otherwise record() predicts one from the
    // recent reads. at_present: states are what all submitted work left on J2's queue.
    bool has_camera = true;
    bool at_present = false;
};

struct State {
    std::mutex m;
    std::atomic<bool> enabled{false};
    std::atomic<bool> configured{false};
    std::atomic<bool> paused{false}; // the user's on/off switch in the overlay
    bool resume_reset = false;       // history is stale after a pause
    bool latched = false;            // stopped for this session after a failure (reason kept)
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    UINT64 fence_next = 1;
    Slot slots[kRing];
    uint32_t slot = 0;
    // Our textures (render size == output size: DLAA).
    ComPtr<ID3D12Resource> depth32, motion, provenance, output;
    ComPtr<ID3D12Resource> color_in; // plain copy of the scene colour's first slice / mip (DLSS input)
    ComPtr<ID3D12Resource> sharp;    // RCAS output (when sharpening is on)
    ComPtr<ID3D12Resource> jshift;   // jitter-shifted colour for DLAA (UNORDERED_ACCESS between uses)
    MsaaResolve msaa;
    UINT samples = 1, depth_samples = 1, sample_quality = 0, depth_quality = 0;
    DXGI_FORMAT depth_format = DXGI_FORMAT_UNKNOWN;
    uint32_t w = 0, h = 0;          // scene colour/depth allocation (J2's input copies)
    uint32_t rw = 0, rh = 0;        // P4-4: active render extent (camera, jitter, motion); == w,h for DLAA
    uint32_t ow = 0, oh = 0;        // P4-4: output size (output/sharp textures); == w,h for DLAA
    DXGI_FORMAT color_format = DXGI_FORMAT_UNKNOWN;
    na::motion::gpu::Arbiter arbiter;
    bool arbiter_ok = false;
    // The game's own motion texture, when one is proven against the picture (game_motion_decide.hpp).
    na::j2::game_motion::Feed game_motion;
    bool game_motion_ok = false;
    na::j2::replay_motion::Pass replay;
    bool replay_ok = false;
    uint64_t replay_fed_frame = 0;
    bool game_motion_logged = false;
    int game_motion_used = -2;
    na::camera::CameraTracker tracker;
    // A second camera that flips into the main camera's slot for 1-3 frames (SE5 menu, p25748).
    na::j2::InterloperFilter interlopers;
    // SE5 flashing (Cort 2026-11-02): ~10 % of frames had no readable camera, so they kept
    // the game's raw (aliased, mip-biased, unsharpened) colour between DLAA'd neighbours:
    // a flash on detailed texture. The last two unjittered cameras let such a frame run
    // DLAA on a predicted camera instead (see record(): 4 frames moving, 120 still).
    na::camera::Matrix cam_u0{}, cam_u1{}; // unjittered VPs of the last two read frames (u1 newest)
    uint64_t cam_f0 = 0, cam_f1 = 0;       // their frames (0 = none)
    bool cam_rev = false;
    uint64_t frame = 1;             // game Present interval, not Execute attempt
    uint64_t submitted_frame = 0;   // last interval that actually submitted DLAA
    uint64_t eval_selection_version = 0; // live selection version of the last committed evaluation
    // Games may draw the scene across several submissions per frame (Sniper Elite
    // Resistance). DLAA belongs after the LAST of them, before post-processing
    // reads the colour. Counted per frame; the previous frame's count is the target.
    // The target is the most common count over the last 16 frames (ties: the
    // smaller), so one odd frame cannot make DLAA alternate early/late/missing.
    // Mid-frame readers of the colour (refraction, reflections) do not end the
    // scene: Sniper Elite Resistance samples it between its scene submissions.
    uint32_t scene_batches_cur = 0, scene_batches_target = 0;
    uint8_t batch_hist[16] = {};
    uint32_t batch_hist_n = 0;
    // Inline insertion (games that record the end of the scene AND their
    // post-processing in one command list, Sniper Elite Resistance): DLAA is
    // recorded INTO the game's list at the moment it binds the scene colour for
    // reading, then the game's state is restored. Explicitly enabled in settings.
    bool inline_mode = false; // mirrors the route selector's Inline decision
    // Automatic route selection (COMMS-Continuation step 2): the selector decides
    // between-list / inline / none per view from per-frame structure; Execute and
    // the inline trigger only carry out its decision.
    na::j2::RouteSelector router;
    na::j2::FrameAggregator frame_agg;
    uint64_t route_prev_late = 0, route_prev_post_scene = 0, route_prev_no_camera = 0, route_prev_no_state = 0;
    uint32_t route_gen_applied = 0;
    bool dlss_ready = false;
    // Placement profile (docs/PLACEMENT-DESIGN-RESPONSE.md): role classes of the scene
    // colour's consumers, learned from SUBMITTED frames; the inline trigger fires at an
    // accepted class, else the boundary matcher decides as before.
    na::j2::ConsumerProfile profile;
    na::j2::PostEntryProfile entries;
    na::j2::InsertionEvent capture_entry;
    SceneInput waiting_scene;
    SceneInput native_scene;
    JitterWrites jitter_writes;
    std::vector<na::j2::ColorEvent> frame_seq; // this frame's writes/consumers of the colour, submission order
    bool frame_seq_complete = true;
    uint64_t scene_color_id = 0;
    uint32_t scene_w = 0, scene_h = 0;         // selected colour, mip 0 (class size reference)
    bool class_fired = false;                   // this frame's DLAA was triggered by a class
    na::j2::ClassKey fired_class;
    uint64_t fired_producer = 0;
    uint64_t fired_context = 0;
    uint32_t fired_mode = 0;                    // 1 class, 2 pair, 3 barrier, 4 boundary matcher, 5 between-list reader
    bool fired_barrier_event = false;           // the placement was judged at a barrier event
    // First scene write after this frame's DLAA (the late writer): pipeline, depth bound, where
    // (1 same list after our insertion, 2 a later list of the same batch, 3 a later batch).
    uint64_t late_frame = 0, late_pso = 0;
    uint32_t late_where = 0, late_writes = 0;
    bool late_depth = false;
    uint64_t route_prev_late_class = 0;
    uint64_t route_prev_external = 0; // the game's own upscaler evaluations so far (NGX + FSR + XeSS)
    uint32_t frame_rejects = 0;       // inline trigger rejections in the current frame
    char frame_reject_text[320] = {}; // the frame's first rejection, with both boundaries
    uint64_t dlss_tried_frame = ~0ull; // one early start attempt per frame
    bool inline_sample_locked = false; // NGX recorded: no new raster sample may start this frame
    bool inline_camera_submitted = false; // an earlier depth/camera batch may already be executing
    struct LearnedInsertion { uint64_t color = 0, depth = 0; na::j2::InsertionDecision decision; };
    LearnedInsertion insertion_frame, insertion_learned;
    // Scene-end shapes of recent frames that DID get DLAA (SE5 p31032, 2026-11-06: the game
    // alternates between two frame shapes; with only the previous frame's shape learned,
    // every shape change missed, so DLSS flipped on/off frame by frame). Most recent first.
    static constexpr int kShapes = 8;
    LearnedInsertion delivered_shapes[kShapes];
    int delivered_shape_count = 0;
    char insertion_trace[2048] = {};
    size_t insertion_trace_used = 0;
    uint64_t inl_frame = 0; // last frame an inline DLAA was recorded
    struct InlinePending {
        const void* list = nullptr;
        uint64_t rec = 0, frame = 0;
        uint32_t slot = 0;
        uint32_t writes = 0;
        uint64_t color = 0;
        PendingHistory history;
        bool valid = false;
    } inl;
    int64_t camera_offset = -1; // learned offset of the ViewProjection inside its block
    int64_t camera_param = -1;  // learned root parameter that binds the camera block
    int64_t camera_inv_delta = 64; // learned byte distance from the ViewProjection to its inverse
    int64_t camera_proj_delta = INT64_MIN; // learned distance to the camera's own projection matrix (none = INT64_MIN)
    bool camera_table = false;  // ... through a descriptor table
    // J1: this frame's injected camera jitter (see jitter_batch).
    struct Jitter {
        bool valid = false;
        bool applied = false;
        uint64_t res = 0, off = 0;           // the camera block it was found in
        double px[2] = {0, 0};               // sample, render pixels (DLSS: colour sampled at centre - jitter)
        uint8_t vp[2][64], inv[2][64];       // original matrices as stored (row / column layout)
        uint8_t jvp[2][64], jinv[2][64];     // jittered replacements
        bool has_proj = false;               // this camera's separate projection, including separate blocks
        uint8_t proj[2][64], jproj[2][64];   // original / jittered, row and column layouts
        uint32_t phase = 0;
        int layout = 0;                      // 0 row-major, 1 column-major (as the game stores it)
        // Projection window: the extent the sample was taken at (window or render size),
        // and the windowed replacements actually written when `windowed`.
        bool windowed = false;
        uint32_t ew = 0, eh = 0;
        uint8_t wvp[2][64], winv[2][64], wproj[2][64];
        // The previous frame's camera as the game stored it, and its windowed copy.
        bool has_prev = false;
        uint64_t frame = 0;                  // frame this sample was started in
        uint8_t prev[64], wprev[64];
    } jit;
    // Projection window engagement (see SetProjectionWindow).
    struct Window {
        bool engaged = false;
        uint64_t engage_frame = 0, last_frame = 0, hold_until = 0;
        uint32_t w = 0, h = 0, ew = 0, eh = 0;
        uint64_t drain_until = 0, scissor_from = UINT64_MAX;
        int mode = 0;
        uint32_t streak = 0;                 // consecutive presents with DLAA
        uint32_t recent_frames = 0, recent_missed = 0, recent_repeated = 0, breaker_stops = 0;
        uint64_t last_eval = 0;              // last frame DLSS evaluated a windowed corner (g.output holds it)
    } win;
    bool jitter_on = true; // DLAA needs jitter (with the texture mip bias set at start-up)
    // Viewport jitter: the sample set for each recent frame (index frame % 8).
    struct VpjEntry { uint64_t frame = 0; float x = 0.f, y = 0.f; };
    VpjEntry vpj[8];
    uint32_t vpj_phase = 0;
    uint64_t vpj_submitted_sample=0,vpj_submitted_frame=0,vpj_submitted_depth=0;
    // Render-small jitter witness: the picture DLSS gets did not show J1's sample shifts (still
    // detailed pixels matched unshifted, not at the declared jitter difference). No jitter is then
    // applied or declared until the next re-test (frame), so DLSS is never told about a shift that
    // never reached the image (Astra 4cbbf44: 96.9% of a still patch identical while jitter changed).
    bool jitter_unseen = false;
    uint64_t jitter_retest = 0;
    // Render-small: this frame's DLSS inputs other than colour, kept for PresentUpscale. depth32
    // and motion hold them (motion back in UAV at rest); the game queue orders them before Present.
    struct Deferred {
        bool valid = false;
        uint64_t frame = 0;          // J2 frame the inputs belong to (Present runs after OnPresent: frame + 1)
        float jx = 0.f, jy = 0.f;    // this frame's jitter, render pixels
        bool reset = false, reversed = false;
        uint32_t w = 0, h = 0;       // depth / motion size (the scene's)
        // Evidence capture (2026-11-04): the camera actually passed to the motion shader.
        na::camera::Matrix vp_cur{}, vp_prev{}, inv_cur{};
        std::array<double, 3> eye{}, forward{};
        uint32_t camera_source = 0, cut_reason = 0;
        uint64_t camera_age = 0, selection_version = 0;
        bool tracker_restart = false, flow_ran = false;
        ID3D12Resource* scene_color = nullptr; // exact pre-flow/pre-post copy of this deferred input
        float flow_jitter_x=0,flow_jitter_y=0;uint32_t flow_jitter_convention=0;uint64_t flow_jitter_epoch=0;
        ID3D12Resource *flow_residual = nullptr, *flow_valid = nullptr, *flow_mask = nullptr; // this frame's tier 3 (NPSR)
    } def;
    std::array<double, 3> def_prev_forward{}; bool def_prev_forward_ok = false; // fast-turn capture trigger
    // I1.4: J1's applied camera of the frame that just closed (OnPresent resets g.jit), for the
    // Present-time source; and the selected depth's clears seen while that frame was recorded.
    struct PresentCamera { bool valid = false; uint64_t frame = 0; na::camera::Matrix vp{}; double px[2] = {0, 0}; } pcam;
    uint32_t depth_clears_closed = 0;
    uint32_t small_csub = 0, small_dsub = 0; uint64_t small_color = 0, small_depth = 0; // last in-frame deferred record's views
    uint64_t small_last = 0;         // J2 frame PresentUpscale's DLSS last evaluated (history continues only from frame - 1)
    uint32_t small_ow = 0, small_oh = 0, small_rw = 0, small_rh = 0; // its last sizes
    Stats stats;
};
// Never destroyed: at process exit the observation hooks' own state may already
// be gone, and releasing D3D12 objects then would call into them.
State& g = *new State;
// Projection window, read by the scissor hook on game recording threads (no g.m there).
std::atomic<bool> g_window_enabled{false};
std::atomic<bool> g_render_small{false};
// I1.4: clears of the selected scene depth recorded since the last present (record time, any thread).
std::atomic<uint64_t> g_clear_watch{0};
std::atomic<uint32_t> g_depth_clears{0};
void on_depth_clear(ID3D12GraphicsCommandList*, uint64_t target)
{
    if (target && target == g_clear_watch.load(std::memory_order_relaxed)) g_depth_clears.fetch_add(1, std::memory_order_relaxed);
}
// Render-small: presents in a row shown stretched (no DLSS). A stretched frame shows the
// jittered picture as it is, so jitter stops after a few of them and resumes with DLSS.
std::atomic<uint32_t> g_small_stretch_streak{0};
std::atomic<ExecuteFn> g_forward{nullptr}; // the host's original ExecuteCommandLists (for PresentUpscale)
constexpr uint64_t kSmallViewId = kViewId + 1; // render-small: DLSS on the finished picture, its own history
std::atomic<bool> g_window_scissor{false};
std::atomic<bool> g_window_scissor_wanted{true}; // [j2] window_scissor (default on)
std::atomic<uint64_t> g_window_depth{0};
std::atomic<uint32_t> g_window_ew{0}, g_window_eh{0};
// P4-4 upscale target: set by Stage P / tests from any thread, read at recording.
std::mutex g_up_m;
UpscaleTarget g_up;
std::atomic<bool> g_native_bridge{false},g_native_reduced{false};
struct NativeExtent{uint32_t rw=0,rh=0,ow=0,oh=0;} g_native_extent; // under g_up_m
thread_local ID3D12GraphicsCommandList* t_native_hint=nullptr;
thread_local uint64_t t_native_color=0,t_native_depth=0;
// Audit 2026-11-06: the hint survives non-qualifying operations (bloom,
// luminance, SSR reading the same colour) for a bounded number of operations
// in the SAME recording, instead of being spent on the first draw after the
// bind. Each examined operation must itself read the scene colour.
thread_local uint64_t t_native_recording=0;thread_local uint32_t t_native_budget=0;
constexpr uint32_t kNativeHintOps=24;
// Why the bridge did or did not find the game's upscale pass (status + log).
struct NativeBridgeCounters {
    std::atomic<uint64_t> hints{0},examined{0},not_reading{0},snapshot{0},source_size{0},draw_state{0},viewport{0},
        no_destination{0},ambiguous{0},candidates{0},recorded{0},record_failed{0},after_skipped{0};
    std::mutex m;char reason[200]{};uint32_t src_w=0,src_h=0,src_fmt=0,seen_w=0,seen_h=0,seen_fmt=0;
};
NativeBridgeCounters& g_nb=*new NativeBridgeCounters;
void native_reject(std::atomic<uint64_t>& counter,const char* why){
    counter.fetch_add(1,std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_nb.m);std::snprintf(g_nb.reason,sizeof g_nb.reason,"%s",why);
}
PhysicalInputProvider g_phys; // under g_up_m

// Exactly one outcome per Provided / Unavailable lookup, on every exit of record().
struct PhysicalReport {
    PhysicalInputProvider p;
    uint64_t generation = 0;
    bool armed = false;
    PhysicalOutcome result = PhysicalOutcome::Failed;
    char why[160] = {};
    void arm(const PhysicalInputProvider& pp, uint64_t gen) { p = pp; generation = gen; armed = true; }
    void fail(const char* w) { result = PhysicalOutcome::Failed; std::snprintf(why, sizeof why, "%s", w); }
    ~PhysicalReport();
};

void reason(const char* r)
{
    std::snprintf(g.stats.reason, sizeof(g.stats.reason), "%s", r);
    auto& rc = g.stats.reason_counts;
    for (uint32_t i = 0; i < g.stats.reason_kinds; ++i)
        if (std::strncmp(rc[i].text, r, sizeof rc[i].text - 1) == 0) { ++rc[i].n; return; }
    if (g.stats.reason_kinds < sizeof rc / sizeof rc[0]) {
        auto& n = rc[g.stats.reason_kinds++];
        std::snprintf(n.text, sizeof n.text, "%s", r);
        n.n = 1;
    }
}
void committed_sources(const PendingHistory& p) {
    if(p.depth_source>=1 && p.depth_source<=4)++g.stats.depth_sources[p.depth_source-1];
    if(p.camera_source>=1 && p.camera_source<=6)++g.stats.camera_sources[p.camera_source-1];
    g.stats.depth_source_last=p.depth_source;g.stats.camera_source_last=p.camera_source;
    g.stats.camera_age_last=p.camera_age;
    g.stats.camera_age_max=(std::max)(g.stats.camera_age_max,p.camera_age);
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, UINT before, UINT after)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(before);
    b.Transition.StateAfter = static_cast<D3D12_RESOURCE_STATES>(after);
    return b;
}
D3D12_RESOURCE_BARRIER uav_barrier(ID3D12Resource* r)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = r;
    return b;
}
// Appends a transition unless the states are equal.
void add(std::vector<D3D12_RESOURCE_BARRIER>& v, ID3D12Resource* r, UINT before, UINT after)
{
    if (before != after) v.push_back(transition(r, before, after));
}
// One subresource only (the scene's mip / slice; the rest of the texture is never touched).
void addsub(std::vector<D3D12_RESOURCE_BARRIER>& v, ID3D12Resource* r, UINT sub, UINT before, UINT after)
{
    if (before == after) return;
    D3D12_RESOURCE_BARRIER b = transition(r, before, after);
    b.Transition.Subresource = sub;
    v.push_back(b);
}
void flush(ID3D12GraphicsCommandList* l, std::vector<D3D12_RESOURCE_BARRIER>& v)
{
    if (!v.empty()) l->ResourceBarrier(static_cast<UINT>(v.size()), v.data());
    v.clear();
}

bool texture(UINT w, UINT h, DXGI_FORMAT fmt, bool uav, D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& out)
{
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    out.Reset();
    return SUCCEEDED(na::gpu::mem::CreateResource(g.device.Get(),D3D12_HEAP_TYPE_DEFAULT,d,state,nullptr,out.GetAddressOf()));
}
// A typed format for our own copies of a (possibly typeless) game texture.
DXGI_FORMAT typed(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    default: return f;
    }
}
bool uav_store(DXGI_FORMAT fmt)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{};
    fs.Format = fmt;
    return SUCCEEDED(g.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))) &&
           (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
}
bool is_float_color(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R11G11B10_FLOAT || f == DXGI_FORMAT_R32G32B32A32_FLOAT ||
           f == DXGI_FORMAT_R32G32B32_FLOAT;
}

// ---- Debug view (Argon's motion-vector audit) ---------------------------------
// Replaces the DLAA result with a picture of what DLAA was given, so it can be
// judged in the game: 1 motion (hue = direction, brightness = speed, 4 px = full;
// black = no motion), 2 fine motion (0.2 px = full: reveals sub-pixel false
// motion on a still camera), 3 depth (banded). White = non-finite motion (no direction
// hue is ever white; magenta is the right-and-up hue, which misled the first Anno read).
std::atomic<int> g_debug_view{0};
struct DebugPass {
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12DescriptorHeap> heap;
    UINT inc = 0;
    bool tried = false, ok = false;
} g_dbg;
constexpr char kDebugHlsl[] = R"(
Texture2D<float2> t_mv : register(t0);
Texture2D<float> t_depth : register(t1);
RWTexture2D<float4> u_out : register(u0);
cbuffer C : register(b0) { uint mode; float scale; uint w; uint h; uint iw; uint ih; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= w || id.y >= h) return;
    uint2 p = id.xy * uint2(iw, ih) / uint2(w, h); // output pixel -> render pixel (P4-4)
    float3 c;
    if (mode == 3) {
        float d = t_depth[p];
        c = frac(d * float3(1.0, 16.0, 256.0));
    } else {
        float2 v = t_mv[p];
        if (any(isnan(v)) || any(isinf(v))) { c = float3(1, 1, 1); }
        else {
            float a = atan2(v.y, v.x) / 6.2831853;
            float3 hue = saturate(abs(frac(a + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0);
            c = hue * saturate(length(v) * scale);
        }
    }
    u_out[id.xy] = float4(c, 1.0);
})";
bool debug_init()
{
    if (g_dbg.tried) return g_dbg.ok;
    g_dbg.tried = true;
    D3D12_DESCRIPTOR_RANGE r[2]{};
    r[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    r[0].NumDescriptors = 2;
    r[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    r[1].NumDescriptors = 1;
    r[1].OffsetInDescriptorsFromTableStart = 2;
    D3D12_ROOT_PARAMETER p[2]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[0].DescriptorTable.NumDescriptorRanges = 2;
    p[0].DescriptorTable.pDescriptorRanges = r;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[1].Constants.Num32BitValues = 6;
    D3D12_ROOT_SIGNATURE_DESC rd{2, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> sig, err, code;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
        FAILED(g.device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_dbg.root))))
        return false;
    if (FAILED(D3DCompile(kDebugHlsl, sizeof(kDebugHlsl) - 1, "j2_debug.hlsl", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err)))
        return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_dbg.root.Get();
    pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    if (FAILED(g.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_dbg.pso)))) return false;
    // Slots 0..kRing-1: the in-frame DLAA evaluation; kRing..2*kRing-1: render-small's DLSS at Present.
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kRing * 2 * 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_dbg.heap)))) return false;
    g_dbg.inc = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_dbg.ok = true;
    return true;
}
// Writes the debug picture into `out` (UNORDERED_ACCESS, ow x oh); motion and depth32 (rw x rh)
// are readable (NPSR). slot < 2 * kRing (see debug_init).
bool debug_record(ID3D12GraphicsCommandList* l, int mode, ID3D12Resource* out, DXGI_FORMAT out_format, uint32_t ow, uint32_t oh,
                  uint32_t rw, uint32_t rh, uint32_t slot)
{
    if (!debug_init() || !out || slot >= 2 * kRing) return false;
    const UINT base = slot * 3;
    D3D12_CPU_DESCRIPTOR_HANDLE c = g_dbg.heap->GetCPUDescriptorHandleForHeapStart();
    c.ptr += static_cast<SIZE_T>(base) * g_dbg.inc;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    sv.Format = DXGI_FORMAT_R32G32_FLOAT;
    g.device->CreateShaderResourceView(g.motion.Get(), &sv, c);
    c.ptr += g_dbg.inc;
    sv.Format = DXGI_FORMAT_R32_FLOAT;
    g.device->CreateShaderResourceView(g.depth32.Get(), &sv, c);
    c.ptr += g_dbg.inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.Format = out_format;
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g.device->CreateUnorderedAccessView(out, nullptr, &uv, c);
    D3D12_GPU_DESCRIPTOR_HANDLE gh = g_dbg.heap->GetGPUDescriptorHandleForHeapStart();
    gh.ptr += static_cast<UINT64>(base) * g_dbg.inc;
    ID3D12DescriptorHeap* heaps[] = {g_dbg.heap.Get()};
    l->SetDescriptorHeaps(1, heaps);
    l->SetComputeRootSignature(g_dbg.root.Get());
    l->SetPipelineState(g_dbg.pso.Get());
    l->SetComputeRootDescriptorTable(0, gh);
    const float scale = mode == 2 ? 5.0f : 0.25f;
    uint32_t k[6] = {static_cast<uint32_t>(mode), 0, ow, oh, rw, rh};
    std::memcpy(&k[1], &scale, 4);
    l->SetComputeRoot32BitConstants(1, 6, k, 0);
    l->Dispatch((ow + 7) / 8, (oh + 7) / 8, 1);
    return true;
}

// ---- Sharpening: RCAS (Robust Contrast-Adaptive Sharpening) ------------------
// AMD FidelityFX Super Resolution 1 "RCAS" (ffx_fsr1.h, MIT licence, (c) Advanced
// Micro Devices): a 5-tap cross with contrast-dependent clipping limiters
// (not a guarantee against edge halos), with its noise detection
// (FSR_RCAS_DENOISE) so flat noisy areas are not amplified. Runs on the DLAA
// result before the game's tonemap and HUD, in a Reinhard-compressed copy of the
// HDR values (RCAS expects 0..1), expanded back afterwards. 100 is RCAS at full strength
// (0 stops); 0 skips the pass. The sharpening gain doubles every 50 over 1..200 (x1/4 near 0,
// x1 at 100, x4 at 200): one rate across the whole slider (Cort 2026-11-04: the top half barely
// changed when only the lobe ceiling rose). The menu's 0..100 slider is half this scale (menu 50
// = 100, menu 100 = 200). Contrast limiters and noise attenuation remain. No second filter.
struct RcasPass {
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12DescriptorHeap> heap;
    UINT inc = 0;
    bool tried = false, ok = false;
} g_rcas;
// Depth awareness (Cort 2026-11-04): RCAS's lobe eases off at depth edges (a relative 1/z
// jump to a 4-neighbour), so silhouettes do not get bright outlines. Without depth it is
// neutral. (The clarity term tried in 7eb7adb was removed at Cort's request: it haloed.)
constexpr char kRcasHlsl[] = R"(
Texture2D<float4> t_in : register(t0);
Texture2D<float>  t_depth : register(t1);
RWTexture2D<float4> u_out : register(u0);
cbuffer C : register(b0) {
    float gain; uint w; uint h; float open;
    uint has_depth; uint reversed; uint dw; uint dh;
};
float3 fwd(float3 x) { x = max(x, 0.0); return x / (1.0 + x); }
float3 inv(float3 t) { t = min(t, 0.99990); return t / (1.0 - t); }
float3 tap(int2 p) { p = clamp(p, int2(0, 0), int2(w - 1, h - 1)); return fwd(t_in[p].rgb); }
float min3(float a, float b, float c) { return min(a, min(b, c)); }
float max3(float a, float b, float c) { return max(a, max(b, c)); }
// Depth as a quantity that grows towards the camera for both conventions (reversed: d; standard:
// 1 - d; both affine in 1/z), compared relatively: an edge heuristic, not a metric distance.
float dlin(int2 q) { q = clamp(q, int2(0, 0), int2(dw - 1, dh - 1)); const float d = t_depth[q]; return reversed != 0 ? d : 1.0 - d; }
float rel(float a, float b) { return abs(a - b) / max(max(a, b), 1e-7); }
int2 to_depth(float2 full) { return int2(full * float2(dw, dh) / float2(w, h)); }

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= w || id.y >= h) return;
    int2 p = int2(id.xy);
    //    b
    //  d e f
    //    h
    float3 b = tap(p + int2(0, -1)), d = tap(p + int2(-1, 0)), e = tap(p), f = tap(p + int2(1, 0)), hh = tap(p + int2(0, 1));
    float alpha = t_in[p].a;
    // Depth edge (relative 1/z jump to any 4-neighbour of the depth texel): 0 flat .. 1 edge.
    float edge = 0;
    if (has_depth != 0) {
        const int2 q = to_depth(float2(p) + 0.5);
        const float z = dlin(q);
        const float r = max(max(rel(dlin(q + int2(1, 0)), z), rel(dlin(q - int2(1, 0)), z)),
                            max(rel(dlin(q + int2(0, 1)), z), rel(dlin(q - int2(0, 1)), z)));
        edge = smoothstep(0.01, 0.04, r);
    }
    // Luma times 2 (FSR1) for noise detection.
    float bL = b.b * 0.5 + (b.r * 0.5 + b.g), dL = d.b * 0.5 + (d.r * 0.5 + d.g), eL = e.b * 0.5 + (e.r * 0.5 + e.g);
    float fL = f.b * 0.5 + (f.r * 0.5 + f.g), hL = hh.b * 0.5 + (hh.r * 0.5 + hh.g);
    float nz = 0.25 * bL + 0.25 * dL + 0.25 * fL + 0.25 * hL - eL;
    float range = max3(max3(bL, dL, eL), fL, hL) - min3(min3(bL, dL, eL), fL, hL);
    nz = saturate(abs(nz) / max(range, 1e-6));
    nz = -0.5 * nz + 1.0;
    float3 mn4 = min(min(min(b, d), f), hh), mx4 = max(max(max(b, d), f), hh);
    float2 peakC = float2(1.0, -1.0 * 4.0);
    float3 hitMin = min(mn4, e) / max(4.0 * mx4, 1e-6);
    float3 hitMax = (peakC.x - max(mx4, e)) / (4.0 * mn4 + peakC.y);
    float3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-3.0 / 16.0, min(max3(lobeRGB.r, lobeRGB.g, lobeRGB.b), 0.0));
    lobe *= nz;
    lobe *= lerp(1.0, 0.35, edge);
    // Strength (Cort 2026-11-04: the same rate over the whole slider): RCAS at 100 % adds
    // g1 = 4a/(1-4a) times (centre - cross mean); the slider multiplies that gain, doubling it
    // every 50 % (x1/4 at 0+, x1 at 100 %, x4 at 200 %), and the lobe is solved back from it,
    // so it stays below 1/4 (finite) at any strength. 100 % is RCAS unchanged.
    if (gain != 1.0) {
        const float a1 = -lobe, g = gain * 4.0 * a1 / (1.0 - 4.0 * a1);
        lobe = -g / (4.0 * (1.0 + g));
    }
    float rcpL = 1.0 / (4.0 * lobe + 1.0);
    float3 o = (lobe * b + lobe * d + lobe * hh + lobe * f + e) * rcpL;
    // Soft halo control (the idea of separate soft bright/dark overshoot limits is from
    // bacondither's Adaptive-sharpen, BSD-2; this is our own formulation): where the centre lies
    // inside its cross's range (an edge or gradient, where halos form), overshoot beyond that
    // range is compressed smoothly towards a small ceiling, tighter for bright halos (4 % of the
    // local range) than dark ones (6 %). A centre that is itself the local peak or trough is
    // fine detail and keeps up to half the range. Flat areas are untouched. Per channel, in the
    // compressed 0..1 domain RCAS works in. Above 100 % the ceilings open up with the extra
    // strength (open 0..1), reaching half the range at 200 %, so the top of the slider still adds bite.
    {
        const float x = open;
        const float3 lo = min(mn4, e), hi = max(mx4, e), rng = hi - lo;
        const float3 peak = saturate(step(mx4, e) + step(e, mn4));
        const float3 kb = rng * lerp(0.04 + 0.46 * x, 0.5, peak) + 1e-6, kd = rng * lerp(0.06 + 0.44 * x, 0.5, peak) + 1e-6;
        const float3 ov = max(o - hi, 0.0), un = max(lo - o, 0.0);
        o = o - ov + ov * kb / (kb + ov);
        o = o + un - un * kd / (kd + un);
    }
    u_out[p] = float4(inv(o), alpha);
})";
bool rcas_init()
{
    if (g_rcas.tried) return g_rcas.ok;
    g_rcas.tried = true;
    D3D12_DESCRIPTOR_RANGE r[2]{};
    r[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    r[0].NumDescriptors = 2;
    r[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    r[1].NumDescriptors = 1;
    r[1].OffsetInDescriptorsFromTableStart = 2;
    D3D12_ROOT_PARAMETER p[2]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[0].DescriptorTable.NumDescriptorRanges = 2;
    p[0].DescriptorTable.pDescriptorRanges = r;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[1].Constants.Num32BitValues = 8;
    D3D12_ROOT_SIGNATURE_DESC rd{2, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> sig, err, code;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
        FAILED(g.device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_rcas.root))))
        return false;
    const UINT cf = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    if (FAILED(D3DCompile(kRcasHlsl, sizeof(kRcasHlsl) - 1, "j2_rcas.hlsl", nullptr, nullptr, "main", "cs_5_0", cf, 0, &code, &err)))
        return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_rcas.root.Get();
    pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    if (FAILED(g.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_rcas.pso)))) return false;
    // Slots 0..kRing-1: the DLAA path; kRing..2*kRing-1: the render-small upscale ring. Each slot
    // has one table of three descriptors (t0 in, t1 depth, u0 out).
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kRing * 2 * 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rcas.heap)))) return false;
    g_rcas.inc = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_rcas.ok = true;
    return true;
}
// RCAS from `in` (readable, NON_PIXEL_SHADER_RESOURCE) into `out` (UNORDERED_ACCESS).
// depth: optional R32_FLOAT dw x dh, NON_PIXEL_SHADER_RESOURCE (same picture, any size).
bool rcas_dispatch(ID3D12GraphicsCommandList* l, ID3D12Resource* in, ID3D12Resource* out, uint32_t w, uint32_t h, uint32_t percent,
                   DXGI_FORMAT fmt, UINT slot, ID3D12Resource* depth = nullptr, uint32_t dw = 0, uint32_t dh = 0, bool reversed = false)
{
    if (!percent || !in || !out || slot >= 2 * kRing || !rcas_init()) return false;
    if (!depth || !dw || !dh) depth = nullptr;
    const UINT base = slot * 3;
    auto cpu = [&](UINT i) { D3D12_CPU_DESCRIPTOR_HANDLE c = g_rcas.heap->GetCPUDescriptorHandleForHeapStart(); c.ptr += SIZE_T(base + i) * g_rcas.inc; return c; };
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    sv.Format = fmt;
    g.device->CreateShaderResourceView(in, &sv, cpu(0));
    sv.Format = DXGI_FORMAT_R32_FLOAT;
    g.device->CreateShaderResourceView(depth, &sv, cpu(1));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.Format = fmt;
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g.device->CreateUnorderedAccessView(out, nullptr, &uv, cpu(2));
    D3D12_GPU_DESCRIPTOR_HANDLE gh = g_rcas.heap->GetGPUDescriptorHandleForHeapStart();
    gh.ptr += UINT64(base) * g_rcas.inc;
    ID3D12DescriptorHeap* heaps[] = {g_rcas.heap.Get()};
    l->SetDescriptorHeaps(1, heaps);
    l->SetComputeRootSignature(g_rcas.root.Get());
    l->SetPipelineState(g_rcas.pso.Get());
    l->SetComputeRootDescriptorTable(0, gh);
    // One rate over the whole range: the gain doubles every 50 % (menu: every 25 points).
    const uint32_t pc = (std::min)(percent, 200u);
    const float gain = pc == 100 ? 1.0f : std::exp2((static_cast<float>(pc) - 100.0f) / 50.0f);
    const float open = pc > 100 ? (std::min)(1.0f, std::log2(gain) / 2.0f) : 0.0f;
    const float con = gain, limit = open;
    uint32_t c[8] = {0, w, h, 0, depth ? 1u : 0u, reversed ? 1u : 0u, dw, dh};
    std::memcpy(&c[0], &con, 4);
    std::memcpy(&c[3], &limit, 4);
    l->SetComputeRoot32BitConstants(1, 8, c, 0);
    l->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    return true;
}

// ---- Jitter shift (Cort 2026-11-04: "resolve the shaking from jitter") ----------
// DLSS is told a jitter the game's colour does not carry (our raster write never reaches the
// picture), so it moves the picture by the declared offset every frame: the shake. The colour
// DLSS gets is therefore resampled by that offset first (Catmull-Rom, 16 taps, render size), so
// the declared jitter and the colour agree and the output stands still. Depth and motion are
// unchanged (a sub-pixel shift of depth would invent values at silhouettes). [j2] jitter_shift:
// 1 on, -1 the opposite sign (if a run shows the sign is wrong), 0 off.
struct JitterShift {
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12DescriptorHeap> heap; // kRing * 2 slots x (t0, u0)
    UINT inc = 0;
    bool tried = false, ok = false;
} g_jshift;
constexpr char kJitterShiftHlsl[] = R"(
Texture2D<float4> t_in : register(t0);
RWTexture2D<float4> u_out : register(u0);
SamplerState s_lin : register(s0);
cbuffer C : register(b0) { uint w; uint h; float jx; float jy; };
float4 tap(float2 uv) { return t_in.SampleLevel(s_lin, uv, 0); }
// Catmull-Rom with 9 bilinear fetches (the usual 16-tap reduction).
float4 catmull(float2 pos) {
    float2 c = floor(pos - 0.5) + 0.5, f = pos - c;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2, off12 = w2 / w12;
    float2 t0 = c - 1.0, t3 = c + 2.0, t12 = c + off12;
    float2 sz = float2(w, h);
    float4 r = 0;
    r += tap(float2(t0.x, t0.y) / sz) * w0.x * w0.y;  r += tap(float2(t12.x, t0.y) / sz) * w12.x * w0.y;  r += tap(float2(t3.x, t0.y) / sz) * w3.x * w0.y;
    r += tap(float2(t0.x, t12.y) / sz) * w0.x * w12.y; r += tap(float2(t12.x, t12.y) / sz) * w12.x * w12.y; r += tap(float2(t3.x, t12.y) / sz) * w3.x * w12.y;
    r += tap(float2(t0.x, t3.y) / sz) * w0.x * w3.y;  r += tap(float2(t12.x, t3.y) / sz) * w12.x * w3.y;  r += tap(float2(t3.x, t3.y) / sz) * w3.x * w3.y;
    return r;
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= w || id.y >= h) return;
    const float2 pos = float2(id.xy) + 0.5 - float2(jx, jy); // DLSS: sample taken at centre - jitter
    float4 v = catmull(pos);
    const float4 n = t_in[id.xy];
    // Catmull-Rom can ring slightly: keep each channel inside the 3x3 range of the source.
    float4 mn = n, mx = n;
    [unroll] for (int j = -1; j <= 1; ++j) [unroll] for (int i = -1; i <= 1; ++i) {
        const float4 s = t_in[clamp(int2(id.xy) + int2(i, j), int2(0, 0), int2(w - 1, h - 1))];
        mn = min(mn, s); mx = max(mx, s);
    }
    u_out[id.xy] = clamp(v, mn, mx);
})";
bool jshift_init()
{
    if (g_jshift.tried) return g_jshift.ok;
    g_jshift.tried = true;
    D3D12_DESCRIPTOR_RANGE r[2]{};
    r[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    r[0].NumDescriptors = 1;
    r[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    r[1].NumDescriptors = 1;
    r[1].OffsetInDescriptorsFromTableStart = 1;
    D3D12_ROOT_PARAMETER p[2]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[0].DescriptorTable.NumDescriptorRanges = 2;
    p[0].DescriptorTable.pDescriptorRanges = r;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[1].Constants.Num32BitValues = 4;
    D3D12_STATIC_SAMPLER_DESC ss{};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.MaxLOD = D3D12_FLOAT32_MAX;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rd{2, p, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> sig, err, code;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
        FAILED(g.device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_jshift.root))))
        return false;
    if (FAILED(D3DCompile(kJitterShiftHlsl, sizeof(kJitterShiftHlsl) - 1, "j2_jitter_shift.hlsl", nullptr, nullptr, "main", "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err)))
        return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_jshift.root.Get();
    pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    if (FAILED(g.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_jshift.pso)))) return false;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kRing * 2 * 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_jshift.heap)))) return false;
    g_jshift.inc = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_jshift.ok = true;
    return true;
}
// A render-size target in UNORDERED_ACCESS for the shifted colour (kept across frames; replaced
// when the size or format changes).
bool jshift_target(ComPtr<ID3D12Resource>& tex, uint32_t w, uint32_t h, DXGI_FORMAT f)
{
    if (tex) {
        const auto d = tex->GetDesc();
        if (d.Width == w && d.Height == h && d.Format == f) return true;
        tex.Reset();
    }
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = f; rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&tex))))
        return false;
    tex->SetName(L"NeuralAlias jitter-shifted colour");
    return true;
}
// `in` readable (NON_PIXEL_SHADER_RESOURCE), `out` UNORDERED_ACCESS (left so; the caller
// transitions it for DLSS). Sign: DLSS's contract (EvalDesc, measured in dlss_real_test) is that
// pixel p's colour was sampled at (centre - jitter), so the shifted colour samples there.
bool jshift_dispatch(ID3D12GraphicsCommandList* l, ID3D12Resource* in, ID3D12Resource* out, uint32_t w, uint32_t h, DXGI_FORMAT f,
                     float jx, float jy, UINT slot)
{
    if (!in || !out || slot >= 2 * kRing || !jshift_init()) return false;
    const int sign = na::settings::Get().j2_jitter_shift.load(std::memory_order_relaxed);
    if (!sign) return false;
    D3D12_CPU_DESCRIPTOR_HANDLE c = g_jshift.heap->GetCPUDescriptorHandleForHeapStart();
    c.ptr += static_cast<SIZE_T>(slot) * 2 * g_jshift.inc;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = f;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    g.device->CreateShaderResourceView(in, &sv, c);
    c.ptr += g_jshift.inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.Format = f;
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g.device->CreateUnorderedAccessView(out, nullptr, &uv, c);
    D3D12_GPU_DESCRIPTOR_HANDLE gh = g_jshift.heap->GetGPUDescriptorHandleForHeapStart();
    gh.ptr += static_cast<UINT64>(slot) * 2 * g_jshift.inc;
    ID3D12DescriptorHeap* heaps[] = {g_jshift.heap.Get()};
    l->SetDescriptorHeaps(1, heaps);
    l->SetComputeRootSignature(g_jshift.root.Get());
    l->SetPipelineState(g_jshift.pso.Get());
    l->SetComputeRootDescriptorTable(0, gh);
    const float sx = static_cast<float>(sign) * jx, sy = static_cast<float>(sign) * jy;
    uint32_t k[4] = {w, h, 0, 0};
    std::memcpy(&k[2], &sx, 4);
    std::memcpy(&k[3], &sy, 4);
    l->SetComputeRoot32BitConstants(1, 4, k, 0);
    l->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    ++g.stats.jitter_shifted;
    return true;
}
// Retained for the standalone NIS test helper; the live slider uses RCAS only.
bool rcas_record(ID3D12GraphicsCommandList* l, uint32_t percent, DXGI_FORMAT fmt)
{
    return g.sharp && rcas_dispatch(l, g.output.Get(), g.sharp.Get(), g.ow, g.oh, percent, fmt, g.slot, g.depth32.Get(), g.rw, g.rh, g.stats.depth_reversed);
}

bool ensure_device(ID3D12CommandQueue* q)
{
    if (g.device && g.queue.Get() == q) return true;
    if (g.device) { reason("Live DLAA follows one queue; another queue submitted the scene."); return false; }
    ComPtr<ID3D12Device> dev;
    if (FAILED(q->GetDevice(IID_PPV_ARGS(&dev)))) return false;
    g.device = dev;
    g.queue = q;
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)))) return false;
    const D3D12_COMMAND_LIST_TYPE type = q->GetDesc().Type;
    for (auto& s : g.slots) {
        if (FAILED(dev->CreateCommandAllocator(type, IID_PPV_ARGS(&s.alloc))) ||
            FAILED(dev->CreateCommandList(0, type, s.alloc.Get(), nullptr, IID_PPV_ARGS(&s.list))))
            return false;
        s.list->Close();
    }
    g.arbiter_ok = g.arbiter.Init(dev.Get(), kRing);
    g.game_motion_ok = g.game_motion.Init(dev.Get(), kRing);
    g.replay_ok = g.replay.Init(dev.Get());
    return true;
}

// DLSS on J2's device (our own NGX instance, data in %TEMP%\NeuralAlias-ngx).
bool dlss_on()
{
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetTempPathW(MAX_PATH, path);
    if (!n || n >= MAX_PATH - 20) { reason("No folder for DLSS data."); return false; }
    wcscat_s(path, L"NeuralAlias-ngx");
    CreateDirectoryW(path, nullptr);
    if (!na::dlss::EnsureOn(g.device.Get(), path)) { reason("DLSS did not start on this device (see DLSS details)."); return false; }
    g.dlss_ready = true;
    return true;
}

// Carry out the route selector's decision (under g.m): inline mode for the inline
// trigger, device + DLSS ready for it, and a history reset when the route changes.
void apply_route_locked(ID3D12CommandQueue* queue)
{
    const auto& d = g.router.Current();
    // Inline is armed only after at least one scene frame was observed under this
    // decision: the trigger matches the boundary learned from a completed frame, and
    // a list recorded before any frame closed has nothing to match (legacy timing).
    g.inline_mode = g_native_reduced.load(std::memory_order_relaxed) || (d.route == na::j2::Route::Inline && d.scene_frames > 0);
    g.stats.inline_mode = g.inline_mode ? 1u : 0u;
    if (d.history_generation != g.route_gen_applied) {
        g.route_gen_applied = d.history_generation;
        if (d.history_generation > 1) ++g.stats.history_resets; // the first route is not a reset
        g.resume_reset = true;   // output of the old route must not feed the new one
        if (!g_native_reduced.load(std::memory_order_relaxed)) {
            if (g.inl.valid) na::capture::j2::Discard(g.inl.history.capture, true);
            g.inl.valid = false; // an inline recording from the old route is not committed
        }
        g.waiting_scene = {};
        g.scene_batches_target = 0;
    }
    // Ready before the game RECORDS the next scene list (the inline trigger runs then).
    // Only from Execute (queue given): it runs inside a host-work scope, so NGX's own
    // submissions are not routed back into J2. From Present they would re-enter
    // Execute while g.m is held.
    if (queue && d.route == na::j2::Route::Inline && !g.dlss_ready) {
        if (!g.device) ensure_device(queue);
        if (g.device) dlss_on();
    }
}

// The resource state at the end of the scene list: its last whole-resource
// transition there, else in an earlier list of the same batch.
// State of subresource `sub` after lists[0..last]. Barriers may name that
// subresource alone (depth vs stencil plane, one mip or slice) or all of them.
bool state_after(ID3D12CommandList* const* lists, UINT last, na_object_id id, uint32_t sub, uint32_t* st)
{
    for (int i = static_cast<int>(last); i >= 0; --i) {
        const auto evidence = na::observe::ListStateSub(lists[i], id, sub, st);
        if (evidence == na::observe::StateLookup::Known) return true;
        if (evidence == na::observe::StateLookup::Unavailable) return false;
    }
    // Not transitioned in this batch up to the scene: the state it carried in.
    return na::observe::KnownSubStateInCurrentSubmission(id, sub, st);
}

// ---- J1: camera jitter ---------------------------------------------------
// DLAA needs a different sub-pixel sample every frame. Anno (like most games
// without temporal AA) renders with a fixed camera, so J1 shifts it: before a
// batch is forwarded, the camera ViewProjection and its inverse in the bound
// constant buffers are replaced by jittered copies (core/temporal_jitter.hpp,
// Halton 2,3 over 8 phases, DLSS's recommendation for DLAA). The same
// replacement goes into every bound block that holds an identical copy, so a
// depth pre-pass in an earlier batch and the scene agree. J2 then reports the
// known sample to the camera tracker (which removes it exactly) and to DLSS.
// Runs only while DLAA is producing frames, so the shake is never left
// unresolved on screen.
constexpr uint32_t kJitterPhases = 8;      // upscaling base: JitterPhasesFor = 8 x ratio^2
constexpr uint32_t kDlaaJitterPhases = 16; // DLAA (Cort 2026-11-05: 16, was NVIDIA's minimum 8)
// The sample for a phase: Halton 2,3 (default) or Martin Roberts' R2 ([j2] jitter_sequence,
// menu, live). Both jitter routes and the value declared to DLSS come from this one function.
neural_alias::JitterSample jitter_sample(uint32_t phase, uint32_t period)
{
    return na::settings::Get().j2_jitter_r2.load(std::memory_order_relaxed)
        ? neural_alias::temporal_jitter_r2(phase, period) : neural_alias::temporal_jitter(phase, period);
}
uint32_t jitter_phases()
{
    // Render-small: the output is the window, known once PresentUpscale has run.
    if (g_render_small.load(std::memory_order_relaxed) && g.rw && g.stats.small_ow) return JitterPhasesFor(g.rw, g.stats.small_ow);
    return g.ow && g.rw ? JitterPhasesFor(g.rw, g.ow) : kDlaaJitterPhases;
}
// Viewport jitter ([j2] viewport_jitter): real samples through the game's own viewports.
bool vpj_mode() { return na::settings::Get().j2_viewport_jitter.load(std::memory_order_relaxed) != 0; }
// Draw-replay probe (SE5 smear, 2026-11-06): while DLAA/upscaling runs, count the scene draws that
// match a draw of the previous frame, and whether their vertex buffers stayed put. One
// replay_probe event at the first frame, then every 600 frames, with sums since the last one.
struct ReplayProbeSums { uint64_t frames = 0, draws = 0, matched = 0, same = 0, moved = 0, untracked = 0, stages = 0, instanced = 0, unique = 0; };
ReplayProbeSums g_rp_sum;
uint64_t g_rp_events = 0;
void replay_probe_frame()
{
    const bool running = g.submitted_frame && g.frame <= g.submitted_frame + 2;
    static bool on = false;
    if (running != on) { on = running; na::observe::SetReplayProbe(on); if (on) na::observe::SetRasterStateTracking(true); }
    // Draw replay capture: only while DLAA/upscaling runs (J2 takes and prepares a slot every frame)
    // and [j2] draw_replay is on, so a stopped DLAA never costs the game a second vertex pass.
    const bool replay = running && g.replay_ok && na::settings::Get().j2_draw_replay.load(std::memory_order_relaxed) != 0 && vpj_mode(); // matrix jitter would sit inside the clip positions (not removed yet)
    na::observe::SetDrawReplay(replay);
    {
        static uint64_t next_log = 0, logs = 0;
        if (replay && g.frame >= next_log) {
            next_log = g.frame + (logs ? 600 : 120);
            ++logs;
            na::observe::DrawReplayStats d{};
            na::observe::GetDrawReplayStats(&d);
            const auto p = g.replay.GetStats();
            na::eventlog::Emit("draw_replay", "noted=%llu built=%llu build_failed=%llu replayed=%llu verts=%llu not_ready=%llu twin_queued=%llu building=%llu twin_failed=%llu no_recipe=%llu hash_mismatch=%llu untwinned=%llu topology=%llu stages=%llu overflow=%llu | frames=%llu fed=%llu draws=%llu prev_draws=%llu exact=%llu loose=%llu unmatched=%llu raster_verts=%llu last_draws=%u last_pairs=%u fed_now=%d",
                (unsigned long long)d.noted, (unsigned long long)d.built, (unsigned long long)d.build_failed, (unsigned long long)d.replayed,
                (unsigned long long)d.vertices, (unsigned long long)d.not_ready, (unsigned long long)d.twin_queued, (unsigned long long)d.twin_building,
                (unsigned long long)d.twin_failed, (unsigned long long)d.no_recipe, (unsigned long long)d.hash_mismatch, (unsigned long long)d.untwinned,
                (unsigned long long)d.topology, (unsigned long long)d.stages, (unsigned long long)d.overflow,
                (unsigned long long)p.frames, (unsigned long long)p.fed, (unsigned long long)p.draws, (unsigned long long)p.prev_draws,
                (unsigned long long)p.matched_exact, (unsigned long long)p.matched_loose, (unsigned long long)p.unmatched, (unsigned long long)p.verts,
                p.last_draws, p.last_pairs, g.replay_fed_frame + 2 >= g.frame ? 1 : 0);
        }
    }
    na::observe::ReplayProbeStats st{};
    if (!na::observe::ReplayProbeFrame(&st) || !on) return;
    auto& m = g_rp_sum;
    ++m.frames; m.draws += st.draws; m.matched += st.matched; m.same += st.same_buffers; m.moved += st.moved_buffers;
    m.untracked += st.untracked; m.stages += st.extra_stages; m.instanced += st.instanced; m.unique += st.unique;
    if (g_rp_events && m.frames < 600) return;
    ++g_rp_events;
    na::eventlog::Emit("replay_probe", "frames=%llu draws=%llu matched=%llu same_buffers=%llu moved_buffers=%llu untracked=%llu extra_stages=%llu instanced=%llu unique=%llu last_draws=%u last_prev=%u last_matched=%u",
        (unsigned long long)m.frames, (unsigned long long)m.draws, (unsigned long long)m.matched, (unsigned long long)m.same,
        (unsigned long long)m.moved, (unsigned long long)m.untracked, (unsigned long long)m.stages, (unsigned long long)m.instanced,
        (unsigned long long)m.unique, st.draws, st.prev_draws, st.matched);
    m = ReplayProbeSums{};
}
// Frame start: the sample this frame's scene draws get, only while DLAA (or upscaling) is
// producing frames, so a stopped DLAA never leaves the game's picture moving.
void vpj_advance()
{
    const bool running = g.submitted_frame && g.frame <= g.submitted_frame + 2;
    if (!vpj_mode() || !g.jitter_on || !running || !g.w) { na::observe::SetSceneViewportJitter(0, 0.f, 0.f); return; }
    const neural_alias::JitterSample js = jitter_sample(++g.vpj_phase, jitter_phases());
    auto& e = g.vpj[g.frame % 8];
    e.frame = g.frame; e.x = static_cast<float>(js.x_pixels); e.y = static_cast<float>(js.y_pixels);
    na::observe::SetSceneViewportJitter(g.frame, e.x, e.y);
    ++g.stats.vpj_frames;
}
// Only the scene recording's actual draw sample may be declared. A frame-table
// entry is a request, not evidence that any draw used it.
bool vpj_declared(ID3D12CommandList* const* lists, UINT scene, float* x, float* y)
{
    uint64_t packed = 0;
    uint64_t depth=0,color=0;na::observe::GetLiveSelection(&depth,&color);
    if (lists) for (UINT i = scene + 1; i-- > 0 && !packed;) if (lists[i]) packed = na::observe::ListSceneViewportJitter(lists[i],depth);
    // Present-time acquisition has no source-list pointer. It may use the
    // sample actually submitted for this exact scene/depth, never a request.
    const bool submitted=!packed&&g.vpj_submitted_frame==g.frame&&g.vpj_submitted_depth==depth;
    if(submitted)packed=g.vpj_submitted_sample;
    uint32_t f = 0;
    bool ok = false;
    if (packed && na::observe::DecodeSceneViewportJitter(packed, &f, x, y)) {
        ok = true;if(submitted)++g.stats.vpj_from_submitted;else ++g.stats.vpj_from_list;
        if (f != static_cast<uint32_t>(g.frame)) ++g.stats.vpj_other_frame;
    }
    if (!ok) { ++g.stats.vpj_none; *x = *y = 0.f; return false; }
    if (na::settings::Get().j2_viewport_jitter.load(std::memory_order_relaxed) < 0) { *x = -*x; *y = -*y; }
    return true;
}

void store_matrix(const na::camera::Matrix& m, na::camera::Layout layout, uint8_t out[64])
{
    for (size_t r = 0; r < 4; ++r)
        for (size_t c = 0; c < 4; ++c) {
            const float v = static_cast<float>(m[r * 4 + c]);
            std::memcpy(out + (layout == na::camera::Layout::RowMajor ? r * 4 + c : c * 4 + r) * 4, &v, 4);
        }
}

// Projection window: clip' = W * clip, W mapping NDC x [-1,1] -> [-1, 2sx-1] and
// y [-1,1] -> [1-2sy, 1] (the top-left sx*w x sy*h pixels, D3D viewport, y up).
// Same convention as jitter_projection (row-major storage, column vectors, row 3 = w).
na::camera::Matrix window_of(const na::camera::Matrix& m, double sx, double sy)
{
    return neural_alias::window_projection(m, sx, sy);
}
bool window_now()
{
    return g.win.engaged && g.frame >= g.win.engage_frame && g.win.ew && g.win.eh;
}
// This frame's scene was drawn into the corner (J1 wrote the windowed camera this frame).
bool window_frame_now()
{
    return g.jit.windowed && g.jit.frame == g.frame && g.win.last_frame == g.frame && g.jit.ew && g.jit.eh;
}
// Extra windowed copies of the main camera written this frame (window_extra_camera): their
// windowed bytes -> jittered (unwindowed) bytes, for J2's own reads, VP and inverse.
struct WindowExtra { uint8_t written[64], jittered[64]; bool vp; };
std::set<uint64_t> g_camera_resources; // buffers that have carried the camera (priority when scanning)
// Record-time copy for the native HUD's draw hook (never under g.m there): the camera-carrying
// buffers and the camera's root slot.
std::mutex g_cam_pub_m;
uint64_t g_cam_pub[64]; size_t g_cam_pub_n = 0; int64_t g_cam_pub_param = -1; bool g_cam_pub_table = false;
int64_t g_cam_pub_offset = -1; bool g_cam_pub_geo_valid = false; na::camera::VpGeometry g_cam_pub_geo{};
void publish_camera_resources_locked()
{
    std::lock_guard<std::mutex> lock(g_cam_pub_m);
    g_cam_pub_n = 0;
    for (uint64_t r : g_camera_resources) if (g_cam_pub_n < 64) g_cam_pub[g_cam_pub_n++] = r;
    g_cam_pub_param = g.camera_param; g_cam_pub_table = g.camera_table; g_cam_pub_offset = g.camera_offset;
}
std::vector<WindowExtra> g_window_extras;
uint64_t g_window_extras_frame = UINT64_MAX;
// J2's own reads see the jittered (unwindowed) camera: windowed bytes the game's blocks
// now hold are mapped back, so camera search, continuity and tracking are unchanged.
void unwindow(uint8_t* bytes, size_t n)
{
    if (!g.jit.windowed) return;
    for (size_t o = 0; o + 64 <= n; o += 16)
        for (int l = 0; l < 2; ++l) {
            if (!std::memcmp(bytes + o, g.jit.wvp[l], 64)) { std::memcpy(bytes + o, g.jit.jvp[l], 64); break; }
            if (!std::memcmp(bytes + o, g.jit.winv[l], 64)) { std::memcpy(bytes + o, g.jit.jinv[l], 64); break; }
            if (g.jit.has_proj && !std::memcmp(bytes + o, g.jit.wproj[l], 64)) { std::memcpy(bytes + o, g.jit.jproj[l], 64); break; }
            if (l == 0 && g.jit.has_prev && !std::memcmp(bytes + o, g.jit.wprev, 64)) { std::memcpy(bytes + o, g.jit.prev, 64); break; }
            if (l == 0 && g_window_extras_frame == g.frame)
                for (const auto& x : g_window_extras)
                    if (!std::memcmp(bytes + o, x.written, 64)) { std::memcpy(bytes + o, x.jittered, 64); break; }
        }
}

// P is this camera's projection when P^-1 * VP is a rigid view (VP = P * V).
bool is_projection_of(const na::camera::Matrix& p, const na::camera::Matrix& vp)
{
    na::camera::Matrix pinv{}, v{};
    if (!na::camera::Invert(p, pinv)) return false;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            double a = 0;
            for (int k = 0; k < 4; ++k) a += pinv[r * 4 + k] * vp[k * 4 + c];
            v[r * 4 + c] = a;
        }
    na::camera::Candidate cand{};
    return na::camera::RigidView(v, cand);
}

void remember_projection(const na::camera::Matrix& p, const uint8_t* exact, na::camera::Layout layout)
{
    auto& j = g.jit;
    const auto jp = neural_alias::jitter_projection(p, neural_alias::JitterSample{j.px[0], j.px[1]}, j.ew, j.eh);
    for (int l = 0; l < 2; ++l) {
        const auto L = l ? na::camera::Layout::ColumnMajor : na::camera::Layout::RowMajor;
        store_matrix(p, L, j.proj[l]);
        store_matrix(jp, L, j.jproj[l]);
        if (j.windowed) store_matrix(window_of(jp, double(j.ew) / g.win.w, double(j.eh) / g.win.h), L, j.wproj[l]);
    }
    std::memcpy(j.proj[layout == na::camera::Layout::RowMajor ? 0 : 1], exact, 64);
    j.has_proj = true;
}

// A camera block's bytes hold a VP + inverse at the learned offset: start this frame's jitter.
// Last main camera the jitter started from (a block outside the learned camera
// slot may start a frame only if it shows that same camera, not a light's).
struct { bool valid = false; na::camera::VpGeometry geo{}; int hand = 0; } g_jit_cam;

// Winding of a ViewProjection: the sign of the determinant of its x, y and w rows (3x3 part).
// A mirrored camera (a planar water / mirror reflection) has the opposite winding of the main
// camera in every engine, whatever its own convention (left- or right-handed): a universal
// tell, used once the main camera is known.
int handedness(const na::camera::Matrix& m)
{
    const double a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g3 = m[12], h = m[13], i = m[14];
    const double det = a * (e * i - f * h) - b * (d * i - f * g3) + c * (d * h - e * g3);
    return det > 0 ? 1 : det < 0 ? -1 : 0;
}
void remember_main_camera(const na::camera::Matrix& m)
{
    g_jit_cam.valid = na::camera::DecomposeViewProjection(m, g_jit_cam.geo);
    g_jit_cam.hand = handedness(m);
    std::lock_guard<std::mutex> lock(g_cam_pub_m);
    g_cam_pub_geo_valid = g_jit_cam.valid;
    g_cam_pub_geo = g_jit_cam.geo;
}

bool same_camera(const na::camera::Matrix& m)
{
    na::camera::VpGeometry v{};
    if (!g_jit_cam.valid || !na::camera::DecomposeViewProjection(m, v)) return false;
    const auto& p = g_jit_cam.geo;
    double dot = 0, d2 = 0, len2 = 0;
    for (int j = 0; j < 3; ++j) {
        dot += v.forward[j] * p.forward[j];
        d2 += (v.position[j] - p.position[j]) * (v.position[j] - p.position[j]);
        len2 += p.position[j] * p.position[j];
    }
    return dot > 0.98 && std::abs(v.sy - p.sy) <= 0.05 * std::abs(p.sy) && d2 <= 0.01 * len2 + 100.0;
}

// A block from the camera slot starts this frame's jitter only if it continues the last main
// camera: a water reflection's mirrored camera (or a sky / light camera) can sit in the same slot
// and be bound earlier in the frame. Generous enough for real motion within one frame.
bool continues_main_camera(const na::camera::Matrix& m)
{
    na::camera::VpGeometry v{};
    if (!g_jit_cam.valid) return true;
    if (!na::camera::DecomposeViewProjection(m, v)) return false;
    if (g_jit_cam.hand && handedness(m) == -g_jit_cam.hand) return false; // mirrored: a reflection
    const auto& p = g_jit_cam.geo;
    double dot = 0, d2 = 0;
    for (int j = 0; j < 3; ++j) {
        dot += v.forward[j] * p.forward[j];
        d2 += (v.position[j] - p.position[j]) * (v.position[j] - p.position[j]);
    }
    return std::acos(std::clamp(dot, -1.0, 1.0)) <= 0.25 && d2 <= 25.0 * 25.0 && std::abs(v.sy - p.sy) <= 0.05 * std::abs(p.sy);
}

bool begin_jitter(uint64_t res, uint64_t off, const std::vector<uint8_t>& in_bytes, bool slot)
{
    const size_t n = in_bytes.size();
    const bool windowed = window_now();
    const uint32_t jw = windowed ? g.win.ew : g.rw, jh = windowed ? g.win.eh : g.rh; // extent the sample is taken at
    if (g.camera_offset < 0 || static_cast<uint64_t>(g.camera_offset) + 64 > n || !jw || !jh) return false;
    // jitter_batch has normalized this exact buffer range using the write ledger.
    const auto& bytes = in_bytes;
    for (auto layout : {na::camera::Layout::RowMajor, na::camera::Layout::ColumnMajor}) {
        const auto m = na::camera::LoadMatrix(bytes, static_cast<size_t>(g.camera_offset), layout);
        na::camera::Candidate c{};
        if (!na::camera::ViewProjection(m, c)) continue;
        if (!slot && !same_camera(m)) continue;
        if (slot && !continues_main_camera(m)) { ++g.stats.jitter_slot_other_camera; continue; }
        // The inverse at its learned position. A block from the camera slot must
        // carry it; another block (an earlier pass with a smaller constant buffer)
        // may start the frame from the ViewProjection alone, the inverse being
        // computed; the game's own inverse copy is adopted when the slot block shows it.
        const int64_t io = g.camera_offset + g.camera_inv_delta;
        const bool inv_here = io >= 0 && static_cast<uint64_t>(io) + 64 <= n;
        na::camera::Matrix inv{};
        if (inv_here) {
            inv = na::camera::LoadMatrix(bytes, static_cast<size_t>(io), layout);
            if (!na::camera::IsInversePair(m, inv)) {
                if (slot) continue;
                if (!na::camera::Invert(m, inv)) continue;
            }
        } else {
            if (slot || !na::camera::Invert(m, inv)) continue;
        }
        remember_main_camera(m);
        const uint32_t phases = windowed ? JitterPhasesFor(jw, g.win.w) : jitter_phases();
        const neural_alias::JitterSample js = jitter_sample(++g.jit.phase, phases);
        std::array<double, 16> src{};
        for (int i = 0; i < 16; ++i) src[i] = m[i];
        const auto jm = neural_alias::jitter_projection(src, js, jw, jh);
        na::camera::Matrix jvp{}, jinv{};
        for (int i = 0; i < 16; ++i) jvp[i] = jm[i];
        if (!na::camera::Invert(jvp, jinv)) return false;
        const double sx = windowed ? double(jw) / double(g.win.w) : 1.0, sy = windowed ? double(jh) / double(g.win.h) : 1.0;
        na::camera::Matrix wvp{}, winv{};
        if (windowed) {
            wvp = window_of(jvp, sx, sy);
            if (!na::camera::Invert(wvp, winv)) return false;
        }
        auto& j = g.jit;
        // The previous frame's camera bytes (games keep it for their own reprojection):
        // its windowed copy keeps those passes consistent with this frame's corner.
        const bool prev_ok = j.frame && j.frame + 1 == g.frame && j.layout == (layout == na::camera::Layout::RowMajor ? 0 : 1);
        if (prev_ok) std::memcpy(j.prev, j.vp[j.layout], 64);
        j.has_prev = prev_ok && windowed;
        if (j.has_prev) {
            const std::vector<uint8_t> pb(j.prev, j.prev + 64);
            store_matrix(window_of(na::camera::LoadMatrix(pb, 0, layout), sx, sy), layout, j.wprev);
            if (!std::memcmp(j.prev, bytes.data() + g.camera_offset, 64)) j.has_prev = false; // a still camera: the current VP
        }
        j.frame = g.frame;
        j.windowed = windowed;
        j.ew = jw;
        j.eh = jh;
        j.applied = false;
        j.res = res;
        j.off = off;
        j.px[0] = js.x_pixels;
        j.px[1] = js.y_pixels;
        j.layout = layout == na::camera::Layout::RowMajor ? 0 : 1;
        for (int l = 0; l < 2; ++l) {
            const auto L = l ? na::camera::Layout::ColumnMajor : na::camera::Layout::RowMajor;
            store_matrix(m, L, j.vp[l]);
            store_matrix(inv, L, j.inv[l]);
            store_matrix(jvp, L, j.jvp[l]);
            store_matrix(jinv, L, j.jinv[l]);
            if (windowed) {
                store_matrix(wvp, L, j.wvp[l]);
                store_matrix(winv, L, j.winv[l]);
            }
        }
        // Keep the bytes exactly as the game stored them for matching (float round trip).
        std::memcpy(j.vp[j.layout], bytes.data() + g.camera_offset, 64);
        if (inv_here && na::camera::IsInversePair(m, na::camera::LoadMatrix(bytes, static_cast<size_t>(io), layout)))
            std::memcpy(j.inv[j.layout], bytes.data() + io, 64);
        // The camera's separate projection (passes that multiply by P themselves): T * P.
        j.has_proj = false;
        const int64_t po = g.camera_proj_delta == INT64_MIN ? -1 : g.camera_offset + g.camera_proj_delta;
        if (po >= 0 && static_cast<uint64_t>(po) + 64 <= n) {
            const auto pm = na::camera::LoadMatrix(bytes, static_cast<size_t>(po), layout);
            if (is_projection_of(pm, m)) {
                remember_projection(pm, bytes.data() + po, layout);
            }
        }
        j.valid = true;
        return true;
    }
    return false;
}

// Projection window: matrices the game DERIVED from its camera (inverse projection,
// clip->UV / pixel transforms of VP or P, the inverse VP times a UV->clip transform, and
// the same of the previous VP) are rebased onto the windowed camera, in any bound block.
// Generic: core/projection_window.hpp derive_rebase recognises only screen-like transforms
// of THIS frame's camera (canonical values, rebuilt to float precision), so another
// camera, a camera pan or arbitrary data is never touched.
// Blocks already scanned for a separate projection without finding one: content hash per block.
std::map<std::pair<uint64_t, uint64_t>, uint64_t> g_p_scan_negative;
struct DerivedBase { na::camera::Matrix b{}, binv{}, bnew{}, bnewinv{}; const char* name = ""; };
struct DerivedBases { uint64_t frame = UINT64_MAX; uint32_t phase = 0; bool projection = false; int count = 0; DerivedBase base[3]; } g_derived;
void build_derived_bases()
{
    auto& d = g_derived;
    if (d.frame == g.frame && d.phase == g.jit.phase && d.projection == g.jit.has_proj) return;
    d.frame = g.frame;
    d.phase = g.jit.phase;
    d.projection = g.jit.has_proj;
    d.count = 0;
    const auto L = g.jit.layout ? na::camera::Layout::ColumnMajor : na::camera::Layout::RowMajor;
    auto add = [&](const uint8_t* orig, const uint8_t* rep, const char* name) {
        const std::vector<uint8_t> ob(orig, orig + 64), rb(rep, rep + 64);
        DerivedBase& x = d.base[d.count];
        x.b = na::camera::LoadMatrix(ob, 0, L);
        x.bnew = na::camera::LoadMatrix(rb, 0, L);
        x.name = name;
        if (na::camera::Invert(x.b, x.binv) && na::camera::Invert(x.bnew, x.bnewinv)) ++d.count;
    };
    add(g.jit.vp[g.jit.layout], g.jit.wvp[g.jit.layout], "VP");
    if (g.jit.has_proj) add(g.jit.proj[g.jit.layout], g.jit.wproj[g.jit.layout], "P");
    if (g.jit.has_prev) add(g.jit.prev, g.jit.wprev, "prevVP");
}
template <class JB>
void window_derived(const JB& k, std::vector<uint8_t>& view, std::vector<bool>& touched)
{
    build_derived_bases();
    const auto& d = g_derived;
    if (!d.count) return;
    const neural_alias::DerivedSpace space{double(g.win.w), double(g.win.h)};
    const size_t n = view.size();
    for (size_t o = 0; o + 64 <= n; o += 16) {
        if (touched[o / 16]) continue;
        // Cheap reject: a camera-derived matrix is never mostly zero or non-finite.
        const float* f = reinterpret_cast<const float*>(view.data() + o);
        int nz = 0;
        bool finite = true;
        for (int i = 0; i < 16; ++i) { if (!std::isfinite(f[i])) { finite = false; break; } if (f[i] != 0) ++nz; }
        if (!finite || nz < 4) continue;
        for (int l = 0; l < 2; ++l) {
            const auto L = l ? na::camera::Layout::ColumnMajor : na::camera::Layout::RowMajor;
            const auto m = na::camera::LoadMatrix(view, o, L);
            int form = 0, bi = 0;
            std::array<double, 16> out{};
            for (; bi < d.count && !form; ++bi) {
                const DerivedBase& x = d.base[bi];
                form = neural_alias::derive_rebase(m, x.b, x.binv, x.bnew, x.bnewinv, space, out);
            }
            if (!form) continue;
            --bi;
            uint8_t rep[64];
            na::camera::Matrix om{};
            for (int i = 0; i < 16; ++i) om[i] = out[i];
            store_matrix(om, L, rep);
            if (std::memcmp(rep, view.data() + o, 64) &&
                g.jitter_writes.Apply(k.res, k.off + o, view.data() + o, rep, [&] {
                    return na::observe::WriteBuffer(k.res, k.off + o, rep, 64) == 64;
                })) {
                ++g.stats.jitter_writes;
                if (form == 1) ++g.stats.window_derived_forward; else ++g.stats.window_derived_inverse;
                // First kinds seen, for the log: e.g. "inv(P)+272c".
                char tag[40];
                std::snprintf(tag, sizeof tag, "%s(%s)+%u%s ", form == 1 ? "scr" : "inv", d.base[bi].name,
                              static_cast<unsigned>(o), l ? "c" : "r");
                auto& note = g.stats.window_derived_note;
                const size_t used = std::strlen(note), add = std::strlen(tag);
                if (!std::strstr(note, tag) && used + add < sizeof note) std::memcpy(note + used, tag, add + 1);
            }
            touched[o / 16] = true;
            break;
        }
    }
}

// Projection window: a block holding ANOTHER copy of the main camera (same camera within
// same_camera's bounds, e.g. last frame's copy still bound, or the camera updated between passes
// while panning) gets the same jitter and window at its learned offsets, with its inverse and
// projection. Left full size, its draws land unpacked (Cort's 8a0ce83 run: zoom pops).
bool window_extra_camera(uint64_t res, uint64_t off, std::vector<uint8_t>& view)
{
    const size_t n = view.size();
    const int64_t vo = g.camera_offset;
    if (vo < 0 || static_cast<uint64_t>(vo) + 64 > n || !g.jit.ew || !g.jit.eh || !g.win.w || !g.win.h) return false;
    const auto L = g.jit.layout ? na::camera::Layout::ColumnMajor : na::camera::Layout::RowMajor;
    const auto m = na::camera::LoadMatrix(view, static_cast<size_t>(vo), L);
    na::camera::Candidate c{};
    if (!na::camera::ViewProjection(m, c) || !same_camera(m)) return false;
    const double sx = double(g.jit.ew) / g.win.w, sy = double(g.jit.eh) / g.win.h;
    const neural_alias::JitterSample js{g.jit.px[0], g.jit.px[1]};
    auto jittered = [&](const na::camera::Matrix& src) {
        std::array<double, 16> a{};
        for (int i = 0; i < 16; ++i) a[i] = src[i];
        const auto j = neural_alias::jitter_projection(a, js, g.jit.ew, g.jit.eh);
        na::camera::Matrix jm{};
        for (int i = 0; i < 16; ++i) jm[i] = j[i];
        return jm;
    };
    auto windowed = [&](const na::camera::Matrix& src) { return window_of(jittered(src), sx, sy); };
    if (g_window_extras_frame != g.frame) { g_window_extras.clear(); g_window_extras_frame = g.frame; }
    auto remember = [&](const na::camera::Matrix& w, const na::camera::Matrix& j, bool is_vp) {
        if (g_window_extras.size() >= 32) return;
        WindowExtra x{};
        store_matrix(w, L, x.written);
        store_matrix(j, L, x.jittered);
        x.vp = is_vp;
        g_window_extras.push_back(x);
    };
    auto put = [&](int64_t o, const na::camera::Matrix& mat) {
        uint8_t rep[64];
        store_matrix(mat, L, rep);
        if (g.jitter_writes.Apply(res, off + static_cast<uint64_t>(o), view.data() + o, rep, [&] {
                return na::observe::WriteBuffer(res, off + static_cast<uint64_t>(o), rep, 64) == 64;
            })) ++g.stats.jitter_writes;
    };
    const auto wm = windowed(m);
    na::camera::Matrix wminv{};
    if (!na::camera::Invert(wm, wminv)) return false;
    const auto jm = jittered(m);
    na::camera::Matrix jminv{};
    if (!na::camera::Invert(jm, jminv)) return false;
    put(vo, wm);
    remember(wm, jm, true);
    const int64_t io = vo + g.camera_inv_delta;
    if (io >= 0 && static_cast<uint64_t>(io) + 64 <= n &&
        na::camera::IsInversePair(m, na::camera::LoadMatrix(view, static_cast<size_t>(io), L))) { put(io, wminv); remember(wminv, jminv, false); }
    const int64_t po = g.camera_proj_delta == INT64_MIN ? -1 : vo + g.camera_proj_delta;
    if (po >= 0 && static_cast<uint64_t>(po) + 64 <= n) {
        const auto pm = na::camera::LoadMatrix(view, static_cast<size_t>(po), L);
        if (is_projection_of(pm, m)) put(po, windowed(pm));
    }
    ++g.stats.window_extra_cameras;
    if (g_camera_resources.size() < 64) { g_camera_resources.insert(res); publish_camera_resources_locked(); }
    return true;
}

// Replaces this frame's VP / inverse in the bound constant buffers of this batch.
// The frame's sample starts at the first block holding a VP + inverse at the
// learned offset (blocks of the learned camera slot are tried first); then every
// exact copy of that VP / inverse in ANY bound block of the batch is replaced,
// because an earlier pass (a depth pre-pass, often with its own root signature)
// must see the same jittered camera as the scene, or depth tests disagree.
void jitter_batch_impl(ID3D12CommandList* const* lists, UINT count);
// CPU cost of J1/window camera work on the game's submit thread (Cort: latency must stay low).
void jitter_batch(ID3D12CommandList* const* lists, UINT count)
{
    LARGE_INTEGER a, b, f;
    QueryPerformanceCounter(&a);
    jitter_batch_impl(lists, count);
    QueryPerformanceCounter(&b);
    QueryPerformanceFrequency(&f);
    const uint64_t us = static_cast<uint64_t>((b.QuadPart - a.QuadPart) * 1000000 / f.QuadPart);
    g.stats.jitter_cpu_us += us;
    if (us > g.stats.jitter_cpu_max_us) g.stats.jitter_cpu_max_us = us;
}
void jitter_batch_impl(ID3D12CommandList* const* lists, UINT count)
{
    // Only while DLAA is producing frames: the previous game frame submitted one
    // (a batch without the scene must not count as "not running").
    const bool running = g.submitted_frame && g.frame == g.submitted_frame + 1;
    // Inline NGX parameters are fixed at recording. An earlier submitted camera
    // batch may start the sample; recording then freezes that choice. Never start
    // a new sample at submission after NGX already recorded zero jitter.
    // An engaged projection window draws every frame into the corner: it keeps jittering
    // (and windowing) through a frame without DLAA rather than show a full-size frame
    // on the windowed recordings' clipped targets.
    if (vpj_mode() || !g.jitter_on || g.jitter_unseen || (g.inline_mode && g.inline_sample_locked && !g.jit.valid) ||
        g.camera_param < 0 || g.camera_offset < 0 || (!running && !window_now()) || !g.w ||
        (g_render_small.load(std::memory_order_relaxed) && g_small_stretch_streak.load(std::memory_order_relaxed) >= 3)) return;
    struct JB { uint64_t res, off; uint32_t size; bool slot, known; };
    // Every list of the batch is listed (cf65722: listing stopped at 256 blocks, so under
    // zooming later lists, camera copies included, were never seen: 31035 capped batches).
    // Blocks bound at the camera's own slot are all taken (bounded); the rest, which only
    // matter for exact copies in other root signatures, within a budget, resources that
    // carried the camera before first.
    std::map<std::pair<uint64_t, uint64_t>, JB> blocks;
    // fab5928 dropped 1500 blocks a frame to a budget and still missed 15 % of frames while
    // zooming. Read bounded observed ranges; one negative offset cannot exclude a component.
    constexpr size_t kSlotBlocks = 1024, kOtherBlocks = 4096;
    size_t slot_blocks = 0, other_blocks = 0, dropped = 0;
    for (UINT i = 0; i < count; ++i) {
        if (!lists[i]) continue;
        for (const auto& b : na::observe::ListRootConstantBuffers(lists[i])) {
            const bool slot = b.param == static_cast<uint32_t>(g.camera_param) && b.table == g.camera_table;
            const bool known = g_camera_resources.count(b.resource) != 0;
            const uint32_t sz = b.table && b.size ? b.size : kBlockBytes;
            const uint32_t bounded_size=(std::min)(sz,kMaxBlockRead);
            auto it = blocks.find({b.resource, b.offset});
            if (it != blocks.end()) {
                // One upload address can be bound through several CBV ranges.
                // A short first view must not hide a later camera/inverse.
                it->second.size=(std::max)(it->second.size,bounded_size);
                if (slot && !it->second.slot) { it->second.slot = true; ++slot_blocks; --other_blocks; }
                continue;
            }
            if (slot ? slot_blocks >= kSlotBlocks : other_blocks >= kOtherBlocks) { ++dropped; continue; }
            JB& k = blocks[{b.resource, b.offset}];
            k.res = b.resource;
            k.off = b.offset;
            k.size = bounded_size;
            k.slot = slot;
            k.known = known;
            if (slot) ++slot_blocks; else ++other_blocks;
        }
    }
    std::vector<const JB*> order;
    for (const auto& kv : blocks) order.push_back(&kv.second);
    std::stable_sort(order.begin(), order.end(), [](const JB* a, const JB* b) {
        return (a->slot ? 2 : a->known ? 1 : 0) > (b->slot ? 2 : b->known ? 1 : 0); });
    std::vector<uint8_t> bytes(kMaxBlockRead);
    // A windowed sample never carries over a Present: the next frame's camera starts its own.
    if (g.jit.valid && g.jit.windowed && g.jit.frame != g.frame && !g.inline_mode) { g.jit.valid = false; ++g.stats.window_stale_samples; }
    if (dropped) ++g.stats.jitter_blocks_capped;
    g.stats.jitter_blocks_dropped += dropped;
    g.stats.jitter_blocks_scanned += order.size();
    g.stats.jitter_slot_blocks += slot_blocks;
    // Read each bounded range once. A single-offset probe cannot exclude camera
    // copies elsewhere, nor standalone P/inverse/previous matrices. The cheap
    // byte comparisons below cover the whole range without a general solver.
    for (const JB* k : order) {
        const uint32_t n = na::observe::ReadBuffer(k->res, k->off, bytes.data(), k->size);
        if (n < 64) continue;
        std::vector<uint8_t> view(bytes.begin(), bytes.begin() + n);
        g.stats.jitter_rebased += g.jitter_writes.Normalize(k->res, k->off, view.data(), view.size());
        const auto holds = [&](const uint8_t (&m)[2][64]) {
            for (size_t o = 0; o + 64 <= n; o += 16)
                if (!std::memcmp(view.data() + o, m[0], 64) || !std::memcmp(view.data() + o, m[1], 64)) return true;
            return false;
        };
        if (g.jit.valid && !g.jit.has_proj) {
            // A depth/VP-only submission can start the sample before a separately
            // uploaded P is consumed. Reuse the existing projection/rigid-view
            // predicates to establish P for this exact camera, not a guessed offset.
            // Cost (Astra 20cddb8 point 4): only 64-byte windows with the shape of a perspective
            // matrix (w row/column = 0,0,+-1,0 in either layout) reach the solver, and a block
            // whose exact bytes held no such window is not scanned again (any new upload changes
            // the bytes, so it is always scanned; a block holding a perspective-shaped window
            // is never cached, because a different camera sample could match it).
            uint64_t content = 1469598103934665603ull;
            for (uint32_t i = 0; i < n; ++i) { content ^= view[i]; content *= 1099511628211ull; }
            auto& seen = g_p_scan_negative[{k->res, k->off}];
            const bool skip = seen == content;
            if (skip) ++g.stats.jitter_p_scan_cached;
            const auto L = g.jit.layout ? na::camera::Layout::ColumnMajor : na::camera::Layout::RowMajor;
            const std::vector<uint8_t> vb(g.jit.vp[g.jit.layout], g.jit.vp[g.jit.layout] + 64);
            const auto vp = na::camera::LoadMatrix(vb, 0, L);
            const auto perspective_shape = [&](size_t o) {
                float f[16];
                std::memcpy(f, view.data() + o, 64);
                const auto one = [](float x) { return x == 1.0f || x == -1.0f; };
                return f[15] == 0.0f && ((one(f[11]) && f[3] == 0.0f && f[7] == 0.0f) || (one(f[14]) && f[12] == 0.0f && f[13] == 0.0f));
            };
            bool shaped = false;
            for (size_t o = 0; !skip && o + 64 <= n && !g.jit.has_proj; o += 16) {
                if (!perspective_shape(o)) continue;
                shaped = true;
                ++g.stats.jitter_p_scan_solved;
                for (auto PLayout : {na::camera::Layout::RowMajor, na::camera::Layout::ColumnMajor}) {
                    const auto p = na::camera::LoadMatrix(view, o, PLayout);
                    na::camera::Candidate candidate{};
                    if (na::camera::Projection(p, candidate) && is_projection_of(p, vp)) {
                        remember_projection(p, view.data() + o, PLayout);
                        break;
                    }
                }
            }
            if (!skip && !shaped) seen = content;
            if (g_p_scan_negative.size() > 16384) g_p_scan_negative.clear();
        }
        if (!g.jit.valid || !holds(g.jit.vp)) {
            // Projection window: EVERY copy of the main camera must be windowed, or the draws using it
            // land full size (a frame-old copy, or a camera updated between passes while panning).
            if (g.jit.valid && g.jit.windowed && window_extra_camera(k->res, k->off, view)) continue;
            const auto holds_one=[&](const uint8_t* m) {
                for(size_t o=0;o+64<=n;o+=16) if(!std::memcmp(view.data()+o,m,64)) return true;
                return false;
            };
            // Camera components can be uploaded separately. Finding the exact
            // known component is sufficient to patch it; requiring a VP beside
            // it silently skipped projection-only/inverse-only consumers.
            const bool component=g.jit.valid && (holds(g.jit.inv) ||
                (g.jit.has_proj && holds(g.jit.proj)) ||
                (g.jit.windowed && g.jit.has_prev && holds_one(g.jit.prev)));
            if (g.jit.valid && !component) { ++g.stats.other_camera_blocks; ++g.stats.jitter_blocks_probed_out; continue; }
            if (!g.jit.valid && !begin_jitter(k->res, k->off, view, k->slot)) continue;
        }
        if (k->slot) { // adopt the game's exact inverse bytes (they may differ from a computed inverse)
            const int64_t vo = g.camera_offset, io = vo + g.camera_inv_delta;
            if (io >= 0 && static_cast<uint64_t>(io) + 64 <= n && static_cast<uint64_t>(vo) + 64 <= n &&
                !std::memcmp(view.data() + vo, g.jit.vp[g.jit.layout], 64) && std::memcmp(view.data() + io, g.jit.inv[g.jit.layout], 64)) {
                const auto L = g.jit.layout ? na::camera::Layout::ColumnMajor : na::camera::Layout::RowMajor;
                if (na::camera::IsInversePair(na::camera::LoadMatrix(view, static_cast<size_t>(vo), L), na::camera::LoadMatrix(view, static_cast<size_t>(io), L)))
                    std::memcpy(g.jit.inv[g.jit.layout], view.data() + io, 64);
            }
            // ...and the camera's projection, if the frame started in a block without it.
            const int64_t po = g.camera_proj_delta == INT64_MIN ? -1 : vo + g.camera_proj_delta;
            if (!g.jit.has_proj && po >= 0 && static_cast<uint64_t>(po) + 64 <= n && static_cast<uint64_t>(vo) + 64 <= n &&
                !std::memcmp(view.data() + vo, g.jit.vp[g.jit.layout], 64)) {
                const auto L = g.jit.layout ? na::camera::Layout::ColumnMajor : na::camera::Layout::RowMajor;
                const auto vm = na::camera::LoadMatrix(view, static_cast<size_t>(vo), L);
                const auto pm = na::camera::LoadMatrix(view, static_cast<size_t>(po), L);
                if (is_projection_of(pm, vm)) {
                    remember_projection(pm, view.data() + po, L);
                }
            }
        }
        const bool w = g.jit.windowed;
        bool matched = false;
        std::vector<bool> touched(n / 16 + 1, false);
        for (size_t o = 0; o + 64 <= n; o += 16)
            for (int l = 0; l < 2; ++l) {
                const uint8_t* rep = nullptr;
                int kind = 0; // 1 vp, 2 inverse, 3 projection, 4 previous vp
                if (!std::memcmp(view.data() + o, g.jit.vp[l], 64)) { rep = w ? g.jit.wvp[l] : g.jit.jvp[l]; kind = 1; }
                else if (!std::memcmp(view.data() + o, g.jit.inv[l], 64)) { rep = w ? g.jit.winv[l] : g.jit.jinv[l]; kind = 2; }
                else if (g.jit.has_proj && !std::memcmp(view.data() + o, g.jit.proj[l], 64)) { rep = w ? g.jit.wproj[l] : g.jit.jproj[l]; kind = 3; ++g.stats.jitter_proj_writes; }
                else if (l == 0 && w && g.jit.has_prev && !std::memcmp(view.data() + o, g.jit.prev, 64)) { rep = g.jit.wprev; kind = 4; }
                if (rep && g.jitter_writes.Apply(k->res, k->off + o, view.data() + o, rep, [&] {
                    return na::observe::WriteBuffer(k->res, k->off + o, rep, 64) == 64;
                })) {
                    ++g.stats.jitter_writes;
                    if (kind == 1) g.jit.applied = true;
                    if (w) {
                        if (kind == 1) { ++g.stats.window_vp_writes; g.win.last_frame = g.frame; }
                        else if (kind == 2) ++g.stats.window_inv_writes;
                        else if (kind == 3) ++g.stats.window_proj_writes;
                        else ++g.stats.window_prev_writes;
                    }
                }
                if (rep) { touched[o / 16] = true; matched = true; break; }
            }
        if (matched && g_camera_resources.size() < 64 && !g_camera_resources.count(k->res)) { g_camera_resources.insert(k->res); publish_camera_resources_locked(); }
        // Derived matrices live beside the camera (the blocks that hold it); scanning every
        // other block for them is what cost 1.2 ms a frame.
        if (w && (k->slot || matched)) window_derived(*k, view, touched);
        else if (w) ++g.stats.window_derived_skipped;
    }
    g.stats.jitter_patches = g.jitter_writes.Size();
    g.stats.jitter_patches_refused = g.jitter_writes.Refused();
}

// J1 coverage diagnostics: work drawn AFTER this frame's DLAA that still uses the
// jittered camera or the (jittered) scene depth gets the shake without DLAA
// resolving it (shimmer). Counted per list.
void count_post_dlaa(ID3D12CommandList* const* lists, UINT first, UINT count, na_object_id depth_id)
{
    const size_t lo = g.camera_offset >= 0 ? static_cast<size_t>(g.camera_offset) : 0;
    std::vector<uint8_t> bytes(64);
    for (UINT i = first; i < count; ++i) {
        if (!lists[i]) continue;
        if (depth_id && na::observe::ListReferences(lists[i], depth_id)) ++g.stats.post_depth_lists;
        if (g.camera_offset < 0) continue;
        for (const auto& b : na::observe::ListRootConstantBuffers(lists[i])) {
            if (na::observe::ReadBuffer(b.resource, b.offset + lo, bytes.data(), 64) != 64) continue;
            if (!std::memcmp(bytes.data(), g.jit.jvp[0], 64) || !std::memcmp(bytes.data(), g.jit.jvp[1], 64)) { ++g.stats.post_camera_lists; break; }
            if (g.jit.windowed && (!std::memcmp(bytes.data(), g.jit.wvp[0], 64) || !std::memcmp(bytes.data(), g.jit.wvp[1], 64))) {
                ++g.stats.post_camera_lists; ++g.stats.window_post_lists; break; // drawn on the full-size output with the corner camera
            }
        }
    }
}

// This frame's camera: a constant buffer holding a perspective ViewProjection
// together with its inverse, bound in this batch up to the scene list. Root CBVs
// and CBVs reached through descriptor tables both count (Anno binds its camera
// through a table). Lists that use the scene depth are searched first, then the
// rest; within that, the binding slot that held the camera last time leads.
struct CamDiag {
    size_t lists=0,binds=0,blocks=0,read=0,vp=0,cands=0;
    size_t learned_seen=0,learned_bounds=0,learned_shape=0,learned_inverse=0;
} g_cd;

struct Block {
    uint64_t resource = 0, offset = 0;
    uint32_t size = 0, param = 0, uses = 0, list_rank = 0;
    bool table = false;
};

bool check_learned(const std::vector<uint8_t>& bytes, na::camera::Matrix* vp, bool* reversed)
{
    const size_t n = bytes.size();
    ++g_cd.learned_seen;
    if (g.camera_offset < 0 || static_cast<uint64_t>(g.camera_offset) + 64 > n) {++g_cd.learned_bounds;return false;}
    bool have_vp=false,have_inverse_bytes=false;
    for (auto layout : {na::camera::Layout::RowMajor, na::camera::Layout::ColumnMajor}) {
        const auto m = na::camera::LoadMatrix(bytes, static_cast<size_t>(g.camera_offset), layout);
        na::camera::Candidate c{};
        if (!na::camera::ViewProjection(m, c)) continue;
        have_vp=true;
        for (int64_t d : {g.camera_inv_delta}) { // the learned inverse position (Anno: not adjacent)
            const int64_t off = g.camera_offset + d;
            if (off < 0 || static_cast<uint64_t>(off) + 64 > n) continue;
            have_inverse_bytes=true;
            if (na::camera::IsInversePair(m, na::camera::LoadMatrix(bytes, static_cast<size_t>(off), layout))) {
                *vp = m;
                *reversed = c.reversed;
                return true;
            }
        }
    }
    if(!have_vp)++g_cd.learned_shape;
    else if(!have_inverse_bytes)++g_cd.learned_bounds;
    else ++g_cd.learned_inverse;
    return false;
}

bool g_injected = false; // the camera found this frame carries J1's jitter (g.jit.px)

// Several blocks can hold a valid ViewProjection + inverse in the same frame: the main camera,
// a water reflection's mirrored camera, a sky camera, a light's. Ranking blocks by how often they
// are bound picks whichever dominates the frame's draws, which changes with where the player
// looks (Sniper: looking down at water or up at the sky alternated cameras: no camera, false cuts,
// unjittered frames, flicker; Cort 2026-11-01). Among the candidates the one most continuous with the
// last main camera wins (holding the matrix J1 jittered is only a tie-break).
struct CamPick {
    na::camera::Matrix vp{};
    bool reversed = false, injected = false;
    double score = 0;
    const Block* block = nullptr;
    int64_t offset = 0, inv_delta = 0, proj_delta = INT64_MIN;
};
bool holds_jittered(const std::vector<uint8_t>& bytes, size_t off)
{
    if (!g.jit.valid || off + 64 > bytes.size()) return false;
    if (!std::memcmp(bytes.data() + off, g.jit.jvp[0], 64) || !std::memcmp(bytes.data() + off, g.jit.jvp[1], 64)) return true;
    if (g_window_extras_frame == g.frame) // an extra windowed copy of the camera (same sample), already unwindowed
        for (const auto& x : g_window_extras)
            if (x.vp && !std::memcmp(bytes.data() + off, x.jittered, 64)) return true;
    return false;
}
double continuity(const na::camera::Matrix& m, bool injected)
{
    // Holding J1's jittered matrix is only a tie-break: J1 could have started on the wrong camera.
    const double bonus = injected ? -0.05 : 0.0;
    na::camera::VpGeometry v{};
    if (!g_jit_cam.valid) return bonus;
    if (!na::camera::DecomposeViewProjection(m, v)) return 1e9;
    const double mirrored = g_jit_cam.hand && handedness(m) == -g_jit_cam.hand ? 10.0 : 0.0; // a reflection's camera
    const auto& p = g_jit_cam.geo;
    double dot = 0, d2 = 0;
    for (int j = 0; j < 3; ++j) {
        dot += v.forward[j] * p.forward[j];
        d2 += (v.position[j] - p.position[j]) * (v.position[j] - p.position[j]);
    }
    return bonus + mirrored + std::acos(std::clamp(dot, -1.0, 1.0)) + std::sqrt(d2) / 25.0 + std::abs(v.sy - p.sy);
}
const CamPick* best_pick(const std::vector<CamPick>& v)
{
    const CamPick* best = nullptr;
    for (const auto& c : v) if (!best || c.score < best->score) best = &c;
    if (best && best != &v.front()) ++g.stats.camera_alternatives;
    if (v.size() > 1) ++g.stats.camera_contested;
    return best;
}

bool pose_of(const na::camera::Matrix& m, na::j2::CameraPose* p)
{
    na::camera::VpGeometry geo{};
    if (!na::camera::DecomposeViewProjection(m, geo) || !(geo.sy > 0)) return false;
    p->position = geo.position; p->forward = geo.forward; p->fov = 2.0 * std::atan(1.0 / geo.sy);
    return true;
}
bool is_interloper(const na::camera::Matrix& m)
{
    na::j2::CameraPose p;
    return pose_of(m, &p) && g.interlopers.IsInterloper(p);
}
bool find_camera_scan(ID3D12CommandList* const* lists, UINT scene, na_object_id depth_id, na::camera::Matrix* vp, bool* reversed);

// ---- Smear test (Cort 2026-11-06: "establish the ability to gather all the data you need") ----
// One button press runs a fixed sequence of DLAA variants on whatever is on screen, each for
// ~2.5 s, and captures 8 consecutive frames at the end of each into its own folder:
//   0 normal, 1 no motion, 2 current frame only (DLSS told to trust this frame everywhere),
//   3 no sharpening, 4 DLSS preset L (a fresh feature; the run's preset otherwise), 5 the game's
//   motion texture forced (best encoding so far, even before it is proven), 6 normal again.
// Comparing a moving surface across them separates history blending (2 sharp, others not),
// wrong motion (1 vs 0 vs 5), the preset (4) and sharpening (3) from the input itself
// (each capture's colour plane = the game's own frame). Only the DLAA (native) path applies
// the variants; the picture visibly changes while it runs.
struct SmearTest {
    std::atomic<bool> requested{false};
    bool running = false;
    int mode = 0;
    uint64_t evals = 0;
    bool capturing = false;
    wchar_t base[MAX_PATH]{};
    char status[200]{};
    ComPtr<ID3D12Resource> ones; // R16_FLOAT rw x rh, all 1 (NPSR): bias-current-colour everywhere
    ComPtr<ID3D12DescriptorHeap> rtv;
    uint32_t ones_w = 0, ones_h = 0;
    bool ones_ready = false;
};
SmearTest g_smear;
constexpr const char* kSmearModes[] = {"0-normal", "1-no-motion", "2-current-frame-only", "3-no-sharpening",
                                       "4-preset-L", "5-game-motion-forced", "6-normal-again"};
constexpr int kSmearModeCount = 7;
constexpr uint64_t kSmearSettle = 150, kSmearBurst = 8, kSmearTimeout = 600;
int smear_mode() { return g_smear.running ? g_smear.mode : -1; }
void smear_status(const char* what)
{
    std::snprintf(g_smear.status, sizeof g_smear.status, "%s", what);
}
// Once per native DLAA evaluation, before it is recorded (g.m held).
void smear_step()
{
    if (g_smear.requested.exchange(false) && !g_smear.running) {
        if (na::capture::MakeDir(L"smear", g_smear.base, MAX_PATH)) {
            na::capture::TagDir(g_smear.base, "smear");
            g_smear.running = true; g_smear.mode = 0; g_smear.evals = 0; g_smear.capturing = false;
            na::eventlog::Emit("smear_test", "start dir=%ls", g_smear.base);
        } else smear_status("Smear test: the captures folder could not be created.");
    }
    if (!g_smear.running) return;
    ++g_smear.evals;
    char st[200];
    const auto prev = na::capture::j2::GetStats();
    const bool writer_idle = prev.written + prev.discarded + prev.errors >= prev.recorded; // the last burst is on disk: 8 free slots
    if (!g_smear.capturing && g_smear.evals >= kSmearSettle && !na::capture::j2::Armed() && (writer_idle || g_smear.evals >= kSmearSettle + kSmearTimeout)) {
        wchar_t dir[MAX_PATH];
        swprintf_s(dir, L"%s\\%hs", g_smear.base, kSmearModes[g_smear.mode]);
        CreateDirectoryW(dir, nullptr);
        na::capture::j2::Request(static_cast<uint32_t>(kSmearBurst), dir);
        g_smear.capturing = true;
    } else if (g_smear.capturing && (!na::capture::j2::Armed() || g_smear.evals >= kSmearSettle + kSmearTimeout)) {
        const auto cs = na::capture::j2::GetStats();
        na::eventlog::Emit("smear_test", "mode=%s recorded=%u written=%u dropped_busy=%u evals=%llu", kSmearModes[g_smear.mode],
                           cs.recorded, cs.written, cs.dropped_busy, static_cast<unsigned long long>(g_smear.evals));
        ++g_smear.mode; g_smear.evals = 0; g_smear.capturing = false;
        if (g_smear.mode >= kSmearModeCount) {
            g_smear.running = false;
            na::eventlog::Emit("smear_test", "done dir=%ls", g_smear.base);
            smear_status("Smear test finished. Exit the game and run Results.");
            return;
        }
    }
    std::snprintf(st, sizeof st, "Smear test running: step %d of %d (%s). Keep the view still.", g_smear.mode + 1, kSmearModeCount,
                  kSmearModes[g_smear.mode] + 2);
    smear_status(st);
}
// The all-ones bias mask, created and cleared once per size in `l`; NPSR when it returns true.
bool smear_ones(ID3D12GraphicsCommandList* l, uint32_t rw, uint32_t rh)
{
    if (g_smear.ones_ready && g_smear.ones_w == rw && g_smear.ones_h == rh) return true;
    g_smear.ones.Reset(); g_smear.ones_ready = false;
    if (!g_smear.rtv) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
        if (FAILED(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_smear.rtv)))) return false;
    }
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = rw; rd.Height = rh; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R16_FLOAT; rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (FAILED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                 IID_PPV_ARGS(&g_smear.ones))))
        return false;
    const auto h = g_smear.rtv->GetCPUDescriptorHandleForHeapStart();
    g.device->CreateRenderTargetView(g_smear.ones.Get(), nullptr, h);
    const float one[4] = {1.f, 1.f, 1.f, 1.f};
    l->ClearRenderTargetView(h, one, 0, nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {g_smear.ones.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    l->ResourceBarrier(1, &b);
    g_smear.ones_w = rw; g_smear.ones_h = rh; g_smear.ones_ready = true;
    return true;
}
// The scan, then the interloper rule: a known second camera is never the main camera (the frame
// then keeps the last main camera, no history reset); every chosen camera updates the rule.
// `judged` receives the committed rule updated by this camera; the caller commits it only when
// the frame enters history (with the tracker), so a failed attempt teaches it nothing.
bool find_camera(ID3D12CommandList* const* lists, UINT scene, na_object_id depth_id, na::camera::Matrix* vp, bool* reversed,
                 na::j2::InterloperFilter* judged)
{
    *judged = g.interlopers;
    if (!find_camera_scan(lists, scene, depth_id, vp, reversed)) return false;
    na::j2::CameraPose p;
    if (!pose_of(*vp, &p)) return true;
    const uint64_t learned_before = judged->Learned();
    const auto verdict = judged->Judge(p, g.frame);
    if (judged->Learned() != learned_before)
        na::eventlog::Emit("camera_interloper", "learned known=%u pos=%.4g,%.4g,%.4g fov=%.4g", judged->KnownCount(),
                           p.position[0], p.position[1], p.position[2], p.fov);
    if (verdict != na::j2::InterloperFilter::Verdict::Interloper) return true;
    ++g.stats.camera_interlopers;
    const uint64_t r = judged->Rejected();
    if (r == 1 || r % 256 == 0) // one line per 256: the menu flips several times a second
        na::eventlog::Emit("camera_interloper", "rejected n=%llu pos=%.4g,%.4g,%.4g fov=%.4g", static_cast<unsigned long long>(r),
                           p.position[0], p.position[1], p.position[2], p.fov);
    return false;
}
bool find_camera_scan(ID3D12CommandList* const* lists, UINT scene, na_object_id depth_id, na::camera::Matrix* vp, bool* reversed)
{
    g_cd = {};
    g_injected = false;
    // Lists to search: the scene list, then earlier lists using the depth, then the others.
    std::vector<std::pair<uint32_t, UINT>> order_lists;
    for (int i = static_cast<int>(scene); i >= 0; --i) {
        if (!lists[i]) continue;
        const uint32_t rank = static_cast<UINT>(i) == scene ? 0u : (na::observe::ListReferences(lists[i], depth_id) ? 1u : 2u);
        order_lists.push_back({rank, static_cast<UINT>(i)});
    }
    std::stable_sort(order_lists.begin(), order_lists.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::map<std::pair<uint64_t, uint64_t>, Block> blocks;
    for (const auto& ol : order_lists) {
        const auto binds = na::observe::ListRootConstantBuffers(lists[ol.second]);
        if (binds.empty()) continue;
        ++g_cd.lists;
        g_cd.binds += binds.size();
        for (const auto& b : binds) {
            Block& k = blocks[{b.resource, b.offset}];
            if (!k.uses) {
                k.resource = b.resource;
                k.offset = b.offset;
                k.size = b.table && b.size ? b.size : kBlockBytes;
                k.param = b.param;
                k.table = b.table;
                k.list_rank = ol.first;
            }
            k.size=(std::max)(k.size,b.table && b.size ? b.size : kBlockBytes);
            ++k.uses;
        }
    }
    std::vector<const Block*> order;
    order.reserve(blocks.size());
    for (const auto& kv : blocks) order.push_back(&kv.second);
    const auto learned = [](const Block* b) { return g.camera_param >= 0 && b->param == static_cast<uint32_t>(g.camera_param) && b->table == g.camera_table; };
    std::stable_sort(order.begin(), order.end(), [&](const Block* a, const Block* b) {
        if (learned(a) != learned(b)) return learned(a);
        if (a->list_rank != b->list_rank) return a->list_rank < b->list_rank;
        return a->uses > b->uses;
    });
    if (order.size() > kMaxBlocks) order.resize(kMaxBlocks);
    g_cd.blocks = order.size();
    std::vector<uint8_t> bytes(kMaxBlockRead);
    const auto read_block = [&](const Block* o, na::camera::Snapshot& s) {
        const uint32_t want = o->size < kMaxBlockRead ? o->size : kMaxBlockRead;
        const uint32_t n = na::observe::ReadBuffer(o->resource, o->offset, bytes.data(), want);
        if (n < 128) return false;
        ++g_cd.read;
        // An in-process read at submission time, inside the submitting call:
        // its scope is known (FindMatrices rejects snapshots without one).
        s.session = s.epoch = 1;
        s.submission = s.content_version = g.frame;
        s.resource = o->resource;
        s.base_offset = 0; // offsets are relative to the block
        s.complete = s.lifetime_valid = true;
        s.sync = na::camera::Sync::AppSyncUnknown;
        s.bytes.assign(bytes.begin(), bytes.begin() + n);
        unwindow(s.bytes.data(), s.bytes.size());
        return true;
    };
    // 1. The learned slot: every block bound there this frame is a candidate (bounded).
    std::vector<CamPick> picks;
    for (const Block* o : order) {
        if (!learned(o) || picks.size() >= 8) break;
        na::camera::Snapshot s{};
        if (!read_block(o, s)) continue;
        CamPick c;
        if (!check_learned(s.bytes, &c.vp, &c.reversed)) continue;
        c.injected = holds_jittered(s.bytes, static_cast<size_t>(g.camera_offset));
        c.score = continuity(c.vp, c.injected);
        c.block = o;
        if (is_interloper(c.vp)) { ++g.stats.camera_interloper_picks; continue; } // the main camera may be among the others
        picks.push_back(c);
    }
    if (const CamPick* p = picks.empty() ? nullptr : best_pick(picks)) {
        ++g.stats.camera_learned_hits;
        *vp = p->vp;
        *reversed = p->reversed;
        g_injected = p->injected;
        return true;
    }
    // 2. A full search; a few candidates are compared the same way before the slot is relearned.
    struct BlockNote { uint32_t param, size, uses, read; bool table; uint32_t vp, inv, proj, view; uint64_t resource,offset; };
    std::vector<BlockNote> notes;
    CamPick loose;               // a ViewProjection WITHOUT its inverse that continues the main camera
    bool have_loose = false;     // (accepted for this frame only; the slot is not relearned from it)
    for (const Block* o : order) {
        if (picks.size() >= 4) break;
        na::camera::Snapshot s{};
        const bool read_ok = read_block(o, s);
        const bool noted = notes.size() < 12;
        if (noted) notes.push_back({o->param, o->size, o->uses, read_ok ? static_cast<uint32_t>(s.bytes.size()) : 0u, o->table, 0, 0, 0, 0,o->resource,o->offset});
        if (!read_ok) continue;
        const auto scan = na::camera::FindMatrices(s);
        g_cd.cands += scan.candidates.size();
        for (const auto& c : scan.candidates) {
            if (noted) {
                BlockNote& bn = notes.back();
                if (c.kind == na::camera::Kind::ViewProjection) { ++bn.vp; if (c.has_inverse_pair) ++bn.inv; }
                else if (c.kind == na::camera::Kind::Projection) ++bn.proj;
                else ++bn.view;
            }
            if (c.kind == na::camera::Kind::ViewProjection) ++g_cd.vp;
            if (c.kind == na::camera::Kind::ViewProjection && !c.has_inverse_pair && !have_loose && same_camera(c.matrix) && !is_interloper(c.matrix)) {
                loose.vp = c.matrix;
                loose.reversed = c.reversed;
                loose.injected = holds_jittered(s.bytes, c.byte_offset);
                loose.block = o;
                have_loose = true;
            }
            if (c.kind != na::camera::Kind::ViewProjection || !c.has_inverse_pair) continue;
            if (is_interloper(c.matrix)) { ++g.stats.camera_interloper_picks; continue; } // never relearn the slot from it
            CamPick k;
            k.vp = c.matrix;
            k.reversed = c.reversed;
            k.block = o;
            k.offset = static_cast<int64_t>(c.byte_offset);
            k.inv_delta = static_cast<int64_t>(c.inverse_byte_offset) - static_cast<int64_t>(c.byte_offset);
            for (const auto& pc : scan.candidates)
                if (pc.kind == na::camera::Kind::Projection && pc.layout == c.layout && is_projection_of(pc.matrix, c.matrix)) {
                    k.proj_delta = static_cast<int64_t>(pc.byte_offset) - static_cast<int64_t>(c.byte_offset);
                    break;
                }
            k.injected = holds_jittered(s.bytes, c.byte_offset);
            k.score = continuity(k.vp, k.injected);
            picks.push_back(k);
            break; // one per block
        }
    }
    const CamPick* p = picks.empty() ? nullptr : best_pick(picks);
    if (!p && have_loose) {
        // The main camera's ViewProjection without an inverse beside it (a smaller per-view
        // block): the tracker inverts it itself; the inverse pair was only a validation, and
        // continuity with the last main camera validates it instead. The slot is left as learned.
        ++g.stats.camera_inverse_computed;
        *vp = loose.vp;
        *reversed = loose.reversed;
        g_injected = loose.injected; // VP-only blocks retain exact patched-sample provenance too
        return true;
    }
    if (!p) {
        // Why the camera was lost (diagnostics for frames like Sniper's steep views).
        if (!g_cd.read) ++g.stats.camera_lost_unread;
        else if (!g_cd.vp) ++g.stats.camera_lost_no_vp;
        else ++g.stats.camera_lost_no_inverse;
        if (g_cd.blocks >= kMaxBlocks) ++g.stats.camera_lost_capped;
        // What the frame held (the last loss only; the status log writes it).
        char* t = g.stats.camera_lost_evidence;
        size_t cap = sizeof g.stats.camera_lost_evidence;
        int n = std::snprintf(t, cap, "frame=%llu lists=%zu binds=%zu blocks=%zu read=%zu vp=%zu cands=%zu learned{param=%lld table=%d off=%lld inv=%lld checked=%zu bounds=%zu shape=%zu inverse=%zu} main_cam=%d blocks[param/table size uses read vp/inv/proj/view resource@offset]:",
                              static_cast<unsigned long long>(g.frame), g_cd.lists, g_cd.binds, g_cd.blocks, g_cd.read, g_cd.vp, g_cd.cands,
                              static_cast<long long>(g.camera_param), g.camera_table ? 1 : 0, static_cast<long long>(g.camera_offset),
                              static_cast<long long>(g.camera_inv_delta),g_cd.learned_seen,g_cd.learned_bounds,g_cd.learned_shape,g_cd.learned_inverse, g_jit_cam.valid ? 1 : 0);
        for (const auto& bn : notes) {
            if (n < 0 || static_cast<size_t>(n) >= cap) break;
            const int m = std::snprintf(t + n, cap - static_cast<size_t>(n), " %u/%d %u %u %u %u/%u/%u/%u %llu@%llu", bn.param, bn.table ? 1 : 0, bn.size, bn.uses, bn.read, bn.vp, bn.inv, bn.proj, bn.view,(unsigned long long)bn.resource,(unsigned long long)bn.offset);
            if (m < 0) break;
            n += m;
        }
        return false;
    }
    g.camera_offset = p->offset;
    g.camera_inv_delta = p->inv_delta;
    g.camera_proj_delta = p->proj_delta;
    g.stats.camera_proj_delta = g.camera_proj_delta == INT64_MIN ? 0 : g.camera_proj_delta;
    g.stats.camera_inv_delta = g.camera_inv_delta;
    ++g.stats.camera_scans;
    g.camera_param = static_cast<int64_t>(p->block->param);
    g.camera_table = p->block->table;
    publish_camera_resources_locked();
    g.stats.camera_offset = static_cast<uint64_t>(p->offset);
    *vp = p->vp;
    *reversed = p->reversed;
    g_injected = p->injected;
    return true;
}

na::motion::Mat4 to_mat4(const na::camera::Matrix& m)
{
    na::motion::Mat4 r{};
    for (int i = 0; i < 16; ++i) r.m[i] = static_cast<float>(m[i]);
    return r;
}

// Records the DLAA list for this frame. Returns false (nothing recorded worth
// submitting) when a precondition fails.
// inl: record INTO this game command list (inline insertion, during the game's
// recording) instead of J2's own list; *touched reports whether anything was
// recorded into it (the caller then restores the game's state).
// SE5 late-scene diagnostic (Cort 2026-11-02: flicker only while panning). Records the first
// scene write after this frame's DLAA; note_late_frame logs it with what placed the DLAA.
void note_late_writer(ID3D12CommandList* list, uint64_t color, uint32_t skip_writes, uint32_t where, uint32_t writes)
{
    if (g.late_frame == g.frame) { g.late_writes += writes; return; }
    g.late_frame = g.frame; g.late_where = where; g.late_writes = writes; g.late_pso = 0; g.late_depth = false;
    std::vector<na::j2::ColorEvent> events;
    if (!list || !na::observe::ListColorEvents(list, color, events)) return;
    uint32_t seen = 0;
    for (const auto& e : events) {
        if (!e.write) continue;
        if (seen++ < skip_writes) continue;
        g.late_pso = e.producer; g.late_depth = e.with_depth; return;
    }
}
void note_late_frame()
{
    static const char* const kMode[] = {"none", "class", "pair", "barrier", "boundary", "between"};
    static const char* const kWhere[] = {"?", "same_list", "later_list", "later_batch"};
    char one[200];
    const bool have = g.late_frame == g.frame;
    std::snprintf(one, sizeof one, "%llu:%s%s/op=%u/prod=%llu/ctx=%llu->late{%s pso=%llu depth=%u writes=%u} ",
        static_cast<unsigned long long>(GetTickCount64() / 100 % 100000000), kMode[g.fired_mode < 6 ? g.fired_mode : 0],
        g.fired_barrier_event ? "@barrier" : "", static_cast<unsigned>(g.fired_class.op),
        (unsigned long long)g.fired_producer, (unsigned long long)g.fired_context,
        have ? kWhere[g.late_where < 4 ? g.late_where : 0] : "?", have ? (unsigned long long)g.late_pso : 0ull,
        have && g.late_depth ? 1u : 0u, have ? g.late_writes : 0u);
    char merged[sizeof g.stats.late_detail];
    std::snprintf(merged, sizeof merged, "%s%s", one, g.stats.late_detail); // newest first, keep what fits
    std::memcpy(g.stats.late_detail, merged, sizeof merged);
    ++g.stats.late_frames;
}

bool record(ID3D12CommandList* const* lists, UINT scene, na_object_id depth_id, na_object_id color_id,
            ID3D12GraphicsCommandList** out_list, PendingHistory& pending, ID3D12GraphicsCommandList* inl = nullptr,
            bool* touched = nullptr, const SceneInput* saved = nullptr, UINT prefix = 0,
            na::j2::flow::Ticket* flow_ticket = nullptr, bool flow_pair_ok = false, bool repeat_only = false)
{
    // repeat_only (projection window): this windowed frame could not be evaluated; put the last
    // DLSS result back into the scene colour so the clipped corner is never presented raw.
    // P4-4 Layer2: an optional immutable physical snapshot for these logical ids.
    std::shared_ptr<const PhysicalInput> phys;
    PhysicalReport phys_report;
    {
        PhysicalInputProvider pp;
        { std::lock_guard<std::mutex> lk(g_up_m); pp = g_phys; }
        if (pp.lookup && !(saved && saved->at_present)) {
            const PhysicalLookup pl = pp.lookup(pp.ctx, color_id, depth_id,
                inl ? inl : pp.source_recording ? static_cast<ID3D12GraphicsCommandList*>(lists[scene]) : nullptr, &phys);
            if (pl == PhysicalLookup::Unavailable) {
                phys_report.arm(pp, phys ? phys->generation : 0);
                phys_report.result = PhysicalOutcome::Unavailable;
                std::snprintf(phys_report.why, sizeof phys_report.why, "replacement unavailable");
                reason("Layer2: this recording was redirected but its replacement is unavailable; not evaluated.");
                return false;
            }
            if (pl != PhysicalLookup::Provided) phys.reset();
            else {
                phys_report.arm(pp, phys ? phys->generation : 0);
                if (!phys || !phys->color || !phys->depth || !phys->output || !phys->owner || !phys->render_w || !phys->render_h ||
                    phys->logical_color != color_id || phys->logical_depth != depth_id || phys->output == phys->color) {
                    phys_report.fail("the physical snapshot is incomplete or names other logical ids");
                    reason("Layer2: the physical snapshot is incomplete or names other logical ids.");
                    return false;
                }
            }
        }
    }
    const auto phys_fail = [&](const char* why) {
        if (phys) phys_report.fail(why);
        reason(why);
        return false;
    };
    ComPtr<ID3D12Resource> color, depth;
    if (phys) { color = phys->color; depth = phys->depth; } // logical ids remain the identity
    else {
        color.Attach(na::observe::AcquireResource(color_id));
        depth.Attach(na::observe::AcquireResource(depth_id));
    }
    if (!color || !depth) { reason("Scene resources are not available."); return false; }
    const D3D12_RESOURCE_DESC cd = color->GetDesc(), dd = depth->GetDesc();
    g.stats.color_mips = cd.MipLevels ? cd.MipLevels : 1;
    g.stats.color_slices = cd.DepthOrArraySize;
    g.stats.color_samples = cd.SampleDesc.Count;
    g.stats.depth_samples = dd.SampleDesc.Count;
    g.stats.depth_slices = dd.DepthOrArraySize;
    // AA settings are not an eligibility condition. Each input is converted
    // independently: a game may already have resolved color but retain MSAA depth.
    if (cd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || dd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        reason("The scene colour or depth is not a 2D texture.");
        return false;
    }
    // The subresources (mip + slice) the scene pass rendered into, from its views:
    // arrays and mip chains are fine, J2 works on exactly that one through its own copies.
    uint32_t csub = 0, dsub = 0;
    if (phys) { csub = phys->color_sub; dsub = phys->depth_sub; }
    else if (saved) { csub = saved->color_sub; dsub = saved->depth_sub; }
    if ((!phys && !saved && !na::observe::ListSceneSubresource(lists[scene], color_id, depth_id, &csub, &dsub)) || csub == 0xFFFFFFFFu || dsub == 0xFFFFFFFFu) {
        if (phys) return phys_fail("Layer2: the physical subresources are invalid.");
        reason("The scene is drawn through a view NeuralAlias cannot place (several array slices at once, or an unusual view).");
        return false;
    }
    // States of exactly the two subresources J2 copies from and restores.
    uint32_t sc = 0, sd = 0;
    // Inline: the state recorded so far in this list, else the state all submitted work left.
    const auto state_now = [&](na_object_id id, uint32_t sub, uint32_t* st) {
        const auto evidence = na::observe::ListStateSub(inl, id, sub, st);
        if(evidence==na::observe::StateLookup::Known){if(id==depth_id)pending.depth_source=1;return true;}
        if(evidence==na::observe::StateLookup::Absent && na::observe::KnownSubStateOnQueueNow(g.queue.Get(),id,sub,st)){
            if(id==depth_id)pending.depth_source=2;return true;
        }
        return false;
    };
    const auto entry_state = [&](na_object_id id, uint32_t sub, uint32_t* st) {
        return prefix ? state_after(lists, prefix - 1, id, sub, st)
                      : na::observe::KnownSubStateInCurrentSubmission(id, sub, st);
    };
    const auto state_of = [&](na_object_id id, uint32_t sub, uint32_t* st) {
        if (saved && saved->at_present) { // I1.4: after the whole frame was submitted
            if (!na::observe::KnownSubStateOnQueueNow(g.queue.Get(), id, sub, st)) return false;
            if (id == depth_id) pending.depth_source = 2;
            return true;
        }
        if(inl&&saved&&saved->native_states&&saved->frame==g.frame&&saved->color==color_id&&saved->depth==depth_id){
            const auto current=na::observe::ListStateSub(inl,id,sub,st);
            if(current==na::observe::StateLookup::Known)return true;
            if(current==na::observe::StateLookup::Unavailable)return false;
            // The bridge's actual operation positively reads this producer's
            // exact selected scene into the native target. Import its explicit
            // end-state payload, as for the existing physical-input contract.
            // A contradictory/current unavailable state never gets replaced.
            if(id==color_id&&sub==saved->color_sub){*st=saved->color_state;return true;}
            if(id==depth_id&&sub==saved->depth_sub){*st=saved->depth_state;pending.depth_source=3;return true;}
        }
        return inl ? state_now(id, sub, st) : saved ? entry_state(id, sub, st) : state_after(lists, scene, id, sub, st);
    };
    // Layer2 companions are private textures, not observed: their exact states come with them.
    // I1.4: the Present-time source never touches the scene colour (DLSS reads the presented
    // picture), so its state at Present is not needed (Cort's 9346113 run: every attempt waited on it).
    const bool skip_color = saved && saved->at_present;
    const bool have_sc = phys ? (sc = phys->color_state, true) : skip_color ? true : state_of(color_id, csub, &sc);
    bool have_sd = phys ? (sd = phys->depth_state, true) : state_of(depth_id, dsub, &sd);
    // Never replace missing state provenance with a state learned on old frames.
    // Private completion bookkeeping must preserve the real carried state instead.
    if(have_sd && !pending.depth_source)pending.depth_source=phys?3u:4u;
    if (!have_sc || !have_sd) {
        ++g.stats.no_state;
        if (!have_sc) ++g.stats.no_state_color; else ++g.stats.no_state_depth;
        if (!have_sd) na::observe::DescribeStateEvidence(inl ? inl : saved ? nullptr : lists[scene], depth_id, dsub,
                                                        g.stats.depth_state_evidence, sizeof g.stats.depth_state_evidence);
        reason(!have_sc ? "Waiting: the scene colour's state at the end of the scene pass is not established."
                        : "Waiting: the scene depth's state at the end of the scene pass is not established.");
        return false;
    }
    g.stats.color_sub = csub;
    const uint32_t cmip = csub % g.stats.color_mips;
    const uint32_t dmip = dsub % (dd.MipLevels ? dd.MipLevels : 1u);
    const uint32_t w = (std::max)(1u, static_cast<uint32_t>(cd.Width >> cmip)), h = (std::max)(1u, cd.Height >> cmip);
    if ((std::max)(1u, static_cast<uint32_t>(dd.Width >> dmip)) != w || (std::max)(1u, dd.Height >> dmip) != h ||
        !na::depth::PlaneZeroCopiesToR32F(dd.Format)) {
        reason("Scene depth does not match the colour size or is not 32-bit depth.");
        return false;
    }
    const DXGI_FORMAT cf = typed(cd.Format); // our copies' format
    const bool color_msaa = cd.SampleDesc.Count > 1, depth_msaa = dd.SampleDesc.Count > 1;

    // P4-4: upscaling destination (slider route). Any reason not to use it falls back
    // to DLAA at the scene's size, written back as before.
    UpscaleTarget up;
    { std::lock_guard<std::mutex> lk(g_up_m); up = g_up; }
    ComPtr<ID3D12Resource> dest;
    uint32_t rw = w, rh = h, ow = w, oh = h, dsub_out = 0, sdest = 0;
    const auto refuse_upscale = [&](const char* why) {
        dest.Reset();
        rw = ow = w; rh = oh = h;
        ++g.stats.upscale_refused;
        std::snprintf(g.stats.upscale_note, sizeof g.stats.upscale_note, "%s", why);
    };
    if (phys) { // Layer2: the destination and extent come from the same snapshot
        up.dest = 0;
        up.dest_sub = phys->output_sub;
        up.render_w = phys->render_w;
        up.render_h = phys->render_h;
    }
    if (phys || up.dest) {
        if (phys) dest = phys->output;
        else dest.Attach(na::observe::AcquireResource(up.dest));
        const uint32_t erw = up.render_w ? up.render_w : w, erh = up.render_h ? up.render_h : h;
        if (!dest) refuse_upscale("Upscale destination is not available.");
        else if (color_msaa) refuse_upscale("Upscaling from an MSAA scene colour is not supported.");
        else if (erw > w || erh > h) refuse_upscale("The render extent is larger than the scene colour.");
        else {
            const D3D12_RESOURCE_DESC od = dest->GetDesc();
            const uint32_t omips = od.MipLevels ? od.MipLevels : 1u;
            const uint32_t omip = up.dest_sub % omips;
            const uint32_t dw = (std::max)(1u, static_cast<uint32_t>(od.Width >> omip)), dh = (std::max)(1u, od.Height >> omip);
            if (od.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || od.SampleDesc.Count != 1 ||
                up.dest_sub >= omips * od.DepthOrArraySize)
                refuse_upscale("The upscale destination is not a single-sample 2D texture.");
            else if (typed(od.Format) != cf)
                refuse_upscale("The upscale destination's format differs from the scene colour (no conversion yet).");
            else if (dw < erw || dh < erh)
                refuse_upscale("The upscale destination is smaller than the render extent.");
            else {
                rw = erw; rh = erh; ow = dw; oh = dh; dsub_out = up.dest_sub;
            }
        }
    }
    if (phys) {
        // A Layer2 frame never falls back to DLAA: that would write into its private companion.
        if (!dest) return phys_fail(g.stats.upscale_note[0] ? g.stats.upscale_note : "Layer2: the physical output cannot be used.");
        sdest = phys->output_state;
    } else {
        if (dest && up.dest == color_id) refuse_upscale("The upscale destination is the scene colour itself.");
        // The destination may be untouched by the game until after J2's insertion: then the
        // state all submitted work left it in.
        if (dest && !state_of(up.dest, dsub_out, &sdest) && !na::observe::KnownSubStateOnQueueNow(g.queue.Get(),up.dest, dsub_out, &sdest)) refuse_upscale("Waiting: the upscale destination's state is not established.");
    }
    const bool upscaling = dest != nullptr;
    // Projection window: this frame's scene was drawn into the top-left corner of the
    // scene colour/depth (J1 wrote the windowed camera). DLSS reads that corner and the
    // full-size output is written back into the scene colour exactly like DLAA.
    const bool windowed = !phys && !upscaling && g.jit.windowed && g.jit.frame == g.frame && g.win.last_frame == g.frame &&
                          g.jit.ew && g.jit.eh && g.jit.ew <= w && g.jit.eh <= h && w == g.win.w && h == g.win.h;
    // Render-small: the scene is the game's own (render-size) frame; DLSS runs at Present.
    const bool deferred = g_render_small.load(std::memory_order_relaxed) && na::present::Scaling() && !phys && !upscaling && !windowed;
    if (windowed) {
        if (color_msaa) { reason("Projection window: MSAA scene colour is not supported."); return false; }
        rw = g.jit.ew; rh = g.jit.eh; ow = w; oh = h;
    }
    // A repeat needs a DLSS result of this scene's size to copy back; the output is at ow x oh.
    const bool can_repeat = windowed && g.output && g.ow == w && g.oh == h && g.color_format == cf && g.cam_f1;
    if (repeat_only && !can_repeat) { reason("Projection window: nothing to repeat for this frame."); return false; }

    // Camera first (nothing is allocated for a frame that cannot run); the tracker
    // update comes after the size check, because a resize resets the tracker.
    na::camera::Matrix vp{};
    bool reversed = false;
    bool held = false, from_j1 = false, held_still=false, extrapolated=false;
    if (saved && saved->has_camera) { vp = saved->vp; reversed = saved->reversed; }
    if (repeat_only) { vp = g.cam_u1; reversed = g.cam_rev; held = true; } // no camera search: the last one is only a placeholder
    na::j2::InterloperFilter judged = g.interlopers;
    const bool found = repeat_only || (saved ? saved->has_camera : find_camera(lists, scene, depth_id, &vp, &reversed, &judged));
    if (saved && saved->judged && !repeat_only) judged = saved->interlopers;
    if (!found && g.jit.valid && g.jit.applied && g.cam_f1) {
        // Astra (COMMS 2026-11-02): the scene list's own blocks may not hold the camera, but
        // J1 found this frame's camera in an earlier batch and rewrote it with this sample:
        // that is the camera the frame was rendered with, and its jitter is known.
        const auto L = g.jit.layout ? na::camera::Layout::ColumnMajor : na::camera::Layout::RowMajor;
        const std::vector<uint8_t> jb(g.jit.jvp[g.jit.layout], g.jit.jvp[g.jit.layout] + 64);
        const na::camera::Matrix m = na::camera::LoadMatrix(jb, 0, L);
        na::camera::Candidate c{};
        if (na::camera::ViewProjection(m, c)) {
            vp = m;
            reversed = g.cam_rev;
            from_j1 = true;
            ++g.stats.camera_from_j1;
        }
    }
    if (!found && !from_j1) {
        // A camera read within the last few frames: predict this frame's (unjittered,
        // so no jitter is claimed for it) rather than leave the game's raw picture showing.
        // A still camera (the last two reads identical: SE5's menu) holds much longer; a
        // moving one stays within the tracker's gap limit. The same frame may also reuse
        // its own camera (an earlier attempt read it but did not run).
        constexpr uint64_t kMaxHeldFrames = 4, kMaxStillHeldFrames = 120;
        bool still = g.cam_f0 != 0;
        for (size_t i = 0; still && i < 16; ++i)
            still = std::fabs(g.cam_u0[i] - g.cam_u1[i]) <= 1e-7 * (std::max)(1.0, std::fabs(g.cam_u1[i]));
        const uint64_t limit = still ? kMaxStillHeldFrames : kMaxHeldFrames;
        const bool fresh = g.cam_f1 && g.frame >= g.cam_f1 && g.frame - g.cam_f1 <= limit;
        if (fresh && !g.resume_reset) {
            const bool velocity = g.cam_f0 && g.cam_f1 == g.cam_f0 + 1 && g.frame == g.cam_f1 + 1;
            vp = velocity ? na::camera::ExtrapolateViewProjection(g.cam_u0, g.cam_u1) : g.cam_u1;
            reversed = g.cam_rev;
            held = true;
            held_still=still;extrapolated=velocity;
            ++g.stats.camera_held;
            if (velocity) ++g.stats.camera_extrapolated;
        }
    }
    if (!found && !held && !from_j1) {
        ++g.stats.no_camera;
        g.camera_offset = g.camera_param = -1; // relearn
        char why[160];
        std::snprintf(why, sizeof why, "Waiting for the camera (lists %zu, binds %zu, blocks %zu, read %zu, matrices %zu, view-proj %zu).",
                      g_cd.lists, g_cd.binds, g_cd.blocks, g_cd.read, g_cd.cands, g_cd.vp);
        reason(why);
        return false;
    }
    pending.camera_source=(saved && saved->has_camera)?6u:from_j1?2u:held?(extrapolated?5u:held_still?3u:4u):1u;
    pending.camera_age=held?g.frame-g.cam_f1:0u;
    if (!held) remember_main_camera(vp); // J1: the main camera, for earlier batches next frame
    // Ring slot (never wait) and size/format changes (only once the GPU is done with the old set).
    const UINT64 done = g.fence->GetCompletedValue();
    Slot& s = g.slots[g.slot];
    if(s.native_scale_use&&!s.native_scale_use.Completed()) {++g.stats.busy;reason("Waiting for the recorded native-scale output to finish.");return false;}
    if (done == UINT64_MAX) { reason("Device removed."); return false; }
    if (s.fence_value && done < s.fence_value) { ++g.stats.busy; reason("Skipped: the previous use of this frame slot is still running."); return false; }
    // Astra (COMMS 2026-11-02): J1 jitters the camera with the extent known at that moment
    // (g.rw/g.rh), which a render-scale change only updates below. The frame was rasterised with
    // a clip offset of 2*px/old_extent, i.e. px*new/old pixels at the new extent: rescale it.
    const uint32_t jit_rw = g.jit.frame == g.frame && g.jit.ew ? g.jit.ew : g.rw, jit_rh = g.jit.frame == g.frame && g.jit.eh ? g.jit.eh : g.rh;
    // A projection window changes only the active extent: the working textures keep the
    // scene's size, so no wait for the GPU (a frame drawn into the corner must not skip).
    if (g.output && g.color_in && g.w == w && g.h == h && g.ow == ow && g.oh == oh && (g.rw != rw || g.rh != rh) && ow == w && oh == h) {
        g.rw = rw; g.rh = rh;
        g.resume_reset = true; // a new extent restarts DLSS history
    }
    const bool resize = !g.output || !g.color_in || g.w != w || g.h != h || g.color_format != cf ||
                        g.rw != rw || g.rh != rh || g.ow != ow || g.oh != oh ||
                        g.samples != cd.SampleDesc.Count || g.sample_quality != cd.SampleDesc.Quality ||
                        g.depth_samples != dd.SampleDesc.Count || g.depth_quality != dd.SampleDesc.Quality || g.depth_format != dd.Format;
    if (resize) {
        for(const auto& old:g.slots)if(old.native_scale_use&&!old.native_scale_use.Completed()) {
            ++g.stats.busy;reason("Waiting for the previous native scene size to finish.");return false;
        }
        if (done + 1 < g.fence_next) { ++g.stats.busy; reason("Waiting for the previous picture size to finish."); return false; }
        if (!uav_store(cf) || !uav_store(DXGI_FORMAT_R32G32_FLOAT)) { reason("This scene colour format cannot be written by DLSS here."); return false; }
        g.msaa = MsaaResolve{};
        if (color_msaa || depth_msaa) {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT support{cf};
            if (FAILED(g.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof support)) ||
                (color_msaa && !(support.Support1 & D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE)) ||
                !g.msaa.Init(g.device.Get(), w, h, cf, dd, cd.SampleDesc, kRing)) {
                g.output.Reset();
                reason("Could not prepare the MSAA resolve pipeline.");
                return false;
            }
        }
        if (!texture(w, h, DXGI_FORMAT_R32_FLOAT, depth_msaa, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, g.depth32) ||
            !texture(w, h, DXGI_FORMAT_R32G32_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g.motion) ||
            !texture(w, h, DXGI_FORMAT_R8_UINT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g.provenance) ||
            !texture(ow, oh, cf, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g.output) ||
            !texture(w, h, cf, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, g.color_in) ||
            !texture(ow, oh, cf, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g.sharp)) {
            g.output.Reset();
            reason("Could not allocate the DLAA working textures.");
            return false;
        }
        g.w = w; g.h = h; g.color_format = cf;
        g.rw = rw; g.rh = rh; g.ow = ow; g.oh = oh;
        g.samples = cd.SampleDesc.Count; g.sample_quality = cd.SampleDesc.Quality;
        g.depth_samples = dd.SampleDesc.Count; g.depth_quality = dd.SampleDesc.Quality; g.depth_format = dd.Format;
        g.resume_reset = true; // remains pending if this recording is not submitted
    }
    const double zero[2] = {0, 0};
    const double* injected = vpj_mode() ? zero : (deferred && g.jitter_unseen) ? zero : held ? zero : from_j1 ? g.jit.px : saved ? (saved->has_camera ? saved->jitter : zero) : g_injected ? g.jit.px : zero; // sample belongs to the captured camera
    double rescaled[2] = {0, 0};
    if (injected != zero && jit_rw && jit_rh && (jit_rw != rw || jit_rh != rh)) {
        rescaled[0] = injected[0] * double(rw) / double(jit_rw);
        rescaled[1] = injected[1] * double(rh) / double(jit_rh);
        injected = rescaled;
        ++g.stats.jitter_extent_rescaled;
    }
    if (!vpj_mode() && !saved && !held && !from_j1 && !g_injected && g.jitter_on) { // a DLAA frame without J1's jitter: why (viewport jitter reports its own coverage)
        if (!g.jit.valid) ++g.stats.unjit_no_sample;
        else if (!g.jit.applied) ++g.stats.unjit_not_applied;
        else ++g.stats.unjit_not_held;
    }
    // Projection window safety: the scene's own camera block held the game's FULL-SIZE camera
    // (J1's windowed copy did not reach it), so the frame was drawn unpacked and scissored to
    // the corner. Upscaling that corner shows a 1/scale zoom of the top left (Cort's 8a0ce83 run:
    // 1-2 frame pops while panning). Never evaluate it: show the last windowed result again.
    const bool unpacked = windowed && !held && !from_j1 && (saved ? !saved->injected : !g_injected);
    if (unpacked) {
        ++g.stats.window_unpacked;
        const uint32_t vpo = g.camera_offset >= 0 ? static_cast<uint32_t>(g.camera_offset) : 0u;
        std::snprintf(g.stats.window_unpacked_note, sizeof g.stats.window_unpacked_note,
                      "frame=%llu jit_frame=%llu applied=%d other_camera_blocks=%llu slot_other=%llu off=%u",
                      static_cast<unsigned long long>(g.frame), static_cast<unsigned long long>(g.jit.frame), g.jit.applied ? 1 : 0,
                      static_cast<unsigned long long>(g.stats.other_camera_blocks),
                      static_cast<unsigned long long>(g.stats.jitter_slot_other_camera), vpo);
        if (!can_repeat || resize) {
            reason("Projection window: this frame was drawn full size and there is no result to repeat.");
            return false;
        }
    }
    const bool repeat = unpacked || repeat_only;
    if (repeat && g.win.last_eval && g.frame > g.win.last_eval + 4) ++g.stats.window_repeat_stale; // an old picture, still better than a clipped one
    pending.tracker = g.tracker;
    pending.interlopers = judged;
    pending.has_interlopers = true;
    // DLSS's history is the last frame it evaluated, and the tracker's previous
    // camera is that same frame, so motion stays correct across a few skipped game
    // frames (a frame whose scene submissions differed): no reset, no visible
    // restart. A long gap (or resume/resize) starts over.
    // 2026-11-04 (Astra 20cddb8 point 2): a gap caused by OUR acquisition (the same live scene
    // selection, so the same scene epoch) keeps the pose history: the previous camera is still
    // exactly the last frame DLSS evaluated, so the motion stays correct over the longer step.
    // Only a different selection, resume or resize starts over. Real cuts are still detected.
    constexpr uint64_t kMaxGapFrames = 4, kMaxSameEpochGapFrames = 32;
    pending.selection_version = na::observe::LiveSelectionVersion();
    const bool same_epoch = g.eval_selection_version && g.eval_selection_version == pending.selection_version;
    const uint64_t max_gap = same_epoch ? kMaxSameEpochGapFrames : kMaxGapFrames;
    const bool tracker_restart = g.resume_reset || (g.submitted_frame && g.frame > g.submitted_frame + max_gap);
    if (tracker_restart) pending.tracker.Reset();
    na::camera::CameraTracker::Limits lim{};
    lim.max_frame_gap = max_gap;
    if (g.submitted_frame && g.frame > g.submitted_frame + 1) { // per-frame motion limits over the whole step
        const double steps = static_cast<double>((std::min)(g.frame - g.submitted_frame, max_gap));
        lim.max_translation *= steps;
        lim.max_rotation = (std::min)(lim.max_rotation * steps, 3.0);
    }
    if (same_epoch && g.submitted_frame && g.frame > g.submitted_frame + kMaxGapFrames && !tracker_restart) ++g.stats.gap_bridged;
    na::camera::CameraFrame cam = pending.tracker.Update(vp, g.frame, rw, rh, injected, lim); // active extent
    if (!cam.valid) { ++g.stats.no_camera; reason("The camera matrix could not be inverted."); return false; }
    bool viewport_sample_known=false;
    if (vpj_mode()) { // the camera is the game's own (no J1 write); the sample is in the viewports
        float vx = 0.f, vy = 0.f;
        viewport_sample_known=vpj_declared(lists, scene, &vx, &vy);
        cam.jitter_px_x = vx; cam.jitter_px_y = vy; cam.jitter_known = true;
    }
    if (!held) { // the newest read camera (one per frame: a later attempt in the same frame replaces it)
        if (g.cam_f1 != g.frame) { g.cam_u0 = g.cam_u1; g.cam_f0 = g.cam_f1; }
        g.cam_u1 = cam.unjittered; g.cam_f1 = g.frame; g.cam_rev = reversed;
    }
    pending.cut = cam.camera_cut;
    if (cam.camera_cut) { // Cort 2026-11-02: an on/off blink every ~30 s matched cuts while standing still: record why
        static const char* const kWhy[] = {"none", "no_history", "gap", "translation", "rotation", "fov", "origin"};
        const char* why = tracker_restart ? (g.resume_reset ? "restart_resume" : "restart_gap") : kWhy[cam.cut_reason < 7 ? cam.cut_reason : 0];
        char one[120];
        std::snprintf(one, sizeof one, "%llu:%s=%.3g%s%s ", static_cast<unsigned long long>(GetTickCount64() / 100 % 100000000),
                      why, cam.cut_value, held ? "/held" : "", from_j1 ? "/j1" : "");
        // Newest first, keep what fits.
        char merged[sizeof g.stats.recent_cuts];
        std::snprintf(merged, sizeof merged, "%s%s", one, g.stats.recent_cuts);
        std::memcpy(g.stats.recent_cuts, merged, sizeof merged);
        // Event log: every camera cut, timed to the frame (a "fov" cut is a zoom, e.g. a scope).
        na::eventlog::Emit("camera_cut", "why=%s value=%.4g held=%u from_j1=%u", why, cam.cut_value, held ? 1u : 0u, from_j1 ? 1u : 0u);
        if (tracker_restart) ++g.stats.cut_restarts;
        else if (cam.cut_reason == 2) ++g.stats.cut_gap;
        else if (cam.cut_reason >= 3 && cam.cut_reason <= 5) ++g.stats.cut_motion;
    }
    // P4-0b: moving-object motion (between-lists route only). The pre list is
    // submitted right before this list; the flow pairs with the frame the camera
    // history pairs with. Any failure leaves this frame camera-only.
    // Flow works on the whole colour at DLAA size; upscaled frames stay camera-only for now.
    if (flow_ticket && !inl && !color_msaa && !upscaling && !windowed && na::j2::flow::Enabled()) {
        na::j2::flow::Source fs;
        fs.color = color.Get();
        fs.subresource = csub;
        fs.state = sc;
        fs.format = cf;
        fs.width = w;
        fs.height = h;
        fs.frame = g.frame;
        fs.previous_frame = g.submitted_frame;
        fs.generation = g.route_gen_applied;
        fs.reset = tracker_restart || cam.camera_cut || resize;
        fs.allow_pair = flow_pair_ok;
        fs.jitter_px[0] = static_cast<float>(cam.jitter_px_x);
        fs.jitter_px[1] = static_cast<float>(cam.jitter_px_y);
        fs.slot = g.slot;
        *flow_ticket = na::j2::flow::Prepare(g.device.Get(), g.queue.Get(), fs);
    }
    // Layer2 payload lifetime, before any command references it: J2's own list keeps it in its
    // ring slot (released when the slot is reused, after its fence); a game list retains it
    // through every submission (the existing recording retention). No retention = no use.
    if (phys && inl) {
        const auto lease = na::nr::native::RetainRecording(inl, std::shared_ptr<void>(std::const_pointer_cast<PhysicalInput>(phys)));
        if (!lease) return phys_fail("Layer2: the game list's recording could not retain the physical inputs.");
    }
    if (!inl) s.physical = phys; // the previous use of this slot has completed (checked above)
    if (!inl && (FAILED(s.alloc->Reset()) || FAILED(s.list->Reset(s.alloc.Get(), nullptr)))) { reason("Could not reset the DLAA command list."); return false; }
    ID3D12GraphicsCommandList* l = inl ? inl : s.list.Get();
    if (touched) *touched = true;
    std::vector<D3D12_RESOURCE_BARRIER> b;
    constexpr UINT NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, SRC = D3D12_RESOURCE_STATE_COPY_SOURCE,
                   DST = D3D12_RESOURCE_STATE_COPY_DEST, UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (repeat) { // the last DLSS result (already sharpened) back into the scene colour; no DLSS, no history change
        add(b, g.output.Get(), UAV, SRC);
        addsub(b, color.Get(), csub, sc, DST);
        flush(l, b);
        D3D12_TEXTURE_COPY_LOCATION cs{}, cdst{};
        cs.Type = cdst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        cs.pResource = g.output.Get();
        cdst.pResource = color.Get();
        cdst.SubresourceIndex = csub;
        l->CopyTextureRegion(&cdst, 0, 0, 0, &cs, nullptr);
        add(b, g.output.Get(), SRC, UAV);
        addsub(b, color.Get(), csub, DST, sc);
        flush(l, b);
        if (!inl && FAILED(l->Close())) { reason("Could not close the repeat list."); return false; }
        pending.tracker = g.tracker; // the tracker's previous camera stays the last evaluated frame
        pending.interlopers = g.interlopers;
        pending.cut = false;
        pending.keep_reset = true;
        ++g.stats.window_repeated;
        if (unpacked) ++g.win.recent_repeated;
        *out_list = l;
        return true;
    }
    // 1. Copy or resolve exactly the scene views into single-sample DLSS inputs.
    const UINT color_source = color_msaa ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE : SRC;
    const UINT color_dest = color_msaa ? D3D12_RESOURCE_STATE_RESOLVE_DEST : DST;
    addsub(b, depth.Get(), dsub, sd, SRC);
    add(b, g.depth32.Get(), NPSR, depth_msaa ? UAV : DST);
    if (!skip_color) {
        addsub(b, color.Get(), csub, sc, color_source);
        add(b, g.color_in.Get(), NPSR, color_dest);
    }
    flush(l, b);
    if (skip_color) {
    } else if (color_msaa) {
        l->ResolveSubresource(g.color_in.Get(), 0, color.Get(), csub, cf);
    } else {
        D3D12_TEXTURE_COPY_LOCATION cs{}, cdst{};
        cs.Type = cdst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        cs.pResource = color.Get(); // the subresource the scene was drawn into
        cs.SubresourceIndex = csub;
        cdst.pResource = g.color_in.Get();
        l->CopyTextureRegion(&cdst, 0, 0, 0, &cs, nullptr);
    }
    if (!skip_color) add(b, g.color_in.Get(), color_dest, NPSR);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.Type = dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = depth.Get();
    src.SubresourceIndex = dsub; // plane 0 of the slice / mip the scene used
    dst.pResource = depth_msaa ? g.msaa.DepthCopy() : g.depth32.Get();
    dst.SubresourceIndex = 0;
    l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    addsub(b, depth.Get(), dsub, SRC, sd);
    if (depth_msaa) {
        addsub(b, g.msaa.DepthCopy(), 0, DST, NPSR);
        flush(l, b);
        g.msaa.ResolveDepth(l, g.depth32.Get(), w, h, reversed, g.slot);
        b.push_back(uav_barrier(g.depth32.Get()));
        add(b, g.depth32.Get(), UAV, NPSR);
        addsub(b, g.msaa.DepthCopy(), 0, NPSR, DST);
    } else add(b, g.depth32.Get(), DST, NPSR);
    flush(l, b);
    // 2/3. camera motion vectors
    if (!deferred && !(saved && saved->at_present)) smear_step(); // smear test variants: native DLAA only
    bool have_motion = false;
    bool in_tier1_fed = false;
    na::j2::compute_flow::Tier3 flow_tier{}; // evidence capture
    if (g.arbiter_ok) {
        na::motion::gpu::Inputs in{};
        in.depth = g.depth32.Get();
        in.tier2_valid_flag = true;
        in.depth_clear = reversed ? 0.0f : 1.0f;
        // One composed current->previous clip matrix (double, then float); the
        // arbiter's second matrix is identity. Never chain two float matrices
        // through world space (false motion on a still camera -> DLAA blur).
        const na::camera::Matrix composed = na::camera::ComposeReprojectionRow(cam.inv_view_proj_current_row, cam.view_proj_previous_row);
        in.inv_view_proj_current = to_mat4(composed);
        // M3 (camera_fit_gpu.hpp): basis of the six rigid camera corrections around this composed
        // matrix; compute_flow fits them on the GPU from depth + this pair's flow.
        const na::motion::gpu::FitBasis fit_basis = na::motion::gpu::MakeFitBasis(
            composed.data(), cam.inv_view_proj_current_row.data(), cam.view_proj_previous_row.data(), cam.position.data());
        in.fit_basis = &fit_basis;
        {
            na::camera::Matrix id{};
            id[0] = id[5] = id[10] = id[15] = 1.0;
            in.view_proj_previous = to_mat4(id);
        }
        // J1: this frame's raster sample sits at pixel centre - jitter in the unjittered
        // projection; the arbiter removes it before reprojecting (Astra, 378663e finding).
        in.jitter_current_px[0] = static_cast<float>(cam.jitter_px_x);
        in.jitter_current_px[1] = static_cast<float>(cam.jitter_px_y);
        // AMD is an in-list dispatch, after this frame's resolved pre-DLAA
        // colour/depth copies. It needs no batch split or external queue wait.
        // Camera reconstruction (Cort 2026-11-04: flow on removed the oily smear because the M3 fit
        // lived inside the flow toggle). When the selected matrix carries no view for 30 frames in a
        // row, the camera term is zero whatever the player does: AMD flow + the M3 fit then run even
        // with optical flow off (the toggle keeps only the object-motion residual); 30 frames with a
        // view end it. Games with a real camera matrix are unaffected.
        static int no_view_streak = 0, view_streak = 0;
        static bool reconstruct = false;
        if (na::motion::gpu::CarriesNoView(cam.view_proj_current_row.data())) { ++no_view_streak; view_streak = 0; }
        else { ++view_streak; no_view_streak = 0; }
        if (!reconstruct && no_view_streak >= 30) reconstruct = true;
        if (reconstruct && view_streak >= 30) reconstruct = false;
        const bool object_flow = na::j2::flow::Enabled();
        if((object_flow || reconstruct) && !windowed && !(saved && saved->at_present) && (inl || !flow_ticket || !flow_ticket->valid)){
            na::j2::compute_flow::Frame frame;frame.frame=g.frame;frame.previous=g.submitted_frame;
            frame.generation=g.router.Current().history_generation;frame.slot=g.slot;
            frame.reset=cam.camera_cut || resize || g.resume_reset;frame.hdr=is_float_color(cf);
            frame.jitter[0]=in.jitter_current_px[0];frame.jitter[1]=in.jitter_current_px[1];
            frame.raster_jitter[0]=frame.jitter[0];frame.raster_jitter[1]=frame.jitter[1];frame.raster_sample=true;
            frame.raster_confirmed=viewport_sample_known;
            frame.source_generation=pending.selection_version;
            frame.residual=object_flow;frame.camera_reconstruct=reconstruct;
            na::j2::compute_flow::Tier3 tier;
            na::j2::compute_flow::SetLoosen(na::settings::Get().j2_flow_loosen.load(std::memory_order_relaxed));
            pending.compute_flow=na::j2::compute_flow::Record(l,g.queue.Get(),g.color_in.Get(),g.depth32.Get(),in,frame,tier);
            // The residual was derived against the fitted camera: tier 2 must use the same fix.
            if(tier.residual){in.tier3_valid_flag=true;in.tier3_residual=tier.residual;in.tier3_valid=tier.valid;in.tier3_mask=tier.mask;}
            in.fix=tier.fix; // also without a residual (camera reconstruction only)
            flow_tier=tier;
        }
        // P4-0b: camera + accepted moving-object residual (tier 3) when this frame's pair ran.
        na::j2::flow::Tier3 t3;
        if (flow_ticket && flow_ticket->pair && na::j2::flow::RecordFusion(l, *flow_ticket, g.depth32.Get(), in, &t3)) {
            in.tier3_residual = t3.residual;
            in.fix = nullptr; // the NVOFA residual is relative to the unfitted camera
            in.tier3_mask = t3.mask;
            in.tier3_valid = t3.valid;
            in.tier3_valid_flag = true;
        }
        // Tier 1: the game's own motion texture, used only once it predicts the picture (moving
        // characters in a still menu get ~0 from the camera and smear; Cort 2026-11-06).
        if (g.game_motion_ok && !(saved && saved->at_present)) {
            static const uint32_t kMotionFormats[] = {
                DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_R16G16_SNORM};
            na_object_id ids[6] = {};
            const uint32_t found_n = na::observe::FindTextures(kMotionFormats, 4, w, h, ids, 6);
            ComPtr<ID3D12Resource> hold[na::j2::game_motion::kCandidates];
            na::j2::game_motion::Candidate cands[na::j2::game_motion::kCandidates];
            uint32_t nc = 0;
            for (uint32_t i = 0; i < found_n && nc < na::j2::game_motion::kCandidates; ++i) {
                uint32_t st = 0;
                // The same state rule as the scene colour/depth (inline: this list so far; otherwise the
                // batch up to the scene list, then the state carried in). p43052: the queue-only lookup
                // never knew it (the game writes it earlier in the same batch).
                const bool known = state_of(ids[i], 0, &st);
                if (!known) continue;
                ID3D12Resource* r = na::observe::AcquireResource(ids[i]);
                if (!r) continue;
                hold[nc].Attach(r);
                const auto rd = r->GetDesc();
                if (rd.SampleDesc.Count != 1 || rd.DepthOrArraySize != 1 || r == g.motion.Get()) { hold[nc].Reset(); continue; }
                cands[nc].resource = r;
                cands[nc].state = static_cast<D3D12_RESOURCE_STATES>(st);
                cands[nc].id = ids[i];
                ++nc;
            }
            g.game_motion.SetForce(smear_mode() == 5);
            const auto gm = g.game_motion.Record(l, g.slot, cands, nc, g.color_in.Get(), rw, rh,
                                                 cam.camera_cut || resize || g.resume_reset);
            if (gm.motion) {
                in_tier1_fed = true;
                in.tier1_motion = gm.motion;
                in.tier1_valid = gm.valid;
                in.tier1_valid_flag = true;
            }
            const auto gs = g.game_motion.GetStats();
            const int used = gs.used_candidate < 0 ? -1 : gs.used_candidate * 16 + gs.used_encoding;
            if (used != g.game_motion_used || (!g.game_motion_logged && gs.measured >= 64)) {
                char text[sizeof g.stats.game_motion];
                g.game_motion.Describe(text, sizeof text);
                na::eventlog::Emit("game_motion", "%s %s", used != g.game_motion_used ? (used >= 0 ? "use" : "none") : "first", text);
                g.game_motion_used = used;
                if (gs.measured >= 64) g.game_motion_logged = true;
            }
        }
        // Tier 1 from the game's own draws replayed with stream output (producers.cpp "Draw replay"):
        // exact per-object motion where the game's motion texture is not in use. Taken every frame
        // (it also prepares the next frame's slot), fed only when it matched draws.
        if (g.replay_ok && !(saved && saved->at_present)) {
            const float no_jitter[2] = {0.f, 0.f}; // viewport jitter: the clip positions carry none
            const auto rm = g.replay.Record(l, g.depth32.Get(), rw, rh, reversed, cam.camera_cut || resize || g.resume_reset, no_jitter);
            if (rm.motion && !in_tier1_fed && na::settings::Get().j2_draw_replay.load(std::memory_order_relaxed)) {
                in_tier1_fed = true;
                g.replay_fed_frame = g.frame;
                in.tier1_motion = rm.motion;
                in.tier1_valid = rm.valid;
                in.tier1_valid_flag = true;
            }
        }
        if (smear_mode() == 1) { // smear test: no motion at all (identity reprojection, no other tier)
            na::camera::Matrix unit{};
            unit[0] = unit[5] = unit[10] = unit[15] = 1.0;
            in.inv_view_proj_current = to_mat4(unit);
            in.tier1_valid_flag = in.tier3_valid_flag = false;
            in.fix = nullptr;
            in.jitter_current_px[0] = in.jitter_current_px[1] = 0.f;
        }
        na::motion::gpu::Outputs o{};
        o.xy = g.motion.Get();
        o.provenance = g.provenance.Get();
        o.width = rw; // the active extent (top left); render pixels
        o.height = rh;
        have_motion = g.arbiter.Dispatch(l, in, o, g.slot);
    }
    if (!have_motion) {
        if (inl) { // hand the game's list back with the scene colour in its own state
            addsub(b, color.Get(), csub, color_source, sc);
            flush(l, b);
        } else {
            l->Close();
        }
        reason("Stopped: motion vectors could not be built.");
        g.latched = true;
        return false;
    }
    b.push_back(uav_barrier(g.motion.Get()));
    add(b, g.motion.Get(), UAV, NPSR);
    flush(l, b);
    std::shared_ptr<na::xegtao::Frame> ambient;
    if (na::settings::Get().xegtao_enabled.load(std::memory_order_relaxed) && !skip_color) {
        na::xegtao::Inputs ai;ai.color=g.color_in.Get();ai.depth=g.depth32.Get();ai.width=rw;ai.height=rh;
        ai.color_format=typed(cf);ai.clear_depth=reversed?0.f:1.f;
        ai.jitter_x=static_cast<float>(cam.jitter_px_x);ai.jitter_y=static_cast<float>(cam.jitter_px_y);
        const auto column=na::camera::TransposeMatrix(cam.view_proj_current_row);
        if(na::xegtao::FromViewProjection(column.data(),ai.projection))
            ambient=na::xegtao::ScenePass().Prepare(g.device.Get(),g.queue.Get(),ai,na::xegtao::CurrentOptions());
        if(ambient){
            // Register ownership BEFORE recording work, using existing machinery.
            if(inl){if(!na::nr::native::RetainRecording(l,ambient))ambient.reset();}
            else s.ambient=ambient;
            if(ambient&&!ambient->Record(l))ambient.reset();
        }
    }
    if (deferred) { // render-small: depth/motion retained for Present, optional AO goes into the pre-HUD scene
        if(ambient){
            const UINT from=color_msaa?D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE:SRC;
            const UINT into=color_msaa?D3D12_RESOURCE_STATE_RENDER_TARGET:DST;
            add(b,ambient->Color(),NPSR,from);addsub(b,color.Get(),csub,color_source,into);flush(l,b);
            if(color_msaa)g.msaa.WriteColor(l,ambient->Color(),color.Get(),csub/g.stats.color_mips,w,h,g.slot);
            else{
                D3D12_TEXTURE_COPY_LOCATION ao_source{},ao_target{};ao_source.Type=ao_target.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                ao_source.pResource=ambient->Color();ao_target.pResource=color.Get();ao_target.SubresourceIndex=csub;
                const D3D12_BOX box{0,0,0,rw,rh,1};l->CopyTextureRegion(&ao_target,0,0,0,&ao_source,&box);
            }
            add(b,ambient->Color(),from,NPSR);addsub(b,color.Get(),csub,into,color_source);flush(l,b);
        }
        if (!skip_color) addsub(b, color.Get(), csub, color_source, sc);
        add(b, g.motion.Get(), NPSR, UAV);
        flush(l, b);
        if (!inl && FAILED(l->Close())) { reason("Render-small: could not close the depth and motion list."); g.latched = true; return false; }
        const bool carry = g.def.valid && g.def.reset; // a reset whose frame never reached DLSS still applies
        g.def.valid = true;
        g.def.frame = g.frame;
        g.def.jx = static_cast<float>(cam.jitter_px_x);
        g.def.jy = static_cast<float>(cam.jitter_px_y);
        g.def.reset = cam.camera_cut || resize || g.resume_reset || carry;
        g.def.reversed = reversed;
        g.def.w = w; g.def.h = h;
        g.def.vp_cur = cam.view_proj_current_row; g.def.vp_prev = cam.view_proj_previous_row; g.def.inv_cur = cam.inv_view_proj_current_row;
        g.def.eye = cam.position; g.def.forward = cam.forward;
        g.def.camera_source = pending.camera_source; g.def.camera_age = pending.camera_age;
        g.def.selection_version = pending.selection_version;
        g.def.tracker_restart = tracker_restart; g.def.cut_reason = cam.cut_reason;
        g.def.flow_ran = flow_tier.residual != nullptr || flow_tier.fix != nullptr;
        g.def.scene_color = g.color_in.Get();
        g.def.flow_jitter_x=pending.compute_flow.frame.jitter[0];g.def.flow_jitter_y=pending.compute_flow.frame.jitter[1];
        g.def.flow_jitter_convention=pending.compute_flow.colour_convention;g.def.flow_jitter_epoch=pending.compute_flow.colour_epoch;
        g.def.flow_residual = flow_tier.residual; g.def.flow_valid = flow_tier.valid; g.def.flow_mask = flow_tier.mask;
        if (saved && saved->at_present) ++g.stats.small_present_source;
        else { ++g.stats.small_deferred; g.small_color = color_id; g.small_depth = depth_id; g.small_csub = csub; g.small_dsub = dsub; }
        g.stats.width = w; g.stats.height = h;
        g.stats.color_format = static_cast<uint32_t>(cd.Format);
        g.stats.render_w = rw; g.stats.render_h = rh;
        g.stats.jitter_phases = jitter_phases();
        *out_list = l;
        return true;
    }
    // 4. DLSS at native resolution
    na::dlss::EvalDesc e{};
    const int smear = smear_mode();
    e.color = ambient?ambient->Color():g.color_in.Get();
    // Jitter shift: the declared offset moved into the colour, so the picture does not shake.
    bool shifted = false;
    flush(l, b); // colour is readable (NON_PIXEL_SHADER_RESOURCE) before the shift reads it
    if (!vpj_mode() && (cam.jitter_px_x != 0 || cam.jitter_px_y != 0) && na::settings::Get().j2_jitter_shift.load(std::memory_order_relaxed) != 0 &&
        jshift_target(g.jshift, rw, rh, typed(cf))) {
        shifted = jshift_dispatch(l, e.color, g.jshift.Get(), rw, rh, typed(cf), static_cast<float>(cam.jitter_px_x), static_cast<float>(cam.jitter_px_y), g.slot);
        if (shifted) {
            b.push_back(uav_barrier(g.jshift.Get()));
            add(b, g.jshift.Get(), UAV, NPSR);
            flush(l, b);
            e.color = g.jshift.Get();
        }
    }
    e.depth = g.depth32.Get();
    e.motion = g.motion.Get();
    e.output = g.output.Get();
    e.render_w = e.max_render_w = rw; // active extent at the top left of the inputs
    e.render_h = e.max_render_h = rh;
    e.out_w = ow;
    e.out_h = oh;
    e.quality = na::dlss::QualityForScale(rw, rh, ow, oh); // DLAA when equal
    e.preset = na::j2::PresetFor(na::settings::Get().j2_preset.load(), rw, ow); // Automatic: L for DLAA/Quality, M for Balanced/Performance
    e.view_id = kViewId;
    e.jitter_x = static_cast<float>(cam.jitter_px_x); // DLSS: colour sampled at pixel centre - jitter
    e.jitter_y = static_cast<float>(cam.jitter_px_y);
    if (inl) {
        g.stats.inline_jitter_x = e.jitter_x;
        g.stats.inline_jitter_y = e.jitter_y;
    }
    e.reset = cam.camera_cut || resize || g.resume_reset;
    e.hdr = is_float_color(cf);
    e.depth_inverted = reversed;
    g.stats.depth_reversed = reversed;
    e.auto_exposure = true;
    {
        // Cort 2026-11-04: after a few mode changes DLAA turned dark (highlights pinned near 1.0
        // in DLSS's raw output, run p27200) and stayed dark; Quality, on its own feature, was fine.
        // Coming back to DLAA from render-small therefore starts a fresh DLAA feature, so no state
        // the old one built up survives a mode change.
        static uint64_t small_seen = 0;
        if (g.stats.small_dlss != small_seen) { small_seen = g.stats.small_dlss; na::dlss::ReleaseView(kViewId); ++g.stats.dlaa_fresh_features; }
    }
    const auto grade_options=na::donut::CurrentOptions();
    if(grade_options.tone||grade_options.bloom){
        na::donut::Inputs di;di.color=e.color;di.format=typed(e.color->GetDesc().Format);di.width=rw;di.height=rh;
        di.srgb=di.format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB||di.format==DXGI_FORMAT_R8G8B8A8_UNORM;di.reset=e.reset;di.stream=kViewId;
        auto grade=na::donut::ScenePass().Prepare(g.device.Get(),g.queue.Get(),di,grade_options);
        if(grade){if(inl){if(!na::nr::native::RetainRecording(l,grade))grade.reset();}else s.grade=grade;
            if(grade&&grade->Record(l))e.color=grade->Color();}
    }
    if (smear == 2 && smear_ones(l, rw, rh)) e.bias_current_color = g_smear.ones.Get();
    {   // smear test 4: preset L on a fresh feature, and a fresh one again afterwards (presets apply at creation)
        static bool preset_test = false;
        if ((smear == 4) != preset_test) { preset_test = smear == 4; na::dlss::ReleaseView(kViewId); e.reset = true; }
        if (smear == 4) e.preset = 12; // NVSDK_NGX_DLSS_Hint_Render_Preset_L
    }
    const bool evaluated = na::dlss::Evaluate(l, e);
    if (!evaluated) ++g.stats.dlss_failed;
    if (shifted) { add(b, g.jshift.Get(), NPSR, UAV); flush(l, b); }
    if (evaluated && na::capture::j2::Armed()) {
        // Evidence capture: copied in this same list right after the evaluate, so the GPU
        // order makes them exactly this evaluation's inputs and raw DLSS output.
        na::capture::j2::Planes cp;
        cp.color = g.color_in.Get(); // the scene as drawn (scene_color below); DLSS got e.color
        cp.depth = g.depth32.Get();
        cp.motion = g.motion.Get();
        cp.output = g.output.Get();
        na::capture::j2::Identity ci;
        ci.present_ordinal = g.frame;
        ci.submission = g.stats.submissions;
        ci.recording = inl ? na::observe::ListRecording(inl) : 0;
        ci.view_color = color_id;
        ci.view_depth = depth_id;
        ci.view_id = static_cast<uint32_t>(kViewId);
        const auto& rd = g.router.Current();
        ci.route = static_cast<uint32_t>(inl ? na::j2::Route::Inline : na::j2::Route::Between);
        ci.route_mode = static_cast<uint32_t>(g.router.Mode());
        ci.history_generation = rd.history_generation;
        // Inline: the root-layout hash of the consumer class that triggered DLAA, when a class did,
        // else the route's boundary signature.
        ci.insertion_signature = inl ? (g.class_fired ? g.fired_class.root : rd.signature) : 0;
        ci.color_sub = csub;
        ci.width = w;
        ci.height = h;
        ci.color_format = static_cast<uint32_t>(cf);
        ci.jitter_x = e.jitter_x;
        ci.jitter_y = e.jitter_y;
        ci.mv_scale_x = e.mv_scale_x;
        ci.mv_scale_y = e.mv_scale_y;
        ci.reset = e.reset ? 1u : 0u;
        ci.camera_cut = cam.camera_cut ? 1u : 0u;
        ci.preset = static_cast<uint32_t>(e.preset);
        ci.sharpness = smear == 3 ? 0u : na::settings::Get().j2_sharpness.load(std::memory_order_relaxed);
        ci.debug_view = static_cast<uint32_t>(g_debug_view.load(std::memory_order_relaxed));
        ci.entry_pipeline=g.capture_entry.pipeline;ci.entry_root=g.capture_entry.root;
        ci.entry_destination=g.capture_entry.destination;ci.entry_operation=g.capture_entry.operation;
        ci.entry_barrier=g.capture_entry.barrier;ci.entry_kind=static_cast<uint32_t>(g.capture_entry.op);
        // The native path's own camera and DLSS facts (were blank here; RC 2026-11-06).
        ci.inputs_frame = g.frame;
        ci.camera_valid = true;
        for (int i = 0; i < 16; ++i) { ci.vp_cur[i] = cam.view_proj_current_row[i]; ci.vp_prev[i] = cam.view_proj_previous_row[i]; ci.inv_vp_cur[i] = cam.inv_view_proj_current_row[i]; }
        for (int i = 0; i < 3; ++i) ci.eye[i] = cam.position[i];
        ci.camera_source = pending.camera_source; ci.camera_age = pending.camera_age;
        ci.selection_version = pending.selection_version;
        ci.tracker_restart = tracker_restart ? 1u : 0u; ci.cut_reason = static_cast<uint32_t>(cam.cut_reason);
        ci.depth_reversed = reversed ? 1u : 0u;
        ci.flow_ran = (flow_tier.residual != nullptr || flow_tier.fix != nullptr) ? 1u : 0u;
        ci.flow_loosen = na::settings::Get().j2_flow_loosen.load(std::memory_order_relaxed);
        ci.raster_jitter_x = static_cast<float>(cam.jitter_px_x); ci.raster_jitter_y = static_cast<float>(cam.jitter_px_y);
        ci.flow_jitter_x=pending.compute_flow.frame.jitter[0];ci.flow_jitter_y=pending.compute_flow.frame.jitter[1];
        ci.flow_jitter_convention=pending.compute_flow.colour_convention;ci.flow_jitter_epoch=pending.compute_flow.colour_epoch;
        ci.dlss_hdr = e.hdr; ci.dlss_depth_inverted = e.depth_inverted; ci.dlss_auto_exposure = e.auto_exposure && !e.exposure;
        ci.dlss_mv_lowres = !e.mv_display_res; ci.dlss_mv_jittered = e.mv_jittered;
        ci.dlss_pre_exposure = e.pre_exposure; ci.dlss_exposure_scale = e.exposure_scale;
        ci.dlss_quality = static_cast<uint32_t>(e.quality); ci.dlss_bias_mask = e.bias_current_color != nullptr;
        ci.jitter_shifted = shifted; ci.smear_mode = smear;
        ci.game_motion_fed = in_tier1_fed;
        // Game velocity candidates (Cort 2026-11-06, menu smear): two-channel float/norm
        // textures with exactly the scene's extent, copied in their known state so a capture
        // shows whether the game writes per-pixel motion (vectors on movers, ~0 on walls).
        ComPtr<ID3D12Resource> velocity_hold[2];
        {
            static const uint32_t kVelocityFormats[] = {
                DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_UNORM,
                DXGI_FORMAT_R16G16_SNORM}; // not RG32: J2's own motion and flow planes use it
            static const char* const kVelocityNames[2] = {"velocity_candidate_0", "velocity_candidate_1"};
            na_object_id ids[6] = {};
            const uint32_t found_n = na::observe::FindTextures(kVelocityFormats, 4, w, h, ids, 6);
            uint32_t used = 0;
            for (uint32_t i = 0; i < found_n && used < 2 && cp.extra_count < na::capture::j2::kMaxExtra; ++i) {
                uint32_t st = 0;
                // The same state rule as the scene colour/depth (inline: this list so far; otherwise the
                // batch up to the scene list, then the state carried in). p43052: the queue-only lookup
                // never knew it (the game writes it earlier in the same batch).
                const bool known = state_of(ids[i], 0, &st);
                if (!known) {
                    ++g.stats.velocity_capture_unknown_state;
                    // p19692: still never known. Say why, once per texture (every 600th after that).
                    static std::unordered_map<uint64_t, uint32_t> told;
                    if (told[ids[i]]++ % 600 == 0) {
                        char why[400] = {};
                        na::observe::DescribeStateEvidence(inl ? static_cast<ID3D12CommandList*>(inl) : lists[scene], ids[i], 0, why, sizeof why);
                        uint32_t qs = 0, ss = 0;
                        const bool on_queue = na::observe::KnownSubStateOnQueueNow(g.queue.Get(), ids[i], 0, &qs);
                        const bool in_sub = na::observe::KnownSubStateInCurrentSubmission(ids[i], 0, &ss);
                        char per_list[48] = {}; // batch lists 0..scene: K known, A absent, U unavailable
                        for (UINT k = 0; k <= scene && k < sizeof per_list - 1; ++k) {
                            uint32_t ks = 0;
                            const auto ev = na::observe::ListStateSub(lists[k], ids[i], 0, &ks);
                            per_list[k] = ev == na::observe::StateLookup::Known ? 'K' : ev == na::observe::StateLookup::Absent ? 'A' : 'U';
                        }
                        na::eventlog::Emit("velocity_state", "id=%llu inline=%d scene=%u lists=%s queue_known=%d(0x%x) submission_known=%d(0x%x) evidence=%s",
                                           static_cast<unsigned long long>(ids[i]), inl ? 1 : 0, scene, per_list, on_queue ? 1 : 0, qs, in_sub ? 1 : 0, ss, why);
                    }
                    continue;
                }
                ID3D12Resource* r = na::observe::AcquireResource(ids[i]);
                if (!r) continue;
                velocity_hold[used].Attach(r);
                if (r->GetDesc().SampleDesc.Count != 1 || r == g.motion.Get()) { velocity_hold[used].Reset(); continue; }
                cp.extra[cp.extra_count++] = {kVelocityNames[used], r, static_cast<D3D12_RESOURCE_STATES>(st)};
                ++used;
                ++g.stats.velocity_captured;
            }
            g.stats.velocity_candidates = found_n;
            // The half-size companions (p25748: R16G16_FLOAT at half render size too).
            static const char* const kHalfNames[2] = {"velocity_half_0", "velocity_half_1"};
            ComPtr<ID3D12Resource> half_hold[2];
            na_object_id hids[6] = {};
            const uint32_t half_n = na::observe::FindTextures(kVelocityFormats, 4, (w + 1) / 2, (h + 1) / 2, hids, 6);
            const uint32_t half_n2 = half_n ? half_n : na::observe::FindTextures(kVelocityFormats, 4, w / 2, h / 2, hids, 6);
            uint32_t hused = 0;
            for (uint32_t i = 0; i < half_n2 && hused < 2 && cp.extra_count < na::capture::j2::kMaxExtra; ++i) {
                uint32_t st = 0;
                if (!state_of(hids[i], 0, &st)) { ++g.stats.velocity_capture_unknown_state; continue; }
                ID3D12Resource* r = na::observe::AcquireResource(hids[i]);
                if (!r) continue;
                half_hold[hused].Attach(r);
                if (r->GetDesc().SampleDesc.Count != 1) { half_hold[hused].Reset(); continue; }
                cp.extra[cp.extra_count++] = {kHalfNames[hused], r, static_cast<D3D12_RESOURCE_STATES>(st)};
                ++hused;
                ++g.stats.velocity_captured;
            }
            pending.capture = na::capture::j2::RecordEvaluation(l, cp, ci);
        }
    }
    if (evaluated) {
        const int dv = g_debug_view.load(std::memory_order_relaxed);
        if (dv >= 1 && dv <= 3) {
            b.push_back(uav_barrier(g.output.Get()));
            flush(l, b);
            debug_record(l, dv, g.output.Get(), cf, g.ow, g.oh, g.rw, g.rh, g.slot); // replaces the DLAA result with the debug picture
        } else if (const uint32_t pct = smear == 3 ? 0u : 2u * na::settings::Get().j2_sharpness.load(std::memory_order_relaxed)) {
            // RCAS: output -> sharp, then back into output so the write-back path is unchanged.
            b.push_back(uav_barrier(g.output.Get()));
            add(b, g.output.Get(), UAV, NPSR);
            flush(l, b);
            if (rcas_record(l, pct, cf)) {
                b.push_back(uav_barrier(g.sharp.Get()));
                add(b, g.sharp.Get(), UAV, SRC);
                add(b, g.output.Get(), NPSR, DST);
                flush(l, b);
                l->CopyResource(g.output.Get(), g.sharp.Get());
                add(b, g.sharp.Get(), SRC, UAV);
                add(b, g.output.Get(), DST, UAV);
                ++g.stats.sharpened_frames;
            } else {
                add(b, g.output.Get(), NPSR, UAV);
            }
            flush(l, b);
        }
    }
    // 5. result -> scene colour (DLAA) or the upscale destination, everything back to the game's states
    if (evaluated && upscaling) {
        b.push_back(uav_barrier(g.output.Get()));
        add(b, g.output.Get(), UAV, SRC);
        addsub(b, color.Get(), csub, color_source, sc); // the game's scene colour is left as drawn
        addsub(b, dest.Get(), dsub_out, sdest, DST);
        flush(l, b);
        D3D12_TEXTURE_COPY_LOCATION cs{}, cdst{};
        cs.Type = cdst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        cs.pResource = g.output.Get();
        cdst.pResource = dest.Get();
        cdst.SubresourceIndex = dsub_out;
        const D3D12_BOX box{0, 0, 0, ow, oh, 1};
        l->CopyTextureRegion(&cdst, 0, 0, 0, &cs, &box);
        add(b, g.output.Get(), SRC, UAV);
        addsub(b, dest.Get(), dsub_out, DST, sdest);
    } else if (evaluated) {
        b.push_back(uav_barrier(g.output.Get()));
        const UINT output_read = color_msaa ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : SRC;
        const UINT color_write = color_msaa ? D3D12_RESOURCE_STATE_RENDER_TARGET : DST;
        add(b, g.output.Get(), UAV, output_read);
        addsub(b, color.Get(), csub, color_source, color_write);
        flush(l, b);
        if (color_msaa) {
            g.msaa.WriteColor(l, g.output.Get(), color.Get(), csub / g.stats.color_mips, w, h, g.slot);
        } else {
            D3D12_TEXTURE_COPY_LOCATION cs{}, cdst{};
            cs.Type = cdst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            cs.pResource = g.output.Get();
            cdst.pResource = color.Get(); // back into the scene's subresource
            cdst.SubresourceIndex = csub;
            l->CopyTextureRegion(&cdst, 0, 0, 0, &cs, nullptr);
        }
        add(b, g.output.Get(), output_read, UAV);
        addsub(b, color.Get(), csub, color_write, sc);
    } else {
        addsub(b, color.Get(), csub, color_source, sc);
    }
    add(b, g.motion.Get(), NPSR, UAV);
    flush(l, b);
    const bool closed = inl ? true : SUCCEEDED(l->Close()); // the game closes its own list
    if (!closed) {
        // A rejected list repeats every frame and may cost the device: stop for this
        // session and say exactly what was being processed. Never retried.
        char why[160];
        std::snprintf(why, sizeof why, "Stopped: the DLAA list was rejected (colour fmt %u mips %u slices %u state 0x%X, depth fmt %u state 0x%X). %s",
                      static_cast<unsigned>(cd.Format), static_cast<unsigned>(cd.MipLevels), static_cast<unsigned>(cd.DepthOrArraySize), sc, static_cast<unsigned>(dd.Format), sd,
                      na::dlss::GetStats().reason);
        reason(why);
        g.latched = true;
        return false;
    }
    if (!evaluated) {
        char why[160];
        std::snprintf(why, sizeof why, "DLSS refused the frame: %s", na::dlss::GetStats().reason[0] ? na::dlss::GetStats().reason : "no reason given.");
        reason(why);
        return false; // nothing useful to submit; retried next frame
    }
    g.stats.width = w;
    g.stats.height = h;
    g.stats.color_format = static_cast<uint32_t>(cd.Format);
    g.stats.render_w = rw;
    g.stats.render_h = rh;
    g.stats.out_w = ow;
    g.stats.out_h = oh;
    g.stats.jitter_phases = jitter_phases();
    if (upscaling) { ++g.stats.upscaled_frames; g.stats.upscale_note[0] = 0; }
    if (windowed) { ++g.stats.window_frames; g.win.last_eval = g.frame; }
    if (phys) phys_report.result = PhysicalOutcome::Evaluated;
    *out_list = l;
    return true;
}

// Inline insertion runs before the actual operation. Completed submitted frames
// teach a terminal post-entry PSO/root even when its colour SRV is unresolved.
// The original boundary matcher remains the warm-up path. Host commands restore
// the game's state before forwarding the real operation; nothing runs at Close.
void on_table_bind_impl(ID3D12GraphicsCommandList* l, const na_object_id* srvs, uint32_t n, bool compute,
                        const na::j2::InsertionEvent* entry=nullptr)
{
    if (!na::settings::EffectsReady() || !g.enabled.load(std::memory_order_relaxed) || g.paused.load(std::memory_order_relaxed)) return;
    uint64_t depth_id = 0, color_id = 0;
    na::observe::GetLiveSelection(&depth_id, &color_id);
    if (!color_id || !depth_id) return;
    bool hit = false;
    for (uint32_t i = 0; i < n && !hit; ++i) hit = srvs[i] == color_id;
    if (!hit) return;
    if(g_native_reduced.load(std::memory_order_relaxed)) {
        // The observer already proved this actual operation reads the selected
        // scene. The reconstruction lane will inspect its exact destination.
        t_native_hint=l;t_native_color=color_id;t_native_depth=depth_id;
        t_native_recording=na::observe::ListRecording(l);t_native_budget=kNativeHintOps;
        g_nb.hints.fetch_add(1,std::memory_order_relaxed);return;
    }
    const na::observe::HostWorkScope host; // our commands in the game's list are not the game's
    std::lock_guard<std::mutex> lock(g.m);
    if (!g.inline_mode || g.latched || !g.device) return;
    // The existing MSAA-depth path changes raster state which this restore does
    // not replay. Keep it at its original boundary, as for resolved consumers.
    if(entry && g.depth_samples>1)return;
    // Match an actual operation even with unresolved SRVs; never recover at Close.
    if(!entry && g.depth_samples<=1 && g.entries.Ready() &&
       na::observe::ListLiveConsumer(l,color_id).op!=na::j2::Op::CopyRead)return;
    if (!na::observe::ListRendersTo(l, color_id, depth_id)) return; // the scene was not drawn in this recording
    const uint64_t rec = na::observe::ListRecording(l);
    if (!rec) return;
    uint32_t csub = 0, dsub = 0, cst = 0;
    if (!na::observe::ListSceneSubresource(l, color_id, depth_id, &csub, &dsub)) return;
    na_object_id cur_rt = 0, cur_ds = 0;
    na::observe::ListCurrentTargets(l, &cur_rt, &cur_ds);
    const bool reading = compute || cur_rt != color_id ||
                         (na::observe::ListLastStateSub(l, color_id, csub, &cst) && !(cst & D3D12_RESOURCE_STATE_RENDER_TARGET));
    if (!reading) return;
    ++g.stats.inline_candidates;
    if (g.inl_frame == g.frame || g.submitted_frame == g.frame) return; // either placement path already owns this frame
    const auto current = na::observe::ListInsertionDecision(l, color_id);
    const auto& learned = g.insertion_learned;
    // Role-class trigger (placement design): at the ACTUAL consumer (draw/dispatch/copy),
    // fire when its class is accepted by the profile. A class known to be a temporary
    // reader blocks the boundary matcher too (a fact from earlier frames). Otherwise the
    // boundary matcher below decides exactly as before, so coverage never drops below it.
    enum : uint32_t { kByClass = 1, kByPair = 2, kByBarrier = 3, kByLegacy = 4, kByEntry=6 };
    uint32_t matched_mode = kByLegacy;
    bool by_class = false;
    na::j2::ClassKey live_class;
    uint64_t live_producer = 0, live_context = 0;
    bool fired_at_barrier = false; // the event being judged is a barrier (whichever trigger fires)
    if(entry) {
        using V=na::j2::PostEntryProfile::Verdict;
        const auto verdict=g.entries.Judge(*entry);
        if(verdict==V::Accepted){by_class=true;matched_mode=kByEntry;if(g.entries.AcceptedByContext(*entry))++g.stats.entry_ctx_matched;}
        else {
            const char* why=verdict==V::Unknown?"new_or_unidentified":verdict==V::Learning?"learning":"seen_mid_scene";
            if(verdict==V::Unknown)++g.stats.entry_unknown;
            else if(verdict==V::Learning)++g.stats.entry_learning;
            else ++g.stats.entry_middle;
            // Prefer a new/learning key over ordinary intermediate entries in
            // the bounded last-miss packet. No change to admission or learning.
            if(!g.frame_rejects || verdict!=V::Middle)
                std::snprintf(g.frame_reject_text,sizeof g.frame_reject_text,
                    "entry=%s pso=%llu root=%llu dst=%llu kind=%u recording=%llu op=%llu barrier=%u",
                    why,(unsigned long long)entry->pipeline,(unsigned long long)entry->root,
                    (unsigned long long)entry->destination,static_cast<unsigned>(entry->op),
                    (unsigned long long)entry->recording,(unsigned long long)entry->operation,entry->barrier);
            ++g.frame_rejects;
            // Rescue (SE5 2026-11-05: every ~40 s the game merges its last two command lists for
            // a few seconds; the terminal entry is then an operation the profile has seen
            // mid-scene, so every entry is refused and ~40% of frames lose DLAA). The learned
            // scene boundary (same final writer, same occurrence, destination and pipeline) is
            // the same in both shapes: when it matches HERE, the scene is complete at this
            // operation, so the boundary matcher below places DLAA as it did before entries.
            // The learned scene boundary: the previous frame's shape, and every recent shape
            // that got DLAA (the game alternates shapes; see delivered_shapes).
            auto judge_shape=[&](const State::LearnedInsertion& L,bool& writer_only){
                writer_only=false;
                const bool same_target=L.decision.final_candidate && L.color==color_id && L.depth==depth_id &&
                    current.destination==L.decision.destination && current.pipeline==L.decision.pipeline;
                const bool exact=same_target &&
                    (L.decision.final_transition && L.decision.boundary_writer
                         ? current.boundary_writer==L.decision.boundary_writer && current.writer_occurrence==L.decision.writer_occurrence
                         : L.decision.final_transition ? current.final_transition==L.decision.final_transition
                                                       : current.candidates==L.decision.final_candidate);
                // SE5 p41768: the same scene end reached by a different final pass (only the
                // writer differs; occurrence, transition, destination and pipeline match).
                writer_only=!exact && same_target && L.decision.final_transition && L.decision.boundary_writer &&
                    current.boundary_writer && current.boundary_writer!=L.decision.boundary_writer &&
                    current.writer_occurrence==L.decision.writer_occurrence &&
                    current.final_transition==L.decision.final_transition && current.writes>0;
                return exact;
            };
            bool writer_only=false,by_memory=false;
            bool boundary=judge_shape(learned,writer_only);
            for(int k=0;k<g.delivered_shape_count&&!boundary&&!writer_only;++k){
                bool w=false;
                if(judge_shape(g.delivered_shapes[k],w)){boundary=true;by_memory=true;}
                else if(w){writer_only=true;by_memory=true;}
            }
            if(writer_only)++g.stats.entry_rescued_writer;
            if(by_memory&&(boundary||writer_only))++g.stats.entry_rescued_memory;
            if(!(boundary||writer_only) || g.inl.valid){
                // Name the first failing check (same order as the boundary matcher below).
                uint32_t f=8;
                if(!learned.decision.final_candidate||!learned.color)f=0;
                else if(learned.color!=color_id||learned.depth!=depth_id)f=1;
                else if(learned.decision.final_transition&&learned.decision.boundary_writer){
                    if(current.boundary_writer!=learned.decision.boundary_writer)f=2;
                    else if(current.writer_occurrence!=learned.decision.writer_occurrence)f=3;}
                else if(learned.decision.final_transition){if(current.final_transition!=learned.decision.final_transition)f=4;}
                else if(current.candidates!=learned.decision.final_candidate)f=5;
                if(f==8&&current.destination!=learned.decision.destination)f=6;
                if(f==8&&current.pipeline!=learned.decision.pipeline)f=7;
                ++g.stats.rescue_fail[f];
                std::snprintf(g.stats.rescue_note,sizeof g.stats.rescue_note,
                    "fail=%u learned{writer=%llu occ=%u tr=%u cand=%u dst=%llu pso=%llu} actual{writer=%llu occ=%u tr=%u cand=%u writes=%u dst=%llu pso=%llu} entry{pso=%llu op=%u}",
                    f,(unsigned long long)learned.decision.boundary_writer,learned.decision.writer_occurrence,learned.decision.final_transition,
                    learned.decision.final_candidate,(unsigned long long)learned.decision.destination,(unsigned long long)learned.decision.pipeline,
                    (unsigned long long)current.boundary_writer,current.writer_occurrence,current.final_transition,current.candidates,current.writes,
                    (unsigned long long)current.destination,(unsigned long long)current.pipeline,(unsigned long long)entry->pipeline,static_cast<unsigned>(entry->op));
                return;
            }
            ++g.stats.entry_rescued;
        }
    } else {
        const auto live = na::observe::ListLiveConsumer(l, color_id);
        // An MSAA depth resolve draws (render target, viewport, topology), which the state
        // restore does not cover: graphics consumers with MSAA depth stay on the boundary matcher.
        const bool raster_safe = live.op != na::j2::Op::GraphicsRead || g.depth_samples <= 1;
        if (live.barrier && live.after_write) {
            // Qualified scene-ending pattern: the drawing pipeline of the last write before
            // this barrier, plus (when that pipeline also appears mid-scene) the class of
            // the latest consumer earlier in this recording. Accepted -> fire here (boundary
            // matcher timing); known mid-scene -> veto; seen in both roles and not separated
            // by the context -> the boundary matcher is NOT allowed to fire here either; a
            // qualified consumer later in this recording may still take the frame.
            live_class.op = na::j2::Op::Barrier;
            live_producer = live.producer_pso;
            live_context = live.has_prior && g.scene_w ? na::j2::ClassHash(na::j2::MakeClass(live.prior, g.scene_w, g.scene_h)) : 0;
            fired_at_barrier = true;
            using V = na::j2::ConsumerProfile::Verdict;
            const V v = g.profile.Judge(live_class, live_producer, live_context);
            if (v == V::Rejected) { ++g.stats.barrier_vetoes; return; }
            // Ambiguous (the pattern was seen both mid-scene and at completion, and the
            // context does not separate them): the boundary matcher below is the
            // distinguishing check: it fires only when the writer pipeline, its
            // occurrence, destination and pipeline all equal the learned boundary. A
            // wrong fire is still a contradiction that demotes this pattern
            // (fired_at_barrier). An ambiguous pattern is not completion proof;
            // the between-list consumer path can also recover a missed boundary.
            if (v == V::Ambiguous) ++g.stats.barrier_ambiguous_skips;
            if (v == V::AcceptedWithProducer || v == V::AcceptedQualified) { by_class = true; matched_mode = kByBarrier; }
        } else if (live.valid && live.after_write && raster_safe && g.scene_w) {
            live_class = na::j2::MakeClass(live.raw, g.scene_w, g.scene_h);
            live_producer = live.producer_pso;
            using V = na::j2::ConsumerProfile::Verdict;
            const V v = g.profile.Judge(live_class, live_producer);
            if (v == V::Rejected) { ++g.stats.class_rejected_skips; return; }
            if (v == V::Accepted || v == V::AcceptedWithProducer) {
                by_class = true;
                matched_mode = v == V::Accepted ? kByClass : kByPair;
            } else {
                std::snprintf(g.stats.class_note, sizeof g.stats.class_note,
                    "%s class{op=%u root=%llu size=%u/%u fmt=%u srvs=%u/%u} producer=%llu",
                    v == V::Ambiguous ? "ambiguous" : "unknown", static_cast<unsigned>(live_class.op),
                    (unsigned long long)live_class.root, live_class.w_class, live_class.h_class, live_class.fmt,
                    live_class.color_srvs, live_class.view_srvs, (unsigned long long)live_producer);
            }
        }
    }
    if (by_class) {
        if (g.inl.valid) {
            ++g.stats.inline_waited;
            ++g.stats.inline_rejects[Stats::kRejPending];
            if (!g.frame_rejects++)
                std::snprintf(g.frame_reject_text, sizeof g.frame_reject_text, "class trigger: previous recording not submitted");
            return;
        }
        g.stats.inline_target = learned.decision.final_candidate;
    } else {
    // Optional passes change the absolute transition ordinal. Match the producer
    // of the learned final scene boundary, retaining an occurrence only among
    // boundaries from that same producer (not unrelated intermediate reads).
    // Same predicates as before, evaluated one by one so a rejection names its cause.
    using S = Stats;
    uint32_t rej = S::kRejCount;
    if (!learned.decision.final_candidate || !learned.color) rej = S::kRejNoLearned;
    else if (learned.color != color_id || learned.depth != depth_id) rej = S::kRejResource;
    else if (learned.decision.final_transition && learned.decision.boundary_writer) {
        if (current.boundary_writer != learned.decision.boundary_writer) rej = S::kRejWriter;
        else if (current.writer_occurrence != learned.decision.writer_occurrence) rej = S::kRejOccurrence;
    } else if (learned.decision.final_transition) {
        if (current.final_transition != learned.decision.final_transition) rej = S::kRejTransition;
    } else if (current.candidates != learned.decision.final_candidate) rej = S::kRejCandidate;
    if (rej == S::kRejCount && current.destination != learned.decision.destination) rej = S::kRejDestination;
    if (rej == S::kRejCount && current.pipeline != learned.decision.pipeline) rej = S::kRejPipeline;
    if (rej == S::kRejCount) g.stats.inline_target = learned.decision.final_candidate;
    if (rej == S::kRejCount && (g.inl_frame == g.frame || g.inl.valid)) {
        // Already inserted this frame (normal for later candidates) or the previous
        // recording was never submitted (a real stall): only the latter is a rejection.
        if (g.inl.valid && g.inl_frame != g.frame) rej = S::kRejPending;
        else return;
    }
    if (rej != S::kRejCount) {
        ++g.stats.inline_waited;
        ++g.stats.inline_rejects[rej];
        if (!g.frame_rejects++) {
            static const char* kWhy[] = {"no learned boundary", "different scene colour/depth", "different boundary writer",
                                         "different writer occurrence", "different transition ordinal", "different candidate ordinal",
                                         "different consumer destination", "different consumer pipeline", "previous recording not submitted"};
            std::snprintf(g.frame_reject_text, sizeof g.frame_reject_text,
                "reject=%s learned{writer=%llu occ=%u tr=%u cand=%u dst=%llu pso=%llu} actual{writer=%llu occ=%u tr=%u cand=%u dst=%llu pso=%llu}",
                kWhy[rej], (unsigned long long)learned.decision.boundary_writer, learned.decision.writer_occurrence,
                learned.decision.final_transition, learned.decision.final_candidate, (unsigned long long)learned.decision.destination,
                (unsigned long long)learned.decision.pipeline, (unsigned long long)current.boundary_writer, current.writer_occurrence,
                current.final_transition, current.candidates, (unsigned long long)current.destination, (unsigned long long)current.pipeline);
        }
        return;
    }
    } // legacy boundary matcher
    if (!na::observe::ListStateRestorable(l)) { ++g.stats.inline_refused; reason("The scene list's state cannot be restored after DLAA here (a bundle ran, or inside a render pass); left untouched."); return; }
    ID3D12CommandList* one[1] = {l};
    // The depth/earlier scene batches must already have established this frame's
    // raster sample. Apply that SAME sample to camera copies in this recording
    // before reading its camera and recording motion/NGX parameters.
    // Recording can precede ALL camera submissions. In that order establish the
    // sample here; Execute patches the earlier recorded pre-pass to this same
    // sample before forwarding it. Do not start late after an unpatched camera
    // batch has already been forwarded to the GPU.
    if ((g.jit.valid && g.jit.applied) || !g.inline_camera_submitted) jitter_batch(one, 1);
    else g.jit.valid = false;
    g.inline_sample_locked = true;
    PendingHistory pending;
    ID3D12GraphicsCommandList* out = nullptr;
    bool touched = false;
    const uint32_t slot = g.slot;
    g.capture_entry=entry?*entry:na::j2::InsertionEvent{};
    bool ok = record(one, 0, depth_id, color_id, &out, pending, l, &touched);
    if (!ok && window_frame_now()) { // projection window: never leave the clipped corner on screen
        na::capture::j2::Discard(pending.capture, true);
        pending = PendingHistory{};
        ok = record(one, 0, depth_id, color_id, &out, pending, l, &touched, nullptr, 0, nullptr, false, true);
        if (ok) ++g.stats.window_repeat_fallbacks;
    }
    g.capture_entry={};
    if (touched && !na::observe::RestoreListState(l)) {
        // Checked as restorable above; this cannot normally happen. Stop for the session.
        g.latched = true;
        na::capture::j2::Discard(pending.capture, true);
        reason("Stopped: the game's list state could not be restored after inline DLAA.");
        return;
    }
    if (!ok) { ++g.stats.inline_record_failed; na::capture::j2::Discard(pending.capture, true); return; } // the game's list still runs our copies
    g.slot = (g.slot + 1) % kRing;
    g.inl.list = l;
    g.inl.rec = rec;
    g.inl.frame = g.frame;
    g.inl.slot = slot;
    g.inl.writes = current.writes;
    g.inl.color = color_id;
    g.inl.history = pending;
    g.inl.valid = true;
    g.inl_frame = g.frame;
    ++g.stats.inline_recorded;
    if(matched_mode==kByEntry)++g.stats.entry_matched;
    else if (matched_mode == kByClass) ++g.stats.class_matched;
    else if (matched_mode == kByPair) ++g.stats.class_pair_matched;
    else if (matched_mode == kByBarrier) ++g.stats.barrier_matched;
    else ++g.stats.legacy_matched;
    // Remember what placed this frame's DLAA, so a later scene write demotes exactly that
    // pattern/class - including a boundary-matcher placement at a barrier.
    g.class_fired = matched_mode!=kByEntry && (by_class || fired_at_barrier);
    g.fired_mode = matched_mode;
    g.fired_barrier_event = fired_at_barrier;
    g.fired_class = live_class;
    g.fired_producer = live_producer;
    g.fired_context = live_context;
}

void on_table_bind(ID3D12GraphicsCommandList* l,const na_object_id* srvs,uint32_t n,bool compute) {
    on_table_bind_impl(l,srvs,n,compute);
}
void on_entry(ID3D12GraphicsCommandList* l,const na::j2::InsertionEvent& entry) {
    const bool compute=entry.op==na::j2::Op::ComputeEntry || entry.op==na::j2::Op::IndirectComputeEntry;
    on_table_bind_impl(l,&entry.resource,1,compute,&entry);
}

// Native engine control owns sizing; J2 only supplies reconstruction at the
// observed source->native-destination pass. Reuse record(), its state restore,
// history commit, capture and per-recording lifetime machinery in both engines.
class NativeScaleHook final:public na::observe::ShaderOperationHook {
    struct Candidate {
        uint64_t color=0,depth=0,dest=0,recording=0;uint32_t sub=0;
        ComPtr<ID3D12Resource> source,guide,target;
    };
public:
    na::observe::ShaderScope Before(ID3D12GraphicsCommandList* l,na::observe::ShaderCall& call) noexcept override {
        if(t_native_hint!=l||!t_native_budget)return {};
        if(!g_native_reduced.load(std::memory_order_relaxed)){t_native_hint=nullptr;return {};}
        try {
            if(na::observe::ListRecording(l)!=t_native_recording){t_native_hint=nullptr;return {};} // a reused list: stale hint
            --t_native_budget;g_nb.examined.fetch_add(1,std::memory_order_relaxed);
            const bool compute=call.kind==na::observe::ShaderKind::Dispatch;
            na::observe::NativeOperation op;const char* why=nullptr;
            if(!na::observe::ListNativeOperation(l,compute,op,&why)){native_reject(g_nb.snapshot,why?why:"The operation's bindings could not be read.");return {};}
            // This exact operation must read the scene colour (a later draw
            // that merely inherits the bind but samples something else is not
            // the upscale pass, and must not consume the hint either).
            bool reads=false;
            for(const auto& d:op.descriptors)reads|=d.used&&d.view.kind==NA_DESC_SRV&&d.view.resource==t_native_color;
            if(!reads){native_reject(g_nb.not_reading,"An operation after the scene bind does not read the scene colour.");return {};}
            NativeExtent extent;{std::lock_guard<std::mutex> lock(g_up_m);extent=g_native_extent;}
            auto c=std::make_shared<Candidate>();c->color=t_native_color;c->depth=t_native_depth;c->recording=op.recording;
            c->source.Attach(na::observe::AcquireResource(c->color));c->guide.Attach(na::observe::AcquireResource(c->depth));
            if(!c->source||!c->guide){native_reject(g_nb.snapshot,"The scene colour or depth has retired.");return {};}
            const auto source=c->source->GetDesc();
            {std::lock_guard<std::mutex> lock(g_nb.m);g_nb.src_w=uint32_t(source.Width);g_nb.src_h=source.Height;g_nb.src_fmt=uint32_t(source.Format);}
            if(source.Width!=extent.rw||source.Height!=extent.rh||source.DepthOrArraySize!=1||source.MipLevels!=1||source.SampleDesc.Count!=1){
                // Logged with the sizes: an engine that renders a sub-rectangle
                // of a full allocation shows up here (see the audit document).
                native_reject(g_nb.source_size,"The scene colour is not exactly the game's reported scene size.");return {};
            }
            uint32_t full_seen=0;
            const auto match=[&](const na::observe::OperationDescriptor& d){
                if(!d.resource||d.view.resource==c->color||!d.used)return;
                const auto target=d.resource->GetDesc();
                if(target.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||target.Width!=extent.ow||target.Height!=extent.oh)return;
                ++full_seen;{std::lock_guard<std::mutex> lock(g_nb.m);g_nb.seen_w=uint32_t(target.Width);g_nb.seen_h=target.Height;g_nb.seen_fmt=uint32_t(target.Format);}
                if(target.SampleDesc.Count!=1||target.DepthOrArraySize!=1||target.MipLevels!=1||typed(target.Format)!=typed(source.Format))return;
                if(c->dest&&c->dest!=d.view.resource){c->dest=~0ull;return;}
                c->dest=d.view.resource;c->target=d.resource;
            };
            if(compute){for(const auto& d:op.descriptors)if(d.view.kind==NA_DESC_UAV)match(d);}
            else {
                na::observe::DrawInfo draw;
                // Several render targets are allowed (an upscale pass that also
                // writes history/luminance): exactly one full-output target of
                // the scene's format must exist, and nothing may be blended.
                if(!na::observe::ListDrawState(l,draw)||!draw.pipeline_known||draw.depth_enable!=0||draw.rt_count<1||draw.blend_mask!=0){
                    native_reject(g_nb.draw_state,"The pass reading the scene uses depth, blending or unknown pipeline state.");return {};}
                if(op.viewports.empty()||op.viewports[0].TopLeftX!=0||op.viewports[0].TopLeftY!=0||
                   op.viewports[0].Width!=float(extent.ow)||op.viewports[0].Height!=float(extent.oh)){
                    native_reject(g_nb.viewport,"The pass reading the scene does not cover the full output.");return {};}
                for(const auto& sc:op.scissors)if(sc.left>0||sc.top>0||sc.right<LONG(extent.ow)||sc.bottom<LONG(extent.oh)){
                    native_reject(g_nb.viewport,"The pass reading the scene is clipped to part of the output.");return {};}
                for(const auto& d:op.colors)match(d);
            }
            if(c->dest==~0ull){native_reject(g_nb.ambiguous,"The pass writes several full-size targets of the scene format.");return {};}
            if(!c->dest){native_reject(g_nb.no_destination,full_seen?"The full-size output of the pass has another format than the scene (for example after tone mapping).":
                "The pass reading the scene writes nothing at full output size.");return {};}
            t_native_hint=nullptr;g_nb.candidates.fetch_add(1,std::memory_order_relaxed);
            return {na::observe::ShaderAction::Forward,c};
        }catch(...){return {};}
    }
    void After(ID3D12GraphicsCommandList* l,const na::observe::ShaderCall&,na::observe::ShaderScope& scope) noexcept override {
        auto c=std::static_pointer_cast<Candidate>(scope.state);if(!c)return;
        try {
            std::lock_guard<std::mutex> lock(g.m);
            const auto skip=[](const char* why){native_reject(g_nb.after_skipped,why);};
            if(g.latched||g.paused.load()||!g.device||!g.dlss_ready){skip("DLSS is not ready (paused, stopped or still starting).");return;}
            if(g.inl.valid||g.submitted_frame==g.frame)return; // this frame already has its DLSS
            if(na::observe::ListRecording(l)!=c->recording||!na::observe::ListStateRestorable(l)){skip("The game's state at the upscale pass cannot be restored.");return;}
            uint64_t depth=0,color=0;na::observe::GetLiveSelection(&depth,&color);
            if(color!=c->color||depth!=c->depth){skip("The scene selection changed during the frame.");return;}
            ID3D12CommandList* one[]{l};SceneInput saved;
            const bool local=na::observe::ListRendersTo(l,color,depth);
            if(!local){saved=g.native_scene;if(saved.frame!=g.frame||saved.color!=color||saved.depth!=depth){skip("The scene list's camera was not kept for this upscale pass.");return;}}
            if(local&&(!g.inline_camera_submitted||(g.jit.valid&&g.jit.applied)))jitter_batch(one,1);
            g.inline_sample_locked=true;
            const uint32_t slot=g.slot;PendingHistory pending;ID3D12GraphicsCommandList* out=nullptr;bool touched=false;
            auto owned=std::make_shared<std::vector<ComPtr<ID3D12Resource>>>();owned->reserve(16);
            *owned={c->source,c->guide,c->target};
            auto lease=na::nr::native::RetainRecording(l,owned);if(!lease)return;
            UpscaleTarget previous;
            {std::lock_guard<std::mutex> up(g_up_m);previous=g_up;g_up={c->dest,c->sub,0,0};}
            struct RestoreTarget{UpscaleTarget old;~RestoreTarget(){std::lock_guard<std::mutex> up(g_up_m);g_up=old;}} restore_target{previous};
            const bool ok=record(one,0,depth,color,&out,pending,l,&touched,local?nullptr:&saved);
            // The same existing recording lease handles submit, Reset/discard,
            // pooled-list reuse and completion. Include resources allocated by
            // record() even when it only recorded a partial failed attempt.
            if(touched){
                *owned={c->source,c->guide,c->target,g.color_in,g.depth32,g.motion,g.output,g.sharp,g.provenance,g.jshift};
                g.slots[slot].native_scale_use=lease;
            }
            if(touched&&!na::observe::RestoreListState(l)){g.latched=true;reason("Stopped: native-scale pass could not restore game state.");return;}
            if(!ok){na::capture::j2::Discard(pending.capture,true);++g.stats.inline_record_failed;
                g_nb.record_failed.fetch_add(1,std::memory_order_relaxed);
                {std::lock_guard<std::mutex> nb(g_nb.m);std::snprintf(g_nb.reason,sizeof g_nb.reason,"DLSS recording at the upscale pass failed: %s",g.stats.reason);}
                return;}
            g_nb.recorded.fetch_add(1,std::memory_order_relaxed);
            {std::lock_guard<std::mutex> nb(g_nb.m);std::snprintf(g_nb.reason,sizeof g_nb.reason,"%s","DLSS reconstructs at the game's own upscale pass.");}
            g.slot=(slot+1)%kRing;g.inl.list=l;g.inl.rec=c->recording;g.inl.frame=g.frame;g.inl.slot=slot;
            g.inl.writes=na::observe::ListInsertionDecision(l,color).writes;g.inl.color=color;g.inl.history=pending;g.inl.valid=true;g.inl_frame=g.frame;
            ++g.stats.inline_recorded;g.inline_mode=true;
        }catch(...){na::observe::RestoreListState(l);std::lock_guard<std::mutex> lock(g.m);g.latched=true;reason("Stopped: native-scale recording failed.");}
    }
    void Closing(ID3D12GraphicsCommandList* l) noexcept override {
        if(!g_native_reduced.load(std::memory_order_relaxed)||g.paused.load())return;
        try{
            uint64_t depth=0,color=0;na::observe::GetLiveSelection(&depth,&color);
            if(!color||!depth||!na::observe::ListRendersTo(l,color,depth))return;
            std::lock_guard<std::mutex> lock(g.m);ID3D12CommandList* one[]{l};
            if(g.latched||!g.device)return;
            // No rendering or GPU readback at Close. Preserve the actual scene
            // camera/sample for a separate post-processing recording, using J1.
            if(!g.inline_camera_submitted||(g.jit.valid&&g.jit.applied))jitter_batch(one,1);
            SceneInput saved;saved.frame=g.frame;saved.color=color;saved.depth=depth;
            if(!na::observe::ListSceneSubresource(l,color,depth,&saved.color_sub,&saved.depth_sub))return;
            saved.native_states=na::observe::ListStateSub(l,color,saved.color_sub,&saved.color_state)==na::observe::StateLookup::Known&&
                na::observe::ListStateSub(l,depth,saved.depth_sub,&saved.depth_state)==na::observe::StateLookup::Known;
            saved.has_camera=find_camera(one,0,depth,&saved.vp,&saved.reversed,&saved.interlopers);
            saved.judged=saved.has_camera;
            saved.injected=g_injected;saved.jitter[0]=g_injected?g.jit.px[0]:0;saved.jitter[1]=g_injected?g.jit.px[1]:0;
            g.native_scene=saved;
        }catch(...){}
    }
};

// Inline and between-list are complementary opportunities in the SAME frame.
// Only an actual, learned post-entry consumer can trigger this fallback. No
// previous-frame batch ordinal and no guess about the final drawing PSO is used.
int record_between_consumer(ID3D12CommandList* const* lists, UINT count,
                            uint64_t color, uint64_t depth,
                            ID3D12GraphicsCommandList** ours, PendingHistory& pending)
{
    auto& saved = g.waiting_scene;
    if (saved.frame != g.frame || saved.color != color || saved.depth != depth) saved = {};
    for (UINT i = 0; i < count; ++i) {
        if (!lists[i]) continue;
        std::vector<na::j2::ColorEvent> events;
        if (!na::observe::ListColorEvents(lists[i], color, events)) { saved = {}; continue; }
        bool writes = false, consumed = false;
        uint64_t producer = 0;
        for (const auto& e : events) {
            if (e.write) { writes = true; consumed = false; producer = e.producer; continue; }
            const bool entry = na::j2::IsEntry(e.op);
            // Inline needs a scene draw in its own recording. A learned entry
            // in a pure post list instead uses this submitted-scene handoff.
            // Unqualified entries are not evidence of a colour read.
            if (entry ? !g.entries.Accepts(e.raw) : !na::j2::IsConsumer(e.op)) continue;
            if (!writes && !consumed && saved.frame == g.frame) {
                const auto cls = na::j2::MakeClass(e.raw, g.scene_w, g.scene_h);
                const auto verdict = g.profile.Judge(cls, saved.producer);
                using V = na::j2::ConsumerProfile::Verdict;
                bool recorded = false;
                if(entry || verdict == V::Accepted || verdict == V::AcceptedWithProducer){
                    g.capture_entry=entry?e.raw:na::j2::InsertionEvent{};
                    recorded=record(lists, 0, depth, color, ours, pending, nullptr, nullptr, &saved, i);
                    g.capture_entry={};
                }
                if (recorded) {
                    g.class_fired = !entry;
                    g.fired_mode = entry ? 6 : 5;
                    if(entry){++g.stats.entry_matched;if(g.entries.AcceptedByContext(e.raw))++g.stats.entry_ctx_matched;}
                    g.fired_barrier_event = false;
                    g.fired_class = cls;
                    g.fired_producer = saved.producer;
                    g.fired_context = 0;
                    saved = {};
                    return static_cast<int>(i);
                }
            }
            consumed = true;
            saved = {}; // never insert later after a consumer already carried the image onward
        }
        if (writes) {
            saved = {};
            // End-of-list metadata is useful only if its final image has not
            // already been consumed within that recording.
            if (!consumed && na::observe::ListRendersTo(lists[i], color, depth) &&
                na::observe::ListSceneSubresource(lists[i], color, depth, &saved.color_sub, &saved.depth_sub) &&
                find_camera(lists, i, depth, &saved.vp, &saved.reversed, &saved.interlopers)) {
                saved.judged = true;
                saved.frame = g.frame; saved.color = color; saved.depth = depth; saved.producer = producer;
                saved.jitter[0] = g_injected ? g.jit.px[0] : 0;
                saved.jitter[1] = g_injected ? g.jit.px[1] : 0;
                saved.injected = g_injected;
            }
        }
    }
    return -1;
}

// ---- Render-small: PresentUpscale's own ring and the bilinear stretch ---------------------
// Its own allocators and fence (on the present queue), separate from J2's scene ring, so a
// present never waits on, or reuses, a scene slot. The stretch is the picture for frames
// without this frame's depth + motion: never a blank or render-size corner.
struct SmallRing {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc[kRing];
    UINT64 value[kRing] = {};
    ComPtr<ID3D12GraphicsCommandList> list;
    std::shared_ptr<na::gpu::Recording> owned;
    bool owned_recording = false;
    ComPtr<ID3D12Fence> fence;
    UINT64 next = 1;
    uint32_t slot = 0;
    std::shared_ptr<na::donut::Frame> grade[kRing];
    std::shared_ptr<void> hud_owner[kRing];
    std::shared_ptr<na::j2::native_hud::Picture> hud_picture[kRing];
    struct HudAssociation{uint64_t source=0,generation=0;} hud_association[kRing];
    bool native_hud_result[kRing]{};
    hud_routes::Ticket hud_ticket[kRing];
    struct CanvasProbe {
        std::shared_ptr<na::j2::native_hud::Picture> picture;
        std::shared_ptr<void> owner;
        uint64_t source=0,generation=0;
    } canvas_probe[kRing];
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12DescriptorHeap> heap; // kRing x (SRV, UAV)
    UINT inc = 0;
    bool tried = false, ok = false;
    ComPtr<ID3D12Resource> sharp; // RCAS target at output size (UNORDERED_ACCESS between uses)
    uint32_t sharp_w = 0, sharp_h = 0;
    DXGI_FORMAT sharp_f = DXGI_FORMAT_UNKNOWN;
    // HUD separation (render-small): which pixels are the game's HUD, kept out of DLSS and
    // upscaled by NIS instead. prev = last presented small frame (NON_PIXEL_SHADER_RESOURCE),
    // hud = per-pixel evidence 0..1 (R32F, UAV), mask = its 3x3 dilation (R16F, UAV between
    // uses), hudup = NIS-upscaled frame at output size (RGBA16F, UAV between uses).
    bool hud_tried = false, hud_ok = false;
    ComPtr<ID3D12RootSignature> hud_root;
    ComPtr<ID3D12PipelineState> hud_pso[9]; // classify, dilate, composite, clear, layer (native HUD), witness, wclear, unblend, uclear
    // Jitter witness: the last DLSS frame (render size), its declared jitter and J2 frame, and
    // a readback of the counts per ring slot (read when the slot comes round again).
    ComPtr<ID3D12Resource> wprev, wread[kRing];
    bool wread_valid[kRing] = {};
    na::jitter::Source wread_source[kRing]{};
    uint64_t wread_frame[kRing]{};
    na::jitter::ColourDomain final_jitter{na::jitter::kStageFinal};
    uint64_t applied_jitter_epoch = 0;
    uint32_t wprev_w = 0, wprev_h = 0;
    DXGI_FORMAT wprev_f = DXGI_FORMAT_UNKNOWN;
    bool wprev_valid = false;
    uint64_t wprev_frame = 0;
    float wjx = 0.f, wjy = 0.f, last_jx = 0.f, last_jy = 0.f; // raster sample; independent of final colour's calibrated declaration
    uint64_t w_frames = 0, w_tested = 0, w_unshifted = 0, w_shifted = 0; // the current window
    uint64_t w_opposite = 0;
    uint32_t w_unseen_windows = 0;
    ComPtr<ID3D12DescriptorHeap> hud_heap;  // kRing x 5 passes x (3 SRV, 4 UAV)
    ComPtr<ID3D12Resource> prev, hud, mask, hudup;
    ComPtr<ID3D12Resource> jshift; // jitter-shifted colour at render size (UNORDERED_ACCESS between uses)
    ComPtr<ID3D12Resource> hud_stats; // 4 uints: per frame parity, detailed pixels and still ones
    uint32_t hud_parity = 0;
    uint32_t hud_w = 0, hud_h = 0, up_w = 0, up_h = 0;
    DXGI_FORMAT prev_f = DXGI_FORMAT_UNKNOWN;
    bool prev_valid = false;
    bool hud_fresh = true; // a new picture size: start the evidence over at the next classify
    ComPtr<ID3D12Resource> hread[kRing]; // HUD fallback counts per ring slot (read when the slot comes round)
    bool hread_valid[kRing] = {};
    // hud_unblend: the scene with the UI's contribution removed (render size, UNORDERED_ACCESS
    // between uses) and the self-check counts per ring slot.
    ComPtr<ID3D12Resource> ublend, uread[kRing];
    bool uread_valid[kRing] = {};
    uint32_t ublend_w = 0, ublend_h = 0;
    DXGI_FORMAT ublend_f = DXGI_FORMAT_UNKNOWN;
    bool hud_protect_off = false;        // last completed mask covered too much: learn, do not apply
} g_small;
na::j2::NisScaler g_hud_nis;
std::atomic<bool> g_hud_view{false}; // tint HUD pixels (menu: Show HUD mask)
std::atomic<bool> g_hud_mask_on{false}; // [present] hud_mask=1: the pixel-guessing HUD mask (off: it misread scenery in Anno)
constexpr char kHudHlsl[] = R"(
Texture2D<float4> t0 : register(t0);
Texture2D<float4> t1 : register(t1);
Texture2D<float4> t2 : register(t2);
RWTexture2D<float4> u0 : register(u0);
RWTexture2D<float> u1 : register(u1);
RWTexture2D<float> u2 : register(u2);
RWByteAddressBuffer stats : register(u3);
SamplerState s_lin : register(s0);
// flags: 1 = tint the HUD (composite), 2 = the camera is jittered this frame, 4 = forget old evidence. cur = parity.
cbuffer C : register(b0) { uint w; uint h; uint ow; uint oh; uint flags; uint cur; uint p1; uint p2; };
groupshared uint gs_detail, gs_still;
float luma(float3 c) { return dot(min(c, 1.0), float3(0.299, 0.587, 0.114)); }
int2 at(int2 p, int2 d) { return clamp(p + d, int2(0, 0), int2(int(w) - 1, int(h) - 1)); }
// HUD protection fallback (2026-11-04, Cort: two independent HUD methods; Astra 20cddb8/3dd71ab:
// motion contradiction, not plain stillness; learned over seconds; protection, not separation).
// A HUD pixel is anchored to the screen; the world under it moves with our motion vectors. Only
// pixels where the camera vector says the world moves (>= 1.5 render px) and the picture has
// detail teach anything, so a still camera, the sky and flat areas learn nothing:
//   * screen-fixed (same as last frame at the same place) AND not where the motion says the world
//     came from: HUD evidence, +1/48 per frame (about a second of real movement at 50 fps);
//   * follows the motion and not screen-fixed: world evidence, -1/12 per frame; a large change at
//     the same place (any motion): -0.5;
//   * anything else (a changing counter, a mover, a disocclusion): no change.
// The evidence persists across camera cuts (a HUD survives them); only a new picture size clears it.
// t0 this frame, t1 the previous frame, t2 motion (current -> previous, render pixels); u1 evidence;
// stats: byte 52 + 8*cur = detailed moving pixels tested this frame.
float maxdiff(float3 a, float3 b) { const float3 d = abs(min(a, 1.0) - min(b, 1.0)); return max(d.r, max(d.g, d.b)); }
[numthreads(8, 8, 1)]
void classify(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    if (gi == 0) { gs_detail = 0; gs_still = 0; }
    GroupMemoryBarrierWithGroupSync();
    if (id.x < w && id.y < h) {
        const int2 p = int2(id.xy);
        float lo = 2.0, hi = -1.0;
        [unroll] for (int y = -1; y <= 1; ++y)
            [unroll] for (int x = -1; x <= 1; ++x) { const float l = luma(t0[at(p, int2(x, y))].rgb); lo = min(lo, l); hi = max(hi, l); }
        const float2 m = t2[p].xy;
        float e = (flags & 4u) != 0u ? 0.0 : u1[p];
        const float3 c = t0[p].rgb;
        const float still = maxdiff(c, t1[p].rgb);
        // A large change at a fixed place, moving camera or not: not a still overlay now (Sol HUD-R2:
        // a HUD element that disappears over a flat background must not stay HUD).
        if (still > 0.15) e -= 0.5;
        else if (all(abs(m) < 512.0) && dot(m, m) >= 2.25) {
            const float2 uv = (float2(p) + 0.5 + m) / float2(w, h);
            const bool inside = all(uv > 0.0) && all(uv < 1.0);
            const float follow = inside ? maxdiff(c, t1.SampleLevel(s_lin, uv, 0).rgb) : 1.0;
            if (hi - lo > 0.06) {
                uint dummy;
                InterlockedAdd(gs_detail, 1u, dummy);
                if (still < 0.02 && inside && follow > max(0.06, 3.0 * still)) e += 1.0 / 48.0;
                else if (inside && follow < 0.03 && still > 0.06) e -= 1.0 / 12.0;
            }
        }
        u1[p] = saturate(e);
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0) stats.InterlockedAdd(52u + cur * 8u, gs_detail);
}
// Zeroes this frame's counts before classify.
[numthreads(1, 1, 1)]
void clear(uint3 id : SV_DispatchThreadID) { stats.Store2(cur * 8u, uint2(0, 0)); stats.Store2(48u + cur * 8u, uint2(0, 0)); }
// Jitter witness (render size): t0 this frame, t1 the previous DLSS frame, t2 motion. On still,
// detailed pixels, does this frame match the previous one unshifted, or shifted by the declared
// jitter difference (asfloat(p1), asfloat(p2) render pixels; both signs tried)? Counts at byte 32:
// tested, unshifted wins, shifted wins.
float diff3(float3 a, float3 b) { const float3 d = abs(min(a, 1.0) - min(b, 1.0)); return d.r + d.g + d.b; }
[numthreads(8, 8, 1)]
void witness(uint3 id : SV_DispatchThreadID) {
    if (id.x + 2 >= w || id.y + 2 >= h || id.x < 2 || id.y < 2) return;
    const int2 p = int2(id.xy);
    const float2 m = t2[p].xy;
    float lo = 2.0, hi = -1.0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x) { const float l = luma(t0[p + int2(x, y)].rgb); lo = min(lo, l); hi = max(hi, l); }
    if (hi - lo < 0.1) return;   // flat: says nothing about sub-pixel shifts
    const float2 dj = float2(asfloat(p1), asfloat(p2));
    const float2 uv = (float2(p) + 0.5) / float2(w, h), du = dj / float2(w, h);
    const float3 c = t0[p].rgb;
    uint dummy;
    stats.InterlockedAdd(32u, 1u, dummy);
    if (dot(m, m) > 1.0) return; // ambiguous world counts; it must not leave only HUD votes
    const float e0 = diff3(c, t1[p].rgb);
    // dj = current raster sample - previous. Separate the two signs: a
    // shifted match alone must not qualify an arbitrary jitter convention.
    const float ej = diff3(c, t1.SampleLevel(s_lin, uv - du, 0).rgb);
    const float eo = diff3(c, t1.SampleLevel(s_lin, uv + du, 0).rgb);
    if (min(e0, min(ej, eo)) > 0.06) return; // moving/unexplained colour is not calibration evidence
    if (e0 * 2.0 < min(ej, eo)) stats.InterlockedAdd(36u, 1u, dummy);
    else if (ej * 2.0 < min(e0, eo)) stats.InterlockedAdd(40u, 1u, dummy);
    else if (eo * 2.0 < min(e0, ej)) stats.InterlockedAdd(44u, 1u, dummy);
}
[numthreads(1, 1, 1)]
void wclear(uint3 id : SV_DispatchThreadID) { stats.Store4(32u, uint4(0, 0, 0, 0)); }
// u1 evidence -> u2 mask (3x3 max, so text edges blended with the scene count too), with
// hysteresis on last frame's mask (on above 0.65, off below 0.3), so a pixel near the threshold
// never flickers. Inside the mask the motion DLSS reads is zero (u0) unless flags 8 (protection
// off this frame: the mask is learned but not applied). stats: byte 48 + 8*cur = mask pixels.
[numthreads(8, 8, 1)]
void dilate(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    if (gi == 0) gs_still = 0;
    GroupMemoryBarrierWithGroupSync();
    if (id.x < w && id.y < h) {
        const int2 p = int2(id.xy);
        float m = 0.0;
        [unroll] for (int y = -1; y <= 1; ++y)
            [unroll] for (int x = -1; x <= 1; ++x) m = max(m, u1[at(p, int2(x, y))]);
        const float was = (flags & 4u) != 0u ? 0.0 : u2[p];
        const bool on = was > 0.5 ? m > 0.3 : m > 0.65;
        u2[p] = on ? 1.0 : 0.0;
        if (on && (flags & 8u) == 0u) u0[p] = float4(0, 0, 0, 0);
        if (on) { uint dummy; InterlockedAdd(gs_still, 1u, dummy); }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0) stats.InterlockedAdd(48u + cur * 8u, gs_still);
}
// Output size: t0 the DLSS (+RCAS) picture, t1 the NIS-upscaled frame, t2 the mask; u0 result.
[numthreads(8, 8, 1)]
void composite(uint3 id : SV_DispatchThreadID) {
    if (id.x >= ow || id.y >= oh) return;
    const float2 uv = (float2(id.xy) + 0.5) / float2(ow, oh);
    const float m = smoothstep(0.35, 0.65, t2.SampleLevel(s_lin, uv, 0).r);
    const float4 s = t0[id.xy];
    float3 o = lerp(s.rgb, t1[id.xy].rgb, m);
    if (flags & 1u) o = lerp(o, float3(1, 0, 1), 0.5 * m);
    u0[id.xy] = float4(o, s.a);
}
// Native HUD (native_hud.hpp): t0 the DLSS (+RCAS) picture, t1 the HUD drawn over black, t2 the
// HUD drawn over grey (asfloat(p1), as read through t1/t2), all output size; u0 result.
// Per channel the game's blending is out = a + dst * b: a = t1, b = (t2 - t1) / grey.
// flags: 1 = tint where there is HUD, 4 = the HUD was drawn through an sRGB view (blend in
// linear; the picture is stored encoded).
float3 srgb_to_linear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float3 linear_to_srgb(float3 c) { c = saturate(c); return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }
[numthreads(8, 8, 1)]
void layer(uint3 id : SV_DispatchThreadID) {
    if (id.x >= ow || id.y >= oh) return;
    const float4 s = t0[id.xy];
    const float3 d = (flags & 4u) ? srgb_to_linear(s.rgb) : s.rgb;
    const float3 a = t1[id.xy].rgb;
    const float3 b = saturate((t2[id.xy].rgb - a) / asfloat(p1));
    float3 o = a + d * b;
    if (flags & 1u) {
        const float hud = saturate(max(1.0 - min(b.r, min(b.g, b.b)), max(a.r, max(a.g, a.b)) * 8.0));
        o = lerp(o, float3(1, 0, 1), 0.5 * hud);
    }
    if (flags & 4u) o = linear_to_srgb(o);
    u0[id.xy] = float4(o, s.a);
}
// HUD by un-blending (hud_unblend.hpp): t0 the game's final picture, t1 the UI replayed over
// black and t2 over grey, all at the game's own size; u0 the scene with the UI removed:
// P = L0 + m * S, m = (L1 - L0) / grey, so S = (P - L0) / m where the UI left anything of the
// destination (m > 0.04); a fully covered pixel keeps P (the native layer covers it anyway).
// flags 4 = the UI was drawn through an sRGB view (blend in linear; the picture is encoded).
// Self-check (stats bytes 64..): UI pixels, fully covered pixels, and of those how many equal
// L0 as stored / sRGB-decoded / sRGB-encoded within 2.5/255 (which transform the game's final
// pass applied, if any). Group-summed, one atomic per counter per group.
groupshared uint gs_ub[5];
float maxdiff3(float3 a, float3 b) { const float3 d = abs(a - b); return max(d.r, max(d.g, d.b)); }
[numthreads(8, 8, 1)]
void unblend(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    if (gi < 5) gs_ub[gi] = 0;
    GroupMemoryBarrierWithGroupSync();
    if (id.x < w && id.y < h) {
        const float4 s = t0[id.xy];
        const bool lin = (flags & 4u) != 0;
        const float3 p = lin ? srgb_to_linear(s.rgb) : s.rgb;
        const float3 a = t1[id.xy].rgb;
        const float3 m = saturate((t2[id.xy].rgb - a) / asfloat(p1));
        const float3 rec = saturate((p - a) / max(m, 0.04));
        const float3 o = (m > 0.04) ? rec : p;
        u0[id.xy] = float4(lin ? linear_to_srgb(o) : o, s.a);
        const float mmax = max(m.r, max(m.g, m.b));
        uint dummy;
        if (mmax < 0.996) InterlockedAdd(gs_ub[0], 1u, dummy);
        if (mmax < 0.008) {
            InterlockedAdd(gs_ub[1], 1u, dummy);
            const float tol = 2.5 / 255.0;
            if (maxdiff3(p, a) <= tol) InterlockedAdd(gs_ub[2], 1u, dummy);
            if (maxdiff3(srgb_to_linear(p), a) <= tol) InterlockedAdd(gs_ub[3], 1u, dummy);
            if (maxdiff3(linear_to_srgb(p), a) <= tol) InterlockedAdd(gs_ub[4], 1u, dummy);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi < 5 && gs_ub[gi]) { uint dummy; stats.InterlockedAdd(64u + gi * 4u, gs_ub[gi], dummy); }
}
[numthreads(1, 1, 1)]
void uclear(uint3 id : SV_DispatchThreadID) { stats.Store4(64u, uint4(0, 0, 0, 0)); stats.Store(80u, 0u); }
)";
bool hud_init()
{
    if (g_small.hud_tried) return g_small.hud_ok;
    g_small.hud_tried = true;
    ID3D12Device* dev = g_small.device.Get();
    D3D12_DESCRIPTOR_RANGE r[2]{};
    r[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    r[0].NumDescriptors = 3;
    r[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    r[1].NumDescriptors = 4;
    r[1].OffsetInDescriptorsFromTableStart = 3;
    D3D12_ROOT_PARAMETER p[2]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[0].DescriptorTable.NumDescriptorRanges = 2;
    p[0].DescriptorTable.pDescriptorRanges = r;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[1].Constants.Num32BitValues = 8;
    D3D12_STATIC_SAMPLER_DESC ss{};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.MaxLOD = D3D12_FLOAT32_MAX;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rd{2, p, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
        FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_small.hud_root))))
        return false;
    const char* entries[9] = {"classify", "dilate", "composite", "clear", "layer", "witness", "wclear", "unblend", "uclear"};
    for (int i = 0; i < 9; ++i) {
        ComPtr<ID3DBlob> code, cerr;
        if (FAILED(D3DCompile(kHudHlsl, sizeof(kHudHlsl) - 1, "j2_small_hud.hlsl", nullptr, nullptr, entries[i], "cs_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &cerr)))
            return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g_small.hud_root.Get();
        pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
        if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_small.hud_pso[i])))) return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kRing * 63, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_small.hud_heap)))) return false;
    D3D12_HEAP_PROPERTIES bhp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 256;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(dev->CreateCommittedResource(&bhp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                            IID_PPV_ARGS(&g_small.hud_stats))))
        return false;
    g_small.hud_ok = true;
    return true;
}
// Render-size and output-size targets for the HUD passes; false = no HUD pass this frame.
bool hud_targets(ID3D12Resource* frame, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh)
{
    ID3D12Device* dev = g_small.device.Get();
    D3D12_RESOURCE_DESC fd = frame->GetDesc();
    if (!g_small.prev || g_small.hud_w != rw || g_small.hud_h != rh || g_small.prev_f != fd.Format) {
        g_small.prev.Reset(); g_small.hud.Reset(); g_small.mask.Reset();
        g_small.prev_valid = false; g_small.hud_fresh = true;
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC pd = fd;
        pd.Flags = D3D12_RESOURCE_FLAG_NONE;
        pd.MipLevels = 1;
        pd.DepthOrArraySize = 1;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &pd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                                IID_PPV_ARGS(&g_small.prev))) ||
            !texture(rw, rh, DXGI_FORMAT_R32_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g_small.hud) ||
            !texture(rw, rh, DXGI_FORMAT_R16_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g_small.mask)) {
            g_small.prev.Reset(); g_small.hud.Reset(); g_small.mask.Reset();
            return false;
        }
        g_small.hud_w = rw; g_small.hud_h = rh; g_small.prev_f = fd.Format;
    }
    if (!g_small.hread[0]) {
        D3D12_HEAP_PROPERTIES rp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = 256; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        for (uint32_t i = 0; i < kRing; ++i)
            if (FAILED(dev->CreateCommittedResource(&rp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_small.hread[i])))) {
                for (auto& r : g_small.hread) r.Reset();
                return false;
            }
    }
    if (!g_small.hudup || g_small.up_w != ow || g_small.up_h != oh) {
        if (!texture(ow, oh, DXGI_FORMAT_R16G16B16A16_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g_small.hudup)) return false;
        g_small.up_w = ow; g_small.up_h = oh;
    }
    return true;
}
// One HUD pass: SRVs t0..t2 and UAVs u0..u2 (nullptr = a null view), 8 constants.
void hud_pass(ID3D12GraphicsCommandList* l, int pass, uint32_t slot, ID3D12Resource* const srv[3], const DXGI_FORMAT sf[3],
              ID3D12Resource* const uav[3], const DXGI_FORMAT uf[3], const uint32_t k[8], uint32_t gx, uint32_t gy)
{
    const na::diag::crumbs::Scope crumb(l, "NeuralAlias HUD pass");
    ID3D12Device* dev = g_small.device.Get();
    const UINT base = (slot * 9 + static_cast<UINT>(pass)) * 7;
    D3D12_CPU_DESCRIPTOR_HANDLE c = g_small.hud_heap->GetCPUDescriptorHandleForHeapStart();
    c.ptr += static_cast<SIZE_T>(base) * g_small.inc;
    for (int i = 0; i < 3; ++i, c.ptr += g_small.inc) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = srv[i] ? sf[i] : DXGI_FORMAT_R8G8B8A8_UNORM;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(srv[i], &sv, c);
    }
    for (int i = 0; i < 3; ++i, c.ptr += g_small.inc) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
        uv.Format = uav[i] ? uf[i] : (i == 0 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R32_FLOAT);
        uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        dev->CreateUnorderedAccessView(uav[i], nullptr, &uv, c);
    }
    {   // u3: the counts buffer (raw)
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
        uv.Format = DXGI_FORMAT_R32_TYPELESS;
        uv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uv.Buffer.NumElements = 64; // 256 bytes: HUD counts at 0, jitter witness at 32
        uv.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        dev->CreateUnorderedAccessView(g_small.hud_stats.Get(), nullptr, &uv, c);
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gh = g_small.hud_heap->GetGPUDescriptorHandleForHeapStart();
    gh.ptr += static_cast<UINT64>(base) * g_small.inc;
    ID3D12DescriptorHeap* heaps[] = {g_small.hud_heap.Get()};
    l->SetDescriptorHeaps(1, heaps);
    l->SetComputeRootSignature(g_small.hud_root.Get());
    l->SetPipelineState(g_small.hud_pso[pass].Get());
    l->SetComputeRootDescriptorTable(0, gh);
    l->SetComputeRoot32BitConstants(1, 8, k, 0);
    l->Dispatch(gx, gy, 1);
}
// The RCAS target for the upscaled picture, (re)made when the output size or format changes.
bool small_sharp_target(uint32_t w, uint32_t h, DXGI_FORMAT f)
{
    if (g_small.sharp && g_small.sharp_w == w && g_small.sharp_h == h && g_small.sharp_f == f) return true;
    g_small.sharp.Reset();
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = f;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(g_small.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                       IID_PPV_ARGS(&g_small.sharp))))
        return false;
    g_small.sharp->SetName(L"NeuralAlias render-small RCAS");
    g_small.sharp_w = w; g_small.sharp_h = h; g_small.sharp_f = f;
    return true;
}
constexpr char kStretchHlsl[] = R"(
Texture2D<float4> t_in : register(t0);
RWTexture2D<float4> u_out : register(u0);
SamplerState s_lin : register(s0);
cbuffer C : register(b0) { uint ow; uint oh; float rx; float ry; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= ow || id.y >= oh) return;
    u_out[id.xy] = t_in.SampleLevel(s_lin, (float2(id.xy) + 0.5) * float2(rx, ry), 0);
})";
bool small_init(ID3D12CommandQueue* q)
{
    if (g_small.tried) return g_small.ok && g_small.queue.Get() == q;
    g_small.tried = true;
    if (FAILED(q->GetDevice(IID_PPV_ARGS(&g_small.device)))) return false;
    ID3D12Device* dev = g_small.device.Get();
    g_small.queue = q;
    const D3D12_COMMAND_LIST_TYPE type = q->GetDesc().Type;
    for (auto& a : g_small.alloc)
        if (FAILED(dev->CreateCommandAllocator(type, IID_PPV_ARGS(&a)))) return false;
    if (FAILED(dev->CreateCommandList(0, type, g_small.alloc[0].Get(), nullptr, IID_PPV_ARGS(&g_small.list)))) return false;
    g_small.list->Close();
    g_small.list->SetName(L"NeuralAlias render-small upscale");
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_small.fence)))) return false;
    D3D12_DESCRIPTOR_RANGE r[2]{};
    r[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    r[0].NumDescriptors = 1;
    r[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    r[1].NumDescriptors = 1;
    r[1].OffsetInDescriptorsFromTableStart = 1;
    D3D12_ROOT_PARAMETER p[2]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[0].DescriptorTable.NumDescriptorRanges = 2;
    p[0].DescriptorTable.pDescriptorRanges = r;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[1].Constants.Num32BitValues = 4;
    D3D12_STATIC_SAMPLER_DESC ss{};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.MaxLOD = D3D12_FLOAT32_MAX;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rd{2, p, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> sig, err, code;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
        FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_small.root))))
        return false;
    if (FAILED(D3DCompile(kStretchHlsl, sizeof(kStretchHlsl) - 1, "j2_small_stretch.hlsl", nullptr, nullptr, "main", "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err)))
        return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_small.root.Get();
    pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_small.pso)))) return false;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kRing * 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_small.heap)))) return false;
    g_small.inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_small.ok = true;
    return true;
}
// `in` readable (NON_PIXEL_SHADER_RESOURCE), `out` in UNORDERED_ACCESS.
void small_stretch(ID3D12GraphicsCommandList* l, ID3D12Resource* in, ID3D12Resource* out, uint32_t ow, uint32_t oh, uint32_t slot)
{
    ID3D12Device* dev = g_small.device.Get();
    D3D12_CPU_DESCRIPTOR_HANDLE c = g_small.heap->GetCPUDescriptorHandleForHeapStart();
    c.ptr += static_cast<SIZE_T>(slot) * 2 * g_small.inc;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = typed(in->GetDesc().Format);
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(in, &sv, c);
    c.ptr += g_small.inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.Format = typed(out->GetDesc().Format);
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    dev->CreateUnorderedAccessView(out, nullptr, &uv, c);
    D3D12_GPU_DESCRIPTOR_HANDLE gh = g_small.heap->GetGPUDescriptorHandleForHeapStart();
    gh.ptr += static_cast<UINT64>(slot) * 2 * g_small.inc;
    ID3D12DescriptorHeap* heaps[] = {g_small.heap.Get()};
    l->SetDescriptorHeaps(1, heaps);
    l->SetComputeRootSignature(g_small.root.Get());
    l->SetPipelineState(g_small.pso.Get());
    l->SetComputeRootDescriptorTable(0, gh);
    const float rx = 1.0f / static_cast<float>(ow), ry = 1.0f / static_cast<float>(oh);
    uint32_t k[4] = {ow, oh, 0, 0};
    std::memcpy(&k[2], &rx, 4);
    std::memcpy(&k[3], &ry, 4);
    l->SetComputeRoot32BitConstants(1, 4, k, 0);
    l->Dispatch((ow + 7) / 8, (oh + 7) / 8, 1);
}
void small_note(const char* t) { std::snprintf(g.stats.small_note, sizeof g.stats.small_note, "%s", t); }
// Native HUD: lays this frame's HUD layers onto the upscaled picture. `cur` holds the picture
// (UNORDERED_ACCESS, or NON_PIXEL_SHADER_RESOURCE when *cur_state says so) and is either `out`
// or g_small.sharp; the result lands in the other one, which is returned (UNORDERED_ACCESS).
// The layers go RENDER_TARGET -> NON_PIXEL_SHADER_RESOURCE -> RENDER_TARGET.
ID3D12Resource* layer_composite(ID3D12GraphicsCommandList* l, uint32_t si, ID3D12Resource* cur, ID3D12Resource* out, UINT* out_state,
                                DXGI_FORMAT of, uint32_t ow, uint32_t oh, const na::j2::native_hud::Layer& layer,
                                std::vector<D3D12_RESOURCE_BARRIER>& b)
{
    constexpr UINT RT = D3D12_RESOURCE_STATE_RENDER_TARGET, NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ID3D12Resource* sharp = g_small.sharp.Get();
    ID3D12Resource* dst = cur == out ? sharp : out;
    if (cur == out) { b.push_back(uav_barrier(out)); add(b, out, *out_state, NPSR); *out_state = NPSR; }
    else { b.push_back(uav_barrier(sharp)); add(b, sharp, UAV, NPSR); add(b, out, *out_state, UAV); *out_state = UAV; }
    add(b, layer.n0, RT, NPSR);
    add(b, layer.n1, RT, NPSR);
    flush(l, b);
    uint32_t grey_bits = 0;
    std::memcpy(&grey_bits, &layer.grey, 4);
    const uint32_t k[8] = {ow, oh, ow, oh, (g_hud_view.load(std::memory_order_relaxed) ? 1u : 0u) | (layer.srgb ? 4u : 0u), 0, grey_bits, 0};
    ID3D12Resource* const srv[3] = {cur, layer.n0, layer.n1};
    const DXGI_FORMAT sf[3] = {of, layer.view, layer.view};
    ID3D12Resource* const uav[3] = {dst, nullptr, nullptr};
    const DXGI_FORMAT uf[3] = {of, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN};
    hud_pass(l, 4, si, srv, sf, uav, uf, k, (ow + 7) / 8, (oh + 7) / 8);
    b.push_back(uav_barrier(dst));
    add(b, layer.n0, NPSR, RT);
    add(b, layer.n1, NPSR, RT);
    if (cur == sharp) add(b, sharp, NPSR, UAV);
    return dst;
}

} // namespace

void SetRenderSmall(bool on)
{
    g_render_small.store(on);
    na::observe::SetDepthClearListener(on ? &on_depth_clear : nullptr);
}
bool RenderSmall() { return g_render_small.load(); }
void SetHudMaskView(bool on) { g_hud_view.store(on); }
void SetHudMaskEnabled(bool on) { g_hud_mask_on.store(on); }
bool HudMaskEnabled() { return g_hud_mask_on.load(); }
bool HudMaskView() { return g_hud_view.load(); }

bool CameraBinding(uint64_t resource, uint64_t offset, uint32_t param, bool table)
{
    int64_t co = -1;
    na::camera::VpGeometry main{};
    {
        std::lock_guard<std::mutex> lock(g_cam_pub_m);
        if (g_cam_pub_param < 0 || param != static_cast<uint32_t>(g_cam_pub_param) || table != g_cam_pub_table || !g_cam_pub_geo_valid) return false;
        bool known = false;
        for (size_t i = 0; i < g_cam_pub_n && !known; ++i) known = g_cam_pub[i] == resource;
        if (!known) return false;
        co = g_cam_pub_offset;
        main = g_cam_pub_geo;
    }
    // The same buffer and slot also carry other constants (Anno: the UI's own blocks in the same
    // ring buffer, 0ff948f lost most UI text). Only a block that holds the main camera itself, at
    // the camera's offset, makes the draw a world draw.
    if (co < 0) return false;
    std::vector<uint8_t> bytes(64);
    if (na::observe::ReadBuffer(resource, offset + static_cast<uint64_t>(co), bytes.data(), 64) != 64) return false;
    for (auto layout : {na::camera::Layout::RowMajor, na::camera::Layout::ColumnMajor}) {
        const auto m = na::camera::LoadMatrix(bytes, 0, layout);
        na::camera::Candidate c{};
        na::camera::VpGeometry v{};
        if (!na::camera::ViewProjection(m, c) || !na::camera::DecomposeViewProjection(m, v)) continue;
        double dot = 0, d2 = 0, len2 = 0;
        for (int j = 0; j < 3; ++j) {
            dot += v.forward[j] * main.forward[j];
            d2 += (v.position[j] - main.position[j]) * (v.position[j] - main.position[j]);
            len2 += main.position[j] * main.position[j];
        }
        if (dot > 0.98 && std::abs(v.sy - main.sy) <= 0.05 * std::abs(main.sy) && d2 <= 0.01 * len2 + 100.0) return true;
    }
    return false;
}
// I1.4 (COMMS a066d76 point 4): the frame that just closed reached Present without a deferred
// record (the in-frame detector missed it). Its depth and motion come from here instead: the
// selected scene depth as all submitted work left it, and that frame's own applied camera and
// jitter (else record()'s usual short prediction). Exactly one source per frame: only when no
// deferred record exists for it, and the result is used by this same Present. A depth cleared
// again after its first clear may no longer hold the scene: refused (counted) until a
// record-time snapshot exists. Called with g.m held, inside PresentUpscale's HostWorkScope.
void present_refuse(const char* why)
{
    ++g.stats.small_present_refused;
    std::snprintf(g.stats.small_present_why, sizeof g.stats.small_present_why, "%s", why);
}
bool present_source(ID3D12CommandQueue* queue, ExecuteFn forward)
{
    if (!g_render_small.load(std::memory_order_relaxed) || !na::present::Scaling() || g.latched || !g.dlss_ready ||
        g.paused.load() || g.queue.Get() != queue || !forward || g.frame == 0)
        return false;
    const uint64_t closed = g.frame - 1;
    if (g.def.valid && g.def.frame == closed) return false; // the in-frame source already ran
    if (g.submitted_frame == closed) return false;           // J2 ran for this frame some other way
    uint64_t depth = 0, color = 0;
    na::observe::GetLiveSelection(&depth, &color);
    if (!depth || !color) { present_refuse("no selected scene depth and colour"); return false; }
    if (g.depth_clears_closed > 1) { present_refuse("the scene depth was cleared again after the scene"); return false; }
    SceneInput si;
    si.frame = closed; si.color = color; si.depth = depth;
    if (color == g.small_color && depth == g.small_depth) { si.color_sub = g.small_csub; si.depth_sub = g.small_dsub; }
    else {
        ComPtr<ID3D12Resource> c, d;
        c.Attach(na::observe::AcquireResource(color));
        d.Attach(na::observe::AcquireResource(depth));
        if (!c || !d) { present_refuse("the scene resources are gone"); return false; }
        const auto cd = c->GetDesc(), dd = d->GetDesc();
        if (cd.MipLevels > 1 || cd.DepthOrArraySize > 1 || dd.MipLevels > 1 || dd.DepthOrArraySize > 1) {
            present_refuse("no learned subresource for a mip/array scene");
            return false;
        }
        si.color_sub = si.depth_sub = 0;
    }
    if (g.pcam.valid && g.pcam.frame == closed) {
        si.has_camera = true; si.vp = g.pcam.vp; si.reversed = g.cam_rev;
        si.jitter[0] = g.pcam.px[0]; si.jitter[1] = g.pcam.px[1]; si.injected = true;
    } else si.has_camera = false;
    si.at_present = true;
    PendingHistory pending;
    ID3D12GraphicsCommandList* ours = nullptr;
    const uint32_t slot = g.slot;
    g.frame = closed; // record() works in the frame the inputs belong to
    const bool ok = record(nullptr, 0, depth, color, &ours, pending, nullptr, nullptr, &si);
    g.frame = closed + 1;
    if (!ok || !ours || !g.def.valid || g.def.frame != closed) {
        na::capture::j2::Discard(pending.capture, true);
        present_refuse(g.stats.reason[0] ? g.stats.reason : "record refused");
        return false;
    }
    ID3D12CommandList* one[1] = {ours};
    forward(queue, 1, one);
    g.slot = (g.slot + 1) % kRing;
    g.slots[slot].fence_value = g.fence_next++;
    queue->Signal(g.fence.Get(), g.slots[slot].fence_value);
    g.tracker = pending.tracker;
    if (pending.has_interlopers) g.interlopers = pending.interlopers;
    g.eval_selection_version = pending.selection_version;
    committed_sources(pending);
    g.submitted_frame = closed;
    if (!pending.keep_reset) g.resume_reset = false;
    if (pending.cut) ++g.stats.cuts;
    na::capture::j2::Discard(pending.capture, false);
    return true;
}
// ---- Render-small jitter witness (generic: compares the game's own consecutive pictures) ----
bool witness_targets(ID3D12Resource* frame, uint32_t rw, uint32_t rh)
{
    ID3D12Device* dev = g_small.device.Get();
    const D3D12_RESOURCE_DESC fd = frame->GetDesc();
    if (!g_small.wread[0]) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 256; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        for (uint32_t i = 0; i < kRing; ++i)
            if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_small.wread[i])))) {
                for (auto& r : g_small.wread) r.Reset();
                return false;
            }
    }
    if (!g_small.wprev || g_small.wprev_w != rw || g_small.wprev_h != rh || g_small.wprev_f != fd.Format) {
        g_small.wprev.Reset();
        g_small.wprev_valid = false;
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC pd = fd;
        pd.Flags = D3D12_RESOURCE_FLAG_NONE; pd.MipLevels = 1; pd.DepthOrArraySize = 1;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &pd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                                IID_PPV_ARGS(&g_small.wprev))))
            return false;
        g_small.wprev_w = rw; g_small.wprev_h = rh; g_small.wprev_f = fd.Format;
    }
    return true;
}
// Completed counts qualify the same colour source for FUTURE frames. Raster
// injection stays on, and evidence continues after a convention is learned.
// hud_unblend: the un-blended scene at render size (the picture's format) and the readbacks.
bool unblend_target(uint32_t w, uint32_t h, DXGI_FORMAT f)
{
    if (!g_small.ublend || g_small.ublend_w != w || g_small.ublend_h != h || g_small.ublend_f != f) {
        g_small.ublend.Reset();
        if (!texture(w, h, f, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g_small.ublend)) return false;
        g_small.ublend->SetName(L"NeuralAlias HUD un-blended scene");
        g_small.ublend_w = w; g_small.ublend_h = h; g_small.ublend_f = f;
    }
    if (!g_small.uread[0]) {
        D3D12_HEAP_PROPERTIES rp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = 256; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        for (uint32_t i = 0; i < kRing; ++i)
            if (FAILED(g_small.device->CreateCommittedResource(&rp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_small.uread[i])))) {
                for (auto& r : g_small.uread) r.Reset();
                return false;
            }
    }
    return true;
}
void unblend_read(uint32_t si)
{
    if (!g_small.uread_valid[si]) return;
    g_small.uread_valid[si] = false;
    uint32_t v[5] = {};
    void* ptr = nullptr;
    const D3D12_RANGE r{0, 20};
    if (FAILED(g_small.uread[si]->Map(0, &r, &ptr)) || !ptr) return;
    std::memcpy(v, ptr, sizeof v);
    const D3D12_RANGE none{0, 0};
    g_small.uread[si]->Unmap(0, &none);
    na::j2::hud_unblend::ReportCheck(v[0], v[1], v[2], v[3], v[4]);
}
// Records the un-blend pass (L0/L1 RENDER_TARGET -> back to RENDER_TARGET; `frame` NON_PIXEL_SHADER_RESOURCE):
// g_small.ublend ends NON_PIXEL_SHADER_RESOURCE, the counts copied to uread[si].
bool unblend_record(ID3D12GraphicsCommandList* l, uint32_t si, ID3D12Resource* frame, DXGI_FORMAT ff, uint32_t rw, uint32_t rh,
                    const na::j2::native_hud::Layer& layer, std::vector<D3D12_RESOURCE_BARRIER>& b)
{
    constexpr UINT RT = D3D12_RESOURCE_STATE_RENDER_TARGET, NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (!hud_init() || !unblend_target(rw, rh, ff)) return false;
    add(b, layer.l0, RT, NPSR);
    add(b, layer.l1, RT, NPSR);
    flush(l, b);
    uint32_t grey_bits = 0;
    std::memcpy(&grey_bits, &layer.grey, 4);
    const uint32_t k[8] = {rw, rh, rw, rh, layer.srgb ? 4u : 0u, 0, grey_bits, 0};
    {
        ID3D12Resource* const srv[3] = {nullptr, nullptr, nullptr};
        const DXGI_FORMAT sf[3] = {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN};
        ID3D12Resource* const uav[3] = {nullptr, nullptr, nullptr};
        const DXGI_FORMAT uf[3] = {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN};
        hud_pass(l, 8, si, srv, sf, uav, uf, k, 1, 1);
    }
    b.push_back(uav_barrier(g_small.hud_stats.Get()));
    flush(l, b);
    {
        ID3D12Resource* const srv[3] = {frame, layer.l0, layer.l1};
        const DXGI_FORMAT sf[3] = {ff, layer.view, layer.view};
        ID3D12Resource* const uav[3] = {g_small.ublend.Get(), nullptr, nullptr};
        const DXGI_FORMAT uf[3] = {ff, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN};
        hud_pass(l, 7, si, srv, sf, uav, uf, k, (rw + 7) / 8, (rh + 7) / 8);
    }
    b.push_back(uav_barrier(g_small.ublend.Get()));
    b.push_back(uav_barrier(g_small.hud_stats.Get()));
    add(b, g_small.ublend.Get(), UAV, NPSR);
    add(b, g_small.hud_stats.Get(), UAV, D3D12_RESOURCE_STATE_COPY_SOURCE);
    add(b, layer.l0, NPSR, RT);
    add(b, layer.l1, NPSR, RT);
    flush(l, b);
    l->CopyBufferRegion(g_small.uread[si].Get(), 0, g_small.hud_stats.Get(), 64, 32);
    add(b, g_small.hud_stats.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, UAV);
    flush(l, b);
    g_small.uread_valid[si] = true;
    return true;
}
void witness_read(uint32_t si)
{
    if (!g_small.wread_valid[si]) return;
    g_small.wread_valid[si] = false;
    uint32_t v[4] = {};
    void* ptr = nullptr;
    const D3D12_RANGE r{0, 16};
    if (FAILED(g_small.wread[si]->Map(0, &r, &ptr)) || !ptr) return;
    std::memcpy(v, ptr, sizeof v);
    const D3D12_RANGE none{0, 0};
    g_small.wread[si]->Unmap(0, &none);
    if (!(g_small.wread_source[si] == g_small.final_jitter.Key())) return;
    g_small.w_tested += v[0]; g_small.w_unshifted += v[1]; g_small.w_shifted += v[2]; g_small.w_opposite += v[3];
    if (++g_small.w_frames < 60) return;
    g.stats.witness_tested = g_small.w_tested; g.stats.witness_unshifted = g_small.w_unshifted; g.stats.witness_shifted = g_small.w_shifted + g_small.w_opposite;
    ++g.stats.witness_windows;
    if (g_small.final_jitter.Observe(g_small.wread_source[si],
        {g_small.wread_frame[si],g_small.w_tested,g_small.w_unshifted,g_small.w_shifted,g_small.w_opposite}))
        ++g.stats.jitter_input_changes;
    g.stats.jitter_input_convention = static_cast<uint32_t>(g_small.final_jitter.Get());
    g.stats.jitter_input_epoch = g_small.final_jitter.Epoch();
    if (g_small.w_tested >= 20000) {
        if (g_small.w_unshifted > 3 * g_small.w_shifted) ++g_small.w_unseen_windows;
        else if (g_small.w_shifted > g_small.w_unshifted) g_small.w_unseen_windows = 0;
        // Measurement only (Astra 20cddb8 point 4): the verdict is reported, it never switches
        // jitter off or resets DLSS history (those switches changed the sampled camera and
        // cut history without repairing anything).
        if (g_small.w_unseen_windows >= 2) {
            if (!g.stats.jitter_unseen) ++g.stats.jitter_unseen_count;
            g.stats.jitter_unseen = true;
            g_small.w_unseen_windows = 0;
        } else if (g_small.w_shifted > g_small.w_unshifted) g.stats.jitter_unseen = false;
    }
    g_small.w_frames = g_small.w_tested = g_small.w_unshifted = g_small.w_shifted = g_small.w_opposite = 0;
}
// HUD fallback counts of the frame that last used this ring slot (its GPU work has completed).
void hud_read(uint32_t si)
{
    if (!g_small.hread_valid[si]) return;
    g_small.hread_valid[si] = false;
    uint32_t v[4] = {};
    void* ptr = nullptr;
    const D3D12_RANGE r{0, 16};
    if (FAILED(g_small.hread[si]->Map(0, &r, &ptr)) || !ptr) return;
    std::memcpy(v, ptr, sizeof v);
    const D3D12_RANGE none{0, 0};
    g_small.hread[si]->Unmap(0, &none);
    // v[0] = mask pixels, v[1] = tested pixels (this slot's parity), at bytes 48 / 52 + 8 * parity.
    const uint32_t px = g_small.hud_w * g_small.hud_h;
    g.stats.hud_fb_mask_px = v[0];
    g.stats.hud_fb_tested_px = v[1];
    g.stats.hud_fb_cover_permille = px ? static_cast<uint32_t>(uint64_t(v[0]) * 1000u / px) : 0u;
    // A HUD rarely covers a third of the screen; more is most likely a menu or misread scenery,
    // which keeps learning but is not protected (it would replace DLSS with NIS over that area).
    g_small.hud_protect_off = g.stats.hud_fb_cover_permille > 300;
}
static bool present_upscale(ID3D12CommandQueue* queue, ID3D12Resource* frame, uint32_t rw, uint32_t rh,
                    ID3D12Resource* out, uint32_t ow, uint32_t oh, bool hdr,
                    const native_hud::Layer* supplied,ID3D12CommandList* replay)
{
    if (!queue || !frame || !out || !rw || !rh || !ow || !oh) return false;
    const bool probe_replay=replay&&native_hud::Automatic();
    if(!supplied)native_hud::AutomaticFrame(frame,queue,rw,rh,ow,oh,false);
    // Native HUD: whether this frame's HUD was drawn into the native layers (then the proxy
    // holds the scene only, which is what DLSS gets).
    na::j2::native_hud::Layer layer;
    const bool layer_frame = supplied?(layer=*supplied,true):na::j2::native_hud::TakeFrame(frame, rw, rh, ow, oh, &layer,queue);
    const bool separate_scene=replay&&layer.reconstruction;
    const ExecuteFn forward = g_forward.load(std::memory_order_relaxed);
    const na::observe::HostWorkScope host; // our list is never observed as the game's
    std::lock_guard<std::mutex> lock(g.m);
    if(replay&&!(g.enabled.load()&&g.def.valid&&g.def.frame==g.frame&&g.def.w==rw&&g.def.h==rh&&
        g.queue.Get()==queue&&g.dlss_ready))return false;
    if (!forward) { ++g.stats.small_failed; small_note("Render-small: no game submission seen yet."); return false; }
    if (!small_init(queue)) { ++g.stats.small_failed; small_note("Render-small could not set up its upscale pass on this queue."); return false; }
    g.stats.small_rw = rw; g.stats.small_rh = rh; g.stats.small_ow = ow; g.stats.small_oh = oh;
    const uint32_t si = g_small.slot;
    if (g_small.value[si] && g_small.fence->GetCompletedValue() < g_small.value[si]) {
        // Six presents behind: never wait on the game's thread; the presenter keeps its old picture.
        ++g.stats.small_failed;
        small_note("Render-small: the upscale ring is still busy (GPU six frames behind).");
        return false;
    }
    // The ring fence proves this slot's previous GPU consumption is finished.
    const auto hud_completed=g_small.fence->GetCompletedValue();
    if(hud_completed!=UINT64_MAX)for(uint32_t j=0;j<kRing;++j)if(g_small.value[j]&&hud_completed>=g_small.value[j]){
        bool identity=false,clean=false;na::j2::native_hud::Picture::ProofReceipt receipt;
        if(g_small.hud_picture[j]&&g_small.hud_picture[j]->ProofAfterCompletion(identity,clean,&receipt)){
            na::j2::native_hud::ReportAutomatic(g_small.hud_ticket[j],receipt.hud_pixels,receipt.separated_pixels);
            if(g_small.native_hud_result[j])na::j2::native_hud::ReportNativePictureResult(identity,receipt.tested,receipt.matched,receipt.maximum_error);
            else na::j2::native_hud::ReportPictureResult(identity,clean,receipt.tested,receipt.matched,receipt.maximum_error);
            const auto& association=g_small.hud_association[j];
            if(association.source)na::j2::hud_unblend::ReportProbe(association.source,association.generation,identity,receipt.tested,receipt.picture_matched);
        }
        g_small.native_hud_result[j]=false;
        g_small.hud_ticket[j]={};
        g_small.hud_association[j]={};
        auto& probe=g_small.canvas_probe[j];
        if(probe.picture&&probe.picture->ProofAfterCompletion(identity,clean,&receipt))
            na::j2::native_hud::ReportCanvasProbe(probe.source,probe.generation,identity,receipt.tested,receipt.matched,receipt.maximum_error);
        probe={};
        g_small.hud_owner[j].reset();g_small.hud_picture[j].reset();
    }
    g_small.hud_owner[si]=layer.owner;
    g_small.hud_picture[si]=layer_frame&&!replay?layer.picture:nullptr;
    g_small.hud_association[si]={layer.association_source,layer.association_generation};
    g_small.hud_ticket[si]=layer.route_ticket;
    if (g_small.owned) na::gpu::Collect();
    g_small.owned_recording=false;
    if(na::gpu::Enabled()){
        if(!g_small.owned)g_small.owned=std::make_shared<na::gpu::Recording>();
        g_small.owned_recording=g_small.owned->Open(g_small.device.Get(),queue);
    }
    ID3D12GraphicsCommandList* l = g_small.owned_recording ? g_small.owned->Native() : g_small.list.Get();
    const auto close_list=[&]{return g_small.owned_recording ? g_small.owned->Close() : SUCCEEDED(l->Close());};
    if (!g_small.owned_recording && (FAILED(g_small.alloc[si]->Reset()) || FAILED(l->Reset(g_small.alloc[si].Get(), nullptr)))) {
        ++g.stats.small_failed; small_note("Render-small: could not reset its list."); return false;
    }
    hud_read(si);
    unblend_read(si);
    constexpr UINT PRESENT = D3D12_RESOURCE_STATE_PRESENT, NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    std::vector<D3D12_RESOURCE_BARRIER> b;
    if(layer.picture&&layer.picture->Prepared()&&(layer.unblend||native_hud::Automatic())){
        auto copy=layer.picture->ForkSnapshot(l);
        if(!copy){layer={};g_small.hud_picture[si].reset();g_small.hud_owner[si].reset();g_small.hud_ticket[si]={};}
        else {layer.picture=std::move(copy);g_small.hud_picture[si]=layer.picture;}
    }
    add(b, frame, PRESENT, NPSR);
    add(b, out, NPSR, UAV);
    if(layer.probe&&layer.probe->Prepared())layer.probe=layer.probe->ForkSnapshot(l);
    if(layer.probe){
        flush(l,b);
        if(layer.probe->Probe(l,frame,frame->GetDesc().Format))
            g_small.canvas_probe[si]={layer.probe,layer.probe_owner,layer.probe_source,layer.probe_generation};
    }
    if(layer.native_picture&&layer.native_check&&layer.picture){
        flush(l,b);
        const bool native=layer.picture->PresentNative(l,frame,typed(frame->GetDesc().Format),layer.native_picture,layer.native_check,out,g_hud_view.load(std::memory_order_relaxed));
        if(!native){g_small.hud_picture[si].reset();small_stretch(l,frame,out,ow,oh,si);}
        g_small.native_hud_result[si]=native;
        uint32_t hud_capture=0;
        if(na::capture::j2::Armed()&&layer.recording_identity){
            auto id=*layer.recording_identity;id.presentation_only=true;id.owns_present=true;id.present_ordinal=g.frame;
            na::capture::j2::Planes planes;
            planes.color=layer.reconstruction?layer.reconstruction->Scene():id.evaluation_succeeded?layer.picture->Input():layer.picture->Scene();
            planes.output=out;id.color_format=planes.color->GetDesc().Format;
            planes.extra[planes.extra_count++]={"hud_scene",layer.picture->Scene(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
            planes.extra[planes.extra_count++]={"hud_full",layer.picture->Full(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
            planes.extra[planes.extra_count++]={"hud_native",layer.native_picture,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
            planes.extra[planes.extra_count++]={"hud_replay_check",layer.native_check,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
            hud_capture=na::capture::j2::RecordEvaluation(l,planes,id);
        }
        add(b,frame,NPSR,PRESENT);add(b,out,UAV,PRESENT);flush(l,b);
        if(!close_list()){na::capture::j2::Discard(hud_capture,false);return false;}
        ID3D12CommandList* work[]={l};
        if(!na::gpu::SubmitMixed(queue,1,work,g_small.owned_recording?g_small.owned.get():nullptr,forward)){na::capture::j2::Discard(hud_capture,false);return false;}
        if(hud_capture)na::capture::j2::Submitted(hud_capture,queue,g.stats.submissions);
        g_small.value[si]=g_small.next++;queue->Signal(g_small.fence.Get(),g_small.value[si]);g_small.slot=(si+1)%kRing;
        if(native)++g.stats.small_hud_layer;
        return true; // DLSS already ran once, before this native HUD replay
    }
    // DLSS needs this frame's depth + motion, recorded on this same queue (so the GPU runs them
    // first), at the picture's size, and a DLSS instance on this device.
    if (!replay&&!(g.def.valid && g.def.frame + 1 == g.frame)) present_source(queue, forward);
    const auto& d = g.def;
    const bool fresh = d.valid && d.frame + (replay?0u:1u) == g.frame && d.w == rw && d.h == rh;
    const bool can_dlss = !probe_replay && fresh && forward && g.dlss_ready && g.device && g.device.Get() == g_small.device.Get() &&
                          g.queue.Get() == queue && g.depth32 && g.motion && g.paused.load() == false;
    if(!supplied)native_hud::AutomaticFrame(frame,queue,rw,rh,ow,oh,can_dlss);
    if (can_dlss) {
        const na::jitter::Source source{d.selection_version,rw,rh,static_cast<uint32_t>(frame->GetDesc().Format)};
        if (!(source == g_small.final_jitter.Key())) {
            g_small.final_jitter.Bind(source);
            g_small.w_frames = g_small.w_tested = g_small.w_unshifted = g_small.w_shifted = g_small.w_opposite = 0;
            g_small.wprev_valid = false;
        }
    }
    witness_read(si);
    g.stats.jitter_input_convention = static_cast<uint32_t>(g_small.final_jitter.Get());
    g.stats.jitter_input_epoch = g_small.final_jitter.Epoch();
    // Declared jitter = the raster offset, as in 0ce11a8 (Cort 2026-11-04: since the calibration
    // declared 0 from 5d5b0f5 on, jitter stopped improving the picture and it blurred over time;
    // declaring the offset is the behaviour that looked far better). The calibration keeps
    // measuring and logging (j2jitter / jitter_witness) but no longer changes the declaration.
    const na::jitter::Sample colour_jitter{d.jx, d.jy};
    bool dlss = false;
    bool evaluation_reset = false;
    bool small_shifted = false; // DLSS got the jitter-shifted colour (g_small.jshift, back to UAV after)
    // HUD separation: on when [j2] hud_nis > 0, for SDR pictures, once the last frame is held.
    const uint32_t hud_nis = na::settings::Get().j2_hud_nis.load(std::memory_order_relaxed);
    const bool hud_want = !native_hud::Automatic()&&!layer_frame && g_hud_mask_on.load(std::memory_order_relaxed) && hud_nis > 0 && !hdr && can_dlss &&
                          hud_init() && hud_targets(frame, rw, rh, ow, oh);
    const DXGI_FORMAT of = typed(out->GetDesc().Format);
    // hud_unblend: the UI's effect is removed from DLSS's input first; the layers are laid on only
    // while the self-check agrees (else the picture is exactly the game's).
    ID3D12Resource* unblended = nullptr;
    bool unblend_ran = false;
    if (layer_frame && layer.unblend && layer.l0 && layer.l1 && !layer.picture) {
        unblend_ran = unblend_record(l, si, frame, typed(frame->GetDesc().Format), rw, rh, layer, b);
        if (unblend_ran && layer.compose) unblended = g_small.ublend.Get();
    }
    const bool hud_layer = layer_frame && !layer.picture && (!layer.unblend || (layer.compose && unblended)) && hud_init() && small_sharp_target(ow, oh, of);
    if (layer_frame && !layer.picture && !hud_layer) small_note("Render-small: the native HUD could not be laid on (no composite pass); HUD missing this frame.");
    bool draw_picture=probe_replay; // native/check discovery replay, without a second DLSS evaluation
    bool hud_mask = false; // mask holds this frame's HUD and is applied
    bool hud_ran = false;  // the fallback passes ran (mask in NON_PIXEL_SHADER_RESOURCE until the end)
    const DXGI_FORMAT ff = typed(frame->GetDesc().Format);
    // Jitter witness inputs: this frame against the previous DLSS frame, when consecutive and the
    // declared jitter moved by at least a fifth of a pixel (else no verdict is possible).
    const float wdx = d.jx - g_small.wjx, wdy = d.jy - g_small.wjy;
    const bool witness = can_dlss && g.jitter_on && !g.jitter_unseen && g_small.wprev_valid && g_small.wprev_frame + 1 == d.frame &&
                         g_small.wprev_w == rw && g_small.wprev_h == rh && g_small.wprev_f == frame->GetDesc().Format &&
                         (wdx * wdx + wdy * wdy) >= 0.04f && hud_init();
    if (can_dlss) {
        add(b, g.motion.Get(), UAV, NPSR);
        flush(l, b);
        if(layer_frame&&layer.picture) {
            draw_picture=separate_scene||layer.picture->Prepare(l,frame,ff,g.motion.Get(),layer.n0,layer.n1,layer.view,layer.grey,layer.srgb,layer.clean_allowed,of,g_hud_view.load(std::memory_order_relaxed));
        }
        if(replay&&!draw_picture){
            close_list();g_small.hud_picture[si].reset();g_small.hud_owner[si].reset();
            return false; // no NGX creation/evaluation has been recorded
        }
        if (witness) {
            const DXGI_FORMAT wf = typed(frame->GetDesc().Format);
            uint32_t k[8] = {rw, rh, 0, 0, 0, 0, 0, 0};
            std::memcpy(&k[6], &wdx, 4); std::memcpy(&k[7], &wdy, 4);
            {
                ID3D12Resource* const srv[3] = {nullptr, nullptr, nullptr};
                const DXGI_FORMAT sf[3] = {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN};
                ID3D12Resource* const uav[3] = {nullptr, nullptr, nullptr};
                const DXGI_FORMAT uf[3] = {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN};
                hud_pass(l, 6, si, srv, sf, uav, uf, k, 1, 1);
            }
            b.push_back(uav_barrier(g_small.hud_stats.Get()));
            flush(l, b);
            {
                ID3D12Resource* const srv[3] = {frame, g_small.wprev.Get(), g.motion.Get()};
                const DXGI_FORMAT sf[3] = {wf, wf, DXGI_FORMAT_R32G32_FLOAT};
                ID3D12Resource* const uav[3] = {nullptr, nullptr, nullptr};
                const DXGI_FORMAT uf[3] = {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN};
                hud_pass(l, 5, si, srv, sf, uav, uf, k, (rw + 7) / 8, (rh + 7) / 8);
            }
            b.push_back(uav_barrier(g_small.hud_stats.Get()));
            add(b, g_small.hud_stats.Get(), UAV, D3D12_RESOURCE_STATE_COPY_SOURCE);
            flush(l, b);
            l->CopyBufferRegion(g_small.wread[si].Get(), 0, g_small.hud_stats.Get(), 32, 16);
            add(b, g_small.hud_stats.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, UAV);
            flush(l, b);
            g_small.wread_valid[si] = true;
            g_small.wread_source[si] = g_small.final_jitter.Key();
            g_small.wread_frame[si] = d.frame;
            ++g.stats.witness_frames;
        }
        if (hud_want && g_small.prev_valid) {
            g_small.hud_parity ^= 1u;
            // The learned map survives camera cuts and gaps (a HUD does); only a new size clears it.
            const bool forget = g_small.hud_fresh;
            g_small.hud_fresh = false;
            const uint32_t k[8] = {rw, rh, ow, oh, ((d.jx != 0.f || d.jy != 0.f) ? 2u : 0u) | (forget ? 4u : 0u) | (g_small.hud_protect_off ? 8u : 0u),
                                   g_small.hud_parity, 0, 0};
            {
                ID3D12Resource* const srv[3] = {nullptr, nullptr, nullptr};
                const DXGI_FORMAT sf[3] = {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN};
                ID3D12Resource* const uav[3] = {nullptr, nullptr, nullptr};
                const DXGI_FORMAT uf[3] = {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN};
                hud_pass(l, 3, si, srv, sf, uav, uf, k, 1, 1);
            }
            b.push_back(uav_barrier(g_small.hud_stats.Get()));
            flush(l, b);
            {
                ID3D12Resource* const srv[3] = {frame, g_small.prev.Get(), g.motion.Get()};
                const DXGI_FORMAT sf[3] = {ff, ff, DXGI_FORMAT_R32G32_FLOAT};
                ID3D12Resource* const uav[3] = {nullptr, g_small.hud.Get(), nullptr};
                const DXGI_FORMAT uf[3] = {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_UNKNOWN};
                hud_pass(l, 0, si, srv, sf, uav, uf, k, (rw + 7) / 8, (rh + 7) / 8);
            }
            b.push_back(uav_barrier(g_small.hud.Get()));
            b.push_back(uav_barrier(g_small.hud_stats.Get()));
            add(b, g.motion.Get(), NPSR, UAV);
            flush(l, b);
            {
                ID3D12Resource* const srv[3] = {nullptr, nullptr, nullptr};
                const DXGI_FORMAT sf[3] = {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN};
                ID3D12Resource* const uav[3] = {g.motion.Get(), g_small.hud.Get(), g_small.mask.Get()};
                const DXGI_FORMAT uf[3] = {DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16_FLOAT};
                hud_pass(l, 1, si, srv, sf, uav, uf, k, (rw + 7) / 8, (rh + 7) / 8);
            }
            b.push_back(uav_barrier(g.motion.Get()));
            b.push_back(uav_barrier(g_small.mask.Get()));
            b.push_back(uav_barrier(g_small.hud_stats.Get()));
            add(b, g.motion.Get(), UAV, NPSR);
            add(b, g_small.mask.Get(), UAV, NPSR);
            add(b, g_small.hud_stats.Get(), UAV, D3D12_RESOURCE_STATE_COPY_SOURCE);
            flush(l, b);
            l->CopyBufferRegion(g_small.hread[si].Get(), 0, g_small.hud_stats.Get(), 48 + 8 * g_small.hud_parity, 8);
            add(b, g_small.hud_stats.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, UAV);
            flush(l, b);
            g_small.hread_valid[si] = true;
            hud_ran = true;
            ++g.stats.hud_fb_frames;
            // Applied: zero motion (dilate), DLSS's current-colour bias and the NIS HUD pixels.
            hud_mask = !g_small.hud_protect_off;
            if (hud_mask) ++g.stats.hud_fb_applied; else ++g.stats.hud_fb_learn_only;
        }
        na::dlss::EvalDesc e{};
        e.color = separate_scene?layer.reconstruction->Scene():draw_picture?layer.picture->Input():unblended?unblended:frame;
        const DXGI_FORMAT input_format=typed(e.color->GetDesc().Format);
        // Jitter shift: the declared offset moved into the colour, so the picture does not shake.
        if (!vpj_mode() && (colour_jitter.x != 0 || colour_jitter.y != 0) && na::settings::Get().j2_jitter_shift.load(std::memory_order_relaxed) != 0 &&
            jshift_target(g_small.jshift, rw, rh, input_format) &&
            jshift_dispatch(l, e.color, g_small.jshift.Get(), rw, rh, input_format, colour_jitter.x, colour_jitter.y, kRing + si)) {
            b.push_back(uav_barrier(g_small.jshift.Get()));
            add(b, g_small.jshift.Get(), UAV, NPSR);
            flush(l, b);
            e.color = g_small.jshift.Get();
            small_shifted = true;
        }
        e.depth = g.depth32.Get();
        e.motion = g.motion.Get();
        e.output = out;
        e.render_w = e.max_render_w = rw;
        e.render_h = e.max_render_h = rh;
        e.out_w = ow;
        e.out_h = oh;
        e.quality = na::dlss::QualityForScale(rw, rh, ow, oh);
        e.preset = na::j2::PresetFor(na::settings::Get().j2_preset.load(), rw, ow);
        e.view_id = kSmallViewId;
        e.jitter_x = colour_jitter.x;
        e.jitter_y = colour_jitter.y;
        g_small.last_jx = d.jx; g_small.last_jy = d.jy;
        // History continues only from the previous frame's evaluation at the same sizes.
        e.reset = d.reset || g.small_last == 0 || d.frame != g.small_last + 1 ||
                  g.small_ow != ow || g.small_oh != oh || g.small_rw != rw || g.small_rh != rh ||
                  g_small.applied_jitter_epoch != g_small.final_jitter.Epoch();
        e.hdr = separate_scene?is_float_color(e.color->GetDesc().Format):hdr;
        e.depth_inverted = d.reversed;
        e.auto_exposure = true;
        // HUD protection: DLSS trusts this frame's colour where the learned mask says HUD.
        e.bias_current_color = draw_picture&&!separate_scene?layer.picture->Mask():hud_mask?g_small.mask.Get():nullptr;
        evaluation_reset = e.reset;
        const auto grade_options=na::donut::CurrentOptions();
        if(grade_options.tone||grade_options.bloom){
            na::donut::Inputs di;di.color=e.color;di.format=typed(e.color->GetDesc().Format);di.width=rw;di.height=rh;
            di.srgb=di.format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB||di.format==DXGI_FORMAT_R8G8B8A8_UNORM;di.reset=e.reset;di.stream=kSmallViewId;
            auto grade=na::donut::ScenePass().Prepare(g_small.device.Get(),queue,di,grade_options);
            if(grade&&grade->Record(l)){g_small.grade[si]=grade;e.color=grade->Color();}
        }
        dlss = na::dlss::Evaluate(l, e);
        if (small_shifted) { add(b, g_small.jshift.Get(), NPSR, UAV); flush(l, b); }
        // Debug view (Cort 2026-11-04: the views showed nothing in render-small, where DLSS runs
        // here and the in-frame path that drew them no longer evaluates): replace the picture with
        // the motion/depth view, then skip sharpening and the HUD so the view is what you see.
        const int dv = dlss ? g_debug_view.load(std::memory_order_relaxed) : 0;
        bool debug_shown = false;
        if (dv >= 1 && dv <= 3) {
            b.push_back(uav_barrier(out));
            flush(l, b);
            debug_shown = debug_record(l, dv, out, of, ow, oh, rw, rh, kRing + si);
            if (debug_shown) b.push_back(uav_barrier(out));
        }
        add(b, g.motion.Get(), NPSR, UAV);
        if (dlss) g_small.applied_jitter_epoch = g_small.final_jitter.Epoch();
        if (dlss && debug_shown) {
            g.small_last = d.frame;
            g.small_ow = ow; g.small_oh = oh; g.small_rw = rw; g.small_rh = rh;
            flush(l, b);
        } else if (dlss) {
            g.small_last = d.frame;
            g.small_ow = ow; g.small_oh = oh; g.small_rw = rw; g.small_rh = rh;
            // After DLSS: RCAS on the picture (menu 0..100 = RCAS 0..200), then the HUD from
            // NIS over it where the mask says HUD. The picture moves between `out` and `sharp`
            // (`cur`); it ends in `out`, UNORDERED_ACCESS, as DLSS left it.
            const uint32_t pct = 2u * na::settings::Get().j2_sharpness.load(std::memory_order_relaxed);
            const bool have_sharp = (pct || hud_mask || hud_layer) && small_sharp_target(ow, oh, of);
            ID3D12Resource* sharp = g_small.sharp.Get();
            ID3D12Resource* cur = out;
            UINT out_state = UAV;
            if (pct && have_sharp) {
                b.push_back(uav_barrier(out));
                add(b, out, UAV, NPSR);
                out_state = NPSR;
                flush(l, b);
                if (rcas_dispatch(l, out, sharp, ow, oh, pct, of, kRing + si, g.depth32.Get(), rw, rh, d.reversed)) {
                    b.push_back(uav_barrier(sharp));
                    cur = sharp;
                    ++g.stats.small_sharpened;
                }
            }
            if (hud_mask && have_sharp) {
                if (g_hud_nis.Dispatch(g_small.device.Get(), l, frame, ff, rw, rh, g_small.hudup.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
                                       ow, oh, false, static_cast<float>(hud_nis) / 100.f, si)) {
                    b.push_back(uav_barrier(g_small.hudup.Get()));
                    add(b, g_small.hudup.Get(), UAV, NPSR);
                    ID3D12Resource* dst = cur == out ? sharp : out;
                    if (cur == out) { b.push_back(uav_barrier(out)); add(b, out, out_state, NPSR); out_state = NPSR; }
                    else { add(b, sharp, UAV, NPSR); add(b, out, out_state, UAV); out_state = UAV; }
                    flush(l, b);
                    const uint32_t k[8] = {rw, rh, ow, oh, g_hud_view.load(std::memory_order_relaxed) ? 1u : 0u, 0, 0, 0};
                    ID3D12Resource* const srv[3] = {cur, g_small.hudup.Get(), g_small.mask.Get()};
                    const DXGI_FORMAT sf[3] = {of, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16_FLOAT};
                    ID3D12Resource* const uav[3] = {dst, nullptr, nullptr};
                    const DXGI_FORMAT uf[3] = {of, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN};
                    hud_pass(l, 2, si, srv, sf, uav, uf, k, (ow + 7) / 8, (oh + 7) / 8);
                    b.push_back(uav_barrier(dst));
                    add(b, g_small.hudup.Get(), NPSR, UAV);
                    if (cur == sharp) add(b, sharp, NPSR, UAV);
                    cur = dst;
                    ++g.stats.small_hud;
                }
            }
            if (hud_layer && have_sharp) {
                cur = layer_composite(l, si, cur, out, &out_state, of, ow, oh, layer, b);
                ++g.stats.small_hud_layer;
            }
            if (cur == sharp) { // the picture is in `sharp`: back into `out`
                constexpr UINT SRC = D3D12_RESOURCE_STATE_COPY_SOURCE, DST = D3D12_RESOURCE_STATE_COPY_DEST;
                add(b, sharp, UAV, SRC);
                add(b, out, out_state, DST);
                flush(l, b);
                l->CopyResource(out, sharp);
                add(b, sharp, SRC, UAV);
                out_state = DST;
            }
            add(b, out, out_state, UAV);
        } else {
            ++g.stats.small_dlss_failed;
            char why[128];
            std::snprintf(why, sizeof why, "Render-small: DLSS refused the frame (%s); stretched.",
                          na::dlss::GetStats().reason[0] ? na::dlss::GetStats().reason : "no reason given");
            small_note(why);
        }
        g.def.valid = false; // used (or refused) once
    } else if(!probe_replay) {
        small_note(!d.valid ? "Render-small: no depth and motion yet for this frame; stretched."
                   : d.frame + 1 != g.frame ? "Render-small: this frame's scene was not captured; stretched."
                   : (d.w != rw || d.h != rh) ? "Render-small: the scene is not the picture's size; stretched."
                   : g.queue.Get() != queue ? "Render-small: the scene was drawn on another queue; stretched."
                   : "Render-small: DLSS is not ready; stretched.");
    }
    if (hud_ran) add(b, g_small.mask.Get(), NPSR, UAV);
    if (unblend_ran) add(b, g_small.ublend.Get(), NPSR, UAV);
    if (!dlss) {
        flush(l, b);
        small_stretch(l, separate_scene?layer.reconstruction->Scene():probe_replay?layer.picture->Scene():replay&&draw_picture?layer.picture->Input():unblended?unblended:frame, out, ow, oh, si);
        if(!probe_replay)g.small_last = 0; // discovery never changes reconstruction history
        if (hud_layer) { // the native HUD over the stretched scene, then back into `out`
            UINT out_state = UAV;
            layer_composite(l, si, out, out, &out_state, of, ow, oh, layer, b);
            constexpr UINT SRC = D3D12_RESOURCE_STATE_COPY_SOURCE, DST = D3D12_RESOURCE_STATE_COPY_DEST;
            ID3D12Resource* sharp = g_small.sharp.Get();
            add(b, sharp, UAV, SRC);
            add(b, out, out_state, DST);
            flush(l, b);
            l->CopyResource(out, sharp);
            add(b, sharp, SRC, UAV);
            add(b, out, DST, UAV);
            ++g.stats.small_hud_layer;
        }
    }
    if(dlss&&draw_picture&&!replay){flush(l,b);if(layer.picture->Composite(l,out))++g.stats.small_hud_layer;else g_small.hud_ticket[si]={};}
    if(!dlss)g_small.hud_ticket[si]={};
    if(replay&&draw_picture){flush(l,b);(separate_scene?layer.reconstruction:layer.picture)->SeedNativeAlpha(l,out);}
    if(separate_scene&&layer.ui_output&&!layer.picture->SeedNativeScene(l,layer.ui_output,layer.ui_view)){
        close_list();++g.stats.small_failed;small_note("Native HUD background could not be seeded.");return false;
    }
    // The jitter witness compares the next frame with this one.
    if (dlss && g.jitter_on && !g.jitter_unseen && witness_targets(frame, rw, rh)) {
        constexpr UINT SRC = D3D12_RESOURCE_STATE_COPY_SOURCE, DST = D3D12_RESOURCE_STATE_COPY_DEST;
        add(b, frame, NPSR, SRC);
        add(b, g_small.wprev.Get(), NPSR, DST);
        flush(l, b);
        l->CopyResource(g_small.wprev.Get(), frame);
        add(b, frame, SRC, NPSR);
        add(b, g_small.wprev.Get(), DST, NPSR);
        g_small.wprev_valid = true;
        g_small.wprev_frame = g.small_last;
        g_small.wjx = g_small.last_jx; g_small.wjy = g_small.last_jy;
    } else g_small.wprev_valid = false;
    // The HUD passes compare with this frame next time.
    if (hud_want) {
        constexpr UINT SRC = D3D12_RESOURCE_STATE_COPY_SOURCE, DST = D3D12_RESOURCE_STATE_COPY_DEST;
        add(b, frame, NPSR, SRC);
        add(b, g_small.prev.Get(), NPSR, DST);
        flush(l, b);
        l->CopyResource(g_small.prev.Get(), frame);
        add(b, frame, SRC, NPSR);
        add(b, g_small.prev.Get(), DST, NPSR);
        g_small.prev_valid = true;
    } else if (!can_dlss || hud_nis == 0 || !g_hud_mask_on.load(std::memory_order_relaxed)) {
        g_small.prev_valid = false; // a gap: the next comparison would be against an old frame
        // The learned map is kept: the HUD is still where it was (it is cleared on a new size).
    }
    // Evidence capture (F8 / "Capture"): render-small had none, so a flicker in the upscaled
    // picture could not be measured (Cort's e7acb94 capture came out empty). Colour = DLSS's
    // input (the proxy, scene only), output = the final picture with the native HUD on it.
    uint32_t capture = 0;
    if (!probe_replay&&(dlss||replay) && (na::capture::j2::Armed()||replay)) {
        b.push_back(uav_barrier(out));
        flush(l, b);
        na::capture::j2::Planes cp;
        cp.color = separate_scene?layer.reconstruction->Scene():draw_picture?layer.picture->Input():frame; // actual NGX input, NPSR
        cp.depth = g.depth32.Get();        // NPSR
        cp.motion = g.motion.Get();        cp.motion_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        cp.output = out;                   // UAV
        na::capture::j2::Identity ci;
        ci.present_ordinal = g.frame;
        ci.submission = g.stats.submissions;
        ci.view_id = static_cast<uint32_t>(kSmallViewId);
        ci.route = static_cast<uint32_t>(na::j2::Route::Between);
        ci.width = rw;
        ci.height = rh;
        ci.color_format = static_cast<uint32_t>(cp.color->GetDesc().Format);
        ci.jitter_x = colour_jitter.x;
        ci.jitter_y = colour_jitter.y;
        ci.raster_jitter_x = d.jx; ci.raster_jitter_y = d.jy;
        ci.jitter_convention = static_cast<uint32_t>(g_small.final_jitter.Get());
        ci.jitter_epoch = g_small.final_jitter.Epoch();
        ci.reset = evaluation_reset ? 1u : 0u;
        ci.preset = na::j2::PresetFor(na::settings::Get().j2_preset.load(), rw, ow);
        ci.sharpness = na::settings::Get().j2_sharpness.load(std::memory_order_relaxed);
        // 2026-11-04: this evaluation's output IS the presented picture (no overlay pairing),
        // plus the camera the motion shader used and the planes behind each motion vector.
        ci.owns_present = true;
        ci.inputs_frame = d.frame;
        ci.camera_valid = true;
        for (int i = 0; i < 16; ++i) { ci.vp_cur[i] = d.vp_cur[i]; ci.vp_prev[i] = d.vp_prev[i]; ci.inv_vp_cur[i] = d.inv_cur[i]; }
        for (int i = 0; i < 3; ++i) ci.eye[i] = d.eye[i];
        ci.camera_source = d.camera_source; ci.camera_age = d.camera_age;
        ci.camera_offset = g.camera_offset; ci.camera_param = g.camera_param;
        ci.selection_version = d.selection_version;
        ci.tracker_restart = d.tracker_restart ? 1u : 0u; ci.cut_reason = d.cut_reason;
        ci.camera_cut = d.reset ? 1u : 0u;
        ci.depth_reversed = d.reversed ? 1u : 0u;
        ci.flow_ran = d.flow_ran ? 1u : 0u;
        ci.flow_jitter_x=d.flow_jitter_x;ci.flow_jitter_y=d.flow_jitter_y;
        ci.flow_jitter_convention=d.flow_jitter_convention;ci.flow_jitter_epoch=d.flow_jitter_epoch;
        ci.flow_loosen = na::settings::Get().j2_flow_loosen.load(std::memory_order_relaxed);
        uint64_t sel_d = 0, sel_c = 0;
        na::observe::GetLiveSelection(&sel_d, &sel_c);
        ci.view_depth = sel_d; ci.view_color = sel_c;
        auto extra = [&](const char* name, ID3D12Resource* r, D3D12_RESOURCE_STATES st) {
            if (r && cp.extra_count < na::capture::j2::kMaxExtra) cp.extra[cp.extra_count++] = {name, r, st};
        };
        extra("provenance", g.provenance.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        extra("scene_color", d.scene_color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (d.flow_ran) {
            extra("flow_residual", d.flow_residual, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            extra("flow_valid", d.flow_valid, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            extra("flow_mask", d.flow_mask, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        if (layer_frame) {
            extra("hud_n0", layer.n0, D3D12_RESOURCE_STATE_RENDER_TARGET);
            extra("hud_n1", layer.n1, D3D12_RESOURCE_STATE_RENDER_TARGET);
            // Un-blend evidence: the UI replayed over black / grey at render size (with the
            // back buffer, P = L0 + m*S can be checked offline pixel by pixel).
            if (layer.unblend) { extra("hud_l0", layer.l0, D3D12_RESOURCE_STATE_RENDER_TARGET); extra("hud_l1", layer.l1, D3D12_RESOURCE_STATE_RENDER_TARGET); }
            if(draw_picture){extra("hud_scene",layer.picture->Scene(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);extra("hud_full",layer.picture->Full(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                if(!separate_scene)extra("hud_draw_mask",layer.picture->Mask(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);}
        }
        if(layer.probe){extra("hud_probe_candidate",layer.probe->Full(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ci.hud_probe_source=layer.probe_source;ci.hud_probe_generation=layer.probe_generation;}
        ci.evaluation_succeeded=dlss;
        if(replay&&layer.recording_identity)*layer.recording_identity=ci;
        if(!replay)capture = na::capture::j2::RecordEvaluation(l, cp, ci);
    }
    b.push_back(uav_barrier(out));
    add(b, frame, NPSR, PRESENT);
    add(b, out, UAV, PRESENT);
    flush(l, b);
    if (!close_list()) {
        na::capture::j2::Discard(capture, false);
        // The list may hold transitions already recorded: never submit it. The presenter
        // keeps its old picture; the frame/out states are unchanged because nothing ran.
        ++g.stats.small_failed;
        small_note("Render-small: its upscale list was rejected.");
        g.latched = true;
        return false;
    }
    ID3D12CommandList* lists[] = {l,replay};const UINT count=replay?2u:1u;
    if (dlss) na::dlss::BeforeExecute(queue, count, lists);
    if(!na::gpu::SubmitMixed(queue,count,lists,g_small.owned_recording?g_small.owned.get():nullptr,forward)){
        na::capture::j2::Discard(capture,false);++g.stats.small_failed;return false;
    } // native HUD stays before this frame's completion fence
    if (dlss) na::dlss::OnExecuted(queue, count, lists);
    if (capture) na::capture::j2::Submitted(capture, queue, g.stats.submissions);
    g_small.value[si] = g_small.next++;
    queue->Signal(g_small.fence.Get(), g_small.value[si]);
    g_small.slot = (si + 1) % kRing;
    if(probe_replay)return true; // not a presented scene, capture gap, camera cut or history event
    {   // Every frame, cheap (CPU): the capture's frames.csv shows the frames around a stretch/reset.
        na::capture::j2::FrameNote fnote;
        fnote.frame = g.frame;
        fnote.dlss = dlss ? 1u : 0u;
        fnote.reset = dlss && evaluation_reset ? 1u : 0u;
        fnote.jitter_x = colour_jitter.x; fnote.jitter_y = colour_jitter.y;
        na::observe::GetLiveSelection(&fnote.selection_depth, &fnote.selection_color);
        fnote.selection_version = na::observe::LiveSelectionVersion();
        fnote.camera_source = d.camera_source; fnote.camera_age = d.camera_age;
        for (int i = 0; i < 3; ++i) fnote.eye[i] = d.eye[i];
        strncpy_s(fnote.why, dlss ? "" : g.stats.small_note, _TRUNCATE);
        na::capture::j2::NoteFrame(fnote);
        // Automatic captures (test packages, [j2] capture_auto): the first stretched frame of a
        // burst, a history reset, a fast camera turn, and otherwise periodically (cooldown).
        const bool was_stretching = g_small_stretch_streak.load(std::memory_order_relaxed) != 0;
        if (!dlss && !was_stretching) na::capture::j2::Trigger("stretch", 24);
        else if (fnote.reset && g.small_last != 0) na::capture::j2::Trigger("reset", 24);
        else if (dlss && g.def_prev_forward_ok) {
            double dot = 0;
            for (int i = 0; i < 3; ++i) dot += d.forward[i] * g.def_prev_forward[i];
            if (std::acos((std::max)(-1.0, (std::min)(1.0, dot))) > 0.02) na::capture::j2::Trigger("camera_turn", 24);
            else na::capture::j2::Trigger("periodic", 24);
        }
        g.def_prev_forward = d.forward; g.def_prev_forward_ok = dlss;
    }
    if (dlss) { ++g.stats.small_dlss; if (g.stats.small_note[0] && std::strstr(g.stats.small_note, "stretched")) g.stats.small_note[0] = 0; }
    else ++g.stats.small_stretched;
    g_small_stretch_streak.store(dlss ? 0u : g_small_stretch_streak.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
    return true;
}

bool PresentUpscale(ID3D12CommandQueue* queue,ID3D12Resource* frame,uint32_t rw,uint32_t rh,
                    ID3D12Resource* out,uint32_t ow,uint32_t oh,bool hdr,void*)
{return present_upscale(queue,frame,rw,rh,out,ow,oh,hdr,nullptr,nullptr);}
bool PreHudUpscale(ID3D12CommandQueue* queue,ID3D12Resource* frame,uint32_t rw,uint32_t rh,
                   ID3D12Resource* out,uint32_t ow,uint32_t oh,const native_hud::Layer& layer,ID3D12CommandList* replay)
{return present_upscale(queue,frame,rw,rh,out,ow,oh,is_float_color(frame->GetDesc().Format),&layer,replay);}

void SetEnabled(bool on)
{
    g.enabled.store(on);
    g.configured.store(on);
    if (on) na::observe::SetTableBindCallback(&on_table_bind);
    na::observe::SetEntryCallback(on?&on_entry:nullptr);
}
bool Enabled() { return na::settings::EffectsReady() && g.enabled.load(); }
PhysicalReport::~PhysicalReport()
{
    if (!armed) return;
    if (result == PhysicalOutcome::Evaluated) ++g.stats.physical_evaluated;
    else if (result == PhysicalOutcome::Unavailable) ++g.stats.physical_unavailable;
    else ++g.stats.physical_failed;
    if (p.outcome) p.outcome(p.ctx, generation, result, why[0] ? why : "J2 could not evaluate this frame (see its status)");
}
void SetPhysicalInputProvider(const PhysicalInputProvider& p) { std::lock_guard<std::mutex> lk(g_up_m); g_phys = p; }
void SetUpscaleTarget(const UpscaleTarget& t) { std::lock_guard<std::mutex> lk(g_up_m); g_up = t; }
UpscaleTarget GetUpscaleTarget() { std::lock_guard<std::mutex> lk(g_up_m); return g_up; }
void EnableNativeScaleBridge(bool enabled) {
    g_native_bridge.store(enabled);g_native_reduced.store(false);
    if(enabled){na::observe::SetOperationSnapshots(true);na::observe::SetRasterStateTracking(true);
        na::observe::SetShaderOperationHook(na::observe::ShaderLane::Reconstruction,std::make_shared<NativeScaleHook>());}
}
NativeBridgeStats GetNativeBridgeStats() {
    NativeBridgeStats r;auto& n=g_nb;
    r.enabled=g_native_bridge.load();r.reduced=g_native_reduced.load();
    r.hints=n.hints.load();r.examined=n.examined.load();r.not_reading=n.not_reading.load();r.snapshot=n.snapshot.load();
    r.source_size=n.source_size.load();r.draw_state=n.draw_state.load();r.viewport=n.viewport.load();r.no_destination=n.no_destination.load();
    r.ambiguous=n.ambiguous.load();r.candidates=n.candidates.load();r.recorded=n.recorded.load();r.record_failed=n.record_failed.load();
    r.after_skipped=n.after_skipped.load();
    {std::lock_guard<std::mutex> lock(g_up_m);r.rw=g_native_extent.rw;r.rh=g_native_extent.rh;r.ow=g_native_extent.ow;r.oh=g_native_extent.oh;}
    std::lock_guard<std::mutex> lock(n.m);std::snprintf(r.reason,sizeof r.reason,"%s",n.reason);
    r.src_w=n.src_w;r.src_h=n.src_h;r.src_fmt=n.src_fmt;r.seen_w=n.seen_w;r.seen_h=n.seen_h;r.seen_fmt=n.seen_fmt;
    return r;
}
void SetNativeRenderExtent(uint32_t rw,uint32_t rh,uint32_t ow,uint32_t oh) {
    {std::lock_guard<std::mutex> lock(g_up_m);g_native_extent={rw,rh,ow,oh};}
    g_native_reduced.store(g_native_bridge.load()&&rw&&rh&&ow&&oh&&(rw<ow||rh<oh),std::memory_order_release);
}
uint32_t JitterPhasesFor(uint32_t render_w, uint32_t out_w)
{
    if (!render_w || out_w <= render_w) return kDlaaJitterPhases;
    const double r = static_cast<double>(out_w) / render_w;
    return static_cast<uint32_t>((std::min)(64.0, std::ceil(kJitterPhases * r * r)));
}
float UpscaleMipBias(uint32_t render_w, uint32_t out_w)
{
    if (!render_w || !out_w) return -1.0f;
    return static_cast<float>(std::log2(static_cast<double>(render_w) / out_w)) - 1.0f;
}
bool Configured() { return g.configured.load(); }
void SetPaused(bool paused)
{
    if (!paused && g.paused.load()) { std::lock_guard<std::mutex> lock(g.m); g.resume_reset = true; }
    g.paused.store(paused);
}
bool Paused() { return g.paused.load(); }
bool RecordRcasForTests(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* in, ID3D12Resource* out,
                        uint32_t w, uint32_t h, uint32_t percent, DXGI_FORMAT fmt)
{
    std::lock_guard<std::mutex> lock(g.m);
    if (!g.device) g.device = device;
    if (g.device.Get() != device) return false;
    return rcas_dispatch(list, in, out, w, h, percent, fmt, 0);
}
bool CompileHudShadersForTests(char* why, size_t why_size)
{
    for (const char* entry : {"classify", "dilate", "composite", "clear", "layer", "witness", "wclear", "unblend", "uclear"}) {
        ComPtr<ID3DBlob> code, err;
        if (FAILED(D3DCompile(kHudHlsl, sizeof(kHudHlsl) - 1, "j2_small_hud.hlsl", nullptr, nullptr, entry, "cs_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) {
            std::snprintf(why, why_size, "%s: %s", entry, err ? static_cast<const char*>(err->GetBufferPointer()) : "no message");
            return false;
        }
    }
    return true;
}
bool RecordHudUnblendForTests(ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* list, ID3D12Resource* picture, DXGI_FORMAT picture_format,
                              ID3D12Resource* l0, ID3D12Resource* l1, DXGI_FORMAT view, float grey, bool srgb, ID3D12Resource* out,
                              uint32_t w, uint32_t h, ID3D12Resource* counts)
{
    std::lock_guard<std::mutex> lock(g.m);
    if (!small_init(queue) || !hud_init()) return false;
    uint32_t grey_bits = 0;
    std::memcpy(&grey_bits, &grey, 4);
    const uint32_t k[8] = {w, h, w, h, srgb ? 4u : 0u, 0, grey_bits, 0};
    {
        ID3D12Resource* const srv[3] = {nullptr, nullptr, nullptr};
        const DXGI_FORMAT sf[3] = {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN};
        ID3D12Resource* const uav[3] = {nullptr, nullptr, nullptr};
        const DXGI_FORMAT uf[3] = {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN};
        hud_pass(list, 8, 0, srv, sf, uav, uf, k, 1, 1);
    }
    std::vector<D3D12_RESOURCE_BARRIER> b;
    b.push_back(uav_barrier(g_small.hud_stats.Get()));
    flush(list, b);
    {
        ID3D12Resource* const srv[3] = {picture, l0, l1};
        const DXGI_FORMAT sf[3] = {picture_format, view, view};
        ID3D12Resource* const uav[3] = {out, nullptr, nullptr};
        const DXGI_FORMAT uf[3] = {picture_format, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN};
        hud_pass(list, 7, 0, srv, sf, uav, uf, k, (w + 7) / 8, (h + 7) / 8);
    }
    if (counts) {
        b.push_back(uav_barrier(g_small.hud_stats.Get()));
        add(b, g_small.hud_stats.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        flush(list, b);
        list->CopyBufferRegion(counts, 0, g_small.hud_stats.Get(), 64, 32);
        add(b, g_small.hud_stats.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        flush(list, b);
    }
    return true;
}
bool RecordHudLayerForTests(ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* list, ID3D12Resource* picture, DXGI_FORMAT picture_format,
                            ID3D12Resource* n0, ID3D12Resource* n1, DXGI_FORMAT view, float grey, bool srgb, ID3D12Resource* out,
                            uint32_t w, uint32_t h)
{
    std::lock_guard<std::mutex> lock(g.m);
    if (!small_init(queue) || !hud_init()) return false;
    uint32_t grey_bits = 0;
    std::memcpy(&grey_bits, &grey, 4);
    const uint32_t k[8] = {w, h, w, h, srgb ? 4u : 0u, 0, grey_bits, 0};
    ID3D12Resource* const srv[3] = {picture, n0, n1};
    const DXGI_FORMAT sf[3] = {picture_format, view, view};
    ID3D12Resource* const uav[3] = {out, nullptr, nullptr};
    const DXGI_FORMAT uf[3] = {picture_format, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN};
    hud_pass(list, 4, 0, srv, sf, uav, uf, k, (w + 7) / 8, (h + 7) / 8);
    return true;
}
bool RecordHudClassifyForTests(ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* list, ID3D12Resource* cur, ID3D12Resource* prev,
                               DXGI_FORMAT color_format, ID3D12Resource* motion, ID3D12Resource* evidence, uint32_t w, uint32_t h, uint32_t flags)
{
    std::lock_guard<std::mutex> lock(g.m);
    if (!small_init(queue) || !hud_init()) return false;
    const uint32_t k[8] = {w, h, w, h, flags, 0, 0, 0};
    ID3D12Resource* const srv[3] = {cur, prev, motion};
    const DXGI_FORMAT sf[3] = {color_format, color_format, DXGI_FORMAT_R32G32_FLOAT};
    ID3D12Resource* const uav[3] = {nullptr, evidence, nullptr};
    const DXGI_FORMAT uf[3] = {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_UNKNOWN};
    hud_pass(list, 0, 0, srv, sf, uav, uf, k, (w + 7) / 8, (h + 7) / 8);
    return true;
}
bool RecordJitterWitnessForTests(ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* list, ID3D12Resource* cur,
    ID3D12Resource* prev, ID3D12Resource* motion, uint32_t w, uint32_t h, float dx, float dy, ID3D12Resource* counts)
{
    std::lock_guard<std::mutex> lock(g.m);
    if (!small_init(queue) || !hud_init() || !counts) return false;
    uint32_t k[8] = {w,h,0,0,0,0,0,0};
    std::memcpy(&k[6],&dx,4); std::memcpy(&k[7],&dy,4);
    ID3D12Resource* const empty[3] = {nullptr,nullptr,nullptr};
    const DXGI_FORMAT none[3] = {DXGI_FORMAT_UNKNOWN,DXGI_FORMAT_UNKNOWN,DXGI_FORMAT_UNKNOWN};
    hud_pass(list,6,0,empty,none,empty,none,k,1,1);
    auto barrier=uav_barrier(g_small.hud_stats.Get());list->ResourceBarrier(1,&barrier);
    ID3D12Resource* const srv[3] = {cur,prev,motion};
    const DXGI_FORMAT sf[3] = {typed(cur->GetDesc().Format),typed(prev->GetDesc().Format),DXGI_FORMAT_R32G32_FLOAT};
    hud_pass(list,5,0,srv,sf,empty,none,k,(w+7)/8,(h+7)/8);
    barrier=uav_barrier(g_small.hud_stats.Get());list->ResourceBarrier(1,&barrier);
    std::vector<D3D12_RESOURCE_BARRIER> b;
    add(b,g_small.hud_stats.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);flush(list,b);
    list->CopyBufferRegion(counts,0,g_small.hud_stats.Get(),32,16);
    add(b,g_small.hud_stats.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);flush(list,b);
    return true;
}
void SetDebugView(int mode) { g_debug_view.store(mode < 0 || mode > 3 ? 0 : mode); }
int DebugView() { return g_debug_view.load(); }
void SetProjectionWindow(bool on) { g_window_enabled.store(on); }
void SetProjectionWindowScissor(bool on) { g_window_scissor_wanted.store(on); }
bool ProjectionWindow() { return g_window_enabled.load(); }
bool ProjectionWindowScissor(uint64_t* depth, uint32_t* ew, uint32_t* eh)
{
    if (!g_window_scissor.load(std::memory_order_acquire)) return false;
    if (depth) *depth = g_window_depth.load();
    if (ew) *ew = g_window_ew.load();
    if (eh) *eh = g_window_eh.load();
    return true;
}
void SetJitter(bool on)
{
    std::lock_guard<std::mutex> lock(g.m);
    if (g.jitter_on != on) g.resume_reset = true; // the input sequence changes character
    g.jitter_on = on;
    if (!on) g.jit.valid = false;
}
bool Jitter()
{
    std::lock_guard<std::mutex> lock(g.m);
    return g.jitter_on;
}

// Projection window engagement, once per present under g.m. Game command lists are
// recorded before the frame they belong to is submitted, so the two switches are ordered:
// a windowed frame without the scissor only costs more GPU time, while a scissored frame
// without the window would lose its picture outside the corner. So the scissor starts
// three frames after windowing starts, and stops three frames before windowing stops.
void update_window(bool executed)
{
    auto& w = g.win;
    constexpr uint64_t kLag = 3;
    const auto stop = [&](const char* why, uint64_t hold) {
        if (!w.engaged || w.drain_until) return;
        std::snprintf(g.stats.window_note, sizeof g.stats.window_note, "%s", why);
        w.drain_until = g.frame + kLag; // windowing continues while scissored recordings drain
        w.hold_until = g.frame + kLag + hold;
        w.scissor_from = UINT64_MAX;
    };
    w.streak = executed ? w.streak + 1 : 0;
    const int mode = static_cast<int>(na::settings::Get().j2_scale_mode.load());
    const double f = mode > 0 && mode <= 4 ? na::scale::FractionForMode(mode) : 1.0;
    const bool wanted = g_window_enabled.load() && f < 1.0 && !g.paused.load() && !g.latched && g.w && g.h && g.jitter_on;
    if (w.engaged && w.last_frame == g.frame) { // a windowed frame is presenting now
        ++w.recent_frames;
        if (!executed) { ++w.recent_missed; ++g.stats.window_missed; }
        if (w.recent_frames >= 300) w.recent_frames = w.recent_missed = w.recent_repeated = 0;
        // No automatic drop to DLAA (Cort 2026-11-03): an on/off cycle (cf65722: 8 engagements in
        // two minutes) is worse than a repeated frame. A miss repeats the last result and is
        // counted; the counters say what to fix. The note records a bad stretch for the log.
        if (w.recent_missed > 10 || w.recent_repeated > 45) {
            ++w.breaker_stops;
            std::snprintf(g.stats.window_note, sizeof g.stats.window_note, "bad stretch #%u: %u missed, %u full-size of %u frames (kept on)",
                          w.breaker_stops, w.recent_missed, w.recent_repeated, w.recent_frames);
            w.recent_frames = w.recent_missed = w.recent_repeated = 0;
        }
    }
    if (w.engaged && w.drain_until && g.frame >= w.drain_until) { w.engaged = false; w.drain_until = 0; }
    if (!wanted) stop(mode == 0 ? "Render mode DLAA." : "Projection window switched off.", 0);
    else if (w.engaged && (g.w != w.w || g.h != w.h)) stop("The scene size changed.", 30);
    else if (w.engaged && !w.drain_until && mode != w.mode) { // live mode change: new corner
        w.mode = mode;
        w.ew = (std::max)(1u, static_cast<uint32_t>(std::lround(g.w * f)));
        w.eh = (std::max)(1u, static_cast<uint32_t>(std::lround(g.h * f)));
        w.scissor_from = g.frame + kLag; // no scissor until recordings use the new corner
    } else if (!w.engaged && g.frame >= w.hold_until && w.streak >= 30) {
        w.engaged = true;
        w.engage_frame = g.frame + 1;
        w.scissor_from = w.engage_frame + kLag;
        w.drain_until = 0;
        w.mode = mode;
        w.w = g.w; w.h = g.h;
        w.ew = (std::max)(1u, static_cast<uint32_t>(std::lround(g.w * f)));
        w.eh = (std::max)(1u, static_cast<uint32_t>(std::lround(g.h * f)));
        w.recent_frames = w.recent_missed = w.recent_repeated = 0;
        ++g.stats.window_engagements;
        g.stats.window_note[0] = 0;
    }
    const bool scissor = w.engaged && !w.drain_until && g.frame + 1 >= w.scissor_from && g_window_scissor_wanted.load();
    if (scissor) {
        uint64_t depth = 0, color = 0;
        na::observe::GetLiveSelection(&depth, &color);
        g_window_depth.store(depth);
        g_window_ew.store(w.ew);
        g_window_eh.store(w.eh);
        g_window_scissor.store(depth != 0, std::memory_order_release);
    } else g_window_scissor.store(false, std::memory_order_release);
    g.stats.window_engaged = w.engaged;
    g.stats.window_w = w.engaged ? w.ew : 0;
    g.stats.window_h = w.engaged ? w.eh : 0;
}

void OnPresent(ID3D12CommandQueue* queue)
{
    na::eventlog::TickFrame(); // every game Present: the event log's frame number
    std::lock_guard<std::mutex> lock(g.m);
    if (queue && g.queue.Get() == queue) {
        {
            // Route selector: close this frame's structure + outcome.
            na::j2::FrameObservation& o = g.frame_agg.Observation();
            o.executed = g.submitted_frame == g.frame;
            const uint64_t late = g.stats.inline_late_writes + g.stats.post_scene_batches;
            o.later_scene_write = o.executed && late > g.route_prev_late;
            na::capture::j2::OnFrameClosed(g.frame, late - g.route_prev_late); // evidence capture identity
            o.camera_missed = g.stats.no_camera > g.route_prev_no_camera;
            o.depth_state_missed = g.stats.no_state > g.route_prev_no_state;
            if (!o.executed && g.stats.frames) { // a present without DLAA, once DLAA has run at all: why
                if (o.camera_missed) ++g.stats.miss_no_camera;
                else if (o.depth_state_missed) ++g.stats.miss_no_state;
                else if (g.inline_mode && g.inl_frame != g.frame) ++g.stats.miss_no_candidate;
                else ++g.stats.miss_other;
            }
            if (g.stats.frames) { // every delivered/not-delivered edge, timed to the frame (sub-5 s flicker)
                static na::eventlog::Edge exec_edge;
                if (exec_edge.Changed(o.executed ? 1 : 0)) {
                    const char* why = o.executed ? "delivered"
                                    : o.camera_missed ? "no_camera"
                                    : o.depth_state_missed ? "no_depth_state"
                                    : (g.inline_mode && g.inl_frame != g.frame) ? "no_candidate" : "other";
                    const auto& fs = g.insertion_frame.decision;
                    const auto& ls = g.insertion_learned.decision;
                    na::eventlog::Emit("dlss", "delivered=%u why=%s inline=%u rejects=%u reject=\"%s\" later_scene_write=%u "
                                       "shape{writer=%llu occ=%u tr=%u cand=%u writes=%u dst=%llu} prev_shape{writer=%llu occ=%u tr=%u cand=%u} remembered=%d",
                                       o.executed ? 1u : 0u, why, g.inline_mode ? 1u : 0u, unsigned(g.frame_rejects),
                                       g.frame_reject_text, o.later_scene_write ? 1u : 0u,
                                       (unsigned long long)fs.boundary_writer, fs.writer_occurrence, fs.final_transition, fs.final_candidate, fs.writes,
                                       (unsigned long long)fs.destination, (unsigned long long)ls.boundary_writer, ls.writer_occurrence,
                                       ls.final_transition, ls.final_candidate, g.delivered_shape_count);
                }
            }
            {
                // Current-frame ownership by a supported upscaler: the GAME's own evaluations
                // this frame (J2's NGX calls are marked as injector calls and excluded).
                // Lifetime/creation counts are not ownership; when the game stops
                // evaluating, the selector relearns from structure.
                const uint64_t external = na::ngx::GetStats().evaluates_external + na::ffx::GetStats().upscale_dispatches +
                                          na::xess::GetStats().executes;
                o.interceptor = external > g.route_prev_external;
                g.route_prev_external = external;
            }
            g.route_prev_late = late;
            g.route_prev_no_camera = g.stats.no_camera;
            g.route_prev_no_state = g.stats.no_state;
            const auto& d = g.router.OnFrame(o);
            g.frame_agg.Reset();
            auto& r = g.stats.route;
            r.route = static_cast<uint32_t>(d.route);
            r.state = static_cast<uint32_t>(d.state);
            r.mode = static_cast<uint32_t>(g.router.Mode());
            r.matches = d.matches;
            r.observed = d.observed;
            r.generation = d.history_generation;
            r.changes = d.route_changes;
            r.scene_frames = d.scene_frames;
            r.executed_frames = d.executed_frames;
            r.placement_consistent = d.placement_consistent;
            r.contradictions = d.contradictions;
            r.unstable_windows = d.unstable_windows;
            r.contribution_established = d.contribution_established ? 1u : 0u;
            std::snprintf(r.reason, sizeof r.reason, "%s", d.reason);
            std::snprintf(r.missing, sizeof r.missing, "%s", d.missing);
            std::snprintf(r.change, sizeof r.change, "%s", d.change);
            {
                static na::eventlog::Edge route_edge;
                int64_t was = 0;
                if (route_edge.Changed(int64_t(r.route) << 16 | int64_t(r.state) << 8 | int64_t(r.mode), &was))
                    na::eventlog::Emit("route", "route=%u state=%u mode=%u was{route=%lld state=%lld mode=%lld} changes=%u reason=\"%s\" change=\"%s\" missing=\"%s\"",
                                       r.route, r.state, r.mode, was < 0 ? -1ll : (long long)(was >> 16), was < 0 ? -1ll : (long long)(was >> 8 & 0xFF),
                                       was < 0 ? -1ll : (long long)(was & 0xFF), unsigned(r.changes), r.reason, r.change, r.missing);
            }
            apply_route_locked(nullptr);
        }
        std::memcpy(g.stats.insertion_frame_trace, g.insertion_trace, sizeof g.insertion_trace);
        g.insertion_trace[0] = 0;
        g.insertion_trace_used = 0;
        if (g.inline_mode) {
            ++g.stats.inline_presents;
            if (g.submitted_frame == g.frame) ++g.stats.inline_covered;
            else if (g.insertion_frame.decision.writes)
                // The reason recorded at rejection time this frame (never a recycled
                // earlier success string); else no candidate reached the matcher.
                std::snprintf(g.stats.insertion_last_miss, sizeof g.stats.insertion_last_miss,
                    "frame=%llu rejects=%u %s", (unsigned long long)g.frame, g.frame_rejects,
                    g.frame_reject_text[0] ? g.frame_reject_text
                                           : "no candidate reached the matcher (no read of the scene colour after its last write was seen in a scene recording)");
            if (g.insertion_frame.decision.writes && g.submitted_frame != g.frame) {
                char why[200];
                std::snprintf(why, sizeof why, "Inline DLAA missed this frame (%s).",
                              g.frame_reject_text[0] ? "the scene boundary did not match the learned one; see j2_last_miss" : "no boundary candidate");
                reason(why); // not the previous frame's "running" text
            }
            g.stats.active = g.submitted_frame == g.frame;
        }
        if (g.insertion_frame.color) {
            g.insertion_learned = g.insertion_frame;
            if (g.submitted_frame == g.frame) { // a shape that got DLAA: remember it (most recent first, no duplicates)
                const auto same = [](const State::LearnedInsertion& a, const State::LearnedInsertion& b) {
                    return a.color == b.color && a.depth == b.depth && a.decision.final_candidate == b.decision.final_candidate &&
                           a.decision.final_transition == b.decision.final_transition && a.decision.boundary_writer == b.decision.boundary_writer &&
                           a.decision.writer_occurrence == b.decision.writer_occurrence && a.decision.destination == b.decision.destination &&
                           a.decision.pipeline == b.decision.pipeline;
                };
                int at = g.delivered_shape_count < State::kShapes ? g.delivered_shape_count : State::kShapes - 1;
                for (int k = 0; k < g.delivered_shape_count; ++k) if (same(g.delivered_shapes[k], g.insertion_frame)) { at = k; break; }
                for (int k = at; k > 0; --k) g.delivered_shapes[k] = g.delivered_shapes[k - 1];
                g.delivered_shapes[0] = g.insertion_frame;
                if (at == g.delivered_shape_count && g.delivered_shape_count < State::kShapes) ++g.delivered_shape_count;
                g.stats.entry_shapes = static_cast<uint32_t>(g.delivered_shape_count);
            }
        }
        {
            // Placement profile: label this frame's consumers from the submission-ordered
            // writes and reads (facts once the frame is closed) and learn their classes.
            if (g.frame_seq_complete && !g.frame_seq.empty() && g.scene_w) {
                g.entries.Observe(g.frame_seq,true,g.frame);
                g.stats.entry_keys=static_cast<uint32_t>(g.entries.Size());g.stats.entry_ctx_keys=static_cast<uint32_t>(g.entries.ContextSize());
                std::vector<na::j2::Labelled> labels;
                na::j2::LabelFrame(g.frame_seq, g.scene_w, g.scene_h, labels);
                for (const auto& lab : labels) g.profile.Observe(lab, g.frame);
                ++g.stats.labelled_frames;
            } else if (!g.frame_seq.empty()) {
                ++g.stats.unlabelled_frames;
            }
            if (g.submitted_frame == g.frame && g.stats.inline_late_writes + g.stats.post_scene_batches > g.route_prev_late_class) note_late_frame();
            // A class-triggered DLAA followed by more scene content: demote that class now.
            if (g.class_fired && g.submitted_frame == g.frame && g.stats.inline_late_writes + g.stats.post_scene_batches > g.route_prev_late_class) {
                // The route tolerates an isolated misplacement; the pattern that caused it
                // does not keep its acceptance.
                g.profile.Contradict(g.fired_class, g.fired_producer, g.frame, g.fired_context);
                if (g.fired_class.op == na::j2::Op::Barrier) ++g.stats.barrier_contradictions;
                else ++g.stats.class_contradictions;
            }
            g.route_prev_late_class = g.stats.inline_late_writes + g.stats.post_scene_batches;
            g.class_fired = false;
            g.fired_mode = 0;
            g.fired_barrier_event = false;
            g.frame_seq.clear();
            g.frame_seq_complete = true;
            na::observe::ResetDerived();
            g.stats.profile_accepted = g.profile.AcceptedCount();
            g.stats.profile_rejected = g.profile.RejectedCount();
            g.stats.profile_ambiguous = g.profile.AmbiguousCount();
            g.stats.profile_entries = static_cast<uint32_t>(g.profile.Size());
            // An inline recording that was never submitted within two frames is released
            // (its capture slot retired; the frame already counted as missed).
            if (g.inl.valid && g.frame >= g.inl.frame + 2) {
                na::capture::j2::Discard(g.inl.history.capture, true);
                g.inl.history.capture = 0;
                g.inl.valid = false;
                ++g.stats.expired_inline;
            }
        }
        g.insertion_frame = {};
        update_window(g.submitted_frame == g.frame);
        // I1.4: what the Present-time source may use for the frame that closes now.
        g.pcam = {};
        if (g.jit.valid && g.jit.applied && g.jit.frame == g.frame) {
            const auto L = g.jit.layout ? na::camera::Layout::ColumnMajor : na::camera::Layout::RowMajor;
            const std::vector<uint8_t> jb(g.jit.jvp[g.jit.layout], g.jit.jvp[g.jit.layout] + 64);
            const na::camera::Matrix m = na::camera::LoadMatrix(jb, 0, L);
            na::camera::Candidate c{};
            if (na::camera::ViewProjection(m, c)) {
                g.pcam.valid = true; g.pcam.frame = g.frame; g.pcam.vp = m;
                g.pcam.px[0] = g.jit.px[0]; g.pcam.px[1] = g.jit.px[1];
            }
        }
        g.depth_clears_closed = g_depth_clears.exchange(0, std::memory_order_relaxed);
        {
            uint64_t dsel = 0, csel = 0;
            na::observe::GetLiveSelection(&dsel, &csel);
            g_clear_watch.store(dsel, std::memory_order_relaxed);
        }
        g.stats.depth_clears_last = g.depth_clears_closed;
        ++g.frame;
        vpj_advance();
        replay_probe_frame();
        g.jit.valid = false; // J1: the next camera block starts a new jitter sample
        g.inline_sample_locked = false;
        g.inline_camera_submitted = false;
        g.stats.inline_frame_rejects = g.frame_rejects;
        g.frame_rejects = 0;
        g.frame_reject_text[0] = 0;
        if (g.scene_batches_cur) {
            g.batch_hist[g.batch_hist_n++ % 16] = static_cast<uint8_t>(g.scene_batches_cur > 255 ? 255 : g.scene_batches_cur);
            uint32_t votes[256] = {};
            const uint32_t n = g.batch_hist_n < 16 ? g.batch_hist_n : 16;
            for (uint32_t i = 0; i < n; ++i) ++votes[g.batch_hist[i]];
            uint32_t best = 0;
            for (uint32_t c = 1; c < 256; ++c) if (votes[c] > votes[best]) best = c;
            g.scene_batches_target = best;
            g.stats.scene_batches = g.scene_batches_cur;
            g.stats.scene_batches_target = best;
            if (g.scene_batches_cur > g.stats.scene_batches_max) g.stats.scene_batches_max = g.scene_batches_cur;
        }
        g.scene_batches_cur = 0;
    }
}

void Execute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists, ExecuteFn forward)
{
    if (forward) g_forward.store(forward, std::memory_order_relaxed);
    if (!na::settings::EffectsReady() || !g.enabled.load(std::memory_order_relaxed) || g.paused.load(std::memory_order_relaxed) || !queue || !lists || !count) {
        forward(queue, count, lists);
        return;
    }
    const na::observe::HostWorkScope host; // our objects and our list are never observed as the game's
    std::unique_lock<std::mutex> lock(g.m);
    if (g.latched) {
        g.stats.active = false;
        lock.unlock();
        forward(queue, count, lists);
        return;
    }
    ++g.stats.submissions;
    {
        // [j2] route=auto|between|inline (diagnostic overrides); legacy inline=1 = inline override.
        const uint32_t rm = na::settings::Get().j2_route.load();
        const na::j2::RouteMode mode = rm == 1 ? na::j2::RouteMode::ForceBetween
                                     : (rm == 2 || na::settings::Get().j2_inline.load()) ? na::j2::RouteMode::ForceInline
                                     : na::j2::RouteMode::Auto;
        g.router.SetMode(mode);
        apply_route_locked(queue);
    }
    uint64_t selected_depth = 0, selected_color = 0;
    na::observe::GetLiveSelection(&selected_depth, &selected_color);
    if(vpj_mode()&&selected_depth)for(UINT i=0;i<count;++i){
        const auto sample=na::observe::ListSceneViewportJitter(lists[i],selected_depth);
        if(sample){g.vpj_submitted_sample=sample;g.vpj_submitted_frame=g.frame;g.vpj_submitted_depth=selected_depth;}
    }
    if (selected_color && selected_color != g.scene_color_id) {
        // A different scene image: its size is the class reference; the old profile does not apply.
        g.scene_color_id = selected_color;
        g.scene_w = g.scene_h = 0;
        g.profile.Clear();
        g.entries.Clear();
        ComPtr<ID3D12Resource> c;
        c.Attach(na::observe::AcquireResource(selected_color));
        if (c) {
            const D3D12_RESOURCE_DESC cd = c->GetDesc();
            g.scene_w = static_cast<uint32_t>(cd.Width);
            g.scene_h = cd.Height;
        }
    }
    if (selected_color && selected_depth) for (UINT i = 0; i < count; ++i) {
        if (!lists[i]) continue;
        const auto id = na::observe::IdOf(lists[i]);
        const auto decision = na::observe::ListInsertionDecision(lists[i], selected_color);
        if (decision.writes && g.scene_w && g.scene_h) { // I1.1: this frame's scene was drawn at this size
            g.stats.seen_scene_w = g.scene_w; g.stats.seen_scene_h = g.scene_h; g.stats.seen_scene_frame = g.frame;
        }
        if ((decision.writes || decision.all_reads) && g.frame_seq_complete) {
            // Placement profile: this list's writes and consumers, in submission order.
            // 512 events silently dropped later lists in the Sniper frames and
            // taught early barriers as final ones. Bound the complete frame, and
            // never train on an unmarked prefix when that bound is exceeded.
            if (!na::observe::ListColorEvents(lists[i], selected_color, g.frame_seq)) g.frame_seq_complete = false;
            constexpr size_t kMaxFrameEvents = 16384;
            if (g.frame_seq.size() > kMaxFrameEvents) {
                g.frame_seq.resize(kMaxFrameEvents);
                g.frame_seq_complete = false;
            }
        }
        {
            // Structure for the route selector, in submission order.
            na::j2::ListSummary sum;
            sum.renders_scene = na::observe::ListRendersTo(lists[i], selected_color, selected_depth);
            sum.color_writes = decision.writes;
            sum.reads = decision.all_reads + decision.derived_reads; // a read of a copy of the scene continues its chain
            sum.reads_after_last_write = decision.final_candidate ? 1u : 0u;
            sum.clears_color = sum.renders_scene && na::observe::ListClearsTarget(lists[i], selected_color);
            uint64_t sig = 1469598103934665603ull; // FNV-1a over the boundary's identity
            for (uint64_t v : {decision.boundary_writer, static_cast<uint64_t>(decision.writer_occurrence), decision.destination, decision.pipeline})
                sig = (sig ^ v) * 1099511628211ull;
            sum.boundary_signature = decision.final_candidate ? sig : 0;
            g.frame_agg.AddList(sum);
            if (sum.renders_scene) {
                // Present must be attributable to learn, and every route needs DLSS:
                // start it here (host-work scope), never from Present.
                if (!g.device) ensure_device(queue);
                if (g.device && g.queue.Get() == queue && !g.dlss_ready && g.dlss_tried_frame != g.frame) {
                    g.dlss_tried_frame = g.frame;
                    dlss_on();
                }
            }
        }
        if ((decision.writes || decision.all_reads) && g.insertion_trace_used < sizeof g.insertion_trace - 160) {
            const int n = std::snprintf(g.insertion_trace + g.insertion_trace_used, sizeof g.insertion_trace - g.insertion_trace_used,
                " L%llu:w%u/r%u/f%u/d%llu/p%llu/b%llu:%u%s", (unsigned long long)id, decision.writes,
                decision.all_reads, decision.final_candidate, (unsigned long long)decision.destination,
                (unsigned long long)decision.pipeline, (unsigned long long)decision.boundary_writer,
                decision.writer_occurrence, g.inl.valid && g.inl.list == lists[i] ? "*" : "");
            if (n > 0) g.insertion_trace_used += static_cast<size_t>(n);
        }
        if (!na::observe::ListRendersTo(lists[i], selected_color, selected_depth)) continue;
        // Command-list objects are pooled/reused. Learn the last scene-writing
        // recording in SUBMISSION order across this frame, not the prior job
        // assigned to this particular object. A later write-only list invalidates
        // an earlier consumer; a later consumer after that write replaces it.
        if (decision.writes) {
            if (g.insertion_frame.decision.final_candidate != decision.final_candidate)
                ++g.stats.insertion_changes;
            // Learned in SUBMISSION order from recordings actually submitted (Astra #4);
            // a recording that is never submitted cannot replace the learned anchor.
            g.insertion_frame = {selected_color, selected_depth, decision};
        }
        ++g.stats.insertion_traces;
        if (decision.candidates) {
            g.stats.insertion_list = id;
            g.stats.insertion_reads = decision.candidates;
            g.stats.insertion_final = decision.final_candidate;
            g.stats.insertion_writes = decision.writes;
            g.stats.insertion_destination = decision.destination;
            g.stats.insertion_pipeline = decision.pipeline;
        }
    }
    if (g.inline_mode) {
        // Count actual observed writes after our insertion, including later
        // lists without the scene depth bound. This is not a full-overwrite claim.
        bool after = g.submitted_frame == g.frame;
        for (UINT i = 0; i < count; ++i) {
            if (!lists[i]) continue;
            const auto d = na::observe::ListInsertionDecision(lists[i], selected_color);
            if (g.inl.valid && lists[i] == g.inl.list && na::observe::ListRecording(lists[i]) == g.inl.rec) {
                if (d.writes > g.inl.writes) { g.stats.inline_late_writes += d.writes - g.inl.writes; note_late_writer(lists[i], selected_color, g.inl.writes, 1, d.writes - g.inl.writes); }
                after = true;
            } else if (after) { g.stats.inline_late_writes += d.writes; if (d.writes) note_late_writer(lists[i], selected_color, 0, 2, d.writes); }
        }
        // Inline mode: no J2 list is inserted; DLAA is already inside the game's list.
        // Its history is committed when that list is actually submitted.
        bool has = false;
        if (g.inl.valid)
            for (UINT i = 0; i < count && !has; ++i)
                has = lists[i] == g.inl.list && na::observe::ListRecording(lists[i]) == g.inl.rec;
        jitter_batch(lists, count);
        for (UINT i = 0; i < count; ++i)
            if (lists[i] && na::observe::ListReferences(lists[i], selected_depth) &&
                !na::observe::ListRootConstantBuffers(lists[i]).empty())
                g.inline_camera_submitted = true;
        if (!g_native_reduced.load(std::memory_order_relaxed) && !g.inl.valid && g.submitted_frame != g.frame && g.queue.Get() == queue && g.dlss_ready &&
            selected_color && selected_depth && g.router.Mode() == na::j2::RouteMode::Auto) {
            ID3D12GraphicsCommandList* ours = nullptr;
            PendingHistory pending;
            const int before = record_between_consumer(lists, count, selected_color, selected_depth, &ours, pending);
            if (before >= 0) {
                std::vector<ID3D12CommandList*> batch(lists, lists + count);
                batch.insert(batch.begin() + before, ours);
                Slot& slot = g.slots[g.slot];
                g.slot = (g.slot + 1) % kRing;
                na::dlss::BeforeExecute(queue, static_cast<UINT>(batch.size()), batch.data());
                forward(queue, static_cast<UINT>(batch.size()), batch.data());
                na::dlss::OnExecuted(queue, static_cast<UINT>(batch.size()), batch.data());
                g.tracker = pending.tracker;
                if (pending.has_interlopers) g.interlopers = pending.interlopers;
                g.eval_selection_version = pending.selection_version;
                committed_sources(pending);
                g.submitted_frame = g.frame;
                if (!pending.keep_reset) g.resume_reset = false;
                g.stats.active = true;
                ++g.stats.frames;
                ++g.stats.between_observed;
                if (pending.cut) ++g.stats.cuts;
                for (UINT i = static_cast<UINT>(before); i < count; ++i)
                    if (lists[i]) g.stats.inline_late_writes += na::observe::ListInsertionDecision(lists[i], selected_color).writes;
                slot.fence_value = g.fence_next++;
                queue->Signal(g.fence.Get(), slot.fence_value);
                na::capture::j2::Submitted(pending.capture, queue, g.stats.submissions);
                na::j2::compute_flow::Committed(pending.compute_flow);
                reason("Live DLAA is running before a learned post-processing consumer in a separate list.");
                return;
            }
            na::capture::j2::Discard(pending.capture, false);
        }
        lock.unlock();
        forward(queue, count, lists);
        if (!has) return;
        lock.lock();
        g.tracker = g.inl.history.tracker;
        if (g.inl.history.has_interlopers) g.interlopers = g.inl.history.interlopers;
        g.eval_selection_version = g.inl.history.selection_version;
        committed_sources(g.inl.history);
        g.submitted_frame = g.frame;
        if (!g.inl.history.keep_reset) g.resume_reset = false;
        if (g.inl.history.cut) ++g.stats.cuts;
        ++g.stats.frames;
        ++g.stats.inline_frames;
        if (g.stats.inline_jitter_x != 0 || g.stats.inline_jitter_y != 0) ++g.stats.inline_jittered;
        g.stats.active = true;
        g.slots[g.inl.slot].fence_value = g.fence_next++;
        queue->Signal(g.fence.Get(), g.slots[g.inl.slot].fence_value);
        na::capture::j2::Submitted(g.inl.history.capture, queue, g.stats.submissions);
        na::j2::compute_flow::Committed(g.inl.history.compute_flow);
        g.inl.history.capture = 0;
        g.inl.valid = false;
        reason(g_native_reduced.load(std::memory_order_relaxed)?"Native scene DLSS reconstructed at full output before later game draws.":
            "Live DLAA is running inside the game's scene list (post-processing shares it).");
        return;
    }
    if (g.router.Current().route != na::j2::Route::Between) {
        // Observing, or a route that J2 does not run between lists (none / interceptor).
        const auto& d = g.router.Current();
        char why[200];
        std::snprintf(why, sizeof why, "%s %s", d.route == na::j2::Route::None ? "Choosing where DLAA belongs:" : "Route:", d.reason);
        reason(why);
        lock.unlock();
        forward(queue, count, lists);
        return;
    }
    if (g.submitted_frame == g.frame) {
        ++g.stats.duplicate_submissions;
        uint64_t dsel = 0, csel = 0;
        na::observe::GetLiveSelection(&dsel, &csel);
        if (g.jitter_on) count_post_dlaa(lists, 0, count, dsel); // later batches of a frame whose DLAA already ran
        // Scene drawing after our DLAA (this frame's insertion was too early): counted so
        // the next frame inserts after it, and reported (post_scene).
        if (dsel && csel) {
            // A batch that CLEARS the colour starts the image over (a repeat or another
            // view), not more of the same scene: not counted. A depth clear alone is
            // fine (first-person weapons are drawn that way onto the same image).
            bool scene_here = false, restarts = false;
            for (UINT i = 0; i < count; ++i) {
                if (!lists[i]) continue;
                if (na::observe::ListRendersTo(lists[i], csel, dsel)) {
                    scene_here = true;
                    if (na::observe::ListClearsTarget(lists[i], csel)) restarts = true;
                }
            }
            if (scene_here && !restarts) {
                ++g.scene_batches_cur; ++g.stats.post_scene_batches;
                for (UINT i = 0; i < count; ++i)
                    if (lists[i] && na::observe::ListRendersTo(lists[i], csel, dsel)) { note_late_writer(lists[i], csel, 0, 3, 1); break; }
            }
        }
        lock.unlock();
        forward(queue, count, lists);
        return;
    }
    uint64_t depth_id = 0, color_id = 0;
    na::observe::GetLiveSelection(&depth_id, &color_id);
    jitter_batch(lists, count); // J1, before anything in this batch is forwarded
    if (!depth_id || !color_id) {
        ++g.stats.no_selection;
        reason("Waiting for discovery to choose the scene depth and colour.");
        lock.unlock();
        forward(queue, count, lists);
        return;
    }
    int scene = -1;
    bool before_reader = false, restarts = false;
    for (UINT i = 0; i < count; ++i) {
        // Prefer the last scene writer BEFORE the first explicit downstream
        // reader (AA, tonemap, etc.). A postprocess list may rebind the scene RTV
        // at its end; that rebind must not move us past its earlier read.
        if (lists[i] && scene >= 0 && na::observe::ListSamplesTexture(lists[i], color_id)) {
            before_reader = true;
            break;
        }
        if (lists[i] && na::observe::ListRendersTo(lists[i], color_id, depth_id)) {
            scene = static_cast<int>(i);
            if (na::observe::ListClearsTarget(lists[i], color_id)) restarts = true;
        }
    }
    // A batch that clears the colour starts the image over: it is scene batch 1 again.
    if (scene >= 0) g.scene_batches_cur = restarts ? 1 : g.scene_batches_cur + 1;
    // More scene batches are expected this frame (the last frame had them) and
    // post-processing has not started: DLAA waits for the last one.
    const bool wait_later = scene >= 0 && g.scene_batches_cur < g.scene_batches_target;
    ID3D12GraphicsCommandList* ours = nullptr;
    PendingHistory pending;
    // P4-0b: the flow pair needs a GPU wait between our pre list and our DLAA list.
    // Without the diagnostic split switch that is only done when no game list follows
    // the scene list in this submission (the game's lists keep their grouping).
    na::j2::flow::Ticket flow_ticket;
    const bool flow_pair_ok = scene >= 0 && (static_cast<UINT>(scene) + 1 == count || na::j2::flow::AllowSplit());
    if (scene < 0) {
        ++g.stats.no_scene_list;
    } else if (wait_later) {
        ++g.stats.waited_batches;
    } else if (!ensure_device(queue)) {
        // reason set
    } else {
        if (!dlss_on()) {
            // reason set
        } else if (record(lists, static_cast<UINT>(scene), depth_id, color_id, &ours, pending, nullptr, nullptr, nullptr, 0,
                          &flow_ticket, flow_pair_ok)) {
            g.stats.later_passes = na::observe::ListTargetsAfter(lists[scene], color_id, depth_id);
        } else if (window_frame_now()) {
            // Projection window: whatever stopped DLSS this frame, the corner must not reach the
            // screen clipped (Cort's cf65722 run: 55 raw frames, black corners). Repeat the last result.
            na::capture::j2::Discard(pending.capture, false);
            na::j2::flow::Abandon(queue, flow_ticket);
            flow_ticket = {};
            pending = PendingHistory{};
            if (record(lists, static_cast<UINT>(scene), depth_id, color_id, &ours, pending, nullptr, nullptr, nullptr, 0, nullptr, false, true))
                ++g.stats.window_repeat_fallbacks;
        }
    }
    if (!wait_later) g.stats.active = ours != nullptr;
    if (scene >= 0 && !wait_later) g.jit.valid = false; // the scene closes this frame's jitter: the next camera block starts a new sample
    if (!ours) {
        na::capture::j2::Discard(pending.capture, false); // our list is reset before any GPU use
        na::j2::flow::Abandon(queue, flow_ticket);        // never leaves the flow engine waiting
        lock.unlock();
        forward(queue, count, lists);
        return;
    }
    if (before_reader) ++g.stats.between_observed; // an observed reader follows in this batch
    else ++g.stats.between_predicted;              // placed after a predicted last scene list
    std::vector<ID3D12CommandList*> batch(lists, lists + count);
    UINT ours_at = static_cast<UINT>(scene) + 1;
    if (flow_ticket.valid) batch.insert(batch.begin() + ours_at++, flow_ticket.pre); // P4-0b pre list, right before ours
    batch.insert(batch.begin() + ours_at, ours);
    const UINT n2 = static_cast<UINT>(batch.size());
    Slot& s = g.slots[g.slot];
    g.slot = (g.slot + 1) % kRing;
    na::dlss::BeforeExecute(queue, n2, batch.data());
    na::j2::flow::Submit(queue, flow_ticket, batch.data(), n2, ours_at, forward); // one forward unless a flow pair is consumed
    if (g.jitter_on) count_post_dlaa(lists, static_cast<UINT>(scene) + 1, count, depth_id); // same batch, after the DLAA list
    // The void D3D12 forward has now submitted our list. Failed recording/NGX
    // attempts above must never replace the previous submitted camera.
    g.tracker = pending.tracker;
    if (pending.has_interlopers) g.interlopers = pending.interlopers;
    g.eval_selection_version = pending.selection_version;
    committed_sources(pending);
    g.submitted_frame = g.frame;
    if (!pending.keep_reset) g.resume_reset = false;
    ++g.stats.frames;
    if (pending.cut) ++g.stats.cuts;
    if (g.stats.later_passes) {
        char why[160];
        std::snprintf(why, sizeof why, "Live DLAA is running (inserted after the scene list; it draws %u other targets after the scene).", g.stats.later_passes);
        reason(why);
    } else if (before_reader) {
        reason("Live DLAA is running before a later scene-colour reader; game AA remains enabled.");
    } else {
        reason("Live DLAA is running.");
    }
    na::dlss::OnExecuted(queue, n2, batch.data());
    s.fence_value = g.fence_next++;
    queue->Signal(g.fence.Get(), s.fence_value);
    na::j2::flow::Committed(flow_ticket, g.fence.Get(), s.fence_value);
    na::j2::compute_flow::Committed(pending.compute_flow);
    na::capture::j2::Submitted(pending.capture, queue, g.stats.submissions);
}

void StartSmearTest() { g_smear.requested.store(true); }
bool SmearTestRunning() { std::lock_guard<std::mutex> lock(g.m); return g_smear.running || g_smear.requested.load(); }
void SmearTestStatus(char* text, size_t capacity)
{
    if (!text || !capacity) return;
    std::lock_guard<std::mutex> lock(g.m);
    std::snprintf(text, capacity, "%s", g_smear.requested.load() && !g_smear.running ? "Smear test starts with the next DLAA frame." : g_smear.status);
}

Stats GetStats()
{
    std::lock_guard<std::mutex> lock(g.m);
    g.stats.frame_now = g.frame;
    if (g.game_motion_ok) g.game_motion.Describe(g.stats.game_motion, sizeof g.stats.game_motion);
    return g.stats;
}

void Shutdown()
{
    std::lock_guard<std::mutex> lock(g.m);
    g.depth32.Reset(); g.motion.Reset(); g.provenance.Reset(); g.output.Reset(); g.color_in.Reset(); g.sharp.Reset(); g.jshift.Reset();
    g.msaa = MsaaResolve{};
    for (auto& s : g.slots) { s.list.Reset(); s.alloc.Reset(); s.fence_value = 0; }
    na::j2::flow::Shutdown(); // before the device and J2's fence go
    na::j2::compute_flow::Reset();
    g.fence.Reset(); g.queue.Reset(); g.device.Reset();
    g.arbiter = na::motion::gpu::Arbiter{};
    g.replay = na::j2::replay_motion::Pass{}; g.replay_ok = false;
    g.game_motion = na::j2::game_motion::Feed{}; g.game_motion_ok = false; g.game_motion_logged = false; g.game_motion_used = -2;
    g_dbg = DebugPass{};
    g_rcas = RcasPass{};
    g_jshift = JitterShift{}; // released here, not in static destruction after D3D12 has gone
    g_small.jshift.Reset();
    g.arbiter_ok = false;
    g.tracker.Reset();
    g.interlopers.Reset();
    g.cam_f0 = g.cam_f1 = 0;
    g.frame = 1;
    g.submitted_frame = 0;
    g.eval_selection_version = 0;
    g.scene_batches_cur = g.scene_batches_target = 0;
    g.batch_hist_n = 0;
    g.inline_mode = false;
    g.router.Reset();
    g.frame_agg.Reset();
    g.route_prev_late = g.route_prev_post_scene = g.route_prev_no_camera = g.route_prev_no_state = 0;
    g.route_gen_applied = 0;
    g.dlss_ready = false;
    g.dlss_tried_frame = ~0ull;
    g.frame_rejects = 0;
    g.frame_reject_text[0] = 0;
    g.profile.Clear();
    g.entries.Clear();
    g.waiting_scene = {};
    g.native_scene = {};
    g_native_reduced.store(false);g_native_bridge.store(false);
    g.jitter_writes.Clear();
    g.frame_seq.clear();
    g.frame_seq_complete = true;
    g.scene_color_id = 0;
    g.scene_w = g.scene_h = 0;
    g.class_fired = false;
    g.route_prev_late_class = 0;
    g.insertion_frame = g.insertion_learned = {};
    g.delivered_shape_count = 0;
    g.inl = State::InlinePending{};
    g.inl_frame = 0;
    g.inline_sample_locked = false;
    g.inline_camera_submitted = false;
    g.resume_reset = false;
    g.w = g.h = 0;
    g.rw = g.rh = g.ow = g.oh = 0;
}

} // namespace na::j2
