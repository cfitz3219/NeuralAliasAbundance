#include "host_dx12/j2/hud_picture.hpp"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace na::j2::native_hud {
using Microsoft::WRL::ComPtr;
namespace {
constexpr auto S=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto U=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr auto R=D3D12_RESOURCE_STATE_RENDER_TARGET;
constexpr auto CS=D3D12_RESOURCE_STATE_COPY_SOURCE;
constexpr auto CD=D3D12_RESOURCE_STATE_COPY_DEST;
void move(ID3D12GraphicsCommandList* l,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
 if(a==b)return;D3D12_RESOURCE_BARRIER t{};t.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
 t.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};l->ResourceBarrier(1,&t);
}
void order(ID3D12GraphicsCommandList* l){D3D12_RESOURCE_BARRIER t{};t.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;l->ResourceBarrier(1,&t);}
bool texture(ID3D12Device* dev,UINT w,UINT h,DXGI_FORMAT f,bool uav,D3D12_RESOURCE_STATES state,ComPtr<ID3D12Resource>& out){
 D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC d{};
 d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;d.DepthOrArraySize=d.MipLevels=1;
 d.SampleDesc.Count=1;d.Format=f;if(uav)d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
 return SUCCEEDED(dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&out)));
}
bool buffer(ID3D12Device* dev,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state,ComPtr<ID3D12Resource>& out){
 D3D12_HEAP_PROPERTIES hp{};hp.Type=type;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
 d.Width=16;d.Height=d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
 if(type==D3D12_HEAP_TYPE_DEFAULT)d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
 return SUCCEEDED(dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&out)));
}
constexpr char shader[]=R"(
Texture2D<float4> frame:register(t0),full:register(t1),scene:register(t2),n0:register(t3),n1:register(t4),picture:register(t5);
RWTexture2D<float4> selected:register(u0);
RWTexture2D<float> mask:register(u1);
RWTexture2D<float2> motion:register(u2);
RWTexture2D<float4> result:register(u3);
RWByteAddressBuffer proof:register(u4);
SamplerState lin:register(s0);
cbuffer K:register(b0){uint w,h,ow,oh,flags;float grey,tolerance;uint pad;};
float coverage(float3 a,float3 b){float3 d=abs(1-b);return saturate(max(max(d.r,max(d.g,d.b)),max(abs(a.r),max(abs(a.g),abs(a.b)))*8));}
float3 to_linear(float3 c){return c<=.04045?c/12.92:pow((c+.055)/1.055,2.4);}
float3 to_srgb(float3 c){c=saturate(c);return c<=.0031308?c*12.92:1.055*pow(c,1/2.4)-.055;}
groupshared uint mismatch;
[numthreads(8,8,1)]void verify(uint3 id:SV_DispatchThreadID,uint gi:SV_GroupIndex){
 if(gi==0)mismatch=0;GroupMemoryBarrierWithGroupSync();
 if(id.x<w&&id.y<h){float3 a=frame[id.xy].rgb,b=full[id.xy].rgb;
  if(any(!isfinite(a))||any(!isfinite(b))||any(abs(a-b)>tolerance*max(1,max(abs(a),abs(b)))))InterlockedOr(mismatch,1);}
 GroupMemoryBarrierWithGroupSync();if(gi==0&&mismatch)proof.InterlockedOr(0,1);
}
[numthreads(8,8,1)]void prepare(uint3 id:SV_DispatchThreadID){
 if(id.x>=w||id.y>=h)return;
 bool transfer=proof.Load(0)==0;
 bool clean=transfer&&(flags&1u)!=0;
 float m=0;
 if(transfer&&!clean){
  // Cover every actual native raster pixel belonging to this input pixel.
  // Centre-only downsampling can miss thin native glyphs between centres.
  uint2 lo=uint2(floor(float2(id.xy)*float2(ow,oh)/float2(w,h)));
  uint2 hi=min(uint2(ow,oh),uint2(ceil(float2(id.xy+1)*float2(ow,oh)/float2(w,h))));
  for(uint y=lo.y;y<hi.y;++y)for(uint x=lo.x;x<hi.x;++x){
   float3 a=n0[uint2(x,y)].rgb,b=(n1[uint2(x,y)].rgb-a)/grey;m=max(m,coverage(a,b));
  }
 }
 selected[id.xy]=clean?scene[id.xy]:frame[id.xy];
 mask[id.xy]=clean?0:m;
 if(!clean&&m>.01)motion[id.xy]=0;
}
[numthreads(8,8,1)]void compose(uint3 id:SV_DispatchThreadID){
 if(id.x>=ow||id.y>=oh)return;
 float4 s=picture[id.xy];float3 outc=s.rgb;
 if(proof.Load(0)==0){
  float2 uv=(float2(id.xy)+.5)/float2(ow,oh);
  float3 a=n0[id.xy].rgb,b=(n1[id.xy].rgb-a)/grey;
  if(flags&1u){
   float3 d=(flags&2u)?to_linear(s.rgb):s.rgb;
   outc=a+d*b;if(flags&2u)outc=to_srgb(outc);
  }else{
   float m=coverage(a,b);
   outc=lerp(s.rgb,frame.SampleLevel(lin,uv,0).rgb,smoothstep(.01,.1,m));
  }
 }
 result[id.xy]=float4(outc,s.a);
})";
struct Program {
 ComPtr<ID3D12Device> dev;ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pso[3];ComPtr<ID3D12Resource> zero;UINT inc=0;
 bool Init(ID3D12Device* d){
  dev=d;D3D12_DESCRIPTOR_RANGE ranges[2]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,6,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,5,0,0,6}};
  D3D12_ROOT_PARAMETER p[2]{};p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;p[0].DescriptorTable={2,ranges};
  p[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;p[1].Constants={0,0,8};
  D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;sampler.MaxLOD=D3D12_FLOAT32_MAX;
  D3D12_ROOT_SIGNATURE_DESC rd{2,p,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_NONE};ComPtr<ID3DBlob> sig,err;
  if(FAILED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&sig,&err))||
     FAILED(d->CreateRootSignature(0,sig->GetBufferPointer(),sig->GetBufferSize(),IID_PPV_ARGS(&root))))return false;
  const char* entries[]={"verify","prepare","compose"};
  for(int i=0;i<3;++i){ComPtr<ID3DBlob> code;if(FAILED(D3DCompile(shader,sizeof(shader)-1,"hud_picture",nullptr,nullptr,entries[i],"cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&err)))return false;
   D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={code->GetBufferPointer(),code->GetBufferSize()};
   if(FAILED(d->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pso[i]))))return false;}
  if(!buffer(d,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,zero))return false;
  void* data=nullptr;if(FAILED(zero->Map(0,nullptr,&data)))return false;std::memset(data,0,16);zero->Unmap(0,nullptr);
  inc=d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);return true;
 }
};
std::mutex programs_m;std::vector<std::weak_ptr<Program>> programs;
std::shared_ptr<Program> program(ID3D12Device* dev){
 std::lock_guard<std::mutex> lock(programs_m);for(const auto& weak:programs)if(auto p=weak.lock();p&&p->dev.Get()==dev)return p;
 auto p=std::make_shared<Program>();if(!p->Init(dev))return {};programs.push_back(p);return p;
}
}
struct Picture::Impl {
 std::shared_ptr<Program> program;UINT w=0,h=0,ow=0,oh=0;DXGI_FORMAT source_format{};
 ComPtr<ID3D12Resource> scene,full,input,mask,proof,readback,result,frame,motion,n[2];ComPtr<ID3D12DescriptorHeap> heap;
 bool scene_ready=false,full_ready=false,scene_current=false,full_current=false,prepared=false,outputs_read=false;DXGI_FORMAT frame_view{},layer_view{},result_format{};
 bool proof_read=false;
 struct K {UINT w,h,ow,oh,flags;float grey,tolerance;UINT pad;} constants{};
 bool snapshot(ID3D12GraphicsCommandList* l,ID3D12Resource* src,D3D12_RESOURCE_STATES state,ComPtr<ID3D12Resource>& dst,bool& ready){
  if(!l||!src)return false;auto d=src->GetDesc();if(d.Width!=w||d.Height!=h||d.Format!=source_format||d.MipLevels!=1||d.DepthOrArraySize!=1||d.SampleDesc.Count!=1)return false;
  move(l,src,state,CS);if(ready)move(l,dst.Get(),S,CD);l->CopyResource(dst.Get(),src);move(l,src,CS,state);move(l,dst.Get(),CD,S);ready=true;return true;
 }
 void bind(ID3D12GraphicsCommandList* l,UINT pass,ID3D12Resource* pic,DXGI_FORMAT pf){
  auto cpu=heap->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=SIZE_T(pass*11)*program->inc;
  ID3D12Resource* srvs[]={frame.Get(),full.Get(),scene.Get(),n[0].Get(),n[1].Get(),pic};
  DXGI_FORMAT formats[]={frame_view,frame_view,frame_view,layer_view,layer_view,pf};
  for(UINT i=0;i<6;++i){D3D12_SHADER_RESOURCE_VIEW_DESC v{};v.Format=srvs[i]?formats[i]:DXGI_FORMAT_R8G8B8A8_UNORM;
   v.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;v.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;v.Texture2D.MipLevels=1;
   program->dev->CreateShaderResourceView(srvs[i],&v,cpu);cpu.ptr+=program->inc;}
  ID3D12Resource* uavs[]={input.Get(),mask.Get(),motion.Get(),result.Get()};
  DXGI_FORMAT uf[]={DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R16_FLOAT,DXGI_FORMAT_R32G32_FLOAT,result_format};
  for(UINT i=0;i<4;++i){D3D12_UNORDERED_ACCESS_VIEW_DESC v{};v.Format=uavs[i]?uf[i]:DXGI_FORMAT_R16G16B16A16_FLOAT;v.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
   program->dev->CreateUnorderedAccessView(uavs[i],nullptr,&v,cpu);cpu.ptr+=program->inc;}
  D3D12_UNORDERED_ACCESS_VIEW_DESC v{};v.Format=DXGI_FORMAT_R32_TYPELESS;v.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;v.Buffer.NumElements=4;v.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;
  program->dev->CreateUnorderedAccessView(proof.Get(),nullptr,&v,cpu);
  ID3D12DescriptorHeap* heaps[]={heap.Get()};l->SetDescriptorHeaps(1,heaps);l->SetComputeRootSignature(program->root.Get());l->SetPipelineState(program->pso[pass].Get());
  auto gpu=heap->GetGPUDescriptorHandleForHeapStart();gpu.ptr+=UINT64(pass*11)*program->inc;
  l->SetComputeRootDescriptorTable(0,gpu);l->SetComputeRoot32BitConstants(1,8,&constants,0);
 }
};
Picture::Picture():p_(std::make_unique<Impl>()){}
Picture::~Picture()=default;
std::shared_ptr<Picture> Picture::Create(ID3D12Device* dev,const D3D12_RESOURCE_DESC& d,uint32_t ow,uint32_t oh){
 if(!dev||d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||d.SampleDesc.Count!=1||d.DepthOrArraySize!=1||d.MipLevels!=1||!ow||!oh)return {};
 auto result=std::make_shared<Picture>();auto& p=*result->p_;p.program=program(dev);if(!p.program)return {};
 p.w=UINT(d.Width);p.h=d.Height;p.ow=ow;p.oh=oh;p.source_format=d.Format;
 if(!texture(dev,p.w,p.h,d.Format,false,CD,p.scene)||!texture(dev,p.w,p.h,d.Format,false,CD,p.full)||
    !texture(dev,p.w,p.h,DXGI_FORMAT_R16G16B16A16_FLOAT,true,U,p.input)||!texture(dev,p.w,p.h,DXGI_FORMAT_R16_FLOAT,true,U,p.mask)||
    !buffer(dev,D3D12_HEAP_TYPE_DEFAULT,U,p.proof)||!buffer(dev,D3D12_HEAP_TYPE_READBACK,CD,p.readback))return {};
 D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,33,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
 if(FAILED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&p.heap))))return {};return result;
}
bool Picture::Matches(ID3D12Device* d,const D3D12_RESOURCE_DESC& s,uint32_t ow,uint32_t oh)const{return p_->program->dev.Get()==d&&p_->w==s.Width&&p_->h==s.Height&&p_->source_format==s.Format&&p_->ow==ow&&p_->oh==oh;}
void Picture::BeginCycle(){p_->prepared=p_->scene_current=p_->full_current=p_->proof_read=false;p_->frame.Reset();p_->motion.Reset();p_->n[0].Reset();p_->n[1].Reset();}
bool Picture::SnapshotScene(ID3D12GraphicsCommandList* l,ID3D12Resource* r,D3D12_RESOURCE_STATES s){return p_->scene_current=p_->snapshot(l,r,s,p_->scene,p_->scene_ready);}
bool Picture::SnapshotFull(ID3D12GraphicsCommandList* l,ID3D12Resource* r,D3D12_RESOURCE_STATES s){return p_->full_current=p_->snapshot(l,r,s,p_->full,p_->full_ready);}
bool Picture::Prepare(ID3D12GraphicsCommandList* l,ID3D12Resource* frame,DXGI_FORMAT fv,ID3D12Resource* motion,
 ID3D12Resource* n0,ID3D12Resource* n1,DXGI_FORMAT lv,float grey,bool srgb,bool clean,DXGI_FORMAT output_format){
 auto& p=*p_;if(!l||!frame||!motion||!n0||!n1||!p.scene_current||!p.full_current||p.prepared||!std::isfinite(grey)||grey<=0)return false;
 auto fd=frame->GetDesc(),md=motion->GetDesc(),nd=n0->GetDesc();
 if(fd.Width!=p.w||fd.Height!=p.h||md.Width!=p.w||md.Height!=p.h||nd.Width!=p.ow||nd.Height!=p.oh)return false;
 if((!p.result||p.result_format!=output_format)&&!texture(p.program->dev.Get(),p.ow,p.oh,output_format,true,U,p.result))return false;
 p.result_format=output_format;p.frame=frame;p.motion=motion;p.n[0]=n0;p.n[1]=n1;p.frame_view=fv;p.layer_view=lv;
 const float tolerance=fv==DXGI_FORMAT_R16G16B16A16_FLOAT?.0005f:fv==DXGI_FORMAT_R10G10B10A2_UNORM?2.f/1023.f:2.f/255.f;
 p.constants={p.w,p.h,p.ow,p.oh,(clean?1u:0u)|(srgb?2u:0u),grey,tolerance,0};
 if(p.outputs_read){move(l,p.input.Get(),S,U);move(l,p.mask.Get(),S,U);}
 move(l,p.proof.Get(),U,CD);l->CopyBufferRegion(p.proof.Get(),0,p.program->zero.Get(),0,16);move(l,p.proof.Get(),CD,U);
 p.bind(l,0,nullptr,DXGI_FORMAT_UNKNOWN);l->Dispatch((p.w+7)/8,(p.h+7)/8,1);order(l);
 move(l,n0,R,S);move(l,n1,R,S);move(l,motion,S,U);
 p.bind(l,1,nullptr,DXGI_FORMAT_UNKNOWN);l->Dispatch((p.w+7)/8,(p.h+7)/8,1);order(l);
 move(l,p.proof.Get(),U,CS);l->CopyBufferRegion(p.readback.Get(),0,p.proof.Get(),0,16);move(l,p.proof.Get(),CS,U);
 move(l,n0,S,R);move(l,n1,S,R);move(l,motion,U,S);move(l,p.input.Get(),U,S);move(l,p.mask.Get(),U,S);
 p.prepared=p.outputs_read=true;return true;
}
ID3D12Resource* Picture::Input()const{return p_->input.Get();}
ID3D12Resource* Picture::Mask()const{return p_->mask.Get();}
ID3D12Resource* Picture::Scene()const{return p_->scene.Get();}
ID3D12Resource* Picture::Full()const{return p_->full.Get();}
bool Picture::ProofAfterCompletion(bool& identity,bool& clean){
 auto& p=*p_;if(!p.prepared||p.proof_read)return false;void* data=nullptr;D3D12_RANGE range{0,4};
 if(FAILED(p.readback->Map(0,&range,&data)))return false;UINT flags=0;std::memcpy(&flags,data,4);D3D12_RANGE none{};p.readback->Unmap(0,&none);
 identity=flags==0;clean=identity&&(p.constants.flags&1u)!=0;p.proof_read=true;return true;
}
bool Picture::Composite(ID3D12GraphicsCommandList* l,ID3D12Resource* out){
 auto& p=*p_;if(!p.prepared||!l||!out)return false;const auto d=out->GetDesc();
 if(d.Width!=p.ow||d.Height!=p.oh||d.Format!=p.result_format||!p.result)return false;
 move(l,out,U,S);move(l,p.n[0].Get(),R,S);move(l,p.n[1].Get(),R,S);
 p.bind(l,2,out,d.Format);l->Dispatch((p.ow+7)/8,(p.oh+7)/8,1);order(l);
 move(l,out,S,CD);move(l,p.result.Get(),U,CS);l->CopyResource(out,p.result.Get());move(l,p.result.Get(),CS,U);move(l,out,CD,U);
 move(l,p.n[0].Get(),S,R);move(l,p.n[1].Get(),S,R);return true;
}
}
