#include "host_dx12/host_boot/process_log.hpp"
#include "host_dx12/sl_boundary/sl_boundary.hpp"

#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl/client.h>
#include "host_dx12/overlay/present_queues.hpp"

#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "host_dx12/hooking/vtable_hook.hpp"
#include "host_dx12/settings.hpp"
#include "host_dx12/adapters/mode_control.hpp"
#include "host_dx12/sl_boundary/sl_dlss_mode.hpp"
#include "host_dx12/sl_boundary/sl_tags.hpp"
#include "host_dx12/sl_proxy/sl_abi.hpp"
#include "host_dx12/upscalers/module_watch.hpp"

// See sl_boundary.hpp for the contract. Implementation notes:
//  - Hooks are installed only from the module-watch worker or a deferred
//    install thread, never inside a detour or under the loader lock.
//  - Detours read game memory through SEH-guarded copies; a layout that does
//    not validate (GUIDs) is counted as undecoded and the original runs as is.
//  - Every detour forwards to the original exactly once with the game's own
//    arguments; this module never changes what the game passes.

namespace na::slb {
namespace {

// IDXGISwapChain vtable slots (single inheritance chain up to IDXGISwapChain4).
constexpr uint32_t kSlotRelease = 2;
constexpr uint32_t kSlotPresent = 8;
constexpr uint32_t kSlotResizeBuffers = 13;
constexpr uint32_t kSlotPresent1 = 22;
constexpr uint32_t kSlotResizeBuffers1 = 39;
constexpr uint32_t kProxyVtableMinSlots = 40;
// IDXGIFactory / IDXGIFactory2 creation slots (same numbers SL hooks).
constexpr uint32_t kSlotCreateSwapChain = 10;
constexpr uint32_t kSlotCreateSwapChainForHwnd = 15;
constexpr uint32_t kSlotCreateSwapChainForCoreWindow = 16;
constexpr uint32_t kSlotCreateSwapChainForComposition = 24;

// sl::interposer::DXGISwapChain  DECLSPEC_UUID("D3F0BBFF-3091-4074-9D9E-B99CE2E5CF9A")
constexpr GUID kSlSwapChainIid = {0xd3f0bbff, 0x3091, 0x4074, {0x9d, 0x9e, 0xb9, 0x9c, 0xe2, 0xe5, 0xcf, 0x9a}};
// sl::interposer::DXGIFactory  DECLSPEC_UUID("AABDF0C6-6A76-4F65-987D-F2CC4C27ED0E")
constexpr GUID kSlFactoryIid = {0xaabdf0c6, 0x6a76, 0x4f65, {0x98, 0x7d, 0xf2, 0xcc, 0x4c, 0x27, 0xed, 0x0e}};
// StreamlineRetrieveBaseInterface  DECLSPEC_UUID("ADEC44E2-61F0-45C3-AD9F-1B37379284FF") (sl.api/internal.h)
constexpr GUID kSlRetrieveBaseIid = {0xadec44e2, 0x61f0, 0x45c3, {0xad, 0x9f, 0x1b, 0x37, 0x37, 0x92, 0x84, 0xff}};

const char* const kProxyRttiNames[] = {".?AUDXGISwapChain@interposer@sl@@", ".?AVDXGISwapChain@interposer@sl@@"};

using SlResult = int32_t;
using PfnSetTagForFrame = SlResult(__cdecl*)(const void* frame, const void* viewport, const void* tags, uint32_t n, void* cmd);
using PfnSetTag = SlResult(__cdecl*)(const void* viewport, const void* tags, uint32_t n, void* cmd);
using PfnUpgradeInterface = SlResult(__cdecl*)(void** base);
using PfnSetConstants = SlResult(__cdecl*)(const void* constants, const void* frame, const void* viewport);
using PfnRelease = ULONG(STDMETHODCALLTYPE*)(IUnknown*);
using PfnCreateFactory = HRESULT(WINAPI*)(REFIID, void**);
using PfnCreateFactory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
using PfnPresent = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using PfnPresent1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using PfnResizeBuffers = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using PfnResizeBuffers1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);
using PfnFactoryCreate = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using PfnFactoryCreateHwnd = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                         const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using PfnFactoryCreateCore = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*,
                                                         IDXGIOutput*, IDXGISwapChain1**);
using PfnFactoryCreateComp = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*,
                                                         IDXGISwapChain1**);

using PfnGetFeatureFunction = SlResult(__cdecl*)(uint32_t feature, const char* name, void** function);
using PfnDlssgSetOptions = SlResult(__cdecl*)(const void* viewport, const void* options);
using PfnDlssgGetState = SlResult(__cdecl*)(const void* viewport, void* state, const void* options);
constexpr uint32_t kFeatureDlssG = 1000; // sl::kFeatureDLSS_G
// sl::DLSSGOptions {FAC5F1CB-2DFD-4F36-A1E6-3A9E865256C5}, sl::DLSSGState {CC8AC8E1-A179-44F5-97FA-E74112F9BC61}
constexpr uint8_t kDlssgOptionsGuid[16] = {0xcb, 0xf1, 0xc5, 0xfa, 0xfd, 0x2d, 0x36, 0x4f, 0xa1, 0xe6, 0x3a, 0x9e, 0x86, 0x52, 0x56, 0xc5};
constexpr uint8_t kDlssgStateGuid[16] = {0xe1, 0xc8, 0x8a, 0xcc, 0x79, 0xa1, 0xf5, 0x44, 0x97, 0xfa, 0xe7, 0x41, 0x12, 0xf9, 0xbc, 0x61};
constexpr size_t kDlssgOptionsCopy = 128; // >= sizeof(sl::DLSSGOptions) v5 (120)
std::recursive_mutex g_fg_api_m; // NVIDIA documents GetState/SetOptions as not thread safe
constexpr uint32_t kSlModeOff = 0, kSlModeOn = 1, kSlModeDynamic = 3;

PfnGetFeatureFunction g_orig_gff = nullptr;
void* g_gff_export = nullptr;
PfnDlssgSetOptions g_orig_fg_set = nullptr;
PfnDlssgGetState g_orig_fg_get = nullptr;
std::atomic<bool> g_fg_set_requested{false}, g_fg_get_requested{false}, g_fg_lookup_done{false};
std::atomic<uint32_t> g_fg_set_hooked{0}, g_fg_get_hooked{0};
std::atomic<uint64_t> g_fg_set_calls{0}, g_fg_get_calls{0};
std::atomic<uint64_t> g_fg_request{0}; // (mode << 32) | frames
std::mutex g_fg_m;
FgState g_fg; // guarded by g_fg_m (counters/hook flags are filled in GetFgState)

void lookup_fg_functions_once(); // defined with the DLSS-G control below

// The hooking core allows one install transaction per process at a time, and
// other modules (NGX, upscalers) install from their own threads: wait briefly.
std::mutex g_install_m, g_attach_m;
std::atomic<bool> g_retry_needed{false},g_fail_acquire{false};
bool g_exports_done=false,g_fallback_done=false,g_hooking_initialized=false;
std::atomic<bool> g_attach_terminal{false};
HMODULE g_attach_module=nullptr;
uint8_t g_export_state[4]{},g_fallback_state[4]{}; // 0 unfinished,1 attached,2 unavailable/terminal target
std::atomic<bool> g_fail_proxy_commit{false};
void prepare_optional(void* target,void* detour,void** original,uint8_t& state,std::vector<uint8_t*>& batch){
    if(state)return;
    if(!target || !na::hooking::PrepareHook(target,detour,original)){state=2;return;}
    batch.push_back(&state);
}
void commit_optional(std::vector<uint8_t*>& batch){
    const uint8_t outcome=na::hooking::CommitInstall()?1u:2u;
    for(auto* state:batch)*state=outcome; // failed/dead optional targets never retried
}
bool begin_install_retry()
{
    const bool ok=!g_fail_acquire.exchange(false) && na::hooking::BeginInstallWait("Streamline boundary");
    if(!ok)g_retry_needed.store(true,std::memory_order_release);
    return ok;
}

using PfnNewFrame=SlResult(__cdecl*)(void**,const uint32_t*);
PfnNewFrame g_new_frame=nullptr;
PfnSetTagForFrame g_orig_tag_frame = nullptr;
PfnSetTag g_orig_tag = nullptr;
PfnUpgradeInterface g_orig_upgrade = nullptr;
PfnSetConstants g_orig_constants = nullptr;
PfnRelease g_orig_release = nullptr;
PfnCreateFactory g_orig_cf = nullptr, g_orig_cf1 = nullptr;
PfnCreateFactory2 g_orig_cf2 = nullptr;
PfnPresent g_orig_present = nullptr;
PfnPresent1 g_orig_present1 = nullptr;
PfnResizeBuffers g_orig_resize = nullptr;
PfnResizeBuffers1 g_orig_resize1 = nullptr;
PfnFactoryCreate g_orig_fc = nullptr;
PfnFactoryCreateHwnd g_orig_fc_hwnd = nullptr;
PfnFactoryCreateCore g_orig_fc_core = nullptr;
PfnFactoryCreateComp g_orig_fc_comp = nullptr;

// ------------------------------------------------------- callback subscription
// One retained subscription per call. Subscriptions are never freed (a few
// bytes per SetCallbacks); what SetCallbacks guarantees is that no callback
// into an old set starts after it returns and none is still running.
struct Subscription {
    Callbacks cb;
    std::atomic<int32_t> inflight{0};
};
std::atomic<Subscription*> g_sub{nullptr};
std::mutex g_sub_m;                 // serializes SetCallbacks
thread_local int t_callback_depth = 0;

