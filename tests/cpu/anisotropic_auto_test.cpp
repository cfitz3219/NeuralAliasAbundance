// Automatic anisotropic filtering (core/anisotropic_auto.hpp): the four world-sampler rules,
// the game-has-AF detector, its hysteresis and the hidden setting's three modes.
#include "test_support.hpp"
#include "core/anisotropic_auto.hpp"

#include <string>

using namespace na::af;

namespace {
// D3D11_FILTER / D3D12_FILTER encodings.
constexpr uint32_t kTrilinear = 0x15, kBilinear = 0x14, kPoint = 0x00, kMinMagPointMipLinear = 0x01, kAniso = 0x55;
constexpr uint32_t kCmpLinear = 0x95, kCmpAniso = 0xD5, kMinimumLinear = 0x115, kMaximumLinear = 0x195, kMinLinearMagPoint = 0x11;
constexpr uint32_t kWrap = 1, kMirror = 2, kClamp = 3, kBorder = 4, kMirrorOnce = 5;

Facts F(uint32_t filter, uint32_t u = kWrap, uint32_t v = kWrap, uint32_t aniso = 1, float max_lod = 1000.0f, float bias = 0.0f)
{
    Facts f;
    f.filter = filter;
    f.address_u = u;
    f.address_v = v;
    f.max_anisotropy = aniso;
    f.max_lod = max_lod;
    f.mip_lod_bias = bias;
    return f;
}

uint32_t g_last_action = 99;
int g_calls = 0;
std::string g_last_report;
void on_change(uint32_t a) { g_last_action = a; ++g_calls; }
void on_report(const char* line) { g_last_report = line; }
} // namespace

