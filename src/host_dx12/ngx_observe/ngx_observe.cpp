#include "host_dx12/dlss/call_origin.hpp"
#include "host_dx12/ngx_observe/ngx_observe.hpp"

#include "host_dx12/nr/nr_pass.hpp"
#include "host_dx12/nr/native_fg.hpp"
#include "host_dx12/nr/queue_trace.hpp"
#include "host_dx12/motion/live_flow.hpp"

#include "host_dx12/capture/frame_capture.hpp"
#include "host_dx12/fg/frame_gen.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
#include "host_dx12/ngx_observe/gpu_timing.hpp"
#include "host_dx12/ngx_observe/size_confirm.hpp"
#include "host_dx12/settings.hpp"

#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <vector>
#include <array>
#include <utility>
#include <algorithm>

#if defined(NA_HAVE_NGX_SDK)
// Locally pinned NVIDIA SDK headers (never committed; see third_party/README).
#include <d3d11.h>
#include <d3d12.h>
#pragma warning(push, 3)
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_defs_dlssd.h> // Ray Reconstruction: preset keys and DLSSDOptimalSettingsCallback
#include <nvsdk_ngx_params.h>
#include "host_dx12/nr/fg_contract.hpp"
#pragma warning(pop)
#endif

namespace na::ngx {
namespace {

// ---- Signatures mirroring the pinned NVIDIA SDK (opaque pointer params). ----
using NgxResult = int32_t;
using PfnInitExt = NgxResult(__cdecl*)(unsigned long long app_id,
                                       const wchar_t* data_path, void* device,
                                       uint32_t sdk_version, const void* params);
using PfnInitProjectId = NgxResult(__cdecl*)(const char* project_id,
                                             uint32_t engine_type,
                                             const char* engine_version,
                                             const wchar_t* data_path,
                                             void* device, uint32_t sdk_version,
                                             const void* params);
using PfnCreateFeature = NgxResult(__cdecl*)(void* cmd_list, uint32_t feature_id,
                                             void* params, void** out_handle);
using PfnEvaluateFeature = NgxResult(__cdecl*)(void* cmd_list, const void* handle,
                                               const void* params, void* callback);
using PfnReleaseFeature = NgxResult(__cdecl*)(void* handle);
using PfnShutdown1 = NgxResult(__cdecl*)(void* device);
using PfnGetCapParams = NgxResult(__cdecl*)(void** out_params);

// A43: one set of ORIGINAL entry points PER HOOKED MODULE. Session 6 root
// cause: a single shared set was overwritten each time another module with
// the same export names was hooked (the driver ships two cores, _nvngx.dll
// and nvngx.dll, and every feature DLL â€” nvngx_dlss.dll, nvngx_dlssnr.dll â€”
// exports the same names). After the Neural Rendering runtime was placed
// beside the game, the game's DLSS CreateFeature was forwarded INTO THE NR
// DLL, which answered 0xBAD00002 for every DLSS creation. Now each module
// gets its own slot and detours, and feature DLLs are never hooked.
struct CoreSlot {
    std::atomic<bool> has_device{false};
    std::atomic<HMODULE> module{nullptr};
    PfnInitExt init_ext = nullptr;
    PfnInitProjectId init_pid = nullptr;
    PfnCreateFeature create = nullptr;
    PfnEvaluateFeature evaluate = nullptr;
    PfnReleaseFeature release = nullptr;
    PfnShutdown1 shutdown1 = nullptr;
    PfnGetCapParams getcap = nullptr;
    PfnGetCapParams getparams = nullptr;   // NVSDK_NGX_D3D12_GetParameters (deprecated path)
    PfnGetCapParams allocate = nullptr; // public AllocateParameters path
    std::atomic<uint32_t> hooked{0};       // exports hooked in this module
    std::atomic<uint32_t> init_succeeded{0}; // the game initialised THIS core
};
constexpr int kMaxSlots = 2; // the driver's two cores; a third core is refused
CoreSlot g_slots[kMaxSlots];

// ---- Counters and record ring (hot path: atomics only). ----
std::atomic<uint64_t> g_inits{0}, g_creates{0}, g_evaluates{0}, g_releases{0},
    g_shutdowns{0}, g_dropped{0};
std::atomic<uint32_t> g_hooked{0};
std::atomic<uint32_t> g_module_attached{0};

constexpr uint32_t kRingSize = 4096; // power of two
CallRecord g_ring[kRingSize];
std::atomic<uint64_t> g_ring_write{0};  // next slot to claim
std::atomic<uint64_t> g_ring_commit{0}; // slots fully written
uint64_t g_ring_read = 0;               // single consumer

void push_record(CallKind kind, int32_t result, uint32_t feature_id,
                 uint64_t app_id, uint64_t handle, uint64_t cmdlist,
                 const EvalParams* params = nullptr)
{
    uint64_t slot = g_ring_write.load(std::memory_order_relaxed);
    for (;;) {
        if (slot - g_ring_read >= kRingSize) {
            // Drop-newest under overflow; the consumer is a diagnostics
            // thread, so precise g_ring_read staleness only over-drops.
            g_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (g_ring_write.compare_exchange_weak(slot, slot + 1,
                                               std::memory_order_acq_rel))
            break;
    }
    CallRecord& r = g_ring[slot & (kRingSize - 1)];
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    r.qpc_ticks = static_cast<uint64_t>(qpc.QuadPart);
    r.kind = static_cast<uint32_t>(kind);
    r.thread_id = GetCurrentThreadId();
    r.ngx_result = result;
    r.feature_id = feature_id;
    r.app_id = app_id;
    r.handle = handle;
    r.cmdlist = cmdlist;
    if (params)
        r.params = *params;
    else
        std::memset(&r.params, 0, sizeof(r.params));
    // Publish in order: spin until predecessors commit (bounded by producers
    // racing in the same instant; detour work is tiny).
    uint64_t expected = slot;
    while (!g_ring_commit.compare_exchange_weak(expected, slot + 1,
                                                std::memory_order_release))
        expected = slot;
}

// ---- Detours. Each is a thin per-slot template around an Impl that takes
// the slot, so the forwarded ORIGINAL always belongs to the module the game
// actually called (A43). InstallForModule is idempotent per module. ----
// A43: once the game has initialised a core, the live NR pass must create
// its feature through THAT core's entry points (one-time work at Init, not
// per frame; SetRuntime only stores pointers under the pass's mutex).
na::nr::Runtime core_runtime(CoreSlot& s)
{
    HMODULE m = s.module.load(std::memory_order_acquire);
    na::nr::Runtime rt;
    if (!m) return rt;
    rt.create_feature = reinterpret_cast<void*>(s.create);
    rt.evaluate_feature = reinterpret_cast<void*>(s.evaluate);
    rt.release_feature = reinterpret_cast<void*>(s.release);
    rt.allocate_parameters = reinterpret_cast<void*>(GetProcAddress(m, "NVSDK_NGX_D3D12_AllocateParameters"));
    rt.destroy_parameters = reinterpret_cast<void*>(GetProcAddress(m, "NVSDK_NGX_D3D12_DestroyParameters"));
    rt.get_capability_parameters = reinterpret_cast<void*>(s.getcap);
    return rt;
}
void on_core_initialised(CoreSlot& s)
{
    if (!s.module.load() || s.init_succeeded.exchange(1)) return;
    na::nr::SetRuntime(core_runtime(s));
}

NgxResult InitExtImpl(CoreSlot& s, unsigned long long app_id, const wchar_t* data_path, void* device,
                      uint32_t sdk_version, const void* params)
{
    NgxResult r = s.init_ext(app_id, data_path, device, sdk_version, params);
    if (r == 1) { s.has_device.store(device!=nullptr); on_core_initialised(s); na::nr::native::PrepareDevice(static_cast<ID3D12Device*>(device)); }
    g_inits.fetch_add(1, std::memory_order_relaxed);
    push_record(CallKind::Init, r, 0, app_id, 0, 0);
    return r;
}

NgxResult InitProjectIdImpl(CoreSlot& s, const char* project_id, uint32_t engine_type, const char* engine_version,
                            const wchar_t* data_path, void* device, uint32_t sdk_version, const void* params)
{
    NgxResult r = s.init_pid(project_id, engine_type, engine_version, data_path, device, sdk_version, params);
    if (r == 1) { s.has_device.store(device!=nullptr); on_core_initialised(s); na::nr::native::PrepareDevice(static_cast<ID3D12Device*>(device)); }
    g_inits.fetch_add(1, std::memory_order_relaxed);
    push_record(CallKind::Init, r, 0, 0, 0, 0);
    return r;
}

std::atomic<bool> g_detail{false};
// A66 (R1): request-scoped size confirmation; the flag keeps the evaluate
// hot path atomics-only while nothing is armed.
std::atomic<bool> g_confirm_active{false};
std::mutex g_confirm_m;
confirm::Monitor g_confirm;
confirm::FeatureFlags g_feature_flags; // A68: creation flags per game handle (guarded by g_confirm_m)
std::atomic<uint32_t> g_rr_seen{0};
std::atomic<uint32_t> g_active_upscaler{0}; // 1 or 13: the game's most recent successful upscaler creation
std::atomic<uint32_t> g_fg_seen{0};
struct FgHandle { const CoreSlot* core = nullptr; const void* handle = nullptr; };
FgHandle g_fg_handles[32]; // guarded by g_confirm_m; do not infer an evaluate from a creation
std::atomic<uint64_t> g_fg_evaluates{0}, g_fg_with_inputs{0}, g_fg_with_hudless{0}, g_fg_with_distortion{0};
std::atomic<uint32_t> g_fg_list_type{0xFFFFFFFFu}, g_fg_color_format{0}, g_fg_color_w{0}, g_fg_color_h{0};
std::atomic<uint64_t> g_fg_with_frame_id{0}, g_fg_with_alpha{0}, g_fg_with_ui{0};
std::atomic<uint32_t> g_fg_last_create_uir{UINT32_MAX};

uint32_t inspect_fg_create_uir(const void* raw)
{
    uint32_t value = UINT32_MAX;
#if defined(NA_HAVE_NGX_SDK)
    __try {
        if (raw) na::nr::FgRead(static_cast<const NVSDK_NGX_Parameter*>(raw),
                               "DLSSG.UserInterfaceRecompositionEnabled", &value);
    } __except (EXCEPTION_EXECUTE_HANDLER) { value = UINT32_MAX; }
#else
    (void)raw;
#endif
    return value;
}

void inspect_fg_inputs(void* command_list, const void* raw)
{
    g_fg_evaluates.fetch_add(1, std::memory_order_relaxed);
#if defined(NA_HAVE_NGX_SDK)
    if (!raw) return;
    __try {
        const auto* p = static_cast<const NVSDK_NGX_Parameter*>(raw);
        const auto frame = na::nr::ReadFgContract(p);
        auto* color = frame.color.resource;
        if (command_list) g_fg_list_type.store(static_cast<ID3D12GraphicsCommandList*>(command_list)->GetType());
        if (color) {
            const auto desc = color->GetDesc();
            g_fg_color_format.store(desc.Format); g_fg_color_w.store(static_cast<uint32_t>(desc.Width)); g_fg_color_h.store(desc.Height);
        }
        if (command_list && color && frame.depth.resource && frame.motion.resource) g_fg_with_inputs.fetch_add(1);
        if (frame.hudless.resource) g_fg_with_hudless.fetch_add(1);
        if (frame.distortion.resource) g_fg_with_distortion.fetch_add(1);
        if (frame.has_frame_id) g_fg_with_frame_id.fetch_add(1);
        if (frame.alpha.resource) g_fg_with_alpha.fetch_add(1);
        if (frame.ui.resource) g_fg_with_ui.fetch_add(1);
    } __except (EXCEPTION_EXECUTE_HANDLER) { /* diagnostics must not interrupt native FG */ }
#else
    (void)command_list; (void)raw;
#endif
}
std::atomic<HMODULE> g_hooked_module{nullptr};
std::atomic<uint64_t> g_sr_creates{0};
std::atomic<uint64_t> g_sr_creates_unknown{0}, g_sr_creates_injector{0};
std::atomic<uint64_t> g_evaluates_external{0}; // the game's own evaluations (never the injector's)
std::atomic<uint32_t> g_last_sr_in_w{0}, g_last_sr_in_h{0}, g_last_sr_out_w{0}, g_last_sr_out_h{0};
std::atomic<uint64_t> g_create_failures{0}, g_create_recovered{0};
std::atomic<uint32_t> g_preset_rejected{0}, g_quality_rejected{0};
std::atomic<const void*> g_last_sr_handle{nullptr}; // A46: the feature "Apply now" re-creates
std::mutex g_reapply_mutex;               // rare path only (apply / release)
CoreSlot* g_last_sr_core=nullptr; // guarded with the handle and snapshot by g_reapply_mutex
using SrReceipt=std::shared_ptr<const std::atomic<bool>>;
struct SrUses {
    std::shared_ptr<std::atomic<uint32_t>> calls=std::make_shared<std::atomic<uint32_t>>(0);
    CoreSlot* core=nullptr;
    const void* handle=nullptr;
    EvalParams create{};
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    std::vector<SrReceipt> receipts;
    bool untracked=false;
};
std::vector<SrUses> g_sr_uses; // g_reapply_mutex
void AddSrUse(SrUses& use,const SrReceipt& receipt) {
    use.receipts.erase(std::remove_if(use.receipts.begin(),use.receipts.end(),
        [](const auto& r){return r->load(std::memory_order_acquire);}),use.receipts.end());
    if(!receipt)use.untracked=true;
    else if(std::find(use.receipts.begin(),use.receipts.end(),receipt)==use.receipts.end())use.receipts.push_back(receipt);
}
constexpr uint32_t kFeatureRayReconstruction = 13; // NVSDK_NGX_Feature_RayReconstruction

// A66 (R3): bounded per-creation records. The detour captures only raw
// return addresses, the thread id and the sizes (no module lookups, no
// formatting); GetCreateRecords resolves modules when it is read.
constexpr uint32_t kCreateRecords = 16;
struct RawCreate {
    uint64_t seq, tick, confirm_id;
    uint32_t thread_id, feature_id;
    int32_t result;
    uint32_t in_w, in_h, out_w, out_h;
    uint32_t frame_count;
    void* frames[24];
};
std::mutex g_create_rec_m;
RawCreate g_create_recs[kCreateRecords] = {};
uint64_t g_create_seq = 0; // guarded by g_create_rec_m
std::atomic<uint64_t> g_create_records{0};

// A71: the game's Streamline settings calls (see RecordSlCall), same shape.
struct RawSlCall {
    uint64_t seq, tick;
    uint32_t thread_id, kind, feature;
    uint32_t raw[3];
    uint32_t repeats;
    uint32_t frame_count;
    void* frames[24];
};
constexpr uint32_t kSlCallRecords = 32;
RawSlCall g_sl_recs[kSlCallRecords] = {}; // guarded by g_create_rec_m
uint64_t g_sl_seq = 0;

// Resolves raw return addresses to module + RVA (read time only, never in a
// detour). Frames inside this module are skipped; unknown ones are kept as
// `game = 2` so the chain stays complete.
uint32_t resolve_frames(void* const* raw, uint32_t n, CreateFrame* out, uint32_t cap, HMODULE self, HMODULE exe)
{
    uint32_t count = 0;
    for (uint32_t f = 0; f < n && count < cap; ++f) {
        HMODULE m = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                static_cast<LPCWSTR>(raw[f]), &m) || !m) {
            CreateFrame& cf = out[count++];
            cf = CreateFrame{};
            cf.address = reinterpret_cast<uint64_t>(raw[f]);
            cf.game = 2;
            continue;
        }
        if (m == self) continue;
        CreateFrame& cf = out[count++];
        cf = CreateFrame{};
        cf.address = reinterpret_cast<uint64_t>(raw[f]);
        cf.rva = cf.address - reinterpret_cast<uint64_t>(m);
        cf.game = m == exe ? 1u : 0u;
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(m, path, MAX_PATH);
        const wchar_t* name = path;
        for (const wchar_t* p = path; *p; ++p) if (*p == L'\\' || *p == L'/') name = p + 1;
        WideCharToMultiByte(CP_ACP, 0, name, -1, cf.module, sizeof(cf.module), nullptr, nullptr);
        cf.module[sizeof(cf.module) - 1] = 0;
    }
    return count;
}

NgxResult CreateFeatureImpl(CoreSlot& s, void* cmd_list, uint32_t feature_id, void* params, void** out_handle)
{
    const PfnCreateFeature g_orig_create = s.create; // this module's original
    // C6 owns these settings/history. Do not apply game overrides, remember its
    // handle as the game's Apply target, or classify its creation as unknown.
    if (na::dlss::IsInjectorCall()) {
        const NgxResult r = g_orig_create(cmd_list, feature_id, params, out_handle);
        g_creates.fetch_add(1, std::memory_order_relaxed);
        if (r == 1 && feature_id == 1) {
            g_sr_creates_injector.fetch_add(1, std::memory_order_release);
            g_sr_creates.fetch_add(1, std::memory_order_release);
        }
        if (r != 1) g_create_failures.fetch_add(1, std::memory_order_relaxed);
        push_record(CallKind::CreateFeature, r, feature_id, 0,
                    out_handle && *out_handle ? reinterpret_cast<uint64_t>(*out_handle) : 0,
                    reinterpret_cast<uint64_t>(cmd_list));
        return r;
    }
    void* raw_frames[24] = {};
    USHORT raw_n = 0;
    if (feature_id == 1) raw_n = RtlCaptureStackBackTrace(1, 24, raw_frames, nullptr); // A66/A71: evidence, resolved later

    if (feature_id == kFeatureRayReconstruction) g_rr_seen.store(1, std::memory_order_release);
    if (feature_id == 11) g_fg_seen.store(1, std::memory_order_release); // NVSDK_NGX_Feature_FrameGeneration
    EvalParams before{};
    ReadEvalParams(params, &before); // the game's own quality value, for the fallback
    // Takeover step: write hints into the game's UPSCALER, whichever it creates
    // (Super Resolution or Ray Reconstruction); other features are left alone.
    bool overrode = ApplyCreateOverridesForFeature(feature_id, params);
    EvalParams ep{};
    ReadEvalParams(params, &ep); // read before: capture inputs even on failure
    ep.overridden = overrode ? 1u : 0u;
    NgxResult r = g_orig_create(cmd_list, feature_id, params, out_handle);
    // A42 fallback (session 6 finding: preset L made every DLSS creation fail
    // with 0xBAD00002 and the game ran without DLSS at all). If OUR writes
    // made the creation fail, retry without the preset hints; if that still
    // fails, retry with the game's own quality value. Never leave the game
    // without DLSS because of a hint.
    if (r != 1 && (feature_id == 1 || feature_id == kFeatureRayReconstruction) && overrode) {
        const bool rr = feature_id == kFeatureRayReconstruction;
        if (rr ? StripRrPresetHints(params) : StripPresetHints(params)) {
            g_preset_rejected.store(1, std::memory_order_release);
            if (rr) na::settings::Get().rr_preset.store(0, std::memory_order_release); // menu saves this
            else na::settings::Get().preset.store(-1, std::memory_order_release);
            r = g_orig_create(cmd_list, feature_id, params, out_handle);
        }
        if (r != 1 && before.perf_quality != 0xFFFFFFFFu && RestoreQualityValue(params, before.perf_quality)) {
            g_quality_rejected.store(1, std::memory_order_release);
            r = g_orig_create(cmd_list, feature_id, params, out_handle);
        }
        if (r == 1) g_create_recovered.fetch_add(1, std::memory_order_relaxed);
    }
    if (r != 1) g_create_failures.fetch_add(1, std::memory_order_relaxed);
    if (r == 1 && feature_id == 11 && out_handle && *out_handle) {
        na::nr::native::Release(&s,*out_handle); // new feature incarnation, even if the core reuses a handle address
        na::nr::native::Register(&s,*out_handle); // compute FG owns the route too, before its first evaluate
        na::nr::native::Prepare(static_cast<ID3D12GraphicsCommandList*>(cmd_list));
        g_fg_last_create_uir.store(inspect_fg_create_uir(params));
        std::lock_guard<std::mutex> lock(g_confirm_m);
        for (auto& entry : g_fg_handles) if (!entry.handle || (entry.core == &s && entry.handle == *out_handle)) {
            entry = {&s, *out_handle}; break;
        }
    }
    if (r == 1 && (feature_id == 1 || feature_id == kFeatureRayReconstruction))
        g_active_upscaler.store(feature_id, std::memory_order_release); // the game's upscaler NOW
    if (r == 1 && feature_id == kFeatureRayReconstruction) {
        // Ray Reconstruction upscales too: the size it was created with is the menu's
        // "active" evidence, as for Super Resolution. (Not the A46/A63 re-creation
        // snapshot, which rebuilds a Super Resolution feature.)
        g_last_sr_in_w.store(ep.in_width, std::memory_order_relaxed);
        g_last_sr_in_h.store(ep.in_height, std::memory_order_relaxed);
        g_last_sr_out_w.store(ep.out_width, std::memory_order_relaxed);
        g_last_sr_out_h.store(ep.out_height, std::memory_order_relaxed);
        // ...and it is the feature an armed render-size confirmation (Apply) waits for.
        std::lock_guard<std::mutex> lock(g_confirm_m);
        const uint64_t hnd = out_handle ? reinterpret_cast<uint64_t>(*out_handle) : 0;
        g_feature_flags.Set(hnd, ep.valid ? ep.create_flags : 0xFFFFFFFFu);
        if (g_confirm_active.load(std::memory_order_acquire))
            g_confirm.OnCreate(ep.valid ? ep.in_width : 0, ep.valid ? ep.in_height : 0, hnd, GetTickCount64());
    }
    if (r == 1 && feature_id == 1) { // NVSDK_NGX_Result_Success, DLSS super resolution
        // A30: evidence for the menu's "active" state â€” the size the game
        // actually created its DLSS feature with (poison 0xFFFFFFFF = unread).
        g_last_sr_in_w.store(ep.in_width, std::memory_order_relaxed);
        g_last_sr_in_h.store(ep.in_height, std::memory_order_relaxed);
        g_last_sr_out_w.store(ep.out_width, std::memory_order_relaxed);
        g_last_sr_out_h.store(ep.out_height, std::memory_order_relaxed);
        g_sr_creates.fetch_add(1, std::memory_order_release);
        g_sr_creates_unknown.fetch_add(1, std::memory_order_release);
        if (out_handle && *out_handle) {
            const auto receipt=s.has_device.load()?na::nr::native::TrackUse(static_cast<ID3D12GraphicsCommandList*>(cmd_list)):SrReceipt{};
            std::lock_guard<std::mutex> lock(g_reapply_mutex);
            g_last_sr_handle.store(*out_handle,std::memory_order_release);
            g_last_sr_core=&s;
            SrUses uses;uses.core=&s;uses.handle=*out_handle;uses.create=ep;AddSrUse(uses,receipt);
            if(receipt)static_cast<ID3D12GraphicsCommandList*>(cmd_list)->GetDevice(IID_PPV_ARGS(&uses.device));
            g_sr_uses.push_back(std::move(uses));
        }
        {
            std::lock_guard<std::mutex> lock(g_confirm_m);
            uint64_t hnd = out_handle ? reinterpret_cast<uint64_t>(*out_handle) : 0;
            g_feature_flags.Set(hnd, ep.valid ? ep.create_flags : 0xFFFFFFFFu); // A68: flags bound to this feature
            if (g_confirm_active.load(std::memory_order_acquire)) { // A66 (R1): the feature the request waits for
                uint32_t w = ep.valid ? ep.in_width : 0, h = ep.valid ? ep.in_height : 0;
                g_confirm.OnCreate(w, h, hnd, GetTickCount64());
            }
        }
    }
    if (feature_id == 1) { // A66 (R3): bounded record, raw addresses only
        uint64_t confirm_id = 0;
        if (g_confirm_active.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(g_confirm_m);
            confirm_id = g_confirm.snapshot().id;
        }
        std::lock_guard<std::mutex> lock(g_create_rec_m);
        RawCreate& rc = g_create_recs[g_create_seq % kCreateRecords];
        rc = RawCreate{};
        rc.seq = ++g_create_seq;
        rc.tick = GetTickCount64();
        rc.confirm_id = confirm_id;
        rc.thread_id = GetCurrentThreadId();
        rc.feature_id = feature_id;
        rc.result = r;
        rc.in_w = ep.valid ? ep.in_width : 0xFFFFFFFFu;
        rc.in_h = ep.valid ? ep.in_height : 0xFFFFFFFFu;
        rc.out_w = ep.valid ? ep.out_width : 0xFFFFFFFFu;
        rc.out_h = ep.valid ? ep.out_height : 0xFFFFFFFFu;
        rc.frame_count = raw_n;
        for (USHORT i = 0; i < raw_n; ++i) rc.frames[i] = raw_frames[i];
        g_create_records.store(g_create_seq, std::memory_order_release);
    }
    g_creates.fetch_add(1, std::memory_order_relaxed);
    uint64_t handle =
        (out_handle && *out_handle) ? reinterpret_cast<uint64_t>(*out_handle) : 0;
    ep.preset_hint = CreatePresetHint(feature_id, params); // what the runtime was finally handed (after any A42 retry)
    push_record(CallKind::CreateFeature, r, feature_id, 0, handle,
                reinterpret_cast<uint64_t>(cmd_list), &ep);
    return r;
}

// ---- A46 "Apply now": re-create the game's DLSS feature in place ----
// The game only re-creates its DLSS feature when the user changes the DLSS
// mode in its own menu. For settings that do not change the render SIZE
// (preset hints), we can re-create the feature ourselves on the game's own
// command list at the next evaluate, using the same parameter object the
// game evaluates with (Streamline keeps one object per feature with the
// creation keys still set), then map the game's handle to ours: evaluates
// and the eventual release are forwarded to the new feature; the old one is
// retained until its recorded commands are discarded or finish on the GPU.
// Size changes (DLAA/Quality/...) still need the game to rebuild its render
// targets; that is stated in the menu and cannot be done from here.
constexpr int kMaxHandleMaps = 4;
struct HandleMap {
    CoreSlot* core=nullptr; // all map access is guarded by g_reapply_mutex
    std::atomic<const void*> game{nullptr};
    std::atomic<const void*> ours{nullptr};
    std::atomic<void*> params{nullptr}; // A63: our own parameter object when the feature was built from scratch
};
HandleMap g_handle_maps[kMaxHandleMaps];
struct Retired { SrUses uses; void* params=nullptr; };
std::vector<Retired> g_retired;           // guarded by g_reapply_mutex
std::atomic<uint32_t> g_reapply_pending{0};
std::atomic<uint64_t> g_reapplies{0}, g_reapply_failures{0};
void DestroySrParams(CoreSlot& core,void* params)
{
    if(!params)return;
    using Fn=int32_t(__cdecl*)(void*);
    if(auto module=core.module.load())if(auto fn=reinterpret_cast<Fn>(GetProcAddress(module,"NVSDK_NGX_D3D12_DestroyParameters")))fn(params);
}
void CollectSrLocked() {
    for(size_t i=0;i<g_retired.size();) {
        auto& r=g_retired[i];
        if(!r.uses.untracked && !r.uses.calls->load() && std::all_of(r.uses.receipts.begin(),r.uses.receipts.end(),
            [](const auto& receipt){return receipt->load(std::memory_order_acquire);}) &&
            r.uses.core->release(const_cast<void*>(r.uses.handle))==1) {
            DestroySrParams(*r.uses.core,r.params);
            g_retired.erase(g_retired.begin()+i);
        } else ++i;
    }
}

// Writes the current preset choice into the feature's parameter object
// (hints, or Default when Automatic). The quality value and sizes are the
// game's and are left untouched.
bool write_preset_only(void* params)
{
    if (!params) return true; // nothing to write (test stubs pass no parameter object)
#if defined(NA_HAVE_NGX_SDK)
    int32_t preset = na::settings::Get().preset.load(std::memory_order_acquire);
    if (preset >= 1 && preset <= 13) {
        auto* p = static_cast<NVSDK_NGX_Parameter*>(params);
        const unsigned int v = static_cast<unsigned int>(preset);
        p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, v);
        p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, v);
        p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, v);
        p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, v);
        p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, v);
        p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality, v);
        return true;
    }
    return StripPresetHints(params);