Subscription* acquire_sub()
{
    for (;;) {
        Subscription* s = g_sub.load(std::memory_order_seq_cst);
        if (!s) return nullptr;
        s->inflight.fetch_add(1, std::memory_order_seq_cst);
        if (g_sub.load(std::memory_order_seq_cst) == s) return s;
        s->inflight.fetch_sub(1, std::memory_order_seq_cst); // replaced meanwhile: never call into it
    }
}
void release_sub(Subscription* s)
{
    if (s) s->inflight.fetch_sub(1, std::memory_order_seq_cst);
}
struct CallbackScope { CallbackScope() { ++t_callback_depth; } ~CallbackScope() { --t_callback_depth; } };

std::atomic<uintptr_t> g_module_lo{0}, g_module_hi{0};

std::atomic<uint32_t> g_module_seen{0}, g_tag_hooks{0}, g_present_hooks{0}, g_discovery{0}, g_proxies{0};
std::atomic<uint64_t> g_tag_calls{0}, g_tag_undecoded{0}, g_tags_seen{0}, g_tag_seq{0};
std::atomic<uint64_t> g_real{0}, g_retry{0}, g_test{0};
std::atomic<uint32_t> g_tags_inflight{0};
std::atomic<uint64_t> g_const_calls{0}, g_const_decoded{0}, g_const_valid{0}, g_const_seq{0};
std::atomic<uint64_t> g_assoc_unique{0}, g_assoc_ambiguous{0}, g_assoc_none{0};
std::atomic<uint64_t> g_present_events{0}; // real presents across all proxies (association provenance)
uint64_t g_last_present_tag_seq = 0;        // tag sequence at the newest real Present of any proxy (g_m)
std::atomic<bool> g_proxy_install_requested{false}, g_factory_install_requested{false};

std::mutex g_m; // proxies, tag window, constants ring, last-tag summary, reason

// Per-proxy stream state (keyed by the proxy object; removed when SL releases it).
struct ProxyState {
    IDXGISwapChain* proxy = nullptr;
    IDXGISwapChain* base = nullptr;
    uint32_t id = 0;
    uint64_t epoch = 0, serial = 0;
    bool retry_pending = false;
    PresentEvent retry_frame{}; // identity only; borrowed call arguments are never replayed
    uint64_t last_tag_seq = 0;       // tag sequence at this proxy's previous real Present
    uint64_t last_present_event = 0; // g_present_events value after its previous real Present
};
constexpr uint32_t kMaxProxies = 8;
ProxyState g_proxy_state[kMaxProxies];
uint32_t g_next_proxy_id = 0;
struct GameQueueBinding { IDXGISwapChain* proxy=nullptr;na::overlay::PresentQueues queues; };
GameQueueBinding g_game_queues[kMaxProxies];
Microsoft::WRL::ComPtr<ID3D12CommandQueue> canonical_queue(IUnknown* object){
    Microsoft::WRL::ComPtr<IUnknown> base;
    if(object && SUCCEEDED(object->QueryInterface(kSlRetrieveBaseIid,&base)))object=base.Get();
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    if(object)object->QueryInterface(IID_PPV_ARGS(&queue));return queue;
}
void notify_chain_seen(IDXGISwapChain* chain);
void bind_game_queue(IDXGISwapChain* chain,IUnknown* object){
    auto queue=canonical_queue(object);if(!chain || !queue)return;
    {std::lock_guard<std::mutex> lock(g_m);
        GameQueueBinding* entry=nullptr;
        for(auto& binding:g_game_queues)if(binding.proxy==chain){entry=&binding;break;}
        if(!entry)for(auto& binding:g_game_queues)if(!binding.proxy){entry=&binding;break;}
        if(entry){entry->proxy=chain;entry->queues.Created(queue.Get());}}
    notify_chain_seen(chain);
}


ProxyState* find_proxy_locked(IDXGISwapChain* p)
{
    for (ProxyState& st : g_proxy_state)
        if (st.proxy == p) return &st;
    return nullptr;
}

// Accepted tag calls (original returned 0), newest last, for association.
struct TagWindowEntry { uint64_t sequence; bool has_viewport, has_frame; uint32_t viewport, frame_token; };
constexpr uint32_t kTagWindow = 64;
TagWindowEntry g_tag_window[kTagWindow] = {};
uint32_t g_tag_window_count = 0; // total pushed (ring index = count % kTagWindow)

// Accepted constants per (frame token, viewport), newest overwrite oldest.
constexpr uint32_t kConstRing = 16;
ConstantsRecord g_const_ring[kConstRing];
uint32_t g_const_count = 0;

uint32_t g_hud_life = 0xFFFFFFFFu, g_hud_state = 0xFFFFFFFFu, g_depth_life = 0xFFFFFFFFu, g_motion_life = 0xFFFFFFFFu;
uint32_t g_bb_tagged = 0, g_ui_tagged = 0, g_ui_alpha_tagged = 0;
char g_reason[96] = "Streamline not loaded.";
// Distinct tag shapes seen (evidence for sl_boundary.log): which buffers the
// game tags, with which lifecycle/state/format/size, on which call.
struct TagShape { uint32_t type, lifecycle, state, format, width, height; bool cmd, for_frame; uint64_t count; };
TagShape g_shapes[32] = {};
uint32_t g_shape_count = 0;

void note_shape_locked(const Tag& d, bool cmd, bool for_frame)
{
    for (uint32_t i = 0; i < g_shape_count; ++i) {
        TagShape& t = g_shapes[i];
        if (t.type == d.buffer_type && t.lifecycle == d.lifecycle && t.state == d.state && t.format == d.native_format &&
            t.width == d.width && t.height == d.height && t.cmd == cmd && t.for_frame == for_frame) {
            ++t.count;
            return;
        }
    }
    if (g_shape_count < 32) g_shapes[g_shape_count++] = {d.buffer_type, d.lifecycle, d.state, d.native_format, d.width, d.height, cmd, for_frame, 1};
}

void set_reason(const char* text)
{
    std::lock_guard<std::mutex> lock(g_m);
    strncpy_s(g_reason, text, _TRUNCATE);
}

