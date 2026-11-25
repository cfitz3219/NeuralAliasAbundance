#pragma once

// The Referee's control rule for the enhancer (docs/NA1-ENHANCER.md section 5).
//
// Cort, 2026-11-22: no fixed-strength look. "We need a way for referee to see the numbers and
// decide if its strong enough. Or even, which of the algorithms need to be strengthend, which need
// to be weakened." So nothing here knows a game: every sample brings the picture's numbers before
// and after the enhancer from the same frame, and each dial is moved toward its goal by what the
// numbers answer. The game's own picture is always acceptable: a ceiling or a floor only limits
// what the enhancer adds, and a number that is already past its ceiling leaves that dial at zero.
//
// No clock and no system call: time arrives with each sample. One Observe per sample (every
// 250 ms), also while suspended, so that the frame time is still watched and it can try again.

#include "core/enhancer_stats.hpp"
#include "core/na1_reference.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace na::enhancer {

class Governor {
public:
    enum Dial : int { kTexture, kLight, kColour, kKey, kPartA, kPartB, kDials };

    struct Params {
        // Goals and rest bands (the table in section 5; Cort keeps them as they are).
        float texture_goal = 1.10f, texture_low = 1.08f, texture_high = 1.12f;
        float colour_gain = 1.12f, colour_ceiling = 0.60f;
        float dark_cut = 0.30f, dark_floor = 2.0f;        // near black: down by this share, never below 2%
        float band_share = 0.15f;                          // rest band: goal +- this share of the gap
        float key_band = 0.01f;                            // stops
        // Hard limits.
        float dark_rise_most = 0.5f, white_rise_most = 0.3f;  // points
        float glow_most = 0.03f;                              // stops
        float broad_least = 0.92f, broad_most = 1.10f;
        // One check of one dial.
        float step_fixed = 0.05f, step_share = 0.25f, approach = 0.80f, response_least = 0.01f;
        float dial_most = 1.5f;
        float cut_share = 0.20f, light_cut_share = 0.15f;
        float key_follow = 0.9f, key_step = 0.1f, key_most = 0.5f;
        // The two external parts: summed darkening they may add (Astra's proposal, set by Cort's eye).
        float shadow_mean = 0.010f, shadow_worst = 0.040f;  // stops: mean, worst tile
        float part_goal = 1.0f;                              // a part's own designed strength
        // Time allowance.
        float frame_over_ms = 22.2f, frame_under_ms = 18.2f;  // 45 fps, 55 fps
        float cost_least_ms = 0.5f, own_cost_most_ms = 3.0f;
        uint64_t retry_ms = 30000;
        uint32_t window = 8, window_least = 4;               // samples behind a median or an average
        // Scene values.
        float smooth_ms = 2000.f;
        float scene_moved_stops = 0.5f;  // a change this large is a new scene for "until the scene changes"
        float glide_ms = 60.f;           // applied dials reach the governed value within one sample
    };

    struct Part {
        float gpu_ms = 0;
        // Darkening of the part's own layer at dial 1, in stops (positive = darker): over the
        // picture, and in its worst tile.
        float mean_darkening = 0, worst_tile = 0;
        bool present = false;  // the part passed a layer this sample
    };
    struct Sample {
        Numbers n;             // not valid when nothing was measured (suspended, readback not back)
        float gpu_ms = 0;      // NA1's own passes
        float frame_ms = 0;    // the game's frame time
        Part part[2];
        bool reset = false;    // camera cut or history reset
        uint64_t now_ms = 0;
        // The dials the measured frame was really made with. Applied dials glide, so they can lag
        // the governed ones; the response is judged against what was applied. Left unset, the
        // governed values are taken.
        na::na1::ref::Dials applied;
        bool have_applied = false;
    };

    Governor() = default;
    explicit Governor(const Params& p) : p_(p) { NewScene(); }
    const Params& Settings() const { return p_; }

