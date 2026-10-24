// C6: generic DLSS Super Resolution / DLAA backend. See dlss_backend.hpp.
#include "host_dx12/dlss/dlss_backend.hpp"
#include "host_dx12/dlss/call_origin.hpp"

#include "core/frame_contract.hpp"

#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <atomic>
#include <cstring>
#include <mutex>

#if defined(NA_HAVE_NGX_SDK)
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_params.h>
#endif

using Microsoft::WRL::ComPtr;

namespace na::dlss {

const char* QualityName(Quality q)
{
    switch (q) {
    case Quality::MaxPerformance: return "Performance";
    case Quality::Balanced: return "Balanced";
    case Quality::MaxQuality: return "Quality";
    case Quality::UltraPerformance: return "Ultra Performance";
    case Quality::UltraQuality: return "Ultra Quality";
    case Quality::DLAA: return "DLAA";
    }
    return "Unknown";
}

Quality QualityForScale(uint32_t render_w, uint32_t render_h, uint32_t out_w, uint32_t out_h)
{
    if (!render_w || !render_h) return Quality::MaxQuality;
    if (render_w == out_w && render_h == out_h) return Quality::DLAA;
    const double ratio = static_cast<double>(out_w) / static_cast<double>(render_w);
    // Nominal DLSS ratios: Quality 1.5, Balanced 1.72, Performance 2.0, Ultra Performance 3.0.
    if (ratio < 1.6) return Quality::MaxQuality;
    if (ratio < 1.85) return Quality::Balanced;
    if (ratio < 2.5) return Quality::MaxPerformance;
    return Quality::UltraPerformance;
}

const char* Validate(const EvalDesc& d)
{
    if (!d.color || !d.depth || !d.motion || !d.output) return "DLSS needs colour, depth, motion and an output texture.";
    if (!d.render_w || !d.render_h || !d.out_w || !d.out_h) return "DLSS was given an empty render or output size.";
    if (d.render_w > d.out_w || d.render_h > d.out_h) return "DLSS cannot make the picture smaller (render larger than output).";
    if ((d.max_render_w && d.max_render_w < d.render_w) || (d.max_render_h && d.max_render_h < d.render_h))
        return "This frame is larger than the game's stated maximum render size.";
    if (d.max_render_w > d.out_w || d.max_render_h > d.out_h) return "The maximum render size is larger than the output.";
    if (d.quality == Quality::DLAA && (d.render_w != d.out_w || d.render_h != d.out_h))
        return "DLAA needs the render size to equal the output size.";
    if (!std::isfinite(d.jitter_x) || !std::isfinite(d.jitter_y) || std::fabs(d.jitter_x) > 1.f || std::fabs(d.jitter_y) > 1.f)
        return "Camera jitter is missing or out of range (must be within one pixel).";
    if (!std::isfinite(d.mv_scale_x) || !std::isfinite(d.mv_scale_y) || d.mv_scale_x == 0.f || d.mv_scale_y == 0.f)
        return "Motion-vector scale is missing or invalid.";
    if (!std::isfinite(d.pre_exposure) || d.pre_exposure <= 0.f) return "Pre-exposure must be a positive number.";
    if (!std::isfinite(d.exposure_scale) || d.exposure_scale <= 0.f) return "Exposure scale must be a positive number.";
    if (static_cast<int>(d.quality) < 0 || static_cast<int>(d.quality) > 5) return "Unknown DLSS quality mode.";
    return nullptr;
}

bool FromFrame(const abundance::Frame& f, const FrameResources& r, Quality q, EvalDesc* out, const char** reason)
{
    static thread_local char buf[128];
    const auto fail = [&](const char* why) {
        if (reason) *reason = why;
        return false;
    };
    if (!out) return fail("No output description.");
    const abundance::Decision v = abundance::validate_inputs(f, abundance::Feature::reconstruction);
    if (!v.ready) {
        std::snprintf(buf, sizeof(buf), "%s", v.reason.c_str());
        return fail(buf);
    }
    if (f.color_space == abundance::ColorSpace::srgb_sdr)
        return fail("DLSS needs linear colour; this frame's colour is display-encoded (sRGB).");
    EvalDesc d;
    d.color = r.color;
    d.depth = r.depth;
    d.motion = r.motion;
    d.output = r.output;
    d.exposure = r.exposure;
    d.render_w = f.color.extent.width;
    d.render_h = f.color.extent.height;
    d.out_w = f.output.width;
    d.out_h = f.output.height;
    d.jitter_x = f.jitter_x;
    d.jitter_y = f.jitter_y;
    d.mv_display_res = f.motion.extent == f.output && !(f.motion.extent == f.color.extent);
    // Contract motion is current->previous. Pixels pass through; normalised
    // values become pixels of the motion texture's own extent.
    if (f.motion_space == abundance::MotionSpace::normalized_current_to_previous) {
        d.mv_scale_x = static_cast<float>(f.motion.extent.width);
        d.mv_scale_y = static_cast<float>(f.motion.extent.height);
    }
    d.mv_jittered = !f.motion_excludes_jitter; // validate_inputs requires excludes_jitter today
    d.hdr = f.color_space == abundance::ColorSpace::linear_hdr;
    d.auto_exposure = r.exposure == nullptr;
    d.depth_inverted = r.depth_inverted;
    d.reset = v.reset;
    d.quality = q;
    if (const char* bad = Validate(d)) return fail(bad);
    *out = d;
    if (reason) *reason = nullptr;
    return true;
}

namespace {

#if defined(NA_HAVE_NGX_SDK)
using PfnParams = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
using PfnDestroy = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);
using PfnCreate = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using PfnEvaluate = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, void*);
using PfnRelease = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
#endif