// ---------------------------------------------------------------- guarded reads
size_t guarded_copy(void* dst, const void* src, size_t n)
{
    __try {
        memcpy(dst, src, n);
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool frame_token_value(const void* token, uint32_t* out)
{
    uint8_t head[8 + na::sl::kHeaderSize] = {};
    if (!token || guarded_copy(head, token, sizeof(head)) != sizeof(head)) return false;
    if (!na::sl::HasType(head + 8, na::sl::kHeaderSize, na::sl::kFrameTokenGuid)) return false;
    __try {
        using Op = uint32_t(__cdecl*)(const void*);
        *out = (*reinterpret_cast<Op const* const*>(token))[0](token);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool viewport_value(const void* viewport, uint32_t* out)
{
    uint8_t vh[na::sl::kHeaderSize + 4] = {};
    const size_t got = viewport ? guarded_copy(vh, viewport, sizeof(vh)) : 0;
    return na::sl::ViewportValue(vh, got, out);
}

bool in_module(const void* p)
{
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    return a >= g_module_lo.load(std::memory_order_acquire) && a < g_module_hi.load(std::memory_order_acquire);
}

// ------------------------------------------------------------------- tag calls
void decode_tags(bool for_frame, const void* frame, const void* viewport, const void* tags, uint32_t n, void* cmd,
                 TagEvent& e, Tag* decoded)
{
    g_tag_calls.fetch_add(1, std::memory_order_relaxed);
    e.sequence = g_tag_seq.fetch_add(1, std::memory_order_acq_rel) + 1;
    e.for_frame = for_frame;
    e.has_frame = for_frame && frame_token_value(frame, &e.frame_token);
    e.has_viewport = viewport_value(viewport, &e.viewport);
    e.command_list = static_cast<ID3D12GraphicsCommandList*>(cmd);
    e.thread_id = GetCurrentThreadId();
    e.raw_count = n;
    e.tags = decoded;
    bool ok = n == 0 || tags != nullptr; // zero tags: nothing to decode, still a valid call
    const uint32_t count = n < kMaxTagsPerCall ? n : kMaxTagsPerCall;
    for (uint32_t i = 0; ok && i < count; ++i) {
        uint8_t raw[abi::kTagSize];
        const uint8_t* src = static_cast<const uint8_t*>(tags) + size_t(i) * abi::kTagSize;
        if (guarded_copy(raw, src, sizeof(raw)) != sizeof(raw)) { ok = false; break; }
        uint8_t res[abi::kResourceReadSize];
        size_t res_len = 0;
        if (const uint64_t rp = abi::TagResourcePointer(raw))
            res_len = guarded_copy(res, reinterpret_cast<const void*>(rp), sizeof(res));
        abi::RawTag t;
        if (!abi::DecodeTag(raw, sizeof(raw), res_len ? res : nullptr, res_len, t)) { ok = false; break; }
        Tag& d = decoded[i];
        d.buffer_type = t.buffer_type;
        d.lifecycle = t.lifecycle;
        d.resource = reinterpret_cast<ID3D12Resource*>(t.resource);
        d.resource_type = t.resource_type;
        d.state = t.state;
        d.width = t.width;
        d.height = t.height;
        d.native_format = t.native_format;
        d.extent_top = t.extent_top;
        d.extent_left = t.extent_left;
        d.extent_width = t.extent_width;
        d.extent_height = t.extent_height;
        d.resource_decoded = t.resource_decoded;
    }
    e.decoded = ok;
    e.count = ok ? count : 0;
    if (!ok) g_tag_undecoded.fetch_add(1, std::memory_order_relaxed);
}

// After the original: an accepted call joins the association window and the summary.
void note_accepted_tags(const TagEvent& e, int32_t result)
{
    std::lock_guard<std::mutex> lock(g_m);
    g_tags_inflight.fetch_sub(1);
    if (result != 0) return; // rejected by Streamline: never associated
    // An accepted call we could not decode still joins the window, as unknown
    // (it makes the association ambiguous instead of silently missing).
    g_tag_window[g_tag_window_count % kTagWindow] = {e.sequence, e.decoded && e.has_viewport, e.decoded && e.has_frame, e.viewport, e.frame_token};
    ++g_tag_window_count;
    if (!e.decoded) return;
    g_tags_seen.fetch_add(e.count, std::memory_order_relaxed);
    for (uint32_t i = 0; i < e.count; ++i) {
        const Tag& d = e.tags[i];
        const bool present = d.resource != nullptr;
        if (present) note_shape_locked(d, e.command_list != nullptr, e.for_frame);
        switch (d.buffer_type) {
        case abi::kBufferHudlessColor:
            if (present) { g_hud_life = d.lifecycle; g_hud_state = d.state; }
            break;
        case abi::kBufferDepth: if (present) g_depth_life = d.lifecycle; break;
        case abi::kBufferMotionVectors: if (present) g_motion_life = d.lifecycle; break;
        case abi::kBufferBackbuffer: g_bb_tagged = present ? 1u : 0u; break;
        case abi::kBufferUiColorAndAlpha: g_ui_tagged = present ? 1u : 0u; break;
        case abi::kBufferUiAlpha: g_ui_alpha_tagged = present ? 1u : 0u; break;
        default: break;
        }
    }
}

template <class Forward>
SlResult tag_call(bool for_frame, const void* frame, const void* viewport, const void* tags, uint32_t n, void* cmd, Forward forward)
{
    g_tags_inflight.fetch_add(1);
    Tag decoded[kMaxTagsPerCall];
    TagEvent e;
    decode_tags(for_frame, frame, viewport, tags, n, cmd, e, decoded);
    Subscription* sub = acquire_sub();
    if (sub && sub->cb.on_tag) { CallbackScope scope; sub->cb.on_tag(e, sub->cb.user); }
    const SlResult r = forward();
    note_accepted_tags(e, r);
    if (sub && sub->cb.after_tag) { CallbackScope scope; sub->cb.after_tag(e, r, sub->cb.user); }
    release_sub(sub);
    return r;
}

SlResult __cdecl DetourSetTagForFrame(const void* frame, const void* viewport, const void* tags, uint32_t n, void* cmd)
{
    return tag_call(true, frame, viewport, tags, n, cmd, [&] { return g_orig_tag_frame(frame, viewport, tags, n, cmd); });
}

SlResult __cdecl DetourSetTag(const void* viewport, const void* tags, uint32_t n, void* cmd)
{
    return tag_call(false, nullptr, viewport, tags, n, cmd, [&] { return g_orig_tag(viewport, tags, n, cmd); });
}

// --------------------------------------------------------------- constants
SlResult __cdecl DetourSetConstants(const void* constants, const void* frame, const void* viewport)
{
    g_const_calls.fetch_add(1, std::memory_order_relaxed);
    Subscription* sub = acquire_sub();
    const SlResult r = g_orig_constants(constants, frame, viewport);
    ConstantsRecord rec;
    rec.result = r;
    rec.thread_id = GetCurrentThreadId();
    uint8_t buf[na::sl::kV2ConstantsSize] = {};
    const size_t got = constants ? guarded_copy(buf, constants, sizeof(buf)) : 0;
    const bool decoded = got == sizeof(buf) && na::sl::HasType(buf, got, na::sl::kConstantsGuid) &&
                         na::sl::DecodeConstants(buf, got, rec.camera) == na::sl::Layout::V2 &&
                         frame_token_value(frame, &rec.frame_token) && viewport_value(viewport, &rec.viewport);
    if (decoded) {
        g_const_decoded.fetch_add(1, std::memory_order_relaxed);
        rec.sequence = g_const_seq.fetch_add(1, std::memory_order_acq_rel) + 1;
        rec.camera_valid = na::fg::ValidCamera(rec.camera);
        if (rec.camera_valid) g_const_valid.fetch_add(1, std::memory_order_relaxed);
        if (r == 0) {
            std::lock_guard<std::mutex> lock(g_m);
            g_const_ring[g_const_count % kConstRing] = rec;
            ++g_const_count;
        }
        if (sub && sub->cb.on_constants) { CallbackScope scope; sub->cb.on_constants(rec, sub->cb.user); }
    }
    release_sub(sub);
    return r;
}

// ------------------------------------------------------------- proxy presents
// Per-proxy state for a Present (registers a newly seen proxy: epoch 1).
void present_state(IDXGISwapChain* sc, PresentEvent& e)
{
    IDXGISwapChain* base = nullptr;
    // SL can replace its underlying chain during an FG/display transition
    // without changing the game-facing proxy address. Never suppress/draw
    // against a cached old base identity. QI is outside our state mutex.
    void* raw = nullptr;
    if(SUCCEEDED(sc->QueryInterface(kSlRetrieveBaseIid,&raw)) && raw){
        base=static_cast<IDXGISwapChain*>(raw);
        base->Release(); // borrowed: the proxy retains it through its Present
    }
    std::lock_guard<std::mutex> lock(g_m);
    ProxyState* st = find_proxy_locked(sc);
    if (!st) {
        st = find_proxy_locked(nullptr);
        if (!st) st = &g_proxy_state[0]; // more than kMaxProxies live chains: reuse (reported as a new stream)
        *st = ProxyState{};
        st->proxy = sc;
        st->base = base;
        st->id = ++g_next_proxy_id;
        st->epoch = 1;
        // A new chain's first window starts after the newest real Present of any
        // chain (from the start if none), so its first frame's tags count.
        st->last_tag_seq = g_last_present_tag_seq;
        st->last_present_event = g_present_events.load(std::memory_order_acquire);
        g_proxies.fetch_add(1, std::memory_order_relaxed);
    }
    if(st->base!=base){
        st->base=base;++st->epoch;st->retry_pending=false;st->retry_frame={};
    }
    e.proxy_id = st->id;
    e.base = st->base;
    e.stream_epoch = st->epoch;
    if (st->retry_pending) {
        e.kind = PresentKind::Retry;
        e.real_serial = st->serial;
        e.tags_after_sequence=st->retry_frame.tags_after_sequence;
        e.last_tag_sequence=st->retry_frame.last_tag_sequence;
        e.association=st->retry_frame.association;
        e.viewport=st->retry_frame.viewport;
        e.frame_token=st->retry_frame.frame_token;
        g_retry.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    e.kind = PresentKind::Real;
    e.real_serial = ++st->serial;
    g_real.fetch_add(1, std::memory_order_relaxed);
    // Association: accepted tag calls since this proxy's previous real Present.
    const uint64_t newest = g_tag_seq.load(std::memory_order_acquire);
    e.tags_after_sequence = st->last_tag_seq;
    e.last_tag_sequence = newest;
    const uint64_t events_now = g_present_events.fetch_add(1, std::memory_order_acq_rel);
    const bool other_presented = events_now != st->last_present_event;
    st->last_present_event = events_now + 1;
    st->last_tag_seq = newest;
    g_last_present_tag_seq = newest;
    uint32_t matched = 0;
    const bool pending_tags=g_tags_inflight.load()!=0;
    bool consistent = !pending_tags;
    const uint32_t available = g_tag_window_count < kTagWindow ? g_tag_window_count : kTagWindow;
    for (uint32_t i = 0; i < available; ++i) {
        const TagWindowEntry& t = g_tag_window[(g_tag_window_count - 1 - i) % kTagWindow];
        // Calls can finish out of issue order. An older completion must not
        // hide a newer accepted call in the rest of the ring.
        if (t.sequence <= e.tags_after_sequence || t.sequence > newest) continue;
        if (!t.has_viewport || !t.has_frame) { consistent = false; ++matched; continue; }
        if (matched == 0) { e.viewport = t.viewport; e.frame_token = t.frame_token; }
        else if (t.viewport != e.viewport || t.frame_token != e.frame_token) consistent = false;
        ++matched;
    }
    if (newest - e.tags_after_sequence > kTagWindow) consistent = false; // more calls than the window retains
    if (matched == 0 && !pending_tags) {
        e.association = Association::None;
        g_assoc_none.fetch_add(1, std::memory_order_relaxed);
    } else if (consistent && !other_presented) {
        e.association = Association::Unique;
        g_assoc_unique.fetch_add(1, std::memory_order_relaxed);
    } else {
        e.association = Association::Ambiguous;
        e.viewport = e.frame_token = 0;
        g_assoc_ambiguous.fetch_add(1, std::memory_order_relaxed);
    }
    st->retry_frame=e;
    st->retry_frame.params=nullptr;
}

void note_present_result(IDXGISwapChain* sc, HRESULT hr)
{
    // DXGI_PRESENT_DO_NOT_WAIT + WAS_STILL_DRAWING: nothing was queued; this
    // chain presents the same frame again, which must not advance its serial.
    std::lock_guard<std::mutex> lock(g_m);
    if (ProxyState* st = find_proxy_locked(sc)) st->retry_pending = hr == DXGI_ERROR_WAS_STILL_DRAWING;
}

std::atomic<const OverlayHooks*> g_overlay_hooks{nullptr};

void notify_chain_seen(IDXGISwapChain* chain){
    Microsoft::WRL::ComPtr<IUnknown> base;
    if(chain && SUCCEEDED(chain->QueryInterface(kSlRetrieveBaseIid,&base)))
        if(const auto* hook=g_overlay_hooks.load(std::memory_order_acquire);hook && hook->chain_seen)
            hook->chain_seen(chain,reinterpret_cast<IDXGISwapChain*>(base.Get()));
}

template <class Forward>
HRESULT present_call(IDXGISwapChain* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp, bool present1, Forward forward)
{
    if (flags & DXGI_PRESENT_TEST) {
        g_test.fetch_add(1, std::memory_order_relaxed);
        return forward();
    }
    PresentEvent e;
    e.present1 = present1;
    e.game_facing = sc;
    e.sync = sync;
    e.flags = flags;
    e.params = pp;
    e.thread_id = GetCurrentThreadId();
    present_state(sc, e);
    Microsoft::WRL::ComPtr<IDXGISwapChain3> indexed;
    const UINT buffer=SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&indexed)))?indexed->GetCurrentBackBufferIndex():0;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    {std::lock_guard<std::mutex> lock(g_m);for(const auto& binding:g_game_queues)if(binding.proxy==sc)queue=binding.queues.At(buffer);}
    e.present_queue=queue.Get(); // kept alive across BOTH callbacks/original Present
    if (e.kind == PresentKind::Real && g_real.load(std::memory_order_relaxed) == 120) lookup_fg_functions_once();
    Subscription* sub = acquire_sub(); // one subscription for before AND after
    if (sub && sub->cb.before_present) { CallbackScope scope; sub->cb.before_present(e, sub->cb.user); }
    if (e.kind == PresentKind::Real)
        if (const OverlayHooks* overlay = g_overlay_hooks.load(std::memory_order_acquire); overlay && overlay->present)
            overlay->present(sc, e.base, queue.Get());
    const HRESULT hr = forward();
    note_present_result(sc, hr);
    if (sub && sub->cb.after_present) { CallbackScope scope; sub->cb.after_present(e, hr, sub->cb.user); }
    release_sub(sub);
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    return present_call(sc, sync, flags, nullptr, false, [&] { return g_orig_present(sc, sync, flags); });
}

HRESULT STDMETHODCALLTYPE DetourPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp)
{
    return present_call(sc, sync, flags, pp, true, [&] { return g_orig_present1(sc, sync, flags, pp); });
}

void bump_epoch(IDXGISwapChain* sc)
{
    std::lock_guard<std::mutex> lock(g_m);
    if (ProxyState* st = find_proxy_locked(sc)) {
        ++st->epoch;
        st->retry_pending = false;
        st->retry_frame={};
        st->last_tag_seq=g_tag_seq.load();
        st->last_present_event=g_present_events.load();
    }
}

void overlay_before_resize(IDXGISwapChain* sc)
{
    if (const OverlayHooks* overlay = g_overlay_hooks.load(std::memory_order_acquire); overlay && overlay->before_resize)
        overlay->before_resize(sc);
}

HRESULT STDMETHODCALLTYPE DetourResizeBuffers(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags)
{
    overlay_before_resize(sc);
    const HRESULT hr = g_orig_resize(sc, n, w, h, f, flags);
    if (SUCCEEDED(hr)) bump_epoch(sc);
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourResizeBuffers1(IDXGISwapChain3* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags,
                                               const UINT* masks, IUnknown* const* queues)
{
    std::vector<Microsoft::WRL::ComPtr<ID3D12CommandQueue>> owned;
    std::vector<IUnknown*> canonical;
    UINT queue_count=n;if(!queue_count){DXGI_SWAP_CHAIN_DESC1 desc{};if(SUCCEEDED(sc->GetDesc1(&desc)))queue_count=desc.BufferCount;}
    if(queues && queue_count){for(UINT i=0;i<queue_count;++i){owned.push_back(canonical_queue(queues[i]));canonical.push_back(owned.back().Get());}}
    overlay_before_resize(sc);
    const HRESULT hr = g_orig_resize1(sc, n, w, h, f, flags, masks, queues);
    if(SUCCEEDED(hr) && !canonical.empty()){
        std::lock_guard<std::mutex> lock(g_m);
        GameQueueBinding* entry=nullptr;
        for(auto& binding:g_game_queues)if(binding.proxy==sc){entry=&binding;break;}
        if(!entry)for(auto& binding:g_game_queues)if(!binding.proxy){entry=&binding;break;}
        if(entry && entry->queues.Resized(hr,queue_count,canonical.data()))entry->proxy=sc;
    }
    if (SUCCEEDED(hr)) bump_epoch(sc);
    return hr;
}

// A released proxy ends its stream: a later object at the same address is new.
ULONG STDMETHODCALLTYPE DetourRelease(IUnknown* obj)
{
    const ULONG r = g_orig_release(obj);
    if (r == 0) {
        uint32_t id=0;
        {std::lock_guard<std::mutex> lock(g_m);
        if (ProxyState* st = find_proxy_locked(reinterpret_cast<IDXGISwapChain*>(obj))) {id=st->id;*st = ProxyState{};}
        for(auto& b:g_game_queues)if(b.proxy==obj)b={};}
        Subscription* sub=acquire_sub();
        if(id && sub && sub->cb.stream_released){CallbackScope scope;sub->cb.stream_released(id,sub->cb.user);}
        release_sub(sub);
        if (const OverlayHooks* overlay = g_overlay_hooks.load(std::memory_order_acquire); overlay && overlay->released)
            overlay->released(reinterpret_cast<IDXGISwapChain*>(obj));
    }
    return r;
}

// Installs the four proxy hooks from the proxy class's vtable. Worker threads only.
bool install_proxy_vtable(void* const* vtable, Discovery how)
{
    if (!vtable) return false;
    std::lock_guard<std::mutex> install_lock(g_install_m);
    if(g_present_hooks.load(std::memory_order_acquire))return true;
    if(g_attach_terminal.load())return false;
    if (!begin_install_retry()) return false;
    bool ok = na::hooking::PrepareHook(vtable[kSlotPresent], reinterpret_cast<void*>(&DetourPresent),
                                       reinterpret_cast<void**>(&g_orig_present)) &&
              na::hooking::PrepareHook(vtable[kSlotPresent1], reinterpret_cast<void*>(&DetourPresent1),
                                       reinterpret_cast<void**>(&g_orig_present1)) &&
              na::hooking::PrepareHook(vtable[kSlotResizeBuffers], reinterpret_cast<void*>(&DetourResizeBuffers),
                                       reinterpret_cast<void**>(&g_orig_resize)) &&
              na::hooking::PrepareHook(vtable[kSlotResizeBuffers1], reinterpret_cast<void*>(&DetourResizeBuffers1),
                                       reinterpret_cast<void**>(&g_orig_resize1)) &&
              na::hooking::PrepareHook(vtable[kSlotRelease], reinterpret_cast<void*>(&DetourRelease),
                                       reinterpret_cast<void**>(&g_orig_release));
    if (!ok) {
        na::hooking::AbandonInstall();
        g_attach_terminal=true;
        set_reason("Could not prepare the Streamline swap-chain hooks.");
        return false;
    }
    if(g_fail_proxy_commit.exchange(false))na::hooking::FailNextCommitEnableForTest(1);
    if (!na::hooking::CommitInstall()) {
        g_attach_terminal=true;
        set_reason("Could not enable the Streamline swap-chain hooks.");
        return false;
    }
    g_present_hooks.store(2, std::memory_order_release);
    g_discovery.store(static_cast<uint32_t>(how), std::memory_order_release);
    set_reason("Observing the game's Present into Streamline.");
    return true;
}

// ------------------------------------------------------------- RTTI discovery
struct Section { uintptr_t lo, hi; bool exec; };

uint32_t sections_of(HMODULE m, Section* out, uint32_t cap)
{
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const uint8_t*>(m) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    uint32_t n = 0;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections && n < cap; ++i, ++s) {
        if (!(s->Characteristics & IMAGE_SCN_MEM_READ)) continue;
        const uintptr_t lo = reinterpret_cast<uintptr_t>(m) + s->VirtualAddress;
        out[n++] = {lo, lo + s->Misc.VirtualSize, (s->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0};
    }
    return n;
}

bool in_exec(const Section* s, uint32_t n, uintptr_t a)
{
    for (uint32_t i = 0; i < n; ++i)
        if (s[i].exec && a >= s[i].lo && a < s[i].hi) return true;
    return false;
}

// MSVC x64 RTTI: TypeDescriptor{vftable*, spare*, char name[]}; CompleteObjectLocator
// {sig=1, offset, cdOffset, typeDescriptor RVA, classDescriptor RVA, self RVA};
// the vtable is preceded by a pointer to its COL. Returns the offset-0 vtable.
void* const* find_vtable_by_rtti_impl(HMODULE m, const char* name,uint32_t min_slots=kProxyVtableMinSlots)
{
    Section sec[32];
    const uint32_t ns = sections_of(m, sec, 32);
    const uintptr_t base = reinterpret_cast<uintptr_t>(m);
    const size_t name_len = strlen(name) + 1;
    for (uint32_t i = 0; i < ns; ++i) {
        if (sec[i].exec) continue;
        for (uintptr_t p = (sec[i].lo + 7) & ~uintptr_t(7); p + 16 + name_len <= sec[i].hi; p += 8) {
            if (memcmp(reinterpret_cast<const void*>(p + 16), name, name_len) != 0) continue;
            const uint32_t td_rva = static_cast<uint32_t>(p - base);
            for (uint32_t j = 0; j < ns; ++j) {
                if (sec[j].exec) continue;
                for (uintptr_t c = (sec[j].lo + 3) & ~uintptr_t(3); c + 24 <= sec[j].hi; c += 4) {
                    const auto* col = reinterpret_cast<const uint32_t*>(c);
                    if (col[0] != 1 || col[1] != 0 || col[3] != td_rva || col[5] != static_cast<uint32_t>(c - base)) continue;
                    for (uint32_t k = 0; k < ns; ++k) {
                        if (sec[k].exec) continue;
                        for (uintptr_t v = (sec[k].lo + 7) & ~uintptr_t(7); v + 8 * (1 + min_slots) <= sec[k].hi; v += 8) {
                            if (*reinterpret_cast<const uintptr_t*>(v) != c) continue;
                            void* const* vt = reinterpret_cast<void* const*>(v + 8);
                            bool all = true;
                            for (uint32_t s = 0; s < min_slots && all; ++s)
                                all = in_exec(sec, ns, reinterpret_cast<uintptr_t>(vt[s]));
                            if (all) return vt;
                        }
                    }
                }
            }
        }
    }
    return nullptr;
}

void* const* find_proxy_vtable(HMODULE m)
{
    for (const char* name : kProxyRttiNames) {
        void* const* vt = nullptr;
        __try {
            vt = find_vtable_by_rtti_impl(m, name);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            vt = nullptr;
        }
        if (vt) return vt;
    }
    return nullptr;
}

// ------------------------------------------------- native DLSS-G control
void fg_reason(const char* text)
{
    std::lock_guard<std::mutex> lock(g_fg_m);
    strncpy_s(g_fg.reason, text, _TRUNCATE);
}

void read_fg_state(const void* state)
{
    uint8_t b[96] = {};
    if(!state || guarded_copy(b,state,32)!=32 || !abi::HasGuid(b,32,kDlssgStateGuid))return;
    const uint64_t version = abi::Read<uint64_t>(b, 24);
    const size_t bytes=version==1?52:version<4?56:81;
    if(!version || guarded_copy(b,state,bytes)!=bytes)return;
    std::lock_guard<std::mutex> lock(g_fg_m);
    g_fg.status = abi::Read<uint32_t>(b, 40);
    g_fg.presented = abi::Read<uint32_t>(b, 48);
    if (version >= 2) g_fg.max_frames = abi::Read<uint32_t>(b, 52);
    if (version >= 4) g_fg.dynamic_supported = b[80] == 1 ? 1 : b[80] == 0 ? 0 : -1;
}

SlResult __cdecl DetourDlssgGetState(const void* viewport, void* state, const void* options)
{
    std::lock_guard<std::recursive_mutex> api_lock(g_fg_api_m);
    g_fg_get_calls.fetch_add(1, std::memory_order_relaxed);
    const SlResult r = g_orig_fg_get(viewport, state, options);
    if (r == 0) read_fg_state(state);
    return r;
}

// Our own state query (when the game never asks): a zeroed DLSSGState of
// version 4 includes dynamic support. Retry v2 for older runtimes.
void query_fg_state(const void* viewport)
{
    if (!g_orig_fg_get) return;
    alignas(8) uint8_t st[96] = {};
    memcpy(st + 8, kDlssgStateGuid, 16);
    uint64_t version = 4;
    memcpy(st + 24, &version, 8);
    st[80]=2; // unknown until explicitly reported
    if (g_orig_fg_get(viewport, st, nullptr) == 0) read_fg_state(st);
    else {version=2;memcpy(st+24,&version,8);if(g_orig_fg_get(viewport,st,nullptr)==0)read_fg_state(st);}
}

SlResult __cdecl DetourDlssgSetOptions(const void* viewport, const void* options)
{
    std::lock_guard<std::recursive_mutex> api_lock(g_fg_api_m);
    g_fg_set_calls.fetch_add(1, std::memory_order_relaxed);
    alignas(8) uint8_t copy[kDlssgOptionsCopy] = {};
    const size_t header = options ? guarded_copy(copy, options, 32) : 0;
    const auto version=header==32?abi::Read<uint64_t>(copy,24):0;
    const size_t size=version==1?104:version==2 || version==3?112:version==4 || version==5?120:0;
    const size_t got = size ? guarded_copy(copy,options,size) : 0;
    if (got < 40 || !abi::HasGuid(copy, got, kDlssgOptionsGuid)) {
        fg_reason("The game's frame generation options were not recognised; passed through unchanged.");
        return g_orig_fg_set(viewport, options);
    }
    const uint32_t game_mode = abi::Read<uint32_t>(copy, 32);
    const uint32_t game_frames = abi::Read<uint32_t>(copy, 36);
    const uint64_t packed = g_fg_request.load(std::memory_order_acquire);
    const bool effects_ready=na::settings::EffectsReady();
    const auto req_mode = effects_ready?static_cast<FgMode>(packed >> 32):FgMode::Game;
    const uint32_t req_frames = effects_ready?static_cast<uint32_t>(packed):0;
    {
        std::lock_guard<std::mutex> lock(g_fg_m);
        g_fg.game_mode = game_mode;
        g_fg.game_frames = game_frames;
    }
    auto pass = [&](const char* why) {
        const SlResult r = g_orig_fg_set(viewport, options);
        std::lock_guard<std::mutex> lock(g_fg_m);
        g_fg.sent_mode = game_mode;
        g_fg.sent_frames = game_frames;
        g_fg.last_result = r;
        if (why) strncpy_s(g_fg.reason, why, _TRUNCATE);
        return r;
    };
    if (req_mode == FgMode::Game && req_frames == 0) return pass("The game's frame generation settings are in use.");
    uint32_t max_frames, dynamic;
    {
        std::lock_guard<std::mutex> lock(g_fg_m);
        max_frames = g_fg.max_frames;
        dynamic = static_cast<uint32_t>(g_fg.dynamic_supported);
    }
    if (max_frames == 0xFFFFFFFFu || (req_mode==FgMode::Dynamic && dynamic==0xFFFFFFFFu)) {
        query_fg_state(viewport);
        std::lock_guard<std::mutex> lock(g_fg_m);
        max_frames = g_fg.max_frames;
        dynamic = static_cast<uint32_t>(g_fg.dynamic_supported);
    }
    uint32_t mode = game_mode;
    switch (req_mode) {
    case FgMode::Off: mode = kSlModeOff; break;
    case FgMode::On: mode = kSlModeOn; break;
    case FgMode::Dynamic: mode = kSlModeDynamic; break;
    default: break;
    }
    const uint32_t frames = req_frames ? req_frames : game_frames;
    if (mode == kSlModeDynamic && dynamic != 1u)
        return pass("Dynamic frame generation is not reported as supported here; the game's setting is kept.");
    if (mode != kSlModeOff && frames != game_frames) {
        if (max_frames == 0xFFFFFFFFu) return pass("The runtime has not reported its frame limit yet; the game's setting is kept.");
        if (frames < 1 || frames > max_frames) return pass("That frame count is above what this system supports; the game's setting is kept.");
    }
    memcpy(copy + 32, &mode, 4);
    memcpy(copy + 36, &frames, 4);
    const SlResult r = g_orig_fg_set(viewport, copy);
    std::lock_guard<std::mutex> lock(g_fg_m);
    g_fg.sent_mode = mode;
    g_fg.sent_frames = frames;
    g_fg.last_result = r;
    strncpy_s(g_fg.reason, r == 0 ? "Frame generation request applied to the game's DLSS-G options." :
                                    "Streamline refused the frame generation request.", _TRUNCATE);
    return r;
}

struct FgInstall { void* target; bool set; };

DWORD WINAPI deferred_fg_install(LPVOID p)
{
    auto* job = static_cast<FgInstall*>(p);
    std::lock_guard<std::mutex> install_lock(g_install_m);
    if (begin_install_retry()) {
        bool ok = job->set ? na::hooking::PrepareHook(job->target, reinterpret_cast<void*>(&DetourDlssgSetOptions),
                                                      reinterpret_cast<void**>(&g_orig_fg_set))
                           : na::hooking::PrepareHook(job->target, reinterpret_cast<void*>(&DetourDlssgGetState),
                                                      reinterpret_cast<void**>(&g_orig_fg_get));
        if (!ok) na::hooking::AbandonInstall();
        else if (na::hooking::CommitInstall()) (job->set ? g_fg_set_hooked : g_fg_get_hooked).store(1, std::memory_order_release);
        // A failed commit is rolled back by the hooking core.
    }
    delete job;
    return 0;
}

void request_fg_install(void* target, bool set)
{
    if (!target) return;
    std::atomic<bool>& flag = set ? g_fg_set_requested : g_fg_get_requested;
    if (flag.exchange(true, std::memory_order_acq_rel)) return;
    if (HANDLE t = CreateThread(nullptr, 0, &deferred_fg_install, new FgInstall{target, set}, 0, nullptr)) CloseHandle(t);
}

SlResult __cdecl DetourGetFeatureFunction(uint32_t feature, const char* name, void** function)
{
    const SlResult r = g_orig_gff(feature, name, function);
    if (r == 0 && feature == 0 && name && function && *function) dlss::OnFeatureFunction(name, *function); // sl::kFeatureDLSS
    if (r == 0 && feature == kFeatureDlssG && name && function && *function) {
        if (strcmp(name, "slDLSSGSetOptions") == 0) request_fg_install(*function, true);
        else if (strcmp(name, "slDLSSGGetState") == 0) request_fg_install(*function, false);
    }
    return r;
}

// The game may have resolved the DLSS-G functions before our hooks went in:
// look them up once ourselves (a lookup only; the device is set by then).
void lookup_fg_functions_once()
{
    if (g_gff_export) dlss::LookupOnce(reinterpret_cast<dlss::LookupFn>(g_gff_export));
    if (!g_gff_export || g_fg_lookup_done.exchange(true, std::memory_order_acq_rel)) return;
    auto gff = reinterpret_cast<PfnGetFeatureFunction>(g_gff_export);
    void* fn = nullptr;
    if (!g_fg_set_requested.load(std::memory_order_acquire) && gff(kFeatureDlssG, "slDLSSGSetOptions", &fn) == 0 && fn)
        request_fg_install(fn, true);
    fn = nullptr;
    if (!g_fg_get_requested.load(std::memory_order_acquire) && gff(kFeatureDlssG, "slDLSSGGetState", &fn) == 0 && fn)
        request_fg_install(fn, false);
}

// --------------------------------------------- fallback discovery (no RTTI)
struct DeferredVtable { void* const* vtable; Discovery how; };

DWORD WINAPI deferred_proxy_install(LPVOID p)
{
    auto* d = static_cast<DeferredVtable*>(p);
    for(unsigned attempt=0;attempt<3;++attempt){
        if(install_proxy_vtable(d->vtable,d->how) || g_attach_terminal.load() || g_present_hooks.load())break;
        if(attempt<2)Sleep(50);
    }
    if(!g_present_hooks.load() && !g_attach_terminal.load())g_proxy_install_requested.store(false);
    delete d;
    return 0;
}

void request_proxy_install(void* object, Discovery how)
{
    if (!object || g_present_hooks.load(std::memory_order_acquire)) return;
    void* const* vt = *static_cast<void* const* const*>(object);
    if (!in_module(vt[kSlotPresent])) return;
    if (g_proxy_install_requested.exchange(true, std::memory_order_acq_rel)) return;
    // Never install inside a detour: hand the vtable to a short-lived thread.
    auto* job=new DeferredVtable{vt,how};
    if(HANDLE t=CreateThread(nullptr,0,&deferred_proxy_install,job,0,nullptr))CloseHandle(t);
    else{delete job;g_proxy_install_requested.store(false);}
}

void inspect_swapchain(IUnknown* obj, Discovery how)
{
    void* raw = nullptr;
    if (obj && SUCCEEDED(obj->QueryInterface(kSlSwapChainIid, &raw)) && raw) {
        static_cast<IUnknown*>(raw)->Release();
        request_proxy_install(raw, how);
    }
}

HRESULT STDMETHODCALLTYPE DetourFactoryCreate(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** out)
{
    const HRESULT hr = g_orig_fc(f, dev, d, out);
    if (SUCCEEDED(hr) && out){bind_game_queue(*out,dev);inspect_swapchain(*out, Discovery::FactoryExport);}
    return hr;
}
HRESULT STDMETHODCALLTYPE DetourFactoryCreateHwnd(IDXGIFactory2* f, IUnknown* dev, HWND w, const DXGI_SWAP_CHAIN_DESC1* d,
                                                  const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* o, IDXGISwapChain1** out)
{
    const HRESULT hr = g_orig_fc_hwnd(f, dev, w, d, fs, o, out);
    if (SUCCEEDED(hr) && out){bind_game_queue(*out,dev);inspect_swapchain(*out, Discovery::FactoryExport);}
    return hr;
}
HRESULT STDMETHODCALLTYPE DetourFactoryCreateCore(IDXGIFactory2* f, IUnknown* dev, IUnknown* w, const DXGI_SWAP_CHAIN_DESC1* d,
                                                  IDXGIOutput* o, IDXGISwapChain1** out)
{
    const HRESULT hr = g_orig_fc_core(f, dev, w, d, o, out);
    if (SUCCEEDED(hr) && out){bind_game_queue(*out,dev);inspect_swapchain(*out, Discovery::FactoryExport);}
    return hr;
}
HRESULT STDMETHODCALLTYPE DetourFactoryCreateComp(IDXGIFactory2* f, IUnknown* dev, const DXGI_SWAP_CHAIN_DESC1* d, IDXGIOutput* o,
                                                  IDXGISwapChain1** out)
{
    const HRESULT hr = g_orig_fc_comp(f, dev, d, o, out);
    if (SUCCEEDED(hr) && out){bind_game_queue(*out,dev);inspect_swapchain(*out, Discovery::FactoryExport);}
    return hr;
}

DWORD WINAPI deferred_factory_install(LPVOID p)
{
    void* const* vt = static_cast<void* const*>(p);
    std::lock_guard<std::mutex> install_lock(g_install_m);
    bool acquired=false;
    for(unsigned attempt=0;attempt<3 && !acquired;++attempt){
        acquired=begin_install_retry();if(!acquired && attempt<2)Sleep(50);
    }
    if(!acquired){g_factory_install_requested.store(false);return 0;}
    bool prepared=na::hooking::PrepareHook(vt[kSlotCreateSwapChain], reinterpret_cast<void*>(&DetourFactoryCreate), reinterpret_cast<void**>(&g_orig_fc));
    prepared=na::hooking::PrepareHook(vt[kSlotCreateSwapChainForHwnd], reinterpret_cast<void*>(&DetourFactoryCreateHwnd), reinterpret_cast<void**>(&g_orig_fc_hwnd)) && prepared;
    prepared=na::hooking::PrepareHook(vt[kSlotCreateSwapChainForCoreWindow], reinterpret_cast<void*>(&DetourFactoryCreateCore), reinterpret_cast<void**>(&g_orig_fc_core)) && prepared;
    prepared=na::hooking::PrepareHook(vt[kSlotCreateSwapChainForComposition], reinterpret_cast<void*>(&DetourFactoryCreateComp), reinterpret_cast<void**>(&g_orig_fc_comp)) && prepared;
    if(!prepared){na::hooking::AbandonInstall();return 0;}
    if(!na::hooking::CommitInstall())set_reason("Optional Streamline factory hooks unavailable; independent proxy attachment continues.");
    return 0;
}

void inspect_object(void* obj, Discovery how)
{
    if (!obj) return;
    auto* unk = static_cast<IUnknown*>(obj);
    inspect_swapchain(unk, how);
    void* raw = nullptr;
    if (SUCCEEDED(unk->QueryInterface(kSlFactoryIid, &raw)) && raw) {
        static_cast<IUnknown*>(raw)->Release();
        void* const* vt = *static_cast<void* const* const*>(raw);
        if (in_module(vt[kSlotCreateSwapChain]) && !g_factory_install_requested.exchange(true, std::memory_order_acq_rel))
            if (HANDLE t = CreateThread(nullptr, 0, &deferred_factory_install, const_cast<void**>(vt), 0, nullptr)) CloseHandle(t);
            else g_factory_install_requested.store(false);
    }
}

SlResult __cdecl DetourUpgradeInterface(void** base)
{
    const SlResult r = g_orig_upgrade(base);
    if (r == 0 && base && *base) inspect_object(*base, Discovery::UpgradeInterface);
    return r;
}
HRESULT WINAPI DetourCreateFactory(REFIID riid, void** out)
{
    const HRESULT hr = g_orig_cf(riid, out);
    if (SUCCEEDED(hr) && out) inspect_object(*out, Discovery::FactoryExport);
    return hr;
}
HRESULT WINAPI DetourCreateFactory1(REFIID riid, void** out)
{
    const HRESULT hr = g_orig_cf1(riid, out);
    if (SUCCEEDED(hr) && out) inspect_object(*out, Discovery::FactoryExport);
    return hr;
}
HRESULT WINAPI DetourCreateFactory2(UINT flags, REFIID riid, void** out)
{
    const HRESULT hr = g_orig_cf2(flags, riid, out);
    if (SUCCEEDED(hr) && out) inspect_object(*out, Discovery::FactoryExport);
    return hr;
}

// --------------------------------------------------------------- installation
// File version of a loaded module from its VS_VERSIONINFO resource (no
// version.lib): the VS_FIXEDFILEINFO block is found by its signature inside
// the resource bytes. Returns false, leaving `out` untouched, when the module
// carries no version resource (test stubs) or the block is malformed.
bool read_module_file_version(HMODULE m, uint16_t out[4])
{
    HRSRC res = FindResourceW(m, MAKEINTRESOURCEW(1), reinterpret_cast<LPCWSTR>(RT_VERSION));
    if (!res) return false;
    const DWORD size = SizeofResource(m, res);
    HGLOBAL global = LoadResource(m, res);
    const auto* bytes = global ? static_cast<const uint8_t*>(LockResource(global)) : nullptr;
    if (!bytes || size < sizeof(VS_FIXEDFILEINFO)) return false;
    for (DWORD off = 0; off + sizeof(VS_FIXEDFILEINFO) <= size; off += 4) {
        VS_FIXEDFILEINFO info{};
        memcpy(&info, bytes + off, sizeof(info));
        if (info.dwSignature != 0xFEEF04BDu) continue;
        out[0] = static_cast<uint16_t>(info.dwFileVersionMS >> 16);
        out[1] = static_cast<uint16_t>(info.dwFileVersionMS & 0xFFFFu);
        out[2] = static_cast<uint16_t>(info.dwFileVersionLS >> 16);
        out[3] = static_cast<uint16_t>(info.dwFileVersionLS & 0xFFFFu);
        return true;
    }
    return false;
}

bool install_for_module(HMODULE m)
{
    std::lock_guard<std::mutex> attach_lock(g_attach_m);
    if (!m || g_attach_terminal || (g_attach_module && g_attach_module!=m)) return false;
    g_attach_module=m;g_module_seen.store(1,std::memory_order_release);
    g_retry_needed.store(false,std::memory_order_release);
    {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const uint8_t*>(m) + dos->e_lfanew);
        g_module_lo.store(reinterpret_cast<uintptr_t>(m), std::memory_order_release);
        g_module_hi.store(reinterpret_cast<uintptr_t>(m) + nt->OptionalHeader.SizeOfImage, std::memory_order_release);
    }
    {
        uint16_t version[4] = {0, 0, 0, 0};
        if (read_module_file_version(m, version)) {
            std::lock_guard<std::mutex> lock(g_fg_m);
            memcpy(g_fg.sl_version, version, sizeof(version));
        }
    }
    if (!g_hooking_initialized && !na::hooking::Initialize()) {
        set_reason("Hooking core failed to initialise.");
        return false;
    }
    g_hooking_initialized=true;
    bool any = g_tag_hooks.load()!=0 || g_present_hooks.load()!=0;
    // Tag exports.
    g_new_frame=reinterpret_cast<PfnNewFrame>(GetProcAddress(m,"slGetNewFrameToken"));
    auto* tag_frame = reinterpret_cast<void*>(GetProcAddress(m, "slSetTagForFrame"));
    auto* tag = reinterpret_cast<void*>(GetProcAddress(m, "slSetTag"));
    auto* gff = reinterpret_cast<void*>(GetProcAddress(m, "slGetFeatureFunction"));
    auto* constants = reinterpret_cast<void*>(GetProcAddress(m, "slSetConstants"));
    std::unique_lock<std::mutex> install_lock(g_install_m);
    if (!g_exports_done && begin_install_retry()) {
        std::vector<uint8_t*> batch;
        prepare_optional(gff,reinterpret_cast<void*>(&DetourGetFeatureFunction),reinterpret_cast<void**>(&g_orig_gff),g_export_state[0],batch);
        prepare_optional(tag_frame,reinterpret_cast<void*>(&DetourSetTagForFrame),reinterpret_cast<void**>(&g_orig_tag_frame),g_export_state[1],batch);
        prepare_optional(tag,reinterpret_cast<void*>(&DetourSetTag),reinterpret_cast<void**>(&g_orig_tag),g_export_state[2],batch);
        prepare_optional(constants,reinterpret_cast<void*>(&DetourSetConstants),reinterpret_cast<void**>(&g_orig_constants),g_export_state[3],batch);
        commit_optional(batch);
        g_exports_done=std::all_of(std::begin(g_export_state),std::end(g_export_state),[](uint8_t state){return state!=0;});
        g_gff_export=g_export_state[0]==1?gff:nullptr;
        const uint32_t n=(g_export_state[1]==1?1u:0u)+(g_export_state[2]==1?1u:0u);
        g_tag_hooks.store(n,std::memory_order_release);
        any=any || n!=0 || g_gff_export;
    }
    install_lock.unlock(); // install_proxy_vtable takes it again
    // Capture the game's queue BEFORE DLSS-G can substitute a private
    // presentation queue. Factory discovery is needed even with proxy RTTI.
    for(const char* name:{".?AUDXGIFactory@interposer@sl@@",".?AVDXGIFactory@interposer@sl@@"}){
        if(auto* factory=find_vtable_by_rtti_impl(m,name,kSlotCreateSwapChainForComposition+1)){
            if(!g_factory_install_requested.exchange(true))deferred_factory_install(const_cast<void**>(factory));
            break;
        }
    }
    // Proxy swap chain: RTTI first (every creation path shares this vtable).
    if (void* const* vt = find_proxy_vtable(m)) {
        any = install_proxy_vtable(vt, Discovery::Rtti) || any;
    }
    {
        install_lock.lock();
        if(g_fallback_done)return any;
        if (!begin_install_retry()) {
            set_reason("Streamline fallback attachment will retry after install contention.");
            return any;
        }
        // Optional discovery exports cannot disable an already working proxy.
        std::vector<uint8_t*> batch;
        const char* names[]={"slUpgradeInterface","CreateDXGIFactory","CreateDXGIFactory1","CreateDXGIFactory2"};
        void* detours[]={reinterpret_cast<void*>(&DetourUpgradeInterface),reinterpret_cast<void*>(&DetourCreateFactory),
            reinterpret_cast<void*>(&DetourCreateFactory1),reinterpret_cast<void*>(&DetourCreateFactory2)};
        void** originals[]={reinterpret_cast<void**>(&g_orig_upgrade),reinterpret_cast<void**>(&g_orig_cf),
            reinterpret_cast<void**>(&g_orig_cf1),reinterpret_cast<void**>(&g_orig_cf2)};
        for(unsigned i=0;i<4;++i)prepare_optional(reinterpret_cast<void*>(GetProcAddress(m,names[i])),detours[i],originals[i],g_fallback_state[i],batch);
        commit_optional(batch);g_fallback_done=true;
        if(!g_present_hooks.load())set_reason("No RTTI for Streamline's swap chain; waiting for slUpgradeInterface or a factory.");
    }
    if (!g_present_hooks.load(std::memory_order_acquire) && g_tag_hooks.load(std::memory_order_acquire) == 0)
        set_reason("Streamline loaded, but none of its entry points could be hooked.");
    return any;
}

void install_cb(HMODULE m) {
    // Module-watch/init workers only. Completed batches are never republished;
    // preparation/commit failures are terminal, unlike transaction contention.
    for(unsigned attempt=0;attempt<3;++attempt){
        install_for_module(m);
        if(g_attach_terminal.load() || !g_retry_needed.load(std::memory_order_acquire))return;
        if(attempt<2)Sleep(50);
    }
    set_reason("Streamline attachment exhausted its bounded install retries; completed hooks remain active.");
}

// One evidence file next to the user ini, written 30 s and 120 s after start:
// the status line plus every distinct tag shape. No file I/O in game callbacks.
wchar_t g_log_path[MAX_PATH] = {};

void write_log()
{
    if (!g_log_path[0]) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_log_path, L"w") != 0 || !f) return;
    char line[1024];
    FormatStatus(line, sizeof(line));
    std::fprintf(f, "%s\n", line);
    if (na::mode::FormatStatus(line, sizeof(line)) > 0) std::fprintf(f, "%s\n", line);
    std::lock_guard<std::mutex> lock(g_m);
    for (uint32_t i = 0; i < g_shape_count; ++i) {
        const TagShape& t = g_shapes[i];
        std::fprintf(f, "tag buffer=%s(%u) lifecycle=%s state=0x%x format=%u size=%ux%u command_buffer=%u for_frame=%u calls=%llu\n",
                     abi::BufferName(t.type), t.type, abi::LifecycleName(t.lifecycle), t.state, t.format, t.width, t.height,
                     t.cmd ? 1u : 0u, t.for_frame ? 1u : 0u, static_cast<unsigned long long>(t.count));
    }
    std::fclose(f);
}

