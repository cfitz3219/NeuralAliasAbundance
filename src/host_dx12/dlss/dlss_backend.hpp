// C6 (MASTER-PLAN Phase 2): the generic "feed DLSS" backend.
//
// Given colour, depth, motion vectors and jitter for a frame, this module
// creates and evaluates NVIDIA DLSS Super Resolution (or DLAA) and writes the
// result into an output texture. It knows nothing about any game: everything
// in Phases 3-4 (FSR/XeSS interception, generic discovery, adapters) plugs in
// by filling an EvalDesc, or an abundance::Frame plus resources (FromFrame).
//
// NGX entry points come from one of three places (Api):
//   * our own NGX instance          InitOwn()  (needs the NGX static library)
//   * a game's existing NGX runtime Attach()   (the proxy's original functions)
//   * a test fake                   Attach()
//
// GPU safety: nothing here waits on the GPU. A feature that has to be
// recreated (output size, quality, flags or a larger render size) is retired
// and released only after the host's fences prove the GPU finished EVERY
// evaluation that used it (Gate 2 R-301). Each tracked evaluation has its own
// fence, so work on independent queues can never vouch for another queue
// (Gate 1 R-202). A use whose completion can never be proven (never seen
// submitted, or Signal failed) marks the feature tainted for
// good: it is released only at Shutdown. Every created handle stays owned
// until it is released exactly once (R-305): when no feature slot is free, a
// new feature is refused for that frame instead.
//
// Views (R-303): each source of frames (an FSR/XeSS context, a game view) has
// its own view_id and therefore its own feature and temporal history. Ids must
// never be reused for a different source; ReleaseView() retires a view when its
// source is destroyed. Work recorded on one device is refused on another. If a
// view's evaluations are submitted on different queues, BeforeExecute() makes
// the later submission's queue wait (GPU-side) for the earlier evaluation, so
// one history is never used concurrently (Gate 2 R-303). Frames recorded
// ahead on separate lists use separate features with reset history while the
// predecessor is unsubmitted. This permits reversed submission without racing
// one feature's mutable GPU state. Completion-record exhaustion refuses before
// recording NGX work. Command lists must not be reset/replayed before tracking
// their submission and completion; raw list identity is not a recording epoch.
// Ordering applies per feature: a view that moves to a new feature starts
// from reset and is not ordered against the old one.
// The caller keeps every input/output resource alive until its submission
// completes; NGX does not take references.
//
// Failure policy: any NGX failure latches the backend OFF with a plain-language
// reason; Evaluate then returns false and records nothing further.
#pragma once

#include <d3d12.h>

#include <cstdint>

namespace abundance {
struct Frame;
}

