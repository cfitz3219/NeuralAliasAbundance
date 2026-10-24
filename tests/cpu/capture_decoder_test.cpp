#include "test_support.hpp"
#include "providers/discovery/capture_decoder.hpp"
using namespace na::discovery;
namespace {
template<class T> std::vector<unsigned char> Packet(T event, uint32_t kind) {
    na_event_batch b{}; b.header_bytes=sizeof(b); b.abi_major=NA_OBSERVE_ABI_MAJOR;
    b.abi_minor=NA_OBSERVE_ABI_MINOR; b.session=42; b.epoch=7;
    b.event_count=1; b.payload_bytes=sizeof(T);
    event.hdr.size_bytes=sizeof(T); event.hdr.kind=kind; event.hdr.epoch=b.epoch;
    std::vector<unsigned char> bytes(sizeof(b)+sizeof(T));
    std::memcpy(bytes.data(), &b, sizeof(b)); std::memcpy(bytes.data()+sizeof(b), &event, sizeof(T));
    return bytes;
}
template<class T> void Put(std::vector<unsigned char>& p, size_t offset, T value) {
    std::memcpy(p.data()+offset, &value, sizeof(value));
}
DecodedBatch Decode(const std::vector<unsigned char>& p) { return DecodeBatch(p.data(),p.size()); }
constexpr size_t base=sizeof(na_event_batch);
}
int main() { return test::run({
    {"unaligned input is copied; owner may release input", [] {
        auto p=Packet(na_ev_resource_created{}, NA_EV_RESOURCE_CREATED);
        std::vector<unsigned char> unaligned(p.size()+1); std::memcpy(unaligned.data()+1,p.data(),p.size());
        auto d=DecodeBatch(unaligned.data()+1,p.size()); CHECK(d.valid && d.events.size()==1);
        unaligned.clear(); CHECK(d.bytes==p); CHECK(d.events[0].offset==base);
    }},
    {"every truncation rejected atomically", [] {
        auto p=Packet(na_ev_resource_created{},NA_EV_RESOURCE_CREATED);
        for(size_t n=0;n<p.size();++n) { auto d=DecodeBatch(p.data(),n); CHECK(!d.valid && d.events.empty() && d.bytes.empty()); }
        Put(p,base+offsetof(na_event_header,size_bytes),uint32_t(32)); CHECK(!Decode(p).valid);
    }},
    {"version count alignment and epoch mismatches rejected", [] {
        auto good=Packet(na_ev_resource_created{},NA_EV_RESOURCE_CREATED);
        auto p=good; Put(p,offsetof(na_event_batch,abi_major),uint16_t(99)); CHECK(!Decode(p).valid);
        p=good; Put(p,offsetof(na_event_batch,abi_minor),uint16_t(99)); CHECK(Decode(p).valid);
        p=good; Put(p,offsetof(na_event_batch,event_count),uint32_t(0)); CHECK(!Decode(p).valid);
        p=good; Put(p,base+offsetof(na_event_header,epoch),uint64_t(8)); CHECK(!Decode(p).valid);
        p=good; Put(p,base+offsetof(na_event_header,size_bytes),uint32_t(33)); CHECK(!Decode(p).valid);
        p=good; Put(p,offsetof(na_event_batch,session),uint64_t(NA_UNKNOWN_U64)); CHECK(!Decode(p).valid);
    }},
    {"unknown foundational event remains explicit", [] {
        auto p=Packet(na_ev_resource_created{},9000); CHECK(Decode(p).valid && !Decode(p).unknown_foundational);
        Put(p,base+offsetof(na_event_header,flags),uint32_t(NA_EVF_FOUNDATIONAL));
        auto d=Decode(p); CHECK(d.valid && d.unknown_foundational);
    }},
    {"array extension offsets and overflow checks", [] {
        na_ev_queue_execute e{}; auto p=Packet(e,NA_EV_QUEUE_EXECUTE);
        p.resize(p.size()+24); Put(p,offsetof(na_event_batch,payload_bytes),uint32_t(sizeof(e)+24));
        Put(p,base+offsetof(na_event_header,size_bytes),uint32_t(sizeof(e)+24));
        Put(p,base+offsetof(na_ev_queue_execute,list_count),uint32_t(1));
        Put(p,base+offsetof(na_ev_queue_execute,refs_offset),uint32_t(sizeof(e)+8));
        Put(p,base+offsetof(na_ev_queue_execute,ref_stride),uint32_t(24)); CHECK(Decode(p).valid);
        auto good=p; Put(p,base+offsetof(na_ev_queue_execute,list_count),UINT32_MAX); CHECK(!Decode(p).valid);
        p=good; Put(p,base+offsetof(na_ev_queue_execute,refs_offset),uint32_t(8)); CHECK(!Decode(p).valid);
        p=good; Put(p,base+offsetof(na_ev_queue_execute,ref_stride),uint32_t(0)); CHECK(!Decode(p).valid);
    }},
    {"malformed tail discards earlier valid events", [] {
        auto p=Packet(na_ev_resource_created{},NA_EV_RESOURCE_CREATED);
        const size_t second=p.size(); p.resize(second+sizeof(na_event_header));
        na_event_header bad{}; bad.kind=NA_EV_RESOURCE_CREATED; bad.epoch=7; bad.size_bytes=sizeof(bad);
        Put(p,second,bad); Put(p,offsetof(na_event_batch,event_count),uint32_t(2));
        Put(p,offsetof(na_event_batch,payload_bytes),static_cast<uint32_t>(p.size()-base));
        auto d=Decode(p); CHECK(!d.valid && d.events.empty() && d.bytes.empty());
    }},
    {"all known kinds reject undersized records", [] {
        for(uint32_t kind=1;kind<=23;++kind) {
            CHECK(KnownEventSize(kind)>sizeof(na_event_header));
            auto p=Packet(na_ev_resource_created{},kind);
            p.resize(base+sizeof(na_event_header));
            Put(p,offsetof(na_event_batch,payload_bytes),uint32_t(sizeof(na_event_header)));
            Put(p,base+offsetof(na_event_header,size_bytes),uint32_t(sizeof(na_event_header)));
            CHECK(!Decode(p).valid);
        }
    }},
    {"future inline stride need not have natural alignment", [] {
        na_ev_capability_report c{}; c.session=42; c.entry_count=2;
        c.entries_offset=sizeof(c); c.entry_stride=20;
        auto p=Packet(c,NA_EV_CAPABILITY_REPORT); p.resize(base+sizeof(c)+40);
        Put(p,offsetof(na_event_batch,payload_bytes),uint32_t(sizeof(c)+40));
        Put(p,base+offsetof(na_event_header,size_bytes),uint32_t(sizeof(c)+40));
        CHECK(Decode(p).valid);
    }},
    {"identity records and capability arrays checked", [] {
        na_ev_epoch_begin e{}; e.session=42; e.epoch=7;
        auto p=Packet(e,NA_EV_EPOCH_BEGIN); CHECK(Decode(p).valid);
        Put(p,base+offsetof(na_ev_epoch_begin,session),uint64_t(43)); CHECK(!Decode(p).valid);
        na_ev_capability_report c{}; c.session=42; c.entry_count=1; c.entries_offset=sizeof(c); c.entry_stride=sizeof(na_cap_entry);
        CHECK(!Decode(Packet(c,NA_EV_CAPABILITY_REPORT)).valid);
    }},
}); }