DWORD WINAPI log_thread(LPVOID)
{
    Sleep(30000);
    write_log();
    Sleep(90000);
    write_log();
    return 0;
}

const wchar_t* const kModuleNames[] = {L"sl.interposer.dll", nullptr};

} // namespace

int32_t RetagForFrame(uint32_t token,uint32_t viewport,const Tag* tags,uint32_t count,ID3D12GraphicsCommandList* list) {
    if(!g_new_frame || !g_orig_tag_frame || !tags || !count || count>kMaxTagsPerCall || !list)return -1;
    // Public Streamline 2.x ABI, include/sl_core_types.h. Full Resource v1
    // storage (112 bytes), not the shorter prefix sufficient for decoding.
    struct alignas(8) ResourceBytes { uint8_t data[112]{}; } resources[kMaxTagsPerCall];
    struct alignas(8) TagBytes { uint8_t data[64]{}; } raw[kMaxTagsPerCall];
    alignas(8) uint8_t view[40]{};
    auto put=[](uint8_t* data,size_t offset,const auto& value){std::memcpy(data+offset,&value,sizeof(value));};
    const uint64_t version=1;
    std::memcpy(view+8,na::sl::kViewportGuid,16);put(view,24,version);put(view,32,viewport);
    for(uint32_t i=0;i<count;++i){
        const auto& t=tags[i];
        if(!t.resource || !t.resource_decoded || t.resource_type!=0 || t.state==UINT32_MAX || t.lifecycle!=abi::kOnlyValidNow)return -1;
        auto* r=resources[i].data;auto* b=raw[i].data;
        std::memcpy(r+8,abi::kResourceGuid,16);put(r,24,version);put(r,40,t.resource);put(r,64,t.state);
        put(r,68,t.width);put(r,72,t.height);put(r,76,t.native_format);
        const uint32_t one=1;put(r,80,one);put(r,84,one);
        std::memcpy(b+8,abi::kResourceTagGuid,16);put(b,24,version);put(b,32,r);
        put(b,40,t.buffer_type);put(b,44,t.lifecycle);put(b,48,t.extent_top);put(b,52,t.extent_left);
        put(b,56,t.extent_width);put(b,60,t.extent_height);
    }
    void* frame=nullptr;
    const auto result=g_new_frame(&frame,&token);if(result!=0)return result;
    uint32_t actual=0;if(!frame_token_value(frame,&actual) || actual!=token)return -1;
    return g_orig_tag_frame(frame,view,raw,count,list);
}

