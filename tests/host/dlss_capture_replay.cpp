// Replay a saved DLAA capture through our own NGX instance, changing ONE input at a time.
//
// Why it exists (Cort's Shadow of War run of 2026-11-18, build 2e062ef): on one screen preset K
// showed slow waves over a distant, still building and preset M did not. The saved captures hold
// what DLAA was given (color, depth, motion, the told jitter) and what it produced, but comparing
// the produced picture with a given picture that itself changes from frame to frame cannot say how
// much of the effect each input causes. A replay can: the same saved pictures go through the same
// DLSS model on the same card, once as they were and once with a single input changed, and the two
// OUTPUTS are compared with each other.
//
// What can be changed, one at a time:
//   --motion captured | zero | far-zero   the motion vectors as saved; none; none beyond --far-depth
//   --motion-dir DIR                      a saved frame's motion comes from DIR\NNNNN-motion.raw when that
//                                         file exists (two 32-bit floats a pixel, tight rows, the motion
//                                         plane's size): motion computed another way for the same frames
//   --bias-dir DIR                        DLSS's "trust this frame's colour here" mask comes from DIR\NNNNN-bias.raw
//                                         when that file exists (one 32-bit float a pixel, tight rows, the
//                                         colour plane's size; 1 = lean on this frame, 0 = normal). Not with --fold.
//   --preset 0 | 10..13                   the DLSS model (0 = as captured; 11 K, 13 M)
//   --parity all | even | odd             every frame, or only those with an even / odd present
//                                         ordinal (the motion of a skipped frame is added to the next)
// and how the saved frames are walked:
//   --first N --frames N                  where the run starts (capture index) and how many frames
//   --all                                 every saved frame; by default only the first run of
//                                         CONSECUTIVE present ordinals (DLSS history across a gap
//                                         between saved frames is not what the game produced)
//   --preroll N                           the first frame N times before the run, so the history is
//                                         formed when the run starts (default 16)
//   --pingpong N                          walk the run back and forth N more times (backwards the
//                                         saved motion is negated): a longer sequence from few frames
//   --crop X Y W H                        save only this rectangle of each output (a whole 3440x1433
//                                         output is 20 to 40 MB a frame; the region studied is far less)
//   --save-last N                         write only the last N outputs to disk (every frame still goes
//                                         through DLSS, and the files keep their numbers)
//
//   --fold cycle | fixed                  the "fold the shading" trial (2026-11-24): every plane is folded to
//                                         half size by keeping ONE pixel of each 2x2 block, and DLSS is
//                                         asked for Performance (half size in, full size out). `cycle` keeps
//                                         a different pixel of the block each step, which is the jitter a
//                                         half-size picture needs; `fixed` always keeps the same one (only
//                                         the capture's own small jitter remains). The saved frames were
//                                         shaded at full detail, so this shows what DLSS rebuilds from a
//                                         folded picture, not what coarse shading itself costs or saves.
//   --fold-sign 1 | -1                    -1 tells DLSS the kept pixel's offset with the opposite sign (a
//                                         check of the convention: the wrong sign is visibly softer)
//
//   --fold-declare pixel | block          what DLSS is told about the folded sample. pixel (default): the kept
//                                         pixel's own centre. block: the block's centre whichever pixel was kept,
//                                         which is what the live test build f98ae48c told DLSS for a scene shaded
//                                         in blocks (use it on a capture taken in a live folded mode).
//   --second-feature                      at the second replayed frame, a second DLSS feature (another view,
//                                         half the render size, Performance) is created and run once on the same
//                                         planes, then the replay carries on. Shows whether one feature's creation
//                                         disturbs another's later frames (Cort's fb0d69cd run: DLAA turned to
//                                         colour noise after the Performance level had been created).
//   Two features at once (Cort's fb0d69cd colour noise; which orders of creating, running and releasing
//   two DLSS features of different sizes keep both pictures right). The second feature is a Performance
//   one: this step's planes folded to half size (the last pixel of each 2x2 block kept), created at
//   that half size, on view 2, with its own output.
//     --second-at N          create and run it at step N (0: before the main feature is first created)
//     --second-every         run it on every step from N on (else only at N)
//     --second-release-at N  release it at step N, before anything is run
//     --main-recreate-at N   release the main feature at step N and create it again (history restarts)
//   Every replayed step logs "stats" of the main output (and of the second's when it ran): the largest
//   value, the mean, the share of pixels with a channel above 64 and of values not finite. Clean output
//   here is under 64 everywhere; the noise reached thousands.
//   --fold-block 2x2 | 2x1                which block the fold keeps one pixel of: 2x2 (Performance, the default)
//                                         or 2x1 (the live Quality level: half width, full height).
//   --fold-create folded | full           the size the DLSS feature is created at: folded (the default, as the
//                                         offline trials ran) or full (as the live build does since 0b7902c6).
//   --time                                GPU time of every DLSS evaluate (timestamps around the one call), each
//                                         frame and the median of the replayed frames after the first, in replay.log.
// Output, in <out folder>\<name>: one NNNNN-dlssout.raw per replayed frame (tight rows, the capture's
// own pixel format), replay.csv (which saved frame each step is, and the rectangle saved) and
// replay.log (every setting, and per frame how far the whole replayed output is from the output the
// game run saved).
//
// A measuring tool for saved captures: it reads files and runs DLSS on this machine's card. It does
// not touch a game, an install or a setting. Needs an RTX card and the DLSS runtime beside it.
#include "test_support.hpp"
#include "host_dx12/dlss/dlss_backend.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
FILE* g_log = nullptr;
void Log(const char* fmt, ...)
{
    char line[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    std::printf("%s\n", line);
    std::fflush(stdout);
    if (g_log) { std::fprintf(g_log, "%s\n", line); std::fflush(g_log); }
}

struct PlaneInfo { bool valid = false; uint32_t width = 0, height = 0, format = 0, row_pitch = 0; };
struct Meta {
    uint32_t index = 0;
    uint64_t present_ordinal = 0;
    uint32_t width = 0, height = 0;
    float jitter_x = 0, jitter_y = 0, mv_scale_x = 1, mv_scale_y = 1, pre_exposure = 1, exposure_scale = 1;
    uint32_t preset = 0, debug_view = 0, quality = 5;
    bool hdr = false, depth_inverted = false, auto_exposure = false, mv_lowres = true, mv_jittered = false;
    bool jitter_shifted = false, evaluation = false, bias_mask = false;
    PlaneInfo color, depth, motion, dlssout;
};

bool ReadText(const std::string& path, std::string* out)
{
    FILE* fp = nullptr;
    if (fopen_s(&fp, path.c_str(), "rb") != 0 || !fp) return false;
    char buffer[4096];
    size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof buffer, fp)) > 0) out->append(buffer, n);
    std::fclose(fp);
    return true;
}
// The value after "key": in our own flat capture metadata. `from` narrows the search to one plane's entry.
size_t ValueAt(const std::string& s, const char* key, size_t from)
{
    const std::string quoted = std::string("\"") + key + "\"";
    size_t p = s.find(quoted, from);
    if (p == std::string::npos) return p;
    p = s.find(':', p + quoted.size());
    if (p == std::string::npos) return p;
    ++p;
    while (p < s.size() && (s[p] == ' ' || s[p] == '[' || s[p] == '"')) ++p;
    return p;
}
bool Number(const std::string& s, const char* key, double* out, size_t from = 0)
{
    const size_t p = ValueAt(s, key, from);
    if (p == std::string::npos) return false;
    char* end = nullptr;
    const double v = std::strtod(s.c_str() + p, &end);
    if (end == s.c_str() + p) return false;
    *out = v;
    return true;
}
bool Pair(const std::string& s, const char* key, float* a, float* b)
{
    const size_t p = ValueAt(s, key, 0);
    if (p == std::string::npos) return false;
    char* end = nullptr;
    const double x = std::strtod(s.c_str() + p, &end);
    if (end == s.c_str() + p) return false;
    const char* q = end;
    while (*q == ' ' || *q == ',') ++q;
    char* end2 = nullptr;
    const double y = std::strtod(q, &end2);
    if (end2 == q) return false;
    *a = static_cast<float>(x);
    *b = static_cast<float>(y);
    return true;
}
bool Flag(const std::string& s, const char* key, bool fallback)
{
    const size_t p = ValueAt(s, key, 0);
    if (p == std::string::npos) return fallback;
    return s.compare(p, 4, "true") == 0;
}
uint32_t Count(const std::string& s, const char* key, uint32_t fallback, size_t from = 0)
{
    double v = 0;
    return Number(s, key, &v, from) && v >= 0 ? static_cast<uint32_t>(v + 0.5) : fallback;
}
PlaneInfo Plane(const std::string& s, const char* name)
{
    PlaneInfo p;
    const size_t at = s.find(std::string("{\"name\": \"") + name + "\"");
    if (at == std::string::npos) return p;
    p.width = Count(s, "width", 0, at);
    p.height = Count(s, "height", 0, at);
    p.format = Count(s, "dxgi_format", 0, at);
    p.row_pitch = Count(s, "row_pitch", 0, at);
    p.valid = p.width && p.height && p.format && p.row_pitch;
    return p;
}
bool ReadMeta(const std::string& dir, uint32_t index, Meta* m)
{
    char name[64];
    snprintf(name, sizeof name, "\\%05u-meta.json", index);
    std::string s;
    if (!ReadText(dir + name, &s)) return false;
    m->index = index;
    double v = 0;
    if (Number(s, "present_ordinal", &v)) m->present_ordinal = static_cast<uint64_t>(v + 0.5);
    float w = 0, h = 0;
    if (Pair(s, "size", &w, &h)) { m->width = static_cast<uint32_t>(w + 0.5f); m->height = static_cast<uint32_t>(h + 0.5f); }
    Pair(s, "jitter_px", &m->jitter_x, &m->jitter_y);
    Pair(s, "mv_scale", &m->mv_scale_x, &m->mv_scale_y);
    m->preset = Count(s, "preset", 0);
    m->debug_view = Count(s, "debug_view", 0);
    m->quality = Count(s, "quality", 5);
    if (Number(s, "pre_exposure", &v)) m->pre_exposure = static_cast<float>(v);
    if (Number(s, "exposure_scale", &v)) m->exposure_scale = static_cast<float>(v);
    m->hdr = Flag(s, "hdr", false);
    m->depth_inverted = Flag(s, "depth_inverted", false);
    m->auto_exposure = Flag(s, "auto_exposure", false);
    m->mv_lowres = Flag(s, "mv_lowres", true);
    m->mv_jittered = Flag(s, "mv_jittered", false);
    m->jitter_shifted = Flag(s, "jitter_shifted", false);
    m->bias_mask = Flag(s, "bias_current_color", false);
    m->evaluation = s.find("\"capture_kind\": \"evaluation\"") != std::string::npos && Flag(s, "evaluation_succeeded", false);
    m->color = Plane(s, "color");
    m->depth = Plane(s, "depth");
    m->motion = Plane(s, "motion");
    m->dlssout = Plane(s, "dlssout");
    return true;
}

