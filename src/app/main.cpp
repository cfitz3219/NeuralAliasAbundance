#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include "core/frame_contract.hpp"
using Microsoft::WRL::ComPtr;
int run_window(bool smoke);
int main(int argc,char** argv) {
    if(argc==1 || (argc==2 && std::strcmp(argv[1],"--ui")==0)) return run_window(false);
    if(argc==2 && std::strcmp(argv[1],"--ui-smoke")==0) return run_window(true);
    if(argc>2 || (argc==2 && std::strcmp(argv[1],"--status"))) {
        std::puts("Usage: NeuralAliasAbundance [--ui | --status | --ui-smoke]");return 2;
    }
    std::puts("NeuralAliasAbundance 0.1 - standalone DX12 foundation\nNo ReShade or Alien dependency.");
    ComPtr<IDXGIFactory6> factory;
    if(FAILED(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)))) { std::puts("DXGI initialization failed");return 1; }
    bool found=false;
    for(UINT i=0;;++i) {
        ComPtr<IDXGIAdapter1> adapter;
        const auto result=factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter));
        if(result==DXGI_ERROR_NOT_FOUND) break;
        if(FAILED(result)) return 1;
        DXGI_ADAPTER_DESC1 desc{};adapter->GetDesc1(&desc);
        if(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        ComPtr<ID3D12Device> device;
        if(SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)))) {
            std::printf("DX12 GPU: %ls (%llu MiB dedicated memory)\n",desc.Description,
                static_cast<unsigned long long>(desc.DedicatedVideoMemory/(1024*1024)));found=true;
        }
    }
    std::puts("Input route: waiting for an observed upscaler call or a validated adapter.");
    std::puts("DLAA / DLSS: backend not connected to standalone host.");
    std::puts("Neural Rendering: experimental backend integration pending.");
    std::puts("Frame Generation: Not implemented yet [disabled].");
    std::puts("This diagnostic does not scan or modify games. Run abundance_dx12_fixture for GPU validation.");
    return found?0:1;
}
