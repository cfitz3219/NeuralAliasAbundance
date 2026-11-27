// C6: generic DLSS Super Resolution / DLAA backend. See dlss_backend.hpp.
#include "host_dx12/dlss/dlss_backend.hpp"
#include "host_dx12/diag/gpu_breadcrumbs.hpp"
#include "host_dx12/diag/dred.hpp"
#include "host_dx12/dlss/call_origin.hpp"

#include "core/frame_contract.hpp"

#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <atomic>
#include <cstring>
#include <mutex>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <vector>
#include <array>

#include "host_dx12/diag/event_log.hpp"
#include "host_dx12/diag/vram_ledger.hpp"
#include "host_dx12/diag/release_trace.hpp"

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
    // NGX's HDR flag selects linear-light processing, not whether the monitor
    // is HDR. The NVIDIA guide accepts sRGB/gamma LDR with IsHDR clear, and
    // requires IsHDR for linear input (including linear SDR values <= 1).
    d.hdr = f.color_space != abundance::ColorSpace::srgb_sdr;
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
    ComPtr<ID3D12Fence> after; // the feature's off-frame creation, waited for on the GPU before this runs
    uint64_t after_value = 0;
};

// R-301/R-305: every created handle lives in exactly one slot until released.
struct Feature {
    void* handle = nullptr;
    bool retired = false;      // no longer current for any view
    uint32_t outstanding = 0;  // tracked evaluations not yet proven complete
    bool tainted = false;      // some use can never be proven complete: release only at Shutdown
    uint32_t render_w = 0, render_h = 0, out_w = 0, out_h = 0, flags = 0, preset = 0;
    Quality quality = Quality::MaxQuality;
    uint64_t ordered_group = 0;
    bool recreate = false;
    bool reserved = false;              // a background creation will put its handle here
    bool prepared = false;              // made off the frame, not yet adopted by its view
    ComPtr<ID3D12Fence> made_fence;     // the creation's own GPU work (made off the frame)
    uint64_t made_value = 0;
};

struct View {
    bool used = false;
    uint64_t id = 0;
    int feature = -1;
    bool needs_reset = true;
    int last_rec = -1;          // record of its most recent evaluation (R-303)
    uint64_t last_rec_gen = 0;
    ComPtr<ID3D12Fence> wait_fence; // an adopted feature's creation: its next evaluation waits for it on the GPU
    uint64_t wait_value = 0;
};

// What a feature is made for (an off-frame creation is adopted only by the same description).
struct Key {
    uint64_t view = 0, group = 0;
    uint32_t w = 0, h = 0, ow = 0, oh = 0, flags = 0, preset = 0;
    Quality q = Quality::DLAA;
    bool operator==(const Key& o) const
    {
        return view == o.view && group == o.group && w == o.w && h == o.h && ow == o.ow && oh == o.oh && flags == o.flags &&
               preset == o.preset && q == o.q;
    }
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
    // The one off-frame creation (PrepareFeature).
    struct Prep {
        Prepared state = Prepared::None;
        Key key{};
        int slot = -1;
        uint64_t gen = 0;
        ComPtr<ID3D12Fence> fence;
        uint64_t value = 0;
    } prep;
    uint64_t prep_gen = 0;
    ComPtr<ID3D12CommandQueue> make_queue; // the creations' own queue
    void* make_params = nullptr;          // their own parameter object (allocated once)
    PrepareStats prep_stats{};
    // Handles no longer in use, released on a thread of their own right after the call that found them
    // free (coordinator 2026-11-27: releasing on the game's thread cost a frame +24-28 ms).
    std::vector<void*> to_release;
    bool releasing = false;
    // The start-up warm-up of the levels' DLSS (a small Quality and Performance feature made and released
    // before the first feature of the session), so the first real creation of each level is not NVIDIA's
    // first of its kind.
    bool warming = false;
    uint64_t warm_gen = 0;
    const char* holder = "none"; // the background work last holding g.m (WHY log of the game's waits)
    std::array<Key, 2> warmed_keys{};
};
State g;
std::condition_variable g_prep_cv; // with g.m: a background creation finished
std::atomic<void (*)()> g_thread_init{nullptr};
std::recursive_mutex g_life_m; // serializes all public lifecycle mutations
ComPtr<ID3D12Device> g_tried_device;
std::atomic<void (*)()> g_release_view_hook{nullptr};
std::atomic<bool> g_any_attached{false}; // hot-path filter for OnExecuted (every game submission)
std::atomic<bool> g_inline_for_tests{false}; // SetInlineForTests

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

