#include "host_dx12/j2/compute_flow.hpp"
#include "host_dx12/j2/flow_shaders.hpp"
#include "host_dx12/ffx_flow/ffx_flow.hpp"
#include "host_dx12/ffx_flow/ffx_flow_shaders.hpp"
#include "host_dx12/nvof/flow_expand.hpp"
#include "host_dx12/nr/native_fg.hpp"
#include "core/motion_arbiter_gpu.hpp"
#include <d3dcompiler.h>
#include <mutex>
#include <atomic>
#include <vector>
#include <algorithm>
#include <cstdio>

namespace na::j2::compute_flow {
std::atomic<uint32_t> g_loosen{50}; // SetLoosen; read at each Record
using Microsoft::WRL::ComPtr;using Lease=na::nr::native::RecordingLease;
namespace {
constexpr unsigned warm_images=8;constexpr auto S=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,U=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
D3D12_RESOURCE_BARRIER tr(ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){D3D12_RESOURCE_BARRIER x{};x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;x.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};return x;}
void move(ID3D12GraphicsCommandList* l,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){auto x=tr(r,a,b);l->ResourceBarrier(1,&x);}
void order(ID3D12GraphicsCommandList* l){D3D12_RESOURCE_BARRIER x{};x.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;l->ResourceBarrier(1,&x);}
// Same layout and gate values as j2_flow.cpp's Constants/k* (the Anno-run tuning):
// snap 0.5 px, image ratio 0.5, temporal agreement (one previous pair, 60 deg, 3 px).
struct Constants {unsigned w,h,ew,eh,mode,block,cost,cost_max,offset,gw,gh;float ratio=.5f;float snap=.5f,margin=.02f,absolute=.08f,texture=.03f;
    unsigned have_hist=0,agree_min=1,pad1=0,pad2=0;float agree_px=3.f,agree_cos=.5f,jitter_x=0,jitter_y=0;};
static_assert(sizeof(Constants)==24*4,"root constants");
constexpr unsigned kTable=12,kUav=8; // descriptors per table: SRVs t0..t7, then UAVs u0..u3
struct Image {ComPtr<ID3D12Resource> tex;std::vector<Lease> uses;Lease producer;uint64_t frame=0;unsigned generation=0;float jitter[2]{};};
struct Slot {
    Lease lease;bool pair=false,submitted=false;uint64_t frame=0;
    ComPtr<ID3D12DescriptorHeap> heap,engine_heap,expand_heap,derive_heap;
    ComPtr<ID3D12Resource> readback;ComPtr<ID3D12QueryHeap> timestamps;
    ComPtr<ID3D12Resource> color_input,depth_input;
    int hist_written=-1; // temporal history this recording's pair writes (published at Committed)
};
struct Context {
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;unsigned w=0,h=0,stride=0;int last=-1;
    ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pso_encode,pso_accept;
    ComPtr<ID3D12Resource> raw,expanded,residual,valid,mask,counters,zero,hist[2];
    unsigned hist_read=0;uint64_t hist_frame=0;unsigned hist_generation=0; // last committed pair's candidates
    UINT64 frequency=0;
    na::ffx_flow::PairFlow engine;na::nvof::FlowExpand expand;na::motion::gpu::Arbiter derive;
    std::vector<std::unique_ptr<Image>> images;std::vector<std::unique_ptr<Slot>> leases;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(unsigned slot,unsigned table,unsigned i){auto a=leases[slot]->heap->GetCPUDescriptorHandleForHeapStart();a.ptr+=SIZE_T(table*kTable+i)*stride;return a;}
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(unsigned slot,unsigned table){auto a=leases[slot]->heap->GetGPUDescriptorHandleForHeapStart();a.ptr+=UINT64(table*kTable)*stride;return a;}
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
        srv(slot,table,7,nullptr,DXGI_FORMAT_R16G16B16A16_FLOAT);
        uav(slot,table,kUav+0,nullptr,DXGI_FORMAT_R8G8B8A8_UNORM);uav(slot,table,kUav+1,nullptr,DXGI_FORMAT_R32_FLOAT);uav(slot,table,kUav+3,nullptr,DXGI_FORMAT_R16G16B16A16_FLOAT);
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};d.Format=DXGI_FORMAT_R32_TYPELESS;d.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;d.Buffer.NumElements=12;d.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(counters.Get(),nullptr,&d,cpu(slot,table,kUav+2));
    }
    void dispatch(ID3D12GraphicsCommandList* l,unsigned slot,unsigned table,ID3D12PipelineState* pso,const Constants& c){
        ID3D12DescriptorHeap* heaps[]={leases[slot]->heap.Get()};l->SetDescriptorHeaps(1,heaps);l->SetComputeRootSignature(root.Get());l->SetPipelineState(pso);
        l->SetComputeRootDescriptorTable(0,gpu(slot,table));l->SetComputeRoot32BitConstants(1,24,&c,0);l->Dispatch((w+7)/8,(h+7)/8,1);
    }
};
std::mutex mutex;std::shared_ptr<Context> current;Stats stats;uint64_t last_submit_tick=0,sample_tick=0;
void reason(const char* r){strncpy_s(stats.reason,r,_TRUNCATE);}
bool programs(Context& g){
#include "compute_flow_programs.inc"
}
bool build(Context& c,ID3D12Device* device,ID3D12CommandQueue* queue,unsigned w,unsigned h){
    c.device=device;c.queue=queue;c.w=w;c.h=h;c.stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if(!c.engine.Init(device,w,h,1)){reason(c.engine.Error());return false;}
    if(!programs(c) || !c.expand.Init(device,1) || !c.derive.Init(device,1))return false;
    for(unsigned i=0;i<warm_images;++i){auto image=std::make_unique<Image>();if(!c.tex(DXGI_FORMAT_R8G8B8A8_UNORM,image->tex))return false;c.images.push_back(std::move(image));}
    if(!c.tex(DXGI_FORMAT_R32G32_FLOAT,c.raw) || !c.tex(DXGI_FORMAT_R32_FLOAT,c.expanded) || !c.tex(DXGI_FORMAT_R32G32_FLOAT,c.residual) || !c.tex(DXGI_FORMAT_R32_FLOAT,c.valid) || !c.tex(DXGI_FORMAT_R32_FLOAT,c.mask) ||
       !c.tex(DXGI_FORMAT_R16G16B16A16_FLOAT,c.hist[0]) || !c.tex(DXGI_FORMAT_R16G16B16A16_FLOAT,c.hist[1]) ||
       !c.buffer(48,D3D12_HEAP_TYPE_DEFAULT,U,c.counters) || !c.buffer(48,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,c.zero))return false;
    void* p=nullptr;if(FAILED(c.zero->Map(0,nullptr,&p)))return false;memset(p,0,48);c.zero->Unmap(0,nullptr);
    queue->GetTimestampFrequency(&c.frequency);return true;
}
bool free(Image& i){i.uses.erase(std::remove_if(i.uses.begin(),i.uses.end(),[](const Lease& l){return l.Retired();}),i.uses.end());return i.uses.empty();}
void collect(Context& c){for(auto& entry:c.leases){auto& s=*entry;if(!s.pair || !s.lease.Retired())continue;
    if(!s.submitted){s.pair=false;continue;} // discarded commands never wrote telemetry
    if(s.frame>=stats.sample_frame){void* p=nullptr;D3D12_RANGE range{0,64};
        if(SUCCEEDED(s.readback->Map(0,&range,&p))){const auto* b=static_cast<const unsigned char*>(p);const auto* v=reinterpret_cast<const unsigned*>(b);
            stats.accepted=v[0];stats.rejected_invalid=v[1];stats.rejected_small=v[2];stats.rejected_cost=v[3];stats.rejected_offscreen=v[4];stats.rejected_image=v[5];stats.rejected_flicker=v[6];
            stats.total=0;for(unsigned j=0;j<7;++j)stats.total+=v[j];stats.sample_frame=s.frame;sample_tick=GetTickCount64();++stats.samples;
            if(s.timestamps && c.frequency){UINT64 t[2]{};memcpy(t,b+48,16);stats.ms=t[1]>=t[0]?double(t[1]-t[0])*1000.0/double(c.frequency):0;}
            D3D12_RANGE none{};s.readback->Unmap(0,&none);}}
    s.pair=false;
}}
bool prepare_slot(Context& c,Slot& slot){
    if(slot.heap)return true;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,2*kTable,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
    if(FAILED(c.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&slot.heap))) ||
       !c.buffer(64,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST,slot.readback)){slot.heap.Reset();return false;}
    D3D12_QUERY_HEAP_DESC qd{};qd.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;qd.Count=2;
    c.device->CreateQueryHeap(&qd,IID_PPV_ARGS(&slot.timestamps));return true;
}
}
Ticket Record(ID3D12GraphicsCommandList* l,ID3D12CommandQueue* queue,ID3D12Resource* color,ID3D12Resource* depth,
              const na::motion::gpu::Inputs& camera,const Frame& f,Tier3& out){
    na::nr::native::Maintain(); // nonblocking retirement collection, before our lock
    std::lock_guard<std::mutex> lock(mutex);out={};
    ++stats.attempts;
    if(!l || !queue || !color || !depth || !na::ffx_flow::Built()){++stats.unavailable;reason("AMD compute shaders unavailable or invalid frame");return {};}
    ComPtr<ID3D12Device> device;if(FAILED(l->GetDevice(IID_PPV_ARGS(&device))))return {};
    auto d=color->GetDesc();const unsigned w=static_cast<unsigned>(d.Width),h=d.Height;
    if(!current || current->device.Get()!=device.Get() || current->queue.Get()!=queue || current->w!=w || current->h!=h){
        reason("AMD working resources unavailable; camera motion used");
        auto next=std::make_shared<Context>();if(!build(*next,device.Get(),queue,w,h))return {};current=std::move(next);++stats.resets;
    }
    auto c=current;collect(*c);
    // Retain context through every submission/replay or discard of this exact recording.
    auto lease=na::nr::native::RetainRecording(l,c);if(!lease){++stats.unavailable;reason("AMD flow recording lifetime unavailable; camera motion used");return {};}
    for(const auto& s:c->leases)if(!s->lease.Retired() && s->lease.SameRecording(lease)){
        ++stats.busy;reason("AMD flow already belongs to this executable recording; camera motion used");return {};}
    unsigned work=0;while(work<c->leases.size() && c->leases[work]->lease && !c->leases[work]->lease.Retired())++work;
    if(work==c->leases.size())c->leases.push_back(std::make_unique<Slot>());
    auto& slot=*c->leases[work];if(!prepare_slot(*c,slot)){++stats.unavailable;reason("AMD recording descriptor allocation failed; camera motion used");return {};}
    int image=-1;for(unsigned j=0;j<c->images.size();++j)if(static_cast<int>(j)!=c->last && free(*c->images[j])){image=static_cast<int>(j);break;}
    if(image<0){auto next=std::make_unique<Image>();if(!c->tex(DXGI_FORMAT_R8G8B8A8_UNORM,next->tex)){++stats.unavailable;reason("AMD history image allocation failed; camera motion used");return {};}
        image=static_cast<int>(c->images.size());c->images.push_back(std::move(next));}
    slot.lease=lease;slot.pair=false;slot.submitted=false;slot.hist_written=-1;slot.frame=f.frame;auto& im=*c->images[image];im.uses.push_back(lease);im.producer=lease;
    slot.color_input=color;slot.depth_input=depth;
    stats.recording_slots=static_cast<uint32_t>(c->leases.size());stats.image_slots=static_cast<uint32_t>(c->images.size());
    const int previous=c->last;
    bool pair=!f.reset && previous>=0 && c->images[previous]->frame==f.previous && f.previous && c->images[previous]->generation==f.generation && c->images[previous]->producer.Ready(queue);
    Ticket t;t.context=c;t.image=static_cast<unsigned>(image);t.previous=previous<0?0u:static_cast<unsigned>(previous);t.slot=work;t.frame=f;t.pair=pair;
    stats.loosen=g_loosen.load(std::memory_order_relaxed);
    const Gates gates=GatesFor(stats.loosen);
    Constants cb{};cb.ratio=gates.image_ratio;cb.absolute=gates.image_abs;cb.texture=gates.texture_min;
    cb.agree_px=gates.agree_px;cb.agree_cos=gates.agree_cos;cb.agree_min=gates.agree_min;
    cb.w=cb.ew=w;cb.h=cb.eh=h;cb.mode=f.hdr?1u:0u;cb.block=8;cb.gw=(w+7)/8;cb.gh=(h+7)/8;
    // Jitter-stabilised encode (j2_flow fe8ea3e): sample texel q at q + this frame's jitter so both
    // images of a pair sit in the unjittered frame and the engine never sees DLAA's shake.
    cb.jitter_x=f.jitter[0];cb.jitter_y=f.jitter[1];
    if(slot.timestamps)l->EndQuery(slot.timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
    move(l,im.tex.Get(),S,U);c->defaults(work,0);c->srv(work,0,0,color,d.Format);c->uav(work,0,kUav+0,im.tex.Get(),DXGI_FORMAT_R8G8B8A8_UNORM);
    c->dispatch(l,work,0,c->pso_encode.Get(),cb);order(l);move(l,im.tex.Get(),U,S);++stats.recorded;
    if(!pair){++stats.waiting_history;reason("AMD flow capturing matching scene history; camera motion used");return t;}
    auto& prev=*c->images[previous];prev.uses.push_back(lease);
    if(!c->engine.Record(l,im.tex.Get(),prev.tex.Get(),0,&slot.engine_heap)){++stats.unavailable;reason("AMD flow refused image pair; camera motion used");return t;}
    move(l,c->raw.Get(),S,U);move(l,c->expanded.Get(),S,U);
    // AMD outputs integral pixels; FlowExpand decodes NVOFA S10.5. Cancel
    // its /32 here. Grid spacing and value scale are separate conventions.
    c->expand.SetVectorScale(32.f);na::nvof::ExpandParams ep;ep.depth=depth;ep.validity_out=c->expanded.Get();ep.depth_clear=camera.depth_clear;ep.consistency_px=gates.consistency_px;ep.sky=gates.sky;
    bool ok=c->expand.DispatchDepthAware(l,c->engine.Grid(),8,ep,c->raw.Get(),w,h,0,&slot.expand_heap);
    order(l);move(l,c->raw.Get(),U,S);move(l,c->expanded.Get(),U,S);move(l,c->residual.Get(),S,U);move(l,c->valid.Get(),S,U);
    // Stabilised images: the raw flow carries no jitter shift, so (jitter_prev - jitter_cur) must be 0.
    ok=ok && c->derive.DispatchDeriveResidual(l,c->raw.Get(),depth,camera,f.jitter,c->residual.Get(),c->valid.Get(),w,h,0,c->expanded.Get(),&slot.derive_heap);
    order(l);move(l,c->residual.Get(),U,S);move(l,c->valid.Get(),U,S);move(l,c->mask.Get(),S,U);
    move(l,c->counters.Get(),U,D3D12_RESOURCE_STATE_COPY_DEST);l->CopyBufferRegion(c->counters.Get(),0,c->zero.Get(),0,40);move(l,c->counters.Get(),D3D12_RESOURCE_STATE_COPY_DEST,U);
    cb.cost=2;c->defaults(work,1);c->srv(work,1,4,c->engine.SceneChange(),DXGI_FORMAT_R32_UINT);
    c->srv(work,1,1,c->raw.Get(),DXGI_FORMAT_R32G32_FLOAT);c->srv(work,1,2,c->residual.Get(),DXGI_FORMAT_R32G32_FLOAT);
    c->srv(work,1,3,c->valid.Get(),DXGI_FORMAT_R32_FLOAT);c->srv(work,1,5,im.tex.Get(),DXGI_FORMAT_R8G8B8A8_UNORM);c->srv(work,1,6,prev.tex.Get(),DXGI_FORMAT_R8G8B8A8_UNORM);c->uav(work,1,kUav+1,c->mask.Get(),DXGI_FORMAT_R32_FLOAT);
    // Temporal agreement (j2_flow 5c5fb0d/319e2ae): the previous pair's candidates, only when that
    // pair's current image is this pair's previous one (same generation); written here, published at Committed.
    const unsigned hist_out=c->hist_read^1u;
    cb.have_hist=(c->hist_frame!=0 && prev.frame==c->hist_frame && prev.generation==c->hist_generation)?1u:0u;
    c->srv(work,1,7,c->hist[c->hist_read].Get(),DXGI_FORMAT_R16G16B16A16_FLOAT);c->uav(work,1,kUav+3,c->hist[hist_out].Get(),DXGI_FORMAT_R16G16B16A16_FLOAT);
    move(l,c->hist[hist_out].Get(),S,U);
    if(ok)c->dispatch(l,work,1,c->pso_accept.Get(),cb);order(l);move(l,c->mask.Get(),U,S);move(l,c->hist[hist_out].Get(),U,S);c->engine.Finish(l);
    move(l,c->counters.Get(),U,D3D12_RESOURCE_STATE_COPY_SOURCE);l->CopyBufferRegion(slot.readback.Get(),0,c->counters.Get(),0,40);move(l,c->counters.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,U);
    if(slot.timestamps){l->EndQuery(slot.timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);l->ResolveQueryData(slot.timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,slot.readback.Get(),48);}
    if(ok){out={c->residual.Get(),c->valid.Get(),c->mask.Get()};slot.pair=true;slot.hist_written=static_cast<int>(hist_out);++stats.pairs;reason("AMD same-frame optical flow recorded before DLAA");}
    else {++stats.unavailable;reason("AMD flow expansion/residual unavailable; camera motion used");}
    return t;
}
Gates GatesFor(uint32_t percent){
    const float k=float(percent>100?100:percent)/100.f;
    Gates g{};
    g.image_ratio=.5f+.5f*k;      // flow must beat camera-only by this ratio: 0.5 -> 1.0 (just not worse)
    g.image_abs=.08f+.12f*k;      // absolute image error allowed: 0.08 -> 0.20
    g.texture_min=.03f-.02f*k;    // local contrast needed to trust a match: 0.03 -> 0.01
    g.agree_px=3.f+3.f*k;         // temporal agreement distance: 3 -> 6 px
    g.agree_cos=.5f-.4f*k;        // temporal agreement direction: 60 -> ~84 degrees
    g.agree_min=k>=.5f?0u:1u;     // from 50: a first sighting counts without a previous agreeing pair
    g.consistency_px=2.f+2.f*k;   // neighbouring-block spread: 2 -> 4 px
    g.sky=k>0.f;                  // objects against clear depth (sky) are no longer rejected
    return g;
}
void SetLoosen(uint32_t percent){g_loosen.store(percent>100?100u:percent,std::memory_order_relaxed);}
void Committed(const Ticket& t){if(!t.context)return;std::lock_guard<std::mutex> lock(mutex);auto c=std::static_pointer_cast<Context>(t.context);if(c!=current)return;auto& im=*c->images[t.image];im.frame=t.frame.frame;im.generation=t.frame.generation;memcpy(im.jitter,t.frame.jitter,sizeof im.jitter);c->last=static_cast<int>(t.image);auto& slot=*c->leases[t.slot];slot.submitted=true;++stats.submitted;if(slot.pair)++stats.submitted_pairs;
    if(slot.pair && slot.hist_written>=0){c->hist_read=static_cast<unsigned>(slot.hist_written);c->hist_frame=t.frame.frame;c->hist_generation=t.frame.generation;slot.hist_written=-1;}stats.last_submitted_frame=t.frame.frame;last_submit_tick=GetTickCount64();}
void Reset(){std::lock_guard<std::mutex> lock(mutex);current.reset();last_submit_tick=sample_tick=0;stats={};}
Stats GetStats(){na::nr::native::Maintain();std::lock_guard<std::mutex> lock(mutex);if(current)collect(*current);const auto now=GetTickCount64();stats.submit_age_ms=last_submit_tick?now-last_submit_tick:0;stats.sample_age_ms=sample_tick?now-sample_tick:0;return stats;}
void DescribeStats(const Stats& s,char* text,size_t capacity){
    if(!text || !capacity)return;
    if(!s.submitted || s.submit_age_ms>2000){std::snprintf(text,capacity,"AMD flow: no new submission%s. %s Frames submitted %llu, paired %llu, skipped %llu.",
        s.submitted?" recently":" yet",s.reason,(unsigned long long)s.submitted,(unsigned long long)s.submitted_pairs,(unsigned long long)(s.busy+s.unavailable));return;}
    if(!s.samples || !s.total){std::snprintf(text,capacity,"AMD flow: frames submitting. Paired %llu / %llu, skipped %llu. Waiting for a completed pixel sample.",
        (unsigned long long)s.submitted_pairs,(unsigned long long)s.submitted,(unsigned long long)(s.busy+s.unavailable));return;}
    std::snprintf(text,capacity,"AMD flow: paired %llu / %llu frames, skipped %llu. Last completed sample: %.2f%% moving pixels, %.2f ms (frame %llu, %llu frames behind).",
        (unsigned long long)s.submitted_pairs,(unsigned long long)s.submitted,(unsigned long long)(s.busy+s.unavailable),100.0*double(s.accepted)/double(s.total),s.ms,
        (unsigned long long)s.sample_frame,(unsigned long long)(s.last_submitted_frame>=s.sample_frame?s.last_submitted_frame-s.sample_frame:0));
}
}
