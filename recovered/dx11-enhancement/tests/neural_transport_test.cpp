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