void SetOverlayHooks(const OverlayHooks* hooks) { g_overlay_hooks.store(hooks, std::memory_order_release); }

bool SetCallbacks(const Callbacks& callbacks)
{
    if (t_callback_depth > 0) return false; // would wait for itself
    std::lock_guard<std::mutex> lock(g_sub_m);
    const bool any = callbacks.on_tag || callbacks.after_tag || callbacks.on_constants || callbacks.before_present || callbacks.after_present || callbacks.stream_released;
    Subscription* next = any ? new Subscription : nullptr;
    if (next) next->cb = callbacks;
    Subscription* old = g_sub.exchange(next, std::memory_order_seq_cst);
    // Quiescence: a caller that acquired `old` before the exchange finishes
    // first; one acquiring after it sees `next` and backs out of `old`.
    if (old)
        while (old->inflight.load(std::memory_order_seq_cst) != 0) Sleep(0);
    return true; // `old` is intentionally not freed: a late acquirer may still touch its counter
}

bool FindConstants(uint32_t frame_token, uint32_t viewport, ConstantsRecord* out)
{
    if (!out) return false;
    std::lock_guard<std::mutex> lock(g_m);
    const uint32_t available = g_const_count < kConstRing ? g_const_count : kConstRing;
    for (uint32_t i = 0; i < available; ++i) {
        const ConstantsRecord& r = g_const_ring[(g_const_count - 1 - i) % kConstRing];
        if (r.frame_token == frame_token && r.viewport == viewport) {
            *out = r;
            return true;
        }
    }
    return false;
}

