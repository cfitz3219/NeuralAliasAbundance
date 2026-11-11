// Opt-in CPU-cost benchmark for the automatic no-slider scene (Layer2). Not a CI
// test: run `auto_scene_bench [--debug] [--seconds N]` by hand (default ~30 s of
// measurement; --serial records the 4 lists in submission order on one thread). Real DX12 recording on 4 threads, 3440x1440 targets, thousands
// of tiny scissored draws over ~500 distinct operation signatures and 16 material
// tables, with a concurrent worker polling at the production cadence
// (session.cpp: 10 ms while work is pending, else 250 ms). Phases: off (mode 0),
// native learning, stable built, 1->3->0->1 transitions, then one legitimately
// unplannable operation. Timings measure our callback machinery on a fake model,
// not game FPS. GPU waits are measured separately, outside recording time.
#include "test_support.hpp"
#include "fake_ngx_runtime.hpp"
#include "host_dx12/scale/auto_scene.hpp"
#include "host_dx12/j2/live_dlaa.hpp"
#include "host_dx12/observe/texture_operation.hpp"
#include "host_dx12/observe/producers.hpp"
#include "host_dx12/settings.hpp"
#include "host_dx12/nr/native_fg.hpp"
#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
namespace {
using Clock=std::chrono::steady_clock;
double ms(Clock::duration d){return std::chrono::duration<double,std::milli>(d).count();}
void __cdecl sink(const na_event_batch*,void*){}
using M=std::array<double,16>;
M camera(){const double n=.5,f=5000,sy=1/std::tan(.45),sx=sy/2;
    const M p{sx,0,0,0,0,sy,0,0,0,0,n/(n-f),n*f/(f-n),0,0,1,0};
    const double c=std::cos(.3),s=std::sin(.3);M v{c,0,-s,0,0,1,0,0,s,0,c,0,0,0,0,1};
    const double pos[3]={500,20,-40};for(int r=0;r<3;++r)v[r*4+3]=-(v[r*4]*pos[0]+v[r*4+1]*pos[1]+v[r*4+2]*pos[2]);
    M o{};for(int r=0;r<4;++r)for(int j=0;j<4;++j)for(int k=0;k<4;++k)o[r*4+j]+=p[r*4+k]*v[k*4+j];return o;
}
M inverse(const M& m){double a[4][8]{};for(int r=0;r<4;++r)for(int c=0;c<4;++c){a[r][c]=m[r*4+c];a[r][c+4]=r==c;}
    for(int c=0;c<4;++c){int p=c;for(int r=c+1;r<4;++r)if(std::abs(a[r][c])>std::abs(a[p][c]))p=r;
        for(int k=0;k<8;++k)std::swap(a[c][k],a[p][k]);const double d=a[c][c];for(double& v:a[c])v/=d;
        for(int r=0;r<4;++r)if(r!=c){const double f=a[r][c];for(int k=0;k<8;++k)a[r][k]-=f*a[c][k];}}
    M out{};for(int r=0;r<4;++r)for(int c=0;c<4;++c)out[r*4+c]=a[r][c+4];return out;
}
void store(char* p,const M& m){for(size_t i=0;i<16;++i){const float f=float(m[i]);std::memcpy(p+4*i,&f,4);}}
NVSDK_NGX_Result NVSDK_CONV model(ID3D12GraphicsCommandList* list,const NVSDK_NGX_Handle* handle,const NVSDK_NGX_Parameter* params,void* callback){
    return fake::Eval(list,handle,params,callback);
}
void __stdcall forward(ID3D12CommandQueue* q,UINT n,ID3D12CommandList* const* lists){
    na::nr::native::Submission retained(q,n,lists);q->ExecuteCommandLists(n,lists);
}
float half(uint16_t h){const uint32_t e=(h>>10)&31,m=h&1023;const float v=e?std::ldexp(float(m|1024),int(e)-25):std::ldexp(float(m),-24);return h&0x8000?-v:v;}
// UHD sizes are not exact in half precision (spacing 2 above 2048): compare within one step.
bool close_to(float v,float e){return std::abs(v-e)<=std::max(.0f,std::ldexp(1.f,int(std::floor(std::log2(std::abs(e))))-10));}
// Whole-target check, as auto_scene_test: x/y carry the size the final full-screen pass saw.
unsigned pixels(test::Device& device,ID3D12Resource* color,float expected_x,float expected_y,D3D12_RECT skip={0,0,0,0}){
    const na::observe::HostWorkScope own;const auto desc=color->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes=0;
    device.device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
    auto readback=test::CreateBuffer(device.device.Get(),bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);test::List copy;
    if(!readback||!copy.Create(device.device.Get()))return UINT_MAX;
    auto barrier=test::Transition(color,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);copy.list->ResourceBarrier(1,&barrier);
    D3D12_TEXTURE_COPY_LOCATION source{},destination{};source.pResource=color;source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.pResource=readback.Get();destination.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;destination.PlacedFootprint=footprint;
    copy.list->CopyTextureRegion(&destination,0,0,0,&source,nullptr);std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);copy.list->ResourceBarrier(1,&barrier);copy.list->Close();
    ID3D12CommandList* lists[]={copy.list.Get()};device.queue->ExecuteCommandLists(1,lists);device.WaitIdle();
    unsigned char* mapped=nullptr;if(FAILED(readback->Map(0,nullptr,reinterpret_cast<void**>(&mapped))))return UINT_MAX;unsigned wrong=0;
    for(UINT y=0;y<desc.Height;++y)for(UINT x=0;x<desc.Width;++x){const auto* p=reinterpret_cast<const uint16_t*>(mapped+footprint.Offset+y*footprint.Footprint.RowPitch+x*8);
        if(LONG(x)>=skip.left&&LONG(x)<skip.right&&LONG(y)>=skip.top&&LONG(y)<skip.bottom)continue; // a draw that is native by design
        const bool bad=!close_to(half(p[0]),expected_x)||!close_to(half(p[1]),expected_y)||half(p[2])!=9||half(p[3])!=.75f;
        if(bad&&!wrong)std::printf("  first wrong pixel (%u,%u): %g %g %g %g\n",x,y,half(p[0]),half(p[1]),half(p[2]),half(p[3]));wrong+=bad;}
    readback->Unmap(0,nullptr);std::printf("  pixels expected %g/%g: wrong %u\n",expected_x,expected_y,wrong);return wrong;
}
struct Summary{double mean=0,p99=0,max=0;size_t n=0;};
Summary summarize(std::vector<double> v){Summary s;s.n=v.size();if(v.empty())return s;std::sort(v.begin(),v.end());
    for(double x:v)s.mean+=x;s.mean/=double(v.size());s.p99=v[std::min(v.size()-1,size_t(std::ceil(.99*double(v.size())))-1)];s.max=v.back();return s;}
