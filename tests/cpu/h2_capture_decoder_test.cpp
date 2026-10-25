// Exercise Claude's real H2 fixture with the discovery decoder on every batch.
#include "../host/test_support.hpp"
#include "providers/discovery/capture_decoder.hpp"
#include <atomic>
namespace decoder_bridge {
na_event_sink downstream{}; void* downstream_user{};
std::atomic<unsigned> invalid{0},h2_events{0},incomplete{0};
void __cdecl Sink(const na_event_batch* b,void*) {
    const auto decoded=na::discovery::DecodeBatch(b,static_cast<size_t>(b->header_bytes)+b->payload_bytes);
    if(!decoded.valid)++invalid;
    if(decoded.has_unverified_semantics)++incomplete;
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
    return test::finish("h2_capture_decoder");
}
