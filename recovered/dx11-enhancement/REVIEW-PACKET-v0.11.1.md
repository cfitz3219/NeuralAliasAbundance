# Independent review packet: DX11 Enhancement v0.11.1

This packet contains current source, not a claim of correctness. Please audit it
independently, cite file/function evidence, and distinguish observed results from
predicted risks. No new gameplay result exists for this build. The code/runtime
interface is experimental. Logs are developer-produced evidence, not independent
verification. No runtime binaries, SDK libraries or captured game imagery included.

## Corrections and qualifications to the supplied review

1. Reinhard plus sRGB approximation belongs to the isolated replay experiment in
   tests/neural_image_probe.hpp. The installed live path in alien_neural.cpp copies
   the game's final SDR UNORM image; neural_bridge.cpp performs a float-format blit
   without added tone mapping or gamma conversion. This distinction does NOT prove
   final-color/depth/motion alignment or HUD handling correct.
2. DX11_DLAA.fx is a control-only no-op compute shader. It performs no tone mapping,
   DLAA or neural dispatch. DX11_Neural.fx is also a control interface. Actual neural
   creation/evaluation occurs in neural_helper.cpp; stage/control callbacks are in
   depth_addon.cpp and alien_neural.cpp. All are included below.
3. Model request hints 0..3 produced identical output in the supplied tests. We do
   NOT claim distinct weights are selected. STYLE is separate: 0/1/2 produced
   different output. Community style nicknames are not official NVIDIA names.
4. Missing UI separation and engine camera-cut detection are real unresolved risks.
   HUD pixels enter processing, but how the network interprets them is not observed.
   Heavy smearing is a prediction, not a reported result. Resets include first frame,
   discontinuous frame numbers, >250ms gaps, display resets, neural setting changes
   and DLAA setting changes. None substitutes for a reliable camera-cut signal.
5. The recorded CPU output-fence wait includes waiting for queued GPU neural work
   and copy completion, plus scheduling/synchronization. It is NOT merely bridge/API
   overhead, nor an isolated GPU timing or game-FPS measurement. Prior direct DX12
   probe GPU brackets were approximately4.7–4.9ms for two saved1920x1200frames only.
   The end-to-end game performance question remains unmeasured.
6. DX11 capability/create failure establishes this runtime's tested path did not
   work; it is not proof every future/runtime DX11 implementation is impossible.
7. Third-party mirror provenance and undocumented API dependence are real limits.
   A valid NVIDIA signature checks publisher/integrity, not compatibility or rights.
   Future breakage is possible, not guaranteed. Installer pins the tested DLL hash.
8. Controls are applied when creating the feature, and changes recreate the helper.
   Calling them only evaluation-time controls misses a relevant lifecycle detail.

## Suggested audit scope

Review shared resource state transitions, fence/metadata ordering, adapter identity,
parent/helper shutdown and timeout cleanup; texture/view validation and graphics
state preservation; temporal input/reset consistency; encoded color handling;
control parsing/application/failure latches; installer ownership and rollback.
Check test coverage rather than treating byte equality as visual/temporal proof.
Do not assume fields in BridgeFrame are consumed by NR: trace what the helper uses.
Request missing dependencies or evidence explicitly. No need to endorse our design.

## Included source and evidence

Each source section below is an exact UTF-8 text copy of the current file, with a
SHA-256 of the original bytes. The ZIP supplies the same files at original paths.

## src/core/neural_transport.hpp

SHA-256: ee52009ccbe88f48f5b8f718c8c10608a9ba64a9208507c2b23152fa985627a7

````
#pragma once
#include "bridge_transport.hpp"
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <utility>

namespace dx11_enhancement::neural {
using namespace transport;
struct Handle {
    HANDLE value=nullptr;
    Handle()=default;
    explicit Handle(HANDLE h):value(h){}
    ~Handle(){if(value)CloseHandle(value);}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
    Handle(Handle&& other)noexcept:value(std::exchange(other.value,nullptr)){}
    Handle& operator=(Handle&& other)noexcept {
        if(this!=&other){if(value)CloseHandle(value);value=std::exchange(other.value,nullptr);}return *this;
    }
};
struct Texture11 {
    ComPtr<ID3D11Texture2D> image;
    Handle share;
    void create(ID3D11Device* device,UINT width,UINT height,DXGI_FORMAT format,const std::wstring& name) {
        D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.MipLevels=d.ArraySize=1;
        d.Format=format;d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
        d.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        require(device->CreateTexture2D(&d,nullptr,&image),"neural shared texture");
        ComPtr<IDXGIResource1> resource;require(image.As(&resource),"NT texture interface");
        require(resource->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,name.c_str(),&share.value),"NT texture handle");
    }
};
struct Fence11 {
    ComPtr<ID3D11Fence> fence;
    Handle share,event;
    void create(ID3D11Device* device,const std::wstring& name) {
        ComPtr<ID3D11Device5> modern;require(device->QueryInterface(IID_PPV_ARGS(&modern)),"D3D11 fence device");
        require(modern->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&fence)),"shared fence");
        require(fence->CreateSharedHandle(nullptr,GENERIC_ALL,name.c_str(),&share.value),"fence handle");
        event=Handle(CreateEventW(nullptr,FALSE,FALSE,nullptr));if(!event.value)throw std::runtime_error("fence event");
    }
    void signal(ID3D11DeviceContext* context,uint64_t value) {
        ComPtr<ID3D11DeviceContext4> modern;require(context->QueryInterface(IID_PPV_ARGS(&modern)),"D3D11 fence context");
        require(modern->Signal(fence.Get(),value),"D3D11 fence signal");context->Flush();
    }
    void wait(uint64_t value,HANDLE child,DWORD timeout) {
        const auto completed=fence->GetCompletedValue();
        if(completed==UINT64_MAX)throw std::runtime_error("neural fence device removed");
        if(completed>=value)return;
        require(fence->SetEventOnCompletion(value,event.value),"fence completion event");
        const HANDLE handles[]={child,event.value};
        const auto waited=WaitForMultipleObjects(2,handles,FALSE,timeout);
        if(waited==WAIT_OBJECT_0)throw std::runtime_error("neural helper exited before output");
        if(waited!=WAIT_OBJECT_0+1 || fence->GetCompletedValue()<value || fence->GetCompletedValue()==UINT64_MAX)
            throw std::runtime_error("neural output deadline/device failure");
    }
};
}

````

## src/core/neural_bridge.hpp

SHA-256: 53ea4d839e3268f8c6d821e67a25b57967f73bcce22f0b6dbc69c75b554e3533

````
#pragma once
#include "bridge_protocol.hpp"
#include "neural_settings.hpp"
#include <d3d11.h>
#include <filesystem>
#include <memory>
namespace dx11_enhancement {
class NeuralBridge {
public:
    NeuralBridge();~NeuralBridge();
    void start(ID3D11DeviceContext*,UINT width,UINT height,const std::filesystem::path& helper,
        const std::filesystem::path& runtime,const std::filesystem::path& cache,NeuralSettings settings={});
    bool started()const;bool ready();void stop();
    // Inputs must be same-extent textures. Color is display-encoded; no tone mapping here.
    void process(ID3D11DeviceContext*,ID3D11ShaderResourceView* color,ID3D11ShaderResourceView* depth,
        ID3D11ShaderResourceView* motion,ID3D11RenderTargetView* output,BridgeFrame);
    double last_wait_ms()const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}

````

## src/core/neural_bridge.cpp

SHA-256: 5ce45c6145c122b29a3f4de637bf45961b9b3ed4cd53f2a57f3c8ad97ff58e93

````
#include "neural_bridge.hpp"
#include "neural_transport.hpp"
#include <d3dcompiler.h>
namespace dx11_enhancement {
using namespace neural;
struct NeuralBridge::Impl {
    ComPtr<ID3D11Device> device;Texture11 color,depth,motion,output;Fence11 input_ready,output_ready;
    std::unique_ptr<Metadata> metadata;Child child;Handle ready,stop;
    UINT width=0,height=0,sequence=0;uint64_t startup=GetTickCount64();double wait_ms=0;
    ComPtr<ID3D11DeviceContext> deferred;ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> copy,restore;
    ComPtr<ID3D11RenderTargetView> color_rt;ComPtr<ID3D11ShaderResourceView> input_view,output_view;
    ~Impl(){if(stop.value)SetEvent(stop.value);if(child.process.hProcess)WaitForSingleObject(child.process.hProcess,100);}
    void blit(ID3D11DeviceContext* context,ID3D11ShaderResourceView* source,ID3D11RenderTargetView* target,bool returning){
        deferred->ClearState();D3D11_VIEWPORT vp{0,0,float(width),float(height),0,1};deferred->RSSetViewports(1,&vp);
        deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);deferred->VSSetShader(vs.Get(),nullptr,0);
        deferred->PSSetShader(returning?restore.Get():copy.Get(),nullptr,0);
        ID3D11ShaderResourceView* views[]={source,returning?input_view.Get():nullptr};deferred->PSSetShaderResources(0,2,views);
        deferred->OMSetRenderTargets(1,&target,nullptr);deferred->Draw(3,0);
        ComPtr<ID3D11CommandList> commands;require(deferred->FinishCommandList(FALSE,&commands),"NR blit recording");context->ExecuteCommandList(commands.Get(),TRUE);
    }
    ComPtr<ID3D11Resource> validate_view(ID3D11ShaderResourceView* view,DXGI_FORMAT expected=DXGI_FORMAT_UNKNOWN){
        if(!view)throw std::runtime_error("missing NR input");
        D3D11_SHADER_RESOURCE_VIEW_DESC v{};view->GetDesc(&v);ComPtr<ID3D11Resource> r;view->GetResource(&r);
        ComPtr<ID3D11Texture2D> t;require(r.As(&t),"NR input texture");D3D11_TEXTURE2D_DESC d{};t->GetDesc(&d);
        if(d.Width!=width || d.Height!=height || d.ArraySize!=1 || d.MipLevels!=1 || d.SampleDesc.Count!=1 ||
            v.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D || v.Texture2D.MostDetailedMip!=0 ||
            (expected!=DXGI_FORMAT_UNKNOWN && v.Format!=expected))throw std::runtime_error("NR input layout mismatch");
        return r;
    }
};
NeuralBridge::NeuralBridge()=default;NeuralBridge::~NeuralBridge()=default;
bool NeuralBridge::started()const{return bool(impl_);}void NeuralBridge::stop(){impl_.reset();}
double NeuralBridge::last_wait_ms()const{return impl_?impl_->wait_ms:0;}
void NeuralBridge::start(ID3D11DeviceContext* context,UINT w,UINT h,const std::filesystem::path& helper,const std::filesystem::path& runtime,const std::filesystem::path& cache,NeuralSettings settings){
    settings.validate();
    if(!context || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE || !w || !h || w>4096 || h>4096)throw std::runtime_error("NR unsupported context/size");
    stop();auto s=std::make_unique<Impl>();context->GetDevice(&s->device);s->width=w;s->height=h;
    ComPtr<IDXGIDevice> dxgi;require(s->device.As(&dxgi),"NR DXGI device");ComPtr<IDXGIAdapter> adapter;require(dxgi->GetAdapter(&adapter),"NR adapter");DXGI_ADAPTER_DESC info{};require(adapter->GetDesc(&info),"NR adapter info");
    const auto name=L"Local\\DX11Neural-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
    s->color.create(s->device.Get(),w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,name+L".color");s->depth.create(s->device.Get(),w,h,DXGI_FORMAT_R32_FLOAT,name+L".depth");
    s->motion.create(s->device.Get(),w,h,DXGI_FORMAT_R16G16_FLOAT,name+L".motion");s->output.create(s->device.Get(),w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,name+L".output");
    s->input_ready.create(s->device.Get(),name+L".input-ready");s->output_ready.create(s->device.Get(),name+L".output-ready");
    require(s->device->CreateRenderTargetView(s->color.image.Get(),nullptr,&s->color_rt),"NR color RTV");
    require(s->device->CreateShaderResourceView(s->color.image.Get(),nullptr,&s->input_view),"NR input SRV");
    require(s->device->CreateShaderResourceView(s->output.image.Get(),nullptr,&s->output_view),"NR output SRV");
    require(s->device->CreateDeferredContext(0,&s->deferred),"NR conversion context");
    const char shader[]=R"(Texture2D<float4> source:register(t0);Texture2D<float4> original:register(t1);
float4 vs(uint id:SV_VertexID):SV_Position{return float4(id==2?3:-1,id==1?3:-1,0,1);}
float4 ps(float4 p:SV_Position):SV_Target{return source.Load(int3(int2(p.xy),0));}
float4 restore(float4 p:SV_Position):SV_Target{return float4(source.Load(int3(int2(p.xy),0)).rgb,original.Load(int3(int2(p.xy),0)).a);})";
    ComPtr<ID3DBlob> code,errors;
    require(D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"vs","vs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&errors),"NR vertex compile");require(s->device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&s->vs),"NR vertex");
    for(bool returning:{false,true}){code.Reset();errors.Reset();require(D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,returning?"restore":"ps","ps_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&errors),"NR pixel compile");
        require(s->device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,returning?&s->restore:&s->copy),"NR pixel");}
    s->metadata=std::make_unique<Metadata>(name,true);
    s->ready=Handle(CreateEventW(nullptr,TRUE,FALSE,(name+L".ready").c_str()));s->stop=Handle(CreateEventW(nullptr,TRUE,FALSE,(name+L".stop").c_str()));
    if(!s->ready.value || !s->stop.value)throw std::runtime_error("NR lifecycle events");std::filesystem::create_directories(cache);settings.save(cache/L"settings.txt");
    std::wstring command=L"\""+helper.wstring()+L"\" "+name+L" "+std::to_wstring(info.AdapterLuid.LowPart)+L" "+std::to_wstring(info.AdapterLuid.HighPart)+L" "+std::to_wstring(w)+L" "+std::to_wstring(h)+L" \""+runtime.wstring()+L"\" \""+cache.wstring()+L"\" "+std::to_wstring(GetCurrentProcessId())+L" 1";
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;
    if(!CreateProcessW(helper.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&s->child.process))throw std::runtime_error("NR helper launch");
    impl_=std::move(s);
}
bool NeuralBridge::ready(){if(!impl_)return false;
    if(WaitForSingleObject(impl_->child.process.hProcess,0)!=WAIT_TIMEOUT)throw std::runtime_error("NR helper exited; inspect session failure.txt");
    if(WaitForSingleObject(impl_->ready.value,0)==WAIT_OBJECT_0)return true;
    if(GetTickCount64()-impl_->startup>15000)throw std::runtime_error("NR startup deadline");return false;
}
void NeuralBridge::process(ID3D11DeviceContext* context,ID3D11ShaderResourceView* color,ID3D11ShaderResourceView* depth,ID3D11ShaderResourceView* motion,ID3D11RenderTargetView* target,BridgeFrame frame){
    if(!impl_ || !ready() || !target)throw std::runtime_error("NR not ready");auto& s=*impl_;
    s.validate_view(color);auto z=s.validate_view(depth,DXGI_FORMAT_R32_FLOAT),m=s.validate_view(motion,DXGI_FORMAT_R16G16_FLOAT);
    ComPtr<ID3D11Resource> target_resource;target->GetResource(&target_resource);ComPtr<ID3D11Texture2D> target_texture;
    require(target_resource.As(&target_texture),"NR target texture");D3D11_TEXTURE2D_DESC target_desc{};target_texture->GetDesc(&target_desc);
    if(target_desc.Width!=s.width || target_desc.Height!=s.height || target_desc.SampleDesc.Count!=1)throw std::runtime_error("NR target layout mismatch");
    frame.sequence=s.sequence;validate(frame,s.sequence,s.width,s.height);
    s.blit(context,color,s.color_rt.Get(),false);context->CopyResource(s.depth.image.Get(),z.Get());context->CopyResource(s.motion.image.Get(),m.Get());
    std::memcpy(s.metadata->data,&frame,sizeof(frame));MemoryBarrier();s.input_ready.signal(context,UINT64(s.sequence)+1);
    const auto start=std::chrono::steady_clock::now();s.output_ready.wait(UINT64(s.sequence)+1,s.child.process.hProcess,100);
    s.wait_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    s.blit(context,s.output_view.Get(),target,true);++s.sequence;
}
}

````

## src/core/neural_helper.cpp

SHA-256: e0861ce07744367c8ad3fca4c72d204bfdcd02e7542bcf16ea4da9887eb818e1

