// The Referee's control rule for the enhancer (docs/NA1-ENHANCER.md section 5), on simulated
// scenes: each "plant" answers the dials the way a picture would.
#include "core/enhancer_governor.hpp"
#include "test_support.hpp"

#include <cstring>
#include <functional>

using na::enhancer::Governor;
using na::enhancer::Numbers;
using na::na1::ref::Dials;

namespace {
// An ordinary daylight scene: little near black (no shadows to open), moderate colour.
Numbers Daylight()
{
    Numbers n;
    n.valid = true;
    n.colour_before = n.colour_after = .40f;
    n.variety_before = n.variety_after = .45f;
    n.dark_before = n.dark_after = 1.f;
    n.white_before = n.white_after = 1.f;
    n.m0 = .2f; n.key_stops = 2.32f; n.range = 3.f; n.mid = -3.f; n.mean_y = -3.f;
    return n;
}
// The same with deep shadows and a wide lighting range: the light dial has work to do.
Numbers Contrasty()
{
    Numbers n = Daylight();
    n.dark_before = n.dark_after = 10.f;
    n.range = 5.5f; n.w_range = 1.f;
    return n;
}
// A scene that answers each dial in a straight line.
struct Plant {
    Numbers base = Daylight();
    float texture_per = .10f, variety_per = .25f, dark_per = .5f, key_per_texture = 0;
    std::function<void(const Dials&, Numbers&)> extra;
    Numbers At(const Dials& d) const
    {
        Numbers n = base;
        n.texture = 1.f + texture_per * d.texture;
        n.variety_after = n.variety_before * (1.f + variety_per * d.colour);
        n.colour_after = n.colour_before * (1.f + .5f * variety_per * d.colour);
        n.dark_after = n.dark_before * (1.f - dark_per * d.light);
        n.key_shift = d.key + key_per_texture * d.texture;
        if (extra) extra(d, n);
        return n;
    }
};
struct Loop {
    Governor governor;
    Plant plant;
    uint64_t now = 0;
    float frame_ms = 10.f, gpu_ms = .2f;
    Governor::Part part[2];
    bool Step(bool reset = false)
    {
        Governor::Sample s;
        s.n = governor.Suspended() ? Numbers() : plant.At(governor.Dials());
        s.frame_ms = frame_ms;
        s.gpu_ms = governor.Suspended() ? 0.f : gpu_ms;
        s.part[0] = part[0]; s.part[1] = part[1];
        s.reset = reset;
        s.now_ms = now += 250;
        return governor.Observe(s);
    }
    void Run(int samples) { for (int i = 0; i < samples; ++i) Step(); }
    Dials D() const { return governor.Dials(); }
    bool Says(int dial, const char* text) const { return std::strstr(governor.DialWhy(dial), text) != nullptr; }
};
bool Within(float v, float lo, float hi) { return v >= lo && v <= hi; }
}