constexpr uint32_t kRecords = 32;        // tracked evaluations awaiting GPU completion
constexpr uint32_t kFeatures = 16;       // feature handles owned at once (current + retired)
constexpr uint32_t kViews = 8;           // independent frame sources
constexpr uint64_t kAbandonCalls = 512;  // OnExecuted calls before an unsubmitted record is given up

enum class RecState : uint8_t { Free, Recorded, Submitted };

struct Record {
    ComPtr<ID3D12Fence> fence; // private timeline (never shared between queues)
    uint64_t next_value = 0, value = 0;
    RecState state = RecState::Free;
    ID3D12CommandList* list = nullptr; // identity only
    int feature = -1;                  // feature slot this evaluation used
    int view = -1;                     // view slot it belongs to
    uint64_t recorded_at = 0;          // OnExecuted counter when recorded
    uint64_t gen = 0;                  // unique per use of this slot
    ID3D12CommandQueue* queue = nullptr; // identity: where it was submitted
    // R-303: the same view's previous evaluation on the same feature (record
    // slot + generation). BeforeExecute orders this one after it on the GPU.
    int pred = -1;
    uint64_t pred_gen = 0;
};

// R-301/R-305: every created handle lives in exactly one slot until released.
struct Feature {
    void* handle = nullptr;
    bool retired = false;      // no longer current for any view
    uint32_t outstanding = 0;  // tracked evaluations not yet proven complete
    bool tainted = false;      // some use can never be proven complete: release only at Shutdown
    uint32_t render_w = 0, render_h = 0, out_w = 0, out_h = 0, flags = 0;
    Quality quality = Quality::MaxQuality;
};

struct View {
    bool used = false;
    uint64_t id = 0;
    int feature = -1;
    bool needs_reset = true;
    int last_rec = -1;          // record of its most recent evaluation (R-303)
    uint64_t last_rec_gen = 0;
};

struct State {
    std::mutex m;
    ComPtr<ID3D12Device> device;
    Api api{};
    bool attached = false, own = false, latched = false;
    void* cap_params = nullptr;  // from GetCapabilityParameters (owned: destroyed at Shutdown)
    void* eval_params = nullptr; // allocated once, reused for create + evaluate
    Capabilities caps{};
    Feature features[kFeatures];
    View views[kViews];
    Record recs[kRecords];
    uint64_t exec_calls = 0;
    uint64_t rec_gen = 0;
    Stats stats{};
};
State g;
std::recursive_mutex g_life_m; // serializes all public lifecycle mutations
ComPtr<ID3D12Device> g_tried_device;
std::atomic<void (*)()> g_release_view_hook{nullptr};
std::atomic<bool> g_any_attached{false}; // hot-path filter for OnExecuted (every game submission)

void set_reason(char* dst, const char* text) { std::snprintf(dst, 128, "%s", text ? text : ""); }

#if defined(NA_HAVE_NGX_SDK)
void latch(const char* why)
{
    g.latched = true;
    g.caps.latched_off = true;
    set_reason(g.caps.reason, why);
    set_reason(g.stats.reason, why);
}

