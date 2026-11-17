// P4-4 read-time render-scale substitution: key finding and same-length value rewriting.
#include "test_support.hpp"
#include "host_dx12/scale/render_scale_patch.hpp"

#include <string>

namespace {
std::string patch(std::string s, double f, int* n = nullptr, na::scale::Match* m = nullptr, bool tag = false,
                  na::scale::PatchInfo* out = nullptr)
{
    na::scale::PatchInfo info;
    const int k = na::scale::PatchRenderScale(s.data(), s.size(), f, &info, tag);
    if (m) *m = info.m;
    if (out) *out = info;
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
    // Tagged (live switching): last decimal nudged; integers cannot be tagged and stay exact.
    {
        int n = 0;
        CHECK(patch("RenderScale = 1.000000\n", 0.666667, &n, nullptr, true) == "RenderScale = 0.666668\n" && n == 1);
        CHECK(patch("ScreenPercentage=100\n", 0.5, &n, nullptr, true) == "ScreenPercentage= 50\n" && n == 1);
    }
    // Tag status is reported per rewritten entry (Astra 2026-11-01): a short field is rewritten
    // but NOT marked, so live discovery must not start from it.
    {
        PatchInfo info;
        CHECK(patch("RenderScale = 1.000000\n", 0.666667, nullptr, nullptr, true, &info) == "RenderScale = 0.666668\n");
        CHECK(info.have && info.tagged && std::string(info.original) == "1.000000" && std::string(info.written) == "0.666668");
        CHECK(patch("RenderScale=1.0\n", 0.5, nullptr, nullptr, true, &info) == "RenderScale=0.5\n");
        CHECK(info.have && !info.tagged && std::string(info.written) == "0.5");
        // The first entry cannot be rewritten (too short for 0.667); the second can: it is the one reported.
        CHECK(patch("RenderScale=1.0\nResolutionScale = 1.000000\n", 0.666667, nullptr, nullptr, true, &info) ==
              "RenderScale=1.0\nResolutionScale = 0.666668\n");
        CHECK(info.patched == 1 && info.have && std::string(info.m.key) == "ResolutionScale" && std::string(info.original) == "1.000000");
        // Nothing rewritten: `have` is false, the first entry found is still described.
        CHECK(patch("RenderScale=1.0\n", 0.666667, nullptr, nullptr, true, &info) == "RenderScale=1.0\n");
        CHECK(!info.have && info.patched == 0 && info.m.current == 1.0);
        // A marker too fine for float precision is not claimed as a marker.
        CHECK(!detail::tag_survives("0.66666670000001", "0.66666670000000"));
        CHECK(detail::tag_survives("0.666668", "0.666667"));
    }
    // UTF-16LE file (BOM FF FE): patched through a narrow view, same byte length, still UTF-16.
    {
        const std::string ascii = "[Display Settings]\r\nRenderScale = 1.000000\r\n";
        std::string w = "\xFF\xFE";
        for (char c : ascii) { w += c; w += '\0'; }
        const std::string orig = w;
        CHECK(LooksUtf16LE(w.data(), w.size()) && !LooksUtf16LE(ascii.data(), ascii.size()));
        PatchInfo info;
        bool wide = false;
        CHECK(PatchRenderScaleText(w.data(), w.size(), 0.666667, &info, true, &wide) == 1 && wide);
        CHECK(w.size() == orig.size() && w[0] == '\xFF' && w[1] == '\xFE');
        CHECK(NarrowUtf16LE(w.data(), w.size()).find("RenderScale = 0.666668\r\n") != std::string::npos);
        CHECK(info.tagged && std::string(info.original) == "1.000000");
        std::string a2 = ascii; // plain text still goes the normal way
        CHECK(PatchRenderScaleText(a2.data(), a2.size(), 0.666667, nullptr, false, &wide) == 1 && !wide);
    }
    return test::finish("render_scale_patch_test");
}
