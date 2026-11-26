// Offline proof for Frame Generation on a game that never asked for it: a saved na-j2-capture-1 capture
// (Route B, our DLAA inside the game's own list) is fed to NVIDIA DLSS Frame Generation (NGX feature 11)
// through OUR module (src/host_dx12/fg/frame_gen.cpp: OnEvaluate -> Execute -> Generate), from our own
// NGX instance, and the generated pictures are saved.
//
//   fg_capture_replay <capture folder> <out folder> <first> <last> <camera> <hudless 0|1> [multiplier 2|3] [lut folder]
//
//   <first>..<last>  consecutive capture indices, each one real frame (index j: present plane j as the back
//                    buffer; depth and motion planes j). The first frame creates the feature, the second
//                    starts it (reset); generated pictures between j-1 and j are saved from the third on.
//   <camera>         a50 | a30 | a90: a static plausible camera (perspective, vertical field of view 50/30/90
//                    degrees, aspect from the capture, near 0.1, far 1000, reversed depth, view = identity,
//                    clip-to-previous = identity; the motion already holds the camera's motion).
//                    zero: every camera field zero, passed as valid (the module's camera gate is bypassed
//                    here only) to see what the runtime does with no camera.
//   <hudless>        1: the HUD-free picture of frame j is given as NVSDK_NGX_DLSSG_Parameter_HUDLess. It is
//                    the capture's hud_sheet_before_prev plane of index j+1 (the sheet of the frame before
//                    that evaluation) through a 256-level lookup per channel (<lut folder>\lut-NNNNN.txt: three
//                    lines of 256 values, linear, made offline from the same pair of planes).
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
        std::printf("usage: fg_capture_replay <capture> <out> <first> <last> <a50|a30|a90|zero> <hudless 0|1> [multiplier 2|3] [lut folder]\n");
        return 2;
    }
    const std::string cap = argv[1], out = argv[2], cam_name = argv[5];
    const uint32_t first = static_cast<uint32_t>(std::atoi(argv[3])), last = static_cast<uint32_t>(std::atoi(argv[4]));
    const bool use_hudless = std::atoi(argv[6]) != 0;
    const uint32_t mult = argc > 7 ? static_cast<uint32_t>(std::atoi(argv[7])) : 2u;
    const std::string lut_dir = argc > 8 ? argv[8] : out;
    CreateDirectoryA(out.c_str(), nullptr);
    char tag[64];
    snprintf(tag, sizeof tag, "%s-h%d-x%u", cam_name.c_str(), use_hudless ? 1 : 0, mult);
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
    Log("camera %s: fov %.1f deg aspect %.4f near %.2f far %.0f inverted %u, matrices pair-check %d", cam_name.c_str(), cam.fov * 57.29578f, cam.aspect,
        cam.near_plane, cam.far_plane, cam.inverted, na::fg::InversePair(cam.view_to_clip, cam.clip_to_view) ? 1 : 0);

    Texture depth, motion, color, hudless;
    if (!MakeTexture(d.device.Get(), rw, rh, DXGI_FORMAT_R32_FLOAT, 4, &depth) ||
        !MakeTexture(d.device.Get(), m0.motion.w, m0.motion.h, static_cast<DXGI_FORMAT>(m0.motion.format), Bpp(m0.motion.format), &motion) ||
        !MakeTexture(d.device.Get(), W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, &color) ||
        !MakeTexture(d.device.Get(), W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, &hudless)) { Log("texture creation failed"); return 1; }
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

    float lut[3][256] = {};
    std::vector<uint8_t> rows, rows2;
    uint64_t frame_id = 0;
    double total_ms = 0;
    uint32_t timed = 0, generated_total = 0;
    for (uint32_t j = first; j <= last; ++j) {
        Meta m;
        if (!ReadMeta(cap, j, &m)) { Log("cannot read meta %u", j); return 1; }
        // Inputs at the "evaluate": depth and motion of frame j.
        la.Reset();
        if (!ReadPlane(cap, j, "depth", m.depth, &rows) || !Fill(la.list.Get(), &depth, rows) ||
            !ReadPlane(cap, j, "motion", m.motion, &rows) || !Fill(la.list.Get(), &motion, rows)) { Log("frame %u: depth/motion not readable", j); return 1; }
        na::fg::EvalInputs in{};
        in.engine_frame = m.ordinal;
        in.camera = cam;
        in.camera_valid = true; // the zero variant is passed as valid on purpose (tool only)
        in.depth = depth.tex.Get();
        in.motion = motion.tex.Get();
        in.render_w = rw; in.render_h = rh; in.out_w = W; in.out_h = H;
        in.jitter_x = m.jitter_x; in.jitter_y = m.jitter_y;
        in.mv_scale_x = m.mv_x; in.mv_scale_y = m.mv_y;
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
        if (!ReadPlane(cap, j, "present", m.present, &rows) || !Fill(lb.list.Get(), &color, rows)) { Log("frame %u: present not readable", j); return 1; }
        bool have_hudless = false;
        if (use_hudless) {
            Meta mn;
            char lname[64];
            snprintf(lname, sizeof lname, "\\lut-%05u.txt", j);
            if (ReadMeta(cap, j + 1, &mn) && mn.sheet.valid && ReadPlane(cap, j + 1, "hud_sheet_before_prev", mn.sheet, &rows2) && ReadLut(lut_dir + lname, lut)) {
                std::vector<uint8_t> hl(static_cast<size_t>(W) * H * 8);
                uint16_t* o = reinterpret_cast<uint16_t*>(hl.data());
                for (size_t px = 0; px < static_cast<size_t>(W) * H; ++px) {
                    for (int c = 0; c < 3; ++c) o[px * 4 + c] = ToHalf(lut[c][rows2[px * 4 + c]]);
                    o[px * 4 + 3] = ToHalf(1.f);
                }
                have_hudless = Fill(lb.list.Get(), &hudless, hl);
            }
            if (!have_hudless) { Log("frame %u: HUD-free picture not available (sheet of %u or %s missing)", j, j + 1, lname + 1); return 1; }
        }
        na::fg::FrameInfo f{};
        f.engine_frame = m.ordinal;
        f.color = color.tex.Get();
        f.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        f.width = W; f.height = H;
        f.hdr = true;
        f.frame_id = ++frame_id;
        f.queue = d.queue.Get();
        f.hudless = have_hudless ? hudless.tex.Get() : nullptr;
        ID3D12Resource* op[kOut] = {outs[0].Get(), outs[1].Get()};
        lb.list->EndQuery(qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        const uint32_t n = na::fg::Generate(lb.list.Get(), d.device.Get(), f, op, mult - 1 < kOut ? mult - 1 : kOut);
        lb.list->EndQuery(qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        lb.list->ResolveQueryData(qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, qrb.Get(), 0);
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
        }
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
