// VARIANT: jitter/shader-depth-readers (DX12). A scene writes a depth-like R32_FLOAT layer with
// depth-tested draws, then mesh draws with the hardware test off compare against it in the pixel
// shader. With viewport jitter on, the readers must rasterise on the layer's grid or they fail
// their own comparison (Three Kingdoms terrain, 2026-11-21). No game or NGX model: the reader
// writes 1 where its own depth equals the layer's and 0 where it does not.
#include "test_support.hpp"
#include "host_dx12/observe/producers.hpp"
#include <d3dcompiler.h>
#include <cmath>
void __cdecl sink(const na_event_batch*,void*){}
int main(){
    test::Device d;if(!test::CreateDevice(d))return test::kSkip;auto* dev=d.device.Get();
    REQUIRE(na::observe::InstallProducers(dev,d.queue.Get(),false));na::observe::SetRasterStateTracking(true);
    na_observe_config config{};config.size_bytes=sizeof config;REQUIRE(na_observe_start(&config,&sink,nullptr)==NA_OK);
    constexpr UINT W=64,H=32;constexpr auto RT=D3D12_RESOURCE_STATE_RENDER_TARGET;constexpr auto PSR=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    constexpr auto F4=DXGI_FORMAT_R32G32B32A32_FLOAT;
    auto layer=test::CreateTexture(dev,W,H,DXGI_FORMAT_R32_FLOAT,RT,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto copy=test::CreateTexture(dev,W,H,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE);
    auto mat_a=test::CreateTexture(dev,W,H,F4,RT,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto mat_b=test::CreateTexture(dev,W,H,F4,RT,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto depth=test::CreateTexture(dev,W,H,DXGI_FORMAT_D32_FLOAT,D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    REQUIRE(layer&&copy&&mat_a&&mat_b&&depth);
    na::observe::SetLiveSelection(na::observe::IdOf(depth.Get()),na::observe::IdOf(mat_a.Get()));
    test::ComPtr<ID3D12DescriptorHeap> rh,dh,views;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,3,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rh))));
    hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;hd.NumDescriptors=1;REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&dh))));
    hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=2;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&views))));
    const UINT rtv_step=dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const UINT srv_step=dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE layer_rt=rh->GetCPUDescriptorHandleForHeapStart(),mats[2]{layer_rt,layer_rt};
    mats[0].ptr+=rtv_step;mats[1].ptr+=2*size_t(rtv_step);const auto dsv=dh->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(layer.Get(),nullptr,layer_rt);dev->CreateRenderTargetView(mat_a.Get(),nullptr,mats[0]);
    dev->CreateRenderTargetView(mat_b.Get(),nullptr,mats[1]);dev->CreateDepthStencilView(depth.Get(),nullptr,dsv);
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=DXGI_FORMAT_R32_FLOAT;sd.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;sd.Texture2D.MipLevels=1;
    auto cpu=views->GetCPUDescriptorHandleForHeapStart();dev->CreateShaderResourceView(layer.Get(),&sd,cpu);
    cpu.ptr+=srv_step;dev->CreateShaderResourceView(copy.Get(),&sd,cpu);
    const auto layer_view=views->GetGPUDescriptorHandleForHeapStart();auto copy_view=layer_view;copy_view.ptr+=srv_step;

    test::ComPtr<ID3DBlob> vs,ps_layer,ps_read,blob,error;test::ComPtr<ID3D12RootSignature> root;
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameter.DescriptorTable={1,&range};parameter.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=1;rd.pParameters=&parameter;
    REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error)));
    REQUIRE(SUCCEEDED(dev->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root))));
    // Two triangles per six vertices over the whole target; depth rises left to right, so a
    // fraction of a pixel sideways is a different depth at every pixel centre.
    constexpr char shader[]=R"(struct V{float4 p:SV_Position;};
