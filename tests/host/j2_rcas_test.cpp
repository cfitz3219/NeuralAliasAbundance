// J2 sharpening (RCAS, AMD FidelityFX FSR1 algorithm) on a real D3D12 device:
//   - 0 % records nothing (the DLAA result is left exactly as it is);
//   - 100 % raises contrast across a soft edge and in fine detail;
//   - no halos: every output pixel stays inside the min/max of its 5-tap cross;
//   - flat areas stay flat; HDR values above 1 survive (Reinhard round trip).
#include "test_support.hpp"
#include "host_dx12/j2/live_dlaa.hpp"

#include <cmath>
#include <cstring>
#include <vector>

namespace {
constexpr uint32_t W = 64, H = 16;
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
    uint32_t x = exp == 0 ? sign : (sign | ((exp - 15 + 127) << 23) | (mant << 13));
    float f;
    std::memcpy(&f, &x, 4);
    return f;
}
// Scene: flat 0.25 on the left, a soft (blurred, tanh-shaped) edge up to 0.75 around
// x = 22 -- what DLAA output looks like -- flat 0.75, alternating detail, an HDR 4.0 block.
float scene(uint32_t x, uint32_t y)
{
    if (x < 12) return 0.25f;
    if (x < 34) return 0.5f + 0.25f * static_cast<float>(std::tanh((static_cast<double>(x) - 21.5) / 2.0));
    if (x < 40) return 0.75f;
    if (x < 56) return ((x + y) & 1) ? 0.6f : 0.4f;
    return 4.0f;
}
} // namespace

int main()
{
    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("j2_rcas_test");
    ID3D12Device* dev = d.device.Get();
    const DXGI_FORMAT fmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
    auto in = test::CreateTexture(dev, W, H, fmt, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    auto out = test::CreateTexture(dev, W, H, fmt, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    REQUIRE(in && out);
    const D3D12_RESOURCE_DESC td = in->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
    auto up = test::CreateBuffer(dev, total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto rb = test::CreateBuffer(dev, total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    REQUIRE(up && rb);
    {
        uint8_t* p = nullptr;
        REQUIRE(SUCCEEDED(up->Map(0, nullptr, reinterpret_cast<void**>(&p))));
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                const uint16_t v = half(scene(x, y)), one = half(1.0f);
                const uint16_t px[4] = {v, v, v, one};
                std::memcpy(p + fp.Offset + y * fp.Footprint.RowPitch + x * 8, px, 8);
            }
        up->Unmap(0, nullptr);
    }
    // 0 %: nothing is recorded.
    {
        test::List l;
        REQUIRE(l.Create(dev));
        CHECK(!na::j2::RecordRcasForTests(dev, l.list.Get(), in.Get(), out.Get(), W, H, 0, fmt));
        l.list->Close();
    }
    test::List l;
    REQUIRE(l.Create(dev));
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = up.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = fp;
    dst.pResource = in.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    l.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER b = test::Transition(in.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    l.list->ResourceBarrier(1, &b);
    REQUIRE(na::j2::RecordRcasForTests(dev, l.list.Get(), in.Get(), out.Get(), W, H, 100, fmt));
    b = test::Transition(out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    l.list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION s2{}, d2{};
    s2.pResource = out.Get();
    s2.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    d2.pResource = rb.Get();
    d2.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    d2.PlacedFootprint = fp;
    l.list->CopyTextureRegion(&d2, 0, 0, 0, &s2, nullptr);
    REQUIRE(SUCCEEDED(l.list->Close()));
    ID3D12CommandList* lists[] = {l.list.Get()};
    d.queue->ExecuteCommandLists(1, lists);
    d.WaitIdle();
    REQUIRE(dev->GetDeviceRemovedReason() == S_OK);
    std::vector<float> o(W * H);
    {
        uint8_t* p = nullptr;
        REQUIRE(SUCCEEDED(rb->Map(0, nullptr, reinterpret_cast<void**>(&p))));
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                uint16_t v;
                std::memcpy(&v, p + fp.Offset + y * fp.Footprint.RowPitch + x * 8, 2);
                o[y * W + x] = single(v);
            }
        rb->Unmap(0, nullptr);
    }
    const auto at = [&](int x, int y) { x = x < 0 ? 0 : (x >= int(W) ? W - 1 : x); y = y < 0 ? 0 : (y >= int(H) ? H - 1 : y); return scene(x, y); };
    uint32_t halos = 0;
    for (uint32_t y = 1; y + 1 < H; ++y)
        for (uint32_t x = 1; x + 1 < W; ++x) {
            const float v[5] = {at(x, y), at(x - 1, y), at(x + 1, y), at(x, y - 1), at(x, y + 1)};
            float mn = v[0], mx = v[0];
            for (float q : v) { mn = q < mn ? q : mn; mx = q > mx ? q : mx; }
            const float r = o[y * W + x];
            if (r < mn * 0.995f - 1e-3f || r > mx * 1.005f + 1e-3f) ++halos;
        }
    const int y = 8;
    const float flat = o[y * W + 5], e18 = o[y * W + 18], e25 = o[y * W + 25];
    const float detail_a = o[y * W + 44], detail_b = o[y * W + 45], hdr = o[y * W + 60];
    std::printf("RCAS 100%%: flat %.4f (0.25), soft edge x18 %.4f (in %.4f) x25 %.4f (in %.4f), detail %.4f/%.4f, hdr %.3f, halos %u\n", flat, e18,
                scene(18, y), e25, scene(25, y), detail_a, detail_b, hdr, halos);
    CHECK(halos == 0);                                      // stays within each pixel's neighbourhood: no halos
    CHECK(std::fabs(flat - 0.25f) < 2e-3f);                 // flat stays flat
    CHECK(e18 < scene(18, y) - 0.003f && e25 > scene(25, y) + 0.003f); // the soft edge gets steeper (crisper)
    CHECK(std::fabs(detail_a - detail_b) >= std::fabs(scene(44, y) - scene(45, y)) - 0.01f); // detail never loses contrast
    CHECK(std::fabs(hdr - 4.0f) < 0.05f);                   // HDR above 1 survives
    return test::finish("j2_rcas_test");
}
