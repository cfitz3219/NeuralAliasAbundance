// Actual raster samples, immutable command-list ownership, and untouched post/HUD
// viewports. No game or NGX model: read back an interpolated coordinate witness.
#include "test_support.hpp"
#include "host_dx12/observe/producers.hpp"
#include <d3dcompiler.h>
#include <cmath>
#include <thread>
void __cdecl sink(const na_event_batch*,void*){}
int main(){
    test::Device d;if(!test::CreateDevice(d))return test::kSkip;auto* dev=d.device.Get();
    REQUIRE(na::observe::InstallProducers(dev,d.queue.Get(),false));na::observe::SetRasterStateTracking(true);
    na_observe_config config{};config.size_bytes=sizeof config;REQUIRE(na_observe_start(&config,&sink,nullptr)==NA_OK);
    constexpr UINT W=64,H=32;constexpr auto RT=D3D12_RESOURCE_STATE_RENDER_TARGET;
    auto color=test::CreateTexture(dev,W,H,DXGI_FORMAT_R32G32B32A32_FLOAT,RT,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto depth=test::CreateTexture(dev,W,H,DXGI_FORMAT_D32_FLOAT,D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    REQUIRE(color&&depth);na::observe::SetLiveSelection(na::observe::IdOf(depth.Get()),na::observe::IdOf(color.Get()));
    test::ComPtr<ID3D12DescriptorHeap> rh,dh;D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rh))));hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&dh))));auto rtv=rh->GetCPUDescriptorHandleForHeapStart(),dsv=dh->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(color.Get(),nullptr,rtv);dev->CreateDepthStencilView(depth.Get(),nullptr,dsv);
    test::ComPtr<ID3DBlob> vs,ps,blob,error;test::ComPtr<ID3D12RootSignature> root;
    D3D12_ROOT_SIGNATURE_DESC rd{};REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error)));
    REQUIRE(SUCCEEDED(dev->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root))));
    constexpr char shader[]=R"(struct V{float4 p:SV_Position;float2 uv:TEXCOORD;};