````
#include "neural_transport.hpp"
#include "neural_settings.hpp"
#include <d3d12.h>
#include <nvsdk_ngx.h>
using namespace dx11_enhancement::neural;
void ngx(NVSDK_NGX_Result r,const char* stage){if(NVSDK_NGX_FAILED(r)){char text[160]{};std::snprintf(text,sizeof(text),"%s: 0x%08X",stage,unsigned(r));throw std::runtime_error(text);}}
template<class T> ComPtr<T> open_shared(ID3D12Device* device,const std::wstring& name){
    Handle h;require(device->OpenSharedHandleByName(name.c_str(),GENERIC_ALL,&h.value),"open named object");
    ComPtr<T> result;require(device->OpenSharedHandle(h.value,IID_PPV_ARGS(&result)),"open shared object");return result;
}
struct Backend {
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;Handle event;UINT64 serial=0;
    NVSDK_NGX_Parameter* parameters=nullptr;NVSDK_NGX_Handle* feature=nullptr;bool initialized=false;
    ~Backend(){if(feature)NVSDK_NGX_D3D12_ReleaseFeature(feature);if(parameters)NVSDK_NGX_D3D12_DestroyParameters(parameters);
        if(initialized)NVSDK_NGX_D3D12_Shutdown1(device.Get());}
    void submit(){
        require(commands->Close(),"close neural commands");ID3D12CommandList* lists[]={commands.Get()};queue->ExecuteCommandLists(1,lists);
        require(queue->Signal(fence.Get(),++serial),"neural queue signal");
        require(fence->SetEventOnCompletion(serial,event.value),"neural completion event");
        if(WaitForSingleObject(event.value,1000)!=WAIT_OBJECT_0)throw std::runtime_error("neural GPU completion deadline");
        require(device->GetDeviceRemovedReason(),"neural device health");
    }
    void reset(){require(allocator->Reset(),"neural allocator reset");require(commands->Reset(allocator.Get(),nullptr),"neural command reset");}
    void barrier(ID3D12Resource* resource,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};commands->ResourceBarrier(1,&b);
    }
};
int run(int argc,wchar_t** argv){
    if(argc!=10)throw std::runtime_error("neural helper arguments");
    const std::wstring name=argv[1];const LUID luid{std::stoul(argv[2]),std::stol(argv[3])};
    const UINT w=std::stoul(argv[4]),h=std::stoul(argv[5]);
    if(!w || !h || w>4096 || h>4096)throw std::runtime_error("invalid neural extent");
    const std::filesystem::path runtime=argv[6],cache=argv[7];
    const auto settings=dx11_enhancement::NeuralSettings::read(cache/L"settings.txt");
    Backend b;ComPtr<IDXGIFactory4> factory;require(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"neural factory");
    ComPtr<IDXGIAdapter1> adapter;require(factory->EnumAdapterByLuid(luid,IID_PPV_ARGS(&adapter)),"neural matching adapter");
    require(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&b.device)),"neural device");
    D3D12_COMMAND_QUEUE_DESC q{};require(b.device->CreateCommandQueue(&q,IID_PPV_ARGS(&b.queue)),"neural queue");
    require(b.device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&b.allocator)),"neural allocator");
    require(b.device->CreateCommandList(0,q.Type,b.allocator.Get(),nullptr,IID_PPV_ARGS(&b.commands)),"neural commands");
    require(b.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&b.fence)),"completion fence");
    b.event=Handle(CreateEventW(nullptr,FALSE,FALSE,nullptr));if(!b.event.value)throw std::runtime_error("neural event");
    auto color=open_shared<ID3D12Resource>(b.device.Get(),name+L".color");
    auto depth=open_shared<ID3D12Resource>(b.device.Get(),name+L".depth");
    auto motion=open_shared<ID3D12Resource>(b.device.Get(),name+L".motion");
    auto output=open_shared<ID3D12Resource>(b.device.Get(),name+L".output");
    const auto check=[&](ID3D12Resource* r,DXGI_FORMAT f){const auto d=r->GetDesc();
        if(d.Width!=w || d.Height!=h || d.Format!=f || d.MipLevels!=1 || d.DepthOrArraySize!=1 || d.SampleDesc.Count!=1)
            throw std::runtime_error("neural shared layout mismatch");};
    check(color.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT);check(output.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT);
    check(depth.Get(),DXGI_FORMAT_R32_FLOAT);check(motion.Get(),DXGI_FORMAT_R16G16_FLOAT);
    auto input_fence=open_shared<ID3D12Fence>(b.device.Get(),name+L".input-ready");
    auto output_fence=open_shared<ID3D12Fence>(b.device.Get(),name+L".output-ready");
    Metadata metadata(name,false);
    const wchar_t* paths[]={runtime.c_str()};NVSDK_NGX_FeatureCommonInfo common{};common.PathListInfo.Path=paths;common.PathListInfo.Length=1;
    ngx(NVSDK_NGX_D3D12_Init_with_ProjectID("211347f0-e644-4da9-bbe8-5d7630b1f7c8",NVSDK_NGX_ENGINE_TYPE_CUSTOM,"0.11.1",cache.c_str(),b.device.Get(),&common),"initialize NR");b.initialized=true;
    ngx(NVSDK_NGX_D3D12_GetCapabilityParameters(&b.parameters),"NR capabilities");auto* p=b.parameters;
    int available=0;ngx(p->Get("DLSSNR.Available",&available),"NR availability");if(!available)throw std::runtime_error("NR unavailable");
    p->Set("CreationNodeMask",1u);p->Set("VisibilityNodeMask",1u);
    p->Set("Width",w);p->Set("Height",h);p->Set("OutWidth",w);p->Set("OutHeight",h);
    p->Set("PerfQualityValue",int(NVSDK_NGX_PerfQuality_Value_DLAA));
    p->Set("DLSS.Feature.Create.Flags",int(NVSDK_NGX_DLSS_Feature_Flags_IsHDR|NVSDK_NGX_DLSS_Feature_Flags_MVLowRes));
    p->Set("DLSSNR.Enabled",1u);p->Set("DLSSNR.Width",w);p->Set("DLSSNR.Height",h);
    p->Set("DLSSNR.Hint.Render.Preset",static_cast<int>(settings.model));p->Set("DLSSNR.Intensity",settings.intensity);p->Set("DLSSNR.Style",settings.style);
    p->Set("DLSSNR.LocalStructureStrength",settings.structure);p->Set("DLSSNR.LocalToneStrength",settings.tone);p->Set("DLSSNR.SkinStructureStrength",settings.skin);
    p->Set("DLSSNR.UseAutoMask",settings.auto_skin);
    // No separate UI/alpha inputs: do not suggest this flag protects composited text.
    p->Set("DLSSNR.UICorrection",0u);
    ngx(NVSDK_NGX_D3D12_CreateFeature(b.commands.Get(),static_cast<NVSDK_NGX_Feature>(18),p,&b.feature),"create NR");b.submit();
    wchar_t loaded[32768]{};const auto module=GetModuleHandleW(L"nvngx_dlssnr.dll");
    if(!module || !GetModuleFileNameW(module,loaded,32768))throw std::runtime_error("NR module unavailable");
    std::ofstream(cache/L"runtime.txt")<<std::filesystem::path(loaded).u8string()<<"\nNative resolution; settings.txt records creation controls. No separate UI inputs.\n";
    auto desc=output->GetDesc();desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> local;require(b.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&local)),"private NR output");
    Handle ready(OpenEventW(EVENT_MODIFY_STATE,FALSE,(name+L".ready").c_str()));
    Handle stop(OpenEventW(SYNCHRONIZE,FALSE,(name+L".stop").c_str()));
    Handle parent(OpenProcess(SYNCHRONIZE,FALSE,std::stoul(argv[8])));
    Handle input_event(CreateEventW(nullptr,FALSE,FALSE,nullptr));
    if(!ready.value || !stop.value || !parent.value || !input_event.value)throw std::runtime_error("NR lifetime handles");
    if(std::wstring(argv[9])!=L"1")throw std::runtime_error("NR protocol version");
    SetEvent(ready.value);
    for(UINT sequence=0;;++sequence){
        require(input_fence->SetEventOnCompletion(UINT64(sequence)+1,input_event.value),"NR input event");
        const HANDLE handles[]={stop.value,parent.value,input_event.value};
        DWORD wait=WAIT_TIMEOUT;
        while(wait==WAIT_TIMEOUT)wait=WaitForMultipleObjects(3,handles,FALSE,100);
        if(wait==WAIT_OBJECT_0 || wait==WAIT_OBJECT_0+1)break;
        if(wait!=WAIT_OBJECT_0+2 || input_fence->GetCompletedValue()==UINT64_MAX)throw std::runtime_error("NR input fence failed");
        MemoryBarrier();dx11_enhancement::BridgeFrame frame{};std::memcpy(&frame,metadata.data,sizeof(frame));validate(frame,sequence,w,h);
        b.reset();for(auto* r:{color.Get(),depth.Get(),motion.Get()})b.barrier(r,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        p->Set("DLSSNR.Color",color.Get());p->Set("DLSSNR.Depth",depth.Get());p->Set("DLSSNR.MVec",motion.Get());p->Set("DLSSNR.Output",local.Get());
        p->Set("DLSSNR.Reset",frame.reset);p->Set("DLSSNR.DepthInverted",0u);
        p->Set("DLSSNR.MVecScaleX",frame.motion_scale_x);p->Set("DLSSNR.MVecScaleY",frame.motion_scale_y);
        for(const char* kind:{"Color","Depth","MVec","Output"}){const auto prefix=std::string("DLSSNR.")+kind+"Subrect";
            p->Set((prefix+"BaseX").c_str(),0u);p->Set((prefix+"BaseY").c_str(),0u);p->Set((prefix+"Width").c_str(),w);p->Set((prefix+"Height").c_str(),h);}
        ngx(NVSDK_NGX_D3D12_EvaluateFeature(b.commands.Get(),b.feature,p,nullptr),"evaluate NR");
        b.barrier(local.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        b.barrier(output.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        b.commands->CopyResource(output.Get(),local.Get());
        b.barrier(output.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
        b.barrier(local.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        for(auto* r:{color.Get(),depth.Get(),motion.Get()})b.barrier(r,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        b.submit();require(b.queue->Signal(output_fence.Get(),UINT64(sequence)+1),"NR output ready");
    }
    return 0;
}
int wmain(int argc,wchar_t** argv){try{return run(argc,argv);}catch(const std::exception& e){
    if(argc==10)std::ofstream(std::filesystem::path(argv[7])/L"failure.txt")<<e.what()<<"\n";
    return 1;}}

````

## src/core/neural_settings.hpp

SHA-256: 04fbd700b306201c6901d032b97080be01177583df1b9971ee7f03bde033a34e

````
#pragma once
#include <cmath>
#include <fstream>
#include <filesystem>
#include <stdexcept>
namespace dx11_enhancement {
struct NeuralSettings {
    unsigned model=0,style=0,auto_skin=1;
    float intensity=1,structure=1,tone=0,skin=1;
    bool operator==(const NeuralSettings& b)const{
        return model==b.model && style==b.style && auto_skin==b.auto_skin && intensity==b.intensity &&
            structure==b.structure && tone==b.tone && skin==b.skin;
    }
    void validate()const{
        const auto range=[](float f,float lo,float hi){return std::isfinite(f) && f>=lo && f<=hi;};
        if(model>3 || style>2 || auto_skin>1 || !range(intensity,0,2) || !range(structure,0,2) ||
            !range(tone,0,2) || !range(skin,-1,2))throw std::runtime_error("invalid neural settings");
    }
    void save(const std::filesystem::path& path)const{
        validate();std::ofstream out(path);out.precision(9);
        out<<1<<' '<<model<<' '<<style<<' '<<auto_skin<<' '<<intensity<<' '<<structure<<' '<<tone<<' '<<skin<<'\n';
        out.flush();if(!out)throw std::runtime_error("write neural settings");
    }
    static NeuralSettings read(const std::filesystem::path& path){
        NeuralSettings s;unsigned version=0;std::ifstream in(path);
        if(!(in>>version>>s.model>>s.style>>s.auto_skin>>s.intensity>>s.structure>>s.tone>>s.skin) || version!=1)
            throw std::runtime_error("read neural settings/version");
        in>>std::ws;if(!in.eof())throw std::runtime_error("extra neural settings data");
        s.validate();return s;
    }
};
}

````

## src/core/bridge_protocol.hpp

SHA-256: 72290c4f4da3e4ad5b9b9ba8ce4f6279dd40db1a447a946bd961a6e4616ab02b

````
#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace dx11_enhancement {
// Pointer-free wire layout shared by x86 and x64. Input keyed-mutex ownership
// protects this single slot too; the producer cannot overwrite an in-flight frame.
struct BridgeFrame {
    uint32_t version = 1;
    uint32_t sequence = 0;
    uint64_t source_frame = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    float jitter_x = 0;
    float jitter_y = 0;
    float motion_scale_x = 1;
    float motion_scale_y = 1;
    float exposure = 1;
    float pre_exposure = 1;
    float frame_ms = 16.6667f;
    uint32_t reset = 0;
};
static_assert(std::is_standard_layout_v<BridgeFrame> && std::is_trivially_copyable_v<BridgeFrame>);
static_assert(sizeof(BridgeFrame) == 56 && offsetof(BridgeFrame, source_frame) == 8);
static_assert(offsetof(BridgeFrame, jitter_x) == 24 && offsetof(BridgeFrame, reset) == 52);
}

````

## src/core/bridge_transport.hpp

SHA-256: cd1ca162a95318b7e53838b11de4a37a6c38a02419b8925ca5107c4e4d0b3e22

````
#pragma once
// Standalone full-input DLAA round trip. Receipts/readback are test instrumentation.
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <stdexcept>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <array>
#include <cmath>
#include <cstring>
#include "bridge_protocol.hpp"

namespace dx11_enhancement::transport {
using Microsoft::WRL::ComPtr;
inline void require(HRESULT hr,const char* operation) {
    if(hr!=S_OK) {
        char message[192]{};std::snprintf(message,sizeof(message),"%s: 0x%08X",operation,static_cast<unsigned>(hr));
        throw std::runtime_error(message);
    }
}
struct Device {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;LUID luid{};
    Device(ID3D11Device* host,ID3D11DeviceContext* immediate):device(host),context(immediate) {}
    explicit Device(const LUID* wanted=nullptr) {
        ComPtr<IDXGIFactory1> factory;require(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");
        ComPtr<IDXGIAdapter1> chosen;
        for(UINT i=0;;++i) {
            ComPtr<IDXGIAdapter1> candidate;
            const HRESULT enumerated=factory->EnumAdapters1(i,&candidate);
            if(enumerated==DXGI_ERROR_NOT_FOUND)break;
            require(enumerated,"enumerate adapter");
            DXGI_ADAPTER_DESC1 desc{};require(candidate->GetDesc1(&desc),"adapter description");
            const bool match=wanted ? desc.AdapterLuid.LowPart==wanted->LowPart && desc.AdapterLuid.HighPart==wanted->HighPart
                : desc.VendorId==0x10de && !(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE);
            if(match){chosen=candidate;luid=desc.AdapterLuid;std::wprintf(L"%zu-bit adapter: %ls\n",sizeof(void*)*8,desc.Description);break;}
        }
        if(!chosen)throw std::runtime_error("matching NVIDIA hardware adapter unavailable");
        const D3D_FEATURE_LEVEL requested=D3D_FEATURE_LEVEL_11_0;D3D_FEATURE_LEVEL obtained{};
        require(D3D11CreateDevice(chosen.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,&requested,1,
            D3D11_SDK_VERSION,&device,&obtained,&context),"hardware device");
    }
};
struct SharedTexture {
    ComPtr<ID3D11Texture2D> texture;ComPtr<IDXGIKeyedMutex> mutex;HANDLE token=nullptr;
    void create(Device& d,UINT width,UINT height,UINT binding,DXGI_FORMAT format=DXGI_FORMAT_R16G16B16A16_FLOAT) {
        D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;desc.MipLevels=desc.ArraySize=1;
        desc.Format=format;desc.SampleDesc.Count=1;
        desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=binding;
        desc.MiscFlags=D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        require(d.device->CreateTexture2D(&desc,nullptr,&texture),"create shared texture");
        require(texture.As(&mutex),"shared mutex");ComPtr<IDXGIResource> resource;require(texture.As(&resource),"DXGI resource");
        // Legacy DXGI share token: OpenSharedResource, not DuplicateHandle/CloseHandle.
        require(resource->GetSharedHandle(&token),"share token");
    }
    void open(Device& d,const wchar_t* value,UINT width,UINT height,DXGI_FORMAT format=DXGI_FORMAT_R16G16B16A16_FLOAT) {
        token=reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::stoull(value)));
        require(d.device->OpenSharedResource(token,IID_PPV_ARGS(&texture)),"open shared texture");
        require(texture.As(&mutex),"open mutex");
        D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
        if(desc.Width!=width || desc.Height!=height || desc.Format!=format)
            throw std::runtime_error("shared texture description mismatch");
    }
    void acquire(UINT64 key) {require(mutex->AcquireSync(key,5000),"acquire key (timeout/abandoned is failure)");}
    void release(UINT64 key) {require(mutex->ReleaseSync(key),"release key");}
};
struct Child {
    PROCESS_INFORMATION process{};
    ~Child() {
        if(process.hProcess) {
            if(WaitForSingleObject(process.hProcess,0)==WAIT_TIMEOUT) {
                TerminateProcess(process.hProcess,99);WaitForSingleObject(process.hProcess,5000);
            }
            CloseHandle(process.hProcess);
        }
        if(process.hThread)CloseHandle(process.hThread);
    }
};
using dx11_enhancement::BridgeFrame;
struct Metadata {
    HANDLE handle=nullptr;BridgeFrame* data=nullptr;
    Metadata(const std::wstring& name,bool create) {
        handle=create?CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(BridgeFrame),name.c_str())
                     :OpenFileMappingW(FILE_MAP_READ,FALSE,name.c_str());
        const DWORD error=GetLastError();
        if(!handle)throw std::runtime_error("metadata mapping failed");
        if(create && error==ERROR_ALREADY_EXISTS){CloseHandle(handle);handle=nullptr;throw std::runtime_error("metadata name collision");}
        data=static_cast<BridgeFrame*>(MapViewOfFile(handle,create?FILE_MAP_ALL_ACCESS:FILE_MAP_READ,0,0,sizeof(BridgeFrame)));
        if(!data){CloseHandle(handle);handle=nullptr;throw std::runtime_error("metadata view failed");}
    }
    ~Metadata(){if(data)UnmapViewOfFile(data);if(handle)CloseHandle(handle);}
    Metadata(const Metadata&)=delete;Metadata& operator=(const Metadata&)=delete;
};
inline void validate(const BridgeFrame& f,UINT sequence,UINT width,UINT height) {
    if(f.version!=1 || f.sequence!=sequence || f.width!=width || f.height!=height || f.reset>1 ||
       !std::isfinite(f.jitter_x) || !std::isfinite(f.jitter_y) || std::abs(f.jitter_x)>.5f || std::abs(f.jitter_y)>.5f ||
       !std::isfinite(f.motion_scale_x) || !std::isfinite(f.motion_scale_y) || f.motion_scale_x==0 || f.motion_scale_y==0 ||
       !std::isfinite(f.exposure) || f.exposure<=0 || !std::isfinite(f.pre_exposure) || f.pre_exposure<=0 ||
       !std::isfinite(f.frame_ms) || f.frame_ms<=0 || f.frame_ms>250)
        throw std::runtime_error("invalid shared frame metadata");
}

}

````

## src/core/depth_addon.cpp

SHA-256: c8284d5fc19641410cc0a79d22d043b9df4492ddb485ca141a7ae22a6d931620

````
#include <imgui.h>
#include <reshade.hpp>
#include "depth_addon.hpp"
#include "depth_preview.hpp"
#include "raw_snapshot.hpp"
#include "frame_history.hpp"
#include "capture_trigger.hpp"
#include "live_bridge.hpp"
#include "dlaa_preset.hpp"
#include "projection_verifier.hpp"
#include <cstring>
#include "adapters/alien_isolation.hpp"
#include "adapters/alien_neural.hpp"
#include <mutex>
#include <cstdio>
#include <fstream>
#include <string>
#include <stdexcept>
#include <d3d11_1.h>
#include <cmath>
#include <iomanip>
#include <chrono>

namespace {
using namespace reshade;
using Microsoft::WRL::ComPtr;
constexpr GUID camera_tag{0xd3d50bf1,0xf2be,0x4a90,{0x9b,0x73,0x49,0x22,0xab,0x06,0x8e,0x42}};
constexpr GUID taa_output_tag{0xf8a91843,0xd8de,0x483e,{0x83,0x61,0x21,0x02,0x45,0x66,0x19,0x73}};
struct __declspec(uuid("8307be10-48d9-4669-a534-18582de42a72")) DepthState {
    std::recursive_mutex mutex;
    dx11_enhancement::LiveBridge live;
    alien_isolation::NeuralTrial neural;
    uint64_t live_displayed_frame=UINT64_MAX;
    diagnostics::TexturePreview live_depth,live_motion;
    diagnostics::ProjectionSnapshot live_projection;
    diagnostics::ProjectionVerifier live_verifier;
    ComPtr<ID3D11Buffer> live_cpu_source;
    UINT live_cpu_offset=0;
    const unsigned char* live_mapped=nullptr;
    std::array<float,16> live_cpu_upload{},live_cpu_frame{};
    uint64_t live_cpu_version=0,live_cpu_used=0,live_cpu_fast=0;
    bool live_cpu_fresh=false;
    ComPtr<ID3D11RenderTargetView> live_motion_source;
    ComPtr<ID3D11Resource> live_target;
    bool live_enabled=false,live_camera=false,live_submitted=false;
    dx11_enhancement::DlaaControlRequest live_request;
    uint64_t live_last_frame=UINT64_MAX,live_last_time=0,live_frames=0;
    double live_projection_ms=0,live_motion_ms=0,live_perf_projection=0,live_perf_motion=0;
    dx11_enhancement::BridgeTimings live_perf{};
    unsigned live_perf_count=0;
    uint64_t live_perf_start=0;
    std::string live_status="Off. Existing anti-aliasing remains active.";

    diagnostics::FrameHistory history;
    diagnostics::HistoryDecision history_decision;
    diagnostics::ResetReason last_reset=diagnostics::ResetReason::first_frame;
    diagnostics::DepthPreview preview;
    bool enabled = false, fresh = false, rendered = false, invert = false;
    bool shader_found = false, first_capture_logged = false, first_image_logged = false;
    bool inspect_requested = true, inspected = false, fit_requested = true, gradient = false;
    diagnostics::DepthStatistics statistics;
    float exponent = 1.0f, minimum = 0, maximum = 1;
    unsigned draws_this_frame = 0, draws_last_frame = 0;
    int occurrence = 0;
    HRESULT status = S_FALSE;
    unsigned long long passes = 0;
    diagnostics::TexturePreview motion;
    diagnostics::MotionStatistics motion_statistics;
    ComPtr<ID3D11RenderTargetView> pending_motion;
    bool motion_enabled = false, motion_fresh = false, motion_rendered = false;
    bool motion_inspect = true, motion_inspected = false, motion_logged = false;
    bool magnitude = false, uv_to_pixels = false;
    bool emphasize_small_motion = true;
    float sample_u = 0.5f, sample_v = 0.5f;
    float motion_gain = 100.0f;
    HRESULT motion_status = S_FALSE;
    diagnostics::RawSnapshot pair_color[2], pair_motion[2];
    diagnostics::RawSnapshot pair_pre_color[2];
    diagnostics::RawSnapshot pair_depth[2];
    diagnostics::ProjectionSnapshot pair_projection[2];
    diagnostics::ProjectionSnapshot pair_dof[2], pair_rgbm[2];
    bool dof_valid[2]{}, rgbm_valid[2]{}, dof_same_depth[2]{}, rgbm_same_color[2]{};
    UINT dof_first[2]{}, rgbm_first[2]{}, dof_seen[2]{}, rgbm_seen[2]{};
    ComPtr<ID3D11Resource> pair_depth_source;
    UINT pair_first_constant[2]{};
    bool pair_pre_fresh=false, pair_projection_fresh=false;
    ComPtr<ID3D11Resource> pair_pre_target;
    bool pair_armed = false, pair_fresh = false;
    diagnostics::CaptureTrigger pair_trigger;
    HRESULT pair_projection_status=S_FALSE, pair_depth_status=S_FALSE;
    unsigned pair_count = 0;
    uint64_t frame = 0, pair_frame[2]{}, pair_due = 0;
    uint64_t pair_time[2]{};
    UINT pair_boundary[2]{};
    std::string pair_status = "No pair captured yet.";
    unsigned trace_remaining = 0, trace_consumers = 0, trace_executes = 0;
    uint64_t trace_armed_frame=0;
    bool trace_matrix_fresh = false;
    ComPtr<ID3D11Buffer> trace_matrix;
    ComPtr<ID3D11Resource> trace_output;
};
HRESULT snapshot_camera_projection(ID3D11DeviceContext* context, diagnostics::ProjectionSnapshot& snapshot, UINT& first) {
    ComPtr<ID3D11Buffer> source;
    ComPtr<ID3D11DeviceContext1> extended;
    first=0;
    UINT constants=0;
    if (SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&extended)))) {
        extended->VSGetConstantBuffers1(1,1,&source,&first,&constants);
        if (constants<4) return E_INVALIDARG;
    } else context->VSGetConstantBuffers(1,1,&source);
    if (first>UINT_MAX/16) return E_INVALIDARG;
    return snapshot.capture(context,source.Get(),first*16);
}
HRESULT snapshot_pixel_constants(ID3D11DeviceContext* context, UINT slot, UINT start, UINT count,
    diagnostics::ProjectionSnapshot& snapshot, UINT& first) {
    ComPtr<ID3D11Buffer> source;ComPtr<ID3D11DeviceContext1> extended;
    first=0;UINT constants=0;
    if (SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&extended)))) {
        extended->PSGetConstantBuffers1(slot,1,&source,&first,&constants);
        if (constants<start || constants-start<count) return E_INVALIDARG;
    } else context->PSGetConstantBuffers(slot,1,&source);
    if (first>UINT_MAX/16-start) return E_INVALIDARG;
    return snapshot.capture(context,source.Get(),(first+start)*16,count*4);
}
struct __declspec(uuid("184f8c81-d84a-4ec0-acb0-f81941b11af8")) CommandState {
    UINT role = 0;
    bool taa_bound = false, taa_dispatched = false;
    ComPtr<ID3D11Resource> taa_output;
};


