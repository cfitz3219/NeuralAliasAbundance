#include "host_dx12/fg/frame_gen.hpp"
#include "host_dx12/depth_format.hpp"

#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "host_dx12/settings.hpp"
#include "host_dx12/diag/event_log.hpp"

#include <string>

#pragma comment(lib, "version.lib")

#if defined(NA_HAVE_NGX_SDK)
#pragma warning(push, 3)
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_defs_dlssg.h>
#include <nvsdk_ngx_params.h>
#pragma warning(pop)
#endif

using Microsoft::WRL::ComPtr;

namespace na::fg {
namespace {

constexpr uint32_t kFeatureFrameGeneration = 11; // NVSDK_NGX_Feature_FrameGeneration
constexpr uint32_t kHardMaxCount = 5;             // 6x; the runtime reports its own maximum

using PfnCreate = int32_t(__cdecl*)(void*, uint32_t, void*, void**);
using PfnEvaluate = int32_t(__cdecl*)(void*, const void*, const void*, void*);
using PfnRelease = int32_t(__cdecl*)(void*);
using PfnAllocParams = int32_t(__cdecl*)(void**);
using PfnDestroyParams = int32_t(__cdecl*)(void*);

struct State {
    std::mutex m;
    Runtime rt;
    std::atomic<uint32_t> has_rt{0};
    ComPtr<ID3D12Device> device;
    // Inputs (NeuralAlias-owned copies at render size).
    ComPtr<ID3D12Resource> depth_copy, motion_copy; // NPSR at rest
    DXGI_FORMAT motion_format = DXGI_FORMAT_UNKNOWN;
    uint32_t in_w = 0, in_h = 0;
    EvalInputs last_in{};
    uint32_t have_inputs = 0;
    uint32_t inputs_fresh = 0; // an evaluate happened since the last Generate
    // Recording and submission are different events. Protect receipts with a
    // separate lock, never held across NGX calls or GPU waits.
    struct InputReceipt {
        ID3D12CommandList* list = nullptr; // identity only, never dereferenced
        EvalInputs in{};
        ComPtr<ID3D12Resource> depth, motion;
        ComPtr<ID3D12CommandQueue> queue;
    };
    std::mutex input_m;
    InputReceipt pending[16], ready;
    ComPtr<ID3D12Resource> recording_depth, recording_motion;
    std::atomic<uint32_t> pending_count{0};
    std::atomic<uint64_t> inputs_recorded{0}, inputs_submitted{0}, inputs_overwritten{0}, missing_inputs{0};
    // Feature.
    void* params = nullptr;
    void* feature = nullptr;
    uint32_t feat_w = 0, feat_h = 0, feat_rw = 0, feat_rh = 0;
    DXGI_FORMAT feat_format = DXGI_FORMAT_UNKNOWN;
    uint32_t feat_hudless = 0; // the feature was created told that HUD-less colour comes with each frame
    uint32_t feat_ui = 0;      // bit 0: UI colour given, bit 1: UI alpha given (UI recomposition on when either, with HUD-less)
    uint32_t feature_pending = 0; // created on this frame's list; evaluate from the next frame
    uint32_t need_reset = 1;
    uint32_t max_supported = 0;
    uint32_t last_count = 0;
    // A74 review: nothing the GPU may still be reading is released at once.
    // A replaced feature or input copy waits in this list until kRetireAfter
    // further Generate calls have passed; by then the presenter's wait on the
    // slot ring (kSlots = 3 frames back) guarantees the lists that used it
    // have completed. Shutdown (called after the presenter waited for the
    // GPU) releases everything immediately.
    struct Retired { void* feature; ComPtr<ID3D12Resource> tex[2]; uint64_t due; };
    std::vector<Retired> retired;
    uint64_t generate_calls = 0;
    std::atomic<uint32_t> mv_size_mismatch{0}; // A74 review: motion vectors not at the depth's size (output-size MVs)
    // Status.
    std::atomic<uint32_t> active{0}, latched{0};
    std::atomic<uint64_t> evaluates{0}, generated{0}, real_frames{0}, skipped{0};
    std::atomic<float> target_fps{0.f};
    std::atomic<uint64_t> resets_cut{0}, resets_other{0};
    char reason[112] = "Frame Generation is off.";
    // Route B: the package's folder for nvngx_dlssg.dll, and what was found / loaded.
    std::mutex file_m;
    wchar_t folder[MAX_PATH] = {};
    RuntimeFile file{};
};
State g;

#if defined(NA_HAVE_NGX_SDK)
// Version of a DLL file as "a.b.c.d"; false when the file has no version record.
bool file_version(const wchar_t* path, uint32_t* major, uint32_t* minor, char* text, size_t n)
{
    DWORD h = 0, size = GetFileVersionInfoSizeW(path, &h);
    if (!size) return false;
    std::vector<uint8_t> info(size);
    VS_FIXEDFILEINFO* v = nullptr;
    UINT vl = 0;
    if (!GetFileVersionInfoW(path, 0, size, info.data()) || !VerQueryValueW(info.data(), L"\\", reinterpret_cast<void**>(&v), &vl) || !v)
        return false;
    *major = HIWORD(v->dwFileVersionMS);
    *minor = LOWORD(v->dwFileVersionMS);
    snprintf(text, n, "%u.%u.%u.%u", HIWORD(v->dwFileVersionMS), LOWORD(v->dwFileVersionMS), HIWORD(v->dwFileVersionLS), LOWORD(v->dwFileVersionLS));
    return true;
}
// The file the package should hold. false + plain-words reason when it is missing or too old.
bool check_folder_file(char* reason, size_t n)
{
    wchar_t folder[MAX_PATH];
    { std::lock_guard<std::mutex> lock(g.file_m); wcsncpy_s(folder, g.folder, _TRUNCATE); }
    if (!folder[0]) return true; // not a Route B package: the runtime finds its own file
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\nvngx_dlssg.dll", folder);
    char path8[MAX_PATH] = {};
    WideCharToMultiByte(CP_UTF8, 0, path, -1, path8, sizeof path8, nullptr, nullptr);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        snprintf(reason, n, "NVIDIA's Frame Generation file nvngx_dlssg.dll (version %u.%u or newer) is missing from %s.", kNeededMajor, kNeededMinor, path8);
        return false;
    }
    uint32_t major = 0, minor = 0;
    char ver[32] = "unknown";
    const bool have = file_version(path, &major, &minor, ver, sizeof ver);
    {
        std::lock_guard<std::mutex> lock(g.file_m);
        if (!g.file.loaded) { strncpy_s(g.file.path, path8, _TRUNCATE); strncpy_s(g.file.version, ver, _TRUNCATE); }
    }
    if (!have || major < kNeededMajor || (major == kNeededMajor && minor < kNeededMinor)) {
        snprintf(reason, n, "nvngx_dlssg.dll in %s is version %s; Frame Generation here needs %u.%u or newer.", path8, ver, kNeededMajor, kNeededMinor);
        return false;
    }
    return true;
}
// After the feature was created: which nvngx_dlssg.dll the runtime actually loaded.
void note_loaded_file()
{
    HMODULE m = GetModuleHandleW(L"nvngx_dlssg.dll");
    if (!m) return;
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(m, path, MAX_PATH)) return;
    uint32_t major = 0, minor = 0;
    char ver[32] = "unknown";
    file_version(path, &major, &minor, ver, sizeof ver);
    std::lock_guard<std::mutex> lock(g.file_m);
    WideCharToMultiByte(CP_UTF8, 0, path, -1, g.file.path, sizeof g.file.path, nullptr, nullptr);
    strncpy_s(g.file.version, ver, _TRUNCATE);
    g.file.loaded = true;
}
#endif

