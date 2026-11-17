#pragma once
// Test-only full GPU search and total-motion audit. Readbacks/waits and candidate experiments
// here never enter a live callback. Ground truth grades results, never selects a candidate.
#include "flow_fixture_pictures.hpp"
#include "core/total_motion.hpp"
#include <map>
#include <utility>
#include <algorithm>

inline int AuditFlowAmbiguity(test::Device& d,unsigned w,unsigned h)
{
    namespace cf=na::j2::compute_flow;namespace tm=na::motion::total;
    constexpr auto S=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    test::List list;REQUIRE(list.Create(d.device.Get()));
    const auto execute=[&]{REQUIRE(SUCCEEDED(list.list->Close()));ID3D12CommandList* ls[]={list.list.Get()};
        {na::nr::native::Submission track(d.queue.Get(),1,ls);d.queue->ExecuteCommandLists(1,ls);}d.WaitIdle();return 0;};
    const auto reset=[&]{na::nr::native::Reset(list.list.Get());na::nr::native::Maintain();list.Reset();};
    const auto copy=[&](ID3D12Resource* src,ID3D12Resource* dst,DXGI_FORMAT fmt,unsigned width,unsigned height,unsigned row){
        auto bar=test::Transition(src,S,D3D12_RESOURCE_STATE_COPY_SOURCE);list.list->ResourceBarrier(1,&bar);
        D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=src;a.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        b.pResource=dst;b.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;b.PlacedFootprint.Footprint={fmt,width,height,1,row};
        list.list->CopyTextureRegion(&b,0,0,0,&a,nullptr);std::swap(bar.Transition.StateBefore,bar.Transition.StateAfter);list.list->ResourceBarrier(1,&bar);
    };
    struct Case{const char* name;float pan;bool noise,mixed;};
    for(const auto c:{Case{"waves-still",0,false,false},Case{"waves-3px",3,false,false},Case{"waves-6px",6,false,false},
                      Case{"waves-reverse6",-6,false,false},Case{"waves-12px",12,false,false},
                      Case{"noise-6px",6,true,false},Case{"mixed-independent",3,true,true}}){
        cf::Reset();std::vector<test::ComPtr<ID3D12Resource>> keep;
        std::vector<float> lum[2];test::ComPtr<ID3D12Resource> pictures[2];
        const float object_pan=-6;
        for(unsigned i=0;i<2;++i){
            lum[i].resize(size_t(w)*h);std::vector<uint32_t> rgba(size_t(w)*h);
            for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
                const bool object=c.mixed && x>w/3 && x<2*w/3 && y>h/3 && y<2*h/3;
                const float dx=i?(object?object_pan:c.pan):0;
                const float z=object?test_pictures::RepeatingWaves(float(x)-dx,float(y)):
                    c.noise?test_pictures::NonRepeating(float(x)-dx,float(y)):test_pictures::RepeatingWaves(float(x)-dx,float(y));
                const unsigned v=unsigned(std::lround(255.f*z));const size_t k=size_t(y)*w+x;
                rgba[k]=0xff000000u|v*0x010101u;lum[i][k]=float(v)/255.f;
            }
            pictures[i]=test::CreateTexture(d.device.Get(),w,h,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE);
            auto up=test::CreateBuffer(d.device.Get(),size_t(w)*h*4,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            void* p=nullptr;REQUIRE(SUCCEEDED(up->Map(0,nullptr,&p)));std::memcpy(p,rgba.data(),rgba.size()*4);up->Unmap(0,nullptr);
            D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=up.Get();a.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            a.PlacedFootprint.Footprint={DXGI_FORMAT_R8G8B8A8_UNORM,w,h,1,w*4};b.pResource=pictures[i].Get();b.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list.list->CopyTextureRegion(&b,0,0,0,&a,nullptr);auto bar=test::Transition(pictures[i].Get(),D3D12_RESOURCE_STATE_COPY_DEST,S);list.list->ResourceBarrier(1,&bar);keep.push_back(up);
        }
        auto depth=test::CreateTexture(d.device.Get(),w,h,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE);
        auto up=test::CreateBuffer(d.device.Get(),size_t(w)*h*4,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        float* z=nullptr;REQUIRE(SUCCEEDED(up->Map(0,nullptr,reinterpret_cast<void**>(&z))));std::fill(z,z+size_t(w)*h,.5f);up->Unmap(0,nullptr);
        D3D12_TEXTURE_COPY_LOCATION da{},db{};da.pResource=up.Get();da.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;da.PlacedFootprint.Footprint={DXGI_FORMAT_R32_FLOAT,w,h,1,w*4};
        db.pResource=depth.Get();db.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list.list->CopyTextureRegion(&db,0,0,0,&da,nullptr);
        auto bar=test::Transition(depth.Get(),D3D12_RESOURCE_STATE_COPY_DEST,S);list.list->ResourceBarrier(1,&bar);keep.push_back(up);REQUIRE(execute()==0);reset();
        na::motion::gpu::Inputs independent{};independent.depth=depth.Get();independent.depth_clear=1;
        const float nan=std::numeric_limits<float>::quiet_NaN();for(unsigned j=0;j<16;++j)independent.inv_view_proj_current.m[j]=independent.view_proj_previous.m[j]=nan;
        auto motion=test::CreateBuffer(d.device.Get(),size_t(w)*h*8,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        auto valid=test::CreateBuffer(d.device.Get(),size_t(w)*h*4,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        for(unsigned i=0;i<2;++i){cf::Frame f;f.frame=6000+i;f.previous=i?6000:0;f.generation=101;f.source_generation=101;f.total=true;f.raster_sample=f.raster_confirmed=true;
            cf::Tier3 t;auto ticket=cf::Record(list.list.Get(),d.queue.Get(),pictures[i].Get(),depth.Get(),independent,f,t);REQUIRE(ticket.context);CHECK(ticket.pair==(i!=0));
            if(i){REQUIRE(t.total && t.total_valid);copy(t.total,motion.Get(),DXGI_FORMAT_R32G32_FLOAT,w,h,w*8);copy(t.total_valid,valid.Get(),DXGI_FORMAT_R32_FLOAT,w,h,w*4);}
            REQUIRE(execute()==0);cf::Committed(ticket);reset();
        }
        // Read the same pair's actual AMD block search separately, without changing production.
        na::ffx_flow::PairFlow search;REQUIRE(search.Init(d.device.Get(),w,h,1));REQUIRE(search.Record(list.list.Get(),pictures[1].Get(),pictures[0].Get(),0));
        const unsigned gw=(w+7)/8,gh=(h+7)/8,row=(gw*4+255u)&~255u;
        auto raw=test::CreateBuffer(d.device.Get(),size_t(row)*gh,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        copy(search.Grid(),raw.Get(),DXGI_FORMAT_R16G16_SINT,gw,gh,row);search.Finish(list.list.Get());REQUIRE(execute()==0);reset();
        void* rp=nullptr;float* mv=nullptr;float* vv=nullptr;REQUIRE(SUCCEEDED(raw->Map(0,nullptr,&rp)));
        REQUIRE(SUCCEEDED(motion->Map(0,nullptr,reinterpret_cast<void**>(&mv))));REQUIRE(SUCCEEDED(valid->Map(0,nullptr,reinterpret_cast<void**>(&vv))));
        std::map<std::pair<int,int>,unsigned> hist;
        for(unsigned y=0;y<gh;++y){auto* v=reinterpret_cast<const int16_t*>(static_cast<const char*>(rp)+size_t(y)*row);
            for(unsigned x=0;x<gw;++x)++hist[{v[x*2],v[x*2+1]}];}
        std::vector<std::pair<unsigned,std::pair<int,int>>> peaks;for(const auto& kv:hist)peaks.push_back({kv.second,kv.first});
        std::sort(peaks.begin(),peaks.end(),[](const auto& a,const auto& b){return a.first>b.first;});
        std::printf("ambiguity %s %ux%u raw peaks:",c.name,w,h);for(size_t i=0;i<(std::min)(size_t(6),peaks.size());++i)std::printf(" (%d,%d)=%u",peaks[i].second.first,peaks[i].second.second,peaks[i].first);std::puts("");
        const tm::Image cur{lum[1].data(),w,h},prev{lum[0].data(),w,h};
        // Compare modal selection with top-4 candidates ranked by spatially distributed image error.
        const auto patch_error=[&](float px,float py,float vx,float vy){float sum=0;
            for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x)sum+=std::fabs(tm::Sample(cur,px+2*x,py+2*y)-tm::Sample(prev,px+2*x+vx,py+2*y+vy));return sum/9;};
        auto scored=peaks.front().second;double best=1e9;
        for(size_t ci=0;ci<(std::min)(size_t(4),peaks.size());++ci){const auto candidate=peaks[ci].second;double e=0;unsigned n=0;
            for(unsigned y=32;y<h-32;y+=37)for(unsigned x=32;x<w-32;x+=43){const float px=float(x)+.5f,py=float(y)+.5f;
                if(px+candidate.first<3 || py+candidate.second<3 || px+candidate.first>w-3 || py+candidate.second>h-3)continue;
                e+=patch_error(px,py,float(candidate.first),float(candidate.second));++n;}
            if(n && e/n<best){best=e/n;scored=candidate;}}
        unsigned n=0,right=0,far_count=0,measured_far=0,approx_far=0,mode_right=0,scored_right=0,multiple_right=0,object_n=0,object_right[4]{};double error=0;
        for(unsigned y=24;y<h-24;y+=4)for(unsigned x=24;x<w-24;x+=4){
            if(c.mixed && (std::abs(int(x)-int(w/3))<12 || std::abs(int(x)-int(2*w/3))<12 || std::abs(int(y)-int(h/3))<12 || std::abs(int(y)-int(2*h/3))<12))continue;
            const bool object=c.mixed && x>w/3 && x<2*w/3 && y>h/3 && y<2*h/3;const float truth=-(object?object_pan:c.pan);
            const size_t k=size_t(y)*w+x;const double e=std::hypot(mv[k*2]-truth,mv[k*2+1]);++n;error+=e;right+=e<1;
            far_count+=e>64;if(e>64){measured_far+=vv[k]>=.99f;approx_far+=vv[k]>.1f && vv[k]<.99f;}
            if(object){++object_n;object_right[0]+=e<1;}
            for(unsigned kind=0;kind<3;++kind){
                float vx=mv[k*2],vy=mv[k*2+1];
                const size_t alternatives=kind==2?(std::min)(size_t(4),peaks.size()):1;
                for(size_t ai=0;ai<alternatives;++ai){const auto candidate=kind==2?peaks[ai].second:kind==1?scored:peaks.front().second;
                auto r=tm::MeasurePixel(cur,prev,float(x)+.5f,float(y)+.5f,true,float(candidate.first),float(candidate.second));
                // Global is only a competing local hypothesis: never overwrite an equally good
                // local/object match; require an informative measurement and clearly lower error.
                if(r.valid==tm::kMeasured && patch_error(float(x)+.5f,float(y)+.5f,r.x,r.y)<.8f*patch_error(float(x)+.5f,float(y)+.5f,vx,vy)){vx=r.x;vy=r.y;}}
                const bool good=std::hypot(vx-truth,vy)<1;if(kind==2)multiple_right+=good;else if(kind==1)scored_right+=good;else mode_right+=good;if(object)object_right[kind+1]+=good;
            }
        }
        std::printf("ambiguity %s %ux%u GPU: right=%.2f%% far=%.2f%% measured_far=%.2f%% approximate_far=%.2f%% mean_error=%.3f; CPU candidate experiment mode=(%d,%d) right=%.2f%% scored=(%d,%d) right=%.2f%% top4-local=%.2f%%; object=%u right %.2f/%.2f/%.2f/%.2f%%\n",
            c.name,w,h,100.*right/n,100.*far_count/n,100.*measured_far/n,100.*approx_far/n,error/n,peaks.front().second.first,peaks.front().second.second,100.*mode_right/n,scored.first,scored.second,100.*scored_right/n,100.*multiple_right/n,object_n,
            object_n?100.*object_right[0]/object_n:0,object_n?100.*object_right[1]/object_n:0,object_n?100.*object_right[2]/object_n:0,object_n?100.*object_right[3]/object_n:0);
        // Grade actual GPU output, never the CPU candidate experiment. These assertions expose
        // the current repeating-picture defect and must not be relaxed to call it corrected.
        CHECK(double(right)/n>.95);CHECK(double(far_count)/n<.01);CHECK(double(measured_far)/n<.01);
        if(object_n)CHECK(double(object_right[0])/object_n>.95);
        raw->Unmap(0,nullptr);motion->Unmap(0,nullptr);valid->Unmap(0,nullptr);cf::Reset();
    }
    test::ComPtr<ID3D12InfoQueue> info;d.device.As(&info);
    if(info){unsigned errors=0;for(UINT64 i=0;i<info->GetNumStoredMessages();++i){
        SIZE_T bytes=0;info->GetMessage(i,nullptr,&bytes);std::vector<uint8_t> data(bytes);auto* message=reinterpret_cast<D3D12_MESSAGE*>(data.data());
        if(SUCCEEDED(info->GetMessage(i,message,&bytes)) && message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){
            std::printf("D3D12 ambiguity: %s\n",message->pDescription);++errors;}}
        std::printf("D3D12 ambiguity %ux%u errors=%u\n",w,h,errors);CHECK(errors==0);info->ClearStoredMessages();}
    return 0;
}