struct Phase{
    std::string name;std::vector<double> record_wall,record_sum,submit,gpu,present,poll;
    na::scale::AutomaticSceneStats before{},after{};
};
std::mutex poll_mutex;Phase* poll_phase=nullptr;
void report(const Phase& p){
    const auto line=[](const char* what,const std::vector<double>& v){const auto s=summarize(v);
        std::printf("  %-28s n=%-6zu mean %8.3f ms  p99 %8.3f ms  max %8.3f ms\n",what,s.n,s.mean,s.p99,s.max);};
    const auto& a=p.before;const auto& b=p.after;
    std::printf("== %s\n",p.name.c_str());
    line("recording frame (wall, 4 thr)",p.record_wall);line("recording CPU (sum of thr)",p.record_sum);
    line("submit (receipt+Execute)",p.submit);line("Present hooks",p.present);line("GPU wait (separate)",p.gpu);line("worker Poll call",p.poll);
    const auto d=[](uint64_t x,uint64_t y){return (unsigned long long)(y-x);};
    std::printf("  counters (delta): poll_builds %llu failed_plans %llu group_builds %llu generation %llu redirected %llu redirected_frames %llu\n",
        d(a.poll_builds,b.poll_builds),d(a.failed_plans,b.failed_plans),d(a.group_builds,b.group_builds),d(a.generation,b.generation),d(a.redirected,b.redirected),d(a.redirected_frames,b.redirected_frames));
    std::printf("  fast_hits %llu snapshots %llu misses %llu unknown %llu binding_fail %llu native_boundaries %llu transfer_fail %llu\n",
        d(a.fast_hits,b.fast_hits),d(a.snapshots,b.snapshots),d(a.misses,b.misses),d(a.unknown_operations,b.unknown_operations),d(a.binding_failures,b.binding_failures),d(a.native_boundaries,b.native_boundaries),d(a.transfer_failures,b.transfer_failures));
    std::printf("  worker_us %llu control wait %llu us hold %llu us max %llu us | end: active %d plans %u members %u render %ux%u: %s\n",
        d(a.worker_us,b.worker_us),d(a.control_wait_us,b.control_wait_us),d(a.control_hold_us,b.control_hold_us),(unsigned long long)b.control_max_us,
        int(b.active),b.plans,b.members,b.render_w,b.render_h,b.reason);
}
}
int main(int argc,char** argv){
    bool debug_layer=false,serial=false;double seconds=30;
    for(int i=1;i<argc;++i){if(!std::strcmp(argv[i],"--debug"))debug_layer=true;else if(!std::strcmp(argv[i],"--serial"))serial=true;else if(!std::strcmp(argv[i],"--seconds")&&i+1<argc)seconds=std::atof(argv[++i]);}
    const double unit=seconds/30.0; // phase budgets below sum to ~30 units
    setvbuf(stdout,nullptr,_IONBF,0);test::ComPtr<ID3D12Debug> debug;
    if(debug_layer&&SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    test::Device d;if(!test::CreateDevice(d,false))return test::SkipNoDevice("auto_scene_bench");auto* dev=d.device.Get();
    test::ComPtr<ID3D12InfoQueue> info;dev->QueryInterface(IID_PPV_ARGS(&info));
    REQUIRE(na::observe::InstallProducers(dev,d.queue.Get(),true));
    REQUIRE(na::observe::InstallListResetTracking(dev,&na::nr::native::Reset));
    na_observe_config cfg{};cfg.size_bytes=sizeof cfg;REQUIRE(na_observe_start(&cfg,sink,nullptr)==NA_OK);
    na::j2::SetEnabled(true);na::j2::SetJitter(false);na::settings::Get().j2_route.store(1);na::settings::Get().j2_scale_mode.store(0);na::settings::Get().j2_sharpness.store(0);
    na::scale::ConfigureAutomaticScene(true);
    na::dlss::SetInitOwnOverrideForTests([](ID3D12Device* device,const wchar_t*){auto api=fake::MakeApi();api.evaluate_feature=reinterpret_cast<void*>(&model);return na::dlss::Attach(device,api);});
    constexpr UINT w=3440,h=1440,threads=4,draws_per_thread=1000,signatures=500,tables=16,stride=256;
    constexpr UINT bad_slot=signatures+1,slots=signatures+2;
    auto color=test::CreateTexture(dev,w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto depth=test::CreateTexture(dev,w,h,DXGI_FORMAT_R32_TYPELESS,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    auto cb=test::CreateBuffer(dev,stride*slots,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);REQUIRE(color&&depth&&cb);
    auto color_copy=test::CreateTexture(dev,w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_NONE);
    auto depth_copy=test::CreateTexture(dev,w,h,DXGI_FORMAT_R32_TYPELESS,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_NONE);REQUIRE(color_copy&&depth_copy);
    // Slot 0: full-screen passes (marker 9). Slots 1..500: one per small-draw signature
    // (own CB offset and scissor). Slot 501: wrong named width, used only in the last phase.
    color->SetName(L"bench color");depth->SetName(L"bench depth");color_copy->SetName(L"bench color copy");depth_copy->SetName(L"bench depth copy");
    char* mapped=nullptr;REQUIRE(SUCCEEDED(cb->Map(0,nullptr,reinterpret_cast<void**>(&mapped))));std::memset(mapped,0,stride*slots);
    const auto vp=camera(),vpi=inverse(vp);
    for(UINT s=0;s<slots;++s){char* p=mapped+s*stride;float sizes[4]={s==bad_slot?w-1.f:float(w),float(h),s?9.f+float(s%7):9.f,0};std::memcpy(p,sizes,sizeof sizes);store(p+80,vp);store(p+144,vpi);}
    const std::string code=R"(
cbuffer View:register(b0){float2 ScreenSize;float marker,pad;float4 junk[4];float4x4 VP;float4x4 VPInv;};
Texture2D<float4> CopiedColor:register(t0);Texture2D<float> CopiedDepth:register(t1);Texture2D<float4> Material:register(t2);
float4 vs(uint i:SV_VertexID):SV_Position{return float4(float2((i<<1)&2,i&2)*2-1,.5,1);}
float4 ps():SV_Target{return float4(ScreenSize.xy,marker+Material.Load(int3(0,0,0)).x,.75);}
float4 water(float4 position:SV_Position):SV_Target{uint2 p=uint2(position.xy);float4 c=CopiedColor.Load(int3(p,0));float d=CopiedDepth.Load(int3(p,0));return float4(c.xy+float2(d,0),marker,.75);})";
    test::ComPtr<ID3DBlob> vs,ps,water,blob,error;
    REQUIRE(SUCCEEDED(D3DCompile(code.data(),code.size(),nullptr,nullptr,nullptr,"vs","vs_5_0",0,0,&vs,&error)));
    REQUIRE(SUCCEEDED(D3DCompile(code.data(),code.size(),nullptr,nullptr,nullptr,"ps","ps_5_0",0,0,&ps,&error)));
    REQUIRE(SUCCEEDED(D3DCompile(code.data(),code.size(),nullptr,nullptr,nullptr,"water","ps_5_0",0,0,&water,&error)));
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,3,0,0,0};D3D12_ROOT_PARAMETER rp[2]{};rp[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;rp[0].Descriptor={0,0};
    rp[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;rp[1].DescriptorTable={1,&range};
    D3D12_ROOT_SIGNATURE_DESC rs{};rs.NumParameters=2;rs.pParameters=rp;
    REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error)));
    test::ComPtr<ID3D12RootSignature> root;REQUIRE(SUCCEEDED(dev->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root))));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pd.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
    pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pd.RasterizerState.DepthClipEnable=TRUE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;pd.DepthStencilState.DepthEnable=TRUE;pd.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;pd.DepthStencilState.FrontFace=pd.DepthStencilState.BackFace={D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_COMPARISON_FUNC_ALWAYS};
    pd.SampleMask=UINT_MAX;pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;pd.NumRenderTargets=1;pd.RTVFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;pd.DSVFormat=DXGI_FORMAT_D32_FLOAT;pd.SampleDesc.Count=1;
    test::ComPtr<ID3D12PipelineState> pso,water_pso;REQUIRE(SUCCEEDED(dev->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&pso))));
    pd.PS={water->GetBufferPointer(),water->GetBufferSize()};REQUIRE(SUCCEEDED(dev->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&water_pso))));
    test::ComPtr<ID3D12DescriptorHeap> rt,ds;D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rt))));hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&ds))));
    const auto rtv=rt->GetCPUDescriptorHandleForHeapStart(),dsv=ds->GetCPUDescriptorHandleForHeapStart();dev->CreateRenderTargetView(color.Get(),nullptr,rtv);
    D3D12_DEPTH_STENCIL_VIEW_DESC dd{};dd.Format=DXGI_FORMAT_D32_FLOAT;dd.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;dev->CreateDepthStencilView(depth.Get(),&dd,dsv);
    // 16 tables of {copied color, copied depth, material}: material variations per draw.
    hd={D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,3*tables,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};test::ComPtr<ID3D12DescriptorHeap> texture_heap;REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&texture_heap))));
    const UINT inc=dev->GetDescriptorHandleIncrementSize(hd.Type);std::array<test::ComPtr<ID3D12Resource>,4> materials;
    for(auto& m:materials){m=test::CreateTexture(dev,4,4,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_NONE);REQUIRE(m);}
    for(UINT t=0;t<tables;++t){auto view=texture_heap->GetCPUDescriptorHandleForHeapStart();view.ptr+=SIZE_T(3*t)*inc;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
        dev->CreateShaderResourceView(color_copy.Get(),&srv,view);view.ptr+=inc;srv.Format=DXGI_FORMAT_R32_FLOAT;dev->CreateShaderResourceView(depth_copy.Get(),&srv,view);
        view.ptr+=inc;srv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;dev->CreateShaderResourceView(materials[t%materials.size()].Get(),&srv,view);}
    na::observe::SetLiveSelection(na::observe::IdOf(depth.Get()),na::observe::IdOf(color.Get()));
    std::array<test::List,threads> lists;for(UINT t=0;t<threads;++t){REQUIRE(lists[t].Create(dev));REQUIRE(SUCCEEDED(lists[t].list->Close()));lists[t].list->SetName((L"bench list "+std::to_wstring(t)).c_str());}
    const auto table=[&](UINT t){auto g=texture_heap->GetGPUDescriptorHandleForHeapStart();g.ptr+=UINT64(3*t)*inc;return g;};
    const auto address=[&](UINT slot){return cb->GetGPUVirtualAddress()+UINT64(slot)*stride;};
    const auto scissor_of=[&](UINT slot)->D3D12_RECT{const LONG x=LONG((slot%50)*64),y=LONG((slot/50)*128);return {x,y,x+8,y+8};};
    std::atomic<bool> bad{false};
    // One thread's list: shared state, its share of the small draws; thread 0 opens the
    // scene (clears + full-screen draw), thread 3 closes it (copies + full-screen read).
    const auto record=[&](UINT t,double& took){
        const auto start=Clock::now();auto& l=lists[t];l.Reset();auto* list=l.list.Get();
        if(t==0){D3D12_RESOURCE_BARRIER b[]={test::Transition(color.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET),test::Transition(depth.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_DEPTH_WRITE)};list->ResourceBarrier(2,b);}
        list->SetGraphicsRootSignature(root.Get());list->SetPipelineState(pso.Get());ID3D12DescriptorHeap* heap=texture_heap.Get();list->SetDescriptorHeaps(1,&heap);
        list->OMSetRenderTargets(1,&rtv,FALSE,&dsv);D3D12_VIEWPORT viewport{0,0,w,h,0,1};D3D12_RECT full{0,0,w,h};list->RSSetViewports(1,&viewport);list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if(t==0){const float clear[4]={0,0,0,0};list->ClearRenderTargetView(rtv,clear,0,nullptr);list->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,1,0,0,nullptr);
            list->RSSetScissorRects(1,&full);list->SetGraphicsRootConstantBufferView(0,address(0));list->SetGraphicsRootDescriptorTable(1,table(0));list->DrawInstanced(3,1,0,0);}
        for(UINT i=t*draws_per_thread;i<(t+1)*draws_per_thread;++i){const UINT slot=1+i%signatures;const auto r=scissor_of(slot);
            list->SetGraphicsRootConstantBufferView(0,address(slot));list->SetGraphicsRootDescriptorTable(1,table(i%tables));list->RSSetScissorRects(1,&r);list->DrawInstanced(3,1,0,0);}
        if(t==1&&bad.load()){const D3D12_RECT r{3300,1300,3308,1308};list->SetGraphicsRootConstantBufferView(0,address(bad_slot));list->SetGraphicsRootDescriptorTable(1,table(0));list->RSSetScissorRects(1,&r);list->DrawInstanced(3,1,0,0);}
        if(t==threads-1){
            D3D12_RESOURCE_BARRIER copies[]={test::Transition(color.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE),test::Transition(depth.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_COPY_SOURCE),
                test::Transition(color_copy.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST),test::Transition(depth_copy.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST)};
            list->ResourceBarrier(4,copies);list->CopyResource(color_copy.Get(),color.Get());list->CopyResource(depth_copy.Get(),depth.Get());
            for(auto& b:copies)std::swap(b.Transition.StateBefore,b.Transition.StateAfter);list->ResourceBarrier(4,copies);
            D3D12_RESOURCE_BARRIER reads[]={test::Transition(color_copy.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),test::Transition(depth_copy.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)};
            list->ResourceBarrier(2,reads);list->SetPipelineState(water_pso.Get());list->RSSetScissorRects(1,&full);list->SetGraphicsRootConstantBufferView(0,address(0));list->SetGraphicsRootDescriptorTable(1,table(0));list->DrawInstanced(3,1,0,0);
            for(auto& b:reads)std::swap(b.Transition.StateBefore,b.Transition.StateAfter);list->ResourceBarrier(2,reads);
            D3D12_RESOURCE_BARRIER finish[]={test::Transition(color.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON),test::Transition(depth.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_COMMON)};list->ResourceBarrier(2,finish);
        }
        if(FAILED(list->Close()))took=-1;else took=ms(Clock::now()-start);
    };
    // Production-cadence worker (session.cpp), timed per call into the current phase.
    std::atomic<bool> running{true};
    std::thread worker([&]{while(running.load()){const auto start=Clock::now();na::scale::PollAutomaticScene();const double took=ms(Clock::now()-start);
        {std::lock_guard<std::mutex> lock(poll_mutex);if(poll_phase)poll_phase->poll.push_back(took);}
        const auto interval=std::chrono::milliseconds(na::scale::AutomaticSceneWorkPending()?10:250);const auto until=Clock::now()+interval;
        while(running.load()&&Clock::now()<until)std::this_thread::sleep_for(std::chrono::milliseconds(1));}});
    bool record_failed=false;
    const auto frame=[&](Phase& p){
        const auto present=Clock::now();na::scale::AutomaticScenePresent();na::j2::OnPresent(d.queue.Get());p.present.push_back(ms(Clock::now()-present));
        std::array<double,threads> took{};std::vector<std::thread> pool;const auto wall=Clock::now();
        if(serial)for(UINT t=0;t<threads;++t)record(t,took[t]);else{for(UINT t=0;t<threads;++t)pool.emplace_back([&,t]{record(t,took[t]);});for(auto& th:pool)th.join();}
        p.record_wall.push_back(ms(Clock::now()-wall)); // includes thread start/join: an upper bound
        double sum=0;for(double x:took){if(x<0)record_failed=true;sum+=x;}p.record_sum.push_back(sum);
        ID3D12CommandList* submitted[threads];for(UINT t=0;t<threads;++t)submitted[t]=lists[t].list.Get();
        {const auto submit=Clock::now();na::scale::AutomaticSceneSubmission receipt(d.queue.Get(),threads,submitted);forward(d.queue.Get(),threads,submitted);p.submit.push_back(ms(Clock::now()-submit));
            const auto gpu=Clock::now();d.WaitIdle();p.gpu.push_back(ms(Clock::now()-gpu));}
    };
    std::vector<Phase> phases;phases.reserve(16);
    // Runs frames for `budget` units, or until `done` holds (then `settle` more units).
    const auto run=[&](const char* name,double budget,const auto& done,double settle){
        phases.emplace_back();auto& p=phases.back();p.name=name;p.before=na::scale::GetAutomaticSceneStats();
        {std::lock_guard<std::mutex> lock(poll_mutex);poll_phase=&p;}
        const auto start=Clock::now();double reached=-1;size_t frames_to=0;
        while(ms(Clock::now()-start)<budget*unit*1000){frame(p);if(reached<0&&done()){reached=ms(Clock::now()-start);frames_to=p.record_wall.size();
            if(ms(Clock::now()-start)+settle*unit*1000<budget*unit*1000)budget=(reached/1000+settle*unit)/unit;}}
        {std::lock_guard<std::mutex> lock(poll_mutex);poll_phase=nullptr;}
        p.after=na::scale::GetAutomaticSceneStats();report(p);
        if(info){unsigned errors=0;std::vector<std::string> seen;for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T size=0;info->GetMessage(i,nullptr,&size);std::vector<unsigned char> bytes(size);
                auto* message=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());if(SUCCEEDED(info->GetMessage(i,message,&size))&&message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::string text=message->pDescription;if(std::find(seen.begin(),seen.end(),text)==seen.end()&&seen.size()<6){seen.push_back(text);std::printf("  D3D12 error: %s\n",text.c_str());}++errors;CHECK(false);}}
            std::printf("  D3D12 errors in phase: %u\n",errors);info->ClearStoredMessages();}
        if(reached>=0)std::printf("  condition reached after %.0f ms (%zu frames)\n",reached,frames_to);else std::printf("  condition NOT reached\n");
        return reached>=0;
    };
    const auto mode=[&](int m){na::settings::Get().j2_scale_mode.store(m);};
    const auto stats=[]{return na::scale::GetAutomaticSceneStats();};
    const auto never=[]{return false;};
    std::printf("auto_scene_bench: %ux%u, %u recording threads x %u draws (+3 full-screen), %u signatures, %u material tables, budget %.0f s\n",w,h,threads,draws_per_thread,signatures,tables,seconds);
    // 1. Off: hooks installed, mode 0.
    run("off (mode 0)",4,never,0);CHECK(!stats().active);CHECK(pixels(d,color.Get(),w+.5f,h)==0);
    // 2. Native learning until active, then 3. stable built.
    mode(1);const bool learned=run("native learning (mode 0->1)",10,[&]{return stats().active&&stats().redirected_frames>phases.back().before.redirected_frames;},0);CHECK(learned);
    run("stable built (mode 1)",6,never,0);
    {const auto& p=phases.back();CHECK(p.after.poll_builds==p.before.poll_builds);CHECK(p.after.group_builds==p.before.group_builds);
        CHECK(p.after.failed_plans==p.before.failed_plans);CHECK(p.after.generation==p.before.generation);CHECK(p.after.redirected_frames>p.before.redirected_frames);
        CHECK(pixels(d,color.Get(),float(p.after.render_w)+.5f,float(p.after.render_h))==0);}
    // 4. Transitions.
    mode(3);run("transition 1->3",3,[&]{const auto s=stats();return s.active&&s.render_w==w/2&&s.render_h==h/2;},1);
    {const auto s=stats();CHECK(s.render_w==w/2&&s.render_h==h/2);CHECK(pixels(d,color.Get(),float(s.render_w)+.5f,float(s.render_h))==0);}
    mode(0);run("transition 3->0",2,[&]{return !stats().active;},1);CHECK(!stats().active);CHECK(pixels(d,color.Get(),w+.5f,h)==0);
    const auto groups_before_on=stats().group_builds;
    mode(1);run("transition 0->1",3,[&]{return stats().active&&stats().redirected_frames>phases.back().before.redirected_frames;},1);CHECK(stats().active);
    std::printf("  0->1 group allocations: %llu\n",(unsigned long long)(stats().group_builds-groups_before_on));
    {const auto s=stats();CHECK(pixels(d,color.Get(),float(s.render_w)+.5f,float(s.render_h))==0);}
    // 5. One legitimately unplannable operation (wrong named width): it must settle into one
    // steady state without endless poll rebuilding.
    bad.store(true);run("unplannable op (settling)",3,never,0);
    run("unplannable op (steady)",3,never,0);
    {const auto& p=phases.back();CHECK(p.after.poll_builds==p.before.poll_builds);CHECK(p.after.group_builds==p.before.group_builds);
        CHECK(p.after.failed_plans==p.before.failed_plans);CHECK(p.after.generation==p.before.generation);
        const bool redirected=p.after.redirected_frames>p.before.redirected_frames;std::printf("  steady state: %s\n",redirected?"still redirected":"native");
        CHECK(pixels(d,color.Get(),redirected?float(p.after.render_w)+.5f:w+.5f,redirected?float(p.after.render_h):float(h),redirected?D3D12_RECT{0,0,0,0}:D3D12_RECT{3300,1300,3308,1308})==0);}
    running.store(false);worker.join();CHECK(!record_failed);
    cb->Unmap(0,nullptr);na::j2::Shutdown();na::dlss::Shutdown();na::dlss::SetInitOwnOverrideForTests(nullptr);
    na::observe::SetShaderOperationHook(na::observe::ShaderLane::Bindings,{});na::observe::SetTextureOperationHook({});na::j2::SetPhysicalInputProvider({});na_observe_stop();
    if(info)for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T size=0;info->GetMessage(i,nullptr,&size);std::vector<unsigned char> bytes(size);
        auto* message=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());if(SUCCEEDED(info->GetMessage(i,message,&size))&&message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::printf("D3D12 error: %s\n",message->pDescription);CHECK(false);}}
    return test::finish("auto_scene_bench");
}