// The video memory every DLSS feature of ours holds together, as NVIDIA reports it (its statistics
// callback); one item in the ledger (diag/vram_ledger.hpp), with how many features hold it.
void note_dlss_vram_locked(const char* why)
{
    unsigned long long bytes = 0;
    uint32_t count = 0;
    for (const Feature& f : g.features) count += f.handle ? 1u : 0u;
    NVSDK_NGX_Parameter* p = CP();
    void* cb = nullptr;
    if (p && NVSDK_NGX_SUCCEED(p->Get(NVSDK_NGX_Parameter_DLSSGetStatsCallback, &cb)) && cb &&
        NVSDK_NGX_SUCCEED(reinterpret_cast<PFN_NVSDK_NGX_DLSS_GetStatsCallback>(cb)(p)))
        p->Get(NVSDK_NGX_Parameter_SizeInBytes, &bytes);
    else if (count)
        bytes = 0; // not reported by this runtime: the count alone is logged
    char text[160];
    std::snprintf(text, sizeof text, "%s (%u DLSS feature%s held%s)", why, count, count == 1 ? "" : "s", bytes || !count ? "" : "; NVIDIA reported no size");
    na::vram::Set("dlss_features", count ? (bytes ? bytes : 1) : 0, text);
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

#if defined(NA_HAVE_NGX_SDK)
void kick_release_locked();
#endif

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
        if (f.made_fence && f.made_fence->GetCompletedValue() < f.made_value) continue; // its creation still runs on the GPU
#if defined(NA_HAVE_NGX_SDK)
        g.to_release.push_back(f.handle); // released by release_thread (kick_release_locked), never on this thread
#endif
        f = Feature{};
        ++g.stats.features_released;
    }
#if defined(NA_HAVE_NGX_SDK)
    kick_release_locked();
#endif
}

#if defined(NA_HAVE_NGX_SDK)
// Releases the handles collect_locked found free. NGX is not thread safe, so this holds g.m while NVIDIA
// releases them; it starts right after the call that found them (an Evaluate, a submission, a view's
// release), so the game's next DLSS call is a frame away. Times every release (PrepareStats).
void release_thread()
{
    if (auto init = g_thread_init.load(std::memory_order_acquire)) init();
    std::lock_guard<std::mutex> lock(g.m);
    g.holder = "release thread";
    std::vector<void*> handles;
    handles.swap(g.to_release);
    double total = 0;
    for (void* h : handles) {
        const auto t0 = std::chrono::steady_clock::now();
        if (g.attached) release_handle(h);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        na::diag::release_trace::Note("dlss_feature_release", ms, GetTickCount64());
        total += ms;
        g.prep_stats.release_last_ms = ms;
        if (ms > g.prep_stats.release_longest_ms) g.prep_stats.release_longest_ms = ms;
        ++g.prep_stats.released_off_frame;
    }
    if (!handles.empty() && g.attached) {
        const auto t0 = std::chrono::steady_clock::now();
        note_dlss_vram_locked("a DLSS feature no longer in use was released (off the frame)");
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (ms > g.prep_stats.size_query_longest_ms) g.prep_stats.size_query_longest_ms = ms;
    }
    g.releasing = false;
    if (!g.to_release.empty()) kick_release_locked();
    g_prep_cv.notify_all();
    if (!handles.empty())
        na::eventlog::Emit("dlss", "released %zu DLSS feature%s off the frame in %.1f ms (longest one so far %.1f ms)", handles.size(),
                           handles.size() == 1 ? "" : "s", total, g.prep_stats.release_longest_ms);
}

void kick_release_locked()
{
    if (g.releasing || g.to_release.empty()) return;
    if (g_inline_for_tests.load(std::memory_order_relaxed)) {
        for (void* h : g.to_release) release_handle(h);
        g.to_release.clear();
        note_dlss_vram_locked("a DLSS feature no longer in use was released");
        return;
    }
    g.releasing = true;
    try {
        std::thread(release_thread).detach();
    } catch (...) { // no thread: released here, as before
        g.releasing = false;
        for (void* h : g.to_release) release_handle(h);
        g.to_release.clear();
        note_dlss_vram_locked("a DLSS feature no longer in use was released");
    }
}
#endif

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
        if (!g.features[i].handle && !g.features[i].reserved) return i;
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

