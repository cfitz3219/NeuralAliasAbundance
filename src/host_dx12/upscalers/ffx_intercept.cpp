#include "host_dx12/settings.hpp"
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
// sign. MEASURED (Gate 2 hardware session, 2026-10-24, RTX 5070 Ti, AMD's
// official amd_fidelityfx_dx12.dll 1.0.1.41314, upscaler_jitter_test): FSR
// expects sample = pixel centre - jitter on both axes (+3.3 dB over bilinear;
// every other convention lost), the same as DLSS (C6) -> sign +1 per axis.
constexpr float kFsrToDlssJitterSign = 1.0f;

bool D3D12State(uint32_t ffx, D3D12_RESOURCE_STATES* out)
{
    switch (ffx) {
    case FFX_API_RESOURCE_STATE_COMMON: *out = D3D12_RESOURCE_STATE_COMMON; return true;
    case FFX_API_RESOURCE_STATE_UNORDERED_ACCESS: *out = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; return true;
    case FFX_API_RESOURCE_STATE_COMPUTE_READ: *out = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; return true;
    case FFX_API_RESOURCE_STATE_PIXEL_READ: *out = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; return true;
    case FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ: *out = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE; return true;
    case FFX_API_RESOURCE_STATE_COPY_SRC: *out = D3D12_RESOURCE_STATE_COPY_SOURCE; return true;
    case FFX_API_RESOURCE_STATE_COPY_DEST: *out = D3D12_RESOURCE_STATE_COPY_DEST; return true;
    case FFX_API_RESOURCE_STATE_GENERIC_READ:
        *out = D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; return true;
    default: return false;
    }
}

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
    // ARGON-G2-003: any FSR state that has a D3D12 equivalent is accepted; the
    // replace path transitions to DLSS's states around the evaluation and back.
    D3D12_RESOURCE_STATES s{};
    if (!D3D12State(o.color_state, &s) || !D3D12State(o.depth_state, &s) || !D3D12State(o.motion_state, &s) ||
        !D3D12State(o.output_state, &s) || (o.exposure && !D3D12State(o.exposure_state, &s)))
        return fail("FSR described a resource state NeuralAlias cannot translate for DLSS.");
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
    // ARGON-G2-001: the context's maximum render size fixes the feature size and
    // quality mode, so dynamic resolution never recreates or resets DLSS.
    d.max_render_w = (o.max_render_w >= o.render_w && o.max_render_w <= ow) ? o.max_render_w : o.render_w;
    d.max_render_h = (o.max_render_h >= o.render_h && o.max_render_h <= oh) ? o.max_render_h : o.render_h;
    d.quality = na::dlss::QualityForScale(d.max_render_w, d.max_render_h, ow, oh);
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
    o.exposure_state = u.exposure.state;
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

