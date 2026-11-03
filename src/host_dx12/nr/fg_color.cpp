#include "host_dx12/nr/fg_color.hpp"
#include "host_dx12/nr/nr_shaders.hpp"
#include <d3dcompiler.h>
#include <mutex>
#include <cmath>
namespace na::nr {
namespace {
using Microsoft::WRL::ComPtr;
std::mutex mutex;
ComPtr<ID3D12Device> device;
ComPtr<ID3D12RootSignature> root;
ComPtr<ID3D12PipelineState> pipeline[2];
struct Constants { float paper; uint32_t w,h,mode,x,y; };
}
bool PrepareFgColor(ID3D12Device* dev) {
    if (!dev) return false;
    std::lock_guard<std::mutex> lock(mutex);
    if (device.Get()==dev && pipeline[0] && pipeline[1]) return true;
    ComPtr<ID3D12RootSignature> r;
    ComPtr<ID3D12PipelineState> p[2];
    D3D12_DESCRIPTOR_RANGE ranges[2]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,3,0,0,0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,3}};
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable={2,ranges};
    params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants={0,0,6};
    D3D12_ROOT_SIGNATURE_DESC rd{}; rd.NumParameters=2; rd.pParameters=params;
    ComPtr<ID3DBlob> blob,error;
    if (FAILED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error)) ||
        FAILED(dev->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&r)))) return false;
    const char* entries[]={"hdr_region_to_sdr","hdr_region_output"};
    for (int i=0;i<2;++i) {
        ComPtr<ID3DBlob> code;
        if (FAILED(D3DCompile(shaders::kSource,sizeof(shaders::kSource)-1,"nr_color",nullptr,nullptr,
            entries[i],"cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error))) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{}; pd.pRootSignature=r.Get();
        pd.CS={code->GetBufferPointer(),code->GetBufferSize()};
        if (FAILED(dev->CreateComputePipelineState(&pd,IID_PPV_ARGS(&p[i])))) return false;
    }
    device=dev; root=r; pipeline[0]=p[0]; pipeline[1]=p[1]; return true;
}
bool FgColorPass::Record(ID3D12GraphicsCommandList* list,bool output_pass,CompositionSurface original,
    ID3D12Resource* nr,ID3D12Resource* sdr,ID3D12Resource* out,uint32_t mode,float paper) {
    if (!list || !original.texture || !out || !std::isfinite(paper) || paper<=0 || (mode!=1 && mode!=2)) return false;
    const auto shape=out->GetDesc(), source=original.texture->GetDesc();
    if(shape.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || shape.DepthOrArraySize!=1 || shape.SampleDesc.Count!=1 ||
       source.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || source.DepthOrArraySize!=1 || source.SampleDesc.Count!=1 ||
       original.x>=source.Width || original.y>=source.Height || shape.Width>source.Width-original.x || shape.Height>source.Height-original.y)return false;
    if (!output_pass) nr=sdr=original.texture;
    if (!nr || !sdr) return false;
    if(output_pass && (nr->GetDesc().Width<shape.Width || nr->GetDesc().Height<shape.Height ||
       sdr->GetDesc().Width<shape.Width || sdr->GetDesc().Height<shape.Height))return false;
    ComPtr<ID3D12Device> dev;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&dev)))) return false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (device.Get()!=dev.Get() || !pipeline[output_pass]) return false;
        root_=root; pipeline_=pipeline[output_pass];
    }
    if (device_.Get()!=dev.Get()) { heap_.Reset(); device_=dev; }
    if (!heap_) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,4,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
        if (FAILED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap_)))) return false;
    }
    refs_[0]=original.texture; refs_[1]=nr; refs_[2]=sdr; refs_[3]=out;
    auto h=heap_->GetCPUDescriptorHandleForHeapStart();
    const auto stride=dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (int i=0;i<3;++i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format=refs_[i]->GetDesc().Format;
        sd.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; sd.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels=1; dev->CreateShaderResourceView(refs_[i].Get(),&sd,h); h.ptr+=stride;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{}; ud.Format=out->GetDesc().Format; ud.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    dev->CreateUnorderedAccessView(out,nullptr,&ud,h);
    D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={out,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    list->ResourceBarrier(1,&b);
    const auto desc=out->GetDesc();
    const Constants c{paper,static_cast<uint32_t>(desc.Width),desc.Height,mode,original.x,original.y};
    ID3D12DescriptorHeap* heaps[]={heap_.Get()}; list->SetDescriptorHeaps(1,heaps);
    list->SetComputeRootSignature(root_.Get()); list->SetPipelineState(pipeline_.Get());
    list->SetComputeRootDescriptorTable(0,heap_->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRoot32BitConstants(1,6,&c,0); list->Dispatch((c.w+7)/8,(c.h+7)/8,1);
    std::swap(b.Transition.StateBefore,b.Transition.StateAfter); list->ResourceBarrier(1,&b); return true;
}
}