#else
    (void)params;
    return false;
#endif
}

// A63: the game's evaluate-time parameter object does not always carry the
// creation keys (session 11: every re-creation answered 0xBAD00005, invalid
// parameter, whatever the preset). Build our own object from the game's last
// successful creation (sizes, quality value, create flags) plus the preset.
// Returns the NGX result; on success *params_out is our object (destroyed
// when the feature is released).
NgxResult create_from_snapshot_locked(CoreSlot& s, const EvalParams& c, void* cmd_list, void** out_handle, void** params_out)
{
#if defined(NA_HAVE_NGX_SDK)
    HMODULE m = s.module.load(std::memory_order_acquire);
    if (!c.valid || !m || c.in_width == 0xFFFFFFFFu || c.out_width == 0xFFFFFFFFu || c.perf_quality == 0xFFFFFFFFu) return -1;
    using PfnAlloc = int32_t(__cdecl*)(void**);
    using PfnDestroy = int32_t(__cdecl*)(void*);
    auto alloc = reinterpret_cast<PfnAlloc>(GetProcAddress(m, "NVSDK_NGX_D3D12_AllocateParameters"));
    auto destroy = reinterpret_cast<PfnDestroy>(GetProcAddress(m, "NVSDK_NGX_D3D12_DestroyParameters"));
    if (!alloc || !destroy) return -1;
    void* raw = nullptr;
    if (alloc(&raw) != 1 || !raw) return -1;
    auto* p = static_cast<NVSDK_NGX_Parameter*>(raw);
    p->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    p->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    p->Set(NVSDK_NGX_Parameter_Width, c.in_width);
    p->Set(NVSDK_NGX_Parameter_Height, c.in_height);
    p->Set(NVSDK_NGX_Parameter_OutWidth, c.out_width);
    p->Set(NVSDK_NGX_Parameter_OutHeight, c.out_height);
    p->Set(NVSDK_NGX_Parameter_PerfQualityValue, static_cast<int>(c.perf_quality));
    if (c.create_flags != 0xFFFFFFFFu) p->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, static_cast<int>(c.create_flags));
    write_preset_only(raw);
    NgxResult r = s.create(cmd_list, 1u, raw, out_handle);
    if (r == 1 && out_handle && *out_handle) *params_out = raw;
    else destroy(raw);
    return r;
