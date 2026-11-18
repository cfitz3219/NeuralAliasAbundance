// Depth reads end to end, with no game: a sloping depth drawn through jittered viewports, then read
// by a "lighting" draw the way a game reads its scene depth. Before the hand-over the read sees the
// jitter; after it (a view that existed before, and one made after) it sees the depth with the
// jitter taken out, whichever sample the frame used; turned off, the game's own depth is back.
// The read draw's state is set BEFORE the game's barrier, so a wrong restore shows in the pixels.
// Wait/readback belong only to this test.
#include "test_support.hpp"
#include "host_dx12/observe/producers.hpp"
#include "host_dx12/observe/operation_snapshot.hpp"
#include "host_dx12/observe/texture_operation.hpp"
#include "host_dx12/nr/native_fg.hpp"
#include "host_dx12/j2/depth_reads.hpp"
#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#include <cmath>
#include <cstring>
#include <vector>
namespace {
void __cdecl sink(const na_event_batch*,void*){}
constexpr UINT W=64,H=32;
// The scene's depth where a pixel centre is, for a viewport moved by (jx,jy). The host gives a
// full-size scene viewport half a pixel on each side, hence the +1s.
float scene_depth(UINT x,UINT y,float jx,float jy){return .1f+.2f*(float(x)+1.f-jx)/float(W+1)+.2f*(float(y)+1.f-jy)/float(H+1);}
}
int main(){
    test::ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    test::Device d;if(!test::CreateDevice(d))return test::SkipNoDevice("depth_reads_test");auto* dev=d.device.Get();
    test::ComPtr<ID3D12InfoQueue> info;dev->QueryInterface(IID_PPV_ARGS(&info));
    REQUIRE(na::observe::InstallProducers(dev,d.queue.Get(),true));
    REQUIRE(na::observe::InstallListResetTracking(dev,&na::nr::native::Reset));
    na::observe::SetOperationSnapshots(true);
    na::observe::SetRasterStateTracking(true);
    na_observe_config config{};config.size_bytes=sizeof config;REQUIRE(na_observe_start(&config,&sink,nullptr)==NA_OK);
    constexpr auto format=DXGI_FORMAT_R32G32B32A32_FLOAT;
    auto color=test::CreateTexture(dev,W,H,format,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto depth=test::CreateTexture(dev,W,H,DXGI_FORMAT_R32_TYPELESS,D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    REQUIRE(color&&depth);const auto depth_id=na::observe::IdOf(depth.Get());na::observe::SetLiveSelection(depth_id,na::observe::IdOf(color.Get()));
    test::ComPtr<ID3D12DescriptorHeap> rt,ds,views;D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rt))));hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&ds))));
    hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=2;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&views))));
    const auto rtv=rt->GetCPUDescriptorHandleForHeapStart(),dsv=ds->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(color.Get(),nullptr,rtv);
    D3D12_DEPTH_STENCIL_VIEW_DESC dd{};dd.Format=DXGI_FORMAT_D32_FLOAT;dd.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;dev->CreateDepthStencilView(depth.Get(),&dd,dsv);
    // The game's two views of its depth: slot 0 exists before anything is handed over, slot 1 is made after.
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=DXGI_FORMAT_R32_FLOAT;sd.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;sd.Texture2D.MipLevels=1;
    const UINT step=dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu0=views->GetCPUDescriptorHandleForHeapStart(),cpu1=cpu0;cpu1.ptr+=step;
    auto gpu0=views->GetGPUDescriptorHandleForHeapStart(),gpu1=gpu0;gpu1.ptr+=step;
    dev->CreateShaderResourceView(depth.Get(),&sd,cpu0);
    test::ComPtr<ID3DBlob> vs,ps,rps,blob,error;test::ComPtr<ID3D12RootSignature> scene_root,read_root;
    D3D12_ROOT_SIGNATURE_DESC rd{};REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error)));
    REQUIRE(SUCCEEDED(dev->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&scene_root))));
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameter.DescriptorTable={1,&range};parameter.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    rd.NumParameters=1;rd.pParameters=&parameter;blob.Reset();REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error)));
    REQUIRE(SUCCEEDED(dev->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&read_root))));
    constexpr char code[]=R"(struct V{float4 p:SV_Position;float2 uv:TEXCOORD;};
