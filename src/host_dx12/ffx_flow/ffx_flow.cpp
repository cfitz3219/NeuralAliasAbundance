#include "host_dx12/ffx_flow/ffx_flow.hpp"
#include "host_dx12/ffx_flow/ffx_flow_shaders.hpp"
#include <array>
#include <vector>
#include <algorithm>
#include <cstring>

namespace na::ffx_flow {
using Microsoft::WRL::ComPtr;
namespace {
constexpr auto U=D3D12_RESOURCE_STATE_UNORDERED_ACCESS,S=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr unsigned tables=32,desc=64,per_slot=tables*desc,permanent=64;
struct Tex {ComPtr<ID3D12Resource> r;DXGI_FORMAT f=DXGI_FORMAT_UNKNOWN;};
D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
    D3D12_RESOURCE_BARRIER t{};t.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    t.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};return t;
}
}
struct PairFlow::Impl {
    ComPtr<ID3D12Device> device;ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12RootSignature> root[7];ComPtr<ID3D12PipelineState> pso[7];
    std::vector<Tex> tex;unsigned w=0,h=0,slots=0,stride=0,table=0;
    unsigned luma[2][7]{},flow[2][7]{},hist=0,previous_hist=0,temp=0,scd=0;
    bool initialized=false;const char* error="Not initialized";
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(unsigned i){auto a=heap->GetCPUDescriptorHandleForHeapStart();a.ptr+=SIZE_T(i)*stride;return a;}
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(unsigned i){auto a=heap->GetGPUDescriptorHandleForHeapStart();a.ptr+=UINT64(i)*stride;return a;}
    void view(unsigned i,ID3D12Resource* r,DXGI_FORMAT f,bool uav){
        if(uav){D3D12_UNORDERED_ACCESS_VIEW_DESC d{};d.Format=f;d.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;device->CreateUnorderedAccessView(r,nullptr,&d,cpu(i));}
        else{D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=f;d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;d.Texture2D.MipLevels=1;device->CreateShaderResourceView(r,&d,cpu(i));}
    }
    bool add(unsigned width,unsigned height,DXGI_FORMAT format,unsigned& index){
        Tex t;t.f=format;D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=width;d.Height=height;
        d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Format=format;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if(FAILED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,U,nullptr,IID_PPV_ARGS(&t.r))))return false;
        index=static_cast<unsigned>(tex.size());tex.push_back(std::move(t));view(index,tex.back().r.Get(),format,true);return true;
    }
    void barrier(ID3D12GraphicsCommandList* l){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;l->ResourceBarrier(1,&b);}
    void clear(ID3D12GraphicsCommandList* l,unsigned i){
        const UINT z[4]{};const float f[4]{};auto& t=tex[i];
        if(t.f==DXGI_FORMAT_R32_FLOAT)l->ClearUnorderedAccessViewFloat(gpu(i),cpu(i),t.r.Get(),f,0,nullptr);
        else l->ClearUnorderedAccessViewUint(gpu(i),cpu(i),t.r.Get(),z,0,nullptr);
    }
    struct Binding {unsigned reg,index;};
    void run(ID3D12GraphicsCommandList* l,Pass pass,std::initializer_list<Binding> srvs,std::initializer_list<Binding> uavs,
             unsigned level,unsigned x,unsigned y,unsigned z=1,ID3D12Resource* color=nullptr){
        const unsigned base=table;table+=desc;
        for(unsigned j=0;j<32;++j){view(base+j,nullptr,DXGI_FORMAT_R32_UINT,true);view(base+32+j,nullptr,DXGI_FORMAT_R32_UINT,false);}
        for(auto b:srvs){auto& t=tex[b.index];auto tr=transition(t.r.Get(),U,S);l->ResourceBarrier(1,&tr);view(base+32+b.reg,t.r.Get(),t.f,false);}
        if(color)view(base+32,color,color->GetDesc().Format,false);
        for(auto b:uavs){auto& t=tex[b.index];view(base+b.reg,t.r.Get(),t.f,true);}
        // Pair inputs/pyramids are fully initialized. AMD's six-frame startup
        // warmup for its implicit stream history does not apply to explicit pairs.
        const unsigned cb[8]={w,h,level,7,6,0,0,0x3f800000};
        const auto pidx=static_cast<unsigned>(pass);l->SetComputeRootSignature(root[pidx].Get());l->SetPipelineState(pso[pidx].Get());
        l->SetComputeRootDescriptorTable(0,gpu(base));l->SetComputeRootDescriptorTable(1,gpu(base+32));l->SetComputeRoot32BitConstants(2,8,cb,0);
        l->Dispatch(x,y,z);barrier(l);
        for(auto b:srvs){auto tr=transition(tex[b.index].r.Get(),S,U);l->ResourceBarrier(1,&tr);}
    }
};
PairFlow::PairFlow():p(new Impl){} PairFlow::~PairFlow()=default;
bool PairFlow::Init(ID3D12Device* dev,unsigned w,unsigned h,unsigned slots){
    if(!dev || !Built() || w<256 || h<64 || w>16384 || h>16384 || !slots || slots>8){p->error="AMD compute flow needs DXC shaders and an image at least 256x64";return false;}
    if(p->device){p->error="Reinitialize only after retirement";return false;}
    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_2};D3D12_FEATURE_DATA_D3D12_OPTIONS1 opt{};
    if(FAILED(dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&sm,sizeof sm)) || sm.HighestShaderModel<D3D_SHADER_MODEL_6_2 ||
       FAILED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1,&opt,sizeof opt)) || !opt.WaveOps){p->error="AMD flow requires Shader Model6.2 and wave operations";return false;}
    p->device=dev;p->w=w;p->h=h;p->slots=slots;p->stride=dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;hd.NumDescriptors=permanent+slots*per_slot;
    if(FAILED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&p->heap))))return false;
    for(unsigned i=0;i<7;++i){const auto b=Shader(static_cast<Pass>(i),false);
        if(FAILED(dev->CreateRootSignature(0,b.data,b.size,IID_PPV_ARGS(&p->root[i]))))return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC d{};d.pRootSignature=p->root[i].Get();d.CS={b.data,b.size};
        if(FAILED(dev->CreateComputePipelineState(&d,IID_PPV_ARGS(&p->pso[i]))))return false;
    }
    for(unsigned image=0;image<2;++image)for(unsigned level=0;level<7;++level){
        if(!p->add((std::max)(1u,w>>level),(std::max)(1u,h>>level),DXGI_FORMAT_R8_UINT,p->luma[image][level]))return false;
        const unsigned scale=8u<<level;
        if(!p->add((w+scale-1)/scale,(h+scale-1)/scale,DXGI_FORMAT_R16G16_SINT,p->flow[image][level]))return false;
    }
    if(!p->add(2304,1,DXGI_FORMAT_R32_UINT,p->hist) || !p->add(2304,1,DXGI_FORMAT_R32_FLOAT,p->previous_hist) ||
       !p->add(3,1,DXGI_FORMAT_R32_UINT,p->temp) || !p->add(3,1,DXGI_FORMAT_R32_UINT,p->scd))return false;
    p->initialized=true;p->error="";return true;
}
bool PairFlow::Record(ID3D12GraphicsCommandList* l,ID3D12Resource* cur,ID3D12Resource* prev,unsigned slot){
    if(!l || !cur || !prev || !p->initialized || slot>=p->slots)return false;
    for(auto r:{cur,prev}){auto d=r->GetDesc();if(d.Width!=p->w || d.Height!=p->h || d.SampleDesc.Count!=1 || d.DepthOrArraySize!=1 || d.MipLevels!=1)return false;}
    ID3D12DescriptorHeap* heap=p->heap.Get();l->SetDescriptorHeaps(1,&heap);
    p->table=permanent+slot*per_slot;
    for(unsigned i=0;i<p->tex.size();++i)p->clear(l,i);p->barrier(l);
    for(unsigned image=0;image<2;++image){
        const auto q=p->luma[image];
        p->run(l,Pass::PrepareLuma,{},{{0,q[0]}},0,(p->w+31)/32,(p->h+31)/32,1,image?cur:prev);
        p->run(l,Pass::LuminancePyramid,{},{{0,q[0]},{1,q[1]},{2,q[2]},{3,q[3]},{4,q[4]},{5,q[5]},{6,q[6]}},0,(p->w+63)/64,(p->h+63)/64);
        p->run(l,Pass::ScdHistogram,{{0,q[0]}},{{0,p->hist}},0,((p->w/4)/3+31)/32,16,9);
        // Seed previous histogram from the supplied previous image, then compare
        // current against it. Do not retain a cut bit from seeding against zero.
        p->clear(l,p->scd);p->clear(l,p->temp);p->barrier(l);
        p->run(l,Pass::ScdDivergence,{},{{0,p->hist},{1,p->previous_hist},{2,p->temp},{3,p->scd}},0,9,3);
    }
    for(int level=6;level>=0;--level){
        const unsigned a=static_cast<unsigned>(level)&1u,b=1u-a,n=static_cast<unsigned>(level),scale=8u<<n;
        p->run(l,Pass::Search,{{0,p->luma[1][n]},{1,p->luma[0][n]}},{{0,p->flow[a][n]},{1,p->scd}},n,
            (((p->w>>n)+3)/4*16+63)/64,((p->h>>n)+15)/16);
        p->run(l,Pass::Filter,{{0,p->flow[a][n]}},{{0,p->flow[b][n]}},n,((p->w+scale-1)/scale+15)/16,((p->h+scale-1)/scale+3)/4);
        if(n){const unsigned next=scale/2;
            p->run(l,Pass::Scale,{{0,p->luma[1][n]},{1,p->luma[0][n]},{2,p->flow[b][n]}},{{0,p->flow[b][n-1]},{1,p->scd}},n,
                ((p->w+next-1)/next+3)/4,((p->h+next-1)/next+3)/4);
        }
    }
    auto a=transition(Grid(),U,S),b=transition(SceneChange(),U,S);l->ResourceBarrier(1,&a);l->ResourceBarrier(1,&b);return true;
}
ID3D12Resource* PairFlow::Grid()const{return p->tex.empty()?nullptr:p->tex[p->flow[1][0]].r.Get();}
ID3D12Resource* PairFlow::SceneChange()const{return p->tex.empty()?nullptr:p->tex[p->scd].r.Get();}
void PairFlow::Finish(ID3D12GraphicsCommandList* l){auto a=transition(Grid(),S,U),b=transition(SceneChange(),S,U);l->ResourceBarrier(1,&a);l->ResourceBarrier(1,&b);}
const char* PairFlow::Error()const{return p->error;}
}