    // True when a dial, a state or a reason changed (the caller logs).
    bool Observe(const Sample& s)
    {
        ++samples_;
        const Snapshot was = Snap();
        if (s.reset) {
            NewScene();
            scene_known_ = false;  // the next numbers are taken whole
        }
        TimeAllowance(s);
        if (s.n.valid) {
            SmoothScene(s);
            if (!off_[0]) Steer(s);
            last_ = s.n;
        }
        state_why_ = off_[0] ? off_why_[0] : !scene_known_ ? "waiting for the first measurement" : "running";
        return !(was == Snap());
    }

    // Governed values. A part that is switched off for time reads zero.
    na::na1::ref::Dials Dials() const
    {
        na::na1::ref::Dials d;
        d.texture = dial_[kTexture];
        d.light = dial_[kLight];
        d.colour = dial_[kColour];
        d.key = dial_[kKey];
        d.part[0] = off_[1] ? 0.f : dial_[kPartA];
        d.part[1] = off_[2] ? 0.f : dial_[kPartB];
        return d;
    }
    // Smoothed scene values. The display curve is set by the caller.
    na::na1::ref::Scene Scene() const
    {
        na::na1::ref::Scene s = scene_;
        s.m0 = (std::max)(s.m0, 1e-4f);  // the near-black protection divides by it
        return s;
    }
    bool Suspended() const { return off_[0]; }
    const char* Why() const { return state_why_; }
    const char* DialWhy(int dial) const { return dial >= 0 && dial < kDials ? why_[dial] : ""; }
    uint64_t Samples() const { return samples_; }
    const Numbers& Last() const { return last_; }
    float FrameMedianMs() const { return Median(frame_ms_, frame_n_); }

    // One step of the glide: the dials a frame is drawn with move toward the governed ones a
    // little each frame so that no step of the governor is visible. A dial that is nearly there
    // lands exactly, so "zero" really is zero and the picture goes back to bit-exact.
    static na::na1::ref::Dials Glide(const na::na1::ref::Dials& applied, const na::na1::ref::Dials& governed, float frame_ms,
                                     float glide_ms = 60.f)
    {
        const float share = !(frame_ms > 0) ? 0.f : !(glide_ms > 0) ? 1.f : 1.f - std::exp(-frame_ms / glide_ms);
        const auto step = [share](float from, float to) {
            const float v = from + (to - from) * share;
            return std::fabs(to - v) < 1e-3f || !std::isfinite(v) ? to : v;
        };
        na::na1::ref::Dials d;
        d.texture = step(applied.texture, governed.texture);
        d.light = step(applied.light, governed.light);
        d.colour = step(applied.colour, governed.colour);
        d.key = step(applied.key, governed.key);
        d.part[0] = step(applied.part[0], governed.part[0]);
        d.part[1] = step(applied.part[1], governed.part[1]);
        return d;
    }

    // One log line: dials, numbers, whys.
    int Format(char* out, size_t n) const
    {
        const na::na1::ref::Dials d = Dials();
        return std::snprintf(out, n,
            "sample %llu texture %.3f light %.3f colour %.3f key %+.3f parts %.2f %.2f | texture x%.3f broad x%.3f "
            "colour %.3f>%.3f variety %.3f>%.3f black %.2f>%.2f%% white %.2f>%.2f%% glow %.4f key shift %+.4f | "
            "scene m0 %.3f (%.2f stops) dark %.2f range %.2f (%.2f) mid %.2f tint %.2f %.2f %.2f | "
            "texture: %s; light: %s; colour: %s; key: %s; parts: %s / %s; %s",
            static_cast<unsigned long long>(samples_), double(d.texture), double(d.light), double(d.colour), double(d.key),
            double(d.part[0]), double(d.part[1]), double(last_.texture), double(last_.broad),
            double(last_.colour_before), double(last_.colour_after), double(last_.variety_before), double(last_.variety_after),
            double(last_.dark_before), double(last_.dark_after), double(last_.white_before), double(last_.white_after),
            double(last_.glow), double(last_.key_shift),
            double(scene_.m0), double(scene_.m0 > 0 ? -std::log2(scene_.m0) : 0.f), double(scene_.w_dark),
            double(last_.range), double(w_range_), double(scene_.mid),
            double(scene_.tint[0]), double(scene_.tint[1]), double(scene_.tint[2]),
            why_[kTexture], why_[kLight], why_[kColour], why_[kKey], why_[kPartA], why_[kPartB], state_why_);
    }

private:
    static constexpr uint32_t kWindowMost = 32;

