// The referee's scoring (core/referee.hpp) on pictures made here, where the truth is known:
//   picture: the lit scene through a tone curve and a HUD matches the screen; a different layer
//            (other edges) and a layer without edges do not;
//   result:  a screen made from DLSS's output says so; a screen made from the input before DLSS
//            says that instead;
//   motion:  the right vectors predict the frame; no vectors while the picture moves, and half
//            vectors, are caught; a still scene is not judged;
//   jitter:  a picture rendered with the jitter DLSS was told follows it; rendered without it, or
//            with the opposite sign, is caught.
#include "test_support.hpp"
#include "core/referee_selection.hpp"
#include "core/referee_recovery.hpp"
#include "core/referee_placement.hpp"
#include "host_dx12/native11/input_provider.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace na::referee;

namespace {
constexpr uint32_t W = 640, H = 360;

// A scene with texture at several scales and hard edges (the edges alias without anti-aliasing).
float Scene(float x, float y)
{
    float v = .06f + .35f * (.5f + .5f * std::sin(.37f * x + .11f * y)) * (.5f + .5f * std::cos(.23f * y - .05f * x));
    v += std::sin(.09f * x + .31f * y) > 0 ? .3f : 0.f;
    v += std::sin(.021f * x - .017f * y) > .4f ? .25f : 0.f;
    v += .1f * std::sin(.6f * x) * std::sin(.45f * y);
    return v;
}
// Another layer: edges of its own, unrelated to the scene's.
float OtherLayer(float x, float y) { return .3f + (std::sin(.13f * x - .29f * y) > 0 ? .4f : 0.f) + .2f * std::cos(.71f * y); }
// A layer with no edges to speak of.
float SmoothLayer(float x, float y) { return .5f + .4f * std::sin(.004f * x) * std::cos(.006f * y); }

template <class F>
Image Render(F f, float jx, float jy, float ux = 0, float uy = 0)
{
    // Pixel p shows the scene point at its centre minus the jitter; the scene moved by u.
    Image im{W, H, std::vector<float>(size_t(W) * H)};
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) im.v[size_t(y) * W + x] = f(float(x) + .5f - jx - ux, float(y) + .5f - jy - uy);
    return im;
}
template <class F>
Image RenderSmooth(F f)
{
    Image im{W, H, std::vector<float>(size_t(W) * H)};
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            float s = 0;
            for (int b = 0; b < 4; ++b)
                for (int a = 0; a < 4; ++a) s += f(float(x) + (float(a) + .5f) / 4, float(y) + (float(b) + .5f) / 4);
            im.v[size_t(y) * W + x] = s / 16;
        }
    return im;
}
// The game's tone curve, a small grain and a HUD panel over the upper left of the screen.
Image Screen(const Image& from)
{
    Image s = from;
    uint32_t seed = 12345;
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            float v = from.v[size_t(y) * W + x] * 1.4f;
            v = std::pow(v / (1 + v), 1 / 2.2f);
            seed = seed * 1664525u + 1013904223u;
            v += (float(seed >> 8) / 16777216.f - .5f) * .01f;
            if (x >= 100 && x < 180 && y >= 80 && y < 140) v = .8f;
            s.v[size_t(y) * W + x] = v;
        }
    return s;
}

struct Built {
    std::vector<float> data = std::vector<float>(kResultFloats, 0.f);
    Sample sample;
};
Built Build(const Image& input, const Image& output, const Image* screen, const MotionInputs* motion)
{
    Built b;
    SampleThumb(input, b.data.data() + BlockOffset(kThumbInput));
    SampleThumb(output, b.data.data() + BlockOffset(kThumbOutput));
    SampleTiles(input, b.data.data() + BlockOffset(kTileInput));
    SampleTiles(output, b.data.data() + BlockOffset(kTileOutput));
    if (screen) {
        SampleThumb(*screen, b.data.data() + BlockOffset(kThumbScreen));
        SampleTiles(*screen, b.data.data() + BlockOffset(kTileScreen));
    }
    if (motion) SamplePoints(*motion, b.data.data() + BlockOffset(kPoints));
    b.sample.data = b.data.data();
    b.sample.have_screen = screen != nullptr;
    b.sample.tiles_comparable = screen && screen->w == output.w && screen->h == output.h;
    b.sample.have_motion = motion != nullptr;
    if (motion) b.sample.jitter_change = std::hypot(motion->jitter_now[0] - motion->jitter_before[0], motion->jitter_now[1] - motion->jitter_before[1]);
    return b;
}
Verdicts JudgeOne(const Built& b, Window* keep = nullptr)
{
    Window w;
    w.Add(Score(b.sample));
    if (keep) *keep = w;
    return Judge(w);
}
std::vector<float> Vectors(float vx, float vy)
{
    std::vector<float> m(size_t(W) * H * 2);
    for (size_t i = 0; i < m.size(); i += 2) { m[i] = vx; m[i + 1] = vy; }
    return m;
}
} // namespace