// Read-only observation of the tracked camera buffer's writable mapping.
// Qualification below detects whether other hooks applied jitter before this event.
void camera_map(api::device* device,api::resource resource,uint64_t offset,uint64_t size,api::map_access access,void** data){
    auto* state=device->get_private_data<DepthState>();if(!state)return;
    std::lock_guard lock(state->mutex);
    if(!state->live_enabled || resource.handle!=reinterpret_cast<uintptr_t>(state->live_cpu_source.Get()))return;
    state->live_mapped=nullptr;
    if(access==api::map_access::read_only || !data || !*data || offset>state->live_cpu_offset)return;
    D3D11_BUFFER_DESC desc{};state->live_cpu_source->GetDesc(&desc);
    const uint64_t start=state->live_cpu_offset,relative=start-offset;
    if(start+64>desc.ByteWidth || (size!=UINT64_MAX && (relative>size || size-relative<64)))return;
    state->live_mapped=static_cast<const unsigned char*>(*data)+relative;
}
void camera_unmap(api::device* device,api::resource resource){
    auto* state=device->get_private_data<DepthState>();if(!state)return;
    std::lock_guard lock(state->mutex);
    if(resource.handle!=reinterpret_cast<uintptr_t>(state->live_cpu_source.Get()) || !state->live_mapped)return;
    std::memcpy(state->live_cpu_upload.data(),state->live_mapped,64);state->live_mapped=nullptr;++state->live_cpu_version;
}
void live_failure(DepthState& state,const char* message) {
    state.live_enabled=false;state.live.stop();state.live_target.Reset();
    state.live_camera=state.live_submitted=false;
    state.live_status=std::string("DLAA off; existing AA retained: ")+message;
    log::message(log::level::warning,state.live_status.c_str());
}
void live_submit(DepthState& state,ID3D11DeviceContext* context,ID3D11Resource* target) {
    if(!state.live_enabled || !state.live_camera || !state.live_motion_source)return;
    try {
        if(state.live_submitted)throw std::runtime_error("multiple temporal passes in one frame");
        ComPtr<ID3D11Texture2D> texture;
        if(FAILED(target->QueryInterface(IID_PPV_ARGS(&texture))))throw std::runtime_error("color texture unavailable");
        D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
        if(desc.Format!=DXGI_FORMAT_R11G11B10_FLOAT || desc.SampleDesc.Count!=1 || desc.ArraySize!=1 || desc.MipLevels!=1 ||
            desc.Width!=state.live_depth.width() || desc.Height!=state.live_depth.height())throw std::runtime_error("unsupported color/depth layout");
        if(!state.live.started()) {
            wchar_t exe[32768]{};if(!GetModuleFileNameW(nullptr,exe,32768))throw std::runtime_error("game path unavailable");
            const auto root=std::filesystem::path(exe).parent_path();
            std::ifstream config(root/L"dx11-dlaa-paths.txt");std::string helper,runtime,cache;
            if(!std::getline(config,helper) || !std::getline(config,runtime) || !std::getline(config,cache))
                throw std::runtime_error("DLAA helper configuration missing");
            state.live.start(context,desc.Width,desc.Height,std::filesystem::u8path(helper),std::filesystem::u8path(runtime),
                std::filesystem::u8path(cache)/std::to_string(GetTickCount64()),state.live_request.preset);
            state.live_last_frame=UINT64_MAX;state.live_last_time=0;
            state.live_status="Starting DLAA helper; existing AA active.";
        }
        if(!state.live.ready())return;
        const auto projection_begin=std::chrono::steady_clock::now();
        std::array<float,16> matrix{};
        const bool trusted=state.live_verifier.trusted();
        if(state.live_cpu_fresh && trusted && state.live_frames%120!=0){
            matrix=state.live_cpu_frame;++state.live_cpu_fast;
        }else{
            if(FAILED(state.live_projection.inspect(context,matrix)))throw std::runtime_error("camera projection read failed");
            if(state.live_cpu_fresh){
                const bool matched=state.live_verifier.observe(state.live_cpu_frame,matrix);
                if(trusted && !matched)log::message(log::level::warning,"[DX11DLAACamera] CPU/GPU verification mismatch; using GPU readback.");
                if(!trusted && state.live_verifier.trusted())log::message(log::level::info,"[DX11DLAACamera] Eight CPU uploads match GPU projection including changing jitter; cached upload path qualified.");
            }else state.live_verifier.reset();
        }
        state.live_projection_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-projection_begin).count();
        const auto motion_begin=std::chrono::steady_clock::now();
        if(FAILED(state.live_motion.capture_motion(context,state.live_motion_source.Get())) ||
            state.live_motion.width()!=desc.Width || state.live_motion.height()!=desc.Height ||
            state.live_motion.format()!=DXGI_FORMAT_R16G16_FLOAT)throw std::runtime_error("motion capture/layout mismatch");
        state.live_motion_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-motion_begin).count();
        ComPtr<ID3D11Device> device;context->GetDevice(&device);ComPtr<ID3D11ShaderResourceView> color;
        if(FAILED(device->CreateShaderResourceView(texture.Get(),nullptr,&color)))throw std::runtime_error("color view failed");
        dx11_enhancement::BridgeFrame frame;
        frame.source_frame=state.frame;frame.width=desc.Width;frame.height=desc.Height;
        frame.jitter_x=matrix[2]*float(desc.Width)*.5f;frame.jitter_y=-matrix[6]*float(desc.Height)*.5f;
        frame.motion_scale_x=-float(desc.Width);frame.motion_scale_y=-float(desc.Height);
        const auto now=GetTickCount64();const auto elapsed=state.live_last_time?now-state.live_last_time:0;
        frame.reset=state.live_last_frame==UINT64_MAX || state.frame!=state.live_last_frame+1 || elapsed>250;
        frame.frame_ms=elapsed && elapsed<=250?float(elapsed):16.6667f;
        state.live.submit(context,color.Get(),state.live_depth.raw_view(),state.live_motion.raw_view(),frame);
        state.live_target=target;state.live_submitted=true;state.live_last_frame=state.frame;state.live_last_time=now;
    }catch(const std::exception& e){live_failure(state,e.what());}
}
void live_consume(DepthState& state,ID3D11DeviceContext* context) {
    if(!state.live_enabled || !state.live.pending() || !state.live_target)return;
    ComPtr<ID3D11ShaderResourceView> view;ComPtr<ID3D11Resource> resource;
    context->PSGetShaderResources(0,1,&view);if(view)view->GetResource(&resource);
    if(resource!=state.live_target)return;
    try {
        ComPtr<ID3D11Device> device;context->GetDevice(&device);ComPtr<ID3D11RenderTargetView> target;
        if(FAILED(device->CreateRenderTargetView(resource.Get(),nullptr,&target)))throw std::runtime_error("return color view failed");
        state.live.resolve(context,target.Get());++state.live_frames;state.live_displayed_frame=state.frame;
        const auto timing=state.live.timings();
        state.live_perf_projection+=state.live_projection_ms;state.live_perf_motion+=state.live_motion_ms;
        state.live_perf.input_wait_ms+=timing.input_wait_ms;state.live_perf.submit_ms+=timing.submit_ms;
        state.live_perf.output_wait_ms+=timing.output_wait_ms;state.live_perf.return_ms+=timing.return_ms;
        if(!state.live_perf_count)state.live_perf_start=GetTickCount64();
        if(++state.live_perf_count==120){
            char line[512]{};
            std::snprintf(line,sizeof(line),"[DX11DLAAPerf] 120 frames CPU elapsed averages ms: projection=%.3f motionCopy=%.3f inputWait=%.3f submit=%.3f outputWait=%.3f return=%.3f; 119-frame wall span=%llums; not GPU timings; cached camera frames=%llu",
                state.live_perf_projection/120,state.live_perf_motion/120,state.live_perf.input_wait_ms/120,
                state.live_perf.submit_ms/120,state.live_perf.output_wait_ms/120,state.live_perf.return_ms/120,
                GetTickCount64()-state.live_perf_start,state.live_cpu_fast);
            log::message(log::level::info,line);state.live_perf_count=0;state.live_perf={};state.live_perf_projection=state.live_perf_motion=0;
        }
        state.live_status=std::string("Active. Preset ")+dx11_enhancement::dlaa_preset_name(state.live_request.preset)+" requested.";
        if(state.live_frames==1)log::message(log::level::info,"[DX11DLAA] First live result supplied at verified color consumer.");
    }catch(const std::exception& e){live_failure(state,e.what());}
}
void init_device(api::device* device) {
    if (device->get_api() == api::device_api::d3d11 && alien_isolation::is_host())
        device->create_private_data<DepthState>();
}
void destroy_device(api::device* device) {
    if (device->get_private_data<DepthState>()) device->destroy_private_data<DepthState>();
}
void init_commands(api::command_list* commands) {
    if (commands->get_device()->get_private_data<DepthState>()) commands->create_private_data<CommandState>();
}
void destroy_commands(api::command_list* commands) {
    if (commands->get_private_data<CommandState>()) commands->destroy_private_data<CommandState>();
}
void init_pipeline(api::device* device, api::pipeline_layout, uint32_t count,
    const api::pipeline_subobject* objects, api::pipeline pipeline)
{
    auto* state = device->get_private_data<DepthState>();
    if (!state) return;
    for (uint32_t i = 0; i < count; ++i) {
        if (objects[i].type == api::pipeline_subobject_type::compute_shader && objects[i].count == 1) {
            const auto& shader = *static_cast<const api::shader_desc*>(objects[i].data);
            if (alien_isolation::is_alias_taa_shader(shader.code,shader.code_size)) {
                const UINT tag=5;
                reinterpret_cast<ID3D11ComputeShader*>(pipeline.handle)->SetPrivateData(camera_tag,sizeof(tag),&tag);
                log::message(log::level::info,"[DX11Trace] Pinned Alias TAA compute shader identified.");
            }
        }
        if (objects[i].type == api::pipeline_subobject_type::vertex_shader && objects[i].count == 1) {
            const auto& shader = *static_cast<const api::shader_desc*>(objects[i].data);
            if (alien_isolation::is_rgbm_vertex_shader(shader.code,shader.code_size)) {
                const UINT tag=4;
                reinterpret_cast<ID3D11VertexShader*>(pipeline.handle)->SetPrivateData(camera_tag,sizeof(tag),&tag);
            }
        }
        if (objects[i].type != api::pipeline_subobject_type::pixel_shader || objects[i].count != 1) continue;
        const auto& shader = *static_cast<const api::shader_desc*>(objects[i].data);
        const bool camera = alien_isolation::is_camera_motion_shader(shader.code, shader.code_size);
        const bool consumer = alien_isolation::is_temporal_consumer_shader(shader.code, shader.code_size);
        if (!camera && !consumer) continue;
        auto* native = reinterpret_cast<ID3D11PixelShader*>(pipeline.handle);
        const UINT tag = camera ? 1 : (alien_isolation::is_rgbm_pixel_shader(shader.code,shader.code_size) ? 3 : 2);
        if (FAILED(native->SetPrivateData(camera_tag, sizeof(tag), &tag))) return;
        std::lock_guard lock(state->mutex);
        if (camera) state->shader_found = true;
        log::message(log::level::info, camera ? "[DX11Depth] Known camera-motion shader identified by DXBC signature."
            : "[DX11Motion] Known DoF/RGBM consumer pixel shader identified.");
    }
}
UINT shader_role(ID3D11DeviceChild* shader) {
    UINT size = sizeof(UINT), tag = 0;
    return shader && SUCCEEDED(shader->GetPrivateData(camera_tag, &size, &tag)) ? tag : 0;
}
void bind_pipeline(api::command_list* commands, api::pipeline_stage stages, api::pipeline pipeline) {
    auto* state = commands->get_private_data<CommandState>();
    if (state && (stages & api::pipeline_stage::pixel_shader) != static_cast<api::pipeline_stage>(0))
        state->role = shader_role(reinterpret_cast<ID3D11PixelShader*>(pipeline.handle));
    if (state && (stages & api::pipeline_stage::compute_shader) != static_cast<api::pipeline_stage>(0))
        state->taa_bound = shader_role(reinterpret_cast<ID3D11ComputeShader*>(pipeline.handle))==5;
}

bool trace_dispatch(api::command_list* commands,uint32_t,uint32_t,uint32_t) {
    auto* state=commands->get_private_data<CommandState>();
    if (state && state->taa_bound) state->taa_dispatched=true;
    return false;
}
bool trace_copy(api::command_list* commands,api::resource,api::resource destination) {
    auto* state=commands->get_private_data<CommandState>();
    if (state && state->taa_dispatched) {
        state->taa_output=reinterpret_cast<ID3D11Resource*>(destination.handle);
        state->taa_dispatched=false;
    }
    return false;
}
void trace_secondary(api::command_list* primary,api::command_list* secondary) {
    auto* device_state=primary->get_device()->get_private_data<DepthState>();
    if (!device_state) return;
    auto* native=reinterpret_cast<IUnknown*>(primary->get_native());
    ComPtr<ID3D11CommandList> recorded;
    if (SUCCEEDED(native->QueryInterface(IID_PPV_ARGS(&recorded)))) {
        // ReShade's after-FinishCommandList event: transfer deferred recording metadata.
        auto* source=secondary->get_private_data<CommandState>();
        if (source && source->taa_output) {
            recorded->SetPrivateDataInterface(taa_output_tag,source->taa_output.Get());
            source->taa_output.Reset();source->taa_dispatched=false;
        }
        return;
    }
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(native->QueryInterface(IID_PPV_ARGS(&context))) || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) return;
    ComPtr<ID3D11CommandList> list;
    if (FAILED(reinterpret_cast<IUnknown*>(secondary->get_native())->QueryInterface(IID_PPV_ARGS(&list)))) return;
    ComPtr<ID3D11Resource> output;
    UINT size=sizeof(ID3D11Resource*);
    if (FAILED(list->GetPrivateData(taa_output_tag,&size,output.GetAddressOf()))) return;
    std::lock_guard lock(device_state->mutex);
    live_submit(*device_state,context.Get(),output.Get());
    if (device_state->trace_remaining) {
        device_state->trace_output=output;
        ++device_state->trace_executes;
    }
    if (device_state->pair_armed && device_state->pair_trigger.ready() &&
        device_state->pair_projection_fresh && device_state->pending_motion) {
        auto& state=*device_state;
        const unsigned index=state.pair_count;
        // This callback runs before the original ExecuteCommandList submits TAA.
        HRESULT hr=state.pair_pre_fresh ? E_UNEXPECTED :
            state.pair_pre_color[index].capture_resource(context.Get(),output.Get(),DXGI_FORMAT_R11G11B10_FLOAT);
        if (SUCCEEDED(hr)) {
            ComPtr<ID3D11Resource> motion;
            D3D11_RENDER_TARGET_VIEW_DESC view{};
            state.pending_motion->GetResource(&motion);state.pending_motion->GetDesc(&view);
            hr=state.pair_motion[index].capture_resource(context.Get(),motion.Get(),view.Format);
        }
        if (SUCCEEDED(hr) && (state.pair_pre_color[index].width()!=state.pair_motion[index].width() ||
            state.pair_pre_color[index].height()!=state.pair_motion[index].height())) hr=E_INVALIDARG;
        if (SUCCEEDED(hr)) {
            state.pair_pre_target=output;state.pair_pre_fresh=true;
        } else {
            state.pair_armed=false;
            state.pair_status="Pre-TAA capture rejected: unsupported/mismatched textures or multiple TAA executions.";
            log::message(log::level::warning,state.pair_status.c_str());
        }
    }
}
void capture(api::command_list* commands) {
    auto* command = commands->get_private_data<CommandState>();
    if (!command || !command->role) return;
    auto* state = commands->get_device()->get_private_data<DepthState>();
    if (!state) return;
    auto* context = reinterpret_cast<ID3D11DeviceContext*>(commands->get_native());
    if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return;
    // Validate the actual binding as other mods may restore state without a bind event.
    ComPtr<ID3D11PixelShader> shader;
    context->PSGetShader(&shader, nullptr, nullptr);
    const UINT role = shader_role(shader.Get());
    if (!role) return;
    std::lock_guard lock(state->mutex);
    if (role == 3) {
        ComPtr<ID3D11VertexShader> vertex;
        context->VSGetShader(&vertex,nullptr,nullptr);
        if (shader_role(vertex.Get())!=4) return;
    }
    if (role == 2 || role == 3) {
        live_consume(*state,context);
        if (state->pair_armed && state->pair_trigger.ready() && state->pair_pre_fresh) {
            const unsigned i=state->pair_count;
            if (role==2 && state->dof_seen[i]++==0) {
                state->dof_valid[i]=SUCCEEDED(snapshot_pixel_constants(context,0,11,2,state->pair_dof[i],state->dof_first[i]));
                ComPtr<ID3D11ShaderResourceView> view;ComPtr<ID3D11Resource> resource;
                context->PSGetShaderResources(2,1,&view);if(view)view->GetResource(&resource);
                state->dof_same_depth[i]=resource && resource==state->pair_depth_source;
            }
            if (role==3 && state->rgbm_seen[i]++==0) {
                state->rgbm_valid[i]=SUCCEEDED(snapshot_pixel_constants(context,2,40,1,state->pair_rgbm[i],state->rgbm_first[i]));
                ComPtr<ID3D11ShaderResourceView> view;ComPtr<ID3D11Resource> resource;
                context->PSGetShaderResources(0,1,&view);if(view)view->GetResource(&resource);
                state->rgbm_same_color[i]=resource && resource==state->pair_pre_target;
            }
        }
        if (state->trace_remaining && state->trace_consumers++<8) {
            ComPtr<ID3D11ShaderResourceView> view;
            ComPtr<ID3D11Resource> color;
            context->PSGetShaderResources(0,1,&view);
            if (view) view->GetResource(&color);
            char message[256]{};
            std::snprintf(message,sizeof(message),"[DX11Trace] frame=%llu consumer=%u priorTaaLists=%u sameColor=%u color=%p taaOutput=%p",
                state->frame,role,state->trace_executes,color && color==state->trace_output ? 1u:0u,
                static_cast<void*>(color.Get()),static_cast<void*>(state->trace_output.Get()));
            log::message(log::level::info,message);
        }
        if (state->motion_enabled && state->pending_motion && !state->motion_fresh) {
            state->motion_status = state->motion.capture_motion(context, state->pending_motion.Get());
            state->motion_fresh = SUCCEEDED(state->motion_status);
            if (state->pair_armed && state->pair_trigger.ready() && state->motion_fresh) {
                ComPtr<ID3D11ShaderResourceView> color;
                context->PSGetShaderResources(0, 1, &color);
                const unsigned index = state->pair_count;
                state->pair_time[index] = GetTickCount64();
                state->pair_boundary[index] = role;
                ComPtr<ID3D11Resource> target;
                if (color) color->GetResource(&target);
                const char* reason="post-color copy failed";
                HRESULT hr=E_UNEXPECTED;
                if (!state->pair_projection_fresh) reason="camera projection/depth missing";
                else if (!state->pair_pre_fresh) reason="pre-TAA snapshot missing";
                else if (!target || target!=state->pair_pre_target) reason="pre/post color resource differs";
                else hr=state->pair_color[index].capture(context,color.Get());
                if (SUCCEEDED(hr) && !state->pair_pre_color[index].matches(state->pair_color[index])) {
                    hr=E_INVALIDARG;reason="pre/post color layout differs";
                }
                if (SUCCEEDED(hr) && (state->pair_color[index].width()!=state->motion.width() ||
                    state->pair_color[index].height()!=state->motion.height())) {
                    hr=E_INVALIDARG;reason="color/motion dimensions differ";
                }
                state->pair_fresh = SUCCEEDED(hr);
                if (FAILED(hr)) {
                    state->pair_armed = false;
                    char message[384]{};
                    std::snprintf(message,sizeof(message),"[DX11Pair] Rejected: %s; frame=%llu role=%u index=%u hr=0x%08X projection=0x%08X depth=0x%08X early=%u cameraDraws=%u",
                        reason,state->frame,role,index,static_cast<unsigned>(hr),
                        static_cast<unsigned>(state->pair_projection_status),static_cast<unsigned>(state->pair_depth_status),
                        state->pair_pre_fresh?1u:0u,state->draws_this_frame);
                    state->pair_status = message;
                    log::message(log::level::warning,message);
                }
            }
            if (!state->motion_logged) {
                char message[256]{};
                D3D11_RENDER_TARGET_VIEW_DESC rt{};
                state->pending_motion->GetDesc(&rt);
                std::snprintf(message, sizeof(message), "[DX11Motion] Consumer snapshot: status=0x%08X format=%u size=%ux%u. Encoding and object coverage unvalidated.",
                    static_cast<unsigned>(state->motion_status), static_cast<unsigned>(rt.Format),
                    state->motion.width(), state->motion.height());
                log::message(log::level::info, message);
                state->motion_logged = true;
            }
        }
        return;
    }
    ++state->passes;
    ++state->draws_this_frame;
    if(state->live_enabled) {
        state->live_camera=false;state->live_cpu_fresh=false;
        if(state->draws_this_frame!=1)live_failure(*state,"multiple camera-motion passes");
        else {
            ComPtr<ID3D11ShaderResourceView> depth;context->PSGetShaderResources(8,1,&depth);
            UINT first=0;
            state->live_camera=SUCCEEDED(state->live_depth.capture(context,depth.Get())) &&
                state->live_depth.format()==DXGI_FORMAT_R32_FLOAT &&
                SUCCEEDED(snapshot_camera_projection(context,state->live_projection,first));
            ComPtr<ID3D11Buffer> buffer;context->VSGetConstantBuffers(1,1,&buffer);
            if(buffer!=state->live_cpu_source || first*16!=state->live_cpu_offset){
                state->live_cpu_source=buffer;state->live_cpu_offset=first*16;state->live_mapped=nullptr;
                state->live_cpu_version=state->live_cpu_used=0;state->live_verifier.reset();
            }
            if(state->live_cpu_version!=state->live_cpu_used){
                state->live_cpu_frame=state->live_cpu_upload;state->live_cpu_fresh=true;
                state->live_cpu_used=state->live_cpu_version;
            }
            context->OMGetRenderTargets(1,state->live_motion_source.ReleaseAndGetAddressOf(),nullptr);
            if(!state->live_camera)live_failure(*state,"unsupported camera depth/projection");
        }
    }
    if (state->occurrence > 0 &&
        state->draws_this_frame != static_cast<unsigned>(state->occurrence)) return;
    if (state->pair_armed && state->pair_trigger.ready()) {
        const unsigned i=state->pair_count;
        state->dof_valid[i]=state->rgbm_valid[i]=false;
        state->dof_same_depth[i]=state->rgbm_same_color[i]=false;
        state->dof_seen[i]=state->rgbm_seen[i]=0;state->dof_first[i]=state->rgbm_first[i]=0;
        state->pair_depth_source.Reset();
        state->pair_pre_fresh=false;state->pair_pre_target.Reset();
        state->pair_projection_status=snapshot_camera_projection(context,
            state->pair_projection[state->pair_count],state->pair_first_constant[state->pair_count]);
        ComPtr<ID3D11ShaderResourceView> depth;
        context->PSGetShaderResources(8,1,&depth);
        if(depth)depth->GetResource(&state->pair_depth_source);
        state->pair_depth_status=state->pair_depth[state->pair_count].capture(context,depth.Get());
        state->pair_projection_fresh=SUCCEEDED(state->pair_projection_status) && SUCCEEDED(state->pair_depth_status);
    }
    if (state->trace_remaining) {
        state->trace_matrix_fresh=false;
        ComPtr<ID3D11Buffer> source;
        context->VSGetConstantBuffers(1,1,&source);
        if (source) {
            D3D11_BUFFER_DESC desc{};source->GetDesc(&desc);
            if (desc.ByteWidth>=64) {
                if (!state->trace_matrix) {
                    D3D11_BUFFER_DESC target{};target.ByteWidth=64;
                    target.Usage=D3D11_USAGE_STAGING;target.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                    ComPtr<ID3D11Device> device;context->GetDevice(&device);
                    device->CreateBuffer(&target,nullptr,&state->trace_matrix);
                }
                if (state->trace_matrix) {
                    const D3D11_BOX box{0,0,0,64,1,1};
                    context->CopySubresourceRegion(state->trace_matrix.Get(),0,0,0,0,source.Get(),0,&box);
                    state->trace_matrix_fresh=true;
                }
            }
        }
    }
    if (state->motion_enabled) {
        state->motion_fresh = false;
        state->pair_fresh = false;
        state->pending_motion.Reset();
        context->OMGetRenderTargets(1, &state->pending_motion, nullptr);
    }
    if (!state->enabled) return;
    ComPtr<ID3D11ShaderResourceView> source;
    context->PSGetShaderResources(8, 1, &source);
    state->status = state->preview.capture(context, source.Get());
    state->fresh = SUCCEEDED(state->status);
    if (state->fresh && !state->first_capture_logged) {
        char message[256]{};
        std::snprintf(message, sizeof(message), "[DX11Depth] Copied camera-motion t8 depth: %ux%u. Raw depth, visual validation pending.",
            state->preview.width(), state->preview.height());
        log::message(log::level::info, message);
        state->first_capture_logged = true;
    }
}
bool draw(api::command_list* c, uint32_t, uint32_t, uint32_t, uint32_t) { capture(c); return false; }
bool draw_indexed(api::command_list* c, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) { capture(c); return false; }

