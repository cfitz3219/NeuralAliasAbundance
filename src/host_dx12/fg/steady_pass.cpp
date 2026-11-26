#include "host_dx12/fg/steady_pass.hpp"

#include <d3dcompiler.h>

#include <cstdio>
#include <cstring>

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace na::fg {
namespace {
// Astra's fg_static_midpoint.hlsl (research/fg-wire-shimmer, 93d279ca). Unchanged in what it writes.
// Added: the cbuffer carries mvScale (each motion sample is decoded to render pixels with it before the
// existing render-to-display scale), and a count of the pixels it changed (one atomic per 16x16 group
// into `touched` at `countAt`; the early return became an `inside` test so every lane reaches the
// group's last barrier).
constexpr char kShader[] = R"(
Texture2D<float4> older : register(t0);
Texture2D<float4> newer : register(t1);
Texture2D<float2> motion : register(t2);
RWTexture2D<float4> generated : register(u0);
RWByteAddressBuffer touched : register(u1);
cbuffer Frame : register(b0) { uint width, height; float2 mvScale; uint countAt; };
groupshared uint changed;
groupshared float diff[28*28];
groupshared float h5[28*24];
groupshared float v5[24*24];
groupshared float h9[24*16];
float proxyLuma(float3 color) {
    return dot(sign(color)*pow(abs(color),1.0/2.2), float3(.2126,.7152,.0722));
}
[numthreads(16,16,1)]
void main(uint3 tile:SV_GroupID, uint3 tid:SV_GroupThreadID, uint lane:SV_GroupIndex) {
    uint2 p=tile.xy*16+tid.xy;
    uint mw,mh;
    motion.GetDimensions(mw,mh);
    int2 edge=int2(width,height)-1;
    if(lane==0) changed=0;
    for(uint i=lane;i<28*28;i+=256) {
        int2 samplePos=clamp(int2(tile.xy*16)+int2(i%28,i/28)-6,int2(0,0),edge);
        diff[i]=proxyLuma(older[samplePos].rgb)-proxyLuma(newer[samplePos].rgb);
    }
    GroupMemoryBarrierWithGroupSync();
    for(uint i=lane;i<28*24;i+=256) {
        uint x=i%24,y=i/24;
        float sum=0;
        [unroll] for(uint k=0;k<5;k++) sum+=diff[y*28+x+k];
        h5[i]=sum*.2;
    }
    GroupMemoryBarrierWithGroupSync();
    for(uint i=lane;i<24*24;i+=256) {
        float sum=0;
        [unroll] for(uint k=0;k<5;k++) sum+=h5[i+k*24];
        v5[i]=abs(sum*.2);
    }
    GroupMemoryBarrierWithGroupSync();
    for(uint i=lane;i<24*16;i+=256) {
        uint x=i%16,y=i/16;
        float sum=0;
        [unroll] for(uint k=0;k<9;k++) sum+=v5[y*24+x+k];
        h9[i]=sum/9;
    }
    GroupMemoryBarrierWithGroupSync();
    if(p.x<width && p.y<height) {
    float disagreement=0;
    [unroll] for(uint k=0;k<9;k++) disagreement+=h9[(tid.y+k)*16+tid.x];
    disagreement*=255.0/9;
    uint2 mSize=uint2(mw,mh), size=uint2(width,height);
    uint2 at=min(p*mSize/size,mSize-1);
    float2 scale=float2(size)/float2(mSize);
    // Include adjacent motion so a thin moving object can veto the still-background rule.
    float speed=0;
    [unroll] for(int yy=-1;yy<=1;yy++) {
        [unroll] for(int xx=-1;xx<=1;xx++) {
            float2 v=motion[clamp(int2(at)+int2(xx,yy),int2(0,0),int2(mSize)-1)]*mvScale*scale;
            speed=max(speed,all(isfinite(v))?length(v):1e6);
        }
    }
    float weight=saturate((.85-speed)/.55)*saturate((2.3-disagreement)/1.4);
    if(weight>0) {
        float4 g=generated[p];
        float3 mid=.5*(older[p].rgb+newer[p].rgb);
        generated[p]=float4(lerp(g.rgb,mid,weight),g.a);
        InterlockedAdd(changed,1);
    }
    }
    GroupMemoryBarrierWithGroupSync();
    if(lane==0 && changed) touched.InterlockedAdd(countAt,changed);
}
)";
} // namespace

const char* SteadyPass::Source() { return kShader; }

