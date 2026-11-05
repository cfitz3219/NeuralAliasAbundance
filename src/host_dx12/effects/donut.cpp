#include "donut.hpp"
#include "host_dx12/gpu/services.hpp"
#include "host_dx12/settings.hpp"
#include "host_dx12/observe/producers.hpp"
#include <wrl/client.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#if defined(NA_HAVE_DONUT)
#include "DonutClear.h"
#include "DonutHistogram.h"
#include "DonutExposure.h"
#include "DonutDownsample.h"
#include "DonutBlur.h"
#include "DonutCompose.h"
#endif
namespace na::donut {
using Microsoft::WRL::ComPtr;
namespace {
constexpr auto Read=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,Write=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
struct Constants {
    uint32_t size[2],origin[2],blurSize[2],outputOrigin[2];
    uint32_t srgb,tone,bloom,curve;
    float amount,exposureBias,white,seconds,speedUp,speedDown,low,high,minLuma,maxLuma,bloomAmount,sigma,threshold;
    uint32_t reset,horizontal,padding;
};
static_assert(sizeof(Constants)==112);
void transition(ID3D12GraphicsCommandList* l,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
    if(a==b)return;D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};l->ResourceBarrier(1,&barrier);
}
void uav(ID3D12GraphicsCommandList* l,ID3D12Resource* r) {
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;b.UAV.pResource=r;l->ResourceBarrier(1,&b);
}
struct Programs {
    ComPtr<ID3D12Device> device;ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pso[6];
    std::atomic<int> ready{0};std::mutex mutex;std::condition_variable cv;std::string error;
};
bool build(Programs& p) {
#if !defined(NA_HAVE_DONUT)
    p.error="Donut shaders were not included (DXC required).";return false;
#else
    na::observe::HostWorkScope own;
    D3D12_DESCRIPTOR_RANGE ranges[]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,3,0,0,2}};
    D3D12_ROOT_PARAMETER rp[2]{};rp[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants={0,0,sizeof(Constants)/4};rp[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;rp[1].DescriptorTable={2,ranges};
    D3D12_STATIC_SAMPLER_DESC ss{};ss.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU=ss.AddressV=ss.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;ss.MaxLOD=D3D12_FLOAT32_MAX;ss.MaxAnisotropy=1;ss.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;
    D3D12_ROOT_SIGNATURE_DESC rd{2,rp,1,&ss,D3D12_ROOT_SIGNATURE_FLAG_NONE};ComPtr<ID3DBlob> blob,error;
    if(FAILED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error))||FAILED(p.device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&p.root))))return false;
    const D3D12_SHADER_BYTECODE shaders[]={
        {na_donut_Clear,sizeof(na_donut_Clear)},{na_donut_Histogram,sizeof(na_donut_Histogram)},
        {na_donut_Exposure,sizeof(na_donut_Exposure)},{na_donut_Downsample,sizeof(na_donut_Downsample)},
        {na_donut_Blur,sizeof(na_donut_Blur)},{na_donut_Compose,sizeof(na_donut_Compose)}};
    for(unsigned i=0;i<6;++i){D3D12_COMPUTE_PIPELINE_STATE_DESC d{};d.pRootSignature=p.root.Get();d.CS=shaders[i];
        if(FAILED(p.device->CreateComputePipelineState(&d,IID_PPV_ARGS(&p.pso[i])))){p.error="Donut pipeline preparation failed.";return false;}}
    return true;