Key key_of(const EvalDesc& d)
{
    Key k;
    k.view = d.view_id;
    k.group = d.ordered_feature_group;
    k.w = d.max_render_w ? d.max_render_w : d.render_w;
    k.h = d.max_render_h ? d.max_render_h : d.render_h;
    k.ow = d.out_w;
    k.oh = d.out_h;
    k.flags = flags_for(d);
    k.preset = d.preset;
    k.q = d.quality;
    return k;
}

// The off-frame creation (PrepareFeature), on its own thread. Holds g.m only while NVIDIA makes the
// feature (NGX is not thread safe); the command list and queue are its own.
void make_prepared(Key k, uint64_t gen, ComPtr<ID3D12Device> dev)
{
    if (auto init = g_thread_init.load(std::memory_order_acquire)) init();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL); // the next frame may be waiting for it
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12CommandQueue> queue;
    bool ok = SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) &&
              SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list))) &&
              SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    int slot = -1;
    double ms = 0;
    int32_t code = 0;
    {
        std::lock_guard<std::mutex> lock(g.m);
        g.holder = "off-frame creation";
        if (g.prep.gen != gen || !g.attached || g.latched || g.device.Get() != dev.Get()) ok = false;
        if (ok && !g.make_queue) {
            D3D12_COMMAND_QUEUE_DESC qd{};
            qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            ok = SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.make_queue)));
            if (ok) g.make_queue->SetName(L"NeuralAlias DLSS feature creation");
        }
        if (ok && !g.make_params) {
            NVSDK_NGX_Parameter* p = nullptr;
            ok = NVSDK_NGX_SUCCEED(reinterpret_cast<PfnParams>(g.api.allocate_parameters)(&p)) && p;
            g.make_params = p;
        }
        if (ok) {
            slot = free_feature_slot_locked();
            ok = slot >= 0;
        }
        if (ok) {
            queue = g.make_queue;
            NVSDK_NGX_Parameter* p = static_cast<NVSDK_NGX_Parameter*>(g.make_params);
            p->Reset();
            p->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
            p->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
            p->Set(NVSDK_NGX_Parameter_Width, k.w);
            p->Set(NVSDK_NGX_Parameter_Height, k.h);
            p->Set(NVSDK_NGX_Parameter_OutWidth, k.ow);
            p->Set(NVSDK_NGX_Parameter_OutHeight, k.oh);
            p->Set(NVSDK_NGX_Parameter_PerfQualityValue, static_cast<int>(k.q));
            p->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, static_cast<int>(k.flags));
            p->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);
            const unsigned int pr = k.preset ? k.preset : static_cast<unsigned int>(NVSDK_NGX_DLSS_Hint_Render_Preset_Default);
            p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, pr);
            p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, pr);
            p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, pr);
            p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, pr);
            p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, pr);
            p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality, pr);
            NVSDK_NGX_Handle* h = nullptr;
            const auto t0 = std::chrono::steady_clock::now();
            const NVSDK_NGX_Result r = reinterpret_cast<PfnCreate>(g.api.create_feature)(list.Get(), NVSDK_NGX_Feature_SuperSampling, p, &h);
            ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            {
                char what[96];
                std::snprintf(what, sizeof what, "dlss_create_off_frame %ux%u->%ux%u q%d", k.w, k.h, k.ow, k.oh, int(k.q));
                na::diag::release_trace::Note(what, ms, GetTickCount64());
            }
            code = static_cast<int32_t>(r);
            ok = NVSDK_NGX_SUCCEED(r) && h;
            if (ok) {
                Feature& nf = g.features[slot];
                nf = Feature{};
                nf.handle = h;
                nf.render_w = k.w;
                nf.render_h = k.h;
                nf.out_w = k.ow;
                nf.out_h = k.oh;
                nf.flags = k.flags;
                nf.quality = k.q;
                nf.preset = k.preset;
                nf.ordered_group = k.group;
                nf.prepared = true;
                nf.made_fence = fence;
                nf.made_value = 1;
            } else {
                slot = -1;
            }
        }
    }
    // Submitted outside the lock: the game's frames go on meanwhile.
    if (ok) ok = SUCCEEDED(list->Close());
    if (ok) {
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        ok = SUCCEEDED(queue->Signal(fence.Get(), 1));
    }
    {
        std::lock_guard<std::mutex> lock(g.m);
        g.holder = "off-frame creation (after)";
        const bool current = g.prep.gen == gen;
        if (slot >= 0 && g.features[slot].handle && g.features[slot].prepared && (!ok || !current)) {
            // Never submitted, or nobody wants it any more: released once nothing of it can run on the GPU.
            g.features[slot].prepared = false;
            if (!ok) g.features[slot].made_fence.Reset();
            retire_feature_locked(slot);
            slot = -1;
        }
        if (current) {
            g.prep.state = ok ? Prepared::Ready : Prepared::Failed;
            g.prep.slot = ok ? slot : -1;
            if (ok) g.prep.fence = fence;
            else g.prep.fence.Reset();
            g.prep.value = ok ? 1 : 0;
            ++(ok ? g.prep_stats.made : g.prep_stats.failed);
            g.prep_stats.last_ms = ms;
            if (ms > g.prep_stats.longest_ms) g.prep_stats.longest_ms = ms;
            g.prep_stats.w = k.w;
            g.prep_stats.h = k.h;
        }
        collect_locked();
        if (ok) note_dlss_vram_locked("a DLSS feature was made off the frame for the next level");
    }
    g_prep_cv.notify_all();
    na::eventlog::Emit("dlss", "feature %s off the frame: %ux%u -> %ux%u quality=%s preset=%u view=0x%llx in %.1f ms (NVIDIA code 0x%08X)",
                       ok ? "made" : "NOT made", k.w, k.h, k.ow, k.oh, QualityName(k.q), k.preset, static_cast<unsigned long long>(k.view), ms,
                       static_cast<unsigned>(code));
}
#endif

