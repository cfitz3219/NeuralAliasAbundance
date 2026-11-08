#pragma once
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <utility>

namespace na::j2 {
// Read the SAME SRV as the game's compute upscale, on the GPU timeline. The
// game may populate its table after recording; copying descriptors on the CPU
// or guessing the last RTV would freeze the wrong input. A root buffer UAV
// lets this copy keep the game's heap bound, without writing into that heap.
struct LateInputBinding {
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    D3D12_GPU_DESCRIPTOR_HANDLE srv{};
};
class LateInputCopy {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D12RootSignature> root_;
    Ptr<ID3D12PipelineState> pso_;
public:
    struct Buffer { Ptr<ID3D12Resource> resource; D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 bytes=0; };
    bool Prepare(ID3D12Device* device, ID3D12Resource* target, Buffer& buffer, LateInputBinding& binding) {
        const auto td=target->GetDesc();
        if(td.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && td.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT)return false;
        if(!pso_){
            static constexpr char shader[]=R"(
Texture2D<float4> image : register(t0);
RWByteAddressBuffer packed : register(u0);
cbuffer Params : register(b0) { uint width; uint height; uint pitch; uint halfFloat; };
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
    if(p.x>=width || p.y>=height)return;
    float4 v=image.Load(int3(p.xy,0));
    uint at=p.y*pitch+p.x*(halfFloat!=0?8:4);
    if(halfFloat!=0){uint4 h=f32tof16(v);packed.Store2(at,uint2(h.x|(h.y<<16),h.z|(h.w<<16)));}
    else {uint4 c=uint4(round(saturate(v)*255.0));packed.Store(at,c.x|(c.y<<8)|(c.z<<16)|(c.w<<24));}
})";
            Ptr<ID3DBlob> code,error,serialized;
            if(FAILED(D3DCompile(shader,sizeof(shader)-1,"late_input.hlsl",nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error)))return false;
            D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};
            D3D12_ROOT_PARAMETER params[3]{};
            params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[0].DescriptorTable={1,&range};
            params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;params[1].Descriptor={0,0};
            params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[2].Constants={0,0,4};
            D3D12_ROOT_SIGNATURE_DESC rd{3,params,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
            // Root signature 1.0 descriptors are volatile, matching late writes.
            if(FAILED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&error)) ||
               FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root_))))return false;
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root_.Get();pd.CS={code->GetBufferPointer(),code->GetBufferSize()};
            if(FAILED(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pso_))))return false;
        }
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes=0;
        device->GetCopyableFootprints(&td,0,1,0,&fp,nullptr,nullptr,&bytes);
        if(!buffer.resource || buffer.bytes!=bytes){
            D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rd.Width=bytes;rd.Height=1;rd.DepthOrArraySize=1;
            rd.MipLevels=1;rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;rd.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            Ptr<ID3D12Resource> next;
            if(FAILED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_SOURCE,nullptr,IID_PPV_ARGS(&next))))return false;
            buffer.resource=std::move(next);buffer.bytes=bytes;
        }
        buffer.footprint=fp;binding.root=root_;binding.pipeline=pso_;return true;
    }
    // Caller owns a completion-qualified ring slot, retains heap/buffer with the
    // game's recording, and restores game shader state after reconstruction.
    void Record(ID3D12GraphicsCommandList* list,const LateInputBinding& binding,const Buffer& buffer,ID3D12Resource* target) {
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={buffer.resource.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        list->ResourceBarrier(1,&b);
        ID3D12DescriptorHeap* heap=binding.heap.Get();list->SetDescriptorHeaps(1,&heap);
        list->SetComputeRootSignature(binding.root.Get());list->SetPipelineState(binding.pipeline.Get());
        list->SetComputeRootDescriptorTable(0,binding.srv);list->SetComputeRootUnorderedAccessView(1,buffer.resource->GetGPUVirtualAddress());
        const auto& fp=buffer.footprint.Footprint;
        const UINT constants[]{fp.Width,fp.Height,fp.RowPitch,fp.Format==DXGI_FORMAT_R16G16B16A16_FLOAT?1u:0u};
        list->SetComputeRoot32BitConstants(2,4,constants,0);list->Dispatch((fp.Width+7)/8,(fp.Height+7)/8,1);
        std::swap(b.Transition.StateBefore,b.Transition.StateAfter);list->ResourceBarrier(1,&b);
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=buffer.resource.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=buffer.footprint;
        dst.pResource=target;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    }
};
}
