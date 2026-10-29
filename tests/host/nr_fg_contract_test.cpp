#include "test_support.hpp"
#include "fake_ngx_params.hpp"
#include "host_dx12/nr/fg_contract.hpp"
int main() {
    test::FakeParams p;
    p.Set("Width",999u); p.Set("MV.Scale.X",42.f);
    auto f=na::nr::ReadFgContract(&p);
    CHECK(!f.has_frame_id && !f.has_motion_x && !f.color.region.fields);
    CHECK(p.non_fg_reads==0);
    p.Set("DLSSG.BackbufferFrameID",0ull);
    p.Set("DLSSG.MvecScaleX",-3440.f); p.Set("DLSSG.MvecScaleY",1440.f);
    p.Set("DLSSG.BackbufferSubrectWidth",1920u);
    f=na::nr::ReadFgContract(&p);
    CHECK(f.has_frame_id && f.frame_id==0 && f.motion_x==-3440.f);
    CHECK(f.color.region.width==1920 && f.color.region.fields==4);
    p.Set("DLSSG.InputBackbufferSubrectBaseX",64u);
    p.Set("DLSSG.HUDLessSubrectWidth",3440u);
    p.Set("DLSSG.MVecsSubrectWidth",1146u);
    p.Set("DLSSG.MultiFrameCount",3u); p.Set("DLSSG.MultiFrameIndex",2u);
    f=na::nr::ReadFgContract(&p);
    CHECK(f.color.region.x==64 && f.color.region.width==0 && f.color.region.fields==1);
    CHECK(f.hudless.region.width==3440 && f.motion.region.width==1146);
    CHECK(f.has_multi_index && f.multi_index==2 && f.multi_count==3);
    CHECK(p.non_fg_reads==0);
    CHECK(!na::nr::ReadFgContract(nullptr).has_frame_id);
    return test::finish("nr_fg_contract_test");
}