NVSDK_NGX_Parameter* EP() { return static_cast<NVSDK_NGX_Parameter*>(g.eval_params); }
NVSDK_NGX_Parameter* CP() { return static_cast<NVSDK_NGX_Parameter*>(g.cap_params); }

void release_handle(void* h)
{
    if (h && g.api.release_feature) reinterpret_cast<PfnRelease>(g.api.release_feature)(static_cast<NVSDK_NGX_Handle*>(h));
}
#endif

void free_record(Record& r)
{
    ComPtr<ID3D12Fence> fence = std::move(r.fence);
    const uint64_t next = r.next_value;
    r = Record{};
    r.fence = std::move(fence);
    r.next_value = next;
}

// A tracked use of feature slot `f` finished (done) or can never be proven (lost).
void use_ended(int f, bool done)
{
    if (f < 0) return;
    Feature& ft = g.features[f];
    if (ft.outstanding) --ft.outstanding;
    if (!done) ft.tainted = true; // sticky until Shutdown
}

// Frees completed records, abandons stale unsubmitted ones, releases retired
// features none of whose uses can still be running. Never waits.
void collect_locked()
{
    for (Record& r : g.recs) {
        if (r.state == RecState::Submitted && r.fence->GetCompletedValue() != UINT64_MAX && r.fence->GetCompletedValue() >= r.value) {
            use_ended(r.feature, true);
            free_record(r);
        } else if (r.state == RecState::Recorded && g.exec_calls - r.recorded_at > kAbandonCalls) {
            use_ended(r.feature, false); // never seen submitted
            free_record(r);
        }
    }
    for (Feature& f : g.features) {
        if (!f.handle || !f.retired || f.tainted || f.outstanding) continue;
#if defined(NA_HAVE_NGX_SDK)
        release_handle(f.handle);
#endif
        f = Feature{};
        ++g.stats.features_released;
    }
}

void retire_feature_locked(int f)
{
    if (f >= 0 && g.features[f].handle) g.features[f].retired = true;
}

int find_view_locked(uint64_t id, bool create)
{
    for (int i = 0; i < static_cast<int>(kViews); ++i)
        if (g.views[i].used && g.views[i].id == id) return i;
    if (!create) return -1;
    for (int i = 0; i < static_cast<int>(kViews); ++i)
        if (!g.views[i].used) {
            g.views[i] = View{};
            g.views[i].used = true;
            g.views[i].id = id;
            return i;
        }
    return -1;
}

#if defined(NA_HAVE_NGX_SDK)
int free_feature_slot_locked()
{
    for (int i = 0; i < static_cast<int>(kFeatures); ++i)
        if (!g.features[i].handle) return i;
    return -1;
}

uint32_t flags_for(const EvalDesc& d)
{
    uint32_t f = 0;
    if (d.hdr) f |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    if (!d.mv_display_res) f |= NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    if (d.mv_jittered) f |= NVSDK_NGX_DLSS_Feature_Flags_MVJittered;
    if (d.depth_inverted) f |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    if (d.auto_exposure && !d.exposure) f |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    return f;
}
#endif

#if defined(NA_HAVE_NGX_SDK)
void read_caps_locked()
{
    Capabilities& c = g.caps;
    NVSDK_NGX_Parameter* p = CP();
    if (!p) {
        set_reason(c.reason, "The NVIDIA runtime gave no DLSS capability information.");
        return;
    }
    int avail = 0, needs = 0;
    unsigned int major = 0, minor = 0;
    int init_result = 0;
    p->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &avail);
    p->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs);
    p->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &major);
    p->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minor);
    p->Get(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &init_result);
    void* cb = nullptr;
    p->Get(NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback, &cb);
    c.available = avail != 0;
    c.needs_driver_update = needs != 0;
    c.min_driver_major = major;
    c.min_driver_minor = minor;
    c.feature_init_result = init_result;
    c.optimal_settings = cb != nullptr;
    if (c.needs_driver_update) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "DLSS needs a newer NVIDIA driver (version %u.%u or later).", major, minor);
        set_reason(c.reason, buf);
    } else if (!c.available) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "DLSS is not available on this GPU or install (NVIDIA code 0x%08X).",
                      static_cast<unsigned>(init_result));
        set_reason(c.reason, buf);
    } else {
        set_reason(c.reason, "");
    }
}
#endif

} // namespace

