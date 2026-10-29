#include "host_dx12/nr/fg_composition.hpp"
#include <d3dcompiler.h>
#include <mutex>

namespace na::nr {
namespace {
using Microsoft::WRL::ComPtr;
// Derivation from nvsdk_ngx_defs_dlssg.h: B=UI+(1-a)H. Therefore
// B'=B+(1-a)(E-H), H'=E retains the original premultiplied UI exactly.
// No-alpha fallback changes BOTH images only where their original RGB agrees.
// Equality alone cannot identify every UI pixel; this is explicitly a fallback.
// Reference: NVIDIA-RTX/Streamline docs/ProgrammingGuideDLSS_G.md sections5/6.6,
// and NVIDIA DLSS SDK nvsdk_ngx_defs_dlssg.h UI/HUDLess resource contract.
constexpr char shader[] = R"(
Texture2D<float4> full : register(t0);
Texture2D<float4> hudless : register(t1);
Texture2D<float4> enhanced : register(t2);
Texture2D<float4> alpha : register(t3);
RWTexture2D<float4> out_full : register(u0);
RWTexture2D<float4> out_hudless : register(u1);
cbuffer Region : register(b0) {
    uint2 extent; uint mode; uint unused;
    uint2 full_base; uint2 hudless_base;
    uint2 enhanced_base; uint2 alpha_base;
};
[numthreads(8,8,1)]
void compose(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= extent)) return;
    float4 b = full[id.xy + full_base], h = hudless[id.xy + hudless_base];
    float3 e = enhanced[id.xy + enhanced_base].rgb;
    float3 bo = b.rgb, ho = h.rgb;
    if (mode == 0) {
        if (all(b.rgb == h.rgb)) { bo = e; ho = e; }
    } else {
        float4 av = alpha[id.xy + alpha_base];
        float a = saturate(mode == 1 ? av.r : av.a);
        bo = b.rgb + (1.0 - a) * (e - h.rgb);
        ho = e;
    }
    // Never filter/replace stored alpha or clamp HDR scene values.
    out_full[id.xy] = float4(bo, b.a);
    out_hudless[id.xy] = float4(ho, h.a);
}
)";
struct Constants { uint32_t w,h,mode,pad,bx,by,hx,hy,ex,ey,ax,ay; };
bool Fits(CompositionSurface s, uint32_t w, uint32_t h) {
    if (!s.texture) return false;
    const auto d = s.texture->GetDesc();
    return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.DepthOrArraySize == 1 &&
        d.SampleDesc.Count == 1 && !(d.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) &&
        s.x <= d.Width && w <= d.Width - s.x && s.y <= d.Height && h <= d.Height - s.y;
}
bool ColorFormat(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R10G10B10A2_UNORM ||
        f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R32G32B32A32_FLOAT;
}
// Cache shader/root objects across packets; each packet also retains its own
// references so changing device cannot retire objects still in GPU use.
std::mutex cache_mutex;
ComPtr<ID3D12Device> cached_device;
ComPtr<ID3D12RootSignature> cached_root;
ComPtr<ID3D12PipelineState> cached_pipeline;
bool Pipeline(ID3D12Device* device, ComPtr<ID3D12RootSignature>& root, ComPtr<ID3D12PipelineState>& pipeline, bool prepare) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    if (cached_device.Get() == device && cached_pipeline) {
        root = cached_root; pipeline = cached_pipeline; return true;
    }
    if (!prepare) return false;
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV,4,0,0,0};
    ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV,2,0,0,4};
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = {2,ranges};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = {0,0,sizeof(Constants)/4};
    D3D12_ROOT_SIGNATURE_DESC desc{}; desc.NumParameters=2; desc.pParameters=params;
    ComPtr<ID3DBlob> sig,code,error;
    if (FAILED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&sig,&error)) ||
        FAILED(device->CreateRootSignature(0,sig->GetBufferPointer(),sig->GetBufferSize(),IID_PPV_ARGS(&root))) ||
        FAILED(D3DCompile(shader,sizeof(shader)-1,"nr_fg_composition",nullptr,nullptr,"compose","cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error))) return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{}; pd.pRootSignature=root.Get();
    pd.CS={code->GetBufferPointer(),code->GetBufferSize()};
    if (FAILED(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)))) return false;
    cached_device=device; cached_root=root; cached_pipeline=pipeline;
    return true;
}
bool Output(ID3D12Device* device,uint32_t w,uint32_t h,DXGI_FORMAT format,ComPtr<ID3D12Resource>& out) {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support))) ||
        !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)) return false;
    D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width=w; d.Height=h;
    d.DepthOrArraySize=d.MipLevels=1; d.SampleDesc.Count=1; d.Format=format;
    d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    return SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&out)));
}
}
bool PrepareFgComposition(ID3D12Device* device) {
    if (!device) return false;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    return Pipeline(device,root,pipeline,true);
}
bool FgComposition::SetCompletion(ID3D12Fence* fence,uint64_t value) {
    if (!recorded_ || !fence || !value || value==UINT64_MAX ||
        (completion_ && (completion_.Get()!=fence || value<completion_value_))) return false;
    completion_=fence; completion_value_=value; return true;
}
bool FgComposition::TryRecycle() {
    if (!recorded_ || !completion_) return false;
    const auto done=completion_->GetCompletedValue();
    if (done==UINT64_MAX || done<completion_value_) return false;
    for (auto& input:inputs_) input.Reset();
    recorded_=false; outputs_readable_=true; completion_.Reset(); completion_value_=0;
    return true;
}
bool FgComposition::Record(ID3D12GraphicsCommandList* list,uint32_t w,uint32_t h,
    CompositionSurface b,CompositionSurface h0,CompositionSurface e,CompositionSurface a,UiComposition mode) {
    if (recorded_ || !list || !w || !h || !Fits(b,w,h) || !Fits(h0,w,h) || !Fits(e,w,h)) return false;
    const auto bf=b.texture->GetDesc().Format, hf=h0.texture->GetDesc().Format;
    if (!ColorFormat(bf) || !ColorFormat(hf) || !ColorFormat(e.texture->GetDesc().Format)) return false;
    if (mode == UiComposition::EqualPixels) {
        if (bf != hf) return false;
        a = b; // valid unused SRV, without reading a missing alpha texture
    } else {
        if (mode != UiComposition::AlphaRed && mode != UiComposition::UiAlpha) return false;
        if (!Fits(a,w,h)) return false;
        const auto af=a.texture->GetDesc().Format;
        if (mode == UiComposition::UiAlpha ? !ColorFormat(af) :
            !(af==DXGI_FORMAT_R8_UNORM || af==DXGI_FORMAT_R16_UNORM || af==DXGI_FORMAT_R16_FLOAT || af==DXGI_FORMAT_R32_FLOAT)) return false;
    }
    ComPtr<ID3D12Device> device;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&device))) ||
        !Pipeline(device.Get(),root_,pipeline_,false)) return false;
    if (!full_ || device_.Get()!=device.Get() || full_->GetDesc().Width!=w || full_->GetDesc().Height!=h ||
        full_->GetDesc().Format!=bf || hudless_->GetDesc().Format!=hf) {
        // Build replacements transactionally. An allocation failure leaves a
        // retired packet reusable, without half replacing its output pair.
        ComPtr<ID3D12Resource> full,hudless;
        if (!Output(device.Get(),w,h,bf,full) || !Output(device.Get(),w,h,hf,hudless)) return false;
        full_=full; hudless_=hudless; outputs_readable_=false;
    }
    if (device_.Get()!=device.Get()) heap_.Reset();
    device_=device;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,6,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
    if (!heap_ && FAILED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap_)))) return false;
    auto cpu=heap_->GetCPUDescriptorHandleForHeapStart();
    const auto stride=device->GetDescriptorHandleIncrementSize(hd.Type);
    CompositionSurface sources[]={b,h0,e,a};
    for (uint32_t i=0;i<4;++i) {
        inputs_[i]=sources[i].texture;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format=inputs_[i]->GetDesc().Format;
        srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; srv.Texture2D.MipLevels=1;
        srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        device->CreateShaderResourceView(inputs_[i].Get(),&srv,cpu); cpu.ptr+=stride;
    }
    ID3D12Resource* outputs[]={full_.Get(),hudless_.Get()};
    for (auto* out:outputs) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.Format=out->GetDesc().Format; uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(out,nullptr,&uav,cpu); cpu.ptr+=stride;
    }
    const Constants c{w,h,static_cast<uint32_t>(mode),0,b.x,b.y,h0.x,h0.y,e.x,e.y,a.x,a.y};
    if (outputs_readable_) {
        D3D12_RESOURCE_BARRIER reuse[2]{};
        for (uint32_t i=0;i<2;++i) {
            reuse[i].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            reuse[i].Transition={outputs[i],D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        }
        list->ResourceBarrier(2,reuse);
    }
    ID3D12DescriptorHeap* heaps[]={heap_.Get()}; list->SetDescriptorHeaps(1,heaps);
    list->SetComputeRootSignature(root_.Get()); list->SetPipelineState(pipeline_.Get());
    list->SetComputeRootDescriptorTable(0,heap_->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRoot32BitConstants(1,sizeof(c)/4,&c,0); list->Dispatch((w+7)/8,(h+7)/8,1);
    D3D12_RESOURCE_BARRIER barriers[2]{};
    for (uint32_t i=0;i<2;++i) {
        barriers[i].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[i].Transition={outputs[i],D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    }
    list->ResourceBarrier(2,barriers); recorded_=true; return true;
}
} // namespace na::nr
