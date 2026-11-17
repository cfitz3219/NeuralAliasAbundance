// Real GPU depth upsampling into a selected native array slice, preserving
// another slice and stencil. GPU readbacks/waits are confined to this utility.
#include "test_support.hpp"
#include "host_dx12/scale/depth_restore.hpp"
#include <cmath>
#include <vector>

namespace {
float sample(UINT x, UINT y) { return ((x + y) & 1) ? .8f : .2f; }
int run(test::Device& d, DXGI_FORMAT resource_format, DXGI_FORMAT dsv_format)
{
    auto* dev = d.device.Get();
    constexpr UINT iw = 8, ih = 4, ow = 32, oh = 16, selected = 2;
    const bool stencil = dsv_format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT || dsv_format == DXGI_FORMAT_D24_UNORM_S8_UINT;
    auto plane = test::CreateTexture(dev, iw, ih, DXGI_FORMAT_R32_FLOAT,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    REQUIRE(plane);
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = ow; td.Height = oh;
    td.DepthOrArraySize = 2; td.MipLevels = 2; td.Format = resource_format;
    td.SampleDesc = {1,0}; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    test::ComPtr<ID3D12Resource> target;
    REQUIRE(SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&target))));
    std::string error;
    auto pass = na::scale::DepthRestore::Create(dev, plane.Get(), target.Get(), selected, error);
    if (!pass) std::printf("depth format %u: %s\n", dsv_format, error.c_str());
    REQUIRE(pass);
    CHECK(!na::scale::DepthRestore::Create(dev, plane.Get(), target.Get(), 4, error)); // not depth plane zero
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    test::ComPtr<ID3D12DescriptorHeap> clear_heap;
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&clear_heap))));
    auto native0 = clear_heap->GetCPUDescriptorHandleForHeapStart(); auto native1 = native0;
    native1.ptr += dev->GetDescriptorHandleIncrementSize(hd.Type);
    D3D12_DEPTH_STENCIL_VIEW_DESC dv{}; dv.Format = dsv_format; dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
    dv.Texture2DArray.ArraySize = 1;
    dev->CreateDepthStencilView(target.Get(), &dv, native0);
    dv.Texture2DArray.FirstArraySlice = 1;
    dev->CreateDepthStencilView(target.Get(), &dv, native1);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT upload_fp{}, out_fp[3]{};
    UINT64 upload_bytes = 0, bytes[3]{};
    const auto sd = plane->GetDesc();
    dev->GetCopyableFootprints(&sd, 0, 1, 0, &upload_fp, nullptr, nullptr, &upload_bytes);
    auto upload = test::CreateBuffer(dev, upload_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    REQUIRE(upload);
    void* writable = nullptr; REQUIRE(SUCCEEDED(upload->Map(0, nullptr, &writable)));
    for (UINT y = 0; y < ih; ++y) for (UINT x = 0; x < iw; ++x) {
        const float z = sample(x,y);
        std::memcpy(static_cast<uint8_t*>(writable) + upload_fp.Offset + y * upload_fp.Footprint.RowPitch + x * 4, &z, 4);
    }
    upload->Unmap(0, nullptr);
    const UINT sub[3]{selected, 0, selected + 4};
    const UINT planes = stencil ? 3 : 2;
    std::vector<test::ComPtr<ID3D12Resource>> readback;
    UINT64 row_sizes[3]{};
    for (UINT i = 0; i < planes; ++i) {
        dev->GetCopyableFootprints(&td, sub[i], 1, 0, &out_fp[i], nullptr, &row_sizes[i], &bytes[i]);
        readback.push_back(test::CreateBuffer(dev, bytes[i], D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST));
        REQUIRE(readback.back());
    }
    test::List recording; REQUIRE(recording.Create(dev));
    auto* list = recording.list.Get();
    const auto clear_flags = static_cast<D3D12_CLEAR_FLAGS>(D3D12_CLEAR_FLAG_DEPTH | (stencil ? D3D12_CLEAR_FLAG_STENCIL : 0));
    list->ClearDepthStencilView(native0, clear_flags, .125f, 0x5a, 0, nullptr);
    list->ClearDepthStencilView(native1, clear_flags, .125f, 0x5a, 0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = upload_fp;
    dst.pResource = plane.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    auto input_barrier = test::Transition(plane.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(1, &input_barrier);
    if (stencil) {
        auto b = test::Transition(target.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ);
        b.Transition.Subresource = selected + 4; list->ResourceBarrier(1, &b);
    }
    REQUIRE(pass->Record(list));
    for (UINT i = 0; i < planes; ++i) {
        auto b = test::Transition(target.Get(), i == 2 ? D3D12_RESOURCE_STATE_DEPTH_READ : D3D12_RESOURCE_STATE_DEPTH_WRITE,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        b.Transition.Subresource = sub[i]; list->ResourceBarrier(1, &b);
        src = {}; dst = {};
        src.pResource = target.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = sub[i];
        dst.pResource = readback[i].Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = out_fp[i];
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    REQUIRE(SUCCEEDED(list->Close()));
    ID3D12CommandList* lists[]{list}; d.queue->ExecuteCommandLists(1, lists); d.WaitIdle();
    for (UINT i = 0; i < planes; ++i) {
        const void* mapped = nullptr; REQUIRE(SUCCEEDED(readback[i]->Map(0, nullptr, const_cast<void**>(&mapped))));
        const size_t stride = static_cast<size_t>(row_sizes[i] / ow);
        std::printf("depth format=%u plane=%u footprint=%u stride=%zu\n", dsv_format, i, out_fp[i].Footprint.Format, stride);
        for (UINT y = 0; y < oh; ++y) for (UINT x = 0; x < ow; ++x) {
            const auto* p = static_cast<const uint8_t*>(mapped) + out_fp[i].Offset + y * out_fp[i].Footprint.RowPitch + x * stride;
            if (i == 2) {
                const size_t offset = stride == 1 ? 0 : (dsv_format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ? 4 : 3);
                CHECK(p[offset] == 0x5a);
                continue;
            }
            const float expected = i == 1 ? .125f : sample(x * iw / ow, y * ih / oh);
            float actual = 0;
            if (dsv_format == DXGI_FORMAT_D16_UNORM) {
                uint16_t raw = 0; std::memcpy(&raw,p,2); actual = raw / 65535.f;
            } else if (dsv_format == DXGI_FORMAT_D24_UNORM_S8_UINT) {
                uint32_t raw = 0; std::memcpy(&raw,p,4); actual = (raw & 0xffffffu) / 16777215.f;
            } else std::memcpy(&actual,p,4);
            CHECK(std::fabs(actual - expected) < 2e-5f);
        }
        readback[i]->Unmap(0, nullptr);
    }
    CHECK(target->GetDesc().Width == ow && target->GetDesc().DepthOrArraySize == 2);
    return 0;
}
}
int main()
{
    test::Device d; if (!test::CreateDevice(d)) return test::SkipNoDevice("depth_restore_test");
    if (run(d, DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT) ||
        run(d, DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_D32_FLOAT_S8X24_UINT) ||
        run(d, DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT) ||
        run(d, DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_D16_UNORM)) return 1;
    return test::finish("depth_restore_test");
}