#else
    (void)s; (void)c; (void)cmd_list; (void)out_handle; (void)params_out;
    return -1;
#endif
}

NgxResult EvaluateFeatureImpl(CoreSlot& s, void* cmd_list, const void* handle, const void* params, void* callback)
{
    if (na::dlss::IsInjectorCall()) {
        g_evaluates.fetch_add(1, std::memory_order_relaxed);
        return s.evaluate(cmd_list, handle, params, callback);
    }
    auto* list = static_cast<ID3D12GraphicsCommandList*>(cmd_list);
    bool native_fg = false;
    {
        std::lock_guard<std::mutex> lock(g_confirm_m);
        for (const auto& entry : g_fg_handles) if (entry.core == &s && entry.handle == handle) { native_fg = true; break; }
    }
    if (native_fg) {
        inspect_fg_inputs(cmd_list, params);
        na::nr::native::InputOverride fg_inputs;
        na::nr::native::Before(list,&s,handle,params,core_runtime(s),&fg_inputs);
        na::nr::trace::Record("fg_begin",nullptr,list);
        const NgxResult result = s.evaluate(cmd_list, handle, params, callback);
        na::nr::trace::Record("fg_end",nullptr,list,0,0,result);
        g_evaluates.fetch_add(1, std::memory_order_relaxed);
        g_evaluates_external.fetch_add(1, std::memory_order_release);
        push_record(CallKind::EvaluateFeature, result, 11u, 0, reinterpret_cast<uint64_t>(handle),
                    reinterpret_cast<uint64_t>(cmd_list));
        // A reused parameter object may still contain SR/RR keys. FG must
        // never publish those stale keys as a new upscaler input snapshot or
        // advance the SR/RR size-confirmation monitor.
        return result;
    }
    const void* game_handle = handle; // A66: identity as the game sees it (creation records use the same)
    // Acquire the recording receipt before any feature can record GPU work.
    // A null-device observation stub has no real command list; never dereference it.
    const auto receipt=s.has_device.load()?na::nr::native::TrackUse(list):SrReceipt{};
    Microsoft::WRL::ComPtr<ID3D12Device> eval_device;
    if(receipt)list->GetDevice(IID_PPV_ARGS(&eval_device));
    std::shared_ptr<std::atomic<uint32_t>> active_call;
    {
        std::lock_guard<std::mutex> lock(g_reapply_mutex);
        CollectSrLocked();
        HandleMap* slot=nullptr;
        for(auto& m:g_handle_maps)if(m.core==&s && m.game.load()==game_handle){slot=&m;handle=m.ours.load();break;}
        auto use=std::find_if(g_sr_uses.begin(),g_sr_uses.end(),[&](const auto& u){return u.core==&s && u.handle==handle;});
        if(use!=g_sr_uses.end()) {
            AddSrUse(*use,receipt);
            if(g_last_sr_core==&s && handle==g_last_sr_handle.load() && g_reapply_pending.exchange(0)) {
                if(!slot)for(auto& m:g_handle_maps)if(!m.game.load()){slot=&m;break;}
                // Reserve capacity BEFORE Create: even a rejected replacement may
                // record commands. Never create then immediately release for lack of a map.
                if(slot && !use->untracked && use->device && use->device==eval_device) {
                    void* replacement=nullptr;void* own_params=nullptr;
                    const auto snapshot=use->create;
                    bool wrote=write_preset_only(const_cast<void*>(params));
                    NgxResult cr=wrote?s.create(cmd_list,1u,const_cast<void*>(params),&replacement):-1;
                    if(cr!=1)cr=create_from_snapshot_locked(s,snapshot,cmd_list,&replacement,&own_params);
                    EvalParams reapply_params{};
                    auto* used=own_params?own_params:const_cast<void*>(params);
                    ReadEvalParams(used,&reapply_params);reapply_params.preset_hint=CreatePresetHint(1u,used);
                    if(cr==1 && replacement) {
                        g_retired.push_back({std::move(*use),slot->params.exchange(own_params)});
                        SrUses fresh;fresh.core=&s;fresh.handle=replacement;fresh.create=snapshot;fresh.device=eval_device;AddSrUse(fresh,receipt);
                        *use=std::move(fresh);
                        slot->core=&s;slot->ours.store(replacement);slot->game.store(game_handle);
                        handle=replacement;g_last_sr_handle.store(handle);
                        g_reapplies.fetch_add(1);
                    } else g_reapply_failures.fetch_add(1);
                    push_record(CallKind::CreateFeature,cr,1u,0,reinterpret_cast<uint64_t>(replacement),reinterpret_cast<uint64_t>(cmd_list),&reapply_params);
                } else g_reapply_failures.fetch_add(1);
            }
            active_call=use->calls;active_call->fetch_add(1);
        }
    }
    na::gputime::Begin(list);
    NgxResult r = s.evaluate(cmd_list, handle, params, callback);
    na::gputime::End(list);
    if(active_call)active_call->fetch_sub(1);
    g_evaluates.fetch_add(1, std::memory_order_relaxed);
    g_evaluates_external.fetch_add(1, std::memory_order_release);
    bool detail = g_detail.load(std::memory_order_relaxed);
    bool capture = na::capture::Pending();
    bool nr = na::nr::WantsInputs();
    bool motion = na::motion::WantsInputs(); // A57: ground-truth comparison for the live flow
    bool fg = na::fg::WantsInputs();         // A59: Frame Generation inputs
    bool watch = g_confirm_active.load(std::memory_order_acquire); // A66: render-size confirmation armed
    if (detail || capture || nr || motion || fg || watch) {
        EvalParams ep{};
        ReadEvalParams(params, &ep);
        if (watch) {
            // A66/A67 (Astra R2/R4): every evaluation reaches the monitor. A
            // failed call or unreadable parameters is an observation of the
            // feature that breaks the streak; it is never skipped.
            confirm::EvalObs obs{};
            obs.handle = reinterpret_cast<uint64_t>(game_handle);
            obs.ok = ep.valid && r >= 0;
            if (obs.ok) {
                uint32_t rw = ep.subrect_width != 0xFFFFFFFFu ? ep.subrect_width : ep.in_width;
                uint32_t rh = ep.subrect_height != 0xFFFFFFFFu ? ep.subrect_height : ep.in_height;
                // Structural coherence of the frame's inputs with that size.
                // Jitter and motion scale must be present (not the 1e38f
                // "unread" sentinel), finite (std::isfinite rejects both
                // infinities and NaN), jitter within one pixel, motion scale
                // non-zero; a NEGATIVE finite scale is a legitimate convention
                // and passes. Each resource must cover base + size for its
                // subrect (an unknown base is NOT coherent); motion vectors at
                // the render size when the feature was created with MVLowRes,
                // else at the output size; the output size must be known and
                // non-zero. These are structural checks only: they do not
                // establish motion-vector units, temporal association, depth
                // interpretation or jitter semantics.
                bool coherent = rw != 0xFFFFFFFFu && rh != 0xFFFFFFFFu && rw && rh &&
                                ep.out_width != 0xFFFFFFFFu && ep.out_height != 0xFFFFFFFFu && ep.out_width && ep.out_height &&
                                confirm::ScalarInputsCoherent(ep.jitter_x, ep.jitter_y, ep.mv_scale_x, ep.mv_scale_y);
                if (coherent) {
                    uint32_t flags = ep.create_flags;
                    if (flags == 0xFFFFFFFFu) { // evaluate objects often lack creation keys: use THIS feature's creation flags (A68)
                        std::lock_guard<std::mutex> lock(g_confirm_m);
                        flags = g_feature_flags.Get(reinterpret_cast<uint64_t>(game_handle));
                    }
                    bool mv_low_res = flags != 0xFFFFFFFFu && (flags & 0x2u) != 0; // NVSDK_NGX_DLSS_Feature_Flags_MVLowRes
                    uint32_t mv_w = mv_low_res ? rw : ep.out_width, mv_h = mv_low_res ? rh : ep.out_height;
                    auto covers = [](uint64_t res, uint32_t bx, uint32_t by, uint32_t w, uint32_t h) {
                        if (!res) return false; // unknown = unverified
                        D3D12_RESOURCE_DESC d = reinterpret_cast<ID3D12Resource*>(res)->GetDesc();
                        return confirm::CoversSubrect(d.Width, d.Height, bx, by, w, h); // A68: 64-bit on both axes
                    };
                    coherent = flags != 0xFFFFFFFFu &&
                               covers(ep.color, ep.color_base_x, ep.color_base_y, rw, rh) &&
                               covers(ep.depth, ep.depth_base_x, ep.depth_base_y, rw, rh) &&
                               covers(ep.motion, ep.mv_base_x, ep.mv_base_y, mv_w, mv_h) &&
                               covers(ep.output, ep.out_base_x, ep.out_base_y, ep.out_width, ep.out_height);
                }
                obs.w = rw;
                obs.h = rh;
                obs.coherent = coherent;
                obs.reset = ep.reset == 1;
            }
            std::lock_guard<std::mutex> lock(g_confirm_m);
            g_confirm.OnEvaluate(obs, GetTickCount64());
            if (!g_confirm.Active()) g_confirm_active.store(false, std::memory_order_release);
        }
        if (fg && ep.valid && r >= 0) {
            na::fg::EvalInputs in{};
            in.engine_frame = na::fg::EvaluationFrame();
            in.camera_valid = na::fg::FindCamera(in.engine_frame, na::fg::EvaluationViewport(), in.camera);
            in.depth = reinterpret_cast<ID3D12Resource*>(ep.depth);
            in.motion = reinterpret_cast<ID3D12Resource*>(ep.motion);
            in.render_w = ep.subrect_width != 0xFFFFFFFFu ? ep.subrect_width : ep.in_width;
            in.render_h = ep.subrect_height != 0xFFFFFFFFu ? ep.subrect_height : ep.in_height;
            in.out_w = ep.out_width;
            in.out_h = ep.out_height;
            in.jitter_x = ep.jitter_x < 1e30f ? ep.jitter_x : 0.f;
            in.jitter_y = ep.jitter_y < 1e30f ? ep.jitter_y : 0.f;
            in.mv_scale_x = ep.mv_scale_x < 1e30f ? ep.mv_scale_x : 1.f;
            in.mv_scale_y = ep.mv_scale_y < 1e30f ? ep.mv_scale_y : 1.f;
            in.reset = ep.reset == 0xFFFFFFFFu ? 0 : ep.reset;
            // DLSS create flags (nvsdk_ngx_defs.h): 1<<3 depth inverted, 1<<2 motion vectors jittered.
            uint32_t flags = ep.create_flags;
            if (flags == 0xFFFFFFFFu) {
                std::lock_guard<std::mutex> lock(g_confirm_m);
                flags = g_feature_flags.Get(reinterpret_cast<uint64_t>(game_handle));
            }
            in.mv_low_res = flags != 0xFFFFFFFFu && (flags & 0x2u);
            in.depth_inverted = flags != 0xFFFFFFFFu && (flags & 0x8u) ? 1u : 0u;
            in.mv_jittered = flags != 0xFFFFFFFFu && (flags & 0x4u) ? 1u : 0u;
            in.depth_base_x = ep.depth_base_x; in.depth_base_y = ep.depth_base_y;
            in.mv_base_x = ep.mv_base_x; in.mv_base_y = ep.mv_base_y;
            na::fg::OnEvaluate(list, in);
        }
        if (nr && ep.valid && r >= 0) {
            na::nr::EvalInputs in{};
            in.depth = reinterpret_cast<ID3D12Resource*>(ep.depth);
            in.motion = reinterpret_cast<ID3D12Resource*>(ep.motion);
            in.render_w = ep.subrect_width != 0xFFFFFFFFu ? ep.subrect_width : ep.in_width;
            in.render_h = ep.subrect_height != 0xFFFFFFFFu ? ep.subrect_height : ep.in_height;
            in.out_w = ep.out_width;
            in.out_h = ep.out_height;
            in.mv_scale_x = ep.mv_scale_x;
            in.mv_scale_y = ep.mv_scale_y;
            in.reset = ep.reset == 0xFFFFFFFFu ? 0 : ep.reset;
            // Argon audit #4.3: depth inversion from the DLSS create flags (1<<3), as for FG.
            uint32_t nflags = ep.create_flags;
            if (nflags == 0xFFFFFFFFu) {
                std::lock_guard<std::mutex> lock(g_confirm_m);
                nflags = g_feature_flags.Get(reinterpret_cast<uint64_t>(game_handle));
            }
            in.depth_inverted = nflags != 0xFFFFFFFFu && (nflags & 0x8u) ? 1u : 0u;
            in.mv_low_res = nflags != 0xFFFFFFFFu && (nflags & 0x2u) ? 1u : 0u;
            in.depth_base_x = ep.depth_base_x; in.depth_base_y = ep.depth_base_y;
            in.mv_base_x = ep.mv_base_x; in.mv_base_y = ep.mv_base_y;
            na::nr::OnEvaluate(list, in);
        }
        if (motion && ep.valid && r >= 0) {
            na::motion::EvalRef ref{};
            ref.depth = reinterpret_cast<ID3D12Resource*>(ep.depth);
            ref.motion = reinterpret_cast<ID3D12Resource*>(ep.motion);
            ref.render_w = ep.subrect_width != 0xFFFFFFFFu ? ep.subrect_width : ep.in_width;
            ref.render_h = ep.subrect_height != 0xFFFFFFFFu ? ep.subrect_height : ep.in_height;
            ref.mv_scale_x = ep.mv_scale_x;
            ref.mv_scale_y = ep.mv_scale_y;
            na::motion::OnEvaluate(list, ref);
        }
        if (detail)
            push_record(CallKind::EvaluateFeature, r, 0, 0,
                        reinterpret_cast<uint64_t>(handle),
                        reinterpret_cast<uint64_t>(cmd_list), &ep);
        if (capture && ep.valid && r >= 0) {
            na::capture::EvalInputs in{};
            in.color = reinterpret_cast<ID3D12Resource*>(ep.color);
            in.depth = reinterpret_cast<ID3D12Resource*>(ep.depth);
            in.motion = reinterpret_cast<ID3D12Resource*>(ep.motion);
            in.output = reinterpret_cast<ID3D12Resource*>(ep.output);
            in.render_w = ep.subrect_width != 0xFFFFFFFFu ? ep.subrect_width : ep.in_width;
            in.render_h = ep.subrect_height != 0xFFFFFFFFu ? ep.subrect_height : ep.in_height;
            in.out_w = ep.out_width;
            in.out_h = ep.out_height;
            in.jitter_x = ep.jitter_x;
            in.jitter_y = ep.jitter_y;
            in.mv_scale_x = ep.mv_scale_x;
            in.mv_scale_y = ep.mv_scale_y;
            in.reset = ep.reset == 0xFFFFFFFFu ? 0 : ep.reset;
            na::capture::OnEvaluate(static_cast<ID3D12GraphicsCommandList*>(cmd_list), in);
        }
    }
    return r;
}

