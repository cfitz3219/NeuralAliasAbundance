// Depth with the raster jitter taken back out (core/depth_unjitter.hpp), on a small deferred scene
// worked out exactly: a camera 1.7 m over flat ground, a wall standing on it, the depth buffer a
// game without its own jitter would get under viewport jitter (the picture moved by +j), and the
// game's own way of finding a pixel's place in the world: the ray through the pixel's CENTRE, as far
// as the depth says.
//
// The symptom this reproduces (Cort's Sniper Elite Resistance run on 7933efc, 2026-11-18): a thin
// ground mark and a lamp's pool of light that switch with the jitter sample far away and are solid
// up close.
//
// What must hold:
//  - with the jittered depth as it is, the far mark and the far pool switch with the jitter and the
//    near ones do not (the fixture really shows the symptom, and shows it for the stated reason);
//  - with the depth this header gives, no pixel of any of them switches;
//  - on a flat surface the result is the surface's depth at the pixel's centre, to float accuracy;
//  - at a silhouette the result stays on the line of the surface the pixel's sample belongs to (or,
//    where two surfaces meet, is the centre's own depth), and is never a value between two surfaces;
//  - a 24-bit depth buffer's steps do not get amplified.
// The printed lines are the measurements.
#include "test_support.hpp"
#include "core/depth_unjitter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {
using namespace na::depth::unjitter;

constexpr int kW = 640, kH = 360;
constexpr float kLens = 320.0f;   // pixels: 90 degrees across
constexpr float kEye = 1.7f;      // metres over the ground
constexpr float kNear = 0.1f;     // reversed depth with no far plane: d = near / distance
constexpr float kWallZ = 30.0f, kWallHalf = 1.47f, kWallTop = 3.0f;

enum Surface : uint8_t { kSky = 0, kGround = 1, kWall = 2 };
struct Hit { float depth; Surface surface; };

// What the unjittered scene shows at a place on the screen (sx, sy in pixels, +y down).
Hit Scene(float sx, float sy)
{
    const float dx = (sx - kW * 0.5f) / kLens, dy = -(sy - kH * 0.5f) / kLens; // ray = (dx, dy, 1)
    const float wall_x = dx * kWallZ, wall_y = kEye + dy * kWallZ;
    if (std::fabs(wall_x) <= kWallHalf && wall_y >= 0.0f && wall_y <= kWallTop) return {kNear / kWallZ, kWall};
    if (dy < 0.0f) return {kNear * -dy / kEye, kGround}; // distance = eye / -dy
    return {0.0f, kSky};
}
// The line of one surface carried across the whole screen: what "the same surface at the centre" is.
float SurfaceLine(Surface s, float sy)
{
    if (s == kWall) return kNear / kWallZ;
    if (s == kGround) return (std::max)(0.0f, kNear * ((sy - kH * 0.5f) / kLens) / kEye);
    return 0.0f;
}
struct Place { float x, y, z; };
// The game's own maths: the centre ray of pixel (px, py), as far as `depth` says.
Place Rebuild(int px, int py, float depth)
{
    const float dx = (px + 0.5f - kW * 0.5f) / kLens, dy = -(py + 0.5f - kH * 0.5f) / kLens;
    const float z = kNear / (std::max)(depth, 1e-12f);
    return {dx * z, kEye + dy * z, z};
}
float Halton(uint32_t i, uint32_t base)
{
    float f = 1.0f, r = 0.0f;
    for (; i; i /= base) { f /= base; r += f * (i % base); }
    return r;
}
struct Frame { float jx, jy; std::vector<float> raw, fixed; std::vector<Surface> surface; };
Frame Draw(float jx, float jy, int bits)
{
    Frame f{jx, jy, std::vector<float>(size_t(kW) * kH), std::vector<float>(size_t(kW) * kH), std::vector<Surface>(size_t(kW) * kH)};
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            const Hit hit = Scene(x + 0.5f - jx, y + 0.5f - jy); // the picture moved by +j
            float d = hit.depth;
            if (bits) { const float steps = float((1u << bits) - 1); d = std::round(d * steps) / steps; }
            f.raw[size_t(y) * kW + x] = d;
            f.surface[size_t(y) * kW + x] = hit.surface;
        }
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) f.fixed[size_t(y) * kW + x] = Pixel(f.raw.data(), kW, kH, x, y, jx, jy);
    return f;
}
// A thin volume lying on the ground, the way a deferred decal is cut out of the rebuilt place.
struct Mark { float x0, x1, z0, z1, half_height; };
bool InMark(const Mark& m, const Place& p) { return p.x >= m.x0 && p.x <= m.x1 && p.z >= m.z0 && p.z <= m.z1 && std::fabs(p.y) <= m.half_height; }
// A lamp's reach.
struct Lamp { float x, y, z, reach; };
bool InLamp(const Lamp& l, const Place& p)
{
    const float dx = p.x - l.x, dy = p.y - l.y, dz = p.z - l.z;
    return dx * dx + dy * dy + dz * dz <= l.reach * l.reach;
}
struct Count { int touched = 0, switching = 0; };
template <class Inside> Count Switching(const std::vector<Frame>& frames, bool fixed, Inside inside)
{
    Count c;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            int in = 0;
            for (const Frame& f : frames) in += inside(Rebuild(x, y, (fixed ? f.fixed : f.raw)[size_t(y) * kW + x])) ? 1 : 0;
            if (in) ++c.touched;
            if (in && in != int(frames.size())) ++c.switching;
        }
    return c;
}

