#pragma once
// Independent, test-only GPU verification. All waits, uploads and readbacks stay in this fixture.
#include "core/screen_motion.hpp"
#include <array>
#include <chrono>

namespace screen_audit {
constexpr auto S = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto U = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
using test::ComPtr;
struct Gpu {
    test::Device& d;
    test::List l;
    std::vector<ComPtr<ID3D12Resource>> keep;
    explicit Gpu(test::Device& device) : d(device) { CHECK(l.Create(d.device.Get())); }
    void Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        auto t = test::Transition(r,a,b); l.list->ResourceBarrier(1,&t);
    }
    void Order() { D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV; l.list->ResourceBarrier(1,&b); }
    int Execute() {
        REQUIRE(SUCCEEDED(l.list->Close())); ID3D12CommandList* lists[]={l.list.Get()};
        { na::nr::native::Submission submitted(d.queue.Get(),1,lists); d.queue->ExecuteCommandLists(1,lists); }
        d.WaitIdle(); return 0;
    }
    void Reset() { na::nr::native::Reset(l.list.Get()); na::nr::native::Maintain(); l.Reset(); keep.clear(); }
    ComPtr<ID3D12Resource> Upload(unsigned w,unsigned h,DXGI_FORMAT fmt,unsigned bpp,const void* data) {
        const unsigned row=(w*bpp+255u)&~255u;
        auto r=test::CreateTexture(d.device.Get(),w,h,fmt,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE);
        auto up=test::CreateBuffer(d.device.Get(),size_t(row)*h,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        void* p=nullptr; CHECK(SUCCEEDED(up->Map(0,nullptr,&p)));
        for(unsigned y=0;y<h;++y) std::memcpy(static_cast<char*>(p)+size_t(y)*row,static_cast<const char*>(data)+size_t(y)*w*bpp,w*bpp);
        up->Unmap(0,nullptr); D3D12_TEXTURE_COPY_LOCATION a{},b{};
        a.pResource=up.Get(); a.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; a.PlacedFootprint.Footprint={fmt,w,h,1,row};
        b.pResource=r.Get(); b.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; l.list->CopyTextureRegion(&b,0,0,0,&a,nullptr);
        Barrier(r.Get(),D3D12_RESOURCE_STATE_COPY_DEST,S); keep.push_back(up); return r;
    }
    ComPtr<ID3D12Resource> Read(ID3D12Resource* r,unsigned w,unsigned h,DXGI_FORMAT fmt,unsigned bpp,D3D12_RESOURCE_STATES state=S) {
        const unsigned row=(w*bpp+255u)&~255u;
        auto rb=test::CreateBuffer(d.device.Get(),size_t(row)*h,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        Barrier(r,state,D3D12_RESOURCE_STATE_COPY_SOURCE); D3D12_TEXTURE_COPY_LOCATION a{},b{};
        a.pResource=r; a.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        b.pResource=rb.Get(); b.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; b.PlacedFootprint.Footprint={fmt,w,h,1,row};
        l.list->CopyTextureRegion(&b,0,0,0,&a,nullptr); Barrier(r,D3D12_RESOURCE_STATE_COPY_SOURCE,state); return rb;
    }
    std::vector<float> Floats(ID3D12Resource* r,unsigned w,unsigned h,unsigned components) {
        const unsigned row=(w*components*4+255u)&~255u;
        void* p=nullptr; CHECK(SUCCEEDED(r->Map(0,nullptr,&p))); std::vector<float> v(size_t(w)*h*components);
        for(unsigned y=0;y<h;++y) std::memcpy(v.data()+size_t(y)*w*components,static_cast<char*>(p)+size_t(y)*row,w*components*4);
        r->Unmap(0,nullptr); return v;
    }
};

inline int ArbiterFallback(test::Device& d) {
    constexpr unsigned w=128,h=64,n=w*h;
    Gpu g(d); std::vector<float> eng(n*2),est(n*2),em(n),mm(n,1.f),z(n,.5f);
    const float nan=std::numeric_limits<float>::quiet_NaN(),inf=std::numeric_limits<float>::infinity();
    for(unsigned i=0;i<n;++i) {
        eng[2*i]=11; eng[2*i+1]=-3; est[2*i]=-4; est[2*i+1]=2;
        // Valid replay, absent replay, nonfinite replay, malformed masks, missing/bad fallback.
        switch(i%8) {
        case 0: em[i]=1; break;
        case 1: em[i]=0; break;
        case 2: em[i]=1; eng[2*i]=nan; break;
        case 3: em[i]=nan; break;
        case 4: em[i]=2; mm[i]=.5f; break;
        case 5: em[i]=0; mm[i]=0; break;
        case 6: em[i]=0; est[2*i+1]=inf; break;
        case 7: em[i]=1; eng[2*i+1]=inf; mm[i]=nan; break;
        }
    }
    auto er=g.Upload(w,h,DXGI_FORMAT_R32G32_FLOAT,8,eng.data()),mr=g.Upload(w,h,DXGI_FORMAT_R32G32_FLOAT,8,est.data());
    auto ev=g.Upload(w,h,DXGI_FORMAT_R32_FLOAT,4,em.data()),mv=g.Upload(w,h,DXGI_FORMAT_R32_FLOAT,4,mm.data());
    auto depth=g.Upload(w,h,DXGI_FORMAT_R32_FLOAT,4,z.data()); REQUIRE(g.Execute()==0); g.Reset();
    na::motion::gpu::Arbiter arb; REQUIRE(arb.Init(d.device.Get()));
    auto xy=test::CreateTexture(d.device.Get(),w,h,DXGI_FORMAT_R32G32_FLOAT,U,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto pr=test::CreateTexture(d.device.Get(),w,h,DXGI_FORMAT_R8_UINT,U,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    for(bool replay:{false,true}) for(bool camera:{false,true}) for(unsigned bytes:{32u,36u}) {
        auto stats=test::CreateBuffer(d.device.Get(),bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto zero=test::CreateBuffer(d.device.Get(),bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        void* p=nullptr; REQUIRE(SUCCEEDED(zero->Map(0,nullptr,&p))); std::memset(p,0,bytes); zero->Unmap(0,nullptr);
        g.l.list->CopyBufferRegion(stats.Get(),0,zero.Get(),0,bytes); g.Barrier(stats.Get(),D3D12_RESOURCE_STATE_COPY_DEST,U);
        na::motion::gpu::Inputs in; in.tier1_valid_flag=replay; in.tier1_motion=er.Get(); in.tier1_valid=ev.Get();
        in.estimated_valid_flag=true; in.estimated_motion=mr.Get(); in.estimated_valid=mv.Get(); in.depth=depth.Get(); in.tier2_valid_flag=camera;
        for(unsigned k=0;k<16;++k) in.inv_view_proj_current.m[k]=in.view_proj_previous.m[k]=(k%5==0)?1.f:0.f;
        na::motion::gpu::Outputs out; out.xy=xy.Get(); out.provenance=pr.Get(); out.stats=stats.Get(); out.width=w; out.height=h;
        REQUIRE(arb.Dispatch(g.l.list.Get(),in,out,0));
        auto rx=g.Read(xy.Get(),w,h,DXGI_FORMAT_R32G32_FLOAT,8,U),rp=g.Read(pr.Get(),w,h,DXGI_FORMAT_R8_UINT,1,U);
        auto rs=test::CreateBuffer(d.device.Get(),bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        g.Barrier(stats.Get(),U,D3D12_RESOURCE_STATE_COPY_SOURCE); g.l.list->CopyBufferRegion(rs.Get(),0,stats.Get(),0,bytes);
        REQUIRE(g.Execute()==0); auto values=g.Floats(rx.Get(),w,h,2);
        uint8_t* provenance=nullptr; REQUIRE(SUCCEEDED(rp->Map(0,nullptr,reinterpret_cast<void**>(&provenance))));
        unsigned errors=0,estimated=0,engine=0;
        for(unsigned i=0;i<n;++i) {
            bool own=replay&&i%8==0, fallback=!own&&i%8<5;
            float x=own?11.f:fallback?-4.f:0.f, y=own?-3.f:fallback?2.f:0.f;
            unsigned label=own?na::motion::NA_MOTION_ENGINE:fallback?na::motion::NA_MOTION_ESTIMATED:camera?na::motion::NA_MOTION_CAMERA:na::motion::NA_MOTION_INVALID;
            if(!std::isfinite(values[2*i])||!std::isfinite(values[2*i+1])||std::abs(values[2*i]-x)>.001f||std::abs(values[2*i+1]-y)>.001f||provenance[(i/w)*256+i%w]!=label) ++errors;
            estimated+=fallback; engine+=own;
        }
        rp->Unmap(0,nullptr); unsigned* counts=nullptr; REQUIRE(SUCCEEDED(rs->Map(0,nullptr,reinterpret_cast<void**>(&counts))));
        CHECK(counts[0]==engine); if(bytes==36)CHECK(counts[8]==estimated); rs->Unmap(0,nullptr);
        std::printf("arbiter replay=%d camera=%d counter_bytes=%u errors=%u engine=%u estimated=%u\n",replay,camera,bytes,errors,engine,estimated); CHECK(errors==0); g.Reset();
    }
    return 0;
}

// A small harness dispatching the unchanged production programs with controlled inputs.
struct Programs {
    Gpu& g; ComPtr<ID3D12RootSignature> root; ComPtr<ID3D12DescriptorHeap> heap;
    std::array<ComPtr<ID3D12PipelineState>,4> psos; unsigned stride=0;
    explicit Programs(Gpu& gpu):g(gpu){}
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(unsigned table,unsigned i) {auto p=heap->GetCPUDescriptorHandleForHeapStart();p.ptr+=size_t(table*12+i)*stride;return p;}
    int Init() {
        D3D12_DESCRIPTOR_RANGE r[2]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,8,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,4,0,0,8}};
        D3D12_ROOT_PARAMETER p[2]{};p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;p[0].DescriptorTable={2,r};
        p[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;p[1].Constants={0,0,28};
        D3D12_STATIC_SAMPLER_DESC s{};s.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;s.AddressU=s.AddressV=s.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;s.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;s.MaxLOD=D3D12_FLOAT32_MAX;
        D3D12_ROOT_SIGNATURE_DESC rd{2,p,1,&s,D3D12_ROOT_SIGNATURE_FLAG_NONE};ComPtr<ID3DBlob> sig,err;
        REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&sig,&err)));
        REQUIRE(SUCCEEDED(g.d.device->CreateRootSignature(0,sig->GetBufferPointer(),sig->GetBufferSize(),IID_PPV_ARGS(&root))));
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,48,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
        REQUIRE(SUCCEEDED(g.d.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap))));stride=g.d.device->GetDescriptorHandleIncrementSize(hd.Type);
        const char* names[]={"fit_sample","fit_accum","fit_solve","measure"};
        for(unsigned i=0;i<4;++i) {ComPtr<ID3DBlob> cs;
            const HRESULT hr=D3DCompile(na::j2::kFlowShader,sizeof na::j2::kFlowShader-1,"screen-audit",nullptr,nullptr,names[i],"cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&cs,&err);
            if(FAILED(hr)&&err)std::printf("%s\n",static_cast<const char*>(err->GetBufferPointer()));REQUIRE(SUCCEEDED(hr));
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={cs->GetBufferPointer(),cs->GetBufferSize()};REQUIRE(SUCCEEDED(g.d.device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&psos[i]))));
        }
        return 0;
    }
    void Srv(unsigned t,unsigned i,ID3D12Resource* r,DXGI_FORMAT f) {D3D12_SHADER_RESOURCE_VIEW_DESC v{};v.Format=f;v.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;v.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;v.Texture2D.MipLevels=1;g.d.device->CreateShaderResourceView(r,&v,Cpu(t,i));}
    void Uav(unsigned t,unsigned i,ID3D12Resource* r,DXGI_FORMAT f) {D3D12_UNORDERED_ACCESS_VIEW_DESC v{};v.Format=f;v.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;g.d.device->CreateUnorderedAccessView(r,nullptr,&v,Cpu(t,i));}
    void Raw(unsigned t,ID3D12Resource* r,unsigned words) {D3D12_UNORDERED_ACCESS_VIEW_DESC v{};v.Format=DXGI_FORMAT_R32_TYPELESS;v.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;v.Buffer.NumElements=words;v.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;g.d.device->CreateUnorderedAccessView(r,nullptr,&v,Cpu(t,10));}
    void Defaults() {for(unsigned t=0;t<4;++t) {
        for(unsigned i=0;i<8;++i)Srv(t,i,nullptr,i==4?DXGI_FORMAT_R32_UINT:i==3?DXGI_FORMAT_R32_FLOAT:(i==1||i==2)?DXGI_FORMAT_R32G32_FLOAT:DXGI_FORMAT_R32G32B32A32_FLOAT);
        Uav(t,8,nullptr,DXGI_FORMAT_R32G32B32A32_FLOAT);Uav(t,9,nullptr,DXGI_FORMAT_R32_FLOAT);Raw(t,nullptr,20);Uav(t,11,nullptr,DXGI_FORMAT_R32G32B32A32_FLOAT);
    }}
    void Dispatch(unsigned table,const std::array<unsigned,28>& c,unsigned x=1,unsigned y=1) {
        ID3D12DescriptorHeap* heaps[]={heap.Get()};g.l.list->SetDescriptorHeaps(1,heaps);g.l.list->SetComputeRootSignature(root.Get());g.l.list->SetPipelineState(psos[table].Get());
        auto h=heap->GetGPUDescriptorHandleForHeapStart();h.ptr+=size_t(table*12)*stride;g.l.list->SetComputeRootDescriptorTable(0,h);g.l.list->SetComputeRoot32BitConstants(1,28,c.data(),0);g.l.list->Dispatch(x,y,1);
    }
};
inline void Word(std::array<unsigned,28>& c,unsigned i,float v){std::memcpy(&c[i],&v,4);}
// The production order of the fit's passes (compute_flow.cpp): the depth groups' split, then the far
// group's fit and the near group's, each a plain step, the reweighted ones and the final evaluation.
// Table 1 is fit_accum, table 2 fit_solve (its t7 must be the grid). `heads`: a readback buffer of
// 160 bytes that receives the far fit's head and then the near fit's.
inline void FitPasses(Programs& p,Gpu& g,std::array<unsigned,28>& c,unsigned tiles,ID3D12Resource* state,ID3D12Resource* zero,ID3D12Resource* heads) {
    namespace sm=na::motion::screen;const sm::Params params;
    c[4]=2;c[8]=0;p.Dispatch(2,c);g.Order();
    for(unsigned row=0;row<2;++row) {
        if(row){g.Barrier(state,U,D3D12_RESOURCE_STATE_COPY_DEST);g.l.list->CopyBufferRegion(state,0,zero,0,76);g.Barrier(state,D3D12_RESOURCE_STATE_COPY_DEST,U);}
        c[8]=row;
        for(int step=0;step<=params.steps;++step) {
            Word(c,25,step==params.steps?params.robust_px:step?sm::ScaleAt(step,params):0.f);
            const float now=sm::TileScaleFor(step,params);Word(c,20,params.tile_px>0?(now>0?now:-1.f):0.f);Word(c,21,sm::TileScaleFor(step+1,params));
            c[4]=0;p.Dispatch(1,c,tiles);g.Order();c[4]=step==params.steps?1u:0u;p.Dispatch(2,c);g.Order();
        }
        if(heads){g.Barrier(state,U,D3D12_RESOURCE_STATE_COPY_SOURCE);g.l.list->CopyBufferRegion(heads,row*80,state,0,80);g.Barrier(state,D3D12_RESOURCE_STATE_COPY_SOURCE,U);}
    }
}