void finish_pair(DepthState& state, ID3D11DeviceContext* context) {
    if (!state.pair_armed || !state.pair_trigger.ready()) return;
    if (!state.pair_fresh) {
        if (state.pair_count || GetTickCount64() > state.pair_due + 10000) {
            state.pair_armed = false;
            state.pair_status = "Pair cancelled: missing consecutive capture or timeout. Try during gameplay.";
            log::message(log::level::warning,state.pair_status.c_str());
        }
        return;
    }
    state.pair_frame[state.pair_count] = state.frame;
    ++state.pair_count;
    if (state.pair_count != 2) return;
    state.pair_armed = false;
    if (state.pair_frame[1] != state.pair_frame[0]+1 ||
        state.pair_boundary[0] != state.pair_boundary[1] || state.pair_time[1]-state.pair_time[0]>250 ||
        !state.pair_color[0].matches(state.pair_color[1]) || !state.pair_motion[0].matches(state.pair_motion[1]) ||
        !state.pair_depth[0].matches(state.pair_depth[1])) {
        state.pair_status = "Pair cancelled: frame/time gap, consumer boundary or texture layout changed.";
        log::message(log::level::warning,state.pair_status.c_str());
        return;
    }
    try {
        wchar_t path[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr,path,32768);
        if (!length || length>=32768) throw std::runtime_error("executable path");
        const auto root = std::filesystem::path(path).parent_path()/L"dx11-captures";
        std::filesystem::create_directories(root);
        const auto directory = root/("pair-"+std::to_string(GetCurrentProcessId())+"-"+
            std::to_string(GetTickCount64())+"-"+std::to_string(state.frame));
        if (!std::filesystem::create_directory(directory)) throw std::runtime_error("capture directory");
        for (unsigned i=0;i<2;++i) {
            if (state.pair_depth[i].width()!=state.pair_color[i].width() ||
                state.pair_depth[i].height()!=state.pair_color[i].height() ||
                FAILED(state.pair_depth[i].save(context,directory/("depth"+std::to_string(i)+".bin"),state.pair_frame[i])))
                throw std::runtime_error("depth layout/readback/write");
            if (FAILED(state.pair_color[i].save(context,directory/("color"+std::to_string(i)+".bin"),state.pair_frame[i])) ||
                FAILED(state.pair_motion[i].save(context,directory/("motion"+std::to_string(i)+".bin"),state.pair_frame[i])))
                throw std::runtime_error("texture readback/write");
            if (FAILED(state.pair_pre_color[i].save(context,directory/("precolor"+std::to_string(i)+".bin"),state.pair_frame[i])))
                throw std::runtime_error("pre-TAA readback/write");
        }
        std::ofstream jitter(directory/"jitter.json");jitter<<std::setprecision(9);
        jitter<<"{\"version\":1,\"coordinate_note\":\"source sample coordinates, not final DLAA API signs\",\"frames\":[";
        for (unsigned i=0;i<2;++i) {
            std::array<float,16> values{};
            if (FAILED(state.pair_projection[i].inspect(context,values))) throw std::runtime_error("projection readback");
            for(float value:values) if (!std::isfinite(value)) throw std::runtime_error("nonfinite projection");
            if (i) jitter<<",";
            jitter<<"{\"frame\":"<<state.pair_frame[i]<<",\"first_constant\":"<<state.pair_first_constant[i]
                <<",\"width\":"<<state.pair_pre_color[i].width()<<",\"height\":"<<state.pair_pre_color[i].height()<<",\"matrix\":[";
            for(unsigned j=0;j<16;++j) { if(j)jitter<<",";jitter<<values[j]; }
            jitter<<"],\"source_sample_pixels\":["<<values[2]*state.pair_pre_color[i].width()/2
                <<","<<values[6]*state.pair_pre_color[i].height()/2<<"]}";
        }
        jitter<<"]}\n";jitter.close();if(!jitter)throw std::runtime_error("jitter write");
        std::ofstream constants(directory/"consumer-constants.json");constants<<std::setprecision(9);
        constants<<"{\"version\":1,\"frames\":[";
        for(unsigned i=0;i<2;++i) {
            if(i)constants<<",";
            constants<<"{\"frame\":"<<state.pair_frame[i]<<",\"dof_seen\":"<<state.dof_seen[i]
                <<",\"rgbm_seen\":"<<state.rgbm_seen[i]<<",\"dof_same_depth_resource\":"<<(state.dof_same_depth[i]?"true":"false")
                <<",\"rgbm_same_precolor_resource\":"<<(state.rgbm_same_color[i]?"true":"false")
                <<",\"dof_binding_first_constant\":"<<state.dof_first[i]<<",\"rgbm_binding_first_constant\":"<<state.rgbm_first[i];
            for(unsigned kind=0;kind<2;++kind) {
                constants<<(kind?",\"rgbm_scale\":":",\"dof_inverse\":");
                const bool valid=kind?state.rgbm_valid[i]:state.dof_valid[i];
                if(!valid) { constants<<"null";continue; }
                std::array<float,16> values{};
                if(FAILED((kind?state.pair_rgbm[i]:state.pair_dof[i]).inspect(context,values)))throw std::runtime_error("consumer constants readback");
                constants<<"[";
                for(unsigned j=0;j<(kind?4u:8u);++j) {
                    if(!std::isfinite(values[j]))throw std::runtime_error("nonfinite consumer constants");
                    if(j)constants<<",";constants<<values[j];
                }
                constants<<"]";
            }
            constants<<"}";
        }
        constants<<"]}\n";constants.close();if(!constants)throw std::runtime_error("consumer constants write");
        // This marker is written last. Its absence means the capture is incomplete.
        std::ofstream manifest(directory/"capture.txt.tmp");
        manifest << "DX11 pair v3; diagnostic 0.9.2\nprecolor: before tagged Alias execution; color: matching consumer t0 afterwards; motion: before tagged execution.\n"
            << "depth: camera-motion PS t8 copied with projection, before tagged Alias execution. Raw device depth, not linear distance.\n"
            << "Consumer role: " << state.pair_boundary[0] << "; snapshot interval ms: "
            << state.pair_time[1]-state.pair_time[0] << "\n"
            << "Two consecutive presented frames; no CPU readback between their GPU snapshots.\n"
            << "jitter.json: same-frame camera VS b1 projection, binding offsets respected; API sign convention pending.\n"
            << "No claim of linear/HDR color or verified motion encoding. Camera cuts not automatically detected.\n";
        manifest.close();if (!manifest) throw std::runtime_error("manifest write");
        std::filesystem::rename(directory/"capture.txt.tmp",directory/"capture.txt");
        state.pair_status = "Saved "+directory.string();
        log::message(log::level::info,state.pair_status.c_str());
    } catch (...) {
        state.pair_status = "Pair save failed; any partial folder without capture.txt must not be analyzed.";
        log::message(log::level::error,state.pair_status.c_str());
    }
    for (unsigned i=0;i<2;++i) { state.pair_color[i]={};state.pair_motion[i]={};state.pair_pre_color[i]={};state.pair_projection[i]={};state.pair_depth[i]={};state.pair_dof[i]={};state.pair_rgbm[i]={}; }
    state.pair_pre_target.Reset();
}

void sync_neural_controls(api::effect_runtime* runtime,DepthState& state){
    const auto t=runtime->find_technique("DX11_Neural.fx","NeuralRendering");
    dx11_enhancement::NeuralSettings s;
    bool valid=t.handle!=0;
    if(valid){
        int32_t model=0,style=0,skin=1;
        valid=runtime->get_annotation_int_from_technique(t,"dx11_nr_model",&model,1) &&
            runtime->get_annotation_int_from_technique(t,"dx11_nr_style",&style,1) &&
            runtime->get_annotation_int_from_technique(t,"dx11_nr_auto_skin",&skin,1) &&
            runtime->get_annotation_float_from_technique(t,"dx11_nr_intensity",&s.intensity,1) &&
            runtime->get_annotation_float_from_technique(t,"dx11_nr_structure",&s.structure,1) &&
            runtime->get_annotation_float_from_technique(t,"dx11_nr_tone",&s.tone,1) &&
            runtime->get_annotation_float_from_technique(t,"dx11_nr_skin",&s.skin,1);
        s.model=static_cast<unsigned>(model);s.style=static_cast<unsigned>(style);s.auto_skin=static_cast<unsigned>(skin);
    }
    state.neural.request(valid && runtime->get_effects_state() && runtime->get_technique_state(t),s);
}
void begin_effects(api::effect_runtime* runtime, api::command_list* commands, api::resource_view target, api::resource_view) {
    auto* state = runtime->get_device()->get_private_data<DepthState>();
    if (!state) return;
    std::lock_guard lock(state->mutex);
    state->rendered = false;
    auto* context = reinterpret_cast<ID3D11DeviceContext*>(commands->get_native());
    sync_neural_controls(runtime,*state);
    state->neural.render(context,reinterpret_cast<ID3D11RenderTargetView*>(target.handle),
        state->live_depth.raw_view(),state->live_motion.raw_view(),state->frame,
        state->live_enabled && state->live_displayed_frame==state->frame);
    finish_pair(*state,context);
    if (state->trace_remaining && !state->pair_armed && state->trace_matrix_fresh) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(state->trace_matrix.Get(),0,D3D11_MAP_READ,0,&mapped))) {
            const auto* f=static_cast<const float*>(mapped.pData);
            char message[640]{};
            std::snprintf(message,sizeof(message),"[DX11Trace] frame=%llu cameraVSb1_first16=[%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g] candidateOnly",
                state->frame,f[0],f[1],f[2],f[3],f[4],f[5],f[6],f[7],f[8],f[9],f[10],f[11],f[12],f[13],f[14],f[15]);
            context->Unmap(state->trace_matrix.Get(),0);
            log::message(log::level::info,message);
        }
    }
    state->motion_rendered = false;
    if (state->motion_enabled && state->motion_fresh) {
        if (state->motion_inspect && !state->pair_armed) {
            state->motion_inspect = false;
            state->motion_inspected = state->motion.inspect_motion(context, state->motion_statistics,
                state->sample_u, state->sample_v) == S_OK;
            if (state->motion_inspected) {
                char message[320]{};
                const auto& s = state->motion_statistics;
                std::snprintf(message, sizeof(message),
                    "[DX11Motion] Raw XY: X=[%.9g,%.9g] Y=[%.9g,%.9g] finite=%llu nonfinite=%llu size=%ux%u format=%u",
                    s.minimum_x,s.maximum_x,s.minimum_y,s.maximum_y,s.finite,s.nonfinite,
                    state->motion.width(), state->motion.height(), static_cast<unsigned>(state->motion.format()));
                log::message(log::level::info, message);
                std::snprintf(message, sizeof(message),
                    "[DX11Motion] Selected pixel (%u,%u): raw XY=(%.9g,%.9g). Display gain/boost not applied.",
                    s.sample_column,s.sample_row,s.sample_x,s.sample_y);
                log::message(log::level::info, message);
            } else log::message(log::level::warning, "[DX11Motion] Inspection failed or no finite samples.");
        }
        state->motion_status = state->motion.render_motion(context, state->motion_gain, state->magnitude,
            state->uv_to_pixels, state->emphasize_small_motion);
        state->motion_rendered = SUCCEEDED(state->motion_status);
    }
    if (!state->enabled || !state->fresh) return;
    if (state->inspect_requested && !state->pair_armed) {
        state->inspect_requested = false;
        const HRESULT result = state->preview.inspect(context, state->statistics);
        state->inspected = result == S_OK;
        if (state->inspected) {
            char message[256]{};
            std::snprintf(message, sizeof(message),
                "[DX11Depth] Raw values min=%.9g max=%.9g finite=%llu nonfinite=%llu draws=%u capture=%d",
                state->statistics.minimum, state->statistics.maximum, state->statistics.finite,
                state->statistics.nonfinite, state->draws_this_frame, state->occurrence);
            log::message(log::level::info, message);
            if (state->fit_requested && state->statistics.maximum > state->statistics.minimum) {
                state->minimum = state->statistics.minimum;
                state->maximum = state->statistics.maximum;
                state->exponent = 1;
            }
        } else log::message(log::level::warning, "[DX11Depth] Raw-value inspection failed or had no finite samples.");
        state->fit_requested = false;
    }
    state->status = state->preview.render(context, state->exponent, state->invert,
        state->minimum, state->maximum, state->gradient);
    state->rendered = SUCCEEDED(state->status);
    if (state->rendered && !state->first_image_logged) {
        log::message(log::level::info, "[DX11Depth] Grayscale preview generated; open ReShade > DX11 Depth Diagnostic.");
        state->first_image_logged = true;
    }
}
void sync_dlaa_controls(api::effect_runtime* runtime,DepthState& state) {
    const auto technique=runtime->find_technique("DX11_DLAA.fx","DLAA");
    int32_t preset=0;
    const bool valid=technique.handle && runtime->get_annotation_int_from_technique(technique,"dx11_dlaa_preset",&preset,1);
    const bool enabled=valid && runtime->get_effects_state() && runtime->get_technique_state(technique);
    if(!state.live_request.update(enabled,static_cast<uint32_t>(preset)))return;
    state.neural.reset(); // DLAA changes invalidate the neural temporal input history.
    state.live.stop();state.live_target.Reset();state.live_camera=state.live_submitted=false;
    state.live_enabled=state.live_request.enabled;
    state.live_last_frame=UINT64_MAX;state.live_last_time=0;state.live_frames=0;
    state.live_verifier.reset();state.live_cpu_fresh=false;state.live_cpu_fast=0;
    state.live_perf_count=0;state.live_perf={};state.live_perf_projection=state.live_perf_motion=0;
    state.live_status=state.live_enabled?"Waiting for gameplay.":"Off.";
    char line[192]{};
    std::snprintf(line,sizeof(line),"[DX11DLAA] ReShade control: %s; requested preset=%u (%s).",
        state.live_enabled?"on":"off",state.live_request.preset,dx11_enhancement::dlaa_preset_name(state.live_request.preset));
    log::message(log::level::info,line);
}
void end_frame(api::effect_runtime* runtime) {
    auto* state = runtime->get_device()->get_private_data<DepthState>();
    if (!state) return;
    std::lock_guard lock(state->mutex);
    if(state->live.pending())live_failure(*state,"matching output consumer missing");
    state->live_camera=state->live_submitted=false;state->live_target.Reset();state->live_motion_source.Reset();
    diagnostics::FrameObservation observation;
    observation.frame=state->frame;observation.time_ms=GetTickCount64();
    observation.width=state->motion.width();observation.height=state->motion.height();
    observation.depth_format=static_cast<uint32_t>(state->preview.format());
    observation.motion_format=static_cast<uint32_t>(state->motion.format());
    observation.enabled=state->enabled && state->motion_enabled;
    // Diagnostic continuity only: no claim about color, exposure, camera cuts,
    // object coverage or numerical validity from successful texture copies.
    observation.usable=state->fresh && state->motion_fresh &&
        state->preview.width()==observation.width && state->preview.height()==observation.height;
    const auto previous=state->history_decision;
    state->history_decision=state->history.observe(observation);
    const auto& decision=state->history_decision;
    if (decision.reset) state->last_reset=decision.reason;
    if (decision.reset || previous.usable!=decision.usable) {
        char message[256]{};
        std::snprintf(message,sizeof(message),"[DX11History] frame=%llu generation=%llu candidateUsable=%u reset=%u reason=%s; diagnostic metadata only",
            state->frame,decision.generation,decision.usable ? 1u:0u,decision.reset ? 1u:0u,
            diagnostics::reset_name(decision.reason));
        log::message(log::level::info,message);
    }
    state->fresh = state->rendered = false;
    state->motion_fresh = state->motion_rendered = false;
    state->pending_motion.Reset();
    state->draws_last_frame = state->draws_this_frame;
    state->draws_this_frame = 0;
    if (state->trace_remaining && state->frame>state->trace_armed_frame) --state->trace_remaining;
    ++state->frame;
    if(state->pair_armed) state->pair_trigger.boundary(GetTickCount64());
    state->pair_projection_status=state->pair_depth_status=S_FALSE;
    state->trace_executes=state->trace_consumers=0;
    state->trace_output.Reset();state->trace_matrix_fresh=false;
    state->pair_fresh = false;
    state->pair_pre_fresh=state->pair_projection_fresh=false;state->pair_pre_target.Reset();
    sync_dlaa_controls(runtime,*state);
    sync_neural_controls(runtime,*state);
}
void reset_swapchain(api::swapchain* swapchain, bool) {
    auto* state = swapchain->get_device()->get_private_data<DepthState>();
    if (!state) return;
    std::lock_guard lock(state->mutex);
    state->live.stop();state->live_target.Reset();state->live_camera=state->live_submitted=false;
    state->neural.reset();state->live_displayed_frame=UINT64_MAX;
    state->live_last_frame=UINT64_MAX;state->live_status="Display reset; DLAA helper will restart if enabled.";
    state->history.invalidate(diagnostics::ResetReason::display_reset);
    state->trace_remaining=0;state->trace_matrix.Reset();state->trace_output.Reset();
    state->fresh = state->rendered = false;
    if (state->pair_armed) state->pair_status = "Pair cancelled: swapchain reset (display resources changed).";
    state->pair_armed = state->pair_fresh = false;
    state->pair_count = 0;
    for (unsigned i=0;i<2;++i) { state->pair_color[i]={};state->pair_motion[i]={};state->pair_pre_color[i]={};state->pair_projection[i]={};state->pair_depth[i]={};state->pair_dof[i]={};state->pair_rgbm[i]={}; }
    state->pair_pre_fresh=state->pair_projection_fresh=false;state->pair_pre_target.Reset();
    state->preview.reset();
    state->pending_motion.Reset();
    state->motion.reset();
    state->motion_fresh = state->motion_rendered = state->motion_inspected = false;
    state->motion_inspect = true;
    state->inspected = false;
    state->inspect_requested = state->fit_requested = true;
}
void dlaa_overlay(api::effect_runtime* runtime) {
    auto* state=runtime->get_device()->get_private_data<DepthState>();
    if(!state){ImGui::TextUnformatted("DLAA is unavailable in this application.");return;}
    std::lock_guard lock(state->mutex);
    ImGui::TextWrapped("%s",state->live_status.c_str());
    ImGui::TextWrapped("%s",state->neural.status().c_str());
    if(state->live_request.enabled && !state->live_enabled)
        ImGui::TextWrapped("Uncheck DLAA in Home, then check it again to retry.");
}
#ifdef DX11_DIAGNOSTIC_UI
void overlay(api::effect_runtime* runtime) {
    auto* state = runtime->get_device()->get_private_data<DepthState>();
    if (!state) { ImGui::TextUnformatted("Alien: Isolation depth adapter is inactive in this program."); return; }
    std::lock_guard lock(state->mutex);
    ImGui::TextWrapped("%s",state->live_status.c_str());
    ImGui::Text("DLAA frames displayed: %llu",state->live_frames);
    ImGui::Text("Camera upload cache frames: %llu",state->live_cpu_fast);
    ImGui::Separator();
    ImGui::TextUnformatted("Raw depth diagnostic - not linear distance");
    ImGui::Checkbox("Capture depth", &state->enabled);
    ImGui::Checkbox("Invert grayscale", &state->invert);
    ImGui::Checkbox("Show test gradient (not game depth)", &state->gradient);
    if (ImGui::Button("Inspect values and fit contrast")) state->inspect_requested = state->fit_requested = true;
    ImGui::SameLine();
    if (ImGui::Button("Reset contrast")) { state->minimum = 0; state->maximum = 1; state->exponent = 1; }
    ImGui::InputFloat("Black value", &state->minimum, 0, 0, "%.9f");
    ImGui::InputFloat("White value", &state->maximum, 0, 0, "%.9f");
    if (ImGui::InputInt("Capture occurrence (0 = last)", &state->occurrence)) {
        if (state->occurrence < 0) state->occurrence = 0;
        state->fresh = state->rendered = state->inspected = false;
        state->inspect_requested = state->fit_requested = true;
    }
    ImGui::Text("Matching draws last frame: %u", state->draws_last_frame);
    if (state->inspected) {
        ImGui::Text("Last inspected raw range: %.9g to %.9g", state->statistics.minimum, state->statistics.maximum);
        if (state->statistics.minimum == state->statistics.maximum)
            ImGui::TextWrapped("The inspected depth snapshot is flat: all finite pixels have the same value. Contrast cannot recover shapes. Try inspecting during gameplay or selecting another capture occurrence.");
    }
    ImGui::SliderFloat("Contrast exponent", &state->exponent, 0.01f, 4.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
    ImGui::Text("Camera-motion shader: %s | observed draws: %llu",
        state->shader_found ? "identified" : "not found yet", state->passes);
    if (!state->enabled) ImGui::TextUnformatted("Capture disabled. No depth image is being copied.");
    else if (state->rendered) {
        ImGui::Text("%u x %u | current frame | scene-depth validation pending",
            state->preview.width(), state->preview.height());
        if (state->gradient) ImGui::TextUnformatted("TEST GRADIENT ONLY - this image is not scene depth.");
        const float width = (ImGui::GetContentRegionAvail().x > 32) ? ImGui::GetContentRegionAvail().x : 32;
        ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(state->preview.image())),
            ImVec2(width, width * static_cast<float>(state->preview.height()) / state->preview.width()));
    } else {
        ImGui::TextUnformatted("No current-frame depth preview. Load a save and enable game motion blur.");
        if (FAILED(state->status)) ImGui::Text("Capture/render status: 0x%08X", static_cast<unsigned>(state->status));
    }
    ImGui::TextWrapped("Look for walls, doors and objects lining up with the scene. Contrast changes only this preview. Motion candidates are in the separate DX11 Motion Diagnostic panel.");
}

