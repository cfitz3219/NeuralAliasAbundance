// Reproduce the actual state touched by Layer2 depth reconstruction: target,
// viewport/scissor, topology, PSO and root. Also reuse the game's CPU RTV slot:
// saving only its numeric handle would restore a different texture.
#include "test_support.hpp"
#include "host_dx12/scale/depth_restore.hpp"
#include "host_dx12/observe/producers.hpp"
#include <d3dcompiler.h>
#include <cmath>
namespace {
void __cdecl sink(const na_event_batch*, void*) {}
test::ComPtr<ID3DBlob> shader(const char* entry, const char* target) {
    constexpr char text[] = R"(
float4 vs(uint i:SV_VertexID):SV_Position { return float4(float2(float(i&1),float(i>>1))*2-1,0,1); }
float4 ps():SV_Target { return float4(0,1,0,1); }
)";
    test::ComPtr<ID3DBlob> code, errors;
    D3DCompile(text, std::strlen(text), nullptr, nullptr, nullptr, entry, target, 0, 0, &code, &errors);
    return code;
}
}
int main()
{
    test::Device d; if (!test::CreateDevice(d)) return test::SkipNoDevice("raster_restore_test");
    auto* dev = d.device.Get();
    REQUIRE(na::observe::InstallProducers(dev,d.queue.Get(),true));
    na::observe::SetRasterStateTracking(true);
    na_observe_config cfg{}; cfg.size_bytes=sizeof cfg;
    REQUIRE(na_observe_start(&cfg,sink,nullptr)==NA_OK);
    constexpr UINT w=32,h=16;
    auto a=test::CreateTexture(dev,w,h,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto b=test::CreateTexture(dev,w,h,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto plane=test::CreateTexture(dev,8,4,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto depth=test::CreateTexture(dev,w,h,DXGI_FORMAT_R32_TYPELESS,D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    REQUIRE(a&&b&&plane&&depth);
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,2,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
    test::ComPtr<ID3D12DescriptorHeap> heap; REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap))));
    auto handle=heap->GetCPUDescriptorHandleForHeapStart();
    auto plane_handle=handle;plane_handle.ptr+=dev->GetDescriptorHandleIncrementSize(hd.Type);
    dev->CreateRenderTargetView(plane.Get(),nullptr,plane_handle);
    dev->CreateRenderTargetView(a.Get(),nullptr,handle);
    D3D12_ROOT_SIGNATURE_DESC rd{}; rd.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    test::ComPtr<ID3DBlob> blob,err; REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&err)));
    test::ComPtr<ID3D12RootSignature> root; REQUIRE(SUCCEEDED(dev->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root))));
    auto vs=shader("vs","vs_5_0"),ps=shader("ps","ps_5_0"); REQUIRE(vs&&ps);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{}; pd.pRootSignature=root.Get();
    pd.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pd.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
    pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pd.RasterizerState.DepthClipEnable=TRUE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask=UINT_MAX;pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets=1;pd.RTVFormats[0]=DXGI_FORMAT_R32G32B32A32_FLOAT;pd.SampleDesc={1,0};
    test::ComPtr<ID3D12PipelineState> pso; REQUIRE(SUCCEEDED(dev->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&pso))));
    std::string error;auto pass=na::scale::DepthRestore::Create(dev,plane.Get(),depth.Get(),0,error);REQUIRE(pass);
    test::List recording;REQUIRE(recording.Create(dev));auto* list=recording.list.Get();
    REQUIRE(SUCCEEDED(list->Close()));REQUIRE(SUCCEEDED(list->Reset(recording.alloc.Get(),nullptr)));
    const float clear[4]={0,0,0,1},blue[4]={0,0,1,1};
    const float plane_clear[4]={.5f,.5f,.5f,.5f};list->ClearRenderTargetView(plane_handle,plane_clear,0,nullptr);
    auto input_ready=test::Transition(plane.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);list->ResourceBarrier(1,&input_ready);
    list->ClearRenderTargetView(handle,clear,0,nullptr);list->OMSetRenderTargets(1,&handle,FALSE,nullptr);
    // Legal reuse: OMSetRenderTargets consumed this CPU descriptor immediately.
    dev->CreateRenderTargetView(b.Get(),nullptr,handle);list->ClearRenderTargetView(handle,blue,0,nullptr);
    list->SetGraphicsRootSignature(root.Get());list->SetPipelineState(pso.Get());
    D3D12_VIEWPORT viewport{8,4,16,8,0,1};D3D12_RECT scissor{12,4,24,12};
    list->RSSetViewports(1,&viewport);list->RSSetScissorRects(1,&scissor);list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    REQUIRE(na::observe::ListRasterStateRestorable(list));
    {
        na::observe::HostWorkScope own;
        REQUIRE(pass->Record(list));
        na::observe::SetRasterStateTracking(false); // new request must not disrupt this recording's restore
        REQUIRE(na::observe::RestoreListState(list));
    }
    list->DrawInstanced(4,1,0,0);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes=0;const auto desc=a->GetDesc();
    dev->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&bytes);
    auto rb=test::CreateBuffer(dev,2*bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);REQUIRE(rb);
    for(UINT i=0;i<2;++i){auto* image=i?b.Get():a.Get();auto barrier=test::Transition(image,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);list->ResourceBarrier(1,&barrier);
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=image;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource=rb.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;dst.PlacedFootprint.Offset=i*bytes;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);}
    REQUIRE(SUCCEEDED(list->Close()));ID3D12CommandList* lists[]{list};d.queue->ExecuteCommandLists(1,lists);d.WaitIdle();
    const void* data=nullptr;REQUIRE(SUCCEEDED(rb->Map(0,nullptr,const_cast<void**>(&data))));
    size_t wrong=0;
    for(UINT i=0;i<2;++i)for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x){float px[4];std::memcpy(px,static_cast<const uint8_t*>(data)+i*bytes+y*fp.Footprint.RowPitch+x*16,16);
        const bool green=i==0&&x>=12&&x<24&&y>=4&&y<12;
        if(std::fabs(px[1]-(green?1.f:0.f))>.00001f||std::fabs(px[2]-(i?1.f:0.f))>.00001f)++wrong;}
    rb->Unmap(0,nullptr);CHECK(wrong==0);std::printf("wrong raster pixels=%zu\n",wrong);
    REQUIRE(SUCCEEDED(list->Reset(recording.alloc.Get(),nullptr)));
    CHECK(!na::observe::ListRasterStateRestorable(list)); // new recording latches Off
    REQUIRE(SUCCEEDED(list->Close()));
    na_observe_stop();return test::finish("raster_restore_test");
}
