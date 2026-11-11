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