void motion_overlay(api::effect_runtime* runtime) {
    auto* state = runtime->get_device()->get_private_data<DepthState>();
    if (!state) { ImGui::TextUnformatted("Alien: Isolation motion adapter is inactive."); return; }
    std::lock_guard lock(state->mutex);
    ImGui::Text("Candidate continuity: %s | history generation: %llu",
        state->history_decision.usable ? "available" : "unavailable",state->history_decision.generation);
    ImGui::Text("Last reset: %s",diagnostics::reset_name(state->last_reset));
    ImGui::TextWrapped("Tracks depth/motion availability only. Camera cuts and full reconstruction readiness are not yet detected.");
    if (ImGui::Button("Request diagnostic history reset"))
        state->history.invalidate(diagnostics::ResetReason::manual);
    ImGui::TextWrapped("Motion candidate: camera-motion output sampled at the DoF/RGBM boundary. Direction, units and moving-object coverage still need validation.");
    ImGui::Checkbox("Capture motion", &state->motion_enabled);
    ImGui::Checkbox("Movement strength (grayscale)", &state->magnitude);
    ImGui::Checkbox("Emphasize small motion (display only)", &state->emphasize_small_motion);
    ImGui::Checkbox("Assume UV units and scale by image size (experimental)", &state->uv_to_pixels);
    ImGui::SliderFloat("Display gain", &state->motion_gain, 0.01f, 10000.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    if (!state->pair_armed && ImGui::Button("Capture pre/post-TAA pair in 3 seconds")) {
        state->pair_armed = true;state->pair_fresh = false;state->pair_count = 0;
        state->pair_pre_fresh=state->pair_projection_fresh=false;state->pair_pre_target.Reset();
        state->pair_due = GetTickCount64()+3000;
        state->pair_trigger.arm(state->pair_due);
        state->motion_enabled = true;
        state->pair_status = "Armed. Close overlay and turn slowly; saving may briefly pause the game.";
        log::message(log::level::info,"[DX11Pair] User armed two-frame capture with three-second delay.");
    }
    ImGui::TextWrapped("%s",state->pair_status.c_str());
    if (ImGui::Button("Trace TAA order and camera constants (32 frames)")) {
        state->trace_remaining=32;
        state->trace_armed_frame=state->frame;
        log::message(log::level::info,"[DX11Trace] User requested 32-frame trace. Matrix values are candidates, not validated jitter.");
    }
    ImGui::Text("Trace frames remaining: %u",state->trace_remaining);
    if (ImGui::Button("Inspect motion values")) state->motion_inspect = true;
    if (state->motion_inspected) {
        const auto& s = state->motion_statistics;
        ImGui::Text("Last snapshot: X %.9g to %.9g | Y %.9g to %.9g", s.minimum_x,s.maximum_x,s.minimum_y,s.maximum_y);
        ImGui::Text("Finite vectors: %llu | invalid: %llu", s.finite,s.nonfinite);
        ImGui::Text("Selected pixel (%u,%u): raw X %.9g | Y %.9g",
            s.sample_column,s.sample_row,s.sample_x,s.sample_y);
    }
    if (!state->motion_enabled) ImGui::TextUnformatted("Motion capture disabled.");
    else if (!state->motion_rendered) {
        ImGui::TextUnformatted("No current-frame motion image. Load a save; keep motion blur enabled.");
        if (FAILED(state->motion_status)) ImGui::Text("Capture/render status: 0x%08X", static_cast<unsigned>(state->motion_status));
    } else {
        ImGui::Text("%u x %u | current-frame candidate | format %u", state->motion.width(), state->motion.height(), static_cast<unsigned>(state->motion.format()));
        const float width = ImGui::GetContentRegionAvail().x > 32 ? ImGui::GetContentRegionAvail().x : 32;
        ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(state->motion.image())),
            ImVec2(width, width * static_cast<float>(state->motion.height()) / state->motion.width()));
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const auto origin = ImGui::GetItemRectMin();
            const auto size = ImGui::GetItemRectSize();
            const auto mouse = ImGui::GetMousePos();
            state->sample_u = (mouse.x - origin.x) / size.x;
            state->sample_v = (mouse.y - origin.y) / size.y;
            state->motion_inspect = true;
        }
    }
    ImGui::TextWrapped("Click an object in this preview to inspect that screen position on the next captured frame. Values above are a snapshot, not live. Small-motion emphasis makes tiny values easier to see; it also emphasizes noise. Raw data is unchanged.");
    ImGui::TextWrapped("Color view: neutral gray is zero; red increases with positive X and green with positive Y. Strength view: black is zero and brighter means larger magnitude. Magenta marks invalid numbers. Gain affects the display only.");
}
#endif
}
void diagnostics::register_depth_events() {
    register_event<addon_event::map_buffer_region>(camera_map);
    register_event<addon_event::unmap_buffer_region>(camera_unmap);
    register_event<addon_event::init_device>(init_device);
    register_event<addon_event::destroy_device>(destroy_device);
    register_event<addon_event::init_command_list>(init_commands);
    register_event<addon_event::destroy_command_list>(destroy_commands);
    register_event<addon_event::init_pipeline>(init_pipeline);
    register_event<addon_event::bind_pipeline>(bind_pipeline);
    register_event<addon_event::dispatch>(trace_dispatch);
    register_event<addon_event::copy_resource>(trace_copy);
    register_event<addon_event::execute_secondary_command_list>(trace_secondary);
    register_event<addon_event::draw>(draw);
    register_event<addon_event::draw_indexed>(draw_indexed);
    register_event<addon_event::reshade_begin_effects>(begin_effects);
    register_event<addon_event::reshade_present>(end_frame);
    register_event<addon_event::destroy_swapchain>(reset_swapchain);
    register_overlay("DLAA status", dlaa_overlay);
#ifdef DX11_DIAGNOSTIC_UI
    register_overlay("DX11 Depth Diagnostic", overlay);
    register_overlay("DX11 Motion Diagnostic", motion_overlay);
#endif
}


````

## src/adapters/alien_neural.hpp

SHA-256: 7c264d6fedf2d7daedd41c510e44d8d0d5c8871b09a7d83e5036de57726e5ee3

````
#pragma once
#include "core/neural_bridge.hpp"
#include <wrl/client.h>
#include <string>

namespace alien_isolation {
// Experimental final SDR stage: game HUD pixels are included, without a UI mask.
class NeuralTrial {
public:
    void request(bool enabled,dx11_enhancement::NeuralSettings settings);
    void reset();
    void render(ID3D11DeviceContext*,ID3D11RenderTargetView*,ID3D11ShaderResourceView* depth,
        ID3D11ShaderResourceView* motion,uint64_t frame,bool current_dlaa);
    const std::string& status()const{return status_;}
private:
    dx11_enhancement::NeuralBridge bridge_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> color_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view_;
    dx11_enhancement::NeuralSettings settings_;
    bool requested_=false,failed_=false;
    uint64_t last_frame_=UINT64_MAX,last_time_=0,frames_=0;
    double waits_=0;
    std::string status_="Neural rendering: off.";
};
}

````

## src/adapters/alien_neural.cpp

SHA-256: 3a27e8a43a7709a9488282a1aebf47652ef9aad454c6fd801bb486c400383470

````
#include "alien_neural.hpp"
#include <reshade.hpp>
#include <fstream>
#include <stdexcept>
#include <cstdio>

namespace alien_isolation {
using Microsoft::WRL::ComPtr;
static void check(HRESULT result,const char* message){if(FAILED(result))throw std::runtime_error(message);}
void NeuralTrial::reset(){
    bridge_.stop();view_.Reset();color_.Reset();last_frame_=UINT64_MAX;last_time_=0;frames_=0;waits_=0;
    if(!failed_)status_=requested_?"Neural rendering: waiting for DLAA gameplay.":"Neural rendering: off.";
}
void NeuralTrial::request(bool enabled,dx11_enhancement::NeuralSettings settings){
    if(enabled==requested_ && settings==settings_)return;
    settings_=settings;
    requested_=enabled;failed_=false;reset();
    reshade::log::message(reshade::log::level::info,status_.c_str());
}
void NeuralTrial::render(ID3D11DeviceContext* context,ID3D11RenderTargetView* target,
    ID3D11ShaderResourceView* depth,ID3D11ShaderResourceView* motion,uint64_t frame,bool current_dlaa){
    if(!requested_ || failed_)return;
    if(!current_dlaa){status_="Neural rendering: waiting for DLAA gameplay.";return;}
    try{
        if(!target || !depth || !motion)throw std::runtime_error("missing current-frame inputs");
        D3D11_RENDER_TARGET_VIEW_DESC vd{};target->GetDesc(&vd);
        if(vd.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D || vd.Texture2D.MipSlice!=0 ||
            (vd.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && vd.Format!=DXGI_FORMAT_B8G8R8A8_UNORM))
            throw std::runtime_error("first trial requires an SDR UNORM backbuffer view");
        ComPtr<ID3D11Resource> resource;target->GetResource(&resource);
        ComPtr<ID3D11Texture2D> source;check(resource.As(&source),"backbuffer is not a texture");
        D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);
        if(desc.MipLevels!=1 || desc.ArraySize!=1 || desc.SampleDesc.Count!=1)
            throw std::runtime_error("unsupported backbuffer layout");
        if(color_){
            D3D11_TEXTURE2D_DESC old{};color_->GetDesc(&old);
            if(old.Width!=desc.Width || old.Height!=desc.Height || old.Format!=vd.Format)reset();
        }
        ComPtr<ID3D11Device> device;context->GetDevice(&device);
        if(!color_){
            desc.Format=vd.Format;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            desc.CPUAccessFlags=0;desc.MiscFlags=0;
            check(device->CreateTexture2D(&desc,nullptr,&color_),"final-color copy allocation failed");
            check(device->CreateShaderResourceView(color_.Get(),nullptr,&view_),"final-color view failed");
        }
        if(!bridge_.started()){
            wchar_t exe[32768]{};if(!GetModuleFileNameW(nullptr,exe,32768))throw std::runtime_error("game path unavailable");
            std::ifstream config(std::filesystem::path(exe).parent_path()/L"dx11-neural-paths.txt");
            std::string helper,runtime,cache;
            if(!std::getline(config,helper) || !std::getline(config,runtime) || !std::getline(config,cache))
                throw std::runtime_error("helper configuration missing");
            bridge_.start(context,desc.Width,desc.Height,std::filesystem::u8path(helper),std::filesystem::u8path(runtime),
                std::filesystem::u8path(cache)/std::to_string(GetTickCount64()),settings_);
        }
        if(!bridge_.ready()){status_="Neural rendering: starting helper; DLAA continues.";return;}
        // Copy encoded SDR values without an sRGB decode. Bridge preserves original alpha.
        context->CopyResource(color_.Get(),source.Get());
        dx11_enhancement::BridgeFrame metadata;
        metadata.width=desc.Width;metadata.height=desc.Height;metadata.source_frame=frame;
        metadata.motion_scale_x=-float(desc.Width);metadata.motion_scale_y=-float(desc.Height);
        const auto now=GetTickCount64(),elapsed=last_time_?now-last_time_:0;
        metadata.reset=last_frame_==UINT64_MAX || frame!=last_frame_+1 || elapsed>250;
        metadata.frame_ms=elapsed && elapsed<=250?float(elapsed):16.6667f;
        bridge_.process(context,view_.Get(),depth,motion,target,metadata);
        last_frame_=frame;last_time_=now;++frames_;waits_+=bridge_.last_wait_ms();
        status_="Neural rendering: active (experimental; includes game HUD).";
        if(frames_==1){
            char line[256]{};std::snprintf(line,sizeof(line),
                "[DX11Neural] First result: final SDR %ux%u format=%u; before ReShade effects; HUD included; controls recorded in helper session settings.txt.",
                desc.Width,desc.Height,static_cast<unsigned>(vd.Format));
            reshade::log::message(reshade::log::level::info,line);
        }
        if(frames_%120==0){
            char line[192]{};std::snprintf(line,sizeof(line),"[DX11Neural] 120-frame mean CPU output wait %.3fms; not GPU timing or FPS.",waits_/120);
            reshade::log::message(reshade::log::level::info,line);waits_=0;
        }
    }catch(const std::exception& error){
        failed_=true;reset();status_=std::string("Neural rendering off: ")+error.what()+". Uncheck/check to retry.";
        reshade::log::message(reshade::log::level::warning,status_.c_str());
    }
}
}

````

## shaders/DX11_DLAA.fx

SHA-256: 0ea11aaf553dbeeea6814e579fa7d83db1175d38cc124e7fef19e9419a6cff5d

````
// UI bridge to the native addon. Reconstruction runs before game post-processing.
// This compute pass does not read or change the picture.
#ifndef DX11_DLAA_PRESET
#define DX11_DLAA_PRESET 0
#endif

#if DX11_DLAA_PRESET != 0 && (DX11_DLAA_PRESET < 10 || DX11_DLAA_PRESET > 13)
#error DX11_DLAA_PRESET must be 0 (Auto), 10 (J), 11 (K), 12 (L), or 13 (M). Older presets are removed, deprecated or reserved by NVIDIA.
#endif

void DLAA_ControlCS(uint3 id : SV_DispatchThreadID) {}

technique DLAA <
    ui_label = "DLAA";
    ui_tooltip = "Native-resolution NVIDIA anti-aliasing. Requires the DX11 Enhancement addon. Edit global preprocessor definitions: DX11_DLAA_PRESET = 0 (Auto), 10 (J), 11 (K), 12 (L), 13 (M). Start with Auto. Status is in the DLAA status panel. Effect order does not change its processing stage.";
    dx11_dlaa_preset = DX11_DLAA_PRESET;
> {
    pass {
        ComputeShader = DLAA_ControlCS<1, 1>;
        DispatchSizeX = 1;
        DispatchSizeY = 1;
    }
}

````

## shaders/DX11_Neural.fx

SHA-256: f4854cb0be73f44d3c0807624fd436e5393b3fc9f867d552e855ff222e9f96ee

````
// Native controls. Change global definitions and Reload; only the neural helper restarts.
#ifndef DX11_NR_MODEL
#define DX11_NR_MODEL 0
#endif
#ifndef DX11_NR_STYLE
#define DX11_NR_STYLE 0
#endif
#ifndef DX11_NR_INTENSITY
#define DX11_NR_INTENSITY 1.0
#endif
#ifndef DX11_NR_STRUCTURE
#define DX11_NR_STRUCTURE 1.0
#endif
#ifndef DX11_NR_TONE
#define DX11_NR_TONE 0.0
#endif
#ifndef DX11_NR_SKIN
#define DX11_NR_SKIN 1.0
#endif
#ifndef DX11_NR_AUTO_SKIN
#define DX11_NR_AUTO_SKIN 1
#endif
#if DX11_NR_MODEL < 0 || DX11_NR_MODEL > 3 || DX11_NR_STYLE < 0 || DX11_NR_STYLE > 2 || DX11_NR_AUTO_SKIN < 0 || DX11_NR_AUTO_SKIN > 1
#error Neural model must be 0..3, style 0..2, auto skin 0 or 1.
#endif
void Neural_ControlCS(uint3 id : SV_DispatchThreadID) {}
technique NeuralRendering <
    ui_label = "Neural rendering (experimental)";
    ui_tooltip = "Requires DLAA. Global definitions: DX11_NR_MODEL 0=runtime choice, 1/2/3=requested hints (no difference observed in this runtime); STYLE 0/1/2=experimental profiles; INTENSITY=overall strength; STRUCTURE=fine detail; TONE=broad lighting/color (0 preserves original tone); SKIN=skin detail (-1 follows structure); AUTO_SKIN=skin detection 0/1 (not HUD protection). Strength range 0..2, start at 1. Reload after edits; only neural helper restarts. Game HUD is included and NOT protected by a UI mask. Status in DLAA status. Uncheck to compare with DLAA alone.";
    dx11_nr_model = DX11_NR_MODEL;
    dx11_nr_style = DX11_NR_STYLE;
    dx11_nr_intensity = 1.0 * DX11_NR_INTENSITY;
    dx11_nr_structure = 1.0 * DX11_NR_STRUCTURE;
    dx11_nr_tone = 1.0 * DX11_NR_TONE;
    dx11_nr_skin = 1.0 * DX11_NR_SKIN;
    dx11_nr_auto_skin = DX11_NR_AUTO_SKIN;
> {
    pass {
        ComputeShader = Neural_ControlCS<1, 1>;
        DispatchSizeX = 1;
        DispatchSizeY = 1;
    }
}

````

## tests/neural_image_probe.hpp

SHA-256: 95403eb876d5c6fbfe882396326026942915454a7a94192316c98ed87608e56e

````
#pragma once
#include <DirectXPackedVector.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

