// Opt-in actual NVIDIA NR execution. No game process/files are touched.
// Caller supplies driver NGX core and model directory. GPU waits are test-only.
#include "test_support.hpp"
#include "host_dx12/nr/native_fg.hpp"
#include "host_dx12/nr/fg_states.hpp"
#include "host_dx12/settings.hpp"
#include <nvsdk_ngx.h>

// Exercise DXGI buffer acquisition/presentation, not only offscreen evaluation.
// This is a separate test window and never attaches to a game.
struct Presentation {
    HWND window=nullptr;
    test::ComPtr<IDXGISwapChain3> chain;
    test::List copies[8];
    uint64_t receipts[8]{};
    unsigned count=0;
    int Init(test::Device& d,UINT w,UINT h,DXGI_FORMAT format) {
        WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"NA_NR_Probe";
        RegisterClassW(&wc);window=CreateWindowW(wc.lpszClassName,L"NeuralAlias standalone NR regression",WS_OVERLAPPEDWINDOW,
            0,0,320,180,nullptr,nullptr,wc.hInstance,nullptr);REQUIRE(window);
        test::ComPtr<IDXGIFactory2> factory;REQUIRE(SUCCEEDED(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory))));
        DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=w;desc.Height=h;desc.Format=format;desc.SampleDesc.Count=1;
        desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=3;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        test::ComPtr<IDXGISwapChain1> base;REQUIRE(SUCCEEDED(factory->CreateSwapChainForHwnd(d.queue.Get(),window,&desc,nullptr,nullptr,&base)));
        REQUIRE(SUCCEEDED(base.As(&chain)));ShowWindow(window,SW_SHOWNOACTIVATE);
        for(auto& c:copies){REQUIRE(c.Create(d.device.Get()));REQUIRE(SUCCEEDED(c.list->Close()));}return 0;
    }
    int Show(test::Device& d,ID3D12Resource* image,D3D12_RESOURCE_STATES state) {
        MSG message;while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
        const auto index=count++%8;
        if(receipts[index]){auto waited=test::WaitFence(d.fence.Get(),receipts[index],d.event,10000);if(waited!=test::WaitResult::Ok)test::FatalWait("present ring",waited,d.device.Get());}
        auto& c=copies[index];c.Reset();test::ComPtr<ID3D12Resource> buffer;REQUIRE(SUCCEEDED(chain->GetBuffer(chain->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&buffer))));
        D3D12_RESOURCE_BARRIER barriers[]={test::Transition(image,state,D3D12_RESOURCE_STATE_COPY_SOURCE),test::Transition(buffer.Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_DEST)};
        c.list->ResourceBarrier(2,barriers);c.list->CopyResource(buffer.Get(),image);
        for(auto& b:barriers)std::swap(b.Transition.StateBefore,b.Transition.StateAfter);
        c.list->ResourceBarrier(2,barriers);REQUIRE(SUCCEEDED(c.list->Close()));ID3D12CommandList* lists[]={c.list.Get()};d.queue->ExecuteCommandLists(1,lists);
        receipts[index]=++d.fence_value;REQUIRE(SUCCEEDED(d.queue->Signal(d.fence.Get(),d.fence_value)));
        REQUIRE(SUCCEEDED(chain->Present(0,0)));return 0;
    }
    ~Presentation(){chain.Reset();if(window)DestroyWindow(window);}
};