#if defined(NA_HAVE_NGX_SDK) && defined(NA_HAVE_NGX_LIB)
bool OwnInit(ID3D12Device* device, const wchar_t* data_path, Api* api, char* reason); // dlss_own_ngx.cpp
void OwnShutdown(ID3D12Device* device);
#endif

bool Attach(ID3D12Device* device, const Api& api)
{
    std::lock_guard<std::recursive_mutex> life(g_life_m);
    std::lock_guard<std::mutex> lock(g.m);
    if (g.attached) return g.device.Get() == device && g.caps.available && !g.latched;
    g.caps = Capabilities{};
    if (!device || !api.get_capability_parameters || !api.allocate_parameters || !api.destroy_parameters ||
        !api.create_feature || !api.evaluate_feature || !api.release_feature) {
        set_reason(g.caps.reason, "The NVIDIA DLSS runtime is incomplete (missing entry points).");
        return false;
    }
#if defined(NA_HAVE_NGX_SDK)
    g.device = device;
    g.api = api;
    NVSDK_NGX_Parameter* cap = nullptr;
    NVSDK_NGX_Result r = reinterpret_cast<PfnParams>(api.get_capability_parameters)(&cap);
    if (NVSDK_NGX_FAILED(r) || !cap) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "The NVIDIA runtime refused to describe DLSS (code 0x%08X).", static_cast<unsigned>(r));
        set_reason(g.caps.reason, buf);
        g.stats.last_ngx_result = static_cast<int32_t>(r);
        g.device.Reset();
        g.api = Api{};
        return false;
    }
    g.cap_params = cap;
    NVSDK_NGX_Parameter* ep = nullptr;
    r = reinterpret_cast<PfnParams>(api.allocate_parameters)(&ep);
    if (NVSDK_NGX_FAILED(r) || !ep) {
        reinterpret_cast<PfnDestroy>(api.destroy_parameters)(cap);
        g.cap_params = nullptr;
        char buf[128];
        std::snprintf(buf, sizeof(buf), "DLSS could not allocate its settings object (code 0x%08X).", static_cast<unsigned>(r));
        set_reason(g.caps.reason, buf);
        g.stats.last_ngx_result = static_cast<int32_t>(r);
        g.device.Reset();
        g.api = Api{};
        return false;
    }
    g.eval_params = ep;
    for (Record& rec : g.recs) {
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&rec.fence)))) {
            set_reason(g.caps.reason, "Could not create the GPU fences DLSS needs for safe cleanup.");
            reinterpret_cast<PfnDestroy>(api.destroy_parameters)(ep);
            reinterpret_cast<PfnDestroy>(api.destroy_parameters)(cap);
            g.eval_params = g.cap_params = nullptr;
            for (Record& x : g.recs) x.fence.Reset();
            g.device.Reset();
            g.api = Api{};
            return false;
        }
    }
    g.attached = true;
    g.caps.attached = true;
    g_any_attached.store(true, std::memory_order_release);
    read_caps_locked();
    return g.caps.available;
#else
    set_reason(g.caps.reason, "This build has no NVIDIA DLSS support (SDK headers were not found).");
    return false;
#endif
}

bool InitOwn(ID3D12Device* device, const wchar_t* data_path)
{
    std::lock_guard<std::recursive_mutex> life(g_life_m);
#if defined(NA_HAVE_NGX_SDK) && defined(NA_HAVE_NGX_LIB)
    {
        std::lock_guard<std::mutex> lock(g.m);
        if (g.attached) return g.device.Get() == device && g.caps.available && !g.latched;
    }
    Api api;
    char reason[128] = {};
    if (!OwnInit(device, data_path, &api, reason)) {
        std::lock_guard<std::mutex> lock(g.m);
        set_reason(g.caps.reason, reason);
        return false;
    }
    const bool ok = Attach(device, api);
    std::lock_guard<std::mutex> lock(g.m);
    if (g.attached) {
        g.own = true;
        g.caps.own_instance = true;
    } else {
        OwnShutdown(device);
    }
    return ok;
#else
    (void)device;
    (void)data_path;
    std::lock_guard<std::mutex> lock(g.m);
    set_reason(g.caps.reason, "This build cannot start DLSS on its own (NVIDIA NGX library not included).");
    return false;
#endif
}

