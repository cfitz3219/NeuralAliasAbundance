// Offline proof for Frame Generation on a game that never asked for it: a saved na-j2-capture-1 capture
// (Route B, our DLAA inside the game's own list) is fed to NVIDIA DLSS Frame Generation (NGX feature 11)
// through OUR module (src/host_dx12/fg/frame_gen.cpp: OnEvaluate -> Execute -> Generate), from our own
// NGX instance, and the generated pictures are saved.
//
//   fg_capture_replay <capture folder> <out folder> <first> <last> <camera> <hud mode 0..5> [multiplier 2|3] [lut folder]
//
//   <first>..<last>  consecutive capture indices, each one real frame (index j: present plane j as the back
//                    buffer; depth and motion planes j). The first frame creates the feature, the second
//                    starts it (reset); generated pictures between j-1 and j are saved from the third on.
//   <camera>         a50 | a30 | a90: a static plausible camera (perspective, vertical field of view 50/30/90
//                    degrees, aspect from the capture, near 0.1, far 1000, reversed depth, view = identity,
//                    clip-to-previous = identity; the motion already holds the camera's motion).
//                    zero: every camera field zero, passed as valid (the module's camera gate is bypassed
//                    here only) to see what the runtime does with no camera.
//   <hud mode>       0 none. 1: the HUD-free picture of frame j is given as NVSDK_NGX_DLSSG_Parameter_HUDLess. It is
//                    the capture's hud_sheet_before_prev plane of index j+1 (the sheet of the frame before
//                    that evaluation) through a 256-level lookup per channel (<lut folder>\lut-NNNNN.txt: three
//                    lines of 256 values, linear, made offline from the same pair of planes).
//                    2: HUD-less colour + UI alpha (the exact HUD mask: shown vs HUD-free beyond 4/255 after
//                    value^(1/2.2), grown 1 px, as 0/1) with NVIDIA's UI recomposition on. 3: HUD-less colour +
//                    UI colour (shown inside the mask, alpha 1, pre-multiplied) with recomposition.
//                    4: the HUD-free picture IS the back buffer (NVIDIA gets no HUD; paste-back is offline).
//                    5: the live T2 path: as 4, with the module's camera-free input (camera "zero") and the
//                    real frame's HUD pasted on the GPU by the live pass (fg/hud_paste.cpp).
//   [multiplier]     2 (default): one generated picture per real frame; 3: two, when the runtime allows it.
//
// NGX search: the runtime looks for nvngx_dlssg.dll beside this executable and in the driver store. The
// environment variable NA_FG_SEARCH may name ONE extra folder, searched read-only (nothing is copied).
// The NGX log goes to <out folder>\ngx.log; the loaded nvngx_dlssg.dll's path and version are logged.
//
// Out: gen-<camera>-h<0|1>-x<m>-<j>-<k>.ppm (8-bit, display-mapped: value^(1/2.2), clamped) per generated
// picture, and fg.log with the GPU time of each Generate (timestamps around the generated pictures only).
// A measuring tool: it reads files and runs NVIDIA's runtime on this machine's card. It touches no game,
// install or setting.
#include "test_support.hpp"
#include "host_dx12/fg/frame_gen.hpp"
#include "host_dx12/fg/hud_paste.hpp"
#include "host_dx12/fg/steady_pass.hpp"
#include "core/fold_shading.hpp"
#include <algorithm>
#include "host_dx12/settings.hpp"



#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(NA_HAVE_NGX_SDK) && defined(NA_HAVE_NGX_LIB)
#pragma warning(push, 3)
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_defs_dlssg.h>
#include <nvsdk_ngx_params.h>
#pragma warning(pop)
#endif

