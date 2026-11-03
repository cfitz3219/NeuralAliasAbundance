// Reconstructed 2026-11: actual GPU raster witness, not a camera-byte test.
// The vertex shader deliberately ignores all camera constants. A translated
// viewport must still move BOTH colour and depth by the advertised sample.
// Synchronous readback is confined to this test executable.
#include "test_support.hpp"
#include "host_dx12/host_observe.h"
#include "host_dx12/observe/producers.hpp"
#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#include <cmath>
#include <cstring>
#include <chrono>

namespace {
constexpr UINT W = 128, H = 64;
double sample_x = 0, sample_y = 0;
unsigned calls = 0;
void __cdecl Sink(const na_event_batch*, void*) {}
bool Sample(uint32_t w, uint32_t h, double* x, double* y) {
    CHECK(w == W && h == H); ++calls;
    *x = sample_x; *y = sample_y; return true;
}
}
int main(int argc, char**) {
    const bool validation=argc>1;
    test::ComPtr<ID3D12Debug> debug;
    if(validation && SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    test::Device d;
    if (!test::CreateDevice(d)) return test::kSkip;
    auto* dev = d.device.Get();
    REQUIRE(na::observe::InstallProducers(dev, d.queue.Get(), false));
    na_observe_config cfg{}; cfg.size_bytes = sizeof(cfg);
    REQUIRE(na_observe_start(&cfg, &Sink, nullptr) == NA_OK);
    auto color = test::CreateTexture(dev,W,H,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto untouched = test::CreateTexture(dev,W,H,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto depth = test::CreateTexture(dev,W,H,DXGI_FORMAT_R32_TYPELESS,D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    REQUIRE(color && untouched && depth);
    test::ComPtr<ID3D12DescriptorHeap> rt, ds;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,2,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rt))));
    hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;hd.NumDescriptors=1;
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&ds))));
    auto rtv=rt->GetCPUDescriptorHandleForHeapStart(), other=rtv, dsv=ds->GetCPUDescriptorHandleForHeapStart();
    other.ptr+=dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    dev->CreateRenderTargetView(color.Get(),nullptr,rtv);dev->CreateRenderTargetView(untouched.Get(),nullptr,other);
    D3D12_DEPTH_STENCIL_VIEW_DESC dd{};dd.Format=DXGI_FORMAT_D32_FLOAT;dd.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
    dev->CreateDepthStencilView(depth.Get(),&dd,dsv);
    na::observe::SetLiveSelection(na::observe::IdOf(depth.Get()),na::observe::IdOf(color.Get()));
    test::ComPtr<ID3D12RootSignature> root;
    D3D12_ROOT_SIGNATURE_DESC rd{}; test::ComPtr<ID3DBlob> blob,err;
    REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&err)));
    REQUIRE(SUCCEEDED(dev->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root))));
    const char* shader=R"(
