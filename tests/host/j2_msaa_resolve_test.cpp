// GPU-only live conversions, with CPU readback confined to this test.
#include "test_support.hpp"
#include "host_dx12/j2/msaa_resolve.hpp"
#include <d3d12sdklayers.h>
#include <vector>
#include <cmath>

int main(int argc, char**) {
    test::ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("j2_msaa_resolve_test");
    auto* dev = d.device.Get();
    test::ComPtr<ID3D12InfoQueue> info; dev->QueryInterface(IID_PPV_ARGS(&info));
    constexpr UINT W = 16, H = 16;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = W; desc.Height = H;
    desc.MipLevels = desc.DepthOrArraySize = 1; desc.SampleDesc.Count = 2;
    const bool stencil = argc == 1;
    desc.Format = stencil ? DXGI_FORMAT_R32G8X24_TYPELESS : DXGI_FORMAT_R32_TYPELESS;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    test::ComPtr<ID3D12Resource> depth, color;
    REQUIRE(SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&depth))));
    const auto depth_desc = desc;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    REQUIRE(SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&color))));
    auto plain = test::CreateTexture(dev,W,H,desc.Format,D3D12_RESOURCE_STATE_RESOLVE_DEST,D3D12_RESOURCE_FLAG_NONE);
    auto normal = test::CreateTexture(dev,W,H,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto reverse = test::CreateTexture(dev,W,H,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    REQUIRE(plain && normal && reverse);
    na::j2::MsaaResolve resolver;
    REQUIRE(resolver.Init(dev,W,H,desc.Format,depth_desc,{2,0},2));
    test::ComPtr<ID3D12DescriptorHeap> dh, rh;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_DSV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&dh))));
    hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rh))));
    auto dsv=dh->GetCPUDescriptorHandleForHeapStart(),rtv=rh->GetCPUDescriptorHandleForHeapStart();
    D3D12_DEPTH_STENCIL_VIEW_DESC dv{}; dv.Format=stencil ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT; dv.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2DMS;
    dev->CreateDepthStencilView(depth.Get(),&dv,dsv); dev->CreateRenderTargetView(color.Get(),nullptr,rtv);
    const char* shader=R"(
