// Scene depth with the raster jitter taken back out, on the card (scale/depth_unjitter.cpp), against
// its processor reference (core/depth_unjitter.hpp) pixel by pixel, in every depth format a game
// uses. What the correction is for, and that it ends the symptom, is in
// tests/cpu/depth_unjitter_test.cpp; this test is only "card = processor".
//
// The source depth is filled through the real depth-stencil path and read back, so the reference
// runs on exactly the numbers the card's program read (24- and 16-bit steps included).
// GPU readbacks and waits are confined to this utility.
#include "test_support.hpp"
#include "host_dx12/scale/depth_restore.hpp"
#include "host_dx12/scale/depth_unjitter.hpp"
#include "core/depth_unjitter.hpp"
#include <cmath>
#include <cstring>
#include <vector>

namespace {
constexpr UINT kW = 96, kH = 64;

// A picture with what matters: ground sloping down the screen, sky, a wall facing the camera, a
// surface sloping on both axes, things one and two pixels wide, a fold, and steps of every size.
float picture(UINT x, UINT y)
{
    float d = y < 20 ? 0.0f : 0.004f + 0.00021f * float(y - 20);                             // sky, then ground
    if (x >= 12 && x < 30 && y >= 8 && y < 34) d = 0.0125f;                                    // wall
    if (x >= 40 && x < 70 && y >= 26 && y < 60) d = 0.03f + 0.0004f * float(x) - 0.00017f * float(y); // tilted surface
    if (x == 80 && y >= 4 && y < 50) d = 0.2f;                                                 // a post one pixel wide
    if (x >= 86 && x < 88 && y >= 10 && y < 56) d = 0.3f + 0.001f * float(y);                  // two pixels wide
    if (y >= 60) d = 0.004f + 0.00021f * 40.0f + 0.0009f * float(y - 60);                      // a fold in the ground
    if (x >= 2 && x < 8 && y >= 40 && y < 46) d = 0.9f + 0.01f * float((x * 7 + y * 3) % 5);   // rough
    return d;
}
struct Plane {
    test::ComPtr<ID3D12Resource> buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 row_bytes = 0;
    bool Prepare(ID3D12Device* dev, const D3D12_RESOURCE_DESC& desc)
    {
        UINT64 bytes = 0;
        dev->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, &row_bytes, &bytes);
        buffer = test::CreateBuffer(dev, bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        return !!buffer;
    }
    void Record(ID3D12GraphicsCommandList* list, ID3D12Resource* texture, D3D12_RESOURCE_STATES state)
    {
        auto b = test::Transition(texture, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        b.Transition.Subresource = 0; list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION from{}, to{};
        from.pResource = texture; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; from.SubresourceIndex = 0;
        to.pResource = buffer.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = fp;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter); list->ResourceBarrier(1, &b);
    }
    bool Read(DXGI_FORMAT dsv, std::vector<float>& out)
    {
        const void* mapped = nullptr;
        if (FAILED(buffer->Map(0, nullptr, const_cast<void**>(&mapped)))) return false;
        const size_t stride = static_cast<size_t>(row_bytes / kW);
        out.resize(size_t(kW) * kH);
        for (UINT y = 0; y < kH; ++y)
            for (UINT x = 0; x < kW; ++x) {
                const auto* p = static_cast<const uint8_t*>(mapped) + fp.Offset + size_t(y) * fp.Footprint.RowPitch + x * stride;
                float v = 0;
                if (dsv == DXGI_FORMAT_D16_UNORM) { uint16_t raw = 0; std::memcpy(&raw, p, 2); v = raw / 65535.f; }
                else if (dsv == DXGI_FORMAT_D24_UNORM_S8_UINT) { uint32_t raw = 0; std::memcpy(&raw, p, 4); v = (raw & 0xffffffu) / 16777215.f; }
                else std::memcpy(&v, p, 4);
                out[size_t(y) * kW + x] = v;
            }
        buffer->Unmap(0, nullptr);
        return true;
    }
};
int run(test::Device& d, DXGI_FORMAT resource_format, DXGI_FORMAT dsv_format)
{
    auto* dev = d.device.Get();
    const bool stencil = dsv_format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT || dsv_format == DXGI_FORMAT_D24_UNORM_S8_UINT;
    const float steps = dsv_format == DXGI_FORMAT_D16_UNORM ? 65535.f : dsv_format == DXGI_FORMAT_D24_UNORM_S8_UINT ? 16777215.f : 0.f;
    auto plane = test::CreateTexture(dev, kW, kH, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    REQUIRE(plane);
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = kW; td.Height = kH;
    td.DepthOrArraySize = 1; td.MipLevels = 1; td.Format = resource_format;
    td.SampleDesc = {1, 0}; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    test::ComPtr<ID3D12Resource> depth;
    REQUIRE(SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&depth))));
    std::string error;
    auto fill = na::scale::DepthRestore::Create(dev, plane.Get(), depth.Get(), 0, error);
    if (!fill) std::printf("depth format %u: fill: %s\n", dsv_format, error.c_str());
    REQUIRE(fill);
    auto pass = na::scale::DepthUnjitter::Create(dev, depth.Get(), error);
    if (!pass) std::printf("depth format %u: %s\n", dsv_format, error.c_str());
    REQUIRE(pass);
    const auto twin_desc = pass->Twin()->GetDesc();
    CHECK(twin_desc.Format == td.Format && twin_desc.Width == td.Width && twin_desc.Height == td.Height && twin_desc.Flags == td.Flags);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT upload_fp{};
    UINT64 upload_bytes = 0;
    const auto sd = plane->GetDesc();
    dev->GetCopyableFootprints(&sd, 0, 1, 0, &upload_fp, nullptr, nullptr, &upload_bytes);
    auto upload = test::CreateBuffer(dev, upload_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    REQUIRE(upload);
    void* writable = nullptr; REQUIRE(SUCCEEDED(upload->Map(0, nullptr, &writable)));
    for (UINT y = 0; y < kH; ++y)
        for (UINT x = 0; x < kW; ++x) {
            const float z = picture(x, y);
            std::memcpy(static_cast<uint8_t*>(writable) + upload_fp.Offset + size_t(y) * upload_fp.Footprint.RowPitch + x * 4, &z, 4);
        }
    upload->Unmap(0, nullptr);
    Plane source, result;
    REQUIRE(source.Prepare(dev, td) && result.Prepare(dev, td));

    const D3D12_RESOURCE_STATES readable = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    test::List recording; REQUIRE(recording.Create(dev));
    auto* list = recording.list.Get();
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = upload_fp;
    dst.pResource = plane.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    auto input = test::Transition(plane.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(1, &input);
    if (stencil) { // the fill's view keeps stencil read-only
        auto b = test::Transition(depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ);
        b.Transition.Subresource = 1; list->ResourceBarrier(1, &b);
    }
    REQUIRE(fill->Record(list));
    {   // the state a game leaves its depth in for its lighting: tested against and read at once
        auto b = test::Transition(depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, readable);
        b.Transition.Subresource = 0; list->ResourceBarrier(1, &b);
    }
    source.Record(list, depth.Get(), readable);
    REQUIRE(SUCCEEDED(list->Close()));
    ID3D12CommandList* lists[]{list}; d.queue->ExecuteCommandLists(1, lists); d.WaitIdle();
    std::vector<float> in, out;
    REQUIRE(source.Read(dsv_format, in));

    const float jitters[][2] = {{0.3125f, -0.4375f}, {-0.46875f, 0.28125f}, {0.0f, 0.40625f}, {-0.21875f, 0.0f}, {0.0f, 0.0f}};
    for (const auto& j : jitters) {
        recording.Reset();
        REQUIRE(pass->Record(list, j[0], j[1]));
        result.Record(list, pass->Twin(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
        REQUIRE(SUCCEEDED(list->Close()));
        d.queue->ExecuteCommandLists(1, lists); d.WaitIdle();
        REQUIRE(result.Read(dsv_format, out));
        unsigned wrong = 0, changed = 0; double largest = 0;
        for (UINT y = 0; y < kH; ++y)
            for (UINT x = 0; x < kW; ++x) {
                const size_t i = size_t(y) * kW + x;
                const float expected = na::depth::unjitter::Pixel(in.data(), kW, kH, int(x), int(y), j[0], j[1]);
                // Whole steps in a 24- or 16-bit buffer (one step of rounding allowed); a few float steps otherwise.
                const double allowed = steps ? 1.01 / steps : 4e-7 * std::fabs(expected) + 1e-30;
                const double off = std::fabs(double(out[i]) - expected);
                largest = (std::max)(largest, steps ? off * steps : off / (std::max)(1e-30, double(std::fabs(expected))));
                if (off > allowed) {
                    if (wrong++ < 6) std::printf("  x %u y %u: card %.9g processor %.9g depth %.9g\n", x, y, out[i], expected, in[i]);
                }
                if (out[i] != in[i]) ++changed;
            }
        std::printf("depth format %u jitter %+.5f %+.5f: %u of %u pixels differ from the processor, largest difference %.3g %s, %u pixels corrected\n",
            dsv_format, j[0], j[1], wrong, kW * kH, largest, steps ? "steps" : "of the value", changed);
        CHECK(wrong == 0);
        if (j[0] == 0.0f && j[1] == 0.0f) CHECK(changed == 0); // no nudge, no change
        else CHECK(changed > kW * kH / 10);
    }
    return 0;
}
int refusals(test::Device& d)
{
    auto* dev = d.device.Get();
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = kW; td.Height = kH;
    td.DepthOrArraySize = 1; td.MipLevels = 2; td.Format = DXGI_FORMAT_R32_TYPELESS;
    td.SampleDesc = {1, 0}; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    test::ComPtr<ID3D12Resource> mips, colour;
    REQUIRE(SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&mips))));
    colour = test::CreateTexture(dev, kW, kH, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE);
    REQUIRE(colour);
    std::string error;
    CHECK(!na::scale::DepthUnjitter::Create(dev, mips.Get(), error) && !error.empty());
    CHECK(!na::scale::DepthUnjitter::Create(dev, colour.Get(), error) && !error.empty());
    CHECK(!na::scale::DepthUnjitter::Create(dev, nullptr, error) && !error.empty());
    return 0;
}
}
int main()
{
    test::Device d; if (!test::CreateDevice(d)) return test::SkipNoDevice("depth_unjitter_test");
    if (run(d, DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT) ||
        run(d, DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_D32_FLOAT_S8X24_UINT) ||
        run(d, DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT) ||
        run(d, DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_D16_UNORM) || refusals(d)) return 1;
    return test::finish("depth_unjitter_test");
}
