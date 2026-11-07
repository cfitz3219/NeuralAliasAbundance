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