inline int SolveControlled(test::Device& d) {
    namespace sm=na::motion::screen;
    constexpr unsigned w=512,h=384,gw=w/8,gh=h/8,groups=((gw+7)/8)*((gh+7)/8),words=20+86*groups; // a thread group is one tile of 8 x 8 samples; 81 sums and 5 tile words each
    Gpu g(d); Programs p(g); REQUIRE(p.Init()==0);
    for(unsigned variant=0;variant<5;++variant) {
        p.Defaults(); std::vector<sm::Sample> samples;std::vector<float> grid(gw*gh*4);
        sm::Model truth;truth.valid=true;truth.nearness_limit=1;
        const float theta[]={-3.f,.21f,-.13f,1.f,.16f,-.09f,.07f,-.03f,13.f,-8.f,4.f};std::copy(theta,theta+11,truth.theta);
        for(unsigned y=0;y<gh;++y)for(unsigned x=0;x<gw;++x) {
            sm::Sample a;a.px=float(x*8)+4.5f;a.py=float(y*8)+4.5f;
            a.nearness=variant==2?0.f:.01f+.06f*float(y)/gh+.02f*std::sin(float(x)*.23f)*std::sin(float(y)*.31f);
            const bool figure=variant==4&&x<gw/2&&y>gh/4; // a near figure in front of the far scene: two groups of depths
            if(figure)a.nearness=.4f+.05f*float(x)/gw;
            a.confirmed=variant==3?(y==0):(x+y)%3!=0;sm::Evaluate(truth,a.px,a.py,a.nearness,w,h,a.vx,a.vy);
            if(variant==1&&x>gw*3/4&&y>gh*2/3){a.vx+=18;a.vy-=9;}
            if(figure){a.vx+=.9f*std::sin(float(y)*.4f);a.vy+=.6f*std::cos(float(x)*.3f);} // it moves by itself and bends
            samples.push_back(a);size_t k=(size_t(y)*gw+x)*4;grid[k]=a.vx;grid[k+1]=a.vy;grid[k+2]=a.nearness;grid[k+3]=a.confirmed?1.f:0.f;
        }
        const auto layers=sm::FitLayers(samples.data(),samples.size(),w,h);const auto& ref=layers.far_fit;
        auto input=g.Upload(gw,gh,DXGI_FORMAT_R32G32B32A32_FLOAT,16,grid.data());
        auto state=test::CreateBuffer(d.device.Get(),words*4,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto zero=test::CreateBuffer(d.device.Get(),80,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* z=nullptr;REQUIRE(SUCCEEDED(zero->Map(0,nullptr,&z)));std::memset(z,0,80);zero->Unmap(0,nullptr);
        g.l.list->CopyBufferRegion(state.Get(),0,zero.Get(),0,80);g.Barrier(state.Get(),D3D12_RESOURCE_STATE_COPY_DEST,U);
        auto model=test::CreateTexture(d.device.Get(),9,2,DXGI_FORMAT_R32G32_FLOAT,U,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        p.Srv(1,7,input.Get(),DXGI_FORMAT_R32G32B32A32_FLOAT);p.Srv(2,7,input.Get(),DXGI_FORMAT_R32G32B32A32_FLOAT);p.Raw(1,state.Get(),words);p.Raw(2,state.Get(),words);p.Uav(2,8,model.Get(),DXGI_FORMAT_R32G32_FLOAT);
        std::array<unsigned,28> c{};c[0]=c[2]=w;c[1]=c[3]=h;c[9]=gw;c[10]=gh;c[5]=8;
        auto rb=test::CreateBuffer(d.device.Get(),160,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        FitPasses(p,g,c,groups,state.Get(),zero.Get(),rb.Get());
        auto rm=g.Read(model.Get(),9,2,DXGI_FORMAT_R32G32_FLOAT,8,U);
        REQUIRE(g.Execute()==0);auto m=g.Floats(rm.Get(),9,2,2);float* head=nullptr;REQUIRE(SUCCEEDED(rb->Map(0,nullptr,reinterpret_cast<void**>(&head))));
        for(unsigned k=0;k<16;++k){CHECK(std::isfinite(m[k]));CHECK(std::isfinite(head[k]));}
        float maximum=0;for(unsigned k=0;k<11;++k)maximum=(std::max)(maximum,std::abs(head[k]-ref.theta[k]));
        CHECK((m[12]>0)==ref.valid);CHECK(head[13]==ref.samples);CHECK(head[14]==ref.agree);CHECK(std::abs(head[11]-ref.nearness_limit)<1e-5f);
        if(ref.valid)CHECK(maximum<.002f);
        std::printf("controlled screen fit variant=%u valid=%d/%d samples=%.0f/%.0f agree=%.0f/%.0f nearness=%.6f/%.6f max_term_delta=%.7f\n",variant,m[12]>0,ref.valid,head[13],ref.samples,head[14],ref.agree,head[11],ref.nearness_limit,maximum);
        // The depth groups: the same split, and the near group's own fit in the model's second row.
        const float* near_head=head+20;float near_maximum=0;for(unsigned k=0;k<11;++k)near_maximum=(std::max)(near_maximum,std::abs(near_head[k]-layers.near_fit.theta[k]));
        CHECK((variant==4)==layers.split.found);CHECK(std::abs(near_head[19]-layers.split.nearness)<=1e-5f*layers.split.nearness);CHECK(m[16]==near_head[19]);
        CHECK((m[18+12]>0)==layers.near_fit.valid);CHECK(near_head[13]==layers.near_fit.samples);
        if(layers.split.found){CHECK(layers.near_fit.valid);CHECK(std::abs(near_head[14]-layers.near_fit.agree)<=2);CHECK(near_maximum<.02f);for(unsigned k=0;k<11;++k)CHECK(std::abs(m[18+k]-near_head[k])<1e-6f);}
        std::printf("controlled depth groups variant=%u split=%.6f/%.6f near valid=%d/%d samples=%.0f/%.0f agree=%.0f/%.0f max_term_delta=%.7f\n",variant,near_head[19],layers.split.nearness,m[18+12]>0,layers.near_fit.valid,near_head[13],layers.near_fit.samples,near_head[14],layers.near_fit.agree,near_maximum);
        rb->Unmap(0,nullptr);g.Reset();
    }
    return 0;
}
inline int SampleFitMeasure(test::Device& d) {
    namespace sm=na::motion::screen;namespace tm=na::motion::total;
    constexpr unsigned w=512,h=384,gw=w/8,gh=h/8,groups=((gw+7)/8)*((gh+7)/8),words=20+86*groups; // a thread group is one tile of 8 x 8 samples; 81 sums and 5 tile words each
    Gpu g(d);Programs p(g);REQUIRE(p.Init()==0);
    for(unsigned variant=0;variant<5;++variant) {
        p.Defaults(); sm::Model truth;truth.valid=true;truth.nearness_limit=1;
        const float jx=variant==3?.375f:0.f,jy=variant==3?-.25f:0.f,far_depth=variant==4?1.f:0.f;
        const float theta[]={-2.5f,.08f,.03f,.5f,.02f,-.02f,.01f,-.02f,15.f,-7.f,4.f};std::copy(theta,theta+11,truth.theta);
        const auto nearness=[&](float x,float y){return variant==2?0.f:.01f+.06f*y/h+.02f*std::sin(x*.017f)*std::sin(y*.023f);};
        const auto object=[&](float x,float y){return variant==1&&x>float(w)*.75f&&y>float(h)*.67f;};
        const auto motion=[&](float x,float y,float& vx,float& vy){sm::Evaluate(truth,x,y,nearness(x,y),w,h,vx,vy);if(object(x,y)){vx+=10;vy-=4;}};
        const auto texture=[](float x,float y){
            const bool textured=std::fmod(x+4096.f,64.f)<30.f&&std::fmod(y+4096.f,48.f)<32.f;
            return textured?.45f+.15f*std::sin(x*.27f+y*.17f)+.13f*std::sin(x*.11f-y*.33f)+.1f*std::cos(x*.41f+y*.23f):.012f;
        };
        std::vector<float> current(w*h),previous(w*h),rgba(w*h*4),raw(w*h*2),valid(w*h,1.f),depth(w*h);
        for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x) {
            const size_t i=size_t(y)*w+x;const float px=float(x)+.5f,py=float(y)+.5f;current[i]=texture(px,py);
            float gx=px,gy=py;for(unsigned k=0;k<8;++k){float vx,vy;motion(gx,gy,vx,vy);gx=px-vx;gy=py-vy;}previous[i]=texture(gx,gy);
            motion(px-jx,py-jy,raw[2*i],raw[2*i+1]);depth[i]=far_depth==0?nearness(px-jx,py-jy):1.f-nearness(px-jx,py-jy);
            if(current[i]==.012f){raw[2*i]=17;raw[2*i+1]=-13;} // explicit bad search in untextured regions
            rgba[4*i]=rgba[4*i+1]=rgba[4*i+2]=current[i];rgba[4*i+3]=1;
        }
        auto cur=g.Upload(w,h,DXGI_FORMAT_R32G32B32A32_FLOAT,16,rgba.data());
        for(unsigned i=0;i<w*h;++i)rgba[4*i]=rgba[4*i+1]=rgba[4*i+2]=previous[i];
        auto prev=g.Upload(w,h,DXGI_FORMAT_R32G32B32A32_FLOAT,16,rgba.data());
        auto rr=g.Upload(w,h,DXGI_FORMAT_R32G32_FLOAT,8,raw.data()),rv=g.Upload(w,h,DXGI_FORMAT_R32_FLOAT,4,valid.data()),rd=g.Upload(w,h,DXGI_FORMAT_R32_FLOAT,4,depth.data());
        auto grid=test::CreateTexture(d.device.Get(),gw,gh,DXGI_FORMAT_R32G32B32A32_FLOAT,U,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto model=test::CreateTexture(d.device.Get(),9,2,DXGI_FORMAT_R32G32_FLOAT,U,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto output=test::CreateTexture(d.device.Get(),w,h,DXGI_FORMAT_R32G32_FLOAT,U,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto mask=test::CreateTexture(d.device.Get(),w,h,DXGI_FORMAT_R32_FLOAT,U,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto state=test::CreateBuffer(d.device.Get(),words*4,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto counters=test::CreateBuffer(d.device.Get(),40,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto zero=test::CreateBuffer(d.device.Get(),80,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* z=nullptr;REQUIRE(SUCCEEDED(zero->Map(0,nullptr,&z)));std::memset(z,0,80);zero->Unmap(0,nullptr);
        g.l.list->CopyBufferRegion(state.Get(),0,zero.Get(),0,80);g.Barrier(state.Get(),D3D12_RESOURCE_STATE_COPY_DEST,U);
        g.l.list->CopyBufferRegion(counters.Get(),0,zero.Get(),0,40);g.Barrier(counters.Get(),D3D12_RESOURCE_STATE_COPY_DEST,U);
        for(unsigned t:{0u,3u}) {p.Srv(t,0,rd.Get(),DXGI_FORMAT_R32_FLOAT);p.Srv(t,1,rr.Get(),DXGI_FORMAT_R32G32_FLOAT);p.Srv(t,3,rv.Get(),DXGI_FORMAT_R32_FLOAT);p.Srv(t,5,cur.Get(),DXGI_FORMAT_R32G32B32A32_FLOAT);p.Srv(t,6,prev.Get(),DXGI_FORMAT_R32G32B32A32_FLOAT);}
        p.Uav(0,8,grid.Get(),DXGI_FORMAT_R32G32B32A32_FLOAT);p.Srv(1,7,grid.Get(),DXGI_FORMAT_R32G32B32A32_FLOAT);p.Srv(2,7,grid.Get(),DXGI_FORMAT_R32G32B32A32_FLOAT);p.Raw(1,state.Get(),words);p.Raw(2,state.Get(),words);p.Uav(2,8,model.Get(),DXGI_FORMAT_R32G32_FLOAT);
        p.Srv(3,2,model.Get(),DXGI_FORMAT_R32G32_FLOAT);p.Uav(3,8,output.Get(),DXGI_FORMAT_R32G32_FLOAT);p.Uav(3,9,mask.Get(),DXGI_FORMAT_R32_FLOAT);p.Raw(3,counters.Get(),10);
        std::array<unsigned,28> c{};c[0]=c[2]=w;c[1]=c[3]=h;c[9]=gw;c[10]=gh;c[5]=8;
        Word(c,22,jx);Word(c,23,jy);Word(c,24,far_depth);
        p.Dispatch(0,c,(gw+7)/8,(gh+7)/8);g.Order();g.Barrier(grid.Get(),U,S);
        FitPasses(p,g,c,groups,state.Get(),zero.Get(),nullptr);
        g.Barrier(model.Get(),U,S);p.Dispatch(3,c,(w+7)/8,(h+7)/8);g.Order();
        auto rg=g.Read(grid.Get(),gw,gh,DXGI_FORMAT_R32G32B32A32_FLOAT,16),rm=g.Read(model.Get(),9,2,DXGI_FORMAT_R32G32_FLOAT,8);
        auto ro=g.Read(output.Get(),w,h,DXGI_FORMAT_R32G32_FLOAT,8,U),rvv=g.Read(mask.Get(),w,h,DXGI_FORMAT_R32_FLOAT,4,U);
        REQUIRE(g.Execute()==0);auto gs=g.Floats(rg.Get(),gw,gh,4),gm=g.Floats(rm.Get(),9,2,2),vectors=g.Floats(ro.Get(),w,h,2),masks=g.Floats(rvv.Get(),w,h,1);
        std::vector<sm::Sample> samples;tm::Image ci{current.data(),w,h},pi{previous.data(),w,h};unsigned sample_decisions=0,sample_errors=0;
        for(unsigned y=0;y<gh;++y)for(unsigned x=0;x<gw;++x){size_t i=size_t(y)*gw+x,j=size_t(y*8+4)*w+x*8+4;sm::Sample a;
            a.px=float(x*8)+4.5f-jx;a.py=float(y*8)+4.5f-jy;a.vx=gs[4*i];a.vy=gs[4*i+1];a.nearness=gs[4*i+2];a.confirmed=gs[4*i+3]>.5f;samples.push_back(a);
            float vx,vy;bool confirmed=tm::SamplePixel(ci,pi,a.px,a.py,true,raw[2*j],raw[2*j+1],vx,vy);sample_decisions+=confirmed!=a.confirmed;
            sample_errors+=confirmed&&a.confirmed&&std::hypot(vx-a.vx,vy-a.vy)>.03f;
        }
        const auto layers=sm::FitLayers(samples.data(),samples.size(),w,h);const auto& ref=layers.far_fit;CHECK(!layers.split.found&&gm[16]==0&&gm[18+12]==0);sm::Model fitted;std::copy(gm.begin(),gm.begin()+11,fitted.theta);fitted.nearness_limit=gm[11];fitted.valid=gm[12]>0;
        for(float value:gm)CHECK(std::isfinite(value));
        float maxterm=0;for(unsigned k=0;k<11;++k)maxterm=(std::max)(maxterm,std::abs(gm[k]-ref.theta[k]));
        CHECK(fitted.valid&&ref.valid);CHECK(gm[13]==ref.samples);CHECK(std::abs(gm[14]-ref.agree)<=1);CHECK(maxterm<.01f);CHECK(std::abs(gm[11]-ref.nearness_limit)<1e-5f);
        CHECK(sample_decisions<samples.size()/100);CHECK(sample_errors<samples.size()/100);
        unsigned flat=0,flat_bad=0,decisions=0,vector_bad=0,compared=0,obj=0,obj_good=0;double flat_error=0;
        for(unsigned y=16;y<h-16;++y)for(unsigned x=16;x<w-16;++x){size_t i=size_t(y)*w+x;const float px=float(x)+.5f-jx,py=float(y)+.5f-jy;float vx,vy;sm::Evaluate(fitted,px,py,std::abs(depth[i]-far_depth),w,h,vx,vy);
            auto expected=tm::MeasurePixel(ci,pi,px,py,true,raw[2*i],raw[2*i+1],{},fitted.valid,vx,vy);
            decisions+=expected.valid!=masks[i];vector_bad+=!std::isfinite(vectors[2*i])||!std::isfinite(vectors[2*i+1])||std::hypot(expected.x-vectors[2*i],expected.y-vectors[2*i+1])>.03f;++compared;
            if(tm::WindowAt(ci,px,py).contrast<tm::Params{}.texture_min&&!object(px,py)){++flat;flat_bad+=masks[i]!=.5f;float tx,ty;motion(px,py,tx,ty);flat_error+=std::hypot(vectors[2*i]-tx,vectors[2*i+1]-ty);}
            if(object(float(x),float(y))&&x>w*3/4+20&&y>h*2/3+20&&expected.valid==1){++obj;float tx,ty;motion(float(x)+.5f,float(y)+.5f,tx,ty);obj_good+=std::hypot(vectors[2*i]-tx,vectors[2*i+1]-ty)<.75f;}
        }
        std::printf("sample->fit->measure variant=%u sample_decisions=%u sample_errors=%u samples=%.0f agree=%.0f term_delta=%.7f flat=%u flat_bad=%u flat_truth_mean=%.5f pixel_decisions=%u vector_errors=%u/%u object=%u/%u\n",variant,sample_decisions,sample_errors,gm[13],gm[14],maxterm,flat,flat_bad,flat_error/(std::max)(flat,1u),decisions,vector_bad,compared,obj_good,obj);
        CHECK(flat>compared/2);CHECK(flat_bad==0);CHECK(flat_error/flat<.25);CHECK(decisions<compared/100);CHECK(vector_bad<compared/100);if(variant==1){CHECK(obj>100);CHECK(obj_good>obj*9/10);}
        g.Reset();
        // Same picture and raw field, no model: flat pixels must become unknown, never raw noise.
        p.Srv(3,2,nullptr,DXGI_FORMAT_R32G32_FLOAT);p.Dispatch(3,c,(w+7)/8,(h+7)/8);
        ro=g.Read(output.Get(),w,h,DXGI_FORMAT_R32G32_FLOAT,8,U);rvv=g.Read(mask.Get(),w,h,DXGI_FORMAT_R32_FLOAT,4,U);REQUIRE(g.Execute()==0);
        vectors=g.Floats(ro.Get(),w,h,2);masks=g.Floats(rvv.Get(),w,h,1);unsigned unknown=0;
        for(unsigned y=16;y<h-16;++y)for(unsigned x=16;x<w-16;++x){size_t i=size_t(y)*w+x;if(tm::WindowAt(ci,float(x)+.5f-jx,float(y)+.5f-jy).contrast<tm::Params{}.texture_min){CHECK(masks[i]==0&&vectors[2*i]==0&&vectors[2*i+1]==0);++unknown;}}
        std::printf("no model variant=%u flat unknown=%u\n",variant,unknown);g.Reset();
    }
    return 0;
}

inline int ProductionBenchmark(test::Device& d,unsigned w,unsigned h,bool overhead=false) {
    namespace cf=na::j2::compute_flow;Gpu g(d);std::vector<float> rgba(size_t(w)*h*4),depth(size_t(w)*h,.03f);ComPtr<ID3D12Resource> image[2];
    for(unsigned f=0;f<2;++f) {
        for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){float px=float(x)-3.f*f;
            bool textured=(x/96+y/64)%3==0;float v=textured?test_pictures::NonRepeating(px,float(y)):.012f;size_t i=(size_t(y)*w+x)*4;rgba[i]=rgba[i+1]=rgba[i+2]=v;rgba[i+3]=1;}
        image[f]=g.Upload(w,h,DXGI_FORMAT_R32G32B32A32_FLOAT,16,rgba.data());
    }
    auto dep=g.Upload(w,h,DXGI_FORMAT_R32_FLOAT,4,depth.data());REQUIRE(g.Execute()==0);g.Reset();na::motion::gpu::Inputs in;in.depth=dep.Get();in.depth_clear=0;
    auto cut_output=overhead?test::CreateTexture(d.device.Get(),w,h,DXGI_FORMAT_R32G32B32A32_FLOAT,U,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS):ComPtr<ID3D12Resource>{};
    auto timing=overhead?test::CreateBuffer(d.device.Get(),16,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST):ComPtr<ID3D12Resource>{};
    ComPtr<ID3D12QueryHeap> queries;
    UINT64 frequency=0;
    if(overhead){
        const D3D12_QUERY_HEAP_DESC query_desc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP,2,0};
        REQUIRE(cut_output&&timing&&SUCCEEDED(d.device->CreateQueryHeap(&query_desc,IID_PPV_ARGS(&queries))));
        REQUIRE(SUCCEEDED(d.queue->GetTimestampFrequency(&frequency))&&frequency);
    }
    for(bool enabled:{false,true,true,false}) {
        if(overhead&&!enabled)continue;
        cf::Reset();cf::SetScreenFit(enabled);std::vector<double> ms,record_us,cut_ms;
        const auto start=std::chrono::steady_clock::now();
        for(unsigned i=0;i<26;++i){cf::Frame f;f.frame=9000+i;f.previous=i?f.frame-1:0;f.generation=303;f.source_generation=303;f.total=true;f.raster_sample=f.raster_confirmed=true;
            const auto record_start=std::chrono::steady_clock::now();
            cf::Tier3 t;auto ticket=cf::Record(g.l.list.Get(),d.queue.Get(),image[i%2].Get(),dep.Get(),in,f,t);REQUIRE(ticket.context);CHECK(ticket.pair==(i!=0));
            if(overhead&&i>=6){
                record_us.push_back(std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-record_start).count());
                auto* list=g.l.list.Get();
                list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
                cf::RecordCutShow(list,ticket,image[i%2].Get(),cut_output.Get());
                list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);
                list->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,timing.Get(),0);
            }
            ComPtr<ID3D12Resource> rx,rv;if(i==25){REQUIRE(t.total&&t.total_valid);rx=g.Read(t.total,w,h,DXGI_FORMAT_R32G32_FLOAT,8);rv=g.Read(t.total_valid,w,h,DXGI_FORMAT_R32_FLOAT,4);}
            REQUIRE(g.Execute()==0);cf::Committed(ticket);g.Reset();const auto s=cf::GetStats();
            if(overhead&&i>=6){
                UINT64* times=nullptr;const D3D12_RANGE range{0,16},none{};
                REQUIRE(SUCCEEDED(timing->Map(0,&range,reinterpret_cast<void**>(&times))));
                cut_ms.push_back(double(times[1]-times[0])*1000.0/double(frequency));
                timing->Unmap(0,&none);
                CHECK(!ticket.cut_reset&&s.total_cuts==0);
            }
            if(i==25){auto x=g.Floats(rx.Get(),w,h,2),v=g.Floats(rv.Get(),w,h,1);unsigned n=0,bad=0;double error=0;
                for(unsigned y=16;y<h-16;++y)for(unsigned xx=16;xx<w-16;++xx){if((xx/96+y/64)%3==0||xx%96<12||xx%96>83||y%64<12||y%64>51)continue;size_t j=size_t(y)*w+xx;++n;
                    if(enabled){const double e=std::hypot(x[2*j]+3.f,x[2*j+1]);error+=e;bad+=v[j]!=.5f||!std::isfinite(e)||e>.25;}
                    else bad+=v[j]!=0||x[2*j]!=0||x[2*j+1]!=0;}
                std::printf("production flat pixels %ux%u on=%d checked=%u bad=%u mean_truth_error=%.6f\n",w,h,enabled,n,bad,error/(std::max)(n,1u));CHECK(n>w*h/5);CHECK(bad==0);}
            if(i>=6){CHECK(s.screen_on==enabled);CHECK(s.ms>0);if(enabled)CHECK(s.screen_last_valid);ms.push_back(s.ms);}}
        std::sort(ms.begin(),ms.end());auto s=cf::GetStats();
        std::printf("screen fit timing %ux%u on=%d samples=%zu median=%.4f min=%.4f max=%.4f ms model=%d samples=%.0f agree=%.0f steps=%u\n",w,h,enabled,ms.size(),(ms[9]+ms[10])*.5,ms.front(),ms.back(),s.screen_last_valid,s.screen_samples,s.screen_agree,s.screen_steps);
        CHECK(s.screen_pairs==(enabled?25u:0u));
        if(overhead){
            std::sort(record_us.begin(),record_us.end());std::sort(cut_ms.begin(),cut_ms.end());
            std::printf("steady %ux%u: record median %.2f us p95 %.2f us; ordinary cut-show median %.4f ms p95 %.4f ms; block wall %.2f s\n",
                        w,h,(record_us[9]+record_us[10])*.5,record_us[18],(cut_ms[9]+cut_ms[10])*.5,cut_ms[18],
                        std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count());
        }
    }
    cf::Reset();cf::SetScreenFit(true);return 0;
}
} // namespace screen_audit