struct V { float4 pos:SV_Position; float2 uv:TEXCOORD0; };
V VS(uint id:SV_VertexID) { V o; float2 p=float2((id<<1)&2,id&2);o.pos=float4(p*float2(2,-2)+float2(-1,1),0.5,1);o.uv=p;return o; }
struct P { float4 color:SV_Target; float depth:SV_Depth; };
P PS(V i) { P o;o.color=float4(i.uv,0,1);o.depth=0.2+0.4*i.uv.x;return o; }
)";
    test::ComPtr<ID3DBlob> vs,ps;
    REQUIRE(SUCCEEDED(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"VS","vs_5_0",0,0,&vs,&err)));
    REQUIRE(SUCCEEDED(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"PS","ps_5_0",0,0,&ps,&err)));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();
    pd.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pd.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
    pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable=TRUE;pd.BlendState.RenderTarget[0].RenderTargetWriteMask=15;
    pd.DepthStencilState.DepthEnable=TRUE;pd.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;pd.SampleMask=UINT_MAX;
    pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;pd.NumRenderTargets=1;
    pd.RTVFormats[0]=DXGI_FORMAT_R32G32B32A32_FLOAT;pd.DSVFormat=DXGI_FORMAT_D32_FLOAT;pd.SampleDesc.Count=1;
    test::ComPtr<ID3D12PipelineState> geometry,post,prepass;
    REQUIRE(SUCCEEDED(dev->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&geometry))));
    pd.DepthStencilState.DepthEnable=FALSE;pd.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;
    REQUIRE(SUCCEEDED(dev->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&post))));
    pd.DepthStencilState.DepthEnable=TRUE;pd.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;
    pd.NumRenderTargets=0;pd.RTVFormats[0]=DXGI_FORMAT_UNKNOWN;
    REQUIRE(SUCCEEDED(dev->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&prepass))));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[3]{};UINT64 total=0;
    ID3D12Resource* inputs[]={color.Get(),depth.Get(),untouched.Get()};
    for(int i=0;i<3;++i){auto desc=inputs[i]->GetDesc();UINT64 bytes=0;dev->GetCopyableFootprints(&desc,0,1,total,&fp[i],nullptr,nullptr,&bytes);total+=bytes;total=(total+511)&~UINT64(511);}
    auto rb=test::CreateBuffer(dev,total,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);REQUIRE(rb);
    test::List l;REQUIRE(l.Create(dev));REQUIRE(SUCCEEDED(l.list->Close()));
    const D3D12_VIEWPORT vp{0,0,float(W),float(H),0,1};const D3D12_RECT sc{0,0,LONG(W),LONG(H)};
    // Halton phases cover both signs; the same recording also draws a depth-
    // disabled post/HUD pass without setting the viewport again.
    for(int phase=0;phase<8;++phase){
        sample_x=(phase%4-1.5)*0.125;sample_y=(phase%3-1)*0.25;
        calls=0;l.Reset();na::observe::SetRasterJitterCallback(&Sample);
        l.list->SetGraphicsRootSignature(root.Get());l.list->RSSetViewports(1,&vp);l.list->RSSetScissorRects(1,&sc);
        l.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        l.list->SetPipelineState(geometry.Get());l.list->OMSetRenderTargets(1,&rtv,FALSE,&dsv);
        l.list->DrawInstanced(3,1,0,0);
        double jx=0,jy=0;REQUIRE(na::observe::ListRasterJitter(l.list.Get(),&jx,&jy));
        CHECK(jx==sample_x && jy==sample_y && calls==1);
        l.list->SetPipelineState(prepass.Get());l.list->OMSetRenderTargets(0,nullptr,FALSE,&dsv);
        l.list->DrawInstanced(3,1,0,0);CHECK(calls==2);
        l.list->SetPipelineState(post.Get());l.list->OMSetRenderTargets(1,&other,FALSE,&dsv);
        l.list->DrawInstanced(3,1,0,0);CHECK(calls==2);
        for(int i=0;i<3;++i){
            auto state=i==1?D3D12_RESOURCE_STATE_DEPTH_WRITE:D3D12_RESOURCE_STATE_RENDER_TARGET;
            auto b=test::Transition(inputs[i],state,D3D12_RESOURCE_STATE_COPY_SOURCE);l.list->ResourceBarrier(1,&b);
            D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=inputs[i];src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.pResource=rb.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp[i];
            l.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);std::swap(b.Transition.StateBefore,b.Transition.StateAfter);l.list->ResourceBarrier(1,&b);
        }
        REQUIRE(SUCCEEDED(l.list->Close()));ID3D12CommandList* lists[]={l.list.Get()};d.queue->ExecuteCommandLists(1,lists);d.WaitIdle();
        uint8_t* data=nullptr;REQUIRE(SUCCEEDED(rb->Map(0,nullptr,reinterpret_cast<void**>(&data))));
        const UINT x=64,y=32;auto pixel=[&](int i){return reinterpret_cast<const float*>(data+fp[i].Offset+y*fp[i].Footprint.RowPitch)+x*(i==1?1:4);};
        const float* c=pixel(0);const float z=*pixel(1);const float* u=pixel(2);
        const double dx=(double(x)+0.5)/W-c[0],dy=(double(y)+0.5)/H-c[1];
        printf("phase %d: commanded %.6f %.6f, raster %.6f %.6f\n",phase,jx,jy,dx*W,dy*H);
        CHECK(std::abs(dx*W-jx)<0.002 && std::abs(dy*H-jy)<0.002);
        CHECK(std::abs(z-(0.2+0.4*c[0]))<1e-6);
        CHECK(std::abs(u[0]-(x+0.5)/W)<1e-6 && std::abs(u[1]-(y+0.5)/H)<1e-6);
        rb->Unmap(0,nullptr);
    }
    l.Reset();double x=0,y=0;CHECK(!na::observe::ListRasterJitter(l.list.Get(),&x,&y));
    REQUIRE(SUCCEEDED(l.list->Close()));na::observe::SetRasterJitterCallback(nullptr);
    double record_us[2]{};
    // Recording-only timing; these lists are discarded, never submitted.
    // Report the incremental work rather than charging all observer work to J1.
    for(int enabled=0;!validation && enabled<2;++enabled){
        l.Reset();l.list->SetGraphicsRootSignature(root.Get());l.list->SetPipelineState(geometry.Get());
        l.list->OMSetRenderTargets(1,&rtv,FALSE,&dsv);l.list->RSSetViewports(1,&vp);l.list->RSSetScissorRects(1,&sc);
        l.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        na::observe::SetRasterJitterCallback(enabled?&Sample:nullptr);
        const auto begin=std::chrono::steady_clock::now();
        for(int i=0;i<20000;++i)l.list->DrawInstanced(3,1,0,0);
        record_us[enabled]=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-begin).count()/20000;
        REQUIRE(SUCCEEDED(l.list->Close()));
    }
    printf("recording cost: observer %.3f us/draw, raster %.3f us/draw, delta %.3f us/draw\n",record_us[0],record_us[1],record_us[1]-record_us[0]);
    na::observe::SetRasterJitterCallback(nullptr);
    if(validation && debug){
        test::ComPtr<ID3D12InfoQueue> info;REQUIRE(SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&info))));
        for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T size=0;info->GetMessage(i,nullptr,&size);std::vector<uint8_t> bytes(size);
            auto* msg=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());info->GetMessage(i,msg,&size);
            if(msg->Severity==D3D12_MESSAGE_SEVERITY_ERROR || msg->Severity==D3D12_MESSAGE_SEVERITY_CORRUPTION){printf("D3D12: %s\n",msg->pDescription);CHECK(false);}
        }
    }
    return test::finish("raster_jitter_test");
}
