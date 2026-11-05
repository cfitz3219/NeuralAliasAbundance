// D3D12 Memory Allocator wrapper (gpu/mem_alloc.hpp): a placed render target is usable after
// its first Clear, the allocation lives exactly as long as the resource, and a second
// resource shares the allocator. Hardware or WARP; SKIP (77) without either.
#include "test_support.hpp"
#include "host_dx12/gpu/mem_alloc.hpp"

#include <cstdio>

int main()
{
    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("mem_alloc_test");
    namespace m = na::gpu::mem;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = 64; rd.Height = 32; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    test::ComPtr<ID3D12Resource> a, b;
    REQUIRE(SUCCEEDED(m::CreateResource(d.device.Get(), D3D12_HEAP_TYPE_DEFAULT, rd, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, a.GetAddressOf())) && a);
    REQUIRE(SUCCEEDED(m::CreateResource(d.device.Get(), D3D12_HEAP_TYPE_DEFAULT, rd, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, b.GetAddressOf())) && b);
    m::Stats s = m::GetStats();
    std::printf("%s\n", m::StatusLine().c_str());
    CHECK(s.placed + s.committed_fallbacks == 2);
    CHECK(s.live == s.placed);
    // First use: a full clear (the rule for placed render targets), then a copy out.
    test::ComPtr<ID3D12DescriptorHeap> h;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    REQUIRE(SUCCEEDED(d.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&h))));
    D3D12_RENDER_TARGET_VIEW_DESC v{};
    v.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    d.device->CreateRenderTargetView(a.Get(), &v, h->GetCPUDescriptorHandleForHeapStart());
    test::List l;
    REQUIRE(l.Create(d.device.Get()));
    const float c[4] = {0.25f, 0.5f, 0.75f, 1.f};
    l.list->ClearRenderTargetView(h->GetCPUDescriptorHandleForHeapStart(), c, 0, nullptr);
    REQUIRE(SUCCEEDED(l.list->Close()));
    ID3D12CommandList* lists[] = {l.list.Get()};
    d.queue->ExecuteCommandLists(1, lists);
    d.WaitIdle();
    CHECK(d.device->GetDeviceRemovedReason() == S_OK);
    const uint64_t live = m::GetStats().live;
    a.Reset();
    CHECK(m::GetStats().live + (s.placed ? 1u : 0u) == live); // freed with the resource
    b.Reset();
    std::printf("%s\n", m::StatusLine().c_str());
    CHECK(m::GetStats().live == 0);
    return test::finish("mem_alloc_test");
}