int main()
{
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("%s: %s\n", ok ? "ok" : "FAIL", what);
        if (!ok) ++failures;
    };
    auto line = [](const Window& w, const Verdicts& v) {
        static char buf[2048];
        Format(buf, sizeof buf, w, v);
        std::printf("  %s\n", buf);
    };

    static_assert(BlockOffset(kThumbOutput) == kThumbFloats, "layout");
    static_assert(BlockOffset(kPoints) == 3 * kThumbFloats + 3 * kTileFloats, "layout");
    static_assert(kResultFloats == 3 * kThumbFloats + 3 * kTileFloats + kPointFloats, "layout");

    { // A changing light can have the same frequency as the raster sample.
        const auto patch = [](float tx, float ty, float log_light, bool other = false) {
            std::array<float, kTile * kTile> pixels;
            for (unsigned y = 0; y < kTile; ++y) for (unsigned x = 0; x < kTile; ++x) {
                const float px = x - tx, py = y - ty;
                pixels[y * kTile + x] = log_light + (other ? std::sin(.9f * px + .5f * py) :
                    .3f * std::sin(.19f * px + .1f * py) + .4f * std::cos(.3f * py - .09f * px));
            }
            return pixels;
        };
        const auto original = patch(0, 0, 0);
        const auto flash = patch(0, 0, .5f);
        const auto shifted = patch(.36f, -.52f, .5f);
        const auto unrelated = patch(0, 0, 0, true);
        const auto flash_shift = MeasureTileShift(original.data(), flash.data());
        const auto translation = MeasureTileShift(original.data(), shifted.data());
        check(flash_shift.valid && std::hypot(flash_shift.x, flash_shift.y) < .001f,
              "geometry: a half-stop light shift does not move edges");
        check(translation.valid && std::fabs(translation.x - .36f) < .01f &&
              std::fabs(translation.y + .52f) < .01f && !MeasureTileShift(original.data(), unrelated.data()).valid,
              "geometry: recover subpixel translation under a flash, reject an unrelated picture");
        JitterVibration leaking, steady, camera;
        for (unsigned i = 0; i < 8; ++i) {
            const float x = i & 1 ? -.48f : .48f, y = i & 2 ? -.37f : .37f;
            const auto in = patch(x + .07f, y - .05f, .1f);
            const auto bad = patch(x * .4f + .07f, y * .4f - .05f, .5f);
            const auto correct = patch(.07f, -.05f, .5f);
            JitterDisplacement d{MeasureTileShift(original.data(), in.data()),
                MeasureTileShift(original.data(), bad.data()), MeasureTileShift(original.data(), bad.data()), x, y};
            leaking.Add(d);
            d.reconstructed = d.presented = MeasureTileShift(original.data(), correct.data());
            steady.Add(d);
            d.presented.x += .24f;
            camera.Add(d);
        }
        check(leaking.Leakage(true) > .22f && leaking.Leakage(false) > .22f &&
              steady.Leakage(false) == 0 && camera.Leakage(false) == 0,
              "geometry: judge phase-locked movement across both axes and exclude constant camera motion");
    }

    const float jx = .3f, jy = -.35f;
    const Image input = Render(Scene, jx, jy);       // what DLSS was given: one sample a pixel, jittered
    const Image output = RenderSmooth(Scene);         // what DLSS made: anti-aliased, unjittered
    const Image screen_out = Screen(output);          // the screen, made from DLSS's output
    const Image screen_in = Screen(input);            // the screen, made from the image before DLSS

    { // the right picture, and its output reached the screen
        Window w;
        const Verdicts v = JudgeOne(Build(input, output, &screen_out, nullptr), &w);
        line(w, v);
        check(v.picture == Verdict::Pass, "picture: the lit scene through a tone curve and a HUD matches the screen");
        check(v.result == Verdict::Pass, "result: a screen made from DLSS's output shows the output");
        check(v.motion == Verdict::NotMeasured && v.jitter == Verdict::NotMeasured, "no pair: motion and jitter not measured");
    }
    { // the output was lost: the screen shows the image before DLSS
        Window w;
        const Verdicts v = JudgeOne(Build(input, output, &screen_in, nullptr), &w);
        line(w, v);
        check(v.picture == Verdict::Pass, "picture: still the right picture");
        check(v.result == Verdict::Fail && w.total.tiles_input > w.total.tiles_output, "result: a screen made from the input shows the input, not the output");
    }
    { // DLSS ran on another layer (its own edges): the screen does not look like it
        const Image layer = Render(OtherLayer, jx, jy);
        Window w;
        const Verdicts v = JudgeOne(Build(layer, RenderSmooth(OtherLayer), &screen_out, nullptr), &w);
        line(w, v);
        check(v.picture == Verdict::Fail, "picture: another layer does not look like the screen");
        check(v.result == Verdict::Fail, "result: the screen shows neither that input nor that output");
    }
    { // DLSS ran on a layer without edges
        const Image layer = Render(SmoothLayer, jx, jy);
        Window w;
        const Verdicts v = JudgeOne(Build(layer, layer, &screen_out, nullptr), &w);
        line(w, v);
        check(v.picture == Verdict::Fail, "picture: a layer without edges does not look like the screen");
    }
    { // no screen sampled
        const Verdicts v = JudgeOne(Build(input, output, nullptr, nullptr));
        check(v.picture == Verdict::NotMeasured && v.result == Verdict::NotMeasured, "no screen: picture and result not measured");
    }

    {
        Window window;
        SampleScore ambiguous;
        ambiguous.tiles = ambiguous.tiles_textured = 8;
        SampleScore output_vote = ambiguous;
        output_vote.tiles_output = 8;
        window.Add(output_vote);
        for (int frame = 0; frame < 4; ++frame) window.Add(ambiguous);
        check(Judge(window).result == Verdict::Partial, "result: one decisive frame cannot speak for five");
        for (int frame = 0; frame < 3; ++frame) window.Add(output_vote);
        check(Judge(window).result == Verdict::Pass, "result: enough agreeing frames support a sampled pass");
    }

    {
        using namespace na::referee::shadow;
        const auto output_sample = Build(input, output, &screen_out, nullptr);
        const auto input_sample = Build(input, output, &screen_in, nullptr);
        const auto good = Score(output_sample.sample), bypass = Score(input_sample.sample);
        const Placement between{Path::Between, 0}, inline_route{Path::Inline, 31};
        Evidence current, alternative;
        current.context = {inline_route, 20, 21, 3, 0, W, H};
        alternative.context = {between, 20, 21, 3, 0, W, H};
        current.Add(good);
        check(!std::strcmp(Preview(current).action, "hold"), "shadow: one good sample cannot certify a route");
        for (int frame = 0; frame < 2; ++frame) current.Add(good);
        for (int frame = 0; frame < 3; ++frame) alternative.Add(good);
        check(!std::strcmp(Preview(current, &alternative).action, "keep"), "shadow: keep a verified current route");
        current.Add(bypass);
        check(!std::strcmp(Preview(current).action, "check_continuity") && Judge(current.measured).result == Verdict::Partial,
              "shadow: three visible outputs and a confirmed bypass report intermittent delivery");
        current.measured = {};
        for (int frame = 0; frame < 3; ++frame) current.Add(bypass);
        check(!std::strcmp(Preview(current).action, "check_placement"), "shadow: bypass alone does not establish another boundary");
        check(!std::strcmp(Preview(current, &alternative).action, "prefer"), "shadow: a verified same-scene alternative beats a bypassed route");
        ++alternative.context.generation;
        check(!std::strcmp(Preview(current, &alternative).action, "check_placement"), "shadow: a reset makes an older alternative ineligible");
        --alternative.context.generation;
        alternative.context.color++;
        check(!std::strcmp(Preview(current, &alternative).action, "check_placement"), "shadow: an alternative for another scene cannot replace the current one");
        RecentSamples recent;
        Context producer = current.context, different = producer;
        different.color++;
        Context other = producer;
        other.placement = between;
        uint64_t ordinal = 0, first;
        for (int repeat = 0; repeat < 3; ++repeat) {
            recent.Add(producer, ++ordinal, bypass, between);
            recent.Add(different, ++ordinal, bypass);
            recent.Add(other, ++ordinal, good);
        }
        const auto candidates = recent.For(producer, first);
        check(first == 1 && candidates.measured.samples == 3 &&
              !std::strcmp(recent.Choose(candidates).action, "prefer"),
              "shadow: rotating exact contexts retain independent screen evidence for the observed alternative");
        for (size_t i = 0; i < RecentSamples::capacity; ++i) recent.Add(different, ++ordinal, bypass);
        check(!recent.For(producer, first).measured.samples, "shadow: stale context ages out");
        different.generation++;
        recent.Add(different, ++ordinal, good);
        check(recent.For(different, first).measured.samples == 1, "shadow: new generation starts clean");
        recent.Add(different, ordinal, bypass);
        check(recent.For(different, first).measured.samples == 1, "shadow: repeated ordinal cannot vote twice");

        DeliveryContinuity alternating, grouped;
        DeliveryContinuity paused;
        for (uint64_t i = 0; i < 2250; ++i) {
            const bool suspended = i > 0 && i < 2245;
            const na::native11::PresentReceipt frame{i, true, i == 0 || i == 2245, suspended || i == 0};
            paused.Observe(&producer, frame.ordinal, frame.EligibleForDlaa(), frame.submitted);
            if (i == 0) check(frame.EligibleForDlaa(), "a delivered frame counts even if paused before Present");
        }
        check(paused.changes == 1 && paused.longest_miss == 4,
              "a long deliberate pause breaks the sequence; four genuine misses after resuming still count");
        for (uint64_t i = 0; i < 12; ++i) {
            alternating.Observe(&producer, i, true, i % 2 == 0);
            grouped.Observe(&producer, i, true, i < 6);
        }
        check(alternating.changes == 11 && alternating.longest_miss == 1 &&
              grouped.changes == 1 && grouped.longest_miss == 6,
              "coverage: distinguish rapid cycling and one long gap at identical submission rates");
        grouped.Observe(&producer, 12, false, false);
        grouped.Observe(&producer, 13, true, true);
        grouped.Observe(&different, 14, true, false);
        grouped.Observe(&different, 18, true, true);
        check(grouped.changes == 1 && grouped.longest_miss == 6,
              "coverage: menus, another chain and an ordinal gap cannot manufacture transitions");
        current.measured = {};
        for (int frame = 0; frame < 3; ++frame) current.Add(bypass, between);
        check(!std::strcmp(Preview(current).action, "probe"), "shadow: a repeated observed boundary is a probe candidate");
        current.Add(bypass, {Path::Inline, 45});
        check(!std::strcmp(Preview(current).action, "check_placement"), "shadow: conflicting boundary observations do not select a probe");
        current.measured = {};
        auto composite = good;
        composite.tiles_textured = 8;
        composite.tiles_input = 6;
        composite.tiles_output = 2;
        current.Add(composite);
        current.Add(composite);
        check(VoteScreen(composite) == ScreenVote::Unknown &&
              !std::strcmp(Preview(current).action, "check_composite"),
              "shadow: partial composition is evidence to inspect local stages, never a whole-route rollback");
        std::swap(composite.tiles_input, composite.tiles_output);
        check(VoteScreen(composite) == ScreenVote::Output,
              "shadow: strong positive delivery survives two disagreeing regions");
    }

    // Motion and jitter: the scene moved by u between the frames; the jitter changed.
    const float ux = 3.3f, uy = -1.7f;
    const float jb[2] = {.25f, -.3f}, jn[2] = {-.35f, .2f};
    const Image before = Render(Scene, jb[0], jb[1]);
    const Image now = Render(Scene, jn[0], jn[1], ux, uy);
    auto motion_case = [&](const Image& cur, const Image& prev, float vx, float vy, const float* told_now, const float* told_before, Window* w) {
        static std::vector<float> m;
        m = Vectors(vx, vy);
        MotionInputs mi;
        mi.current = &cur; mi.previous = &prev; mi.motion = &m; mi.motion_w = W; mi.motion_h = H;
        mi.jitter_now[0] = told_now[0]; mi.jitter_now[1] = told_now[1];
        mi.jitter_before[0] = told_before[0]; mi.jitter_before[1] = told_before[1];
        const Verdicts v = JudgeOne(Build(cur, cur, nullptr, &mi), w);
        line(*w, v);
        return v;
    };
    { // the right vectors (previous position minus current: -u)
        Window w;
        const Verdicts v = motion_case(now, before, -ux, -uy, jn, jb, &w);
        check(v.motion == Verdict::Pass, "motion: the right vectors predict the frame");
        check(v.jitter == Verdict::Pass, "jitter: the picture follows the jitter DLSS was told");
    }
    { // no vectors while the scene moved
        Window w;
        const Verdicts v = motion_case(now, before, 0, 0, jn, jb, &w);
        check(v.motion == Verdict::Fail, "motion: zero vectors on a moving picture are caught");
    }
    { // Adjacent materials independently brighten or dim, while a displaced texture still fails.
        Image relit = before, pan = now;
        for (unsigned y = 0; y < H; ++y) for (unsigned x = 0; x < W; ++x) {
            const float gain = ((x / 37 + y / 29) % 2) ? 2.f : .5f;
            relit.v[y * W + x] *= gain;
            pan.v[y * W + x] *= gain;
        }
        Window w;
        auto v = motion_case(relit, before, 0, 0, jb, jb, &w);
        check(v.motion == Verdict::NotMeasured && w.total.light_compatible > w.total.still_points * .8f,
              "local lighting on static detail does not convict the motion source");
        v = motion_case(pan, before, 0, 0, jn, jb, &w);
        check(v.motion == Verdict::Fail, "a pan with missing vectors remains visible under patchy relighting");
        auto foliage = before;
        for (unsigned y = H / 4; y < H / 2; ++y)
            for (unsigned x = W / 4; x < W * 3 / 4; ++x) foliage.v[y * W + x] = now.v[y * W + x];
        v = motion_case(foliage, before, 0, 0, jb, jb, &w);
        check(v.motion == Verdict::NotMeasured && w.total.changed_still > 0 && w.total.changed_still < w.total.still_points / 3,
              "a moving patch surrounded by stationary detail is recorded without blaming the entire motion source");
    }
    { // A prior-frame position far outside the image supplies no evidence.
        Window w;
        const Verdicts v = motion_case(now, before, 1e30f, 0, jn, jb, &w);
        check(w.total.points == 0 && v.motion == Verdict::NotMeasured && v.jitter == Verdict::NotMeasured,
              "motion: off-screen projections cannot vote against a working motion source");
    }
    { // half the vectors
        Window w;
        const Verdicts v = motion_case(now, before, -ux / 2, -uy / 2, jn, jb, &w);
        check(v.motion == Verdict::Fail && w.total.twice_better * 2 >= w.total.moving, "motion: half vectors are caught as half the real motion");
        check(v.jitter == Verdict::NotMeasured, "jitter: not judged while the vectors are wrong");
    }
    { // twice the vectors
        Window w;
        const Verdicts v = motion_case(now, before, -2 * ux, -2 * uy, jn, jb, &w);
        check(v.motion == Verdict::Fail && w.total.half_better * 2 >= w.total.moving, "motion: doubled vectors are caught as twice the real motion");
    }
    { // vectors with the wrong sign
        Window w;
        const Verdicts v = motion_case(now, before, ux, uy, jn, jb, &w);
        check(v.motion == Verdict::Fail, "motion: vectors with the wrong sign are caught");
    }
    { // the picture was rendered without jitter, DLSS was told it had one
        const Image b0 = Render(Scene, 0, 0), n0 = Render(Scene, 0, 0, ux, uy);
        Window w;
        const Verdicts v = motion_case(n0, b0, -ux, -uy, jn, jb, &w);
        check(v.motion == Verdict::Pass, "motion: still predicted without jitter in the picture");
        check(v.jitter == Verdict::Fail && w.total.jitter_none > w.total.jitter_follows, "jitter: a picture without the jitter is caught");
    }
    { // the picture was rendered with the opposite jitter
        const float rb[2] = {-jb[0], -jb[1]}, rn[2] = {-jn[0], -jn[1]};
        const Image br = Render(Scene, rb[0], rb[1]), nr = Render(Scene, rn[0], rn[1], ux, uy);
        Window w;
        const Verdicts v = motion_case(nr, br, -ux, -uy, jn, jb, &w);
        check(v.jitter == Verdict::Fail && w.total.jitter_reverse > w.total.jitter_follows, "jitter: the opposite jitter is caught");
        check(w.total.half_better * 2 < w.total.moving && w.total.twice_better * 2 < w.total.moving,
              "motion: changing scale does not explain the opposite jitter");
    }
    { // a still scene: motion is not judged, the jitter still is
        const Image ns = Render(Scene, jn[0], jn[1]);
        Window w;
        const Verdicts v = motion_case(ns, before, 0, 0, jn, jb, &w);
        check(v.motion == Verdict::NotMeasured, "motion: a still scene is not judged");
        check(v.jitter == Verdict::Pass, "jitter: judged on a still scene too");
    }
    { // DLSS's motion scale: vectors in a quarter of a pixel, scale 4
        static std::vector<float> m;
        m = Vectors(-ux / 4, -uy / 4);
        MotionInputs mi;
        mi.current = &now; mi.previous = &before; mi.motion = &m; mi.motion_w = W; mi.motion_h = H;
        mi.scale_x = mi.scale_y = 4;
        mi.jitter_now[0] = jn[0]; mi.jitter_now[1] = jn[1]; mi.jitter_before[0] = jb[0]; mi.jitter_before[1] = jb[1];
        const Verdicts v = JudgeOne(Build(now, now, nullptr, &mi));
        check(v.motion == Verdict::Pass, "motion: the motion scale is applied");
    }
    { // a window adds samples: one sample of each kind
        Window w;
        w.Add(Score(Build(input, output, &screen_out, nullptr).sample));
        w.Add(Score(Build(input, output, &screen_out, nullptr).sample));
        check(w.samples == 2 && w.screens == 2 && w.total.tiles == 2 * kTiles, "a window counts its samples");
        char buf[2048];
        check(Format(buf, sizeof buf, w, Judge(w)) > 0 && std::strstr(buf, "picture{verdict=pass") != nullptr, "the log line names the verdicts");
    }
    { // Highly textured but identical patches cannot distinguish a route.
        std::vector<float> data(kResultFloats);
        float* in = data.data() + BlockOffset(kThumbInput);
        float* out = data.data() + BlockOffset(kThumbOutput);
        float* shown = data.data() + BlockOffset(kThumbScreen);
        for (unsigned y = 0; y < kThumbH; ++y) for (unsigned x = 0; x < kThumbW; ++x) {
            const unsigned i = y * kThumbW + x;
            const auto u = x % 32, v = y % 36;
            const float stripe = (x & 2) ? 1.f : -1.f;
            in[i] = (u < 13 ? 1.f : .2f) * stripe;
            out[i] = (u >= 20 && u <= 27 && v >= 18 && v <= 27) ? in[i] * .3f : in[i];
            shown[i] = in[i];
        }
        const auto ordinary = FindTexturedTiles(data.data());
        const auto adaptive = FindTexturedTiles(data.data(), true, ordinary);
        unsigned witnesses = 0, controls = 0;
        for (unsigned k = 0; k < kTiles; ++k) {
            const unsigned p = adaptive.cell[k] - 1;
            if (adaptive.cell[k] == ordinary.cell[k]) ++controls;
            if (p < kThumbFloats && std::fabs(in[p] - out[p]) > .05f) ++witnesses;
        }
        check(witnesses == 4 && controls == 4, "ambiguous textured views: four probes find distinguishing detail and four retain independent coverage");
    }
    {
        using Phase = RouteTrial::Phase;
        constexpr unsigned region = 18;
        const auto exercise = [&](float candidate, float repeat, bool lost, bool changed,
                                  float new_flicker, Phase expected) {
            RouteTrial trial;
            uint64_t ordinal = 0;
            trial.Start(51, ordinal, 100);
            check(!trial.Ticket(ordinal) && !trial.UsesCandidate(), "trial: start from ordinary route with warmup");
            const auto window = [&](float amplitude, bool delivered = true, uint64_t layout = 1, float side_effect = 0.f) {
                ordinal += RouteTrial::warmup_frames + 1;
                const auto ticket = trial.Ticket(ordinal);
                for (unsigned i = 0; i < RouteTrial::samples; ++i) {
                    TrialSample frame;
                    frame.ordinal = ordinal++;
                    frame.configuration = layout;
                    frame.delivered = delivered;
                    frame.bypassed = !delivered;
                    frame.width = 3440;
                    frame.height = 1440;
                    for (unsigned j = 0; j < frame.change[region].size(); ++j)
                        frame.change[region][j] = (i + j) % 2 ? -amplitude : amplitude;
                    frame.change[63].fill(side_effect);
                    trial.Add(ticket ^ 8, frame, ordinal); // different experiment
                    trial.Add(ticket, frame, ordinal);
                    trial.Add(ticket, frame, ordinal); // same pair cannot vote twice
                }
            };
            window(.25f);
            check(trial.Current() == Phase::Candidate && trial.UsesCandidate(), "trial: repeated incumbent reversal unlocks replacement");
            window(candidate, !lost, changed ? 2u : 1u, new_flicker);
            if (trial.Current() == Phase::ConfirmBaseline) {
                check(!trial.UsesCandidate(), "trial: restore ordinary route for the independent recheck");
                window(repeat);
                trial.Tick(100 + RouteTrial::maximum_ms, true);
            }
            if (changed) {
                check(trial.Current() == Phase::Rejected, "trial: changing settings immediately returns the ordinary route");
                trial.Tick(100 + RouteTrial::maximum_ms, true);
            }
            check(trial.Current() == expected, trial.Reason());
            check(trial.UsesCandidate() == (expected == Phase::Kept), "trial: failed comparison falls back");
            if (expected == Phase::Kept) {
                auto monitored = trial;
                window(.24f);
                check(trial.Current() == Phase::Rejected && !trial.UsesCandidate(),
                      "trial: the same flicker returning after approval restores the ordinary route");
                trial = monitored;
                window(0.f, true, 2);
                check(trial.Current() == Phase::Rejected && !trial.UsesCandidate(),
                      "trial: approval cannot leak into new settings");
                trial = monitored;
                window(0.f);
                check(trial.Current() == Phase::Kept, "trial: a quiet verified winner stays in use");
                trial.Tick(110, false);
                check(trial.Current() == Phase::Rejected && !trial.UsesCandidate(), "trial: withdraw when a proved reader is lost");
                check(trial.CanStart(8100 + 11), "trial: temporary loss can qualify again in the same scene");
            }
        };
        exercise(.07f, .24f, false, false, .05f, Phase::Kept);
        exercise(.20f, .24f, false, false, 0.f, Phase::Rejected);
        exercise(.07f, .01f, false, false, 0.f, Phase::Rejected);
        exercise(.07f, .24f, true, false, 0.f, Phase::Rejected);
        exercise(.07f, .24f, false, true, 0.f, Phase::Rejected);
        exercise(.04f, .24f, false, false, .32f, Phase::Rejected);
        // A stationary two-region bypass has no flash. Exercise the actual trial transitions.
        const auto regional_case = [&](unsigned candidate, bool restore_original) {
            RouteTrial t;
            t.Start(57, 0, 100);
            uint64_t ordinal = 0;
            for (unsigned arm = 0; arm < 3 && t.Active(); ++arm) {
                ordinal += RouteTrial::warmup_frames + 1;
                for (unsigned i = 0; i < 4; ++i) {
                    TrialSample f;
                    f.ordinal = ++ordinal; f.width = 960; f.height = 540;
                    f.delivered = true; // even a positive global verdict must not rescue a bad candidate
                    TileVotes votes{0x03, 0xfc};
                    if ((arm == 1 && candidate != 1) || (arm == 2 && !restore_original)) votes = {0, 0xff};
                    if (arm == 1 && candidate == 2) votes = {0x04, 0xfb};
                    f.tiles.Set(votes, f.width, f.height, {});
                    f.tiles.detail[2] = (arm == 1 && candidate == 5) ? .04f : .10f;
                    if (arm == 1 && candidate == 3) f.tiles.at[0].x++;
                    if (arm == 1 && candidate == 4) f.change[20].fill(.3f);
                    t.Tick(100 + ordinal, true);
                    t.Add(t.Ticket(f.ordinal), f, f.ordinal + 1);
                }
            }
            if (t.Active()) t.Tick(100 + ordinal + RouteTrial::maximum_ms, true);
            return t;
        };
        auto won = regional_case(0, true);
        check(won.Regional() && won.Current() == Phase::Kept, "mixed tiles: disputed areas recover, controls hold, original bypass returns");
        won.Tick(100000, true);
        check(!won.UsesCandidate(), "a regional winner expires without accepted screen receipts");
        for (unsigned c = 1; c <= 5; ++c)
            check(regional_case(c, true).Current() == Phase::Rejected, "reject unchanged, damaged, relocated or newly flickering regional trial");
        check(regional_case(0, false).Current() == Phase::Rejected, "an original bypass that does not recur cannot certify a replacement");
        using Path = shadow::Path;
        const auto placement_sample = [](uint64_t ordinal, uint64_t now, bool alternate, bool repaired) {
            TrialSample sample;
            sample.ordinal = ordinal;
            sample.observed_at = now;
            sample.width = 960;
            sample.height = 540;
            sample.layout = 1;
            sample.configuration = 1;
            sample.delivered = true;
            sample.context = {{alternate ? Path::Between : Path::Inline, alternate ? 0ull : 17ull},
                31, 41, 3, 0, sample.width, sample.height, 0, 0, sample.configuration};
            sample.alternate = {alternate ? Path::None : Path::Between, 0};
            sample.tiles.Set(repaired ? TileVotes{0, 0xff} : TileVotes{3, 0xfc}, sample.width, sample.height, {});
            sample.tiles.detail.fill(.10f);
            return sample;
        };
        for (bool repaired : {true, false}) {
            PlacementTrial trial;
            const PlacementTrial::Scope scope{31, 41, 3, 12};
            auto witness = placement_sample(1, 100, false, false);
            check(!trial.Begin({}, witness, 3, 102), "mixed output alone cannot nominate a reader");
            check(!trial.Begin(scope, witness, 300, 103), "old proof cannot start a trial");
            check(trial.Begin(scope, witness, 3, 103), "mixed output with a proved reader starts baseline");
            check(!trial.Matches({31, 41, 4, 12}) && !trial.Matches({31, 41, 3, 13}), "scene epoch and selection own the trial");
            uint64_t ordinal = 3;
            const auto arm = [&](bool candidate, bool improves) {
                ordinal += 2 + RouteTrial::warmup_frames;
                for (unsigned i = 0; i < RouteTrial::samples; ++i, ++ordinal) {
                    const auto tick = 100 + ordinal;
                    auto sample = placement_sample(ordinal, tick, candidate, improves);
                    trial.Tick(tick, true);
                    const auto ticket = trial.Ticket(ordinal);
                    trial.Add({31, 42, 3, 12}, ticket, sample, ordinal, tick);
                    trial.Add(scope, ticket ^ 8u, sample, ordinal, tick);
                    trial.Add(scope, ticket, sample, ordinal, tick);
                }
            };
            arm(false, false);
            check(trial.UsesCandidate() && trial.Comparison().Regional(), "DX12: comparable baseline unlocks independent candidate");
            arm(true, repaired);
            if (repaired) {
                check(trial.Comparison().Current() == Phase::ConfirmBaseline && !trial.UsesCandidate(),
                    "DX12: restore incumbent for a confirmation on the same area");
                arm(false, false);
                check(trial.Comparison().Current() == Phase::Kept && trial.UsesCandidate(),
                    "DX12: improved areas, preserved controls and repeatable bypass keep the handoff");
                trial.Tick(100 + ordinal, false);
                check(trial.Comparison().Current() == Phase::Rejected && !trial.UsesCandidate(),
                    "DX12: lost qualified submission returns the original route");
            } else {
                check(trial.Comparison().RuledOut() && !trial.UsesCandidate(),
                    "DX12: unchanged regional bypass rules out this handoff");
                witness = placement_sample(ordinal, 10000, false, false);
                check(!trial.Begin(scope, witness, ordinal, 10000), "remember failed handoff for the same view and settings");
                auto next_reader = trial;
                check(next_reader.BeginVerified({31, 41, 3, 12, 19}, ordinal, 10000),
                    "a failed handoff cannot rule out a different proved reader");
                witness.context.trial_configuration++;
                witness.configuration++;
                check(trial.Begin(scope, witness, ordinal, 10000), "a changed configuration can qualify for a new comparison");
            }
        }
        // Same localized strobe, same proved logical scene. Exercise both
        // adapters' receipt shapes, including two independently proved owners.
        for (auto path : {Path::Native11, Path::Inline}) {
            PlacementTrial trial;
            const PlacementTrial::Scope scope{31, 41, 3, 12};
            auto sample = placement_sample(1, 100, false, true);
            sample.context.placement = {path, 17};
            check(!trial.Begin(scope, sample, 1, 100), "quiet fully delivered output stays on its current route");
            sample.change[13].fill(.24f);
            check(trial.Begin(scope, sample, 1, 100), "localized flicker can start a placement trial with full delivery");
            uint64_t ordinal = 1;
            for (unsigned arm = 0; arm < 3; ++arm) {
                ordinal += RouteTrial::warmup_frames + 2;
                for (unsigned i = 0; i < RouteTrial::samples; ++i, ++ordinal) {
                    sample.ordinal = ordinal;
                    sample.observed_at = ordinal + 100;
                    sample.context.color = arm == 1 ? 31 + (i % 2) : 31;
                    sample.context.placement.path = arm == 1 && path == Path::Inline ? Path::Between : path;
                    sample.change[13].fill(arm == 1 ? .01f : (i % 2 ? .24f : -.24f));
                    const auto ticket = trial.Ticket(ordinal);
                    trial.Tick(sample.observed_at, true);
                    trial.Add(scope, ticket, sample, ordinal, sample.observed_at);
                }
            }
            check(trial.Comparison().Current() == Phase::Kept && trial.UsesCandidate(),
                "both receipt shapes keep the later placement only after the same flicker decreases");
        }
        RegionalTiles overlap;
        overlap.Set({3, 12}, 960, 540, {});
        overlap.at[0] = overlap.at[1];
        check(!overlap.Mixed(), "overlapping tiles cannot act as independent witnesses");
        RouteTrial quiet;
        quiet.Start(52, 0, 0);
        for (uint64_t i = 40; i < 44; ++i) {
            TrialSample frame;
            frame.ordinal = i;
            frame.delivered = true;
            frame.change[region].fill(i == 40 ? .4f : 0.f);
            quiet.Add(quiet.Ticket(i), frame, i + 1);
        }
        check(quiet.Current() == Phase::Baseline && !quiet.UsesCandidate(), "trial: a single brightness jump does not trigger a replacement");
        quiet.Tick(RouteTrial::maximum_ms, true);
        check(quiet.Current() == Phase::Rejected && !quiet.CanStart(RouteTrial::maximum_ms),
              "trial: close a quiet measurement window with a cooldown");
        check(quiet.CanStart(RouteTrial::maximum_ms + 8000), "trial: a later change can still trigger a comparison");
    }
    {
        using namespace na::referee;
        using T = RecoveryTool;
        using S = FlickerStage;
        check(NextTool(S::Unknown, 255) == T::None && NextTool(S::Reconstruction, Bit(T::Bloom)) == T::None &&
              NextTool(S::Input, Bit(T::Sharpen)) == T::None, "unknown and wrong-stage controls are never tried");
        check(NextTool(S::Input, Bit(T::AmbientHistory)) == T::AmbientHistory &&
              NextTool(S::Reconstruction, Bit(T::CameraResidual)) == T::CameraResidual &&
              NextTool(S::Reconstruction, Bit(T::ScreenFit)) == T::ScreenFit &&
              NextTool(S::Composition, Bit(T::Sharpen)) == T::Sharpen, "controls belong to their stage");
        StageRecovery r;
        const StageRecovery::Scene scene{1, 2, 1920, 1080};
        r.SetScene(scene);
        TrialSample s;
        s.width = 1920; s.height = 1080; s.configuration = 8;
        s.comparable = s.delivered = true; s.tools = Bit(T::Sharpen);
        s.origin = {S::Composition, 33, {0, 0, .3f}};
        uint64_t test_time = 101;
        auto send = [&](float amplitude, float contrast, uint8_t applied = 0) {
            s.ordinal += RouteTrial::warmup_frames + 1;
            test_time += 100;
            s.origin.stops[2] *= -1;
            s.change[33].fill(s.origin.stops[2] > 0 ? amplitude : -amplitude);
            s.contrast[33] = contrast;
            s.applied = applied;
            r.Tick(test_time);
            r.Add(r.Ticket(s.ordinal), s, s.ordinal + 1, test_time);
        };
        s.delivered = false;
        send(.3f, .2f); send(.3f, .2f);
        check(!r.Busy(), "delivery must be distinguished before an experiment");
        s.delivered = true;
        s.origin.stops[0] = -.2f;
        send(.3f, .2f); send(.3f, .2f);
        check(!r.Busy(), "strong ambiguous upstream motion cannot authorize a downstream fix");
        s.origin.stops[0] = 0;
        send(.3f, .2f); send(.3f, .2f);
        check(r.Selected() == T::Sharpen && r.ToolInUse() == T::None, "ordinary route measures the target before a change");
        for (int i = 0; i < 4; ++i) send(.3f, .2f);
        check(r.ToolInUse() == T::Sharpen, "matched target unlocks the change");
        for (int i = 0; i < 4; ++i) send(.01f, .01f, Bit(T::Sharpen));
        check(!r.Busy() && r.Excluded() == Bit(T::Sharpen), "removing detail cannot earn a win");
        r.SetScene({9, 2, 1920, 1080});
        r.SetScene(scene);
        send(.3f, .2f); send(.3f, .2f);
        check(!r.Busy() && r.Exhausted(), "returning to the same view remembers what failed");
        ++s.configuration;
        send(.3f, .2f); send(.3f, .2f);
        check(r.Busy() && !r.Excluded(), "new user settings get an independent result");
        auto stale = r.Ticket(s.ordinal + 80);
        r.Clear(); r.SetScene(scene);
        check(!r.Busy() && r.Ticket(s.ordinal) != stale, "device change invalidates even a late worker receipt");
        test_time += 5000;
        s.observed_at = 1;
        send(.3f, .2f); send(.3f, .2f);
        check(!r.Busy(), "stale accepted presents cannot start a live change");
        s.observed_at = 0;
        send(.3f, .2f); send(.3f, .2f);
        for (int i = 0; i < 4; ++i) send(.3f, .2f);
        check(r.ToolInUse() == T::Sharpen, "fresh receipts recover after a stale pair");
        send(.45f, .2f, Bit(T::Sharpen));
        for (int i = 0; i < 7; ++i) send(.01f, .2f, Bit(T::Sharpen));
        check(!r.Busy() && !r.Excluded(), "one unrelated flash is inconclusive, not a lasting rejection");
        StageRecovery dx12(true);
        dx12.SetScene(scene);
        check(dx12.Ticket(s.ordinal) != r.Ticket(s.ordinal), "DX11 and DX12 mailboxes are independent");
    }
    std::printf("referee_test: %s (%d failures)\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
}
