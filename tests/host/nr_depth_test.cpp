// Neural Rendering input preparation (Argon audit #2 / C12, #4.1/#4.2):
//   - the pass's device objects build, which compiles every NR shader
//     (including the new depth conversion and the subrect-sized upsamplers);
//   - 24-bit (R24G8_TYPELESS) and 16-bit (R16_TYPELESS) depth convert to R32F
//     through a typed view, and only the requested region (base + size) is
//     read: the region was cleared to a known value, the rest to another.
#include "test_support.hpp"
#include "host_dx12/nr/nr_pass.hpp"

#include <cmath>
#include <cstring>

namespace {

int ConvertCase(test::Device& d, DXGI_FORMAT resource, DXGI_FORMAT dsv, DXGI_FORMAT view, const char* name)
{
    constexpr uint32_t W = 16, H = 12, BX = 3, BY = 2, RW = 8, RH = 6;
    auto depth = test::CreateTexture(d.device.Get(), W, H, resource, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    auto out = test::CreateTexture(d.device.Get(), RW, RH, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    REQUIRE(depth && out);
    test::ComPtr<ID3D12DescriptorHeap> dh;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    REQUIRE(SUCCEEDED(d.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dh))));
    D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
    dv.Format = dsv;
    dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    const D3D12_CPU_DESCRIPTOR_HANDLE h = dh->GetCPUDescriptorHandleForHeapStart();
    d.device->CreateDepthStencilView(depth.Get(), &dv, h);
    // Readback of `out` (R32F, row pitch aligned to 256).
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    const D3D12_RESOURCE_DESC od = out->GetDesc();
    d.device->GetCopyableFootprints(&od, 0, 1, 0, &fp, nullptr, nullptr, &total);
    auto rb = test::CreateBuffer(d.device.Get(), total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    REQUIRE(rb);

    test::List l;
    REQUIRE(l.Create(d.device.Get()));
    const D3D12_RECT region{static_cast<LONG>(BX), static_cast<LONG>(BY), static_cast<LONG>(BX + RW), static_cast<LONG>(BY + RH)};
    l.list->ClearDepthStencilView(h, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    l.list->ClearDepthStencilView(h, D3D12_CLEAR_FLAG_DEPTH, 0.25f, 0, 1, &region);
    D3D12_RESOURCE_BARRIER to_srv = test::Transition(depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    l.list->ResourceBarrier(1, &to_srv);
    REQUIRE(na::nr::RecordDepthToR32ForTests(l.list.Get(), depth.Get(), view, out.Get(), BX, BY, RW, RH));
    D3D12_RESOURCE_BARRIER to_copy = test::Transition(out.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    l.list->ResourceBarrier(1, &to_copy);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = out.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = rb.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    l.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    REQUIRE(SUCCEEDED(l.list->Close()));
    ID3D12CommandList* lists[] = {l.list.Get()};
    d.queue->ExecuteCommandLists(1, lists);
    d.WaitIdle();
    REQUIRE(d.device->GetDeviceRemovedReason() == S_OK);

    void* p = nullptr;
    REQUIRE(SUCCEEDED(rb->Map(0, nullptr, &p)));
    float lo = 1e9f, hi = -1e9f;
    for (uint32_t y = 0; y < RH; ++y)
        for (uint32_t x = 0; x < RW; ++x) {
            float v;
            std::memcpy(&v, static_cast<const unsigned char*>(p) + y * fp.Footprint.RowPitch + x * 4, 4);
            lo = v < lo ? v : lo;
            hi = v > hi ? v : hi;
        }
    rb->Unmap(0, nullptr);
    std::printf("%s depth -> R32F: region %ux%u at (%u,%u) min %.6f max %.6f (expected 0.25)\n", name, RW, RH, BX, BY, lo, hi);
    CHECK(std::fabs(lo - 0.25f) < 1e-3f && std::fabs(hi - 0.25f) < 1e-3f); // every pixel from the region, none from outside
    return 0;
}

} // namespace

int main()
{
    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("nr_depth_test");
    if (int r = ConvertCase(d, DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_R24_UNORM_X8_TYPELESS, "24-bit")) return r;
    if (int r = ConvertCase(d, DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_R16_UNORM, "16-bit")) return r;
    na::nr::Shutdown();
    return test::finish("nr_depth_test");
}