NgxResult ReleaseFeatureImpl(CoreSlot& s, void* handle)
{
    bool native_fg=false;
    {
        std::lock_guard<std::mutex> lock(g_confirm_m);
        for(const auto& entry:g_fg_handles)if(entry.core==&s && entry.handle==handle){native_fg=true;break;}
    }
    if(native_fg){
        const auto result=s.release(handle);
        if(result==1){
            na::nr::native::Release(&s,handle);
            std::lock_guard<std::mutex> lock(g_confirm_m);
            g_feature_flags.Erase(reinterpret_cast<uint64_t>(handle));
            for(auto& entry:g_fg_handles)if(entry.core==&s && entry.handle==handle)entry={};
        }
        g_releases.fetch_add(1,std::memory_order_relaxed);
        push_record(CallKind::ReleaseFeature,result,11u,0,reinterpret_cast<uint64_t>(handle),0);
        return result;
    }
    na::nr::native::Release(&s,handle);
    {
        std::lock_guard<std::mutex> lock(g_confirm_m);
        g_feature_flags.Erase(reinterpret_cast<uint64_t>(handle)); // A68: a released handle value may be reused
        for (auto& entry : g_fg_handles) if (entry.core == &s && entry.handle == handle) entry = {};
    }
    // A mapped handle belongs to us. Retirement follows its actual recordings;
    // the game's original handle is only an external key and may already be retired.
    {
        std::lock_guard<std::mutex> lock(g_reapply_mutex);
        for(auto& m:g_handle_maps)if(m.core==&s && m.game.load()==handle) {
            auto use=std::find_if(g_sr_uses.begin(),g_sr_uses.end(),[&](const auto& u){return u.core==&s && u.handle==m.ours.load();});
            if(use!=g_sr_uses.end()) {
                g_retired.push_back({std::move(*use),m.params.exchange(nullptr)});
                g_sr_uses.erase(use);
            }
            if(g_last_sr_core==&s && g_last_sr_handle.load()==m.ours.load())g_last_sr_handle.store(nullptr);
            m.game.store(nullptr);m.ours.store(nullptr);m.core=nullptr;
            CollectSrLocked();
            g_releases.fetch_add(1);
            push_record(CallKind::ReleaseFeature,1,1u,0,reinterpret_cast<uint64_t>(handle),0);
            return 1; // virtual handle retired; backend errors remain queued for retry
        }
    }
    NgxResult r = s.release(handle);
    if(r==1){
        std::lock_guard<std::mutex> lock(g_reapply_mutex);
        g_sr_uses.erase(std::remove_if(g_sr_uses.begin(),g_sr_uses.end(),[&](const auto& u){return u.core==&s && u.handle==handle;}),g_sr_uses.end());
        if(g_last_sr_core==&s && g_last_sr_handle.load()==handle)g_last_sr_handle.store(nullptr);
        CollectSrLocked();
    }
    g_releases.fetch_add(1, std::memory_order_relaxed);
    push_record(CallKind::ReleaseFeature, r, 0, 0,
                reinterpret_cast<uint64_t>(handle), 0);
    return r;
}

NgxResult Shutdown1Impl(CoreSlot& s, void* device)
{
    na::nr::native::CoreShutdown(&s);
    na::nr::OnRuntimeShutdown(core_runtime(s));
    s.init_succeeded.store(0);
    NgxResult r = s.shutdown1(device);
    if(r==1){
        std::lock_guard<std::mutex> lock(g_reapply_mutex);
        for(auto& m:g_handle_maps)if(m.core==&s){m.game.store(nullptr);m.ours.store(nullptr);m.params.store(nullptr);m.core=nullptr;}
        g_sr_uses.erase(std::remove_if(g_sr_uses.begin(),g_sr_uses.end(),[&](const auto& u){return u.core==&s;}),g_sr_uses.end());
        g_retired.erase(std::remove_if(g_retired.begin(),g_retired.end(),[&](const auto& u){return u.uses.core==&s;}),g_retired.end());
        if(g_last_sr_core==&s){g_last_sr_core=nullptr;g_last_sr_handle.store(nullptr);}
        s.has_device.store(false);
    }
    if (r == 1) {
        std::lock_guard<std::mutex> lock(g_confirm_m);
        for (auto& entry : g_fg_handles) if (entry.core == &s) entry = {};
    }
    g_shutdowns.fetch_add(1, std::memory_order_relaxed);
    push_record(CallKind::Shutdown, r, 0, 0, 0, 0);
    return r;
}

// Per-slot detour entry points (one instantiation per slot index).
template <int S> NgxResult __cdecl DetourInitExt(unsigned long long a, const wchar_t* d, void* dev, uint32_t v, const void* p)
{ return InitExtImpl(g_slots[S], a, d, dev, v, p); }
template <int S> NgxResult __cdecl DetourInitProjectId(const char* id, uint32_t et, const char* ev, const wchar_t* d, void* dev, uint32_t v, const void* p)
{ return InitProjectIdImpl(g_slots[S], id, et, ev, d, dev, v, p); }
template <int S> NgxResult __cdecl DetourCreateFeature(void* cl, uint32_t f, void* p, void** h)
{ return CreateFeatureImpl(g_slots[S], cl, f, p, h); }
template <int S> NgxResult __cdecl DetourEvaluateFeature(void* cl, const void* h, const void* p, void* cb)
{ return EvaluateFeatureImpl(g_slots[S], cl, h, p, cb); }
template <int S> NgxResult __cdecl DetourReleaseFeature(void* h) { return ReleaseFeatureImpl(g_slots[S], h); }
template <int S> NgxResult __cdecl DetourShutdown1(void* dev) { return Shutdown1Impl(g_slots[S], dev); }

struct HookSpec {
    const char* name;
    void* detour;
    void** original;
};

