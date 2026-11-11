#include "test_support.hpp"
#include "host_dx12/j2/compute_flow.hpp"
#include "host_dx12/ffx_flow/ffx_flow_shaders.hpp"
#include "host_dx12/ffx_flow/ffx_flow.hpp"
#include "host_dx12/nr/native_fg.hpp"
#include "core/motion_arbiter_gpu.hpp"
#include <vector>
#include <cmath>
int main(int argc,char**){
    if(!na::ffx_flow::Built())return test::kSkip;
    test::Device d;if(!test::CreateDevice(d))return test::kSkip;
    if(!na::ffx_flow::DeviceSupported(d.device.Get())){std::puts("SKIP: AMD search needs 32/64-lane hardware waves");return test::kSkip;}
    const unsigned w=argc>1?1920:512,h=argc>1?1080:256;constexpr auto S=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,U=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    test::List l;REQUIRE(l.Create(d.device.Get()));std::vector<test::ComPtr<ID3D12Resource>> uploads;
    auto image=[&](int dx,bool depth){
        auto fmt=depth?DXGI_FORMAT_R32_FLOAT:DXGI_FORMAT_R8G8B8A8_UNORM;
        auto r=test::CreateTexture(d.device.Get(),w,h,fmt,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE);
        auto up=test::CreateBuffer(d.device.Get(),w*h*4,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);unsigned* p=nullptr;up->Map(0,nullptr,reinterpret_cast<void**>(&p));
        for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){unsigned v=25;
            const int a=static_cast<int>(x)-dx;
            if(a>=128 && a<256 && y>=64 && y<192){unsigned t=unsigned(a)*1664525u+y*1013904223u;t^=t>>13;t*=2246822519u;t^=t>>16;v=60+(t%190);}
            p[y*w+x]=depth?0x3f000000u:0xff000000u|v*0x010101u;
        }up->Unmap(0,nullptr);D3D12_TEXTURE_COPY_LOCATION src{},dst{};
        src.pResource=up.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint.Footprint={fmt,w,h,1,w*4};
        dst.pResource=r.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;l.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        auto tr=test::Transition(r.Get(),D3D12_RESOURCE_STATE_COPY_DEST,S);l.list->ResourceBarrier(1,&tr);uploads.push_back(up);return r;
    };
    auto before=image(0,false),after=image(8,false),later=image(16,false),depth=image(0,true);
    auto execute=[&](){l.list->Close();ID3D12CommandList* lists[]={l.list.Get()};{na::nr::native::Submission track(d.queue.Get(),1,lists);d.queue->ExecuteCommandLists(1,lists);}d.WaitIdle();};
    auto reset=[&](){na::nr::native::Reset(l.list.Get());na::nr::native::Maintain();l.Reset();};execute();reset();
    na::motion::gpu::Inputs camera;camera.depth=depth.Get();camera.tier2_valid_flag=true;
    for(int j=0;j<16;++j)camera.inv_view_proj_current.m[j]=camera.view_proj_previous.m[j]=(j%5==0)?1.f:0.f;
    namespace cf=na::j2::compute_flow;
    // The original gates (flow_loosen 0) are what this case pins down; the looser levels below.
    {const auto g0=cf::GatesFor(0),g50=cf::GatesFor(50),g100=cf::GatesFor(100);
     CHECK(g0.agree_min==1 && !g0.sky && g0.image_ratio==.5f && g0.texture_min==.03f && g0.consistency_px==2.f);
     CHECK(g50.agree_min==0 && g50.sky && g50.image_ratio>g0.image_ratio && g50.texture_min<g0.texture_min);
     CHECK(g100.image_ratio==1.f && g100.agree_px==6.f && g100.consistency_px==4.f && cf::GatesFor(500).image_ratio==1.f);}
    cf::SetLoosen(0);
    cf::Frame f;f.frame=1;f.generation=1;cf::Tier3 tier;
    auto seed=cf::Record(l.list.Get(),d.queue.Get(),before.Get(),depth.Get(),camera,f,tier);REQUIRE(seed.context);CHECK(!tier.residual);execute();cf::Committed(seed);
    // Completion without Reset is insufficient: the closed list can be replayed.
    auto busy=cf::Record(l.list.Get(),d.queue.Get(),after.Get(),depth.Get(),camera,f,tier);CHECK(!busy.context);reset();
    f.frame=2;f.previous=1;
    // First pair: every moving pixel passes the image checks but has no agreeing previous pair yet
    // (temporal agreement, j2_flow 5c5fb0d), so it is held back as flicker; the second pair accepts.
    auto first=cf::Record(l.list.Get(),d.queue.Get(),after.Get(),depth.Get(),camera,f,tier);REQUIRE(first.context && tier.residual);
    execute();cf::Committed(first);reset();
    {auto s1=cf::GetStats();std::printf("first pair: accepted=%llu flicker=%llu total=%llu\n",(unsigned long long)s1.accepted,(unsigned long long)s1.rejected_flicker,(unsigned long long)s1.total);
     CHECK(s1.accepted==0);CHECK(s1.rejected_flicker>0);}
    f.frame=3;f.previous=2;
    auto pair=cf::Record(l.list.Get(),d.queue.Get(),later.Get(),depth.Get(),camera,f,tier);REQUIRE(pair.context && tier.residual);
    auto output=test::CreateTexture(d.device.Get(),w,h,DXGI_FORMAT_R32G32_FLOAT,U,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto prov=test::CreateTexture(d.device.Get(),w,h,DXGI_FORMAT_R8_UINT,U,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    na::motion::gpu::Arbiter arbiter;REQUIRE(arbiter.Init(d.device.Get()));camera.tier3_valid_flag=true;camera.tier3_residual=tier.residual;camera.tier3_valid=tier.valid;camera.tier3_mask=tier.mask;
    na::motion::gpu::Outputs out;out.xy=output.Get();out.provenance=prov.Get();out.width=w;out.height=h;REQUIRE(arbiter.Dispatch(l.list.Get(),camera,out,0));
    auto tr=test::Transition(output.Get(),U,D3D12_RESOURCE_STATE_COPY_SOURCE);l.list->ResourceBarrier(1,&tr);
    auto rb=test::CreateBuffer(d.device.Get(),w*h*8,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=output.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource=rb.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint.Footprint={DXGI_FORMAT_R32G32_FLOAT,w,h,1,w*8};l.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    execute();cf::Committed(pair);reset();
    float* p=nullptr;rb->Map(0,nullptr,reinterpret_cast<void**>(&p));unsigned good=0,total=0,bg_wrong=0;
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){const float vx=p[(y*w+x)*2],vy=p[(y*w+x)*2+1];
        if(x>=160 && x<256 && y>=80 && y<176){++total;if(std::abs(vx+8)<1 && std::abs(vy)<1)++good;}
        if((x<110 || x>290 || y<40 || y>216) && (std::abs(vx)>.01f || std::abs(vy)>.01f))++bg_wrong;
    }rb->Unmap(0,nullptr);std::printf("AMD->expand->residual->arbiter: object %u/%u correct, background errors %u\n",good,total,bg_wrong);CHECK(good>total*8/10);CHECK(bg_wrong==0);
    auto stats=cf::GetStats();std::printf("pairs=%llu accepted=%llu total=%llu GPU=%.3fms %s\n",(unsigned long long)stats.pairs,(unsigned long long)stats.accepted,(unsigned long long)stats.total,stats.ms,stats.reason);CHECK(stats.pairs==2);CHECK(stats.accepted>0);
    // Reset, skipped history and a new generation cannot use a stale pair.
    f.frame=30;f.previous=99;f.generation=2;auto gap=cf::Record(l.list.Get(),d.queue.Get(),after.Get(),depth.Get(),camera,f,tier);CHECK(gap.context && !tier.residual);execute();cf::Committed(gap);reset();
    f.frame=4;f.previous=30;f.reset=true;auto cut=cf::Record(l.list.Get(),d.queue.Get(),after.Get(),depth.Get(),camera,f,tier);CHECK(cut.context && !tier.residual);execute();cf::Committed(cut);reset();
    f.frame=5;f.previous=4;f.reset=false;auto abandoned=cf::Record(l.list.Get(),d.queue.Get(),after.Get(),depth.Get(),camera,f,tier);CHECK(abandoned.context && tier.residual);
    l.list->Close();reset(); // deliberately discard, never publish frame5 as history
    f.frame=6;f.previous=5;auto next=cf::Record(l.list.Get(),d.queue.Get(),after.Get(),depth.Get(),camera,f,tier);CHECK(next.context && !tier.residual);execute();cf::Committed(next);reset();
    cf::Reset();CHECK(d.device->GetDeviceRemovedReason()==S_OK);execute();return test::finish("j2_compute_flow_test");
}
