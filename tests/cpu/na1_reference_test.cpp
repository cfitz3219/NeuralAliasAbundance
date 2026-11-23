// The NA1 enhancer's picture maths on the processor (docs/NA1-ENHANCER.md sections 2 to 4):
// what each dial does to a known picture, what it must leave alone, and what Measure counts.
#include "core/na1_reference.hpp"
#include "core/referee.hpp"
#include "test_support.hpp"

#include <cstring>
#include <limits>
#include <vector>

using namespace na::na1::ref;

namespace {
struct Picture {
    uint32_t w = 0, h = 0;
    std::vector<float> rgba, depth;
    Picture(uint32_t w_, uint32_t h_) : w(w_), h(h_), rgba(size_t(w_) * h_ * 4, 1.f), depth(size_t(w_) * h_, 0.5f) {}
    float* At(uint32_t x, uint32_t y) { return rgba.data() + (size_t(y) * w + x) * 4; }
    const float* At(uint32_t x, uint32_t y) const { return rgba.data() + (size_t(y) * w + x) * 4; }
    void Fill(float r, float g, float b)
    {
        for (uint32_t i = 0; i < w * h; ++i) { rgba[i * 4] = r; rgba[i * 4 + 1] = g; rgba[i * 4 + 2] = b; }
    }
};
// A repeatable rough surface: two waves and a little noise around `level`, slightly coloured.
float Rough(uint32_t x, uint32_t y)
{
    uint32_t n = x * 374761393u + y * 668265263u;
    n = (n ^ (n >> 13)) * 1274126177u;
    const float noise = float((n ^ (n >> 16)) & 1023u) / 1023.f - .5f;
    return 1.f + .25f * std::sin(float(x) * .9f) * std::cos(float(y) * .7f) + .15f * std::sin(float(x + 2 * y) * .23f) + .2f * noise;
}
void Texture(Picture& p, float level, uint32_t x0 = 0, uint32_t x1 = ~0u)
{
    for (uint32_t y = 0; y < p.h; ++y)
        for (uint32_t x = x0; x < (std::min)(x1, p.w); ++x) {
            const float v = level * Rough(x, y);
            float* o = p.At(x, y);
            o[0] = v * 1.1f; o[1] = v; o[2] = v * .8f;
        }
}
Scene Ordinary(float middle)
{
    Scene s;
    s.mid = LogLum(middle);
    s.m0 = 0.18f;
    s.curve.mode = 2;
    s.curve.mean_y = LogLum(middle);
    return s;
}
float YAt(const std::vector<float>& rgba, size_t i) { return LogLum(Lum(rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2])); }
// Spread of y against the mean of its four neighbours two pixels away: the Referee's "fine".
float FineSpread(const std::vector<float>& rgba, uint32_t w, uint32_t h)
{
    double sum = 0, sum2 = 0;
    uint32_t n = 0;
    for (uint32_t y = 2; y + 2 < h; ++y)
        for (uint32_t x = 2; x + 2 < w; ++x) {
            const size_t i = size_t(y) * w + x;
            const double f = YAt(rgba, i) - .25 * (YAt(rgba, i - 2) + YAt(rgba, i + 2) + YAt(rgba, i - 2 * w) + YAt(rgba, i + 2 * w));
            sum += f; sum2 += f * f; ++n;
        }
    return float(std::sqrt(sum2 / n - (sum / n) * (sum / n)));
}
bool Same(const std::vector<float>& a, const std::vector<float>& b) { return !std::memcmp(a.data(), b.data(), a.size() * sizeof(float)); }
std::vector<float> Run(const Picture& p, const Dials& d, const Scene& s, const float* da = nullptr, const float* db = nullptr, bool reversed = false)
{
    std::vector<float> out(p.rgba.size(), -1.f);
    Enhance(p.rgba.data(), p.depth.data(), p.w, p.h, reversed, d, s, da, db, out.data());
    return out;
}
std::vector<float> RunPasses(const Picture& p, const Dials& d, const Scene& s, const float* da = nullptr, const float* db = nullptr, bool srgb = false)
{
    Pyramid pyramid;
    BuildPyramid(p.rgba.data(), p.depth.data(), p.w, p.h, false, pyramid, srgb);
    std::vector<float> out(p.rgba.size(), -1.f);
    Compose(p.rgba.data(), pyramid, d, s, da, db, out.data(), srgb);
    return out;
}
}

