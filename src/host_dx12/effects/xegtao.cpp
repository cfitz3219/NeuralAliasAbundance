#include "host_dx12/background/work.hpp"
#include "xegtao.hpp"
#include "host_dx12/gpu/services.hpp"
#include "host_dx12/gpu/mem_alloc.hpp"
#include "host_dx12/settings.hpp"
#include "host_dx12/observe/producers.hpp"
#include "providers/camera/camera_tracker.hpp"
#include <wrl/client.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "../../../third_party/xegtao/XeGTAO.h"
#if defined(NA_HAVE_XEGTAO)
#include "Extract.h"
#include "Prefilter.h"
#include "Normals.h"
#include "Main.h"
#include "Denoise.h"
#include "Temporal.h"
#include "Compose.h"
#endif

namespace na::xegtao {
using Microsoft::WRL::ComPtr;
namespace {
constexpr auto Read=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto Write=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
// Tables: 0 extract, 1 prefilter, 2 normals, 3 main, 4-6 denoise, 7/10 compose
// (history parity 0/1), 8/9 temporal (parity 0/1).
constexpr unsigned Tables=11, Srvs=5, Uavs=5, TableSize=Srvs+Uavs;
constexpr unsigned kCompose[2]={7,10}, kTemporal[2]={8,9};
struct Constants {
    XeGTAO::GTAOConstants gtao{};
    uint32_t color_base[2]{},depth_base[2]{};
    float amount=0,clear=0;uint32_t quality=0,srgb=0;
    uint32_t output_base[2]{},final_apply=1,padding=0;
    float temporal_blend=1,jitter_delta[2]{},depth_tolerance=.06f;
};
static_assert(sizeof(Constants)==160,"HLSL constants layout");
void move(ID3D12GraphicsCommandList* l,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
    if(a==b)return;
    D3D12_RESOURCE_BARRIER x{};x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};l->ResourceBarrier(1,&x);
}
struct Programs {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso[Tables];
    std::atomic<int> ready{0};std::mutex mutex;std::condition_variable cv;
    std::string error;
};
bool build_programs(Programs& p) {
#if !defined(NA_HAVE_XEGTAO)
    p.error="XeGTAO shaders were not included in this build (DXC required).";return false;
#else
    na::observe::HostWorkScope own;
    D3D12_DESCRIPTOR_RANGE ranges[]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,Srvs,0,0,0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV,Uavs,0,0,Srvs}};
    D3D12_ROOT_PARAMETER rp[2]{};rp[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants={0,0,sizeof(Constants)/4};rp[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable={2,ranges};
    D3D12_STATIC_SAMPLER_DESC ss{};ss.Filter=D3D12_FILTER_MIN_MAG_MIP_POINT;
    ss.AddressU=ss.AddressV=ss.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.MaxLOD=D3D12_FLOAT32_MAX;ss.MaxAnisotropy=1;ss.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;
    D3D12_ROOT_SIGNATURE_DESC rd{2,rp,1,&ss,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    static_assert(sizeof(Constants)/4+1<=64,"root signature size");ComPtr<ID3DBlob> blob,error;
    if(FAILED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error))||
       FAILED(p.device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&p.root)))) {
        p.error="XeGTAO root signature could not be prepared.";return false;
    }
    const D3D12_SHADER_BYTECODE shaders[]={
        {na_xegtao_Extract,sizeof(na_xegtao_Extract)}, {na_xegtao_Prefilter,sizeof(na_xegtao_Prefilter)},
        {na_xegtao_Normals,sizeof(na_xegtao_Normals)}, {na_xegtao_Main,sizeof(na_xegtao_Main)},
        {na_xegtao_Denoise,sizeof(na_xegtao_Denoise)}, {na_xegtao_Denoise,sizeof(na_xegtao_Denoise)},
        {na_xegtao_Denoise,sizeof(na_xegtao_Denoise)}, {na_xegtao_Compose,sizeof(na_xegtao_Compose)},
        {na_xegtao_Temporal,sizeof(na_xegtao_Temporal)}, {na_xegtao_Temporal,sizeof(na_xegtao_Temporal)},
        {na_xegtao_Compose,sizeof(na_xegtao_Compose)}};
    for(unsigned i=0;i<Tables;++i){
        if(i==5||i==6){p.pso[i]=p.pso[4];continue;}
        if(i==9){p.pso[i]=p.pso[8];continue;}
        if(i==10){p.pso[i]=p.pso[7];continue;}
        D3D12_COMPUTE_PIPELINE_STATE_DESC d{};d.pRootSignature=p.root.Get();d.CS=shaders[i];
        if(FAILED(p.device->CreateComputePipelineState(&d,IID_PPV_ARGS(&p.pso[i])))){
            p.error="XeGTAO pipeline could not be prepared.";return false;
        }
    }
    return true;