#if defined(NA_HAVE_NGX_SDK)
// VARIANT: dlss/levels-warmed-at-start (coordinator 2026-11-27, option a)
// At start-up, before the session's first DLSS feature: the Quality and Performance features exactly as the
// levels will ask for them (the host's descriptions: render and output size, quality, flags, preset) made on
// this thread and released again, so the first real creation of each level is not NVIDIA's first of its kind
// (the first DLAA -> Quality creation took 62 ms against 10-20 ms later; with small stand-ins the first
// Performance one still took up to 30 ms). Made before any real feature: DLSS 310.9.16 spoils a larger feature
// evaluated after a smaller one was made, so an Evaluate that would make the first real feature waits for this
// (Evaluate, below). Holds g.m only while NVIDIA makes and releases them; their GPU work runs on its own queue,
// waited for on this thread. The alternative was small 640x360 stand-ins at Attach (2026-11-27, 932f89a0).
void warm_levels(uint64_t gen, ComPtr<ID3D12Device> dev, std::array<Key, 2> keys)
{
    if (auto init = g_thread_init.load(std::memory_order_acquire)) init();
    const auto start = std::chrono::steady_clock::now();
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    bool ok = SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) &&
              SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list))) &&
              SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) &&
              SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
    if (ok) queue->SetName(L"NeuralAlias DLSS levels warm-up");
    NVSDK_NGX_Handle* made[2] = {};
    double ms[2] = {0, 0};
    int32_t code[2] = {0, 0};

    {
        std::lock_guard<std::mutex> lock(g.m);
        g.holder = "warm-up creation";
        if (g.warm_gen != gen || !g.attached || g.latched || g.device.Get() != dev.Get()) ok = false;
        NVSDK_NGX_Parameter* p = nullptr;
        if (ok) ok = NVSDK_NGX_SUCCEED(reinterpret_cast<PfnParams>(g.api.allocate_parameters)(&p)) && p;
        for (int i = 0; ok && i < 2; ++i) {
            p->Reset();
            p->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
            p->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
            const Key& k = keys[i];
            p->Set(NVSDK_NGX_Parameter_Width, k.w);
            p->Set(NVSDK_NGX_Parameter_Height, k.h);
            p->Set(NVSDK_NGX_Parameter_OutWidth, k.ow);
            p->Set(NVSDK_NGX_Parameter_OutHeight, k.oh);
            p->Set(NVSDK_NGX_Parameter_PerfQualityValue, static_cast<int>(k.q));
            p->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, static_cast<int>(k.flags));
            p->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);
            const unsigned int pr = k.preset ? k.preset : static_cast<unsigned int>(NVSDK_NGX_DLSS_Hint_Render_Preset_Default);
            p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, pr);
            p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, pr);
            p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, pr);
            p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, pr);
            p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, pr);
            p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality, pr);
            const auto t0 = std::chrono::steady_clock::now();
            const NVSDK_NGX_Result r = reinterpret_cast<PfnCreate>(g.api.create_feature)(list.Get(), NVSDK_NGX_Feature_SuperSampling, p, &made[i]);
            ms[i] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            code[i] = static_cast<int32_t>(r);
            if (!NVSDK_NGX_SUCCEED(r)) made[i] = nullptr;
        }
        if (p) reinterpret_cast<PfnDestroy>(g.api.destroy_parameters)(p);
    }
    // Their GPU work, on this thread's own queue, finished before they are released.
    bool gpu_done = false;
    if (ok && SUCCEEDED(list->Close())) {
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        if (SUCCEEDED(queue->Signal(fence.Get(), 1))) {
            if (fence->GetCompletedValue() < 1) {
                HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (ev && SUCCEEDED(fence->SetEventOnCompletion(1, ev))) WaitForSingleObject(ev, 5000);
                if (ev) CloseHandle(ev);
            }
            gpu_done = fence->GetCompletedValue() >= 1;
        }
    } else if (!ok) {
        gpu_done = true; // nothing was submitted
    }
    double release_ms = 0;
    {
        std::lock_guard<std::mutex> lock(g.m);
        g.holder = "warm-up release";
        const auto t0 = std::chrono::steady_clock::now();
        // Released while NVIDIA is still attached (Shutdown waits for this thread); never evaluated. A
        // creation whose GPU work did not finish in 5 s is left (never happens unless the device is lost).
        for (auto* h : made)
            if (h && g.attached && gpu_done) release_handle(h);
        release_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        g.prep_stats.warm_quality_ms = ms[0];
        g.prep_stats.warm_performance_ms = ms[1];
        g.prep_stats.warm_release_ms = release_ms;
        g.prep_stats.warm_total_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        g.prep_stats.warmed = made[0] && made[1];
        if (g.warm_gen == gen) g.warming = false;
    }
    g_prep_cv.notify_all();
    na::eventlog::Emit("dlss", "levels warmed at start: Quality %s in %.1f ms (0x%08X), Performance %s in %.1f ms (0x%08X), released in %.1f ms; %.1f ms in all, off the frame",
                       made[0] ? "made" : "NOT made", ms[0], static_cast<unsigned>(code[0]), made[1] ? "made" : "NOT made", ms[1],
                       static_cast<unsigned>(code[1]), release_ms, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
}

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