bool Start(const wchar_t* log_path)
{
    if (log_path && log_path[0]) {
        na::host::ProcessLogPath(g_log_path, MAX_PATH, log_path);
        if (HANDLE t = CreateThread(nullptr, 0, &log_thread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    // This runs on the host init worker, before the early-factory gate opens.
    // Already-loaded SL must be covered before the game creates its chain;
    // queuing it to another worker loses the pre-DLSS-G creation queue.
    for(const wchar_t* const* name=kModuleNames;*name;++name){
        HMODULE module=nullptr;
        if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN,*name,&module))install_cb(module);
    }
    return na::upscalers::AddModuleWatch(kModuleNames, &install_cb);
}

Stats GetStats()
{
    Stats s;
    s.module_seen = g_module_seen.load(std::memory_order_acquire);
    s.tag_hooks = g_tag_hooks.load(std::memory_order_acquire);
    s.present_hooks = g_present_hooks.load(std::memory_order_acquire);
    s.discovery = static_cast<Discovery>(g_discovery.load(std::memory_order_acquire));
    s.tag_calls = g_tag_calls.load(std::memory_order_relaxed);
    s.tag_undecoded = g_tag_undecoded.load(std::memory_order_relaxed);
    s.tags_seen = g_tags_seen.load(std::memory_order_relaxed);
    s.presents_real = g_real.load(std::memory_order_relaxed);
    s.presents_retry = g_retry.load(std::memory_order_relaxed);
    s.presents_test = g_test.load(std::memory_order_relaxed);
    s.proxies_seen = g_proxies.load(std::memory_order_relaxed);
    s.constants_calls = g_const_calls.load(std::memory_order_relaxed);
    s.constants_decoded = g_const_decoded.load(std::memory_order_relaxed);
    s.constants_valid_camera = g_const_valid.load(std::memory_order_relaxed);
    s.presents_unique = g_assoc_unique.load(std::memory_order_relaxed);
    s.presents_ambiguous = g_assoc_ambiguous.load(std::memory_order_relaxed);
    s.presents_untagged = g_assoc_none.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_m);
    for (const ProxyState& st : g_proxy_state)
        if (st.proxy) ++s.proxies_live;
    s.hudless_lifecycle = g_hud_life;
    s.hudless_state = g_hud_state;
    s.depth_lifecycle = g_depth_life;
    s.motion_lifecycle = g_motion_life;
    s.backbuffer_tagged = g_bb_tagged;
    s.ui_tagged = g_ui_tagged;
    s.ui_alpha_tagged = g_ui_alpha_tagged;
    strncpy_s(s.reason, g_reason, _TRUNCATE);
    return s;
}

