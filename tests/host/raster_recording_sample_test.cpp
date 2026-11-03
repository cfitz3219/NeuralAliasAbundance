// A render thread may still be recording when Present advances the suggested
// sample. Every main-view draw in ONE recording must retain its first sample.
#include "test_support.hpp"
#include "host_dx12/observe/producers.hpp"
#include "host_dx12/host_observe.h"
#include <d3dcompiler.h>
#include <vector>
#include <cmath>
namespace {
double sample_x=.25,sample_y=-.375;
bool reject_sample=false;
bool sample(uint32_t,uint32_t,double* x,double* y) { *x=sample_x;*y=sample_y;return !reject_sample; }
void __cdecl sink(const na_event_batch*,void*) {}
std::vector<unsigned char> read(test::Device& d,ID3D12Resource* t) {
    const auto desc=t->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes=0;
    d.device->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&bytes);
    auto rb=test::CreateBuffer(d.device.Get(),bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    test::List l;if(!rb || !l.Create(d.device.Get()))return {};
    auto b=test::Transition(t,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);l.list->ResourceBarrier(1,&b);
    D3D12_TEXTURE_COPY_LOCATION a{},z{};a.pResource=rb.Get();a.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;a.PlacedFootprint=fp;
    z.pResource=t;z.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;l.list->CopyTextureRegion(&a,0,0,0,&z,nullptr);
    std::swap(b.Transition.StateBefore,b.Transition.StateAfter);l.list->ResourceBarrier(1,&b);l.list->Close();
    ID3D12CommandList* lists[]={l.list.Get()};d.queue->ExecuteCommandLists(1,lists);d.WaitIdle();
    void* p=nullptr;if(FAILED(rb->Map(0,nullptr,&p)))return {};
    std::vector<unsigned char> pixels(desc.Width*desc.Height*4);
    for(UINT y=0;y<desc.Height;++y)std::memcpy(pixels.data()+y*desc.Width*4,static_cast<unsigned char*>(p)+fp.Offset+y*fp.Footprint.RowPitch,desc.Width*4);
    D3D12_RANGE empty{0,0};rb->Unmap(0,&empty);return pixels;
}
}
int main() {
    test::Device d;if(!test::CreateDevice(d,true))return test::kSkip;
    REQUIRE(na::observe::InstallProducers(d.device.Get(),d.queue.Get(),false));na_observe_config cfg{};cfg.size_bytes=sizeof cfg;
    REQUIRE(na_observe_start(&cfg,sink,nullptr)==NA_OK);
    constexpr UINT W=64,H=32;
    auto depth=test::CreateTexture(d.device.Get(),W,H,DXGI_FORMAT_D32_FLOAT,D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    auto a=test::CreateTexture(d.device.Get(),W,H,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto b=test::CreateTexture(d.device.Get(),W,H,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto c=test::CreateTexture(d.device.Get(),W,H,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    REQUIRE(depth&&a&&b&&c);
    test::ComPtr<ID3D12DescriptorHeap> rh,dh;D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=3;
    REQUIRE(SUCCEEDED(d.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rh))));hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;hd.NumDescriptors=1;
    REQUIRE(SUCCEEDED(d.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&dh))));
    auto r0=rh->GetCPUDescriptorHandleForHeapStart(),r1=r0;r1.ptr+=d.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    auto r2=r1;r2.ptr+=d.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const auto ds=dh->GetCPUDescriptorHandleForHeapStart();d.device->CreateRenderTargetView(a.Get(),nullptr,r0);d.device->CreateRenderTargetView(b.Get(),nullptr,r1);d.device->CreateRenderTargetView(c.Get(),nullptr,r2);d.device->CreateDepthStencilView(depth.Get(),nullptr,ds);
    D3D12_ROOT_PARAMETER rp{};rp.ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;rp.Constants={0,0,1};rp.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rd{1,&rp,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    test::ComPtr<ID3DBlob> sig,err,vs,ps;REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&sig,&err)));
    test::ComPtr<ID3D12RootSignature> root;REQUIRE(SUCCEEDED(d.device->CreateRootSignature(0,sig->GetBufferPointer(),sig->GetBufferSize(),IID_PPV_ARGS(&root))));
    const char* sh=R"(
