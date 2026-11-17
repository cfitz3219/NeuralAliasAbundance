// Opt-in real-DX12 CPU recording benchmark. No game, model evaluation, or GPU
// wait in the timed section. Same6000 indexed draws/table binds before and
// after installing producers. Persistent workers keep native thread caches warm.
#include "test_support.hpp"
#include "host_dx12/observe/producers.hpp"
#include "host_dx12/observe/operation_snapshot.hpp"
#include "host_dx12/host_observe.h"
#include "host_dx12/j2/native_hud.hpp"
#include <d3dcompiler.h>
#include <algorithm>
#include <atomic>
#include <barrier>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
namespace {
using Clock=std::chrono::steady_clock;
std::atomic<uint64_t> batches{0},events{0};
void __cdecl sink(const na_event_batch* b,void*){++batches;events.fetch_add(b->event_count);}
struct Stats{double mean=0,p95=0,cpu=0;unsigned frames=0;};
uint64_t cpu_time(){FILETIME c,e,k,u;GetProcessTimes(GetCurrentProcess(),&c,&e,&k,&u);ULARGE_INTEGER a{},b{};a.LowPart=k.dwLowDateTime;a.HighPart=k.dwHighDateTime;b.LowPart=u.dwLowDateTime;b.HighPart=u.dwHighDateTime;return a.QuadPart+b.QuadPart;}
}
int main(int argc,char** argv){
    unsigned threads=1,span=64,copies=0,writes=0;double seconds=2.;bool bare=false,profile=false,latch=false,copy_only=false;
    for(int i=1;i<argc;++i){if(!std::strcmp(argv[i],"--threads")&&i+1<argc)threads=unsigned(std::atoi(argv[++i]));
        else if(!std::strcmp(argv[i],"--seconds")&&i+1<argc)seconds=std::atof(argv[++i]);
        else if(!std::strcmp(argv[i],"--span")&&i+1<argc)span=unsigned(std::atoi(argv[++i]));
        else if(!std::strcmp(argv[i],"--copies")&&i+1<argc)copies=unsigned(std::atoi(argv[++i]));
        else if(!std::strcmp(argv[i],"--writes")&&i+1<argc)writes=unsigned(std::atoi(argv[++i]));
        else if(!std::strcmp(argv[i],"--copy-only"))copy_only=true;
        else if(!std::strcmp(argv[i],"--bare"))bare=true;
        else if(!std::strcmp(argv[i],"--profile"))profile=true;
        else if(!std::strcmp(argv[i],"--latch-jitter"))latch=true;}
    if(!threads||threads>8||!span||span>256||copies>64||writes>64)return 2;
    std::setvbuf(stdout,nullptr,_IONBF,0);test::Device d;if(!test::CreateDevice(d,false))return test::SkipNoDevice("observe_hotpath_bench");
    auto* dev=d.device.Get();
    // Creation must be observed for the production shadow's real descriptors/PSO.
    // Native control excludes callbacks using HostWorkScope, then a real bare
    // original entry point is also measured in a separate process if required.
    if(!bare)REQUIRE(na::observe::InstallProducers(dev,d.queue.Get(),true));
    na::observe::SetRasterStateTracking(true);na::observe::SetOperationSnapshots(true);
    constexpr unsigned tables=16,draws=6000;const unsigned stride=span+1;
    // Each worker owns disjoint, stripe-aligned destinations. The immutable CPU
    // source is NON-shader-visible, as required by D3D12 descriptor copies.
    const unsigned worker_slots=(tables*stride+63u)&~63u;
    auto target=test::CreateTexture(dev,64,32,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto depth=test::CreateTexture(dev,64,32,DXGI_FORMAT_R32_TYPELESS,D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    auto cb=test::CreateBuffer(dev,256*tables,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    auto ib=test::CreateBuffer(dev,12,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);REQUIRE(target&&depth&&cb&&ib);
    void* p=nullptr;REQUIRE(SUCCEEDED(cb->Map(0,nullptr,&p)));for(unsigned i=0;i<tables;++i){const float col[]={.2f,.4f,.6f,1};std::memcpy(static_cast<char*>(p)+i*256,col,16);}cb->Unmap(0,nullptr);
    REQUIRE(SUCCEEDED(ib->Map(0,nullptr,&p)));const unsigned indices[]={0,1,2};std::memcpy(p,indices,12);ib->Unmap(0,nullptr);
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};test::ComPtr<ID3D12DescriptorHeap> rtv,dsv,heap;
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtv))));dev->CreateRenderTargetView(target.Get(),nullptr,rtv->GetCPUDescriptorHandleForHeapStart());
    hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&dsv))));
    D3D12_DEPTH_STENCIL_VIEW_DESC dv{};dv.Format=DXGI_FORMAT_D32_FLOAT;dv.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;dev->CreateDepthStencilView(depth.Get(),&dv,dsv->GetCPUDescriptorHandleForHeapStart());
    hd={D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,threads*worker_slots,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap))));
    test::ComPtr<ID3D12DescriptorHeap> source_heap;
    hd.NumDescriptors=tables*stride;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&source_heap))));
    const UINT increment=dev->GetDescriptorHandleIncrementSize(hd.Type);auto cpu=heap->GetCPUDescriptorHandleForHeapStart();
    std::vector<test::ComPtr<ID3D12Resource>> textures;
    for(unsigned t=0;t<tables;++t){D3D12_CONSTANT_BUFFER_VIEW_DESC view{cb->GetGPUVirtualAddress()+256*t,256};dev->CreateConstantBufferView(&view,cpu);cpu.ptr+=increment;
        for(unsigned j=0;j<span;++j){auto image=test::CreateTexture(dev,8,8,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_FLAG_NONE);REQUIRE(image);
            D3D12_SHADER_RESOURCE_VIEW_DESC sv{};sv.Format=DXGI_FORMAT_R8G8B8A8_UNORM;sv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;sv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;sv.Texture2D.MipLevels=1;
            dev->CreateShaderResourceView(image.Get(),&sv,cpu);cpu.ptr+=increment;textures.push_back(image);}}
    // Source descriptors are created directly: shader-visible heaps cannot be a
    // CopyDescriptors source, even in a CPU-only recording benchmark.
    const auto source_cpu=source_heap->GetCPUDescriptorHandleForHeapStart();
    for(unsigned t=0;t<tables;++t){D3D12_CONSTANT_BUFFER_VIEW_DESC view{cb->GetGPUVirtualAddress()+256*t,256};
        dev->CreateConstantBufferView(&view,{source_cpu.ptr+SIZE_T(t*stride)*increment});
        for(unsigned j=0;j<span;++j){D3D12_SHADER_RESOURCE_VIEW_DESC sv{};sv.Format=DXGI_FORMAT_R8G8B8A8_UNORM;sv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
            sv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;sv.Texture2D.MipLevels=1;
            dev->CreateShaderResourceView(textures[t*span+j].Get(),&sv,{source_cpu.ptr+SIZE_T(t*stride+1+j)*increment});}}
    for(unsigned t=0;t<threads;++t)dev->CopyDescriptorsSimple(tables*stride,{heap->GetCPUDescriptorHandleForHeapStart().ptr+SIZE_T(t*worker_slots)*increment},source_cpu,hd.Type);
    D3D12_DESCRIPTOR_RANGE ranges[]={{D3D12_DESCRIPTOR_RANGE_TYPE_CBV,1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,span,0,0,1}};
    D3D12_ROOT_PARAMETER param{};param.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;param.DescriptorTable={2,ranges};
    D3D12_ROOT_SIGNATURE_DESC rd{1,&param,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};test::ComPtr<ID3DBlob> sig,err,vs,ps;test::ComPtr<ID3D12RootSignature> root;
    REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&sig,&err)));REQUIRE(SUCCEEDED(dev->CreateRootSignature(0,sig->GetBufferPointer(),sig->GetBufferSize(),IID_PPV_ARGS(&root))));
    const char code[]="cbuffer C:register(b0){float4 c;}Texture2D<float4> tex:register(t0);float4 vs(uint id:SV_VertexID):SV_Position{return float4(float2((id<<1)&2,id&2)*float2(2,-2)+float2(-1,1),.5,1);}float4 ps():SV_Target{return c+tex.Load(int3(0,0,0))*.001;}";
    REQUIRE(SUCCEEDED(D3DCompile(code,sizeof(code),nullptr,nullptr,nullptr,"vs","vs_5_0",0,0,&vs,&err)));REQUIRE(SUCCEEDED(D3DCompile(code,sizeof(code),nullptr,nullptr,nullptr,"ps","ps_5_0",0,0,&ps,&err)));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pd.PS={ps->GetBufferPointer(),ps->GetBufferSize()};pd.SampleMask=UINT_MAX;
    pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pd.RasterizerState.DepthClipEnable=TRUE;
    pd.DepthStencilState.DepthEnable=TRUE;pd.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;pd.DSVFormat=DXGI_FORMAT_D32_FLOAT;
    pd.NumRenderTargets=1;pd.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;pd.SampleDesc.Count=1;pd.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    test::ComPtr<ID3D12PipelineState> pso;REQUIRE(SUCCEEDED(dev->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&pso))));
    std::vector<test::List> lists(threads);for(auto& list:lists){REQUIRE(list.Create(dev));REQUIRE(SUCCEEDED(list.list->Close()));}
    na::observe::SetHookProfiling(profile);
    auto phase=[&](const char* name,bool bypass)->Stats{
        if(profile){char cost[4096];na::observe::FormatHookCost(cost,sizeof(cost));}
        std::vector<double> times;std::barrier sync(static_cast<std::ptrdiff_t>(threads+1));std::atomic<bool> stop{false};std::vector<std::thread> workers;
        for(unsigned t=0;t<threads;++t)workers.emplace_back([&,t]{
            auto record=[&]{auto& list=lists[t];list.alloc->Reset();list.list->Reset(list.alloc.Get(),pso.Get());auto* l=list.list.Get();
                    const auto rt=rtv->GetCPUDescriptorHandleForHeapStart(),ds=dsv->GetCPUDescriptorHandleForHeapStart();l->OMSetRenderTargets(1,&rt,FALSE,&ds);l->SetGraphicsRootSignature(root.Get());ID3D12DescriptorHeap* hs[]={heap.Get()};l->SetDescriptorHeaps(1,hs);
                    const D3D12_VIEWPORT vp{0,0,64,32,0,1};const D3D12_RECT sc{0,0,64,32};const D3D12_INDEX_BUFFER_VIEW indicesView{ib->GetGPUVirtualAddress(),12,DXGI_FORMAT_R32_UINT};
                    l->RSSetViewports(1,&vp);l->RSSetScissorRects(1,&sc);l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);l->IASetIndexBuffer(&indicesView);
                    for(unsigned i=t;i<draws;i+=threads){const unsigned table=(i/threads)%tables;
                        for(unsigned c=0;c<copies;++c){const unsigned slot=table*stride+1+(i+c)%span;
                            dev->CopyDescriptorsSimple(1,{heap->GetCPUDescriptorHandleForHeapStart().ptr+SIZE_T(t*worker_slots+slot)*increment},
                                {source_cpu.ptr+SIZE_T(slot)*increment},D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);}
                        for(unsigned c=0;c<writes;++c){const unsigned tex=table*span+(i+c)%span;D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
                            sv.Format=DXGI_FORMAT_R8G8B8A8_UNORM;sv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;sv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;sv.Texture2D.MipLevels=1;
                            dev->CreateShaderResourceView(textures[tex].Get(),&sv,{heap->GetCPUDescriptorHandleForHeapStart().ptr+SIZE_T(t*worker_slots+table*stride+1+(i+c)%span)*increment});}
                        if(!copy_only){const auto gpu=heap->GetGPUDescriptorHandleForHeapStart();l->SetGraphicsRootDescriptorTable(0,{gpu.ptr+UINT64(t*worker_slots+table*stride)*increment});l->DrawIndexedInstanced(3,1,0,0,0);}}l->Close();};
            auto loop=[&]{for(;;){sync.arrive_and_wait();if(stop.load())break;record();sync.arrive_and_wait();}};
            if(bypass){na::observe::HostWorkScope own;loop();}else loop();
        });
        for(unsigned warm=0;warm<5;++warm){sync.arrive_and_wait();sync.arrive_and_wait();}
        const auto start=Clock::now();const auto before=cpu_time();
        while(std::chrono::duration<double>(Clock::now()-start).count()<seconds){
            const auto begin=Clock::now();sync.arrive_and_wait();sync.arrive_and_wait();times.push_back(std::chrono::duration<double,std::milli>(Clock::now()-begin).count());
        }
        const auto cpu_after=cpu_time();stop.store(true);sync.arrive_and_wait();for(auto& worker:workers)worker.join();
        Stats result;result.frames=unsigned(times.size());result.cpu=double(cpu_after-before)/10000./times.size();for(double x:times)result.mean+=x;result.mean/=times.size();std::sort(times.begin(),times.end());result.p95=times[std::min(times.size()-1,size_t(times.size()*.95))];
        std::printf("%s threads=%u span=%u copies=%u writes=%u copy_only=%u frames=%u wall_ms=%.4f p95_ms=%.4f process_cpu_ms=%.4f events=%llu\n",name,threads,span,copies,writes,unsigned(copy_only),result.frames,result.mean,result.p95,result.cpu,(unsigned long long)events.load());if(profile){char cost[4096];na::observe::FormatHookCost(cost,sizeof(cost));std::puts(cost);}return result;
    };
    phase(bare?"native_bare":"host_bypass",!bare);
    if(bare)return 0;
    phase("observer_stopped",false);
    na_observe_config cfg{};cfg.size_bytes=sizeof(cfg);
    REQUIRE(na_observe_start(&cfg,sink,nullptr)==NA_OK);na::observe::SetLiveSelection(na::observe::IdOf(depth.Get()),na::observe::IdOf(target.Get()));
    if(latch){na::observe::SetSceneViewportJitter(1,.25f,.25f);na::observe::SetSceneViewportJitter(0,0,0);}
    phase("observer_full",false);na::j2::native_hud::Install();phase("observer_plus_hud",false);
    na_observe_stop();na::observe::SetProcessExiting();return 0;
}
