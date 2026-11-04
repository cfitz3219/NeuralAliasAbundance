// Actual raster samples, immutable command-list ownership, and untouched post/HUD
// viewports. No game or NGX model: read back an interpolated coordinate witness.
#include "test_support.hpp"
#include "host_dx12/observe/producers.hpp"
#include <d3dcompiler.h>
#include <cmath>
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
float4 PS(V v):SV_Target{return float4(v.uv,0,1);})";
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
    test::List l;REQUIRE(l.Create(dev));auto rb=test::CreateBuffer(dev,W*H*16,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);REQUIRE(rb);
    for(UINT round=0;round<2;++round){
        if(round)l.Reset();na::observe::SetSceneViewportJitter(round+1,round?-.375f:.25f,round?.125f:-.375f);
        const float jx=round?-.375f:.25f,jy=round?.125f:-.375f;
        l.list->SetGraphicsRootSignature(root.Get());l.list->SetPipelineState(scene.Get());l.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        l.list->OMSetRenderTargets(1,&rtv,FALSE,&dsv);D3D12_VIEWPORT vp{0,0,float(W),float(H),0,1};l.list->RSSetViewports(1,&vp);
        D3D12_RECT sc{0,0,32,32};l.list->RSSetScissorRects(1,&sc);l.list->DrawInstanced(3,1,0,0);
        na::observe::SetSceneViewportJitter(99,-.125f,.4375f); // another thread's later frame
        sc={32,0,64,32};l.list->RSSetScissorRects(1,&sc);l.list->DrawInstanced(3,1,0,0);
        sc={0,8,64,16};l.list->RSSetScissorRects(1,&sc);l.list->SetPipelineState(post.Get());l.list->DrawInstanced(3,1,0,0);
        uint32_t frame=0;float x=0,y=0;REQUIRE(na::observe::DecodeSceneViewportJitter(na::observe::ListSceneViewportJitter(l.list.Get()),&frame,&x,&y));
        CHECK(frame==round+1&&x==jx&&y==jy);
        CHECK(!na::observe::ListSceneViewportJitter(l.list.Get(),na::observe::IdOf(color.Get())));
        auto barrier=test::Transition(color.Get(),RT,D3D12_RESOURCE_STATE_COPY_SOURCE);l.list->ResourceBarrier(1,&barrier);
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=color.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource=rb.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint.Footprint={DXGI_FORMAT_R32G32B32A32_FLOAT,W,H,1,W*16};
        l.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);l.list->ResourceBarrier(1,&barrier);
        REQUIRE(SUCCEEDED(l.list->Close()));ID3D12CommandList* lists[]={l.list.Get()};d.queue->ExecuteCommandLists(1,lists);d.WaitIdle();
        float* pixels=nullptr;REQUIRE(SUCCEEDED(rb->Map(0,nullptr,reinterpret_cast<void**>(&pixels))));
        for(UINT X:{16u,48u}){const auto k=(20*W+X)*4;CHECK(std::abs(pixels[k]-(float(X)+.5f-jx)/W)<1e-5f);CHECK(std::abs(pixels[k+1]-(20.5f-jy)/H)<1e-5f);}
        CHECK(std::abs(pixels[(10*W+32)*4]-32.5f/W)<1e-5f);CHECK(std::abs(pixels[(10*W+32)*4+1]-10.5f/H)<1e-5f);rb->Unmap(0,nullptr);
    }
    na::observe::SetSceneViewportJitter(0,0,0);CHECK(dev->GetDeviceRemovedReason()==S_OK);na_observe_stop();na::observe::SetProcessExiting();
    return test::finish("viewport_jitter_test");
}
