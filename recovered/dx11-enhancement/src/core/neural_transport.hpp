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