int main()
{
    return test::run({
        {"rule 1: comparison and min/max samplers are never world samplers", [] {
            CHECK(Classify(F(kCmpLinear)) == Kind::Comparison);
            CHECK(Classify(F(kCmpAniso, kWrap, kWrap, 16)) == Kind::Comparison);
            CHECK(Classify(F(kMinimumLinear)) == Kind::MinMax);
            CHECK(Classify(F(kMaximumLinear)) == Kind::MinMax);
            auto c = F(kCmpLinear);
            CHECK(!Apply(c, kForce16) && c.filter == kCmpLinear);
        }},
        {"rule 2: point filtering stays (min or mag point)", [] {
            CHECK(Classify(F(kPoint)) == Kind::Point);
            CHECK(Classify(F(kMinMagPointMipLinear)) == Kind::Point);
            CHECK(Classify(F(kMinLinearMagPoint)) == Kind::Point);
            CHECK(Classify(F(kBilinear)) == Kind::World); // linear min/mag with point mips: a low-quality world sampler
        }},
        {"rule 3: no mips (MaxLOD 0) stays", [] {
            CHECK(Classify(F(kTrilinear, kWrap, kWrap, 1, 0.0f)) == Kind::NoMips);
            CHECK(Classify(F(kAniso, kWrap, kWrap, 16, 0.0f)) == Kind::NoMips);
        }},
        {"rule 4: only WRAP/MIRROR on U or V; CLAMP/BORDER stays", [] {
            CHECK(Classify(F(kTrilinear, kClamp, kClamp)) == Kind::NotTiled);
            CHECK(Classify(F(kTrilinear, kBorder, kClamp)) == Kind::NotTiled);
            CHECK(Classify(F(kTrilinear, kClamp, kWrap)) == Kind::World);
            CHECK(Classify(F(kTrilinear, kMirror, kClamp)) == Kind::World);
            CHECK(Classify(F(kTrilinear, kMirrorOnce, kMirrorOnce)) == Kind::World);
        }},
        {"automatic raises world samplers below 8x and leaves the game's 8x/16x", [] {
            auto t = F(kTrilinear);
            CHECK(Apply(t, kAuto16) && t.filter == kAniso && t.max_anisotropy == 16);
            auto low = F(kAniso, kWrap, kWrap, 4);
            CHECK(Apply(low, kAuto16) && low.max_anisotropy == 16);
            auto eight = F(kAniso, kWrap, kWrap, 8);
            CHECK(!Apply(eight, kAuto16) && eight.max_anisotropy == 8);
            auto sixteen = F(kAniso, kWrap, kWrap, 16);
            CHECK(!Apply(sixteen, kAuto16));
            auto keep = F(kTrilinear);
            CHECK(!Apply(keep, kKeep) && keep.filter == kTrilinear);
        }},
        {"force 16x raises 8x world samplers too, still never touches non-world", [] {
            auto eight = F(kAniso, kWrap, kWrap, 8);
            CHECK(Apply(eight, kForce16) && eight.max_anisotropy == 16);
            auto hud = F(kTrilinear, kClamp, kClamp);
            CHECK(!Apply(hud, kForce16));
            auto lut = F(kPoint);
            CHECK(!Apply(lut, kForce16));
        }},
        {"a game that filters its world samplers at 16x: ours stays off", [] {
            Detector d;
            d.SetOnChange(&on_change);
            g_calls = 0;
            CHECK(d.Current() == kKeep);
            d.Observe(F(kAniso, kWrap, kWrap, 16));
            CHECK(d.Get().state == Detector::State::GameHasAf);
            CHECK(d.Current() == kKeep);
            CHECK(g_calls == 0); // keep -> keep: nothing to switch
            // Post-processing, shadows and LUTs do not count either way.
            d.Observe(F(kTrilinear, kClamp, kClamp));
            d.Observe(F(kCmpLinear, kClamp, kClamp));
            d.Observe(F(kPoint));
            d.Observe(F(kTrilinear, kWrap, kWrap)); // one tiled trilinear: 1 of 2 still at 16x (50 %)
            CHECK(d.Get().state == Detector::State::GameHasAf);
            const auto s = d.Get();
            CHECK(s.world == 2 && s.world_af8 == 1);
            CHECK(s.rejected[int(Kind::NotTiled)] == 1 && s.rejected[int(Kind::Comparison)] == 1 && s.rejected[int(Kind::Point)] == 1);
        }},
        {"a game without AF: world samplers are raised, decision reported with its reason", [] {
            Detector d;
            d.SetOnChange(&on_change);
            d.SetOnReport(&on_report);
            g_calls = 0;
            g_last_report.clear();
            CHECK(d.Observe(F(kTrilinear)));
            CHECK(d.Current() == kAuto16 && g_last_action == kAuto16 && g_calls == 1);
            CHECK(g_last_report.find("game_lacks_af") != std::string::npos);
            CHECK(g_last_report.find("why=") != std::string::npos);
            // Repeats of one description (a sampler re-created every frame) count once.
            for (int i = 0; i < 100; ++i) d.Observe(F(kAniso, kWrap, kWrap, 16));
            CHECK(d.Get().world == 2 && d.Get().observed == 101);
            CHECK(d.Current() == kAuto16); // 50 %: below the 60 % needed to flip
        }},
        {"hysteresis: flips to the game's own at 60 % and back below 40 %", [] {
            Detector d;
            d.SetOnChange(&on_change);
            d.Observe(F(kTrilinear));                         // 0/1 -> lacks
            d.Observe(F(kAniso, kWrap, kWrap, 16));           // 1/2 = 50 %
            CHECK(d.Current() == kAuto16);
            d.Observe(F(kAniso, kWrap, kWrap, 16, 1000.0f, -0.5f)); // 2/3 = 66 %
            CHECK(d.Current() == kKeep && g_last_action == kKeep);
            d.Observe(F(kTrilinear, kMirror, kMirror));       // 2/4 = 50 %: stays
            CHECK(d.Current() == kKeep);
            d.Observe(F(kBilinear));                          // 2/5 = 40 %: stays
            CHECK(d.Current() == kKeep);
            d.Observe(F(kTrilinear, kWrap, kClamp));          // 2/6 = 33 %: back
            CHECK(d.Current() == kAuto16 && d.Get().changes == 3);
        }},
        {"the hidden setting: 0 = game's own, 16 = force, -1 = automatic, all live", [] {
            CHECK(ModeFromSetting(-1) == Mode::Auto && ModeFromSetting(0) == Mode::Off && ModeFromSetting(16) == Mode::Force);
            CHECK(ModeFromSetting(8) == Mode::Force && ModeFromSetting(-5) == Mode::Auto);
            Detector d;
            d.SetOnChange(&on_change);
            d.Observe(F(kAniso, kWrap, kWrap, 16)); // game has it
            CHECK(d.Current() == kKeep);
            CHECK(d.SetMode(Mode::Force) && d.Current() == kForce16 && g_last_action == kForce16);
            CHECK(d.SetMode(Mode::Off) && d.Current() == kKeep);
            CHECK(!d.SetMode(Mode::Off));
            CHECK(!d.SetMode(Mode::Auto)); // automatic: the game has it -> still the game's own
            d.Observe(F(kTrilinear));
            d.Observe(F(kBilinear));
            d.Observe(F(kTrilinear, kMirror, kWrap)); // 1/4 = 25 %
            CHECK(d.Current() == kAuto16);
            CHECK(!d.SetMode(Mode::Auto));
        }},
        {"the log line names the mode, the decision and what was left alone", [] {
            Detector d;
            d.Observe(F(kTrilinear));
            d.Observe(F(kCmpLinear, kClamp, kClamp));
            char line[600];
            CHECK(Detector::Format(d.Get(), line, sizeof line) > 0);
            const std::string s = line;
            CHECK(s.find("mode=auto") != std::string::npos);
            CHECK(s.find("state=game_lacks_af") != std::string::npos);
            CHECK(s.find("action=raise_below_8x_to_16x") != std::string::npos);
            CHECK(s.find("comparison=1") != std::string::npos);
        }},
    });
}