// Live Frame Generation: one run must show how far it got. Every link logs its first success and
// its first failure (once each), and every distinct reason text is logged the first time it appears.
void milestone(const char* link, bool ok, const char* detail)
{
    static std::mutex m;
    static std::vector<std::string> seen;
    std::string key = std::string(ok ? "+" : "-") + link;
    {
        std::lock_guard<std::mutex> lock(m);
        for (const auto& s : seen) if (s == key) return;
        if (seen.size() < 64) seen.push_back(key);
    }
    na::eventlog::Emit("fg_live", "FIRST %s %s%s%s", ok ? "SUCCESS" : "FAILURE", link, detail && *detail ? ": " : "", detail ? detail : "");
}
void set_reason(const char* text)
{
    if (std::strcmp(g.reason, text)) {
        static std::vector<std::string> seen; // caller holds g.m
        bool fresh = seen.size() < 48;
        for (const auto& s : seen) if (s == text) { fresh = false; break; }
        if (fresh) { seen.emplace_back(text); na::eventlog::Emit("fg_live", "reason: %s", text); }
    }
    strncpy_s(g.reason, text, _TRUNCATE);
}
void latch(const char* text)
{
    set_reason(text);
    g.latched.store(1, std::memory_order_release);
    g.active.store(0, std::memory_order_release);
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER br{};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    br.Transition.StateBefore = a;
    br.Transition.StateAfter = b;
    return br;
}

bool create_texture(UINT w, UINT h, DXGI_FORMAT fmt, D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& out)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    out.Reset();
    return SUCCEEDED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&out)));
}

constexpr uint64_t kRetireAfter = 4; // Generate calls (> kSlots of the presenter)