namespace {
std::atomic<OwnAttachedFn> g_own_attached{nullptr};
wchar_t g_search_folder[260] = {}; // written once at boot, before our NGX starts
} // namespace
void SetOwnAttachedListener(OwnAttachedFn fn) { g_own_attached.store(fn, std::memory_order_release); }
void SetOwnSearchFolder(const wchar_t* folder) { wcsncpy_s(g_search_folder, folder ? folder : L"", _TRUNCATE); }
const wchar_t* OwnSearchFolder() { return g_search_folder; }

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
    bool attached = false;
    {
        std::lock_guard<std::mutex> lock(g.m);
        if (g.attached) {
            g.own = true;
            g.caps.own_instance = true;
            attached = true;
        } else {
            OwnShutdown(device);
        }
    }
    if (attached)
        if (const OwnAttachedFn fn = g_own_attached.load(std::memory_order_acquire)) fn(api);
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
    std::unique_lock<std::mutex> lock(g.m);
#if defined(NA_HAVE_NGX_SDK)
    // The start-up warm-up releases its features while NVIDIA is attached: waited for (its GPU work is short).
    if (g.warming) g_prep_cv.wait_for(lock, std::chrono::seconds(6), [] { return !g.warming; });
    g.warming = false;
    ++g.warm_gen;
    if (g.attached) {
        for (void* h : g.to_release) release_handle(h);
        g.to_release.clear();
        // Caller guarantees GPU idle: release every owned handle exactly once,
        // tainted ones included.
        for (Feature& f : g.features)
            if (f.handle) {
                release_handle(f.handle);
                ++g.stats.features_released;
            }
        if (g.eval_params) reinterpret_cast<PfnDestroy>(g.api.destroy_parameters)(EP());
        if (g.cap_params) reinterpret_cast<PfnDestroy>(g.api.destroy_parameters)(CP());
        if (g.make_params) reinterpret_cast<PfnDestroy>(g.api.destroy_parameters)(static_cast<NVSDK_NGX_Parameter*>(g.make_params));
    }
    g.make_params = nullptr;
    g.make_queue.Reset();
    g.prep = State::Prep{};
    g.prep.gen = ++g.prep_gen; // a creation still running finds its generation gone and gives up
    g_prep_cv.notify_all();
    na::vram::Set("dlss_features", 0, "DLSS stopped: every feature released");
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