V VS(uint i:SV_VertexID){V v;v.uv=float2((i<<1)&2,i&2);v.p=float4(v.uv.x*2-1,1-v.uv.y*2,.1+.2*v.uv.x+.2*v.uv.y,1);return v;}
float4 PS(V v):SV_Target{return float4(-1,-1,-1,1);}
Texture2D<float> scene_depth:register(t0);
float4 Read(V v):SV_Target{return float4(scene_depth.Load(int3(v.p.xy,0)),v.uv,1);})";
    REQUIRE(SUCCEEDED(D3DCompile(code,sizeof code-1,nullptr,nullptr,nullptr,"VS","vs_5_0",0,0,&vs,&error)));
    REQUIRE(SUCCEEDED(D3DCompile(code,sizeof code-1,nullptr,nullptr,nullptr,"PS","ps_5_0",0,0,&ps,&error)));
    REQUIRE(SUCCEEDED(D3DCompile(code,sizeof code-1,nullptr,nullptr,nullptr,"Read","ps_5_0",0,0,&rps,&error)));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};pd.pRootSignature=scene_root.Get();pd.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pd.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
    pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pd.RasterizerState.DepthClipEnable=TRUE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask=15;pd.SampleMask=UINT_MAX;pd.SampleDesc.Count=1;pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets=1;pd.RTVFormats[0]=format;pd.DSVFormat=DXGI_FORMAT_D32_FLOAT;
    pd.DepthStencilState.DepthEnable=TRUE;pd.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
    test::ComPtr<ID3D12PipelineState> scene,read;REQUIRE(SUCCEEDED(dev->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&scene))));
    pd.pRootSignature=read_root.Get();pd.PS={rps->GetBufferPointer(),rps->GetBufferSize()};pd.DSVFormat=DXGI_FORMAT_UNKNOWN;pd.DepthStencilState={};
    REQUIRE(SUCCEEDED(dev->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&read))));
    const auto cdesc=color->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes=0;dev->GetCopyableFootprints(&cdesc,0,1,0,&fp,nullptr,nullptr,&bytes);
    auto readback=test::CreateBuffer(dev,bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);REQUIRE(readback);
    test::List list;REQUIRE(list.Create(dev));REQUIRE(SUCCEEDED(list.list->Close()));
    uint64_t frame_number=0;
    // One frame the way a deferred game records it; returns what its lighting read at every pixel.
    auto frame=[&](float jx,float jy,D3D12_GPU_DESCRIPTOR_HANDLE view,std::vector<float>& out)->bool{
        list.Reset();auto* l=list.list.Get();na::observe::SetSceneViewportJitter(++frame_number,jx,jy);
        const D3D12_VIEWPORT vp{0,0,float(W),float(H),0,1};const D3D12_RECT sc{0,0,LONG(W),LONG(H)};
        l->SetGraphicsRootSignature(scene_root.Get());l->SetPipelineState(scene.Get());l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        l->OMSetRenderTargets(1,&rtv,FALSE,&dsv);l->RSSetViewports(1,&vp);l->RSSetScissorRects(1,&sc);
        l->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,1.f,0,0,nullptr);
        l->DrawInstanced(3,1,0,0);
        // Everything the lighting draw needs is bound first; the host's work at the barrier must hand it all back.
        ID3D12DescriptorHeap* heaps[]={views.Get()};l->SetDescriptorHeaps(1,heaps);
        l->SetGraphicsRootSignature(read_root.Get());l->SetGraphicsRootDescriptorTable(0,view);l->SetPipelineState(read.Get());
        l->OMSetRenderTargets(1,&rtv,FALSE,nullptr);l->RSSetViewports(1,&vp);l->RSSetScissorRects(1,&sc);
        auto b=test::Transition(depth.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);l->ResourceBarrier(1,&b);
        l->DrawInstanced(3,1,0,0);
        std::swap(b.Transition.StateBefore,b.Transition.StateAfter);l->ResourceBarrier(1,&b);
        {const na::observe::HostWorkScope own;
            auto c=test::Transition(color.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);l->ResourceBarrier(1,&c);
            D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=color.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=fp;l->CopyTextureRegion(&to,0,0,0,&from,nullptr);
            std::swap(c.Transition.StateBefore,c.Transition.StateAfter);l->ResourceBarrier(1,&c);}
        if(FAILED(l->Close()))return false;ID3D12CommandList* lists[]={l};
        {na::nr::native::Submission keep(d.queue.Get(),1,lists);d.queue->ExecuteCommandLists(1,lists);}d.WaitIdle();
        const void* data=nullptr;if(FAILED(readback->Map(0,nullptr,const_cast<void**>(&data))))return false;
        out.resize(size_t(W)*H);
        for(UINT y=0;y<H;++y)for(UINT x=0;x<W;++x){
            const auto* p=reinterpret_cast<const float*>(static_cast<const char*>(data)+fp.Offset+y*fp.Footprint.RowPitch+x*16);
            out[size_t(y)*W+x]=p[0];
            // The lighting draw's own interpolant: wrong if its viewport, scissor or target came back wrong.
            if(std::abs(p[1]-(float(x)+.5f)/W)>1e-5f||std::abs(p[2]-(float(y)+.5f)/H)>1e-5f){std::printf("read draw state wrong at %u,%u: %.6f/%.6f\n",x,y,p[1],p[2]);readback->Unmap(0,nullptr);return false;}
        }
        readback->Unmap(0,nullptr);return true;
    };
    // Largest difference from the scene's depth for a viewport moved by (jx,jy), away from the picture's rim.
    auto worst=[&](const std::vector<float>& got,float jx,float jy){float m=0;UINT wx=0,wy=0;
        for(UINT y=2;y+2<H;++y)for(UINT x=2;x+2<W;++x){const float e=std::abs(got[size_t(y)*W+x]-scene_depth(x,y,jx,jy));if(e>m){m=e;wx=x;wy=y;}}
        std::printf("  jitter %+.3f/%+.3f: worst %.3g at %u,%u (read %.7f, scene %.7f)\n",jx,jy,m,wx,wy,got[size_t(wy)*W+wx],scene_depth(wx,wy,jx,jy));return m;};
    auto tick=[&](bool want){const na::observe::HostWorkScope own;na::j2::depth_reads::Tick(d.queue.Get(),depth_id,want);};
    auto status=[&](const char* expect){char line[448];na::j2::depth_reads::Format(line,sizeof line);std::printf("%s\n",line);
        char want[32];std::snprintf(want,sizeof want,"state=%s ",expect);return std::strstr(line,want)!=nullptr;};
    constexpr float tol=2e-5f;const float s1[2]={.25f,-.375f},s2[2]={-.375f,-.25f};
    std::vector<float> got;
    // 0. Nothing handed over: the read sees the jitter (this is the flicker's source).
    std::printf("off:\n");REQUIRE(frame(s1[0],s1[1],gpu0,got));CHECK(worst(got,s1[0],s1[1])<tol);CHECK(worst(got,0,0)>1e-3f);
    // 1. Asked for: the twin fills at the barrier, the game's views still name its own depth.
    tick(true);CHECK(status("filling"));
    std::printf("filling:\n");REQUIRE(frame(s1[0],s1[1],gpu0,got));CHECK(worst(got,s1[0],s1[1])<tol);
    // 2. Handed over: the view that existed (slot 0) is rewritten; a view made now (slot 1) goes to the twin.
    tick(true);CHECK(status("on"));
    dev->CreateShaderResourceView(depth.Get(),&sd,cpu1);
    std::printf("on, the view that existed, second sample:\n");REQUIRE(frame(s2[0],s2[1],gpu0,got));CHECK(worst(got,0,0)<tol);
    std::printf("on, a view made after, first sample:\n");REQUIRE(frame(s1[0],s1[1],gpu1,got));CHECK(worst(got,0,0)<tol);
    tick(true);
    std::printf("on, a view made after, second sample:\n");REQUIRE(frame(s2[0],s2[1],gpu1,got));CHECK(worst(got,0,0)<tol);
    {const auto stats=na::observe::GetDepthReadStats();CHECK(stats.created>=1&&stats.swept>=1&&stats.readable>=4);CHECK(status("on"));}
    // 3. Turned off live: both views are back on the game's depth.
    tick(false);CHECK(status("off"));
    std::printf("off again:\n");REQUIRE(frame(s2[0],s2[1],gpu0,got));CHECK(worst(got,s2[0],s2[1])<tol);
    REQUIRE(frame(s1[0],s1[1],gpu1,got));CHECK(worst(got,s1[0],s1[1])<tol);
    // 4. And on again (a second twin; the first is parked).
    tick(true);REQUIRE(frame(s1[0],s1[1],gpu0,got));tick(true);CHECK(status("on"));
    std::printf("on again:\n");REQUIRE(frame(s2[0],s2[1],gpu1,got));CHECK(worst(got,0,0)<tol);
    tick(false);na::j2::depth_reads::Shutdown();
    na::observe::SetSceneViewportJitter(0,0,0);
    if(info)for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T size=0;info->GetMessage(i,nullptr,&size);std::vector<uint8_t> data(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(data.data());
        if(SUCCEEDED(info->GetMessage(i,message,&size))&&message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::printf("D3D12 error: %s\n",message->pDescription);CHECK(false);}}
    CHECK(dev->GetDeviceRemovedReason()==S_OK);na_observe_stop();na::observe::SetProcessExiting();return test::finish("depth_reads_test");
}
