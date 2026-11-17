#include "test_support.hpp"
#include "host_dx12/native_d3d11/shared_resources.hpp"
using namespace na::native_d3d11;

int main()
{
    return test::run({
        {"selected mip and array slice have exact extent", [] {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = 1920; d.Height = 1080; d.MipLevels = 3; d.ArraySize = 2;
            d.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; d.SampleDesc.Count = 1;
            CHECK(CompatibleImageCopy(d, 4, {960, 540, DXGI_FORMAT_R8G8B8A8_UNORM}));
            CHECK(!CompatibleImageCopy(d, 3, {960, 540, DXGI_FORMAT_R8G8B8A8_UNORM}));
            CHECK(!CompatibleImageCopy(d, 6, {1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM}));
            CHECK(!CompatibleImageCopy(d, 0, {1920, 1080, DXGI_FORMAT_B8G8R8A8_UNORM}));
        }},
        {"depth copies preserve bits and reject fake conversion", [] {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = 1280; d.Height = 720; d.MipLevels = d.ArraySize = 1;
            d.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; d.SampleDesc.Count = 1;
            CHECK(CompatibleImageCopy(d, 0, {1280, 720, DXGI_FORMAT_R24G8_TYPELESS}));
            CHECK(!CompatibleImageCopy(d, 0, {1280, 720, DXGI_FORMAT_R32_FLOAT}));
            d.Format = DXGI_FORMAT_D32_FLOAT;
            CHECK(CompatibleImageCopy(d, 0, {1280, 720, DXGI_FORMAT_R32_TYPELESS}));
            d.SampleDesc.Count = 2;
            CHECK(!CompatibleImageCopy(d, 0, {1280, 720, DXGI_FORMAT_R32_TYPELESS}));
        }},
        {"retirement needs both producers and rejects removal sentinel", [] {
            CHECK(!Completed(8, 5, 9, 5));
            CHECK(!Completed(9, 4, 9, 5));
            CHECK(Completed(9, 5, 9, 5));
            CHECK(!Completed(UINT64_MAX, 5, 9, 5));
            CHECK(!Completed(9, UINT64_MAX, 9, 5));
        }},
        {"uninitialized handoff never queues work or claims retirement", [] {
            SharedFences f;
            uint64_t value = 99;
            CHECK(FAILED(f.SignalNative(&value))); CHECK(value == 0);
            value = 99;
            CHECK(FAILED(f.SignalEffects(&value))); CHECK(value == 0);
            CHECK(FAILED(f.WaitEffects(1))); CHECK(FAILED(f.WaitNative(1)));
            CHECK(!f.Retired(0, 0));
            SharedTexture t;
            CHECK(FAILED(t.Create(nullptr, nullptr, {128, 128, DXGI_FORMAT_R8G8B8A8_UNORM})));
            CHECK(!t.Native() && !t.Effects());
        }}
    });
}