    struct Snapshot {
        float dial[kDials];
        const char* why[kDials];
        const char* state;
        bool off[3];
        bool operator==(const Snapshot& o) const
        {
            for (int i = 0; i < kDials; ++i)
                if (dial[i] != o.dial[i] || why[i] != o.why[i]) return false;
            return state == o.state && off[0] == o.off[0] && off[1] == o.off[1] && off[2] == o.off[2];
        }
    };
    Snapshot Snap() const
    {
        Snapshot s{};
        const na::na1::ref::Dials d = Dials();
        const float v[kDials] = {d.texture, d.light, d.colour, d.key, d.part[0], d.part[1]};
        for (int i = 0; i < kDials; ++i) { s.dial[i] = v[i]; s.why[i] = why_[i]; }
        s.state = state_why_;
        for (int i = 0; i < 3; ++i) s.off[i] = off_[i];
        return s;
    }

    static float Median(const float* v, uint32_t n)
    {
        if (!n) return 0.f;
        float copy[kWindowMost];
        std::copy(v, v + n, copy);
        std::nth_element(copy, copy + n / 2, copy + n);
        return copy[n / 2];
    }
    void Push(float* v, uint32_t& n, float value) const
    {
        const uint32_t most = (std::clamp)(p_.window, 1u, kWindowMost);
        if (n >= most) {
            std::copy(v + 1, v + n, v);
            n = most - 1;
        }
        v[n++] = value;
    }

    // "Until the scene changes": a camera cut, a history reset, or the picture's brightness
    // having moved a long way since.
    void NewScene()
    {
        for (int i = 0; i < kDials; ++i) {
            dead_[i] = false;
            ceiling_[i] = p_.dial_most;
        }
        key_high_ = p_.key_most;
        key_low_ = -p_.key_most;
    }
    void MarkScene() { mark_key_ = SceneKey(); mark_mid_ = scene_.mid; marked_ = true; }
    float SceneKey() const { return -std::log2((std::max)(scene_.m0, 1e-6f)); }

    void SmoothScene(const Sample& s)
    {
        const Numbers& n = s.n;
        const uint64_t dt = s.now_ms >= scene_ms_ ? s.now_ms - scene_ms_ : 0;
        const float share = !scene_known_ ? 1.f : !(p_.smooth_ms > 0) ? 1.f : 1.f - std::exp(-float(dt) / p_.smooth_ms);
        const auto mix = [share](float from, float to) { return from + (to - from) * share; };
        scene_.mid = mix(scene_.mid, n.mid);
        scene_.m0 = mix(scene_.m0, n.m0);
        scene_.w_dark = mix(scene_.w_dark, n.w_dark);
        scene_.p85 = mix(scene_.p85, n.p85);
        scene_.p97 = mix(scene_.p97, n.p97);
        for (int c = 0; c < 3; ++c) scene_.tint[c] = mix(scene_.tint[c], n.tint[c]);
        w_range_ = mix(w_range_, n.w_range);
        scene_known_ = true;
        scene_ms_ = s.now_ms;
        if (marked_ && (std::fabs(SceneKey() - mark_key_) > p_.scene_moved_stops || std::fabs(scene_.mid - mark_mid_) > p_.scene_moved_stops)) {
            marked_ = false;
            NewScene();
        }
    }