// ---- Module watch via LdrRegisterDllNotification (documented ntdll API).
// The notification callback runs under the loader lock: it only queues and
// signals; a worker thread installs hooks afterwards. ----
typedef struct _NA_UNICODE_STRING { // ntdef UNICODE_STRING layout; not
    USHORT Length;                  // provided under WIN32_LEAN_AND_MEAN
    USHORT MaximumLength;
    PWSTR Buffer;
} NA_UNICODE_STRING;
typedef struct _NA_LDR_DLL_NOTIFICATION_DATA {
    ULONG Flags;
    const NA_UNICODE_STRING* FullDllName;
    const NA_UNICODE_STRING* BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
} NA_LDR_DLL_NOTIFICATION_DATA;
using LdrNotifyFn = VOID(CALLBACK*)(ULONG reason,
                                    const NA_LDR_DLL_NOTIFICATION_DATA* data,
                                    PVOID context);
using LdrRegisterFn = LONG(NTAPI*)(ULONG flags, LdrNotifyFn fn, PVOID context,
                                   PVOID* cookie);
using LdrUnregisterFn = LONG(NTAPI*)(PVOID cookie);
constexpr ULONG kLdrDllLoaded = 1;

std::mutex g_watch_mutex;
std::vector<HMODULE> g_pending;      // guarded by g_watch_mutex
HANDLE g_watch_event = nullptr;
HANDLE g_watch_thread = nullptr;
PVOID g_ldr_cookie = nullptr;
std::atomic<bool> g_watch_stop{false};

VOID CALLBACK LdrNotify(ULONG reason, const NA_LDR_DLL_NOTIFICATION_DATA* data,
                        PVOID)
{
    if (reason != kLdrDllLoaded || !data || !data->BaseDllName ||
        !data->BaseDllName->Buffer)
        return;
    if (!IsNgxCoreModuleName(data->BaseDllName->Buffer,
                             data->BaseDllName->Length / sizeof(wchar_t)))
        return;
    {
        std::lock_guard<std::mutex> lock(g_watch_mutex);
        g_pending.push_back(static_cast<HMODULE>(data->DllBase));
    }
    SetEvent(g_watch_event);
}

DWORD WINAPI WatchThread(LPVOID)
{
    while (!g_watch_stop.load(std::memory_order_acquire)) {
        WaitForSingleObject(g_watch_event, 500);
        std::vector<HMODULE> batch;
        {
            std::lock_guard<std::mutex> lock(g_watch_mutex);
            batch.swap(g_pending);
        }
        for (HMODULE m : batch) InstallForModule(m);
    }
    return 0;
}

// ---- Synchronous nvngx attach (closes the Init race): detour
// LoadLibraryExW; AFTER the original returns (loader lock released for this
// load), install hooks in the same call so the module is observed before the
// caller can invoke any of its exports. The async LdrNotification path stays
// as the catch-all for loads that bypass this entry point. ----
using PfnLoadLibraryExW = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
PfnLoadLibraryExW g_orig_loadlibexw = nullptr;

HMODULE WINAPI DetourLoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags)
{
    HMODULE m = g_orig_loadlibexw(name, file, flags);
    if (m && name && !(flags & (LOAD_LIBRARY_AS_DATAFILE |
                                LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE |
                                LOAD_LIBRARY_AS_IMAGE_RESOURCE))) {
        if (IsNgxCoreModuleName(name, static_cast<size_t>(lstrlenW(name))))
            InstallForModule(m);
    }
    return m;
}

// ---- Logger thread ----
HANDLE g_log_thread = nullptr;
std::atomic<bool> g_log_stop{false};
FILE* g_log_file = nullptr;

DWORD WINAPI LoggerThread(LPVOID)
{
    CallRecord buf[256];
    {
        Stats s = GetStats();
        std::fprintf(g_log_file,
                     "# session start: core_preloaded=%u hooked=%u attached=%u\n",
                     s.core_preloaded, s.hooked_functions, s.module_attached);
        std::fflush(g_log_file);
    }
    while (!g_log_stop.load(std::memory_order_acquire)) {
        Sleep(250);
        uint32_t n = DrainRecords(buf, 256);
        for (uint32_t i = 0; i < n; ++i) {
            const CallRecord& r = buf[i];
            std::fprintf(g_log_file,
                         "qpc=%llu tid=%u kind=%u result=0x%08X feature=%u "
                         "app=0x%llX handle=0x%llX cmdlist=0x%llX",
                         static_cast<unsigned long long>(r.qpc_ticks),
                         r.thread_id, r.kind,
                         static_cast<uint32_t>(r.ngx_result), r.feature_id,
                         static_cast<unsigned long long>(r.app_id),
                         static_cast<unsigned long long>(r.handle),
                         static_cast<unsigned long long>(r.cmdlist));
            if (r.params.valid) {
                std::fprintf(
                    g_log_file,
                    " in=%ux%u out=%ux%u jitter=%.6f,%.6f mvscale=%.4f,%.4f "
                    "reset=%u color=0x%llX depth=0x%llX mv=0x%llX "
                    "outres=0x%llX exposure=0x%llX",
                    r.params.in_width, r.params.in_height, r.params.out_width,
                    r.params.out_height, r.params.jitter_x, r.params.jitter_y,
                    r.params.mv_scale_x, r.params.mv_scale_y, r.params.reset,
                    static_cast<unsigned long long>(r.params.color),
                    static_cast<unsigned long long>(r.params.depth),
                    static_cast<unsigned long long>(r.params.motion),
                    static_cast<unsigned long long>(r.params.output),
                    static_cast<unsigned long long>(r.params.exposure));
                std::fprintf(g_log_file,
                             " subrect=%ux%u pq=%u cflags=0x%X ovr=%u preset=%u",
                             r.params.subrect_width, r.params.subrect_height,
                             r.params.perf_quality, r.params.create_flags,
                             r.params.overridden, r.params.preset_hint);
            }
            std::fputc('\n', g_log_file);
        }
        if (n) std::fflush(g_log_file);
    }
    return 0;
}

} // namespace

void SetDetailLogging(bool enabled) { g_detail.store(enabled, std::memory_order_release); }
// A66 (R1): request-scoped size confirmation.
uint64_t ArmSizeConfirmation(uint32_t w, uint32_t h, uint32_t need, uint32_t create_timeout_ms, uint32_t confirm_timeout_ms,
                             uint32_t overall_timeout_ms)
{
    std::lock_guard<std::mutex> lock(g_confirm_m);
    uint64_t id = g_confirm.Arm(w, h, need, GetTickCount64(), create_timeout_ms, confirm_timeout_ms, overall_timeout_ms);
    g_confirm_active.store(true, std::memory_order_release);
    return id;
}
void MarkSizeConfirmationRestored(uint64_t id)
{
    std::lock_guard<std::mutex> lock(g_confirm_m);
    if (g_confirm.snapshot().id == id) g_confirm.MarkRestored(GetTickCount64());
    if (!g_confirm.Active()) g_confirm_active.store(false, std::memory_order_release);
}
void CancelSizeConfirmation()
{
    std::lock_guard<std::mutex> lock(g_confirm_m);
    g_confirm.Cancel(GetTickCount64());
    g_confirm_active.store(g_confirm.Active(), std::memory_order_release);
}
SizeConfirmation GetSizeConfirmation()
{
    std::lock_guard<std::mutex> lock(g_confirm_m);
    g_confirm.Tick(GetTickCount64()); // deadlines hold even when no evaluate arrives
    if (!g_confirm.Active()) g_confirm_active.store(false, std::memory_order_release);
    const confirm::Snapshot& s = g_confirm.snapshot();
    SizeConfirmation o{};
    o.id = s.id; o.state = s.state; o.reason = s.reason;
    o.req_w = s.req_w; o.req_h = s.req_h; o.need = s.need; o.streak = s.streak;
    o.evals = s.evals; o.matching = s.matching; o.mismatched = s.mismatched; o.incoherent = s.incoherent; o.failed = s.failed;
    o.resets = s.resets; o.unknown_handle = s.unknown_handle; o.other_feature = s.other_feature; o.before_restore = s.before_restore;
    o.creations = s.creations;
    o.created_w = s.created_w; o.created_h = s.created_h; o.last_w = s.last_w; o.last_h = s.last_h;
    o.armed_tick = s.armed_tick; o.restored_tick = s.restored_tick; o.created_tick = s.created_tick; o.done_tick = s.done_tick;
    return o;
}

// A66 (R3): resolve modules at read time (log thread), never in the detour.
uint32_t GetCreateRecords(uint64_t after_seq, CreateRecord* out, uint32_t capacity)
{
    RawCreate raw[kCreateRecords];
    uint32_t n = 0;
    {
        std::lock_guard<std::mutex> lock(g_create_rec_m);
        uint64_t first = g_create_seq > kCreateRecords ? g_create_seq - kCreateRecords + 1 : 1;
        if (after_seq + 1 > first) first = after_seq + 1;
        for (uint64_t s = first; s <= g_create_seq && n < capacity; ++s) raw[n++] = g_create_recs[(s - 1) % kCreateRecords];
    }
    HMODULE exe = GetModuleHandleW(nullptr);
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&GetCreateRecords), &self);
    for (uint32_t i = 0; i < n; ++i) {
        const RawCreate& rc = raw[i];
        CreateRecord& r = out[i];
        r = CreateRecord{};
        r.seq = rc.seq; r.tick = rc.tick; r.thread_id = rc.thread_id; r.feature_id = rc.feature_id; r.result = rc.result;
        r.in_w = rc.in_w; r.in_h = rc.in_h; r.out_w = rc.out_w; r.out_h = rc.out_h; r.confirm_id = rc.confirm_id;
        r.frame_count = resolve_frames(rc.frames, rc.frame_count, r.frames, 24, self, exe);
    }
    return n;
}

// The constants block is the game's; its size is not ours to know. A guarded
// copy of its first 12 bytes (no C++ objects in this function: SEH rule).
static bool copy_raw3(const void* consts, uint32_t* out)
{
    __try {
        memcpy(out, consts, 12);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A71: called by the proxy's typed Streamline wrappers, on the game's thread,
// with the raw return addresses it captured; nothing is resolved here.
void RecordSlCall(uint32_t kind, uint32_t feature, const void* consts, void* const* frames, uint32_t frame_count)
{
    uint32_t raw[3] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    if (consts && !copy_raw3(consts, raw)) raw[0] = raw[1] = raw[2] = 0xFFFFFFFFu;
    std::lock_guard<std::mutex> lock(g_create_rec_m);
    // A73: the game makes this call every frame with the same content (session
    // 13: ~30 identical records per second). A repeat of the previous record
    // (same kind, feature, raw words and first six callers) is folded into it.
    if (g_sl_seq) {
        RawSlCall& last = g_sl_recs[(g_sl_seq - 1) % kSlCallRecords];
        bool same = last.kind == kind && last.feature == feature && last.raw[0] == raw[0] && last.raw[1] == raw[1] && last.raw[2] == raw[2];
        for (uint32_t i = 0; same && i < 6 && i < last.frame_count && i < frame_count; ++i) same = last.frames[i] == frames[i];
        if (same) { ++last.repeats; last.tick = GetTickCount64(); return; }
    }
    RawSlCall& r = g_sl_recs[g_sl_seq % kSlCallRecords];
    r = RawSlCall{};
    r.seq = ++g_sl_seq;
    r.tick = GetTickCount64();
    r.thread_id = GetCurrentThreadId();
    r.kind = kind;
    r.feature = feature;
    r.raw[0] = raw[0]; r.raw[1] = raw[1]; r.raw[2] = raw[2];
    r.frame_count = frame_count > 24 ? 24 : frame_count;
    for (uint32_t i = 0; i < r.frame_count; ++i) r.frames[i] = frames[i];
}

uint32_t GetSlCallRecords(uint64_t after_seq, SlCallRecord* out, uint32_t capacity)
{
    RawSlCall raw[kSlCallRecords];
    uint32_t n = 0;
    {
        std::lock_guard<std::mutex> lock(g_create_rec_m);
        uint64_t first = g_sl_seq > kSlCallRecords ? g_sl_seq - kSlCallRecords + 1 : 1;
        if (after_seq + 1 > first) first = after_seq + 1;
        for (uint64_t s = first; s <= g_sl_seq && n < capacity; ++s) raw[n++] = g_sl_recs[(s - 1) % kSlCallRecords];
    }
    HMODULE exe = GetModuleHandleW(nullptr);
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&GetSlCallRecords), &self);
    for (uint32_t i = 0; i < n; ++i) {
        const RawSlCall& rc = raw[i];
        SlCallRecord& r = out[i];
        r = SlCallRecord{};
        r.seq = rc.seq; r.tick = rc.tick; r.thread_id = rc.thread_id; r.kind = rc.kind; r.feature = rc.feature;
        r.raw[0] = rc.raw[0]; r.raw[1] = rc.raw[1]; r.raw[2] = rc.raw[2];
        r.repeats = rc.repeats;
        r.frame_count = resolve_frames(rc.frames, rc.frame_count, r.frames, 24, self, exe);
    }
    return n;
}

void RequestReapply()
{
    if (g_last_sr_handle.load(std::memory_order_acquire)) g_reapply_pending.store(1, std::memory_order_release);
}