#endif
}
struct Scratch {
    ComPtr<ID3D12CommandQueue> queue;uint64_t stream=0;
    uint32_t w=0,h=0,ow=0,oh=0,bw=0,bh=0;DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;bool first=true;std::chrono::steady_clock::time_point last{};
    na::gpu::Texture color,encoded,blurA,blurB;ComPtr<ID3D12Resource> histogram,exposure;
    bool nvrhi=false;
};
bool texture(ID3D12Device* d,ID3D12CommandQueue* queue,uint32_t w,uint32_t h,DXGI_FORMAT f,na::gpu::Texture& r) {
    D3D12_RESOURCE_DESC td{};
    td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=w;td.Height=h;td.DepthOrArraySize=td.MipLevels=1;
    td.Format=f;td.SampleDesc.Count=1;td.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return na::gpu::CreateTexture(d,queue,td,Read,r);
}
bool buffer(ID3D12Device* d,unsigned count,ComPtr<ID3D12Resource>& r) {
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    td.Width=count*4;td.Height=td.DepthOrArraySize=td.MipLevels=1;td.SampleDesc.Count=1;td.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;td.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return SUCCEEDED(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,Write,nullptr,IID_PPV_ARGS(&r)));
}
bool shape(const Scratch& s,ID3D12CommandQueue* q,const Inputs& in,DXGI_FORMAT f,uint32_t ow,uint32_t oh) {
    return s.queue.Get()==q&&s.stream==in.stream&&s.w==in.width&&s.h==in.height&&s.ow==ow&&s.oh==oh&&s.format==f&&s.nvrhi==na::gpu::Enabled();
}
struct Counters {std::atomic<uint64_t> recorded{0};};
}
struct Frame::Work {
    std::shared_ptr<Programs> programs;std::shared_ptr<Scratch> scratch;std::shared_ptr<Counters> counters;
    ComPtr<ID3D12Resource> input;ComPtr<ID3D12DescriptorHeap> heap;
    Constants constants{};uint32_t stride=0;
    std::shared_ptr<na::gpu::ComputePacket> gpu[7];
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(unsigned table,unsigned slot)const {auto h=heap->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(table*5+slot)*stride;return h;}
};
struct Pass::State {
    mutable std::mutex mutex;std::shared_ptr<Programs> programs;std::vector<std::shared_ptr<Frame>> frames;
    std::vector<std::shared_ptr<Scratch>> scratch;std::shared_ptr<Counters> counters=std::make_shared<Counters>();
    uint64_t allocations=0,bindings=0,busy=0;std::string reason="Off. Enable an effect to begin.";
};
Frame::Frame():work_(std::make_unique<Work>()){}Frame::~Frame()=default;
Pass::Pass():state_(std::make_unique<State>()){}Pass::~Pass()=default;
ID3D12Resource* Frame::Color()const {return work_->scratch->encoded?work_->scratch->encoded.Get():work_->scratch->color.Get();}
std::shared_ptr<Frame> Pass::Prepare(ID3D12Device* device,ID3D12CommandQueue* queue,const Inputs& in,const Options& o) {
    if((!o.tone||o.amount<=0)&&(!o.bloom||o.bloom_amount<=0))return {};
    auto& s=*state_;std::lock_guard<std::mutex> lock(s.mutex);
    if(!device||!in.color||in.width<4||in.height<4)return {};
    auto td=in.color->GetDesc();const auto format=in.format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB?DXGI_FORMAT_R8G8B8A8_UNORM:in.format;
    if(td.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||td.DepthOrArraySize!=1||td.MipLevels!=1||td.SampleDesc.Count!=1||
       td.Width>UINT32_MAX||in.x>td.Width||in.width>td.Width-in.x||in.y>td.Height||in.height>td.Height-in.y||
       (format!=DXGI_FORMAT_R16G16B16A16_FLOAT&&format!=DXGI_FORMAT_R32G32B32A32_FLOAT&&format!=DXGI_FORMAT_R11G11B10_FLOAT&&format!=DXGI_FORMAT_R8G8B8A8_UNORM)){
        s.reason="Waiting for a plain scene-colour texture.";return {};}
    const float values[]={o.amount,o.exposure_bias,o.white,o.speed_up,o.speed_down,o.low,o.high,o.min_luminance,o.max_luminance,o.bloom_amount,o.bloom_sigma,o.bloom_threshold,in.seconds};
    for(float v:values)if(!std::isfinite(v)){s.reason="Invalid effect setting.";return {};}
    if(o.white<=0||o.min_luminance<=0||o.max_luminance<o.min_luminance||o.high<=o.low||o.low<0||o.high>1)return {};
    if(!s.programs||s.programs->device.Get()!=device){
        s.programs=std::make_shared<Programs>();s.programs->device=device;s.scratch.clear();s.frames.clear();auto p=s.programs;
        std::thread([p]{SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);const bool ok=build(*p);
            {std::lock_guard<std::mutex> guard(p->mutex);p->ready.store(ok?1:-1,std::memory_order_release);}p->cv.notify_all();}).detach();
    }
    if(s.programs->ready.load(std::memory_order_acquire)!=1){s.reason="Preparing Donut effects in the background.";return {};}
    na::observe::HostWorkScope own;
    std::shared_ptr<Frame> frame;for(auto& f:s.frames)if(f.use_count()==1){frame=f;break;}
    if(!frame){if(s.frames.size()>=16){++s.busy;s.reason="Waiting for submitted effect frames to finish.";return {};}
        frame=std::shared_ptr<Frame>(new Frame);s.frames.push_back(frame);}
    auto& w=*frame->work_;const unsigned ow=in.preserve_extent?unsigned(td.Width):in.width,oh=in.preserve_extent?td.Height:in.height;
    std::shared_ptr<Scratch> scratch;
    if(queue)for(auto& p:s.scratch)if(shape(*p,queue,in,format,ow,oh)){scratch=p;break;}
    if(!queue&&w.scratch&&!w.scratch->queue&&shape(*w.scratch,nullptr,in,format,ow,oh))scratch=w.scratch;
    if(!scratch){
        scratch=std::make_shared<Scratch>();scratch->queue=queue;scratch->stream=in.stream;scratch->w=in.width;scratch->h=in.height;
        scratch->ow=ow;scratch->oh=oh;scratch->nvrhi=na::gpu::Enabled();scratch->bw=(in.width+3)/4;scratch->bh=(in.height+3)/4;scratch->format=format;
        if(!texture(device,queue,ow,oh,format,scratch->color)||!buffer(device,256,scratch->histogram)||!buffer(device,1,scratch->exposure)){s.reason="Effect resources could not be allocated.";return {};}
        if(in.format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB){
            // sRGB UAVs are unsupported; create a compatible copy destination without UAV flags.
            D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;auto ed=scratch->color->GetDesc();ed.Format=in.format;ed.Flags=D3D12_RESOURCE_FLAG_NONE;
            if(FAILED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&ed,Read,nullptr,IID_PPV_ARGS(&scratch->encoded))))return {};
        }
        ++s.allocations;
        if(queue){if(s.scratch.size()>=8)s.scratch.erase(s.scratch.begin());s.scratch.push_back(scratch);}
    }
    if(o.bloom&&!scratch->blurA){if(!texture(device,queue,scratch->bw,scratch->bh,DXGI_FORMAT_R16G16B16A16_FLOAT,scratch->blurA)||
        !texture(device,queue,scratch->bw,scratch->bh,DXGI_FORMAT_R16G16B16A16_FLOAT,scratch->blurB))return {};}
    const bool rebind=!w.heap||w.input.Get()!=in.color||w.scratch!=scratch||(!w.constants.bloom&&o.bloom);
    w.programs=s.programs;w.counters=s.counters;w.input=in.color;w.scratch=scratch;
    w.constants={{in.width,in.height},{in.x,in.y},{scratch->bw,scratch->bh},{in.preserve_extent?in.x:0,in.preserve_extent?in.y:0},
        in.srgb?1u:0u,o.tone?1u:0u,o.bloom?1u:0u,(std::min)(o.curve,1u),
        std::clamp(o.amount,0.f,1.f),std::clamp(o.exposure_bias,-8.f,8.f),o.white,std::clamp(in.seconds,0.f,1.f),
        std::clamp(o.speed_up,0.f,20.f),std::clamp(o.speed_down,0.f,20.f),o.low,o.high,o.min_luminance,o.max_luminance,
        std::clamp(o.bloom_amount,0.f,1.f),std::clamp(o.bloom_sigma*.25f,1.f,16.f),(std::max)(o.bloom_threshold,0.f),in.reset?1u:0u,0,0};
    if(rebind){
        if(!w.heap){D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=30;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if(FAILED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&w.heap))))return {};w.stride=device->GetDescriptorHandleIncrementSize(hd.Type);}
        for(unsigned t=0;t<6;++t){
            auto* blur=(t==4||t==5)?scratch->blurA.Get():scratch->blurB.Get();
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=format;sd.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;sd.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;sd.Texture2D.MipLevels=1;
            device->CreateShaderResourceView(in.color,&sd,w.cpu(t,0));sd.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;device->CreateShaderResourceView(blur,&sd,w.cpu(t,1));
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};ud.Format=t==3||t==4?DXGI_FORMAT_R16G16B16A16_FLOAT:format;ud.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
            auto* dest=t==3?scratch->blurA.Get():t==4?scratch->blurB.Get():scratch->color.Get();device->CreateUnorderedAccessView(dest,nullptr,&ud,w.cpu(t,2));
            ud={};ud.Format=DXGI_FORMAT_R32_UINT;ud.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;ud.Buffer.NumElements=256;
            device->CreateUnorderedAccessView(scratch->histogram.Get(),nullptr,&ud,w.cpu(t,3));ud.Buffer.NumElements=1;device->CreateUnorderedAccessView(scratch->exposure.Get(),nullptr,&ud,w.cpu(t,4));
        }
        for(auto& packet:w.gpu)packet.reset();