    // The time allowance. Unit 0 is NA1's own passes, 1 and 2 the external parts.
    void TimeAllowance(const Sample& s)
    {
        if (s.frame_ms > 0 && std::isfinite(s.frame_ms)) Push(frame_ms_, frame_n_, s.frame_ms);
        if (!off_[0] && s.gpu_ms > 0 && std::isfinite(s.gpu_ms)) Push(own_ms_, own_n_, s.gpu_ms);
        const bool judged = frame_n_ >= p_.window_least;
        const float median = Median(frame_ms_, frame_n_);

        // Try the most recently shed unit again: 30 seconds on, and the game back above 55 fps.
        int latest = -1;
        for (int u = 0; u < 3; ++u)
            if (off_[u] && (latest < 0 || off_at_[u] >= off_at_[latest])) latest = u;
        if (latest >= 0 && judged && median < p_.frame_under_ms && s.now_ms >= off_at_[latest] + p_.retry_ms) {
            off_[latest] = false;
            frame_n_ = 0;  // judge the frame time afresh with it back on
            if (latest == 0) own_n_ = 0;
            else why_[kPartA + latest - 1] = "switched back on to try again";
            return;
        }
        if (off_[0]) return;  // nothing of ours is running

        float own = 0;
        for (uint32_t i = 0; i < own_n_; ++i) own += own_ms_[i];
        own = own_n_ ? own / float(own_n_) : 0.f;
        if (own_n_ >= p_.window_least && own > p_.own_cost_most_ms) {
            Shed(0, s.now_ms, "suspended: NA1's own passes cost more than 3 ms; trying again in 30 seconds");
            return;
        }
        if (!judged || !(median > p_.frame_over_ms)) return;
        float cost[3] = {own_n_ ? own : (std::max)(s.gpu_ms, 0.f), 0.f, 0.f};
        for (int i = 0; i < 2; ++i)
            if (!off_[i + 1] && s.part[i].present && dial_[kPartA + i] > 0 && std::isfinite(s.part[i].gpu_ms)) cost[i + 1] = (std::max)(s.part[i].gpu_ms, 0.f);
        if (!(cost[0] + cost[1] + cost[2] > p_.cost_least_ms)) return;  // we are not what is slow
        int worst = 0;
        for (int u = 1; u < 3; ++u)
            if (cost[u] > cost[worst]) worst = u;
        Shed(worst, s.now_ms, worst == 0 ? "suspended: frame time over 22.2 ms and NA1 is the costliest part; trying again in 30 seconds"
                                        : "switched off: frame time over 22.2 ms and this is the costliest part; trying again in 30 seconds");
    }
    void Shed(int unit, uint64_t now, const char* why)
    {
        off_[unit] = true;
        off_at_[unit] = now;
        off_why_[unit] = why;
        if (unit > 0) why_[kPartA + unit - 1] = why;
        frame_n_ = 0;  // the next decision waits for frame times measured without it
        if (unit == 0) own_n_ = 0;
    }

    struct Check { float dial; const char* why; bool dead; };
    // One check of one dial. d governed value, a the value the measured frame was made with,
    // m measured, m0 the same number before the enhancer, g goal, [lo, hi] rest band.
    Check Bounded(float d, float a, float m, float m0, float g, float lo, float hi) const
    {
        if (lo <= m && m <= hi) return {d, "in band", false};
        const bool want_up = m < lo;
        const float resp = m - m0;
        // No slope to trust: the dial is barely on, the number hardly moved, or it moved away
        // from the goal (another dial's doing in the same frame).
        if (a < p_.step_fixed || std::fabs(resp) < p_.response_least * (std::max)(std::fabs(m0), 1e-6f) || resp * (g - m0) <= 0) {
            if (want_up) {
                if (d >= p_.dial_most - 1e-6f) return {0.f, "no response at the top: back to zero until the scene changes", true};
                return {(std::min)(d + p_.step_fixed, p_.dial_most), "no reliable response yet: fixed step up", false};
            }
            return {(std::max)(d - p_.step_fixed, 0.f), "over the band: fixed step down", false};
        }
        const float ideal = a * (g - m0) / resp;
        const float cap = (std::max)(p_.step_share * d, p_.step_fixed);
        const float step = (std::clamp)(p_.approach * (ideal - d), -cap, cap);
        return {(std::clamp)(d + step, 0.f, p_.dial_most), want_up ? "under the band: toward the goal" : "over the band: toward the goal", false};
    }
    float Cut(float d, float share) const { return (std::max)(d - (std::max)(share * d, p_.step_fixed), 0.f); }