uint32_t CreatePresetHint(uint32_t feature_id, void* ngx_parameter)
{
#if defined(NA_HAVE_NGX_SDK)
    if (!ngx_parameter || (feature_id != 1u && feature_id != kFeatureRayReconstruction)) return 0;
    auto* p = static_cast<NVSDK_NGX_Parameter*>(ngx_parameter);
    int pq = -1;
    p->Get(NVSDK_NGX_Parameter_PerfQualityValue, &pq);
    const bool rr = feature_id == kFeatureRayReconstruction;
    const char* key = nullptr;
    switch (pq) {
    case NVSDK_NGX_PerfQuality_Value_MaxPerf: key = rr ? NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance : NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance; break;
    case NVSDK_NGX_PerfQuality_Value_Balanced: key = rr ? NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced : NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced; break;
    case NVSDK_NGX_PerfQuality_Value_MaxQuality: key = rr ? NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality : NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality; break;
    case NVSDK_NGX_PerfQuality_Value_UltraPerformance: key = rr ? NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance : NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance; break;
    case NVSDK_NGX_PerfQuality_Value_UltraQuality: key = rr ? NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraQuality : NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality; break;
    case NVSDK_NGX_PerfQuality_Value_DLAA: key = rr ? NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA : NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA; break;
    default: return 0;
    }
    unsigned int v = 0;
    return p->Get(key, &v) == NVSDK_NGX_Result_Success ? v : 0u;
#else
    (void)feature_id; (void)ngx_parameter;
    return 0;
#endif
}

bool ApplyCreateOverrides(void* ngx_parameter) { return ApplyCreateOverridesForFeature(1u, ngx_parameter); }

bool ApplyCreateOverridesForFeature(uint32_t feature_id, void* ngx_parameter)
{
#if defined(NA_HAVE_NGX_SDK)
    if (!ngx_parameter) return false;
    if (feature_id != 1u && feature_id != kFeatureRayReconstruction) return false; // FG and others: not ours
    if (!na::settings::Get().enabled.load(std::memory_order_acquire)) return false;
    auto* p = static_cast<NVSDK_NGX_Parameter*>(ngx_parameter);
    bool wrote = false;
    // A20 perf/correctness: tell DLSS which quality mode WE chose, so the
    // runtime picks the model path matching the render size we imposed
    // (e.g. the DLAA path for native, instead of "Balanced at native").
    auto mode = static_cast<na::settings::DlssMode>(
        na::settings::Get().dlss_mode.load(std::memory_order_acquire));
    int pq = NVSDK_NGX_PerfQuality_Value_DLAA;
    switch (mode) {
    case na::settings::DlssMode::Quality: pq = NVSDK_NGX_PerfQuality_Value_MaxQuality; break;
    case na::settings::DlssMode::Balanced: pq = NVSDK_NGX_PerfQuality_Value_Balanced; break;
    case na::settings::DlssMode::Performance: pq = NVSDK_NGX_PerfQuality_Value_MaxPerf; break;
    case na::settings::DlssMode::UltraPerformance: pq = NVSDK_NGX_PerfQuality_Value_UltraPerformance; break;
    default: break;
    }
    // Some engines cache their render scale and never query optimal settings
    // on a resize (PBZ). A preset-only request must preserve both that scale
    // and the game's matching quality value, rather than label low-res DLAA.
    // Only when the game takes its render size from DLSS's optimal settings:
    // otherwise its input stays at the size IT chose, and labelling that input
    // with our mode (e.g. Balanced on an Ultra Performance input) is just wrong.
    unsigned int create_out_w = 0, create_out_h = 0;
    p->Get(NVSDK_NGX_Parameter_OutWidth, &create_out_w);
    p->Get(NVSDK_NGX_Parameter_OutHeight, &create_out_h);
    unsigned int input_w=0,input_h=0;
    p->Get(NVSDK_NGX_Parameter_Width,&input_w);p->Get(NVSDK_NGX_Parameter_Height,&input_h);
    auto expected=[&](unsigned int extent){return mode==na::settings::DlssMode::DLAA?extent:
        (static_cast<unsigned int>(extent*na::settings::ScaleForMode(mode)+0.5f)+7u)&~7u;};
    const bool size_matches=input_w && input_h && create_out_w<=16384 && create_out_h<=16384 &&
        input_w==expected(create_out_w) && input_h==expected(create_out_h);
    if (!na::settings::Get().follow_game_resolution.load(std::memory_order_acquire) &&
        OptimalQueriedFor(create_out_w, create_out_h) && size_matches) {
        p->Set(NVSDK_NGX_Parameter_PerfQualityValue, pq);
        wrote = true;
    }
    if (feature_id == kFeatureRayReconstruction) {
        // Ray Reconstruction reads its own preset keys; only D, E and F are usable
        // (the SDK marks the rest "do not use, reverts to default").
        const int32_t rp = na::settings::Get().rr_preset.load(std::memory_order_acquire);
        if (rp >= 4 && rp <= 6) {
            wrote = true;
            const unsigned int v = static_cast<unsigned int>(rp);
            p->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA, v);
            p->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality, v);
            p->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced, v);
            p->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance, v);
            p->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance, v);
            p->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraQuality, v);
        }
        return wrote;
    }
    int32_t preset = na::settings::Get().preset.load(std::memory_order_acquire);
    if (preset >= 1 && preset <= 13) {
        wrote = true;
        const unsigned int v = static_cast<unsigned int>(preset);
        p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, v);
        p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, v);
        p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, v);
        p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, v);
        p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, v);
        p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality, v);
    }
    return wrote;
#else
    (void)feature_id;
    (void)ngx_parameter;
    return false;
#endif
}

bool StripRrPresetHints(void* ngx_parameter)
{
#if defined(NA_HAVE_NGX_SDK)
    if (!ngx_parameter) return false;
    auto* p = static_cast<NVSDK_NGX_Parameter*>(ngx_parameter);
    const unsigned int def = NVSDK_NGX_RayReconstruction_Hint_Render_Preset_Default;
    p->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA, def);
    p->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality, def);
    p->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced, def);
    p->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance, def);
    p->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance, def);
    p->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraQuality, def);
    return true;
#else
    (void)ngx_parameter;
    return false;
#endif
}

bool StripPresetHints(void* ngx_parameter)
{
#if defined(NA_HAVE_NGX_SDK)
    if (!ngx_parameter) return false;
    auto* p = static_cast<NVSDK_NGX_Parameter*>(ngx_parameter);
    const unsigned int def = NVSDK_NGX_DLSS_Hint_Render_Preset_Default;
    p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, def);
    p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, def);
    p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, def);
    p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, def);
    p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, def);
    p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality, def);
    return true;
#else
    (void)ngx_parameter;
    return false;
#endif
}

bool RestoreQualityValue(void* ngx_parameter, uint32_t perf_quality)
{
#if defined(NA_HAVE_NGX_SDK)
    if (!ngx_parameter) return false;
    static_cast<NVSDK_NGX_Parameter*>(ngx_parameter)->Set(NVSDK_NGX_Parameter_PerfQualityValue, static_cast<int>(perf_quality));
    return true;
#else
    (void)ngx_parameter; (void)perf_quality;
    return false;
#endif
}

// ---- Optimal-settings takeover ----
namespace {
using PfnOptimalCallback = int32_t(__cdecl*)(void* params);
std::atomic<void*> g_orig_optimal{nullptr};
std::atomic<void*> g_orig_optimal_rr{nullptr}; // Ray Reconstruction's DLSSDOptimalSettingsCallback
std::atomic<uint64_t> g_opt_calls{0};
// Every optimal-settings answer, so a log shows what each caller asked
// (display or reference size, quality value), what NVIDIA answered and what we
// returned. UE-style engines may query a reference size (e.g. 1000x1000) per
// quality mode and derive their screen percentage from the answer.
constexpr uint32_t kOptLog = 16;
OptimalCall g_opt_log[kOptLog];
std::atomic<uint64_t> g_opt_log_seq{0};
std::mutex g_opt_log_m;
std::atomic<uint64_t> g_cap_swaps[3]{}; // optimal-callback swaps per path: capability, allocate, get-parameters
// Every display size anyone asked DLSS's optimal-settings callback about (a
// few distinct sizes: the game's real output, probes such as 1000x1000).
// No query observed here does not prove the engine computes its own size;
// another NGX/Streamline acquisition interface may be in use.
constexpr uint32_t kOptSizes = 16;
std::atomic<uint64_t> g_opt_sizes[kOptSizes]{};
void note_opt_size(uint32_t w, uint32_t h)
{
    if (!w || !h) return;
    const uint64_t v = (uint64_t(w) << 32) | h;
    for (auto& e : g_opt_sizes) {
        uint64_t cur = e.load(std::memory_order_acquire);
        if (cur == v) return;
        if (!cur && e.compare_exchange_strong(cur, v, std::memory_order_acq_rel)) return;
        if (cur == v) return;
    }
}
std::atomic<uint32_t> g_opt_user_w{0}, g_opt_user_h{0}, g_opt_out_w{0},
    g_opt_out_h{0}, g_opt_overridden{0};

inline uint32_t align8(uint32_t v) { return (v + 7u) & ~7u; }
} // namespace

void SetOriginalOptimalCallbackForTest(void* original_callback)
{
    g_orig_optimal.store(original_callback, std::memory_order_release);
}
void SetOriginalOptimalCallbackRRForTest(void* original_callback)
{
    g_orig_optimal_rr.store(original_callback, std::memory_order_release);
}

namespace {
int32_t optimal_settings_common(void* ngx_parameter, void* orig, bool rr);
}
int32_t OptimalSettingsCallback(void* ngx_parameter)
{
    return optimal_settings_common(ngx_parameter, g_orig_optimal.load(std::memory_order_acquire), false);
}
int32_t OptimalSettingsCallbackRR(void* ngx_parameter)
{
    return optimal_settings_common(ngx_parameter, g_orig_optimal_rr.load(std::memory_order_acquire), true);
}

namespace {
// Bind each thunk permanently to its original. A second runtime query must
// not retarget callbacks already handed out. Never recycle retained pointers.
constexpr size_t kOptimalBindings = 32;
std::array<std::atomic<void*>, kOptimalBindings> g_opt_originals{};
std::array<bool, kOptimalBindings> g_opt_rr{};
std::mutex g_opt_bind_mutex;
template<size_t I> int32_t __cdecl BoundOptimal(void* p)
{
    void* original=g_opt_originals[I].load(std::memory_order_acquire);
    return optimal_settings_common(p, original, g_opt_rr[I]);
}
template<size_t... I> auto optimal_thunks(std::index_sequence<I...>)
{
    return std::array<PfnOptimalCallback, sizeof...(I)>{&BoundOptimal<I>...};
}
const auto g_opt_thunks = optimal_thunks(std::make_index_sequence<kOptimalBindings>{});
void* bind_optimal(void* original, bool rr)
{
    for (auto thunk : g_opt_thunks)
        if (original == reinterpret_cast<void*>(thunk)) return original;
    std::lock_guard<std::mutex> lock(g_opt_bind_mutex);
    for (size_t i = 0; i < kOptimalBindings; ++i) {
        void* stored = g_opt_originals[i].load(std::memory_order_acquire);
        if (stored == original && g_opt_rr[i] == rr) return reinterpret_cast<void*>(g_opt_thunks[i]);
        if (!stored) {
            g_opt_rr[i]=rr;
            g_opt_originals[i].store(original, std::memory_order_release);
            return reinterpret_cast<void*>(g_opt_thunks[i]);
        }
    }
    return original; // full: retain the runtime answer, never misroute it
}
int32_t optimal_settings_common(void* ngx_parameter, void* orig, bool rr)
{
    int32_t r = 1; // NVSDK_NGX_Result_Success when no original (tests)
    if (orig) r = reinterpret_cast<PfnOptimalCallback>(orig)(ngx_parameter);
    g_opt_calls.fetch_add(1, std::memory_order_relaxed);
#if defined(NA_HAVE_NGX_SDK)
    if (!ngx_parameter || r < 0) return r;
    auto* p = static_cast<NVSDK_NGX_Parameter*>(ngx_parameter);
    unsigned int user_w = 0, user_h = 0, out_w = 0, out_h = 0;
    p->Get(NVSDK_NGX_Parameter_Width, &user_w);
    p->Get(NVSDK_NGX_Parameter_Height, &user_h);
    p->Get(NVSDK_NGX_Parameter_OutWidth, &out_w);
    p->Get(NVSDK_NGX_Parameter_OutHeight, &out_h);
    g_opt_user_w.store(user_w); g_opt_user_h.store(user_h);
    note_opt_size(user_w, user_h);
    int pq = -1;
    p->Get(NVSDK_NGX_Parameter_PerfQualityValue, &pq);
    const unsigned int native_w = out_w, native_h = out_h; // NVIDIA's own answer, before ours

    bool enabled = na::settings::Get().enabled.load(std::memory_order_acquire) != 0;
    auto mode = static_cast<na::settings::DlssMode>(
        na::settings::Get().dlss_mode.load(std::memory_order_acquire));
    bool overridden = false;
    if (enabled && !na::settings::Get().follow_game_resolution.load(std::memory_order_acquire) && user_w && user_h) {
        unsigned int tw = user_w, th = user_h;
        if (mode != na::settings::DlssMode::DLAA) {
            float ratio = na::settings::ScaleForMode(mode);
            tw = align8(static_cast<unsigned int>(user_w * ratio + 0.5f));
            th = align8(static_cast<unsigned int>(user_h * ratio + 0.5f));
            if (tw > user_w) tw = user_w;
            if (th > user_h) th = user_h;
        }
        if (tw != out_w || th != out_h) {
            p->Set(NVSDK_NGX_Parameter_OutWidth, tw);
            p->Set(NVSDK_NGX_Parameter_OutHeight, th);
            p->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width, tw);
            p->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height, th);
            p->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width, tw);
            p->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height, th);
            out_w = tw; out_h = th;
            overridden = true;
        }
    }
    g_opt_out_w.store(out_w); g_opt_out_h.store(out_h);
    g_opt_overridden.store(overridden ? 1u : 0u);
    {
        std::lock_guard<std::mutex> lock(g_opt_log_m);
        const uint64_t seq = g_opt_log_seq.load(std::memory_order_relaxed) + 1;
        OptimalCall& c = g_opt_log[seq % kOptLog];
        c = {};
        c.sequence = seq; c.user_w = user_w; c.user_h = user_h; c.perf_quality = static_cast<int32_t>(pq);
        c.native_w = native_w; c.native_h = native_h; c.out_w = out_w; c.out_h = out_h;
        c.overridden = overridden ? 1u : 0u; c.rr = rr ? 1u : 0u;
        g_opt_log_seq.store(seq, std::memory_order_release);
    }
