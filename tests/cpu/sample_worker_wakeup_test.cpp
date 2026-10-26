// Pending producer regression: request publication must wake an idle worker.
// Real device, but no GPU submissions or sample copies.
#include "../host/test_support.hpp"
#include "host_dx12/host_observe_samples.h"
#include "host_dx12/observe/producers.hpp"
namespace {
void __cdecl Sink(const na_event_batch*,void*){}
struct Stop { ~Stop(){na_observe_stop();} };
na_sample_info Poll(uint64_t token){na_sample_info i{};i.size_bytes=sizeof(i);na_observe_sample_poll(token,&i);return i;}
}
int main(){
    test::Device d;if(!test::CreateDevice(d))return test::SkipNoDevice("sample_worker_wakeup");
    REQUIRE(na::observe::InstallProducers(d.device.Get(),d.queue.Get(),true));
    na_observe_config cfg{};cfg.size_bytes=sizeof(cfg);
    REQUIRE(na_observe_start(&cfg,Sink,nullptr)==NA_OK);Stop stop;
    auto tex=test::CreateTexture(d.device.Get(),8,8,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_NONE);
    REQUIRE(tex);
    na_sample_request r{};r.size_bytes=sizeof(r);r.resource=na::observe::IdOf(tex.Get());r.arm_timeout_ms=20;
    uint64_t first=0;REQUIRE(na_observe_sample_request(&r,&first)==NA_OK);
    // Confirm maintenance runs before arranging the idle-to-request transition.
    na_sample_info warm{};
    for(int n=0;n<200;++n){warm=Poll(first);if(warm.status!=NA_SAMPLE_ARMED)break;Sleep(5);}
    CHECK(warm.status==NA_SAMPLE_FAILED && warm.reason==NA_SAMPLE_E_TIMEOUT);
    REQUIRE(na_observe_sample_release(first)==NA_OK);
    Sleep(100); // allow the worker to enter its empty-store wait
    uint64_t second=0;REQUIRE(na_observe_sample_request(&r,&second)==NA_OK);
    na_sample_info result{};
    for(int n=0;n<200;++n){result=Poll(second);if(result.status!=NA_SAMPLE_ARMED)break;Sleep(5);}
    std::printf("idle worker request: status %u reason %u (expected FAILED/TIMEOUT)\n",result.status,result.reason);
    CHECK(result.status==NA_SAMPLE_FAILED && result.reason==NA_SAMPLE_E_TIMEOUT);
    CHECK(na::observe::GetProducerStats().sample_workers_started==1);
    na_observe_sample_release(second);
    return test::finish("sample_worker_wakeup");
}
