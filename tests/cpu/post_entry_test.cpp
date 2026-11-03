#include "host_dx12/j2/post_entry.hpp"
#include "host_dx12/j2/consumer_profile.hpp"
#include <cstdio>
using namespace na::j2;
namespace {
int failures=0;
void check(bool ok,const char* why){if(!ok){std::printf("FAIL %s\n",why);++failures;}}
ColorEvent write(uint64_t pso){ColorEvent e;e.write=true;e.op=Op::DrawWrite;e.raw={1,1,pso,true,Op::DrawWrite};return e;}
ColorEvent entry(uint64_t pso,Op op=Op::GraphicsEntry){ColorEvent e;e.op=op;e.raw={1,2,pso,false,op,7};e.raw.dst_w=3440;e.raw.dst_h=1440;e.raw.dst_fmt=10;return e;}
}
int main(){
    PostEntryProfile p;
    const auto mid=entry(10),post=entry(20);
    for(uint64_t f=1;f<=3;++f){
        // Two recordings, submitted in different Execute calls but in one frame.
        // Final geometry changes every frame. Hidden descriptors teach entries too.
        auto last=write(100+f);last.list_start=true;
        p.Observe({write(100),mid,last,post},true,f);
    }
    check(!p.Accepts(mid.raw) && p.Accepts(post.raw),"only terminal entry learned, independent of last mesh");
    check(p.Accepts(entry(20,Op::IndirectGraphicsEntry).raw),"direct/indirect use the same graphics entry");
    check(!p.Accepts(entry(20,Op::ComputeEntry).raw),"compute and graphics roots are separate");
    auto other=post.raw;++other.root;check(!p.Accepts(other),"root is part of identity");
    other=post.raw;++other.destination;check(!p.Accepts(other),"target is part of identity");
    other=post.raw;++other.dst_w;check(!p.Accepts(other),"resize requires relearning");
    p.Observe({write(100),post,write(101)},true,4);
    check(!p.Accepts(post.raw),"later scene write demotes even after qualification");
    for(uint64_t f=5;f<20;++f)p.Observe({write(f),post},true,f);
    check(!p.Accepts(post.raw),"ambiguous entry does not oscillate back to accepted");
    p.Clear();check(!p.Ready() && p.Size()==0,"selection reset forgets prior image");
    for(uint64_t f=1;f<=10;++f)p.Observe({write(f),post},false,f);
    check(!p.Ready(),"incomplete/truncated frame cannot train");
    p.Observe({write(1),post,post,post},true,1);
    check(!p.Ready(),"repeated entry within one frame counts once");
    p.Observe({post},true,2);check(!p.Ready(),"no write cannot train");
    p.Observe({write(2),post},true,2);p.Observe({write(3),post},true,3);
    check(p.Ready(),"three completed frames qualify");
    // Entry evidence must never become a synthetic colour read in the old route.
    InsertionTrace t;t.Add(write(1).raw);t.Add({1,0,UINT64_MAX,false,Op::Barrier});
    const auto before=t.Analyze(1);t.Add(post.raw);const auto after=t.Analyze(1);
    check(before.candidates==after.candidates && before.all_reads==after.all_reads && before.final_transition==after.final_transition,
          "legacy candidate counts unchanged");
    std::vector<Labelled> labels;LabelFrame({write(1),post},3440,1440,labels);
    check(labels.empty(),"unresolved entry is not a proven consumer label");
    PostEntryProfile bounded;
    for(uint64_t f=1;f<600;++f)bounded.Observe({write(f),entry(f)},true,f);
    check(bounded.Size()==512,"profile storage bounded");
    std::printf("post entry: %d failures\n",failures);return failures?1:0;
}