namespace neural_probe {
using namespace DirectX::PackedVector;
struct Texture {
    ComPtr<ID3D12Resource> image,upload;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
};
ComPtr<ID3D12Resource> buffer(Session& s,UINT64 size,D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES heap{};heap.Type=type;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width=size;desc.Height=desc.DepthOrArraySize=desc.MipLevels=1;
    desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> result;
    dx(s.device12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
        type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,IID_PPV_ARGS(&result)),"probe buffer");return result;
}
void transition(Session& s,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};
    s.commands->ResourceBarrier(1,&barrier);
}
Texture texture(Session& s,UINT width,UINT height,DXGI_FORMAT format,const void* pixels,UINT pixel_bytes,bool output=false) {
    Texture result;D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width=width;desc.Height=height;desc.DepthOrArraySize=desc.MipLevels=1;
    desc.SampleDesc.Count=1;desc.Format=format;
    desc.Flags=output?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE;
    dx(s.device12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,IID_PPV_ARGS(&result.image)),"probe texture");
    UINT64 total=0;s.device12->GetCopyableFootprints(&desc,0,1,0,&result.footprint,nullptr,nullptr,&total);
    result.upload=buffer(s,total,D3D12_HEAP_TYPE_UPLOAD);
    void* mapped=nullptr;const D3D12_RANGE no_read{0,0};dx(result.upload->Map(0,&no_read,&mapped),"upload map");
    for(UINT y=0;y<height;++y)std::memcpy(static_cast<char*>(mapped)+size_t(y)*result.footprint.Footprint.RowPitch,
        static_cast<const char*>(pixels)+size_t(y)*width*pixel_bytes,size_t(width)*pixel_bytes);
    result.upload->Unmap(0,nullptr);
    D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=result.upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint=result.footprint;dst.pResource=result.image.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    s.commands->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    transition(s,result.image.Get(),D3D12_RESOURCE_STATE_COPY_DEST,output?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return result;
}
template<class T> void read(const std::filesystem::path& path,std::vector<T>& data) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file || file.tellg()!=static_cast<std::streamoff>(data.size()*sizeof(T)))throw std::runtime_error("probe input length mismatch");
    file.seekg(0);file.read(reinterpret_cast<char*>(data.data()),static_cast<std::streamsize>(data.size()*sizeof(T)));
    if(!file)throw std::runtime_error("probe input read failed");
}
void save(const std::filesystem::path& path,const std::vector<uint16_t>& pixels) {
    std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(pixels.data()),static_cast<std::streamsize>(pixels.size()*2));
    if(!file)throw std::runtime_error("probe output write failed");
}
void images(Session& s,const std::filesystem::path& prepared,const std::filesystem::path& output,UINT w,UINT h) {
    std::printf("REPLAY DIAGNOSTIC: %ux%u. Reinhard + sRGB approximation of captured linear color; NOT the game's final color stage.\n",w,h);
    std::ofstream(output/L"image-layout.txt")<<w<<" "<<h<<"\nRGBA16F little endian, RGB display-encoded approximation\n";
    for(unsigned frame=0;frame<2;++frame){
        std::vector<uint16_t> color(size_t(w)*h*4),motion(size_t(w)*h*2),result(size_t(w)*h*4,XMConvertFloatToHalf(-10.f));
        std::vector<float> depth(size_t(w)*h);
        read(prepared/("color"+std::to_string(frame)+".raw"),color);
        read(prepared/("motion"+std::to_string(frame)+".raw"),motion);
        read(prepared/("depth"+std::to_string(frame)+".raw"),depth);
        for(size_t i=0;i<color.size();++i){
            if(i%4==3){color[i]=XMConvertFloatToHalf(1.f);continue;}
            float value=XMConvertHalfToFloat(color[i]);if(!std::isfinite(value))throw std::runtime_error("invalid source color");
            value=std::max(0.f,value);value=value/(1.f+value);
            value=value<=.0031308f?12.92f*value:1.055f*std::pow(value,1.f/2.4f)-.055f;
            color[i]=XMConvertFloatToHalf(value);
        }
        save(output/("input"+std::to_string(frame)+".rgba16"),color);
        dx(s.allocator->Reset(),"allocator reset");dx(s.commands->Reset(s.allocator.Get(),nullptr),"command reset");
        auto c=texture(s,w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,color.data(),8);
        auto z=texture(s,w,h,DXGI_FORMAT_R32_FLOAT,depth.data(),4);
        auto m=texture(s,w,h,DXGI_FORMAT_R16G16_FLOAT,motion.data(),4);
        auto out=texture(s,w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,result.data(),8,true);
        auto* p=s.params;
        p->Set("DLSSNR.Color",c.image.Get());p->Set("DLSSNR.Depth",z.image.Get());
        p->Set("DLSSNR.MVec",m.image.Get());p->Set("DLSSNR.Output",out.image.Get());
        p->Set("DLSSNR.Reset",frame==0?1u:0u);p->Set("DLSSNR.DepthInverted",0u);
        p->Set("DLSSNR.MVecScaleX",-float(w));p->Set("DLSSNR.MVecScaleY",-float(h));
        for(const char* resource:{"Color","Depth","MVec","Output"}){
            const auto prefix=std::string("DLSSNR.")+resource+"Subrect";
            p->Set((prefix+"BaseX").c_str(),0u);p->Set((prefix+"BaseY").c_str(),0u);
            p->Set((prefix+"Width").c_str(),w);p->Set((prefix+"Height").c_str(),h);
        }
        D3D12_QUERY_HEAP_DESC query_desc{};query_desc.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;query_desc.Count=2;
        ComPtr<ID3D12QueryHeap> query;dx(s.device12->CreateQueryHeap(&query_desc,IID_PPV_ARGS(&query)),"timestamp heap");
        auto times=buffer(s,16,D3D12_HEAP_TYPE_READBACK);
        s.commands->EndQuery(query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
        const auto start=std::chrono::steady_clock::now();
        const auto evaluation=NVSDK_NGX_D3D12_EvaluateFeature(s.commands.Get(),s.feature,p,nullptr);
        const auto api_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        if(!report(evaluation,"EvaluateFeature(18)"))throw std::runtime_error("neural evaluation failed");
        s.commands->EndQuery(query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);
        s.commands->ResolveQueryData(query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,times.Get(),0);
        transition(s,out.image.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        const UINT64 bytes=UINT64(out.footprint.Footprint.RowPitch)*h;
        auto readback=buffer(s,bytes,D3D12_HEAP_TYPE_READBACK);
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=out.image.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=out.footprint;
        s.commands->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        s.finish();
        void* mapped=nullptr;D3D12_RANGE range{0,static_cast<SIZE_T>(bytes)};
        dx(readback->Map(0,&range,&mapped),"readback map");
        for(UINT y=0;y<h;++y)std::memcpy(result.data()+size_t(y)*w*4,static_cast<char*>(mapped)+size_t(y)*out.footprint.Footprint.RowPitch,size_t(w)*8);
        const D3D12_RANGE no_write{0,0};readback->Unmap(0,&no_write);
        UINT64 frequency=0;dx(s.queue->GetTimestampFrequency(&frequency),"timestamp frequency");
        range={0,16};dx(times->Map(0,&range,&mapped),"timestamp map");
        UINT64 stamps[2]{};std::memcpy(stamps,mapped,16);times->Unmap(0,&no_write);
        size_t invalid=0,sentinel=0,changed=0;float minimum=INFINITY,maximum=-INFINITY;double error=0;
        for(size_t i=0;i<result.size();++i)if(i%4!=3){
            const float value=XMConvertHalfToFloat(result[i]);
            invalid+=!std::isfinite(value);sentinel+=value==-10.f;changed+=result[i]!=color[i];
            minimum=std::min(minimum,value);maximum=std::max(maximum,value);error+=std::abs(value-XMConvertHalfToFloat(color[i]));
        }
        save(output/("output"+std::to_string(frame)+".rgba16"),result);
        std::printf("frame=%u RGB=[%g,%g] invalid=%zu sentinel=%zu changed=%zu meanAbsDifference=%g; CPU evaluate=%.3fms GPU bracket=%.3fms\n",
            frame,minimum,maximum,invalid,sentinel,changed,error/(double(w)*h*3),api_ms,double(stamps[1]-stamps[0])*1000/double(frequency));
        if(invalid || sentinel || !changed || maximum-minimum<.01f)throw std::runtime_error("invalid or trivial neural output");
    }
    std::puts("PASS: two recorded-input neural evaluations, finite nontrivial output; NOT live integration or final color/temporal validation.");
}
}

````

## tests/neural_runtime_probe.cpp

SHA-256: 23bd85ef0f10adca58f90c817275b087f3bbaada9420b9aa3d32f1704d9e2665

````
// Isolated API feasibility probe. Never loads into the game or changes its files.
// Feature 18 and DLSSNR.* names are community-documented, not a public SDK contract.
// Research references and pinned revisions are in NEURAL-RENDERING.md.
#include <Windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <nvsdk_ngx.h>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
using Microsoft::WRL::ComPtr;
void dx(HRESULT result,const char* stage) {
    if(FAILED(result)){char text[160]{};std::snprintf(text,sizeof(text),"%s HRESULT=%08X",stage,unsigned(result));throw std::runtime_error(text);}
}
bool report(NVSDK_NGX_Result result,const char* stage) {
    std::printf("%s: 0x%08X\n",stage,unsigned(result));std::fflush(stdout);
    return NVSDK_NGX_SUCCEED(result);
}
struct Session {
    bool use12=false,initialized=false;
    ComPtr<ID3D11Device> device11;ComPtr<ID3D11DeviceContext> context11;
    ComPtr<ID3D12Device> device12;ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> commands;
    NVSDK_NGX_Parameter* params=nullptr;NVSDK_NGX_Handle* feature=nullptr;
    void finish() {
        if(!use12)return;
        dx(commands->Close(),"close commands");
        ID3D12CommandList* lists[]={commands.Get()};queue->ExecuteCommandLists(1,lists);
        ComPtr<ID3D12Fence> fence;dx(device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)throw std::runtime_error("fence event");
        const HRESULT signal=queue->Signal(fence.Get(),1);
        const HRESULT completion=SUCCEEDED(signal)?fence->SetEventOnCompletion(1,event):signal;
        const DWORD wait=SUCCEEDED(completion)?WaitForSingleObject(event,10000):WAIT_FAILED;
        CloseHandle(event);dx(completion,"GPU completion");
        if(wait!=WAIT_OBJECT_0)throw std::runtime_error("GPU completion timeout");
        dx(device12->GetDeviceRemovedReason(),"device health");
    }
    ~Session() {
        if(feature){if(use12)NVSDK_NGX_D3D12_ReleaseFeature(feature);else NVSDK_NGX_D3D11_ReleaseFeature(feature);}
        if(params){if(use12)NVSDK_NGX_D3D12_DestroyParameters(params);else NVSDK_NGX_D3D11_DestroyParameters(params);}
        if(initialized){if(use12)NVSDK_NGX_D3D12_Shutdown1(device12.Get());else NVSDK_NGX_D3D11_Shutdown1(device11.Get());}
    }
};
#include "neural_image_probe.hpp"
int wmain(int argc,wchar_t** argv) {
    try {
        if((argc!=5 && argc!=6) || (std::wstring(argv[1])!=L"dx11" && std::wstring(argv[1])!=L"dx12"))
            throw std::runtime_error("usage: neural_runtime_probe dx11|dx12 runtime-directory cache-directory nr|dlaa [prepared-images]");
        const bool neural=std::wstring(argv[4])==L"nr";
        if(!neural && std::wstring(argv[4])!=L"dlaa")throw std::runtime_error("invalid feature choice");
        const auto runtime=std::filesystem::absolute(argv[2]),cache=std::filesystem::absolute(argv[3]);
        std::filesystem::create_directories(cache);
        Session session;session.use12=std::wstring(argv[1])==L"dx12";
        UINT width=1280,height=720;
        if(argc==6){
            if(!neural || !session.use12)throw std::runtime_error("image evaluation requires DX12 neural mode");
            unsigned frames=0;float exposure=0;std::ifstream config(std::filesystem::path(argv[5])/L"replay.txt");
            config>>width>>height>>frames>>exposure;
            if(!config || !width || !height || width>4096 || height>4096 || frames!=2 || exposure!=1.f)
                throw std::runtime_error("unsupported image test layout/exposure");
        }
        ComPtr<IDXGIFactory1> factory;dx(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");
        ComPtr<IDXGIAdapter1> adapter;
        for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> candidate;const auto r=factory->EnumAdapters1(i,&candidate);
            if(r==DXGI_ERROR_NOT_FOUND)break;dx(r,"adapter enumeration");
            DXGI_ADAPTER_DESC1 info{};dx(candidate->GetDesc1(&info),"adapter info");
            if(info.VendorId==0x10de && !(info.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)){
                adapter=candidate;std::wprintf(L"GPU: %ls; API=%ls; feature=%ls\n",info.Description,argv[1],argv[4]);break;}}
        if(!adapter)throw std::runtime_error("NVIDIA adapter missing");
        if(session.use12){
            dx(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&session.device12)),"D3D12 device");
            D3D12_COMMAND_QUEUE_DESC desc{};desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
            dx(session.device12->CreateCommandQueue(&desc,IID_PPV_ARGS(&session.queue)),"queue");
            dx(session.device12->CreateCommandAllocator(desc.Type,IID_PPV_ARGS(&session.allocator)),"allocator");
            dx(session.device12->CreateCommandList(0,desc.Type,session.allocator.Get(),nullptr,IID_PPV_ARGS(&session.commands)),"commands");
        }else dx(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,D3D11_SDK_VERSION,
            &session.device11,nullptr,&session.context11),"D3D11 device");
        const wchar_t* paths[]={runtime.c_str()};NVSDK_NGX_FeatureCommonInfo common{};
        common.PathListInfo.Path=paths;common.PathListInfo.Length=1;
        constexpr const char* project="211347f0-e644-4da9-bbe8-5d7630b1f7c8";
        auto result=session.use12?NVSDK_NGX_D3D12_Init_with_ProjectID(project,NVSDK_NGX_ENGINE_TYPE_CUSTOM,"0.10.0",cache.c_str(),session.device12.Get(),&common):
            NVSDK_NGX_D3D11_Init_with_ProjectID(project,NVSDK_NGX_ENGINE_TYPE_CUSTOM,"0.10.0",cache.c_str(),session.device11.Get(),&common);
        if(!report(result,"NGX initialization (own project ID)"))return 2;
        session.initialized=true;
        result=session.use12?NVSDK_NGX_D3D12_GetCapabilityParameters(&session.params):NVSDK_NGX_D3D11_GetCapabilityParameters(&session.params);
        if(!report(result,"GetCapabilityParameters"))return 3;
        auto* p=session.params;
        for(const char* key:{"SuperSampling.Available","DLSSNR.Available","NeuralRendering.Available"}){
            int available=-1;const auto query=p->Get(key,&available);
            std::printf("Capability %s: result=0x%08X value=%d\n",key,unsigned(query),available);
        }
        p->Set("CreationNodeMask",1u);p->Set("VisibilityNodeMask",1u);
        p->Set("Width",width);p->Set("Height",height);p->Set("OutWidth",width);p->Set("OutHeight",height);
        p->Set("PerfQualityValue",int(NVSDK_NGX_PerfQuality_Value_DLAA));
        p->Set("DLSS.Feature.Create.Flags",int(NVSDK_NGX_DLSS_Feature_Flags_IsHDR|NVSDK_NGX_DLSS_Feature_Flags_MVLowRes));
        if(neural){
            p->Set("DLSSNR.Enabled",1u);p->Set("DLSSNR.Width",width);p->Set("DLSSNR.Height",height);
            p->Set("DLSSNR.Hint.Render.Preset",0u);p->Set("DLSSNR.Intensity",1.f);
            p->Set("DLSSNR.Style",0u);p->Set("DLSSNR.LocalStructureStrength",1.f);
            p->Set("DLSSNR.LocalToneStrength",0.f);p->Set("DLSSNR.SkinStructureStrength",1.f);
            p->Set("DLSSNR.UseAutoMask",1u);p->Set("DLSSNR.UICorrection",1u);
        }
        const auto feature=neural?static_cast<NVSDK_NGX_Feature>(18):NVSDK_NGX_Feature_SuperSampling;
        result=session.use12?NVSDK_NGX_D3D12_CreateFeature(session.commands.Get(),feature,p,&session.feature):
            NVSDK_NGX_D3D11_CreateFeature(session.context11.Get(),feature,p,&session.feature);
        const bool created=report(result,"CreateFeature");
        wchar_t loaded[32768]{};
        if(HMODULE module=GetModuleHandleW(neural?L"nvngx_dlssnr.dll":L"nvngx_dlss.dll")){
            GetModuleFileNameW(module,loaded,32768);std::wprintf(L"Loaded feature runtime: %ls\n",loaded);
        }else std::puts("Feature runtime not loaded under expected module name.");
        session.finish();
        if(!created || !session.feature){std::puts("NOT READY: feature creation failed; no images evaluated.");return 4;}
        std::puts("CREATED: feature initialization only; image evaluation and compatibility NOT proven.");
        if(argc==6)neural_probe::images(session,argv[5],cache,width,height);
        return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}

````

## tests/neural_transport_test.cpp

SHA-256: cbef073c567547c6a4e98f9faf388cc3b57f48d83b171a56fa88681d394f848a