bool Evaluate(ID3D12GraphicsCommandList* list, const EvalDesc& d, bool* history_reset)
{
    if (history_reset) *history_reset = false;
    const na::diag::crumbs::Scope crumb(list, "NeuralAlias DLSS evaluate");
    const InjectorCallScope origin;
    LARGE_INTEGER lq0{};
    QueryPerformanceCounter(&lq0);
    std::unique_lock<std::mutex> lock(g.m);
    {   // WHY log: the game's thread waiting for the backend's lock (a creation, warm-up or release holds it).
        LARGE_INTEGER lq1{}, hz{};
        QueryPerformanceCounter(&lq1);
        QueryPerformanceFrequency(&hz);
        const double waited = double(lq1.QuadPart - lq0.QuadPart) * 1000.0 / double(hz.QuadPart);
        if (waited > 2.0) {
            char what[96];
            std::snprintf(what, sizeof what, "dlss_lock_wait (held by %s)", g.holder);
            na::diag::release_trace::Note(what, waited, GetTickCount64());
            na::eventlog::Emit("release_trace", "what=dlss_lock_wait thread=game ms=%.2f holder=%s at_tick=%llu", waited, g.holder,
                               static_cast<unsigned long long>(GetTickCount64()));
        }
    }
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
    bool fits = cur && !cur->tainted && !cur->recreate && cur->ordered_group == d.ordered_feature_group &&
                      cur->out_w == d.out_w && cur->out_h == d.out_h && cur->flags == flags &&
                      cur->quality == d.quality && cur->preset == d.preset && create_w <= cur->render_w && create_h <= cur->render_h;
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
    if (!fits && g.prep.state == Prepared::Ready && g.prep.slot >= 0 && g.prep.key == key_of(d)) {
        // Made off the frame for exactly this (PrepareFeature): adopted, nothing is made on this frame.
        const int slot = g.prep.slot;
        Feature& nf = g.features[slot];
        nf.prepared = false;
        if (nf.ordered_group) {
            for (Feature& other : g.features) {
                if (&other != &nf && other.handle && !other.retired && other.ordered_group == nf.ordered_group &&
                    (other.render_w > nf.render_w || other.render_h > nf.render_h)) other.recreate = true;
            }
        }
        retire_feature_locked(view.feature); // released once every use of it has completed
        view.feature = slot;
        view.needs_reset = true;
        view.last_rec = -1; // a new feature: nothing to order against
        view.wait_fence = g.prep.fence;
        view.wait_value = g.prep.value;
        ++g.stats.features_created;
        ++g.prep_stats.adopted;
        g.stats.feature_render_w = nf.render_w;
        g.stats.feature_render_h = nf.render_h;
        g.stats.feature_out_w = nf.out_w;
        g.stats.feature_out_h = nf.out_h;
        g.stats.feature_flags = nf.flags;
        g.stats.feature_preset = nf.preset;
        {
            auto& c = g.stats.creates[g.stats.create_seq % 8];
            c = {++g.stats.create_seq, d.view_id, nf.flags, nf.preset, static_cast<uint32_t>(nf.quality), nf.render_w, nf.render_h, nf.out_w, nf.out_h};
        }
        g.prep = State::Prep{};
        g.prep.gen = ++g.prep_gen;
        pred = -1;
        fits = true;
        collect_locked();
    }
    if (!fits && g.warming) {
        // The start-up warm-up of the levels is still making its small features: the session's first real
        // feature is made after them (see warm_levels). Only the first frames can meet this.
        const auto t0 = std::chrono::steady_clock::now();
        g_prep_cv.wait_for(lock, std::chrono::seconds(3), [] { return !g.warming; });
        g.prep_stats.warm_waited_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (!g.attached || g.latched) return false;
    }
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
        {   // Every mode's hint: NGX reads the one matching PerfQualityValue.
            const unsigned int pr = d.preset ? d.preset : static_cast<unsigned int>(NVSDK_NGX_DLSS_Hint_Render_Preset_Default);
            p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, pr);
            p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, pr);
            p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, pr);
            p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, pr);
            p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, pr);
            p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality, pr);
        }
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
        Feature& nf = g.features[slot];
        nf = Feature{};
        nf.handle = h;
        nf.render_w = create_w;
        nf.render_h = create_h;
        nf.out_w = d.out_w;
        nf.out_h = d.out_h;
        nf.flags = flags;
        nf.quality = d.quality;
        nf.preset = d.preset;
        nf.ordered_group = d.ordered_feature_group;
        if (nf.ordered_group) {
            // Reproduced on an HDR Resistance replay with independent resources for the two views.
            // A new small feature broke the existing larger view. A newly created larger view
            // produced clean frames, and it did not change the small view's temporal output.
            // Keep recorded/submitted work alive under the normal feature retirement rules.
            for (Feature& other : g.features) {
                if (other.handle && !other.retired && other.ordered_group == nf.ordered_group &&
                    (other.render_w > create_w || other.render_h > create_h)) other.recreate = true;
            }
        }
        retire_feature_locked(view.feature); // released once every use of it has completed
        view.feature = slot;
        view.needs_reset = true;
        ++g.stats.features_created;
        g.stats.feature_render_w = create_w;
        g.stats.feature_render_h = create_h;
        g.stats.feature_out_w = d.out_w;
        g.stats.feature_out_h = d.out_h;
        g.stats.feature_flags = flags;
        g.stats.feature_preset = d.preset;
        {
            auto& c = g.stats.creates[g.stats.create_seq % 8];
            c = {++g.stats.create_seq, d.view_id, flags, d.preset, static_cast<uint32_t>(d.quality), create_w, create_h, d.out_w, d.out_h};
        }
        collect_locked(); // the retired feature may already be free of uses
        note_dlss_vram_locked("a DLSS feature was made on the frame");
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
    p->Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, d.bias_current_color);
    p->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, d.jitter_x);
    p->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, d.jitter_y);
    p->Set(NVSDK_NGX_Parameter_MV_Scale_X, d.mv_scale_x);
    p->Set(NVSDK_NGX_Parameter_MV_Scale_Y, d.mv_scale_y);
    p->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, d.pre_exposure);
    p->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, d.exposure_scale);
    const bool reset = d.reset || view.needs_reset;
    p->Set(NVSDK_NGX_Parameter_Reset, reset ? 1 : 0);
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
#ifdef NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_X
    p->Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_X, 0u);
    p->Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_Y, 0u);