    // VARIANT: enhancer/governed-strength
    // The dials are chosen here, from what this scene's numbers answered, never from a preset.
    void Steer(const Sample& s)
    {
        const Numbers& n = s.n;
        const na::na1::ref::Dials governed = Dials();
        const na::na1::ref::Dials& a = s.have_applied ? s.applied : governed;
        Relax(s.now_ms);

        // What this scene wants. Colour: the game's own times 1.12, never past strength 0.60.
        const float colour_gain = n.colour_before < p_.colour_ceiling
            ? (std::min)(p_.colour_gain, p_.colour_ceiling / (std::max)(n.colour_before, 1e-6f)) : 1.f;
        const bool want_colour = colour_gain > 1.001f;
        // Near black: shadows are opened only where the lighting range is wide, and less the
        // darker the scene is (a night keeps its blacks), never below 2% of the picture.
        const float open = p_.dark_cut * (1.f - scene_.w_dark) * w_range_;
        const float dark_goal = (std::max)(n.dark_before * (1.f - open), (std::min)(p_.dark_floor, n.dark_before));
        const bool want_light = n.dark_before > dark_goal + 1e-3f;

        // Hard limits first: a broken one cuts the dial that is responsible instead of steering
        // it. What a dial was cut to stays its ceiling for a while, or the next check would step
        // straight back over the limit.
        const float white_rise = n.white_after - n.white_before, dark_rise = n.dark_after - n.dark_before;
        const bool white_broken = white_rise > p_.white_rise_most;
        const bool dark_broken = !want_light && dark_rise > p_.dark_rise_most;
        const char* broken = white_broken ? "held back: pure white rose by more than 0.3 points"
                           : n.glow > p_.glow_most ? "held back: edge glow over 0.03 stops"
                           : n.broad > p_.broad_most ? "held back: broad contrast over 1.10x"
                           : dark_broken ? "held back: near black rose by more than 0.5 points" : nullptr;
        bool texture_cut = false, colour_cut = false, key_cut = false;
        int part_cut = -1;
        if (white_broken && dial_[kKey] > 0) {
            // The key dial lifts the whole picture after the white protection: it is the first
            // suspect when white rises, and gives way before anything that adds detail.
            dial_[kKey] = (std::max)(dial_[kKey] - p_.key_step, 0.f);
            key_high_ = dial_[kKey];
            key_cut = true;
        } else if (!white_broken && dark_broken && dial_[kKey] < 0) {
            dial_[kKey] = (std::min)(dial_[kKey] + p_.key_step, 0.f);
            key_low_ = dial_[kKey];
            key_cut = true;
        } else if (broken && dial_[kTexture] > 0) {
            // Texture is what reaches white, darkens its own troughs, leaks past an edge and
            // lifts broad shapes.
            Hold(kTexture, Cut(dial_[kTexture], p_.cut_share), broken, s.now_ms);
            texture_cut = true;
        } else if (white_broken && dial_[kColour] > 0) {
            Hold(kColour, Cut(dial_[kColour], p_.cut_share), broken, s.now_ms);
            colour_cut = true;
        } else if (broken) {
            // Only the external layers are left to have done it: the larger one gives way.
            part_cut = governed.part[1] * s.part[1].mean_darkening > governed.part[0] * s.part[0].mean_darkening ? 1 : 0;
            if (!(governed.part[part_cut] > 0)) part_cut = -1;
        }
        if (key_cut) {
            why_[kKey] = broken;
            key_ms_ = s.now_ms;
            MarkScene();
        }

        if (dead_[kTexture]) dial_[kTexture] = 0;
        else if (!texture_cut)
            Take(kTexture, Bounded(dial_[kTexture], a.texture, n.texture, 1.f, p_.texture_goal, p_.texture_low, p_.texture_high));

        if (!want_colour) {
            dial_[kColour] = 0;
            why_[kColour] = "the game's colour is already at its ceiling: left alone";
        } else if (dead_[kColour]) {
            dial_[kColour] = 0;
        } else if (!colour_cut) {
            const float goal = n.variety_before * colour_gain, band = p_.band_share * (goal - n.variety_before);
            Take(kColour, Bounded(dial_[kColour], a.colour, n.variety_after, n.variety_before, goal, goal - band, goal + band));
        }

        if (!want_light) {
            dial_[kLight] = 0;
            why_[kLight] = n.dark_before <= p_.dark_floor ? "near black is already at or under 2%: no shadows to open"
                         : scene_.w_dark >= 1.f ? "dark scene: its shadows are kept"
                         : "narrow lighting range: no shadows to open";
        } else if (dead_[kLight]) {
            dial_[kLight] = 0;
        } else if (n.broad < p_.broad_least) {
            Hold(kLight, Cut(dial_[kLight], p_.light_cut_share), "held back: broad contrast under 0.92x", s.now_ms);
        } else {
            // Near black falls as the dial rises: signs flipped so that "up" means more effect.
            const float band = p_.band_share * (n.dark_before - dark_goal);
            Take(kLight, Bounded(dial_[kLight], a.light, -n.dark_after, -n.dark_before, -dark_goal, -dark_goal - band, -dark_goal + band));
        }

        SteerParts(s, part_cut, broken);

        // Key: hold the average screen brightness where the game put it, as long as that does
        // not itself push the picture into white or black.
        const bool adding = dial_[kTexture] > 0 || dial_[kLight] > 0 || dial_[kColour] > 0 || Dials().part[0] > 0 || Dials().part[1] > 0;
        if (!adding) {
            dial_[kKey] = 0;  // exactly zero, so that an idle enhancer leaves the picture bit for bit
            why_[kKey] = "nothing is being added";
        } else if (key_cut) {
            // Already moved above, by the limit it broke.
        } else if (std::fabs(n.key_shift) <= p_.key_band) {
            why_[kKey] = "in band";
        } else {
            const float step = (std::clamp)(-p_.key_follow * n.key_shift, -p_.key_step, p_.key_step);
            const float next = (std::clamp)(dial_[kKey] + step, (std::max)(-p_.key_most, key_low_), (std::min)(p_.key_most, key_high_));
            if (step > 0 && (next <= dial_[kKey] || white_rise > 0.5f * p_.white_rise_most)) {
                why_[kKey] = "the picture got darker on average, but a brighter key would push more of it to pure white: held";
            } else if (step < 0 && (next >= dial_[kKey] || (!want_light && dark_rise > 0.5f * p_.dark_rise_most))) {
                why_[kKey] = "the picture got brighter on average, but a darker key would push more of it to black: held";
            } else {
                dial_[kKey] = next;
                why_[kKey] = step < 0 ? "the picture got brighter on average: key down" : "the picture got darker on average: key up";
            }
        }
    }

