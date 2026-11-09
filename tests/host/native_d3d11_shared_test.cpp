// Standalone hardware fixture; never launches or injects into a game.
// CPU completion waits/readback below belong only to this pixel assertion.
#include "host_dx12/native_d3d11/shared_resources.hpp"
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace na::native_d3d11;
#define REQUIRE(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* image,
                D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = image;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from; b.Transition.StateAfter = to;
    list->ResourceBarrier(1, &b);
}

int main()
{
    ComPtr<ID3D11Device> d11;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                  D3D11_SDK_VERSION, &d11, &level, &context);
    if (FAILED(hr)) { std::printf("SKIP native hardware DX11 unavailable: %08lx\n", hr); return 77; }
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    REQUIRE(SUCCEEDED(d11.As(&dxgi)));
    REQUIRE(SUCCEEDED(dxgi->GetAdapter(&adapter)));
    ComPtr<ID3D12Device> d12;
    REQUIRE(SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d12))));
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    REQUIRE(SUCCEEDED(d12->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))));
    SharedFences handoff;
    REQUIRE(SUCCEEDED(handoff.Create(d11.Get(), d12.Get(), queue.Get())));
    REQUIRE(FAILED(handoff.WaitNative(1)) && FAILED(handoff.WaitEffects(1)));
    for (auto format : {DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R32_FLOAT}) {
        SharedTexture image;
        hr = image.Create(d11.Get(), d12.Get(), {64, 19, format, true});
        if (FAILED(hr)) std::printf("campaign format %u: %08lx\n", static_cast<unsigned>(format), hr);
        REQUIRE(SUCCEEDED(hr));
    }

    // Two resource generations exercise exact-size recreation; each is retired
    // before releasing either API's reference. No resource created per frame.
    for (UINT width : {64u, 37u}) {
        const UINT height = 19;
        SharedTexture input, output;
        hr = input.Create(d11.Get(), d12.Get(), {width, height, DXGI_FORMAT_R8G8B8A8_UNORM});
        if (FAILED(hr)) std::printf("shared input creation: %08lx\n", hr);
        REQUIRE(SUCCEEDED(hr));
        REQUIRE(SUCCEEDED(output.Create(d11.Get(), d12.Get(), {width, height, DXGI_FORMAT_R8G8B8A8_UNORM, true})));
        REQUIRE(FAILED(input.Create(d11.Get(), d12.Get(), {width, height, DXGI_FORMAT_R8G8B8A8_UNORM})));
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> source, destination, staging;
        REQUIRE(SUCCEEDED(d11->CreateTexture2D(&desc, nullptr, &source)));
        REQUIRE(SUCCEEDED(d11->CreateTexture2D(&desc, nullptr, &destination)));
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        REQUIRE(SUCCEEDED(d11->CreateTexture2D(&desc, nullptr, &staging)));
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        REQUIRE(SUCCEEDED(d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))));
        REQUIRE(SUCCEEDED(d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))));
        REQUIRE(SUCCEEDED(list->Close()));
        std::vector<uint32_t> expected(width * height);
        for (UINT frame = 0; frame != 6; ++frame) {
            for (UINT i = 0; i != width * height; ++i) expected[i] = 0xff000000u | ((i * 173u + frame * 1973u) & 0x00ffffffu);
            context->UpdateSubresource(source.Get(), 0, nullptr, expected.data(), width * 4, 0);
            context->CopySubresourceRegion(input.Native(), 0, 0, 0, 0, source.Get(), 0, nullptr);
            uint64_t ready = 0, done = 0, retired = 0;
            REQUIRE(SUCCEEDED(handoff.SignalNative(&ready)));
            REQUIRE(SUCCEEDED(handoff.WaitEffects(ready)));
            REQUIRE(SUCCEEDED(allocator->Reset()));
            REQUIRE(SUCCEEDED(list->Reset(allocator.Get(), nullptr)));
            Transition(list.Get(), input.Effects(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transition(list.Get(), output.Effects(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyResource(output.Effects(), input.Effects());
            Transition(list.Get(), input.Effects(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            Transition(list.Get(), output.Effects(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            REQUIRE(SUCCEEDED(list->Close()));
            ID3D12CommandList* lists[] = {list.Get()};
            queue->ExecuteCommandLists(1, lists);
            REQUIRE(SUCCEEDED(handoff.SignalEffects(&done)));
            REQUIRE(SUCCEEDED(handoff.WaitNative(done)));
            context->CopySubresourceRegion(destination.Get(), 0, 0, 0, 0, output.Native(), 0, nullptr);
            context->CopyResource(staging.Get(), destination.Get());
            REQUIRE(SUCCEEDED(handoff.SignalNative(&retired)));
            // Only the fixture waits. Product slots poll and preserve references.
            const auto deadline = GetTickCount64() + 3000;
            while (!handoff.Retired(retired, done) && GetTickCount64() < deadline) Sleep(1);
            REQUIRE(handoff.Retired(retired, done));
            D3D11_MAPPED_SUBRESOURCE map{};
            REQUIRE(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map)));
            bool equal = true;
            for (UINT y = 0; y != height; ++y)
                equal &= std::memcmp(static_cast<const char*>(map.pData) + y * map.RowPitch,
                                     expected.data() + y * width, width * 4) == 0;
            context->Unmap(staging.Get(), 0);
            REQUIRE(equal);
        }
    }
    std::printf("PASS native DX11 -> shared DX12 -> native DX11 current pixels, 12 frames and resize\n");
    return 0;
}
