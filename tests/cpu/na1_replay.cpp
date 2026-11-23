// na_na1_replay: runs the NA1 processor reference and its governor over one frame of a saved
// capture, the way the program runs them live, and writes the picture before and after.
//
//   na_na1_replay <capture dir> <frame index> <out prefix> [checks=24]
//
// It reads NNNNN-dlssout.raw (the DLSS output, linear), NNNNN-depth.raw (raw device depth) and
// NNNNN-present.raw (the screen) with the plane table of NNNNN-meta.json, fits the display curve
// from Referee-style thumbnails of output and screen, then closes the loop: enhance with the
// governor's dials, measure, summarise, observe; one line a check. Nothing here is a card.
#include "core/enhancer_governor.hpp"
#include "core/enhancer_screen_curve.hpp"
#include "core/enhancer_stats.hpp"
#include "core/na1_reference.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {
using namespace na::na1::ref;

struct Plane {
    uint32_t width = 0, height = 0, format = 0, row_pitch = 0;
    std::vector<unsigned char> bytes;
    const unsigned char* Row(uint32_t y) const { return bytes.data() + size_t(y) * row_pitch; }
};

bool ReadFile(const std::string& path, std::vector<unsigned char>& out)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff size = f.tellg();
    if (size < 0) return false;
    out.resize(size_t(size));
    f.seekg(0);
    if (!out.empty()) f.read(reinterpret_cast<char*>(out.data()), std::streamsize(size));
    return bool(f);
}

// The number after "key": inside text[from, to). No JSON library: the capture writer's layout is fixed.
bool NumberAfter(const std::string& text, size_t from, size_t to, const char* key, double& value)
{
    const std::string quoted = std::string("\"") + key + "\"";
    const size_t at = text.find(quoted, from);
    if (at == std::string::npos || at >= to) return false;
    const size_t colon = text.find(':', at + quoted.size());
    if (colon == std::string::npos || colon >= to) return false;
    const char* start = text.c_str() + colon + 1;
    char* end = nullptr;
    value = std::strtod(start, &end);
    if (end == start) {  // true / false
        while (*start == ' ') ++start;
        if (!std::strncmp(start, "true", 4)) { value = 1; return true; }
        if (!std::strncmp(start, "false", 5)) { value = 0; return true; }
        return false;
    }
    return true;
}

bool LoadPlane(const std::string& base, const std::string& meta, const char* name, Plane& plane, std::string& why)
{
    const size_t planes = meta.find("\"planes\"");
    const std::string entry = std::string("\"name\": \"") + name + "\"";
    const size_t at = planes == std::string::npos ? std::string::npos : meta.find(entry, planes);
    if (at == std::string::npos) { why = std::string("meta.json lists no plane called ") + name; return false; }
    const size_t close = meta.find('}', at);
    double w = 0, h = 0, format = 0, pitch = 0;
    if (close == std::string::npos || !NumberAfter(meta, at, close, "width", w) || !NumberAfter(meta, at, close, "height", h) ||
        !NumberAfter(meta, at, close, "dxgi_format", format) || !NumberAfter(meta, at, close, "row_pitch", pitch)) {
        why = std::string("meta.json's entry for ") + name + " is incomplete";
        return false;
    }
    plane.width = uint32_t(w); plane.height = uint32_t(h); plane.format = uint32_t(format); plane.row_pitch = uint32_t(pitch);
    const std::string path = base + name + ".raw";
    if (!ReadFile(path, plane.bytes)) { why = "cannot read " + path; return false; }
    return true;
}