// Releases retired entries whose time has come (`all`: every entry, now).
void drain_retired_locked(bool all)
{
    for (size_t i = 0; i < g.retired.size();) {
        State::Retired& r = g.retired[i];
        if (all || r.due <= g.generate_calls) {
            if (r.feature && g.rt.release_feature) reinterpret_cast<PfnRelease>(g.rt.release_feature)(r.feature);
            g.retired.erase(g.retired.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

void release_feature_locked()
{
    if (g.feature) g.retired.push_back({g.feature, {}, g.generate_calls + kRetireAfter}); // A74 review: deferred
    g.feature = nullptr;
    g.feature_pending = 0;
    g.need_reset = 1;
}

#if defined(NA_HAVE_NGX_SDK)
NVSDK_NGX_Parameter* P() { return static_cast<NVSDK_NGX_Parameter*>(g.params); }

// NVIDIA's capability record for Frame Generation, in plain words. Caller holds g.m.
bool capability_locked(char* why, size_t n, unsigned* max_count)
{
    if (!check_folder_file(why, n)) return false;
    if (!g.rt.get_capability_parameters) { snprintf(why, n, "Frame Generation is waiting for the DLSS connection."); return false; }
    void* cap = nullptr;
    const int32_t r = reinterpret_cast<PfnAllocParams>(g.rt.get_capability_parameters)(&cap);
    if (!NVSDK_NGX_SUCCEED(static_cast<NVSDK_NGX_Result>(r)) || !cap) {
        snprintf(why, n, "NVIDIA's runtime did not answer the Frame Generation question (code 0x%08X).", static_cast<unsigned>(r));
        return false;
    }
    auto* c = static_cast<NVSDK_NGX_Parameter*>(cap);
    int avail = 0, needs = 0, init = 0;
    unsigned maj = 0, mnr = 0;
    c->Get(NVSDK_NGX_Parameter_FrameGeneration_Available, &avail);
    c->Get(NVSDK_NGX_Parameter_FrameGeneration_NeedsUpdatedDriver, &needs);
    c->Get(NVSDK_NGX_Parameter_FrameGeneration_FeatureInitResult, &init);
    c->Get(NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMajor, &maj);
    c->Get(NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMinor, &mnr);
    if (max_count) c->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, max_count);
    if (avail) return true;
    if (needs) {
        if (maj) snprintf(why, n, "Frame Generation needs a newer NVIDIA graphics driver (%u.%u or newer).", maj, mnr);
        else snprintf(why, n, "Frame Generation needs a newer NVIDIA graphics driver.");
    } else if (static_cast<uint32_t>(init) == static_cast<uint32_t>(NVSDK_NGX_Result_FAIL_FeatureNotSupported) ||
               static_cast<uint32_t>(init) == static_cast<uint32_t>(NVSDK_NGX_Result_FAIL_PlatformError)) {
        snprintf(why, n, "This graphics card does not support NVIDIA Frame Generation (it needs a GeForce RTX 40 series or newer).");
    } else if (static_cast<uint32_t>(init) == static_cast<uint32_t>(NVSDK_NGX_Result_FAIL_FeatureNotFound)) {
        snprintf(why, n, "NVIDIA's Frame Generation file nvngx_dlssg.dll was not found by the runtime.");
    } else {
        snprintf(why, n, "NVIDIA Frame Generation is not available here (NVIDIA code 0x%08X).", static_cast<unsigned>(init));
    }
    return false;
}

bool ensure_feature_locked(ID3D12GraphicsCommandList* list, const FrameInfo& f)
{
    if (g.feature && g.feat_w == f.width && g.feat_h == f.height && g.feat_format == f.format &&
        g.feat_rw == g.in_w && g.feat_rh == g.in_h && g.feat_hudless == (f.hudless ? 1u : 0u) &&
        g.feat_ui == ((f.hudless && f.ui ? 1u : 0u) | (f.hudless && f.ui_alpha ? 2u : 0u)))
        return true;
    release_feature_locked();
    if (!g.params) {
        void* p = nullptr;
        int32_t r = reinterpret_cast<PfnAllocParams>(g.rt.allocate_parameters)(&p);
        if (!NVSDK_NGX_SUCCEED(static_cast<NVSDK_NGX_Result>(r)) || !p) {
            latch("Frame Generation could not allocate its settings object.");
            return false;
        }
        g.params = p;
    }
    if (g.rt.get_capability_parameters) {
        char why[112];
        unsigned mx = 0;
        if (!capability_locked(why, sizeof why, &mx)) {
            latch(why);
            return false;
        }
        g.max_supported = mx ? (mx > kHardMaxCount ? kHardMaxCount : mx) : 1;
    } else {
        g.max_supported = 1;
    }
    NVSDK_NGX_Parameter* p = P();
    p->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    p->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    p->Set(NVSDK_NGX_DLSSG_Parameter_Width, f.width);
    p->Set(NVSDK_NGX_DLSSG_Parameter_Height, f.height);
    p->Set(NVSDK_NGX_Parameter_Width, f.width);
    p->Set(NVSDK_NGX_Parameter_Height, f.height);
    p->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFormat, static_cast<unsigned>(f.format));
    p->Set(NVSDK_NGX_DLSSG_Parameter_InternalWidth, g.in_w);
    p->Set(NVSDK_NGX_DLSSG_Parameter_InternalHeight, g.in_h);
    p->Set(NVSDK_NGX_DLSSG_Parameter_DynamicResolution, 0u);
    // No distortion field; HUD-less colour and UI layers only when the caller supplies them.
    // UI recomposition needs HUD-less colour plus UI colour or UI alpha (SDK header).
    const bool ui = f.hudless && f.ui, ui_alpha = f.hudless && f.ui_alpha;
    unsigned never = NVSDK_NGX_DLSSG_ResourceFlags_BidirectionalDistortionField;
    if (!f.hudless) never |= NVSDK_NGX_DLSSG_ResourceFlags_HUDLess;
    if (!ui) never |= NVSDK_NGX_DLSSG_ResourceFlags_UI;
    if (!ui_alpha) never |= NVSDK_NGX_DLSSG_ResourceFlags_UIAlpha;
    p->Set(NVSDK_NGX_DLSSG_Parameter_ResourceNeverProvided_Flags, never);
    p->Set(NVSDK_NGX_DLSSG_Parameter_UserInterfaceRecompositionEnabled, (ui || ui_alpha) ? 1u : 0u);
    void* handle = nullptr;
    int32_t r = reinterpret_cast<PfnCreate>(g.rt.create_feature)(list, kFeatureFrameGeneration, g.params, &handle);
    if (!NVSDK_NGX_SUCCEED(static_cast<NVSDK_NGX_Result>(r)) || !handle) {
        char buf[112];
        snprintf(buf, sizeof(buf), "Frame Generation could not start (NVIDIA code 0x%08X).", static_cast<unsigned>(r));
        latch(buf);
        return false;
    }
    g.feature = handle;
    {
        char d[160];
        snprintf(d, sizeof d, "%ux%u format %u from %ux%u, HUD-less %u", f.width, f.height, static_cast<unsigned>(f.format), g.in_w, g.in_h, f.hudless ? 1u : 0u);
        milestone("NVIDIA Frame Generation feature created", true, d);
    }
    note_loaded_file();
    g.feat_w = f.width;
    g.feat_h = f.height;
    g.feat_rw = g.in_w;
    g.feat_rh = g.in_h;
    g.feat_format = f.format;
    g.feat_hudless = f.hudless ? 1u : 0u;
    g.feat_ui = (ui ? 1u : 0u) | (ui_alpha ? 2u : 0u);
    g.feature_pending = 1;
    g.need_reset = 1;
    return true;
}

bool evaluate_locked(ID3D12GraphicsCommandList* list, const FrameInfo& f, ID3D12Resource* output, uint32_t count, uint32_t index)
{
    NVSDK_NGX_Parameter* p = P();
    p->Set(NVSDK_NGX_DLSSG_Parameter_Backbuffer, f.color);
    p->Set(NVSDK_NGX_DLSSG_Parameter_MVecs, g.motion_copy.Get());
    p->Set(NVSDK_NGX_DLSSG_Parameter_Depth, g.depth_copy.Get());
    p->Set(NVSDK_NGX_DLSSG_Parameter_HUDLess, f.hudless);
    p->Set(NVSDK_NGX_DLSSG_Parameter_UI, (g.feat_ui & 1u) ? f.ui : static_cast<ID3D12Resource*>(nullptr));
    p->Set(NVSDK_NGX_DLSSG_Parameter_UIAlpha, (g.feat_ui & 2u) ? f.ui_alpha : static_cast<ID3D12Resource*>(nullptr));
    if (g.feat_ui & 1u) {
        p->Set(NVSDK_NGX_DLSSG_Parameter_UISubrectBaseX, 0u);
        p->Set(NVSDK_NGX_DLSSG_Parameter_UISubrectBaseY, 0u);
        p->Set(NVSDK_NGX_DLSSG_Parameter_UISubrectWidth, f.width);
        p->Set(NVSDK_NGX_DLSSG_Parameter_UISubrectHeight, f.height);
    }
    if (g.feat_ui & 2u) {
        p->Set(NVSDK_NGX_DLSSG_Parameter_UIAlphaSubrectBaseX, 0u);
        p->Set(NVSDK_NGX_DLSSG_Parameter_UIAlphaSubrectBaseY, 0u);
        p->Set(NVSDK_NGX_DLSSG_Parameter_UIAlphaSubrectWidth, f.width);
        p->Set(NVSDK_NGX_DLSSG_Parameter_UIAlphaSubrectHeight, f.height);
    }
    p->Set(NVSDK_NGX_DLSSG_Parameter_BidirectionalDistortionField, static_cast<ID3D12Resource*>(nullptr));
    p->Set(NVSDK_NGX_DLSSG_Parameter_OutputInterpolated, output);
    p->Set(NVSDK_NGX_DLSSG_Parameter_OutputReal, static_cast<ID3D12Resource*>(nullptr));
    p->Set(NVSDK_NGX_DLSSG_Parameter_OutputDisableInterpolation, f.disable_flag); // A62: runtime veto (null = not used)
    p->Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, count);
    p->Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, index);
    // Exact SL frame/viewport camera, carried with the submitted depth/MV.
    // Other adapters retain the explicit unavailable fallback pending qualification.
    static float zero[16]{};
    static float identity[16]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    auto& camera = g.last_in.camera;
    // Route B: the all-zero camera of the proven offline configuration, passed as a camera.
    const bool have_camera = g.last_in.camera_valid || g.last_in.camera_free;
    p->Set(NVSDK_NGX_DLSSG_Parameter_CameraViewToClip, static_cast<void*>(have_camera ? camera.view_to_clip : zero));
    p->Set(NVSDK_NGX_DLSSG_Parameter_ClipToCameraView, static_cast<void*>(have_camera ? camera.clip_to_view : zero));
    p->Set(NVSDK_NGX_DLSSG_Parameter_ClipToLensClip, static_cast<void*>(identity)); // TLOU provides no lens field
    p->Set(NVSDK_NGX_DLSSG_Parameter_ClipToPrevClip, static_cast<void*>(have_camera ? camera.clip_to_previous : zero));
    p->Set(NVSDK_NGX_DLSSG_Parameter_PrevClipToClip, static_cast<void*>(have_camera ? camera.previous_to_clip : zero));
    p->Set(NVSDK_NGX_DLSSG_Parameter_JitterOffsetX, g.last_in.jitter_x);
    p->Set(NVSDK_NGX_DLSSG_Parameter_JitterOffsetY, g.last_in.jitter_y);
    // DLSS: value * scale = render pixels. DLSS-G wants values normalised to
    // the render extent, so divide the pixel scale by the render size.
    float rw = static_cast<float>(g.in_w ? g.in_w : 1u), rh = static_cast<float>(g.in_h ? g.in_h : 1u);
    p->Set(NVSDK_NGX_DLSSG_Parameter_MvecScaleX, g.last_in.mv_scale_x / rw);
    p->Set(NVSDK_NGX_DLSSG_Parameter_MvecScaleY, g.last_in.mv_scale_y / rh);
    p->Set(NVSDK_NGX_DLSSG_Parameter_CameraPinholeOffsetX, have_camera ? camera.pinhole[0] : 0.f);
    p->Set(NVSDK_NGX_DLSSG_Parameter_CameraPinholeOffsetY, have_camera ? camera.pinhole[1] : 0.f);
    // Keep the complete camera contract together. These fields are captured
    // from the same SL frame as the matrices, not from the latest camera.
    p->Set(NVSDK_NGX_DLSSG_Parameter_CameraPosX, have_camera ? camera.position[0] : 0.f);
    p->Set(NVSDK_NGX_DLSSG_Parameter_CameraPosY, have_camera ? camera.position[1] : 0.f);
    p->Set(NVSDK_NGX_DLSSG_Parameter_CameraPosZ, have_camera ? camera.position[2] : 0.f);
    p->Set(NVSDK_NGX_DLSSG_Parameter_CameraUpX, have_camera ? camera.up[0] : 0.f);
    p->Set(NVSDK_NGX_DLSSG_Parameter_CameraUpY, have_camera ? camera.up[1] : 0.f);
    p->Set(NVSDK_NGX_DLSSG_Parameter_CameraUpZ, have_camera ? camera.up[2] : 0.f);
    p->Set(NVSDK_NGX_DLSSG_Parameter_CameraRightX, have_camera ? camera.right[0] : 0.f);
    p->Set(NVSDK_NGX_DLSSG_Parameter_CameraRightY, have_camera ? camera.right[1] : 0.f);
    p->Set(NVSDK_NGX_DLSSG_Parameter_CameraRightZ, have_camera ? camera.right[2] : 0.f);
    p->Set(NVSDK_NGX_DLSSG_Parameter_CameraFwdX, have_camera ? camera.forward[0] : 0.f);
    p->Set(NVSDK_NGX_DLSSG_Parameter_CameraFwdY, have_camera ? camera.forward[1] : 0.f);
    p->Set(NVSDK_NGX_DLSSG_Parameter_CameraFwdZ, have_camera ? camera.forward[2] : 0.f);
    p->Set(NVSDK_NGX_DLSSG_Parameter_CameraNear, have_camera ? camera.near_plane : 0.f);
    p->Set(NVSDK_NGX_DLSSG_Parameter_CameraFar, have_camera ? camera.far_plane : 0.f);
    p->Set(NVSDK_NGX_DLSSG_Parameter_CameraFOV, have_camera ? camera.fov : 0.f);
    p->Set(NVSDK_NGX_DLSSG_Parameter_CameraAspectRatio, have_camera ? camera.aspect : 0.f);
    p->Set(NVSDK_NGX_DLSSG_Parameter_ColorBuffersHDR, f.hdr ? 1u : 0u);
    p->Set(NVSDK_NGX_DLSSG_Parameter_DepthInverted, g.last_in.depth_inverted ? 1u : 0u);
    p->Set(NVSDK_NGX_DLSSG_Parameter_CameraMotionIncluded, have_camera ? unsigned(camera.camera_motion) : 1u);
    const bool reset = g.need_reset || g.last_in.reset || (have_camera && camera.reset);
    if (reset && index == 1) (g.last_in.reset_reason == 1 ? g.resets_cut : g.resets_other).fetch_add(1, std::memory_order_relaxed);
    p->Set(NVSDK_NGX_DLSSG_Parameter_Reset, reset ? 1u : 0u);
    p->Set(NVSDK_NGX_DLSSG_Parameter_AutomodeOverrideReset, 0u);
    p->Set(NVSDK_NGX_DLSSG_Parameter_NotRenderingGameFrames, have_camera ? unsigned(camera.not_game) : 0u);
    p->Set(NVSDK_NGX_DLSSG_Parameter_OrthoProjection, have_camera ? unsigned(camera.ortho) : 0u);
    p->Set(NVSDK_NGX_DLSSG_Parameter_MvecInvalidValue, have_camera ? camera.invalid_motion : 0.f);
    p->Set(NVSDK_NGX_DLSSG_Parameter_MvecDilated, have_camera ? unsigned(camera.dilated) : 0u);
    p->Set(NVSDK_NGX_DLSSG_Parameter_MvecJittered, g.last_in.mv_jittered ? 1u : 0u);
    p->Set(NVSDK_NGX_DLSSG_Parameter_MenuDetectionEnabled, 1u); // we cannot tell the runtime about menus ourselves
    p->Set(NVSDK_NGX_DLSSG_Parameter_MVecsSubrectBaseX, g.last_in.mv_base_x);
    p->Set(NVSDK_NGX_DLSSG_Parameter_MVecsSubrectBaseY, g.last_in.mv_base_y);
    p->Set(NVSDK_NGX_DLSSG_Parameter_MVecsSubrectWidth, g.in_w);
    p->Set(NVSDK_NGX_DLSSG_Parameter_MVecsSubrectHeight, g.in_h);
    p->Set(NVSDK_NGX_DLSSG_Parameter_DepthSubrectBaseX, g.last_in.depth_base_x);
    p->Set(NVSDK_NGX_DLSSG_Parameter_DepthSubrectBaseY, g.last_in.depth_base_y);
    p->Set(NVSDK_NGX_DLSSG_Parameter_DepthSubrectWidth, g.in_w);
    p->Set(NVSDK_NGX_DLSSG_Parameter_DepthSubrectHeight, g.in_h);
    p->Set(NVSDK_NGX_DLSSG_Parameter_InputBackbufferSubrectBaseX, 0u);
    p->Set(NVSDK_NGX_DLSSG_Parameter_InputBackbufferSubrectBaseY, 0u);
    p->Set(NVSDK_NGX_DLSSG_Parameter_InputBackbufferSubrectWidth, f.width);
    p->Set(NVSDK_NGX_DLSSG_Parameter_InputBackbufferSubrectHeight, f.height);
    // The deprecated whole-back-buffer extent too: the driver-store runtime (310.2.1) checks it
    // ("Invalid Backbuffer resource extent", fg_capture_replay 2026-11-25). Same values.
    p->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferSubrectBaseX, 0u);
    p->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferSubrectBaseY, 0u);
    p->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferSubrectWidth, f.width);
    p->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferSubrectHeight, f.height);
    if (f.hudless) {
        p->Set(NVSDK_NGX_DLSSG_Parameter_HUDLessSubrectBaseX, 0u);
        p->Set(NVSDK_NGX_DLSSG_Parameter_HUDLessSubrectBaseY, 0u);
        p->Set(NVSDK_NGX_DLSSG_Parameter_HUDLessSubrectWidth, f.width);
        p->Set(NVSDK_NGX_DLSSG_Parameter_HUDLessSubrectHeight, f.height);
    }
    p->Set(NVSDK_NGX_DLSSG_Parameter_OutputInterpolatedSubrectBaseX, 0u);
    p->Set(NVSDK_NGX_DLSSG_Parameter_OutputInterpolatedSubrectBaseY, 0u);
    p->Set(NVSDK_NGX_DLSSG_Parameter_OutputInterpolatedSubrectWidth, f.width);
    p->Set(NVSDK_NGX_DLSSG_Parameter_OutputInterpolatedSubrectHeight, f.height);
    p->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, static_cast<unsigned long long>(f.frame_id));
    int32_t r = reinterpret_cast<PfnEvaluate>(g.rt.evaluate_feature)(list, g.feature, g.params, nullptr);
    if (!NVSDK_NGX_SUCCEED(static_cast<NVSDK_NGX_Result>(r))) {
        char buf[112];
        snprintf(buf, sizeof(buf), "Frame Generation stopped (NVIDIA code 0x%08X).", static_cast<unsigned>(r));
        latch(buf);
        return false;
    }
    g.evaluates.fetch_add(1, std::memory_order_relaxed);
    milestone("Frame Generation evaluate recorded", true, nullptr);
    return true;
}
#endif

} // namespace

