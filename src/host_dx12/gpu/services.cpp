#include "services.hpp"
#include "host_dx12/settings.hpp"
#include "host_dx12/observe/producers.hpp"
#include <atomic>
#include <mutex>
#include <vector>
#include <cstdio>
#include <algorithm>
#include <unordered_map>
#include <cstring>
#if defined(NA_HAVE_NVRHI)
#pragma warning(push,3)
#include <nvrhi/nvrhi.h>
#include <nvrhi/d3d12.h>
#pragma warning(pop)
#endif
namespace na::gpu {
namespace {
std::atomic<uint64_t> textures{0},opens{0},submits{0},collections{0},errors{0},dispatches{0},bindings{0},retained{0};
#if defined(NA_HAVE_NVRHI)
struct Messages final:nvrhi::IMessageCallback {
    void message(nvrhi::MessageSeverity severity,const char* text)override {
        if(severity==nvrhi::MessageSeverity::Error||severity==nvrhi::MessageSeverity::Fatal)++errors;
        OutputDebugStringA(text);OutputDebugStringA("\n");
    }
};
struct Kernel {std::vector<uint8_t> code;uint32_t constants=0;nvrhi::BindingLayoutHandle layout;nvrhi::ComputePipelineHandle pipeline;nvrhi::SamplerHandle sampler;};
struct Context {
    Microsoft::WRL::ComPtr<ID3D12Device> native;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    Messages messages;nvrhi::DeviceHandle device;
    std::mutex submit,kernel_mutex;std::vector<std::shared_ptr<Kernel>> kernels;
    struct Pending {nvrhi::EventQueryHandle query;std::vector<std::shared_ptr<void>> owners;};
    std::vector<Pending> pending;
    void collect(){
        if(native->GetDeviceRemovedReason()!=S_OK)return;
        device->runGarbageCollection();
        pending.erase(std::remove_if(pending.begin(),pending.end(),[&](const auto& p){if(!device->pollEventQuery(p.query))return false;retained-=p.owners.size();return true;}),pending.end());
        ++collections;
    }

};
// Bounded process-lifetime device/queue contexts: NVRHI's Device destructor
// waits for idle, so it must never run from a live toggle/resize callback.
// Textures are not cached here; external recording owners release them normally.
struct Active {std::shared_ptr<Context> context;nvrhi::CommandListHandle list;};
struct ActiveRegistry {std::mutex mutex;std::unordered_map<ID3D12GraphicsCommandList*,Active> recordings;};
// Exported recordings may be released by other translation units at DLL exit.
// Keep the empty registry alive so their destructors never touch dead statics.
ActiveRegistry& activeRegistry(){static auto* r=new ActiveRegistry;return *r;}
struct Registry {std::mutex mutex;std::vector<std::shared_ptr<Context>> contexts;};
Registry& registry(){static auto* r=new Registry;return *r;}
std::shared_ptr<Context> context(ID3D12Device* device,ID3D12CommandQueue* queue) {
    if(!device||!queue||queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)return {};
    auto& r=registry();std::lock_guard<std::mutex> guard(r.mutex);
    for(auto& c:r.contexts)if(c->native.Get()==device&&c->queue.Get()==queue)return c;
    if(r.contexts.size()>=8)return {};
    Microsoft::WRL::ComPtr<ID3D12Device> queueDevice;
    if(FAILED(queue->GetDevice(IID_PPV_ARGS(&queueDevice)))||queueDevice.Get()!=device)return {};
    na::observe::HostWorkScope own;
    auto c=std::make_shared<Context>();c->native=device;c->queue=queue;
    nvrhi::d3d12::DeviceDesc desc;desc.pDevice=device;desc.pGraphicsCommandQueue=queue;desc.errorCB=&c->messages;
    desc.shaderResourceViewHeapSize=2048;desc.samplerHeapSize=64;desc.renderTargetViewHeapSize=128;desc.depthStencilViewHeapSize=64;
    desc.enableEnhancedBarriers=false;desc.enableDxrClusters=false;desc.enableNvapiClusters=false;
    c->device=nvrhi::d3d12::createDevice(desc);if(!c->device)return {};
    r.contexts.push_back(c);return c;
}
nvrhi::Format format(DXGI_FORMAT f){using F=nvrhi::Format;switch(f){
    case DXGI_FORMAT_R8_UINT:return F::R8_UINT;case DXGI_FORMAT_R8_UNORM:return F::R8_UNORM;
    case DXGI_FORMAT_R16_FLOAT:return F::R16_FLOAT;case DXGI_FORMAT_R32_FLOAT:return F::R32_FLOAT;case DXGI_FORMAT_R32_UINT:return F::R32_UINT;
    case DXGI_FORMAT_R16G16_FLOAT:return F::RG16_FLOAT;case DXGI_FORMAT_R32G32_FLOAT:return F::RG32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:return F::RGBA16_FLOAT;case DXGI_FORMAT_R32G32B32A32_FLOAT:return F::RGBA32_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_UNORM:return F::RGBA8_UNORM;case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:return F::SRGBA8_UNORM;
    case DXGI_FORMAT_R11G11B10_FLOAT:return F::R11G11B10_FLOAT;default:return F::UNKNOWN;}}
nvrhi::ResourceStates state(D3D12_RESOURCE_STATES s){using S=nvrhi::ResourceStates;
    switch(s){case D3D12_RESOURCE_STATE_COMMON:return S::Common;case D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE:return S::NonPixelShaderResource;
    case D3D12_RESOURCE_STATE_UNORDERED_ACCESS:return S::UnorderedAccess;case D3D12_RESOURCE_STATE_COPY_SOURCE:return S::CopySource;
    case D3D12_RESOURCE_STATE_COPY_DEST:return S::CopyDest;case D3D12_RESOURCE_STATE_RENDER_TARGET:return S::RenderTarget;default:return S::Unknown;}}
struct Allocation {std::shared_ptr<Context> context;nvrhi::TextureHandle texture;~Allocation(){na::observe::HostWorkScope own;texture=nullptr;}};
#endif
}
bool Enabled(){return na::settings::Get().experimental_nvrhi.load(std::memory_order_relaxed)!=0;}
bool CreateTexture(ID3D12Device* dev,ID3D12CommandQueue* queue,const D3D12_RESOURCE_DESC& td,D3D12_RESOURCE_STATES initial,Texture& out){
    na::observe::HostWorkScope own;out.Reset();
#if defined(NA_HAVE_NVRHI)
    if(Enabled()&&td.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&td.Width<=UINT32_MAX&&td.DepthOrArraySize==1&&
       format(td.Format)!=nvrhi::Format::UNKNOWN&&state(initial)!=nvrhi::ResourceStates::Unknown){
        if(auto ctx=context(dev,queue)){
            auto a=std::make_shared<Allocation>();a->context=ctx;nvrhi::TextureDesc d;
            d.width=uint32_t(td.Width);d.height=td.Height;d.mipLevels=td.MipLevels;d.sampleCount=td.SampleDesc.Count;d.sampleQuality=td.SampleDesc.Quality;
            d.format=format(td.Format);d.isUAV=(td.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)!=0;
            d.isRenderTarget=(td.Flags&D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)!=0;
            d.isShaderResource=(td.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)==0;d.initialState=state(initial);d.keepInitialState=false;
            a->texture=ctx->device->createTexture(d);
            if(a->texture){out=Texture(static_cast<ID3D12Resource*>(a->texture->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource)));out.owner_=a;++textures;return true;}
        }
    }
#else
    (void)queue;
#endif
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    return SUCCEEDED(dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,initial,nullptr,IID_PPV_ARGS(&out)));
}
struct Recording::State {
#if defined(NA_HAVE_NVRHI)
    std::shared_ptr<Context> context;nvrhi::CommandListHandle list;
#endif
    std::vector<std::shared_ptr<void>> owners;
    ID3D12GraphicsCommandList* native=nullptr;bool open=false,closed=false,submitted=false;
};
Recording::Recording():state_(std::make_unique<State>()){}Recording::~Recording(){Discard();}
bool Recording::Open(ID3D12Device* dev,ID3D12CommandQueue* queue){
#if defined(NA_HAVE_NVRHI)
    if(!Enabled()||state_->open)return false;
    na::observe::HostWorkScope own;auto ctx=context(dev,queue);if(!ctx)return false;
    if(state_->closed&&!state_->submitted)Discard();
    if(state_->context!=ctx||!state_->list){state_->context=ctx;state_->list=ctx->device->createCommandList();}
    if(!state_->list)return false;
    state_->list->open();state_->list->setEnableAutomaticBarriers(false);
    state_->native=static_cast<ID3D12GraphicsCommandList*>(state_->list->getNativeObject(nvrhi::ObjectTypes::D3D12_GraphicsCommandList));
    state_->open=state_->native!=nullptr;state_->closed=state_->submitted=false;if(state_->open){auto& a=activeRegistry();std::lock_guard<std::mutex> lock(a.mutex);a.recordings[state_->native]={ctx,state_->list};++opens;}return state_->open;
#else
    (void)dev;(void)queue;return false;
#endif
}
ID3D12GraphicsCommandList* Recording::Native()const{return state_->native;}
bool Recording::Close(){
    if(!state_->open)return false;na::observe::HostWorkScope own;
#if defined(NA_HAVE_NVRHI)
    {auto& a=activeRegistry();std::lock_guard<std::mutex> lock(a.mutex);a.recordings.erase(state_->native);}
    state_->list->close();
#endif
    state_->open=false;state_->closed=true;return true;
}
bool Recording::Ready(ID3D12CommandQueue* queue)const{
#if defined(NA_HAVE_NVRHI)
    return state_->closed&&!state_->submitted&&state_->context&&state_->context->queue.Get()==queue;
#else
    (void)queue;return false;
#endif
}
bool Recording::Submit(){
#if defined(NA_HAVE_NVRHI)
    if(!state_->closed||state_->submitted)return false;
    na::observe::HostWorkScope own;auto c=state_->context;std::lock_guard<std::mutex> lock(c->submit);
    const auto instance=c->device->executeCommandList(state_->list);state_->submitted=true;
    if(!state_->owners.empty()){
        auto query=c->device->createEventQuery();
        retained+=state_->owners.size();c->device->setEventQuery(query,nvrhi::CommandQueue::Graphics);
        c->pending.push_back({query,std::move(state_->owners)});
    }
    c->collect();if(instance){++submits;return true;}++errors;
#endif
    return false;
}
void Recording::Retain(std::shared_ptr<void> p){if(p)state_->owners.push_back(std::move(p));}
void Recording::Discard(){
    na::observe::HostWorkScope own;
    if(state_->open)Close();
#if defined(NA_HAVE_NVRHI)
    {auto& a=activeRegistry();std::lock_guard<std::mutex> lock(a.mutex);a.recordings.erase(state_->native);}
    // Context stays cached; releasing a discarded list never destroys its device.
    state_->list=nullptr;state_->context.reset();
#endif
    state_->owners.clear();state_->native=nullptr;state_->open=state_->closed=state_->submitted=false;
}
bool SubmitMixed(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList* const* lists,Recording* owned,Forward forward){
    if(!queue||!count||!lists||!forward)return false;
    if(!owned||!owned->Native()){forward(queue,count,lists);return true;}
    unsigned matches=0;for(UINT i=0;i<count;++i)matches+=lists[i]==owned->Native();
    // These owned recordings are single-use; replayed game lists remain native.
    if(matches!=1||!owned->Ready(queue)){++errors;return false;}
    UINT first=0;bool ok=true;for(UINT i=0;i<count;++i)if(lists[i]==owned->Native()){
        if(i>first)forward(queue,i-first,lists+first);
        ok=owned->Submit();if(!ok)return false;first=i+1;
    }
    if(first<count)forward(queue,count-first,lists+first);return ok;
}