void Shutdown()
{
    std::lock_guard<std::recursive_mutex> life(g_life_m);
    g_tried_device.Reset();
    std::lock_guard<std::mutex> lock(g.m);
#if defined(NA_HAVE_NGX_SDK)
    if (g.attached) {
        // Caller guarantees GPU idle: release every owned handle exactly once,
        // tainted ones included.
        for (Feature& f : g.features)
            if (f.handle) {
                release_handle(f.handle);
                ++g.stats.features_released;
            }
        if (g.eval_params) reinterpret_cast<PfnDestroy>(g.api.destroy_parameters)(EP());
        if (g.cap_params) reinterpret_cast<PfnDestroy>(g.api.destroy_parameters)(CP());
    }
#if defined(NA_HAVE_NGX_LIB)
    if (g.own && g.device) OwnShutdown(g.device.Get());
#endif
#endif
    g_any_attached.store(false, std::memory_order_release);
    g.device.Reset();
    for (Record& r : g.recs) r = Record{};
    for (Feature& f : g.features) f = Feature{};
    for (View& v : g.views) v = View{};
    g.api = Api{};
    g.attached = g.own = g.latched = false;
    g.cap_params = g.eval_params = nullptr;
    g.exec_calls = 0;
    g.caps = Capabilities{};
    g.stats = Stats{};
}

bool IsBoundTo(ID3D12Device* device)
{
    std::lock_guard<std::mutex> lock(g.m);
    return g.attached && device && g.device.Get() == device;
}

bool BoundDeviceRemoved()
{
    std::lock_guard<std::mutex> lock(g.m);
    return g.attached && g.device && g.device->GetDeviceRemovedReason() != S_OK;
}

Capabilities GetCapabilities()
{
    std::lock_guard<std::mutex> lock(g.m);
    return g.caps;
}

bool QueryOptimal(uint32_t out_w, uint32_t out_h, Quality q, OptimalSettings* out)
{
    if (!out) return false;
    *out = OptimalSettings{};
    std::lock_guard<std::mutex> lock(g.m);
#if defined(NA_HAVE_NGX_SDK)
    if (!g.attached || !g.caps.available || !out_w || !out_h) return false;
    if (q == Quality::DLAA) { // native resolution by definition
        *out = OptimalSettings{true, out_w, out_h, out_w, out_h, out_w, out_h};
        return true;
    }
    NVSDK_NGX_Parameter* p = CP();
    void* cb = nullptr;
    if (p->Get(NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback, &cb) != NVSDK_NGX_Result_Success || !cb) return false;
    p->Set(NVSDK_NGX_Parameter_Width, out_w);
    p->Set(NVSDK_NGX_Parameter_Height, out_h);
    p->Set(NVSDK_NGX_Parameter_PerfQualityValue, static_cast<int>(q));
    p->Set(NVSDK_NGX_Parameter_RTXValue, 0);
    const NVSDK_NGX_Result r = reinterpret_cast<PFN_NVSDK_NGX_DLSS_GetOptimalSettingsCallback>(cb)(p);
    if (NVSDK_NGX_FAILED(r)) return false;
    unsigned int w = 0, h = 0;
    p->Get(NVSDK_NGX_Parameter_OutWidth, &w);
    p->Get(NVSDK_NGX_Parameter_OutHeight, &h);
    if (!w || !h) return false; // mode not offered at this size
    OptimalSettings s{true, w, h, w, h, w, h};
    p->Get(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width, &s.min_w);
    p->Get(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height, &s.min_h);
    p->Get(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width, &s.max_w);
    p->Get(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height, &s.max_h);
    *out = s;
    return true;
#else
    (void)out_w;
    (void)out_h;
    (void)q;
    return false;
#endif
}