````
#include "core/neural_transport.hpp"
#include <thread>
#ifdef _WIN64
#include <d3d12.h>
#endif
using namespace dx11_enhancement::neural;
#ifdef _WIN64
template<class T> ComPtr<T> open(ID3D12Device* device,const std::wstring& name) {
    Handle handle;require(device->OpenSharedHandleByName(name.c_str(),GENERIC_ALL,&handle.value),"DX12 named handle");
    ComPtr<T> result;require(device->OpenSharedHandle(handle.value,IID_PPV_ARGS(&result)),"DX12 shared object");return result;
}
int worker(int argc,wchar_t** argv) {
    if(argc!=7)throw std::runtime_error("worker arguments");
    const std::wstring name=argv[2];const LUID luid{std::stoul(argv[3]),std::stol(argv[4])};
    const UINT frame_count=std::stoul(argv[6]);
    ComPtr<IDXGIFactory4> factory;require(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DX12 factory");
    ComPtr<IDXGIAdapter1> adapter;require(factory->EnumAdapterByLuid(luid,IID_PPV_ARGS(&adapter)),"matching adapter");
    ComPtr<ID3D12Device> device;require(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)),"DX12 device");
    auto input=open<ID3D12Resource>(device.Get(),name+L".input");auto output=open<ID3D12Resource>(device.Get(),name+L".output");
    auto ready=open<ID3D12Fence>(device.Get(),name+L".ready");auto done=open<ID3D12Fence>(device.Get(),name+L".done");
    const auto a=input->GetDesc(),b=output->GetDesc();
    if(a.Width!=b.Width || a.Height!=b.Height || a.Format!=b.Format || a.MipLevels!=1 || a.DepthOrArraySize!=1)
        throw std::runtime_error("shared layout mismatch");
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC q{};
    require(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"DX12 queue");
    ComPtr<ID3D12CommandAllocator> allocator;require(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)),"DX12 allocator");
    ComPtr<ID3D12GraphicsCommandList> commands;require(device->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)),"DX12 list");
    require(commands->Close(),"initial close");
    Handle event(CreateEventW(nullptr,FALSE,FALSE,nullptr));
    Handle parent(OpenProcess(SYNCHRONIZE,FALSE,std::stoul(argv[5])));
    Handle stop(OpenEventW(SYNCHRONIZE,FALSE,(name+L".stop").c_str()));
    if(!event.value || !parent.value || !stop.value)throw std::runtime_error("worker lifetime handles");
    for(UINT64 frame=1;frame<=frame_count;++frame){
        require(ready->SetEventOnCompletion(frame,event.value),"wait input");
        const HANDLE handles[]={parent.value,stop.value,event.value};
        const auto waited=WaitForMultipleObjects(3,handles,FALSE,10000);
        if(waited==WAIT_OBJECT_0 || waited==WAIT_OBJECT_0+1)return 0;
        if(waited!=WAIT_OBJECT_0+2 || ready->GetCompletedValue()<frame || ready->GetCompletedValue()==UINT64_MAX)
            throw std::runtime_error("input deadline/device failure");
        require(allocator->Reset(),"reset allocator");require(commands->Reset(allocator.Get(),nullptr),"reset commands");
        D3D12_RESOURCE_BARRIER barriers[2]{};
        for(auto& item:barriers){item.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;item.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;item.Transition.StateBefore=D3D12_RESOURCE_STATE_COMMON;}
        barriers[0].Transition.pResource=input.Get();barriers[0].Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[1].Transition.pResource=output.Get();barriers[1].Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_DEST;
        commands->ResourceBarrier(2,barriers);commands->CopyResource(output.Get(),input.Get());
        for(auto& item:barriers)std::swap(item.Transition.StateBefore,item.Transition.StateAfter);
        commands->ResourceBarrier(2,barriers);require(commands->Close(),"close copy");
        ID3D12CommandList* lists[]={commands.Get()};queue->ExecuteCommandLists(1,lists);
        require(queue->Signal(done.Get(),frame),"output signal");
        // Allocator reuse and destruction require completion even if the parent exits.
        require(done->SetEventOnCompletion(frame,event.value),"worker completion");
        if(WaitForSingleObject(event.value,10000)!=WAIT_OBJECT_0)throw std::runtime_error("copy completion deadline");
        require(device->GetDeviceRemovedReason(),"worker device health");
    }
    return 0;
}
#else
int host(int argc,wchar_t** argv) {
    if(argc!=2)throw std::runtime_error("usage: x86 neural_transport_test <x64 worker>");
    Device d;
    constexpr UINT width=193,height=127,frames=24;
    unsigned index=0;
    for(const auto format:{DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R16G16_FLOAT}){
        const UINT bytes=format==DXGI_FORMAT_R16G16B16A16_FLOAT?8:4;
        const std::wstring name=L"Local\\DX11NeuralTransfer-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(index++);
        Texture11 input,output;input.create(d.device.Get(),width,height,format,name+L".input");output.create(d.device.Get(),width,height,format,name+L".output");
        Fence11 ready,done;ready.create(d.device.Get(),name+L".ready");done.create(d.device.Get(),name+L".done");
        Handle stop(CreateEventW(nullptr,TRUE,FALSE,(name+L".stop").c_str()));if(!stop.value)throw std::runtime_error("stop event");
        Child child;std::wstring command=L"\""+std::wstring(argv[1])+L"\" --worker "+name+L" "+std::to_wstring(d.luid.LowPart)+L" "+std::to_wstring(d.luid.HighPart)+L" "+std::to_wstring(GetCurrentProcessId())+L" "+std::to_wstring(frames+1);
        STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;
        require(CreateProcessW(argv[1],command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child.process)?S_OK:E_FAIL,"start worker");
        D3D11_TEXTURE2D_DESC desc{};output.image->GetDesc(&desc);desc.MiscFlags=desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> readback;require(d.device->CreateTexture2D(&desc,nullptr,&readback),"test readback");
        std::vector<unsigned char> expected(size_t(width)*height*bytes);
        for(UINT frame=1;frame<=frames;++frame){
            for(size_t i=0;i<expected.size();++i)expected[i]=static_cast<unsigned char>((i*17+frame*31+(i>>8))&255);
            d.context->UpdateSubresource(input.image.Get(),0,nullptr,expected.data(),width*bytes,0);
            ready.signal(d.context.Get(),frame);done.wait(frame,child.process.hProcess,5000);
            d.context->CopyResource(readback.Get(),output.image.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
            require(d.context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped),"host readback");
            bool equal=true;for(UINT y=0;y<height;++y)equal&=std::memcmp(static_cast<char*>(mapped.pData)+size_t(y)*mapped.RowPitch,expected.data()+size_t(y)*width*bytes,size_t(width)*bytes)==0;
            d.context->Unmap(readback.Get(),0);if(!equal)throw std::runtime_error("roundtrip bytes differ");
        }
        SetEvent(stop.value);
        if(WaitForSingleObject(child.process.hProcess,5000)!=WAIT_OBJECT_0)throw std::runtime_error("worker stop deadline");
        DWORD exit_code=99;GetExitCodeProcess(child.process.hProcess,&exit_code);if(exit_code)throw std::runtime_error("worker failed");
        bool rejected=false;try{done.wait(frames+1,child.process.hProcess,50);}catch(const std::exception&){rejected=true;}
        if(!rejected)throw std::runtime_error("dead worker output was accepted");
        std::printf("PASS format=%u: %u changing frames exact across x86 DX11 -> x64 DX12 -> x86 DX11; graceful stop and dead-child rejection.\n",unsigned(format),frames);
    }
    require(d.device->GetDeviceRemovedReason(),"host device health");return 0;
}
#endif
int wmain(int argc,wchar_t** argv) {
    try {
#ifdef _WIN64
        return worker(argc,argv);
#else
        return host(argc,argv);
#endif
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}

````

## tests/neural_bridge_test.cpp

SHA-256: 79724e22a783cac454a3eea208b96241e42ff65b91802ac2e84b8928095b87f2

````
#include "core/neural_bridge.hpp"
#include "core/bridge_transport.hpp"
#include <thread>
#include <DirectXPackedVector.h>
#include <algorithm>
#include <cmath>
using namespace dx11_enhancement;
using namespace dx11_enhancement::transport;
template<class T> void read(const std::filesystem::path& path,std::vector<T>& out){
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    if(!f || f.tellg()!=static_cast<std::streamoff>(out.size()*sizeof(T)))throw std::runtime_error("test input length");
    f.seekg(0);f.read(reinterpret_cast<char*>(out.data()),static_cast<std::streamsize>(out.size()*sizeof(T)));if(!f)throw std::runtime_error("test input read");
}
int wmain(int argc,wchar_t** argv){try{
    if((argc!=6 && argc!=7) || sizeof(void*)!=4)throw std::runtime_error("x86 test: helper runtime prepared-input direct-reference cache");
    const std::filesystem::path prepared=argv[3],reference=argv[4];
    UINT w=0,h=0,count=0;float exposure=0;std::ifstream cfg(prepared/L"replay.txt");cfg>>w>>h>>count>>exposure;
    if(!cfg || count!=2)throw std::runtime_error("replay config");
    Device d;
    auto make=[&](DXGI_FORMAT format,UINT binds){D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=h;desc.Format=format;
        desc.MipLevels=desc.ArraySize=1;desc.SampleDesc.Count=1;desc.BindFlags=binds;ComPtr<ID3D11Texture2D> t;
        require(d.device->CreateTexture2D(&desc,nullptr,&t),"test texture");return t;};
    auto color=make(DXGI_FORMAT_R16G16B16A16_FLOAT,D3D11_BIND_SHADER_RESOURCE);
    auto depth=make(DXGI_FORMAT_R32_FLOAT,D3D11_BIND_SHADER_RESOURCE),motion=make(DXGI_FORMAT_R16G16_FLOAT,D3D11_BIND_SHADER_RESOURCE);
    auto output=make(DXGI_FORMAT_R16G16B16A16_FLOAT,D3D11_BIND_RENDER_TARGET);
    ComPtr<ID3D11ShaderResourceView> c,z,m;ComPtr<ID3D11RenderTargetView> rt;
    require(d.device->CreateShaderResourceView(color.Get(),nullptr,&c),"test color view");require(d.device->CreateShaderResourceView(depth.Get(),nullptr,&z),"test depth view");
    require(d.device->CreateShaderResourceView(motion.Get(),nullptr,&m),"test motion view");require(d.device->CreateRenderTargetView(output.Get(),nullptr,&rt),"test output view");
    D3D11_TEXTURE2D_DESC desc{};output->GetDesc(&desc);desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;require(d.device->CreateTexture2D(&desc,nullptr,&staging),"test readback");
    const auto settings=argc==7?NeuralSettings::read(argv[6]):NeuralSettings{};
    NeuralBridge bridge;bridge.start(d.context.Get(),w,h,argv[1],argv[2],argv[5],settings);
    while(!bridge.ready())std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::vector<uint16_t> pixels(size_t(w)*h*4),vectors(size_t(w)*h*2),expected(pixels.size());std::vector<float> depths(size_t(w)*h);
    for(UINT i=0;i<2;++i){
        read(reference/("input"+std::to_string(i)+".rgba16"),pixels);read(reference/("output"+std::to_string(i)+".rgba16"),expected);
        read(prepared/("depth"+std::to_string(i)+".raw"),depths);read(prepared/("motion"+std::to_string(i)+".raw"),vectors);
        d.context->UpdateSubresource(color.Get(),0,nullptr,pixels.data(),w*8,0);d.context->UpdateSubresource(depth.Get(),0,nullptr,depths.data(),w*4,0);d.context->UpdateSubresource(motion.Get(),0,nullptr,vectors.data(),w*4,0);
        D3D11_VIEWPORT vp{7,9,111,113,.1f,.9f};d.context->RSSetViewports(1,&vp);d.context->PSSetShaderResources(0,1,c.GetAddressOf());
        BridgeFrame frame;frame.width=w;frame.height=h;frame.reset=i==0;frame.motion_scale_x=-float(w);frame.motion_scale_y=-float(h);
        bridge.process(d.context.Get(),c.Get(),z.Get(),m.Get(),rt.Get(),frame);
        D3D11_VIEWPORT restored{};UINT n=1;d.context->RSGetViewports(&n,&restored);ComPtr<ID3D11ShaderResourceView> view;d.context->PSGetShaderResources(0,1,&view);
        if(std::memcmp(&vp,&restored,sizeof(vp)) || view!=c)throw std::runtime_error("host graphics state changed");
        d.context->CopyResource(staging.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE mapped{};require(d.context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"readback map");
        std::ofstream result(std::filesystem::path(argv[5])/("output"+std::to_string(i)+".rgba16"),std::ios::binary);
        for(UINT y=0;y<h;++y)result.write(static_cast<const char*>(mapped.pData)+size_t(y)*mapped.RowPitch,w*8);
        uint64_t mismatch=0;for(UINT y=0;y<h;++y){const auto* row=reinterpret_cast<const uint16_t*>(static_cast<const char*>(mapped.pData)+size_t(y)*mapped.RowPitch);
            for(UINT x=0;x<w;++x)for(UINT channel=0;channel<4;++channel){const size_t offset=(size_t(y)*w+x)*4+channel;
                mismatch+=row[x*4+channel]!=(channel==3?pixels[offset]:expected[offset]);}}
        d.context->Unmap(staging.Get(),0);if(mismatch && argc==6)throw std::runtime_error("NR shared output differs from direct reference or alpha changed");
        if(argc==7)std::printf("VARIANT frame%u reference-channel differences=%llu; output saved.\n",i,mismatch);
        else std::printf("PASS frame%u: RGB bit-exact versus direct DX12 NR; alpha/viewport/SRV preserved; wait %.3fms (not game FPS).\n",i,bridge.last_wait_ms());
    }
    // Exercise the two encoded 8-bit backbuffer layouts accepted by the live adapter.
    for(auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM}){
        auto source8=make(format,D3D11_BIND_SHADER_RESOURCE),output8=make(format,D3D11_BIND_RENDER_TARGET);
        ComPtr<ID3D11ShaderResourceView> view8;ComPtr<ID3D11RenderTargetView> target8;
        require(d.device->CreateShaderResourceView(source8.Get(),nullptr,&view8),"SDR source view");
        require(d.device->CreateRenderTargetView(output8.Get(),nullptr,&target8),"SDR target view");
        std::vector<unsigned char> bytes(size_t(w)*h*4);
        for(size_t pixel=0;pixel<size_t(w)*h;++pixel){
            for(size_t channel=0;channel<3;++channel){
                const auto input_channel=format==DXGI_FORMAT_B8G8R8A8_UNORM?2-channel:channel;
                const float value=DirectX::PackedVector::XMConvertHalfToFloat(pixels[pixel*4+input_channel]);
                bytes[pixel*4+channel]=static_cast<unsigned char>(std::lround(std::clamp(value,0.f,1.f)*255));
            }
            bytes[pixel*4+3]=static_cast<unsigned char>(pixel%256);
        }
        d.context->UpdateSubresource(source8.Get(),0,nullptr,bytes.data(),w*4,0);
        BridgeFrame f;f.width=w;f.height=h;f.reset=1;f.motion_scale_x=-float(w);f.motion_scale_y=-float(h);
        bridge.process(d.context.Get(),view8.Get(),z.Get(),m.Get(),target8.Get(),f);
        output8->GetDesc(&desc);desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> read8;require(d.device->CreateTexture2D(&desc,nullptr,&read8),"SDR staging");
        d.context->CopyResource(read8.Get(),output8.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
        require(d.context->Map(read8.Get(),0,D3D11_MAP_READ,0,&mapped),"SDR map");
        uint64_t changed=0,nonzero=0,alpha_errors=0;
        for(UINT y=0;y<h;++y){const auto* row=static_cast<const unsigned char*>(mapped.pData)+size_t(y)*mapped.RowPitch;
            for(UINT x=0;x<w;++x){const size_t offset=(size_t(y)*w+x)*4;
                for(UINT ch=0;ch<3;++ch){changed+=row[x*4+ch]!=bytes[offset+ch];nonzero+=row[x*4+ch]!=0;}
                alpha_errors+=row[x*4+3]!=bytes[offset+3];}}
        d.context->Unmap(read8.Get(),0);
        if(alpha_errors || (settings.intensity!=0 && changed<100) || nonzero<100)throw std::runtime_error("SDR alpha/output check failed");
        std::printf("PASS SDR format%u: nonempty changed RGB; all 256 alpha levels preserved. Not a visual-quality test.\n",unsigned(format));
    }
    bridge.stop();std::puts("PASS: actual x86 host -> x64 DX12 neural helper -> x86 output.");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}

````

## tests/neural_settings_test.cpp

SHA-256: e6d704a7460ed9892f88d9c51520dbf352e45aca7b770c407de75ed2104161db

````
#include "core/neural_settings.hpp"
#include <limits>
#include <cstdio>
int main(){
    using dx11_enhancement::NeuralSettings;
    NeuralSettings s; s.validate();
    for(unsigned i=0;i<8;++i){
        s={};
        switch(i){case 0:s.model=4;break;case 1:s.style=3;break;case 2:s.auto_skin=2;break;
        case 3:s.intensity=std::numeric_limits<float>::quiet_NaN();break;case 4:s.structure=3;break;
        case 5:s.tone=-1;break;case 6:s.skin=-2;break;case 7:s.skin=std::numeric_limits<float>::infinity();break;}
        bool rejected=false;try{s.validate();}catch(const std::runtime_error&){rejected=true;}
        if(!rejected)return 1;
    }
    s={};s.skin=-1;s.validate();s.skin=2;s.intensity=2;s.structure=2;s.tone=2;s.validate();
    if(s==NeuralSettings{})return 2;
    std::puts("PASS: neural invalid controls rejected; supported boundaries accepted; change comparison works.");
}

````

## scripts/install-neural-trial.ps1

SHA-256: fb3462b93af85cf05e126db5b08e1055febab5dfae1550a591492809447577c1

````
param([Parameter(Mandatory)][string]$GameDirectory, [switch]$ValidateOnly)
$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot -Parent
$gameRoot=(Resolve-Path -LiteralPath $GameDirectory).Path
if(Get-Process -Name AI -ErrorAction SilentlyContinue){throw 'Close Alien: Isolation before installing.'}
$helper=Join-Path $projectRoot 'build/x64/RelWithDebInfo/neural_helper.exe'
$runtime=Join-Path $projectRoot 'local/neural-runtime-310.8.0'
$dll=Join-Path $runtime 'nvngx_dlssnr.dll'
$shader=Join-Path $projectRoot 'shaders/DX11_Neural.fx'
$addon=Join-Path $projectRoot 'build/Win32/RelWithDebInfo/dx11_diagnostic.addon32'
$destination=Join-Path $gameRoot 'reshade-shaders/Shaders/DX11_Neural.fx'
$config=Join-Path $gameRoot 'dx11-neural-paths.txt'
$manifest=Join-Path $projectRoot 'local/neural-trial-deployment.json'
foreach($file in @($helper,$dll,$shader,$addon,(Join-Path $gameRoot 'dx11-dlaa-paths.txt'))){
    if(!(Test-Path -LiteralPath $file)){throw "Required file missing: $file"}
}
$signature=Get-AuthenticodeSignature -LiteralPath $dll
if($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'NVIDIA'){
    throw 'NVIDIA runtime signature verification failed.'
}
if((Get-FileHash -LiteralPath $dll).Hash -ne 'E16BCF15E16E13F527491CDF7845B2FE6521A738D8F7C9C721866A8496E1FC8E'){
    throw 'Neural runtime differs from the tested version.'
}
$previous=$null
if(Test-Path -LiteralPath $manifest){$previous=Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json}
foreach($item in @(@($destination,'ShaderPath','ShaderSHA256'),@($config,'ConfigPath','ConfigSHA256'))){
    if(Test-Path -LiteralPath $item[0]){
        if(!$previous -or $previous.($item[1]) -ne $item[0] -or (Get-FileHash -LiteralPath $item[0]).Hash -ne $previous.($item[2])){
            throw "Existing unowned or changed file: $($item[0])"
        }
    }
}
$installed=Join-Path $gameRoot 'dx11_diagnostic.addon32'
$addonManifest=Join-Path $projectRoot 'local/deployment.json'
$old=Get-Content -LiteralPath $addonManifest -Raw | ConvertFrom-Json
if($old.InstalledFile -ne $installed -or (Get-FileHash -LiteralPath $installed).Hash -ne $old.SHA256){throw 'Installed addon differs from ownership record.'}
$ini=Join-Path $gameRoot 'ReShade.ini'
$iniHash=(Get-FileHash -LiteralPath $ini).Hash
$text=[IO.File]::ReadAllText($ini)
$general=[regex]::Match($text,'(?ms)^\[GENERAL\]\r?\n(?<body>.*?)(?=^\[|\z)')
if(!$general.Success){throw 'Missing ReShade GENERAL section.'}
$body=$general.Groups['body'].Value
$definitions=[regex]::Match($body,'(?m)^PreprocessorDefinitions=(?<value>[^\r\n]*)')
$values=if($definitions.Success){$definitions.Groups['value'].Value}else{''}
foreach($entry in @('MODEL=0','STYLE=0','INTENSITY=1.0','STRUCTURE=1.0','TONE=0.0','SKIN=1.0','AUTO_SKIN=1')){
    $key='DX11_NR_'+$entry.Split('=')[0]
    if($values -notmatch ('(^|,)\s*'+[regex]::Escape($key)+'=')){
        if($values.Length){$values+=','}
        $values+='DX11_NR_'+$entry
    }
}
if($definitions.Success){$body=$body.Remove($definitions.Index,$definitions.Length).Insert($definitions.Index,'PreprocessorDefinitions='+$values)}
else{$body="PreprocessorDefinitions=$values`r`n"+$body}
$edited=$text.Remove($general.Groups['body'].Index,$general.Groups['body'].Length).Insert($general.Groups['body'].Index,$body)
if($ValidateOnly){Write-Output 'Neural trial preflight passed; no game files changed.';return}
$backup=Join-Path $projectRoot ('local/neural-install-backup-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $backup | Out-Null
foreach($file in @($ini,$destination,$config,$manifest,(Join-Path $gameRoot 'ReShade.log'))){
    if(Test-Path -LiteralPath $file){Copy-Item -LiteralPath $file -Destination $backup}
}
if((Get-FileHash -LiteralPath $ini).Hash -ne $iniHash){throw 'ReShade settings changed during installation.'}
& (Join-Path $PSScriptRoot 'install-diagnostic.ps1') -GameDirectory $gameRoot -Update
$cache=Join-Path $projectRoot 'local/live-neural'
[IO.File]::WriteAllText($config,"$helper`n$runtime`n$cache`n",[Text.UTF8Encoding]::new($false))
Copy-Item -LiteralPath $shader -Destination $destination -Force
if((Get-FileHash -LiteralPath $destination).Hash -ne (Get-FileHash -LiteralPath $shader).Hash){throw 'Neural control copy failed verification.'}
if($edited -ne $text){[IO.File]::WriteAllText($ini,$edited,[Text.UTF8Encoding]::new($false))}
[ordered]@{
    IniPath=$ini;IniSHA256=(Get-FileHash -LiteralPath $ini).Hash
    ConfigPath=$config;ConfigSHA256=(Get-FileHash -LiteralPath $config).Hash
    ShaderPath=$destination;ShaderSHA256=(Get-FileHash -LiteralPath $destination).Hash
    HelperPath=$helper;HelperSHA256=(Get-FileHash -LiteralPath $helper).Hash
    RuntimePath=$dll;RuntimeSHA256=(Get-FileHash -LiteralPath $dll).Hash
    Backup=$backup;InstalledAt=(Get-Date).ToString('o')
    Default='New ReShade technique initially unchecked; subsequent choice follows user preset'
} | ConvertTo-Json | Set-Content -LiteralPath $manifest
Write-Output 'Installed experimental neural checkbox and helper configuration. Existing DLAA helper/settings preserved. No NVIDIA binaries copied into game.'
Write-Output "Prelaunch log/config backup: $backup"

````

## scripts/install-diagnostic.ps1

SHA-256: 47c24650ac10d89d2faa05bf86572c298ce87b6d447fc73cb3b879a27a800950

````
param([Parameter(Mandatory)][string]$GameDirectory, [switch]$Update)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$gameRoot = (Resolve-Path -LiteralPath $GameDirectory).Path
$executable = Join-Path $gameRoot 'AI.exe'
if (!(Test-Path -LiteralPath $executable)) { throw 'The selected folder does not contain AI.exe.' }
if (Get-Process -Name AI -ErrorAction SilentlyContinue) { throw 'Close Alien: Isolation before installing.' }
if (!(Test-Path -LiteralPath (Join-Path $gameRoot 'dxgi.dll'))) { throw 'This diagnostic needs an existing compatible ReShade installation.' }
$stream = [IO.File]::OpenRead($executable)
$reader = [IO.BinaryReader]::new($stream)
try {
    if ($reader.ReadUInt16() -ne 0x5A4D) { throw 'AI.exe is not a PE executable.' }
    $stream.Position = 0x3c
    $peOffset = $reader.ReadInt32()
    $stream.Position = $peOffset
    if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x14c) {
        throw 'Expected a 32-bit x86 AI.exe.'
    }
} finally { $reader.Dispose() }
$source = Join-Path $projectRoot 'build/Win32/RelWithDebInfo/dx11_diagnostic.addon32'
if (!(Test-Path -LiteralPath $source)) { throw 'Run scripts/build.ps1 first.' }
$destination = Join-Path $gameRoot 'dx11_diagnostic.addon32'
if (Test-Path -LiteralPath $destination) {
    if (!$Update) { throw 'The diagnostic already exists. Use -Update to back up and replace a verified installation.' }
    $manifestPath = Join-Path $projectRoot 'local/deployment.json'
    if (!(Test-Path -LiteralPath $manifestPath)) { throw 'No previous deployment record. Refusing to replace an unknown file.' }
    $previous = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($previous.InstalledFile -ne $destination -or (Get-FileHash -LiteralPath $destination).Hash -ne $previous.SHA256) {
        throw 'Installed diagnostic differs from the deployment record. No changes made.'
    }
    $backupDir = Join-Path $projectRoot ('local/backup-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $backupDir | Out-Null
    Copy-Item -LiteralPath $destination -Destination $backupDir
    Copy-Item -LiteralPath $manifestPath -Destination $backupDir
    Write-Output "Previous diagnostic backed up to $backupDir"
    [IO.File]::Copy($source, $destination, $true)
} else {
    [IO.File]::Copy($source, $destination, $false)
}
$sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $sourceHash) {
    throw 'Installed file verification failed.'
}
New-Item -ItemType Directory -Force -Path (Join-Path $projectRoot 'local') | Out-Null
[pscustomobject]@{
    InstalledFile = $destination
    SHA256 = $sourceHash
    GameSHA256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
    InstalledAt = (Get-Date).ToString('o')
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $projectRoot 'local/deployment.json')
Write-Output "Installed and verified $destination. Existing mods and settings were not changed."

````

## CMakeLists.txt

SHA-256: 5c4d17adf7f674dd6a0e07492f9422872039e6e43e3ac590faac36fa16693787

````
cmake_minimum_required(VERSION 3.25)
project(DX11Enhancement VERSION 0.11.1 LANGUAGES CXX)
if(NOT WIN32 OR NOT MSVC)
  message(FATAL_ERROR "Build with Visual Studio on Windows.")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_library(dx11_diagnostic SHARED src/addon.cpp src/core/diagnostics.cpp src/adapters/alien_isolation.cpp)
target_sources(dx11_diagnostic PRIVATE src/core/depth_addon.cpp src/core/depth_preview.cpp src/core/raw_snapshot.cpp src/core/live_bridge.cpp src/core/neural_bridge.cpp src/adapters/alien_neural.cpp)
target_include_directories(dx11_diagnostic PRIVATE src)
target_include_directories(dx11_diagnostic SYSTEM PRIVATE third_party/reshade/include)
target_include_directories(dx11_diagnostic SYSTEM PRIVATE third_party/imgui)
target_compile_definitions(dx11_diagnostic PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
option(DX11_DIAGNOSTIC_UI "Show developer capture and preview panels" OFF)
if(DX11_DIAGNOSTIC_UI)
  target_compile_definitions(dx11_diagnostic PRIVATE DX11_DIAGNOSTIC_UI)
endif()
target_compile_options(dx11_diagnostic PRIVATE /W4 /WX /permissive-)
target_link_libraries(dx11_diagnostic PRIVATE d3d11 dxgi d3dcompiler)
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
  set_target_properties(dx11_diagnostic PROPERTIES PREFIX "" SUFFIX ".addon32")
else()
  set_target_properties(dx11_diagnostic PROPERTIES PREFIX "" SUFFIX ".addon64")
endif()
option(BUILD_SMOKE_HOST "Build a small DX11 program for integration testing" ON)
set(NGX_SDK_ROOT "" CACHE PATH "Optional local NVIDIA DLSS SDK, not redistributed")
if(CMAKE_SIZEOF_VOID_P EQUAL 4 OR NGX_SDK_ROOT)
  add_executable(bridge_dlaa_probe tests/bridge_dlaa_probe.cpp)
  target_compile_definitions(bridge_dlaa_probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(bridge_dlaa_probe PRIVATE /W4 /WX /permissive-)
  target_link_libraries(bridge_dlaa_probe PRIVATE d3d11 dxgi)
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_include_directories(bridge_dlaa_probe SYSTEM PRIVATE "${NGX_SDK_ROOT}/include")
    target_link_libraries(bridge_dlaa_probe PRIVATE "${NGX_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib")
  endif()
endif()
if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND NGX_SDK_ROOT)
  add_executable(neural_helper src/core/neural_helper.cpp)
  target_include_directories(neural_helper SYSTEM PRIVATE "${NGX_SDK_ROOT}/include")
  target_compile_definitions(neural_helper PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(neural_helper PRIVATE /W4 /WX /permissive-)
  target_link_libraries(neural_helper PRIVATE d3d11 d3d12 dxgi "${NGX_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib")
  add_executable(neural_runtime_probe tests/neural_runtime_probe.cpp)
  target_include_directories(neural_runtime_probe SYSTEM PRIVATE "${NGX_SDK_ROOT}/include")
  target_compile_definitions(neural_runtime_probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(neural_runtime_probe PRIVATE /W4 /WX /permissive-)
  target_link_libraries(neural_runtime_probe PRIVATE d3d11 d3d12 dxgi "${NGX_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib")
  add_executable(dlaa_helper src/core/dlaa_helper.cpp)
  target_include_directories(dlaa_helper SYSTEM PRIVATE "${NGX_SDK_ROOT}/include")
  target_compile_definitions(dlaa_helper PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(dlaa_helper PRIVATE /W4 /WX /permissive-)
  target_link_libraries(dlaa_helper PRIVATE d3d11 dxgi "${NGX_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib")
  add_executable(ngx_runtime_probe tests/ngx_runtime_probe.cpp)
  target_include_directories(ngx_runtime_probe SYSTEM PRIVATE "${NGX_SDK_ROOT}/include")
  target_compile_definitions(ngx_runtime_probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(ngx_runtime_probe PRIVATE /W4 /WX /permissive-)
  target_link_libraries(ngx_runtime_probe PRIVATE d3d11 dxgi "${NGX_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib")
endif()
if(BUILD_SMOKE_HOST)
  enable_testing()
  add_executable(neural_settings_test tests/neural_settings_test.cpp)
  target_include_directories(neural_settings_test PRIVATE src)
  target_compile_options(neural_settings_test PRIVATE /W4 /WX /permissive-)
  add_test(NAME neural_settings COMMAND neural_settings_test)
  add_executable(neural_bridge_test tests/neural_bridge_test.cpp src/core/neural_bridge.cpp)
  target_include_directories(neural_bridge_test PRIVATE src)
  target_compile_definitions(neural_bridge_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(neural_bridge_test PRIVATE /W4 /WX /permissive-)
  target_link_libraries(neural_bridge_test PRIVATE d3d11 dxgi d3dcompiler)
  add_executable(neural_transport_test tests/neural_transport_test.cpp)
  target_include_directories(neural_transport_test PRIVATE src)
  target_compile_definitions(neural_transport_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(neural_transport_test PRIVATE /W4 /WX /permissive-)
  target_link_libraries(neural_transport_test PRIVATE d3d11 d3d12 dxgi)
  add_executable(dlaa_control_test tests/dlaa_control_test.cpp)
  target_include_directories(dlaa_control_test PRIVATE src)
  target_compile_options(dlaa_control_test PRIVATE /W4 /WX /permissive-)
  add_test(NAME dlaa_control COMMAND dlaa_control_test)
  add_executable(projection_verifier_test tests/projection_verifier_test.cpp)
  target_include_directories(projection_verifier_test PRIVATE src)
  target_compile_options(projection_verifier_test PRIVATE /W4 /WX /permissive-)
  add_test(NAME projection_verifier COMMAND projection_verifier_test)
  add_executable(live_bridge_test tests/live_bridge_test.cpp src/core/live_bridge.cpp)
  target_include_directories(live_bridge_test PRIVATE src)
  target_compile_definitions(live_bridge_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(live_bridge_test PRIVATE /W4 /WX /permissive-)
  target_link_libraries(live_bridge_test PRIVATE d3d11 dxgi d3dcompiler)
  add_executable(bridge_transport_probe tests/bridge_transport_probe.cpp)
  target_compile_definitions(bridge_transport_probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(bridge_transport_probe PRIVATE /W4 /WX /permissive-)
  target_link_libraries(bridge_transport_probe PRIVATE d3d11 dxgi d3dcompiler)
  add_executable(capture_trigger_test tests/capture_trigger_test.cpp)
  target_include_directories(capture_trigger_test PRIVATE src)
  target_compile_options(capture_trigger_test PRIVATE /W4 /WX /permissive-)
  add_test(NAME capture_trigger COMMAND capture_trigger_test)
  add_executable(frame_history_test tests/frame_history_test.cpp)
  target_include_directories(frame_history_test PRIVATE src)
  target_compile_options(frame_history_test PRIVATE /W4 /WX /permissive-)
  add_test(NAME frame_history COMMAND frame_history_test)
  add_executable(depth_preview_test tests/depth_preview_test.cpp src/core/depth_preview.cpp src/adapters/alien_isolation.cpp)
  target_include_directories(depth_preview_test PRIVATE src)
  target_compile_definitions(depth_preview_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(depth_preview_test PRIVATE /W4 /WX /permissive-)
  target_link_libraries(depth_preview_test PRIVATE d3d11 dxgi d3dcompiler)
  add_test(NAME depth_preview COMMAND depth_preview_test)
  add_executable(motion_preview_test tests/motion_preview_test.cpp src/core/depth_preview.cpp src/adapters/alien_isolation.cpp)
  target_include_directories(motion_preview_test PRIVATE src)
  target_compile_definitions(motion_preview_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(motion_preview_test PRIVATE /W4 /WX /permissive-)
  target_link_libraries(motion_preview_test PRIVATE d3d11 dxgi d3dcompiler)
  add_test(NAME motion_preview COMMAND motion_preview_test)
  add_executable(raw_snapshot_test tests/raw_snapshot_test.cpp src/core/raw_snapshot.cpp src/adapters/alien_isolation.cpp)
  target_include_directories(raw_snapshot_test PRIVATE src)
  target_compile_definitions(raw_snapshot_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(raw_snapshot_test PRIVATE /W4 /WX /permissive-)
  target_link_libraries(raw_snapshot_test PRIVATE d3d11 dxgi)
  add_test(NAME raw_snapshot COMMAND raw_snapshot_test)
  add_executable(dx11_smoke_host tests/smoke_host.cpp)
  target_include_directories(dx11_smoke_host SYSTEM PRIVATE third_party/reshade/include)
  target_compile_definitions(dx11_smoke_host PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(dx11_smoke_host PRIVATE /W4 /WX /permissive-)
  target_link_libraries(dx11_smoke_host PRIVATE d3d11 dxgi user32)
endif()






````

## NEURAL-CONTROLS.md

SHA-256: 1c14cd59e135692ebc8e593c8a506afa2e469e674110766de839cde2f2a1c79f

````
# Neural controls and audit — v0.11.1

Open ReShade with **Home**, keep **DLAA** checked, and enable **Neural rendering
(experimental)**. Open **Edit global preprocessor definitions** to adjust these
settings. Use **Reload** after editing if ReShade has not reloaded automatically.
Only the neural helper restarts; no game restart is required. The existing DLAA
status panel also reports neural status. Uncheck neural to compare with DLAA alone.

| Definition | Default | Meaning |
|---|---:|---|
| `DX11_NR_INTENSITY` | 1.0 | Overall strength, 0–2. Zero exactly returns input RGB in replay tests, but still costs processing time. Uncheck neural to remove that cost. |
| `DX11_NR_STRUCTURE` | 1.0 | Fine detail enhancement, 0–2. Stronger is not necessarily better. |
| `DX11_NR_TONE` | 0.0 | Broader lighting/color adjustment, 0–2. Zero preserves original tone. Try 0.25 for a modest comparison; this is not a proven best setting. |
| `DX11_NR_SKIN` | 1.0 | Skin detail, -1–2. -1 follows structure; zero reduces that contribution. |
| `DX11_NR_AUTO_SKIN` | 1 | Automatic skin detection: 0 off, 1 on. Does NOT protect HUD text. |
| `DX11_NR_STYLE` | 0 | Profiles 0/1/2, all producing different replay results. Community names standard/natural/cinematic are not verified NVIDIA names. |
| `DX11_NR_MODEL` | 0 | Runtime choice 0, requested hints 1/2/3. All four produced identical output here. Actual different model selection is unverified; keep 0. |

Change one setting at a time in the same scene. Defaults reproduce the earlier
picture. Restore the table values to undo tuning. ReShade saves your selections.
The separate `DX11_DLAA_PRESET` remains Auto 0, J 10, K 11, L 12, M 13; these are
reconstruction presets, not neural styles or neural model letters.

NVIDIA describes DLSS 5 as generating enhanced final appearance from rendered
color, engine motion and temporal state, with artistic controls. It complements
the renderer; it does not turn this game into a newly ray-traced renderer.
Sources: [NVIDIA research](https://research.nvidia.com/labs/adlr/DLSS5/) and
[NVIDIA artistic controls](https://www.nvidia.com/en-us/geforce/news/dlss-5-3d-guided-neural-rendering/).

The audit added creation-time settings, validation, session settings logs and
helper recreation on changes. Eight automated tests pass. Actual ReShade tests
verify all seven settings and hot reload. Two-frame model replay confirms style,
intensity, structure, tone, skin and automatic skin masking alter output. All
variant outputs are finite and preserve alpha. Model hints remain unverified
after testing both signed and unsigned parameter types. This is not a gameplay
benchmark or proof of long-term temporal quality.

The current final SDR stage includes HUD/text. There is no separate UI texture or
mask. The unused UI-correction hint was removed; automatic skin masking serves a
different purpose. HUD exclusion, material-specific masks and reliable camera-cut
detection need further game integration. Final color versus earlier motion/depth
alignment also needs live testing. A quality setting cannot guarantee those issues
are solved. No live v0.11.1 gameplay result has been reported yet.

Start at defaults, compare tone 0 versus 0.25 and styles 0/1/2, and watch moving
details, text, dark scenes and FPS. Maximum values are not a verified best preset.
This remains an unofficial, experimental integration, not a release-ready mod.

Interface facts were checked against pinned primary community source:
[OptiScaler contract](https://github.com/Dagherbou/OptiScaler_DLSSNR/blob/973761621353b99bee3dc7d4bb27b117fef2644f/OptiScaler/shaders/dlssnr/DlssNr_Common.h),
[OptiScaler controls](https://github.com/Dagherbou/OptiScaler_DLSSNR/blob/973761621353b99bee3dc7d4bb27b117fef2644f/OptiScaler/dlssnr/DlssNr_Menu.cpp),
and [AIO integration](https://github.com/kibblerz/DLSS5-Reshade-AIO/tree/09301f5528e619e8b9ec17c257d167e2985f53b0).
No third-party implementation was copied. These are community interface findings,
not an official NVIDIA SDK guarantee for this DLL.

Evidence: `docs/evidence/2026-09-21-neural-control-audit.json`,
`2026-09-21-neural-controls-smoke.log`, `2026-09-21-v0.11.1-tests.log`.
Private session `settings.txt` records exact parameters beside `runtime.txt`.
See WORKLOG entries 127–128 for actions, failures, deployment and limitations.

````

## NEURAL-RENDERING.md

SHA-256: b0af48d11baaa062495ec15ba1c857d5efcd5d8880f4e14b224f5e40a409aeb5

````
# DLSS 5 feasibility — 21 September 2026

The isolated and cross-architecture neural-rendering tests work on this RTX5070Ti.
v0.11.1 is installed with an initially unchecked experimental ReShade technique.
Live gameplay validation is pending. The existing DLAA helper and settings remain
unchanged; the addon now also supports the separate neural helper.

## Runtime and provenance

Downloaded `nvngx_dlssnr.dll` version310.8.0.0 from the community mirror used by
DLSS5-Autopilot. This is not a download hosted by NVIDIA. The unmodified Windows
binary has a **Valid NVIDIA Corporation Authenticode signature**; the archive hash
matches GitHub's published release digest. Signature validation establishes
publisher/integrity, not redistribution permission or game compatibility.

- [Source release](https://github.com/RankFTW/rhi-repo/releases/tag/dlssnr-310.8.0)
- Archive SHA256: `388C0A7912E15EC911B9C9E11A692142B11FE387DDF2B637D8C358138FFFB3AC`
- DLL SHA256: `E16BCF15E16E13F527491CDF7845B2FE6521A738D8F7C9C721866A8496E1FC8E`
- DLL architecture: x64 (PE machine0x8664), size165840496bytes.
- Private location: `local/neural-runtime-310.8.0/`; provenance in `source.json`.
- Evidence: `docs/evidence/2026-09-21-neural-runtime-identity.json` and
  `2026-09-21-neural-runtime-exports.log`.

The SDK used for compilation remains the existing pinned NVIDIA SDK. Its public
headers do not declare a complete neural-rendering contract. Feature18 and the
`DLSSNR.*` parameter names were researched in community source. They are treated
as an experimental, version-specific interface. Our probe uses the normal NGX SDK
and our existing custom project GUID. No patched runtime, other game's application
identity, caller-identity shim, ReShade interposer or downloaded installer is used.

## What passed

`neural_runtime_probe` is a separate x64 command-line test, not a game addon.
Both DX11 and DX12 can create ordinary DLAA through this test (positive controls).
With the signed neural runtime, DX11 reports `DLSSNR.Available=0` and creation
returns0xBAD00007. DX12 reports availability1 and successfully creates feature18.
Exports bearing a DX11 name did not establish functional DX11 support.

The DX12 test then processes the two previously captured1920x1200 frames from
`local/replay-88730/prepared`. Inputs: RGBA16F color, R32F depth, RG16F engine motion;
output RGBA16F. It supplies per-resource rectangles, non-inverted depth, motion
scale(-width,-height), and a reset on the first frame only. Defaults requested:
model0, style0, intensity1, local structure1, local tone0, skin structure1,
automatic mask1, UI correction1. These settings are not a recommended final look.

Captured linear color is converted through Reinhard tone mapping and sRGB encoding
for this experiment. **That is an approximation, not Alien's final color pipeline.**
The captured input also predates temporal AA. These frames demonstrate API/image
feasibility, not a finished appearance or correct production temporal contract.

| Frame | Invalid RGB values | Unwritten sentinel values | Mean absolute RGB change | GPU command bracket |
| --- | --- | --- | --- | --- |
| 0 | 0 | 0 | 0.0128057 | 4.946ms |
| 1 | 0 | 0 | 0.0127044 | 4.743ms |

GPU timestamps bracket the neural evaluation commands; upload/readback is outside
the bracket. CPU evaluation calls took1.668ms and0.364ms. These are two samples,
including startup, not a steady-state benchmark or total in-game latency. Do not
extrapolate an Alien frame rate or a cost at the user's larger display resolution.
Both outputs are finite/nontrivial and differ from their input. Visual inspection
shows subtle surface/shading changes; this is not evidence of a dramatic remaster.
Text fidelity and motion stability need longer, properly placed live tests.

Raw private results: `local/neural-image-test`. Reviewable PNG comparison and
full-size images: sibling output folder `../neural-rendering-preview/`.
Evidence: `2026-09-21-neural-create-probes.log`, `2026-09-21-neural-image-probe.log`.

## Reproducing the isolated test

Build the x64 `neural_runtime_probe` target with the already configured private
`NGX_SDK_ROOT`. Command from the project root, with paths resolved to absolute:

```powershell
./build/x64/RelWithDebInfo/neural_runtime_probe.exe dx12 `
  "$PWD/local/neural-runtime-310.8.0" "$PWD/local/neural-image-test" nr `
  "$PWD/local/replay-88730/prepared"
```

Omit the final input directory for creation-only testing. Use `dx11` to repeat the
unavailable path. Use `dlaa` with the normal SDK runtime directory for the positive
control. No new game capture, game restart or settings change is needed.

## Experimental live trial (v0.11.0)

`core/neural_transport.hpp`, `neural_bridge.cpp`, and `neural_helper.cpp` implement
NT-shared DX11/DX12 textures and fences on the same adapter. Seventy-two changing
texture roundtrips were byte-exact. Actual NR bridge replay matches the standalone
DX12 RGB result bit-for-bit on two saved1920x1200 frames, while preserving input
alpha and tested viewport/SRV state. RGBA8/BGRA8 output tests also pass, including
all256 alpha levels. This is transfer correctness, not proof of motion quality.
CPU output waits in the repeated two-frame test:8.738/7.528ms; not GPU/gameFPS.
Evidence: neural-transport-test, neural-bridge-test, neural-bridge-sdr-test logs.

The Alien adapter runs immediately before ReShade effects on the final SDR
UNORM picture, with this-frame successful DLAA, raw depth and engine motion.
Game HUD/text are included. No explicit UI mask is supplied. Automatic skin masking
does not protect the HUD. UI correction is disabled because separate UI inputs
are absent. See [controls and audit](NEURAL-CONTROLS.md) for the seven new settings.
Only single-mip/array/sample RGBA8/BGRA8 views are accepted; matching image extents
are mandatory. No sRGB decode/encode is added. First/gap/>250ms/display resets
clear history. Camera cuts within continuous gameplay and post-processing versus
motion/depth alignment remain unverified. No assertion of a correct scene-only
stage. Model0/style0, intensity1, local structure1, local tone0, skin structure1.

Launch game, enable DLAA in ReShade Home, then **Neural rendering (experimental)**.
Check the existing DLAA status panel for neural active/starting/error. Uncheck
neural to return to DLAA alone without restart. Failures latch only neural off;
uncheck/check to retry. Existing global effects switch disables both. Initial
install does not modify the preset; later checkbox choice is saved by ReShade.
Helper startup polls without waiting on each frame; output deadline100ms, startup
15seconds. A failed frame can hitch before fallback. Higher-resolution performance,
long temporal behavior, camera cuts, HUD/text and visual quality need live testing.
No new captures are requested for the initial trial. Report active status, visible
changes, ghosting, text quality, and frame rate with the checkbox off versus on.

Install script `scripts/install-neural-trial.ps1` checks runtime hash/signature and
owned file hashes, backs up previous addon/config/log, leaves SDK binaries private,
and adds only own neural control/config plus updated addon. Current source and
matching known-good0.10 addon/helper checkpoint retained under ignored local/.
Unchecking neural is immediate rollback; binary rollback while game is closed can
restore the0.10 addon and deployment record from local/backup-e2a4bb6fdade4ce9b3fc459ae8f2d2e4.
DLAA helper was not rebuilt or changed in this milestone.

## Research references

Original code for the probe is in `tests/neural_runtime_probe.cpp` and
`tests/neural_image_probe.hpp`. Community sources were inspected for interface
facts; their implementations have not been copied into our runtime:

- [DLSS5-Autopilot](https://github.com/Kizzuwatnaa/DLSS5-Autopilot/tree/669486769d03e83b2d6c0edcaa7309486ed711f8): runtime-source discovery.
- [DLSS5-Feeder](https://github.com/jlrouzies-fr/DLSS5-Feeder/tree/cb24507e93a93b5a8d2740e82717af6adaecf190):32-bit host/helper design research.
- [OptiScaler_DLSSNR](https://github.com/Dagherbou/OptiScaler_DLSSNR/tree/973761621353b99bee3dc7d4bb27b117fef2644f): feature18 creation/evaluation parameter research.
- [DLSS5-Reshade-AIO](https://github.com/kibblerz/DLSS5-Reshade-AIO/tree/09301f5528e619e8b9ec17c257d167e2985f53b0): independent lab/interface research; its caller-identity bridge was not used.
- [NVIDIA research](https://research.nvidia.com/labs/adlr/DLSS5/): model overview; distinct from the undocumented community integration contract.

Downloaded binaries and captured game imagery stay out of the public source
repository. A public distribution needs its own verified runtime sourcing and
applicable licensing review; the signed DLL is not bundled by this project.

````

## docs/evidence/2026-09-21-neural-control-audit.json

SHA-256: ce9328a75500879c169a6f59cfec1e7b657ed880737893deab061bad0ad32a2a

````
{
  "baseline": [
    {
      "changed_rgb": 0,
      "mean_abs_rgb": 0.0,
      "input_rgb_equal": false
    },
    {
      "changed_rgb": 0,
      "mean_abs_rgb": 0.0,
      "input_rgb_equal": false
    }
  ],
  "int-baseline": [
    {
      "changed_rgb": 0,
      "mean_abs_rgb": 0.0,
      "input_rgb_equal": false
    },
    {
      "changed_rgb": 0,
      "mean_abs_rgb": 0.0,
      "input_rgb_equal": false
    }
  ],
  "int-intensity": [
    {
      "changed_rgb": 6867992,
      "mean_abs_rgb": 0.012805727310478687,
      "input_rgb_equal": true
    },
    {
      "changed_rgb": 6863925,
      "mean_abs_rgb": 0.012704388238489628,
      "input_rgb_equal": true
    }
  ],
  "int-model1": [
    {
      "changed_rgb": 0,
      "mean_abs_rgb": 0.0,
      "input_rgb_equal": false
    },
    {
      "changed_rgb": 0,
      "mean_abs_rgb": 0.0,
      "input_rgb_equal": false
    }
  ],
  "int-model2": [
    {
      "changed_rgb": 0,
      "mean_abs_rgb": 0.0,
      "input_rgb_equal": false
    },
    {
      "changed_rgb": 0,
      "mean_abs_rgb": 0.0,
      "input_rgb_equal": false
    }
  ],
  "int-model3": [
    {
      "changed_rgb": 0,
      "mean_abs_rgb": 0.0,
      "input_rgb_equal": false
    },
    {
      "changed_rgb": 0,
      "mean_abs_rgb": 0.0,
      "input_rgb_equal": false
    }
  ],
  "intensity": [
    {
      "changed_rgb": 6867992,
      "mean_abs_rgb": 0.012805727310478687,
      "input_rgb_equal": true
    },
    {
      "changed_rgb": 6863925,
      "mean_abs_rgb": 0.012704388238489628,
      "input_rgb_equal": true
    }
  ],
  "maskoff": [
    {
      "changed_rgb": 6651036,
      "mean_abs_rgb": 0.001614779350347817,
      "input_rgb_equal": false
    },
    {
      "changed_rgb": 6614895,
      "mean_abs_rgb": 0.001438654726371169,
      "input_rgb_equal": false
    }
  ],
  "model1": [
    {
      "changed_rgb": 0,
      "mean_abs_rgb": 0.0,
      "input_rgb_equal": false
    },
    {
      "changed_rgb": 0,
      "mean_abs_rgb": 0.0,
      "input_rgb_equal": false
    }
  ],
  "model2": [
    {
      "changed_rgb": 0,
      "mean_abs_rgb": 0.0,
      "input_rgb_equal": false
    },
    {
      "changed_rgb": 0,
      "mean_abs_rgb": 0.0,
      "input_rgb_equal": false
    }
  ],
  "model3": [
    {
      "changed_rgb": 0,
      "mean_abs_rgb": 0.0,
      "input_rgb_equal": false
    },
    {
      "changed_rgb": 0,
      "mean_abs_rgb": 0.0,
      "input_rgb_equal": false
    }
  ],
  "skin": [
    {
      "changed_rgb": 0,
      "mean_abs_rgb": 0.0,
      "input_rgb_equal": false
    },
    {
      "changed_rgb": 0,
      "mean_abs_rgb": 0.0,
      "input_rgb_equal": false
    }
  ],
  "skin0": [
    {
      "changed_rgb": 6645639,
      "mean_abs_rgb": 0.0015884314198046923,
      "input_rgb_equal": false
    },
    {
      "changed_rgb": 6604786,
      "mean_abs_rgb": 0.0014141469728201628,
      "input_rgb_equal": false
    }
  ],
  "structure": [
    {
      "changed_rgb": 6870151,
      "mean_abs_rgb": 0.012950482778251171,
      "input_rgb_equal": false
    },
    {
      "changed_rgb": 6865581,
      "mean_abs_rgb": 0.012844422832131386,
      "input_rgb_equal": false
    }
  ],
  "style1": [
    {
      "changed_rgb": 6894978,
      "mean_abs_rgb": 0.0234079547226429,
      "input_rgb_equal": false
    },
    {
      "changed_rgb": 6893484,
      "mean_abs_rgb": 0.02302554063498974,
      "input_rgb_equal": false
    }
  ],
  "style2": [
    {
      "changed_rgb": 6866027,
      "mean_abs_rgb": 0.009838975965976715,
      "input_rgb_equal": false
    },
    {
      "changed_rgb": 6864121,
      "mean_abs_rgb": 0.009606361389160156,
      "input_rgb_equal": false
    }
  ],
  "tone": [
    {
      "changed_rgb": 6870677,
      "mean_abs_rgb": 0.008915901184082031,
      "input_rgb_equal": false
    },
    {
      "changed_rgb": 6872089,
      "mean_abs_rgb": 0.009279443882405758,
      "input_rgb_equal": false
    }
  ],
  "upper": [
    {
      "changed_rgb": 6883882,
      "mean_abs_rgb": 0.013157689943909645,
      "input_rgb_equal": false
    },
    {
      "changed_rgb": 6881198,
      "mean_abs_rgb": 0.013420713134109974,
      "input_rgb_equal": false
    }
  ]
}
````

## docs/evidence/2026-09-21-neural-controls-smoke.log

SHA-256: 4c440d4ecccf9dd3a72c3ff433c17441dcf12558e6f5a71985d1ee666b958d4d

````
PASS: DLAA and neural control FX compiled/toggled; preset=0; technique and global toggles checked.
PASS: all seven neural definitions hot-reloaded into typed addon metadata.
PASS: global definition hot reload updated compiled preset to 11.
PASS: DX11 device, depth creation, 121 presents, resize and teardown. Verify DX11Diag log markers.
PASS: add-on registration, callbacks, resize and cleanup confirmed in C:\Users\cortf\Documents\Codex\2026-09-21\we-re-starting-a-reusable-dx11\outputs\dx11-enhancement\local\smoke-e5d2949973b145d18a164435c9d5e893/ReShade.log

````

## docs/evidence/2026-09-21-v0.11.1-tests.log

SHA-256: 2b55cc5b24de0acb19ed73b65a0b8055f53a04d4818bb449550548a9a481a6a9

````
Test project C:/Users/cortf/Documents/Codex/2026-09-21/we-re-starting-a-reusable-dx11/outputs/dx11-enhancement/build/Win32
    Start 1: neural_settings
1/8 Test #1: neural_settings ..................   Passed    0.08 sec
    Start 2: dlaa_control
2/8 Test #2: dlaa_control .....................   Passed    0.01 sec
    Start 3: projection_verifier
3/8 Test #3: projection_verifier ..............   Passed    0.01 sec
    Start 4: capture_trigger
4/8 Test #4: capture_trigger ..................   Passed    0.01 sec
    Start 5: frame_history
5/8 Test #5: frame_history ....................   Passed    0.01 sec
    Start 6: depth_preview
6/8 Test #6: depth_preview ....................   Passed    0.20 sec
    Start 7: motion_preview
7/8 Test #7: motion_preview ...................   Passed    0.19 sec
    Start 8: raw_snapshot
8/8 Test #8: raw_snapshot .....................   Passed    0.19 sec

100% tests passed, 0 tests failed out of 8

Total Test time (real) =   0.78 sec

````