#endif
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
        rec.after = std::move(view.wait_fence); // the first evaluation of an adopted feature waits for its creation
        rec.after_value = view.wait_value;
        view.wait_value = 0;
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
    if (history_reset) *history_reset = reset;
    set_reason(g.stats.reason, "");
    return true;
#else
    set_reason(g.stats.reason, "This build has no NVIDIA DLSS support (SDK headers were not found).");
    return false;
#endif
}

static void on_executed_locked(ID3D12CommandQueue* queue, uint32_t count, ID3D12CommandList* const* lists);

bool ReleaseIfDeviceRemoved()
{
    if (!g_any_attached.load(std::memory_order_acquire)) return false;
    {
        std::lock_guard<std::mutex> lock(g.m);
        if (!g.attached || !g.device || g.device->GetDeviceRemovedReason() == S_OK) return false;
        na::diag::ReportIfRemoved(g.device.Get());
    }
    Shutdown(); // see OnExecuted: a removed device must not stay pinned
    return true;
}

void OnExecuted(ID3D12CommandQueue* queue, uint32_t count, ID3D12CommandList* const* lists)
{
    if (!queue || !lists || !count || !g_any_attached.load(std::memory_order_acquire)) return;
    {
        std::lock_guard<std::mutex> lock(g.m);
        if (!g.attached) return;
        if (!g.device || g.device->GetDeviceRemovedReason() == S_OK) {
            on_executed_locked(queue, count, lists);
            return;
        }
        na::diag::ReportIfRemoved(g.device.Get());
    }
    // Hardware session (G2-004): the bound device was removed. D3D12 devices
    // are per-adapter singletons, so while we (or NGX) hold it the game's
    // D3D12CreateDevice gets the SAME removed device back and can never
    // recover. Nothing can still run on a removed device: shut down now,
    // releasing NGX and every reference; EnsureOn restarts on the new device.
    Shutdown();
}