int FormatStatus(char* out, size_t cap)
{
    if (!out || !cap) return 0;
    const Stats s = GetStats();
    static const char* const kHow[] = {"none", "rtti", "upgrade_interface", "factory_export"};
    const uint32_t how = static_cast<uint32_t>(s.discovery);
    const int n = _snprintf_s(out, cap, _TRUNCATE,
        "sl_boundary module=%u tag_hooks=%u present_hooks=%u discovery=%s tag_calls=%llu undecoded=%llu tags=%llu "
        "presents=%llu retry=%llu test=%llu assoc=%llu/%llu/%llu proxies=%u/%u constants=%llu/%llu/%llu "
        "hudless=%s/0x%x depth=%s motion=%s backbuffer_tag=%u ui_tag=%u ui_alpha_tag=%u reason=\"%s\"",
        s.module_seen, s.tag_hooks, s.present_hooks, how < 4 ? kHow[how] : "?",
        static_cast<unsigned long long>(s.tag_calls), static_cast<unsigned long long>(s.tag_undecoded),
        static_cast<unsigned long long>(s.tags_seen), static_cast<unsigned long long>(s.presents_real),
        static_cast<unsigned long long>(s.presents_retry), static_cast<unsigned long long>(s.presents_test),
        static_cast<unsigned long long>(s.presents_unique), static_cast<unsigned long long>(s.presents_ambiguous),
        static_cast<unsigned long long>(s.presents_untagged), s.proxies_live, s.proxies_seen,
        static_cast<unsigned long long>(s.constants_calls), static_cast<unsigned long long>(s.constants_decoded),
        static_cast<unsigned long long>(s.constants_valid_camera),
        abi::LifecycleName(s.hudless_lifecycle), s.hudless_state, abi::LifecycleName(s.depth_lifecycle),
        abi::LifecycleName(s.motion_lifecycle), s.backbuffer_tagged, s.ui_tagged, s.ui_alpha_tagged, s.reason);
    const int used = n < 0 ? static_cast<int>(strlen(out)) : n;
    const FgState fg = GetFgState();
    const int m = _snprintf_s(out + used, cap - static_cast<size_t>(used), _TRUNCATE,
        " | dlssg sl=%u.%u.%u.%u set_hook=%u get_hook=%u set_calls=%llu get_calls=%llu game=%u/%u sent=%u/%u max=%d dynamic=%d status=0x%x request=%u/%u result=%d \"%s\"",
        fg.sl_version[0], fg.sl_version[1], fg.sl_version[2], fg.sl_version[3],
        fg.set_hooked, fg.get_hooked, static_cast<unsigned long long>(fg.set_calls), static_cast<unsigned long long>(fg.get_calls),
        fg.game_mode, fg.game_frames, fg.sent_mode, fg.sent_frames,
        fg.max_frames == 0xFFFFFFFFu ? -1 : static_cast<int>(fg.max_frames), fg.dynamic_supported, fg.status,
        static_cast<uint32_t>(fg.request.mode), fg.request.frames, fg.last_result, fg.reason);
    return m < 0 ? static_cast<int>(strlen(out)) : used + m;
}

