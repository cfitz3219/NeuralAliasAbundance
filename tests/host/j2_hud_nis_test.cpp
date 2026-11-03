// Render-small HUD separation: the HUD passes compile, and the NIS scaler upscales a
// picture on a real D3D12 device (WARP on CI): flat input stays flat, a hard edge stays
// a clean step at the right place, every value stays in 0..1.
#include "test_support.hpp"
#include "host_dx12/j2/live_dlaa.hpp"
#include "host_dx12/j2/nis_scaler.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

int main()
{
    char why[1024] = {};
    const bool hud_ok = na::j2::CompileHudShadersForTests(why, sizeof why);
    if (!hud_ok) std::printf("HUD shaders: %s\n", why);
    CHECK(hud_ok);

    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("j2_hud_nis_test");
    ID3D12Device* dev = d.device.Get();
    constexpr uint32_t IW = 64, IH = 32, OW = 96, OH = 48;
    constexpr DXGI_FORMAT F = DXGI_FORMAT_R8G8B8A8_UNORM;
    auto in = test::CreateTexture(dev, IW, IH, F, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    auto out = test::CreateTexture(dev, OW, OH, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    REQUIRE(in && out);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fi{}, fo{};
    UINT64 ti = 0, to = 0;
    const D3D12_RESOURCE_DESC di = in->GetDesc(), dout = out->GetDesc();
    dev->GetCopyableFootprints(&di, 0, 1, 0, &fi, nullptr, nullptr, &ti);
    dev->GetCopyableFootprints(&dout, 0, 1, 0, &fo, nullptr, nullptr, &to);
    auto up = test::CreateBuffer(dev, ti, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto rb = test::CreateBuffer(dev, to, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    REQUIRE(up && rb);
    uint8_t* p = nullptr;
    REQUIRE(SUCCEEDED(up->Map(0, nullptr, reinterpret_cast<void**>(&p))));
    for (uint32_t y = 0; y < IH; ++y)
        for (uint32_t x = 0; x < IW; ++x) {
            const uint8_t v = x < IW / 2 ? 64 : 192; // flat halves, one hard vertical edge
            const uint8_t c[4] = {v, v, v, 255};
            std::memcpy(p + fi.Offset + y * fi.Footprint.RowPitch + x * 4, c, 4);
        }
    up->Unmap(0, nullptr);

    na::j2::NisScaler nis;
    REQUIRE(nis.WaitCompiled());
    test::List l;
    REQUIRE(l.Create(dev));
    D3D12_TEXTURE_COPY_LOCATION src{up.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}}, dst{in.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    src.PlacedFootprint = fi;
    l.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER b = test::Transition(in.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    l.list->ResourceBarrier(1, &b);
    const bool ran = nis.Dispatch(dev, l.list.Get(), in.Get(), F, IW, IH, out.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, OW, OH, false, 0.5f, 0);
    if (!ran) std::printf("NIS scaler: %s\n", nis.Error());
    REQUIRE(ran);
    b = test::Transition(out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    l.list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION s2{out.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}}, d2{rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
    d2.PlacedFootprint = fo;
    l.list->CopyTextureRegion(&d2, 0, 0, 0, &s2, nullptr);
    REQUIRE(SUCCEEDED(l.list->Close()));
    ID3D12CommandList* lists[] = {l.list.Get()};
    d.queue->ExecuteCommandLists(1, lists);
    d.WaitIdle();
    CHECK(dev->GetDeviceRemovedReason() == S_OK);

    auto single = [](uint16_t h) {
        const uint32_t sign = (h & 0x8000u) << 16, e = (h >> 10) & 0x1F, m = h & 0x3FF;
        uint32_t x = e == 0 ? sign : (sign | ((e - 15 + 127) << 23) | (m << 13));
        float f;
        std::memcpy(&f, &x, 4);
        return f;
    };
    REQUIRE(SUCCEEDED(rb->Map(0, nullptr, reinterpret_cast<void**>(&p))));
    uint32_t bad = 0;
    float left = 0.f, right = 0.f;
    for (uint32_t y = 0; y < OH; ++y)
        for (uint32_t x = 0; x < OW; ++x) {
            uint16_t v;
            std::memcpy(&v, p + fo.Offset + y * fo.Footprint.RowPitch + x * 8, 2);
            const float f = single(v);
            if (!(f >= 0.f && f <= 1.f)) ++bad;
            if (y == OH / 2 && x == OW / 4) left = f;
            if (y == OH / 2 && x == 3 * OW / 4) right = f;
        }
    rb->Unmap(0, nullptr);
    std::printf("NIS scaler 64x32 -> 96x48: left %.3f right %.3f, out of range %u\n", left, right, bad);
    CHECK(bad == 0);
    CHECK(std::fabs(left - 64.f / 255.f) < 0.02f);
    CHECK(std::fabs(right - 192.f / 255.f) < 0.02f);
    nis.Reset();
    return test::finish("j2_hud_nis_test");
}
