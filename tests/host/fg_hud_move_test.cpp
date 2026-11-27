// Live Frame Generation: interface that moves with the scene (fg/hud_move.hpp, VARIANT hudpaste/moves-with-scene)
// against the T2 paste (fg/hud_paste.hpp, hudpaste/screen-fixed), on a panning map with labels drawn after the
// hand-over, a fixed interface panel, and a unit moving against the pan under one label.
// The generated picture is a perfect Frame Generation answer (the HUD-free scene at the half step), so every
// error measured is the paste's. Per pan speed and picture encoding (linear FP16, 8-bit SDR):
//   - label position error on the generated picture against the true half-step position (before: half a
//     real-frame step; after: under 0.25 px);
//   - the fixed panel byte-identical between the two pastes;
//   - the label over the moving unit stays one piece (its residual after the best shift).
// Pictures (before | after | truth, 3x) are written when a folder is given: fg_hud_move_test <folder>.
#include "test_support.hpp"
#include "host_dx12/fg/hud_move.hpp"
#include "host_dx12/fg/hud_paste.hpp"

#include <DirectXPackedVector.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace pv = DirectX::PackedVector;

namespace {
constexpr uint32_t W = 640, H = 360;
constexpr int kFrames = 7; // real frames 0..6; the last pair is measured
struct Label { float wx, wy; };
const Label kLabels[] = {{150, 60}, {330, 90}, {520, 140}, {260, 200}, {700, 240}, {420, 270}};
constexpr int kLabelOverUnit = 5; // its pixels lie over the unit's path at the end
constexpr float kLw = 52, kLh = 11;
struct Pic { std::vector<float> rgb; };

float terrain(float xw, float y)
{
    float v = .12f + .06f * std::sin(xw * .21f) * std::cos(y * .17f) + .04f * std::sin((xw + y) * .05f);
    if (std::fmod(std::fabs(xw * .7f + y * .3f), 40.f) < 1.6f) v += .10f; // rivers / borders
    return v;
}
// Scene at map offset o (content at screen x was at world x + o), unit at screen x ux.
void scene(std::vector<float>& rgb, float o, float ux)
{
    rgb.assign(size_t(W) * H * 3, 0.f);
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            float r = 0, g = 0, b = 0;
            for (int sy = 0; sy < 2; ++sy)
                for (int sx = 0; sx < 2; ++sx) {
                    const float px = x + .25f + .5f * sx, py = y + .25f + .5f * sy;
                    const float t = terrain(px + o, py);
                    float cr = t * .9f, cg = t * 1.1f, cb = t * .8f;
                    if (px >= ux && px < ux + 40 && py >= 255 && py < 285) { cr = .7f; cg = .08f; cb = .06f; } // the unit
                    r += cr; g += cg; b += cb;
                }
            float* p = &rgb[(size_t(y) * W + x) * 3];
            p[0] = r * .25f; p[1] = g * .25f; p[2] = b * .25f;
        }
}
// Glyph-like coverage of a label whose left edge is at screen x lx (anti-aliased, 4x4 samples).
float label_cover(float lx, float ly, float px, float py)
{
    float c = 0;
    for (int sy = 0; sy < 4; ++sy)
        for (int sx = 0; sx < 4; ++sx) {
            const float x = px + (sx + .5f) / 4 - lx, y = py + (sy + .5f) / 4 - ly;
            if (x < 0 || y < 0 || x >= kLw || y >= kLh) continue;
            const bool stroke = (int(x / 3) % 2 == 0 && y >= 1 && y < 10) || (y >= 4.5f && y < 6.f) || x < 1.2f;
            c += stroke ? 1.f : 0.f;
        }
    return c / 16;
}
// Interface over a picture: labels at map offset o (shift d added to label `only` when >= 0) and the fixed panel.
void interface_over(std::vector<float>& rgb, float o, int only = -1, float d = 0)
{
    for (int i = 0; i < int(std::size(kLabels)); ++i) {
        const float lx = kLabels[i].wx - o + (i == only ? d : 0), ly = kLabels[i].wy;
        const int x0 = std::max(0, int(std::floor(lx)) - 1), x1 = std::min(int(W), int(std::ceil(lx + kLw)) + 1);
        for (int y = int(ly) - 1; y < int(ly + kLh) + 2 && y < int(H); ++y)
            for (int x = x0; x < x1; ++x) {
                const float a = label_cover(lx, ly, float(x), float(y)) * .9f;
                if (a <= 0) continue;
                float* p = &rgb[(size_t(y) * W + x) * 3];
                for (int c = 0; c < 3; ++c) p[c] = p[c] * (1 - a) + .95f * a;
            }
    }
    for (uint32_t y = 300; y < 350; ++y)
        for (uint32_t x = 20; x < 200; ++x) {
            float* p = &rgb[(size_t(y) * W + x) * 3];
            const bool border = x < 22 || x >= 198 || y < 302 || y >= 348, text = y >= 320 && y < 328 && (x / 4) % 2 == 0;
            p[0] = border ? .8f : text ? .9f : .22f; p[1] = border ? .7f : text ? .9f : .22f; p[2] = border ? .2f : text ? .9f : .25f;
        }
}

