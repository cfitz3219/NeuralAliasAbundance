#include "host_dx12/j2/compute_flow.hpp"
#include "host_dx12/j2/flow_shaders.hpp"
#include "host_dx12/ffx_flow/ffx_flow.hpp"
#include "host_dx12/ffx_flow/ffx_flow_shaders.hpp"
#include "host_dx12/nvof/flow_expand.hpp"
#include "host_dx12/nr/native_fg.hpp"
#include "core/motion_arbiter_gpu.hpp"
#include <d3dcompiler.h>
#include <mutex>
#include <vector>
#include <algorithm>
#include <cstdio>

namespace na::j2::compute_flow {
using Microsoft::WRL::ComPtr;using Lease=na::nr::native::RecordingLease;
namespace {
constexpr unsigned slots=8;constexpr auto S=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,U=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
D3D12_RESOURCE_BARRIER tr(ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){D3D12_RESOURCE_BARRIER x{};x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;x.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};return x;}
void move(ID3D12GraphicsCommandList* l,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){auto x=tr(r,a,b);l->ResourceBarrier(1,&x);}
void order(ID3D12GraphicsCommandList* l){D3D12_RESOURCE_BARRIER x{};x.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;l->ResourceBarrier(1,&x);}
struct Constants {unsigned w,h,ew,eh,mode,block,cost,cost_max,offset,gw,gh,pad;float snap=1,margin=.02f,absolute=.08f,texture=.03f;};
struct Image {ComPtr<ID3D12Resource> tex;std::vector<Lease> uses;Lease producer;uint64_t frame=0;unsigned generation=0;float jitter[2]{};};
struct Slot {Lease lease;bool pair=false,submitted=false;};
struct Context {
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;unsigned w=0,h=0,stride=0;int last=-1;
    ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pso_encode,pso_accept;ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12Resource> raw,expanded,residual,valid,mask,counters,zero,readback;
    ComPtr<ID3D12QueryHeap> timestamps;UINT64 frequency=0;
    na::ffx_flow::PairFlow engine;na::nvof::FlowExpand expand;na::motion::gpu::Arbiter derive;
    Image images[slots];Slot leases[slots];
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(unsigned slot,unsigned table,unsigned i){auto a=heap->GetCPUDescriptorHandleForHeapStart();a.ptr+=SIZE_T((slot*2+table)*10+i)*stride;return a;}
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(unsigned slot,unsigned table){auto a=heap->GetGPUDescriptorHandleForHeapStart();a.ptr+=UINT64((slot*2+table)*10)*stride;return a;}
    bool tex(DXGI_FORMAT f,ComPtr<ID3D12Resource>& r){
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width=w;d.Height=h;d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Format=f;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,S,nullptr,IID_PPV_ARGS(&r)));
    }
    bool buffer(unsigned bytes,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state,ComPtr<ID3D12Resource>& r){
        D3D12_HEAP_PROPERTIES hp{};hp.Type=type;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if(type==D3D12_HEAP_TYPE_DEFAULT)d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r)));
    }
    void srv(unsigned slot,unsigned table,unsigned i,ID3D12Resource* r,DXGI_FORMAT f){D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=f;d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;d.Texture2D.MipLevels=1;device->CreateShaderResourceView(r,&d,cpu(slot,table,i));}
    void uav(unsigned slot,unsigned table,unsigned i,ID3D12Resource* r,DXGI_FORMAT f){D3D12_UNORDERED_ACCESS_VIEW_DESC d{};d.Format=f;d.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;device->CreateUnorderedAccessView(r,nullptr,&d,cpu(slot,table,i));}
    void defaults(unsigned slot,unsigned table){
        srv(slot,table,0,nullptr,DXGI_FORMAT_R8G8B8A8_UNORM);srv(slot,table,1,nullptr,DXGI_FORMAT_R32G32_FLOAT);
        srv(slot,table,2,nullptr,DXGI_FORMAT_R32G32_FLOAT);srv(slot,table,3,nullptr,DXGI_FORMAT_R32_FLOAT);
        srv(slot,table,4,nullptr,DXGI_FORMAT_R8_UINT);srv(slot,table,5,nullptr,DXGI_FORMAT_R8G8B8A8_UNORM);srv(slot,table,6,nullptr,DXGI_FORMAT_R8G8B8A8_UNORM);
        uav(slot,table,7,nullptr,DXGI_FORMAT_R8G8B8A8_UNORM);uav(slot,table,8,nullptr,DXGI_FORMAT_R32_FLOAT);
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};d.Format=DXGI_FORMAT_R32_TYPELESS;d.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;d.Buffer.NumElements=slots*8;d.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(counters.Get(),nullptr,&d,cpu(slot,table,9));
    }
    void dispatch(ID3D12GraphicsCommandList* l,unsigned slot,unsigned table,ID3D12PipelineState* pso,const Constants& c){
        ID3D12DescriptorHeap* heaps[]={heap.Get()};l->SetDescriptorHeaps(1,heaps);l->SetComputeRootSignature(root.Get());l->SetPipelineState(pso);
        l->SetComputeRootDescriptorTable(0,gpu(slot,table));l->SetComputeRoot32BitConstants(1,16,&c,0);l->Dispatch((w+7)/8,(h+7)/8,1);
    }
};
std::mutex mutex;std::shared_ptr<Context> current;Stats stats;
void reason(const char* r){strncpy_s(stats.reason,r,_TRUNCATE);}
bool programs(Context& g){
#include "compute_flow_programs.inc"
}
bool build(Context& c,ID3D12Device* device,ID3D12CommandQueue* queue,unsigned w,unsigned h){
    c.device=device;c.queue=queue;c.w=w;c.h=h;c.stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if(!c.engine.Init(device,w,h,slots)){reason(c.engine.Error());return false;}
    if(!programs(c) || !c.expand.Init(device,slots) || !c.derive.Init(device,slots))return false;
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;hd.NumDescriptors=slots*20;
    if(FAILED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&c.heap))))return false;
    for(auto& i:c.images)if(!c.tex(DXGI_FORMAT_R8G8B8A8_UNORM,i.tex))return false;
    if(!c.tex(DXGI_FORMAT_R32G32_FLOAT,c.raw) || !c.tex(DXGI_FORMAT_R32_FLOAT,c.expanded) || !c.tex(DXGI_FORMAT_R32G32_FLOAT,c.residual) || !c.tex(DXGI_FORMAT_R32_FLOAT,c.valid) || !c.tex(DXGI_FORMAT_R32_FLOAT,c.mask) ||
       !c.buffer(slots*32,D3D12_HEAP_TYPE_DEFAULT,U,c.counters) || !c.buffer(32,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,c.zero) || !c.buffer(slots*48,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST,c.readback))return false;
    void* p=nullptr;if(FAILED(c.zero->Map(0,nullptr,&p)))return false;memset(p,0,32);c.zero->Unmap(0,nullptr);
    D3D12_QUERY_HEAP_DESC qd{};qd.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;qd.Count=slots*2;
    if(FAILED(device->CreateQueryHeap(&qd,IID_PPV_ARGS(&c.timestamps))))c.timestamps.Reset();queue->GetTimestampFrequency(&c.frequency);return true;
}
bool free(Image& i){i.uses.erase(std::remove_if(i.uses.begin(),i.uses.end(),[](const Lease& l){return l.Retired();}),i.uses.end());return i.uses.empty();}
void collect(Context& c){for(unsigned s=0;s<slots;++s)if(c.leases[s].pair && c.leases[s].lease.Retired()){
    if(!c.leases[s].submitted){c.leases[s].pair=false;continue;} // discarded commands never wrote telemetry
    void* p=nullptr;D3D12_RANGE range{SIZE_T(s*48),SIZE_T((s+1)*48)};
    if(SUCCEEDED(c.readback->Map(0,&range,&p))){auto b=static_cast<const unsigned char*>(p)+s*48;const auto v=reinterpret_cast<const unsigned*>(b);stats.accepted=v[0];stats.total=0;for(unsigned j=0;j<6;++j)stats.total+=v[j];
        if(c.timestamps && c.frequency){UINT64 t[2]{};memcpy(t,b+32,16);stats.ms=double(t[1]-t[0])*1000.0/double(c.frequency);}
        D3D12_RANGE none{};c.readback->Unmap(0,&none);}
    c.leases[s].pair=false;
}}
}
Ticket Record(ID3D12GraphicsCommandList* l,ID3D12CommandQueue* queue,ID3D12Resource* color,ID3D12Resource* depth,
              const na::motion::gpu::Inputs& camera,const Frame& f,Tier3& out){
    na::nr::native::Maintain(); // nonblocking retirement collection, before our lock
    std::lock_guard<std::mutex> lock(mutex);out={};
    if(!l || !queue || !color || !depth || !na::ffx_flow::Built() || f.slot>=slots){reason("AMD compute shaders unavailable or invalid frame");return {};}
    ComPtr<ID3D12Device> device;if(FAILED(l->GetDevice(IID_PPV_ARGS(&device))))return {};
    auto d=color->GetDesc();const unsigned w=static_cast<unsigned>(d.Width),h=d.Height;
    if(!current || current->device.Get()!=device.Get() || current->queue.Get()!=queue || current->w!=w || current->h!=h){
        reason("AMD working resources unavailable; camera motion used");
        auto next=std::make_shared<Context>();if(!build(*next,device.Get(),queue,w,h))return {};current=std::move(next);++stats.resets;
    }
    auto c=current;collect(*c);auto& slot=c->leases[f.slot];
    if(slot.lease && !slot.lease.Retired()){++stats.busy;reason("AMD flow slot still executable/in flight; camera motion used");return {};}
    int image=-1;for(unsigned j=0;j<slots;++j)if(static_cast<int>(j)!=c->last && free(c->images[j])){image=static_cast<int>(j);break;}
    if(image<0){++stats.busy;reason("AMD flow images still in use; camera motion used");return {};}
    // Retain context through every submission/replay or discard of this exact recording.
    auto lease=na::nr::native::RetainRecording(l,c);if(!lease){reason("AMD flow recording lifetime unavailable; camera motion used");return {};}
    slot.lease=lease;slot.pair=false;slot.submitted=false;auto& im=c->images[image];im.uses.push_back(lease);im.producer=lease;
    const int previous=c->last;
    bool pair=!f.reset && previous>=0 && c->images[previous].frame==f.previous && f.previous && c->images[previous].generation==f.generation && c->images[previous].producer.Ready(queue);
    Ticket t;t.context=c;t.image=static_cast<unsigned>(image);t.previous=previous<0?0u:static_cast<unsigned>(previous);t.slot=f.slot;t.frame=f;t.pair=pair;
    Constants cb{};cb.w=cb.ew=w;cb.h=cb.eh=h;cb.mode=f.hdr?1u:0u;cb.block=8;cb.offset=f.slot*32;cb.gw=(w+7)/8;cb.gh=(h+7)/8;
    if(c->timestamps)l->EndQuery(c->timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,f.slot*2);
    move(l,im.tex.Get(),S,U);c->defaults(f.slot,0);c->srv(f.slot,0,0,color,d.Format);c->uav(f.slot,0,7,im.tex.Get(),DXGI_FORMAT_R8G8B8A8_UNORM);
    c->dispatch(l,f.slot,0,c->pso_encode.Get(),cb);order(l);move(l,im.tex.Get(),U,S);++stats.recorded;
    if(!pair){reason("AMD flow capturing matching scene history; camera motion used");return t;}
    auto& prev=c->images[previous];prev.uses.push_back(lease);
    if(!c->engine.Record(l,im.tex.Get(),prev.tex.Get(),f.slot)){reason("AMD flow refused image pair; camera motion used");return t;}
    move(l,c->raw.Get(),S,U);move(l,c->expanded.Get(),S,U);
    // AMD outputs integral pixels; FlowExpand decodes NVOFA S10.5. Cancel
    // its /32 here. Grid spacing and value scale are separate conventions.
    c->expand.SetVectorScale(32.f);na::nvof::ExpandParams ep;ep.depth=depth;ep.validity_out=c->expanded.Get();ep.depth_clear=camera.depth_clear;
    bool ok=c->expand.DispatchDepthAware(l,c->engine.Grid(),8,ep,c->raw.Get(),w,h,f.slot);
    order(l);move(l,c->raw.Get(),U,S);move(l,c->expanded.Get(),U,S);move(l,c->residual.Get(),S,U);move(l,c->valid.Get(),S,U);
    ok=ok && c->derive.DispatchDeriveResidual(l,c->raw.Get(),depth,camera,prev.jitter,c->residual.Get(),c->valid.Get(),w,h,f.slot,c->expanded.Get());
    order(l);move(l,c->residual.Get(),U,S);move(l,c->valid.Get(),U,S);move(l,c->mask.Get(),S,U);
    move(l,c->counters.Get(),U,D3D12_RESOURCE_STATE_COPY_DEST);l->CopyBufferRegion(c->counters.Get(),f.slot*32,c->zero.Get(),0,32);move(l,c->counters.Get(),D3D12_RESOURCE_STATE_COPY_DEST,U);
    cb.cost=2;c->defaults(f.slot,1);c->srv(f.slot,1,4,c->engine.SceneChange(),DXGI_FORMAT_R32_UINT);
    c->srv(f.slot,1,1,c->raw.Get(),DXGI_FORMAT_R32G32_FLOAT);c->srv(f.slot,1,2,c->residual.Get(),DXGI_FORMAT_R32G32_FLOAT);
    c->srv(f.slot,1,3,c->valid.Get(),DXGI_FORMAT_R32_FLOAT);c->srv(f.slot,1,5,im.tex.Get(),DXGI_FORMAT_R8G8B8A8_UNORM);c->srv(f.slot,1,6,prev.tex.Get(),DXGI_FORMAT_R8G8B8A8_UNORM);c->uav(f.slot,1,8,c->mask.Get(),DXGI_FORMAT_R32_FLOAT);
    if(ok)c->dispatch(l,f.slot,1,c->pso_accept.Get(),cb);order(l);move(l,c->mask.Get(),U,S);c->engine.Finish(l);
    move(l,c->counters.Get(),U,D3D12_RESOURCE_STATE_COPY_SOURCE);l->CopyBufferRegion(c->readback.Get(),f.slot*48,c->counters.Get(),f.slot*32,32);move(l,c->counters.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,U);
    if(c->timestamps){l->EndQuery(c->timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,f.slot*2+1);l->ResolveQueryData(c->timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,f.slot*2,2,c->readback.Get(),f.slot*48+32);}
    if(ok){out={c->residual.Get(),c->valid.Get(),c->mask.Get()};slot.pair=true;++stats.pairs;reason("AMD same-frame optical flow recorded before DLAA");}
    return t;
}
void Committed(const Ticket& t){if(!t.context)return;std::lock_guard<std::mutex> lock(mutex);auto c=std::static_pointer_cast<Context>(t.context);auto& im=c->images[t.image];im.frame=t.frame.frame;im.generation=t.frame.generation;memcpy(im.jitter,t.frame.jitter,sizeof im.jitter);c->last=static_cast<int>(t.image);c->leases[t.slot].submitted=true;++stats.submitted;}
void Reset(){std::lock_guard<std::mutex> lock(mutex);current.reset();}
Stats GetStats(){std::lock_guard<std::mutex> lock(mutex);if(current)collect(*current);return stats;}
}