bool Evaluate(ID3D12GraphicsCommandList* list, const EvalDesc& d)
{
    std::lock_guard<std::mutex> lock(g.m);
    if (!list) return false;
    if (const char* bad = Validate(d)) {
        ++g.stats.rejected;
        set_reason(g.stats.reason, bad);
        return false;
    }
#if defined(NA_HAVE_NGX_SDK)
    if (!g.attached) {
        set_reason(g.stats.reason, g.caps.reason[0] ? g.caps.reason : "DLSS is not connected to an NVIDIA runtime.");
        return false;
    }
    if (g.latched) return false;
    if (!g.caps.available) {
        set_reason(g.stats.reason, g.caps.reason);
        return false;
    }
    collect_locked();
    const D3D12_COMMAND_LIST_TYPE type = list->GetType();
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COMPUTE) {
        ++g.stats.rejected;
        set_reason(g.stats.reason, "DLSS must be recorded on a graphics or compute command list.");
        return false;
    }
    {
        ComPtr<ID3D12Device> dev;
        if (FAILED(list->GetDevice(IID_PPV_ARGS(&dev))) || dev.Get() != g.device.Get()) {
            ++g.stats.rejected;
            set_reason(g.stats.reason, "This frame comes from a different graphics device than DLSS was started on.");
            return false;
        }
    }
    const int vi = find_view_locked(d.view_id, true);
    if (vi < 0) {
        ++g.stats.rejected;
        set_reason(g.stats.reason, "Too many separate views are using DLSS at once.");
        return false;
    }
    View& view = g.views[vi];
    const uint32_t flags = flags_for(d);
    // Dynamic resolution: a smaller render size fits the view's existing feature.
    const Feature* cur = view.feature >= 0 ? &g.features[view.feature] : nullptr;
    // ARGON-G2-001: create at the source's maximum render size, so dynamic
    // resolution anywhere up to it keeps the same feature and history.
    const uint32_t create_w = d.max_render_w ? d.max_render_w : d.render_w;
    const uint32_t create_h = d.max_render_h ? d.max_render_h : d.render_h;
    bool fits = cur && !cur->tainted && cur->out_w == d.out_w && cur->out_h == d.out_h && cur->flags == flags &&
                      cur->quality == d.quality && create_w <= cur->render_w && create_h <= cur->render_h;
    // A submitted predecessor supplies a fence dependency. A predecessor on
    // this same list is ordered by the list itself. Separate unsubmitted lists
    // may execute in either order: give the new recording independent history
    // NOW, before emitting NGX work. Resetting a future frame cannot repair a
    // concurrent use of one feature's internal resources.
    int pred = -1;
    uint64_t pred_gen = 0;
    if (fits && view.last_rec >= 0) {
        const Record& pr = g.recs[view.last_rec];
        if (pr.gen == view.last_rec_gen && pr.state != RecState::Free && pr.feature == view.feature) {
            if (pr.state == RecState::Recorded && pr.list != list) {
                fits = false;
            } else {
                pred = view.last_rec;
                pred_gen = pr.gen;
            }
        }
    }
    // Reserve completion capacity before NGX can record any GPU commands.
    bool record_available = false;
    for (const Record& rec : g.recs) record_available |= rec.state == RecState::Free;
    if (!record_available) {
        ++g.stats.refused_busy;
        set_reason(g.stats.reason, "DLSS completion tracking is full; this frame uses the game's upscaler.");
        return false;
    }
    NVSDK_NGX_Parameter* p = EP();
    if (!fits) {
        // R-305: secure a slot BEFORE giving up the current feature, so no handle is ever lost.
        const int slot = free_feature_slot_locked();
        if (slot < 0) {
            ++g.stats.refused_busy;
            set_reason(g.stats.reason, "Older DLSS work is still on the GPU; this frame is left to the game's own upscaler.");
            return false;
        }
        p->Reset();
        p->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
        p->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
        p->Set(NVSDK_NGX_Parameter_Width, create_w);
        p->Set(NVSDK_NGX_Parameter_Height, create_h);
        p->Set(NVSDK_NGX_Parameter_OutWidth, d.out_w);
        p->Set(NVSDK_NGX_Parameter_OutHeight, d.out_h);
        p->Set(NVSDK_NGX_Parameter_PerfQualityValue, static_cast<int>(d.quality));
        p->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, static_cast<int>(flags));
        p->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);
        NVSDK_NGX_Handle* h = nullptr;
        const InjectorCallScope origin;
        const NVSDK_NGX_Result r = reinterpret_cast<PfnCreate>(g.api.create_feature)(list, NVSDK_NGX_Feature_SuperSampling, p, &h);
        g.stats.last_ngx_result = static_cast<int32_t>(r);
        if (NVSDK_NGX_FAILED(r) || !h) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "DLSS could not start at %ux%u -> %ux%u (NVIDIA code 0x%08X).", d.render_w,
                          d.render_h, d.out_w, d.out_h, static_cast<unsigned>(r));
            latch(buf);
            return false;
        }
        Feature& nf = g.features[slot];
        nf = Feature{};
        nf.handle = h;
        nf.render_w = create_w;
        nf.render_h = create_h;
        nf.out_w = d.out_w;
        nf.out_h = d.out_h;
        nf.flags = flags;
        nf.quality = d.quality;
        retire_feature_locked(view.feature); // released once every use of it has completed
        view.feature = slot;
        view.needs_reset = true;
        ++g.stats.features_created;
        g.stats.feature_render_w = create_w;
        g.stats.feature_render_h = create_h;
        g.stats.feature_out_w = d.out_w;
        g.stats.feature_out_h = d.out_h;
        g.stats.feature_flags = flags;
        collect_locked(); // the retired feature may already be free of uses
    }
    Feature& ft = g.features[view.feature];
    // Every key is written every frame: the object is shared with creation and
    // stale optional resources must never leak into a later evaluation.
    p->Set(NVSDK_NGX_Parameter_Color, d.color);
    p->Set(NVSDK_NGX_Parameter_Depth, d.depth);
    p->Set(NVSDK_NGX_Parameter_MotionVectors, d.motion);
    p->Set(NVSDK_NGX_Parameter_Output, d.output);
    p->Set(NVSDK_NGX_Parameter_ExposureTexture, d.exposure);
    p->Set(NVSDK_NGX_Parameter_TransparencyMask, static_cast<ID3D12Resource*>(nullptr));
    p->Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, static_cast<ID3D12Resource*>(nullptr));
    p->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, d.jitter_x);
    p->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, d.jitter_y);
    p->Set(NVSDK_NGX_Parameter_MV_Scale_X, d.mv_scale_x);
    p->Set(NVSDK_NGX_Parameter_MV_Scale_Y, d.mv_scale_y);
    p->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, d.pre_exposure);
    p->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, d.exposure_scale);
    p->Set(NVSDK_NGX_Parameter_Reset, (d.reset || view.needs_reset) ? 1 : 0);
    p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, d.render_w);
    p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, d.render_h);
    p->Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, 0u);
    p->Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, 0u);
    p->Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, 0u);
    p->Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, 0u);
    p->Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, 0u);
    p->Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, 0u);
    p->Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 0u);
    p->Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, 0u);
    const NVSDK_NGX_Result r = reinterpret_cast<PfnEvaluate>(g.api.evaluate_feature)(list, static_cast<NVSDK_NGX_Handle*>(ft.handle), p, nullptr);
    g.stats.last_ngx_result = static_cast<int32_t>(r);
    if (NVSDK_NGX_FAILED(r)) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "DLSS stopped while upscaling (NVIDIA code 0x%08X).", static_cast<unsigned>(r));
        // The failed call may still have recorded work: its use can't be tracked.
        ft.tainted = true;
        latch(buf);
        return false;
    }
    view.needs_reset = false;
    bool tracked = false;
    for (int ri = 0; ri < static_cast<int>(kRecords); ++ri) {
        Record& rec = g.recs[ri];
        if (rec.state != RecState::Free) continue;
        rec.state = RecState::Recorded;
        rec.list = list;
        rec.feature = view.feature;
        rec.view = vi;
        rec.recorded_at = g.exec_calls;
        rec.gen = ++g.rec_gen;
        rec.pred = pred;
        rec.pred_gen = pred_gen;
        view.last_rec = ri;
        view.last_rec_gen = rec.gen;
        ++ft.outstanding;
        tracked = true;
        break;
    }
    if (!tracked) {
        // R-301: an untracked use is never forgotten; R-303: its order can't be
        // enforced either, so the view's next frame gets a separate feature.
        ft.tainted = true;
        retire_feature_locked(view.feature);
        view.feature = -1;
        view.last_rec = -1;
        ++g.stats.untracked;
    }
    ++g.stats.evaluates;
    set_reason(g.stats.reason, "");
    return true;