void SetFgRequest(const FgRequest& request)
{
    const uint64_t packed = (uint64_t(static_cast<uint32_t>(request.mode)) << 32) | request.frames;
    g_fg_request.store(packed, std::memory_order_release);
    std::lock_guard<std::mutex> lock(g_fg_m);
    g_fg.request = request;
}

FgState GetFgState()
{
    FgState out;
    {
        std::lock_guard<std::mutex> lock(g_fg_m);
        out = g_fg;
    }
    out.set_hooked = g_fg_set_hooked.load(std::memory_order_acquire);
    out.get_hooked = g_fg_get_hooked.load(std::memory_order_acquire);
    out.set_calls = g_fg_set_calls.load(std::memory_order_relaxed);
    out.get_calls = g_fg_get_calls.load(std::memory_order_relaxed);
    return out;
}

void FailNextAttachAcquireForTests(){g_fail_acquire.store(true);}
void FailNextProxyCommitForTests(){g_fail_proxy_commit.store(true);}
bool InstallForModuleForTests(void* module) { return install_for_module(static_cast<HMODULE>(module)); }

void ResetCountersForTests()
{
    g_tag_calls = 0;
    g_tag_undecoded = 0;
    g_tags_seen = 0;
    g_tag_seq = 0;
    g_real = 0;
    g_retry = 0;
    g_test = 0;
    g_proxies = 0;
    g_present_events = 0;
    std::lock_guard<std::mutex> lock(g_m);
    for (ProxyState& st : g_proxy_state) st = ProxyState{};
    g_last_present_tag_seq = 0;
    g_tag_window_count = 0;
    g_const_count = 0;
}

} // namespace na::slb
