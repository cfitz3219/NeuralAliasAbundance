#include "host_dx12/j2/hud_picture.hpp"
#include "host_dx12/j2/native_hud.hpp"
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
bool buffer(ID3D12Device* dev,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state,ComPtr<ID3D12Resource>& out,UINT64 bytes=16){
 D3D12_HEAP_PROPERTIES hp{};hp.Type=type;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
 d.Width=bytes;d.Height=d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
 if(type==D3D12_HEAP_TYPE_DEFAULT)d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
 return SUCCEEDED(dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&out)));
}
constexpr char shader[]=R"(
Texture2D<float4> frame:register(t0),full:register(t1),scene:register(t2),n0:register(t3),n1:register(t4),picture:register(t5);
Texture2D<float4> low0:register(t6),low1:register(t7);
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
// The existing proof allocation also owns a pixel-local agreement map. A
// cursor/late overlay can retain its original pixels without disabling native
// HUD everywhere else. No new image pass or relaxed colour tolerance.
bool agrees(uint2 p){return proof.Load(16+4*(p.y*w+p.x))==0;}
bool agrees_at(float2 uv){
 int2 q=int2(floor(uv*float2(w,h)-.5));
 [unroll]for(int y=0;y<2;++y)[unroll]for(int x=0;x<2;++x)
  if(!agrees(uint2(clamp(q+int2(x,y),int2(0,0),int2(w-1,h-1)))))return false;
 return true;
}
groupshared uint mismatch,matched,error_bits,picture_matched;
[numthreads(8,8,1)]void verify(uint3 id:SV_DispatchThreadID,uint gi:SV_GroupIndex){
 if(gi==0){mismatch=0;matched=0;error_bits=0;picture_matched=0;}GroupMemoryBarrierWithGroupSync();
 if(id.x<w&&id.y<h){float3 a=frame[id.xy].rgb,b=full[id.xy].rgb;
  bool bad=any(!isfinite(a))||any(!isfinite(b))||any(abs(a-b)>tolerance*max(1,max(abs(a),abs(b))));
  if(!bad)InterlockedAdd(picture_matched,1);
  float error=max(abs(a.r-b.r),max(abs(a.g-b.g),abs(a.b-b.b)));
  if(flags&64u){
   float3 base=scene[id.xy].rgb,lo=low0[id.xy].rgb,scale=(low1[id.xy].rgb-lo)/grey;
   if(flags&256u)base=to_linear(base);
   float3 prediction=lo+scale*base;
   if(flags&2u)prediction=to_srgb(prediction);else if(!(flags&128u))prediction=saturate(prediction);
   float3 delta=abs(prediction-a);
   if(any(!isfinite(prediction))||any(delta>tolerance*max(1,max(abs(prediction),abs(a)))))bad=true;
   error=max(error,max(delta.r,max(delta.g,delta.b)));
  }
  if(flags&8u){float4 c=n1[id.xy],d=full[id.xy];
   if(any(!isfinite(c))||any(!isfinite(d))||any(abs(c-d)>tolerance*max(1,max(abs(c),abs(d))))||
      abs(frame[id.xy].a-d.a)>tolerance*max(1,max(abs(frame[id.xy].a),abs(d.a))))bad=true;}
  proof.Store(16+4*(id.y*w+id.x),bad?1u:0u);
  if(bad)InterlockedOr(mismatch,1);else InterlockedAdd(matched,1);
  if(isfinite(error))InterlockedMax(error_bits,asuint(error));}
 GroupMemoryBarrierWithGroupSync();if(gi==0){if(mismatch)proof.InterlockedOr(0,1);proof.InterlockedAdd(4,matched);proof.InterlockedMax(8,error_bits);proof.InterlockedAdd(12,picture_matched);}
}
[numthreads(8,8,1)]void prepare(uint3 id:SV_DispatchThreadID){
 if(id.x>=w||id.y>=h)return;
 bool transfer=agrees(id.xy);
 bool clean=transfer&&(flags&1u)!=0;
 float m=0;
 if(!clean&&(transfer||(flags&64u))){
  // Cover every actual native raster pixel belonging to this input pixel.
  // Centre-only downsampling can miss thin native glyphs between centres.
  uint2 lo=uint2(floor(float2(id.xy)*float2(ow,oh)/float2(w,h)));
  uint2 hi=min(uint2(ow,oh),uint2(ceil(float2(id.xy+1)*float2(ow,oh)/float2(w,h))));
  for(uint y=lo.y;y<hi.y;++y)for(uint x=lo.x;x<hi.x;++x){
   float3 a=n0[uint2(x,y)].rgb,b=(n1[uint2(x,y)].rgb-a)/grey;m=max(m,coverage(a,b));
  }
 }
 float4 input_scene=scene[id.xy];
 if((flags&64u)&&clean){
  if(flags&256u)input_scene.rgb=to_linear(input_scene.rgb);
  if(flags&2u)input_scene.rgb=to_srgb(input_scene.rgb);
 }
 selected[id.xy]=clean?input_scene:frame[id.xy];
 mask[id.xy]=clean?0:m;
 if(!clean&&m>.01)motion[id.xy]=0;
}
[numthreads(8,8,1)]void compose(uint3 id:SV_DispatchThreadID){
 if(id.x>=ow||id.y>=oh)return;
 float2 uv=(float2(id.xy)+.5)/float2(ow,oh);
 if(flags&32u){result[id.xy]=scene.SampleLevel(lin,uv,0);return;}
 if(flags&8u){bool valid=agrees_at(uv);float4 c=valid?n0[id.xy]:frame.SampleLevel(lin,uv,0);
  if((flags&4u)&&valid){float3 d=abs(full.SampleLevel(lin,uv,0).rgb-scene.SampleLevel(lin,uv,0).rgb);
   c.rgb=lerp(c.rgb,float3(1,0,1),.5*saturate(max(d.r,max(d.g,d.b))*16));}result[id.xy]=c;return;}
 if(flags&16u){result[id.xy]=float4(picture[id.xy].rgb,scene.SampleLevel(lin,uv,0).a);return;}
 float4 s=picture[id.xy];float3 outc=s.rgb;
 if((flags&64u)&&!agrees_at(uv)){result[id.xy]=frame.SampleLevel(lin,uv,0);return;}
 if(agrees_at(uv)){
  float2 uv=(float2(id.xy)+.5)/float2(ow,oh);
  float3 a=n0[id.xy].rgb,b=(n1[id.xy].rgb-a)/grey;
  if(flags&1u){
   float3 d=(flags&2u)?to_linear(s.rgb):s.rgb;
   outc=a+d*b;if(flags&2u)outc=to_srgb(outc);
  }else{
   float m=coverage(a,b);
   outc=lerp(s.rgb,frame.SampleLevel(lin,uv,0).rgb,smoothstep(.01,.1,m));
  }
  if(flags&4u)outc=lerp(outc,float3(1,0,1),.5*coverage(a,b));
 }
 result[id.xy]=float4(outc,(flags&64u)?frame.SampleLevel(lin,uv,0).a:s.a);
})";
struct Program {
 ComPtr<ID3D12Device> dev;ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pso[3];ComPtr<ID3D12Resource> zero;UINT inc=0;
 bool Init(ID3D12Device* d){
  dev=d;D3D12_DESCRIPTOR_RANGE ranges[2]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,8,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,5,0,0,8}};
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
 ComPtr<ID3D12Resource> scene,full,input,mask,proof,readback,result,frame,motion,n[2],low[2];ComPtr<ID3D12DescriptorHeap> heap;
 bool scene_current=false,full_current=false,prepared=false,outputs_read=false;DXGI_FORMAT frame_view{},layer_view{},result_format{};
 bool proof_read=false,source_srgb=false;
 std::vector<std::shared_ptr<Picture>> readers;
 UINT descriptor_page=0; // pre-HUD work and final Present may both be in flight
 struct K {UINT w,h,ow,oh,flags;float grey,tolerance;UINT pad;} constants{};
 bool snapshot(ID3D12GraphicsCommandList* l,ID3D12Resource* src,D3D12_RESOURCE_STATES state,ComPtr<ID3D12Resource>& dst){
  if(!l||!src)return false;auto d=src->GetDesc();if(d.Width!=w||d.Height!=h||
    (d.Format!=source_format&&(TypelessOf(d.Format)==DXGI_FORMAT_UNKNOWN||TypelessOf(d.Format)!=TypelessOf(source_format)))||d.MipLevels!=1||d.DepthOrArraySize!=1||d.SampleDesc.Count!=1)return false;
  // Executable game recordings may run more than once. The snapshot always
  // enters/leaves in S, including its first use; no one-shot initial-state
  // shortcut may be baked into a reusable command list.
  move(l,src,state,CS);move(l,dst.Get(),S,CD);l->CopyResource(dst.Get(),src);move(l,src,CS,state);move(l,dst.Get(),CD,S);return true;
 }
 void bind(ID3D12GraphicsCommandList* l,UINT pass,ID3D12Resource* pic,DXGI_FORMAT pf){
  auto cpu=heap->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=SIZE_T(descriptor_page*39+pass*13)*program->inc;
  // A discovery probe can precede Prepare (menus have no depth/motion).
  // Retained counterfactuals then have no layer_view and remain RT, while a
  // previously prepared picture's input/mask are NPSR. Bind only this entry
  // point's actual inputs/outputs; unused table slots get typed null views.
  // Keeping a resource alive does not make its view/state valid in every pass.
  const bool verify=pass==0,prepare=pass==1,compose=pass==2;
  const bool model=(constants.flags&64u)!=0,native=(constants.flags&8u)!=0,seed=(constants.flags&48u)!=0;
  ID3D12Resource* srvs[]={
   !seed?frame.Get():nullptr,
   (verify||(compose&&native))&&full_current?full.Get():nullptr,
   (prepare||(verify&&model)||(compose&&(seed||native)))&&scene_current?scene.Get():nullptr,
   (prepare||(compose&&!seed))?n[0].Get():nullptr,
   (prepare||(verify&&native)||(compose&&!seed&&!native))?n[1].Get():nullptr,
   compose&&!native&&(constants.flags&32u)==0?pic:nullptr,
   verify&&model?low[0].Get():nullptr,verify&&model?low[1].Get():nullptr};
  DXGI_FORMAT formats[]={frame_view,frame_view,frame_view,layer_view,layer_view,pf,layer_view,layer_view};
  for(UINT i=0;i<8;++i){D3D12_SHADER_RESOURCE_VIEW_DESC v{};v.Format=srvs[i]?formats[i]:DXGI_FORMAT_R8G8B8A8_UNORM;
   v.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;v.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;v.Texture2D.MipLevels=1;
   program->dev->CreateShaderResourceView(srvs[i],&v,cpu);cpu.ptr+=program->inc;}
  ID3D12Resource* uavs[]={prepare?input.Get():nullptr,prepare?mask.Get():nullptr,prepare?motion.Get():nullptr,compose?result.Get():nullptr};
  DXGI_FORMAT uf[]={DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R16_FLOAT,DXGI_FORMAT_R32G32_FLOAT,result_format};
  for(UINT i=0;i<4;++i){D3D12_UNORDERED_ACCESS_VIEW_DESC v{};v.Format=uavs[i]?uf[i]:DXGI_FORMAT_R16G16B16A16_FLOAT;v.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
   program->dev->CreateUnorderedAccessView(uavs[i],nullptr,&v,cpu);cpu.ptr+=program->inc;}
  D3D12_UNORDERED_ACCESS_VIEW_DESC v{};v.Format=DXGI_FORMAT_R32_TYPELESS;v.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;v.Buffer.NumElements=4;v.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;
  v.Buffer.NumElements=UINT(proof->GetDesc().Width/4);program->dev->CreateUnorderedAccessView(proof.Get(),nullptr,&v,cpu);
  ID3D12DescriptorHeap* heaps[]={heap.Get()};l->SetDescriptorHeaps(1,heaps);l->SetComputeRootSignature(program->root.Get());l->SetPipelineState(program->pso[pass].Get());
  auto gpu=heap->GetGPUDescriptorHandleForHeapStart();gpu.ptr+=UINT64(descriptor_page*39+pass*13)*program->inc;
  l->SetComputeRootDescriptorTable(0,gpu);l->SetComputeRoot32BitConstants(1,8,&constants,0);
 }
};
Picture::Picture():p_(std::make_unique<Impl>()){}
Picture::~Picture()=default;
std::shared_ptr<Picture> Picture::Create(ID3D12Device* dev,const D3D12_RESOURCE_DESC& d,uint32_t ow,uint32_t oh){
 if(!dev||d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||d.SampleDesc.Count!=1||d.DepthOrArraySize!=1||d.MipLevels!=1||!ow||!oh)return {};
 auto result=std::make_shared<Picture>();auto& p=*result->p_;p.program=program(dev);if(!p.program)return {};
 p.w=UINT(d.Width);p.h=d.Height;p.ow=ow;p.oh=oh;p.source_format=d.Format;
 if(!texture(dev,p.w,p.h,d.Format,false,S,p.scene)||!texture(dev,p.w,p.h,d.Format,false,S,p.full)||
    !texture(dev,p.w,p.h,DXGI_FORMAT_R16G16B16A16_FLOAT,true,U,p.input)||!texture(dev,p.w,p.h,DXGI_FORMAT_R16_FLOAT,true,U,p.mask)||
    !buffer(dev,D3D12_HEAP_TYPE_DEFAULT,U,p.proof,16+UINT64(p.w)*p.h*4)||!buffer(dev,D3D12_HEAP_TYPE_READBACK,CD,p.readback))return {};
 D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,78,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
 if(FAILED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&p.heap))))return {};return result;
}
bool Picture::Matches(ID3D12Device* d,const D3D12_RESOURCE_DESC& s,uint32_t ow,uint32_t oh)const{return p_->program->dev.Get()==d&&p_->w==s.Width&&p_->h==s.Height&&p_->source_format==s.Format&&p_->ow==ow&&p_->oh==oh;}
void Picture::BeginCycle(){
 // Present uses page1. The UI-only path deliberately skips Prepare, so it
 // cannot rely on Prepare selecting page0 for the next pre-HUD recording.
 p_->descriptor_page=0;
 p_->prepared=p_->scene_current=p_->full_current=p_->proof_read=false;p_->frame.Reset();p_->motion.Reset();p_->n[0].Reset();p_->n[1].Reset();p_->low[0].Reset();p_->low[1].Reset();
}
bool Picture::Prepared()const{return p_->prepared;}
std::shared_ptr<Picture> Picture::ForkSnapshot(ID3D12GraphicsCommandList* l){
 auto& p=*p_;if(!l||!p.scene_current||!p.full_current)return {};
 std::shared_ptr<Picture> result;
 for(const auto& r:p.readers)if(r.use_count()==1){result=r;result->BeginCycle();break;}
 if(!result){if(p.readers.size()>=8)return {};result=Create(p.program->dev.Get(),p.scene->GetDesc(),p.ow,p.oh);if(!result)return {};p.readers.push_back(result);}
 if(!result->SnapshotScene(l,p.scene.Get(),S)||!result->SnapshotFull(l,p.full.Get(),S))return {};
 result->SetValidationLayers(p.low[0].Get(),p.low[1].Get(),p.source_srgb);return result;
}
bool Picture::SnapshotScene(ID3D12GraphicsCommandList* l,ID3D12Resource* r,D3D12_RESOURCE_STATES s){return p_->scene_current=p_->snapshot(l,r,s,p_->scene);}
bool Picture::SnapshotFull(ID3D12GraphicsCommandList* l,ID3D12Resource* r,D3D12_RESOURCE_STATES s){return p_->full_current=p_->snapshot(l,r,s,p_->full);}
void Picture::SetValidationLayers(ID3D12Resource* low,ID3D12Resource* high,bool source_srgb){p_->low[0]=low;p_->low[1]=high;p_->source_srgb=source_srgb;}
bool Picture::Probe(ID3D12GraphicsCommandList* l,ID3D12Resource* frame,DXGI_FORMAT view){
 auto& p=*p_;if(!l||!frame||!p.full_current)return false;
 const auto d=frame->GetDesc();if(d.Width!=p.w||d.Height!=p.h||
  TypelessOf(d.Format)!=TypelessOf(p.source_format)||TypelessOf(view)!=TypelessOf(p.source_format))return false;
 // GetDesc may supply a typeless allocation format. The comparison reads its
 // stored colour through the corresponding typed view, never TYPELESS/UNKNOWN.
 switch(view){
 case DXGI_FORMAT_R8G8B8A8_TYPELESS:view=DXGI_FORMAT_R8G8B8A8_UNORM;break;
 case DXGI_FORMAT_B8G8R8A8_TYPELESS:view=DXGI_FORMAT_B8G8R8A8_UNORM;break;
 case DXGI_FORMAT_R10G10B10A2_TYPELESS:view=DXGI_FORMAT_R10G10B10A2_UNORM;break;
 case DXGI_FORMAT_R16G16B16A16_TYPELESS:view=DXGI_FORMAT_R16G16B16A16_FLOAT;break;
 default:break;
 }
 p.frame=frame;p.frame_view=view;p.descriptor_page=0;p.proof_read=false;
 if(!p.result)p.result_format=DXGI_FORMAT_R16G16B16A16_FLOAT; // typed null UAV; retained real outputs keep their actual format
 const float tolerance=view==DXGI_FORMAT_R16G16B16A16_FLOAT?.0005f:view==DXGI_FORMAT_R10G10B10A2_UNORM?2.f/1023.f:2.f/255.f;
 p.constants={p.w,p.h,p.ow,p.oh,0,1.f,tolerance,0};
 move(l,p.proof.Get(),U,CD);l->CopyBufferRegion(p.proof.Get(),0,p.program->zero.Get(),0,16);move(l,p.proof.Get(),CD,U);
 p.bind(l,0,nullptr,DXGI_FORMAT_UNKNOWN);l->Dispatch((p.w+7)/8,(p.h+7)/8,1);order(l);
 move(l,p.proof.Get(),U,CS);l->CopyBufferRegion(p.readback.Get(),0,p.proof.Get(),0,16);move(l,p.proof.Get(),CS,U);
 p.prepared=true;return true;
}
bool Picture::Prepare(ID3D12GraphicsCommandList* l,ID3D12Resource* frame,DXGI_FORMAT fv,ID3D12Resource* motion,
 ID3D12Resource* n0,ID3D12Resource* n1,DXGI_FORMAT lv,float grey,bool srgb,bool clean,DXGI_FORMAT output_format,bool tint){
 auto& p=*p_;if(!l||!frame||!motion||!n0||!n1||!p.scene_current||!p.full_current||p.prepared||!std::isfinite(grey)||grey<=0)return false;
 auto fd=frame->GetDesc(),md=motion->GetDesc(),nd=n0->GetDesc();
 if(fd.Width!=p.w||fd.Height!=p.h||md.Width!=p.w||md.Height!=p.h||nd.Width!=p.ow||nd.Height!=p.oh)return false;
 if((!p.result||p.result_format!=output_format)&&!texture(p.program->dev.Get(),p.ow,p.oh,output_format,true,U,p.result))return false;
 p.result_format=output_format;p.frame=frame;p.motion=motion;p.n[0]=n0;p.n[1]=n1;p.frame_view=fv;p.layer_view=lv;p.descriptor_page=0;
 const float tolerance=fv==DXGI_FORMAT_R16G16B16A16_FLOAT?.0005f:fv==DXGI_FORMAT_R10G10B10A2_UNORM?2.f/1023.f:2.f/255.f;
 const bool model=p.low[0]&&p.low[1];
 if(model)for(const auto& low:p.low){const auto ld=low->GetDesc();
  if(ld.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||ld.Width!=p.w||ld.Height!=p.h||ld.DepthOrArraySize!=1||ld.MipLevels!=1||
     ld.SampleDesc.Count!=1||TypelessOf(ld.Format)!=TypelessOf(lv))return false;}
 const bool floating=fv==DXGI_FORMAT_R16G16B16A16_FLOAT||fv==DXGI_FORMAT_R32G32B32A32_FLOAT;
 p.constants={p.w,p.h,p.ow,p.oh,(clean?1u:0u)|(srgb?2u:0u)|(tint?4u:0u)|(model?64u:0u)|(floating?128u:0u)|(p.source_srgb?256u:0u),grey,tolerance,0};
 if(p.outputs_read){move(l,p.input.Get(),S,U);move(l,p.mask.Get(),S,U);}
 move(l,n0,R,S);move(l,n1,R,S);
 if(model){move(l,p.low[0].Get(),R,S);move(l,p.low[1].Get(),R,S);}
 move(l,p.proof.Get(),U,CD);l->CopyBufferRegion(p.proof.Get(),0,p.program->zero.Get(),0,16);move(l,p.proof.Get(),CD,U);
 p.bind(l,0,nullptr,DXGI_FORMAT_UNKNOWN);l->Dispatch((p.w+7)/8,(p.h+7)/8,1);order(l);
 move(l,motion,S,U);
 p.bind(l,1,nullptr,DXGI_FORMAT_UNKNOWN);l->Dispatch((p.w+7)/8,(p.h+7)/8,1);order(l);
 move(l,p.proof.Get(),U,CS);l->CopyBufferRegion(p.readback.Get(),0,p.proof.Get(),0,16);move(l,p.proof.Get(),CS,U);
 move(l,n0,S,R);move(l,n1,S,R);move(l,motion,U,S);move(l,p.input.Get(),U,S);move(l,p.mask.Get(),U,S);
 if(model){move(l,p.low[0].Get(),S,R);move(l,p.low[1].Get(),S,R);}
 p.prepared=p.outputs_read=true;return true;
}
ID3D12Resource* Picture::Input()const{return p_->input.Get();}
ID3D12Resource* Picture::Mask()const{return p_->mask.Get();}
ID3D12Resource* Picture::Scene()const{return p_->scene.Get();}
ID3D12Resource* Picture::Full()const{return p_->full.Get();}
bool Picture::ProofAfterCompletion(bool& identity,bool& clean,ProofReceipt* receipt){
 auto& p=*p_;if(!p.prepared||p.proof_read)return false;void* data=nullptr;D3D12_RANGE range{0,16};
 if(FAILED(p.readback->Map(0,&range,&data)))return false;UINT values[4]{};std::memcpy(values,data,16);const UINT flags=values[0];
 if(receipt){receipt->tested=p.w*p.h;receipt->matched=values[1];receipt->picture_matched=values[3];std::memcpy(&receipt->maximum_error,values+2,4);}
 D3D12_RANGE none{};p.readback->Unmap(0,&none);
 identity=flags==0;clean=identity&&(p.constants.flags&9u)!=0;p.proof_read=true;return true;
}
bool Picture::Composite(ID3D12GraphicsCommandList* l,ID3D12Resource* out){
 auto& p=*p_;if(!p.prepared||!l||!out)return false;const auto d=out->GetDesc();
 if(d.Width!=p.ow||d.Height!=p.oh||d.Format!=p.result_format||!p.result)return false;
 move(l,out,U,S);move(l,p.n[0].Get(),R,S);move(l,p.n[1].Get(),R,S);
 p.bind(l,2,out,d.Format);l->Dispatch((p.ow+7)/8,(p.oh+7)/8,1);order(l);
 move(l,out,S,CD);move(l,p.result.Get(),U,CS);l->CopyResource(out,p.result.Get());move(l,p.result.Get(),CS,U);move(l,out,CD,U);
 move(l,p.n[0].Get(),S,R);move(l,p.n[1].Get(),S,R);return true;
}
bool Picture::SeedNativeAlpha(ID3D12GraphicsCommandList* l,ID3D12Resource* out){
 auto& p=*p_;if(!l||!out||!p.scene_current)return false;
 const auto d=out->GetDesc();if(d.Width!=p.ow||d.Height!=p.oh)return false;
 if(p.result&&p.result_format!=d.Format)return false;
 if(!p.result&&!texture(p.program->dev.Get(),p.ow,p.oh,d.Format,true,U,p.result))return false;
 p.result_format=d.Format;
 const auto saved=p.constants;const auto saved_view=p.frame_view;auto saved_n0=p.n[0],saved_n1=p.n[1];p.n[0].Reset();p.n[1].Reset();
 if(p.frame_view==DXGI_FORMAT_UNKNOWN)p.frame_view=p.source_format;
 p.constants={p.w,p.h,p.ow,p.oh,16,1,0,0};
 move(l,out,U,S);p.bind(l,2,out,d.Format);l->Dispatch((p.ow+7)/8,(p.oh+7)/8,1);order(l);
 move(l,out,S,CD);move(l,p.result.Get(),U,CS);l->CopyResource(out,p.result.Get());move(l,p.result.Get(),CS,U);move(l,out,CD,U);
 p.constants=saved;p.frame_view=saved_view;p.n[0]=std::move(saved_n0);p.n[1]=std::move(saved_n1);return true;
}
bool Picture::SeedNativeScene(ID3D12GraphicsCommandList* l,ID3D12Resource* out,DXGI_FORMAT source_view){
 auto& p=*p_;if(!l||!out||!p.scene_current)return false;
 const auto d=out->GetDesc();if(d.Width!=p.ow||d.Height!=p.oh)return false;
 if(p.result&&p.result_format!=d.Format)return false;
 if(!p.result&&!texture(p.program->dev.Get(),p.ow,p.oh,d.Format,true,U,p.result))return false;
 p.result_format=d.Format;const auto saved=p.constants;const auto saved_view=p.frame_view;auto saved_n0=p.n[0],saved_n1=p.n[1];p.n[0].Reset();p.n[1].Reset();
 // Both buffers are storage encoded: use the corresponding UNORM view, not
 // an sRGB decode, when merely seeding the transparent UI canvas.
 p.frame_view=source_view==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB?DXGI_FORMAT_R8G8B8A8_UNORM:
              source_view==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB?DXGI_FORMAT_B8G8R8A8_UNORM:source_view;
 p.constants={p.w,p.h,p.ow,p.oh,32,1,0,0};
 p.bind(l,2,nullptr,DXGI_FORMAT_UNKNOWN);l->Dispatch((p.ow+7)/8,(p.oh+7)/8,1);order(l);
 move(l,out,S,CD);move(l,p.result.Get(),U,CS);l->CopyResource(out,p.result.Get());move(l,p.result.Get(),CS,U);
 move(l,out,CD,D3D12_RESOURCE_STATE_PRESENT);p.constants=saved;p.frame_view=saved_view;p.n[0]=std::move(saved_n0);p.n[1]=std::move(saved_n1);return true;
}
bool Picture::PresentNative(ID3D12GraphicsCommandList* l,ID3D12Resource* frame,DXGI_FORMAT view,
                            ID3D12Resource* native,ID3D12Resource* check,ID3D12Resource* out,bool tint){
 auto& p=*p_;if(!l||!frame||!native||!check||!out||!p.full_current||!p.scene_current)return false;
 const auto fd=frame->GetDesc(),nd=native->GetDesc(),cd=check->GetDesc(),od=out->GetDesc();
 if(fd.Width!=p.w||fd.Height!=p.h||cd.Width!=p.w||cd.Height!=p.h||nd.Width!=p.ow||nd.Height!=p.oh||
    od.Width!=p.ow||od.Height!=p.oh||TypelessOf(fd.Format)!=TypelessOf(p.source_format)||
    TypelessOf(cd.Format)!=TypelessOf(view)||TypelessOf(nd.Format)!=TypelessOf(view))return false;
 if(p.result&&p.result_format!=od.Format)return false; // prior seed can still be in flight
 if(!p.result&&!texture(p.program->dev.Get(),p.ow,p.oh,od.Format,true,U,p.result))return false;
 p.result_format=od.Format;p.frame=frame;p.frame_view=p.layer_view=view;p.n[0]=native;p.n[1]=check;p.motion.Reset();
 const float tolerance=view==DXGI_FORMAT_R16G16B16A16_FLOAT?.0005f:view==DXGI_FORMAT_R10G10B10A2_UNORM?2.f/1023.f:2.f/255.f;
 p.constants={p.w,p.h,p.ow,p.oh,8u|(tint?4u:0u),1,tolerance,0};p.proof_read=false;p.descriptor_page=1;
 move(l,p.proof.Get(),U,CD);l->CopyBufferRegion(p.proof.Get(),0,p.program->zero.Get(),0,16);move(l,p.proof.Get(),CD,U);
 p.bind(l,0,nullptr,DXGI_FORMAT_UNKNOWN);l->Dispatch((p.w+7)/8,(p.h+7)/8,1);order(l);
 p.bind(l,2,nullptr,DXGI_FORMAT_UNKNOWN);l->Dispatch((p.ow+7)/8,(p.oh+7)/8,1);order(l);
 move(l,out,U,CD);move(l,p.result.Get(),U,CS);l->CopyResource(out,p.result.Get());move(l,p.result.Get(),CS,U);move(l,out,CD,U);
 move(l,p.proof.Get(),U,CS);l->CopyBufferRegion(p.readback.Get(),0,p.proof.Get(),0,16);move(l,p.proof.Get(),CS,U);
 p.prepared=true;return true;
}
}