int main()
{
    return test::run({
        {"texture reaches its band from zero within 20 samples, by bounded steps, and rests there", [] {
            Loop l;
            int reached = -1;
            for (int i = 0; i < 40; ++i) {
                const float before = l.D().texture;
                const bool changed = l.Step();
                const float after = l.D().texture;
                CHECK(std::fabs(after - before) <= (std::max)(.25f * before, .05f) + 1e-6f);
                const float measured = l.plant.At(l.D()).texture;
                if (reached < 0 && Within(measured, 1.08f, 1.12f)) reached = i + 1;
                if (reached >= 0 && i > reached) {
                    CHECK(after == before);   // in band: hold
                    CHECK(!changed);          // and nothing to log
                }
            }
            CHECK(reached > 0 && reached <= 20);
            CHECK(Within(l.plant.At(l.D()).texture, 1.08f, 1.12f));
            CHECK(l.Says(Governor::kTexture, "in band"));
            CHECK(l.governor.Samples() == 40);
        }},
        {"a weak and a strong response both end in the band", [] {
            for (float per : {.07f, .2f, .6f}) {
                Loop l;
                l.plant.texture_per = per;
                l.Run(40);
                CHECK(Within(l.plant.At(l.D()).texture, 1.08f, 1.12f));
            }
        }},
        {"no response: fixed steps to the top, then back to zero until the scene changes", [] {
            Loop l;
            l.plant.texture_per = 0;
            for (int i = 1; i <= 30; ++i) {
                l.Step();
                NEAR(l.D().texture, .05f * float(i), 1e-4);
            }
            CHECK(l.Says(Governor::kTexture, "fixed step"));
            CHECK(l.Step());
            NEAR(l.D().texture, 0.f, 0);
            CHECK(l.Says(Governor::kTexture, "no response"));
            l.Run(200);
            NEAR(l.D().texture, 0.f, 0);
            NEAR(l.D().key, 0.f, 0);
            l.Step(true);  // a camera cut: a new scene may answer
            NEAR(l.D().texture, .05f, 1e-6);
        }},
        {"a dial that got no response tries again when the picture's brightness has moved a long way", [] {
            Loop l;
            l.plant.texture_per = 0;
            l.Run(40);
            NEAR(l.D().texture, 0.f, 0);
            l.plant.base.m0 = .6f;  // out of the tunnel: 1.6 stops brighter
            l.plant.texture_per = .1f;
            l.Run(60);
            CHECK(Within(l.plant.At(l.D()).texture, 1.08f, 1.12f));
        }},
        {"each hard limit cuts texture by a fifth and holds it under what broke", [] {
            struct Case { const char* says; std::function<void(const Dials&, Numbers&)> extra; };
            const Case cases[] = {
                {"pure white", [](const Dials& d, Numbers& n) { if (d.texture > .55f) n.white_after = n.white_before + .35f; }},
                {"edge glow", [](const Dials& d, Numbers& n) { if (d.texture > .55f) n.glow = .035f; }},
                {"broad contrast over", [](const Dials& d, Numbers& n) { if (d.texture > .55f) n.broad = 1.11f; }},
                {"near black", [](const Dials& d, Numbers& n) { if (d.texture > .55f) n.dark_after = n.dark_before + .6f; }},
            };
            for (const Case& c : cases) {
                Loop l;
                l.Run(40);
                const float rest = l.D().texture;
                CHECK(rest > .8f);
                l.plant.extra = c.extra;
                CHECK(l.Step());
                NEAR(l.D().texture, rest * .8f, 1e-5);
                CHECK(l.Says(Governor::kTexture, c.says));
                l.Run(40);
                CHECK(l.D().texture <= .55f && l.D().texture > .4f);   // under the limit, not at zero
                CHECK(l.Says(Governor::kTexture, c.says));             // the log still says what holds it
                const float held = l.D().texture;
                l.Run(100);                                             // 30 seconds on: one step of room is tried
                CHECK(l.D().texture <= held + .05f + 1e-6f);
                CHECK(l.plant.At(l.D()).white_after - l.plant.At(l.D()).white_before <= .3f || l.D().texture < held + .06f);
            }
        }},
        {"small cuts are at least 0.05, down to zero", [] {
            Loop l;
            l.plant.extra = [](const Dials& d, Numbers& n) { if (d.texture > 0) n.glow = .05f; };
            l.Step();
            NEAR(l.D().texture, .05f, 1e-6);
            l.Step();
            NEAR(l.D().texture, 0.f, 0);
            l.Run(20);
            NEAR(l.D().texture, 0.f, 0);
        }},
        {"near black rising is not a limit when shadows are being opened, and broad under 0.92 cuts light", [] {
            Loop l;
            l.plant.base = Contrasty();
            l.plant.extra = [](const Dials& d, Numbers& n) { n.dark_after += .8f * d.texture; };  // texture's troughs
            l.Run(60);
            CHECK(Within(l.plant.At(l.D()).texture, 1.08f, 1.12f));
            CHECK(l.D().light > .3f);
            Loop m;
            m.plant.base = Contrasty();
            m.plant.extra = [](const Dials& d, Numbers& n) { if (d.light > .32f) n.broad = .91f; };
            float before = 0;
            for (int i = 0; i < 40 && !m.Says(Governor::kLight, "broad contrast under"); ++i) { before = m.D().light; m.Step(); }
            CHECK(m.Says(Governor::kLight, "broad contrast under"));
            NEAR(m.D().light, before - (std::max)(.15f * before, .05f), 1e-5);
            m.Run(40);
            CHECK(m.D().light <= .32f && m.D().light > .2f);
            CHECK(Within(m.plant.At(m.D()).texture, 1.08f, 1.12f));  // texture is not what gives way
        }},
        {"colour goes for the game's own times 1.12, and stays at zero when the game is already past 0.60", [] {
            Loop l;
            l.Run(40);
            NEAR(l.plant.At(l.D()).variety_after / .45f, 1.12f, .12f * .15f + 1e-4);
            CHECK(l.D().colour > .3f);
            l.plant.base.colour_before = .65f;   // the scene turns colourful
            l.Step();
            NEAR(l.D().colour, 0.f, 0);
            CHECK(l.Says(Governor::kColour, "ceiling"));
            l.Run(30);
            NEAR(l.D().colour, 0.f, 0);
            Loop near_ceiling;                   // 0.57: only up to 0.60, a gain of 1.053
            near_ceiling.plant.base.colour_before = .57f;
            near_ceiling.Run(40);
            const float gain = near_ceiling.plant.At(near_ceiling.D()).variety_after / .45f;
            CHECK(gain > 1.04f && gain < 1.065f);
        }},
        {"light opens shadows in a contrasty scene and stays at zero in a dark one or a flat one", [] {
            Loop l;
            l.plant.base = Contrasty();
            l.Run(40);
            NEAR(l.plant.At(l.D()).dark_after, 7.f, .45f + 1e-3);  // down by 30%, within 15% of the gap
            CHECK(l.Says(Governor::kLight, "in band"));
            Loop night;
            night.plant.base = Contrasty();
            night.plant.base.m0 = .04f; night.plant.base.w_dark = 1.f;
            night.Run(40);
            NEAR(night.D().light, 0.f, 0);
            CHECK(night.Says(Governor::kLight, "dark scene"));
            CHECK(Within(night.plant.At(night.D()).texture, 1.08f, 1.12f));  // a dark scene is still enhanced
            Loop flat;
            flat.plant.base = Contrasty();
            flat.plant.base.w_range = 0;
            flat.Run(40);
            NEAR(flat.D().light, 0.f, 0);
            CHECK(flat.Says(Governor::kLight, "narrow lighting range"));
            Loop few;
            few.Run(40);
            NEAR(few.D().light, 0.f, 0);
            CHECK(few.Says(Governor::kLight, "2%"));
            Loop dusk;  // half dark: shadows opened half as far
            dusk.plant.base = Contrasty();
            dusk.plant.base.w_dark = .5f;
            dusk.Run(40);
            NEAR(dusk.plant.At(dusk.D()).dark_after, 8.5f, .225f + 1e-3);
        }},
        {"near black is never taken under 2% of the picture", [] {
            Loop l;
            l.plant.base = Contrasty();
            l.plant.base.dark_before = 2.4f;  // 30% off would be 1.68
            l.Run(40);
            NEAR(l.plant.At(l.D()).dark_after, 2.f, .06f + 1e-3);
        }},
        {"the key dial brings the average brightness back to within the band, 0.1 stops a check at most", [] {
            Loop l;
            l.plant.key_per_texture = .4f;  // texture brightens this picture a lot
            for (int i = 0; i < 60; ++i) {
                const float before = l.D().key;
                l.Step();
                CHECK(std::fabs(l.D().key - before) <= .1f + 1e-6f);
            }
            CHECK(Within(l.plant.At(l.D()).texture, 1.08f, 1.12f));
            CHECK(std::fabs(l.plant.At(l.D()).key_shift) <= .01f);
            CHECK(l.D().key < -.3f);
            CHECK(l.Says(Governor::kKey, "in band"));
            Loop far;
            far.plant.key_per_texture = -2.f;  // more than the dial may give
            far.Run(60);
            NEAR(far.D().key, .5f, 1e-6);
        }},
        {"the key dial does not push the picture into white, and gives way first when it has", [] {
            Loop l;
            l.plant.extra = [](const Dials&, Numbers& n) { n.key_shift -= .05f; n.white_after = n.white_before + .2f; };
            l.Run(40);
            NEAR(l.D().key, 0.f, 0);
            CHECK(l.Says(Governor::kKey, "pure white"));
            CHECK(Within(l.plant.At(l.D()).texture, 1.08f, 1.12f));
            Loop m;
            m.plant.extra = [](const Dials& d, Numbers& n) { n.key_shift -= .05f; if (d.key > .03f) n.white_after = n.white_before + .4f; };
            m.Run(40);
            NEAR(m.D().key, 0.f, 0);                                           // cut, and not stepped back over
            CHECK(Within(m.plant.At(m.D()).texture, 1.08f, 1.12f));            // texture never paid for it
            CHECK(m.plant.At(m.D()).white_after - m.plant.At(m.D()).white_before <= .3f);
        }},
        {"with nothing added the key is exactly zero", [] {
            Loop l;
            l.plant.texture_per = 0;
            l.plant.base.colour_before = .7f;
            l.plant.key_per_texture = .1f;
            l.Run(10);
            CHECK(l.D().key < 0);
            l.Run(40);
            NEAR(l.D().texture, 0.f, 0);
            CHECK(l.D().key == 0.f && l.D().colour == 0.f && l.D().light == 0.f);
            CHECK(l.Says(Governor::kKey, "nothing is being added"));
        }},
        {"over the time allowance the costliest part is switched off, and tried again later", [] {
            Loop l;
            l.gpu_ms = .4f;
            l.part[0].present = l.part[1].present = true;
            l.part[0].gpu_ms = .8f; l.part[1].gpu_ms = .3f;
            l.part[0].mean_darkening = l.part[1].mean_darkening = .001f;
            l.Run(30);
            CHECK(l.D().part[0] > .9f && l.D().part[1] > .9f);
            l.frame_ms = 25.f;  // 40 fps
            int after = 0;
            while (l.D().part[0] > 0 && after < 12) { l.Step(); ++after; }
            CHECK(after >= 4 && after <= 8);                    // the median has to move first
            CHECK(l.Says(Governor::kPartA, "frame time over 22.2 ms"));
            CHECK(l.D().part[1] > .9f && !l.governor.Suspended());
            l.frame_ms = 15.f;  // that was it
            l.Run(100);         // 25 seconds: not yet
            NEAR(l.D().part[0], 0.f, 0);
            l.Run(30);          // past 30 seconds, and above 55 fps
            CHECK(l.D().part[0] > .9f);
            CHECK(!l.Says(Governor::kPartA, "frame time"));
            // Still slow with it off: the next costliest goes, which here is NA1 itself.
            l.frame_ms = 25.f;
            l.Run(8);
            NEAR(l.D().part[0], 0.f, 0);
            l.Run(8);
            CHECK(l.governor.Suspended());
            CHECK(std::strstr(l.governor.Why(), "frame time over 22.2 ms"));
            l.Run(200);         // the game stays slow: nothing comes back
            CHECK(l.governor.Suspended());
            l.frame_ms = 12.f;
            l.Run(6);
            CHECK(!l.governor.Suspended());                     // NA1 first, it went last; one at a time
            NEAR(l.D().part[0], 0.f, 0);
            l.Run(20);
            CHECK(l.D().part[0] > .9f);
        }},
        {"a slow game is not blamed on an enhancer that costs next to nothing", [] {
            Loop l;
            l.gpu_ms = .3f;
            l.frame_ms = 30.f;
            l.Run(60);
            CHECK(!l.governor.Suspended());
            CHECK(Within(l.plant.At(l.D()).texture, 1.08f, 1.12f));
        }},
        {"NA1's own passes over 3 ms on average are suspended and tried again after 30 seconds", [] {
            Loop l;
            l.gpu_ms = 3.4f;
            l.Run(3);
            CHECK(!l.governor.Suspended());
            CHECK(l.Step());
            CHECK(l.governor.Suspended());
            CHECK(std::strstr(l.governor.Why(), "3 ms"));
            const float kept = l.D().texture;
            l.Run(110);
            CHECK(l.governor.Suspended());
            l.gpu_ms = 1.f;
            l.Run(12);
            CHECK(!l.governor.Suspended());
            CHECK(l.D().texture >= kept);                       // it carries on from where it was
            CHECK(std::strstr(l.governor.Why(), "running"));
        }},
        {"the two external layers share one shadow budget", [] {
            Loop l;
            l.part[0].present = true;
            l.part[0].mean_darkening = .019f;  // at its own strength it alone would be nearly twice the budget
            l.part[0].worst_tile = .03f;
            l.Run(40);
            NEAR(l.D().part[0], .5f, 1e-5);
            CHECK(l.Says(Governor::kPartA, "shared shadow budget"));
            CHECK(l.Says(Governor::kPartB, "no layer"));
            NEAR(l.D().part[1], 0.f, 0);
            l.part[1].present = true;          // the second layer arrives; the first already fills the budget
            l.part[1].mean_darkening = .004f;
            l.Run(40);
            CHECK(l.D().part[0] * .019f + l.D().part[1] * .004f <= .010f + 1e-6f);
            l.part[0].mean_darkening = .05f;   // the scene changes and the first layer darkens much more
            const float before = l.D().part[0];
            l.Step();
            NEAR(l.D().part[0], before * .8f, 1e-5);  // the larger contributor is cut first
            CHECK(l.Says(Governor::kPartA, "0.010 stops"));
            l.Run(40);
            CHECK(l.D().part[0] * .05f + l.D().part[1] * .004f <= .010f + 1e-6f);
            CHECK(l.D().part[1] > 0);
            Loop tile;
            tile.part[1].present = true;
            tile.part[1].mean_darkening = .002f;
            tile.part[1].worst_tile = .09f;    // one tile much darker than the rest
            tile.Run(40);
            NEAR(tile.D().part[1], .4f, 1e-5);
            Loop free;
            free.part[0].present = free.part[1].present = true;
            free.part[0].mean_darkening = free.part[1].mean_darkening = .002f;
            free.Run(40);
            NEAR(free.D().part[0], 1.f, 1e-5);
            NEAR(free.D().part[1], 1.f, 1e-5);
            CHECK(free.Says(Governor::kPartA, "own strength"));
        }},
        {"scene values are smoothed over about two seconds and snap on a reset", [] {
            Loop l;
            l.Step();
            NEAR(l.governor.Scene().mid, -3.f, 0);     // the first sample is taken whole
            NEAR(l.governor.Scene().m0, .2f, 0);
            l.plant.base.mid = -1.f; l.plant.base.m0 = .3f; l.plant.base.p85 = .7f; l.plant.base.p97 = .8f;
            l.plant.base.tint[0] = 1.4f;
            l.Step();
            const float share = 1.f - std::exp(-250.f / 2000.f);
            NEAR(l.governor.Scene().mid, -3.f + 2.f * share, 1e-4);
            NEAR(l.governor.Scene().m0, .2f + .1f * share, 1e-5);
            NEAR(l.governor.Scene().tint[0], 1.f + .4f * share, 1e-5);
            l.Run(8);                                   // two seconds on: most of the way
            CHECK(l.governor.Scene().mid > -1.8f && l.governor.Scene().mid < -1.2f);
            l.plant.base.mid = -6.f; l.plant.base.w_dark = 1.f;
            l.Step(true);
            NEAR(l.governor.Scene().mid, -6.f, 0);
            NEAR(l.governor.Scene().w_dark, 1.f, 0);
            NEAR(l.governor.Scene().p85, .7f, 0);
            l.plant.base.m0 = 0;
            l.Step(true);
            CHECK(l.governor.Scene().m0 > 0);           // the picture maths divides by it
        }},
        {"the response is judged against the dials the frame was really drawn with", [] {
            Governor g;
            Plant plant;
            Governor::Sample s;
            for (int i = 0; i < 8; ++i) {  // up to 0.4 with applied following
                s.n = plant.At(g.Dials());
                s.now_ms += 250;
                g.Observe(s);
            }
            const float governed = g.Dials().texture;
            CHECK(governed > .3f);
            Dials lagging = g.Dials();
            lagging.texture = governed * .5f;  // the glide was only half way when the sample was taken
            s.n = plant.At(lagging);
            s.applied = lagging;
            s.have_applied = true;
            s.now_ms += 250;
            g.Observe(s);
            // The slope is the plant's own (0.1 a unit), so the goal is 1.0 and the capped step is a quarter.
            NEAR(g.Dials().texture, governed * 1.25f, 1e-5);
        }},
        {"a sample with nothing measured changes no dial", [] {
            Loop l;
            l.Run(5);
            const Dials before = l.D();
            Governor::Sample s;
            s.now_ms = l.now + 250;
            s.frame_ms = 10;
            CHECK(!l.governor.Observe(s));
            CHECK(l.D().texture == before.texture && l.D().colour == before.colour && l.D().key == before.key);
            Governor fresh;
            CHECK(std::strstr(fresh.Why(), "waiting"));
            NEAR(fresh.Dials().texture, 0.f, 0);
        }},
        {"applied dials glide to the governed ones and land exactly", [] {
            Dials governed, applied;
            applied.texture = 1.f; applied.key = -.2f; applied.part[1] = .5f;
            governed.light = .6f;
            float last = applied.texture;
            int frames = 0;
            while ((applied.texture != 0 || applied.light != .6f || applied.key != 0 || applied.part[1] != 0) && frames < 200) {
                applied = Governor::Glide(applied, governed, 16.7f);
                CHECK(applied.texture <= last && applied.texture >= 0 && applied.light <= .6f);
                last = applied.texture;
                ++frames;
            }
            CHECK(frames > 3 && frames < 40);  // no step, and there within a sample or two
            CHECK(applied.texture == 0.f && applied.light == .6f && applied.key == 0.f && applied.part[1] == 0.f);
            const Dials still = Governor::Glide(applied, governed, 0.f);
            CHECK(still.light == .6f);
        }},
        {"the log line carries the dials, the numbers and every why", [] {
            Loop l;
            l.plant.base = Contrasty();
            l.Run(12);
            char line[1024];
            const int n = l.governor.Format(line, sizeof line);
            CHECK(n > 0 && n < int(sizeof line));
            for (const char* part : {"sample 12", "texture", "light", "colour", "key", "black", "white", "glow", "scene m0", "running"})
                CHECK(std::strstr(line, part));
            char small[16];
            CHECK(l.governor.Format(small, sizeof small) > 15 && std::strlen(small) == 15);  // cut, never overrun
            for (int d = 0; d < Governor::kDials; ++d) CHECK(std::strlen(l.governor.DialWhy(d)) > 0);
            CHECK(std::strlen(l.governor.DialWhy(99)) == 0);
        }},
    });
}