class ComputePacket {
public:
#if defined(NA_HAVE_NVRHI)
    std::shared_ptr<Context> context;std::shared_ptr<Kernel> kernel;
    nvrhi::BindingSetHandle bindings;
    Microsoft::WRL::ComPtr<ID3D12Resource> keep[5];
    ~ComputePacket(){na::observe::HostWorkScope own;bindings=nullptr;kernel.reset();context.reset();}
#endif
};
std::shared_ptr<ComputePacket> PrepareCompute(ID3D12Device* dev,ID3D12CommandQueue* queue,const void* code,size_t bytes,uint32_t constantBytes,const ComputeBindings& b){
#if defined(NA_HAVE_NVRHI)
    if(!Enabled()||!code||!bytes||!constantBytes||constantBytes>128||constantBytes%4)return {};
    auto ctx=context(dev,queue);if(!ctx)return {};na::observe::HostWorkScope own;
    std::shared_ptr<Kernel> kernel;
    {std::lock_guard<std::mutex> lock(ctx->kernel_mutex);
        for(auto& k:ctx->kernels)if(k->constants==constantBytes&&k->code.size()==bytes&&!std::memcmp(k->code.data(),code,bytes)){kernel=k;break;}
        if(!kernel){
            kernel=std::make_shared<Kernel>();kernel->constants=constantBytes;kernel->code.assign(static_cast<const uint8_t*>(code),static_cast<const uint8_t*>(code)+bytes);
            nvrhi::BindingLayoutDesc layout;layout.visibility=nvrhi::ShaderType::Compute;
            layout.bindings={nvrhi::BindingLayoutItem::PushConstants(0,constantBytes),nvrhi::BindingLayoutItem::Texture_SRV(0),nvrhi::BindingLayoutItem::Texture_SRV(1),
                nvrhi::BindingLayoutItem::Texture_UAV(0),nvrhi::BindingLayoutItem::TypedBuffer_UAV(1),nvrhi::BindingLayoutItem::TypedBuffer_UAV(2),nvrhi::BindingLayoutItem::Sampler(0)};
            kernel->layout=ctx->device->createBindingLayout(layout);
            nvrhi::ShaderDesc sd;sd.shaderType=nvrhi::ShaderType::Compute;
            auto shader=ctx->device->createShader(sd,code,bytes);
            if(!shader||!kernel->layout)return {};
            nvrhi::ComputePipelineDesc pd;pd.CS=shader;pd.bindingLayouts={kernel->layout};kernel->pipeline=ctx->device->createComputePipeline(pd);
            nvrhi::SamplerDesc sampler;sampler.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
            kernel->sampler=ctx->device->createSampler(sampler);if(!kernel->pipeline||!kernel->sampler)return {};
            ctx->kernels.push_back(kernel);
        }
    }
    auto packet=std::make_shared<ComputePacket>();packet->context=ctx;packet->kernel=kernel;
    const auto importTexture=[&](ID3D12Resource* r,DXGI_FORMAT f)->nvrhi::TextureHandle{
        if(!r)return {};const auto native=r->GetDesc();nvrhi::TextureDesc d;d.width=uint32_t(native.Width);d.height=native.Height;
        d.mipLevels=native.MipLevels;d.format=format(f);d.isUAV=(native.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)!=0;
        d.isRenderTarget=(native.Flags&D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)!=0;d.initialState=nvrhi::ResourceStates::Unknown;d.keepInitialState=false;
        return ctx->device->createHandleForNativeTexture(nvrhi::ObjectTypes::D3D12_Resource,r,d);
    };
    const auto importBuffer=[&](ID3D12Resource* r)->nvrhi::BufferHandle{
        if(!r)return {};nvrhi::BufferDesc d;d.byteSize=r->GetDesc().Width;d.format=nvrhi::Format::R32_UINT;d.canHaveTypedViews=true;d.canHaveUAVs=true;
        d.initialState=nvrhi::ResourceStates::Unknown;d.keepInitialState=false;
        return ctx->device->createHandleForNativeBuffer(nvrhi::ObjectTypes::D3D12_Resource,r,d);
    };
    auto s0=importTexture(b.source[0],b.source_format[0]),s1=importTexture(b.source[1],b.source_format[1]);
    auto dest=importTexture(b.destination,b.destination_format);auto u1=importBuffer(b.uint_buffers[0]),u2=importBuffer(b.uint_buffers[1]);
    nvrhi::BindingSetDesc desc;desc.bindings={nvrhi::BindingSetItem::PushConstants(0,constantBytes),nvrhi::BindingSetItem::Sampler(0,kernel->sampler)};
    // Omit unused slots. The pinned backend creates null descriptors for absent
    // bindings, but dereferences a Texture_SRV item with a null resource.
    if(s0)desc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(0,s0));
    if(s1)desc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(1,s1));
    if(dest)desc.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(0,dest));
    if(u1)desc.bindings.push_back(nvrhi::BindingSetItem::TypedBuffer_UAV(1,u1));
    if(u2)desc.bindings.push_back(nvrhi::BindingSetItem::TypedBuffer_UAV(2,u2));
    packet->bindings=ctx->device->createBindingSet(desc,kernel->layout);if(!packet->bindings)return {};
    packet->keep[0]=b.source[0];packet->keep[1]=b.source[1];packet->keep[2]=b.destination;packet->keep[3]=b.uint_buffers[0];packet->keep[4]=b.uint_buffers[1];++bindings;return packet;