static void on_executed_locked(ID3D12CommandQueue* queue, uint32_t count, ID3D12CommandList* const* lists)
{
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
        if (rec.state == RecState::Recorded && rec.after && index_of(rec.list) >= 0) {
            // The feature was made off the frame on our own queue: this queue waits for that on the GPU.
            if (rec.after->GetCompletedValue() < rec.after_value && SUCCEEDED(queue->Wait(rec.after.Get(), rec.after_value)))
                ++g.stats.queue_waits;
            rec.after.Reset();
        }
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

void SetBackgroundThreadInit(void (*init)()) { g_thread_init.store(init, std::memory_order_release); }
void SetInlineForTests(bool inline_work) { g_inline_for_tests.store(inline_work, std::memory_order_relaxed); }

bool WarmLevels(const EvalDesc& quality, const EvalDesc& performance)
{
#if defined(NA_HAVE_NGX_SDK)
    std::lock_guard<std::mutex> lock(g.m);
    if (!g.attached || g.latched || !g.caps.available || !g.device || g.warming || g_inline_for_tests.load(std::memory_order_relaxed)) return false;
    const std::array<Key, 2> keys{key_of(quality), key_of(performance)};
    if (g.warmed_keys == keys) return false; // these exact features were warmed already
    g.warmed_keys = keys;
    g.warming = true;
    g.warm_gen = ++g.prep_gen;
    try {
        std::thread(warm_levels, g.warm_gen, g.device, keys).detach();
    } catch (...) {
        g.warming = false;
        return false;
    }
    return true;
#else
    (void)quality; (void)performance;
    return false;
#endif
}

#if defined(NA_HAVE_NGX_SDK)
// The view of `k` already has a feature that serves it (nothing to make).
bool view_has_locked(const Key& k)
{
    const int vi = find_view_locked(k.view, false);
    if (vi < 0 || g.views[vi].feature < 0) return false;
    const Feature& f = g.features[g.views[vi].feature];
    return f.handle && !f.tainted && !f.recreate && f.ordered_group == k.group && f.out_w == k.ow && f.out_h == k.oh &&
           f.flags == k.flags && f.quality == k.q && f.preset == k.preset && k.w <= f.render_w && k.h <= f.render_h;
}
#endif

Prepared PreparedState(const EvalDesc& d)
{
#if defined(NA_HAVE_NGX_SDK)
    std::lock_guard<std::mutex> lock(g.m);
    const Key k = key_of(d);
    if (g.prep.state != Prepared::None && g.prep.key == k) return g.prep.state;
    return view_has_locked(k) ? Prepared::Ready : Prepared::None;
#else
    (void)d;
    return Prepared::None;
#endif
}

Prepared PrepareFeature(const EvalDesc& d)
{
#if defined(NA_HAVE_NGX_SDK)
    std::lock_guard<std::mutex> lock(g.m);
    if (!g.attached || g.latched || !g.caps.available || !g.device) return Prepared::None;
    const Key k = key_of(d);
    if (g.prep.state != Prepared::None && g.prep.key == k) return g.prep.state;
    if (g.prep.state == Prepared::Making) return Prepared::Making; // another one is being made: asked again next frame
    if (view_has_locked(k)) return Prepared::Ready; // already the view's feature: nothing to make
    if (g.prep.state == Prepared::Ready && g.prep.slot >= 0) { // made for another request: given up
        g.features[g.prep.slot].prepared = false;
        retire_feature_locked(g.prep.slot);
        ++g.prep_stats.cancelled;
    }
    g.prep = State::Prep{};
    g.prep.gen = ++g.prep_gen;
    g.prep.key = k;
    g.prep.state = Prepared::Making;
    try {
        std::thread(make_prepared, k, g.prep.gen, g.device).detach();
    } catch (...) {
        g.prep.state = Prepared::Failed;
    }
    return g.prep.state;
#else
    (void)d;
    return Prepared::None;
#endif
}

Prepared WaitPrepared(const EvalDesc& d, double* waited_ms)
{
    if (waited_ms) *waited_ms = 0;
#if defined(NA_HAVE_NGX_SDK)
    const auto t0 = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(g.m);
    const Key k = key_of(d);
    if (g.prep.state == Prepared::None || !(g.prep.key == k)) return view_has_locked(k) ? Prepared::Ready : Prepared::None;
    g_prep_cv.wait_for(lock, std::chrono::seconds(2), [&] { return g.prep.state != Prepared::Making || !(g.prep.key == k); });
    if (waited_ms) *waited_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return g.prep.key == k ? g.prep.state : Prepared::None;
#else
    (void)d;
    return Prepared::None;
#endif
}

void CancelPrepared()
{
#if defined(NA_HAVE_NGX_SDK)
    std::lock_guard<std::mutex> lock(g.m);
    if (g.prep.state == Prepared::Ready && g.prep.slot >= 0) {
        g.features[g.prep.slot].prepared = false;
        retire_feature_locked(g.prep.slot);
        ++g.prep_stats.cancelled;
    }
    if (g.prep.state != Prepared::None) {
        g.prep = State::Prep{};
        g.prep.gen = ++g.prep_gen;
    }
    collect_locked();
#endif
}

PrepareStats GetPrepareStats()
{
    std::lock_guard<std::mutex> lock(g.m);
    return g.prep_stats;
}

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