#else
    (void)ngx_parameter;
    (void)rr;
#endif
    return r;
}
} // namespace

uint32_t GetOptimalCalls(uint64_t after, OptimalCall* out, uint32_t max)
{
    std::lock_guard<std::mutex> lock(g_opt_log_m);
    const uint64_t last = g_opt_log_seq.load(std::memory_order_acquire);
    uint64_t first = after + 1;
    if (last >= kOptLog && first < last - kOptLog + 1) first = last - kOptLog + 1;
    uint32_t n = 0;
    for (uint64_t q = first; q <= last && n < max; ++q) {
        const OptimalCall& c = g_opt_log[q % kOptLog];
        if (c.sequence == q) out[n++] = c;
    }
    return n;
}

bool OptimalQueriedFor(uint32_t out_w, uint32_t out_h)
{
    if (!out_w || !out_h || out_w == 0xFFFFFFFFu || out_h == 0xFFFFFFFFu) return false;
    for (const auto& e : g_opt_sizes) {
        const uint64_t v = e.load(std::memory_order_acquire);
        if (!v) break;
        const uint32_t w = uint32_t(v >> 32), h = uint32_t(v);
        // The generic rebuild nudges the window by one pixel; count that as the same display.
        if (w + 1 >= out_w && w <= out_w + 1 && h + 1 >= out_h && h <= out_h + 1) return true;
    }
    return false;
}

OptimalStats GetOptimalStats()
{
    OptimalStats s{};
    s.calls = g_opt_calls.load();
    s.last_user_w = g_opt_user_w.load(); s.last_user_h = g_opt_user_h.load();
    s.last_out_w = g_opt_out_w.load(); s.last_out_h = g_opt_out_h.load();
    s.last_overridden = g_opt_overridden.load();
    s.swaps_capability = g_cap_swaps[0].load(); s.swaps_allocate = g_cap_swaps[1].load(); s.swaps_getparams = g_cap_swaps[2].load();
    return s;
}

namespace {
// After the runtime hands out capability parameters, swap the optimal-
// settings callback pointer inside them for ours (idempotent: never
// re-capture our own pointer as the "original").
void swap_optimal_callbacks(void* params, int path);
int32_t GetCapabilityParametersImpl(CoreSlot& s, void** out_params)
{
    int32_t r = s.getcap(out_params);
    if (r == 1 && out_params && *out_params) swap_optimal_callbacks(*out_params, 0);
    return r;
}
// Allocations may be empty; only wrap callbacks already supplied by the runtime.
int32_t AllocateParametersImpl(CoreSlot& s, void** out_params)
{
    int32_t r = s.allocate(out_params);
    if (r == 1 && out_params && *out_params) swap_optimal_callbacks(*out_params, 1);
    return r;
}
int32_t GetParametersImpl(CoreSlot& s, void** out_params)
{
    int32_t r = s.getparams(out_params);
    if (r == 1 && out_params && *out_params) swap_optimal_callbacks(*out_params, 2);
    return r;
}
void swap_optimal_callbacks(void* params, int path)
{
#if defined(NA_HAVE_NGX_SDK)
    {
        auto* p = static_cast<NVSDK_NGX_Parameter*>(params);
        bool swapped = false;
        for (const char* key : {NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback,
                                NVSDK_NGX_Parameter_DLSSDOptimalSettingsCallback}) {
            void* cb = nullptr;
            if (p->Get(key, &cb) != NVSDK_NGX_Result_Success || !cb) continue;
            void* replacement = bind_optimal(cb, std::strcmp(key,NVSDK_NGX_Parameter_DLSSDOptimalSettingsCallback)==0);
            if (replacement == cb) continue;
            p->Set(key, replacement);
            swapped = true;
        }
        if (swapped && path >= 0 && path < 3) g_cap_swaps[path].fetch_add(1, std::memory_order_relaxed);
    }
#else
    (void)params; (void)path;
#endif
}
template <int S> int32_t __cdecl DetourGetCapabilityParameters(void** out) { return GetCapabilityParametersImpl(g_slots[S], out); }
template <int S> int32_t __cdecl DetourAllocateParameters(void** p) { return AllocateParametersImpl(g_slots[S], p); }
template <int S> int32_t __cdecl DetourGetParameters(void** out) { return GetParametersImpl(g_slots[S], out); }

// Base file name of a module name or full path.
const wchar_t* base_name(const wchar_t* name, size_t len, size_t* out_len)
{
    size_t start = 0;
    for (size_t i = 0; i < len; ++i)
        if (name[i] == L'\\' || name[i] == L'/') start = i + 1;
    *out_len = len - start;
    return name + start;
}
bool equals_nocase(const wchar_t* a, size_t len, const wchar_t* lit)
{
    size_t n = static_cast<size_t>(lstrlenW(lit));
    if (n != len) return false;
    for (size_t i = 0; i < n; ++i)
        if (towlower(a[i]) != towlower(lit[i])) return false;
    return true;
}
} // namespace

bool IsNgxCoreModuleName(const wchar_t* name, size_t len)
{
    if (!name) return false;
    size_t blen = 0;
    const wchar_t* b = base_name(name, len, &blen);
    // The driver's cores, plus the test stub. Feature DLLs (nvngx_dlss.dll,
    // nvngx_dlssnr.dll, nvngx_dlssg.dll, ...) export the same names but are
    // NOT entry points the game calls; hooking them mis-routes calls (A43).
    return equals_nocase(b, blen, L"_nvngx.dll") || equals_nocase(b, blen, L"nvngx.dll") ||
           equals_nocase(b, blen, L"nvngx_stub.dll");
}

bool ReadEvalParams(const void* ngx_parameter, EvalParams* out)
{
    if (!out) return false;
    std::memset(out, 0, sizeof(*out));
    out->reset = 0xFFFFFFFFu;
    out->in_width = out->in_height = 0xFFFFFFFFu;
    out->out_width = out->out_height = 0xFFFFFFFFu;
    out->jitter_x = out->jitter_y = 1e38f;
    out->mv_scale_x = out->mv_scale_y = 1e38f;
    out->subrect_width = out->subrect_height = 0xFFFFFFFFu;
    out->perf_quality = 0xFFFFFFFFu;
    out->create_flags = 0xFFFFFFFFu;
    out->color_base_x = out->color_base_y = out->depth_base_x = out->depth_base_y = 0xFFFFFFFFu;
    out->mv_base_x = out->mv_base_y = out->out_base_x = out->out_base_y = 0xFFFFFFFFu;
#if defined(NA_HAVE_NGX_SDK)
    if (!ngx_parameter) return false;
    auto* p = static_cast<const NVSDK_NGX_Parameter*>(ngx_parameter);
    unsigned int u = 0;
    float f = 0;
    void* v = nullptr;
    if (p->Get(NVSDK_NGX_Parameter_Width, &u) == NVSDK_NGX_Result_Success)
        out->in_width = u;
    if (p->Get(NVSDK_NGX_Parameter_Height, &u) == NVSDK_NGX_Result_Success)
        out->in_height = u;
    if (p->Get(NVSDK_NGX_Parameter_OutWidth, &u) == NVSDK_NGX_Result_Success)
        out->out_width = u;
    if (p->Get(NVSDK_NGX_Parameter_OutHeight, &u) == NVSDK_NGX_Result_Success)
        out->out_height = u;
    if (p->Get(NVSDK_NGX_Parameter_Reset, &u) == NVSDK_NGX_Result_Success)
        out->reset = u;
    if (p->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &f) == NVSDK_NGX_Result_Success)
        out->jitter_x = f;
    if (p->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &f) == NVSDK_NGX_Result_Success)
        out->jitter_y = f;
    if (p->Get(NVSDK_NGX_Parameter_MV_Scale_X, &f) == NVSDK_NGX_Result_Success)
        out->mv_scale_x = f;
    if (p->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &f) == NVSDK_NGX_Result_Success)
        out->mv_scale_y = f;
    if (p->Get(NVSDK_NGX_Parameter_Color, &v) == NVSDK_NGX_Result_Success)
        out->color = reinterpret_cast<uint64_t>(v);
    if (p->Get(NVSDK_NGX_Parameter_Depth, &v) == NVSDK_NGX_Result_Success)
        out->depth = reinterpret_cast<uint64_t>(v);
    if (p->Get(NVSDK_NGX_Parameter_MotionVectors, &v) == NVSDK_NGX_Result_Success)
        out->motion = reinterpret_cast<uint64_t>(v);
    if (p->Get(NVSDK_NGX_Parameter_Output, &v) == NVSDK_NGX_Result_Success)
        out->output = reinterpret_cast<uint64_t>(v);
    if (p->Get(NVSDK_NGX_Parameter_ExposureTexture, &v) == NVSDK_NGX_Result_Success)
        out->exposure = reinterpret_cast<uint64_t>(v);
    if (p->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &u) ==
        NVSDK_NGX_Result_Success)
        out->subrect_width = u;
    if (p->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &u) ==
        NVSDK_NGX_Result_Success)
        out->subrect_height = u;
    if (p->Get(NVSDK_NGX_Parameter_PerfQualityValue, &u) == NVSDK_NGX_Result_Success)
        out->perf_quality = u;
    if (p->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &u) ==
        NVSDK_NGX_Result_Success)
        out->create_flags = u;
    // A67: subrect origins (coverage is base + size, not size alone).
    if (p->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, &u) == NVSDK_NGX_Result_Success) out->color_base_x = u;
    if (p->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, &u) == NVSDK_NGX_Result_Success) out->color_base_y = u;
    if (p->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, &u) == NVSDK_NGX_Result_Success) out->depth_base_x = u;
    if (p->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, &u) == NVSDK_NGX_Result_Success) out->depth_base_y = u;
    if (p->Get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, &u) == NVSDK_NGX_Result_Success) out->mv_base_x = u;
    if (p->Get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, &u) == NVSDK_NGX_Result_Success) out->mv_base_y = u;
    if (p->Get(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, &u) == NVSDK_NGX_Result_Success) out->out_base_x = u;
    if (p->Get(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, &u) == NVSDK_NGX_Result_Success) out->out_base_y = u;
    out->valid = 1;
    return true;
#else
    (void)ngx_parameter;
    return false;
#endif
}