// Bytes per pixel of the formats a capture holds; 0 = not one this tool reads.
uint32_t Bpp(uint32_t format)
{
    switch (format) {
    case 2: return 16;                                              // R32G32B32A32_FLOAT
    case 10: case 16: return 8;                                     // R16G16B16A16_FLOAT, R32G32_FLOAT
    case 24: case 26: case 28: case 29: case 87: case 91: return 4; // 10:10:10:2, R11G11B10_FLOAT, 8-bit RGBA / BGRA
    case 34: case 39: case 40: case 41: return 4;                   // R16G16_FLOAT, 32-bit depth kinds
    default: return 0;
    }
}
// The format to create a texture with: the depth kinds are read as plain 32-bit float.
DXGI_FORMAT TextureFormat(uint32_t format)
{
    return format == 39 || format == 40 ? DXGI_FORMAT_R32_FLOAT : static_cast<DXGI_FORMAT>(format);
}
// One plane's file as tight rows (width * bytes per pixel each). The last row may be saved without its padding.
bool ReadPlane(const std::string& dir, uint32_t index, const char* name, const PlaneInfo& p, std::vector<uint8_t>* rows)
{
    const uint32_t bpp = Bpp(p.format);
    if (!p.valid || !bpp) return false;
    char file[64];
    snprintf(file, sizeof file, "\\%05u-%s.raw", index, name);
    FILE* fp = nullptr;
    if (fopen_s(&fp, (dir + file).c_str(), "rb") != 0 || !fp) return false;
    const size_t tight = static_cast<size_t>(p.width) * bpp;
    rows->assign(tight * p.height, 0);
    std::vector<uint8_t> row(p.row_pitch);
    bool ok = p.row_pitch >= tight;
    for (uint32_t y = 0; ok && y < p.height; ++y) {
        const size_t got = std::fread(row.data(), 1, p.row_pitch, fp);
        if (got < tight) { ok = false; break; }
        std::memcpy(rows->data() + tight * y, row.data(), tight);
    }
    std::fclose(fp);
    return ok;
}

