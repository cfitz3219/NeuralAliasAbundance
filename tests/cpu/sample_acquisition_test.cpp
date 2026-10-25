// reconstructed 2026-10: worker ownership, completion provenance and packed rows.
#include "test_support.hpp"
#include "providers/discovery/sample_acquisition.hpp"
#include "providers/discovery/resource_formats.hpp"
using namespace na::discovery;
struct Fake : SampleApi {
    na_sample_info info{};
    std::vector<uint8_t> bytes;
    uint64_t next=1; unsigned reads=0,releases=0;
    bool fail_read=false,short_read=false,fail_request=false,buffer=false;
    Fake() {
        info.status=NA_SAMPLE_READY; info.session=1; info.epoch=2; info.resource=3;
        info.queue=9; info.queue_ticket=17; info.width=2; info.height=2; info.format=40;
        info.row_pitch=256; info.data_bytes=264; bytes.resize(264,0xCC);
        const float values[]={0.1f,0.2f,0.3f,0.4f};
        std::memcpy(bytes.data(),values,8); std::memcpy(bytes.data()+256,values+2,8);
    }
    int Request(const na_sample_request& r,uint64_t& t) override {
        if(buffer) CHECK(r.width==64 && r.height==1 && r.x==256 && r.y==1);
        else CHECK(r.width==0 && r.height==0);
        if(fail_request) return NA_E_STATE;
        t=next++; return NA_OK;
    }
    int Poll(uint64_t,na_sample_info& i) override { i=info; return NA_OK; }
    int Read(uint64_t,void* p,uint64_t n,uint64_t& w) override {
        ++reads; if(fail_read) return NA_E_STATE;
        CHECK(n==bytes.size()); std::memcpy(p,bytes.data(),bytes.size()); w=n-(short_read?1:0); return NA_OK;
    }
    void Release(uint64_t) noexcept override { ++releases; }
};
static SamplePlan plan() { return {3,0,2,2,40}; }
int main() { return test::run({
    {"timeout releases its lease and the same resource can complete on retry", [] {
        Fake f;SampleAcquisition a(f);a.Scope(1,2);CHECK(a.Begin(plan()));
        f.info.status=NA_SAMPLE_FAILED;f.info.reason=NA_SAMPLE_E_TIMEOUT;
        auto out=a.Pump();CHECK(out.size()==1 && out[0].status==AcquisitionStatus::HostFailure);
        CHECK(out[0].plan.resource==3 && a.Pending()==0 && f.releases==1 && f.reads==0);
        CHECK(a.Begin(plan()));f.info.status=NA_SAMPLE_READY;f.info.reason=0;
        out=a.Pump();CHECK(out.size()==1 && out[0].status==AcquisitionStatus::Ready);
        CHECK(f.releases==2 && f.reads==1);
    }},
    {"retirement cancels pending ranges without reading or releasing other resources", [] {
        Fake f;SampleAcquisition a(f);a.Scope(1,2);
        CHECK(a.Begin(plan()));auto sub=plan();sub.subresource=1;CHECK(a.Begin(sub));
        auto other=plan();other.resource=4;CHECK(a.Begin(other));
        a.Retire(3);CHECK(a.Pending()==1 && f.releases==2 && f.reads==0);
        a.Retire(3);CHECK(f.releases==2);
        f.info.resource=4;auto out=a.Pump();CHECK(out.size()==1 && out[0].plan.resource==4);
        CHECK(out[0].status==AcquisitionStatus::Ready && f.releases==3);
        a.Clear();CHECK(f.releases==3);
    }},
    {"buffer leases preserve 64-bit offsets and reject wrong completion ranges", [] {
        for(int mode=0;mode<4;++mode) {
            Fake f;f.buffer=true;SampleAcquisition a(f);a.Scope(1,2);
            SamplePlan p{3,0,64,1,0,true,(1ull<<32)+256};CHECK(a.Begin(p));
            f.info.format=0;f.info.width=64;f.info.height=1;f.info.x=256;f.info.y=1;
            f.info.row_pitch=64;f.info.data_bytes=64;f.bytes.resize(64);
            if(mode==1) ++f.info.x;if(mode==2) f.info.y=0;if(mode==3) ++f.info.row_pitch;
            const auto out=a.Pump();CHECK(out.size()==1 && out[0].plan.offset==p.offset);
            CHECK((out[0].status==AcquisitionStatus::Ready)==(mode==0));
            CHECK(out[0].bytes.size()==(mode==0?64u:0u));
        }
    }},
    {"16-bit scalar samples preserve padded rows and explicit interpretation", [] {
        AcquiredSample s{};s.status=AcquisitionStatus::Ready;s.info.status=NA_SAMPLE_READY;s.info.width=2;s.info.height=2;
        s.info.row_pitch=8;s.info.data_bytes=12;s.bytes.resize(12);
        const uint16_t raw[]={0,65535,32768,16384};
        std::memcpy(s.bytes.data(),raw,4);std::memcpy(s.bytes.data()+8,raw+2,4);
        std::vector<double> values;
        for(uint32_t format:{55u,56u}) {
            s.info.format=format;CHECK(DecodeScalarDepth(s,values));
            NEAR(values[0],0,1e-9);NEAR(values[1],1,1e-9);NEAR(values[2],32768.0/65535,1e-9);
        }
        const uint16_t half[]={0x0001,0x3c00,0x3800,0xbc00};
        std::memcpy(s.bytes.data(),half,4);std::memcpy(s.bytes.data()+8,half+2,4);
        s.info.format=54;CHECK(DecodeScalarDepth(s,values));
        NEAR(values[0],std::ldexp(1.0,-24),1e-12);NEAR(values[1],1,1e-9);
        NEAR(values[2],0.5,1e-9);NEAR(values[3],-1,1e-9);
        s.info.format=53;CHECK(!DecodeScalarDepth(s,values));
        s.info.format=54;s.bytes.pop_back();CHECK(!DecodeScalarDepth(s,values));
    }},
    {"common HDR and packed-depth inventory formats are acquisition candidates", [] {
        using na::classifier::Format;
        CHECK(ResourceFormat(19)==Format::Depth && ResourceFormat(44)==Format::Depth);
        CHECK(ResourceFormat(26)==Format::ColorFloat && ResourceFormat(24)==Format::ColorUnorm);
        CHECK(ResourceFormat(0)==Format::Unknown);
    }},
    {"packed depth plans require the host's explicit plane format and padded decoding", [] {
        for(uint32_t resource_format : {19u,20u,44u,45u}) {
            Fake f; SampleAcquisition a(f);a.Scope(1,2);
            auto p=plan();p.format=resource_format;CHECK(a.Begin(p));
            f.info.format=SampleFormat(resource_format);
            if(f.info.format==46) {
                const uint32_t words[]={0xAA000000u,0x55FFFFFFu,0xCC800000u,0xDD400000u};
                std::memcpy(f.bytes.data(),words,8);std::memcpy(f.bytes.data()+256,words+2,8);
            }
            auto out=a.Pump();CHECK(out.size()==1 && out[0].status==AcquisitionStatus::Ready);
            std::vector<double> depth;CHECK(DecodeScalarDepth(out[0],depth));
            if(f.info.format==46) {NEAR(depth[0],0,1e-7);NEAR(depth[1],1,1e-7);NEAR(depth[2],0.5,1e-7);}
            else NEAR(depth[3],0.4,1e-6);
            out[0].info.format=39;CHECK(!DecodeScalarDepth(out[0],depth));
        }
        Fake f;SampleAcquisition a(f);a.Scope(1,2);auto p=plan();p.format=19;CHECK(a.Begin(p));
        f.info.format=19;CHECK(a.Pump()[0].status==AcquisitionStatus::ShapeMismatch);
    }},
    {"pending leases never read and completion keeps actual ticket and owned padded rows", [] {
        Fake f; SampleAcquisition a(f); a.Scope(1,2); CHECK(a.Begin(plan()));
        f.info.status=NA_SAMPLE_IN_FLIGHT; CHECK(a.Pump().empty() && f.reads==0);
        f.info.status=NA_SAMPLE_READY; auto out=a.Pump(); CHECK(out.size()==1 && a.Pending()==0 && f.releases==1);
        CHECK(out[0].status==AcquisitionStatus::Ready && out[0].info.queue_ticket==17);
        f.bytes.clear(); std::vector<double> depth; CHECK(DecodeScalarDepth(out[0],depth));
        CHECK(depth.size()==4); NEAR(depth[0],0.1,1e-6); NEAR(depth[3],0.4,1e-6);
        out[0].bytes.pop_back(); CHECK(!DecodeScalarDepth(out[0],depth) && depth.empty());
    }},
    {"scope shape and expiry reject without exposing bytes", [] {
        for(int mode=0;mode<5;++mode) {
            Fake f; SampleAcquisition a(f); a.Scope(1,2); CHECK(a.Begin(plan()));
            if(mode==0) ++f.info.epoch;
            if(mode==1) ++f.info.resource;
            if(mode==2) f.info.queue_ticket=UINT64_MAX;
            if(mode==3) f.fail_read=true;
            if(mode==4) f.short_read=true;
            const auto out=a.Pump(); CHECK(out.size()==1 && out[0].bytes.empty());
            CHECK(out[0].status!=AcquisitionStatus::Ready && f.releases==1);
        }
    }},
    {"bounded leases reject duplicates cancel on reset and retry host refusal", [] {
        Fake f;
        { SampleAcquisition a(f); CHECK(!a.Begin(plan())); a.Scope(1,2);
          f.fail_request=true; CHECK(!a.Begin(plan()) && a.Pending()==0); f.fail_request=false;
          CHECK(a.Begin(plan()) && !a.Begin(plan()));
          for(uint64_t id=4;id<7;++id) { auto p=plan(); p.resource=id; CHECK(a.Begin(p)); }
          auto p=plan(); p.resource=7; CHECK(!a.Begin(p));
          a.Scope(1,3); CHECK(a.Pending()==0 && f.releases==4); CHECK(a.Begin(plan())); }
        CHECK(f.releases==5);
    }},
    {"typeless format is not silently interpreted as depth", [] {
        Fake f; SampleAcquisition a(f); a.Scope(1,2); CHECK(a.Begin(plan()));
        auto out=a.Pump(); out[0].info.format=39; std::vector<double> depth;
        CHECK(!DecodeScalarDepth(out[0],depth));
    }}
}); }

