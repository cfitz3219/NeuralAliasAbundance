#pragma once
// The pictures referee_gpu_test.cpp hands to the card, built on the processor (no D3D12 here, so
// the same pictures can be scored on any machine). Sizes are chosen so that no sample place of
// core/referee.hpp lands on a whole pixel: the card and the processor then pick the same pixels.
//   previous input: scene seen with the jitter before
//   input:          scene moved by u, seen with the jitter now, plus a little grain (aliasing)
//   output:         scene moved by u, no jitter (what DLSS should make)
//   screen:         a tone curve over output (or over input for the "screen shows the input" case),
//                   8 bits a channel
//   motion:         -u everywhere (pixels; scale 1)
#include "core/referee.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

namespace referee_scene {

constexpr uint32_t kW = 408, kH = 226;
constexpr float kU[2] = {3.3f, -1.7f};            // scene motion, pixels
constexpr float kJitterBefore[2] = {.25f, -.3f};  // pixels, as DLSS was told
constexpr float kJitterNow[2] = {-.35f, .2f};

inline float Scene(float x, float y)
{
    return .32f + .12f * std::sin(.37f * x + .11f * y) + .10f * std::sin(.23f * y - .29f * x + 1.f) +
           .08f * std::sin(.71f * x) * std::cos(.53f * y) + .05f * std::sin(1.3f * x + .9f * y);
}
inline float Grain(uint32_t x, uint32_t y, uint32_t seed)
{
    uint32_t h = x * 374761393u + y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float((h ^ (h >> 16)) & 0xffff) / 65535.f - .5f;
}
// Pixel (x, y) shows the scene at its centre minus `shift`.
inline na::referee::Image Render(float sx, float sy, float grain, uint32_t seed)
{
    na::referee::Image im;
    im.w = kW; im.h = kH; im.v.resize(size_t(kW) * kH);
    for (uint32_t y = 0; y < kH; ++y)
        for (uint32_t x = 0; x < kW; ++x)
            im.v[size_t(y) * kW + x] = Scene(float(x) + .5f - sx, float(y) + .5f - sy) * (1.f + grain * Grain(x, y, seed));
    return im;
}
inline na::referee::Image Previous() { return Render(kJitterBefore[0], kJitterBefore[1], .04f, 1); }
inline na::referee::Image Input() { return Render(kJitterNow[0] + kU[0], kJitterNow[1] + kU[1], .04f, 2); }
inline na::referee::Image Output() { return Render(kU[0], kU[1], 0.f, 0); }
// The screen as 8-bit values (the card reads value/255), from a linear picture.
inline std::vector<uint8_t> ScreenBytes(const na::referee::Image& from)
{
    std::vector<uint8_t> b(size_t(kW) * kH);
    for (size_t i = 0; i < b.size(); ++i) {
        const float v = from.v[i] * 1.4f;
        const float t = std::pow(v / (1.f + v), 1.f / 2.2f);
        b[i] = uint8_t(std::lround(std::fmin(std::fmax(t, 0.f), 1.f) * 255.f));
    }
    return b;
}
inline na::referee::Image ScreenImage(const std::vector<uint8_t>& b)
{
    na::referee::Image im;
    im.w = kW; im.h = kH; im.v.resize(b.size());
    for (size_t i = 0; i < b.size(); ++i) im.v[i] = float(b[i]) / 255.f;
    return im;
}
inline std::vector<float> Motion()
{
    std::vector<float> m(size_t(kW) * kH * 2);
    for (size_t i = 0; i < size_t(kW) * kH; ++i) { m[i * 2] = -kU[0]; m[i * 2 + 1] = -kU[1]; }
    return m;
}

// What the card should write, from the processor reference.
inline std::vector<float> Expected(const na::referee::Image& prev, const na::referee::Image& input,
                                   const na::referee::Image& output, const na::referee::Image& screen,
                                   const std::vector<float>& motion)
{
    using namespace na::referee;
    std::vector<float> d(kResultFloats, 0.f);
    SampleThumb(input, d.data() + BlockOffset(kThumbInput));
    SampleThumb(output, d.data() + BlockOffset(kThumbOutput));
    SampleThumb(screen, d.data() + BlockOffset(kThumbScreen));
    SampleTiles(input, d.data() + BlockOffset(kTileInput));
    SampleTiles(output, d.data() + BlockOffset(kTileOutput));
    SampleTiles(screen, d.data() + BlockOffset(kTileScreen));
    MotionInputs in;
    in.current = &input; in.previous = &prev; in.motion = &motion;
    in.motion_w = kW; in.motion_h = kH;
    in.jitter_now[0] = kJitterNow[0]; in.jitter_now[1] = kJitterNow[1];
    in.jitter_before[0] = kJitterBefore[0]; in.jitter_before[1] = kJitterBefore[1];
    SamplePoints(in, d.data() + BlockOffset(kPoints));
    return d;
}
inline float JitterChange()
{
    return std::hypot(kJitterNow[0] - kJitterBefore[0], kJitterNow[1] - kJitterBefore[1]);
}
// One sample judged alone, as the referee's log would judge a window holding only it.
inline na::referee::Verdicts JudgeAlone(const std::vector<float>& d)
{
    na::referee::Sample s;
    s.data = d.data();
    s.have_screen = s.tiles_comparable = s.have_motion = true;
    s.jitter_change = JitterChange();
    na::referee::Window w;
    w.Add(na::referee::Score(s));
    return na::referee::Judge(w);
}

} // namespace referee_scene
