// C7b: XeSS (libxess.dll, D3D12) interception. See xess_intercept.hpp.
#include "host_dx12/upscalers/xess_intercept.hpp"

#include "host_dx12/dlss/dlss_backend.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
#include "host_dx12/upscalers/module_watch.hpp"
#include "host_dx12/upscalers/xess_abi.hpp"

#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>

namespace na::xess {

// XeSS's jitter (after its jitter scale) is the projection offset in pixels,
// like DLSS's. MEASURED (Gate 2 hardware session, 2026-10-24, RTX 5070 Ti,
// Intel's official libxess.dll 2.0.2.68, upscaler_jitter_test): XeSS expects
// sample = pixel centre - jitter on both axes (+3.5 dB over bilinear; every
// other convention lost), the same as DLSS (C6) -> sign +1 per axis.
constexpr float kXessToDlssJitterSign = 1.0f;

bool ToDlss(const Observation& o, na::dlss::EvalDesc* out, const char** reason)
{
    using namespace na::xess::abi;
    const auto fail = [&](const char* why) {
        if (reason) *reason = why;
        return false;
    };
    if (!out) return fail("No output description.");
    if (!o.context_known) return fail("XeSS was set up before NeuralAlias attached (output size and conventions unknown).");
    if (!o.color || !o.velocity || !o.output) return fail("XeSS was not given colour, motion and an output.");
    if (!o.depth) return fail("This game gives XeSS no depth buffer; DLSS needs one.");
    if (o.nonzero_bases) return fail("This game uses sub-rectangles of its textures (handled later, C13).");
    if (!o.in_w || !o.in_h || !o.out_w || !o.out_h) return fail("XeSS's input or output size is missing.");
    na::dlss::EvalDesc d;
    d.color = o.color;
    d.depth = o.depth;
    d.motion = o.velocity;
    d.output = o.output;
    d.exposure = (o.init_flags & kExposureScaleTexture) ? o.exposure : nullptr;
    d.render_w = o.in_w;
    d.render_h = o.in_h;
    d.out_w = o.out_w;
    d.out_h = o.out_h;
    d.jitter_x = kXessToDlssJitterSign * o.jitter_x * o.jitter_scale_x;
    d.jitter_y = kXessToDlssJitterSign * o.jitter_y * o.jitter_scale_y;
    d.mv_display_res = (o.init_flags & kHighResMv) != 0;
    const float mv_w = static_cast<float>(d.mv_display_res ? o.out_w : o.in_w);
    const float mv_h = static_cast<float>(d.mv_display_res ? o.out_h : o.in_h);
    if (o.init_flags & kUseNdcVelocity) {
        // NDC [-1,1] with +Y up -> pixels with +Y down.
        d.mv_scale_x = o.velocity_scale_x * 0.5f * mv_w;
        d.mv_scale_y = -o.velocity_scale_y * 0.5f * mv_h;
    } else {
        d.mv_scale_x = o.velocity_scale_x;
        d.mv_scale_y = o.velocity_scale_y;
    }
    d.mv_jittered = (o.init_flags & kJitteredMv) != 0;
    d.depth_inverted = (o.init_flags & kInvertedDepth) != 0;
    d.hdr = (o.init_flags & kLdrInputColor) == 0;
    d.auto_exposure = (o.init_flags & kEnableAutoExposure) != 0 && !d.exposure;
    const float scale = (o.exposure_scale > 0.f ? o.exposure_scale : 1.f) * (o.exposure_multiplier > 0.f ? o.exposure_multiplier : 1.f);
    d.exposure_scale = scale;
    d.reset = o.reset;
    // ARGON-G2-001: the game's chosen XeSS mode fixes the DLSS mode, and the
    // feature is created at the output size (XeSS states no maximum render
    // size; it is never larger than the output), so dynamic resolution never
    // recreates or resets DLSS.
    switch (o.quality) {
    case kUltraPerformance: d.quality = na::dlss::Quality::UltraPerformance; break;
    case kPerformance: d.quality = na::dlss::Quality::MaxPerformance; break;
    case kBalanced: d.quality = na::dlss::Quality::Balanced; break;
    case kNativeAA: d.quality = na::dlss::Quality::DLAA; break;
    default: d.quality = na::dlss::Quality::MaxQuality; break; // Quality, UltraQuality, UltraQualityPlus
    }
    if (d.quality == na::dlss::Quality::DLAA && (o.in_w != o.out_w || o.in_h != o.out_h)) d.quality = na::dlss::Quality::MaxQuality;
    d.max_render_w = d.quality == na::dlss::Quality::DLAA ? o.in_w : o.out_w;
    d.max_render_h = d.quality == na::dlss::Quality::DLAA ? o.in_h : o.out_h;
    if (const char* bad = na::dlss::Validate(d)) return fail(bad);
    *out = d;
    if (reason) *reason = nullptr;
    return true;
}

namespace {

using namespace na::xess::abi;

constexpr int kSlots = 2;
constexpr uint32_t kMaxContexts = 16;

struct Slot {
    std::atomic<HMODULE> module{nullptr};
    PfnD3D12CreateContext create = nullptr;
    PfnD3D12Init init = nullptr;
    PfnD3D12Execute execute = nullptr;
    PfnDestroyContext destroy = nullptr;
    PfnSetScale set_jitter = nullptr;
    PfnSetScale set_velocity = nullptr;
    PfnSetExposureMultiplier set_exposure = nullptr;
};
Slot g_slots[kSlots];

enum class Backend : uint8_t { None, Native, Dlss }; // R-304: who produced the previous frame

struct ContextInfo {
    uint64_t view_id = 0;           // R-303: this context's own DLSS view (never reused)
    Backend last = Backend::None;
    Context context = nullptr;
    ID3D12Device* device = nullptr; // identity; the game owns it
    bool initialized = false;
    uint32_t flags = 0;
    int32_t quality = 0;
    uint32_t out_w = 0, out_h = 0;
    float jitter_scale_x = 1, jitter_scale_y = 1, velocity_scale_x = 1, velocity_scale_y = 1, exposure_multiplier = 1;
};

constexpr uint64_t kViewTag = 2ull << 40;
std::atomic<uint64_t> g_next_view{1};

std::mutex g_m;
ContextInfo g_contexts[kMaxContexts];
Observation g_last{};
bool g_have_last = false;
Stats g_stats{};
std::atomic<uint32_t> g_mode{static_cast<uint32_t>(Mode::Observe)};

void set_reason(const char* why) { std::snprintf(g_stats.reason, sizeof(g_stats.reason), "%s", why ? why : ""); }

ContextInfo* find(Context c)
{
    for (ContextInfo& ci : g_contexts)
        if (ci.context && ci.context == c) return &ci;
    return nullptr;
}
ContextInfo* find_or_add(Context c)
{
    if (ContextInfo* ci = find(c)) return ci;
    for (ContextInfo& ci : g_contexts)
        if (!ci.context) {
            ci = ContextInfo{};
            ci.context = c;
            ci.view_id = kViewTag | g_next_view.fetch_add(1, std::memory_order_relaxed);
            return &ci;
        }
    return nullptr; // table full: executes report context_known=false
}

// All producers share one backend lifecycle transaction; never hold g_m here.
bool ensure_dlss(ID3D12Device* device)
{
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetTempPathW(MAX_PATH, path);
    if (!n || n > MAX_PATH - 20) return false;
    wcscat_s(path, L"NeuralAlias-ngx");
    CreateDirectoryW(path, nullptr);
    return na::dlss::EnsureOn(device, path);
}

template <int S>
Result DetourCreate(ID3D12Device* device, Context* out)
{
    const Result r = g_slots[S].create(device, out);
    if (r != kSuccess || !out || !*out) return r;
    std::lock_guard<std::mutex> lock(g_m);
    ++g_stats.contexts_created;
    if (ContextInfo* ci = find_or_add(*out)) {
        const uint64_t view = ci->view_id; // keep the id assigned by find_or_add
        *ci = ContextInfo{};
        ci->context = *out;
        ci->view_id = view;
        ci->device = device;
    }
    return r;
}

template <int S>
Result DetourInit(Context c, const D3D12InitParams* p)
{
    const Result r = g_slots[S].init(c, p);
    if (r != kSuccess || !p) return r;
    std::lock_guard<std::mutex> lock(g_m);
    ++g_stats.contexts_initialized;
    if (ContextInfo* ci = find_or_add(c)) {
        ci->initialized = true;
        ci->last = Backend::None; // ARGON-G2-006: (re)Init starts fresh DLSS history
        ci->flags = p->initFlags;
        ci->quality = p->qualitySetting;
        ci->out_w = p->outputResolution.x;
        ci->out_h = p->outputResolution.y;
    }
    return r;
}

template <int S>
Result DetourDestroy(Context c)
{
    uint64_t view = 0;
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (ContextInfo* ci = find(c)) {
            view = ci->view_id;
            *ci = ContextInfo{};
        }
        ++g_stats.contexts_destroyed;
    }
    if (view) na::dlss::ReleaseView(view); // backend calls never nest under g_m
    return g_slots[S].destroy(c);
}

template <int S>
Result DetourSetJitterScale(Context c, float x, float y)
{
    const Result r = g_slots[S].set_jitter(c, x, y);
    if (r == kSuccess) {
        std::lock_guard<std::mutex> lock(g_m);
        if (ContextInfo* ci = find_or_add(c)) { ci->jitter_scale_x = x; ci->jitter_scale_y = y; }
    }
    return r;
}

template <int S>
Result DetourSetVelocityScale(Context c, float x, float y)
{
    const Result r = g_slots[S].set_velocity(c, x, y);
    if (r == kSuccess) {
        std::lock_guard<std::mutex> lock(g_m);
        if (ContextInfo* ci = find_or_add(c)) { ci->velocity_scale_x = x; ci->velocity_scale_y = y; }
    }
    return r;
}

template <int S>
Result DetourSetExposureMultiplier(Context c, float m)
{
    const Result r = g_slots[S].set_exposure(c, m);
    if (r == kSuccess) {
        std::lock_guard<std::mutex> lock(g_m);
        if (ContextInfo* ci = find_or_add(c)) ci->exposure_multiplier = m;
    }
    return r;
}

template <int S>
Result DetourExecute(Context c, ID3D12GraphicsCommandList* list, const D3D12ExecuteParams* p)
{
    bool native_needs_reset = false;
    uint64_t native_view = 0; // R-309: the context whose native call is about to run
    if (p) {
        std::unique_lock<std::mutex> lock(g_m);
        Observation o;
        o.serial = ++g_stats.executes;
        o.context = c;
        if (const ContextInfo* ci = find(c)) {
            o.context_known = ci->initialized;
            o.device = ci->device;
            o.init_flags = ci->flags;
            o.quality = ci->quality;
            o.out_w = ci->out_w;
            o.out_h = ci->out_h;
            o.jitter_scale_x = ci->jitter_scale_x;
            o.jitter_scale_y = ci->jitter_scale_y;
            o.velocity_scale_x = ci->velocity_scale_x;
            o.velocity_scale_y = ci->velocity_scale_y;
            o.exposure_multiplier = ci->exposure_multiplier;
        }
        if (!o.context_known) ++g_stats.unknown_context;
        o.list = list;
        o.color = p->pColorTexture;
        o.velocity = p->pVelocityTexture;
        o.depth = p->pDepthTexture;
        o.exposure = p->pExposureScaleTexture;
        o.responsive = p->pResponsivePixelMaskTexture;
        o.output = p->pOutputTexture;
        o.jitter_x = p->jitterOffsetX;
        o.jitter_y = p->jitterOffsetY;
        o.exposure_scale = p->exposureScale;
        o.reset = p->resetHistory != 0;
        o.in_w = p->inputWidth;
        o.in_h = p->inputHeight;
        const Vec2u bases[] = {p->inputColorBase, p->inputMotionVectorBase, p->inputDepthBase, p->outputColorBase};
        for (const Vec2u& b : bases)
            if (b.x || b.y) o.nonzero_bases = true;
        g_last = o;
        g_have_last = true;
        uint64_t view_id = 0;
        Backend last = Backend::None;
        if (const ContextInfo* ci0 = find(c)) { view_id = ci0->view_id; last = ci0->last; }
        const bool replace = static_cast<Mode>(g_mode.load(std::memory_order_relaxed)) == Mode::ReplaceWithDlss;
        lock.unlock(); // ARGON-G2-005: NGX start-up and evaluation run without g_m
        bool replaced = false;
        const char* why = nullptr;
        if (replace) {
            na::dlss::EvalDesc d;
            if (ToDlss(o, &d, &why)) {
                d.view_id = view_id;
                if (last != Backend::Dlss) d.reset = true; // R-304 / G2-006: DLSS did not see the previous frame
                Microsoft::WRL::ComPtr<ID3D12Device> from_list;
                ID3D12Device* dev = o.device; // ARGON-G2-002: fall back to the list's device
                if (!dev && list && SUCCEEDED(list->GetDevice(IID_PPV_ARGS(&from_list)))) dev = from_list.Get();
                if (!ensure_dlss(dev)) why = "DLSS could not start on this device.";
                else replaced = na::dlss::Evaluate(list, d);
            }
        }
        // ARGON-G2-005: the backend's lock is never taken under g_m.
        const na::dlss::Stats backend = (replace && !replaced && !why) ? na::dlss::GetStats() : na::dlss::Stats{};
        lock.lock();
        ContextInfo* ci = find(c);
        if (ci && ci->view_id != view_id) ci = nullptr; // destroyed and recreated meanwhile
        if (replaced) {
            ++g_stats.replaced;
            set_reason("");
            if (ci) ci->last = Backend::Dlss;
            return kSuccess; // DLSS wrote XeSS's output on the game's list
        }
        if (replace) {
            ++g_stats.fallbacks;
            set_reason(why ? why : backend.reason);
        }
        // R-304: XeSS skipped the frames DLSS produced; its history is stale.
        native_needs_reset = ci && ci->last == Backend::Dlss && !p->resetHistory;
        native_view = ci ? view_id : 0;
    }
    Result rc;
    if (native_needs_reset) {
        D3D12ExecuteParams copy = *p; // never modify the game's parameters
        copy.resetHistory = 1;
        rc = g_slots[S].execute(c, list, &copy);
    } else {
        rc = g_slots[S].execute(c, list, p);
    }
    // R-309: XeSS owns the history only once its frame actually ran; after a
    // failed native call the next XeSS frame after DLSS still resets.
    if (rc == kSuccess && native_view) {
        std::lock_guard<std::mutex> relock(g_m);
        if (ContextInfo* ci = find(c); ci && ci->view_id == native_view) ci->last = Backend::Native;
    }
    return rc;
}

template <int S>
uint32_t install_slot(HMODULE m)
{
    Slot& slot = g_slots[S];
    struct Spec { const char* name; void* detour; void** original; bool required; };
    const Spec specs[] = {
        {"xessD3D12CreateContext", reinterpret_cast<void*>(&DetourCreate<S>), reinterpret_cast<void**>(&slot.create), true},
        {"xessD3D12Init", reinterpret_cast<void*>(&DetourInit<S>), reinterpret_cast<void**>(&slot.init), true},
        {"xessD3D12Execute", reinterpret_cast<void*>(&DetourExecute<S>), reinterpret_cast<void**>(&slot.execute), true},
        {"xessDestroyContext", reinterpret_cast<void*>(&DetourDestroy<S>), reinterpret_cast<void**>(&slot.destroy), true},
        {"xessSetJitterScale", reinterpret_cast<void*>(&DetourSetJitterScale<S>), reinterpret_cast<void**>(&slot.set_jitter), false},
        {"xessSetVelocityScale", reinterpret_cast<void*>(&DetourSetVelocityScale<S>), reinterpret_cast<void**>(&slot.set_velocity), false},
        {"xessSetExposureMultiplier", reinterpret_cast<void*>(&DetourSetExposureMultiplier<S>), reinterpret_cast<void**>(&slot.set_exposure), false},
    };
    for (const Spec& s : specs)
        if (s.required && !GetProcAddress(m, s.name)) return 0;
    if (!na::hooking::BeginInstall()) return 0;
    uint32_t hooked = 0;
    for (const Spec& s : specs) {
        void* target = reinterpret_cast<void*>(GetProcAddress(m, s.name));
        if (target && na::hooking::PrepareHook(target, s.detour, s.original)) {
            ++hooked;
        } else if (s.required) {
            na::hooking::AbandonInstall();
            return 0;
        }
    }
    slot.module.store(m, std::memory_order_release);
    if (!na::hooking::CommitInstall()) {
        slot.module.store(nullptr, std::memory_order_release);
        return 0;
    }
    return hooked;
}

using InstallFn = uint32_t (*)(HMODULE);
constexpr InstallFn kInstall[kSlots] = {&install_slot<0>, &install_slot<1>};
std::mutex g_install_m;

constexpr const wchar_t* kModuleNames[] = {L"libxess.dll", nullptr};
void install_cb(HMODULE m) { InstallForModule(m); }

} // namespace