float HalfToFloat(uint16_t v)
{
    const uint32_t sign = uint32_t(v >> 15) << 31, exponent = (v >> 10) & 31u, mantissa = v & 1023u;
    uint32_t bits;
    if (exponent == 0) {
        if (mantissa == 0) bits = sign;
        else {
            int e = -1;
            uint32_t m = mantissa;
            do { ++e; m <<= 1; } while (!(m & 1024u));
            bits = sign | uint32_t(127 - 15 - e) << 23 | (m & 1023u) << 13;
        }
    } else if (exponent == 31) bits = sign | 0x7f800000u | mantissa << 13;
    else bits = sign | (exponent + 127 - 15) << 23 | mantissa << 13;
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

uint32_t BytesPerPixel(uint32_t format)
{
    switch (format) {
    case 2: return 16;                          // R32G32B32A32_FLOAT
    case 10: return 8;                          // R16G16B16A16_FLOAT
    case 28: case 29: case 87: case 91: return 4;  // RGBA8 / BGRA8, plain or sRGB
    case 41: return 4;                          // R32_FLOAT
    default: return 0;
    }
}
// The stored value of one pixel as four floats (8-bit stays encoded). False past the end of the
// file (the last row of a readback is written without its padding; a short file is not).
bool Pixel(const Plane& p, uint32_t x, uint32_t y, float rgba[4])
{
    const uint32_t size = BytesPerPixel(p.format);
    const size_t at = size_t(y) * p.row_pitch + size_t(x) * size;
    if (!size || at + size > p.bytes.size()) return false;
    const unsigned char* s = p.bytes.data() + at;
    if (p.format == 10) {
        uint16_t v[4];
        std::memcpy(v, s, sizeof v);
        for (int c = 0; c < 4; ++c) rgba[c] = HalfToFloat(v[c]);
    } else if (p.format == 2) {
        std::memcpy(rgba, s, 4 * sizeof(float));
    } else if (p.format == 41) {
        std::memcpy(rgba, s, sizeof(float));
        rgba[1] = rgba[2] = rgba[3] = 0;
    } else {
        const bool bgra = p.format == 87 || p.format == 91;
        rgba[0] = float(s[bgra ? 2 : 0]) / 255.f;
        rgba[1] = float(s[1]) / 255.f;
        rgba[2] = float(s[bgra ? 0 : 2]) / 255.f;
        rgba[3] = float(s[3]) / 255.f;
    }
    return true;
}

// The Referee's thumbnail of a plane (core/referee.hpp, capture/referee_shader.hlsl `thumb`):
// 128 x 72 cells, 4 x 4 taps a cell, mean of log2(luma of the stored value + 1e-4).
void Thumbnail(const Plane& p, std::vector<float>& out)
{
    constexpr uint32_t taps = 4;
    out.assign(size_t(kCellsW) * kCellsH, 0.f);
    for (uint32_t cy = 0; cy < kCellsH; ++cy)
        for (uint32_t cx = 0; cx < kCellsW; ++cx) {
            float sum = 0;
            for (uint32_t b = 0; b < taps; ++b)
                for (uint32_t a = 0; a < taps; ++a) {
                    const uint32_t x = (std::min)(uint32_t((float(cx) + (float(a) + .5f) / taps) / kCellsW * float(p.width)), p.width - 1);
                    const uint32_t y = (std::min)(uint32_t((float(cy) + (float(b) + .5f) / taps) / kCellsH * float(p.height)), p.height - 1);
                    float v[4] = {};
                    Pixel(p, x, y, v);
                    sum += std::log2(Lum(Finite(v[0]), Finite(v[1]), Finite(v[2])) + na::enhancer::kRefereeLumaFloor);
                }
            out[size_t(cy) * kCellsW + cx] = sum / float(taps * taps);
        }
}

bool WritePpm(const std::string& path, const float* rgba, uint32_t w, uint32_t h, const Curve& curve)
{
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << "P6\n" << w << ' ' << h << "\n255\n";
    std::vector<unsigned char> row(size_t(w) * 3);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const float* p = rgba + (size_t(y) * w + x) * 4;
            const float rgb[3] = {(std::max)(Finite(p[0]), 0.f), (std::max)(Finite(p[1]), 0.f), (std::max)(Finite(p[2]), 0.f)};
            const float lum = Lum(rgb[0], rgb[1], rgb[2]);
            const float gain = (Shown(curve, LogLum(lum)) + 1e-5f) / (lum + 1e-3f);
            for (int c = 0; c < 3; ++c)
                row[size_t(x) * 3 + size_t(c)] = static_cast<unsigned char>(std::pow(Sat(rgb[c] * gain), 1.f / 2.2f) * 255.f + .5f);
        }
        f.write(reinterpret_cast<const char*>(row.data()), std::streamsize(row.size()));
    }
    f.flush();
    return bool(f);
}

void PrintNumbers(const char* title, const na::enhancer::Numbers& n)
{
    std::printf("%s: texture x%.4f broad x%.4f | colour %.4f -> %.4f variety %.4f -> %.4f | near black %.3f%% -> %.3f%% "
                "pure white %.3f%% -> %.3f%% | glow %.4f stops key shift %+.4f stops\n",
                title, double(n.texture), double(n.broad), double(n.colour_before), double(n.colour_after),
                double(n.variety_before), double(n.variety_after), double(n.dark_before), double(n.dark_after),
                double(n.white_before), double(n.white_after), double(n.glow), double(n.key_shift));
    std::printf("scene feel: average screen brightness %.4f (%.2f stops under white) dark weight %.2f | lighting range %.2f stops "
                "weight %.2f | mid %.2f | p85 %.3f p97 %.3f | tint %.3f %.3f %.3f | solid cells %u\n",
                double(n.m0), double(n.key_stops), double(n.w_dark), double(n.range), double(n.w_range), double(n.mid),
                double(n.p85), double(n.p97), double(n.tint[0]), double(n.tint[1]), double(n.tint[2]), n.solid_cells);
}
}