#else
    (void)dev;(void)queue;(void)code;(void)bytes;(void)constantBytes;(void)b;return {};
#endif
}
bool Dispatch(ID3D12GraphicsCommandList* native,const std::shared_ptr<ComputePacket>& packet,const void* constants,uint32_t bytes,uint32_t x,uint32_t y){
#if defined(NA_HAVE_NVRHI)
    if(!packet||!native||!constants||bytes!=packet->kernel->constants)return false;
    Active recording;
    {auto& a=activeRegistry();std::lock_guard<std::mutex> lock(a.mutex);auto it=a.recordings.find(native);if(it==a.recordings.end())return false;recording=it->second;}
    if(recording.context!=packet->context)return false;
    na::observe::HostWorkScope own;
    // Native pre/post commands may have changed every binding. Clear NVRHI's
    // cache before returning to it. Exact transitions remain with the caller.
    recording.list->clearState();recording.list->setEnableAutomaticBarriers(false);
    nvrhi::ComputeState state;state.pipeline=packet->kernel->pipeline;state.bindings={packet->bindings};
    recording.list->setComputeState(state);recording.list->setPushConstants(constants,bytes);recording.list->dispatch(x,y,1);++dispatches;return true;
#else
    (void)native;(void)packet;(void)constants;(void)bytes;(void)x;(void)y;return false;
#endif
}

void Collect(){
#if defined(NA_HAVE_NVRHI)
    na::observe::HostWorkScope own;auto& r=registry();std::lock_guard<std::mutex> guard(r.mutex);
    for(auto& c:r.contexts){std::lock_guard<std::mutex> lock(c->submit);c->collect();}
#endif
}
Stats GetStats(){Stats s;s.textures=textures;s.opens=opens;s.submits=submits;s.collections=collections;s.errors=errors;s.dispatches=dispatches;s.bindings=bindings;s.retained=retained;
#if defined(NA_HAVE_NVRHI)
    s.built=true;
#endif
    return s;
}
}
