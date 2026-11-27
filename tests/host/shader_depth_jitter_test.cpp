// VARIANT: jitter/shader-depth-readers (DX12). A scene writes a depth-like R32_FLOAT layer with
// depth-tested draws, then mesh draws with the hardware test off compare against it in the pixel
// shader. With viewport jitter on, the readers must rasterise on the layer's grid or they fail
// their own comparison (Three Kingdoms terrain, 2026-11-21). No game or NGX model: the reader
// writes 1 where its own depth equals the layer's and 0 where it does not.
#include "test_support.hpp"
#include "host_dx12/observe/producers.hpp"
#include "core/referee_coverage.hpp"
#include <d3dcompiler.h>
#include <cmath>
void __cdecl sink(const na_event_batch*,void*){}
int main(){
    test::Device d;if(!test::CreateDevice(d))return test::kSkip;auto* dev=d.device.Get();
    REQUIRE(na::observe::InstallProducers(dev,d.queue.Get(),false));na::observe::SetRasterStateTracking(true);
    // The shader-depth rule alone first (jitter/depth-tested-only); the overlay rule is checked at the end.
    na::observe::SetSceneOverlayJitter(false,"test: the shader-depth rule alone");
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
    // Astra's stationary source (2026-11-26): the layer drawn with no depth bound and the test off, so it keeps the
    // game's grid while a sample is live.
    test::ComPtr<ID3D12PipelineState> write_still;
    {
        auto sd2=pd;sd2.PS={ps_layer->GetBufferPointer(),ps_layer->GetBufferSize()};sd2.NumRenderTargets=1;sd2.RTVFormats[0]=DXGI_FORMAT_R32_FLOAT;sd2.RTVFormats[1]=DXGI_FORMAT_UNKNOWN;
        sd2.DSVFormat=DXGI_FORMAT_UNKNOWN;sd2.DepthStencilState.DepthEnable=FALSE;sd2.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;
        REQUIRE(SUCCEEDED(dev->CreateGraphicsPipelineState(&sd2,IID_PPV_ARGS(&write_still))));
    }

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

    // VARIANT: jitter/scene-overlays (the default): a mesh drawn into the scene colour with the depth test
    // off and the scene depth bound takes the sample whatever it reads, so the reader of the unproved copy
    // now rasterises on the copy's (sampled) grid too; the full-screen pass still never moves.
    na::observe::SetSceneOverlayJitter(true,"test: overlays on");
    na::observe::SetSceneViewportJitter(4,.323165f,-.452396f);
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
    consume(l.list.Get());read_back(l.list.Get());
    REQUIRE(SUCCEEDED(l.list->Close()));d.queue->ExecuteCommandLists(1,one);d.WaitIdle();
    {
        float* pixels=nullptr;REQUIRE(SUCCEEDED(rb->Map(0,nullptr,reinterpret_cast<void**>(&pixels))));
        const auto ok=[&](UINT x,UINT y){return pixels[(y*W+x)*4];};
        std::printf("overlays on: layer reader %.0f/%.0f, copy reader %.0f/%.0f, full-screen reader %.0f/%.0f\n",
                    ok(24,5),ok(48,5),ok(24,15),ok(48,15),ok(24,25),ok(48,25));
        CHECK(ok(24,5)==1.f&&ok(48,5)==1.f);   // the proven reader, as before
        CHECK(ok(24,15)==1.f&&ok(48,15)==1.f); // the overlay rule: the copy reader is on the sample's grid too
        CHECK(ok(24,25)==0.f&&ok(48,25)==0.f); // a full-screen pass is never moved
        rb->Unmap(0,nullptr);
    }
    stats=na::observe::GetViewportJitterStats();
    std::printf("overlays on: variant=%s overlay_moved=%llu left{other_target=%llu own_viewport=%llu rule_off=%llu after_handover=%llu}\n",
                stats.overlays?"jitter/scene-overlays":"jitter/depth-tested-only",(unsigned long long)stats.overlay_moved,
                (unsigned long long)stats.left.other_target,(unsigned long long)stats.left.own_viewport,
                (unsigned long long)stats.left.rule_off,(unsigned long long)stats.left.after_handover);
    CHECK(stats.overlays&&stats.overlay_moved>=1);

    // VARIANT: jitter/stationary-source-readers (Astra's reproducer): the layer drawn on the game's grid while the
    // sample is live. Its readers, of the layer itself and of its exact copy, keep the game's grid at both of the
    // samples Three Kingdoms failed on; moved they read the wrong texel (0.0 in Astra's fixture).
    for(const auto& off:{std::pair{.323165f,-.452396f},std::pair{-.470734f,.419042f}}){
        static uint32_t f=5;na::observe::SetSceneViewportJitter(f++,off.first,off.second);
        l.Reset();
        {
            D3D12_RESOURCE_BARRIER b[2]{test::Transition(layer.Get(),PSR,RT),test::Transition(copy.Get(),PSR,D3D12_RESOURCE_STATE_COPY_DEST)};
            l.list->ResourceBarrier(2,b);
        }
        common(l.list.Get());l.list->SetPipelineState(write_still.Get());l.list->OMSetRenderTargets(1,&layer_rt,FALSE,nullptr);
        l.list->SetGraphicsRootDescriptorTable(0,copy_view);l.list->DrawInstanced(12,1,0,0);
        {
            D3D12_RESOURCE_BARRIER b[2]{test::Transition(layer.Get(),RT,D3D12_RESOURCE_STATE_COPY_SOURCE),test::Transition(copy.Get(),D3D12_RESOURCE_STATE_COPY_DEST,PSR)};
            l.list->ResourceBarrier(1,&b[0]);l.list->CopyResource(copy.Get(),layer.Get());
            b[0]=test::Transition(layer.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,PSR);l.list->ResourceBarrier(2,b);
        }
        consume(l.list.Get());read_back(l.list.Get());
        REQUIRE(SUCCEEDED(l.list->Close()));d.queue->ExecuteCommandLists(1,one);d.WaitIdle();
        float* pixels=nullptr;REQUIRE(SUCCEEDED(rb->Map(0,nullptr,reinterpret_cast<void**>(&pixels))));
        const auto ok=[&](UINT x,UINT y){return pixels[(y*W+x)*4];};
        std::printf("stationary source, sample %.3f,%.3f: layer reader %.0f/%.0f, copy reader %.0f/%.0f, full-screen %.0f/%.0f\n",
                    off.first,off.second,ok(24,5),ok(48,5),ok(24,15),ok(48,15),ok(24,25),ok(48,25));
        CHECK(ok(24,5)==1.f&&ok(48,5)==1.f);   // reads the stationary layer: stays on its grid
        CHECK(ok(24,15)==1.f&&ok(48,15)==1.f); // reads its exact copy: the same
        CHECK(ok(24,25)==1.f&&ok(48,25)==1.f); // the full-screen pass never moves, and its source did not either
        rb->Unmap(0,nullptr);
    }
    stats=na::observe::GetViewportJitterStats();
    std::printf("stationary source: left_on_game_grid{stationary_source=%llu}\n",(unsigned long long)stats.left.stationary_source);
    CHECK(stats.left.stationary_source>=2);

    // VARIANT: referee/coverage-trial on DX12: with the overlay rule off, the copy reader is left on the game's grid
    // (class rule_off). The Referee moving that class puts it on the sample; a trial put back demotes the pipeline,
    // which then stays on the game's grid even while the class is moved again.
    {
        na::observe::SetSceneOverlayJitter(false,"test: the coverage trial");
        const auto frame_with=[&](uint32_t f)->std::pair<float,float>{
            na::observe::SetSceneViewportJitter(f,.323165f,-.452396f);
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
            consume(l.list.Get());read_back(l.list.Get());
            if(FAILED(l.list->Close()))return {-1.f,-1.f};
            d.queue->ExecuteCommandLists(1,one);d.WaitIdle();
            float* pixels=nullptr;if(FAILED(rb->Map(0,nullptr,reinterpret_cast<void**>(&pixels))))return {-1.f,-1.f};
            const std::pair<float,float> r{pixels[(15*W+24)*4],pixels[(15*W+48)*4]};rb->Unmap(0,nullptr);return r;
        };
        constexpr unsigned rule_off=unsigned(na::referee::LeftClass::RuleOff);
        uint64_t left[na::referee::kLeftClasses];na::observe::LeftByClass(left);
        na::observe::SetCoverage(0,-1,0,-1,false);
        const auto still=frame_with(11);
        na::observe::LeftByClass(left);
        const uint64_t left_rule_off=left[rule_off];
        na::observe::SetCoverage(1u<<rule_off,int(rule_off),0,-1,false); // on trial
        const auto moved=frame_with(12);
        const uint64_t moved_draws=na::observe::CoverageMovedDraws();
        na::observe::SetCoverage(0,-1,1,int(rule_off),false);             // put back: its pipeline is demoted
        na::observe::SetCoverage(1u<<rule_off,-1,1,int(rule_off),false);  // the class moved again later
        const auto demoted=frame_with(13);
        std::printf("coverage trial (dx12): rule_off left %llu; copy reader %.0f/%.0f left, %.0f/%.0f moved by the Referee (%llu draws), "
                    "%.0f/%.0f after its pipeline was put back\n",(unsigned long long)left_rule_off,still.first,still.second,moved.first,moved.second,
                    (unsigned long long)moved_draws,demoted.first,demoted.second);
        CHECK(left_rule_off>=1);
        CHECK(still.first==0.f&&still.second==0.f);
        CHECK(moved.first==1.f&&moved.second==1.f&&moved_draws>=1);
        CHECK(demoted.first==0.f&&demoted.second==0.f);
        na::observe::SetCoverage(0,-1,1,int(rule_off),false);
    }
    CHECK(dev->GetDeviceRemovedReason()==S_OK);na_observe_stop();na::observe::SetProcessExiting();
    return test::finish("shader_depth_jitter_test");
}
