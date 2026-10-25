#include "../host/test_support.hpp"
#include "host_dx12/host_observe_samples.h"
#include "host_dx12/observe/producers.hpp"
#include "providers/discovery/capture_decoder.hpp"
#include <atomic>
namespace {
std::atomic<unsigned> ambiguous_waits{}, ambiguous_executes{};
void __cdecl Sink(const na_event_batch* b,void*) {
    const auto decoded=na::discovery::DecodeBatch(b,size_t(b->header_bytes)+b->payload_bytes);
    for(const auto& e:decoded.events) if(e.header.flags&NA_EVF_ORDER_AMBIGUOUS) {
        if(e.header.kind==NA_EV_QUEUE_WAIT) ++ambiguous_waits;
        if(e.header.kind==NA_EV_QUEUE_EXECUTE) ++ambiguous_executes;
    }
}
}
int main(){
    test::Device d;if(!test::CreateDevice(d))return test::SkipNoDevice("queue_reentry");
    REQUIRE(na::observe::InstallProducers(d.device.Get(),d.queue.Get(),true));
    REQUIRE(na_observe_start(nullptr,Sink,nullptr)==NA_OK);
    auto tex=test::CreateTexture(d.device.Get(),8,8,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_NONE);
    REQUIRE(tex);
    na_sample_request r{};r.size_bytes=sizeof(r);r.resource=na::observe::IdOf(tex.Get());r.arm_timeout_ms=5000;
    uint64_t sample{};REQUIRE(na_observe_sample_request(&r,&sample)==NA_OK);
    test::ComPtr<ID3D12Fence> fence;
    REQUIRE(SUCCEEDED(d.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))));
    // Reproduce Anno's driver Wait callback inside the forwarded Execute scope.
    // This is not a game submission or evidence of texture-copy completion.
    const auto outer=na::observe::ExecuteBegin(d.queue.Get());
    REQUIRE(outer.submit_locked);
    bool threw=false;
    try { CHECK(SUCCEEDED(d.queue->Wait(fence.Get(),0))); }
    catch(const std::exception& e){ std::printf("nested queue wait threw: %s\n",e.what());threw=true; }
    na::observe::ExecuteEnd(outer,d.queue.Get(),0,nullptr);
    CHECK(!threw);
    na_sample_info info{};info.size_bytes=sizeof(info);
    REQUIRE(na_observe_sample_poll(sample,&info)==NA_OK);
    CHECK(info.status==NA_SAMPLE_ARMED); // nested operations must not authorize a copy
    na_observe_sample_release(sample);
    REQUIRE(na_observe_stop()==NA_OK);
    CHECK(ambiguous_waits==1);CHECK(ambiguous_executes==1);
    return test::finish("queue_reentry");
}
