// D3D12 Memory Allocator wrapper (gpu/mem_alloc.hpp): a placed render target is usable after
// its first Clear, the allocation lives exactly as long as the resource, and a second
// resource shares the allocator. Hardware or WARP; SKIP (77) without either.
#include "test_support.hpp"
#include "host_dx12/gpu/mem_alloc.hpp"

#include <cstdio>
#include <vector>
#include <algorithm>
#include <chrono>
#include <d3d12sdklayers.h>

int main()
{
    test::ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("mem_alloc_test");
    namespace m = na::gpu::mem;
    test::ComPtr<ID3D12InfoQueue> info;d.device.As(&info);
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
    // Churn reuses backing blocks. Measure equal resource batches without asserting a guessed time saving.
    constexpr unsigned count=64;std::vector<double> pooled,native;
    const auto batch=[&](bool pool){std::vector<test::ComPtr<ID3D12Resource>> objects(count);
        const auto begin=std::chrono::steady_clock::now();
        for(auto& object:objects){HRESULT hr;
            if(pool)hr=m::CreateResource(d.device.Get(),D3D12_HEAP_TYPE_DEFAULT,rd,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,object.GetAddressOf());
            else {D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;hr=d.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&object));}
            CHECK(SUCCEEDED(hr)&&object);}
        if(pool)CHECK(m::GetStats().blocks<count);
        objects.clear();return std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-begin).count();};
    batch(true);const auto warmed=m::GetStats().block_bytes;
    for(unsigned i=0;i<5;++i){if(i%2){pooled.push_back(batch(true));native.push_back(batch(false));}else{native.push_back(batch(false));pooled.push_back(batch(true));}}
    std::sort(pooled.begin(),pooled.end());std::sort(native.begin(),native.end());
    std::printf("Allocation batch64 median_us: pooled=%.2f committed=%.2f cached_bytes=%llu\n",pooled[2],native[2],(unsigned long long)warmed);
    CHECK(m::GetStats().live==0);CHECK(m::GetStats().block_bytes<=warmed);
    // A different actual device gets its own allocator, rather than silently losing pooling.
    test::ComPtr<IDXGIAdapter> warp;test::ComPtr<ID3D12Device> other;
    if(SUCCEEDED(d.factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))&&SUCCEEDED(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&other)))&&other.Get()!=d.device.Get()){
        test::ComPtr<ID3D12Resource> extra;REQUIRE(SUCCEEDED(m::CreateResource(other.Get(),D3D12_HEAP_TYPE_DEFAULT,rd,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,extra.GetAddressOf())));
        CHECK(m::GetStats().devices==2);extra.Reset();CHECK(m::GetStats().live==0);
    }
    if(info)for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T bytes=0;info->GetMessage(i,nullptr,&bytes);std::vector<uint8_t> data(bytes);auto* message=reinterpret_cast<D3D12_MESSAGE*>(data.data());
        if(SUCCEEDED(info->GetMessage(i,message,&bytes))&&message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::printf("D3D12: %s\n",message->pDescription);CHECK(false);}}
    return test::finish("mem_alloc_test");
}
