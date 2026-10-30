// Real DX12 execution, deterministic stand-in for the NR model. All waits and
// readbacks are test-only. This proves placement/lifetimes, not model quality.
#include "test_support.hpp"
#include "fake_ngx_params.hpp"
#include "host_dx12/nr/native_fg.hpp"
#include "host_dx12/nr/fg_states.hpp"
#include "host_dx12/observe/producers.hpp"
#include "host_dx12/settings.hpp"
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <DirectXPackedVector.h>
#include <array>
#include <vector>
#include <cmath>
#include <set>
#include <thread>

namespace {
using test::ComPtr;
using Pixel=std::array<float,4>;
test::Device d;
ComPtr<ID3D12RootSignature> root;
ComPtr<ID3D12PipelineState> pso;
std::vector<ComPtr<ID3D12DescriptorHeap>> heaps;
uint32_t creates=0,releases=0,evaluates=0;
std::set<uint32_t> alive;
std::set<int*> handles;
std::set<test::FakeParams*> allocations;
uint32_t reentrant_forwards=0;
void STDMETHODCALLTYPE ReentrantForward(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*){++reentrant_forwards;}
float delta=.125f;
bool fail=false;
bool check_busy_status=false;
int core,fg;
constexpr uint32_t W=8,H=4;
auto ReadState=D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
D3D12_COMMAND_LIST_TYPE list_type=D3D12_COMMAND_LIST_TYPE_DIRECT;
bool native_identity=true,legacy_pair=false;
int32_t __cdecl Alloc(void** p){auto* v=new test::FakeParams;*p=v;allocations.insert(v);return 1;}
int32_t __cdecl Destroy(void* p){allocations.erase(static_cast<test::FakeParams*>(p));delete static_cast<test::FakeParams*>(p);return 1;}
int32_t __cdecl Create(void*,uint32_t feature,void*,void** p){CHECK(feature==18);auto* h=new int(++creates);*p=h;alive.insert(creates);handles.insert(h);
    // A runtime's internal submission must not recursively lock the NR mutex.
    std::thread helper([]{na::nr::Execute(d.queue.Get(),0,nullptr,ReentrantForward);});helper.join();return 1;}
int32_t __cdecl Release(void* p){++releases;handles.erase(static_cast<int*>(p));alive.erase(*static_cast<int*>(p));delete static_cast<int*>(p);return 1;}
int32_t __cdecl Eval(void* list,const void*,const void* params,void*) {
    ++evaluates;
    if(check_busy_status){
        // Before holds the native context mutex while calling the model.
        // A simultaneous overlay poll must retain the last active snapshot.
        std::thread poll([]{const auto s=na::nr::native::GetStatus();CHECK(s.active==1);CHECK(s.evaluates>0);});poll.join();
    }
    if(fail)return static_cast<int32_t>(NVSDK_NGX_Result_Fail);
    auto* p=static_cast<const test::FakeParams*>(params);
    auto* l=static_cast<ID3D12GraphicsCommandList*>(list);
    ID3D12Resource *in=nullptr,*out=nullptr,*depth=nullptr;
    p->Get("DLSSNR.Color",&in);p->Get("DLSSNR.Output",&out);p->Get("DLSSNR.Depth",&depth);
    CHECK(in && out && depth);CHECK(out->GetDesc().Format==DXGI_FORMAT_R32G32B32A32_FLOAT);
    unsigned int x=0,y=0,w=0,h=0,dw=0;
    p->Get("DLSSNR.ColorSubrectBaseX",&x);p->Get("DLSSNR.ColorSubrectBaseY",&y);
    p->Get("DLSSNR.OutputSubrectWidth",&w);p->Get("DLSSNR.OutputSubrectHeight",&h);
    p->Get("DLSSNR.DepthSubrectWidth",&dw);CHECK(dw==3);
    float sx=0,sy=0;p->Get("DLSSNR.MVecScaleX",&sx);p->Get("DLSSNR.MVecScaleY",&sy);
    CHECK(std::fabs(sx-w/3.f)<.00001f);CHECK(std::fabs(sy-h/2.f)<.00001f);
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,2,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
    ComPtr<ID3D12DescriptorHeap> heap;CHECK(SUCCEEDED(d.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap))));
    auto cpu=heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=in->GetDesc().Format;sd.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels=1;sd.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.device->CreateShaderResourceView(in,&sd,cpu);cpu.ptr+=d.device->GetDescriptorHandleIncrementSize(hd.Type);
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};ud.Format=out->GetDesc().Format;ud.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    d.device->CreateUnorderedAccessView(out,nullptr,&ud,cpu);
    struct C{uint32_t x,y,w,h;float delta;} c{x,y,w,h,delta};
    ID3D12DescriptorHeap* hs[]={heap.Get()};l->SetDescriptorHeaps(1,hs);
    l->SetComputeRootSignature(root.Get());l->SetPipelineState(pso.Get());
    l->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());
    l->SetComputeRoot32BitConstants(1,5,&c,0);l->Dispatch((w+7)/8,(h+7)/8,1);heaps.push_back(heap);
    return 1;
}
na::nr::Runtime Runtime(){return {reinterpret_cast<void*>(Create),reinterpret_cast<void*>(Eval),reinterpret_cast<void*>(Release),reinterpret_cast<void*>(Alloc),reinterpret_cast<void*>(Destroy),nullptr};}
void STDMETHODCALLTYPE Forward(ID3D12CommandQueue* q,UINT n,ID3D12CommandList* const* lists){q->ExecuteCommandLists(n,lists);}
int Prepare() {
    D3D12_DESCRIPTOR_RANGE ranges[]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,1}};
    D3D12_ROOT_PARAMETER rp[2]{};rp[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;rp[0].DescriptorTable={2,ranges};
    rp[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;rp[1].Constants={0,0,5};
    D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=2;rd.pParameters=rp;
    ComPtr<ID3DBlob> blob,err;REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&err)));
    REQUIRE(SUCCEEDED(d.device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root))));
    const char* shader="Texture2D<float4> t:register(t0); RWTexture2D<float4> o:register(u0); cbuffer C:register(b0){uint x,y,w,h;float delta;} [numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID){if(id.x<w&&id.y<h){float4 a=t[id.xy+uint2(x,y)];o[id.xy]=float4(a.rgb+delta,a.a);}}";
    ComPtr<ID3DBlob> code;REQUIRE(SUCCEEDED(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&code,&err)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={code->GetBufferPointer(),code->GetBufferSize()};
    REQUIRE(SUCCEEDED(d.device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pso))));return 0;
}
struct Image{ComPtr<ID3D12Resource> texture,upload,readback;D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};uint32_t x,y;DXGI_FORMAT format;};
uint32_t Bytes(DXGI_FORMAT f){return f==DXGI_FORMAT_R32G32B32A32_FLOAT?16:f==DXGI_FORMAT_R16G16B16A16_FLOAT?8:4;}
void Encode(void* dest,Pixel p,DXGI_FORMAT f){
    if(f==DXGI_FORMAT_R32G32B32A32_FLOAT)memcpy(dest,p.data(),16);
    else if(f==DXGI_FORMAT_R16G16B16A16_FLOAT){auto* v=static_cast<uint16_t*>(dest);for(int i=0;i<4;++i)v[i]=DirectX::PackedVector::XMConvertFloatToHalf(p[i]);}
    else {uint32_t v=static_cast<uint32_t>(std::lround(p[0]*1023))|(static_cast<uint32_t>(std::lround(p[1]*1023))<<10)|(static_cast<uint32_t>(std::lround(p[2]*1023))<<20)|(static_cast<uint32_t>(std::lround(p[3]*3))<<30);memcpy(dest,&v,4);}
}
Pixel Decode(const void* data,DXGI_FORMAT f){Pixel p{};if(f==DXGI_FORMAT_R32G32B32A32_FLOAT)memcpy(p.data(),data,16);
    else if(f==DXGI_FORMAT_R16G16B16A16_FLOAT){auto* v=static_cast<const uint16_t*>(data);for(int i=0;i<4;++i)p[i]=DirectX::PackedVector::XMConvertHalfToFloat(v[i]);}
    else {uint32_t v;memcpy(&v,data,4);p={float(v&1023)/1023,float((v>>10)&1023)/1023,float((v>>20)&1023)/1023,float(v>>30)/3};}return p;}