int main(int argc, char** argv)
{
    if (argc < 4) {
        std::fprintf(stderr, "usage: na_na1_replay <capture dir> <frame index> <out prefix> [checks=24]\n");
        return 2;
    }
    const int checks = argc > 4 ? (std::max)(std::atoi(argv[4]), 1) : 24;
    char number[16];
    std::snprintf(number, sizeof number, "%05u", unsigned(std::strtoul(argv[2], nullptr, 10)));
    const std::string base = std::string(argv[1]) + "/" + number + "-", prefix = argv[3];

    std::vector<unsigned char> meta_bytes;
    if (!ReadFile(base + "meta.json", meta_bytes)) { std::fprintf(stderr, "cannot read %smeta.json\n", base.c_str()); return 1; }
    const std::string meta(meta_bytes.begin(), meta_bytes.end());
    double reversed_value = 0;
    NumberAfter(meta, 0, meta.size(), "depth_reversed", reversed_value);
    const bool reversed = reversed_value != 0;

    Plane out_plane, depth_plane, present_plane;
    std::string why;
    if (!LoadPlane(base, meta, "dlssout", out_plane, why) || !LoadPlane(base, meta, "depth", depth_plane, why)) {
        std::fprintf(stderr, "%s\n", why.c_str());
        return 1;
    }
    const uint32_t w = out_plane.width, h = out_plane.height;
    if ((out_plane.format != 10 && out_plane.format != 2) || depth_plane.format != 41 || depth_plane.width != w || depth_plane.height != h || !w || !h) {
        std::fprintf(stderr, "dlssout must be a float picture and depth R32_FLOAT of the same size (formats %u and %u)\n",
                     out_plane.format, depth_plane.format);
        return 1;
    }
    const size_t pixels = size_t(w) * h;
    std::vector<float> before(pixels * 4), after(pixels * 4), depth(pixels);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            float v[4] = {};
            Pixel(out_plane, x, y, before.data() + (size_t(y) * w + x) * 4);
            Pixel(depth_plane, x, y, v);
            depth[size_t(y) * w + x] = v[0];
        }
    std::printf("capture %s frame %s: %ux%u, depth %s\n", argv[1], number, w, h, reversed ? "reversed" : "standard");

    Pyramid pyramid;
    BuildPyramid(before.data(), depth.data(), w, h, reversed, pyramid);
    double sum_y = 0;
    for (size_t i = 0; i < pixels; ++i) sum_y += pyramid.yq[i * 2];

    // The display curve, exactly as the program gets it: from the Referee's two thumbnails.
    Curve curve = na::enhancer::AssumedCurve(float(sum_y / double(pixels)));
    bool measured = false;
    const char* curve_why = "";
    if (LoadPlane(base, meta, "present", present_plane, why) && BytesPerPixel(present_plane.format) && present_plane.format != 41) {
        std::vector<float> thumb_out, thumb_screen;
        Thumbnail(out_plane, thumb_out);
        Thumbnail(present_plane, thumb_screen);
        const bool screen_float = present_plane.format == 10 || present_plane.format == 2;
        for (float& t : thumb_screen) {
            const float v = (std::max)(std::exp2(t) - na::enhancer::kRefereeLumaFloor, 0.f);
            t = screen_float ? v / (1.f + v)        // an HDR screen folded into 0..1
                             : std::pow(v, 2.2f);    // an 8-bit screen: stored values to light
        }
        measured = na::enhancer::FitScreenCurve(thumb_out.data(), thumb_screen.data(), uint32_t(thumb_out.size()), curve, &curve_why);
        std::printf("display curve: %s (%s; present format %u)\n", measured ? "fitted" : "assumed", curve_why, present_plane.format);
    } else {
        std::printf("display curve: assumed (%s)\n", why.empty() ? "the present plane is not a picture" : why.c_str());
    }
    if (measured) {
        std::printf("  knots from y %.3f every %.3f:", double(curve.x0), double(curve.dx));
        for (float v : curve.cy) std::printf(" %.4f", double(v));
        std::printf("\n");
    }

    na::enhancer::Governor governor;
    std::vector<float> cells(size_t(kCellsW) * kCellsH * kCellFloats);
    uint64_t now_ms = 0;
    char line[1024];
    auto run = [&](const Dials& dials, Scene scene) {
        scene.curve = curve;
        if (AllZero(dials, nullptr, nullptr)) after = before;
        else Compose(before.data(), pyramid, dials, scene, nullptr, nullptr, after.data());
        Measure(before.data(), after.data(), depth.data(), w, h, reversed, scene, dials.key, cells.data());
        return na::enhancer::Summarise(cells.data(), measured);
    };
    for (int check = 0; check < checks; ++check) {
        na::enhancer::Governor::Sample sample;
        sample.n = run(governor.Dials(), governor.Scene());
        sample.frame_ms = 10.f;
        sample.reset = check == 0;
        sample.now_ms = now_ms += 250;
        governor.Observe(sample);
        governor.Format(line, sizeof line);
        std::printf("%s\n", line);
    }
    const Dials dials = governor.Dials();
    Scene scene = governor.Scene();
    const na::enhancer::Numbers final_numbers = run(dials, scene);
    scene.curve = curve;
    std::printf("final dials: texture %.3f light %.3f colour %.3f key %+.3f\n", double(dials.texture), double(dials.light),
                double(dials.colour), double(dials.key));
    PrintNumbers("final numbers", final_numbers);
    if (!WritePpm(prefix + "-before.ppm", before.data(), w, h, curve) || !WritePpm(prefix + "-after.ppm", after.data(), w, h, curve)) {
        std::fprintf(stderr, "cannot write %s-before.ppm / -after.ppm\n", prefix.c_str());
        return 1;
    }
    std::printf("wrote %s-before.ppm and %s-after.ppm\n", prefix.c_str(), prefix.c_str());
    return 0;
}