// True when DLSS produced this frame (FSR must then be skipped).
// Runs WITHOUT g_m (ARGON-G2-005). On failure *why is the reason.
bool try_replace(const Observation& o, uint64_t view_id, Backend last, const char** why_out, uint32_t* barriers)
{
    na::dlss::EvalDesc d;
    const char* why = nullptr;
    if (!ToDlss(o, &d, &why)) {
        *why_out = why;
        return false;
    }
    d.view_id = view_id;
    if (last != Backend::Dlss) d.reset = true; // R-304: DLSS did not see the previous frame
    Microsoft::WRL::ComPtr<ID3D12Device> dev;
    if (!o.list || FAILED(o.list->GetDevice(IID_PPV_ARGS(&dev))) || !ensure_dlss(dev.Get())) {
        *why_out = "DLSS could not start on this device.";
        return false;
    }
    // ARGON-G2-003: move resources into DLSS's states and back afterwards.
    struct Want { ID3D12Resource* r; uint32_t ffx; D3D12_RESOURCE_STATES dlss; };
    const Want wants[] = {
        {o.color, o.color_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
        {o.depth, o.depth_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
        {o.motion, o.motion_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
        {o.output, o.output_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
        {o.exposure, o.exposure_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
    };
    D3D12_RESOURCE_BARRIER to[5] = {}, back[5] = {};
    if (o.list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT &&
        o.list->GetType() != D3D12_COMMAND_LIST_TYPE_COMPUTE) {
        *why_out = "DLSS requires a graphics or compute command list.";
        return false;
    }
    // FFX declares whole-resource states. Reject aliased roles with conflicting
    // requirements before recording any transitions (subresource views need C13).
    for (size_t i = 0; i < 5; ++i) {
        if (!wants[i].r) continue;
        D3D12_RESOURCE_STATES state{};
        D3D12State(wants[i].ffx, &state);
        if (o.list->GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE &&
            (state & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)) {
            *why_out = "FSR's graphics resource state cannot be restored on a compute list.";
            return false;
        }
        for (size_t j = 0; j < i; ++j) {
            if (wants[i].r == wants[j].r &&
                (wants[i].ffx != wants[j].ffx || wants[i].dlss != wants[j].dlss)) {
                *why_out = "FSR resource roles require conflicting states.";
                return false;
            }
        }
    }
    UINT n = 0;
    for (size_t i = 0; i < 5; ++i) {
        const Want& w = wants[i];
        if (!w.r) continue;
        bool duplicate = false;
        for (size_t j = 0; j < i; ++j) duplicate |= wants[j].r == w.r;
        if (duplicate) continue;
        D3D12_RESOURCE_STATES game{};
        D3D12State(w.ffx, &game);
        // Already readable by compute (or already a UAV for the output): no barrier.
        if (game == w.dlss || (w.dlss == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE &&
                               (game & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) && game != D3D12_RESOURCE_STATE_GENERIC_READ))
            continue;
        to[n].Type = back[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to[n].Transition.pResource = back[n].Transition.pResource = w.r;
        to[n].Transition.Subresource = back[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        to[n].Transition.StateBefore = back[n].Transition.StateAfter = game;
        to[n].Transition.StateAfter = back[n].Transition.StateBefore = w.dlss;
        ++n;
    }
    if (n) o.list->ResourceBarrier(n, to);
    const bool ok = na::dlss::Evaluate(o.list, d);
    if (n) o.list->ResourceBarrier(n, back); // restore exactly what FSR was promised
    if (!ok) {
        *why_out = nullptr; // reason is in na::dlss::GetStats()
        return false;
    }
    *barriers = n;
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
    uint64_t view = 0;
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (context)
            if (ContextInfo* ci = find_context(*context)) {
                view = ci->view_id;
                *ci = ContextInfo{};
            }
        ++g_stats.contexts_destroyed;
    }
    if (view) na::dlss::ReleaseView(view); // backend calls never nest under g_m
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
        Observation o;
        uint64_t view_id = 0;
        Backend last = Backend::None;
        bool replace = false;
        {
            std::lock_guard<std::mutex> lock(g_m);
            o = observe_locked(context, *u);
            if (const ContextInfo* ci = find_context(o.context)) { view_id = ci->view_id; last = ci->last; }
            replace = na::settings::EffectsReady() && static_cast<Mode>(g_mode.load(std::memory_order_relaxed)) == Mode::ReplaceWithDlss;
        }
        // ARGON-G2-005: NGX start-up and evaluation run without g_m.
        const char* why = nullptr;
        uint32_t barriers = 0;
        const bool replaced = replace && try_replace(o, view_id, last, &why, &barriers);
        // ARGON-G2-005: the backend's lock is never taken under g_m.
        const na::dlss::Stats backend = (replace && !replaced && !why) ? na::dlss::GetStats() : na::dlss::Stats{};
        {
            std::lock_guard<std::mutex> lock(g_m);
            ContextInfo* ci = find_context(o.context);
            if (ci && ci->view_id != view_id) ci = nullptr; // destroyed and recreated meanwhile
            if (replaced) {
                ++g_stats.replaced;
                g_stats.barriers += barriers;
                set_reason("");
                if (ci) ci->last = Backend::Dlss;
                return FFX_API_RETURN_OK; // DLSS wrote FSR's output on the game's list
            }
            if (replace) {
                ++g_stats.fallbacks;
                set_reason(why ? why : backend.reason);
            }
            // R-304: FSR skipped the frames DLSS produced; its history is stale.
            native_needs_reset = ci && ci->last == Backend::Dlss && !u->reset;
        }
        ffxReturnCode_t rc;
        if (native_needs_reset) {
            ffxDispatchDescUpscale copy = *u; // never modify the game's descriptor
            copy.reset = true;
            rc = g_slots[S].dispatch(context, &copy.header);
        } else {
            rc = g_slots[S].dispatch(context, desc);
        }
        // R-309: FSR owns the history only once its frame actually ran. After a
        // failed native call the context keeps its previous owner, so the next
        // FSR frame after DLSS still resets.
        if (rc == FFX_API_RETURN_OK) {
            std::lock_guard<std::mutex> lock(g_m);
            ContextInfo* ci = find_context(o.context);
            if (ci && ci->view_id == view_id) ci->last = Backend::Native;
        }
        return rc;
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
    if (!na::hooking::BeginInstallWait("upscalers/ffx_intercept.cpp")) return 0;
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
}

} // namespace na::ffx
