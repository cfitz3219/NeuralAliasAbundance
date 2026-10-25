// reconstructed 2026-10: no oracle world positions or supplied visibility mask.
#include "test_support.hpp"
#include "providers/classifier/temporal_depth.hpp"
using namespace na::classifier;
static DepthFrame frame(uint64_t id=10) {
    DepthFrame f{}; f.camera={1,2,3,id,id,{{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}},0,0,1,true,true,false};
    f.resource=4; f.width=16; f.height=8; f.depth.assign(128,0.4); return f;
}
int main() { return test::run({
    {"translated camera corroborates independent depth versions and estimates overlap", [] {
        const auto p=frame(); auto c=frame(11); c.camera.view_projection[3]=-0.125;
        const auto r=CheckTemporalDepth(p,c);
        CHECK(r.verdict==TemporalVerdict::Consistent && r.checked==120 && r.excluded==8);
        CHECK(r.inliers==120 && r.current_agreement[15]==0 && r.current_agreement[0]==1);
        c.depth.assign(128,0.6); CHECK(CheckTemporalDepth(p,c).verdict==TemporalVerdict::Contradicted);
    }},
    {"stationary or jitter only pairs cannot establish depth semantics", [] {
        const auto p=frame(); auto c=frame(11);
        CHECK(CheckTemporalDepth(p,c).verdict==TemporalVerdict::Uninformative);
        c.camera.jitter_x=0.25;
        CHECK(CheckTemporalDepth(p,c).verdict==TemporalVerdict::Uninformative);
    }},
    {"reverse Z and camera depth translation use explicit projection convention", [] {
        auto p=frame(),c=frame(11); p.camera.view_projection[10]=-1; p.camera.view_projection[11]=1;
        c.camera.view_projection=p.camera.view_projection; c.camera.view_projection[3]=-0.125;
        c.camera.view_projection[11]+=0.05; c.depth.assign(128,0.45);
        p.camera.clear_depth=c.camera.clear_depth=0;
        CHECK(CheckTemporalDepth(p,c).verdict==TemporalVerdict::Consistent);
    }},
    {"reset epochs nonadjacent frames queue mismatch and invalid cameras reject", [] {
        const auto p=frame();
        for(int mode=0;mode<7;++mode) {
            auto c=frame(11); c.camera.view_projection[3]=-0.125;
            if(mode==0) c.camera.reset=true;
            if(mode==1) ++c.camera.epoch;
            if(mode==2) ++c.camera.frame;
            if(mode==3) ++c.camera.queue;
            if(mode==4) c.camera.qualified=false;
            if(mode==5) c.camera.view_projection={};
            if(mode==6) c.depth[0]=std::nan("");
            CHECK(CheckTemporalDepth(p,c).verdict==TemporalVerdict::Unavailable);
        }
    }},
    {"all sky and missing overlap remain acquisition work", [] {
        auto p=frame(),c=frame(11); c.depth.assign(128,1);
        CHECK(CheckTemporalDepth(p,c).verdict==TemporalVerdict::Unavailable);
        c=frame(11); c.camera.view_projection[3]=-1;
        CHECK(CheckTemporalDepth(p,c).verdict==TemporalVerdict::Unavailable);
    }},
    {"camera association must name actual sampled queue ticket", [] {
        na::discovery::AcquiredSample s{}; s.status=na::discovery::AcquisitionStatus::Ready;
        s.info.session=1;s.info.epoch=2;s.info.queue=3;s.info.queue_ticket=10;s.info.resource=4;
        s.info.width=16;s.info.height=8;s.info.row_pitch=64;s.info.format=40;
        s.info.status=NA_SAMPLE_READY;s.info.data_bytes=512;
        s.bytes.resize(512); const float value=0.4f;
        for(size_t i=0;i<128;++i) std::memcpy(s.bytes.data()+i*4,&value,4);
        auto camera=frame().camera; DepthFrame out;
        CHECK(AcquireDepthFrame(s,camera,out)); CHECK(out.depth.size()==128);
        ++camera.ticket; CHECK(!AcquireDepthFrame(s,camera,out) && out.depth.empty());
    }}
}); }
