#include "test_support.hpp"
#include "providers/classifier/production_checks.hpp"
using namespace na::classifier;
namespace motion_discovery=na::motion_discovery;
struct ColorCase { Window window;Observation observation;ColorFrame source;ColorBoundary boundary; };
static ColorCase color_case() {
    const Scope scope{1,2,3,4,5,6};ColorCase c;
    c.window={scope,16,8,100,120,true,true,true};
    c.observation={scope,7,8,0,16,8,Format::ColorFloat,true,true,false,true,1,100,false,0};
    c.source.identity={scope,7,8,0,16,8,true,true,true};
    for(unsigned y=0;y<8;++y) for(unsigned x=0;x<16;++x) c.source.color.push_back({double(x)/16,double(y)/8,0.2});
    c.boundary.consumed_source=c.source.identity;c.boundary.final_color=c.source;
    c.boundary.final_color.identity.resource=9;
    c.boundary.composition_op=110;c.boundary.first_overlay_op=120;c.boundary.last_overlay_op=121;
    c.boundary.complete=c.boundary.source_preserved=c.boundary.presentation_linked=c.boundary.no_other_writes=true;
    c.boundary.overlays.push_back({0,0,4,2});
    for(unsigned y=0;y<2;++y) for(unsigned x=0;x<4;++x) c.boundary.final_color.color[y*16+x]={1,1,1};
    return c;
}
static DepthFrame depth(uint64_t frame) {
    DepthFrame d;d.camera={1,2,4,frame,frame,{{1,0,0,frame==11?-0.0625:0,0,1,0,0,0,0,1,0,0,0,0,1}},0,0,1,true,true,false,true};
    d.resource=7;d.width=32;d.height=16;d.depth.assign(512,0.4);return d;
}
int main() { return test::run({
    {"composition corroborates preserved source and catches wrong contents", [] {
        auto c=color_case();auto result=ConfirmPreHudComposite(c.window,c.observation,c.source,c.boundary);
        CHECK(result.result==Confirmation::Supported && result.checked==120);
        for(size_t i=16*4;i<16*5;++i) c.source.color[i]={0,0,0};
        CHECK(ConfirmPreHudComposite(c.window,c.observation,c.source,c.boundary).result==Confirmation::Contradicted);
    }},
    {"overwritten stale unlinked unbounded and ambiguous sources cannot gain support", [] {
        for(unsigned mode=0;mode<7;++mode) {
            auto c=color_case();
            if(mode==0) c.boundary.source_preserved=false;
            if(mode==1) ++c.boundary.consumed_source.version;
            if(mode==2) c.boundary.presentation_linked=false;
            if(mode==3) c.boundary.no_other_writes=false;
            if(mode==4) c.boundary.overlays={{0,0,16,8}};
            if(mode==5) c.boundary.final_color.identity.resource=c.source.identity.resource;
            if(mode==6) c.boundary.overlays={{0,0,17,8}};
            CHECK(ConfirmPreHudComposite(c.window,c.observation,c.source,c.boundary).result==Confirmation::Unavailable);
        }
    }},
    {"production motion derives visibility from depth without an oracle mask", [] {
        const auto p=depth(10),c=depth(11);
        motion_discovery::Frame a{1,2,10,32,16,true,true,false,0,0,{},{}},b=a;
        b.frame=11;
        for(unsigned y=0;y<16;++y) for(unsigned x=0;x<32;++x) {
            const double v=double((x*37+y*61+x*y*13)%101)/100;
            a.color.push_back({v,1-v,v*v});
        }
        for(unsigned y=0;y<16;++y) for(unsigned x=0;x<32;++x) b.color.push_back(a.color[y*32+std::min(x+1,31u)]);
        motion_discovery::Motion m{1,2,11,12,13,true,true,{}};m.values.assign(512,{1,0});
        CHECK(a.stable.empty() && b.stable.empty());
        const auto r=ConfirmMotion(p,c,a,b,m,{});
        CHECK(r.evidence.result==Confirmation::Supported && r.evidence.coverage>0.9);
        CHECK(ConfirmMotion(p,c,a,b,m,{-1,1,false}).evidence.result==Confirmation::Contradicted);
        // About 15% moving geometry must not veto motion on the remaining
        // independently corroborated static surfaces.
        auto moving=c;
        for(unsigned y=0;y<16;++y) for(unsigned x=0;x<5;++x) moving.depth[y*32+x]=0.6;
        CHECK(ConfirmMotion(p,moving,a,b,m,{}).evidence.result==Confirmation::Supported);
        ++b.frame;CHECK(ConfirmMotion(p,c,a,b,m,{}).evidence.result==Confirmation::Unavailable);
    }}
}); }
