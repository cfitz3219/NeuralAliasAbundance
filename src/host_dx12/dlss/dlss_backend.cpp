// C6: generic DLSS Super Resolution / DLAA backend. See dlss_backend.hpp.
#include "host_dx12/dlss/dlss_backend.hpp"

#include "core/frame_contract.hpp"

#include <wrl/client.h>

#include <cmath>
#include <cstdio>
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

const char* Validate(const EvalDesc& d)
{
    if (!d.color || !d.depth || !d.motion || !d.output) return "DLSS needs colour, depth, motion and an output texture.";
    if (!d.render_w || !d.render_h || !d.out_w || !d.out_h) return "DLSS was given an empty render or output size.";
    if (d.render_w > d.out_w || d.render_h > d.out_h) return "DLSS cannot make the picture smaller (render larger than output).";
    if (d.quality == Quality::DLAA && (d.render_w != d.out_w || d.render_h != d.out_h))
        return "DLAA needs the render size to equal the output size.";
    if (!std::isfinite(d.jitter_x) || !std::isfinite(d.jitter_y) || std::fabs(d.jitter_x) > 1.f || std::fabs(d.jitter_y) > 1.f)
        return "Camera jitter is missing or out of range (must be within one pixel).";
    if (!std::isfinite(d.mv_scale_x) || !std::isfinite(d.mv_scale_y) || d.mv_scale_x == 0.f || d.mv_scale_y == 0.f)
        return "Motion-vector scale is missing or invalid.";
    if (!std::isfinite(d.pre_exposure) || d.pre_exposure <= 0.f) return "Pre-exposure must be a positive number.";
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

constexpr uint32_t kRecords = 16;            // tracked evaluations awaiting GPU completion
constexpr uint32_t kRetired = 8;             // features waiting to be released
constexpr uint64_t kAbandonCalls = 512;      // OnExecuted calls before an unsubmitted record is given up

enum class RecState : uint8_t { Free, Recorded, Submitted };

struct Record {
    ComPtr<ID3D12Fence> fence; // private timeline (never shared between queues)
    uint64_t next_value = 0, value = 0;
    RecState state = RecState::Free;
    ID3D12CommandList* list = nullptr; // identity only
    uint64_t serial = 0;               // evaluation serial carried by this record
    uint64_t recorded_at = 0;          // OnExecuted counter when recorded
};

struct Retired {
    void* handle = nullptr;
    uint64_t last_serial = 0; // last evaluation that used it (0 = never evaluated)
    bool done = false;        // that evaluation is proven complete
    bool leaked = false;      // completion can never be proven: free only at Shutdown
};

enum class UseState : uint8_t { None, Pending, Done, Untracked };

struct State {
    std::mutex m;
    ComPtr<ID3D12Device> device;
    Api api{};
    bool attached = false, own = false, latched = false;
    void* cap_params = nullptr;  // from GetCapabilityParameters (owned: destroyed at Shutdown)
    void* eval_params = nullptr; // allocated once, reused for create + evaluate
    Capabilities caps{};
    // Current feature.
    void* feature = nullptr;
    uint32_t f_render_w = 0, f_render_h = 0, f_out_w = 0, f_out_h = 0, f_flags = 0;
    Quality f_quality = Quality::MaxQuality;
    uint64_t f_last_serial = 0;
    UseState f_use = UseState::None;
    bool f_needs_reset = false;
    Record recs[kRecords];
    Retired retired[kRetired];
    uint64_t serial = 0, exec_calls = 0;
    Stats stats{};
};
State g;

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

// Evaluation `serial` is now proven complete (done) or can never be proven (lost).
void on_serial(uint64_t serial, bool done)
{
    for (Retired& t : g.retired) {
        if (!t.handle || t.last_serial != serial || t.done || t.leaked) continue;
        if (done) {
            t.done = true;
        } else {
            t.leaked = true;
            ++g.stats.features_leaked;
            if (g.stats.features_pending) --g.stats.features_pending;
        }
    }
    if (g.feature && g.f_last_serial == serial && g.f_use == UseState::Pending)
        g.f_use = done ? UseState::Done : UseState::Untracked;
}

// Frees completed records, abandons stale unsubmitted ones, releases retired
// features whose last use is proven complete. Never waits.
void collect_locked()
{
    for (Record& r : g.recs) {
        if (r.state == RecState::Submitted && r.fence->GetCompletedValue() >= r.value) {
            on_serial(r.serial, true);
            r.state = RecState::Free;
            r.list = nullptr;
        } else if (r.state == RecState::Recorded && g.exec_calls - r.recorded_at > kAbandonCalls) {
            on_serial(r.serial, false); // never seen submitted
            r.state = RecState::Free;
            r.list = nullptr;
        }
    }
    for (Retired& t : g.retired) {
        if (!t.handle || t.leaked || !(t.done || t.last_serial == 0)) continue;
#if defined(NA_HAVE_NGX_SDK)
        release_handle(t.handle);
#endif
        t = Retired{};
        ++g.stats.features_released;
        if (g.stats.features_pending) --g.stats.features_pending;
    }
}

#if defined(NA_HAVE_NGX_SDK)
void retire_current_locked()
{
    if (!g.feature) return;
    for (Retired& t : g.retired) {
        if (t.handle) continue;
        t.handle = g.feature;
        t.last_serial = g.f_last_serial;
        t.done = g.f_use == UseState::Done || g.f_use == UseState::None;
        t.leaked = g.f_use == UseState::Untracked;
        if (t.leaked) ++g.stats.features_leaked; else ++g.stats.features_pending;
        g.feature = nullptr;
        return;
    }
    // No room: never release a handle the GPU might still use. Keep it until Shutdown.
    ++g.stats.features_leaked;
    g.feature = nullptr;
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
    std::lock_guard<std::mutex> lock(g.m);
    if (g.attached) return g.caps.available && !g.latched;
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
    read_caps_locked();
    return g.caps.available;
#else
    set_reason(g.caps.reason, "This build has no NVIDIA DLSS support (SDK headers were not found).");
    return false;
#endif
}

bool InitOwn(ID3D12Device* device, const wchar_t* data_path)
{
#if defined(NA_HAVE_NGX_SDK) && defined(NA_HAVE_NGX_LIB)
    {
        std::lock_guard<std::mutex> lock(g.m);
        if (g.attached) return g.caps.available && !g.latched;
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
    std::lock_guard<std::mutex> lock(g.m);
#if defined(NA_HAVE_NGX_SDK)
    if (g.attached) {
        // Caller guarantees GPU idle: release everything, including leaked handles.
        release_handle(g.feature);
        for (Retired& t : g.retired) release_handle(t.handle);
        if (g.eval_params) reinterpret_cast<PfnDestroy>(g.api.destroy_parameters)(EP());
        if (g.cap_params) reinterpret_cast<PfnDestroy>(g.api.destroy_parameters)(CP());
    }
#if defined(NA_HAVE_NGX_LIB)
    if (g.own && g.device) OwnShutdown(g.device.Get());
#endif
#endif
    g.device.Reset();
    for (Record& r : g.recs) r = Record{};
    for (Retired& t : g.retired) t = Retired{};
    g.api = Api{};
    g.attached = g.own = g.latched = false;
    g.cap_params = g.eval_params = g.feature = nullptr;
    g.f_render_w = g.f_render_h = g.f_out_w = g.f_out_h = g.f_flags = 0;
    g.f_last_serial = 0;
    g.f_use = UseState::None;
    g.f_needs_reset = false;
    g.serial = g.exec_calls = 0;
    g.caps = Capabilities{};
    g.stats = Stats{};
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
    const uint32_t flags = flags_for(d);
    // Dynamic resolution: a smaller render size fits the existing feature.
    const bool fits = g.feature && g.f_out_w == d.out_w && g.f_out_h == d.out_h && g.f_flags == flags &&
                      g.f_quality == d.quality && d.render_w <= g.f_render_w && d.render_h <= g.f_render_h;
    NVSDK_NGX_Parameter* p = EP();
    if (!fits) {
        retire_current_locked();
        collect_locked(); // a retired feature whose last use already completed is released now
        p->Reset();
        p->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
        p->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
        p->Set(NVSDK_NGX_Parameter_Width, d.render_w);
        p->Set(NVSDK_NGX_Parameter_Height, d.render_h);
        p->Set(NVSDK_NGX_Parameter_OutWidth, d.out_w);
        p->Set(NVSDK_NGX_Parameter_OutHeight, d.out_h);
        p->Set(NVSDK_NGX_Parameter_PerfQualityValue, static_cast<int>(d.quality));
        p->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, static_cast<int>(flags));
        p->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);
        NVSDK_NGX_Handle* h = nullptr;
        const NVSDK_NGX_Result r = reinterpret_cast<PfnCreate>(g.api.create_feature)(list, NVSDK_NGX_Feature_SuperSampling, p, &h);
        g.stats.last_ngx_result = static_cast<int32_t>(r);
        if (NVSDK_NGX_FAILED(r) || !h) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "DLSS could not start at %ux%u -> %ux%u (NVIDIA code 0x%08X).", d.render_w,
                          d.render_h, d.out_w, d.out_h, static_cast<unsigned>(r));
            latch(buf);
            return false;
        }
        g.feature = h;
        g.f_render_w = d.render_w;
        g.f_render_h = d.render_h;
        g.f_out_w = d.out_w;
        g.f_out_h = d.out_h;
        g.f_flags = flags;
        g.f_quality = d.quality;
        g.f_last_serial = 0;
        g.f_use = UseState::None;
        g.f_needs_reset = true;
        ++g.stats.features_created;
        g.stats.feature_render_w = d.render_w;
        g.stats.feature_render_h = d.render_h;
        g.stats.feature_out_w = d.out_w;
        g.stats.feature_out_h = d.out_h;
        g.stats.feature_flags = flags;
    }
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
    p->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);
    p->Set(NVSDK_NGX_Parameter_Reset, (d.reset || g.f_needs_reset) ? 1 : 0);
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
    const NVSDK_NGX_Result r = reinterpret_cast<PfnEvaluate>(g.api.evaluate_feature)(list, static_cast<NVSDK_NGX_Handle*>(g.feature), p, nullptr);
    g.stats.last_ngx_result = static_cast<int32_t>(r);
    if (NVSDK_NGX_FAILED(r)) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "DLSS stopped while upscaling (NVIDIA code 0x%08X).", static_cast<unsigned>(r));
        // The failed call may still have recorded work: keep the feature (it is
        // released only at Shutdown or once a later tracked use completes).
        latch(buf);
        return false;
    }
    g.f_needs_reset = false;
    const uint64_t serial = ++g.serial;
    g.f_last_serial = serial;
    g.f_use = UseState::Untracked;
    for (Record& rec : g.recs) {
        if (rec.state != RecState::Free) continue;
        rec.state = RecState::Recorded;
        rec.list = list;
        rec.serial = serial;
        rec.recorded_at = g.exec_calls;
        g.f_use = UseState::Pending;
        break;
    }
    if (g.f_use == UseState::Untracked) ++g.stats.untracked;
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
    if (!queue || !lists || !count) return;
    std::lock_guard<std::mutex> lock(g.m);
    if (!g.attached) return;
    ++g.exec_calls;
    for (Record& rec : g.recs) {
        if (rec.state != RecState::Recorded) continue;
        bool in_submission = false;
        for (uint32_t k = 0; k < count; ++k)
            if (lists[k] == rec.list) { in_submission = true; break; }
        if (!in_submission) continue;
        const uint64_t value = ++rec.next_value;
        if (FAILED(queue->Signal(rec.fence.Get(), value))) {
            // No completion evidence can ever arrive for this evaluation: features
            // it used are treated as leaked, never released early.
            on_serial(rec.serial, false);
            rec.state = RecState::Free;
            rec.list = nullptr;
            continue;
        }
        rec.value = value;
        rec.state = RecState::Submitted;
    }
    collect_locked();
}

Stats GetStats()
{
    std::lock_guard<std::mutex> lock(g.m);
    return g.stats;
}

} // namespace na::dlss