#endif
}
struct Scratch {
    na::gpu::Texture raw,pyramid,normals,ao,edges,filtered,color,encoded;
    na::gpu::Texture history[2],history_depth[2]; // temporal visibility + its view depth (ping-pong)
    unsigned next=0;bool history_valid=false;float last_jitter[2]{};
    bool nvrhi=false;
    ComPtr<ID3D12CommandQueue> queue; // absent => this scratch belongs to only one recording
    uint32_t w=0,h=0;DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    uint32_t ow=0,oh=0;
};
bool texture(ID3D12Device* dev,ID3D12CommandQueue* queue,uint32_t w,uint32_t h,DXGI_FORMAT format,unsigned mips,na::gpu::Texture& out) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;d.DepthOrArraySize=1;
    d.MipLevels=static_cast<UINT16>(mips);d.Format=format;d.SampleDesc.Count=1;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return na::gpu::CreateTexture(dev,queue,d,Read,out);
}
bool covered(ID3D12Resource* r,uint32_t x,uint32_t y,uint32_t w,uint32_t h) {
    if(!r)return false;const auto d=r->GetDesc();
    return d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&d.DepthOrArraySize==1&&d.SampleDesc.Count==1&&
        x<=d.Width&&w<=d.Width-x&&y<=d.Height&&h<=d.Height-y;
}
struct Counters {std::atomic<uint64_t> recorded{0};};
}
struct Frame::Work {
    std::shared_ptr<Programs> programs;
    std::shared_ptr<Scratch> scratch;
    std::shared_ptr<Counters> counters;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12Resource> input_color,input_depth;
    ComPtr<ID3D12Resource> copied_depth;
    D3D12_RESOURCE_STATES depth_state=Read;
    bool copy_depth=false;
    unsigned denoise=1,parity=0;
    ComPtr<ID3D12Resource> input_motion;ID3D12Resource* bound_motion=nullptr;
    Constants constants;
    ID3D12Resource* bound_color=nullptr;ID3D12Resource* bound_depth=nullptr;Scratch* bound_scratch=nullptr;
    DXGI_FORMAT bound_color_format=DXGI_FORMAT_UNKNOWN,bound_depth_format=DXGI_FORMAT_UNKNOWN;unsigned bound_denoise=0;
    unsigned stride=0;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(unsigned table,unsigned descriptor)const {
        auto h=heap->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(table*TableSize+descriptor)*stride;return h;
    }
    void srv(unsigned t,unsigned n,ID3D12Resource* r,DXGI_FORMAT f,unsigned mips=1) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=f;d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;d.Texture2D.MipLevels=mips;
        programs->device->CreateShaderResourceView(r,&d,cpu(t,n));
    }
    void uav(unsigned t,unsigned n,ID3D12Resource* r,DXGI_FORMAT f,unsigned mip=0) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};d.Format=f;d.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;d.Texture2D.MipSlice=mip;
        programs->device->CreateUnorderedAccessView(r,nullptr,&d,cpu(t,Srvs+n));
    }
};
struct Pass::State {
    mutable std::mutex mutex;
    std::shared_ptr<Programs> programs;
    std::shared_ptr<Scratch> scratch;
    std::vector<std::shared_ptr<Frame>> frames;
    std::shared_ptr<Counters> counters=std::make_shared<Counters>();
    uint64_t allocations=0,busy=0,bindings=0;std::string reason="Waiting for the first scene.";
};
Frame::Frame():work_(std::make_unique<Work>()){}
Frame::~Frame()=default;
Pass::Pass():state_(std::make_unique<State>()){}
Pass::~Pass()=default;
ID3D12Resource* Frame::Color()const{return work_->scratch->encoded?work_->scratch->encoded.Get():work_->scratch->color.Get();}
ID3D12Resource* Frame::Occlusion()const{return work_->denoise==2?work_->scratch->ao.Get():work_->scratch->filtered.Get();}
ID3D12Resource* Frame::ViewDepthForTests()const{return work_->scratch->pyramid.Get();}
ID3D12Resource* Frame::NormalsForTests()const{return work_->scratch->normals.Get();}

