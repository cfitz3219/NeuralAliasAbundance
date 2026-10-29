#include "host_dx12/nr/native_fg.hpp"
#ifdef NA_HAVE_NGX_SDK
#include "host_dx12/nr/fg_contract.hpp"
#include "host_dx12/nr/fg_frame_cache.hpp"
#include "host_dx12/nr/fg_color.hpp"
#include "host_dx12/nr/fg_states.hpp"
#include "host_dx12/nr/fg_identity.hpp"
#include "host_dx12/hooking/variant_registry.hpp"
#include "host_dx12/settings.hpp"
#include "host_dx12/dlss/call_origin.hpp"
#include <nvsdk_ngx.h>
#include <wrl/client.h>
#include <memory>
#include <vector>
#include <atomic>
#include <algorithm>
#include <cmath>

namespace na::nr::native {
namespace {
using Microsoft::WRL::ComPtr;
using CreateFn=int32_t(__cdecl*)(void*,uint32_t,void*,void**);
using EvaluateFn=int32_t(__cdecl*)(void*,const void*,const void*,void*);
using ObjectFn=int32_t(__cdecl*)(void*);
using AllocateFn=int32_t(__cdecl*)(void**);
bool Good(int32_t r) { return NVSDK_NGX_SUCCEED(static_cast<NVSDK_NGX_Result>(r)); }
std::mutex mutex;
thread_local uint32_t internal=0;
struct Internal { Internal(){++internal;} ~Internal(){--internal;} };
std::atomic<uint32_t> contexts_count{0},records_count{0};
Status status;
std::atomic<uint64_t> submissions{0};
const GUID swapchain_key={0xc3f25562,0xe301,0x46c8,{0xb8,0x2d,0xa7,0x9e,0x0d,0x83,0xfe,0xf1}};
bool IsSwapchain(ID3D12Resource* r){UINT value=0,size=sizeof(value);return r && SUCCEEDED(r->GetPrivateData(swapchain_key,&size,&value)) && value==1;}
void Reason(const char* text) { strncpy_s(status.reason,text,_TRUNCATE); }
bool Texture(ID3D12Device* d,uint32_t w,uint32_t h,DXGI_FORMAT f,D3D12_RESOURCE_STATES state,ComPtr<ID3D12Resource>& out) {
    D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width=w; desc.Height=h;
    desc.DepthOrArraySize=desc.MipLevels=1; desc.Format=f; desc.SampleDesc.Count=1; desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    return SUCCEEDED(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&out)));
}
bool Region(FgSurface& s) {
    if (!s.resource) return false;
    const auto d=s.resource->GetDesc();
    if (d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.DepthOrArraySize!=1 || d.SampleDesc.Count!=1 ||
        d.Width>UINT32_MAX || d.MipLevels!=1 || s.region.x>=d.Width || s.region.y>=d.Height) return false;
    if (!s.region.width) s.region.width=static_cast<uint32_t>(d.Width)-s.region.x;
    if (!s.region.height) s.region.height=d.Height-s.region.y;
    return s.region.width<=d.Width-s.region.x && s.region.height<=d.Height-s.region.y;
}
void Barrier(ID3D12GraphicsCommandList* l,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER v{}; v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    v.Transition={r,0,a,b}; l->ResourceBarrier(1,&v);
}
struct InputStates {
    ID3D12GraphicsCommandList* list;
    ID3D12Resource* resources[4]{};
    D3D12_RESOURCE_STATES original[4]{};
    uint32_t count=0;
    bool ready=false;
    InputStates(ID3D12GraphicsCommandList* l,const FgContract& f):list(l) {
        ID3D12Resource* inputs[]={f.color.resource,f.hudless.resource,f.depth.resource,f.motion.resource};
        for(auto* r:inputs) {
            bool duplicate=false;for(uint32_t i=0;i<count;++i)duplicate|=resources[i]==r;
            if(duplicate)continue;
            resources[count]=r;if(!states::Read(l,r,original[count]) || !states::CanTransitionOn(l->GetType(),original[count]))return;++count;
        }
        D3D12_RESOURCE_STATES observed{};
        states::Read(l,f.color.resource,observed);status.color_state=static_cast<uint32_t>(observed);
        states::Read(l,f.hudless.resource,observed);status.hudless_state=static_cast<uint32_t>(observed);
        for(uint32_t i=0;i<count;++i)if(original[i]!=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
            Barrier(list,resources[i],original[i],D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ready=true;
    }
    ~InputStates(){if(ready)for(uint32_t i=0;i<count;++i)if(original[i]!=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
        Barrier(list,resources[i],D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,original[i]);}
};
struct Feature {
    Runtime rt{}; void* handle=nullptr; NVSDK_NGX_Parameter* params=nullptr;
    ComPtr<ID3D12Resource> color,nr,enhanced;
    uint32_t w=0,h=0,mode=0,model=0; bool reset=true;
    ComPtr<ID3D12Device> device;
    DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    const void* core=nullptr;
    uint64_t signature=0,last_frame=0;
    ~Feature() {
        na::dlss::InjectorCallScope origin;
        if(handle && rt.release_feature) reinterpret_cast<ObjectFn>(rt.release_feature)(handle);
        if(params && rt.destroy_parameters) reinterpret_cast<ObjectFn>(rt.destroy_parameters)(params);
    }
};
struct Packet {
    std::shared_ptr<Feature> feature;
    FgComposition composition;
    FgColorPass input,output;
    ComPtr<ID3D12Resource> refs[6];
    bool receipt_set=false;
    bool successful=false;
    ComPtr<ID3D12QueryHeap> queries;
    ComPtr<ID3D12Resource> timestamps;
    ComPtr<ID3D12Fence> timing_fence;
    uint64_t timing_value=0,frequency=0;
    bool timed=false;
    std::weak_ptr<std::atomic<bool>> producer;
    uint64_t id=0;
};
struct Context {
    const void* core=nullptr; const void* handle=nullptr;
    FgFrameCache cache;
    FgIdentity identity;
    uint32_t identity_mode=0;
    std::shared_ptr<Feature> feature;
    std::shared_ptr<Packet> packets[FgFrameCache::Capacity];
    bool live[FgFrameCache::Capacity]{};
    FgFrameCache::Ticket tickets[FgFrameCache::Capacity];
};
std::vector<std::unique_ptr<Context>> contexts;
struct Recording {
    ID3D12CommandList* list=nullptr;
    std::shared_ptr<std::atomic<bool>> valid;
    ComPtr<ID3D12Fence> fence;
    std::atomic<uint64_t> value{0},frequency{0};
    std::atomic<uint32_t> forwarding{0};
    std::atomic<bool> failed{false};
    std::mutex submission_mutex;
    std::vector<std::shared_ptr<Packet>> packets;
    std::vector<ComPtr<ID3D12Resource>> destinations;
};
std::shared_ptr<Recording> recordings[64];
const GUID token_id={0x354fc040,0xe9a9,0x48cd,{0x8d,0xb1,0xe1,0x6a,0x4d,0x21,0x6c,0xb3}};
class Token final:public IUnknown {
public:
    explicit Token(const std::shared_ptr<Recording>& r):recording(r),valid(r->valid){}
    std::weak_ptr<Recording> recording;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {
        if(!out)return E_POINTER; *out=nullptr; if(id!=__uuidof(IUnknown))return E_NOINTERFACE;
        *out=this; AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release() override{auto n=--refs;if(!n){Invalidate();delete this;}return n;}
    void Invalidate(){valid->store(false);}
private: std::atomic<ULONG> refs{1}; std::shared_ptr<std::atomic<bool>> valid;
};
void Collect() {
    // Submission threads only publish atomic receipts. They never take this
    // context mutex, including while NGX is creating a model on another thread.
    for(auto& r:recordings)if(r && !r->forwarding.load() && !r->failed.load() && r->value.load()) {
        for(auto& p:r->packets)if(p->successful && p->producer.lock()==r->valid) {
            if(!p->receipt_set)p->receipt_set=p->composition.SetCompletion(r->fence.Get(),r->value.load());
            p->timing_fence=r->fence;p->timing_value=r->value.load();p->frequency=r->frequency.load();
            for(auto& c:contexts)for(uint32_t i=0;i<FgFrameCache::Capacity;++i)
                if(c->live[i] && c->packets[i]==p)c->cache.Submitted(c->tickets[i]);
        }
    }
    // Timestamp readback is nonblocking: only map an already-completed pair.
    for(auto& c:contexts)for(auto& p:c->packets)if(p && !p->timed && p->successful && p->timestamps && p->timing_fence && p->frequency) {
        // A still-executable recording can be resubmitted and overwrite its
        // timestamp pair. Read only after Reset/destruction ends that lifetime.
        if(auto producer=p->producer.lock();producer && producer->load())continue;
        const auto done=p->timing_fence->GetCompletedValue();
        if(done!=UINT64_MAX && done>=p->timing_value) {
            void* data=nullptr;D3D12_RANGE read{0,16};
            if(SUCCEEDED(p->timestamps->Map(0,&read,&data))) {
                uint64_t ticks[2];memcpy(ticks,data,sizeof(ticks));D3D12_RANGE written{0,0};p->timestamps->Unmap(0,&written);
                if(ticks[1]>=ticks[0]) {
                    status.last_ms=static_cast<float>(1000.0*(ticks[1]-ticks[0])/p->frequency);
                    status.avg_ms=status.gpu_samples?status.avg_ms*.9f+status.last_ms*.1f:status.last_ms;++status.gpu_samples;
                }
                p->timed=true;
            }
        }
    }
    for(auto& r:recordings) if(r && !r->valid->load() && !r->forwarding.load() && !r->failed &&
        (!r->value || (r->fence->GetCompletedValue()!=UINT64_MAX && r->fence->GetCompletedValue()>=r->value))) {
        if(r->value) ++status.completed;
        for(auto& p:r->packets) if(!r->value || !p->successful) {
            // Reset before submission discards commands; failed evaluations
            // may have recorded work, so wait for its receipt before retry.
            for(auto& c:contexts) for(uint32_t i=0;i<FgFrameCache::Capacity;++i)
                if(c->live[i] && c->packets[i]==p && !c->cache.WasSubmitted(c->tickets[i])) {
                    c->cache.AbandonUnsubmitted(c->tickets[i]); c->live[i]=false;
                    if(c->feature==p->feature)c->feature.reset();
                    c->packets[i].reset();
                }
        }
        r.reset(); records_count.fetch_sub(1);
    }
    for(auto& c:contexts)for(auto& p:c->packets)if(p && p.use_count()==1) {
        p->composition.ReleaseInputs();p->input.ReleaseInputs();p->output.ReleaseInputs();
        for(auto& ref:p->refs)ref.Reset();
    }
}
Recording* Track(ID3D12GraphicsCommandList* list,ID3D12Device* device) {
    for(auto& r:recordings) if(r && r->list==list && r->valid->load()) return r.get();
    for(auto& r:recordings) if(!r) {
        auto fresh=std::make_shared<Recording>();
        fresh->list=list; fresh->valid=std::make_shared<std::atomic<bool>>(true);
        if(FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fresh->fence))))return nullptr;
        auto* token=new Token(fresh); const auto hr=list->SetPrivateDataInterface(token_id,token); token->Release();
        if(FAILED(hr))return nullptr;
        r=std::move(fresh); records_count.fetch_add(1); return r.get();
    }
    return nullptr;
}
void Keep(Recording& r,const std::shared_ptr<Packet>& p) {
    if(std::find(r.packets.begin(),r.packets.end(),p)==r.packets.end()) r.packets.push_back(p);
}
void SetRegion(NVSDK_NGX_Parameter* p,const char* name,const FgRegion& r) {
    char key[80]; const char* ends[]={"BaseX","BaseY","Width","Height"};
    const uint32_t values[]={r.x,r.y,r.width,r.height};
    for(int i=0;i<4;++i){snprintf(key,sizeof(key),"DLSSNR.%sSubrect%s",name,ends[i]);p->Set(key,values[i]);}
}
bool MakeFeature(ID3D12GraphicsCommandList* list,ID3D12Device* device,const Runtime& rt,
    FgContract& f,uint32_t mode,std::shared_ptr<Feature>& feature) {
    auto& n=*feature; n.rt=rt; n.w=f.color.region.width; n.h=f.color.region.height; n.mode=mode;
    n.device=device;n.format=f.hudless.resource->GetDesc().Format;
    n.model=settings::Get().nr_model.load();
    if(!rt.allocate_parameters || !rt.destroy_parameters || !rt.create_feature || !rt.evaluate_feature || !rt.release_feature)return false;
    if(!Texture(device,n.w,n.h,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,n.nr))return false;
    if(mode && (!Texture(device,n.w,n.h,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,n.color) ||
        !Texture(device,n.w,n.h,f.hudless.resource->GetDesc().Format,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,n.enhanced)))return false;
    void* raw=nullptr; if(!Good(reinterpret_cast<AllocateFn>(rt.allocate_parameters)(&raw)) || !raw)return false;
    n.params=static_cast<NVSDK_NGX_Parameter*>(raw);
    auto* p=n.params;
    p->Set("CreationNodeMask",1u);p->Set("VisibilityNodeMask",1u);
    p->Set("Width",n.w);p->Set("Height",n.h);p->Set("OutWidth",n.w);p->Set("OutHeight",n.h);
    p->Set("DLSSNR.Width",n.w);p->Set("DLSSNR.Height",n.h);p->Set("DLSSNR.Enabled",1u);
    p->Set("DLSSNR.Hint.Render.Preset",n.model==1?1u:2u);p->Set("DLSSNR.UseAutoMask",1u);
    p->Set("DLSSNR.UICorrection",0u);p->Set("DLSSNR.Style",0u);
    return Good(reinterpret_cast<CreateFn>(rt.create_feature)(list,18,p,&n.handle)) && n.handle;
}
void CopyRegion(ID3D12GraphicsCommandList* list,ID3D12Resource* source,FgSurface dest) {
    Barrier(list,source,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(list,dest.resource,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION s{},d{};s.pResource=source;s.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    d.pResource=dest.resource;d.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&d,dest.region.x,dest.region.y,0,&s,nullptr);
    Barrier(list,dest.resource,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(list,source,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}
}
bool OwnsRoute(){return contexts_count.load()!=0;}
void Register(const void* core,const void* handle){
    std::lock_guard<std::mutex> lock(mutex);
    for(auto& c:contexts)if(c->core==core && c->handle==handle)return;
    if(contexts.size()>=8)return;
    auto c=std::make_unique<Context>();c->core=core;c->handle=handle;contexts.push_back(std::move(c));
    contexts_count.store(static_cast<uint32_t>(contexts.size()));
}
void PrepareDevice(ID3D12Device* device) {
    if(device){PrepareFgComposition(device);PrepareFgColor(device);}
}
void Prepare(ID3D12GraphicsCommandList* list) {
    ComPtr<ID3D12Device> device;
    if(list && SUCCEEDED(list->GetDevice(IID_PPV_ARGS(&device)))) {
        PrepareDevice(device.Get());
    }
}
void Maintain() {
    if(internal)return;Internal guard;
    std::unique_lock<std::mutex> lock(mutex,std::try_to_lock);
    if(lock.owns_lock())Collect();
}
void Reset(ID3D12GraphicsCommandList* list) {
    IUnknown* raw=nullptr;UINT bytes=sizeof(raw);
    if(list && SUCCEEDED(list->GetPrivateData(token_id,&bytes,&raw)) && raw){static_cast<Token*>(raw)->Invalidate();raw->Release();}
}
void Release(const void* core,const void* handle) {
    if(internal)return;Internal guard;
    std::lock_guard<std::mutex> lock(mutex);
    contexts.erase(std::remove_if(contexts.begin(),contexts.end(),[&](const auto& c){return c->core==core && (!handle || c->handle==handle);}),contexts.end());
    contexts_count.store(static_cast<uint32_t>(contexts.size()));Collect();
}
void CoreShutdown(const void* core) {
    if(internal)return;Internal guard;std::lock_guard<std::mutex> lock(mutex);Collect();
    // NGX owns teardown of objects at Shutdown. No callback into that core is
    // legal later. GPU resource packets still follow their submission fences.
    auto orphan=[&](const std::shared_ptr<Feature>& f){if(f && f->core==core){f->handle=nullptr;f->params=nullptr;}};
    for(auto& c:contexts){orphan(c->feature);for(auto& p:c->packets)if(p)orphan(p->feature);}
    for(auto& r:recordings)if(r)for(auto& p:r->packets)orphan(p->feature);
    contexts.erase(std::remove_if(contexts.begin(),contexts.end(),[&](const auto& c){return c->core==core;}),contexts.end());
    contexts_count.store(static_cast<uint32_t>(contexts.size()));
}
Status GetStatus(){std::unique_lock<std::mutex> lock(mutex,std::try_to_lock);if(lock.owns_lock()){auto s=status;s.submitted=submissions.load();return s;} Status s;strcpy_s(s.reason,"Updating Neural Rendering.");return s;}
void MarkSwapchainBuffer(ID3D12Resource* r){if(r){const UINT value=1;r->SetPrivateData(swapchain_key,sizeof(value),&value);}}
void Before(ID3D12GraphicsCommandList* list,const void* core,const void* handle,const void* parameters,const Runtime& runtime) {
    if(!list || !parameters || (list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT && list->GetType()!=D3D12_COMMAND_LIST_TYPE_COMPUTE))return;
    if(internal)return;Internal guard;na::dlss::InjectorCallScope origin;
    std::lock_guard<std::mutex> lock(mutex);Collect();
    Context* ctx=nullptr; for(auto& c:contexts)if(c->core==core && c->handle==handle)ctx=c.get();
    if(!ctx){if(contexts.size()>=8)return;auto c=std::make_unique<Context>();c->core=core;c->handle=handle;ctx=c.get();contexts.push_back(std::move(c));contexts_count.store(static_cast<uint32_t>(contexts.size()));}
    status.active=0;
    if(!settings::Get().nr_enabled.load()){ctx->feature.reset();Reason("Neural Rendering is off.");return;}
    auto f=ReadFgContract(static_cast<const NVSDK_NGX_Parameter*>(parameters));
    status.input_states_known=0;status.frame_id=f.frame_id;
    status.backbuffer_is_swapchain=IsSwapchain(f.color.resource);
    status.output_real_present=f.real_output.resource!=nullptr;
    status.output_real_is_swapchain=IsSwapchain(f.real_output.resource);
    const auto identity=ctx->identity.Resolve(f);
    status.identity_mode=ctx->identity.mode;status.multi_count=f.has_multi_count?f.multi_count:1;
    status.multi_index=f.has_multi_index?f.multi_index:1;status.list_type=list->GetType();
    if(!identity){++status.skipped;Reason("Neural Rendering needs a valid native FG inner-frame sequence.");return;}
    if(ctx->identity_mode!=ctx->identity.mode){
        // IDs from different protocols must not collide. Recordings retain
        // old payloads independently until their submitted GPU work retires.
        ctx->cache={};for(auto& p:ctx->packets)p.reset();for(auto& live:ctx->live)live=false;
        ctx->feature.reset();ctx->identity_mode=ctx->identity.mode;
    }
    f.frame_id=*identity;status.frame_id=f.frame_id;
    if(!f.camera_motion || f.motion_jittered){++status.skipped;Reason("Neural Rendering needs unjittered camera-inclusive motion vectors.");return;}
    if(!Region(f.color) || !Region(f.hudless) || !Region(f.depth) || !Region(f.motion) ||
        f.color.region.width!=f.hudless.region.width || f.color.region.height!=f.hudless.region.height ||
        f.color.resource->GetDesc().Format!=f.hudless.resource->GetDesc().Format || f.distortion.resource ||
        !f.has_hdr || !f.has_motion_x || !f.has_motion_y || !std::isfinite(f.motion_x) || !std::isfinite(f.motion_y)) {
        ++status.skipped;Reason("Neural Rendering is waiting for matching FG picture and guide regions.");return;
    }
    if(f.color.resource==f.hudless.resource && (f.color.region.x!=f.hudless.region.x || f.color.region.y!=f.hudless.region.y)) {
        ++status.skipped;Reason("Neural Rendering needs separate nonoverlapping FG image regions.");return;
    }
    InputStates input_states(list,f);status.input_states_known=input_states.ready;
    if(!input_states.ready){++status.skipped;Reason("Neural Rendering is waiting for observed native FG input states.");return;}
    using namespace na::hooking::variants;
    if(Active(Kind::CommandList) && CoverageOf(Kind::CommandList,list)!=Coverage::Complete){++status.skipped;Reason("Neural Rendering needs tracked native FG command lists.");return;}
    ComPtr<ID3D12Device> device; if(FAILED(list->GetDevice(IID_PPV_ARGS(&device))))return;
    auto* recording=Track(list,device.Get());if(!recording){++status.skipped;Reason("Neural Rendering is waiting for its recorded frames to finish.");return;}
    // Retain a small history for delayed MFG calls, recycle only payloads with
    // no recorded/submitted consumers left. Slots retain their allocations.
    for(uint32_t i=0;i<FgFrameCache::Capacity;++i) if(ctx->live[i] && ctx->packets[i].use_count()==1 &&
        ctx->packets[i]->id<f.frame_id && f.frame_id-ctx->packets[i]->id>2 && ctx->cache.WasSubmitted(ctx->tickets[i])) {
        if(ctx->cache.RetireCompleted(ctx->tickets[i]))ctx->live[i]=false;
    }
    auto decision=ctx->cache.Acquire(f.frame_id);
    if(decision.action==FgFrameCache::Action::Record && !f.has_frame_id && status.multi_index>1){
        ctx->cache.AbandonUnsubmitted(decision.ticket);++status.skipped;
        Reason("Neural Rendering did not enhance the first FG inner frame.");return;
    }
    if(decision.action==FgFrameCache::Action::Full || decision.action==FgFrameCache::Action::Expired || decision.action==FgFrameCache::Action::Pending){++status.skipped;Reason("Neural Rendering is waiting for its real-frame cache.");return;}
    const auto index=decision.ticket.slot;
    auto& packet=ctx->packets[index];
    if(decision.action!=FgFrameCache::Action::Record) {
        if(!packet || !packet->successful || !packet->composition.Full())return;
        const auto cached=packet->composition.Full()->GetDesc();
        if(cached.Width!=f.color.region.width || cached.Height!=f.color.region.height || cached.Format!=f.color.resource->GetDesc().Format) {
            ++status.skipped;Reason("Native FG reused a frame identity with a different picture shape.");return;
        }
        // Native FG evaluations of one feature already require GPU ordering:
        // its own temporal history and these cached images follow that same
        // caller-established order, without an injected cross-queue wait.
        Keep(*recording,packet);
        recording->destinations.emplace_back(f.color.resource);recording->destinations.emplace_back(f.hudless.resource);
        CopyRegion(list,packet->composition.Full(),f.color);
        if(f.hudless.resource!=f.color.resource)CopyRegion(list,packet->composition.Hudless(),f.hudless);
        ++status.reused;status.active=1;Reason("Neural Rendering is active before native Frame Generation.");return;
    }
    if(!packet)packet=std::make_shared<Packet>();
    else if(packet->receipt_set && !packet->composition.TryRecycle()){ctx->cache.AbandonUnsubmitted(decision.ticket);++status.skipped;return;}
    if(packet->feature && packet->feature->device.Get()!=device.Get())packet=std::make_shared<Packet>();
    packet->id=f.frame_id;packet->receipt_set=false;packet->successful=false;packet->timed=false;
    packet->timing_fence.Reset();packet->timing_value=packet->frequency=0;ctx->live[index]=true;ctx->tickets[index]=decision.ticket;
    packet->producer=recording->valid;
    Keep(*recording,packet); // lifetime starts BEFORE any NR GPU commands
    const auto fmt=f.hudless.resource->GetDesc().Format;
    const uint32_t mode=f.hdr ? (fmt==DXGI_FORMAT_R16G16B16A16_FLOAT?2u:1u):0u;
    if(mode==1 && fmt!=DXGI_FORMAT_R10G10B10A2_UNORM){Reason("Neural Rendering needs a supported HDR encoding.");return;}
    if(!ctx->feature || ctx->feature->w!=f.color.region.width || ctx->feature->h!=f.color.region.height ||
        ctx->feature->mode!=mode || ctx->feature->format!=fmt || ctx->feature->device.Get()!=device.Get() ||
        ctx->feature->model!=settings::Get().nr_model.load()) {
        packet->feature=std::make_shared<Feature>();
        packet->feature->core=core;
        if(!MakeFeature(list,device.Get(),runtime,f,mode,packet->feature)){ctx->feature.reset();Reason("Neural Rendering could not create its pre-FG feature.");return;}
        ctx->feature=packet->feature;
    } else packet->feature=ctx->feature;
    auto& n=*packet->feature; auto* p=n.params;const auto& st=settings::Get();
    const uint64_t signature=st.nr_intensity.load() | (uint64_t(st.nr_structure.load())<<8) |
        (uint64_t(st.nr_tone.load())<<16) | (uint64_t(st.nr_skin.load())<<24) | (uint64_t(st.nr_paper_white.load())<<32);
    // A skipped real image or changed reconstruction settings invalidates NR's
    // temporal history even when the game's own FG history did not reset.
    n.reset=n.reset || signature!=n.signature || f.frame_id-n.last_frame!=1;
    n.signature=signature;
    packet->refs[0]=f.color.resource;packet->refs[1]=f.hudless.resource;packet->refs[2]=f.depth.resource;packet->refs[3]=f.motion.resource;
    if(!packet->queries) {
        D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_TIMESTAMP,2,0};
        device->CreateQueryHeap(&qd,IID_PPV_ARGS(&packet->queries));
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rd.Width=16;rd.Height=rd.DepthOrArraySize=rd.MipLevels=1;
        rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&packet->timestamps));
    }
    if(packet->queries && packet->timestamps)list->EndQuery(packet->queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
    CompositionSurface original{f.hudless.resource,f.hudless.region.x,f.hudless.region.y};
    if(mode && !packet->input.Record(list,false,original,nullptr,nullptr,n.color.Get(),mode,static_cast<float>(st.nr_paper_white.load())))return;
    p->Set("DLSSNR.Color",mode?n.color.Get():f.hudless.resource);p->Set("DLSSNR.Depth",f.depth.resource);
    p->Set("DLSSNR.MVec",f.motion.resource);p->Set("DLSSNR.Output",n.nr.Get());p->Set("DLSSNR.Reset",n.reset||f.reset?1u:0u);
    p->Set("DLSSNR.DepthInverted",f.depth_inverted);p->Set("DLSSNR.MVecScaleX",f.motion_x*n.w);p->Set("DLSSNR.MVecScaleY",f.motion_y*n.h);
    p->Set("DLSSNR.Intensity",st.nr_intensity.load()/100.f);p->Set("DLSSNR.LocalStructureStrength",st.nr_structure.load()/100.f);
    p->Set("DLSSNR.LocalToneStrength",st.nr_tone.load()/100.f);p->Set("DLSSNR.SkinStructureStrength",st.nr_skin.load()/100.f);
    const FgRegion compact{0,0,n.w,n.h,15};SetRegion(p,"Color",mode?compact:f.hudless.region);
    SetRegion(p,"Depth",f.depth.region);SetRegion(p,"MVec",f.motion.region);SetRegion(p,"Output",compact);
    if(!Good(reinterpret_cast<EvaluateFn>(n.rt.evaluate_feature)(list,n.handle,p,nullptr))){n.reset=true;ctx->feature.reset();Reason("Neural Rendering pre-FG evaluation failed.");return;}
    n.reset=false;n.last_frame=f.frame_id;++status.evaluates;
    Barrier(list,n.nr.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const bool converted=!mode || packet->output.Record(list,true,original,n.nr.Get(),n.color.Get(),n.enhanced.Get(),mode,static_cast<float>(st.nr_paper_white.load()));
    const bool composed=converted && packet->composition.Record(list,n.w,n.h,{f.color.resource,f.color.region.x,f.color.region.y},
        original,{mode?n.enhanced.Get():n.nr.Get(),0,0},{},UiComposition::EqualPixels);
    Barrier(list,n.nr.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if(!composed){Reason("Neural Rendering could not compose the pre-FG picture.");return;}
    CopyRegion(list,packet->composition.Full(),f.color);
    if(f.hudless.resource!=f.color.resource)CopyRegion(list,packet->composition.Hudless(),f.hudless);
    if(packet->queries && packet->timestamps) {
        list->EndQuery(packet->queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);
        list->ResolveQueryData(packet->queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,packet->timestamps.Get(),0);
    }
    packet->successful=true;ctx->cache.Recorded(decision.ticket);status.width=n.w;status.height=n.h;status.active=1;
    Reason("Neural Rendering is active before native Frame Generation.");
}
Submission::Submission(ID3D12CommandQueue* q,UINT n,ID3D12CommandList* const* lists) {
    if(internal || !records_count.load())return;
    for(UINT i=0;i<n && count_<64;++i) {
        IUnknown* raw=nullptr;UINT bytes=sizeof(raw);
        if(lists[i] && SUCCEEDED(lists[i]->GetPrivateData(token_id,&bytes,&raw)) && raw) {
            auto r=static_cast<Token*>(raw)->recording.lock();raw->Release();
            if(r && r->valid->load() && std::find(records_,records_+count_,r)==records_+count_)records_[count_++]=r;
        }
    }
    if(!count_)return; // ordinary game/helper submissions never enter an NR lock
    std::sort(records_,records_+count_,[](const auto& a,const auto& b){return std::less<const void*>{}(a.get(),b.get());});
    ++internal;entered_=true;queue_=q;
    for(uint32_t i=0;i<count_;++i){auto& r=*static_cast<Recording*>(records_[i].get());
        r.forwarding.fetch_add(1);locks_[i]=std::unique_lock<std::mutex>(r.submission_mutex);}
}
Submission::~Submission() {
    if(!queue_)return;
    for(uint32_t i=0;i<count_;++i){auto& r=*static_cast<Recording*>(records_[i].get());
        uint64_t frequency=0;queue_->GetTimestampFrequency(&frequency);r.frequency.store(frequency);
        if(FAILED(queue_->Signal(r.fence.Get(),++r.value)))r.failed.store(true);
        else submissions.fetch_add(1);
        r.forwarding.fetch_sub(1);
        locks_[i].unlock(); // before the shared Recording owner can be destroyed
    }
    if(entered_)--internal;
}
} // namespace na::nr::native
#else
namespace na::nr::native {
void Before(ID3D12GraphicsCommandList*,const void*,const void*,const void*,const Runtime&){}
void Release(const void*,const void*){} void Prepare(ID3D12GraphicsCommandList*){} void Maintain(){}
void PrepareDevice(ID3D12Device*){}
void MarkSwapchainBuffer(ID3D12Resource*){}
void Register(const void*,const void*){}
void CoreShutdown(const void*){}
void Reset(ID3D12GraphicsCommandList*){} bool OwnsRoute(){return false;} Status GetStatus(){return {};}
Submission::Submission(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*){} Submission::~Submission(){}
}
#endif