void SetMode(Mode m) { g_mode.store(static_cast<uint32_t>(m), std::memory_order_relaxed); }
Mode GetMode() { return static_cast<Mode>(g_mode.load(std::memory_order_relaxed)); }

bool IsXessModuleName(const wchar_t* name, size_t len) { return na::upscalers::NameMatches(name, len, kModuleNames); }

uint32_t InstallForModule(HMODULE module)
{
    if (!module) return 0;
    std::lock_guard<std::mutex> lock(g_install_m);
    for (int i = 0; i < kSlots; ++i)
        if (g_slots[i].module.load(std::memory_order_acquire) == module) return 0;
    if (!na::hooking::Initialize()) return 0;
    for (int i = 0; i < kSlots; ++i) {
        if (g_slots[i].module.load(std::memory_order_acquire)) continue;
        const uint32_t n = kInstall[i](module);
        if (n) {
            std::lock_guard<std::mutex> g(g_m);
            ++g_stats.modules_hooked;
            g_stats.hooked_functions += n;
        }
        return n;
    }
    return 0;
}

bool StartModuleWatch() { return na::upscalers::AddModuleWatch(kModuleNames, &install_cb); }

bool LastObservation(Observation* out)
{
    std::lock_guard<std::mutex> lock(g_m);
    if (!out || !g_have_last) return false;
    *out = g_last;
    return true;
}

Stats GetStats()
{
    std::lock_guard<std::mutex> lock(g_m);
    return g_stats;
}

void ResetForTests()
{
    std::lock_guard<std::mutex> lock(g_m);
    const uint32_t modules = g_stats.modules_hooked, functions = g_stats.hooked_functions;
    for (ContextInfo& ci : g_contexts) ci = ContextInfo{};
    g_stats = Stats{};
    g_stats.modules_hooked = modules;
    g_stats.hooked_functions = functions;
    g_last = Observation{};
    g_have_last = false;
}

} // namespace na::xess