int main()
{
    return test::run({
        {"with every dial at zero the picture comes back bit for bit, with or without the passes running", [] {
            Picture p(48, 40);
            Texture(p, .3f);
            p.At(3, 3)[0] = std::numeric_limits<float>::quiet_NaN();  // even a pixel that is not a number is left as it was
            p.At(5, 5)[3] = 123.5f;
            CHECK(Same(Run(p, Dials(), Ordinary(.3f)), p.rgba));
            CHECK(Same(RunPasses(p, Dials(), Ordinary(.3f)), p.rgba));
        }},
        {"a dial at zero contributes nothing: layers with zero dials and a zero light or colour change no bit", [] {
            Picture p(48, 40);
            Texture(p, .3f);
            std::vector<float> layer(p.depth.size(), -.4f);
            Dials only_texture;
            only_texture.texture = .8f;
            const auto reference = RunPasses(p, only_texture, Ordinary(.3f));
            CHECK(!Same(reference, p.rgba));
            CHECK(Same(RunPasses(p, only_texture, Ordinary(.3f), layer.data(), layer.data()), reference));
            CHECK(Same(RunPasses(p, Dials(), Ordinary(.3f), layer.data(), layer.data()), p.rgba));
            CHECK(Same(Run(p, Dials(), Ordinary(.3f), layer.data(), layer.data()), p.rgba));
        }},
        {"a flat picture is not changed by the texture dial", [] {
            Picture p(40, 32);
            p.Fill(.33f, .3f, .2f);
            Dials d;
            d.texture = 1.5f;
            const auto out = Run(p, d, Ordinary(.3f));
            for (size_t i = 0; i < out.size(); ++i) NEAR(out[i], p.rgba[i], 1e-5);
        }},
        {"the blur keeps a constant, keeps a depth step sharp, and levels halve rounding up", [] {
            const uint32_t w = 21, h = 13;
            std::vector<float> yq(size_t(w) * h * 2), bx(size_t(w) * h), b(size_t(w) * h);
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x) {
                    yq[(size_t(y) * w + x) * 2] = x < 10 ? -3.f : 1.f;      // two surfaces, each flat
                    yq[(size_t(y) * w + x) * 2 + 1] = x < 10 ? .5f : .1f;   // at different distances
                }
            BlurX(yq.data(), w, h, 6, 1, bx.data());
            BlurY(bx.data(), yq.data(), w, h, 6, 1, b.data());
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x) NEAR(b[size_t(y) * w + x], x < 10 ? -3.f : 1.f, 1e-5);
            CHECK(HalfSize(21) == 11 && HalfSize(13) == 7 && HalfSize(1) == 1);
            std::vector<float> half(size_t(11) * 7 * 2);
            Down(b.data(), yq.data(), w, h, half.data());
            // The half texel that straddles the step (x 10 is far, x 11 is far; x 8,9 near) keeps the near surface.
            NEAR(half[(size_t(3) * 11 + 4) * 2], -3.f, 1e-5);
            NEAR(half[(size_t(3) * 11 + 4) * 2 + 1], .5f, 1e-7);
            NEAR(half[(size_t(3) * 11 + 5) * 2], 1.f, 1e-5);
            NEAR(half[(size_t(6) * 11 + 10) * 2], 1.f, 1e-5);  // the last column and row repeat the edge
        }},
        {"the texture dial raises fine contrast", [] {
            Picture p(96, 64);
            Texture(p, .2f);
            Dials d;
            d.texture = 1.f;
            const float before = FineSpread(p.rgba, p.w, p.h), after = FineSpread(Run(p, d, Ordinary(.2f)), p.w, p.h);
            CHECK(after > before * 1.1f);
            CHECK(after < before * 1.6f);  // the top layer gain is 1.5 and large detail gains less
        }},
        {"texture does not cross a depth step: a flat sky beside a rough object gets no glow", [] {
            Picture p(96, 64);
            p.Fill(.5f, .5f, .5f);
            Texture(p, .15f, 48);
            for (uint32_t y = 0; y < p.h; ++y)
                for (uint32_t x = 0; x < 48; ++x) p.depth[size_t(y) * p.w + x] = 1.f;  // sky: as far as depth goes
            Dials d;
            d.texture = 1.5f;
            const auto out = Run(p, d, Ordinary(.2f));
            float sky = 0, object = 0;
            for (uint32_t y = 0; y < p.h; ++y)
                for (uint32_t x = 0; x < p.w; ++x) {
                    const size_t i = size_t(y) * p.w + x;
                    const float change = std::fabs(YAt(out, i) - YAt(p.rgba, i));
                    (x < 48 ? sky : object) = (std::max)(x < 48 ? sky : object, change);
                }
            CHECK(object > .1f);
            CHECK(sky < .003f);  // a tenth of the 0.03-stop glow limit
        }},
        {"near white nothing is made brighter and near black nothing is made darker", [] {
            Scene s;
            s.curve.mode = 0;  // the picture is what the screen shows
            s.m0 = 0.18f;
            Dials d;
            d.texture = 1.5f;
            Picture bright(64, 48);
            Texture(bright, .975f);
            for (float& v : bright.rgba) v = (std::clamp)(v, .96f, 1.2f);
            auto out = Run(bright, d, s);
            bool darker = false;
            for (size_t i = 0; i < out.size(); ++i) {
                CHECK(out[i] <= bright.rgba[i]);
                darker = darker || out[i] < bright.rgba[i];
            }
            CHECK(darker);
            Picture dark(64, 48);
            Texture(dark, .0012f);  // under m0/64 on the screen
            out = Run(dark, d, s);
            bool brighter = false;
            for (size_t i = 0; i < out.size(); ++i) {
                CHECK(out[i] >= dark.rgba[i]);
                brighter = brighter || out[i] > dark.rgba[i];
            }
            CHECK(brighter);
        }},
        {"alpha passes through bit for bit whatever the dials do", [] {
            Picture p(40, 32);
            Texture(p, .3f);
            for (uint32_t i = 0; i < p.w * p.h; ++i) p.rgba[size_t(i) * 4 + 3] = i % 7 == 0 ? std::numeric_limits<float>::infinity() : float(i) * .37f - 100.f;
            Dials d;
            d.texture = 1.2f; d.light = .7f; d.colour = .9f; d.key = -.3f;
            const auto out = Run(p, d, Ordinary(.3f));
            for (uint32_t i = 0; i < p.w * p.h; ++i) CHECK(!std::memcmp(&out[size_t(i) * 4 + 3], &p.rgba[size_t(i) * 4 + 3], sizeof(float)));
            CHECK(!Same(out, p.rgba));
        }},
        {"the light dial lifts what is under the middle and lowers what is over it, and leaves sky alone", [] {
            Picture p(256, 32);
            for (uint32_t y = 0; y < p.h; ++y)
                for (uint32_t x = 0; x < p.w; ++x) {
                    const float v = x < 128 ? .03f : .4f;
                    float* o = p.At(x, y);
                    o[0] = o[1] = o[2] = v;
                }
            Dials d;
            d.light = 1.f;
            const Scene s = Ordinary(.11f);
            auto out = Run(p, d, s);
            // Far from the border each half sees only itself: the change is 0.35 of its distance from the middle.
            const float dark_gain = out[(size_t(16) * p.w + 8) * 4] / .03f, bright_gain = out[(size_t(16) * p.w + 248) * 4] / .4f;
            NEAR(std::log2(dark_gain), -.35f * (LogLum(.03f) - s.mid), 2e-3);
            NEAR(std::log2(bright_gain), -.35f * (LogLum(.4f) - s.mid), 2e-3);
            CHECK(dark_gain > 1.2f && bright_gain < .85f);
            for (float& z : p.depth) z = 1.f;
            CHECK(Same(Run(p, d, s), p.rgba));          // sky, standard depth
            for (float& z : p.depth) z = 0.f;
            CHECK(Same(Run(p, d, s, nullptr, nullptr, true), p.rgba));  // sky, reversed depth
            CHECK(!Same(Run(p, d, s), p.rgba));         // the same numbers read the other way are solid
        }},
        {"colour grows around grey in an ordinary scene and around the scene's tint in a dark one", [] {
            Picture p(8, 8);
            const float tint[3] = {.8f, 1.02f, 1.39f};  // a blue night; dot(tint, weights) is about 1
            p.Fill(.1f, .1f, .1f);
            for (uint32_t y = 0; y < 8; ++y)
                for (uint32_t x = 4; x < 8; ++x)
                    for (int c = 0; c < 3; ++c) p.At(x, y)[c] = .1f * tint[c];
            Dials d;
            d.colour = 1.f;
            Scene s = Ordinary(.1f);
            auto out = Run(p, d, s);
            const float* const grey = &out[0];  // out is reused below at the same size, so these stay valid
            const float* const cast = &out[(size_t(4) * 8 + 6) * 4];
            for (int c = 0; c < 3; ++c) NEAR(grey[c], .1f, 1e-6);  // grey stays grey
            CHECK(cast[2] / cast[1] > tint[2] / tint[1] * 1.05f);  // the cast itself is amplified
            s.w_dark = 1.f;
            for (int c = 0; c < 3; ++c) s.tint[c] = tint[c] / Lum(tint[0], tint[1], tint[2]);
            Enhance(p.rgba.data(), p.depth.data(), p.w, p.h, false, d, s, nullptr, nullptr, out.data());
            for (int c = 0; c < 3; ++c) NEAR(cast[c], .1f * tint[c], 2e-5);  // the mood colour is not amplified
            CHECK(grey[0] > grey[2] * 1.2f);                                 // and what differs from it stands out more
        }},
        {"the key dial multiplies the picture by exactly two to the key", [] {
            Picture p(24, 16);
            Texture(p, .3f);
            Dials d;
            d.key = .5f;
            const auto out = Run(p, d, Ordinary(.3f));
            const float gain = std::exp2(.5f);
            for (uint32_t i = 0; i < p.w * p.h; ++i)
                for (int c = 0; c < 3; ++c) CHECK(out[size_t(i) * 4 + c] == p.rgba[size_t(i) * 4 + c] * gain);
        }},
        {"lighting layers are added in stops, each times its own dial", [] {
            Picture p(24, 16);
            p.Fill(.2f, .2f, .2f);
            std::vector<float> a(p.depth.size(), -.1f), b(p.depth.size(), -.3f);
            b[5] = std::numeric_limits<float>::quiet_NaN();  // a layer value that is not a number is no change
            Dials d;
            d.part[0] = .5f;
            auto out = Run(p, d, Ordinary(.2f), a.data(), b.data());
            for (uint32_t i = 0; i < p.w * p.h; ++i) NEAR(out[size_t(i) * 4], .2f * std::exp2(-.05f), 1e-6);
            d.part[1] = 1.f;
            out = Run(p, d, Ordinary(.2f), a.data(), b.data());
            for (uint32_t i = 0; i < p.w * p.h; ++i) NEAR(out[size_t(i) * 4 + 1], .2f * std::exp2(i == 5 ? -.05f : -.35f), 1e-6);
            out = Run(p, d, Ordinary(.2f), nullptr, b.data());
            NEAR(out[0], .2f * std::exp2(-.3f), 1e-6);
        }},
        {"an 8-bit picture is worked on in linear light and unchanged pixels keep their stored bits", [] {
            for (float v : {0.f, .001f, .04f, .2f, .5f, 1.f}) NEAR(SrgbEncode(SrgbDecode(v)), v, 1e-6);
            NEAR(SrgbDecode(.5f), .21404f, 1e-5);
            Picture p(24, 16);
            for (uint32_t i = 0; i < p.w * p.h; ++i)
                for (int c = 0; c < 3; ++c) p.rgba[size_t(i) * 4 + c] = float((i * 7 + uint32_t(c) * 31) % 256) / 255.f;
            Scene s;
            s.curve.mode = 0;
            std::vector<float> out(p.rgba.size());
            EnhanceSrgb8(p.rgba.data(), p.depth.data(), p.w, p.h, false, Dials(), s, nullptr, nullptr, out.data());
            CHECK(Same(out, p.rgba));
            for (float& z : p.depth) z = 1.f;
            Dials light;
            light.light = 1.f;  // the passes run, but nothing in a sky changes: no decode and encode round trip
            EnhanceSrgb8(p.rgba.data(), p.depth.data(), p.w, p.h, false, light, s, nullptr, nullptr, out.data());
            CHECK(Same(out, p.rgba));
            Dials key;
            key.key = -.2f;
            EnhanceSrgb8(p.rgba.data(), p.depth.data(), p.w, p.h, false, key, s, nullptr, nullptr, out.data());
            for (size_t i = 0; i < out.size(); ++i)
                if (i % 4 != 3) NEAR(out[i], SrgbEncode(SrgbDecode(p.rgba[i]) * std::exp2(-.2f)), 1e-6);
        }},
        {"screen brightness: display-referred, a fitted table, and the assumed curve", [] {
            Curve c;
            c.mode = 0;
            NEAR(Shown(c, LogLum(.25f)), .25f, 1e-6);
            NEAR(Shown(c, LogLum(4.f)), 1.f, 1e-6);
            c.mode = 1; c.x0 = -4.f; c.dx = .5f;
            for (int i = 0; i < 16; ++i) c.cy[i] = float(i) / 20.f;
            NEAR(Shown(c, -10.f), 0.f, 1e-7);            // held below the first knot
            NEAR(Shown(c, -4.f + .5f * 3.5f), 3.5f / 20.f, 1e-6);
            NEAR(Shown(c, 3.5f), 15.f / 20.f, 1e-7);     // the last knot
            NEAR(Shown(c, 30.f), 15.f / 20.f, 1e-7);     // and held above it
            c.mode = 2; c.mean_y = -3.f;
            NEAR(Shown(c, -3.f), .18f / 1.18f, 1e-6);
            NEAR(Shown(c, -2.f), .36f / 1.36f, 1e-6);
        }},
        {"Measure: one flat colour gives sums that can be worked out by hand", [] {
            Picture b(512, 288), a(512, 288);  // cells of 4 x 4 pixels, sampled every other pixel: 4 samples
            b.Fill(.2f, .4f, .1f);
            a.Fill(.4f, .8f, .2f);
            Scene s;
            s.curve.mode = 0;
            std::vector<float> cells(size_t(kCellsW) * kCellsH * kCellFloats, -1.f);
            Measure(b.rgba.data(), a.rgba.data(), b.depth.data(), b.w, b.h, false, s, 0.f, cells.data());
            const float lum = .2f * .2126f + .4f * .7152f + .1f * .0722f;
            for (uint32_t cell : {0u, 77u * 100u, kCellsW * kCellsH - 1}) {
                const float* c = cells.data() + size_t(cell) * kCellFloats;
                NEAR(c[kCount], 4, 0);
                NEAR(c[kYBefore], 4 * std::log2(lum + 1e-3f), 1e-5);
                NEAR(c[kYAfter], 4 * std::log2(2 * lum + 1e-3f), 1e-5);
                NEAR(c[kShownBefore], 4 * lum, 1e-5);
                NEAR(c[kShownAfter], 8 * lum, 1e-5);
                NEAR(c[kBlackBefore] + c[kBlackAfter] + c[kWhiteBefore] + c[kWhiteAfter], 0, 0);
                NEAR(c[kSolid], 4, 0);
                NEAR(c[kSolidY], 4 * std::log2(lum + 1e-3f), 1e-5);
                NEAR(c[kSolidR], .8f, 1e-6); NEAR(c[kSolidG], 1.6f, 1e-6); NEAR(c[kSolidB], .4f, 1e-6);
                NEAR(c[kStrengthBefore], 4 * .3f / .4001f, 1e-5);
                NEAR(c[kStrengthAfter], 4 * .6f / .8001f, 1e-5);
                const float chroma[3] = {.2f / (lum + 1e-5f) - 1, .4f / (lum + 1e-5f) - 1, .1f / (lum + 1e-5f) - 1};
                NEAR(c[kVarietyBefore], 4 * std::sqrt(chroma[0] * chroma[0] + chroma[1] * chroma[1] + chroma[2] * chroma[2]), 1e-4);
                NEAR(c[kFineBefore], 0, 1e-5); NEAR(c[kFineBefore2], 0, 1e-9);
                NEAR(c[kFineAfter], 0, 1e-5); NEAR(c[kFineAfter2], 0, 1e-9);
                NEAR(c[kBand], 0, 0); NEAR(c[kBandGlow], 0, 0);
            }
        }},
        {"Measure: black, white, sky, the band beside an object and the fine term are counted where they are", [] {
            Picture b(512, 288);
            b.Fill(0, 0, 0);
            for (uint32_t y = 0; y < b.h; ++y)
                for (uint32_t x = 256; x < 512; ++x) {
                    b.At(x, y)[0] = b.At(x, y)[1] = b.At(x, y)[2] = 1.f;  // the right half: white sky
                    b.depth[size_t(y) * b.w + x] = 1.f;
                }
            b.At(100, 100)[1] = .5f;  // one green spot on the black object, on a sampled pixel
            Picture a = b;
            for (uint32_t i = 0; i < a.w * a.h * 4; ++i) a.rgba[i] *= 2.f;
            Scene s;
            s.curve.mode = 0;
            std::vector<float> cells(size_t(kCellsW) * kCellsH * kCellFloats);
            const float key = .25f;
            Measure(b.rgba.data(), a.rgba.data(), b.depth.data(), b.w, b.h, false, s, key, cells.data());
            double total[kCellFloats] = {};
            for (size_t i = 0; i < cells.size(); ++i) total[i % kCellFloats] += cells[i];
            const double samples = 256.0 * 144.0;
            NEAR(total[kCount], samples, 0);
            NEAR(total[kSolid], samples / 2, 0);
            NEAR(total[kBlackBefore], samples / 2 - 1, 0);  // the object, less the green spot
            NEAR(total[kWhiteBefore], samples / 2, 0);
            NEAR(total[kWhiteAfter], samples / 2 + 1, 0);  // doubled, the spot's green channel reaches full scale too
            // Sky with something solid 8 pixels away but not 3: columns 259..263, of which 260 and 262 are sampled.
            NEAR(total[kBand], 2 * 144, 0);
            NEAR(total[kBandGlow], 2 * 144 * std::fabs(std::log2(2.001f) - std::log2(1.001f) - key), 1e-2);
            // The spot stands out from its four neighbours two pixels away, and each of them sees it once.
            const float spot = std::log2(.5f * .7152f + 1e-3f) - std::log2(1e-3f);
            // So does the object's last sampled column (254), which sees the white sky two pixels on.
            const float edge = .25f * (std::log2(1.001f) - std::log2(1e-3f));
            NEAR(total[kFineBefore], spot - 4 * .25f * spot - 144 * edge, 2e-2);
            NEAR(total[kFineBefore2], spot * spot + 4 * (.25f * spot) * (.25f * spot) + 144 * edge * edge, 5e-2);
            const float* c = cells.data() + (size_t(25) * kCellsW + 25) * kCellFloats;  // the spot's own cell
            NEAR(c[kSolidG], .5f, 1e-7);
            NEAR(c[kStrengthBefore], .5f / .5001f, 1e-6);
        }},
        {"Measure: a small picture leaves cells empty without reading outside it", [] {
            Picture p(50, 30);
            Texture(p, .3f);
            std::vector<float> cells(size_t(kCellsW) * kCellsH * kCellFloats, -1.f);
            Measure(p.rgba.data(), p.rgba.data(), p.depth.data(), p.w, p.h, false, Ordinary(.3f), 0.f, cells.data());
            double count = 0;
            for (size_t i = 0; i < cells.size(); i += kCellFloats) count += cells[i];
            CHECK(count > 0 && count <= 50.0 * 30.0);
            for (float v : cells) CHECK(std::isfinite(v));
        }},
        {"the existing Referee still recognises the enhanced picture as the same picture", [] {
            // The Referee matches the screen against the DLSS output by edge correlation and calls
            // 0.3 "seen". NA1 changes the screen after DLSS; even at the top of every dial the two
            // must stay far above that.
            Picture p(256, 144);
            Texture(p, .2f);
            for (uint32_t y = 0; y < p.h; ++y)
                for (uint32_t x = 0; x < p.w; ++x) {
                    const float shade = x / 64 % 2 ? .25f : 1.f;  // lit and shadowed slabs at different distances
                    for (int c = 0; c < 3; ++c) p.At(x, y)[c] *= shade;
                    p.depth[size_t(y) * p.w + x] = y < 20 ? 1.f : x / 64 % 2 ? .6f : .9f;
                }
            Dials d;
            d.texture = d.light = d.colour = 1.5f;
            d.key = .5f;
            const auto out = Run(p, d, Ordinary(.1f));
            CHECK(!Same(out, p.rgba));
            const auto log_picture = [&](const std::vector<float>& rgba) {
                std::vector<float> v(p.depth.size());
                for (size_t i = 0; i < v.size(); ++i) v[i] = std::log2(Lum(rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2]) + na::referee::kLumaFloor);
                return v;
            };
            const auto before = log_picture(p.rgba), after = log_picture(out);
            CHECK(na::referee::EdgeCorrelation(before.data(), after.data(), p.w, p.h) > .8f);  // measured 0.89
            // And as the Referee's 128 x 72 thumbnails (here 2 x 2 pixels a cell).
            const auto thumb = [&](const std::vector<float>& v) {
                std::vector<float> t(size_t(na::referee::kThumbW) * na::referee::kThumbH);
                for (uint32_t y = 0; y < na::referee::kThumbH; ++y)
                    for (uint32_t x = 0; x < na::referee::kThumbW; ++x) {
                        const size_t i = size_t(y) * 2 * p.w + x * 2;
                        t[size_t(y) * na::referee::kThumbW + x] = .25f * (v[i] + v[i + 1] + v[i + p.w] + v[i + p.w + 1]);
                    }
                return t;
            };
            const auto tb = thumb(before), ta = thumb(after);
            CHECK(na::referee::EdgeCorrelation(tb.data(), ta.data(), na::referee::kThumbW, na::referee::kThumbH) > .8f);
        }},
    });
}
