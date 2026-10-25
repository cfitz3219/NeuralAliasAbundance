// Exercise Claude's real H2 fixture with the discovery decoder on every batch.
#include "../host/test_support.hpp"
#include "providers/discovery/capture_decoder.hpp"
#include "providers/discovery/submission_evidence.hpp"
#include <atomic>
#include <cstdlib>
namespace decoder_bridge {
na_event_sink downstream{}; void* downstream_user{};
std::atomic<unsigned> invalid{0},h2_events{0},incomplete{0};
na::discovery::SubmissionEvidence execution;
std::atomic<unsigned> candidates{0};
void __cdecl Sink(const na_event_batch* b,void*) {
    const auto decoded=na::discovery::DecodeBatch(b,static_cast<size_t>(b->header_bytes)+b->payload_bytes);
    if(!decoded.valid)++invalid;
    if(decoded.has_unverified_semantics)++incomplete;
    if(GetEnvironmentVariableW(L"NA_TRACE_REDUCER",nullptr,0)) for(const auto& item:decoded.events) {
        std::printf("event kind=%u thread=%u seq=%llu flags=%u\n",item.header.kind,item.header.producer_thread,item.header.thread_seq,item.header.flags);
        if(item.header.kind==NA_EV_LIST_OP) {
            na_ev_list_op e{};std::memcpy(&e,decoded.bytes.data()+item.offset,sizeof(e));
            std::printf("op list=%llu gen=%llu index=%llu op=%u a0=%llu a1=%llu\n",e.list,e.recording_generation,e.op_index,e.op,e.a[0],e.a[1]);
            if(e.op==NA_OP_CLEAR_DSV || e.op==NA_OP_CLEAR_RTV || e.op==NA_OP_OM_SET_RENDER_TARGETS) {
                na_view_ref v{};std::memcpy(&v,decoded.bytes.data()+item.offset+e.payload_offset,sizeof(v));
                std::printf("view resource=%llu sub=%u format=%u kind=%u\n",v.resource,v.first_subresource,v.format,v.descriptor_kind);
            }
        }
    }
    na_observe_status status{};status.size_bytes=sizeof(status);
    if(na_observe_query_status(&status)==NA_OK) {
        execution.Consume(decoded,status);
        if(GetEnvironmentVariableW(L"NA_TRACE_REDUCER",nullptr,0)) std::printf("latest=%zu drops=%llu foundation=%llu\n",execution.Latest().size(),status.detail_dropped,status.foundational_dropped);
        for(const auto& pair:execution.Latest()) {
            const auto& v=pair.second;
            for(auto signal:{na::classifier::Signal::Depth,na::classifier::Signal::SceneColor})
                if(na::classifier::Describe(signal,v.window,v.observation).confidence==na::classifier::Confidence::Candidate) ++candidates;
        }
    }
    for(const auto& e:decoded.events)if(e.header.kind>=24&&e.header.kind<=26)++h2_events;
    downstream(b,downstream_user);
}
int Start(const na_observe_config* c,na_event_sink sink,void* user) {
    downstream=sink;downstream_user=user;return na_observe_start(c,&Sink,nullptr);
}
}
#define na_observe_start decoder_bridge::Start
#define main RunHostPassFixture
#include "../host/observe_passes_test.cpp"
#undef main
#undef na_observe_start
int main() {
    const int result=RunHostPassFixture();
    if(result) return result;
    CHECK(decoder_bridge::invalid.load()==0);
    CHECK(decoder_bridge::h2_events.load()>0);
    CHECK(decoder_bridge::incomplete.load()>0); // root table binding semantics require local verification
    CHECK(decoder_bridge::candidates.load()>0); // actual producer -> wire -> reducer -> classifier
    return test::finish("h2_capture_decoder");
}