uint16_t ToHalf(float f)
{
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int32_t exp = static_cast<int32_t>((x >> 23) & 0xFF) - 127 + 15;
    const uint32_t mant = x & 0x7FFFFFu;
    if (exp <= 0) return static_cast<uint16_t>(sign); // a vector this small is zero
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
    const uint32_t bits = (static_cast<uint32_t>(exp) << 10) | (mant >> 13);
    return static_cast<uint16_t>(sign | (bits + ((mant >> 12) & 1u)));
}
float FromHalf(uint16_t h)
{
    const uint32_t exp = (h >> 10) & 0x1Fu, mant = h & 0x3FFu;
    float v;
    if (exp == 0) v = std::ldexp(static_cast<float>(mant), -24);
    else if (exp == 31) v = 65504.0f;
    else v = std::ldexp(1.0f + static_cast<float>(mant) / 1024.0f, static_cast<int>(exp) - 15);
    return (h & 0x8000u) ? -v : v;
}
float SmallFloat(uint32_t bits, uint32_t mantissa_bits)
{
    const uint32_t m = bits & ((1u << mantissa_bits) - 1u), e = bits >> mantissa_bits;
    if (e == 31) return 65504.0f;
    const float scale = static_cast<float>(1u << mantissa_bits);
    return e == 0 ? std::ldexp(static_cast<float>(m) / scale, -14) : std::ldexp(1.0f + static_cast<float>(m) / scale, static_cast<int>(e) - 15);
}
// One pixel's brightness on a display-like scale (scene-linear formats are compressed), for the
// comparison with the output the game run saved. Not used for anything DLSS is given.
float Luma(const uint8_t* p, uint32_t format)
{
    float r = 0, g = 0, b = 0;
    bool linear = false;
    switch (format) {
    case 10: {
        uint16_t v[3];
        std::memcpy(v, p, 6);
        r = FromHalf(v[0]); g = FromHalf(v[1]); b = FromHalf(v[2]);
        linear = true;
        break;
    }
    case 26: {
        uint32_t v;
        std::memcpy(&v, p, 4);
        r = SmallFloat(v & 2047u, 6); g = SmallFloat((v >> 11) & 2047u, 6); b = SmallFloat(v >> 22, 5);
        linear = true;
        break;
    }
    case 24: {
        uint32_t v;
        std::memcpy(&v, p, 4);
        r = static_cast<float>(v & 1023u) / 1023.0f; g = static_cast<float>((v >> 10) & 1023u) / 1023.0f; b = static_cast<float>((v >> 20) & 1023u) / 1023.0f;
        break;
    }
    case 87: case 91: r = p[2] / 255.0f; g = p[1] / 255.0f; b = p[0] / 255.0f; break;
    default: r = p[0] / 255.0f; g = p[1] / 255.0f; b = p[2] / 255.0f; break;
    }
    float y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    if (!(y >= 0.0f)) y = 0.0f;
    return linear ? y / (1.0f + y) : y;
}

// The motion plane as floats (x, y per pixel) and back, for the two formats a capture uses.
bool MotionToFloat(const std::vector<uint8_t>& rows, uint32_t format, std::vector<float>* out)
{
    if (format == 16) {
        out->resize(rows.size() / 4);
        std::memcpy(out->data(), rows.data(), out->size() * 4);
        return true;
    }
    if (format == 34) {
        out->resize(rows.size() / 2);
        for (size_t i = 0; i < out->size(); ++i) {
            uint16_t h;
            std::memcpy(&h, rows.data() + 2 * i, 2);
            (*out)[i] = FromHalf(h);
        }
        return true;
    }
    return false;
}
void FloatToMotion(const std::vector<float>& values, uint32_t format, std::vector<uint8_t>* rows)
{
    if (format == 16) {
        rows->resize(values.size() * 4);
        std::memcpy(rows->data(), values.data(), rows->size());
        return;
    }
    rows->resize(values.size() * 2);
    for (size_t i = 0; i < values.size(); ++i) {
        const uint16_t h = ToHalf(values[i]);
        std::memcpy(rows->data() + 2 * i, &h, 2);
    }
}

