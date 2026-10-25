#include "test_support.hpp"
#include "providers/discovery/submission_evidence.hpp"
using namespace na::discovery;
struct Feed {
    SubmissionEvidence reducer;
    na_observe_status status{};
    uint64_t op{},generation=1,seq{};
    Feed() {status.session=1;status.epoch=2;status.size_bytes=sizeof(status);}
    template<class T> void Event(T e,uint32_t kind,const void* payload=nullptr,size_t n=0) {
        na_event_batch b{};b.header_bytes=sizeof(b);b.abi_major=NA_OBSERVE_ABI_MAJOR;
        b.session=1;b.epoch=2;b.event_count=1;b.payload_bytes=static_cast<uint32_t>((sizeof(e)+n+7)&~size_t(7));
        e.hdr.kind=kind;e.hdr.epoch=2;e.hdr.size_bytes=b.payload_bytes;e.hdr.producer_thread=1;e.hdr.thread_seq=++seq;
        std::vector<unsigned char> bytes(sizeof(b)+b.payload_bytes);
        std::memcpy(bytes.data(),&b,sizeof(b));std::memcpy(bytes.data()+sizeof(b),&e,sizeof(e));
        if(n)std::memcpy(bytes.data()+sizeof(b)+sizeof(e),payload,n);
        auto decoded=DecodeBatch(bytes.data(),bytes.size());CHECK(decoded.valid);reducer.Consume(decoded,status);
    }
    void Metadata() {
        na_ev_resource_created r{};r.resource=3;r.width=1920;r.height=1080;r.format=40;Event(r,NA_EV_RESOURCE_CREATED);
        na_ev_pipeline_created p{};p.pipeline=4;p.depth_enable=p.depth_write=1;Event(p,NA_EV_PIPELINE_CREATED);
    }
    void Reset() {op=0;na_ev_list_reset_close e{};e.list=5;e.recording_generation=generation;Event(e,NA_EV_LIST_RESET);}
    void Close() {na_ev_list_reset_close e{};e.list=5;e.recording_generation=generation;Event(e,NA_EV_LIST_CLOSE);}
    void Op(na_ev_list_op e,const void* data=nullptr,size_t n=0,uint32_t stride=0) {
        e.list=5;e.recording_generation=generation;e.op_index=++op;
        if(n) {e.payload_offset=sizeof(e);e.payload_bytes=static_cast<uint32_t>(n);e.payload_stride=stride;}
        Event(e,NA_EV_LIST_OP,data,n);
    }
    void Draw() {
        na_ev_list_op e{};e.op=NA_OP_SET_PIPELINE;e.a[0]=4;Op(e);
        e={};e.op=NA_OP_RS_VIEWPORT;e.a[0]=1;float viewport[]={0,0,1920,1080,0,1};Op(e,viewport,sizeof(viewport),sizeof(viewport));
        e={};e.op=NA_OP_OM_SET_RENDER_TARGETS;e.a[1]=1;
        na_view_ref view{};view.resource=3;view.descriptor_kind=NA_DESC_DSV;view.format=NA_UNKNOWN_U32;Op(e,&view,sizeof(view),sizeof(view));
        e={};e.op=NA_OP_DRAW;e.a[0]=3;e.a[1]=1;Op(e);
    }
    void Execute(uint64_t ticket=17,uint64_t gen=1,uint32_t flags=0) {
        na_ev_queue_execute e{};e.queue=6;e.queue_ticket=ticket;e.list_count=1;e.refs_offset=sizeof(e);e.ref_stride=sizeof(na_list_ref);e.hdr.flags=flags;
        na_list_ref ref{5,gen};Event(e,NA_EV_QUEUE_EXECUTE,&ref,sizeof(ref));
    }
};
int main() {return test::run({
    {"wire execution creates an acquisition candidate with exact observed identities", [] {
        Feed f;f.Metadata();f.Reset();f.Draw();f.Close();f.Execute();
        const auto* v=f.reducer.Match(6,17,3,0);CHECK(v);
        CHECK(v->window.scope.list==5 && v->window.scope.recording==1 && v->observation.last_write==4);
        CHECK(v->window.hud_begin==0);
        CHECK(na::classifier::Describe(na::classifier::Signal::Depth,v->window,v->observation).confidence==na::classifier::Confidence::Candidate);
        CHECK(!f.reducer.Match(7,17,3,0) && !f.reducer.Match(6,18,3,0));
    }},
    {"delayed execute and generation reuse retain the executing recording", [] {
        Feed f;f.Metadata();f.Reset();f.Draw();f.Close();
        f.generation=2;f.Reset();f.Execute(17,1);CHECK(f.reducer.Match(6,17,3,0));
        f.Execute(18,2);CHECK(!f.reducer.Match(6,18,3,0));
        f.Draw();f.Close();CHECK(f.reducer.Match(6,18,3,0));
    }},
    {"missing operations resets pipeline or ambiguous execution cannot rank", [] {
        for(int mode=0;mode<5;++mode) {
            Feed f;f.Metadata();if(mode!=0) f.Reset();
            if(mode==1) ++f.op;
            if(mode==2) {na_ev_object_retired e{};e.object=4;f.Event(e,NA_EV_OBJECT_RETIRED);}
            f.Draw();if(mode!=3) f.Close();f.Execute(17,1,mode==4?NA_EVF_ORDER_AMBIGUOUS:0);
            CHECK(!f.reducer.Match(6,17,3,0));
        }
    }},
    {"copy overwrite dispatch and aliasing revoke render versions", [] {
        for(int mode=0;mode<3;++mode) {
            Feed f;f.Metadata();f.Reset();f.Draw();na_ev_list_op e{};
            if(mode==0) {e.op=NA_OP_COPY_RESOURCE;e.a[0]=3;e.a[1]=9;f.Op(e);}
            if(mode==1) {e.op=NA_OP_DISPATCH;f.Op(e);}
            if(mode==2) {e.op=NA_OP_RESOURCE_BARRIER;e.a[0]=1;na_barrier b{};b.barrier_type=1;b.resource=3;b.resource_after=9;f.Op(e,&b,sizeof(b),sizeof(b));}
            f.Close();f.Execute();CHECK(!f.reducer.Match(6,17,3,0));
        }
    }},
    {"actual loss revokes retained matches and a fresh complete recording reacquires", [] {
        Feed f;f.Metadata();f.Reset();f.Draw();f.Close();f.Execute();CHECK(f.reducer.Match(6,17,3,0));
        ++f.status.detail_dropped;f.Execute(18);CHECK(!f.reducer.Match(6,17,3,0));CHECK(!f.reducer.Match(6,18,3,0));
        ++f.generation;f.Reset();f.Draw();f.Close();f.Execute(19,2);CHECK(f.reducer.Match(6,19,3,0));
        ++f.seq;f.Execute(20,2);CHECK(!f.reducer.Match(6,19,3,0));CHECK(!f.reducer.Match(6,20,3,0));
    }},
    {"retired resource cannot match historical samples", [] {
        Feed f;f.Metadata();f.Reset();f.Draw();f.Close();f.Execute();na_ev_object_retired e{};e.object=3;f.Event(e,NA_EV_OBJECT_RETIRED);
        CHECK(!f.reducer.Match(6,17,3,0) && f.reducer.Latest().empty());
    }}
});}