static const float2 q[6]={float2(0,0),float2(1,0),float2(0,1),float2(0,1),float2(1,0),float2(1,1)};
V VS(uint i:SV_VertexID){V v;float2 uv=q[i%6];v.p=float4(uv.x*2-1,1-uv.y*2,.25+.5*uv.x,1);return v;}
float PSLayer(V v):SV_Target{return v.p.z;}
Texture2D<float> layer:register(t0);
struct O{float4 a:SV_Target0;float4 b:SV_Target1;};
O PSRead(V v){float s=layer.Load(int3(v.p.xy,0));O o;o.a=o.b=float4(abs(s-v.p.z)<1e-6?1.:0.,s,v.p.z,1);return o;})";
    REQUIRE(SUCCEEDED(D3DCompile(shader,sizeof(shader),nullptr,nullptr,nullptr,"VS","vs_5_0",0,0,&vs,&error)));
    REQUIRE(SUCCEEDED(D3DCompile(shader,sizeof(shader),nullptr,nullptr,nullptr,"PSLayer","ps_5_0",0,0,&ps_layer,&error)));
    REQUIRE(SUCCEEDED(D3DCompile(shader,sizeof(shader),nullptr,nullptr,nullptr,"PSRead","ps_5_0",0,0,&ps_read,&error)));
    test::ComPtr<ID3D12PipelineState> write,read;D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature=root.Get();pd.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pd.PS={ps_layer->GetBufferPointer(),ps_layer->GetBufferSize()};
    pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pd.RasterizerState.DepthClipEnable=TRUE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask=15;pd.BlendState.RenderTarget[1].RenderTargetWriteMask=15;pd.SampleMask=UINT_MAX;pd.SampleDesc.Count=1;
    pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;pd.NumRenderTargets=1;pd.RTVFormats[0]=DXGI_FORMAT_R32_FLOAT;
    pd.DSVFormat=DXGI_FORMAT_D32_FLOAT;pd.DepthStencilState.DepthEnable=TRUE;pd.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
    REQUIRE(SUCCEEDED(dev->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&write))));
    // The material pass: hardware depth test off, two targets, the scene depth still bound.
    pd.PS={ps_read->GetBufferPointer(),ps_read->GetBufferSize()};pd.NumRenderTargets=2;pd.RTVFormats[0]=pd.RTVFormats[1]=F4;
    pd.DepthStencilState.DepthEnable=FALSE;pd.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;
    REQUIRE(SUCCEEDED(dev->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&read))));

    auto rb=test::CreateBuffer(dev,W*H*16,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);REQUIRE(rb);
    const D3D12_VIEWPORT vp{0,0,float(W),float(H),0,1};const D3D12_RECT whole{0,0,W,H};
    const float grey[]{.5f,.5f,.5f,.5f};
    const auto common=[&](ID3D12GraphicsCommandList* l){
        ID3D12DescriptorHeap* heaps[]={views.Get()};l->SetDescriptorHeaps(1,heaps);l->SetGraphicsRootSignature(root.Get());
        l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);l->RSSetViewports(1,&vp);l->RSSetScissorRects(1,&whole);
    };
    // The 258-draw pass of the game in one draw: depth-tested, writes the layer first.
    const auto produce=[&](ID3D12GraphicsCommandList* l){
        common(l);l->SetPipelineState(write.Get());l->OMSetRenderTargets(1,&layer_rt,FALSE,&dsv);
        l->SetGraphicsRootDescriptorTable(0,copy_view);l->DrawInstanced(12,1,0,0);
    };
    // Rows 0..9: the mesh reading the layer itself. Rows 12..19: the same mesh reading an identical
    // copy nobody saw being written. Rows 22..29: a full-screen pass (six vertices) reading the layer.
    const auto consume=[&](ID3D12GraphicsCommandList* l){
        common(l);l->SetPipelineState(read.Get());l->OMSetRenderTargets(2,mats,FALSE,&dsv);
        l->ClearRenderTargetView(mats[0],grey,0,nullptr);
        D3D12_RECT band{0,0,W,10};l->RSSetScissorRects(1,&band);l->SetGraphicsRootDescriptorTable(0,layer_view);l->DrawInstanced(12,1,0,0);
        band={0,12,W,20};l->RSSetScissorRects(1,&band);l->SetGraphicsRootDescriptorTable(0,copy_view);l->DrawInstanced(12,1,0,0);
        band={0,22,W,30};l->RSSetScissorRects(1,&band);l->SetGraphicsRootDescriptorTable(0,layer_view);l->DrawInstanced(6,1,0,0);
    };
    const auto read_back=[&](ID3D12GraphicsCommandList* l){
        auto b=test::Transition(mat_a.Get(),RT,D3D12_RESOURCE_STATE_COPY_SOURCE);l->ResourceBarrier(1,&b);
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=mat_a.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource=rb.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint.Footprint={F4,W,H,1,W*16};
        l->CopyTextureRegion(&dst,0,0,0,&src,nullptr);std::swap(b.Transition.StateBefore,b.Transition.StateAfter);l->ResourceBarrier(1,&b);
    };
    const auto check=[&](const char* what)->int{
        float* pixels=nullptr;REQUIRE(SUCCEEDED(rb->Map(0,nullptr,reinterpret_cast<void**>(&pixels))));
        const auto ok=[&](UINT x,UINT y){return pixels[(y*W+x)*4];};
        std::printf("%s: layer reader %.0f/%.0f, copy reader %.0f/%.0f, full-screen reader %.0f/%.0f, untouched %.1f\n",what,
                    ok(24,5),ok(48,5),ok(24,15),ok(48,15),ok(24,25),ok(48,25),ok(24,11));
        CHECK(ok(24,5)==1.f&&ok(48,5)==1.f&&ok(1,1)==1.f&&ok(W-2,8)==1.f); // on the layer's grid: its own depth
        CHECK(ok(24,15)==0.f&&ok(48,15)==0.f); // unproved copy: the game's viewport, so the comparison fails
        CHECK(ok(24,25)==0.f&&ok(48,25)==0.f); // a full-screen pass is never moved
        CHECK(ok(24,11)==.5f&&ok(24,21)==.5f); // the scissors held
        rb->Unmap(0,nullptr);
        return 0;
    };
    test::List l;REQUIRE(l.Create(dev));
    // Frame 1, one recording: writer, then readers. The jitter is the sample Three Kingdoms failed on.
    na::observe::SetSceneViewportJitter(1,.323165f,-.452396f);
    produce(l.list.Get());
    {
        D3D12_RESOURCE_BARRIER b[2]{test::Transition(layer.Get(),RT,D3D12_RESOURCE_STATE_COPY_SOURCE),test::Transition(copy.Get(),D3D12_RESOURCE_STATE_COPY_DEST,PSR)};
        l.list->ResourceBarrier(1,&b[0]);l.list->CopyResource(copy.Get(),layer.Get());
        b[0]=test::Transition(layer.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,PSR);l.list->ResourceBarrier(2,b);
    }
    consume(l.list.Get());read_back(l.list.Get());
    REQUIRE(SUCCEEDED(l.list->Close()));ID3D12CommandList* one[]={l.list.Get()};d.queue->ExecuteCommandLists(1,one);d.WaitIdle();
    REQUIRE(check("frame 1, one recording")==0);
    auto stats=na::observe::GetViewportJitterStats();
    CHECK(stats.shader_depth==1);CHECK(stats.shader_depth_refused.not_read==1);

    // Frame 2, two recordings made in the other order: the reader is recorded before the writer
    // of its frame, as parallel recording allows, and executed after it.
    na::observe::SetSceneViewportJitter(2,-.470734f,.419042f);
    test::List reader;REQUIRE(reader.Create(dev));
    consume(reader.list.Get());read_back(reader.list.Get());
    {
        auto b=test::Transition(layer.Get(),PSR,RT);reader.list->ResourceBarrier(1,&b);
    }
    REQUIRE(SUCCEEDED(reader.list->Close()));
    l.Reset();
    {
        D3D12_RESOURCE_BARRIER b[2]{test::Transition(layer.Get(),PSR,RT),test::Transition(copy.Get(),PSR,D3D12_RESOURCE_STATE_COPY_DEST)};
        l.list->ResourceBarrier(2,b);
    }
    produce(l.list.Get());
    {
        D3D12_RESOURCE_BARRIER b[2]{test::Transition(layer.Get(),RT,D3D12_RESOURCE_STATE_COPY_SOURCE),test::Transition(copy.Get(),D3D12_RESOURCE_STATE_COPY_DEST,PSR)};
        l.list->ResourceBarrier(1,&b[0]);l.list->CopyResource(copy.Get(),layer.Get());
        b[0]=test::Transition(layer.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,PSR);l.list->ResourceBarrier(2,b);
    }
    REQUIRE(SUCCEEDED(l.list->Close()));ID3D12CommandList* two[]={l.list.Get(),reader.list.Get()};d.queue->ExecuteCommandLists(2,two);d.WaitIdle();
    REQUIRE(check("frame 2, reader recorded first")==0);
    stats=na::observe::GetViewportJitterStats();
    CHECK(stats.shader_depth==2);CHECK(stats.shader_depth_refused.not_read==2);

    // Jitter off: nothing moves, and every reader agrees with the layer it reads.
    na::observe::SetSceneViewportJitter(0,0,0);
    l.Reset();
    {
        D3D12_RESOURCE_BARRIER b[2]{test::Transition(layer.Get(),RT,PSR),test::Transition(copy.Get(),PSR,D3D12_RESOURCE_STATE_COPY_DEST)};
        l.list->ResourceBarrier(1,&b[1]);std::swap(b[0].Transition.StateBefore,b[0].Transition.StateAfter);
    }
    produce(l.list.Get());
    {
        D3D12_RESOURCE_BARRIER b[2]{test::Transition(layer.Get(),RT,D3D12_RESOURCE_STATE_COPY_SOURCE),test::Transition(copy.Get(),D3D12_RESOURCE_STATE_COPY_DEST,PSR)};
        l.list->ResourceBarrier(1,&b[0]);l.list->CopyResource(copy.Get(),layer.Get());
        b[0]=test::Transition(layer.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,PSR);l.list->ResourceBarrier(2,b);
    }
    consume(l.list.Get());read_back(l.list.Get());
    REQUIRE(SUCCEEDED(l.list->Close()));d.queue->ExecuteCommandLists(1,one);d.WaitIdle();
    {
        float* pixels=nullptr;REQUIRE(SUCCEEDED(rb->Map(0,nullptr,reinterpret_cast<void**>(&pixels))));
        CHECK(pixels[(5*W+24)*4]==1.f&&pixels[(15*W+24)*4]==1.f&&pixels[(25*W+24)*4]==1.f);rb->Unmap(0,nullptr);
    }
    stats=na::observe::GetViewportJitterStats();
    CHECK(stats.shader_depth==2); // no sample, no move
    CHECK(dev->GetDeviceRemovedReason()==S_OK);na_observe_stop();na::observe::SetProcessExiting();
    return test::finish("shader_depth_jitter_test");
}