namespace {
std::mutex g_install_mutex;

template <int S> uint32_t install_slot(HMODULE ngx_module)
{
    CoreSlot& slot = g_slots[S];
    const HookSpec specs[] = {
        {"NVSDK_NGX_D3D12_Init_Ext", reinterpret_cast<void*>(&DetourInitExt<S>), reinterpret_cast<void**>(&slot.init_ext)},
        {"NVSDK_NGX_D3D12_Init_ProjectID", reinterpret_cast<void*>(&DetourInitProjectId<S>), reinterpret_cast<void**>(&slot.init_pid)},
        {"NVSDK_NGX_D3D12_CreateFeature", reinterpret_cast<void*>(&DetourCreateFeature<S>), reinterpret_cast<void**>(&slot.create)},
        {"NVSDK_NGX_D3D12_EvaluateFeature", reinterpret_cast<void*>(&DetourEvaluateFeature<S>), reinterpret_cast<void**>(&slot.evaluate)},
        {"NVSDK_NGX_D3D12_ReleaseFeature", reinterpret_cast<void*>(&DetourReleaseFeature<S>), reinterpret_cast<void**>(&slot.release)},
        {"NVSDK_NGX_D3D12_Shutdown1", reinterpret_cast<void*>(&DetourShutdown1<S>), reinterpret_cast<void**>(&slot.shutdown1)},
        {"NVSDK_NGX_D3D12_GetCapabilityParameters", reinterpret_cast<void*>(&DetourGetCapabilityParameters<S>), reinterpret_cast<void**>(&slot.getcap)},
        {"NVSDK_NGX_D3D12_AllocateParameters", reinterpret_cast<void*>(&DetourAllocateParameters<S>), reinterpret_cast<void**>(&slot.allocate)},
        {"NVSDK_NGX_D3D12_GetParameters", reinterpret_cast<void*>(&DetourGetParameters<S>), reinterpret_cast<void**>(&slot.getparams)},
    };
    // A45 installation transaction (answers A43-NGX-ACK finding 2): every
    // original is prepared and published, then the slot's module record is
    // published, and only then are the detours enabled together. A detour
    // therefore never runs against a half-filled slot, and a failed commit
    // leaves nothing patched.
    if (!na::hooking::BeginInstall()) return 0;
    uint32_t hooked = 0;
    for (const HookSpec& s : specs) {
        void* target = reinterpret_cast<void*>(GetProcAddress(ngx_module, s.name));
        if (!target) continue;
        if (na::hooking::PrepareHook(target, s.detour, s.original)) ++hooked;
    }
    if (!hooked) {
        na::hooking::AbandonInstall();
        return 0;
    }
    slot.hooked.store(hooked, std::memory_order_relaxed);
    slot.module.store(ngx_module, std::memory_order_release); // published before enable
    if (!na::hooking::CommitInstall()) {
        slot.module.store(nullptr, std::memory_order_release);
        slot.hooked.store(0, std::memory_order_relaxed);
        return 0;
    }
    return hooked;
}
} // namespace

uint32_t InstallForModule(HMODULE ngx_module)
{
    if (!ngx_module) return 0;
    std::lock_guard<std::mutex> lock(g_install_mutex);
    // A43: only NGX CORE modules are hooked, verified by the module's own file
    // name (the notification/known-name paths pre-filter, this is the guard).
    wchar_t path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(ngx_module, path, MAX_PATH);
    if (!n || !IsNgxCoreModuleName(path, n)) return 0;
    // Idempotent per module: a module already in a slot is not re-counted.
    int free_slot = -1;
    for (int i = 0; i < kMaxSlots; ++i) {
        HMODULE m = g_slots[i].module.load(std::memory_order_acquire);
        if (m == ngx_module) return g_slots[i].hooked.load(std::memory_order_relaxed);
        if (!m && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return 0; // both driver cores already hooked; refuse a third
    uint32_t hooked = free_slot == 0 ? install_slot<0>(ngx_module) : install_slot<1>(ngx_module);
    if (hooked) { // install_slot published the slot's module/count before enabling (A45)
        g_hooked.fetch_add(hooked, std::memory_order_relaxed);
        g_hooked_module.store(ngx_module, std::memory_order_release);
        g_module_attached.store(1, std::memory_order_release);
    }
    return hooked;
}

bool GetRuntimeEntryPoints(void** create, void** evaluate, void** release, void** allocate_params,
                           void** destroy_params, void** get_capability_params)
{
    // A43: the core the game INITIALISED is the one whose entry points the NR
    // pass must use (a feature created on the other core would have no
    // initialised state). Fall back to the first hooked slot before any Init.
    CoreSlot* s = nullptr;
    for (int i = 0; i < kMaxSlots; ++i)
        if (g_slots[i].module.load(std::memory_order_acquire) && g_slots[i].init_succeeded.load(std::memory_order_acquire)) { s = &g_slots[i]; break; }
    if (!s)
        for (int i = 0; i < kMaxSlots; ++i)
            if (g_slots[i].module.load(std::memory_order_acquire)) { s = &g_slots[i]; break; }
    if (!s || !s->create || !s->evaluate || !s->release) return false;
    HMODULE m = s->module.load(std::memory_order_acquire);
    if (create) *create = reinterpret_cast<void*>(s->create);
    if (evaluate) *evaluate = reinterpret_cast<void*>(s->evaluate);
    if (release) *release = reinterpret_cast<void*>(s->release);
    if (allocate_params) *allocate_params = reinterpret_cast<void*>(GetProcAddress(m, "NVSDK_NGX_D3D12_AllocateParameters"));
    if (destroy_params) *destroy_params = reinterpret_cast<void*>(GetProcAddress(m, "NVSDK_NGX_D3D12_DestroyParameters"));
    if (get_capability_params) *get_capability_params = reinterpret_cast<void*>(s->getcap);
    return true;
}

namespace {
std::atomic<uint32_t> g_core_preloaded{0};
std::mutex g_preload_mutex;
}

bool PreloadAndHookNgxCore()
{
    std::lock_guard<std::mutex> lock(g_preload_mutex);
    if (g_core_preloaded.load(std::memory_order_acquire)) return true;
    wchar_t dir[MAX_PATH] = {};
    DWORD size = sizeof(dir);
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", 0,
                      KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return false;
    LONG rc = RegQueryValueExW(key, L"FullPath", nullptr, nullptr,
                               reinterpret_cast<LPBYTE>(dir), &size);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || !dir[0]) return false;
    wchar_t path[MAX_PATH];
    wsprintfW(path, L"%s\\_nvngx.dll", dir);
    HMODULE core = LoadLibraryW(path); // reference intentionally kept
    if (!core) return false;
    uint32_t hooked = InstallForModule(core);
    if (hooked) g_core_preloaded.store(1, std::memory_order_release);
    return hooked != 0;
}

Stats GetStats()
{
    Stats s{};
    s.core_preloaded = g_core_preloaded.load(std::memory_order_acquire);
    s.rr_seen = g_rr_seen.load(std::memory_order_acquire);
    s.active_upscaler = g_active_upscaler.load(std::memory_order_acquire);
    s.fg_seen = g_fg_seen.load(std::memory_order_acquire);
    s.fg_evaluates = g_fg_evaluates.load(); s.fg_with_inputs = g_fg_with_inputs.load();
    s.fg_with_hudless = g_fg_with_hudless.load(); s.fg_with_distortion = g_fg_with_distortion.load();
    s.fg_list_type = g_fg_list_type.load(); s.fg_color_format = g_fg_color_format.load();
    s.fg_color_w = g_fg_color_w.load(); s.fg_color_h = g_fg_color_h.load();
    s.fg_with_frame_id = g_fg_with_frame_id.load(); s.fg_with_alpha = g_fg_with_alpha.load();
    s.fg_with_ui = g_fg_with_ui.load(); s.fg_last_create_uir = g_fg_last_create_uir.load();
    s.inits = g_inits.load(std::memory_order_relaxed);
    s.creates = g_creates.load(std::memory_order_relaxed);
    s.evaluates = g_evaluates.load(std::memory_order_relaxed);
    s.releases = g_releases.load(std::memory_order_relaxed);
    s.shutdowns = g_shutdowns.load(std::memory_order_relaxed);
    s.records_dropped = g_dropped.load(std::memory_order_relaxed);
    s.hooked_functions = g_hooked.load(std::memory_order_relaxed);
    s.module_attached = g_module_attached.load(std::memory_order_acquire);
    s.sr_creates = g_sr_creates.load(std::memory_order_acquire);
    s.sr_creates_unknown = g_sr_creates_unknown.load(std::memory_order_acquire);
    s.sr_creates_injector = g_sr_creates_injector.load(std::memory_order_acquire);
    s.evaluates_external = g_evaluates_external.load(std::memory_order_acquire);
    s.create_failures = g_create_failures.load(std::memory_order_relaxed);
    s.create_recovered = g_create_recovered.load(std::memory_order_relaxed);
    s.preset_rejected = g_preset_rejected.load(std::memory_order_acquire);
    s.quality_rejected = g_quality_rejected.load(std::memory_order_acquire);
    s.reapplies = g_reapplies.load(std::memory_order_relaxed);
    s.reapply_failures = g_reapply_failures.load(std::memory_order_relaxed);
    s.reapply_pending = g_reapply_pending.load(std::memory_order_acquire);
    s.create_records = g_create_records.load(std::memory_order_acquire);
    s.last_sr_in_w = g_last_sr_in_w.load(std::memory_order_relaxed);
    s.last_sr_in_h = g_last_sr_in_h.load(std::memory_order_relaxed);
    s.last_sr_out_w = g_last_sr_out_w.load(std::memory_order_relaxed);
    s.last_sr_out_h = g_last_sr_out_h.load(std::memory_order_relaxed);
    return s;
}

uint32_t DrainRecords(CallRecord* out, uint32_t capacity)
{
    uint64_t committed = g_ring_commit.load(std::memory_order_acquire);
    uint32_t count = 0;
    while (g_ring_read < committed && count < capacity) {
        out[count++] = g_ring[g_ring_read & (kRingSize - 1)];
        ++g_ring_read;
    }
    return count;
}

bool StartModuleWatch(bool sync_loader_hook)
{
    if (g_watch_thread) return true;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return false;
    auto reg = reinterpret_cast<LdrRegisterFn>(reinterpret_cast<void*>(
        GetProcAddress(ntdll, "LdrRegisterDllNotification")));
    if (!reg) return false;
    g_watch_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_watch_event) return false;
    g_watch_stop.store(false, std::memory_order_release);
    if (reg(0, &LdrNotify, nullptr, &g_ldr_cookie) != 0) {
        CloseHandle(g_watch_event);
        g_watch_event = nullptr;
        return false;
    }
    // Catch modules loaded before the watch started.
    for (const wchar_t* known : {L"_nvngx.dll", L"nvngx.dll", L"nvngx_stub.dll"}) { // cores only (A43)
        if (HMODULE m = GetModuleHandleW(known)) {
            std::lock_guard<std::mutex> lock(g_watch_mutex);
            g_pending.push_back(m);
        }
    }
    SetEvent(g_watch_event);
    g_watch_thread = CreateThread(nullptr, 0, &WatchThread, nullptr, 0, nullptr);
    if (!g_watch_thread) {
        StopModuleWatch();
        return false;
    }
    // Synchronous fast path (Init race): hook LoadLibraryExW so nvngx modules
    // are hooked before the loader call returns to the game. OPT-IN ONLY â€”
    // correlated with early-startup crashes in TLOU (WORKLOG A12). Failure is
    // not fatal; the async notification path still covers attachment.
    if (sync_loader_hook) {
        // Prefer kernelbase's function body: kernel32's export forwards there
        // and LoadLibraryW funnels into the same body.
        HMODULE loader_mod = GetModuleHandleW(L"kernelbase.dll");
        if (!loader_mod) loader_mod = GetModuleHandleW(L"kernel32.dll");
        if (loader_mod) {
            void* target = reinterpret_cast<void*>(
                GetProcAddress(loader_mod, "LoadLibraryExW"));
            if (target)
                na::hooking::HookFunction(
                    target, reinterpret_cast<void*>(&DetourLoadLibraryExW),
                    reinterpret_cast<void**>(&g_orig_loadlibexw));
        }
    }
    return true;
}

void StopModuleWatch()
{
    if (g_ldr_cookie) {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll) {
            auto unreg = reinterpret_cast<LdrUnregisterFn>(
                reinterpret_cast<void*>(
                    GetProcAddress(ntdll, "LdrUnregisterDllNotification")));
            if (unreg) unreg(g_ldr_cookie);
        }
        g_ldr_cookie = nullptr;
    }
    if (g_watch_thread) {
        g_watch_stop.store(true, std::memory_order_release);
        SetEvent(g_watch_event);
        WaitForSingleObject(g_watch_thread, 5000);
        CloseHandle(g_watch_thread);
        g_watch_thread = nullptr;
    }
    if (g_watch_event) {
        CloseHandle(g_watch_event);
        g_watch_event = nullptr;
    }
}

bool StartLogger(const wchar_t* log_path)
{
    if (g_log_thread || !log_path) return false;
    if (_wfopen_s(&g_log_file, log_path, L"ab") != 0 || !g_log_file)
        return false;
    g_log_stop.store(false, std::memory_order_release);
    g_log_thread = CreateThread(nullptr, 0, &LoggerThread, nullptr, 0, nullptr);
    if (!g_log_thread) {
        std::fclose(g_log_file);
        g_log_file = nullptr;
        return false;
    }
    return true;
}

void StopLogger()
{
    if (!g_log_thread) return;
    g_log_stop.store(true, std::memory_order_release);
    WaitForSingleObject(g_log_thread, 5000);
    CloseHandle(g_log_thread);
    g_log_thread = nullptr;
    if (g_log_file) {
        std::fclose(g_log_file);
        g_log_file = nullptr;
    }
}

} // namespace na::ngx
