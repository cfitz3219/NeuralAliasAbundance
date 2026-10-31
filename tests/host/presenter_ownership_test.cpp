// Reviewed O1: an external presenter cannot strand an acquired image.
// All GPU readbacks/waits here are test-only; no game is launched.
#include "test_support.hpp"
#include "host_dx12/overlay/overlay.hpp"
#include "host_dx12/overlay/presenter.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
#include "host_dx12/settings.hpp"
#include <thread>
namespace na::overlay {
long ReadNativeBufferForTests(IDXGISwapChain*,unsigned,REFIID,void**);
void BoundaryPresentForTests(IDXGISwapChain*,IDXGISwapChain*,ID3D12CommandQueue*);
void BoundarySeenForTests(IDXGISwapChain*,IDXGISwapChain*);
}

bool red_pixel(test::Device& d,ID3D12Resource* image) {
    auto desc=image->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes=0;
    d.device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
    auto read=test::CreateBuffer(d.device.Get(),bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    test::List commands;if(!read || !commands.Create(d.device.Get()))return false;
    auto pre=test::Transition(image,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_SOURCE);
    commands.list->ResourceBarrier(1,&pre);
    D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=image;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=read.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprint;
    commands.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    auto post=test::Transition(image,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PRESENT);
    commands.list->ResourceBarrier(1,&post);commands.list->Close();
    ID3D12CommandList* lists[]={commands.list.Get()};d.queue->ExecuteCommandLists(1,lists);d.WaitIdle();
    void* data=nullptr;D3D12_RANGE range{0,4};if(FAILED(read->Map(0,&range,&data)))return false;
    const auto* pixel=static_cast<const unsigned char*>(data);
    const bool red=pixel[0]>240 && pixel[1]<10 && pixel[2]<10;
    D3D12_RANGE empty{0,0};read->Unmap(0,&empty);return red;
}
struct ConcurrentAcquire {
    IDXGISwapChain* chain=nullptr;HANDLE complete=nullptr;std::thread worker;bool acquired=false,returned=false;
};
void acquire_during_frame(ID3D12CommandQueue*,ID3D12Resource*,void* user){
    auto* state=static_cast<ConcurrentAcquire*>(user);
    state->worker=std::thread([state]{test::ComPtr<ID3D12Resource> buffer;
        state->acquired=SUCCEEDED(state->chain->GetBuffer(0,IID_PPV_ARGS(&buffer)));SetEvent(state->complete);});
    state->returned=WaitForSingleObject(state->complete,1000)==WAIT_OBJECT_0;
}
int main() {
    setvbuf(stdout,nullptr,_IONBF,0);
    SetEnvironmentVariableW(L"NEURALALIAS_TEST_WARP",L"1");
    test::Device d;REQUIRE(SUCCEEDED(CreateDXGIFactory2(0,IID_PPV_ARGS(&d.factory))));
    test::ComPtr<IDXGIAdapter> warp;REQUIRE(SUCCEEDED(d.factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))));
    REQUIRE(SUCCEEDED(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d.device))));
    D3D12_COMMAND_QUEUE_DESC q{};REQUIRE(SUCCEEDED(d.device->CreateCommandQueue(&q,IID_PPV_ARGS(&d.queue))));
    REQUIRE(SUCCEEDED(d.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&d.fence))));
    d.event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    std::printf("stage: core\n");
    REQUIRE(na::hooking::Initialize());REQUIRE(na::overlay::StartCore("OWNERSHIP",nullptr));
    na::settings::Get().nr_enabled.store(0);na::present::SetEnabled(true);
    HWND window=test::CreateTestWindow(L"presenter ownership");REQUIRE(window);
    DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=320;desc.Height=180;desc.BufferCount=2;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;
    desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    test::ComPtr<IDXGISwapChain1> chain;REQUIRE(SUCCEEDED(d.factory->CreateSwapChainForHwnd(d.queue.Get(),window,&desc,nullptr,nullptr,&chain)));
    test::ComPtr<IDXGISwapChain3> sc;REQUIRE(SUCCEEDED(chain.As(&sc)));
    test::ComPtr<ID3D12Resource> proxy,real;
    REQUIRE(SUCCEEDED(sc->GetBuffer(0,IID_PPV_ARGS(&proxy))));
    REQUIRE(SUCCEEDED(na::overlay::ReadNativeBufferForTests(sc.Get(),0,IID_PPV_ARGS(&real))));
    CHECK(proxy.Get()!=real.Get());real.Reset();CHECK(na::present::Proxied(sc.Get()));
    std::printf("stage: late-owner\n");
    // Late discovery preserves proxy identity and delivery, without generating
    // another set of intermediate pictures underneath native FG.
    HWND external_window=test::CreateTestWindow(L"external presentation");REQUIRE(external_window);
    test::ComPtr<IDXGISwapChain1> external;
    REQUIRE(SUCCEEDED(d.factory->CreateSwapChainForHwnd(d.queue.Get(),external_window,&desc,nullptr,nullptr,&external)));
    // Exercise actual route selection, not just the presenter's owner flag.
    na::overlay::BoundaryPresentForTests(sc.Get(),external.Get(),d.queue.Get());
    CHECK(!na::present::NoteExternalOwner(sc.Get()));
    // The early creation callback prevents substitution before first GetBuffer.
    na::overlay::BoundarySeenForTests(external.Get(),external.Get());
    test::ComPtr<ID3D12Resource> early,early_real;
    REQUIRE(SUCCEEDED(external->GetBuffer(0,IID_PPV_ARGS(&early))));
    REQUIRE(SUCCEEDED(na::overlay::ReadNativeBufferForTests(external.Get(),0,IID_PPV_ARGS(&early_real))));
    CHECK(early.Get()==early_real.Get());early.Reset();early_real.Reset();
    test::ComPtr<ID3D12DescriptorHeap> heap;D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=1;
    REQUIRE(SUCCEEDED(d.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap))));
    const auto rtv=heap->GetCPUDescriptorHandleForHeapStart();d.device->CreateRenderTargetView(proxy.Get(),nullptr,rtv);
    test::List commands;REQUIRE(commands.Create(d.device.Get()));
    auto pre=test::Transition(proxy.Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);
    commands.list->ResourceBarrier(1,&pre);const float red[4]={1,0,0,1};commands.list->ClearRenderTargetView(rtv,red,0,nullptr);
    auto post=test::Transition(proxy.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);
    commands.list->ResourceBarrier(1,&post);commands.list->Close();ID3D12CommandList* lists[]={commands.list.Get()};
    d.queue->ExecuteCommandLists(1,lists);REQUIRE(SUCCEEDED(sc->Present(0,0)));d.WaitIdle();
    std::printf("stage: read-delivery\n");
    // Test-only CPU drain before copying display-owned resources back to CPU.
    na::present::BeforeResize(sc.Get());na::present::AfterResize(sc.Get(),E_FAIL);
    bool delivered=false;for(UINT i=0;i<2;++i){REQUIRE(SUCCEEDED(na::overlay::ReadNativeBufferForTests(sc.Get(),i,IID_PPV_ARGS(&real))));delivered=red_pixel(d,real.Get())||delivered;real.Reset();}
    CHECK(delivered);
    ConcurrentAcquire concurrent;concurrent.chain=sc.Get();concurrent.complete=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    REQUIRE(concurrent.complete);na::present::PresentArgs args;args.sc=sc.Get();args.queue=d.queue.Get();
    args.frame=&acquire_during_frame;args.user=&concurrent;
    CHECK(SUCCEEDED(na::present::Present(args)));concurrent.worker.join();
    CHECK(concurrent.acquired && concurrent.returned);CloseHandle(concurrent.complete);
    d.WaitIdle();
    std::printf("stage: failed-resize\n");
    // A rejected resize must keep the exact old proxy and virtual rotation.
    const auto index=sc->GetCurrentBackBufferIndex();
    CHECK(FAILED(sc->ResizeBuffers(1,0,0,DXGI_FORMAT_UNKNOWN,0))); // flip chains require >=2 buffers
    test::ComPtr<ID3D12Resource> again;REQUIRE(SUCCEEDED(sc->GetBuffer(0,IID_PPV_ARGS(&again))));
    CHECK(again.Get()==proxy.Get());CHECK(sc->GetCurrentBackBufferIndex()==index);
    std::printf("stage: successful-resize\n");
    again.Reset();proxy.Reset();
    REQUIRE(SUCCEEDED(sc->ResizeBuffers(0,400,220,DXGI_FORMAT_UNKNOWN,0)));
    CHECK(!na::present::Proxied(sc.Get()));CHECK(na::present::NoteExternalOwner(sc.Get()));
    REQUIRE(SUCCEEDED(sc->GetBuffer(0,IID_PPV_ARGS(&again))));REQUIRE(SUCCEEDED(na::overlay::ReadNativeBufferForTests(sc.Get(),0,IID_PPV_ARGS(&real))));
    CHECK(again.Get()==real.Get());again.Reset();real.Reset();
    std::printf("stage: shutdown\n");
    // Clear owner metadata at release/shutdown, with no borrowed chain pin.
    REQUIRE(d.device->GetDeviceRemovedReason()==S_OK);REQUIRE(na::overlay::Stop());sc.Reset();chain.Reset();external.Reset();DestroyWindow(window);DestroyWindow(external_window);
    return test::finish("presenter_ownership_test");
}