void STDMETHODCALLTYPE Forward(ID3D12CommandQueue* q,UINT n,ID3D12CommandList* const* l){q->ExecuteCommandLists(n,l);}
int wmain(int argc,wchar_t** argv) {
    if(argc!=4){puts("SKIP: supply NGX core path, NR model directory and FG model directory");return test::kSkip;}
    setvbuf(stdout,nullptr,_IONBF,0);
    test::Device d;REQUIRE(test::CreateDevice(d,false));
    HMODULE core=LoadLibraryW(argv[1]);REQUIRE(core);
    using Init=int32_t(__cdecl*)(const char*,uint32_t,const char*,const wchar_t*,void*,uint32_t,const void*);
    auto init=reinterpret_cast<Init>(GetProcAddress(core,"NVSDK_NGX_D3D12_Init_ProjectID"));REQUIRE(init);
    NVSDK_NGX_FeatureCommonInfo common{};common.PathListInfo={argv+2,static_cast<unsigned>(argc-2)};
    auto result=init("a0f57b54-1daf-4934-90ae-c4035c19df04",0,"NeuralAliasProbe",L".",d.device.Get(),NVSDK_NGX_Version_API,&common);
    printf("NGX init=0x%08x\n",static_cast<unsigned>(result));REQUIRE(result==1);
    na::nr::Runtime rt{reinterpret_cast<void*>(GetProcAddress(core,"NVSDK_NGX_D3D12_CreateFeature")),
        reinterpret_cast<void*>(GetProcAddress(core,"NVSDK_NGX_D3D12_EvaluateFeature")),
        reinterpret_cast<void*>(GetProcAddress(core,"NVSDK_NGX_D3D12_ReleaseFeature")),
        reinterpret_cast<void*>(GetProcAddress(core,"NVSDK_NGX_D3D12_AllocateParameters")),
        reinterpret_cast<void*>(GetProcAddress(core,"NVSDK_NGX_D3D12_DestroyParameters")),nullptr};
    REQUIRE(rt.create_feature && rt.evaluate_feature && rt.allocate_parameters);
    constexpr UINT w=3440,h=1440;
    Presentation presentation;REQUIRE(presentation.Init(d,w,h,DXGI_FORMAT_R10G10B10A2_UNORM)==0);
    test::ComPtr<ID3D12Resource> inputs[4];
    DXGI_FORMAT formats[]={DXGI_FORMAT_R10G10B10A2_UNORM,DXGI_FORMAT_R10G10B10A2_UNORM,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R16G16_FLOAT};
    test::ComPtr<ID3D12DescriptorHeap> heap;D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,4,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
    REQUIRE(SUCCEEDED(d.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap))));
    auto cpu=heap->GetCPUDescriptorHandleForHeapStart();const auto stride=d.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for(UINT i=0;i<4;++i){inputs[i]=test::CreateTexture(d.device.Get(),w,h,formats[i],D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);REQUIRE(inputs[i]);d.device->CreateRenderTargetView(inputs[i].Get(),nullptr,{cpu.ptr+i*stride});}
    test::List list;REQUIRE(list.Create(d.device.Get()));na::nr::native::PrepareDevice(d.device.Get());
    for(UINT i=0;i<4;++i){float value[4]={i<2?.25f:i==2?.5f:0.f,i<2?.25f:0.f,i<2?.25f:0.f,1.f};
        list.list->ClearRenderTargetView({cpu.ptr+i*stride},value,0,nullptr);
        auto b=test::Transition(inputs[i].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);list.list->ResourceBarrier(1,&b);}
    REQUIRE(SUCCEEDED(list.list->Close()));ID3D12CommandList* lists[]={list.list.Get()};d.queue->ExecuteCommandLists(1,lists);d.WaitIdle();
    using Alloc=int32_t(__cdecl*)(void**);using Create=int32_t(__cdecl*)(void*,uint32_t,void*,void**);using Eval=int32_t(__cdecl*)(void*,const void*,const void*,void*);using Object=int32_t(__cdecl*)(void*);
    void* raw=nullptr;REQUIRE(reinterpret_cast<Alloc>(rt.allocate_parameters)(&raw)==1);auto& p=*static_cast<NVSDK_NGX_Parameter*>(raw);
    void* fg_handle=nullptr;
    auto generated=test::CreateTexture(d.device.Get(),w,h,formats[0],D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);REQUIRE(generated);
    if(argc==4){list.Reset();p.Set("CreationNodeMask",1u);p.Set("VisibilityNodeMask",1u);p.Set("Width",w);p.Set("Height",h);
        p.Set("DLSSG.BackbufferFormat",static_cast<unsigned>(formats[0]));p.Set("DLSSG.InternalWidth",w);p.Set("DLSSG.InternalHeight",h);
        auto code=reinterpret_cast<Create>(rt.create_feature)(list.list.Get(),11,&p,&fg_handle);printf("FG create=0x%08x\n",static_cast<unsigned>(code));REQUIRE(code==1);
        REQUIRE(SUCCEEDED(list.list->Close()));d.queue->ExecuteCommandLists(1,lists);d.WaitIdle();}
    p.Set("DLSSG.Backbuffer",inputs[0].Get());p.Set("DLSSG.HUDLess",inputs[1].Get());p.Set("DLSSG.Depth",inputs[2].Get());p.Set("DLSSG.MVecs",inputs[3].Get());
    p.Set("DLSSG.OutputInterpolated",generated.Get());p.Set("DLSSG.MultiFrameCount",2u);p.Set("DLSSG.MultiFrameIndex",1u);
    float identity[16]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    for(const char* key:{"DLSSG.CameraViewToClip","DLSSG.ClipToCameraView","DLSSG.ClipToLensClip","DLSSG.ClipToPrevClip","DLSSG.PrevClipToClip"})p.Set(key,static_cast<void*>(identity));
    p.Set("DLSSG.CameraNear",.1f);p.Set("DLSSG.CameraFar",1000.f);p.Set("DLSSG.CameraFOV",1.f);p.Set("DLSSG.CameraAspectRatio",float(w)/h);
    p.Set("DLSSG.ColorBuffersHDR",1u);p.Set("DLSSG.MvecScaleX",1.f);p.Set("DLSSG.MvecScaleY",1.f);
    na::settings::Get().nr_enabled.store(1);int handle;
    test::List ring[8];uint64_t receipts[8]{};
    for(auto& slot:ring){REQUIRE(slot.Create(d.device.Get()));REQUIRE(SUCCEEDED(slot.list->Close()));}
    for(unsigned long long frame=1;frame<=32;++frame)for(unsigned inner=1;inner<=2;++inner){
        const auto index=((frame-1)*2+inner-1)%8;
        if(receipts[index]){auto waited=test::WaitFence(d.fence.Get(),receipts[index],d.event,10000);if(waited!=test::WaitResult::Ok)test::FatalWait("probe ring",waited,d.device.Get());}
        auto& current=ring[index];current.Reset();auto* cmd=current.list.Get();na::nr::OnCommandListReset(cmd);
        auto b=test::Transition(inputs[0].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        // PBZ's native FG does not provide BackbufferFrameID. Exercise that
        // exact protocol rather than silently giving it a stronger identity.
        na::nr::states::Observe(cmd,1,&b);
        p.Set("DLSSG.Reset",frame==1?1u:0u);p.Set("DLSSG.MultiFrameIndex",inner);
        {na::nr::native::InputOverride replacement;na::nr::native::Before(cmd,core,&handle,&p,rt,&replacement);
            if(fg_handle){auto code=reinterpret_cast<Eval>(rt.evaluate_feature)(cmd,fg_handle,&p,nullptr);printf("FG eval=0x%08x\n",static_cast<unsigned>(code));REQUIRE(code==1);}}
        auto s=na::nr::native::GetStatus();printf("frame=%llu evaluated=%llu reason=%s\n",frame,static_cast<unsigned long long>(s.evaluates),s.reason);
        REQUIRE(s.evaluates==frame && s.active);REQUIRE(SUCCEEDED(cmd->Close()));
        ID3D12CommandList* submitted[]={cmd};na::nr::Execute(d.queue.Get(),1,submitted,Forward);
        receipts[index]=++d.fence_value;REQUIRE(SUCCEEDED(d.queue->Signal(d.fence.Get(),d.fence_value)));
        REQUIRE(presentation.Show(d,generated.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS)==0);
        if(inner==2)REQUIRE(presentation.Show(d,inputs[0].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)==0);
    }
    d.WaitIdle();for(auto& slot:ring){slot.Reset();na::nr::OnCommandListReset(slot.list.Get());slot.list->Close();}
    list.Reset();na::nr::OnCommandListReset(list.list.Get());list.list->Close();na::nr::native::Maintain();
    puts("release NR");na::nr::native::Release(core,&handle);puts("released NR");
    if(fg_handle)reinterpret_cast<Object>(rt.release_feature)(fg_handle);
    reinterpret_cast<Object>(rt.destroy_parameters)(&p);
    using Shutdown=int32_t(__cdecl*)(void*);auto shutdown=reinterpret_cast<Shutdown>(GetProcAddress(core,"NVSDK_NGX_D3D12_Shutdown1"));puts("shutdown NGX");if(shutdown)shutdown(d.device.Get());puts("shutdown complete");
    return test::finish("nr_runtime_probe");
}