std::vector<Frame> Cycle(int bits)
{
    std::vector<Frame> frames; // the 16 samples of the Halton (2,3) cycle, centred
    for (uint32_t i = 1; i <= 16; ++i) frames.push_back(Draw(Halton(i, 2) - 0.5f, Halton(i, 3) - 0.5f, bits));
    return frames;
}
const Mark kFarMark{-8.0f, -4.0f, 40.0f, 70.0f, 0.02f}; // clear of the wall's edge: a pixel that shows wall in one sample and marked ground in the next is anti-aliasing, not the symptom

void symptom_and_its_end()
{
    const std::vector<Frame> frames = Cycle(0);

    // 1. The symptom and its end. Marks 2 cm thick each way; lamps 2 m over the ground reaching 4 m.
    const Mark near_mark{-1.0f, 1.0f, 4.0f, 6.0f, 0.02f}, far_mark = kFarMark;
    const Lamp near_lamp{2.5f, 2.0f, 6.0f, 4.0f}, far_lamp{8.0f, 2.0f, 60.0f, 4.0f};
    struct Case { const char* name; Count raw, fixed; bool far_away; };
    const Case cases[] = {
        {"ground mark, 4 to 6 m", Switching(frames, false, [&](const Place& p) { return InMark(near_mark, p); }),
            Switching(frames, true, [&](const Place& p) { return InMark(near_mark, p); }), false},
        {"ground mark, 40 to 70 m", Switching(frames, false, [&](const Place& p) { return InMark(far_mark, p); }),
            Switching(frames, true, [&](const Place& p) { return InMark(far_mark, p); }), true},
        {"lamp pool at 6 m", Switching(frames, false, [&](const Place& p) { return InLamp(near_lamp, p); }),
            Switching(frames, true, [&](const Place& p) { return InLamp(near_lamp, p); }), false},
        {"lamp pool at 60 m", Switching(frames, false, [&](const Place& p) { return InLamp(far_lamp, p); }),
            Switching(frames, true, [&](const Place& p) { return InLamp(far_lamp, p); }), true},
    };
    for (const Case& c : cases) {
        std::printf("%-24s pixels touched %5d   switching with the jitter: as it is %5d (%5.1f %%)   with the nudge taken out %d\n", c.name,
            c.raw.touched, c.raw.switching, c.raw.touched ? 100.0 * c.raw.switching / c.raw.touched : 0.0, c.fixed.switching);
        CHECK(c.raw.touched > 20);
        CHECK(c.fixed.switching == 0);
        if (c.far_away) CHECK(c.raw.switching * 2 > c.raw.touched); // far away: most of it switches
        else CHECK(c.raw.switching * 20 < c.raw.touched);           // up close: solid but for an edge pixel or two
    }
}

