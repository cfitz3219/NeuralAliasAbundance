// The frame's own most common search vectors, offered to every block (core/flow_candidates.hpp,
// VARIANT flow/frame-candidates). CI cannot run the real search or the GPU programs, so this runs
// the CPU model of the search (ffx_search_model.hpp) and the CPU reference of the pass on the
// pictures the GPU fixture draws (tests/host/flow_fixture_pictures.hpp).
//
// What must hold:
//  - a picture that repeats, panned: the search loses most of it (R3), the pass gets it back,
//    forward, backward and by half a pixel;
//  - nothing else moves: a picture the search follows, a still picture and a slow pan come out
//    exactly as they went in;
//  - an object moving its own way keeps its vector, and a repeating object is repaired by its own
//    candidate while the background around it is left alone;
//  - the vector with the most votes is not believed for having them (Astra's GPU counterexample,
//    COMMS ff92ac32: strongest peak wrong on 20,003 blocks, the true vector on 2,740).
#include "test_support.hpp"
#include "ffx_search_model.hpp"
#include "../host/flow_fixture_pictures.hpp"
#include "../../src/core/flow_candidates.hpp"

#include <vector>

namespace {
namespace cand = na::motion::candidates;

using PictureFn = float (*)(float, float);

struct Scene {
    int w = 1920, h = 1080;
    PictureFn background = test_pictures::RepeatingWaves;
    float pan = 6; // the background's content moves this many pixels right per frame
    PictureFn object = nullptr;
    float object_dx = 0, object_dy = 0; // the object's own movement per frame
    int ox = 0, oy = 0, ow = 0, oh = 0;
};

struct Frame {
    std::vector<float> stored; // what the pass compares: the 10-bit stabilised picture
    ffx_model::Luma luma;      // what the search is given
};

Frame Draw(const Scene& s, int frame)
{
    Frame f;
    f.stored.resize(size_t(s.w) * size_t(s.h));
    f.luma.w = s.w; f.luma.h = s.h; f.luma.p.resize(f.stored.size());
    for (int y = 0; y < s.h; ++y)
        for (int x = 0; x < s.w; ++x) {
            float z = s.background(float(x) - s.pan * float(frame), float(y));
            if (s.object) {
                const float lx = float(x) - (float(s.ox) + s.object_dx * float(frame)), ly = float(y) - (float(s.oy) + s.object_dy * float(frame));
                if (lx >= 0 && ly >= 0 && lx < float(s.ow) && ly < float(s.oh)) z = s.object(lx + 5000.f, ly + 3000.f);
            }
            z = std::clamp(z, 0.f, 1.f);
            f.stored[size_t(y) * size_t(s.w) + size_t(x)] = std::floor(z * 1023.f + .5f) / 1023.f;
            f.luma.p[size_t(y) * size_t(s.w) + size_t(x)] = uint8_t(std::lround(255.f * z));
        }
    return f;
}

struct Outcome {
    cand::Result result;
    double before = 0, after = 0;               // background blocks within a pixel of the pan
    double object_before = 0, object_after = 0; // object blocks within a pixel of the object's vector
    unsigned background_changed = 0, object_changed = 0;
};

// Blocks well inside the object in the current frame, the ones well outside it, and a band left out.
int Where(const Scene& s, int bx, int by)
{
    if (!s.object) return 0;
    const float x0 = float(bx * 8), y0 = float(by * 8), x1 = x0 + 8, y1 = y0 + 8;
    const float left = float(s.ox) + s.object_dx, top = float(s.oy) + s.object_dy;
    const float m = 24.f + std::fabs(s.pan) + std::fabs(s.object_dx) + std::fabs(s.object_dy);
    if (x0 >= left + m && y0 >= top + m && x1 <= left + float(s.ow) - m && y1 <= top + float(s.oh) - m) return 1;
    if (x1 <= left - m || y1 <= top - m || x0 >= left + float(s.ow) + m || y0 >= top + float(s.oh) + m) return 0;
    return 2;
}

Outcome Run(const Scene& s, const char* name)
{
    const Frame previous = Draw(s, 0), current = Draw(s, 1);
    const ffx_model::Grid g = ffx_model::Flow(current.luma, previous.luma);
    std::vector<cand::Vector> in(g.v.size()), out(g.v.size());
    for (size_t i = 0; i < g.v.size(); ++i) in[i] = cand::Vector{g.v[i].x, g.v[i].y};
    const cand::Picture pc{current.stored.data(), uint32_t(s.w), uint32_t(s.h)}, pp{previous.stored.data(), uint32_t(s.w), uint32_t(s.h)};
    Outcome o;
    o.result = cand::Reconsider(in.data(), uint32_t(g.w), uint32_t(g.h), pc, pp, out.data());
    unsigned n[2]{}, right_before[2]{}, right_after[2]{};
    for (int by = 0; by < g.h; ++by)
        for (int bx = 0; bx < g.w; ++bx) {
            const int where = Where(s, bx, by);
            if (where == 2) continue;
            const size_t i = size_t(by) * size_t(g.w) + size_t(bx);
            const float tx = where ? -s.object_dx : -s.pan, ty = where ? -s.object_dy : 0.f;
            ++n[where];
            if (std::fabs(float(in[i].x) - tx) <= 1.f && std::fabs(float(in[i].y) - ty) <= 1.f) ++right_before[where];
            if (std::fabs(float(out[i].x) - tx) <= 1.f && std::fabs(float(out[i].y) - ty) <= 1.f) ++right_after[where];
            if (in[i].x != out[i].x || in[i].y != out[i].y) ++(where ? o.object_changed : o.background_changed);
        }
    o.before = double(right_before[0]) / double((std::max)(n[0], 1u));
    o.after = double(right_after[0]) / double((std::max)(n[0], 1u));
    o.object_before = double(right_before[1]) / double((std::max)(n[1], 1u));
    o.object_after = double(right_after[1]) / double((std::max)(n[1], 1u));
    std::printf("  %s: background right %.1f %% -> %.1f %% (%u of %u blocks changed)", name, 100. * o.before, 100. * o.after, o.background_changed, n[0]);
    if (s.object) std::printf("; object right %.1f %% -> %.1f %% (%u of %u changed)", 100. * o.object_before, 100. * o.object_after, o.object_changed, n[1]);
    std::printf("\n    candidates:");
    for (uint32_t k = 0; k < o.result.found; ++k) {
        const cand::Vector v = cand::Unpack(o.result.top[k]);
        std::printf(" (%d, %d) x %u", v.x, v.y, o.result.top[k] >> 16);
    }
    std::printf("%s; %u blocks compared, %u took a candidate\n", o.result.found ? "" : " none", o.result.offered, o.result.changed);
    return o;
}
} // namespace

