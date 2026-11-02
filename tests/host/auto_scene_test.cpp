// Real DX12: automatic same-launch operation recognition -> owned smaller
// rendering -> full native fallback -> existing J2 with a test NGX runtime.
// Tests may wait/read back. This is not a neural-model image-quality test.
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
#include <array>
#include <cmath>
#include <cstring>
#include <string>
namespace {
void __cdecl sink(const na_event_batch*,void*){}
using M=std::array<double,16>;
// Same reverse-Z perspective convention as the existing J2 end-to-end fixture.
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
std::vector<test::ComPtr<ID3D12DescriptorHeap>> model_heaps;
NVSDK_NGX_Result NVSDK_CONV model(ID3D12GraphicsCommandList* list,const NVSDK_NGX_Handle* handle,const NVSDK_NGX_Parameter* params,void* callback){
    const auto result=fake::Eval(list,handle,params,callback);if(NVSDK_NGX_FAILED(result))return result;
    ID3D12Resource* output=nullptr;params->Get(NVSDK_NGX_Parameter_Output,&output);if(!output)return NVSDK_NGX_Result_FAIL_InvalidParameter;
    test::ComPtr<ID3D12Device> dev;output->GetDevice(IID_PPV_ARGS(&dev));test::ComPtr<ID3D12DescriptorHeap> gpu,cpu;
    D3D12_DESCRIPTOR_HEAP_DESC h{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,1,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
    if(FAILED(dev->CreateDescriptorHeap(&h,IID_PPV_ARGS(&gpu))))return NVSDK_NGX_Result_FAIL_OutOfGPUMemory;
    h.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;if(FAILED(dev->CreateDescriptorHeap(&h,IID_PPV_ARGS(&cpu))))return NVSDK_NGX_Result_FAIL_OutOfGPUMemory;
    dev->CreateUnorderedAccessView(output,nullptr,nullptr,gpu->GetCPUDescriptorHandleForHeapStart());
    dev->CreateUnorderedAccessView(output,nullptr,nullptr,cpu->GetCPUDescriptorHandleForHeapStart());
    ID3D12DescriptorHeap* heap=gpu.Get();list->SetDescriptorHeaps(1,&heap);const float value[4]={55,66,9,.75f};
    list->ClearUnorderedAccessViewFloat(gpu->GetGPUDescriptorHandleForHeapStart(),cpu->GetCPUDescriptorHandleForHeapStart(),output,value,0,nullptr);
    model_heaps.push_back(gpu);model_heaps.push_back(cpu);return result;
}
void __stdcall forward(ID3D12CommandQueue* q,UINT n,ID3D12CommandList* const* lists){
    na::nr::native::Submission retained(q,n,lists);q->ExecuteCommandLists(n,lists);
}
float half(uint16_t h){const uint32_t e=(h>>10)&31,m=h&1023;const float v=e?std::ldexp(float(m|1024),int(e)-25):std::ldexp(float(m),-24);return h&0x8000?-v:v;}
int pixels(test::Device& device,ID3D12Resource* color,float expected_x,float expected_y){
    const na::observe::HostWorkScope own;const auto desc=color->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes=0;
    device.device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
    auto readback=test::CreateBuffer(device.device.Get(),bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);test::List copy;REQUIRE(readback&&copy.Create(device.device.Get()));
    auto barrier=test::Transition(color,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);copy.list->ResourceBarrier(1,&barrier);
    D3D12_TEXTURE_COPY_LOCATION source{},destination{};source.pResource=color;source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.pResource=readback.Get();destination.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;destination.PlacedFootprint=footprint;
    copy.list->CopyTextureRegion(&destination,0,0,0,&source,nullptr);std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);copy.list->ResourceBarrier(1,&barrier);copy.list->Close();
    ID3D12CommandList* lists[]={copy.list.Get()};device.queue->ExecuteCommandLists(1,lists);device.WaitIdle();
    unsigned char* mapped=nullptr;REQUIRE(SUCCEEDED(readback->Map(0,nullptr,reinterpret_cast<void**>(&mapped))));unsigned wrong=0;
    for(UINT y=0;y<desc.Height;++y)for(UINT x=0;x<desc.Width;++x){const auto* p=reinterpret_cast<const uint16_t*>(mapped+footprint.Offset+y*footprint.Footprint.RowPitch+x*8);
        wrong+=half(p[0])!=expected_x||half(p[1])!=expected_y||half(p[2])!=9||half(p[3])!=.75f;}
    readback->Unmap(0,nullptr);std::printf("automatic native pixels %g/%g: wrong %u\n",expected_x,expected_y,wrong);CHECK(wrong==0);return 0;
}
}
int main(int argc,char** argv){
    const bool stencil=argc>1&&std::strcmp(argv[1],"--stencil")==0;
    const bool vector4=argc>1&&std::strcmp(argv[1],"--vector4")==0;
    const bool boundary=argc>1&&std::strcmp(argv[1],"--boundary")==0;
    const bool split=argc>1&&std::strcmp(argv[1],"--split")==0;
    const bool unsupported=argc>1&&std::strcmp(argv[1],"--unsupported")==0;
    setvbuf(stdout,nullptr,_IONBF,0);test::ComPtr<ID3D12Debug> debug;
    if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    test::Device d;if(!test::CreateDevice(d))return test::SkipNoDevice("auto_scene_test");auto* dev=d.device.Get();
    test::ComPtr<ID3D12InfoQueue> info;dev->QueryInterface(IID_PPV_ARGS(&info));
    REQUIRE(na::observe::InstallProducers(dev,d.queue.Get(),true));
    REQUIRE(na::observe::InstallListResetTracking(dev,&na::nr::native::Reset));
    na_observe_config cfg{};cfg.size_bytes=sizeof cfg;REQUIRE(na_observe_start(&cfg,sink,nullptr)==NA_OK);
    na::j2::SetEnabled(true);na::j2::SetJitter(false);na::settings::Get().j2_route.store(1);na::settings::Get().j2_scale_mode.store(1);na::settings::Get().j2_sharpness.store(0);
    na::scale::ConfigureAutomaticScene(true);
    na::dlss::SetInitOwnOverrideForTests([](ID3D12Device* device,const wchar_t*){auto api=fake::MakeApi();api.evaluate_feature=reinterpret_cast<void*>(&model);return na::dlss::Attach(device,api);});
    constexpr UINT w=64,h=32;
    auto color=test::CreateTexture(dev,w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    const auto depth_resource=stencil?DXGI_FORMAT_R32G8X24_TYPELESS:DXGI_FORMAT_R32_TYPELESS;
    const auto depth_view=stencil?DXGI_FORMAT_D32_FLOAT_S8X24_UINT:DXGI_FORMAT_D32_FLOAT;
    auto depth=test::CreateTexture(dev,w,h,depth_resource,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    auto cb=test::CreateBuffer(dev,4096,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);REQUIRE(color&&depth&&cb);
    auto color_copy=test::CreateTexture(dev,w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_NONE);
    auto depth_copy=test::CreateTexture(dev,w,h,depth_resource,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_NONE);REQUIRE(color_copy&&depth_copy);
    char* mapped=nullptr;REQUIRE(SUCCEEDED(cb->Map(0,nullptr,reinterpret_cast<void**>(&mapped))));std::memset(mapped,0,4096);
    float sizes[4]={w,h,9,0};std::memcpy(mapped,sizes,sizeof sizes);const auto vp=camera();store(mapped+80,vp);store(mapped+144,inverse(vp));
    if(unsupported){const float wrong_width=w-1.f;std::memcpy(mapped,&wrong_width,4);}
    if(vector4){const float values[8]={w,h,1.f/w,1.f/h,9,0,0,0};std::memcpy(mapped,values,sizeof values);}
    std::string code=vector4
        ? "cbuffer View:register(b0){float4 ScreenSize;float marker;float3 pad;float4 junk[3];float4x4 VP;float4x4 VPInv;};\n#define INVERSE_ERROR (abs(ScreenSize.x*ScreenSize.z-1)+abs(ScreenSize.y*ScreenSize.w-1))\n"
        : "cbuffer View:register(b0){float2 ScreenSize;float marker,pad;float4 junk[4];float4x4 VP;float4x4 VPInv;};\n#define INVERSE_ERROR 0\n";
    code+=R"(
Texture2D<float4> CopiedColor:register(t0);Texture2D<float> CopiedDepth:register(t1);
float4 vs(uint i:SV_VertexID):SV_Position{return float4(float2((i<<1)&2,i&2)*2-1,.5,1);}
float4 ps():SV_Target{return float4(ScreenSize.xy,marker+INVERSE_ERROR,.75);}
float4 water(float4 position:SV_Position):SV_Target{uint2 p=uint2(position.xy);float4 c=CopiedColor.Load(int3(p,0));float d=CopiedDepth.Load(int3(p,0));return float4(c.xy+float2(d,0),marker+INVERSE_ERROR,.75);})";
    test::ComPtr<ID3DBlob> vs,ps,blob,error;REQUIRE(SUCCEEDED(D3DCompile(code.data(),code.size(),nullptr,nullptr,nullptr,"vs","vs_5_0",0,0,&vs,&error)));
    REQUIRE(SUCCEEDED(D3DCompile(code.data(),code.size(),nullptr,nullptr,nullptr,"ps","ps_5_0",0,0,&ps,&error)));
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0};D3D12_ROOT_PARAMETER rp[2]{};rp[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;rp[0].Descriptor={0,0};
    rp[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;rp[1].DescriptorTable={1,&range};
    D3D12_ROOT_SIGNATURE_DESC rs{};rs.NumParameters=2;rs.pParameters=rp;
    REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error)));
    test::ComPtr<ID3D12RootSignature> root;REQUIRE(SUCCEEDED(dev->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root))));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pd.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
    pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pd.RasterizerState.DepthClipEnable=TRUE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;pd.DepthStencilState.DepthEnable=TRUE;pd.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;pd.DepthStencilState.FrontFace=pd.DepthStencilState.BackFace={D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_COMPARISON_FUNC_ALWAYS};
    pd.SampleMask=UINT_MAX;pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;pd.NumRenderTargets=1;pd.RTVFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;pd.DSVFormat=depth_view;pd.SampleDesc.Count=1;
    test::ComPtr<ID3D12PipelineState> pso;REQUIRE(SUCCEEDED(dev->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&pso))));
    test::ComPtr<ID3DBlob> water;REQUIRE(SUCCEEDED(D3DCompile(code.data(),code.size(),nullptr,nullptr,nullptr,"water","ps_5_0",0,0,&water,&error)));
    pd.PS={water->GetBufferPointer(),water->GetBufferSize()};test::ComPtr<ID3D12PipelineState> water_pso;REQUIRE(SUCCEEDED(dev->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&water_pso))));
    test::ComPtr<ID3D12DescriptorHeap> rt,ds;D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rt))));hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&ds))));
    const auto rtv=rt->GetCPUDescriptorHandleForHeapStart(),dsv=ds->GetCPUDescriptorHandleForHeapStart();dev->CreateRenderTargetView(color.Get(),nullptr,rtv);
    D3D12_DEPTH_STENCIL_VIEW_DESC dd{};dd.Format=depth_view;dd.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;dev->CreateDepthStencilView(depth.Get(),&dd,dsv);
    hd={D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,2,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};test::ComPtr<ID3D12DescriptorHeap> texture_heap;REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&texture_heap))));
    auto view=texture_heap->GetCPUDescriptorHandleForHeapStart();D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;dev->CreateShaderResourceView(color_copy.Get(),&srv,view);
    view.ptr+=dev->GetDescriptorHandleIncrementSize(hd.Type);srv.Format=stencil?DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:DXGI_FORMAT_R32_FLOAT;dev->CreateShaderResourceView(depth_copy.Get(),&srv,view);
    na::observe::SetLiveSelection(na::observe::IdOf(depth.Get()),na::observe::IdOf(color.Get()));
    test::ComPtr<ID3D12CommandSignature> indirect_signature;
    auto indirect_args=test::CreateBuffer(dev,256,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);REQUIRE(indirect_args);
    D3D12_INDIRECT_ARGUMENT_DESC indirect_draw{};indirect_draw.Type=D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC indirect_desc{sizeof(D3D12_DRAW_ARGUMENTS),1,&indirect_draw,0};
    REQUIRE(SUCCEEDED(dev->CreateCommandSignature(&indirect_desc,nullptr,IID_PPV_ARGS(&indirect_signature))));
    void* arguments=nullptr;REQUIRE(SUCCEEDED(indirect_args->Map(0,nullptr,&arguments)));const D3D12_DRAW_ARGUMENTS draw{3,1,0,0};std::memcpy(arguments,&draw,sizeof draw);indirect_args->Unmap(0,nullptr);
    test::List scene;REQUIRE(scene.Create(dev));REQUIRE(SUCCEEDED(scene.list->Close()));
    test::List tail;REQUIRE(tail.Create(dev));REQUIRE(SUCCEEDED(tail.list->Close()));
    const auto frame=[&](bool reconstruct){
        d.WaitIdle();na::scale::AutomaticScenePresent();na::j2::OnPresent(d.queue.Get());scene.Reset();auto* list=scene.list.Get();
        D3D12_RESOURCE_BARRIER start[]={test::Transition(color.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET),test::Transition(depth.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_DEPTH_WRITE)};list->ResourceBarrier(2,start);
        list->SetGraphicsRootSignature(root.Get());list->SetPipelineState(pso.Get());list->SetGraphicsRootConstantBufferView(0,cb->GetGPUVirtualAddress());
        ID3D12DescriptorHeap* heap=texture_heap.Get();list->SetDescriptorHeaps(1,&heap);list->SetGraphicsRootDescriptorTable(1,texture_heap->GetGPUDescriptorHandleForHeapStart());
        list->OMSetRenderTargets(1,&rtv,FALSE,&dsv);D3D12_VIEWPORT viewport{0,0,w,h,0,1};D3D12_RECT scissor{0,0,w,h};list->RSSetViewports(1,&viewport);list->RSSetScissorRects(1,&scissor);list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const float clear[4]={0,0,0,0};list->ClearRenderTargetView(rtv,clear,0,nullptr);
        list->ClearDepthStencilView(dsv,static_cast<D3D12_CLEAR_FLAGS>(D3D12_CLEAR_FLAG_DEPTH|(stencil?D3D12_CLEAR_FLAG_STENCIL:0)),1,0x5a,0,nullptr);list->DrawInstanced(3,1,0,0);
        D3D12_RESOURCE_BARRIER copies[]={test::Transition(color.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE),test::Transition(depth.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_COPY_SOURCE),
            test::Transition(color_copy.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST),test::Transition(depth_copy.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST)};
        list->ResourceBarrier(4,copies);list->CopyResource(color_copy.Get(),color.Get());list->CopyResource(depth_copy.Get(),depth.Get());
        for(auto& barrier:copies)std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);list->ResourceBarrier(4,copies);
        if(split){
            CHECK(SUCCEEDED(list->Close()));tail.Reset();list=tail.list.Get();
            list->SetGraphicsRootSignature(root.Get());list->SetGraphicsRootConstantBufferView(0,cb->GetGPUVirtualAddress());
            list->SetDescriptorHeaps(1,&heap);list->SetGraphicsRootDescriptorTable(1,texture_heap->GetGPUDescriptorHandleForHeapStart());
            list->OMSetRenderTargets(1,&rtv,FALSE,&dsv);list->RSSetViewports(1,&viewport);list->RSSetScissorRects(1,&scissor);list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        }
        D3D12_RESOURCE_BARRIER reads[]={test::Transition(color_copy.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),test::Transition(depth_copy.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)};
        list->ResourceBarrier(2,reads);list->SetPipelineState(water_pso.Get());list->DrawInstanced(3,1,0,0);
        for(auto& barrier:reads)std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);list->ResourceBarrier(2,reads);
        if(boundary){
            // The unknown command must receive native bindings and a current
            // full-size scene, before its state effects invalidate observation.
            for(auto& barrier:reads)std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);list->ResourceBarrier(2,reads);
            list->ExecuteIndirect(indirect_signature.Get(),1,indirect_args.Get(),0,nullptr,0);
            for(auto& barrier:reads)std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);list->ResourceBarrier(2,reads);
        }
        D3D12_RESOURCE_BARRIER finish[]={test::Transition(color.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON),test::Transition(depth.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_COMMON)};list->ResourceBarrier(2,finish);CHECK(SUCCEEDED(list->Close()));
        ID3D12CommandList* lists[]={scene.list.Get(),tail.list.Get()};const UINT count=split?2:1;
        na::scale::AutomaticSceneSubmission receipt(d.queue.Get(),count,lists);
        if(reconstruct)na::j2::Execute(d.queue.Get(),count,lists,&forward);else forward(d.queue.Get(),count,lists);
        d.WaitIdle();na::scale::PollAutomaticScene();const auto status=na::scale::GetAutomaticSceneStats();
        std::printf("automatic: %u plans %u members %ux%u, redirected %llu misses %llu: %s | J2: %s\n",status.plans,status.members,status.render_w,status.render_h,(unsigned long long)status.redirected,(unsigned long long)status.misses,status.reason,na::j2::GetStats().reason);
    };
    for(int i=0;i<8;++i)frame(false);
    auto status=na::scale::GetAutomaticSceneStats();
    if(unsupported){
        CHECK(status.failed_plans>0&&status.redirected==0);
        const auto attempts=status.poll_builds,groups=status.group_builds,operations=status.operations;
        for(int i=0;i<32;++i)na::scale::PollAutomaticScene();
        CHECK(na::scale::GetAutomaticSceneStats().poll_builds==attempts&&na::scale::GetAutomaticSceneStats().group_builds==groups);
        na::settings::Get().j2_scale_mode.store(0);frame(false);
        CHECK(na::scale::GetAutomaticSceneStats().operations==operations);
        REQUIRE(pixels(d,color.Get(),w-.5f,h)==0);
    }else{
    CHECK(status.plans>0&&status.members==4&&status.redirected>0);
    // A one-Present menu candidate must not discard the learned scene. A
    // stable alternate selection may switch, then restoring the original pair
    // must reuse its prepared group rather than allocate/relearn everything.
    auto alternate_color=test::CreateTexture(dev,w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto alternate_depth=test::CreateTexture(dev,w,h,depth_resource,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    REQUIRE(alternate_color&&alternate_depth);
    const auto learned_generation=status.generation,learned_builds=status.group_builds;
    na::observe::SetLiveSelection(na::observe::IdOf(alternate_depth.Get()),na::observe::IdOf(alternate_color.Get()));
    na::scale::AutomaticScenePresent();
    na::observe::SetLiveSelection(na::observe::IdOf(depth.Get()),na::observe::IdOf(color.Get()));
    frame(false);CHECK(na::scale::GetAutomaticSceneStats().generation==learned_generation);
    na::observe::SetLiveSelection(na::observe::IdOf(alternate_depth.Get()),na::observe::IdOf(alternate_color.Get()));
    for(int i=0;i<3;++i)na::scale::AutomaticScenePresent();
    na::observe::SetLiveSelection(na::observe::IdOf(depth.Get()),na::observe::IdOf(color.Get()));
    for(int i=0;i<3;++i)na::scale::AutomaticScenePresent();
    frame(false);status=na::scale::GetAutomaticSceneStats();
    CHECK(status.generation==learned_generation&&status.group_builds==learned_builds);
    REQUIRE(pixels(d,color.Get(),float(status.render_w)+.5f,float(status.render_h))==0);
    REQUIRE(pixels(d,color_copy.Get(),float(status.render_w),float(status.render_h))==0);
    frame(true);const auto js=na::j2::GetStats();
    CHECK(boundary?(js.physical_evaluated==0&&js.render_w==w):(js.physical_evaluated>0&&js.render_w==status.render_w&&js.out_w==w));
    REQUIRE(pixels(d,color.Get(),55,66)==0);
    na::settings::Get().j2_scale_mode.store(3);na::scale::PollAutomaticScene();frame(true);
    status=na::scale::GetAutomaticSceneStats();CHECK(status.render_w==w/2&&status.render_h==h/2);
    CHECK(na::j2::GetStats().render_w==(boundary?w:w/2));
    REQUIRE(pixels(d,color.Get(),55,66)==0);
    fake::eval_result=NVSDK_NGX_Result_FAIL_InvalidParameter;frame(true);REQUIRE(pixels(d,color.Get(),float(status.render_w)+.5f,float(status.render_h))==0);
    fake::eval_result=NVSDK_NGX_Result_Success;
    na::settings::Get().j2_scale_mode.store(0);frame(false);CHECK(!na::scale::GetAutomaticSceneStats().active);
    REQUIRE(pixels(d,color.Get(),w+.5f,h)==0);
    }
    cb->Unmap(0,nullptr);na::j2::Shutdown();na::dlss::Shutdown();na::dlss::SetInitOwnOverrideForTests(nullptr);
    na::observe::SetShaderOperationHook(na::observe::ShaderLane::Bindings,{});na::observe::SetTextureOperationHook({});na::j2::SetPhysicalInputProvider({});na_observe_stop();
    if(info)for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T size=0;info->GetMessage(i,nullptr,&size);std::vector<unsigned char> bytes(size);
        auto* message=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());if(SUCCEEDED(info->GetMessage(i,message,&size))&&message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::printf("D3D12 error: %s\n",message->pDescription);CHECK(false);}}
    return test::finish("auto_scene_test");
}