namespace {
FILE* g_log = nullptr;
FILE* g_ngx_log = nullptr;
void Log(const char* fmt, ...)
{
    char line[1024];
    va_list a;
    va_start(a, fmt);
    vsnprintf(line, sizeof line, fmt, a);
    va_end(a);
    std::printf("%s\n", line);
    std::fflush(stdout);
    if (g_log) { std::fprintf(g_log, "%s\n", line); std::fflush(g_log); }
}

// ---- capture reading (flat metadata of our own writer) ----
bool ReadText(const std::string& path, std::string* out)
{
    FILE* fp = nullptr;
    if (fopen_s(&fp, path.c_str(), "rb") != 0 || !fp) return false;
    char b[4096];
    size_t n = 0;
    while ((n = std::fread(b, 1, sizeof b, fp)) > 0) out->append(b, n);
    std::fclose(fp);
    return true;
}
size_t ValueAt(const std::string& s, const char* key, size_t from)
{
    const std::string q = std::string("\"") + key + "\"";
    size_t p = s.find(q, from);
    if (p == std::string::npos) return p;
    p = s.find(':', p + q.size());
    if (p == std::string::npos) return p;
    ++p;
    while (p < s.size() && (s[p] == ' ' || s[p] == '[' || s[p] == '"')) ++p;
    return p;
}
double Num(const std::string& s, const char* key, double fallback, size_t from = 0)
{
    const size_t p = ValueAt(s, key, from);
    if (p == std::string::npos) return fallback;
    char* end = nullptr;
    const double v = std::strtod(s.c_str() + p, &end);
    return end == s.c_str() + p ? fallback : v;
}
void Pair(const std::string& s, const char* key, float* a, float* b)
{
    const size_t p = ValueAt(s, key, 0);
    if (p == std::string::npos) return;
    char* e = nullptr;
    *a = static_cast<float>(std::strtod(s.c_str() + p, &e));
    while (*e == ' ' || *e == ',') ++e;
    *b = static_cast<float>(std::strtod(e, nullptr));
}
struct PlaneInfo { bool valid = false; uint32_t w = 0, h = 0, format = 0, pitch = 0; };
PlaneInfo Plane(const std::string& s, const char* name)
{
    PlaneInfo p;
    const size_t at = s.find(std::string("{\"name\": \"") + name + "\"");
    if (at == std::string::npos) return p;
    p.w = static_cast<uint32_t>(Num(s, "width", 0, at));
    p.h = static_cast<uint32_t>(Num(s, "height", 0, at));
    p.format = static_cast<uint32_t>(Num(s, "dxgi_format", 0, at));
    p.pitch = static_cast<uint32_t>(Num(s, "row_pitch", 0, at));
    p.valid = p.w && p.h && p.format && p.pitch;
    return p;
}
struct Meta {
    std::string text;
    uint64_t ordinal = 0;
    float jitter_x = 0, jitter_y = 0, mv_x = 1, mv_y = 1;
    bool depth_inverted = true, mv_jittered = false;
    PlaneInfo depth, motion, present, sheet;
};
bool ReadMeta(const std::string& dir, uint32_t i, Meta* m)
{
    char n[64];
    snprintf(n, sizeof n, "\\%05u-meta.json", i);
    if (!ReadText(dir + n, &m->text)) return false;
    m->ordinal = static_cast<uint64_t>(Num(m->text, "present_ordinal", 0));
    Pair(m->text, "jitter_px", &m->jitter_x, &m->jitter_y);
    Pair(m->text, "mv_scale", &m->mv_x, &m->mv_y);
    m->depth_inverted = m->text.find("\"depth_inverted\": true") != std::string::npos;
    m->mv_jittered = m->text.find("\"mv_jittered\": true") != std::string::npos;
    m->depth = Plane(m->text, "depth");
    m->motion = Plane(m->text, "motion");
    m->present = Plane(m->text, "present");
    m->sheet = Plane(m->text, "hud_sheet_before_prev");
    return m->depth.valid && m->motion.valid && m->present.valid;
}
uint32_t Bpp(uint32_t f) { return f == 10 || f == 16 ? 8 : (f == 27 || f == 28 || f == 41 || f == 34) ? 4 : 0; }
bool ReadPlane(const std::string& dir, uint32_t i, const char* name, const PlaneInfo& p, std::vector<uint8_t>* rows)
{
    const uint32_t bpp = Bpp(p.format);
    if (!p.valid || !bpp) return false;
    char f[96];
    snprintf(f, sizeof f, "\\%05u-%s.raw", i, name);
    FILE* fp = nullptr;
    if (fopen_s(&fp, (dir + f).c_str(), "rb") != 0 || !fp) return false;
    const size_t tight = static_cast<size_t>(p.w) * bpp;
    rows->assign(tight * p.h, 0);
    std::vector<uint8_t> row(p.pitch);
    bool ok = true;
    for (uint32_t y = 0; ok && y < p.h; ++y) {
        if (std::fread(row.data(), 1, p.pitch, fp) < tight) ok = false;
        else std::memcpy(rows->data() + tight * y, row.data(), tight);
    }
    std::fclose(fp);
    return ok;
}
uint16_t ToHalf(float f)
{
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int32_t e = static_cast<int32_t>((x >> 23) & 0xFF) - 127 + 15;
    const uint32_t m = x & 0x7FFFFFu;
    if (e <= 0) return static_cast<uint16_t>(sign);
    if (e >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
    return static_cast<uint16_t>(sign | (((static_cast<uint32_t>(e) << 10) | (m >> 13)) + ((m >> 12) & 1u)));
}
float FromHalf(uint16_t h)
{
    const uint32_t e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    float v = e == 0 ? std::ldexp(static_cast<float>(m), -24) : e == 31 ? 65504.f : std::ldexp(1.f + static_cast<float>(m) / 1024.f, static_cast<int>(e) - 15);
    return (h & 0x8000u) ? -v : v;
}

// ---- GPU textures ----
struct Texture {
    test::ComPtr<ID3D12Resource> tex, upload;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    uint32_t bpp = 0;
    bool filled = false;
};
bool MakeTexture(ID3D12Device* dev, uint32_t w, uint32_t h, DXGI_FORMAT f, uint32_t bpp, Texture* t)
{
    t->bpp = bpp;
    t->tex = test::CreateTexture(dev, w, h, f, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    if (!t->tex) return false;
    const D3D12_RESOURCE_DESC d = t->tex->GetDesc();
    UINT64 bytes = 0;
    dev->GetCopyableFootprints(&d, 0, 1, 0, &t->fp, nullptr, nullptr, &bytes);
    t->upload = test::CreateBuffer(dev, bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    return t->upload != nullptr;
}
bool Fill(ID3D12GraphicsCommandList* l, Texture* t, const std::vector<uint8_t>& rows)
{
    constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const size_t tight = static_cast<size_t>(t->fp.Footprint.Width) * t->bpp;
    if (rows.size() != tight * t->fp.Footprint.Height) return false;
    uint8_t* p = nullptr;
    if (FAILED(t->upload->Map(0, nullptr, reinterpret_cast<void**>(&p)))) return false;
    for (UINT y = 0; y < t->fp.Footprint.Height; ++y) std::memcpy(p + static_cast<size_t>(y) * t->fp.Footprint.RowPitch, rows.data() + tight * y, tight);
    t->upload->Unmap(0, nullptr);
    if (t->filled) {
        const auto b = test::Transition(t->tex.Get(), read, D3D12_RESOURCE_STATE_COPY_DEST);
        l->ResourceBarrier(1, &b);
    }
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = t->upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = t->fp;
    dst.pResource = t->tex.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const auto b = test::Transition(t->tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, read);
    l->ResourceBarrier(1, &b);
    t->filled = true;
    return true;
}

// ---- camera ----
void Inverse(const float* m, float* out)
{
    double a[4][8];
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 8; ++c) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r ? 1 : 0);
    for (int c = 0; c < 4; ++c) {
        int piv = c;
        for (int r = c + 1; r < 4; ++r) if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) piv = r;
        for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[piv][k]);
        const double d = a[c][c];
        if (std::fabs(d) < 1e-12) { std::memset(out, 0, 64); return; }
        for (int k = 0; k < 8; ++k) a[c][k] /= d;
        for (int r = 0; r < 4; ++r) if (r != c) { const double f = a[r][c]; for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k]; }
    }
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) out[r * 4 + c] = static_cast<float>(a[r][c + 4]);
}
na::fg::Camera MakeCamera(float fov_deg, float aspect, bool inverted)
{
    na::fg::Camera c{};
    const float n = 0.1f, f = 1000.f, fov = fov_deg * 3.14159265f / 180.f, ys = 1.f / std::tan(fov * 0.5f), xs = ys / aspect;
    // Row-vector (D3D) perspective; reversed depth maps near to 1 and far to 0.
    float* p = c.view_to_clip;
    p[0] = xs; p[5] = ys; p[11] = 1;
    if (inverted) { p[10] = n / (n - f); p[14] = -f * n / (n - f); }
    else { p[10] = f / (f - n); p[14] = -n * f / (f - n); }
    Inverse(c.view_to_clip, c.clip_to_view);
    for (int i = 0; i < 4; ++i) c.clip_to_lens[i * 5] = c.clip_to_previous[i * 5] = c.previous_to_clip[i * 5] = 1;
    c.motion_scale[0] = c.motion_scale[1] = 1;
    c.up[1] = 1; c.right[0] = 1; c.forward[2] = 1;
    c.near_plane = n; c.far_plane = f; c.fov = fov; c.aspect = aspect;
    c.invalid_motion = 0;
    c.inverted = inverted ? 1 : 0;
    c.camera_motion = 1;
    return c;
}