    // A dial that a hard limit cut may not pass the value it was cut to until there has been time
    // to see whether the scene has room again (or the scene changes).
    void Hold(int dial, float value, const char* why, uint64_t now)
    {
        dial_[dial] = value;
        why_[dial] = why;
        ceiling_[dial] = value;
        ceiling_why_[dial] = why;
        ceiling_ms_[dial] = now;
        MarkScene();
    }
    void Relax(uint64_t now)
    {
        for (int i = 0; i < kDials; ++i)
            if (ceiling_[i] < p_.dial_most && now >= ceiling_ms_[i] + p_.retry_ms) {
                ceiling_[i] = (std::min)(ceiling_[i] + p_.step_fixed, p_.dial_most);
                ceiling_ms_[i] = now;
            }
        if ((key_high_ < p_.key_most || key_low_ > -p_.key_most) && now >= key_ms_ + p_.retry_ms) {
            key_high_ = p_.key_most;
            key_low_ = -p_.key_most;
        }
    }

    // The shared shadow budget: both layers darken, and together they stay under the budget.
    // Darkening is linear in a dial, so what a step would add is known before it is taken.
    void SteerParts(const Sample& s, int part_cut, const char* broken)
    {
        float mean[2], worst[2];
        for (int i = 0; i < 2; ++i) {
            const int d = kPartA + i;
            if (!s.part[i].present) {
                dial_[d] = 0;
                if (!off_[i + 1]) why_[d] = "no layer from this part";
            }
            const float on = off_[i + 1] ? 0.f : dial_[d];
            mean[i] = on * (std::max)(Usable(s.part[i].mean_darkening), 0.f);
            worst[i] = on * (std::max)(Usable(s.part[i].worst_tile), 0.f);
        }
        if (part_cut >= 0) {
            dial_[kPartA + part_cut] = Cut(dial_[kPartA + part_cut], p_.cut_share);
            why_[kPartA + part_cut] = broken;
            return;
        }
        const bool over_mean = mean[0] + mean[1] > p_.shadow_mean, over_worst = worst[0] + worst[1] > p_.shadow_worst;
        if (over_mean || over_worst) {
            const float* by = over_mean ? mean : worst;
            const int larger = by[1] > by[0] ? 1 : 0;
            dial_[kPartA + larger] = Cut(dial_[kPartA + larger], p_.cut_share);
            why_[kPartA + larger] = over_mean ? "cut: the two layers together darken more than 0.010 stops on average"
                                              : "cut: the two layers together darken a tile by more than 0.040 stops";
            return;
        }
        // Room left: the smaller contributor steps up first, as long as the step stays inside.
        const int order[2] = {mean[0] <= mean[1] ? 0 : 1, mean[0] <= mean[1] ? 1 : 0};
        float sum_mean = mean[0] + mean[1], sum_worst = worst[0] + worst[1];
        for (int k = 0; k < 2; ++k) {
            const int i = order[k], d = kPartA + i;
            if (!s.part[i].present || off_[i + 1]) continue;
            if (dial_[d] >= p_.part_goal) {
                dial_[d] = p_.part_goal;
                why_[d] = "at its own strength";
                continue;
            }
            const float step = (std::min)(p_.step_fixed, p_.part_goal - dial_[d]);
            const float more_mean = step * (std::max)(Usable(s.part[i].mean_darkening), 0.f);
            const float more_worst = step * (std::max)(Usable(s.part[i].worst_tile), 0.f);
            if (sum_mean + more_mean > p_.shadow_mean || sum_worst + more_worst > p_.shadow_worst) {
                why_[d] = "held: one more step would take the two layers over the shared shadow budget";
                continue;
            }
            dial_[d] += step;
            sum_mean += more_mean;
            sum_worst += more_worst;
            why_[d] = "inside the shared shadow budget: fixed step up";
        }
    }
    static float Usable(float v) { return std::isfinite(v) ? v : 0.f; }

