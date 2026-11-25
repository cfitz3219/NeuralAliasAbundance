#include "test_support.hpp"
#include "host_dx12/j2/compute_flow.hpp"
#include "host_dx12/ffx_flow/ffx_flow_shaders.hpp"
#include "host_dx12/ffx_flow/ffx_flow.hpp"
#include "host_dx12/nr/native_fg.hpp"
#include "core/motion_arbiter_gpu.hpp"
#include "core/jitter_domain.hpp"
#include "core/camera_fit_gpu.hpp"
#include <vector>
#include <cmath>
#include <limits>
#include <cstring>
#include <d3d12sdklayers.h>
#include "total_motion_gpu_audit.hpp"
#include "flow_ambiguity_gpu_audit.hpp"
#include "flow_fixture_pictures.hpp"
#include "screen_motion_gpu_audit.hpp"
#include "fold_planes_gpu.hpp"
int main(int argc,char** argv){
    if(!na::ffx_flow::Built())return test::kSkip;
    const bool debug_mode=(argc>1 && !std::strcmp(argv[1],"--debug")) || (argc>2 && !std::strcmp(argv[2],"--debug"));
    test::ComPtr<ID3D12Debug> debug;
    if(debug_mode){REQUIRE(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))));debug->EnableDebugLayer();}
    test::Device d;if(!test::CreateDevice(d))return test::kSkip;
    if(argc>1 && !std::strcmp(argv[1],"--fold-planes")) {
        REQUIRE(AuditFoldPlanes(d)==0);
        return test::finish("j2_fold_planes_gpu");
    }
    if(argc>1 && !std::strcmp(argv[1],"--screen-fit")) {
        REQUIRE(screen_audit::ArbiterFallback(d)==0);
        REQUIRE(screen_audit::SolveControlled(d)==0);
        REQUIRE(screen_audit::SampleFitMeasure(d)==0);
        if(debug_mode) {
            test::ComPtr<ID3D12InfoQueue> messages;REQUIRE(SUCCEEDED(d.device.As(&messages)));
            unsigned errors=0;
            for(UINT64 i=0;i<messages->GetNumStoredMessages();++i) {
                SIZE_T size=0;messages->GetMessage(i,nullptr,&size);std::vector<char> bytes(size);
                auto* m=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());messages->GetMessage(i,m,&size);
                if(m->Severity==D3D12_MESSAGE_SEVERITY_ERROR||m->Severity==D3D12_MESSAGE_SEVERITY_CORRUPTION){std::printf("GPU validation: %s\n",m->pDescription);++errors;}
            }
            std::printf("screen fit debug errors=%u\n",errors);CHECK(errors==0);
        }
        return test::finish("j2_screen_motion_gpu_audit");
    }
    if(argc>1 && !std::strcmp(argv[1],"--overhead-benchmark")) {
        REQUIRE(screen_audit::ProductionBenchmark(d,3440,1440,true)==0);
        return test::finish("j2_overhead_benchmark");
    }
    if(argc>1 && !std::strcmp(argv[1],"--fold-motion-benchmark")) {
        // First two runs match the live motion workload. The last two measure the
        // cost if the motion pipeline itself were given a truly smaller scene.
        for(unsigned block_y : {1u, 2u}) {
            std::printf("Folded input, motion at display size, block=2x%u\n", block_y);
            REQUIRE(screen_audit::ProductionBenchmark(d,3440,1440,true,2,block_y)==0);
        }
        for(unsigned height : {1440u, 720u}) {
            std::printf("Motion at input size, 1720x%u\n", height);
            REQUIRE(screen_audit::ProductionBenchmark(d,1720,height,true)==0);
        }
        return test::finish("j2_fold_motion_benchmark");
    }
    if(argc>1 && !std::strcmp(argv[1],"--screen-benchmark")) {
        REQUIRE(screen_audit::ProductionBenchmark(d,1920,1080)==0);
        REQUIRE(screen_audit::ProductionBenchmark(d,3440,1440)==0);
        return test::finish("j2_screen_motion_benchmark");
    }
    if(argc>1 && !std::strcmp(argv[1],"--ambiguity")){
        if(!na::ffx_flow::DeviceSupported(d.device.Get()))return test::kSkip;
        REQUIRE(AuditFlowAmbiguity(d,512,256)==0);REQUIRE(AuditFlowAmbiguity(d,1920,1080)==0);
        return test::finish("j2_compute_flow_ambiguity");
    }
    if(argc>1 && !std::strcmp(argv[1],"--candidate-benchmark")){
        if(!na::ffx_flow::DeviceSupported(d.device.Get()))return test::kSkip;
        REQUIRE(AuditFlowAmbiguity(d,1920,1080,true)==0);
        return test::finish("j2_compute_flow_candidate_benchmark");
    }
    test::ComPtr<ID3D12InfoQueue> info;if(debug_mode)REQUIRE(SUCCEEDED(d.device.As(&info)));
    REQUIRE(AuditTotalMotionShader(d)==0);
    if(!na::ffx_flow::DeviceSupported(d.device.Get())){std::puts("SKIP: AMD search needs 32/64-lane hardware waves");return test::kSkip;}
    const bool large=argc>1 && !debug_mode;
    const unsigned w=large?1920:512,h=large?1080:256;constexpr auto S=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,U=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    test::List l;REQUIRE(l.Create(d.device.Get()));std::vector<test::ComPtr<ID3D12Resource>> uploads;
    // world: 0 the flat picture with one textured box, 1 the four-sine picture, 2 the non-repeating one (flow_fixture_pictures.hpp).
    auto image=[&](int dx,bool depth,float jx=0.f,float jy=0.f,int world=0){
        auto fmt=depth?DXGI_FORMAT_R32_FLOAT:DXGI_FORMAT_R8G8B8A8_UNORM;
        auto r=test::CreateTexture(d.device.Get(),w,h,fmt,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE);
        auto up=test::CreateBuffer(d.device.Get(),w*h*4,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);unsigned* p=nullptr;up->Map(0,nullptr,reinterpret_cast<void**>(&p));
        for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){unsigned v=25;
            const int a=static_cast<int>(x)-dx;
            if(a>=128 && a<256 && y>=64 && y<192){unsigned t=unsigned(a)*1664525u+y*1013904223u;t^=t>>13;t*=2246822519u;t^=t>>16;v=60+(t%190);}
            if(world){const float X=float(x)-dx-jx,Y=float(y)-jy;
                const float z=world==1?test_pictures::RepeatingWaves(X,Y):test_pictures::NonRepeating(X,Y);
                v=unsigned(std::lround(255.f*z));}
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
    if(argc==1){
        // Real AMD pipeline, stationary UNJITTERED colour, but nonzero nominal
        // raster samples. This is the captured SE5 failure, not ideal flow fed
        // directly into the fitter. Calibration must remove the artificial
        // encode shifts and seed a new image pair exactly once at changeover.
        unsigned history_only=0;uint64_t last_epoch=0;unsigned stable=0;
        f={};f.generation=4;f.source_generation=444;f.raster_sample=true;
        for(unsigned i=0;i<156;++i){
            f.frame=1000+i;f.previous=f.frame-1;f.jitter[0]=(i&1u)?.375f:-.375f;
            f.raster_jitter[0]=f.jitter[0];
            auto actual=cf::Record(l.list.Get(),d.queue.Get(),before.Get(),depth.Get(),camera,f,tier);
            REQUIRE(actual.context);if(!actual.pair)++history_only;
            if(actual.colour_convention==static_cast<uint32_t>(na::jitter::Convention::Unjittered)){
                CHECK(actual.frame.jitter[0]==0.f && actual.frame.jitter[1]==0.f);
                if(last_epoch)CHECK(last_epoch==actual.colour_epoch);
                last_epoch=actual.colour_epoch;++stable;
            }
            execute();cf::Committed(actual);reset();
        }
        const auto calibrated=cf::GetStats();
        std::printf("source calibration convention=%u changes=%llu stable_frames=%u history_only=%u\n",
            calibrated.colour_jitter_convention,(unsigned long long)calibrated.colour_jitter_changes,stable,history_only);
        CHECK(calibrated.colour_jitter_convention==static_cast<uint32_t>(na::jitter::Convention::Unjittered));
        CHECK(stable>=20 && history_only==2);
        // Correcting the still scene must preserve real object motion after
        // that same source has calibrated, with nominal raster offsets on.
        for(auto* moving:{after.Get(),later.Get()}){
            ++f.frame;f.previous=f.frame-1;f.jitter[0]=-f.jitter[0];f.raster_jitter[0]=f.jitter[0];
            auto actual=cf::Record(l.list.Get(),d.queue.Get(),moving,depth.Get(),camera,f,tier);
            REQUIRE(actual.context && actual.pair && tier.residual);CHECK(actual.frame.jitter[0]==0.f);
            execute();cf::Committed(actual);reset();
        }
        CHECK(cf::GetStats().accepted>0);
    }
    // p44832 reproduction: the earlier matrix writer trained an Unjittered cache.
    // Real viewport jitter now reaches the scene, whose camera is projection-only.
    // Run the actual AMD engine + reconstruction + arbiter, not synthetic flow.
    {
        na::jitter::Remembered().enabled=true;
        na::jitter::Remembered().Put(na::jitter::kStageFlow,28,2);
        double identity[16]{},eye[3]{};for(int k=0;k<16;++k)identity[k]=k%5==0?1.:0.;
        const auto basis=na::motion::gpu::MakeFitBasis(identity,identity,identity,eye);
        camera.tier3_valid_flag=false;camera.fit_basis=&basis;
        cf::SetLoosen(100);
        const float jx[]={-.4375f,.4375f,-.375f,.375f,-.3125f,.3125f,-.25f,.25f};
        const float jy[]={.3889f,-.3889f,.2778f,-.2778f,.1667f,-.1667f,.0556f,-.0556f};
        auto barrier=test::Transition(output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,U);l.list->ResourceBarrier(1,&barrier);execute();reset();
        // R3: the raw search locks onto a repetition about 94 pixels away at 1080p.
        // The shared frame-candidate correction must make the camera fit follow the real pan,
        // with the same strict bound as the ordinary picture. A refused fit is not a pass.
        struct Picture {int kind;const char* name;};
        for(const Picture picture:{Picture{1,"four-sine"},Picture{2,"non-repeating"}}){
        std::vector<test::ComPtr<ID3D12Resource>> scenes;
        for(int i=0;i<12;++i)scenes.push_back(image(i<8?0:(i-7)*6,false,jx[i%8],jy[i%8],picture.kind));
        execute();reset();
        for(bool confirmed:{false,true}){
            cf::Reset();double still=0,moving=0;unsigned sn=0,mn=0;
            for(unsigned i=0;i<scenes.size();++i){
                cf::Frame jf;jf.frame=2000+i;jf.previous=i?jf.frame-1:0;jf.generation=77;jf.source_generation=77;
                jf.residual=false;jf.camera_reconstruct=true;jf.raster_sample=true;jf.raster_confirmed=confirmed;
                jf.jitter[0]=jf.raster_jitter[0]=jx[i%8];jf.jitter[1]=jf.raster_jitter[1]=jy[i%8];
                camera.jitter_current_px[0]=jx[i%8];camera.jitter_current_px[1]=jy[i%8];
                cf::Tier3 t;auto ticket=cf::Record(l.list.Get(),d.queue.Get(),scenes[i].Get(),depth.Get(),camera,jf,t);REQUIRE(ticket.context);
                if(confirmed)CHECK(ticket.frame.jitter[0]==jx[i%8]);
                camera.fix=t.fix;REQUIRE(arbiter.Dispatch(l.list.Get(),camera,out,0));
                auto copy_barrier=test::Transition(output.Get(),U,D3D12_RESOURCE_STATE_COPY_SOURCE);l.list->ResourceBarrier(1,&copy_barrier);
                l.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);std::swap(copy_barrier.Transition.StateBefore,copy_barrier.Transition.StateAfter);l.list->ResourceBarrier(1,&copy_barrier);
                execute();cf::Committed(ticket);reset();
                float* data=nullptr;REQUIRE(SUCCEEDED(rb->Map(0,nullptr,reinterpret_cast<void**>(&data))));
                double error=0;unsigned n=0;
                for(unsigned y=32;y<h-32;y+=4)for(unsigned x=48;x<w-48;x+=4){const auto q=(y*w+x)*2;
                    error+=std::hypot(data[q]+(i>=8?6.:0.),data[q+1]);++n;}
                rb->Unmap(0,nullptr);
                if(i>0&&i<8){still+=error/n;++sn;}if(i>=8){moving+=error/n;++mn;}
            }
            std::printf("real raster + remembered Unjittered, %s picture, confirmed=%d: stationary motion %.4f px, moving error %.4f px\n",picture.name,confirmed,still/sn,moving/mn);
            // The fit's own admission counters (running totals, and the last fit's numbers), so a
            // refusal names its gate. Diagnostic only.
            {const auto fs=cf::GetStats();
             std::printf("  camera fit %ux%u: recorded %llu collected %llu accepted %llu | flow test passed %llu, refused by the image check %llu, by the jitter test %llu | last fit: samples %u -> %u, energy %.4f -> %.4f (other hypothesis %.4f), accepted %d | image check: cells %u better %u worse %u, regions %u good %u, error fit %.4f camera %.4f\n",
                 w,h,(unsigned long long)fs.fit_recorded,(unsigned long long)fs.fit_collected,(unsigned long long)fs.fit_accepted,
                 (unsigned long long)fs.fit_flow_passed,(unsigned long long)fs.fit_rejected_image,(unsigned long long)fs.fit_rejected_jitter,
                 fs.fit_n0,fs.fit_n,fs.fit_e0,fs.fit_e,fs.fit_e_alt,fs.fit_last_accepted?1:0,
                 fs.fit_photo_tested,fs.fit_photo_better,fs.fit_photo_worse,fs.fit_photo_regions,fs.fit_photo_good,fs.fit_photo_e_fit,fs.fit_photo_e_cam);
             // Inspect the computed transform independently of its admission. This distinguishes
             // "solver found the wrong camera" from "right camera rejected because most flow is wrong".
             double fit_error=0,fit_x=0,fit_y=0;unsigned fit_count=0;
             for(unsigned y=32;y<h-32;y+=16)for(unsigned x=48;x<w-48;x+=16){
                 const double px=x+.5-camera.jitter_current_px[0],py=y+.5-camera.jitter_current_px[1];
                 const double ndc_point[]={2*px/w-1,1-2*py/h,.5,1};double clip[4]{};
                 for(unsigned row=0;row<4;++row)for(unsigned col=0;col<4;++col){double m=basis.C[row*4+col];
                     for(unsigned k=0;k<6;++k)m+=fs.fit_theta[k]*basis.G[k][row*4+col];clip[col]+=ndc_point[row]*m;}
                 const double vx=(clip[0]/clip[3]+1)*.5*w-px,vy=(1-clip[1]/clip[3])*.5*h-py;
                 fit_error+=std::hypot(vx+6,vy);fit_x+=vx;fit_y+=vy;++fit_count;
             }
             std::printf("  computed fit before admission: mean=(%.4f,%.4f) error=%.4f px\n",fit_x/fit_count,fit_y/fit_count,fit_error/fit_count);}
            if(confirmed){CHECK(still/sn<.08);CHECK(moving/mn<.35);}
            else CHECK(still/sn>.15); // old failure is reproduced on the identical pictures
        }
        }
        camera.fix=nullptr;camera.fit_basis=nullptr;na::jitter::Remembered().enabled=false;
    }
    // Independent audit: the actual AMD search -> expansion -> total refinement -> arbiter,
    // with all camera data absent/poisoned. Readbacks and waits are test-only.
    {
        cf::Reset();
        auto total_image=[&](float dx,float jx,float jy,int solid=-1){
            auto r=test::CreateTexture(d.device.Get(),w,h,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE);
            auto up=test::CreateBuffer(d.device.Get(),w*h*4,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            unsigned* data=nullptr;up->Map(0,nullptr,reinterpret_cast<void**>(&data));
            for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
                const float X=float(x)-dx-jx,Y=float(y)-jy;
                const float z=.5f+.18f*std::sin(.17f*X)+.14f*std::sin(.11f*X+.13f*Y)+.12f*std::cos(.07f*Y)+.05f*std::sin(.09f*X-.21f*Y);
                const unsigned v=solid>=0?unsigned(solid):unsigned(std::lround(255.f*z));
                data[y*w+x]=0xff000000u|v*0x010101u;
            }
            up->Unmap(0,nullptr);D3D12_TEXTURE_COPY_LOCATION a{},b{};
            a.pResource=up.Get();a.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;a.PlacedFootprint.Footprint={DXGI_FORMAT_R8G8B8A8_UNORM,w,h,1,w*4};
            b.pResource=r.Get();b.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;l.list->CopyTextureRegion(&b,0,0,0,&a,nullptr);
            auto bar=test::Transition(r.Get(),D3D12_RESOURCE_STATE_COPY_DEST,S);l.list->ResourceBarrier(1,&bar);uploads.push_back(up);return r;
        };
        na::motion::gpu::Inputs independent{};
        const float nan=std::numeric_limits<float>::quiet_NaN();
        for(int i=0;i<16;++i)independent.inv_view_proj_current.m[i]=independent.view_proj_previous.m[i]=nan;
        independent.jitter_current_px[0]=independent.jitter_current_px[1]=nan;
        independent.depth_clear=1.f;
        independent.tier1_source=na::motion::Tier1Source::Estimated;
        auto motion=test::CreateTexture(d.device.Get(),w,h,DXGI_FORMAT_R32G32_FLOAT,U,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto provenance=test::CreateTexture(d.device.Get(),w,h,DXGI_FORMAT_R8_UINT,U,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto read=test::CreateBuffer(d.device.Get(),w*h*8,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        const unsigned prov_stride=(w+255u)&~255u;
        auto read_prov=test::CreateBuffer(d.device.Get(),prov_stride*h,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        auto copy=[&](ID3D12Resource* texture,DXGI_FORMAT format,unsigned row,ID3D12Resource* target){
            auto bar=test::Transition(texture,U,D3D12_RESOURCE_STATE_COPY_SOURCE);l.list->ResourceBarrier(1,&bar);
            D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=texture;a.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            b.pResource=target;b.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;b.PlacedFootprint.Footprint={format,w,h,1,row};
            l.list->CopyTextureRegion(&b,0,0,0,&a,nullptr);std::swap(bar.Transition.StateBefore,bar.Transition.StateAfter);l.list->ResourceBarrier(1,&bar);
        };
        const float shifts[]={0,0,.125f,.375f,.125f,3.125f,3.125f};
        const float jitx[]={.25f,-.375f,.125f,-.25f,.375f,-.125f,.25f};
        const float jity[]={-.25f,.125f,.375f,-.125f,.25f,-.375f,-.25f};
        for(unsigned i=0;i<7;++i){
            auto scene=total_image(shifts[i],jitx[i],jity[i]);
            cf::Frame tf;tf.total=true;tf.frame=5000+i;tf.previous=i?tf.frame-1:0;tf.generation=99;tf.source_generation=99;
            tf.raster_sample=tf.raster_confirmed=true;tf.raster_jitter[0]=tf.jitter[0]=jitx[i];tf.raster_jitter[1]=tf.jitter[1]=jity[i];
            tf.last_reset_frame=5000;
            cf::Tier3 total_tier;auto ticket=cf::Record(l.list.Get(),d.queue.Get(),scene.Get(),depth.Get(),independent,tf,total_tier);
            REQUIRE(ticket.context);CHECK(ticket.pair==(i!=0));CHECK(!total_tier.residual && !total_tier.fix);
            if(i){
                REQUIRE(total_tier.total && total_tier.total_valid);
                independent.tier1_motion=total_tier.total;independent.tier1_valid=total_tier.total_valid;independent.tier1_valid_flag=true;
                na::motion::gpu::Outputs mo{motion.Get(),provenance.Get(),nullptr,w,h};REQUIRE(arbiter.Dispatch(l.list.Get(),independent,mo,0));
                copy(motion.Get(),DXGI_FORMAT_R32G32_FLOAT,w*8,read.Get());copy(provenance.Get(),DXGI_FORMAT_R8_UINT,prov_stride,read_prov.Get());
            }
            execute();cf::Committed(ticket);reset();
            if(i){
                float* data=nullptr;unsigned char* tags=nullptr;REQUIRE(SUCCEEDED(read->Map(0,nullptr,reinterpret_cast<void**>(&data))));
                REQUIRE(SUCCEEDED(read_prov->Map(0,nullptr,reinterpret_cast<void**>(&tags))));
                double err=0,meanx=0,meany=0;unsigned count=0,estimated=0,wrong_tag=0;
                const double expected=shifts[i-1]-shifts[i];
                for(unsigned y=16;y<h-16;++y)for(unsigned x=16;x<w-16;++x){const size_t k=size_t(y)*w+x;
                    err+=std::hypot(data[k*2]-expected,data[k*2+1]);meanx+=data[k*2];meany+=data[k*2+1];++count;
                    const auto tag=tags[size_t(y)*prov_stride+x];
                    if(tag==na::motion::NA_MOTION_ESTIMATED)++estimated;
                    else if(tag!=na::motion::NA_MOTION_INVALID)++wrong_tag;
                }
                read->Unmap(0,nullptr);read_prov->Unmap(0,nullptr);
                const auto s=cf::GetStats();
                std::printf("total pipeline i=%u expected=%.3f mean=(%.4f,%.4f) error=%.4f estimated=%u/%u wrong_tag=%u GPU=%.3fms cuts=%llu format=%u\n",
                    i,expected,meanx/count,meany/count,err/count,estimated,count,wrong_tag,s.ms,(unsigned long long)s.total_cuts,s.stabilized_format);
                CHECK(wrong_tag==0);CHECK(estimated>count*9/10);CHECK(err/count<.13);
                CHECK(s.stabilized_format==DXGI_FORMAT_R10G10B10A2_UNORM || s.stabilized_format==DXGI_FORMAT_R8G8B8A8_UNORM);
                if(s.stabilized_format==DXGI_FORMAT_R10G10B10A2_UNORM){
                    // Whole GPU chain, including the stabilized store: the shader-only fixture
                    // cannot catch losing the subpixel evidence to 8-bit quantisation again.
                    if(i==2)CHECK(err/count<.07); // 1/8 px
                    if(i==3)CHECK(err/count<.105); // 1/4 px
                    if(i==4)CHECK(err/count<.095); // reversal
                    if(i==1 || i==6)CHECK(err/count<.005); // stationary, alternating raster jitter
                    if(i==5)CHECK(err/count<.04); // 3 px: precision must not worsen ordinary motion
                }
            }
        }
        // A strong image cut must hide the old temporal picture on the cut frame and stop hiding it
        // when the caller submits the requested reset. Grey 47 is the synthetic DLSS output witness.
        auto show=test::CreateTexture(d.device.Get(),w,h,DXGI_FORMAT_R8G8B8A8_UNORM,U,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto shown=test::CreateBuffer(d.device.Get(),w*h*4,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        auto marker=total_image(0,0,0,47);auto white=total_image(0,0,0,250);
        auto mark=[&]{
            auto a=test::Transition(marker.Get(),S,D3D12_RESOURCE_STATE_COPY_SOURCE),b=test::Transition(show.Get(),U,D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_RESOURCE_BARRIER bars[]={a,b};l.list->ResourceBarrier(2,bars);l.list->CopyResource(show.Get(),marker.Get());
            for(auto& bar:bars)std::swap(bar.Transition.StateBefore,bar.Transition.StateAfter);l.list->ResourceBarrier(2,bars);
        };
        for(unsigned i=0;i<3;++i){
            cf::Frame tf;tf.total=true;tf.frame=5007+i;tf.previous=tf.frame-1;tf.generation=99;tf.source_generation=99;
            tf.raster_sample=tf.raster_confirmed=true;tf.last_reset_frame=i==2?5008:5000;
            cf::Tier3 total_tier;auto ticket=cf::Record(l.list.Get(),d.queue.Get(),white.Get(),depth.Get(),independent,tf,total_tier);
            REQUIRE(ticket.context && ticket.pair && total_tier.total);
            mark();cf::RecordCutShow(l.list.Get(),ticket,white.Get(),show.Get());copy(show.Get(),DXGI_FORMAT_R8G8B8A8_UNORM,w*4,shown.Get());
            execute();cf::Committed(ticket);reset();
            unsigned* data=nullptr;REQUIRE(SUCCEEDED(shown->Map(0,nullptr,reinterpret_cast<void**>(&data))));
            const unsigned pixel=data[(h/2)*w+w/2]&255u;
            unsigned wrong_pixels=0;
            for(unsigned j=0;j<w*h;++j)wrong_pixels+=(data[j]&255u)!=(i==0?250u:47u);
            CHECK(wrong_pixels==0);
            shown->Unmap(0,nullptr);
            const auto s=cf::GetStats();std::printf("cut show i=%u reset=%d pixel=%u cuts=%llu cut_frame=%llu\n",i,ticket.cut_reset,pixel,(unsigned long long)s.total_cuts,(unsigned long long)s.total_cut_frame);
            if(i==0){CHECK(s.total_cuts>0);CHECK(pixel==250);}
            else {CHECK(pixel==47);CHECK(ticket.cut_reset==(i==1));}
        }
    }
    cf::Reset();CHECK(d.device->GetDeviceRemovedReason()==S_OK);execute();
    if(info){unsigned errors=0;for(UINT64 i=0;i<info->GetNumStoredMessages();++i){
        SIZE_T bytes=0;info->GetMessage(i,nullptr,&bytes);std::vector<uint8_t> data(bytes);auto* message=reinterpret_cast<D3D12_MESSAGE*>(data.data());
        if(SUCCEEDED(info->GetMessage(i,message,&bytes)) && message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){
            std::printf("D3D12: %s\n",message->pDescription);++errors;}}
        std::printf("D3D12 debug errors=%u\n",errors);CHECK(errors==0);}
    return test::finish("j2_compute_flow_test");
}