float4 vs(uint i:SV_VertexID):SV_Position { float2 uv=float2((i<<1)&2,i&2); return float4(uv*float2(2,-2)+float2(-1,1),0,1); }
struct Output { float4 color:SV_Target; float depth:SV_Depth; };
Output ps(uint sample:SV_SampleIndex) { Output o; o.color=sample?float4(0,0,1,1):float4(1,0,0,1); o.depth=sample?0.75:0.25; return o; }
)";
    test::ComPtr<ID3DBlob> vs,ps,blob,err;
    REQUIRE(SUCCEEDED(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"vs","vs_5_0",0,0,&vs,&err)));
    REQUIRE(SUCCEEDED(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"ps","ps_5_0",0,0,&ps,&err)));
    D3D12_ROOT_SIGNATURE_DESC rd{}; rd.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&err)));
    test::ComPtr<ID3D12RootSignature> root;
    REQUIRE(SUCCEEDED(dev->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root))));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC gp{};
    gp.pRootSignature=root.Get(); gp.VS={vs->GetBufferPointer(),vs->GetBufferSize()}; gp.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
    auto& blend=gp.BlendState.RenderTarget[0]; blend.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    blend.SrcBlend=blend.SrcBlendAlpha=D3D12_BLEND_ONE; blend.DestBlend=blend.DestBlendAlpha=D3D12_BLEND_ZERO;
    blend.BlendOp=blend.BlendOpAlpha=D3D12_BLEND_OP_ADD; blend.LogicOp=D3D12_LOGIC_OP_NOOP;
    gp.SampleMask=UINT_MAX; gp.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID; gp.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
    gp.RasterizerState.DepthClipEnable=TRUE; gp.RasterizerState.MultisampleEnable=TRUE;
    gp.DepthStencilState.DepthEnable=TRUE; gp.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;
    gp.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
    gp.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; gp.NumRenderTargets=1; gp.RTVFormats[0]=desc.Format;
    gp.DSVFormat=dv.Format; gp.SampleDesc={2,0};
    test::ComPtr<ID3D12PipelineState> pso;
    REQUIRE(SUCCEEDED(dev->CreateGraphicsPipelineState(&gp,IID_PPV_ARGS(&pso))));
    test::List l; REQUIRE(l.Create(dev));
    l.list->SetGraphicsRootSignature(root.Get()); l.list->SetPipelineState(pso.Get());
    l.list->OMSetRenderTargets(1,&rtv,FALSE,&dsv);
    D3D12_VIEWPORT vp{0,0,float(W),float(H),0,1}; D3D12_RECT rect{0,0,LONG(W),LONG(H)};
    l.list->RSSetViewports(1,&vp); l.list->RSSetScissorRects(1,&rect);
    l.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); l.list->DrawInstanced(3,1,0,0);
    auto barrier=[&](ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b,UINT sub=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
        auto t=test::Transition(r,a,b); t.Transition.Subresource=sub; l.list->ResourceBarrier(1,&t);
    };
    barrier(depth.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_COPY_SOURCE,0);
    D3D12_TEXTURE_COPY_LOCATION src{},dst{}; src.Type=dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource=depth.Get(); dst.pResource=resolver.DepthCopy();
    l.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    barrier(depth.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_DEPTH_WRITE,0);
    barrier(resolver.DepthCopy(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,0);
    resolver.ResolveDepth(l.list.Get(),normal.Get(),W,H,false,0);
    resolver.ResolveDepth(l.list.Get(),reverse.Get(),W,H,true,1);
    barrier(color.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    l.list->ResolveSubresource(plain.Get(),0,color.Get(),0,desc.Format);
    barrier(plain.Get(),D3D12_RESOURCE_STATE_RESOLVE_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    barrier(color.Get(),D3D12_RESOURCE_STATE_RESOLVE_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float green[4] = {0,1,0,1};
    l.list->ClearRenderTargetView(rtv,green,0,nullptr); // writeback must replace this, not merely retain the original purple average
    resolver.WriteColor(l.list.Get(),plain.Get(),color.Get(),0,W,H,0);
    // Resolve writeback again: every output sample must contain the processed purple pixel.
    auto back=test::CreateTexture(dev,W,H,desc.Format,D3D12_RESOURCE_STATE_RESOLVE_DEST,D3D12_RESOURCE_FLAG_NONE);
    REQUIRE(back);
    barrier(color.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    l.list->ResolveSubresource(back.Get(),0,color.Get(),0,desc.Format);
    ID3D12Resource* resources[]={normal.Get(),reverse.Get(),back.Get()};
    test::ComPtr<ID3D12Resource> readbacks[3]; D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[3]{};
    for (UINT i=0;i<3;++i) {
        auto rdesc=resources[i]->GetDesc(); UINT64 bytes=0;
        dev->GetCopyableFootprints(&rdesc,0,1,0,&fp[i],nullptr,nullptr,&bytes);
        readbacks[i]=test::CreateBuffer(dev,bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        REQUIRE(readbacks[i]);
        barrier(resources[i],i<2?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_RESOLVE_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
        src.pResource=resources[i]; dst.pResource=readbacks[i].Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint=fp[i];
        l.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    }
    REQUIRE(SUCCEEDED(l.list->Close())); ID3D12CommandList* lists[]={l.list.Get()}; d.queue->ExecuteCommandLists(1,lists); d.WaitIdle();
    for (UINT i=0;i<3;++i) {
        uint8_t* p=nullptr; REQUIRE(SUCCEEDED(readbacks[i]->Map(0,nullptr,reinterpret_cast<void**>(&p))));
        for(UINT y=0;y<H;++y) for(UINT x=0;x<W;++x) {
            if(i<2) { float value; memcpy(&value,p+fp[i].Footprint.RowPitch*y+x*4,4); CHECK(std::abs(value-(i?0.75f:0.25f))<1e-6f); }
            else { uint16_t c[4]; memcpy(c,p+fp[i].Footprint.RowPitch*y+x*8,8); CHECK(c[0]==0x3800 && c[1]==0 && c[2]==0x3800 && c[3]==0x3c00); }
        }
        readbacks[i]->Unmap(0,nullptr);
    }
    if(info) for(UINT64 i=0;i<info->GetNumStoredMessages();++i) {
        SIZE_T size=0; info->GetMessage(i,nullptr,&size); std::vector<uint8_t> data(size);
        auto* m=reinterpret_cast<D3D12_MESSAGE*>(data.data()); info->GetMessage(i,m,&size);
        if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) { printf("D3D12: %s\n",m->pDescription); CHECK(false); }
    }
    CHECK(dev->GetDeviceRemovedReason()==S_OK);
    return test::finish("j2_msaa_resolve_test");
}