cbuffer C:register(b0){uint side;}
struct V{float4 p:SV_Position;};
V VS(uint id:SV_VertexID){float2 p[6]={float2(0,0),float2(0,1),float2(1,0),float2(1,0),float2(0,1),float2(1,1)};
 float2 q=p[id];float cut=32.3/64.0;q.x=side==0?q.x*cut:cut+q.x*(1-cut);V o;o.p=float4(q*float2(2,-2)+float2(-1,1),.5,1);return o;}
float4 PS(V v):SV_Target{return side==0?float4(1,0,0,1):float4(0,1,0,1);})";
    REQUIRE(SUCCEEDED(D3DCompile(sh,std::strlen(sh),nullptr,nullptr,nullptr,"VS","vs_5_0",0,0,&vs,&err)));
    REQUIRE(SUCCEEDED(D3DCompile(sh,std::strlen(sh),nullptr,nullptr,nullptr,"PS","ps_5_0",0,0,&ps,&err)));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pd.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
    pd.DepthStencilState.DepthEnable=TRUE;pd.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
    pd.DSVFormat=DXGI_FORMAT_D32_FLOAT;pd.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;pd.NumRenderTargets=1;pd.SampleDesc.Count=1;pd.SampleMask=UINT_MAX;pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    test::ComPtr<ID3D12PipelineState> pso;REQUIRE(SUCCEEDED(d.device->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&pso))));
    na::observe::SetRasterJitterCallback(sample);
    auto render=[&](ID3D12Resource* color,D3D12_CPU_DESCRIPTOR_HANDLE rt,int advance)->int {
        test::List l;REQUIRE(l.Create(d.device.Get()));REQUIRE(SUCCEEDED(l.list->Close()));l.Reset();
        na::observe::SetLiveSelection(na::observe::IdOf(depth.Get()),na::observe::IdOf(color));
        l.list->SetPipelineState(pso.Get());l.list->SetGraphicsRootSignature(root.Get());l.list->OMSetRenderTargets(1,&rt,FALSE,&ds);
        D3D12_VIEWPORT vp{0,0,float(W),float(H),0,1};D3D12_RECT rc{0,0,LONG(W),LONG(H)};l.list->RSSetViewports(1,&vp);l.list->RSSetScissorRects(1,&rc);l.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const float black[4]{};l.list->ClearRenderTargetView(rt,black,0,nullptr);l.list->ClearDepthStencilView(ds,D3D12_CLEAR_FLAG_DEPTH,1,0,0,nullptr);
        sample_x=.25;sample_y=-.375;reject_sample=false;l.list->SetGraphicsRoot32BitConstant(0,0,0);l.list->DrawInstanced(6,1,0,0);
        if(advance){sample_x=-.375;sample_y=1./6;} // Present/another recording advances global suggestion while THIS list remains open.
        reject_sample=advance==2; // A later suggestion can also be temporarily unavailable.
        l.list->SetGraphicsRoot32BitConstant(0,1,0);l.list->DrawInstanced(6,1,0,0);
        double x=0,y=0;CHECK(na::observe::ListRasterJitter(l.list.Get(),&x,&y));CHECK(std::abs(x-.25)<1e-9&&std::abs(y+.375)<1e-9);
        std::vector<na::j2::ColorEvent> events;
        REQUIRE(na::observe::ListColorEvents(l.list.Get(),na::observe::IdOf(color),events));
        unsigned draws=0;for (const auto& e:events) if(e.op==na::j2::Op::DrawWrite) {
            ++draws;CHECK(e.producer==na::observe::IdOf(pso.Get()));
        }
        CHECK(draws==2);
        REQUIRE(SUCCEEDED(l.list->Close()));ID3D12CommandList* lists[]={l.list.Get()};d.queue->ExecuteCommandLists(1,lists);d.WaitIdle();return 0;
    };
    REQUIRE(render(a.Get(),r0,0)==0);REQUIRE(render(b.Get(),r1,1)==0);REQUIRE(render(c.Get(),r2,2)==0);
    const auto expected=read(d,a.Get()),actual=read(d,b.Get());REQUIRE(!expected.empty()&&expected.size()==actual.size());
    size_t changed=0;for(size_t i=0;i<expected.size();i+=4)changed+=std::memcmp(expected.data()+i,actual.data()+i,4)!=0;
    std::printf("mixed-recording sample: %zu GPU pixels differ from the coherent baseline\n",changed);CHECK(changed==0);
    CHECK(read(d,c.Get())==expected);
    na::observe::SetRasterJitterCallback(nullptr);na_observe_stop();
    return test::finish("raster_recording_sample_test");
}