    void Take(int dial, const Check& c)
    {
        dial_[dial] = c.dial;
        why_[dial] = c.why;
        if (c.dead) {
            dead_[dial] = true;
            MarkScene();
        } else if (dial_[dial] > ceiling_[dial]) {
            dial_[dial] = ceiling_[dial];
            why_[dial] = ceiling_why_[dial];
        }
    }

    Params p_;
    float dial_[kDials] = {};
    const char* why_[kDials] = {"no sample yet", "no sample yet", "no sample yet", "no sample yet", "no sample yet", "no sample yet"};
    bool dead_[kDials] = {};
    float ceiling_[kDials] = {1.5f, 1.5f, 1.5f, 1.5f, 1.5f, 1.5f};
    const char* ceiling_why_[kDials] = {"", "", "", "", "", ""};
    uint64_t ceiling_ms_[kDials] = {};
    float key_high_ = 0.5f, key_low_ = -0.5f;  // where a hard limit stopped the key dial
    uint64_t key_ms_ = 0;
    na::na1::ref::Scene scene_;
    float w_range_ = 0;
    bool scene_known_ = false;
    uint64_t scene_ms_ = 0;
    bool marked_ = false;
    float mark_key_ = 0, mark_mid_ = 0;
    bool off_[3] = {};
    uint64_t off_at_[3] = {};
    const char* off_why_[3] = {"", "", ""};
    float frame_ms_[kWindowMost] = {}, own_ms_[kWindowMost] = {};
    uint32_t frame_n_ = 0, own_n_ = 0;
    const char* state_why_ = "waiting for the first measurement";
    Numbers last_;
    uint64_t samples_ = 0;
};

}  // namespace na::enhancer