void SetRuntime(const Runtime& rt)
{
    std::lock_guard<std::mutex> lock(g.m);
    g.rt = rt;
    bool ok = rt.create_feature && rt.evaluate_feature && rt.release_feature && rt.allocate_parameters;
    g.has_rt.store(ok ? 1u : 0u, std::memory_order_release);
}
bool HasRuntime() { return g.has_rt.load(std::memory_order_acquire) != 0; }

bool WantsInputs()
{
    return na::settings::EffectsReady() && HasRuntime() && na::settings::Get().fg_enabled.load(std::memory_order_relaxed) != 0 &&
           !g.latched.load(std::memory_order_acquire);
}

void OnEvaluate(ID3D12GraphicsCommandList* list, const EvalInputs& in)
{
    if (!list || !in.depth || !in.motion) return;
    std::lock_guard<std::mutex> lock(g.m);
    if (!WantsInputs()) return;
    std::lock_guard<std::mutex> inputs(g.input_m);
    State::InputReceipt* receipt = nullptr;
    for (auto& p : g.pending) if (p.list == list) { receipt = &p; break; }
    if (!receipt) for (auto& p : g.pending) if (!p.list) { receipt = &p; break; }
    if (!receipt) { latch("Frame Generation stopped: too many unsubmitted input lists."); return; }
    if (!g.device) {
        ComPtr<ID3D12Device> dev;
        if (FAILED(list->GetDevice(IID_PPV_ARGS(&dev)))) return;
        g.device = dev;
    }
    D3D12_RESOURCE_DESC dd = in.depth->GetDesc(), md = in.motion->GetDesc();
    uint32_t w = static_cast<uint32_t>(dd.Width), h = dd.Height;
    auto covers = [](const D3D12_RESOURCE_DESC& d, uint32_t x, uint32_t y, uint32_t rw, uint32_t rh) {
        return rw && rh && x != UINT32_MAX && y != UINT32_MAX &&
            uint64_t(x) + rw <= d.Width && uint64_t(y) + rh <= d.Height;
    };
    if (!in.mv_low_res || !covers(dd, in.depth_base_x, in.depth_base_y, in.render_w, in.render_h) ||
        !covers(md, in.mv_base_x, in.mv_base_y, in.render_w, in.render_h)) {
        g.skipped.fetch_add(1, std::memory_order_relaxed);
        g.mv_size_mismatch.store(1, std::memory_order_release); // A74 review: say so instead of "waiting"
        milestone("inputs recorded at the DLSS evaluate", false, "the motion or depth does not cover the rendering size");
        return;
    }
    g.mv_size_mismatch.store(0, std::memory_order_relaxed);
    if (!na::depth::PlaneZeroCopiesToR32F(dd.Format)) { // Argon audit #2: never issue an invalid depth copy
        char buf[112];
        snprintf(buf, sizeof(buf), "Frame Generation is not available in this game yet: it uses %s depth.", na::depth::DepthKind(dd.Format));
        latch(buf);
        return;
    }
    if (!g.recording_depth || g.recording_depth->GetDesc().Width != w ||
        g.recording_depth->GetDesc().Height != h || g.recording_motion->GetDesc().Format != md.Format ||
        g.recording_motion->GetDesc().Width != md.Width || g.recording_motion->GetDesc().Height != md.Height) {
        // A74 review: the previous copies may still be read by lists in flight; retire, do not free.
        if (g.recording_depth) g.retired.push_back({nullptr, {g.recording_depth, g.recording_motion}, g.generate_calls + kRetireAfter});
        ComPtr<ID3D12Resource> depth_new, motion_new;
        if (!create_texture(w, h, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, depth_new) ||
            !create_texture(static_cast<uint32_t>(md.Width), md.Height, md.Format, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, motion_new)) {
            latch("Frame Generation could not allocate its motion buffers.");
            return;
        }
        g.recording_depth = depth_new;
        g.recording_motion = motion_new;
    }
    D3D12_RESOURCE_BARRIER pre[4] = {
        transition(in.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
        transition(in.motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
        transition(g.recording_depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
        transition(g.recording_motion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST)};
    pre[0].Transition.Subresource = 0; // depth plane only
    list->ResourceBarrier(4, pre);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.Type = dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = in.depth;
    src.SubresourceIndex = 0;
    dst.pResource = g.recording_depth.Get();
    dst.SubresourceIndex = 0;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    list->CopyResource(g.recording_motion.Get(), in.motion);
    D3D12_RESOURCE_BARRIER post[4] = {
        transition(in.depth, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        transition(in.motion, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        transition(g.recording_depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        transition(g.recording_motion.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
    post[0].Transition.Subresource = 0;
    list->ResourceBarrier(4, post);
    if (!receipt->list) g.pending_count.fetch_add(1, std::memory_order_release);
    receipt->list = list;
    receipt->in = in;
    receipt->in.depth = receipt->in.motion = nullptr;
    receipt->depth = g.recording_depth;
    receipt->motion = g.recording_motion;
    g.inputs_recorded.fetch_add(1, std::memory_order_relaxed);
    {
        char d[96];
        snprintf(d, sizeof d, "%ux%u, camera %s, reset %u", in.render_w, in.render_h, in.camera_free ? "free (zero)" : (in.camera_valid ? "game" : "none"), in.reset);
        milestone("inputs recorded at the DLSS evaluate", true, d);
    }
}

void Execute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists, ExecuteFn forward)
{
    if (!g.pending_count.load(std::memory_order_acquire)) { forward(queue, count, lists); return; }
    std::unique_lock<std::mutex> lock(g.input_m);
    bool found = false;
    for (UINT i = 0; i < count; ++i)
        for (const auto& p : g.pending) if (p.list && p.list == lists[i]) found = true;
    if (!found) { lock.unlock(); forward(queue, count, lists); return; }
    forward(queue, count, lists);
    for (UINT i = 0; i < count; ++i) for (auto& p : g.pending) {
        if (!p.list || p.list != lists[i]) continue;
        if (g.ready.list) g.inputs_overwritten.fetch_add(1, std::memory_order_relaxed);
        g.ready = p;
        g.ready.queue = queue;
        p = {};
        g.pending_count.fetch_sub(1, std::memory_order_release);
        g.inputs_submitted.fetch_add(1, std::memory_order_relaxed);
        milestone("inputs submitted with the game's list", true, nullptr);
    }
}

uint32_t ChooseCount(float base_frame_ms, float target_fps, uint32_t max_count, uint32_t previous)
{
    // A72: NVIDIA's DLSS-G contract (Streamline DLSS-G programming guide):
    // numFramesToGenerate is between 1 and the runtime's maximum, and in
    // dynamic mode "Streamline automatically adjusts the number of generated
    // frames to align the output frame rate with the display's refresh rate
    // or a user-defined target". Generation never switches itself off while
    // enabled: the minimum is ONE generated picture per rendered frame (2x).
    // (A71's display ceiling, which produced 0 above half the refresh rate and
    // therefore cycled on and off around that point, is gone.)
    if (max_count == 0) return 0;
    if (!(base_frame_ms > 0.f) || !(target_fps > 0.f)) return 1; // rate or target unknown: 2x
    // Frames per rendered frame that align the shown rate with the target.
    float ideal = target_fps * base_frame_ms / 1000.f; // e.g. 165 Hz at 10.0 ms -> 1.65
    if (previous < 1) previous = 1;
    if (previous > max_count) previous = max_count;
    float prev_total = static_cast<float>(previous + 1);
    // Hysteresis: stay put while the ideal is within a quarter step of the current total.
    if (std::fabs(ideal - prev_total) < 0.25f) return previous;
    // A73: the shown rate lands AT OR JUST UNDER the target (round down, 5 %
    // tolerance), the way NVIDIA's Reflex cap keeps native Frame Generation
    // under the refresh rate: pictures above the refresh rate cannot be
    // shown by a fixed-rate display and with V-Sync on they queue up. The
    // minimum stays 2x (a base rate already at or above the target gets 2x
    // regardless: generation never switches itself off).
    float total = std::floor(ideal + 0.05f);
    if (total < 2.f) total = 2.f;
    uint32_t count = static_cast<uint32_t>(total) - 1;
    return count > max_count ? max_count : count;
}

void NotePaused(const char* why)
{
    // A74 review: under the same lock as every other writer of the reason,
    // and never over a latched error (opening the menu to read why Frame
    // Generation stopped must not erase the NVIDIA code it stopped with).
    std::lock_guard<std::mutex> lock(g.m);
    if (g.latched.load(std::memory_order_acquire)) return;
    if (why && *why) set_reason(why);
    g.active.store(0, std::memory_order_release);
    g.need_reset=1; // resume must not interpolate across a menu/pause gap
}

uint32_t Generate(ID3D12GraphicsCommandList* list, ID3D12Device* device, const FrameInfo& f,
                  ID3D12Resource* const* outputs, uint32_t capacity)
{
    const na::settings::Settings& st = na::settings::Get();
    if (!na::settings::EffectsReady() || !st.fg_enabled.load(std::memory_order_relaxed)) {
        g.active.store(0, std::memory_order_release);
        return 0;
    }
    std::lock_guard<std::mutex> lock(g.m);
    g.real_frames.fetch_add(1, std::memory_order_relaxed);
    ++g.generate_calls;
    drain_retired_locked(false); // A74 review: what the GPU can no longer be using
    if (!list || !device || !f.color || !outputs || !capacity) { g.skipped.fetch_add(1, std::memory_order_relaxed); return 0; }
    if (!HasRuntime()) { set_reason("Frame Generation is waiting for the DLSS connection."); g.skipped.fetch_add(1, std::memory_order_relaxed); return 0; }
    if (g.latched.load(std::memory_order_acquire)) { g.skipped.fetch_add(1, std::memory_order_relaxed); return 0; }
    if (!g.device) g.device = device;
    {
        std::lock_guard<std::mutex> inputs(g.input_m);
        g.inputs_fresh = g.ready.list ? 1u : 0u;
        if (g.inputs_fresh) {
            if (g.ready.queue.Get() != f.queue) {
                g.ready = {};
                latch("Frame Generation stopped: its inputs came from another queue.");
                return 0;
            }
            if (g.depth_copy != g.ready.depth || g.motion_copy != g.ready.motion) {
                if (g.depth_copy) g.retired.push_back({nullptr, {g.depth_copy, g.motion_copy}, g.generate_calls + kRetireAfter});
                g.need_reset = 1;
            }
            g.depth_copy = g.ready.depth; g.motion_copy = g.ready.motion;
            g.in_w = g.ready.in.render_w;
            g.in_h = g.ready.in.render_h;
            g.motion_format = g.motion_copy->GetDesc().Format;
            g.last_in = g.ready.in;
            g.have_inputs = 1;
            g.ready = {};
        }
    }
    if (!g.have_inputs) {
        if (g.mv_size_mismatch.load(std::memory_order_acquire))
            set_reason("Frame Generation is not available in this game: its motion vectors are not at the rendering size (output-size motion vectors are not supported yet).");
        else
            set_reason("Waiting for the game's first DLSS frame.");
        g.active.store(0, std::memory_order_release);
        g.skipped.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    if (!g.inputs_fresh) {
        g.missing_inputs.fetch_add(1, std::memory_order_relaxed);
        // Menus, videos, loading screens: the game did not draw through DLSS
        // for this frame, so there is no motion to interpolate with.
        set_reason("Frame Generation is paused: the game is not drawing through DLSS right now (menus, videos).");
        g.active.store(0, std::memory_order_release);
        g.need_reset = 1;
        g.last_count = 0;
        g.skipped.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    g.inputs_fresh = 0;
    if(f.engine_frame && !g.last_in.camera_free && g.last_in.engine_frame!=f.engine_frame) { // Route B has no engine frame to match
        g.need_reset=1; g.active.store(0); g.skipped.fetch_add(1);
        set_reason("Frame Generation is paused: camera and displayed frame do not match.");
        return 0;
    }
    // Argon audit #3: DLSS-G needs the real camera. Never feed zero (singular)
    // matrices / zero near-far-FOV; without a validated camera, do not generate.
    if (!g.last_in.camera_valid && !g.last_in.camera_free) {
        g.need_reset = 1; g.active.store(0); g.skipped.fetch_add(1);
        set_reason("Frame Generation is paused: the game's camera is not available for this frame.");
        return 0;
    }
#if defined(NA_HAVE_NGX_SDK)
    if (!ensure_feature_locked(list, f)) return 0;
    if (g.feature_pending) { // created on this frame's list; the runtime needs it executed first
        g.feature_pending = 0;
        set_reason("Frame Generation is starting.");
        g.skipped.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    uint32_t mult = st.fg_multiplier.load(std::memory_order_relaxed);
    if (mult < 2) mult = 2;
    if (mult > 6) mult = 6;
    uint32_t max_count = mult - 1;
    if (max_count > g.max_supported) max_count = g.max_supported;
    if (max_count > capacity) max_count = capacity;
    uint32_t count = max_count;
    float target = static_cast<float>(st.fg_target_fps.load(std::memory_order_relaxed));
    if (target <= 0.f) target = f.display_hz;
    g.target_fps.store(target);
    if (st.fg_mode.load(std::memory_order_relaxed) == 1) {
        count = ChooseCount(f.base_frame_ms, target, max_count, g.last_count);
    }
    // A72: no ceiling here. As in NVIDIA's own integration, every enabled
    // frame gets its pictures (Fixed: exactly the multiplier; Dynamic: at
    // least 2x, more to reach the target). What the display can show is the
    // game's V-Sync setting's business, exactly as for a native title:
    // V-Sync off tears above the refresh rate, V-Sync on queues and paces.
    if (count < 1) count = 1;
    for (uint32_t i = 0; i < count; ++i) {
        if (!evaluate_locked(list, f, outputs[i], count, i + 1)) return i; // latched; earlier outputs stay usable
    }
    g.need_reset=0;
    g.last_count = count;
    g.generated.fetch_add(count, std::memory_order_relaxed);
    g.active.store(1, std::memory_order_release);
    char why[112];
    snprintf(why, sizeof(why), "Frame Generation is active: %ux (%u picture%s per rendered frame%s).", count + 1, count, count == 1 ? "" : "s",
             f.hdr ? ", HDR picture" : "");
    set_reason(why);
    return count;
#else
    (void)capacity;
    latch("Frame Generation needs a build with the NVIDIA SDK.");
    return 0;
#endif
}

Stats GetStats()
{
    Stats s{};
    const na::settings::Settings& st = na::settings::Get();
    s.runtime_present = HasRuntime() ? 1u : 0u;
    s.enabled = st.fg_enabled.load(std::memory_order_relaxed);
    s.active = g.active.load(std::memory_order_acquire);
    s.latched_off = g.latched.load(std::memory_order_acquire);
    s.mode = st.fg_mode.load(std::memory_order_relaxed);
    s.multiplier = st.fg_multiplier.load(std::memory_order_relaxed);
    s.evaluates = g.evaluates.load(std::memory_order_relaxed);
    s.generated = g.generated.load(std::memory_order_relaxed);
    s.real_frames = g.real_frames.load(std::memory_order_relaxed);
    s.skipped = g.skipped.load(std::memory_order_relaxed);
    s.inputs_recorded = g.inputs_recorded.load(); s.inputs_submitted = g.inputs_submitted.load();
    s.inputs_overwritten = g.inputs_overwritten.load(); s.missing_inputs = g.missing_inputs.load();
    s.target_fps = g.target_fps.load();
    s.resets_cut = g.resets_cut.load();
    s.resets_other = g.resets_other.load();
    if (g.m.try_lock()) {
        s.max_supported = g.max_supported;
        s.last_count = g.last_count;
        s.width = g.feat_w;
        s.height = g.feat_h;
        strncpy_s(s.reason, g.reason, _TRUNCATE);
        g.m.unlock();
    } else {
        strncpy_s(s.reason, "Frame Generation is busy.", _TRUNCATE);
    }
    if (!s.enabled) strncpy_s(s.reason, "Frame Generation is off.", _TRUNCATE);
    return s;
}

void SetRuntimeFolder(const wchar_t* folder)
{
    std::lock_guard<std::mutex> lock(g.file_m);
    wcsncpy_s(g.folder, folder ? folder : L"", _TRUNCATE);
    size_t n = wcslen(g.folder);
    while (n && (g.folder[n - 1] == L'\\' || g.folder[n - 1] == L'/')) g.folder[--n] = 0;
}

RuntimeFile GetRuntimeFile()
{
    std::lock_guard<std::mutex> lock(g.file_m);
    return g.file;
}

bool Available(char* reason, size_t reason_size)
{
    if (!reason || !reason_size) return false;
    reason[0] = 0;
    std::lock_guard<std::mutex> lock(g.m);
    if (g.latched.load(std::memory_order_acquire)) { strncpy_s(reason, reason_size, g.reason, _TRUNCATE); return false; }
#if defined(NA_HAVE_NGX_SDK)
    if (!HasRuntime()) {
        if (!check_folder_file(reason, reason_size)) return false;
        snprintf(reason, reason_size, "Frame Generation is waiting for DLSS to start (it starts with the first DLAA frame).");
        return false;
    }
    return capability_locked(reason, reason_size, nullptr);
#else
    snprintf(reason, reason_size, "This build has no NVIDIA Frame Generation support.");
    return false;
#endif
}

void ResetLatch()
{
    std::lock_guard<std::mutex> lock(g.m);
    g.latched.store(0, std::memory_order_release);
    g.need_reset = 1;
    set_reason("Frame Generation is starting.");
}

void Shutdown()
{
    std::lock_guard<std::mutex> lock(g.m);
    std::lock_guard<std::mutex> inputs(g.input_m);
    release_feature_locked();
    drain_retired_locked(true); // the caller (presenter resize / stop) has waited for the GPU
    if (g.params && g.rt.destroy_parameters) reinterpret_cast<PfnDestroyParams>(g.rt.destroy_parameters)(g.params);
    g.params = nullptr;
    g.depth_copy.Reset();
    g.motion_copy.Reset();
    g.recording_depth.Reset(); g.recording_motion.Reset();
    for (auto& p : g.pending) p = {};
    g.ready = {};
    g.pending_count.store(0);
    g.device.Reset();
    g.have_inputs = 0;
    g.inputs_fresh = 0;
    g.in_w = g.in_h = 0;
    g.feat_w = g.feat_h = g.feat_rw = g.feat_rh = 0;
    g.feat_format = DXGI_FORMAT_UNKNOWN;
    g.feat_hudless = 0;
    g.feat_ui = 0;
    g.last_count = 0;
    g.active.store(0, std::memory_order_release);
}

} // namespace na::fg
