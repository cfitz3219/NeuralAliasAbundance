#include "../host/test_support.hpp"
#include "host_dx12/observe/producers.hpp"
#include "providers/discovery/capture_decoder.hpp"
#include <vector>
namespace {
std::vector<na_ev_present> presents;
std::vector<na_ev_swapchain_created> chains;
std::vector<na_object_id> retired;
void __cdecl Sink(const na_event_batch* b,void*) {
    auto decoded=na::discovery::DecodeBatch(b,size_t(b->header_bytes)+b->payload_bytes);
    for(const auto& e:decoded.events) {
        const auto* p=decoded.bytes.data()+e.offset;
        if(e.header.kind==NA_EV_PRESENT){na_ev_present v;std::memcpy(&v,p,sizeof(v));presents.push_back(v);}
        if(e.header.kind==NA_EV_SWAPCHAIN_CREATED){na_ev_swapchain_created v;std::memcpy(&v,p,sizeof(v));chains.push_back(v);}
        if(e.header.kind==NA_EV_OBJECT_RETIRED){na_ev_object_retired v;std::memcpy(&v,p,sizeof(v));retired.push_back(v.object);}
    }
}
}
int main(){
    test::Device d;if(!test::CreateDevice(d))return test::SkipNoDevice("present_observation");
    REQUIRE(na::observe::InstallProducers(d.device.Get(),d.queue.Get(),true));
    REQUIRE(na_observe_start(nullptr,Sink,nullptr)==NA_OK);
    HWND window=test::CreateTestWindow(L"Present observation");REQUIRE(window);
    DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=320;desc.Height=180;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    test::ComPtr<IDXGISwapChain1> chain;
    REQUIRE(SUCCEEDED(d.factory->CreateSwapChainForHwnd(d.queue.Get(),window,&desc,nullptr,nullptr,&chain)));
    const auto first=na::observe::PresentBegin(chain.Get());
    const HRESULT ok=chain->Present(0,DXGI_PRESENT_TEST);
    na::observe::PresentEnd(first,0,DXGI_PRESENT_TEST,ok);
    const auto second=na::observe::PresentBegin(chain.Get());
    const HRESULT bad=chain->Present(5,0); // invalid sync interval, must still consume an attempt
    na::observe::PresentEnd(second,5,0,bad);
    REQUIRE(FAILED(bad));CHECK(first.swapchain==second.swapchain);CHECK(first.ordinal==1 && second.ordinal==2);
    chain.Reset();DestroyWindow(window);
    REQUIRE(na_observe_stop()==NA_OK);
    REQUIRE(presents.size()==2);REQUIRE(chains.size()==1);
    CHECK(presents[0].hresult==ok && presents[0].present_flags==DXGI_PRESENT_TEST);
    CHECK(presents[1].hresult==bad && presents[1].present_ordinal==2);
    CHECK(presents[0].backbuffer_index<2);
    CHECK(chains[0].queue==0 && chains[0].buffer_generation==NA_UNKNOWN_U32);
    CHECK(chains[0].width==320 && chains[0].height==180);
    CHECK(std::find(retired.begin(),retired.end(),first.swapchain)!=retired.end());
    return test::finish("present_observation");
}