namespace na::dlss {

// Same numeric values as NVSDK_NGX_PerfQuality_Value.
enum class Quality : int {
    MaxPerformance = 0,
    Balanced = 1,
    MaxQuality = 2,
    UltraPerformance = 3,
    UltraQuality = 4,
    DLAA = 5,
};
const char* QualityName(Quality q);
// The quality mode whose scale factor is closest to output/render (DLSS uses
// the exact render size given; the mode only selects its internal model).
// Equal sizes -> DLAA.
Quality QualityForScale(uint32_t render_w, uint32_t render_h, uint32_t out_w, uint32_t out_h);

// NGX entry points, typed opaquely so this header stays SDK-free. Each is the
// matching NVSDK_NGX_D3D12_* function (EvaluateFeature may be the _C variant:
// the progress callback argument is always null).
struct Api {
    void* get_capability_parameters = nullptr; // (NVSDK_NGX_Parameter**)
    void* allocate_parameters = nullptr;       // (NVSDK_NGX_Parameter**)
    void* destroy_parameters = nullptr;        // (NVSDK_NGX_Parameter*)
    void* create_feature = nullptr;            // (list, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**)
    void* evaluate_feature = nullptr;          // (list, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, callback)
    void* release_feature = nullptr;           // (NVSDK_NGX_Handle*)
};

// What the runtime reports for DLSS Super Resolution on this device. This is
// also the per-backend capability record the frame contract consumes (A3).
struct Capabilities {
    bool attached = false;            // an NGX runtime is connected
    bool own_instance = false;        // true when InitOwn() created it
    bool available = false;           // SuperSampling.Available
    bool needs_driver_update = false; // SuperSampling.NeedsUpdatedDriver
    uint32_t min_driver_major = 0, min_driver_minor = 0;
    int32_t feature_init_result = 0;  // SuperSampling.FeatureInitResult (NGX code)
    bool optimal_settings = false;    // the DLSSOptimalSettingsCallback is present
    bool latched_off = false;
    char reason[128] = {};            // plain language when !available or latched
};

// DLSS's answer for one output size and quality mode. Render sizes between
// min and max can be used per frame without recreating the feature (dynamic
// resolution); render extents need no particular alignment.
struct OptimalSettings {
    bool valid = false;
    uint32_t render_w = 0, render_h = 0; // recommended
    uint32_t min_w = 0, min_h = 0, max_w = 0, max_h = 0;
};

struct EvalDesc {
    // Inputs in NON_PIXEL_SHADER_RESOURCE (or PIXEL|NON_PIXEL) state, output
    // in UNORDERED_ACCESS state, all on the device NGX was initialised with.
    ID3D12Resource* color = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motion = nullptr;
    ID3D12Resource* output = nullptr;
    ID3D12Resource* exposure = nullptr; // optional 1x1 exposure texture
    uint32_t render_w = 0, render_h = 0; // valid region of colour/depth (top-left)
    // ARGON-G2-001: largest render size this source will use (dynamic resolution).
    // The feature is created at this size once; per-frame render sizes up to it
    // reuse the feature and its history. 0 = use render_w/render_h.
    uint32_t max_render_w = 0, max_render_h = 0;
    uint32_t out_w = 0, out_h = 0;
    // Jitter in render-space pixels. Convention (measured on RTX hardware,
    // dlss_real_test, 2026-10-24): the colour sample for a pixel was taken at
    // (pixel centre - jitter). Passing the opposite sign blurs the result.
    float jitter_x = 0.f, jitter_y = 0.f;
    // Motion-vector scale (NGX MV.Scale): multiplying the stored motion values by
    // it gives motion in PIXELS OF THE MOTION TEXTURE'S OWN RESOLUTION, i.e.
    // render-resolution pixels when mv_display_res is false (MVLowRes) and
    // output-resolution pixels when it is true. Normalised [-1,1]-style motion
    // therefore uses that texture's width/height (see FromFrame).
    float mv_scale_x = 1.f, mv_scale_y = 1.f;
    float pre_exposure = 1.f;
    float exposure_scale = 1.f; // multiplier applied to colour before upscaling (DLSS.Exposure.Scale)
    uint64_t view_id = 0;       // source of these frames (see Views above); 0 = the default view
    Quality quality = Quality::MaxQuality;
    // DLSS render preset (NVSDK_NGX_DLSS_Hint_Render_Preset_*: 10 J, 11 K, 12 L, 13 M);
    // 0 = NVIDIA's default for the quality mode. Applied when the feature is created.
    uint32_t preset = 0;
    bool reset = false;
    bool hdr = false;            // linear HDR colour (IsHDR)
    bool depth_inverted = false; // reversed-Z
    bool mv_jittered = false;    // motion includes the jitter delta
    bool mv_display_res = false; // motion is at output resolution (MVLowRes off)
    bool auto_exposure = false;  // let DLSS compute exposure (ignored when exposure given)
};

// Returns nullptr when the description is usable, else a plain-language reason.
const char* Validate(const EvalDesc& d);

// GPU resources for one frame, matched to an abundance::Frame by the provider.
struct FrameResources {
    ID3D12Resource* color = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motion = nullptr;
    ID3D12Resource* output = nullptr;
    ID3D12Resource* exposure = nullptr;
    bool depth_inverted = false;
};
// Converts a validated frame-contract Frame into an EvalDesc. Uses the core's
// validate_inputs(Feature::reconstruction) first, so every contract rule
// applies. Returns false with a reason when the frame cannot drive DLSS.
bool FromFrame(const abundance::Frame& f, const FrameResources& r, Quality q, EvalDesc* out, const char** reason);

// Our own NGX instance on `device` (games with no DLSS of their own). Needs a
// build with the NGX static library (NA_HAVE_NGX_LIB); otherwise returns false
// with a reason. `data_path` is a writable folder for NGX logs/cache.
bool InitOwn(ID3D12Device* device, const wchar_t* data_path);
// Connect an already-initialised runtime (game's NGX via the proxy, or a fake).
bool Attach(ID3D12Device* device, const Api& api);
// Releases features and parameter objects (and our NGX instance if we own
// it). The caller guarantees the GPU is idle for every queue that ran DLSS.
void Shutdown();

Capabilities GetCapabilities();
// ARGON-G2-004: which device the backend runs on, and whether that device has
// been removed (the game will recreate it). After removal no GPU work can be
// pending, so Shutdown() is safe and the backend can start again on the new one.
bool IsBoundTo(ID3D12Device* device);
bool BoundDeviceRemoved();
bool QueryOptimal(uint32_t out_w, uint32_t out_h, Quality q, OptimalSettings* out);

// Records DLSS into `list` (open, DIRECT or COMPUTE). Creates or recreates the
// feature on this list when needed. Returns false (and records nothing) on
// invalid input or when latched off; the reason is in GetStats().
bool Evaluate(ID3D12GraphicsCommandList* list, const EvalDesc& d);

// From the host's ExecuteCommandLists path, after the original call returned
// (no waits): signals completion for tracked evaluations in this submission
// and releases retired features whose last use has completed.
void OnExecuted(ID3D12CommandQueue* queue, uint32_t count, ID3D12CommandList* const* lists);

// From the host's ExecuteCommandLists path, BEFORE the original call: adds the
// GPU waits that order a view's evaluations across queues (R-303). No CPU wait.
void BeforeExecute(ID3D12CommandQueue* queue, uint32_t count, ID3D12CommandList* const* lists);

// Hardware session (G2-004): if the bound device was removed, shut DLSS down
// now and release every reference to it (true). D3D12 devices are per-adapter
// singletons: while anything holds a removed device, the game's
// D3D12CreateDevice returns that same removed device, so it could never
// recover. Called from the host's Present path (every game presents, and
// Present is where games learn of the removal) and from OnExecuted.
bool ReleaseIfDeviceRemoved();

// ARGON-G2-004: the one DLSS start-up/recovery path for every producer (FSR,
// XeSS, ...), serialised as a single transaction. Already running on `device`
// -> true. Running on another device -> false, unless that device was removed
// (the game recreated it after a device loss), in which case the backend is
// shut down (nothing can be pending on a removed device) and started on
// `device`. Each device is tried at most once (it is kept alive while
// remembered, so a recycled pointer can never be mistaken for it).
bool EnsureOn(ID3D12Device* device, const wchar_t* data_path);

// Test seams (never set in the product): replace our own NGX start-up and the
// device-removed check.
void SetInitOwnOverrideForTests(bool (*init)(ID3D12Device* device, const wchar_t* data_path));
void SetDeviceRemovedProbeForTests(bool (*removed)(ID3D12Device* device));

// The source of `view_id` is gone (e.g. its FSR/XeSS context was destroyed):
// its feature is retired (released once its uses complete) and the id forgotten.
void ReleaseView(uint64_t view_id);
// Test synchronization point before ReleaseView acquires the backend lock.
void SetReleaseViewHookForTests(void (*hook)());

struct Stats {
    uint64_t evaluates = 0;         // successful EvaluateFeature calls
    uint64_t features_created = 0;
    uint64_t features_released = 0; // retired handles actually released
    uint64_t features_pending = 0;  // retired, waiting for GPU completion of every use
    uint64_t features_leaked = 0;   // retired with a use that can never be proven complete; freed only at Shutdown
    uint64_t rejected = 0;          // Evaluate calls refused by validation (incl. wrong device)
    uint64_t untracked = 0;         // evaluations with no free completion record (taint their feature)
    uint64_t refused_busy = 0;      // frames refused because feature/completion capacity is full
    uint64_t order_violations = 0;  // a view's frame submitted before its previous one: next frame resets
    uint64_t queue_waits = 0;       // GPU waits added to order a view across queues
    uint32_t views_active = 0;
    uint32_t feature_render_w = 0, feature_render_h = 0, feature_out_w = 0, feature_out_h = 0;
    uint32_t feature_flags = 0;
    uint32_t feature_preset = 0; // render preset requested at creation (0 = default)
    int32_t last_ngx_result = 0;
    char reason[128] = {};
};
Stats GetStats();

} // namespace na::dlss
