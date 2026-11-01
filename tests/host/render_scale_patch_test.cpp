// P4-4 read-time render-scale substitution: key finding and same-length value rewriting.
#include "test_support.hpp"
#include "host_dx12/scale/render_scale_patch.hpp"

#include <string>

namespace {
std::string patch(std::string s, double f, int* n = nullptr, na::scale::Match* m = nullptr)
{
    const int k = na::scale::PatchRenderScale(s.data(), s.size(), f, m);
    if (n) *n = k;
    return s;
}
} // namespace

int main()
{
    using namespace na::scale;
    // Sniper Elite 5 / Resistance GraphicsOptions.ini shape (comment line above the key).
    {
        const std::string ini = "[Display Settings]\r\n; RenderScale = 0.5 to 2.0\r\nRenderScale = 1.000000\r\nResolution_Width = 3440\r\n";
        int n = 0;
        Match m;
        const std::string out = patch(ini, 0.666667, &n, &m);
        std::printf("sniper: \"%s\"\n", out.c_str());
        CHECK(n == 1 && m.unit == Unit::Fraction && m.current == 1.0);
        CHECK(out.size() == ini.size());
        CHECK(out.find("RenderScale = 0.666667\r\n") != std::string::npos);
        CHECK(out.find("; RenderScale = 0.5 to 2.0") != std::string::npos); // comment untouched
        CHECK(out.find("Resolution_Width = 3440") != std::string::npos);
    }
    // JSON percentage, integer: leading space keeps the length.
    {
        const std::string js = "{\"ScreenPercentage\": 100, \"Vsync\": 1}";
        int n = 0;
        const std::string out = patch(js, 0.58, &n);
        std::printf("json: \"%s\"\n", out.c_str());
        CHECK(n == 1 && out == "{\"ScreenPercentage\":  58, \"Vsync\": 1}");
    }
    // Too short to write 0.667 within 1 % ("0.7" is 5 % off): left untouched. 0.5 fits.
    {
        int n = 0;
        const std::string out = patch("RenderScale=1.0\n", 0.666667, &n);
        CHECK(n == 0 && out == "RenderScale=1.0\n");
        const std::string half = patch("RenderScale=1.0\n", 0.5, &n);
        CHECK(n == 1 && half == "RenderScale=0.5\n");
    }
    // Whole identifiers only; quoted values; YAML.
    {
        int n = 0;
        CHECK(patch("DynamicResolutionScale = 1.000000\n", 0.666667, &n) == "DynamicResolutionScale = 1.000000\n" && n == 0);
        CHECK(patch("resolutionscale: \"1.000\"\n", 0.5, &n) == "resolutionscale: \"0.500\"\n" && n == 1);
        CHECK(patch("# RenderScale: 1.0\nGamma: 2.2\n", 0.5, &n) == "# RenderScale: 1.0\nGamma: 2.2\n" && n == 0);
        CHECK(patch("RenderScale = 1.0e0\n", 0.5, &n) == "RenderScale = 1.0e0\n" && n == 0);
    }
    // Percent detected from the value (> 4) even under a fraction-style key.
    {
        int n = 0;
        Match m;
        const std::string out = patch("ResolutionScale=100.0\n", 0.5, &n, &m);
        CHECK(m.unit == Unit::Percent);
        CHECK(n == 1 && out == "ResolutionScale=50.00\n");
    }
    return test::finish("render_scale_patch_test");
}
