// Live Frame Generation, T2 HUD: the GPU paste (fg/hud_paste.cpp) gives exactly the CPU reference
// (core/live_frame_gen.hpp PasteHud) on an HDR (RGBA16F) picture with a HUD, a sub-threshold
// difference, bright HDR values that clip in display terms, and edges of the picture.
#include "test_support.hpp"
#include "host_dx12/fg/hud_paste.hpp"
#include "core/live_frame_gen.hpp"

#include <DirectXPackedVector.h>
#include <cmath>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace pv = DirectX::PackedVector;

namespace {
constexpr uint32_t W = 67, H = 41; // not multiples of the 16x16 groups
float hf(pv::HALF v) { return pv::XMConvertHalfToFloat(v); }
pv::HALF fh(float v) { return pv::XMConvertFloatToHalf(v); }
// Upload rgba rows (half) into `tex` (COPY_DEST) and leave it in `after`.
void upload(test::Device& d, test::List& l, ID3D12Resource* tex, const std::vector<pv::HALF>& rgba, D3D12_RESOURCE_STATES after,
            std::vector<ComPtr<ID3D12Resource>>& keep)
{
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    const auto desc = tex->GetDesc();
    d.device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
    auto up = test::CreateBuffer(d.device.Get(), total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t* m = nullptr;
    up->Map(0, nullptr, reinterpret_cast<void**>(&m));
    for (uint32_t y = 0; y < H; ++y) std::memcpy(m + fp.Offset + size_t(y) * fp.Footprint.RowPitch, rgba.data() + size_t(y) * W * 4, W * 8);
    up->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = tex; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = up.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
    l.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const auto b = test::Transition(tex, D3D12_RESOURCE_STATE_COPY_DEST, after);
    l.list->ResourceBarrier(1, &b);
    keep.push_back(up);
}
} // namespace

int main()
{
    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("fg_hud_paste_test");
    // Pictures: a mid-grey scene; the generated picture is a different grey. The real frame has a
    // HUD block, one lone HUD pixel at the corner, a tiny (sub-threshold) difference, and an HDR
    // highlight brighter in the real frame than in its HUD-free picture (both above 1.0: no HUD).
    const size_t n = size_t(W) * H;
    std::vector<float> real(n * 3, 0.18f), hudfree(n * 3, 0.18f), gen(n * 3, 0.35f);
    auto set = [](std::vector<float>& v, uint32_t x, uint32_t y, float r, float g, float b) {
        const size_t i = (size_t(y) * W + x) * 3; v[i] = r; v[i + 1] = g; v[i + 2] = b; };
    for (uint32_t y = 10; y < 14; ++y) for (uint32_t x = 20; x < 30; ++x) set(real, x, y, 1.0f, 0.8f, 0.1f);
    set(real, 0, 0, 0.9f, 0.9f, 0.9f);
    set(real, 50, 30, 0.18f + 0.0005f, 0.18f, 0.18f);
    set(real, 40, 5, 6.f, 6.f, 6.f);
    set(hudfree, 40, 5, 3.f, 3.f, 3.f);
    // Exactly what the GPU will store (half precision), for the reference.
    auto to_half = [&](const std::vector<float>& v) {
        std::vector<pv::HALF> out(n * 4);
        for (size_t i = 0; i < n; ++i) { for (int c = 0; c < 3; ++c) out[i * 4 + c] = fh(v[i * 3 + c]); out[i * 4 + 3] = fh(1.f); }
        return out;
    };
    auto from_half = [&](const std::vector<pv::HALF>& h) {
        std::vector<float> out(n * 3);
        for (size_t i = 0; i < n; ++i) for (int c = 0; c < 3; ++c) out[i * 3 + c] = hf(h[i * 4 + c]);
        return out;
    };
    const auto real_h = to_half(real), free_h = to_half(hudfree), gen_h = to_half(gen);
    std::vector<float> expect = from_half(gen_h);
    {
        const auto r = from_half(real_h), f = from_half(free_h);
        na::fglive::PasteHud(r.data(), f.data(), expect.data(), W, H);
    }
    const DXGI_FORMAT fmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
    auto t_real = test::CreateTexture(d.device.Get(), W, H, fmt, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    auto t_free = test::CreateTexture(d.device.Get(), W, H, fmt, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    auto t_gen = test::CreateTexture(d.device.Get(), W, H, fmt, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    REQUIRE(t_real && t_free && t_gen);
    test::List l;
    REQUIRE(l.Create(d.device.Get()));
    std::vector<ComPtr<ID3D12Resource>> keep;
    upload(d, l, t_real.Get(), real_h, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, keep);
    upload(d, l, t_free.Get(), free_h, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, keep);
    upload(d, l, t_gen.Get(), gen_h, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, keep);
    na::fg::HudPaste paste;
    REQUIRE(paste.Init(d.device.Get()));
    REQUIRE(paste.Record(l.list.Get(), t_gen.Get(), t_real.Get(), t_free.Get(), W, H, fmt, 3));
    // Read the result back.
    const auto b = test::Transition(t_gen.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    l.list->ResourceBarrier(1, &b);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    const auto desc = t_gen->GetDesc();
    d.device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
    auto rb = test::CreateBuffer(d.device.Get(), total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = rb.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
    src.pResource = t_gen.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    l.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    REQUIRE(SUCCEEDED(l.list->Close()));
    ID3D12CommandList* lists[] = {l.list.Get()};
    d.queue->ExecuteCommandLists(1, lists);
    d.WaitIdle();
    std::vector<pv::HALF> got(n * 4);
    uint8_t* m = nullptr;
    REQUIRE(SUCCEEDED(rb->Map(0, nullptr, reinterpret_cast<void**>(&m))));
    for (uint32_t y = 0; y < H; ++y) std::memcpy(got.data() + size_t(y) * W * 4, m + fp.Offset + size_t(y) * fp.Footprint.RowPitch, W * 8);
    rb->Unmap(0, nullptr);
    const auto out = from_half(got);
    uint32_t mismatches = 0, pasted = 0;
    for (size_t i = 0; i < n * 3; ++i) {
        if (out[i] != expect[i]) ++mismatches;
    }
    for (size_t i = 0; i < n; ++i) if (out[i * 3 + 2] != hf(gen_h[i * 4 + 2])) ++pasted;
    std::printf("pixels %zu, pasted %u, mismatches against the CPU reference %u\n", n, pasted, mismatches);
    CHECK(mismatches == 0);
    // The HUD block grown by one pixel (12 x 6) plus the corner pixel grown (2 x 2) = 72 + 4.
    CHECK(pasted == 76);
    CHECK(out[(size_t(30) * W + 50) * 3] == hf(gen_h[(size_t(30) * W + 50) * 4])); // sub-threshold: not HUD
    CHECK(out[(size_t(5) * W + 40) * 3] == hf(gen_h[(size_t(5) * W + 40) * 4]));   // HDR highlight above 1.0: not HUD
    return test::finish("fg_hud_paste_test");
}