#else
    set_reason(g.stats.reason, "This build has no NVIDIA DLSS support (SDK headers were not found).");
    return false;
#endif
}

void OnExecuted(ID3D12CommandQueue* queue, uint32_t count, ID3D12CommandList* const* lists)
{
    if (!queue || !lists || !count || !g_any_attached.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(g.m);
    if (!g.attached) return;
    ++g.exec_calls;
    for (Record& rec : g.recs) {
        if (rec.state != RecState::Recorded) continue;
        bool in_submission = false;
        for (uint32_t k = 0; k < count; ++k)
            if (lists[k] == rec.list) { in_submission = true; break; }
        if (!in_submission) continue;
        rec.queue = queue;
        const uint64_t value = ++rec.next_value;
        if (FAILED(queue->Signal(rec.fence.Get(), value))) {
            use_ended(rec.feature, false); // completion can never be proven
            free_record(rec);
            continue;
        }
        rec.value = value;
        rec.state = RecState::Submitted;
    }
    collect_locked();
}

void BeforeExecute(ID3D12CommandQueue* queue, uint32_t count, ID3D12CommandList* const* lists)
{
    if (!queue || !lists || !count || !g_any_attached.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(g.m);
    if (!g.attached) return;
    const auto index_of = [&](ID3D12CommandList* l) -> int {
        for (uint32_t k = 0; k < count; ++k)
            if (lists[k] == l) return static_cast<int>(k);
        return -1;
    };
    for (Record& rec : g.recs) {
        if (rec.state != RecState::Recorded || rec.pred < 0) continue;
        const int self = index_of(rec.list);
        if (self < 0) continue;
        const Record& pr = g.recs[rec.pred];
        const bool live = pr.gen == rec.pred_gen && pr.state != RecState::Free;
        rec.pred = -1;
        if (!live) continue; // the previous evaluation already completed
        if (pr.state == RecState::Submitted) {
            // Same queue: already ordered. Finished: nothing to wait for.
            if (pr.queue != queue && pr.fence->GetCompletedValue() < pr.value &&
                SUCCEEDED(queue->Wait(pr.fence.Get(), pr.value)))
                ++g.stats.queue_waits;
            continue;
        }
        // Still unsubmitted: fine when it runs earlier in this same submission
        // (or on the same list). Otherwise the game submitted this view's
        // frames out of order; nothing recorded can be reordered now, so the
        // view's next frame restarts its history.
        const int at = index_of(pr.list);
        if (at >= 0 && at <= self) continue;
        ++g.stats.order_violations;
        if (rec.view >= 0) g.views[rec.view].needs_reset = true;
    }
}

namespace {
std::atomic<bool (*)(ID3D12Device*, const wchar_t*)> g_init_override{nullptr};
std::atomic<bool (*)(ID3D12Device*)> g_removed_probe{nullptr};
bool device_removed(ID3D12Device* d)
{
    if (auto probe = g_removed_probe.load(std::memory_order_acquire)) return probe(d);
    return d && d->GetDeviceRemovedReason() != S_OK;
}
} // namespace

void SetInitOwnOverrideForTests(bool (*init)(ID3D12Device*, const wchar_t*)) { g_init_override.store(init); }
void SetDeviceRemovedProbeForTests(bool (*removed)(ID3D12Device*)) { g_removed_probe.store(removed); }

bool EnsureOn(ID3D12Device* device, const wchar_t* data_path)
{
    if (!device) return false;
    std::lock_guard<std::recursive_mutex> life(g_life_m);
    ComPtr<ID3D12Device> bound;
    bool available = false;
    {
        std::lock_guard<std::mutex> lock(g.m);
        if (g.attached) {
            bound = g.device;
            available = g.caps.available && !g.latched;
        }
    }
    if (bound) {
        const bool removed = device_removed(bound.Get());
        if (bound.Get() == device && !removed) return available;
        if (!removed) return false; // another live device owns DLSS
        Shutdown();                                     // nothing can be pending on a removed device
        g_tried_device.Reset();
    }
    if (g_tried_device.Get() == device) return false; // already failed on this device
    g_tried_device = device;
    if (auto init = g_init_override.load(std::memory_order_acquire)) return init(device, data_path);
    return InitOwn(device, data_path);
}

void SetReleaseViewHookForTests(void (*hook)()) { g_release_view_hook.store(hook); }

void ReleaseView(uint64_t view_id)
{
    if (auto hook = g_release_view_hook.load()) hook();
    std::lock_guard<std::mutex> lock(g.m);
    const int vi = find_view_locked(view_id, false);
    if (vi < 0) return;
    retire_feature_locked(g.views[vi].feature);
    for (Record& r : g.recs)
        if (r.view == vi) r.view = -1;
    g.views[vi] = View{};
    collect_locked();
}

Stats GetStats()
{
    std::lock_guard<std::mutex> lock(g.m);
    Stats s = g.stats;
    s.features_pending = s.features_leaked = 0;
    for (const Feature& f : g.features) {
        if (!f.handle || !f.retired) continue;
        if (f.tainted) ++s.features_leaked; else ++s.features_pending;
    }
    s.views_active = 0;
    for (const View& v : g.views) s.views_active += v.used ? 1u : 0u;
    return s;
}

} // namespace na::dlss