int main()
{
    return test::run({
        {"a vector's key keeps its vector and ranks by support", [] {
            for (const cand::Vector v : {cand::Vector{-127, 127}, cand::Vector{6, 0}, cand::Vector{-7, -94}, cand::Vector{1, -1}}) {
                const cand::Vector back = cand::Unpack(cand::Pack(v, 300));
                CHECK(back.x == v.x && back.y == v.y);
            }
            CHECK(cand::Pack(cand::Vector{127, 127}, 9) < cand::Pack(cand::Vector{-127, -127}, 10));
            CHECK(!cand::Votes(cand::Vector{0, 0}) && !cand::Votes(cand::Vector{-622, 0}) && cand::Votes(cand::Vector{-127, 5}));
            CHECK(cand::MinimumSupport(32400) == 324 && cand::MinimumSupport(100) == 8);
        }},
        {"repeating picture, 6 pixel pan at 1920x1080: lost by the search, recovered", [] {
            Scene s;
            const Outcome o = Run(s, "four-sine, 6 px pan");
            CHECK(o.before < .4);
            CHECK(o.after > .97);
        }},
        {"the same pan backwards (the direction of Astra's counterexample)", [] {
            Scene s; s.pan = -6;
            const Outcome o = Run(s, "four-sine, reverse 6 px pan");
            CHECK(o.before < .4);
            CHECK(o.after > .97);
        }},
        {"a pan of 6.5 pixels: neither whole-pixel neighbour is exact, both are accepted", [] {
            Scene s; s.pan = 6.5f;
            const Outcome o = Run(s, "four-sine, 6.5 px pan");
            CHECK(o.before < .4);
            CHECK(o.after > .95);
        }},
        {"what the search follows is left exactly as it was", [] {
            Scene s; s.pan = 3;
            CHECK(Run(s, "four-sine, 3 px pan").result.changed == 0);
            s.pan = 0;
            const Outcome still = Run(s, "four-sine, still");
            CHECK(still.result.found == 0 && still.result.changed == 0);
            s.pan = .25f;
            CHECK(Run(s, "four-sine, quarter-pixel pan").result.changed == 0); // blocks the search called still stay still
            s.background = test_pictures::NonRepeating; s.pan = 6;
            const Outcome followed = Run(s, "non-repeating, 6 px pan");
            CHECK(followed.result.changed == 0 && followed.after > .99);
        }},
        {"an object moving its own way keeps its vector", [] {
            Scene s; s.background = test_pictures::NonRepeating; s.pan = 6;
            s.object = test_pictures::NonRepeating; s.object_dx = 10; s.object_dy = 4; s.ox = 800; s.oy = 400; s.ow = 320; s.oh = 220;
            const Outcome o = Run(s, "non-repeating, 6 px pan, object (+10, +4)");
            CHECK(o.object_changed == 0 && o.background_changed == 0);
            CHECK(o.object_after > .99 && o.after > .99);
            s.background = test_pictures::RepeatingWaves; // the background is repaired around it, the object still untouched
            const Outcome r = Run(s, "four-sine, 6 px pan, object (+10, +4)");
            CHECK(r.object_changed == 0 && r.object_after > .99);
            CHECK(r.before < .4 && r.after > .97);
        }},
        {"a repeating object moving against the background is repaired by its own candidate", [] {
            Scene s; s.background = test_pictures::NonRepeating; s.pan = 3;
            s.object = test_pictures::RepeatingWaves; s.object_dx = -6; s.object_dy = 0; s.ox = 500; s.oy = 250; s.ow = 900; s.oh = 560;
            const Outcome o = Run(s, "non-repeating 3 px pan, four-sine object moving 6 px the other way");
            CHECK(o.object_before < .6);
            CHECK(o.object_after > .95);
            CHECK(o.background_changed == 0 && o.after > .99);
        }},
        {"the vector with the most votes is not believed for having them", [] {
            // The pictures say (6, 0). Six blocks in ten claim a far vector, one in ten the true one,
            // the rest something else: the common one must not win, and every block must end right.
            Scene s; s.w = 512; s.h = 256; s.background = test_pictures::NonRepeating; s.pan = -6;
            const Frame previous = Draw(s, 0), current = Draw(s, 1);
            const uint32_t gw = 64, gh = 32;
            std::vector<cand::Vector> grid(size_t(gw) * gh), out(grid.size());
            for (size_t i = 0; i < grid.size(); ++i) grid[i] = i % 10 < 6 ? cand::Vector{-100, 3} : i % 10 == 6 ? cand::Vector{6, 0} : cand::Vector{7, 94};
            const cand::Picture pc{current.stored.data(), 512, 256}, pp{previous.stored.data(), 512, 256};
            const cand::Result r = cand::Reconsider(grid.data(), gw, gh, pc, pp, out.data());
            CHECK(r.found == 3);
            const cand::Vector first = cand::Unpack(r.top[0]);
            CHECK(first.x == -100 && first.y == 3); // it is the strongest, and it is offered ...
            unsigned right = 0, common = 0;
            for (const auto& v : out) { if (v.x == 6 && v.y == 0) ++right; if (v.x == -100) ++common; }
            std::printf("  strongest candidate wrong on 60 %% of blocks: %u of %zu blocks end on the true vector, %u on the common one\n", right, out.size(), common);
            // ... but only blocks whose whole 8x8 can be compared may change: the picture's right edge cannot see 6 pixels further right.
            CHECK(right > out.size() * 95 / 100);
            for (size_t i = 0; i < out.size(); ++i) CHECK(i % gw >= gw - 1 || out[i].x == 6);
        }},
    });
}