bool SteadyPass::Init(ID3D12Device* device, DXGI_FORMAT format, char* reason, size_t n)
{
    auto why = [&](const char* t) { if (reason && n) snprintf(reason, n, "%s", t); return false; };
    if (pso_ && format == format_) return true;
    if (!device) return why("no device");
    // Linear FP16 only: the agreement test assumes linear light (Astra's README).
    if (format != DXGI_FORMAT_R16G16B16A16_FLOAT) return why("Still-area steadying needs a linear FP16 (HDR) picture; this game's picture format is not supported yet.");
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{};
    fs.Format = format;
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs)) ||
        !(fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) || !(fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
        return why("Still-area steadying needs typed UAV reads and writes of FP16 pictures, which this graphics card does not report.");
    D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 3}};
    D3D12_ROOT_PARAMETER p[2]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[0].DescriptorTable = {2, ranges};
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[1].Constants = {0, 0, 5};
    D3D12_ROOT_SIGNATURE_DESC rd{2, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> sig, err, code;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
        FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root_))))
        return why("Still-area steadying could not build its root signature.");
    if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "fg_static_midpoint", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err)))
        return why("Still-area steadying could not compile its shader.");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root_.Get();
    pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 5 * kPages, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso))) || FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_))))
        return why("Still-area steadying could not create its pipeline.");
    {   // the changed-pixel counters: one uint per page, and their readback copy
        D3D12_HEAP_PROPERTIES hp{};
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 4 * kPages;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&counter_))))
            return why("Still-area steadying could not create its counter.");
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        bd.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&counter_readback_))))
            return why("Still-area steadying could not create its counter.");
    }
    inc_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    device_ = device;
    format_ = format;
    pso_ = pso;
    return true;
}

bool SteadyPass::Record(ID3D12GraphicsCommandList* list, ID3D12Resource* generated, ID3D12Resource* older, ID3D12Resource* newer,
                        ID3D12Resource* motion, uint32_t w, uint32_t h, float mv_scale_x, float mv_scale_y, uint32_t page)
{
    if (!pso_ || !list || !generated || !older || !newer || !motion || !w || !h) return false;
    auto cpu = heap_->GetCPUDescriptorHandleForHeapStart();
    page %= kPages;
    cpu.ptr += SIZE_T(page) * 5 * inc_;
    auto gpu = heap_->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += UINT64(page) * 5 * inc_;
    ID3D12Resource* srvs[3] = {older, newer, motion};
    for (ID3D12Resource* r : srvs) {
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = r->GetDesc().Format;
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(r, &v, cpu);
        cpu.ptr += inc_;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
    u.Format = format_;
    u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device_->CreateUnorderedAccessView(generated, nullptr, &u, cpu);
    cpu.ptr += inc_;
    D3D12_UNORDERED_ACCESS_VIEW_DESC cu{};
    cu.Format = DXGI_FORMAT_R32_TYPELESS;
    cu.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    cu.Buffer.NumElements = kPages;
    cu.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device_->CreateUnorderedAccessView(counter_.Get(), nullptr, &cu, cpu);
    // This page's counter starts at zero (written by the list itself).
    ComPtr<ID3D12GraphicsCommandList2> l2;
    const bool counted = SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&l2)));
    auto move = [&](D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER t{};
        t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        t.Transition = {counter_.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
        list->ResourceBarrier(1, &t);
    };
    if (counted) {
        move(D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_WRITEBUFFERIMMEDIATE_PARAMETER wp{counter_->GetGPUVirtualAddress() + 4ull * page, 0u};
        D3D12_WRITEBUFFERIMMEDIATE_MODE wm = D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT;
        l2->WriteBufferImmediate(1, &wp, &wm);
        move(D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    counted_[page] = counted;
    ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root_.Get());
    list->SetPipelineState(pso_.Get());
    list->SetComputeRootDescriptorTable(0, gpu);
    struct { uint32_t w, h; float sx, sy; uint32_t at; } k{w, h, mv_scale_x, mv_scale_y, 4u * page};
    list->SetComputeRoot32BitConstants(1, 5, &k, 0);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = generated;
    list->ResourceBarrier(1, &b);
    list->Dispatch((w + 15) / 16, (h + 15) / 16, 1);
    list->ResourceBarrier(1, &b);
    if (counted) {
        move(D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(counter_readback_.Get(), 4ull * page, counter_.Get(), 4ull * page, 4);
        move(D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    return true;
}

bool SteadyPass::ReadChanged(uint32_t page, uint32_t* changed)
{
    page %= kPages;
    if (!counter_readback_ || !counted_[page] || !changed) return false;
    void* m = nullptr;
    D3D12_RANGE rr{4ull * page, 4ull * page + 4};
    if (FAILED(counter_readback_->Map(0, &rr, &m))) return false;
    std::memcpy(changed, static_cast<const uint8_t*>(m) + 4ull * page, 4);
    D3D12_RANGE none{0, 0};
    counter_readback_->Unmap(0, &none);
    return true;
}

} // namespace na::fg