V VS(uint i:SV_VertexID){V v;v.uv=float2((i<<1)&2,i&2);v.p=float4(v.uv.x*2-1,1-v.uv.y*2,.5,1);return v;}
float4 PS(V v):SV_Target{float glint=step(24.45/64.,v.uv.x)*(1.-step(25.5/64.,v.uv.x));
return float4(v.uv,glint,1);})";
    REQUIRE(SUCCEEDED(D3DCompile(shader,sizeof(shader),nullptr,nullptr,nullptr,"VS","vs_5_0",0,0,&vs,&error)));
    REQUIRE(SUCCEEDED(D3DCompile(shader,sizeof(shader),nullptr,nullptr,nullptr,"PS","ps_5_0",0,0,&ps,&error)));
    test::ComPtr<ID3D12PipelineState> scene,post;D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature=root.Get();pd.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pd.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
    pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pd.RasterizerState.DepthClipEnable=TRUE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask=15;pd.SampleMask=UINT_MAX;pd.SampleDesc.Count=1;
    pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;pd.NumRenderTargets=1;pd.RTVFormats[0]=DXGI_FORMAT_R32G32B32A32_FLOAT;
    pd.DSVFormat=DXGI_FORMAT_D32_FLOAT;pd.DepthStencilState.DepthEnable=TRUE;pd.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
    REQUIRE(SUCCEEDED(dev->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&scene))));pd.DepthStencilState.DepthEnable=FALSE;
    REQUIRE(SUCCEEDED(dev->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&post))));
    test::ComPtr<ID3D12PipelineState> glass;
    pd.DepthStencilState.DepthEnable=TRUE;pd.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;
    auto& blend=pd.BlendState.RenderTarget[0];blend.BlendEnable=TRUE;
    blend.SrcBlend=D3D12_BLEND_ONE;blend.DestBlend=D3D12_BLEND_INV_SRC_ALPHA;blend.BlendOp=D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha=D3D12_BLEND_ONE;blend.DestBlendAlpha=D3D12_BLEND_ZERO;blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;
    REQUIRE(SUCCEEDED(dev->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&glass))));
    test::List l;REQUIRE(l.Create(dev));auto rb=test::CreateBuffer(dev,W*H*16,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);REQUIRE(rb);
    for(UINT round=0;round<2;++round){
        if(round)l.Reset();na::observe::SetSceneViewportJitter(round+1,round?-.375f:.25f,round?.125f:-.375f);
        const float jx=round?-.375f:.25f,jy=round?.125f:-.375f;
        l.list->SetGraphicsRootSignature(root.Get());l.list->SetPipelineState(scene.Get());l.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        l.list->OMSetRenderTargets(1,&rtv,FALSE,&dsv);D3D12_VIEWPORT vp{0,0,float(W),float(H),0,1};l.list->RSSetViewports(1,&vp);
        const float clear[]{1,1,1,1};
        l.list->ClearRenderTargetView(rtv,clear,0,nullptr);
        D3D12_RECT sc{0,0,32,32};l.list->RSSetScissorRects(1,&sc);
        if (!round) {
            // An entirely inactive call can skip the jitter registry. A later
            // recording thread must still pin a real sample, and turning the
            // request off must not drop it from this unfinished recording.
            na::observe::SetSceneViewportJitter(0,0,0);l.list->DrawInstanced(3,0,0,0);
            na::observe::SetSceneViewportJitter(round+1,jx,jy);
            std::thread recorder([&]{l.list->DrawInstanced(3,1,0,0);});recorder.join();
            na::observe::SetSceneViewportJitter(0,0,0);
        } else {
            l.list->DrawInstanced(3,1,0,0);
            na::observe::SetSceneViewportJitter(99,-.125f,.4375f); // another thread's later frame
        }
        sc={32,0,64,32};l.list->RSSetScissorRects(1,&sc);l.list->DrawInstanced(3,1,0,0);
        sc={0,8,64,16};l.list->RSSetScissorRects(1,&sc);l.list->SetPipelineState(post.Get());l.list->DrawInstanced(3,1,0,0);
        const auto color_id=na::observe::IdOf(color.Get()), depth_id=na::observe::IdOf(depth.Get());
        l.list->SetPipelineState(glass.Get());sc={0,16,64,19};l.list->RSSetScissorRects(1,&sc);
        l.list->DrawInstanced(3,1,0,0); // the same glass drawn before reconstruction still takes the scene sample
        const auto resolved=na::observe::FinishSceneRaster(l.list.Get(),color_id,0,depth_id,0,0);
        CHECK(resolved.sample!=0);
        sc={0,22,64,25};l.list->RSSetScissorRects(1,&sc);l.list->DrawInstanced(3,1,0,0); // unchanged PSO and DSV
        auto moved=vp;moved.TopLeftX+=2.f;
        l.list->RSSetViewports(1,&moved);sc={0,24,64,25};l.list->RSSetScissorRects(1,&sc);l.list->DrawInstanced(3,1,0,0);
        l.list->RSSetViewports(1,&vp);
        l.list->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,1.f,0,0,nullptr);
        sc={0,26,64,29};l.list->RSSetScissorRects(1,&sc);l.list->DrawInstanced(3,1,0,0);
        uint32_t frame=0;float x=0,y=0;REQUIRE(na::observe::DecodeSceneViewportJitter(na::observe::ListSceneViewportJitter(l.list.Get()),&frame,&x,&y));
        CHECK(frame==round+1&&x==jx&&y==jy);
        CHECK(!na::observe::ListSceneViewportJitter(l.list.Get(),na::observe::IdOf(color.Get())));
        auto barrier=test::Transition(color.Get(),RT,D3D12_RESOURCE_STATE_COPY_SOURCE);l.list->ResourceBarrier(1,&barrier);
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=color.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource=rb.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint.Footprint={DXGI_FORMAT_R32G32B32A32_FLOAT,W,H,1,W*16};
        l.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);l.list->ResourceBarrier(1,&barrier);
        REQUIRE(SUCCEEDED(l.list->Close()));ID3D12CommandList* lists[]={l.list.Get()};d.queue->ExecuteCommandLists(1,lists);d.WaitIdle();
        float* pixels=nullptr;REQUIRE(SUCCEEDED(rb->Map(0,nullptr,reinterpret_cast<void**>(&pixels))));
        for(UINT X:{0u,16u,48u,W-1}){
            const auto k=(20*W+X)*4;
            CHECK(std::abs(pixels[k]-(float(X)+1.f-jx)/(W+1))<1e-5f);
            CHECK(std::abs(pixels[k+1]-(21.f-jy)/(H+1))<1e-5f);
        }
        CHECK(std::abs(pixels[(17*W+24)*4]-(25.f-jx)/(W+1))<1e-5f);
        CHECK(std::abs(pixels[(23*W+24)*4]-24.5f/W)<1e-5f);
        CHECK(std::abs(pixels[(23*W+24)*4+1]-23.5f/H)<1e-5f);
        CHECK(std::abs(pixels[(27*W+24)*4]-(25.f-jx)/(W+1))<1e-5f);
        const auto glint=[&](UINT x,UINT y){return pixels[(y*W+x)*4+2];};
        std::printf("phase %u glint: scene %.0f, resolved glass %.0f, moving glass %.0f -> %.0f\n",
                    round,glint(24,17),glint(24,23),glint(24,24),glint(26,24));
        CHECK(glint(24,17)==float(round)); // changes between dark and bright before DLAA
        CHECK(glint(24,23)==1.f);          // stationary glass keeps one brightness on both phases
        CHECK(glint(24,24)==0.f && glint(26,24)==1.f); // actual two-pixel movement survives immediately
        CHECK(pixels[(20*W+W-1)*4+2]==0.f);
        CHECK(pixels[((H-1)*W+16)*4+2]==0.f);
        CHECK(pixels[16*4+2]==0.f);
        CHECK(std::abs(pixels[(10*W+32)*4]-32.5f/W)<1e-5f);CHECK(std::abs(pixels[(10*W+32)*4+1]-10.5f/H)<1e-5f);rb->Unmap(0,nullptr);
    }
    // A separate recording can inherit only the submitted destination, its exact sample
    // and a blended depth-tested draw. Read back the actual pixels on either side of submission.
    auto layer=test::CreateTexture(dev,W,H,DXGI_FORMAT_R32G32B32A32_FLOAT,RT,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    test::ComPtr<ID3D12DescriptorHeap> layer_heap;hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    REQUIRE(layer&&SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&layer_heap))));
    auto layer_rt=layer_heap->GetCPUDescriptorHandleForHeapStart();dev->CreateRenderTargetView(layer.Get(),nullptr,layer_rt);
    const auto layer_id=na::observe::IdOf(layer.Get()),color_id=na::observe::IdOf(color.Get()),depth_id=na::observe::IdOf(depth.Get());
    const D3D12_VIEWPORT vp{0,0,float(W),float(H),0,1};const D3D12_RECT whole{0,0,W,H};
    const auto bind=[&](ID3D12GraphicsCommandList* list,D3D12_CPU_DESCRIPTOR_HANDLE target,ID3D12PipelineState* pso){
        list->SetGraphicsRootSignature(root.Get());list->SetPipelineState(pso);list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->OMSetRenderTargets(1,&target,FALSE,&dsv);list->RSSetViewports(1,&vp);list->RSSetScissorRects(1,&whole);
    };
    na::observe::SetSceneViewportJitter(101,.375f,-.25f);
    l.Reset();bind(l.list.Get(),rtv,scene.Get());l.list->DrawInstanced(3,1,0,0);
    l.list->OMSetRenderTargets(1,&layer_rt,FALSE,nullptr);
    const auto handoff=na::observe::FinishSceneRaster(l.list.Get(),color_id,0,depth_id,0,layer_id);
    REQUIRE(handoff.destination==layer_id&&handoff.sample);
    REQUIRE(SUCCEEDED(l.list->Close()));ID3D12CommandList* producer[]={l.list.Get()};
    test::List reader;REQUIRE(reader.Create(dev));bind(reader.list.Get(),layer_rt,glass.Get());
    const auto stripe=[&](int y){D3D12_RECT r{0,y,W,y+1};reader.list->RSSetScissorRects(1,&r);reader.list->DrawInstanced(3,1,0,0);};
    stripe(5); // destination is known, but the producer is unsubmitted
    d.queue->ExecuteCommandLists(1,producer);na::observe::SubmitResolvedRaster(handoff);
    stripe(9);reader.list->SetPipelineState(scene.Get());stripe(13); // depth-writing geometry still samples the scene
    auto barrier=test::Transition(layer.Get(),RT,D3D12_RESOURCE_STATE_COPY_SOURCE);reader.list->ResourceBarrier(1,&barrier);
    D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=layer.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource=rb.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint.Footprint={DXGI_FORMAT_R32G32B32A32_FLOAT,W,H,1,W*16};
    reader.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    REQUIRE(SUCCEEDED(reader.list->Close()));ID3D12CommandList* consumer[]={reader.list.Get()};
    d.queue->ExecuteCommandLists(1,consumer);d.WaitIdle();
    float* pixels=nullptr;REQUIRE(SUCCEEDED(rb->Map(0,nullptr,reinterpret_cast<void**>(&pixels))));
    CHECK(std::abs(pixels[(5*W+24)*4]-(25.f-.375f)/(W+1))<1e-5f);
    CHECK(std::abs(pixels[(9*W+24)*4]-24.5f/W)<1e-5f);
    CHECK(std::abs(pixels[(13*W+24)*4]-(25.f-.375f)/(W+1))<1e-5f);
    rb->Unmap(0,nullptr);
    // Identical XY at a new frame ordinal is a different sample. The old handoff must expire.
    na::observe::SetSceneViewportJitter(102,.375f,-.25f);
    reader.Reset();
    std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);reader.list->ResourceBarrier(1,&barrier);
    bind(reader.list.Get(),layer_rt,glass.Get());stripe(9);
    std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);reader.list->ResourceBarrier(1,&barrier);
    reader.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    REQUIRE(SUCCEEDED(reader.list->Close()));d.queue->ExecuteCommandLists(1,consumer);d.WaitIdle();
    REQUIRE(SUCCEEDED(rb->Map(0,nullptr,reinterpret_cast<void**>(&pixels))));
    CHECK(std::abs(pixels[(9*W+24)*4]-(25.f-.375f)/(W+1))<1e-5f);
    rb->Unmap(0,nullptr);
    const auto stats=na::observe::GetViewportJitterStats();
    CHECK(stats.local_layers==2 && stats.submitted_layers==1);
    na::observe::SetSceneViewportJitter(0,0,0);CHECK(dev->GetDeviceRemovedReason()==S_OK);na_observe_stop();na::observe::SetProcessExiting();
    return test::finish("viewport_jitter_test");
}