#if defined(NA_HAVE_DONUT)
        if(na::gpu::Enabled()&&queue){
            const D3D12_SHADER_BYTECODE shaders[]={
                {na_donut_Clear,sizeof(na_donut_Clear)},{na_donut_Histogram,sizeof(na_donut_Histogram)},
                {na_donut_Exposure,sizeof(na_donut_Exposure)},{na_donut_Downsample,sizeof(na_donut_Downsample)},
                {na_donut_Blur,sizeof(na_donut_Blur)},{na_donut_Compose,sizeof(na_donut_Compose)}};
            for(unsigned packet=0;packet<7;++packet){const unsigned t=packet==6?3:packet;const unsigned program=packet==6?4:packet;na::gpu::ComputeBindings cb;cb.source[0]=in.color;cb.source_format[0]=format;
                cb.source[1]=(t==4||t==5)?scratch->blurA.Get():scratch->blurB.Get();cb.source_format[1]=DXGI_FORMAT_R16G16B16A16_FLOAT;
                cb.destination=t==3?scratch->blurA.Get():t==4?scratch->blurB.Get():scratch->color.Get();
                cb.destination_format=t==3||t==4?DXGI_FORMAT_R16G16B16A16_FLOAT:format;
                cb.uint_buffers[0]=scratch->histogram.Get();cb.uint_buffers[1]=scratch->exposure.Get();
                w.gpu[packet]=na::gpu::PrepareCompute(device,queue,shaders[program].pShaderBytecode,shaders[program].BytecodeLength,sizeof(Constants),cb);
            }
        }
