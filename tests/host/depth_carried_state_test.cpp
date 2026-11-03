#include "test_support.hpp"
#include "host_dx12/host_observe.h"
#include "host_dx12/observe/producers.hpp"
static void __cdecl Sink(const na_event_batch*,void*) {}
int main() {
    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("depth_carried_state_test");
    auto* dev=d.device.Get();
    REQUIRE(na::observe::InstallProducers(dev,d.queue.Get(),true));
    na_observe_config cfg{}; cfg.size_bytes=sizeof cfg;
    REQUIRE(na_observe_start(&cfg,&Sink,nullptr)==NA_OK);
    D3D12_HEAP_DESC hd{};
    hd.SizeInBytes=1<<20; hd.Properties.Type=D3D12_HEAP_TYPE_DEFAULT;
    hd.Flags=D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;
    test::ComPtr<ID3D12Heap> heap;
    REQUIRE(SUCCEEDED(dev->CreateHeap(&hd,IID_PPV_ARGS(&heap))));
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width=rd.Height=64;
    rd.DepthOrArraySize=rd.MipLevels=1; rd.Format=DXGI_FORMAT_R32_TYPELESS;
    rd.SampleDesc.Count=1; rd.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    test::ComPtr<ID3D12Resource> depth, alias;
    REQUIRE(SUCCEEDED(dev->CreatePlacedResource(heap.Get(),0,&rd,D3D12_RESOURCE_STATE_DEPTH_WRITE,nullptr,IID_PPV_ARGS(&depth))));
    REQUIRE(SUCCEEDED(dev->CreatePlacedResource(heap.Get(),0,&rd,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&alias))));
    const auto id=na::observe::IdOf(depth.Get());
    constexpr auto read=static_cast<D3D12_RESOURCE_STATES>(D3D12_RESOURCE_STATE_DEPTH_READ|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    test::List pass;
    REQUIRE(pass.Create(dev));
    REQUIRE(SUCCEEDED(pass.list->Close()));
    pass.Reset(); // begin a fully observed recording, as the game does after pool creation
    auto transition=test::Transition(depth.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,read);
    pass.list->ResourceBarrier(1,&transition);
    REQUIRE(SUCCEEDED(pass.list->Close()));
    ID3D12CommandList* lists[]={pass.list.Get()};
    d.queue->ExecuteCommandLists(1,lists); d.WaitIdle();
    uint32_t state=0;
    char evidence[256]{};
    na::observe::DescribeStateEvidence(pass.list.Get(),id,0,evidence,sizeof evidence);
    std::printf("depth %llu: %s\n",static_cast<unsigned long long>(id),evidence);
    REQUIRE(na::observe::KnownSubStateNow(id,0,&state));
    CHECK(state==read);
    // An unrelated, empty later recording must not erase the depth pass state.
    pass.Reset(); REQUIRE(SUCCEEDED(pass.list->Close()));
    CHECK(na::observe::ListStateSub(pass.list.Get(),id,0,&state)==na::observe::StateLookup::Absent);
    d.queue->ExecuteCommandLists(1,lists); d.WaitIdle();
    CHECK(na::observe::KnownSubStateNow(id,0,&state) && state==read);
    pass.Reset();
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    barrier.Aliasing.pResourceBefore=depth.Get(); barrier.Aliasing.pResourceAfter=alias.Get();
    pass.list->ResourceBarrier(1,&barrier);
    CHECK(na::observe::ListStateSub(pass.list.Get(),id,0,&state)==na::observe::StateLookup::Unavailable);
    REQUIRE(SUCCEEDED(pass.list->Close()));
    d.queue->ExecuteCommandLists(1,lists); d.WaitIdle();
    CHECK(!na::observe::KnownSubStateNow(id,0,&state));
    pass.Reset(); barrier.Aliasing.pResourceBefore=alias.Get(); barrier.Aliasing.pResourceAfter=depth.Get();
    pass.list->ResourceBarrier(1,&barrier);
    transition=test::Transition(depth.Get(),read,D3D12_RESOURCE_STATE_DEPTH_WRITE);
    pass.list->ResourceBarrier(1,&transition);
    REQUIRE(SUCCEEDED(pass.list->Close()));
    d.queue->ExecuteCommandLists(1,lists); d.WaitIdle();
    CHECK(na::observe::KnownSubStateNow(id,0,&state) && state==D3D12_RESOURCE_STATE_DEPTH_WRITE);
    pass.Reset(); barrier.Aliasing.pResourceBefore=nullptr; barrier.Aliasing.pResourceAfter=nullptr;
    pass.list->ResourceBarrier(1,&barrier); REQUIRE(SUCCEEDED(pass.list->Close()));
    d.queue->ExecuteCommandLists(1,lists); d.WaitIdle();
    CHECK(!na::observe::KnownSubStateNow(id,0,&state));
    CHECK(dev->GetDeviceRemovedReason()==S_OK);
    na_observe_stop();
    return test::finish("depth_carried_state_test");
}
