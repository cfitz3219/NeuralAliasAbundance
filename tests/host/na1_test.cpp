// The NA1 enhancer on a real Direct3D 12 device, with no game (docs/NA1-ENHANCER.md section 7).
// Not labelled gpu: it runs on the software device in CI, so pictures are kept small.
//
// What it proves:
//   1. switched off, Prepare returns nothing and allocates nothing;
//   2. the card's result equals the processor reference (core/na1_reference.hpp) pixel by pixel on
//      16-bit float pictures, forward and reversed depth, with and without a lighting layer, on an
//      even and an odd size; each working texture of the passes is compared too, so a failure names
//      the pass;
//   3. the Measure readback equals the reference's cells;
//   4. every dial at zero gives no picture to copy (and still measures); texture alone on a flat
//      picture leaves it as it was;
//   5. alpha comes back bit for bit;
//   6. the 8-bit sRGB path matches the reference within one step, and pixels nothing changes come
//      back bit for bit;
//   7. a format NA1 does not handle, and depth of another size, are refused with a reason;
//   8. the D3D12 debug layer, where it is installed, reports no error;
//   9. left to itself, the governor raises the texture dial from zero on a textured picture.
// GPU waits and readbacks are confined to this program; the product never waits.
#include "test_support.hpp"
#include "host_dx12/effects/na1.hpp"
#include "host_dx12/background/work.hpp"
#include "core/na1_reference.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include <d3d12sdklayers.h>

