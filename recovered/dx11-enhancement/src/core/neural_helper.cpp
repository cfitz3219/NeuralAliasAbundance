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
    // D3D12 node selection is infrastructure; image/quality options below are NR-specific.
    p->Set("CreationNodeMask",1u);p->Set("VisibilityNodeMask",1u);
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