struct Texture {
    test::ComPtr<ID3D12Resource> tex, upload;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    uint32_t bpp = 0;
    bool filled = false; // false: still in COPY_DEST from creation
};
bool MakeInput(ID3D12Device* dev, const PlaneInfo& p, Texture* t)
{
    t->bpp = Bpp(p.format);
    if (!t->bpp) return false;
    t->tex = test::CreateTexture(dev, p.width, p.height, TextureFormat(p.format), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    if (!t->tex) return false;
    const D3D12_RESOURCE_DESC desc = t->tex->GetDesc();
    UINT64 bytes = 0;
    dev->GetCopyableFootprints(&desc, 0, 1, 0, &t->fp, nullptr, nullptr, &bytes);
    t->upload = test::CreateBuffer(dev, bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    return t->upload != nullptr;
}
// Tight rows into the upload buffer, then a copy into the texture, left readable for DLSS.
bool Fill(ID3D12GraphicsCommandList* list, Texture* t, const std::vector<uint8_t>& rows)
{
    constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const size_t tight = static_cast<size_t>(t->fp.Footprint.Width) * t->bpp;
    if (rows.size() != tight * t->fp.Footprint.Height) return false;
    uint8_t* p = nullptr;
    if (FAILED(t->upload->Map(0, nullptr, reinterpret_cast<void**>(&p)))) return false;
    for (UINT y = 0; y < t->fp.Footprint.Height; ++y)
        std::memcpy(p + static_cast<size_t>(y) * t->fp.Footprint.RowPitch, rows.data() + tight * y, tight);
    t->upload->Unmap(0, nullptr);
    if (t->filled) {
        const D3D12_RESOURCE_BARRIER b = test::Transition(t->tex.Get(), read, D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(1, &b);
    }
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = t->upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = t->fp;
    dst.pResource = t->tex.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const D3D12_RESOURCE_BARRIER b = test::Transition(t->tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, read);
    list->ResourceBarrier(1, &b);
    t->filled = true;
    return true;
}

enum class MotionMode { Captured, Zero, FarZero };
enum class FoldMode { Off, Cycle, Fixed };
// One pixel of each 2x2 block: (2x + px, 2y + py). Tight rows in, tight rows out.
void FoldRows(const std::vector<uint8_t>& in, uint32_t width, uint32_t bpp, uint32_t fw, uint32_t fh, uint32_t px, uint32_t py, std::vector<uint8_t>* out,
              uint32_t bx = 2, uint32_t by = 2)
{
    out->resize(static_cast<size_t>(fw) * fh * bpp);
    for (uint32_t y = 0; y < fh; ++y) {
        const uint8_t* src = in.data() + (static_cast<size_t>(by * y + py) * width + px) * bpp;
        uint8_t* dst = out->data() + static_cast<size_t>(y) * fw * bpp;
        for (uint32_t x = 0; x < fw; ++x) std::memcpy(dst + static_cast<size_t>(x) * bpp, src + static_cast<size_t>(bx * x) * bpp, bpp);
    }
}
struct OutStats { float max = 0; double mean = 0, above = 0; uint64_t nonfinite = 0; };
OutStats Measure(const uint8_t* p, size_t pitch, uint32_t w, uint32_t h, uint32_t format, uint32_t bpp)
{
    OutStats o;
    uint64_t n = 0, above = 0;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* q = p + static_cast<size_t>(y) * pitch + static_cast<size_t>(x) * bpp;
            float c[3];
            if (format == 10) {
                uint16_t v[3];
                std::memcpy(v, q, 6);
                c[0] = FromHalf(v[0]); c[1] = FromHalf(v[1]); c[2] = FromHalf(v[2]);
            } else {
                c[0] = c[1] = c[2] = Luma(q, format);
            }
            bool bad = false, high = false;
            for (const float v : c) {
                if (!std::isfinite(v)) { bad = true; continue; }
                o.max = (std::max)(o.max, v);
                high |= v > 64.0f;
            }
            if (bad) { ++o.nonfinite; continue; }
            o.mean += (static_cast<double>(c[0]) + c[1] + c[2]) / 3.0;
            above += high ? 1 : 0;
            ++n;
        }
    o.mean = n ? o.mean / static_cast<double>(n) : 0.0;
    o.above = n ? 100.0 * static_cast<double>(above) / static_cast<double>(n) : 0.0;
    return o;
}
struct Step {
    size_t frame = 0;            // index into the selected frames
    bool preroll = false;
    int direction = 0;           // +1 forwards from the step before, -1 backwards, 0 no step before (no motion)
    std::vector<size_t> motions; // the saved frames whose motion planes make up this step (all frames list)
};

int Usage()
{
    std::printf("dlss_capture_replay <capture folder> <out folder> [--name N] [--motion captured|zero|far-zero] [--far-depth D] [--motion-dir DIR] [--bias-dir DIR]\n"
                "    [--preset 0|10..13] [--parity all|even|odd] [--first INDEX] [--frames N] [--all] [--preroll N] [--pingpong N]\n"
                "    [--crop X Y W H] [--save-last N] [--fold cycle|fixed] [--fold-block 2x2|2x1] [--fold-create folded|full] [--fold-declare pixel|block] [--fold-sign 1|-1] [--time]\n");
    return 2;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) return Usage();
    const std::string capture = argv[1], out_root = argv[2];
    std::string name = "replay", motion_dir, bias_dir;
    MotionMode mode = MotionMode::Captured;
    const char* mode_name = "captured";
    float far_depth = 0.005f;
    uint32_t preset = 0, first = 0, max_frames = 0, preroll = 16, pingpong = 0, save_last = 0;
    uint32_t crop_x = 0, crop_y = 0, crop_w = 0, crop_h = 0; // 0 size = the whole output
    int parity = -1;
    bool all = false;
    FoldMode fold = FoldMode::Off;
    float fold_sign = 1.0f;
    bool fold_block = false;
    bool second_feature = false;
    uint32_t fold_bx = 2, fold_by = 2;
    int second_at = -1, second_release_at = -1, main_recreate_at = -1;
    bool second_every = false;
    bool fold_create_full = false, timing = false;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        const char* value = i + 1 < argc ? argv[i + 1] : nullptr;
        const auto number = [&](uint32_t* out) { if (!value) return false; *out = static_cast<uint32_t>(std::strtoul(value, nullptr, 10)); ++i; return true; };
        if (a == "--name" && value) { name = value; ++i; }
        else if (a == "--motion" && value) {
            const std::string v = value;
            ++i;
            if (v == "captured") mode = MotionMode::Captured;
            else if (v == "zero") mode = MotionMode::Zero;
            else if (v == "far-zero") mode = MotionMode::FarZero;
            else return Usage();
            mode_name = argv[i];
        }
        else if (a == "--motion-dir" && value) { motion_dir = value; ++i; }
        else if (a == "--bias-dir" && value) { bias_dir = value; ++i; }
        else if (a == "--far-depth" && value) { far_depth = static_cast<float>(std::strtod(value, nullptr)); ++i; }
        else if (a == "--preset") { if (!number(&preset)) return Usage(); }
        else if (a == "--first") { if (!number(&first)) return Usage(); }
        else if (a == "--frames") { if (!number(&max_frames)) return Usage(); }
        else if (a == "--preroll") { if (!number(&preroll)) return Usage(); }
        else if (a == "--pingpong") { if (!number(&pingpong)) return Usage(); }
        else if (a == "--save-last") { if (!number(&save_last)) return Usage(); }
        else if (a == "--parity" && value) {
            const std::string v = value;
            ++i;
            parity = v == "even" ? 0 : v == "odd" ? 1 : -1;
            if (parity < 0 && v != "all") return Usage();
        }
        else if (a == "--all") all = true;
        else if (a == "--fold" && value) {
            const std::string v = value;
            ++i;
            if (v == "cycle") fold = FoldMode::Cycle;
            else if (v == "fixed") fold = FoldMode::Fixed;
            else return Usage();
        }
        else if (a == "--second-feature") second_feature = true;
        else if (a == "--second-at" && value) { second_at = std::atoi(value); ++i; }
        else if (a == "--second-every") second_every = true;
        else if (a == "--second-release-at" && value) { second_release_at = std::atoi(value); ++i; }
        else if (a == "--main-recreate-at" && value) { main_recreate_at = std::atoi(value); ++i; }
        else if (a == "--fold-block" && value) {
            const std::string v = value;
            ++i;
            if (v == "2x2") fold_by = 2;
            else if (v == "2x1") fold_by = 1;
            else return Usage();
        }
        else if (a == "--fold-create" && value) {
            const std::string v = value;
            ++i;
            if (v == "full") fold_create_full = true;
            else if (v != "folded") return Usage();
        }
        else if (a == "--time") timing = true;
        else if (a == "--fold-declare" && value) { fold_block = std::string(value) == "block"; ++i; }
        else if (a == "--fold-sign" && value) { fold_sign = std::strtod(value, nullptr) < 0 ? -1.0f : 1.0f; ++i; }
        else if (a == "--crop" && i + 4 < argc) {
            crop_x = static_cast<uint32_t>(std::strtoul(argv[i + 1], nullptr, 10));
            crop_y = static_cast<uint32_t>(std::strtoul(argv[i + 2], nullptr, 10));
            crop_w = static_cast<uint32_t>(std::strtoul(argv[i + 3], nullptr, 10));
            crop_h = static_cast<uint32_t>(std::strtoul(argv[i + 4], nullptr, 10));
            i += 4;
        }
        else return Usage();
    }
    CreateDirectoryA(out_root.c_str(), nullptr);
    const std::string out_dir = out_root + "\\" + name;
    CreateDirectoryA(out_dir.c_str(), nullptr);
    fopen_s(&g_log, (out_dir + "\\replay.log").c_str(), "wb");
    if (!motion_dir.empty()) Log("motion planes from %s where present", motion_dir.c_str());
    if (!bias_dir.empty()) Log("bias masks from %s where present", bias_dir.c_str());
    Log("dlss_capture_replay: capture=%s name=%s motion=%s far_depth=%g preset=%u parity=%s first=%u frames=%u all=%d preroll=%u pingpong=%u",
        capture.c_str(), name.c_str(), mode_name, far_depth, preset, parity < 0 ? "all" : parity ? "odd" : "even", first, max_frames,
        all ? 1 : 0, preroll, pingpong);

    // The saved frames DLSS really evaluated, in capture order.
    std::vector<Meta> frames;
    for (uint32_t index = 0, missing = 0; missing < 64 && index < 100000; ++index) {
        Meta m;
        if (!ReadMeta(capture, index, &m)) { ++missing; continue; }
        missing = 0;
        if (index < first || !m.evaluation || m.debug_view || !m.color.valid || !m.depth.valid || !m.motion.valid) continue;
        frames.push_back(m);
    }
    if (frames.empty()) { Log("FATAL: no evaluated frame with color, depth and motion in %s (from index %u)", capture.c_str(), first); return 1; }
    // By default only the first run of consecutive present ordinals: across a gap the history is not the game's.
    size_t run = frames.size();
    if (!all) {
        run = 1;
        while (run < frames.size() && frames[run].present_ordinal == frames[run - 1].present_ordinal + 1) ++run;
    }
    if (max_frames && run > max_frames) run = max_frames;
    std::vector<size_t> selected;
    for (size_t i = 0; i < run; ++i)
        if (parity < 0 || static_cast<int>(frames[i].present_ordinal & 1u) == parity) selected.push_back(i);
    if (selected.empty()) { Log("FATAL: the run of %zu frames has none with that parity", run); return 1; }
    const Meta& head = frames[selected[0]];
    Log("frames: %zu saved and usable, run of %zu from index %u (present ordinal %llu), %zu selected", frames.size(), run, frames[0].index,
        static_cast<unsigned long long>(frames[0].present_ordinal), selected.size());
    if (head.bias_mask) Log("note: the game run gave DLSS a bias mask (not saved); the replay gives none");
    if (head.jitter_shifted) Log("note: the capture marks its jitter as shifted");
    PlaneInfo out_plane = head.dlssout.valid && Bpp(head.dlssout.format) ? head.dlssout : head.color;
    // Folding: inputs of one pixel per block, and an output of exactly the blocks' size (an odd row or column is dropped).
    const uint32_t fold_w = head.color.width / fold_bx, fold_h = head.color.height / fold_by;
    if (fold != FoldMode::Off) {
        if (head.depth.width != head.color.width || head.depth.height != head.color.height || head.motion.width != head.color.width ||
            head.motion.height != head.color.height || out_plane.width != head.color.width || out_plane.height != head.color.height) {
            Log("FATAL: --fold needs a capture whose color, depth, motion and output are one size (a DLAA capture)");
            return 1;
        }
        out_plane.width = fold_w * fold_bx;
        out_plane.height = fold_h * fold_by;
        Log("fold: %s, block %ux%u, sign %+.0f: %u x %u kept from each %u x %u plane, output %u x %u, DLSS mode %s, feature created at %s size",
            fold == FoldMode::Cycle ? "cycle" : "fixed", fold_bx, fold_by, static_cast<double>(fold_sign), fold_w, fold_h, head.color.width,
            head.color.height, out_plane.width, out_plane.height, fold_by == 2 ? "Performance" : "Quality", fold_create_full ? "the full" : "the folded");
    }
    if (!crop_w || !crop_h || crop_x >= out_plane.width || crop_y >= out_plane.height) { crop_x = 0; crop_y = 0; crop_w = out_plane.width; crop_h = out_plane.height; }
    crop_w = std::min(crop_w, out_plane.width - crop_x);
    crop_h = std::min(crop_h, out_plane.height - crop_y);
    Log("saved rectangle: x %u y %u, %u x %u of the %u x %u output", crop_x, crop_y, crop_w, crop_h, out_plane.width, out_plane.height);
    if (!Bpp(head.color.format) || !Bpp(head.depth.format) || Bpp(head.depth.format) != 4 || (head.motion.format != 16 && head.motion.format != 34)) {
        Log("FATAL: plane formats not read by this tool: color %u depth %u motion %u", head.color.format, head.depth.format, head.motion.format);
        return 1;
    }
    for (size_t i = 0; i < run; ++i) {
        const Meta& m = frames[i];
        if (m.color.width != head.color.width || m.color.height != head.color.height || m.color.format != head.color.format ||
            m.motion.width != head.motion.width || m.motion.format != head.motion.format || m.depth.width != head.depth.width) {
            Log("FATAL: frame %u differs in size or format from frame %u", m.index, head.index);
            return 1;
        }
    }

    // The walk: the first frame again and again, then the run, then back and forth.
    std::vector<Step> steps;
    for (uint32_t i = 0; i < preroll; ++i) { Step s; s.frame = 0; s.preroll = true; steps.push_back(s); }
    {
        Step s; s.frame = 0; steps.push_back(s);
        const auto between = [&](size_t lo, size_t hi) { // the saved frames after `lo` up to `hi` (indices into frames)
            std::vector<size_t> list;
            for (size_t f = selected[lo] + 1; f <= selected[hi]; ++f) list.push_back(f);
            return list;
        };
        for (size_t k = 1; k < selected.size(); ++k) { Step t; t.frame = k; t.direction = 1; t.motions = between(k - 1, k); steps.push_back(t); }
        for (uint32_t pass = 0; pass < pingpong && selected.size() > 1; ++pass) {
            if (pass % 2 == 0) for (size_t k = selected.size() - 1; k-- > 0;) { Step t; t.frame = k; t.direction = -1; t.motions = between(k, k + 1); steps.push_back(t); }
            else for (size_t k = 1; k < selected.size(); ++k) { Step t; t.frame = k; t.direction = 1; t.motions = between(k - 1, k); steps.push_back(t); }
        }
    }

    test::Device d;
    if (!test::CreateDevice(d, false)) { Log("FATAL: no hardware D3D12 device"); return test::kSkip; }
    {
        test::ComPtr<IDXGIAdapter1> adapter;
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(d.factory->EnumAdapterByLuid(d.device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)))) adapter->GetDesc1(&desc);
        Log("device: %ls (vendor 0x%04X)", desc.Description, desc.VendorId);
        if (desc.VendorId != 0x10DE) { Log("FATAL: DLSS needs an NVIDIA card"); return test::kSkip; }
    }
    const std::string ngx_dir = out_root + "\\ngx-data";
    CreateDirectoryA(ngx_dir.c_str(), nullptr);
    const std::wstring ngx_wide(ngx_dir.begin(), ngx_dir.end());
    const bool init_ok = na::dlss::InitOwn(d.device.Get(), ngx_wide.c_str());
    const auto caps = na::dlss::GetCapabilities();
    Log("NGX own instance: %s; DLSS available=%d init-result=0x%08X %s", init_ok ? "started" : "FAILED", caps.available ? 1 : 0,
        static_cast<unsigned>(caps.feature_init_result), caps.reason);
    if (!init_ok || !caps.available) return 1;

    ID3D12Device* dev = d.device.Get();
    Texture color, depth, motion, bias;
    PlaneInfo bias_plane;
    if (!bias_dir.empty()) {
        if (fold != FoldMode::Off) { Log("FATAL: --bias-dir is not read with --fold"); return 1; }
        bias_plane.valid = true;
        bias_plane.width = head.color.width;
        bias_plane.height = head.color.height;
        bias_plane.format = 41; // R32_FLOAT
        bias_plane.row_pitch = bias_plane.width * 4;
        REQUIRE(MakeInput(dev, bias_plane, &bias));
    }
    {
        PlaneInfo c = head.color, z = head.depth, v = head.motion;
        if (fold != FoldMode::Off) { c.width = z.width = v.width = fold_w; c.height = z.height = v.height = fold_h; }
        REQUIRE(MakeInput(dev, c, &color) && MakeInput(dev, z, &depth) && MakeInput(dev, v, &motion));
    }
    const uint32_t out_bpp = Bpp(out_plane.format);
    auto output = test::CreateTexture(dev, out_plane.width, out_plane.height, TextureFormat(out_plane.format), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    REQUIRE(output);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT out_fp{};
    UINT64 out_bytes = 0;
    {
        const D3D12_RESOURCE_DESC desc = output->GetDesc();
        dev->GetCopyableFootprints(&desc, 0, 1, 0, &out_fp, nullptr, nullptr, &out_bytes);
    }
    auto readback = test::CreateBuffer(dev, out_bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    REQUIRE(readback);
    test::List l;
    REQUIRE(l.Create(dev));
    REQUIRE(SUCCEEDED(l.list->Close()));
    // --time: two timestamps around the one DLSS evaluate of each step.
    test::ComPtr<ID3D12QueryHeap> time_queries;
    test::ComPtr<ID3D12Resource> time_readback;
    UINT64 time_frequency = 0;
    std::vector<double> evaluate_ms;
    if (second_feature && second_at < 0) second_at = 1; // the original one-off trial
    // The second feature's own inputs (one pixel of each 2x2 block), output and readback.
    Texture color2, depth2, motion2;
    test::ComPtr<ID3D12Resource> output2, readback2;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT out2_fp{};
    UINT64 out2_bytes = 0;
    const uint32_t half_w = head.color.width / 2, half_h = head.color.height / 2;
    if (second_at >= 0) {
        PlaneInfo c = head.color, z = head.depth, v = head.motion;
        c.width = z.width = v.width = half_w;
        c.height = z.height = v.height = half_h;
        REQUIRE(MakeInput(dev, c, &color2) && MakeInput(dev, z, &depth2) && MakeInput(dev, v, &motion2));
        output2 = test::CreateTexture(dev, half_w * 2, half_h * 2, TextureFormat(out_plane.format), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        REQUIRE(output2);
        const D3D12_RESOURCE_DESC desc = output2->GetDesc();
        dev->GetCopyableFootprints(&desc, 0, 1, 0, &out2_fp, nullptr, nullptr, &out2_bytes);
        readback2 = test::CreateBuffer(dev, out2_bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        REQUIRE(readback2);
        Log("second feature: %ux%u Performance on view 2, created at that size, at step %d%s; released at step %d; main recreated at step %d",
            half_w, half_h, second_at, second_every ? ", run every step after" : ", run once", second_release_at, main_recreate_at);
    }
    bool second_ran = false;
    if (timing) {
        const D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2, 0};
        time_readback = test::CreateBuffer(dev, 16, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        REQUIRE(time_readback && SUCCEEDED(dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&time_queries))) &&
                SUCCEEDED(d.queue->GetTimestampFrequency(&time_frequency)) && time_frequency);
    }

    const uint32_t use_preset = preset ? preset : head.preset;
    Log("evaluate: render %ux%u -> output %ux%u, color format %u, output format %u, preset %u (captured %u), quality %u, hdr=%d depth_inverted=%d "
        "auto_exposure=%d mv_lowres=%d mv_jittered=%d pre_exposure=%g exposure_scale=%g mv_scale=%g,%g",
        head.width, head.height, out_plane.width, out_plane.height, head.color.format, out_plane.format, use_preset, head.preset, head.quality,
        head.hdr ? 1 : 0, head.depth_inverted ? 1 : 0, head.auto_exposure ? 1 : 0, head.mv_lowres ? 1 : 0, head.mv_jittered ? 1 : 0,
        static_cast<double>(head.pre_exposure), static_cast<double>(head.exposure_scale), static_cast<double>(head.mv_scale_x),
        static_cast<double>(head.mv_scale_y));

    FILE* csv = nullptr;
    fopen_s(&csv, (out_dir + "\\replay.csv").c_str(), "wb");
    if (csv) std::fprintf(csv, "step,file,source_index,present_ordinal,direction,jitter_x,jitter_y,motion_planes,width,height,dxgi_format,bytes_per_pixel,crop_x,crop_y\n");

    std::vector<uint8_t> rows, depth_rows, motion_rows, saved_rows, folded, bias_rows;
    uint32_t bias_planes = 0;
    // The pixel of each 2x2 block kept at each step: all four in turn, opposite corners first.
    static const uint32_t kFoldPhase[4][2] = {{0, 0}, {1, 1}, {1, 0}, {0, 1}};
    std::vector<float> vectors, sum;
    const size_t pixels = static_cast<size_t>(head.motion.width) * head.motion.height;
    uint32_t written = 0, replaced_planes = 0;
    for (size_t si = 0; si < steps.size(); ++si) {
        const Step& s = steps[si];
        const Meta& m = frames[selected[s.frame]];
        l.Reset();
        uint32_t fold_px = fold == FoldMode::Cycle ? kFoldPhase[si % 4][0] : 0, fold_py = fold == FoldMode::Cycle ? kFoldPhase[si % 4][1] : 0;
        if (fold_bx == 2 && fold_by == 1) { fold_px = fold == FoldMode::Cycle ? static_cast<uint32_t>(si % 2) : 0; fold_py = 0; }
        REQUIRE(ReadPlane(capture, m.index, "color", m.color, &rows));
        REQUIRE(ReadPlane(capture, m.index, "depth", m.depth, &depth_rows));
        if (fold != FoldMode::Off) {
            FoldRows(rows, m.color.width, color.bpp, fold_w, fold_h, fold_px, fold_py, &folded, fold_bx, fold_by);
            REQUIRE(Fill(l.list.Get(), &color, folded));
            FoldRows(depth_rows, m.depth.width, depth.bpp, fold_w, fold_h, fold_px, fold_py, &folded, fold_bx, fold_by);
            REQUIRE(Fill(l.list.Get(), &depth, folded));
        } else {
            REQUIRE(Fill(l.list.Get(), &color, rows) && Fill(l.list.Get(), &depth, depth_rows));
        }
        // This step's motion: none without a step before or when asked for none; else the saved planes of
        // every saved frame the step covers, added up, negated when the walk runs backwards.
        sum.assign(pixels * 2, 0.0f);
        if (s.direction != 0 && mode != MotionMode::Zero) {
            for (const size_t f : s.motions) {
                // A replacement plane when one was given for this frame, else the saved one.
                bool replaced = false;
                if (!motion_dir.empty()) {
                    PlaneInfo given = frames[f].motion;
                    given.format = 16;
                    given.row_pitch = given.width * 8;
                    replaced = ReadPlane(motion_dir, frames[f].index, "motion", given, &motion_rows) && MotionToFloat(motion_rows, 16, &vectors);
                    if (replaced) ++replaced_planes;
                }
                REQUIRE(replaced || (ReadPlane(capture, frames[f].index, "motion", frames[f].motion, &motion_rows) && MotionToFloat(motion_rows, frames[f].motion.format, &vectors)));
                REQUIRE(vectors.size() == sum.size());
                const float sign = s.direction < 0 ? -1.0f : 1.0f;
                for (size_t i = 0; i < sum.size(); ++i) sum[i] += sign * vectors[i];
            }
            if (mode == MotionMode::FarZero && depth_rows.size() == pixels * 4) {
                // Far = within far_depth of the depth's far end (0 with reversed depth, 1 otherwise).
                for (size_t i = 0; i < pixels; ++i) {
                    float z;
                    std::memcpy(&z, depth_rows.data() + 4 * i, 4);
                    const float nearness = m.depth_inverted ? z : 1.0f - z;
                    if (!(nearness > far_depth)) { sum[2 * i] = 0.0f; sum[2 * i + 1] = 0.0f; }
                }
            }
        }
        FloatToMotion(sum, m.motion.format, &motion_rows);
        if (fold != FoldMode::Off) {
            FoldRows(motion_rows, m.motion.width, motion.bpp, fold_w, fold_h, fold_px, fold_py, &folded, fold_bx, fold_by);
            REQUIRE(Fill(l.list.Get(), &motion, folded));
        } else {
            REQUIRE(Fill(l.list.Get(), &motion, motion_rows));
        }

        na::dlss::EvalDesc e;
        e.color = color.tex.Get();
        e.depth = depth.tex.Get();
        e.motion = motion.tex.Get();
        e.output = output.Get();
        e.render_w = m.width ? m.width : m.color.width;
        e.render_h = m.height ? m.height : m.color.height;
        e.out_w = out_plane.width;
        e.out_h = out_plane.height;
        e.jitter_x = m.jitter_x;
        e.jitter_y = m.jitter_y;
        e.mv_scale_x = m.mv_scale_x;
        e.mv_scale_y = m.mv_scale_y;
        e.pre_exposure = m.pre_exposure;
        e.exposure_scale = m.exposure_scale;
        e.view_id = 1;
        e.quality = static_cast<na::dlss::Quality>(static_cast<int>(m.quality));
        e.preset = use_preset;
        e.reset = si == 0;
        if (fold != FoldMode::Off) {
            // A half-size picture whose pixel (x, y) is the full-size pixel (2x + px, 2y + py). In half-size
            // pixels that sample sits at centre - (0.25 - px / 2) from the pick, and the capture's own jitter
            // (full-size pixels) is half as large here. The saved vectors are full-size pixels too.
            e.render_w = fold_w;
            e.render_h = fold_h;
            // General block b on an axis: the kept pixel p sits (b/2 - p - 0.5) full-size pixels before the
            // block's centre, and full-size distances are b times larger than folded ones.
            const auto kept = [&](uint32_t b, uint32_t p, float full) {
                const float fb = static_cast<float>(b);
                return (fold_block ? 0.f : fold_sign * (fb * 0.5f - static_cast<float>(p) - 0.5f) / fb) + full / fb;
            };
            e.jitter_x = kept(fold_bx, fold_px, m.jitter_x);
            e.jitter_y = kept(fold_by, fold_py, m.jitter_y);
            e.mv_scale_x = m.mv_scale_x / static_cast<float>(fold_bx);
            e.mv_scale_y = m.mv_scale_y / static_cast<float>(fold_by);
            e.quality = fold_by == 2 ? na::dlss::Quality::MaxPerformance : na::dlss::Quality::MaxQuality;
            if (fold_create_full) {
                e.max_render_w = head.color.width;
                e.max_render_h = head.color.height;
            }
        }
        e.hdr = m.hdr;
        e.depth_inverted = m.depth_inverted;
        e.mv_jittered = m.mv_jittered;
        e.mv_display_res = !m.mv_lowres;
        if (fold != FoldMode::Off) e.mv_display_res = false; // the folded motion plane is render size
        e.auto_exposure = m.auto_exposure;
        if (!bias_dir.empty() && ReadPlane(bias_dir, m.index, "bias", bias_plane, &bias_rows)) {
            REQUIRE(Fill(l.list.Get(), &bias, bias_rows));
            e.bias_current_color = bias.tex.Get();
            ++bias_planes;
        }
        if (second_release_at >= 0 && si == static_cast<size_t>(second_release_at)) {
            na::dlss::ReleaseView(e.view_id + 1);
            Log("step %zu: second feature released", si);
        }
        if (main_recreate_at >= 0 && si == static_cast<size_t>(main_recreate_at)) {
            na::dlss::ReleaseView(e.view_id);
            e.reset = true;
            Log("step %zu: main feature released, created again on this step", si);
        }
        second_ran = false;
        const bool second_released = second_release_at >= 0 && si >= static_cast<size_t>(second_release_at);
        if (second_at >= 0 && !second_released && (si == static_cast<size_t>(second_at) || (second_every && si > static_cast<size_t>(second_at)))) {
            FoldRows(rows, m.color.width, color.bpp, half_w, half_h, 1, 1, &folded);
            REQUIRE(Fill(l.list.Get(), &color2, folded));
            FoldRows(depth_rows, m.depth.width, depth.bpp, half_w, half_h, 1, 1, &folded);
            REQUIRE(Fill(l.list.Get(), &depth2, folded));
            FoldRows(motion_rows, m.motion.width, motion.bpp, half_w, half_h, 1, 1, &folded);
            REQUIRE(Fill(l.list.Get(), &motion2, folded));
            na::dlss::EvalDesc x = e;
            x.color = color2.tex.Get();
            x.depth = depth2.tex.Get();
            x.motion = motion2.tex.Get();
            x.output = output2.Get();
            x.view_id = e.view_id + 1;
            x.render_w = x.max_render_w = half_w;
            x.render_h = x.max_render_h = half_h;
            x.out_w = half_w * 2;
            x.out_h = half_h * 2;
            x.jitter_x = (m.jitter_x - 0.5f) * 0.5f; // the kept pixel (1,1) is half a full-size pixel past the block's centre
            x.jitter_y = (m.jitter_y - 0.5f) * 0.5f;
            x.mv_scale_x = m.mv_scale_x * 0.5f;
            x.mv_scale_y = m.mv_scale_y * 0.5f;
            x.quality = na::dlss::Quality::MaxPerformance;
            x.mv_display_res = false;
            x.reset = si == static_cast<size_t>(second_at);
            x.bias_current_color = nullptr;
            const bool ok = na::dlss::Evaluate(l.list.Get(), x);
            if (!ok) Log("step %zu: the second feature was refused: %s", si, na::dlss::GetStats().reason);
            if (ok) {
                D3D12_RESOURCE_BARRIER b = test::Transition(output2.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                l.list->ResourceBarrier(1, &b);
                D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
                src.pResource = output2.Get();
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.pResource = readback2.Get();
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint = out2_fp;
                l.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                b = test::Transition(output2.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                l.list->ResourceBarrier(1, &b);
                second_ran = true;
            }
        }
        if (timing) l.list->EndQuery(time_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        const bool evaluated = na::dlss::Evaluate(l.list.Get(), e);
        if (timing) {
            l.list->EndQuery(time_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            l.list->ResolveQueryData(time_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, time_readback.Get(), 0);
        }
        if (!evaluated) {
            Log("FATAL: evaluate refused at step %zu (saved frame %u): %s", si, m.index, na::dlss::GetStats().reason);
            return 1;
        }
        if (!s.preroll) {
            D3D12_RESOURCE_BARRIER b = test::Transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            l.list->ResourceBarrier(1, &b);
            D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
            src.pResource = output.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.pResource = readback.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = out_fp;
            l.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            b = test::Transition(output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            l.list->ResourceBarrier(1, &b);
        }
        REQUIRE(SUCCEEDED(l.list->Close()));
        ID3D12CommandList* lists[] = {l.list.Get()};
        na::dlss::BeforeExecute(d.queue.Get(), 1, lists);
        d.queue->ExecuteCommandLists(1, lists);
        na::dlss::OnExecuted(d.queue.Get(), 1, lists);
        d.WaitIdle();
        if (dev->GetDeviceRemovedReason() != S_OK) { Log("FATAL: the device was removed at step %zu", si); return 1; }
        if (timing) {
            UINT64* t = nullptr;
            const D3D12_RANGE range{0, 16}, none{};
            REQUIRE(SUCCEEDED(time_readback->Map(0, &range, reinterpret_cast<void**>(&t))));
            const double ms = static_cast<double>(t[1] - t[0]) * 1000.0 / static_cast<double>(time_frequency);
            time_readback->Unmap(0, &none);
            Log("time: step %zu DLSS evaluate %.3f ms (render %ux%u -> %ux%u)%s", si, ms, e.render_w, e.render_h, e.out_w, e.out_h, si == 0 ? " (first: includes creation, not counted)" : "");
            if (si) evaluate_ms.push_back(ms);
        }
        if (s.preroll) continue;
        {
            uint8_t* mp = nullptr;
            const D3D12_RANGE whole{0, static_cast<SIZE_T>(out_bytes)}, none{0, 0};
            REQUIRE(SUCCEEDED(readback->Map(0, &whole, reinterpret_cast<void**>(&mp))));
            const OutStats a = Measure(mp, out_fp.Footprint.RowPitch, out_plane.width, out_plane.height, out_plane.format, out_bpp);
            readback->Unmap(0, &none);
            char second_text[160] = " | second: not run";
            if (second_ran) {
                const D3D12_RANGE whole2{0, static_cast<SIZE_T>(out2_bytes)};
                REQUIRE(SUCCEEDED(readback2->Map(0, &whole2, reinterpret_cast<void**>(&mp))));
                const OutStats b2 = Measure(mp, out2_fp.Footprint.RowPitch, half_w * 2, half_h * 2, out_plane.format, out_bpp);
                readback2->Unmap(0, &none);
                std::snprintf(second_text, sizeof second_text, " | second: max %.3f mean %.5f above64 %.2f%% nonfinite %llu", static_cast<double>(b2.max), b2.mean, b2.above,
                              static_cast<unsigned long long>(b2.nonfinite));
            }
            const na::dlss::Stats fs = na::dlss::GetStats();
            Log("stats: step %zu main: max %.3f mean %.5f above64 %.2f%% nonfinite %llu%s | features created %llu released %llu waiting %llu, views %u", si,
                static_cast<double>(a.max), a.mean, a.above, static_cast<unsigned long long>(a.nonfinite), second_text,
                static_cast<unsigned long long>(fs.features_created), static_cast<unsigned long long>(fs.features_released),
                static_cast<unsigned long long>(fs.features_pending), fs.views_active);
        }
        if (save_last && si + save_last < steps.size()) { ++written; continue; } // not kept on disk; the number is

        // The replayed output as tight rows, and (first forward pass) how far it is from the saved output.
        const size_t tight = static_cast<size_t>(out_plane.width) * out_bpp;
        rows.assign(tight * out_plane.height, 0);
        uint8_t* p = nullptr;
        const D3D12_RANGE whole{0, static_cast<SIZE_T>(out_bytes)};
        REQUIRE(SUCCEEDED(readback->Map(0, &whole, reinterpret_cast<void**>(&p))));
        for (uint32_t y = 0; y < out_plane.height; ++y) std::memcpy(rows.data() + tight * y, p + static_cast<size_t>(y) * out_fp.Footprint.RowPitch, tight);
        const D3D12_RANGE none{0, 0};
        readback->Unmap(0, &none);
        char file[64];
        snprintf(file, sizeof file, "%05u-dlssout.raw", written);
        FILE* fp = nullptr;
        if (fopen_s(&fp, (out_dir + "\\" + file).c_str(), "wb") != 0 || !fp) { Log("FATAL: cannot write %s", file); return 1; }
        const size_t crop_row = static_cast<size_t>(crop_w) * out_bpp;
        size_t put = 0;
        for (uint32_t y = 0; y < crop_h; ++y)
            put += std::fwrite(rows.data() + tight * (crop_y + y) + static_cast<size_t>(crop_x) * out_bpp, 1, crop_row, fp);
        std::fclose(fp);
        if (put != crop_row * crop_h) { Log("FATAL: short write of %s (disk full?)", file); return 1; }
        double apart = -1.0, level = 0.0;
        if (m.dlssout.valid && m.dlssout.format == out_plane.format && m.dlssout.width == out_plane.width && m.dlssout.height == out_plane.height &&
            ReadPlane(capture, m.index, "dlssout", m.dlssout, &saved_rows)) {
            double total = 0.0;
            uint64_t n = 0;
            for (uint32_t y = 2; y < out_plane.height; y += 4)
                for (uint32_t x = 2; x < out_plane.width; x += 4) {
                    const size_t at = tight * y + static_cast<size_t>(x) * out_bpp;
                    const float a = Luma(rows.data() + at, out_plane.format), b = Luma(saved_rows.data() + at, out_plane.format);
                    total += std::fabs(static_cast<double>(a) - static_cast<double>(b));
                    level += static_cast<double>(b);
                    ++n;
                }
            apart = n ? total / static_cast<double>(n) : -1.0;
            level = n ? level / static_cast<double>(n) : 0.0;
        }
        Log("step %zu -> %s: saved frame %u (present %llu) %s jitter %.5f,%.5f motion planes %zu; replay against saved output: mean |difference| %.5f at mean level %.4f",
            si, file, m.index, static_cast<unsigned long long>(m.present_ordinal), s.direction > 0 ? "forwards" : s.direction < 0 ? "backwards" : "start",
            static_cast<double>(m.jitter_x), static_cast<double>(m.jitter_y), s.direction != 0 && mode != MotionMode::Zero ? s.motions.size() : size_t(0), apart, level);
        if (csv) {
            std::fprintf(csv, "%u,%s,%u,%llu,%d,%.6f,%.6f,%zu,%u,%u,%u,%u,%u,%u\n", written, file, m.index, static_cast<unsigned long long>(m.present_ordinal), s.direction,
                         static_cast<double>(m.jitter_x), static_cast<double>(m.jitter_y), s.direction != 0 && mode != MotionMode::Zero ? s.motions.size() : size_t(0),
                         crop_w, crop_h, out_plane.format, out_bpp, crop_x, crop_y);
            std::fflush(csv);
        }
        ++written;
    }
    if (csv) std::fclose(csv);
    const auto st = na::dlss::GetStats();
    if (!motion_dir.empty()) Log("motion planes taken from the given folder: %u", replaced_planes);
    if (!bias_dir.empty()) Log("bias masks taken from the given folder: %u", bias_planes);
    if (!evaluate_ms.empty()) {
        std::vector<double> sorted = evaluate_ms;
        std::sort(sorted.begin(), sorted.end());
        double mean = 0;
        for (const double v : sorted) mean += v;
        mean /= static_cast<double>(sorted.size());
        Log("time summary: DLSS evaluate over %zu steps: median %.3f ms, mean %.3f, fastest %.3f, slowest %.3f", sorted.size(),
            sorted[sorted.size() / 2], mean, sorted.front(), sorted.back());
    }
    Log("done: %u outputs written to %s; evaluates=%llu features created=%llu (preset requested %u)", written, out_dir.c_str(),
        static_cast<unsigned long long>(st.evaluates), static_cast<unsigned long long>(st.features_created), st.feature_preset);
    d.WaitIdle();
    na::dlss::Shutdown();
    if (g_log) std::fclose(g_log);
    g_log = nullptr;
    return test::finish("dlss_capture_replay");
}
