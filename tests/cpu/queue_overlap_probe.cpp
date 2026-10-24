#include "../host/test_support.hpp"
#include "host_dx12/observe/producers.hpp"
#include "providers/discovery/capture_decoder.hpp"
#include <mutex>
#include <vector>
namespace {
std::mutex mutex;
std::vector<na_ev_queue_sync> syncs;
std::vector<na_ev_queue_execute> executes;
std::vector<na_ev_order_ambiguity_range> ranges;
void __cdecl Sink(const na_event_batch* b,void*) {
    auto batch=na::discovery::DecodeBatch(b,static_cast<size_t>(b->header_bytes)+b->payload_bytes);
    std::lock_guard<std::mutex> lock(mutex);
    for(const auto& item:batch.events) {
        const auto* p=batch.bytes.data()+item.offset;
        if(item.header.kind==NA_EV_QUEUE_SIGNAL || item.header.kind==NA_EV_QUEUE_WAIT) {na_ev_queue_sync e{};std::memcpy(&e,p,sizeof(e));syncs.push_back(e);}
        if(item.header.kind==NA_EV_QUEUE_EXECUTE) {na_ev_queue_execute e{};std::memcpy(&e,p,sizeof(e));executes.push_back(e);}
        if(item.header.kind==NA_EV_ORDER_AMBIGUITY_RANGE) {na_ev_order_ambiguity_range e{};std::memcpy(&e,p,sizeof(e));ranges.push_back(e);}
    }
}
struct Stop { bool running{}; ~Stop(){if(running)na_observe_stop();} };
}
int main() {
    test::Device d; if(!test::CreateDevice(d))return 77;
    REQUIRE(na::observe::InstallProducers(d.device.Get(),d.queue.Get(),false));
    test::ComPtr<ID3D12Fence> f; REQUIRE(SUCCEEDED(d.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f))));
    Stop stop; REQUIRE(na_observe_start(nullptr,Sink,nullptr)==NA_OK);stop.running=true;
    // Controlled host-owned Execute hook window. No ExecuteCommandLists or GPU
    // work is submitted here; this isolates bookkeeping overlap, not GPU order.
    auto token=na::observe::ExecuteBegin(d.queue.Get());
    REQUIRE(SUCCEEDED(d.queue->Signal(f.Get(),1)));
    REQUIRE(SUCCEEDED(d.queue->Wait(f.Get(),1)));
    na::observe::ExecuteEnd(token,d.queue.Get(),0,nullptr);
    REQUIRE(na_observe_stop()==NA_OK);stop.running=false;
    const auto covered=[&](uint64_t queue,uint64_t ticket,uint32_t flags) {
        if(flags&NA_EVF_ORDER_AMBIGUOUS)return true;
        for(const auto& r:ranges)if(r.queue==queue&&r.first_ticket<=ticket&&ticket<=r.last_ticket)return true;
        return false;
    };
    REQUIRE(executes.size()==1 && syncs.size()==2);
    for(const auto& e:executes){std::printf("execute ticket=%llu flags=%u\n",e.queue_ticket,e.hdr.flags);CHECK(covered(e.queue,e.queue_ticket,e.hdr.flags));}
    for(const auto& e:syncs){std::printf("sync kind=%u ticket=%llu flags=%u\n",e.hdr.kind,e.queue_ticket,e.hdr.flags);CHECK(covered(e.queue,e.queue_ticket,e.hdr.flags));}
    std::printf("ambiguity ranges=%zu\n",ranges.size());
    return test::finish("queue_overlap_probe");
}
