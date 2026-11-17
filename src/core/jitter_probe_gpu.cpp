#include "core/jitter_probe_gpu.hpp"
#include <d3dcompiler.h>
#include <cstring>
#include <cmath>

namespace na::jitter::gpu {
using Microsoft::WRL::ComPtr;
namespace {
constexpr auto U=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
const char shader[]=R"(
Texture2D<float4> cur:register(t0);
Texture2D<float4> prev:register(t1);
RWByteAddressBuffer counts:register(u0);
SamplerState linear_clamp:register(s0);
cbuffer K:register(b0){uint w,h,step,pad;float2 raster_delta,encode_delta;};
groupshared uint sums[4];
float error_at(float3 c,float2 uv){float3 d=abs(c-prev.SampleLevel(linear_clamp,uv,0).rgb);return d.r+d.g+d.b;}
[numthreads(8,8,1)]
void main(uint3 id:SV_DispatchThreadID,uint gi:SV_GroupIndex){
 if(gi<4)sums[gi]=0;GroupMemoryBarrierWithGroupSync();
 const uint2 p=id.xy*step+step/2;
 if(p.x>2 && p.y>2 && p.x+2<w && p.y+2<h){
  float3 c=cur[p].rgb;float lo=2,hi=-1;
  [unroll]for(int y=-1;y<=1;++y)[unroll]for(int x=-1;x<=1;++x){
   float l=dot(cur[int2(p)+int2(x,y)].rgb,float3(.299,.587,.114));lo=min(lo,l);hi=max(hi,l);}
  if(hi-lo>.1 && all(isfinite(c))){
   // Unexplained world pixels count as ambiguous. Filtering them out of
   // the denominator would let a small static HUD decide the whole source.
   InterlockedAdd(sums[0],1u);
   const float2 uv=(float2(p)+.5+encode_delta)/float2(w,h);
   const float2 dj=raster_delta/float2(w,h);
   const float e0=error_at(c,uv),er=error_at(c,uv-dj),eo=error_at(c,uv+dj);
   // Displacement not explained by ANY sampling hypothesis is acquisition,
   // not a reason to label a moving scene unjittered.
   if(min(e0,min(er,eo))<.06){
    if(e0*2<min(er,eo))InterlockedAdd(sums[1],1u);
    else if(er*2<min(e0,eo))InterlockedAdd(sums[2],1u);
    else if(eo*2<min(e0,er))InterlockedAdd(sums[3],1u);
   }
  }
 }
 GroupMemoryBarrierWithGroupSync();
 if(gi<4 && sums[gi])counts.InterlockedAdd(gi*4,sums[gi]);
})";
D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
 D3D12_RESOURCE_BARRIER x{};x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
 x.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};return x;
}
bool buffer(ID3D12Device* dev,UINT64 bytes,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state,bool uav,ComPtr<ID3D12Resource>& r){
 D3D12_HEAP_PROPERTIES hp{};hp.Type=type;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
 d.Width=bytes;d.Height=d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
 if(uav)d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
 return SUCCEEDED(dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r)));
}
}
bool Probe::Init(ID3D12Device* dev){
 if(!dev)return false;device_=dev;
 D3D12_DESCRIPTOR_RANGE ranges[2]{};ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0};
 ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,2};
 D3D12_ROOT_PARAMETER p[2]{};p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
 p[0].DescriptorTable={2,ranges};p[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;p[1].Constants={0,0,8};
 D3D12_STATIC_SAMPLER_DESC s{};s.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
 s.AddressU=s.AddressV=s.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;s.MaxLOD=D3D12_FLOAT32_MAX;
 D3D12_ROOT_SIGNATURE_DESC rd{2,p,1,&s,D3D12_ROOT_SIGNATURE_FLAG_NONE};ComPtr<ID3DBlob> sig,err,cs;
 if(FAILED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&sig,&err)) ||
    FAILED(dev->CreateRootSignature(0,sig->GetBufferPointer(),sig->GetBufferSize(),IID_PPV_ARGS(&root_))) ||
    FAILED(D3DCompile(shader,sizeof(shader)-1,"jitter_probe",nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&cs,&err)))return false;
 D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root_.Get();pd.CS={cs->GetBufferPointer(),cs->GetBufferSize()};
 if(FAILED(dev->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pso_))) ||
    !buffer(dev,16,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,false,zero_))return false;
 void* data=nullptr;if(FAILED(zero_->Map(0,nullptr,&data)))return false;std::memset(data,0,16);zero_->Unmap(0,nullptr);
 stride_=dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);return true;
}
bool Probe::Prepare(Slot& s){
 if(s.heap)return true;
 D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,3,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
 if(FAILED(device_->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&s.heap))) ||
    !buffer(device_.Get(),16,D3D12_HEAP_TYPE_DEFAULT,U,true,s.counts) ||
    !buffer(device_.Get(),16,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST,false,s.readback)){s.heap.Reset();return false;}
 return true;
}
bool Probe::Record(ID3D12GraphicsCommandList* l,uint32_t slot,ID3D12Resource* current,ID3D12Resource* previous,
                   Source key,uint64_t frame,Sample raster,Sample encode){
 if(!pso_ || !l || !current || !previous || !frame || !key.width || !key.height ||
    !std::isfinite(raster.x) || !std::isfinite(raster.y) || !std::isfinite(encode.x) || !std::isfinite(encode.y) ||
    raster.x*raster.x+raster.y*raster.y<.04f)return false;
 const auto a=current->GetDesc(),b=previous->GetDesc();
 if(a.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || b.Dimension!=a.Dimension || a.Width!=key.width ||
    b.Width!=a.Width || a.Height!=key.height || b.Height!=a.Height || a.Format!=b.Format || a.SampleDesc.Count!=1)return false;
 if(slots_.size()<=slot)slots_.resize(slot+1);auto& s=slots_[slot];if(!Prepare(s))return false;
 auto cpu=[&](uint32_t i){auto h=s.heap->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(i)*stride_;return h;};
 D3D12_SHADER_RESOURCE_VIEW_DESC sv{};sv.Format=a.Format;sv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
 sv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;sv.Texture2D.MipLevels=1;
 device_->CreateShaderResourceView(current,&sv,cpu(0));device_->CreateShaderResourceView(previous,&sv,cpu(1));
 D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};uv.Format=DXGI_FORMAT_R32_TYPELESS;uv.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;
 uv.Buffer.NumElements=4;uv.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;device_->CreateUnorderedAccessView(s.counts.Get(),nullptr,&uv,cpu(2));
 auto t=transition(s.counts.Get(),U,D3D12_RESOURCE_STATE_COPY_DEST);l->ResourceBarrier(1,&t);
 l->CopyBufferRegion(s.counts.Get(),0,zero_.Get(),0,16);t=transition(s.counts.Get(),D3D12_RESOURCE_STATE_COPY_DEST,U);l->ResourceBarrier(1,&t);
 ID3D12DescriptorHeap* heaps[]={s.heap.Get()};l->SetDescriptorHeaps(1,heaps);l->SetComputeRootSignature(root_.Get());
 l->SetPipelineState(pso_.Get());l->SetComputeRootDescriptorTable(0,s.heap->GetGPUDescriptorHandleForHeapStart());
 struct K{uint32_t w,h,step,pad;float rx,ry,ex,ey;} k{key.width,key.height,4,0,raster.x,raster.y,encode.x,encode.y};
 l->SetComputeRoot32BitConstants(1,8,&k,0);l->Dispatch((key.width+31)/32,(key.height+31)/32,1);
 t=transition(s.counts.Get(),U,D3D12_RESOURCE_STATE_COPY_SOURCE);l->ResourceBarrier(1,&t);
 l->CopyBufferRegion(s.readback.Get(),0,s.counts.Get(),0,16);t=transition(s.counts.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,U);l->ResourceBarrier(1,&t);
 s.source=key;s.frame=frame;s.recorded=true;return true;
}
bool Probe::Collect(uint32_t slot,Source& source,Evidence& e){
 if(slot>=slots_.size() || !slots_[slot].recorded)return false;auto& s=slots_[slot];s.recorded=false;
 void* data=nullptr;D3D12_RANGE range{0,16};if(FAILED(s.readback->Map(0,&range,&data)))return false;
 uint32_t n[4];std::memcpy(n,data,16);D3D12_RANGE none{};s.readback->Unmap(0,&none);
 source=s.source;e={s.frame,n[0],n[1],n[2],n[3]};return true;
}
}
