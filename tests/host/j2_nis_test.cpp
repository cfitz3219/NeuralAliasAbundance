// J2 Sharpness 101..200 %: NVIDIA Image Scaling sharpen-only stacked on RCAS, on a
// real D3D12 device (WARP on CI). Over the whole slider range:
//   - every percent 0..200 gives finite values (no NaN/Inf, nothing negative);
//   - <= 100 % records no NIS pass (RCAS alone, unchanged behaviour);
//   - 101 % is continuous with 100 % (NIS fades in from nothing);
//   - strength rises with the slider: the soft edge gets steeper 125 -> 150 -> 200;
//   - flat areas stay flat; HDR values above 1 survive (linear-HDR mode);
//   - the SDR (UNORM) variant compiles and runs too.
#include "test_support.hpp"
#include "host_dx12/j2/live_dlaa.hpp"
#include "host_dx12/j2/nis_sharpen.hpp"

#include <cmath>
#include <cstring>
#include <vector>

namespace {
constexpr uint32_t W = 96, H = 48;
uint16_t half(float f)
{
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    int exp = static_cast<int>((x >> 23) & 0xFF) - 127 + 15;
    const uint32_t mant = x & 0x7FFFFFu;
    if (exp <= 0) return static_cast<uint16_t>(sign);
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
    return static_cast<uint16_t>(sign | (exp << 10) | (mant >> 13));
}
float single(uint16_t h)
{
    const uint32_t sign = (h & 0x8000u) << 16, exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
    if (exp == 31) return mant ? NAN : (sign ? -INFINITY : INFINITY);
    uint32_t x = exp == 0 ? sign : (sign | ((exp - 15 + 127) << 23) | (mant << 13));
    float f;
    std::memcpy(&f, &x, 4);
    return f;
}
// Flat 0.25, a soft tanh edge up to 0.75 around x = 30, flat 0.75, checker detail, HDR 4.0.
float scene(uint32_t x, uint32_t y)
{
    if (x < 16) return 0.25f;
    if (x < 46) return 0.5f + 0.25f * static_cast<float>(std::tanh((static_cast<double>(x) - 29.5) / 2.5));
    if (x < 54) return 0.75f;
    if (x < 80) return ((x + y) & 1) ? 0.6f : 0.4f;
    return 4.0f;
}

struct Rig {
    test::Device& d;
    ID3D12Device* dev;
    DXGI_FORMAT fmt;
    uint32_t px; // bytes per pixel
    test::ComPtr<ID3D12Resource> in, out, up, rb;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    bool make()
    {
        in = test::CreateTexture(dev, W, H, fmt, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
        out = test::CreateTexture(dev, W, H, fmt, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (!in || !out) return false;
        const D3D12_RESOURCE_DESC td = in->GetDesc();
        UINT64 total = 0;
        dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
        up = test::CreateBuffer(dev, total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        rb = test::CreateBuffer(dev, total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!up || !rb) return false;
        uint8_t* p = nullptr;
        if (FAILED(up->Map(0, nullptr, reinterpret_cast<void**>(&p)))) return false;
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                uint8_t* q = p + fp.Offset + y * fp.Footprint.RowPitch + x * px;
                const float s = scene(x, y);
                if (px == 8) {
                    const uint16_t v = half(s), one = half(1.0f);
                    const uint16_t c[4] = {v, v, v, one};
                    std::memcpy(q, c, 8);
                } else {
                    const uint8_t v = static_cast<uint8_t>(std::lround((s > 1.f ? 1.f : s) * 255.f));
                    const uint8_t c[4] = {v, v, v, 255};
                    std::memcpy(q, c, 4);
                }
            }
        up->Unmap(0, nullptr);
        return true;
    }
    // Runs NIS at `percent`; returns false when nothing was recorded (o = input copy then).
    bool run(uint32_t percent, bool hdr, std::vector<float>* o)
    {
        test::List l;
        if (!l.Create(dev)) return false;
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = up.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = fp;
        dst.pResource = in.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        l.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER b = test::Transition(in.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        l.list->ResourceBarrier(1, &b);
        const bool recorded = na::j2::RecordNisForTests(dev, l.list.Get(), in.Get(), out.Get(), W, H, percent, fmt, hdr);
        ID3D12Resource* read = recorded ? out.Get() : in.Get();
        const D3D12_RESOURCE_STATES from = recorded ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b = test::Transition(read, from, D3D12_RESOURCE_STATE_COPY_SOURCE);
        l.list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION s2{}, d2{};
        s2.pResource = read;
        s2.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        d2.pResource = rb.Get();
        d2.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        d2.PlacedFootprint = fp;
        l.list->CopyTextureRegion(&d2, 0, 0, 0, &s2, nullptr);
        b = test::Transition(read, D3D12_RESOURCE_STATE_COPY_SOURCE, from);
        l.list->ResourceBarrier(1, &b);
        b = test::Transition(in.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        l.list->ResourceBarrier(1, &b); // ready for the next upload
        if (FAILED(l.list->Close())) return false;
        ID3D12CommandList* lists[] = {l.list.Get()};
        d.queue->ExecuteCommandLists(1, lists);
        d.WaitIdle();
        o->assign(W * H, 0.f);
        uint8_t* p = nullptr;
        if (FAILED(rb->Map(0, nullptr, reinterpret_cast<void**>(&p)))) return recorded;
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                const uint8_t* q = p + fp.Offset + y * fp.Footprint.RowPitch + x * px;
                if (px == 8) {
                    uint16_t v;
                    std::memcpy(&v, q, 2);
                    (*o)[y * W + x] = single(v);
                } else {
                    (*o)[y * W + x] = q[0] / 255.f;
                }
            }
        rb->Unmap(0, nullptr);
        return recorded;
    }
};

bool finite_nonneg(const std::vector<float>& o)
{
    for (float v : o) if (!std::isfinite(v) || v < -1e-4f) return false;
    return true;
}
} // namespace

int main()
{
    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("j2_nis_test");
    ID3D12Device* dev = d.device.Get();

    // Amount mapping: nothing at or below 100 %, continuous fade-in, full at 200 %.
    {
        float s = -1.f, m = -1.f;
        na::j2::NisSharpen::Amount(100, &s, &m);
        CHECK(s == 0.f && m == 0.f);
        na::j2::NisSharpen::Amount(101, &s, &m);
        CHECK(s > 0.f && s < 0.02f && m > 0.f && m < 0.05f);
        na::j2::NisSharpen::Amount(200, &s, &m);
        CHECK(s == 1.f && m == 1.f);
        na::j2::NisSharpen::Amount(250, &s, &m);
        CHECK(s == 1.f && m == 1.f);
    }

    Rig hdr{d, dev, DXGI_FORMAT_R16G16B16A16_FLOAT, 8};
    REQUIRE(hdr.make());
    std::vector<float> o;
    // <= 100 %: no NIS pass at all.
    for (uint32_t pct : {0u, 50u, 100u}) CHECK(!hdr.run(pct, true, &o));

    // Every slider step above 100 % (strided) produces sane values.
    uint32_t bad = 0;
    for (uint32_t pct = 101; pct <= 200; pct += 3) {
        REQUIRE(hdr.run(pct, true, &o));
        if (!finite_nonneg(o)) { ++bad; std::printf("non-finite or negative output at %u%%\n", pct); }
    }
    REQUIRE(hdr.run(200, true, &o));
    CHECK(finite_nonneg(o));
    CHECK(bad == 0);
    REQUIRE(dev->GetDeviceRemovedReason() == S_OK);

    const int y = H / 2;
    auto edge_step = [&](const std::vector<float>& v) { return v[y * W + 31] - v[y * W + 28]; }; // across the soft edge
    std::vector<float> o101, o125, o150, o200;
    REQUIRE(hdr.run(101, true, &o101));
    REQUIRE(hdr.run(125, true, &o125));
    REQUIRE(hdr.run(150, true, &o150));
    REQUIRE(hdr.run(200, true, &o200));
    float jump = 0.f; // 100 % (input) -> 101 % continuity
    for (uint32_t x = 0; x < 80; ++x) jump = std::fmax(jump, std::fabs(o101[y * W + x] - scene(x, y)));
    const float in_step = scene(31, y) - scene(28, y);
    std::printf("NIS soft edge step: in %.4f, 125%% %.4f, 150%% %.4f, 200%% %.4f; 101%% max change %.4f; "
                "flat %.4f; detail %.4f/%.4f; hdr %.3f\n",
                in_step, edge_step(o125), edge_step(o150), edge_step(o200), jump, o200[y * W + 8], o200[y * W + 64],
                o200[y * W + 65], o200[y * W + 90]);
    CHECK(jump < 0.02f);                                   // 101 % is a tiny step from 100 %
    CHECK(edge_step(o150) >= edge_step(o125) - 1e-4f);     // strength rises with the slider
    CHECK(edge_step(o200) > edge_step(o150) + 1e-4f);
    CHECK(edge_step(o200) > in_step + 0.005f);             // 200 % visibly steepens the edge
    CHECK(std::fabs(o200[y * W + 8] - 0.25f) < 3e-3f);     // flat stays flat
    CHECK(std::fabs(o200[y * W + 90] - 4.0f) < 0.2f);      // HDR above 1 survives

    // SDR variant (UNORM colour): compiles, runs, stays in range.
    Rig sdr{d, dev, DXGI_FORMAT_R8G8B8A8_UNORM, 4};
    REQUIRE(sdr.make());
    REQUIRE(sdr.run(200, false, &o));
    CHECK(finite_nonneg(o));
    CHECK(std::fabs(o[y * W + 8] - 0.25f) < 2.f / 255.f);
    REQUIRE(dev->GetDeviceRemovedReason() == S_OK);
    return test::finish("j2_nis_test");
}
