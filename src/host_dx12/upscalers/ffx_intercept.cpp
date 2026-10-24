// C7a: FSR 3.1+ (FidelityFX API) interception. See ffx_intercept.hpp.
#include "host_dx12/upscalers/ffx_intercept.hpp"

#include "host_dx12/dlss/dlss_backend.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
#include "host_dx12/upscalers/module_watch.hpp"

#include <Kits/FidelityFX/api/include/ffx_api.h>
#include <Kits/FidelityFX/upscalers/include/ffx_upscale.h>

#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <mutex>

namespace na::ffx {

// FSR's jitterOffset and DLSS's Jitter.Offset are both documented as the
// sub-pixel offset applied to the camera projection, so they carry the same
// sign; DLSS's convention was measured in C6 (sample = pixel centre - jitter).
// The FSR side is NOT measured yet: it is scheduled for the Gate 2 hardware
// session with AMD's official runtime DLL (FSR also runs on NVIDIA GPUs).
constexpr float kFsrToDlssJitterSign = 1.0f;

bool ToDlss(const Observation& o, na::dlss::EvalDesc* out, const char** reason)
{
    const auto fail = [&](const char* why) {
        if (reason) *reason = why;
        return false;
    };
    if (!out) return fail("No output description.");
    if (!o.context_known)
        return fail("FSR's settings were created before NeuralAlias attached (HDR, depth and motion conventions unknown).");
    if (!o.color || !o.depth || !o.motion || !o.output) return fail("FSR was not given colour, depth, motion and an output.");
    const uint32_t ow = o.upscale_w ? o.upscale_w : o.max_upscale_w;
    const uint32_t oh = o.upscale_h ? o.upscale_h : o.max_upscale_h;
    if (!o.render_w || !o.render_h || !ow || !oh) return fail("FSR's render or output size is missing.");
    // DLSS reads inputs as shader resources and writes its output as a UAV.
    // FSR's states are passed by the game; only the matching layout is handled
    // without adding barriers to the game's list.
    const uint32_t kRead = FFX_API_RESOURCE_STATE_COMPUTE_READ;
    if (!(o.color_state & kRead) || !(o.depth_state & kRead) || !(o.motion_state & kRead))
        return fail("FSR's inputs are not in a shader-readable state DLSS can use as-is.");
    if (o.output_state != FFX_API_RESOURCE_STATE_UNORDERED_ACCESS)
        return fail("FSR's output is not in the writable (UAV) state DLSS needs.");
    if (o.context_flags & FFX_UPSCALE_ENABLE_DEPTH_INFINITE) {
        // DLSS takes raw depth; infinite far plane changes nothing it relies on.
    }
    na::dlss::EvalDesc d;
    d.color = o.color;
    d.depth = o.depth;
    d.motion = o.motion;
    d.output = o.output;
    d.exposure = o.exposure;
    d.render_w = o.render_w;
    d.render_h = o.render_h;
    d.out_w = ow;
    d.out_h = oh;
    d.jitter_x = kFsrToDlssJitterSign * o.jitter_x;
    d.jitter_y = kFsrToDlssJitterSign * o.jitter_y;
    d.mv_scale_x = o.mv_scale_x;
    d.mv_scale_y = o.mv_scale_y;
    d.pre_exposure = o.pre_exposure > 0.f ? o.pre_exposure : 1.f;
    d.reset = o.reset;
    d.hdr = (o.context_flags & FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE) != 0;
    d.depth_inverted = (o.context_flags & FFX_UPSCALE_ENABLE_DEPTH_INVERTED) != 0;
    d.mv_jittered = (o.context_flags & FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION) != 0;
    d.mv_display_res = (o.context_flags & FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS) != 0;
    d.auto_exposure = (o.context_flags & FFX_UPSCALE_ENABLE_AUTO_EXPOSURE) != 0;
    d.quality = na::dlss::QualityForScale(d.render_w, d.render_h, ow, oh);
    if (const char* bad = na::dlss::Validate(d)) return fail(bad);
    *out = d;
    if (reason) *reason = nullptr;
    return true;
}

namespace {

constexpr int kSlots = 4;
constexpr uint32_t kMaxContexts = 32;

struct Slot {
    std::atomic<HMODULE> module{nullptr};
    std::atomic<uint32_t> hooked{0};
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxConfigure configure = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxDispatch dispatch = nullptr;
};
Slot g_slots[kSlots];

// Which upscaler produced this context's previous frame (R-304).
enum class Backend : uint8_t { None, Native, Dlss };

struct ContextInfo {
    ffxContext context = nullptr;
    bool upscale = false;
    uint32_t flags = 0;
    uint32_t max_render_w = 0, max_render_h = 0, max_upscale_w = 0, max_upscale_h = 0;
    uint64_t view_id = 0;           // R-303: this context's own DLSS view (never reused)
    Backend last = Backend::None;
};
// FFX view ids: tag in the high bits, a counter that never repeats in the process.
constexpr uint64_t kViewTag = 1ull << 40;
std::atomic<uint64_t> g_next_view{1};

std::mutex g_m;
ContextInfo g_contexts[kMaxContexts];
Observation g_last{};
bool g_have_last = false;
Stats g_stats{};
std::atomic<uint32_t> g_mode{static_cast<uint32_t>(Mode::Observe)};
bool g_dlss_init_tried = false;

void set_reason(const char* why) { std::snprintf(g_stats.reason, sizeof(g_stats.reason), "%s", why ? why : ""); }

ContextInfo* find_context(ffxContext c)
{
    for (ContextInfo& ci : g_contexts)
        if (ci.context && ci.context == c) return &ci;
    return nullptr;
}

ID3D12Resource* res(const FfxApiResource& r) { return static_cast<ID3D12Resource*>(r.resource); }

// Records an upscale dispatch; returns the observation (under g_m).
Observation observe_locked(ffxContext* ctx, const ffxDispatchDescUpscale& u)
{
    Observation o;
    o.serial = ++g_stats.upscale_dispatches;
    o.context = ctx ? *ctx : nullptr;
    if (const ContextInfo* ci = find_context(o.context)) {
        o.context_known = ci->upscale;
        o.context_flags = ci->flags;
        o.max_render_w = ci->max_render_w;
        o.max_render_h = ci->max_render_h;
        o.max_upscale_w = ci->max_upscale_w;
        o.max_upscale_h = ci->max_upscale_h;
    }
    if (!o.context_known) ++g_stats.unknown_context;
    o.list = static_cast<ID3D12GraphicsCommandList*>(u.commandList);
    o.color = res(u.color);
    o.depth = res(u.depth);
    o.motion = res(u.motionVectors);
    o.exposure = res(u.exposure);
    o.reactive = res(u.reactive);
    o.transparency = res(u.transparencyAndComposition);
    o.output = res(u.output);
    o.color_state = u.color.state;
    o.depth_state = u.depth.state;
    o.motion_state = u.motionVectors.state;
    o.output_state = u.output.state;
    o.render_w = u.renderSize.width;
    o.render_h = u.renderSize.height;
    o.upscale_w = u.upscaleSize.width;
    o.upscale_h = u.upscaleSize.height;
    o.jitter_x = u.jitterOffset.x;
    o.jitter_y = u.jitterOffset.y;
    o.mv_scale_x = u.motionVectorScale.x;
    o.mv_scale_y = u.motionVectorScale.y;
    o.pre_exposure = u.preExposure;
    o.frame_time_ms = u.frameTimeDelta;
    o.camera_near = u.cameraNear;
    o.camera_far = u.cameraFar;
    o.camera_fov_y = u.cameraFovAngleVertical;
    o.reset = u.reset;
    o.sharpening = u.enableSharpening;
    o.sharpness = u.sharpness;
    o.dispatch_flags = u.flags;
    g_last = o;
    g_have_last = true;
    return o;
}

bool ensure_dlss_locked(ID3D12GraphicsCommandList* list)
{
    const na::dlss::Capabilities c = na::dlss::GetCapabilities();
    if (c.attached) return c.available && !c.latched_off;
    if (g_dlss_init_tried) return false;
    g_dlss_init_tried = true;
    Microsoft::WRL::ComPtr<ID3D12Device> dev;
    if (!list || FAILED(list->GetDevice(IID_PPV_ARGS(&dev)))) return false;
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetTempPathW(MAX_PATH, path);
    if (!n || n > MAX_PATH - 20) return false;
    wcscat_s(path, L"NeuralAlias-ngx");
    CreateDirectoryW(path, nullptr);
    return na::dlss::InitOwn(dev.Get(), path);
}

// True when DLSS produced this frame (FSR must then be skipped).
bool try_replace_locked(const Observation& o, ContextInfo* ci)
{
    na::dlss::EvalDesc d;
    const char* why = nullptr;
    if (!ToDlss(o, &d, &why)) {
        ++g_stats.fallbacks;
        set_reason(why);
        return false;
    }
    if (ci) {
        d.view_id = ci->view_id;
        if (ci->last != Backend::Dlss) d.reset = true; // R-304: DLSS did not see the previous frame
    }
    if (!ensure_dlss_locked(o.list)) {
        ++g_stats.fallbacks;
        const na::dlss::Capabilities c = na::dlss::GetCapabilities();
        set_reason(c.reason[0] ? c.reason : "DLSS could not start on this device.");
        return false;
    }
    if (!na::dlss::Evaluate(o.list, d)) {
        ++g_stats.fallbacks;
        set_reason(na::dlss::GetStats().reason);
        return false;
    }
    ++g_stats.replaced;
    set_reason("");
    return true;
}

template <int S>
ffxReturnCode_t DetourCreateContext(ffxContext* context, ffxCreateContextDescHeader* desc, const ffxAllocationCallbacks* mem)
{
    const ffxReturnCode_t r = g_slots[S].create(context, desc, mem);
    if (r != FFX_API_RETURN_OK || !context || !*context) return r;
    ContextInfo info;
    info.context = *context;
    info.view_id = kViewTag | g_next_view.fetch_add(1, std::memory_order_relaxed);
    for (const ffxApiHeader* h = desc; h; h = h->pNext) {
        if (h->type != FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE) continue;
        const auto* u = reinterpret_cast<const ffxCreateContextDescUpscale*>(h);
        info.upscale = true;
        info.flags = u->flags;
        info.max_render_w = u->maxRenderSize.width;
        info.max_render_h = u->maxRenderSize.height;
        info.max_upscale_w = u->maxUpscaleSize.width;
        info.max_upscale_h = u->maxUpscaleSize.height;
        break;
    }
    std::lock_guard<std::mutex> lock(g_m);
    ++g_stats.contexts_created;
    if (info.upscale) ++g_stats.upscale_contexts;
    ContextInfo* slot = find_context(info.context);
    for (ContextInfo& ci : g_contexts)
        if (!slot && !ci.context) slot = &ci;
    if (slot) *slot = info; // table full: dispatches report context_known=false (FSR keeps running)
    return r;
}

template <int S>
ffxReturnCode_t DetourDestroyContext(ffxContext* context, const ffxAllocationCallbacks* mem)
{
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (context)
            if (ContextInfo* ci = find_context(*context)) {
                na::dlss::ReleaseView(ci->view_id); // R-303: its DLSS history ends with it
                *ci = ContextInfo{};
            }
        ++g_stats.contexts_destroyed;
    }
    return g_slots[S].destroy(context, mem);
}

template <int S>
ffxReturnCode_t DetourConfigure(ffxContext* context, const ffxConfigureDescHeader* desc)
{
    return g_slots[S].configure(context, desc);
}

template <int S>
ffxReturnCode_t DetourQuery(ffxContext* context, ffxQueryDescHeader* desc)
{
    return g_slots[S].query(context, desc);
}

template <int S>
ffxReturnCode_t DetourDispatch(ffxContext* context, const ffxDispatchDescHeader* desc)
{
    if (desc && desc->type == FFX_API_DISPATCH_DESC_TYPE_UPSCALE) {
        const auto* u = reinterpret_cast<const ffxDispatchDescUpscale*>(desc);
        bool native_needs_reset = false;
        {
            std::lock_guard<std::mutex> lock(g_m);
            const Observation o = observe_locked(context, *u);
            ContextInfo* ci = find_context(o.context);
            if (static_cast<Mode>(g_mode.load(std::memory_order_relaxed)) == Mode::ReplaceWithDlss && try_replace_locked(o, ci)) {
                if (ci) ci->last = Backend::Dlss;
                return FFX_API_RETURN_OK; // DLSS wrote FSR's output on the game's list
            }
            // R-304: FSR skipped the frames DLSS produced; its history is stale.
            native_needs_reset = ci && ci->last == Backend::Dlss && !u->reset;
            if (ci) ci->last = Backend::Native;
        }
        if (native_needs_reset) {
            ffxDispatchDescUpscale copy = *u; // never modify the game's descriptor
            copy.reset = true;
            return g_slots[S].dispatch(context, &copy.header);
        }
    } else {
        std::lock_guard<std::mutex> lock(g_m);
        ++g_stats.other_dispatches;
    }
    return g_slots[S].dispatch(context, desc);
}

template <int S>
uint32_t install_slot(HMODULE m)
{
    Slot& slot = g_slots[S];
    struct Spec { const char* name; void* detour; void** original; };
    const Spec specs[] = {
        {"ffxCreateContext", reinterpret_cast<void*>(&DetourCreateContext<S>), reinterpret_cast<void**>(&slot.create)},
        {"ffxDestroyContext", reinterpret_cast<void*>(&DetourDestroyContext<S>), reinterpret_cast<void**>(&slot.destroy)},
        {"ffxConfigure", reinterpret_cast<void*>(&DetourConfigure<S>), reinterpret_cast<void**>(&slot.configure)},
        {"ffxQuery", reinterpret_cast<void*>(&DetourQuery<S>), reinterpret_cast<void**>(&slot.query)},
        {"ffxDispatch", reinterpret_cast<void*>(&DetourDispatch<S>), reinterpret_cast<void**>(&slot.dispatch)},
    };
    // Create, Destroy and Dispatch are required (their detours keep the context
    // table honest); Configure and Query are pass-through and hooked when possible.
    if (!GetProcAddress(m, "ffxDispatch") || !GetProcAddress(m, "ffxCreateContext") || !GetProcAddress(m, "ffxDestroyContext"))
        return 0;
    if (!na::hooking::BeginInstall()) return 0;
    uint32_t hooked = 0;
    for (size_t i = 0; i < sizeof(specs) / sizeof(specs[0]); ++i) {
        const Spec& s = specs[i];
        void* target = reinterpret_cast<void*>(GetProcAddress(m, s.name));
        const bool required = i == 0 || i == 1 || i == 4;
        if (target && na::hooking::PrepareHook(target, s.detour, s.original)) {
            ++hooked;
        } else if (required) {
            na::hooking::AbandonInstall();
            return 0;
        }
    }
    slot.hooked.store(hooked, std::memory_order_relaxed);
    slot.module.store(m, std::memory_order_release);
    if (!na::hooking::CommitInstall()) {
        slot.module.store(nullptr, std::memory_order_release);
        slot.hooked.store(0, std::memory_order_relaxed);
        return 0;
    }
    return hooked;
}

using InstallFn = uint32_t (*)(HMODULE);
constexpr InstallFn kInstall[kSlots] = {&install_slot<0>, &install_slot<1>, &install_slot<2>, &install_slot<3>};
std::mutex g_install_m;

constexpr const wchar_t* kModuleNames[] = {L"amd_fidelityfx_dx12.dll", L"amd_fidelityfx_loader_dx12.dll", nullptr};
void install_cb(HMODULE m) { InstallForModule(m); }

} // namespace

void SetMode(Mode m) { g_mode.store(static_cast<uint32_t>(m), std::memory_order_relaxed); }
Mode GetMode() { return static_cast<Mode>(g_mode.load(std::memory_order_relaxed)); }

bool IsFfxModuleName(const wchar_t* name, size_t len) { return na::upscalers::NameMatches(name, len, kModuleNames); }

uint32_t InstallForModule(HMODULE module)
{
    if (!module) return 0;
    std::lock_guard<std::mutex> lock(g_install_m);
    for (int i = 0; i < kSlots; ++i)
        if (g_slots[i].module.load(std::memory_order_acquire) == module) return 0; // already hooked
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
    return 0; // all slots used
}

bool StartModuleWatch() { return na::upscalers::AddModuleWatch(kModuleNames, &install_cb); }
void StopModuleWatch() { na::upscalers::StopModuleWatches(); }

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
    g_dlss_init_tried = false;
}

} // namespace na::ffx