void STDMETHODCALLTYPE Forward(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* l) { q->ExecuteCommandLists(n, l); }

#if defined(NA_HAVE_NGX_SDK) && defined(NA_HAVE_NGX_LIB)
void NVSDK_CONV NgxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
{
    if (g_ngx_log && message) { std::fputs(message, g_ngx_log); std::fflush(g_ngx_log); }
}
#endif
void LogLoaded(const char* module)
{
    HMODULE m = GetModuleHandleA(module);
    if (!m) { Log("%s: not loaded in this process", module); return; }
    char path[MAX_PATH] = {};
    GetModuleFileNameA(m, path, MAX_PATH);
    DWORD h = 0, size = GetFileVersionInfoSizeA(path, &h);
    std::vector<uint8_t> info(size ? size : 1);
    VS_FIXEDFILEINFO* v = nullptr;
    UINT vl = 0;
    if (size && GetFileVersionInfoA(path, 0, size, info.data()) && VerQueryValueA(info.data(), "\\", reinterpret_cast<void**>(&v), &vl) && v)
        Log("%s loaded from %s, version %u.%u.%u.%u", module, path, HIWORD(v->dwFileVersionMS), LOWORD(v->dwFileVersionMS), HIWORD(v->dwFileVersionLS), LOWORD(v->dwFileVersionLS));
    else Log("%s loaded from %s (no version information)", module, path);
}

bool SavePpm(const std::string& path, const std::vector<uint8_t>& rows, uint32_t pitch, uint32_t w, uint32_t h)
{
    FILE* fp = nullptr;
    if (fopen_s(&fp, path.c_str(), "wb") != 0 || !fp) return false;
    std::fprintf(fp, "P6\n%u %u\n255\n", w, h);
    std::vector<uint8_t> line(static_cast<size_t>(w) * 3);
    for (uint32_t y = 0; y < h; ++y) {
        const uint16_t* s = reinterpret_cast<const uint16_t*>(rows.data() + static_cast<size_t>(y) * pitch);
        for (uint32_t x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c) {
                float v = FromHalf(s[x * 4 + c]);
                v = !(v > 0.f) ? 0.f : std::pow(v, 1.f / 2.2f);
                line[x * 3 + c] = static_cast<uint8_t>(v >= 1.f ? 255 : static_cast<int>(v * 255.f + 0.5f));
            }
        std::fwrite(line.data(), 1, line.size(), fp);
    }
    std::fclose(fp);
    return true;
}
bool ReadLut(const std::string& path, float lut[3][256])
{
    FILE* fp = nullptr;
    if (fopen_s(&fp, path.c_str(), "rb") != 0 || !fp) return false;
    bool ok = true;
    for (int c = 0; c < 3 && ok; ++c) for (int i = 0; i < 256 && ok; ++i) ok = fscanf_s(fp, "%f", &lut[c][i]) == 1;
    std::fclose(fp);
    return ok;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 7) {
        std::printf("usage: fg_capture_replay <capture> <out> <first> <last> <a50|a30|a90|zero> <hud mode 0..5> [multiplier 2|3] [lut folder]\n");
        return 2;
    }
    const std::string cap = argv[1], out = argv[2], cam_name = argv[5];
    const uint32_t first = static_cast<uint32_t>(std::atoi(argv[3])), last = static_cast<uint32_t>(std::atoi(argv[4]));
    // HUD mode: 0 none, 1 HUD-less colour, 2 + UI alpha (exact mask, 0/1) with NVIDIA's UI recomposition,
    // 3 + UI colour (pre-multiplied: shown inside the mask, alpha = mask) with recomposition,
    // 4 the HUD-free picture AS the back buffer (NVIDIA never sees a HUD; paste-back is done offline).
    const int hud_mode = std::atoi(argv[6]);
    const bool use_hudless = hud_mode != 0;
    const uint32_t mult = argc > 7 ? static_cast<uint32_t>(std::atoi(argv[7])) : 2u;
    const std::string lut_dir = argc > 8 ? argv[8] : out;
    CreateDirectoryA(out.c_str(), nullptr);
    char tag[64];
    {
        char tag_env[24] = {};
        GetEnvironmentVariableA("NA_FG_TAG", tag_env, sizeof tag_env);
        snprintf(tag, sizeof tag, "%s-h%d-x%u%s%s", cam_name.c_str(), hud_mode, mult, tag_env[0] ? "-" : "", tag_env);
    }
    fopen_s(&g_log, (out + "\\fg-" + tag + ".log").c_str(), "wb");
    fopen_s(&g_ngx_log, (out + "\\ngx-" + tag + ".log").c_str(), "wb");
    Log("fg_capture_replay %s frames %u..%u camera %s hudless %d multiplier %u", cap.c_str(), first, last, cam_name.c_str(), use_hudless ? 1 : 0, mult);