int Make(test::List& list,Image& im,uint32_t bx,uint32_t by,uint32_t w,bool hud,DXGI_FORMAT format) {
    im.x=bx;im.y=by;im.format=format;
    im.texture=test::CreateTexture(d.device.Get(),w+4,H+4,format,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE);REQUIRE(im.texture);
    auto desc=im.texture->GetDesc();UINT64 total=0;d.device->GetCopyableFootprints(&desc,0,1,0,&im.fp,nullptr,nullptr,&total);
    im.upload=test::CreateBuffer(d.device.Get(),total,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    im.readback=test::CreateBuffer(d.device.Get(),total,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    void* data=nullptr;REQUIRE(SUCCEEDED(im.upload->Map(0,nullptr,&data)));
    for(uint32_t y=0;y<desc.Height;++y)for(uint32_t x=0;x<desc.Width;++x){
        Pixel p{.875f,.875f,.875f,1};if(x>=bx&&x<bx+w&&y>=by&&y<by+H)p={.25f,.25f,.25f,1};
        if(hud&&x==bx&&y>=by&&y<by+H)p={.5f,.5f,.5f,1};
        Encode(static_cast<char*>(data)+y*im.fp.Footprint.RowPitch+x*Bytes(format),p,format);
    }im.upload->Unmap(0,nullptr);
    D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=im.upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=im.fp;
    dst.pResource=im.texture.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    auto b=test::Transition(im.texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,ReadState);list.list->ResourceBarrier(1,&b);return 0;
}
void Read(test::List& list,Image& im,D3D12_RESOURCE_STATES state=ReadState){auto b=test::Transition(im.texture.Get(),state,D3D12_RESOURCE_STATE_COPY_SOURCE);list.list->ResourceBarrier(1,&b);
    D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=im.texture.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.pResource=im.readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=im.fp;
    list.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);std::swap(b.Transition.StateBefore,b.Transition.StateAfter);list.list->ResourceBarrier(1,&b);}
void CheckImage(Image& im,uint32_t w,bool hud,bool enhanced,bool padding=true){void* data=nullptr;CHECK(SUCCEEDED(im.readback->Map(0,nullptr,&data)));if(!data)return;
    auto desc=im.texture->GetDesc();bool all=true;
    for(uint32_t y=0;y<desc.Height;++y)for(uint32_t x=0;x<desc.Width;++x){if(!padding && !(x>=im.x&&x<im.x+w&&y>=im.y&&y<im.y+H))continue;float want=.875f;
        if(x>=im.x&&x<im.x+w&&y>=im.y&&y<im.y+H)want=x==im.x?(hud?.5f:.25f):enhanced?.375f:.25f;
        const auto got=Decode(static_cast<char*>(data)+y*im.fp.Footprint.RowPitch+x*Bytes(im.format),im.format);
        for(int c=0;c<3;++c)all &= std::fabs(got[c]-want)<.002f;all &= got[3]==1;
    }im.readback->Unmap(0,nullptr);CHECK(all);
}
void Params(test::FakeParams& p,Image& b,Image& h,ID3D12Resource* depth,ID3D12Resource* mv,uint64_t id,uint32_t w){
    p.Set("DLSSG.Backbuffer",b.texture.Get());p.Set("DLSSG.HUDLess",h.texture.Get());p.Set("DLSSG.Depth",depth);p.Set("DLSSG.MVecs",mv);
    if(native_identity)p.Set("DLSSG.BackbufferFrameID",static_cast<unsigned long long>(id));p.Set("DLSSG.ColorBuffersHDR",b.format==DXGI_FORMAT_R32G32B32A32_FLOAT?0u:1u);
    p.Set("DLSSG.MvecScaleX",1.f/3);p.Set("DLSSG.MvecScaleY",1.f/2);
    const char* names[]={"InputBackbuffer","HUDLess","Depth","MVecs"};const uint32_t xs[]={b.x,h.x,0,0},ys[]={b.y,h.y,0,0};
    for(int i=0;i<4;++i){char key[100];const char* suffix[]={"BaseX","BaseY","Width","Height"};const uint32_t v[]={xs[i],ys[i],i<2?w:3,i<2?H:2};
        for(int j=0;j<4;++j){snprintf(key,sizeof(key),"DLSSG.%sSubrect%s",names[i],suffix[j]);p.Set(key,v[j]);}}
}
int Frame(uint64_t id,uint32_t w= W,DXGI_FORMAT fmt=DXGI_FORMAT_R32G32B32A32_FLOAT,bool discard=false,bool backlog=false,bool enabled=true){
    test::List list;REQUIRE(list.Create(d.device.Get(),list_type));Image b,h;REQUIRE(Make(list,b,1,2,w,true,fmt)==0);REQUIRE(Make(list,h,2,1,w,false,fmt)==0);
    auto depth=test::CreateTexture(d.device.Get(),3,2,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_NONE);
    auto mv=test::CreateTexture(d.device.Get(),3,2,DXGI_FORMAT_R16G16_FLOAT,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_NONE);
    D3D12_RESOURCE_BARRIER guides[]={test::Transition(depth.Get(),D3D12_RESOURCE_STATE_COMMON,ReadState),test::Transition(mv.Get(),D3D12_RESOURCE_STATE_COMMON,ReadState)};
    const bool missing=id>=40 && id<=43;
    if(missing)for(auto& guide:guides)guide.Transition.StateAfter=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    list.list->ResourceBarrier(2,guides);
    test::FakeParams p;Params(p,b,h,depth.Get(),mv.Get(),id,w);na::nr::native::Prepare(list.list.Get());
    na::nr::native::MarkSwapchainBuffer(b.texture.Get());
    na::settings::Get().nr_enabled=enabled;const auto before=evaluates;
    if(id==0){
        // Diagnostic-only observations: do not submit these artificial barriers.
        using E=na::nr::states::Evidence;
        auto split=guides[0];split.Flags=D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY;
        na::nr::states::Observe(list.list.Get(),1,&split);
        na::nr::native::Before(list.list.Get(),&core,&fg,&p,Runtime());
        const auto missing_status=na::nr::native::GetStatus();
        CHECK(evaluates==before && !missing_status.input_states_known);
        CHECK(missing_status.input_evidence[0]==static_cast<unsigned>(E::Known));
        CHECK(missing_status.input_evidence[2]==static_cast<unsigned>(E::SplitPending));
        CHECK(missing_status.input_evidence[3]==static_cast<unsigned>(E::Known));
        CHECK(missing_status.input_states[2]==~0u && missing_status.color_state==static_cast<unsigned>(ReadState));
        na::nr::states::InvalidateEnhanced(list.list.Get());
        D3D12_RESOURCE_STATES observed{};
        CHECK(na::nr::states::Inspect(list.list.Get(),depth.Get(),observed)==E::EnhancedInvalidated);
        CHECK(static_cast<unsigned>(observed)==~0u);
        na::nr::states::Reset(list.list.Get());
        CHECK(na::nr::states::Inspect(list.list.Get(),depth.Get(),observed)==E::NoTransition);
        D3D12_RESOURCE_BARRIER colors[]={test::Transition(b.texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,ReadState),test::Transition(h.texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,ReadState)};
        na::nr::states::Observe(list.list.Get(),2,colors);
        na::nr::states::Observe(list.list.Get(),2,guides);
    }
    Image fg_hud;
    if(missing){
        // Only color transition is observed. HUD really remains ALL_SHADER_READ
        // on DIRECT; guides really remain NON_PIXEL. Never guess equality.
        na::nr::states::Reset(list.list.Get());
        auto color=test::Transition(b.texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,ReadState);
        na::nr::states::Observe(list.list.Get(),1,&color);
    }
    for(uint32_t m=1;m<=(legacy_pair?1u:3u);++m){
        if(!legacy_pair){p.Set("DLSSG.MultiFrameCount",3u);p.Set("DLSSG.MultiFrameIndex",m);}
        {
            na::nr::native::InputOverride override;
            na::nr::native::Before(list.list.Get(),&core,&fg,&p,Runtime(),&override);
            if(missing){
                ID3D12Resource* replacement=nullptr;p.Get("DLSSG.HUDLess",&replacement);
                CHECK(replacement && replacement!=h.texture.Get());
                const auto status=na::nr::native::GetStatus();
                CHECK(status.input_evidence[1]==static_cast<unsigned>(na::nr::states::Evidence::NgxReadContract));
                CHECK(status.input_states[1]==~0u); // permission to read, no fake exact state
                if(m==(legacy_pair?1u:3u)){
                    fg_hud.texture=replacement;fg_hud.x=h.x;fg_hud.y=h.y;fg_hud.format=h.format;fg_hud.fp=h.fp;
                    fg_hud.readback=test::CreateBuffer(d.device.Get(),h.readback->GetDesc().Width,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
                    Read(list,fg_hud,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                }
            }
        }
        ID3D12Resource* restored=nullptr;p.Get("DLSSG.HUDLess",&restored);CHECK(restored==h.texture.Get());
    }
    CHECK(evaluates==before+(enabled?1:0));
    if(enabled&&!fail)CHECK(na::nr::native::GetStatus().active);
    if(enabled){const auto status=na::nr::native::GetStatus();CHECK(status.backbuffer_is_swapchain && !status.output_real_present);
        CHECK(status.input_states_ready && status.input_states_known==!missing && status.color_state==static_cast<uint32_t>(ReadState) && status.hudless_state==(missing?~0u:static_cast<uint32_t>(ReadState)));}
    // Original FG sees the already-modified pair, on its own command list.
    Read(list,b);Read(list,h);REQUIRE(SUCCEEDED(list.list->Close()));
    if(discard){na::nr::OnCommandListReset(list.list.Get());na::nr::native::Maintain();return 0;}
    ComPtr<ID3D12Fence> gate;if(backlog){REQUIRE(SUCCEEDED(d.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate))));REQUIRE(SUCCEEDED(d.queue->Wait(gate.Get(),1)));}
    ID3D12CommandList* ls[]={list.list.Get()};na::nr::Execute(d.queue.Get(),1,ls,Forward);
    if(backlog){const auto current=creates;na::nr::native::Release(&core,&fg);na::nr::OnCommandListReset(list.list.Get());na::nr::native::Maintain();CHECK(alive.count(current)==1);REQUIRE(SUCCEEDED(gate->Signal(1)));}
    d.WaitIdle();CHECK(d.device->GetDeviceRemovedReason()==S_OK);
    if(!fail){CheckImage(b,w,true,enabled&&delta!=0);CheckImage(h,w,false,enabled&&delta!=0&&!missing);if(missing)CheckImage(fg_hud,w,false,enabled&&delta!=0,false);}
    na::nr::OnCommandListReset(list.list.Get());na::nr::native::Maintain();heaps.clear();return 0;
}
}
int main(){
    CHECK(na::nr::states::CanTransitionOn(D3D12_COMMAND_LIST_TYPE_COMPUTE,D3D12_RESOURCE_STATE_COMMON));
    CHECK(na::nr::states::CanTransitionOn(D3D12_COMMAND_LIST_TYPE_COMPUTE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    CHECK(!na::nr::states::CanTransitionOn(D3D12_COMMAND_LIST_TYPE_COMPUTE,D3D12_RESOURCE_STATE_RENDER_TARGET));
    CHECK(!na::nr::states::CanTransitionOn(D3D12_COMMAND_LIST_TYPE_COMPUTE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
    ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    if(!test::CreateDevice(d))return test::SkipNoDevice("nr_native_fg_test");ComPtr<ID3D12InfoQueue> info;d.device.As(&info);
    REQUIRE(na::nr::states::InstallEnhancedInvalidation());
    REQUIRE(na::observe::InstallListResetTracking(d.device.Get(),na::nr::OnCommandListReset,na::nr::states::Observe));
    REQUIRE(Prepare()==0);
    REQUIRE(Frame(0)==0); // zero identity, MFG evaluates NR once
    check_busy_status=true;
    CHECK(na::nr::native::GetStatus().gpu_samples>0);
    REQUIRE(Frame(1,W,DXGI_FORMAT_R32G32B32A32_FLOAT,true)==0); // discard and retry same identity
    check_busy_status=false;
    REQUIRE(Frame(1)==0);
    for(uint64_t i=2;i<24;++i)REQUIRE(Frame(i,i<18 || i%2?W:W+2)==0); // stable feature pool recycling, then quality/extent changes
    REQUIRE(Frame(24,W,DXGI_FORMAT_R32G32B32A32_FLOAT,false,false,false)==0);
    REQUIRE(Frame(25)==0); // off/on
    fail=true;REQUIRE(Frame(26)==0);fail=false;REQUIRE(Frame(26)==0); // failed recorded evaluation retires before retry
    delta=0;REQUIRE(Frame(27,W,DXGI_FORMAT_R16G16B16A16_FLOAT)==0);REQUIRE(Frame(28,W,DXGI_FORMAT_R10G10B10A2_UNORM)==0);
    delta=.125f;REQUIRE(Frame(29,W,DXGI_FORMAT_R32G32B32A32_FLOAT,false,true)==0); // release while GPU blocked
    na::nr::native::Release(&core,nullptr);na::nr::native::Maintain();CHECK(creates==releases);
    REQUIRE(Frame(30)==0);const auto before_shutdown=releases;
    na::nr::native::CoreShutdown(&core);CHECK(releases==before_shutdown);
    const auto orphaned=handles.size();for(auto* h:handles){alive.erase(*h);delete h;}handles.clear();
    for(auto* p:allocations)delete p;allocations.clear(); // fake core owns shutdown cleanup
    REQUIRE(Frame(0)==0); // fresh core/feature incarnation may restart frame IDs
    na::nr::native::Release(&core,nullptr);na::nr::native::Maintain();CHECK(creates==releases+orphaned);
    CHECK(allocations.empty() && handles.empty() && reentrant_forwards==creates);
    native_identity=false;na::nr::native::Register(&core,&fg);CHECK(na::nr::native::OwnsRoute());
    REQUIRE(Frame(0)==0);REQUIRE(Frame(1)==0); // missing optional ID, explicit MFG group
    na::nr::native::Release(&core,nullptr);
    REQUIRE(Frame(40,W,DXGI_FORMAT_R32G32B32A32_FLOAT,false,true)==0);na::nr::native::Release(&core,nullptr);
    list_type=D3D12_COMMAND_LIST_TYPE_COMPUTE;ReadState=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    d.WaitIdle();D3D12_COMMAND_QUEUE_DESC q{};q.Type=list_type;d.queue.Reset();
    REQUIRE(SUCCEEDED(d.device->CreateCommandQueue(&q,IID_PPV_ARGS(&d.queue))));
    na::nr::native::Register(&core,&fg);CHECK(na::nr::native::OwnsRoute());
    REQUIRE(Frame(0)==0);REQUIRE(Frame(1)==0); // PBZ compute list, no optional ID
    REQUIRE(Frame(41)==0);
    delta=0; // HDR round-trip: the fake additive model is defined in normalized SDR space
    REQUIRE(Frame(42,W,DXGI_FORMAT_R10G10B10A2_UNORM)==0);
    REQUIRE(Frame(43,W+2,DXGI_FORMAT_R16G16B16A16_FLOAT)==0);delta=.125f;na::nr::native::Release(&core,nullptr);
    legacy_pair=true;na::nr::native::Release(&core,nullptr);
    REQUIRE(Frame(2)==0);REQUIRE(Frame(3)==0); // pre-MFG API defaults, one pair/evaluate
    na::nr::native::Release(&core,nullptr);na::nr::native::Maintain();
    if(info)for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T n=0;info->GetMessage(i,nullptr,&n);std::vector<char> buf(n);auto* m=reinterpret_cast<D3D12_MESSAGE*>(buf.data());info->GetMessage(i,m,&n);
        if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::printf("D3D12: %s\n",m->pDescription);CHECK(false);}}
    return test::finish("nr_native_fg_test");
}