// 2. Flat surfaces are exact; silhouettes stay on their own surface's line.
void flat_is_exact_and_edges_stay_on_their_surface()
{
    const std::vector<Frame> frames = Cycle(0);
    double worst_flat = 0, worst_edge = 0, worst_raw = 0;
    long flat = 0, edge = 0, edge_unchanged = 0, edge_off_line = 0;
    for (const Frame& f : frames)
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x) {
                const size_t i = size_t(y) * kW + x;
                const Hit centre = Scene(x + 0.5f, y + 0.5f);
                const float line = SurfaceLine(f.surface[i], y + 0.5f);
                const double scale = (std::max)(1e-9, double((std::max)(line, f.raw[i])));
                if (centre.surface == f.surface[i]) {
                    ++flat;
                    worst_flat = (std::max)(worst_flat, std::fabs(double(f.fixed[i]) - centre.depth) / scale);
                    worst_raw = (std::max)(worst_raw, std::fabs(double(f.raw[i]) - centre.depth) / scale);
                } else {
                    ++edge;
                    // Where two surfaces meet (a wall's foot) the centre's own depth is as good an answer.
                    const double off = (std::min)(std::fabs(double(f.fixed[i]) - line), std::fabs(double(f.fixed[i]) - centre.depth)) / scale;
                    if (f.fixed[i] == f.raw[i]) ++edge_unchanged;
                    else if (off > 1e-3) ++edge_off_line;
                    else worst_edge = (std::max)(worst_edge, off);
                }
            }
    std::printf("same surface at the centre: %ld pixels, largest error %.2e of the depth (as it is: %.2e)\n", flat, worst_flat, worst_raw);
    // "Left as they were" is mostly right as it is: sky and a wall facing the camera have no slope.
    std::printf("another surface at the centre (silhouettes): %ld pixels, on their own surface's line to %.2e, left as they were %ld, off the line %ld\n",
        edge, worst_edge, edge_unchanged, edge_off_line);
    CHECK(worst_flat < 2e-4);
    CHECK(worst_raw > 50 * worst_flat);
    CHECK(edge > 100 && edge_off_line == 0);
}

// 3. A 24-bit depth buffer: the steps are not amplified, and the far mark still holds.
void coarse_depth()
{
    const std::vector<Frame> coarse = Cycle(24);
    const Mark far_mark = kFarMark;
    const float step = 1.0f / float((1u << 24) - 1);
    double worst_steps = 0;
    for (const Frame& f : coarse)
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x) {
                const size_t i = size_t(y) * kW + x;
                const Hit centre = Scene(x + 0.5f, y + 0.5f);
                if (centre.surface == f.surface[i]) worst_steps = (std::max)(worst_steps, std::fabs(double(f.fixed[i]) - centre.depth) / step);
            }
    const Count coarse_raw = Switching(coarse, false, [&](const Place& p) { return InMark(far_mark, p); });
    const Count coarse_fixed = Switching(coarse, true, [&](const Place& p) { return InMark(far_mark, p); });
    std::printf("24-bit depth: largest error %.2f steps; far mark switching as it is %d of %d, with the nudge taken out %d\n", worst_steps,
        coarse_raw.switching, coarse_raw.touched, coarse_fixed.switching);
    CHECK(worst_steps < 2.5);
    CHECK(coarse_fixed.switching * 50 < coarse_fixed.touched + 50);
}
} // namespace

int main()
{
    return test::run({
        {"a far ground mark and lamp pool switch with the jitter, near ones do not; with the nudge taken out none do", symptom_and_its_end},
        {"flat surfaces are exact and silhouettes stay on their own surface's line", flat_is_exact_and_edges_stay_on_their_surface},
        {"a 24-bit depth buffer's steps are not amplified", coarse_depth},
        {"no nudge, no change", [] { const Frame still = Draw(0.0f, 0.0f, 0); CHECK(still.fixed == still.raw); }},
    });
}