#endif
        ++s.bindings;
    }
    s.reason=queue?"Donut scene effects ready.":"Scene effects ready. Exposure adjusts immediately on this route.";return frame;
}
bool Frame::Record(ID3D12GraphicsCommandList* l) {
    if(!l)return false;auto& w=*work_;auto& s=*w.scratch;na::observe::HostWorkScope own;
    Constants c=w.constants;c.reset|=s.first||!s.queue;
    const auto now=std::chrono::steady_clock::now();
    if(c.seconds==0)c.seconds=s.first?1.f/60:std::clamp(std::chrono::duration<float>(now-s.last).count(),0.f,1.f);
    if(c.tone){s.first=false;s.last=now;}
    l->SetComputeRootSignature(w.programs->root.Get());auto* heap=w.heap.Get();l->SetDescriptorHeaps(1,&heap);
    auto dispatch=[&](unsigned program,unsigned table,unsigned x,unsigned y){
        const auto& packet=w.gpu[program==4&&table==3?6:table];
        if(packet&&na::gpu::Dispatch(l,packet,&c,sizeof(c),x,y))return;
        // Native inline lists never enter NVRHI. Restore native bindings as an
        // entire set when a backend packet is unavailable for this dispatch.
        l->SetComputeRootSignature(w.programs->root.Get());l->SetDescriptorHeaps(1,&heap);
        l->SetPipelineState(w.programs->pso[program].Get());
        l->SetComputeRoot32BitConstants(0,sizeof(c)/4,&c,0);auto h=heap->GetGPUDescriptorHandleForHeapStart();h.ptr+=UINT64(table*5)*w.stride;
        l->SetComputeRootDescriptorTable(1,h);l->Dispatch(x,y,1);};
    if(c.tone){dispatch(0,0,1,1);uav(l,s.histogram.Get());dispatch(1,1,(s.w+63)/64,(s.h+63)/64);uav(l,s.histogram.Get());
        dispatch(2,2,1,1);uav(l,s.exposure.Get());}
    if(c.bloom){transition(l,s.blurA.Get(),Read,Write);dispatch(3,3,(s.bw+7)/8,(s.bh+7)/8);transition(l,s.blurA.Get(),Write,Read);
        c.horizontal=1;transition(l,s.blurB.Get(),Read,Write);dispatch(4,4,(s.bw+7)/8,(s.bh+7)/8);transition(l,s.blurB.Get(),Write,Read);
        // Vertical pass uses B as source, A as destination: temporarily use table3's UAV and table5's SRV via a prebuilt table below.
        c.horizontal=0;transition(l,s.blurA.Get(),Read,Write);dispatch(4,3,(s.bw+7)/8,(s.bh+7)/8);transition(l,s.blurA.Get(),Write,Read);
    }
    // Preserve unused texels/bases for native NGX subrects and compatible sRGB copies.
    if(s.ow!=s.w||s.oh!=s.h){transition(l,w.input.Get(),Read,D3D12_RESOURCE_STATE_COPY_SOURCE);transition(l,s.color.Get(),Read,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=w.input.Get();from.Type=to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.pResource=s.color.Get();
        l->CopyTextureRegion(&to,0,0,0,&from,nullptr);transition(l,w.input.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,Read);transition(l,s.color.Get(),D3D12_RESOURCE_STATE_COPY_DEST,Read);}
    transition(l,s.color.Get(),Read,Write);dispatch(5,5,(s.w+7)/8,(s.h+7)/8);transition(l,s.color.Get(),Write,Read);
    if(s.encoded){transition(l,s.color.Get(),Read,D3D12_RESOURCE_STATE_COPY_SOURCE);transition(l,s.encoded.Get(),Read,D3D12_RESOURCE_STATE_COPY_DEST);l->CopyResource(s.encoded.Get(),s.color.Get());
        transition(l,s.color.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,Read);transition(l,s.encoded.Get(),D3D12_RESOURCE_STATE_COPY_DEST,Read);}
    w.counters->recorded.fetch_add(1);return true;
}
Stats Pass::GetStats()const {auto& s=*state_;std::lock_guard<std::mutex> lock(s.mutex);Stats out;
    out.recorded=s.counters->recorded.load();out.allocations=s.allocations;out.bindings=s.bindings;out.busy=s.busy;
    out.compiling=s.programs&&s.programs->ready.load()==0;
    std::snprintf(out.reason,sizeof out.reason,"%s",s.programs&&s.programs->ready.load()<0?s.programs->error.c_str():s.reason.c_str());return out;}
bool Pass::WaitPrepared(uint32_t ms){std::shared_ptr<Programs> p;{std::lock_guard<std::mutex> l(state_->mutex);p=state_->programs;}if(!p)return false;
    std::unique_lock<std::mutex> l(p->mutex);return p->cv.wait_for(l,std::chrono::milliseconds(ms),[&]{return p->ready.load()!=0;})&&p->ready.load()==1;}
Pass& ScenePass(){static auto* pass=new Pass;return *pass;}
Options CurrentOptions(){const auto& s=na::settings::Get();Options o;o.tone=s.donut_tone.load()!=0;o.bloom=s.donut_bloom.load()!=0;
    o.amount=s.donut_amount.load()*.01f;o.exposure_bias=(int(s.donut_bias.load())-800)*.01f;o.white=s.donut_white.load()*.001f;
    o.speed_up=s.donut_speed_up.load()*.001f;o.speed_down=s.donut_speed_down.load()*.001f;o.curve=s.donut_curve.load();
    o.bloom_amount=s.donut_bloom_amount.load()*.001f;o.bloom_sigma=s.donut_bloom_sigma.load()*.001f;o.bloom_threshold=s.donut_bloom_threshold.load()*.001f;return o;}
}