#if !defined(NA_HAVE_NA1)
int main()
{
    std::printf("NA1 enhancer: SKIP (the NA1 shaders were not included in this build)\n");
    return test::kSkip;
}
#else
namespace {
namespace ref = na::na1::ref;
constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// ---------------------------------------------------------------- 16-bit float helpers
// A value rounded to 11 significant bits: exactly what a 16-bit float holds, so the picture handed
// to the processor reference is bit for bit the picture the card loads.
float half_exact(float v)
{
    if (!(v > 0)) return 0.f;
    int e = 0;
    const float m = std::frexp(v, &e);
    return std::ldexp(std::round(m * 2048.f) / 2048.f, e);
}
// Only for values half_exact made (or zero): no rounding is needed.
uint16_t to_half(float v)
{
    uint32_t x = 0;
    std::memcpy(&x, &v, 4);
    if ((x & 0x7fffffffu) == 0) return 0;
    const uint32_t sign = (x >> 16) & 0x8000u;
    const uint32_t exponent = ((x >> 23) & 0xffu) - 112u;  // 127 - 15
    const uint32_t mantissa = (x >> 13) & 0x3ffu;
    return uint16_t(sign | (exponent << 10) | mantissa);
}
float from_half(uint16_t h)
{
    const uint32_t sign = uint32_t(h & 0x8000u) << 16, e = (h >> 10) & 0x1fu, m = h & 0x3ffu;
    if (e == 0) {
        const float v = std::ldexp(float(m), -24);
        return sign ? -v : v;
    }
    const uint32_t bits = e == 31 ? (sign | 0x7f800000u | (m << 13)) : (sign | ((e + 112u) << 23) | (m << 13));
    float f = 0;
    std::memcpy(&f, &bits, 4);
    return f;
}

// ---------------------------------------------------------------- upload and readback
struct Uploaded {
    test::ComPtr<ID3D12Resource> texture, staging;
};
// Records the upload of `data` (rows of w * bytes_per_pixel bytes) and leaves the texture readable.
bool upload(test::Device& d, ID3D12GraphicsCommandList* l, unsigned w, unsigned h, DXGI_FORMAT format, const void* data,
            unsigned bytes_per_pixel, Uploaded& out)
{
    out.texture = test::CreateTexture(d.device.Get(), w, h, format, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    if (!out.texture) return false;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes = 0;
    const D3D12_RESOURCE_DESC td = out.texture->GetDesc();
    d.device->GetCopyableFootprints(&td, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    out.staging = test::CreateBuffer(d.device.Get(), bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped = nullptr;
    if (!out.staging || FAILED(out.staging->Map(0, nullptr, &mapped))) return false;
    const size_t row = size_t(w) * bytes_per_pixel;
    for (unsigned y = 0; y < h; ++y)
        std::memcpy(static_cast<uint8_t*>(mapped) + size_t(y) * footprint.Footprint.RowPitch,
                    static_cast<const uint8_t*>(data) + size_t(y) * row, row);
    out.staging->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = out.staging.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint;
    dst.pResource = out.texture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const D3D12_RESOURCE_BARRIER b = test::Transition(out.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kRead);
    l->ResourceBarrier(1, &b);
    return true;
}
struct Readback {
    test::ComPtr<ID3D12Resource> buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    unsigned w = 0, h = 0;
};
// Records a copy of `image` (in the read state, and left in it) into a readback buffer.
Readback capture(test::Device& d, ID3D12GraphicsCommandList* l, ID3D12Resource* image)
{
    Readback out;
    const D3D12_RESOURCE_DESC desc = image->GetDesc();
    out.w = unsigned(desc.Width);
    out.h = desc.Height;
    UINT64 bytes = 0;
    d.device->GetCopyableFootprints(&desc, 0, 1, 0, &out.footprint, nullptr, nullptr, &bytes);
    out.buffer = test::CreateBuffer(d.device.Get(), bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!out.buffer) {
        CHECK(false);
        return out;
    }
    D3D12_RESOURCE_BARRIER b = test::Transition(image, kRead, D3D12_RESOURCE_STATE_COPY_SOURCE);
    l->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = image;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = out.buffer.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = out.footprint;
    l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    l->ResourceBarrier(1, &b);
    return out;
}
// The rows of a readback, packed. Call after the GPU has finished.
template <class T>
std::vector<T> values(const Readback& rb, unsigned per_pixel)
{
    std::vector<T> out(size_t(rb.w) * rb.h * per_pixel);
    void* mapped = nullptr;
    if (!rb.buffer || FAILED(rb.buffer->Map(0, nullptr, &mapped))) {
        CHECK(false);
        return out;
    }
    const size_t row = size_t(rb.w) * per_pixel * sizeof(T);
    for (unsigned y = 0; y < rb.h; ++y)
        std::memcpy(reinterpret_cast<uint8_t*>(out.data()) + size_t(y) * row,
                    static_cast<const uint8_t*>(mapped) + size_t(y) * rb.footprint.Footprint.RowPitch, row);
    rb.buffer->Unmap(0, nullptr);
    return out;
}
bool submit(test::Device& d, test::List& list)
{
    if (FAILED(list.list->Close())) return false;
    ID3D12CommandList* lists[] = {list.list.Get()};
    d.queue->ExecuteCommandLists(1, lists);
    return true;
}

// ---------------------------------------------------------------- the pictures
float noise(unsigned x, unsigned y, unsigned salt)
{
    uint32_t v = x * 374761393u + y * 668265263u + salt * 2246822519u;
    v = (v ^ (v >> 13)) * 1274126177u;
    v ^= v >> 16;
    return float(v & 0xffffu) / 65536.f;
}
// A textured scene with a near slab, a far wall, a strip of sky, a bright patch and a dark one.
// rgba holds values a 16-bit float keeps exactly; q is the depth key (0 = sky).
struct Scene {
    unsigned w = 0, h = 0;
    std::vector<float> rgba, depth, layer;
};
Scene make_scene(unsigned w, unsigned h, bool reversed)
{
    Scene s;
    s.w = w;
    s.h = h;
    s.rgba.resize(size_t(w) * h * 4);
    s.depth.resize(size_t(w) * h);
    s.layer.resize(size_t(w) * h);
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) {
            const size_t i = size_t(y) * w + x;
            const float fx = float(x) / float(w), fy = float(y) / float(h);
            float base = 0.06f + 1.2f * fx * (0.6f + 0.4f * std::sin(float(y) * 0.21f));
            if (fx > 0.70f && fx < 0.82f && fy > 0.55f && fy < 0.80f) base = 5.0f;   // bright patch
            if (fx > 0.10f && fx < 0.22f && fy > 0.60f && fy < 0.85f) base = 0.006f;  // dark patch
            const float grain = 0.7f + 0.6f * noise(x, y, 1);
            const float tint[3] = {0.9f + 0.3f * noise(x / 6, y / 6, 2), 1.0f, 0.7f + 0.5f * noise(x / 9, y / 5, 3)};
            for (unsigned c = 0; c < 3; ++c) s.rgba[i * 4 + c] = half_exact(base * grain * tint[c]);
            s.rgba[i * 4 + 3] = half_exact(0.05f + 0.9f * noise(x, y, 4));
            float q = fx < 0.34f ? 0.8f : 0.2f + 0.05f * fy;  // a near slab, then a far wall
            if (fy < 0.18f && fx > 0.25f) q = 0.f;            // sky
            s.depth[i] = reversed ? q : 1.f - q;
            s.layer[i] = -0.25f * noise(x / 4, y / 4, 5);     // a lighting layer that darkens, in stops
        }
    return s;
}
ref::Scene scene_values(int curve_mode)
{
    ref::Scene v;
    v.mid = -2.5f;
    v.m0 = 0.2f;
    v.w_dark = 0.3f;
    v.p85 = 0.3f;
    v.p97 = 0.7f;
    v.tint[0] = 1.1f;
    v.tint[1] = 0.95f;
    v.tint[2] = 0.8f;
    v.curve.mode = curve_mode;
    v.curve.x0 = -8.f;
    v.curve.dx = 0.7f;
    for (int k = 0; k < 16; ++k) v.curve.cy[k] = 0.9f * float(k * k) / 225.f;  // a table that rises faster and faster
    v.curve.mean_y = -2.f;
    return v;
}

uint64_t g_now = 100000;  // the clock the test drives; a step of 1000 makes every Prepare a sample tick

struct Result {
    bool prepared = false, recorded = false, has_color = false, measured = false;
    Readback color, stage[9];
};
// One frame through the pass: Prepare, Record, the readback copies, submit, report the submission,
// let the GPU finish, and wait for the measurement to be read.
Result one_frame(test::Device& d, test::List& list, na::na1::Pass& pass, na::na1::Inputs in, bool stages, uint64_t step_ms = 1000)
{
    Result r;
    na::na1::Options on;
    on.enabled = true;
    g_now += step_ms;
    in.now_ms = g_now;
    list.Reset();
    auto frame = pass.Prepare(d.device.Get(), d.queue.Get(), in, on);
    r.prepared = frame != nullptr;
    if (frame) {
        r.recorded = frame->Record(list.list.Get());
        r.measured = frame->Measures();
        if (frame->Color()) {
            r.has_color = true;
            r.color = capture(d, list.list.Get(), frame->Color());
            if (stages)
                for (unsigned i = 0; i < 9; ++i) r.stage[i] = capture(d, list.list.Get(), frame->StageForTests(i));
        }
    }
    if (!submit(d, list)) {
        CHECK(false);
        return r;
    }
    if (frame) frame->Submitted(d.queue.Get());
    d.WaitIdle();
    CHECK(pass.WaitMeasuredForTests(20000));
    return r;  // the Frame is released here, after the GPU has finished with it
}

bool near_value(float got, float want, float relative, float absolute)
{
    return std::fabs(got - want) <= relative * std::fabs(want) + absolute;
}
// Compares one working texture with the reference's; prints the worst place when it fails.
void compare_stage(const char* name, const Readback& rb, unsigned per_pixel, const std::vector<float>& want)
{
    const std::vector<float> got = values<float>(rb, per_pixel);
    if (got.size() != want.size()) {
        std::printf("FAIL stage %s: %zu values, the reference has %zu\n", name, got.size(), want.size());
        ++test::g_failures;
        return;
    }
    size_t bad = 0, worst = 0;
    float worst_gap = 0;
    for (size_t i = 0; i < got.size(); ++i) {
        const float gap = std::fabs(got[i] - want[i]);
        if (!near_value(got[i], want[i], 2e-4f, 2e-4f)) ++bad;
        if (!(gap <= worst_gap)) {
            worst_gap = gap;
            worst = i;
        }
    }
    if (bad) {
        std::printf("FAIL stage %s: %zu of %zu values differ; worst at value %zu: card %.7g, processor %.7g\n", name, bad, got.size(),
                    worst, double(got[worst]), double(want[worst]));
        ++test::g_failures;
    }
}

struct Uploads {
    Uploaded color, depth, layer;
};
// 2, 3, 5: card equals processor on a 16-bit float picture.
int card_equals_processor(test::Device& d, test::List& list, na::na1::Pass& pass, unsigned w, unsigned h, bool reversed,
                          bool with_layer, int curve_mode)
{
    std::printf("case %ux%u %s depth, %s lighting layer, curve mode %d\n", w, h, reversed ? "reversed" : "forward",
                with_layer ? "with a" : "no", curve_mode);
    const Scene s = make_scene(w, h, reversed);
    std::vector<uint16_t> halves(s.rgba.size());
    for (size_t i = 0; i < halves.size(); ++i) halves[i] = to_half(s.rgba[i]);
    Uploads u;
    list.Reset();
    REQUIRE(upload(d, list.list.Get(), w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, halves.data(), 8, u.color));
    REQUIRE(upload(d, list.list.Get(), w, h, DXGI_FORMAT_R32_FLOAT, s.depth.data(), 4, u.depth));
    REQUIRE(upload(d, list.list.Get(), w, h, DXGI_FORMAT_R32_FLOAT, s.layer.data(), 4, u.layer));
    REQUIRE(submit(d, list));
    d.WaitIdle();

    ref::Dials dials;
    dials.texture = 1.0f;
    dials.light = 0.6f;
    dials.colour = 0.5f;
    dials.key = 0.1f;
    dials.part[0] = 0.8f;
    dials.part[1] = 0.5f;  // no second layer is passed: this dial must do nothing
    const ref::Scene scene = scene_values(curve_mode);
    pass.PinForTests(dials, scene);

    na::na1::Inputs in;
    in.color = u.color.texture.Get();
    in.color_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    in.depth = u.depth.texture.Get();
    in.width = w;
    in.height = h;
    in.reversed = reversed;
    if (with_layer) {
        in.delta[0].layer = u.layer.texture.Get();
        in.delta[0].format = DXGI_FORMAT_R32_FLOAT;
        in.delta[0].name = "test layer";
    }
    const Result r = one_frame(d, list, pass, in, true);
    if (!r.prepared) std::printf("reason: %s\n", pass.GetStats().reason);
    REQUIRE(r.prepared && r.recorded && r.has_color && r.measured);

    // The passes one by one, so that a difference names its pass.
    ref::Pyramid p;
    ref::BuildPyramid(s.rgba.data(), s.depth.data(), w, h, reversed, p);
    compare_stage("yq (Prep)", r.stage[0], 2, p.yq);
    compare_stage("b1 after x (BlurX, level 1)", r.stage[1], 1, p.b1x);
    compare_stage("b1 (BlurY, level 1)", r.stage[2], 1, p.b1);
    compare_stage("yq2 (Down 1)", r.stage[3], 2, p.yq2);
    compare_stage("b2 after x (BlurX, level 2)", r.stage[4], 1, p.b2x);
    compare_stage("b2 (BlurY, level 2)", r.stage[5], 1, p.b2);
    compare_stage("yq4 (Down 2)", r.stage[6], 2, p.yq4);
    compare_stage("b3 after x (BlurX, level 3)", r.stage[7], 1, p.b3x);
    compare_stage("b3 (BlurY, level 3)", r.stage[8], 1, p.b3);

    // The result, pixel by pixel. The card stores 16-bit floats, so a step of that format is allowed for.
    std::vector<float> want(s.rgba.size());
    ref::Enhance(s.rgba.data(), s.depth.data(), w, h, reversed, dials, scene, with_layer ? s.layer.data() : nullptr, nullptr, want.data());
    const std::vector<uint16_t> got = values<uint16_t>(r.color, 4);
    REQUIRE(got.size() == halves.size());
    std::vector<float> after(got.size());
    size_t bad = 0, alpha_bad = 0, changed = 0;
    double worst = 0;
    for (size_t i = 0; i < got.size(); ++i) {
        after[i] = from_half(got[i]);
        if (i % 4 == 3) {
            if (got[i] != halves[i]) ++alpha_bad;  // 5: alpha bit for bit
            continue;
        }
        if (got[i] != halves[i]) ++changed;
        if (!near_value(after[i], want[i], 2e-3f, 1e-4f)) {
            if (!bad) std::printf("first difference at value %zu: card %.7g, processor %.7g\n", i, double(after[i]), double(want[i]));
            ++bad;
        }
        if (want[i] > 1e-3f) worst = (std::max)(worst, double(std::fabs(after[i] - want[i]) / want[i]));
    }
    std::printf("  result: %zu of %zu colour values differ from the processor, worst relative difference %.5f\n", bad, got.size() / 4 * 3, worst);
    CHECK(bad == 0);
    CHECK(alpha_bad == 0);
    CHECK(changed > got.size() / 4);  // the enhancer did something

    // 3: the measurement the Referee gets, against the reference fed the same two pictures.
    std::vector<float> cells, cells_want(size_t(ref::kCellsW) * ref::kCellsH * ref::kCellFloats);
    REQUIRE(pass.LastCellsForTests(cells));
    REQUIRE(cells.size() == cells_want.size());
    ref::Measure(s.rgba.data(), after.data(), s.depth.data(), w, h, reversed, scene, dials.key, cells_want.data());
    size_t cells_bad = 0;
    double count = 0, band = 0;
    for (size_t i = 0; i < cells.size(); ++i) {
        if (i % ref::kCellFloats == ref::kCount) count += double(cells[i]);
        if (i % ref::kCellFloats == ref::kBand) band += double(cells[i]);
        if (!near_value(cells[i], cells_want[i], 1e-3f, 1e-3f)) {
            if (!cells_bad)
                std::printf("first cell difference: cell %zu column %zu: card %.7g, processor %.7g\n", i / ref::kCellFloats,
                            i % ref::kCellFloats, double(cells[i]), double(cells_want[i]));
            ++cells_bad;
        }
    }
    std::printf("  measurement: %zu of %zu sums differ; %.0f samples, %.0f in the sky band\n", cells_bad, cells.size(), count, band);
    CHECK(cells_bad == 0);
    CHECK(count > 0);
    return 0;
}

// 4: zero is zero, and a flat picture stays as it was.
int zero_and_flat(test::Device& d, test::List& list, na::na1::Pass& pass)
{
    constexpr unsigned W = 96, H = 64;
    const Scene s = make_scene(W, H, false);
    std::vector<uint16_t> halves(s.rgba.size());
    for (size_t i = 0; i < halves.size(); ++i) halves[i] = to_half(s.rgba[i]);
    const float flat_value[4] = {0.25f, 0.5f, 0.125f, 0.75f};
    const uint8_t flat_byte[4] = {64, 128, 32, 200};
    std::vector<uint16_t> flat16(size_t(W) * H * 4);
    std::vector<uint8_t> flat8(size_t(W) * H * 4);
    for (size_t i = 0; i < flat16.size(); ++i) {
        flat16[i] = to_half(flat_value[i % 4]);
        flat8[i] = flat_byte[i % 4];
    }
    const std::vector<float> flat_depth(size_t(W) * H, 0.5f);
    Uploaded color, depth, color_flat16, color_flat8, depth_flat;
    list.Reset();
    REQUIRE(upload(d, list.list.Get(), W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, halves.data(), 8, color));
    REQUIRE(upload(d, list.list.Get(), W, H, DXGI_FORMAT_R32_FLOAT, s.depth.data(), 4, depth));
    REQUIRE(upload(d, list.list.Get(), W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, flat16.data(), 8, color_flat16));
    REQUIRE(upload(d, list.list.Get(), W, H, DXGI_FORMAT_R8G8B8A8_UNORM, flat8.data(), 4, color_flat8));
    REQUIRE(upload(d, list.list.Get(), W, H, DXGI_FORMAT_R32_FLOAT, flat_depth.data(), 4, depth_flat));
    REQUIRE(submit(d, list));
    d.WaitIdle();

    // Every dial at zero: a frame that only measures, with no picture to copy.
    const ref::Scene scene = scene_values(2);
    pass.PinForTests(ref::Dials{}, scene);
    na::na1::Inputs in;
    in.color = color.texture.Get();
    in.color_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    in.depth = depth.texture.Get();
    in.width = W;
    in.height = H;
    Result r = one_frame(d, list, pass, in, false);
    REQUIRE(r.prepared && r.recorded && r.measured);
    CHECK(!r.has_color);  // null means "unchanged, copy nothing"
    std::vector<float> cells;
    REQUIRE(pass.LastCellsForTests(cells));
    size_t differ = 0;
    double count = 0;
    for (size_t i = 0; i + ref::kCellFloats <= cells.size(); i += ref::kCellFloats) {
        count += double(cells[i + ref::kCount]);
        if (cells[i + ref::kYBefore] != cells[i + ref::kYAfter] || cells[i + ref::kShownBefore] != cells[i + ref::kShownAfter]) ++differ;
    }
    CHECK(count > 0);
    CHECK(differ == 0);  // before is after
    // Between sample ticks there is nothing to do at all, and the reason says the picture is unchanged.
    r = one_frame(d, list, pass, in, false, 10);
    CHECK(!r.prepared);
    CHECK(std::strstr(pass.GetStats().reason, "unchanged") != nullptr);

    // Texture alone on a flat picture. The blur's num/den is not exactly the flat value in floating
    // point, so the 16-bit float result may sit one step of that format away; the 8-bit result,
    // which rounds to the nearest step, must be the same bytes.
    ref::Dials texture_only;
    texture_only.texture = 1.0f;
    pass.PinForTests(texture_only, scene);
    in.color = color_flat16.texture.Get();
    in.depth = depth_flat.texture.Get();
    r = one_frame(d, list, pass, in, false);
    REQUIRE(r.prepared && r.recorded && r.has_color);
    const std::vector<uint16_t> got16 = values<uint16_t>(r.color, 4);
    REQUIRE(got16.size() == flat16.size());
    size_t exact = 0, off = 0;
    for (size_t i = 0; i < got16.size(); ++i) {
        const int step = int(got16[i]) - int(flat16[i]);
        if (step == 0) ++exact;
        else if (step < -1 || step > 1 || i % 4 == 3) ++off;
    }
    std::printf("flat 16-bit float picture, texture dial 1: %zu of %zu values bit-identical, %zu further than one step\n", exact, got16.size(), off);
    CHECK(off == 0);

    ref::Scene display = scene;
    display.curve.mode = 0;
    pass.PinForTests(texture_only, display);
    in.color = color_flat8.texture.Get();
    in.color_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    in.srgb = true;
    r = one_frame(d, list, pass, in, false);
    REQUIRE(r.prepared && r.recorded && r.has_color);
    const std::vector<uint8_t> got8 = values<uint8_t>(r.color, 4);
    REQUIRE(got8.size() == flat8.size());
    CHECK(std::memcmp(got8.data(), flat8.data(), flat8.size()) == 0);  // bit-identical
    return 0;
}

// 6: the 8-bit sRGB path. `m0` of 64 makes the near-black protection total, so every pixel NA1 would
// darken, and every pixel at full white, takes the "nothing changed" path of the exactness rule.
int eight_bit(test::Device& d, test::List& list, na::na1::Pass& pass, bool protect_everything)
{
    constexpr unsigned W = 96, H = 64;
    std::vector<uint8_t> bytes(size_t(W) * H * 4);
    std::vector<float> stored(bytes.size()), depth(size_t(W) * H);
    for (unsigned y = 0; y < H; ++y)
        for (unsigned x = 0; x < W; ++x) {
            const size_t i = (size_t(y) * W + x) * 4;
            const bool white = x >= 60 && x < 84 && y >= 8 && y < 28, black = x >= 8 && x < 24 && y >= 36 && y < 56;
            for (unsigned c = 0; c < 3; ++c) {
                unsigned v = 40u + unsigned(150.f * float(x) / float(W)) + unsigned(50.f * noise(x, y, 10 + c));
                if (white) v = 251u + unsigned(4.99f * noise(x, y, 20 + c));
                if (black) v = 0u;
                bytes[i + c] = uint8_t((std::min)(v, 255u));
            }
            bytes[i + 3] = uint8_t(255.f * noise(x, y, 30));
            depth[i / 4] = x < 40 ? 0.3f : 0.85f;
        }
    for (size_t i = 0; i < bytes.size(); ++i) stored[i] = float(bytes[i]) / 255.f;
    Uploaded color, depth_texture;
    list.Reset();
    REQUIRE(upload(d, list.list.Get(), W, H, DXGI_FORMAT_R8G8B8A8_UNORM, bytes.data(), 4, color));
    REQUIRE(upload(d, list.list.Get(), W, H, DXGI_FORMAT_R32_FLOAT, depth.data(), 4, depth_texture));
    REQUIRE(submit(d, list));
    d.WaitIdle();

    ref::Dials dials;
    dials.texture = 1.2f;
    dials.light = 0.5f;
    if (!protect_everything) {
        dials.colour = 0.4f;
        dials.key = 0.05f;
    }
    ref::Scene scene = scene_values(0);  // an 8-bit picture is display-referred
    scene.mid = -1.5f;
    if (protect_everything) scene.m0 = 64.f;
    pass.PinForTests(dials, scene);
    na::na1::Inputs in;
    in.color = color.texture.Get();
    in.color_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    in.srgb = true;
    in.depth = depth_texture.texture.Get();
    in.width = W;
    in.height = H;
    const Result r = one_frame(d, list, pass, in, false);
    if (!r.prepared) std::printf("reason: %s\n", pass.GetStats().reason);
    REQUIRE(r.prepared && r.recorded && r.has_color);
    const std::vector<uint8_t> got = values<uint8_t>(r.color, 4);
    REQUIRE(got.size() == bytes.size());
    std::vector<float> want(stored.size());
    ref::EnhanceSrgb8(stored.data(), depth.data(), W, H, false, dials, scene, nullptr, nullptr, want.data());
    size_t bad = 0, untouched = 0, untouched_bad = 0, changed = 0, alpha_bad = 0;
    for (size_t i = 0; i < got.size(); i += 4) {
        if (got[i + 3] != bytes[i + 3]) ++alpha_bad;
        const bool same = want[i] == stored[i] && want[i + 1] == stored[i + 1] && want[i + 2] == stored[i + 2];
        if (same) {
            ++untouched;
            if (got[i] != bytes[i] || got[i + 1] != bytes[i + 1] || got[i + 2] != bytes[i + 2]) ++untouched_bad;
        } else {
            ++changed;
        }
        for (unsigned c = 0; c < 3; ++c)
            if (std::fabs(float(got[i + c]) - want[i + c] * 255.f) > 1.0f) {
                if (!bad) std::printf("first difference at pixel %zu channel %u: card %u, processor %.3f\n", i / 4, c, unsigned(got[i + c]), double(want[i + c] * 255.f));
                ++bad;
            }
    }
    std::printf("8-bit sRGB (%s): %zu values further than one step, %zu pixels unchanged (%zu of them not bit-exact), %zu changed\n",
                protect_everything ? "protection total" : "ordinary scene", bad, untouched, untouched_bad, changed);
    CHECK(bad == 0);
    CHECK(alpha_bad == 0);
    CHECK(untouched_bad == 0);
    CHECK(changed > 100);
    if (protect_everything) CHECK(untouched > 100);
    return 0;
}

// 7: refusals say why and touch nothing.
int refusals(test::Device& d, na::na1::Pass& pass)
{
    constexpr unsigned W = 96, H = 64;
    const auto packed = test::CreateTexture(d.device.Get(), W, H, DXGI_FORMAT_R10G10B10A2_UNORM, kRead, D3D12_RESOURCE_FLAG_NONE);
    const auto bgra = test::CreateTexture(d.device.Get(), W, H, DXGI_FORMAT_B8G8R8A8_UNORM, kRead, D3D12_RESOURCE_FLAG_NONE);
    const auto good = test::CreateTexture(d.device.Get(), W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, kRead, D3D12_RESOURCE_FLAG_NONE);
    const auto depth = test::CreateTexture(d.device.Get(), W, H, DXGI_FORMAT_R32_FLOAT, kRead, D3D12_RESOURCE_FLAG_NONE);
    const auto small_depth = test::CreateTexture(d.device.Get(), W / 2, H / 2, DXGI_FORMAT_R32_FLOAT, kRead, D3D12_RESOURCE_FLAG_NONE);
    REQUIRE(packed && bgra && good && depth && small_depth);
    na::na1::Options on;
    on.enabled = true;
    const uint64_t before = pass.GetStats().allocations;
    na::na1::Inputs in;
    in.depth = depth.Get();
    in.width = W;
    in.height = H;
    in.now_ms = g_now += 1000;
    in.color = packed.Get();
    in.color_format = DXGI_FORMAT_R10G10B10A2_UNORM;
    CHECK(!pass.Prepare(d.device.Get(), d.queue.Get(), in, on));
    CHECK(std::strstr(pass.GetStats().reason, "colour format") != nullptr);
    in.color = bgra.Get();
    in.color_format = DXGI_FORMAT_B8G8R8A8_UNORM;
    CHECK(!pass.Prepare(d.device.Get(), d.queue.Get(), in, on));
    CHECK(std::strstr(pass.GetStats().reason, "colour format") != nullptr);
    // A picture described as one format but made as another would be a device error if it were viewed.
    in.color = bgra.Get();
    in.color_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    CHECK(!pass.Prepare(d.device.Get(), d.queue.Get(), in, on));
    CHECK(std::strstr(pass.GetStats().reason, "does not match") != nullptr);
    in.color = good.Get();
    in.color_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    in.depth = small_depth.Get();
    CHECK(!pass.Prepare(d.device.Get(), d.queue.Get(), in, on));
    CHECK(std::strstr(pass.GetStats().reason, "Depth") != nullptr);
    CHECK(pass.GetStats().allocations == before);
    return 0;
}

// 9: nothing pinned. The first measurement gives the governor its numbers and the texture dial
// starts to rise; on the software device NA1's own passes then cost more than the allowance and
// the Referee may suspend it, which is the rule working (the reason line is printed).
int governor_moves(test::Device& d, test::List& list)
{
    constexpr unsigned W = 192, H = 108;
    const Scene s = make_scene(W, H, true);
    std::vector<uint16_t> halves(s.rgba.size());
    for (size_t i = 0; i < halves.size(); ++i) halves[i] = to_half(s.rgba[i]);
    Uploaded color, depth;
    list.Reset();
    REQUIRE(upload(d, list.list.Get(), W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, halves.data(), 8, color));
    REQUIRE(upload(d, list.list.Get(), W, H, DXGI_FORMAT_R32_FLOAT, s.depth.data(), 4, depth));
    REQUIRE(submit(d, list));
    d.WaitIdle();

    na::na1::Pass pass;
    na::na1::Options on;
    on.enabled = true;
    na::na1::Inputs in;
    in.color = color.texture.Get();
    in.color_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    in.depth = depth.texture.Get();
    in.width = W;
    in.height = H;
    in.reversed = true;
    in.frame_ms = 10.f;
    in.now_ms = g_now += 1000;
    CHECK(!pass.Prepare(d.device.Get(), d.queue.Get(), in, on));  // the first call only starts the build
    REQUIRE(pass.WaitPrepared(60000));
    CHECK(pass.GetStats().texture == 0);

    float texture_most = 0;
    unsigned enhanced = 0, governor_lines = 0, curve_lines = 0;
    char line[1024];
    bool fitted_seen = false;
    for (unsigned i = 0; i < 24; ++i) {
        if (i == 12) {
            // The Referee's paired thumbnails: a screen that shows the output through a soft shoulder.
            std::vector<float> output(ref::kCellsW * ref::kCellsH), screen(output.size());
            for (size_t k = 0; k < output.size(); ++k) {
                const float y = -9.f + 9.f * float(k) / float(output.size());  // log2 of the output's luma
                const float linear = std::exp2(y) / (0.1f + std::exp2(y));
                output[k] = y;
                screen[k] = std::log2(std::pow(linear, 1.f / 2.2f) + 1e-4f);
            }
            pass.SetScreenPair(output.data(), screen.data(), uint32_t(output.size()), true);
        }
        const Result r = one_frame(d, list, pass, in, false, 250);
        if (r.has_color) ++enhanced;
        texture_most = (std::max)(texture_most, pass.GetStats().texture);
        if (pass.TakeGovernorLine(line, sizeof(line))) {
            if (governor_lines < 3) std::printf("na1_governor %s\n", line);
            ++governor_lines;
        }
        if (pass.TakeCurveLine(line, sizeof(line))) {
            std::printf("na1_curve %s\n", line);
            ++curve_lines;
            if (std::strstr(line, "mode=fitted")) fitted_seen = true;
        }
    }
    const na::na1::Stats stats = pass.GetStats();
    std::printf("governor: texture dial reached %.3f, %u enhanced frames, %llu samples, %.2f ms on the card; now: %s\n", double(texture_most),
                enhanced, static_cast<unsigned long long>(stats.samples), double(stats.gpu_ms), stats.reason);
    CHECK(texture_most > 0);
    CHECK(enhanced > 0);
    CHECK(stats.samples > 0);
    CHECK(governor_lines > 0);
    CHECK(curve_lines > 0);
    CHECK(fitted_seen);
    return 0;
}
}  // namespace

int main()
{
    // 8: the debug layer where it is installed (it is an optional Windows feature).
    test::ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("NA1 enhancer");
    test::ComPtr<ID3D12InfoQueue> info;
    d.device.As(&info);
    test::List list;
    REQUIRE(list.Create(d.device.Get()));
    REQUIRE(SUCCEEDED(list.list->Close()));

    na::na1::Pass pass;
    {
        // 1: switched off.
        na::na1::Inputs nothing;
        CHECK(!pass.Prepare(d.device.Get(), d.queue.Get(), nothing, na::na1::Options{}));
        const na::na1::Stats stats = pass.GetStats();
        CHECK(stats.allocations == 0 && stats.recorded == 0 && !stats.compiling);
        CHECK(std::strstr(stats.reason, "switched off") != nullptr);
    }
    {
        // The first switched-on call starts the background build and leaves the picture alone.
        const auto color = test::CreateTexture(d.device.Get(), 96, 64, DXGI_FORMAT_R16G16B16A16_FLOAT, kRead, D3D12_RESOURCE_FLAG_NONE);
        const auto depth = test::CreateTexture(d.device.Get(), 96, 64, DXGI_FORMAT_R32_FLOAT, kRead, D3D12_RESOURCE_FLAG_NONE);
        REQUIRE(color && depth);
        na::na1::Inputs in;
        in.color = color.Get();
        in.color_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        in.depth = depth.Get();
        in.width = 96;
        in.height = 64;
        in.now_ms = g_now += 1000;
        na::na1::Options on;
        on.enabled = true;
        CHECK(!pass.Prepare(d.device.Get(), d.queue.Get(), in, on));
        if (!pass.WaitPrepared(60000)) {
            std::printf("FATAL the NA1 programs were not prepared: %s\n", pass.GetStats().reason);
            return 1;
        }
        CHECK(pass.GetStats().allocations == 0);
    }
    REQUIRE(refusals(d, pass) == 0);
    REQUIRE(card_equals_processor(d, list, pass, 384, 216, false, true, 1) == 0);
    REQUIRE(card_equals_processor(d, list, pass, 101, 67, true, false, 2) == 0);
    REQUIRE(card_equals_processor(d, list, pass, 96, 64, true, true, 1) == 0);
    REQUIRE(card_equals_processor(d, list, pass, 96, 64, false, false, 2) == 0);
    REQUIRE(zero_and_flat(d, list, pass) == 0);
    REQUIRE(eight_bit(d, list, pass, true) == 0);
    REQUIRE(eight_bit(d, list, pass, false) == 0);
    REQUIRE(governor_moves(d, list) == 0);

    CHECK(na::background::Stop(5000));
    CHECK(d.device->GetDeviceRemovedReason() == S_OK);
    if (info) {
        unsigned errors = 0;
        for (UINT64 i = 0; i < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size = 0;
            info->GetMessage(i, nullptr, &size);
            std::vector<uint8_t> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (SUCCEEDED(info->GetMessage(i, message, &size)) &&
                (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR || message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)) {
                ++errors;
                std::printf("D3D12: %s\n", message->pDescription);
            }
        }
        CHECK(errors == 0);
    }
    return test::finish("NA1 enhancer");
}
#endif