#if !(defined(NA_HAVE_NGX_SDK) && defined(NA_HAVE_NGX_LIB))
    Log("SKIP: this build has no NVIDIA NGX library (own instance not possible)");
    return 77;
#else
    test::Device d;
    if (!test::CreateDevice(d, false)) { Log("no hardware D3D12 device"); return 77; }
    // Our own NGX instance, with NGX's log routed to ngx-*.log and one optional extra read-only search folder.
    NVSDK_NGX_FeatureCommonInfo info{};
    wchar_t extra[MAX_PATH] = {};
    const wchar_t* paths[1] = {extra};
    if (GetEnvironmentVariableW(L"NA_FG_SEARCH", extra, MAX_PATH)) {
        info.PathListInfo.Path = paths;
        info.PathListInfo.Length = 1;
        Log("extra NGX search folder: %ls", extra);
    }
    info.LoggingInfo.LoggingCallback = NgxLog;
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    info.LoggingInfo.DisableOtherLoggingSinks = true;
    const std::string data = out + "\\ngx-data";
    CreateDirectoryA(data.c_str(), nullptr);
    const std::wstring wdata(data.begin(), data.end());
    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID("a0f57b54-1daf-4934-90ae-c4035c19df04", NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                              "NeuralAliasAbundance", wdata.c_str(), d.device.Get(), &info);
    Log("NGX init: 0x%08X", static_cast<unsigned>(r));
    if (NVSDK_NGX_FAILED(r)) return 1;
    {
        NVSDK_NGX_Parameter* capp = nullptr;
        r = NVSDK_NGX_D3D12_GetCapabilityParameters(&capp);
        int avail = 0, needs = 0, init = 0;
        unsigned mx = 0, maj = 0, mnr = 0;
        if (NVSDK_NGX_SUCCEED(r) && capp) {
            capp->Get(NVSDK_NGX_Parameter_FrameGeneration_Available, &avail);
            capp->Get(NVSDK_NGX_Parameter_FrameGeneration_NeedsUpdatedDriver, &needs);
            capp->Get(NVSDK_NGX_Parameter_FrameGeneration_FeatureInitResult, &init);
            capp->Get(NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMajor, &maj);
            capp->Get(NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMinor, &mnr);
            capp->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &mx);
        }
        Log("capabilities 0x%08X: FrameGeneration.Available=%d NeedsUpdatedDriver=%d FeatureInitResult=0x%08X MinDriver=%u.%u MultiFrameCountMax=%u",
            static_cast<unsigned>(r), avail, needs, static_cast<unsigned>(init), maj, mnr, mx);
    }
    using CreateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
    na::fg::Runtime rt;
    rt.create_feature = reinterpret_cast<void*>(static_cast<CreateFn>(&NVSDK_NGX_D3D12_CreateFeature));
    rt.evaluate_feature = reinterpret_cast<void*>(&NVSDK_NGX_D3D12_EvaluateFeature_C);
    rt.release_feature = reinterpret_cast<void*>(&NVSDK_NGX_D3D12_ReleaseFeature);
    rt.allocate_parameters = reinterpret_cast<void*>(&NVSDK_NGX_D3D12_AllocateParameters);
    rt.destroy_parameters = reinterpret_cast<void*>(&NVSDK_NGX_D3D12_DestroyParameters);
    rt.get_capability_parameters = reinterpret_cast<void*>(&NVSDK_NGX_D3D12_GetCapabilityParameters);
    na::fg::SetRuntime(rt);
    auto& st = na::settings::Get();
    st.fg_enabled.store(1);
    st.fg_mode.store(0); // fixed: exactly multiplier - 1 pictures
    st.fg_multiplier.store(mult);
    st.fg_target_fps.store(0);

    Meta m0;
    if (!ReadMeta(cap, first, &m0)) { Log("cannot read meta %u", first); return 1; }
    const uint32_t W = m0.present.w, H = m0.present.h, rw = m0.depth.w, rh = m0.depth.h;
    Log("back buffer %ux%u format %u; depth %ux%u format %u; motion %ux%u format %u", W, H, m0.present.format, rw, rh, m0.depth.format,
        m0.motion.w, m0.motion.h, m0.motion.format);
    if (m0.present.format != 10 || m0.depth.format != 41 || (m0.motion.format != 16 && m0.motion.format != 34)) { Log("unexpected plane formats"); return 1; }
    const float aspect = static_cast<float>(W) / static_cast<float>(H);
    na::fg::Camera cam{};
    const bool zero_cam = cam_name == "zero";
    if (!zero_cam) cam = MakeCamera(static_cast<float>(std::atof(cam_name.c_str() + 1)), aspect, m0.depth_inverted);
    {   // NA_FG_CAMMOTION=1: the motion INCLUDES the camera's motion (CameraMotionIncluded=1) even with the zero camera
        char cm[8] = {};
        if (GetEnvironmentVariableA("NA_FG_CAMMOTION", cm, sizeof cm) && cm[0] == '1') cam.camera_motion = 1;
        Log("camera motion included in the motion: %u", cam.camera_motion);
        char iv[32] = {};
        if (GetEnvironmentVariableA("NA_FG_INVALID", iv, sizeof iv)) cam.invalid_motion = static_cast<float>(std::atof(iv)); // the value meaning "no motion known"
        if (GetEnvironmentVariableA("NA_FG_DILATED", iv, sizeof iv) && iv[0] == '1') cam.dilated = 1;
        Log("motion invalid value %g, dilated %u", cam.invalid_motion, cam.dilated);
    }
    Log("camera %s: fov %.1f deg aspect %.4f near %.2f far %.0f inverted %u, matrices pair-check %d", cam_name.c_str(), cam.fov * 57.29578f, cam.aspect,
        cam.near_plane, cam.far_plane, cam.inverted, na::fg::InversePair(cam.view_to_clip, cam.clip_to_view) ? 1 : 0);

    // Live-input variants (2026-11-26, the shimmer study), by environment so the argument list stays:
    //   NA_FG_FOLD=performance  depth and motion as the live Performance level hands them to Frame Generation:
    //                           half size, the last pixel of each 2x2 block, jitter declared for that pixel
    //                           (core/fold_shading.hpp), motion in folded pixels.
    //   NA_FG_MOTION_DIR=DIR    motion from DIR\NNNNN-motion.raw (float2, tight rows, render size of this run;
    //                           flow_motion_dump --fold-input performance writes the folded ones, folded pixels)
    //   NA_FG_ZERO_MOTION=1     all motion zero (what NVIDIA does with no motion from us)
    //   NA_FG_TAG=text          appended to the output names
    char env[512] = {};
    const bool fold_perf = GetEnvironmentVariableA("NA_FG_FOLD", env, sizeof env) && !std::strcmp(env, "performance");
    std::string motion_dir;
    if (GetEnvironmentVariableA("NA_FG_MOTION_DIR", env, sizeof env)) motion_dir = env;
    const bool zero_motion = GetEnvironmentVariableA("NA_FG_ZERO_MOTION", env, sizeof env) && env[0] == '1';
    const uint32_t iw = fold_perf ? (rw + 1) / 2 : rw, ih = fold_perf ? (rh + 1) / 2 : rh;
    Log("inputs: %s depth %ux%u, motion %s", fold_perf ? "folded (Performance, kept pixel 1,1)" : "full-size", iw, ih,
        zero_motion ? "zero" : motion_dir.empty() ? (fold_perf ? "capture motion at the kept pixel" : "capture motion") : motion_dir.c_str());
    Texture depth, motion, color, hudless;
    if (!MakeTexture(d.device.Get(), iw, ih, DXGI_FORMAT_R32_FLOAT, 4, &depth) ||
        !MakeTexture(d.device.Get(), iw, ih, DXGI_FORMAT_R32G32_FLOAT, 8, &motion) ||
        !MakeTexture(d.device.Get(), W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, &color) ||
        !MakeTexture(d.device.Get(), W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, &hudless)) { Log("texture creation failed"); return 1; }
    // UI inputs for NVIDIA's recomposition (SDK: UI = four channels, colour pre-multiplied by alpha;
    // UIAlpha = one channel 0..1). Created only for the modes that use them.
    Texture ui_alpha, ui_tex;
    if (hud_mode == 2 && !MakeTexture(d.device.Get(), W, H, DXGI_FORMAT_R8_UNORM, 1, &ui_alpha)) { Log("UI alpha texture failed"); return 1; }
    if (hud_mode == 3 && !MakeTexture(d.device.Get(), W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, &ui_tex)) { Log("UI texture failed"); return 1; }
    const uint32_t kOut = 2;
    test::ComPtr<ID3D12Resource> outs[kOut], readback[kOut];
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT ofp{};
    UINT64 obytes = 0;
    for (uint32_t i = 0; i < kOut; ++i) {
        outs[i] = test::CreateTexture(d.device.Get(), W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        const D3D12_RESOURCE_DESC od = outs[i]->GetDesc();
        d.device->GetCopyableFootprints(&od, 0, 1, 0, &ofp, nullptr, nullptr, &obytes);
        readback[i] = test::CreateBuffer(d.device.Get(), obytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    test::ComPtr<ID3D12QueryHeap> qh;
    D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2, 0};
    d.device->CreateQueryHeap(&qd, IID_PPV_ARGS(&qh));
    auto qrb = test::CreateBuffer(d.device.Get(), 16, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    UINT64 freq = 1;
    d.queue->GetTimestampFrequency(&freq);
    test::List la, lb;
    if (!la.Create(d.device.Get()) || !lb.Create(d.device.Get())) return 1;
    la.list->Close();
    lb.list->Close();

    char steady_env[8] = {}, raw_env[8] = {};
    const bool steady_on = GetEnvironmentVariableA("NA_FG_STEADY", steady_env, sizeof steady_env) && steady_env[0] == '1';
    const bool raw_out = GetEnvironmentVariableA("NA_FG_RAW_OUT", raw_env, sizeof raw_env) && raw_env[0] == '1';
    na::fg::SteadyPass steady;
    if (steady_on) { char why[160] = {}; if (!steady.Init(d.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, why, sizeof why)) { Log("steadying: %s", why); return 1; } }
    auto color_prev = test::CreateTexture(d.device.Get(), W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_FLAG_NONE);
    bool have_prev = false;
    na::fg::HudPaste paste; // live T2 (mode 5)
    if (hud_mode == 5 && !paste.Init(d.device.Get())) { Log("HUD paste pass could not be built"); return 1; }
    float lut[3][256] = {};
    std::vector<uint8_t> rows, rows2;
    uint64_t frame_id = 0;
    double total_ms = 0;
    uint32_t timed = 0, generated_total = 0;
    // NA_FG_STEP=2: every other saved frame is a "real" frame and the skipped one in between is the truth the
    // generated picture can be compared with; the motion of a pair is the sum of its two saved planes.
    char step_env[8] = {};
    const uint32_t step = GetEnvironmentVariableA("NA_FG_STEP", step_env, sizeof step_env) && step_env[0] == '2' ? 2u : 1u;
    for (uint32_t j = first; j <= last; j += step) {
        Meta m;
        if (!ReadMeta(cap, j, &m)) { Log("cannot read meta %u", j); return 1; }
        // Inputs at the "evaluate": depth and motion of frame j.
        la.Reset();
        std::vector<uint8_t> drows, mrows;
        if (!ReadPlane(cap, j, "depth", m.depth, &drows) || !ReadPlane(cap, j, "motion", m.motion, &mrows) || m.motion.format != 16) { Log("frame %u: depth/motion not readable", j); return 1; }
        if (step == 2 && j > first) { // pair (j-2 -> j): add the skipped frame's motion
            std::vector<uint8_t> prev;
            Meta mp;
            if (!ReadMeta(cap, j - 1, &mp) || !ReadPlane(cap, j - 1, "motion", mp.motion, &prev) || prev.size() != mrows.size()) { Log("frame %u: motion of %u not readable", j, j - 1); return 1; }
            for (size_t k = 0; k + 4 <= mrows.size(); k += 4) { float u, v; std::memcpy(&u, &mrows[k], 4); std::memcpy(&v, &prev[k], 4); u += v; std::memcpy(&mrows[k], &u, 4); }
        }
        float mv_div = 1.f; // capture motion is in full-size pixels; folded pixels are half of that
        if (fold_perf) {
            std::vector<uint8_t> fd(static_cast<size_t>(iw) * ih * 4), fm(static_cast<size_t>(iw) * ih * 8);
            for (uint32_t y = 0; y < ih; ++y)
                for (uint32_t x = 0; x < iw; ++x) {
                    const size_t sx = (std::min)(2u * x + 1u, rw - 1), sy = (std::min)(2u * y + 1u, rh - 1);
                    std::memcpy(&fd[(static_cast<size_t>(y) * iw + x) * 4], &drows[(sy * rw + sx) * 4], 4);
                    std::memcpy(&fm[(static_cast<size_t>(y) * iw + x) * 8], &mrows[(sy * m.motion.w + sx) * 8], 8);
                }
            drows.swap(fd); mrows.swap(fm); mv_div = 2.f;
        }
        if (!motion_dir.empty()) {
            char mf[64];
            snprintf(mf, sizeof mf, "\\%05u-motion.raw", j);
            FILE* fp = nullptr;
            std::vector<uint8_t> given(static_cast<size_t>(iw) * ih * 8);
            if (fopen_s(&fp, (motion_dir + mf).c_str(), "rb") != 0 || !fp || std::fread(given.data(), 1, given.size(), fp) != given.size()) {
                if (fp) std::fclose(fp);
                Log("frame %u: motion plane %s%s not readable at %ux%u", j, motion_dir.c_str(), mf, iw, ih); return 1;
            }
            std::fclose(fp);
            mrows.swap(given); mv_div = 1.f; // already in this run's pixels
        }
        if (zero_motion) std::fill(mrows.begin(), mrows.end(), static_cast<uint8_t>(0));
        {   // NA_FG_CONST_MOTION=px: every vector (px, 0) in this run's pixels: does the runtime follow our motion at all?
            char cmv[16] = {};
            if (GetEnvironmentVariableA("NA_FG_CONST_MOTION", cmv, sizeof cmv)) {
                const float v = static_cast<float>(std::atof(cmv)) * mv_div, z = 0.f;
                for (size_t k = 0; k + 8 <= mrows.size(); k += 8) { std::memcpy(&mrows[k], &v, 4); std::memcpy(&mrows[k + 4], &z, 4); }
            }
        }
        if (mv_div != 1.f)
            for (size_t k = 0; k + 4 <= mrows.size(); k += 4) { float v; std::memcpy(&v, &mrows[k], 4); v /= mv_div; std::memcpy(&mrows[k], &v, 4); }
        {   // NA_FG_DEADZONE=px: vectors shorter than this (render pixels) become exactly zero
            char dz[16] = {};
            if (GetEnvironmentVariableA("NA_FG_DEADZONE", dz, sizeof dz)) {
                const float lim = static_cast<float>(std::atof(dz));
                for (size_t k = 0; k + 8 <= mrows.size(); k += 8) {
                    float u, v; std::memcpy(&u, &mrows[k], 4); std::memcpy(&v, &mrows[k + 4], 4);
                    if (u * u + v * v < lim * lim) { u = v = 0.f; std::memcpy(&mrows[k], &u, 4); std::memcpy(&mrows[k + 4], &v, 4); }
                }
            }
        }
        if (!Fill(la.list.Get(), &depth, drows) || !Fill(la.list.Get(), &motion, mrows)) { Log("frame %u: depth/motion not filled", j); return 1; }
        na::fg::EvalInputs in{};
        in.engine_frame = m.ordinal;
        in.camera = cam;
        in.camera_valid = hud_mode != 5; // the zero variant is passed as valid on purpose (tool only)
        in.camera_free = hud_mode == 5;  // live T2: the module's own Route B camera-free path
        in.depth = depth.tex.Get();
        in.motion = motion.tex.Get();
        in.render_w = iw; in.render_h = ih; in.out_w = W; in.out_h = H;
        in.jitter_x = fold_perf ? na::fold::DeclaredJitter(2, 1, m.jitter_x) : m.jitter_x;
        in.jitter_y = fold_perf ? na::fold::DeclaredJitter(2, 1, m.jitter_y) : m.jitter_y;
        in.mv_scale_x = m.mv_x; in.mv_scale_y = m.mv_y;
        {   // NA_FG_MVSCALE_PIXELS=1: hand the runtime a scale that leaves the vectors in pixels (MvecScale = 1)
            char ms[8] = {};
            if (GetEnvironmentVariableA("NA_FG_MVSCALE_PIXELS", ms, sizeof ms) && ms[0] == '1') { in.mv_scale_x *= static_cast<float>(iw); in.mv_scale_y *= static_cast<float>(ih); }
        }
        {   // NA_FG_PXSCALE=kx,ky: MvecScale handed to the runtime = (kx, ky) times "render-size pixels"
            char ps[48] = {};
            if (GetEnvironmentVariableA("NA_FG_PXSCALE", ps, sizeof ps)) {
                float kx = 1.f, ky = 1.f;
                if (sscanf_s(ps, "%f,%f", &kx, &ky) == 2) { in.mv_scale_x = m.mv_x * kx * static_cast<float>(iw); in.mv_scale_y = m.mv_y * ky * static_cast<float>(ih); }
            }
        }
        in.mv_low_res = true;
        in.reset = j == first ? 1u : 0u;
        in.depth_inverted = m.depth_inverted ? 1u : 0u;
        in.mv_jittered = m.mv_jittered ? 1u : 0u;
        na::fg::OnEvaluate(la.list.Get(), in);
        la.list->Close();
        ID3D12CommandList* al[] = {la.list.Get()};
        na::fg::Execute(d.queue.Get(), 1, al, Forward);
        // The real frame and (optionally) its HUD-free picture.
        lb.Reset();
        {   // NA_FG_BB_PLANE=name: another saved plane as the back buffer (e.g. dlssout, before our sharpening and the
            // game's post); NA_FG_BB_DIR=DIR: DIR\NNNNN-bb.raw (RGBA16F, tight rows) made offline.
            char bbp[32] = {}, bbd[400] = {};
            const bool by_dir = GetEnvironmentVariableA("NA_FG_BB_DIR", bbd, sizeof bbd) != 0;
            const bool by_plane = !by_dir && GetEnvironmentVariableA("NA_FG_BB_PLANE", bbp, sizeof bbp) != 0;
            if (by_dir) {
                char bf[64]; snprintf(bf, sizeof bf, "\\%05u-bb.raw", j);
                FILE* fp = nullptr; rows.assign(static_cast<size_t>(W) * H * 8, 0);
                if (fopen_s(&fp, (std::string(bbd) + bf).c_str(), "rb") != 0 || !fp || std::fread(rows.data(), 1, rows.size(), fp) != rows.size()) {
                    if (fp) std::fclose(fp); Log("frame %u: back buffer file not readable", j); return 1; }
                std::fclose(fp);
            } else if (by_plane) {
                const PlaneInfo pi = Plane(m.text, bbp);
                if (!ReadPlane(cap, j, bbp, pi, &rows)) { Log("frame %u: plane %s not readable", j, bbp); return 1; }
            } else if (!ReadPlane(cap, j, "present", m.present, &rows)) { Log("frame %u: present not readable", j); return 1; }
        }
        bool have_hudless = false, have_ui = false, have_alpha = false;
        std::vector<uint8_t> hl;
        if (use_hudless) {
            Meta mn;
            char lname[64];
            snprintf(lname, sizeof lname, "\\lut-%05u.txt", j);
            if (ReadMeta(cap, j + 1, &mn) && mn.sheet.valid && ReadPlane(cap, j + 1, "hud_sheet_before_prev", mn.sheet, &rows2) && ReadLut(lut_dir + lname, lut)) {
                hl.assign(static_cast<size_t>(W) * H * 8, 0);
                uint16_t* o = reinterpret_cast<uint16_t*>(hl.data());
                for (size_t px = 0; px < static_cast<size_t>(W) * H; ++px) {
                    for (int c = 0; c < 3; ++c) o[px * 4 + c] = ToHalf(lut[c][rows2[px * 4 + c]]);
                    o[px * 4 + 3] = ToHalf(1.f);
                }
            }
            if (hl.empty()) { Log("frame %u: HUD-free picture not available (sheet of %u or %s missing)", j, j + 1, lname + 1); return 1; }
        }
        if (hud_mode == 4 || hud_mode == 5) {
            if (!Fill(lb.list.Get(), &color, hl)) return 1; // NVIDIA gets the HUD-free picture as the frame itself
            if (hud_mode == 5 && !Fill(lb.list.Get(), &hudless, rows)) return 1; // live T2: the real frame, for the paste
        } else {
            if (!Fill(lb.list.Get(), &color, rows)) return 1;
            if (use_hudless) have_hudless = Fill(lb.list.Get(), &hudless, hl);
        }
        if (hud_mode == 2 || hud_mode == 3) {
            // The exact HUD mask: shown vs HUD-free beyond 4/255 after value^(1/2.2) in any channel, grown by 1 pixel.
            const uint16_t* sp = reinterpret_cast<const uint16_t*>(rows.data());
            const uint16_t* hp = reinterpret_cast<const uint16_t*>(hl.data());
            std::vector<uint8_t> raw_mask(static_cast<size_t>(W) * H, 0), mask(static_cast<size_t>(W) * H, 0);
            auto enc = [](float v) { return v > 0.f ? std::pow(v, 1.f / 2.2f) : 0.f; };
            for (size_t px = 0; px < raw_mask.size(); ++px)
                for (int c = 0; c < 3; ++c)
                    if (std::fabs(enc(FromHalf(sp[px * 4 + c])) - enc(FromHalf(hp[px * 4 + c]))) > 4.f / 255.f) { raw_mask[px] = 1; break; }
            for (uint32_t y = 0; y < H; ++y)
                for (uint32_t x = 0; x < W; ++x) {
                    const size_t i = static_cast<size_t>(y) * W + x;
                    mask[i] = raw_mask[i] || (x && raw_mask[i - 1]) || (x + 1 < W && raw_mask[i + 1]) || (y && raw_mask[i - W]) || (y + 1 < H && raw_mask[i + W]);
                }
            if (hud_mode == 2) {
                std::vector<uint8_t> a(mask.size());
                for (size_t i = 0; i < a.size(); ++i) a[i] = mask[i] ? 255 : 0;
                have_alpha = Fill(lb.list.Get(), &ui_alpha, a);
            } else {
                std::vector<uint8_t> u(static_cast<size_t>(W) * H * 8, 0);
                uint16_t* o = reinterpret_cast<uint16_t*>(u.data());
                for (size_t i = 0; i < mask.size(); ++i) {
                    if (!mask[i]) continue; // colour pre-multiplied by alpha 0: all zero
                    for (int c = 0; c < 3; ++c) o[i * 4 + c] = sp[i * 4 + c];
                    o[i * 4 + 3] = ToHalf(1.f);
                }
                have_ui = Fill(lb.list.Get(), &ui_tex, u);
            }
            if (!have_ui && !have_alpha) { Log("frame %u: UI input not made", j); return 1; }
        }
        na::fg::FrameInfo f{};
        f.engine_frame = m.ordinal;
        f.color = color.tex.Get();
        f.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        f.width = W; f.height = H;
        f.hdr = true;
        f.frame_id = ++frame_id;
        f.queue = d.queue.Get();
        f.hudless = have_hudless && hud_mode != 5 ? hudless.tex.Get() : nullptr;
        f.ui = have_ui ? ui_tex.tex.Get() : nullptr;
        f.ui_alpha = have_alpha ? ui_alpha.tex.Get() : nullptr;
        ID3D12Resource* op[kOut] = {outs[0].Get(), outs[1].Get()};
        lb.list->EndQuery(qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        const uint32_t n = na::fg::Generate(lb.list.Get(), d.device.Get(), f, op, mult - 1 < kOut ? mult - 1 : kOut);
        // NA_FG_STEADY=1: Astra's still-area steadying (fg/steady_pass), as the live presenter runs it: on the
        // generated picture, with the previous and current back buffers and the motion in render pixels.
        // Route B (mode 5) uses the module's prepared motion (filled, no dead zone) exactly as live does.
        if (steady_on && have_prev) {
            const na::fg::UsedMotion um = na::fg::LastUsedMotion();
            ID3D12Resource* sm = um.filled ? um.filled : motion.tex.Get();
            const float sx = um.filled ? um.mv_scale_x : m.mv_x, sy = um.filled ? um.mv_scale_y : m.mv_y;
            for (uint32_t i = 0; i < n; ++i)
                if (!steady.Record(lb.list.Get(), outs[i].Get(), color_prev.Get(), color.tex.Get(), sm, W, H, sx, sy, i)) { Log("frame %u: steadying not recorded", j); return 1; }
        }
        if (hud_mode == 5) /* live T2: the real frame's HUD through its own mask, by the live pass */
            for (uint32_t i = 0; i < n; ++i)
                if (!paste.Record(lb.list.Get(), outs[i].Get(), hudless.tex.Get(), color.tex.Get(), W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, i)) { Log("frame %u: HUD paste not recorded", j); return 1; }
        lb.list->EndQuery(qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        lb.list->ResolveQueryData(qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, qrb.Get(), 0);
        {   // keep this back buffer as the next pair's older picture
            D3D12_RESOURCE_BARRIER pb[2] = {test::Transition(color.tex.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                            test::Transition(color_prev.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST)};
            lb.list->ResourceBarrier(2, pb);
            lb.list->CopyResource(color_prev.Get(), color.tex.Get());
            for (auto& x : pb) std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
            lb.list->ResourceBarrier(2, pb);
        }
        for (uint32_t i = 0; i < n; ++i) {
            const auto b1 = test::Transition(outs[i].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            lb.list->ResourceBarrier(1, &b1);
            D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
            src.pResource = outs[i].Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.pResource = readback[i].Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = ofp;
            lb.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            const auto b2 = test::Transition(outs[i].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            lb.list->ResourceBarrier(1, &b2);
        }
        if (FAILED(lb.list->Close())) { Log("frame %u: command list close failed", j); return 1; }
        ID3D12CommandList* bl[] = {lb.list.Get()};
        d.queue->ExecuteCommandLists(1, bl);
        d.WaitIdle();
        if (j == first) LogLoaded("nvngx_dlssg.dll");
        UINT64* ts = nullptr;
        double ms = 0;
        if (SUCCEEDED(qrb->Map(0, nullptr, reinterpret_cast<void**>(&ts)))) { ms = static_cast<double>(ts[1] - ts[0]) * 1000.0 / static_cast<double>(freq); qrb->Unmap(0, nullptr); }
        const na::fg::Stats s = na::fg::GetStats();
        Log("frame %u (ordinal %llu): generated %u in %.3f ms GPU | active %u latched %u max_supported %u evaluates %llu | %s", j,
            static_cast<unsigned long long>(m.ordinal), n, ms, s.active, s.latched_off, s.max_supported, static_cast<unsigned long long>(s.evaluates), s.reason);
        if (n && j >= first + 2) { total_ms += ms; ++timed; }
        generated_total += n;
        for (uint32_t i = 0; i < n; ++i) {
            uint8_t* p = nullptr;
            if (FAILED(readback[i]->Map(0, nullptr, reinterpret_cast<void**>(&p)))) continue;
            std::vector<uint8_t> o(p, p + obytes);
            readback[i]->Unmap(0, nullptr);
            char fn[160];
            snprintf(fn, sizeof fn, "\\gen-%s-%05u-%u.ppm", tag, j, i + 1);
            SavePpm(out + fn, o, ofp.Footprint.RowPitch, W, H);
            if (raw_out) { // tight RGBA16F, before any display clamp (Astra's measure.py reads these)
                snprintf(fn, sizeof fn, "\\gen-%s-%05u-%u.raw", tag, j, i + 1);
                FILE* fp = nullptr;
                if (fopen_s(&fp, (out + fn).c_str(), "wb") == 0 && fp) {
                    for (uint32_t y = 0; y < H; ++y) std::fwrite(o.data() + static_cast<size_t>(y) * ofp.Footprint.RowPitch, 1, static_cast<size_t>(W) * 8, fp);
                    std::fclose(fp);
                }
            }
        }
        have_prev = true;
        if (s.latched_off) {
            // The runtime may report a recoverable first-evaluate error ("Re-initializing DLSS-FG from recoverable
            // error state" in its log). Retry once to see whether the next frame works; a second failure stops.
            static int retries = 0;
            if (retries++ == 0) { Log("latched (%s): one retry", s.reason); na::fg::ResetLatch(); continue; }
            Log("stopped: %s", s.reason);
            break;
        }
    }
    LogLoaded("nvngx_dlssg.dll");
    LogLoaded("nvngx_dlss.dll");
    Log("summary %s: generated %u pictures; mean GPU ms per Generate (from the third frame) %.3f over %u frames", tag, generated_total,
        timed ? total_ms / timed : 0.0, timed);
    na::fg::Shutdown();
    NVSDK_NGX_D3D12_Shutdown1(d.device.Get());
    if (g_ngx_log) std::fclose(g_ngx_log);
    return generated_total ? 0 : 1;
#endif
}