bool FromViewProjection(const double* data,Projection& p) {
    if(!data)return false;
    na::camera::Matrix m{};std::copy(data,data+16,m.begin());na::camera::VpGeometry g{};
    if(!na::camera::DecomposeViewProjection(m,g))return false;
    double norm=0,dot=0;for(unsigned i=0;i<3;++i){norm+=m[12+i]*m[12+i];dot+=m[8+i]*m[12+i];}
    const double a=dot/norm,b=(m[11]-a*m[15])/std::sqrt(norm);
    if(!std::isfinite(a)||!std::isfinite(b)||std::abs(b)<1e-12)return false;
    p={float(g.sx),float(g.sy),float(g.dx),float(g.dy),float(a),float(b)};
    return true;
}
std::shared_ptr<Frame> Pass::Prepare(ID3D12Device* dev,ID3D12CommandQueue* queue,const Inputs& in,const Options& options) {
    if(!options.enabled||!options.amount)return {};
    auto& s=*state_;std::lock_guard<std::mutex> lock(s.mutex);
    const auto& p=in.projection;
    if(!dev||in.width<16||in.height<16||!covered(in.color,in.color_x,in.color_y,in.width,in.height)||
       !covered(in.depth,in.depth_x,in.depth_y,in.width,in.height)||!(p.sx>0&&p.sy>0)||!std::isfinite(p.sx)||!std::isfinite(p.sy)||
       !std::isfinite(p.a)||!std::isfinite(p.b)||std::abs(p.b)<1e-12f||
       !std::isfinite(p.dx)||!std::isfinite(p.dy)||!std::isfinite(in.jitter_x)||!std::isfinite(in.jitter_y)||std::abs(in.jitter_x)>.5f||std::abs(in.jitter_y)>.5f||
       !std::isfinite(options.radius)||options.radius<=0) {
        s.reason="Waiting for matching scene depth and camera.";return {};
    }
    const DXGI_FORMAT format=in.color_format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB?DXGI_FORMAT_R8G8B8A8_UNORM:in.color_format;
    if(format!=DXGI_FORMAT_R16G16B16A16_FLOAT&&format!=DXGI_FORMAT_R8G8B8A8_UNORM&&
       format!=DXGI_FORMAT_R32G32B32A32_FLOAT&&format!=DXGI_FORMAT_R11G11B10_FLOAT){
        s.reason="Scene colour format is not supported by XeGTAO yet.";return {};
    }
    na::observe::HostWorkScope own;
    if(!s.programs||s.programs->device.Get()!=dev){
        s.programs=std::make_shared<Programs>();s.programs->device=dev;s.scratch.reset();s.frames.clear();
        const auto programs=s.programs;
        if(!na::background::Submit([programs]{
            const bool ok=build_programs(*programs);
            {std::lock_guard<std::mutex> lock(programs->mutex);programs->ready.store(ok?1:-1,std::memory_order_release);}
            programs->cv.notify_all();
        })){s.programs.reset();s.reason="Waiting for background preparation.";return {};}
    }
    const int ready=s.programs->ready.load(std::memory_order_acquire);
    if(ready<=0){s.reason=ready<0?s.programs->error:"Preparing XeGTAO in the background.";return {};}
    std::shared_ptr<Frame> frame;
    for(const auto& f:s.frames)if(f.use_count()==1){frame=f;break;}
    if(!frame){
        if(s.frames.size()>=12){++s.busy;s.reason="Waiting for an earlier scene to finish.";return {};}
        frame=std::shared_ptr<Frame>(new Frame);s.frames.push_back(frame);++s.allocations;
    }
    auto& w=*frame->work_;w.programs=s.programs;w.counters=s.counters;
    const bool shared=queue!=nullptr;
    auto scratch=shared?s.scratch:w.scratch;
    const auto cd=in.color->GetDesc();
    const auto ow=in.preserve_color_extent?uint32_t(cd.Width):in.width,oh=in.preserve_color_extent?cd.Height:in.height;
    if(!scratch||scratch->w!=in.width||scratch->h!=in.height||scratch->ow!=ow||scratch->oh!=oh||scratch->format!=format||scratch->queue.Get()!=queue||bool(scratch->encoded)!=in.srgb||scratch->nvrhi!=na::gpu::Enabled()){
        scratch=std::make_shared<Scratch>();scratch->w=in.width;scratch->h=in.height;scratch->format=format;scratch->queue=queue;
        scratch->ow=ow;scratch->oh=oh;scratch->nvrhi=na::gpu::Enabled();
        const auto make=[&](DXGI_FORMAT f,unsigned mips,na::gpu::Texture& r){return texture(dev,queue,in.width,in.height,f,mips,r);};
        if(!make(DXGI_FORMAT_R32_FLOAT,1,scratch->raw)||!make(DXGI_FORMAT_R32_FLOAT,5,scratch->pyramid)||
           !make(DXGI_FORMAT_R32_UINT,1,scratch->normals)||!make(DXGI_FORMAT_R8_UINT,1,scratch->ao)||
           !make(DXGI_FORMAT_R8_UNORM,1,scratch->edges)||!make(DXGI_FORMAT_R8_UINT,1,scratch->filtered)||
           !make(DXGI_FORMAT_R16_FLOAT,1,scratch->history[0])||!make(DXGI_FORMAT_R16_FLOAT,1,scratch->history[1])||
           !make(DXGI_FORMAT_R32_FLOAT,1,scratch->history_depth[0])||!make(DXGI_FORMAT_R32_FLOAT,1,scratch->history_depth[1])||
           !texture(dev,queue,ow,oh,format,1,scratch->color)){s.reason="XeGTAO working textures could not be allocated.";return {};}
        if(in.srgb){
            auto desc=scratch->color->GetDesc();desc.Format=in.color_format;desc.Flags=D3D12_RESOURCE_FLAG_NONE;
            if(FAILED(na::gpu::mem::CreateResource(dev,D3D12_HEAP_TYPE_DEFAULT,desc,Read,nullptr,scratch->encoded.GetAddressOf()))){
                s.reason="XeGTAO sRGB output could not be allocated.";return {};
            }
        }
        if(shared)s.scratch=scratch;
    }
    w.scratch=scratch;w.input_color=in.color;w.input_depth=in.depth;
    w.copy_depth=in.copy_depth;w.depth_state=in.depth_state;
    if(in.copy_depth){
        auto dd=in.depth->GetDesc();
        if(dd.MipLevels!=1){s.reason="XeGTAO needs the selected native depth mip.";return {};}
        dd.Flags=D3D12_RESOURCE_FLAG_NONE;
        switch(in.depth_format){
        case DXGI_FORMAT_R32_FLOAT:dd.Format=DXGI_FORMAT_R32_TYPELESS;break;
        case DXGI_FORMAT_R16_UNORM:dd.Format=DXGI_FORMAT_R16_TYPELESS;break;
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:dd.Format=DXGI_FORMAT_R24G8_TYPELESS;break;
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:dd.Format=DXGI_FORMAT_R32G8X24_TYPELESS;break;
        default:s.reason="Native depth format is unavailable for XeGTAO.";return {};
        }
        const auto old=w.copied_depth?w.copied_depth->GetDesc():D3D12_RESOURCE_DESC{};
        if(old.Width!=dd.Width||old.Height!=dd.Height||old.Format!=dd.Format){
            D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;w.copied_depth.Reset();
            if(FAILED(dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&dd,Read,nullptr,IID_PPV_ARGS(&w.copied_depth)))){
                s.reason="Native depth copy could not be allocated.";return {};
            }
        }
    }
    if(!w.heap){
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,Tables*TableSize,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
        if(FAILED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&w.heap)))){s.reason="XeGTAO descriptors could not be allocated.";return {};}
        w.stride=dev->GetDescriptorHandleIncrementSize(hd.Type);
    }
    XeGTAO::GTAOSettings settings;settings.Radius=options.radius;settings.DenoisePasses=int(std::min(options.denoise,3u));
    settings.RadiusMultiplier=options.radius_multiplier;settings.FalloffRange=options.falloff;
    settings.SampleDistributionPower=options.sample_distribution;settings.ThinOccluderCompensation=options.thin_occluder;
    settings.FinalValuePower=options.final_power;settings.DepthMIPSamplingOffset=options.depth_mip_offset;
    w.denoise=std::max(1u,std::min(options.denoise,3u));
    const float projection[16]={p.sx,0,0,0,0,p.sy,0,0,p.dx,p.dy,p.a,1,0,0,p.b,0};
    auto& c=w.constants;XeGTAO::GTAOUpdateConstants(c.gtao,int(in.width),int(in.height),settings,projection,true,0);
    c.gtao.DepthUnpackConsts={-p.b,p.a}; // exact forward/reversed/infinite-Z convention
    const float dx=p.dx+2*in.jitter_x/in.width,dy=p.dy-2*in.jitter_y/in.height;
    c.gtao.NDCToViewAdd={(-1-dx)/p.sx,(1-dy)/p.sy};
    c.color_base[0]=in.color_x;c.color_base[1]=in.color_y;c.depth_base[0]=in.depth_x;c.depth_base[1]=in.depth_y;
    c.amount=std::min(options.amount,100u)*.01f;c.clear=in.clear_depth;c.quality=std::min(options.quality,3u);c.srgb=in.srgb?1u:0u;
    c.output_base[0]=in.preserve_color_extent?in.color_x:0;c.output_base[1]=in.preserve_color_extent?in.color_y:0;
    // Temporal history: only with motion vectors of this extent, a shared
    // (GPU-ordered) scratch and a history that belongs to the same scene.
    const bool motion_ok=in.motion&&covered(in.motion,0,0,in.width,in.height);
    const float keep=std::isfinite(options.temporal)?std::clamp(options.temporal,0.f,.95f):0.f;
    const bool use_history=shared&&motion_ok&&keep>0&&scratch->history_valid&&!in.reset;
    c.temporal_blend=use_history?1.f-keep:1.f;
    c.jitter_delta[0]=use_history?scratch->last_jitter[0]-in.jitter_x:0.f;c.jitter_delta[1]=use_history?scratch->last_jitter[1]-in.jitter_y:0.f;
    w.parity=scratch->next;scratch->next^=1u;
    scratch->last_jitter[0]=in.jitter_x;scratch->last_jitter[1]=in.jitter_y;scratch->history_valid=shared;
    w.input_motion=motion_ok?in.motion:nullptr;
    // Immutable binding-set cache (NVRHI pattern). This packet is exclusively
    // reusable only after its existing recording/fence owners release it.
    auto* depth_input=in.copy_depth?w.copied_depth.Get():in.depth;
    if(w.bound_color!=in.color||w.bound_depth!=depth_input||w.bound_scratch!=scratch.get()||
       w.bound_color_format!=in.color_format||w.bound_depth_format!=in.depth_format||w.bound_denoise!=w.denoise||
       w.bound_motion!=w.input_motion.Get()){
    for(unsigned t=0;t<Tables;++t){for(unsigned i=0;i<Srvs;++i)w.srv(t,i,nullptr,DXGI_FORMAT_R32_FLOAT);
        for(unsigned i=0;i<Uavs;++i)w.uav(t,i,nullptr,DXGI_FORMAT_R32_FLOAT);}
    w.srv(0,0,in.copy_depth?w.copied_depth.Get():in.depth,in.depth_format);w.uav(0,0,scratch->raw.Get(),DXGI_FORMAT_R32_FLOAT);
    w.srv(1,0,scratch->raw.Get(),DXGI_FORMAT_R32_FLOAT);
    for(unsigned m=0;m<5;++m)w.uav(1,m,scratch->pyramid.Get(),DXGI_FORMAT_R32_FLOAT,m);
    w.srv(2,0,scratch->raw.Get(),DXGI_FORMAT_R32_FLOAT);w.uav(2,0,scratch->normals.Get(),DXGI_FORMAT_R32_UINT);
    w.srv(3,0,scratch->pyramid.Get(),DXGI_FORMAT_R32_FLOAT,5);w.srv(3,1,scratch->normals.Get(),DXGI_FORMAT_R32_UINT);
    w.uav(3,0,scratch->ao.Get(),DXGI_FORMAT_R8_UINT);w.uav(3,1,scratch->edges.Get(),DXGI_FORMAT_R8_UNORM);
    w.srv(4,0,scratch->ao.Get(),DXGI_FORMAT_R8_UINT);w.srv(4,1,scratch->edges.Get(),DXGI_FORMAT_R8_UNORM);
    w.uav(4,0,scratch->filtered.Get(),DXGI_FORMAT_R8_UINT);
    w.srv(5,0,scratch->filtered.Get(),DXGI_FORMAT_R8_UINT);w.srv(5,1,scratch->edges.Get(),DXGI_FORMAT_R8_UNORM);
    w.uav(5,0,scratch->ao.Get(),DXGI_FORMAT_R8_UINT);
    w.srv(6,0,scratch->ao.Get(),DXGI_FORMAT_R8_UINT);w.srv(6,1,scratch->edges.Get(),DXGI_FORMAT_R8_UNORM);
    w.uav(6,0,scratch->filtered.Get(),DXGI_FORMAT_R8_UINT);
    auto* final_ao=w.denoise==2?scratch->ao.Get():scratch->filtered.Get();
    for(unsigned k=0;k<2;++k){
        const unsigned t=kTemporal[k];
        w.srv(t,0,final_ao,DXGI_FORMAT_R8_UINT);w.srv(t,1,scratch->history[k^1].Get(),DXGI_FORMAT_R16_FLOAT);
        w.srv(t,2,scratch->history_depth[k^1].Get(),DXGI_FORMAT_R32_FLOAT);w.srv(t,3,scratch->pyramid.Get(),DXGI_FORMAT_R32_FLOAT,1);
        w.srv(t,4,w.input_motion.Get(),DXGI_FORMAT_R32G32_FLOAT);
        w.uav(t,0,scratch->history[k].Get(),DXGI_FORMAT_R16_FLOAT);w.uav(t,1,scratch->history_depth[k].Get(),DXGI_FORMAT_R32_FLOAT);
        const unsigned o=kCompose[k];
        w.srv(o,0,in.color,in.color_format);w.srv(o,1,scratch->history[k].Get(),DXGI_FORMAT_R16_FLOAT);
        w.srv(o,2,scratch->raw.Get(),DXGI_FORMAT_R32_FLOAT);w.uav(o,0,scratch->color.Get(),format);
    }
    w.bound_motion=w.input_motion.Get();
    w.bound_color=in.color;w.bound_depth=depth_input;w.bound_scratch=scratch.get();
    w.bound_color_format=in.color_format;w.bound_depth_format=in.depth_format;w.bound_denoise=w.denoise;++s.bindings;
    }
    s.reason="Ready";return frame;
}
bool Frame::Record(ID3D12GraphicsCommandList* l) {
    if(!l||(l->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT&&l->GetType()!=D3D12_COMMAND_LIST_TYPE_COMPUTE))return false;
    auto& w=*work_;auto& s=*w.scratch;na::observe::HostWorkScope own;
    if(w.copy_depth){
        move(l,w.input_depth.Get(),w.depth_state,D3D12_RESOURCE_STATE_COPY_SOURCE);
        move(l,w.copied_depth.Get(),Read,D3D12_RESOURCE_STATE_COPY_DEST);
        l->CopyResource(w.copied_depth.Get(),w.input_depth.Get());
        move(l,w.copied_depth.Get(),D3D12_RESOURCE_STATE_COPY_DEST,Read);
        move(l,w.input_depth.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,w.depth_state);
    }
    ID3D12DescriptorHeap* heaps[]={w.heap.Get()};l->SetDescriptorHeaps(1,heaps);l->SetComputeRootSignature(w.programs->root.Get());
    l->SetComputeRoot32BitConstants(0,sizeof(Constants)/4,&w.constants,0);
    const auto dispatch=[&](unsigned table,unsigned x,unsigned y){
        auto h=w.heap->GetGPUDescriptorHandleForHeapStart();h.ptr+=UINT64(table*TableSize)*w.stride;
        l->SetComputeRootDescriptorTable(1,h);l->SetPipelineState(w.programs->pso[table].Get());l->Dispatch(x,y,1);
    };
    const auto gx=(s.w+7)/8,gy=(s.h+7)/8;
    move(l,s.raw.Get(),Read,Write);dispatch(0,gx,gy);move(l,s.raw.Get(),Write,Read);
    move(l,s.pyramid.Get(),Read,Write);dispatch(1,(s.w+15)/16,(s.h+15)/16);move(l,s.pyramid.Get(),Write,Read);
    move(l,s.normals.Get(),Read,Write);dispatch(2,gx,gy);move(l,s.normals.Get(),Write,Read);
    move(l,s.ao.Get(),Read,Write);move(l,s.edges.Get(),Read,Write);dispatch(3,gx,gy);
    move(l,s.ao.Get(),Write,Read);move(l,s.edges.Get(),Write,Read);
    for(unsigned i=0;i<w.denoise;++i){
        auto* destination=i==1?s.ao.Get():s.filtered.Get();
        l->SetComputeRoot32BitConstant(0,i+1==w.denoise?1u:0u,34);
        move(l,destination,Read,Write);dispatch(4+i,(s.w+15)/16,gy);move(l,destination,Write,Read);
    }
    {   // Temporal visibility (an exact copy of this frame's AO when no history applies).
        auto* h=s.history[w.parity].Get();auto* hd=s.history_depth[w.parity].Get();
        move(l,h,Read,Write);move(l,hd,Read,Write);dispatch(kTemporal[w.parity],gx,gy);move(l,h,Write,Read);move(l,hd,Write,Read);
    }
    move(l,s.color.Get(),Read,Write);dispatch(kCompose[w.parity],gx,gy);move(l,s.color.Get(),Write,Read);
    if(s.encoded){
        move(l,s.color.Get(),Read,D3D12_RESOURCE_STATE_COPY_SOURCE);move(l,s.encoded.Get(),Read,D3D12_RESOURCE_STATE_COPY_DEST);
        l->CopyResource(s.encoded.Get(),s.color.Get());
        move(l,s.color.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,Read);move(l,s.encoded.Get(),D3D12_RESOURCE_STATE_COPY_DEST,Read);
    }
    w.counters->recorded.fetch_add(1,std::memory_order_relaxed);return true;
}
Stats Pass::GetStats()const {
    auto& s=*state_;std::lock_guard<std::mutex> lock(s.mutex);Stats stats;
    stats.recorded=s.counters->recorded.load();stats.allocations=s.allocations;stats.busy=s.busy;stats.bindings=s.bindings;
    stats.compiling=s.programs&&s.programs->ready.load(std::memory_order_acquire)==0;
    const bool failed=s.programs&&s.programs->ready.load(std::memory_order_acquire)<0;
    std::snprintf(stats.reason,sizeof(stats.reason),"%s",failed?s.programs->error.c_str():s.reason.c_str());return stats;
}
bool Pass::WaitPrepared(uint32_t milliseconds) {
    std::shared_ptr<Programs> p;{std::lock_guard<std::mutex> lock(state_->mutex);p=state_->programs;}
    if(!p)return false;std::unique_lock<std::mutex> lock(p->mutex);
    return p->cv.wait_for(lock,std::chrono::milliseconds(milliseconds),[&]{return p->ready.load()!=0;})&&p->ready.load()==1;
}
void Pass::Unavailable(const char* reason){std::lock_guard<std::mutex> lock(state_->mutex);state_->reason=reason;}
Pass& ScenePass(){static auto* pass=new Pass;return *pass;}
Options CurrentOptions(){const auto& s=na::settings::Get();Options o;
    o.enabled=s.xegtao_enabled.load()!=0;o.amount=s.xegtao_amount.load();o.quality=s.xegtao_quality.load();
    o.radius=s.xegtao_radius_x1000.load()*.001f;o.denoise=s.xegtao_denoise.load();
    o.radius_multiplier=s.xegtao_radius_multiplier.load()*.001f;o.falloff=s.xegtao_falloff.load()*.001f;
    o.sample_distribution=s.xegtao_sample_distribution.load()*.001f;o.thin_occluder=s.xegtao_thin_occluder.load()*.001f;
    o.final_power=s.xegtao_final_power.load()*.001f;o.depth_mip_offset=s.xegtao_depth_mip_offset.load()*.001f;
    o.temporal=s.xegtao_temporal.load()*.001f;return o;
}
} // namespace na::xegtao