struct Gpu {
    test::Device& d;
    DXGI_FORMAT fmt;
    bool sdr;
    std::vector<ComPtr<ID3D12Resource>> keep;
    ComPtr<ID3D12Resource> tex(bool uav) {
        return test::CreateTexture(d.device.Get(), W, H, fmt, D3D12_RESOURCE_STATE_COPY_DEST,
                                   uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
    }
    void upload(test::List& l, ID3D12Resource* t, const std::vector<float>& rgb, D3D12_RESOURCE_STATES after) {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 total = 0;
        const auto desc = t->GetDesc();
        d.device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
        auto up = test::CreateBuffer(d.device.Get(), total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        uint8_t* m = nullptr;
        up->Map(0, nullptr, reinterpret_cast<void**>(&m));
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                const float* s = &rgb[(size_t(y) * W + x) * 3];
                if (sdr) {
                    uint8_t* o = m + fp.Offset + size_t(y) * fp.Footprint.RowPitch + x * 4;
                    for (int c = 0; c < 3; ++c) o[c] = uint8_t(std::lround(std::clamp(std::pow(s[c], 1 / 2.2f), 0.f, 1.f) * 255));
                    o[3] = 255;
                } else {
                    pv::HALF* o = reinterpret_cast<pv::HALF*>(m + fp.Offset + size_t(y) * fp.Footprint.RowPitch) + x * 4;
                    for (int c = 0; c < 3; ++c) o[c] = pv::XMConvertFloatToHalf(s[c]);
                    o[3] = pv::XMConvertFloatToHalf(1.f);
                }
            }
        up->Unmap(0, nullptr);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = t; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = up.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
        l.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        const auto b = test::Transition(t, D3D12_RESOURCE_STATE_COPY_DEST, after);
        l.list->ResourceBarrier(1, &b);
        keep.push_back(up);
    }
    // Reads back as linear floats (SDR decoded with the same 2.2 rule it was encoded with).
    std::vector<float> read(test::List& l, ID3D12Resource* t, D3D12_RESOURCE_STATES state, std::vector<uint8_t>* raw = nullptr) {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 total = 0;
        const auto desc = t->GetDesc();
        d.device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
        auto rb = test::CreateBuffer(d.device.Get(), total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        auto b = test::Transition(t, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        l.list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = rb.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
        src.pResource = t; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        l.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        b = test::Transition(t, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
        l.list->ResourceBarrier(1, &b);
        l.list->Close();
        ID3D12CommandList* lists[] = {l.list.Get()};
        d.queue->ExecuteCommandLists(1, lists);
        d.WaitIdle();
        l.Reset();
        std::vector<float> out(size_t(W) * H * 3);
        uint8_t* m = nullptr;
        rb->Map(0, nullptr, reinterpret_cast<void**>(&m));
        const uint32_t bpp = sdr ? 4 : 8;
        if (raw) raw->resize(size_t(W) * H * bpp);
        for (uint32_t y = 0; y < H; ++y) {
            const uint8_t* row = m + fp.Offset + size_t(y) * fp.Footprint.RowPitch;
            if (raw) std::memcpy(raw->data() + size_t(y) * W * bpp, row, size_t(W) * bpp);
            for (uint32_t x = 0; x < W; ++x)
                for (int c = 0; c < 3; ++c)
                    out[(size_t(y) * W + x) * 3 + c] = sdr ? std::pow(row[x * 4 + c] / 255.f, 2.2f)
                                                          : pv::XMConvertHalfToFloat(reinterpret_cast<const pv::HALF*>(row)[x * 4 + c]);
        }
        rb->Unmap(0, nullptr);
        return out;
    }
};

// The shift of label i in `got` against the truth (labels at offset o), searched in 0.05 px steps.
float label_shift(const std::vector<float>& got, const std::vector<float>& base, float o, int i, float range, float* residual)
{
    const float lx = kLabels[i].wx - o, ly = kLabels[i].wy;
    const int x0 = std::max(0, int(lx - range - 3)), x1 = std::min(int(W), int(lx + kLw + range + 3));
    const int y0 = std::max(0, int(ly) - 2), y1 = std::min(int(H), int(ly + kLh) + 3);
    float best = 1e30f, best_d = 0, energy = 0;
    std::vector<float> t;
    for (float d = -range; d <= range + 1e-3f; d += .05f) {
        t = base;
        // Only label i at shift d over the HUD-free base (other labels outside this window are irrelevant).
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                const float a = label_cover(lx + d, ly, float(x), float(y)) * .9f;
                if (a <= 0) continue;
                float* p = &t[(size_t(y) * W + x) * 3];
                for (int c = 0; c < 3; ++c) p[c] = p[c] * (1 - a) + .95f * a;
            }
        float sad = 0;
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x)
                for (int c = 0; c < 3; ++c) {
                    const size_t k = (size_t(y) * W + x) * 3 + c;
                    sad += std::fabs(std::pow(std::max(got[k], 0.f), 1 / 2.2f) - std::pow(std::max(t[k], 0.f), 1 / 2.2f));
                }
        if (sad < best) { best = sad; best_d = d; }
    }
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) energy += label_cover(lx, ly, float(x), float(y)) * .9f * 3;
    if (residual) *residual = energy > 0 ? best / energy : 0;
    return best_d;
}

void write_bmp(const std::string& path, const std::vector<float>& rgb, int x0, int y0, int cw, int ch, int scale)
{
    const int ow = cw * scale, oh = ch * scale, pitch = (ow * 3 + 3) & ~3;
    std::vector<uint8_t> f(54 + size_t(pitch) * oh, 0);
    auto put32 = [&](size_t at, uint32_t v) { std::memcpy(&f[at], &v, 4); };
    f[0] = 'B'; f[1] = 'M'; put32(2, uint32_t(f.size())); put32(10, 54); put32(14, 40); put32(18, ow); put32(22, oh);
    f[26] = 1; f[28] = 24; put32(34, uint32_t(pitch) * oh);
    for (int y = 0; y < oh; ++y)
        for (int x = 0; x < ow; ++x) {
            const int sx = std::min(int(W) - 1, x0 + x / scale), sy = std::min(int(H) - 1, y0 + y / scale);
            const float* p = &rgb[(size_t(sy) * W + sx) * 3];
            uint8_t* o = &f[54 + size_t(oh - 1 - y) * pitch + x * 3];
            for (int c = 0; c < 3; ++c) o[2 - c] = uint8_t(std::lround(std::clamp(std::pow(p[c], 1 / 2.2f), 0.f, 1.f) * 255));
        }
    FILE* fp = nullptr;
    if (fopen_s(&fp, path.c_str(), "wb") == 0 && fp) { fwrite(f.data(), 1, f.size(), fp); fclose(fp); }
}

int run_case(test::Device& d, bool sdr, float speed, const char* out_dir)
{
    const float unit_speed = 5.f; // screen pixels a frame, against the pan (the pan moves the map left)
    Gpu g{d, sdr ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT, sdr, {}};
    test::List l;
    REQUIRE(l.Create(d.device.Get()));
    na::fg::HudMove move;
    na::fg::HudPaste t2;
    if (!move.Init(d.device.Get())) std::printf("hud_move: %s\n", move.Error());
    REQUIRE(move.Ready() && t2.Init(d.device.Get()));
    const uint32_t enc = sdr ? 0u : 2u;
    const float o0 = 0;
    auto offset = [&](float k) { return o0 + speed * k; };
    auto unit_x = [&](float k) { return 290.f - 60.f + unit_speed * k; };
    ComPtr<ID3D12Resource> real[2], hfree[2];
    for (int i = 0; i < 2; ++i) { real[i] = g.tex(false); hfree[i] = g.tex(false); REQUIRE(real[i] && hfree[i]); }
    auto motion = test::CreateTexture(d.device.Get(), W, H, DXGI_FORMAT_R32G32_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    auto gen_before = g.tex(true), gen_after = g.tex(true);
    REQUIRE(motion && gen_before && gen_after);
    std::vector<float> base, shown;
    const auto npsr = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    bool first = true;
    for (int k = 0; k < kFrames; ++k) {
        const int cur = k & 1, prev = cur ^ 1;
        if (!first) {
            for (ID3D12Resource* r : {real[cur].Get(), hfree[cur].Get(), motion.Get()}) {
                auto b = test::Transition(r, npsr, D3D12_RESOURCE_STATE_COPY_DEST);
                l.list->ResourceBarrier(1, &b);
            }
        }
        scene(base, offset(float(k)), unit_x(float(k)));
        shown = base;
        interface_over(shown, offset(float(k)));
        g.upload(l, hfree[cur].Get(), base, npsr);
        g.upload(l, real[cur].Get(), shown, npsr);
        {   // the scene's motion: the map came from x + speed; the unit from x - unit_speed
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
            UINT64 total = 0;
            const auto desc = motion->GetDesc();
            d.device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
            auto up = test::CreateBuffer(d.device.Get(), total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            uint8_t* m = nullptr;
            up->Map(0, nullptr, reinterpret_cast<void**>(&m));
            const float ux = unit_x(float(k));
            for (uint32_t y = 0; y < H; ++y) {
                float* row = reinterpret_cast<float*>(m + fp.Offset + size_t(y) * fp.Footprint.RowPitch);
                for (uint32_t x = 0; x < W; ++x) {
                    const bool unit = x + .5f >= ux && x + .5f < ux + 40 && y >= 255 && y < 285;
                    row[x * 2] = unit ? -unit_speed : speed;
                    row[x * 2 + 1] = 0;
                }
            }
            up->Unmap(0, nullptr);
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
            dst.pResource = motion.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.pResource = up.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
            l.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            auto b = test::Transition(motion.Get(), D3D12_RESOURCE_STATE_COPY_DEST, npsr);
            l.list->ResourceBarrier(1, &b);
            g.keep.push_back(up);
        }
        if (!first) REQUIRE(move.Classify(l.list.Get(), real[cur].Get(), hfree[cur].Get(), real[prev].Get(), hfree[prev].Get(), motion.Get(),
                                          1.f, 1.f, W, H, g.fmt, enc, uint64_t(k)));
        REQUIRE(SUCCEEDED(l.list->Close()));
        ID3D12CommandList* lists[] = {l.list.Get()};
        d.queue->ExecuteCommandLists(1, lists);
        d.WaitIdle();
        l.Reset();
        g.keep.clear();
        first = false;
    }
    // The generated picture between frames 5 and 6: Frame Generation's perfect answer (the HUD-free scene at the half step).
    const int last = kFrames - 1, cur = last & 1;
    const float o_half = offset(last - .5f);
    std::vector<float> gen;
    scene(gen, o_half, unit_x(last - .5f));
    std::vector<float> truth = gen;
    interface_over(truth, o_half);
    g.upload(l, gen_before.Get(), gen, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g.upload(l, gen_after.Get(), gen, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    REQUIRE(t2.Record(l.list.Get(), gen_before.Get(), real[cur].Get(), hfree[cur].Get(), W, H, g.fmt, 1, enc));
    REQUIRE(move.Paste(l.list.Get(), gen_after.Get(), real[cur].Get(), hfree[cur].Get(), motion.Get(), 1.f, 1.f, .5f, W, H, g.fmt, enc));
    REQUIRE(SUCCEEDED(l.list->Close()));
    {
        ID3D12CommandList* lists[] = {l.list.Get()};
        d.queue->ExecuteCommandLists(1, lists);
        d.WaitIdle();
        l.Reset();
        g.keep.clear();
    }
    std::vector<uint8_t> raw_before, raw_after;
    const auto before = g.read(l, gen_before.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &raw_before);
    const auto after = g.read(l, gen_after.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &raw_after);
    const float range = speed * .5f + 2.f;
    float worst_before = 0, worst_after = 0, worst_residual = 0, unit_err = 0, unit_residual = 0;
    int counted = 0;
    for (int i = 0; i < int(std::size(kLabels)); ++i) {
        const float lx = kLabels[i].wx - o_half;
        if (lx < 2 || lx + kLw > W - 2) continue; // only labels fully on screen at the half step are measured
        float rb = 0, ra = 0;
        const float eb = std::fabs(label_shift(before, gen, o_half, i, range, &rb));
        const float ea = std::fabs(label_shift(after, gen, o_half, i, range, &ra));
        char classes[64] = {};
        {   // the classes of the tiles the label covers in frame n
            const float nx = kLabels[i].wx - offset(float(last));
            int at = 0;
            for (int tx = int(std::max(0.f, nx)) / 16; tx <= int(std::min(float(W - 1), nx + kLw)) / 16 && at < 60; ++tx)
                for (int ty = int(kLabels[i].wy) / 16; ty <= int(kLabels[i].wy + kLh) / 16 && at < 60; ++ty)
                    classes[at++] = char('0' + std::max(0, move.TileClassForTests(uint32_t(tx), uint32_t(ty))));
        }
        std::printf("    label %d at x %.1f: error before %.2f px, after %.2f px (residual after %.3f) tiles %s%s\n", i, lx, eb, ea, ra, classes,
                    i == kLabelOverUnit ? "  [over the unit moving against the pan]" : "");
        if (i == kLabelOverUnit) { unit_err = ea; unit_residual = ra; }
        worst_before = std::max(worst_before, eb);
        worst_after = std::max(worst_after, ea);
        worst_residual = std::max(worst_residual, ra);
        ++counted;
    }
    // The fixed panel (and a one-pixel apron): byte-identical between the two pastes.
    const uint32_t bpp = sdr ? 4 : 8;
    size_t panel_diff = 0;
    for (uint32_t y = 299; y < 351; ++y)
        for (uint32_t x = 19; x < 201; ++x)
            if (std::memcmp(&raw_before[(size_t(y) * W + x) * bpp], &raw_after[(size_t(y) * W + x) * bpp], bpp)) ++panel_diff;
    const auto st = move.Read();
    std::printf("  %s pan %.1f px/frame: labels %d, worst position error before %.2f px, after %.2f px; label over the unit %.2f px "
                "(residual %.3f); fixed panel pixels differing from T2 %zu\n",
                sdr ? "SDR 8-bit" : "FP16", speed, counted, worst_before, worst_after, unit_err, unit_residual, panel_diff);
    (void)st;
    CHECK(counted >= 4);
    CHECK(worst_before >= speed * .5f - .3f); // T2 leaves them half a real-frame step away
    CHECK(worst_after < .25f);
    CHECK(panel_diff == 0);
    CHECK(unit_residual < .35f); // the label over the unit stays one piece
    if (out_dir && *out_dir) {
        char name[256];
        const std::vector<float>* pics[3] = {&before, &after, &truth};
        const char* whats[3] = {"before", "after", "truth"};
        for (int i = 0; i < 3; ++i) {
            std::snprintf(name, sizeof name, "%s\\names_%s_pan%02.0f_%s.bmp", out_dir, sdr ? "sdr" : "fp16", speed, whats[i]);
            write_bmp(name, *pics[i], 200, 50, 260, 240, 3);
        }
    }
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("fg_hud_move_test");
    const char* out = argc > 1 ? argv[1] : nullptr;
    for (const bool sdr : {false, true})
        for (const float speed : {2.f, 7.4f, 24.f})
            if (run_case(d, sdr, speed, out) != 0) return 1;
    return test::finish("fg_hud_move_test");
}
