// Real raster pixels from reduced scene bindings, including cached repeated
// draws, an intervening native pass, recording-pinned samples and live Off.
// No game or neural model. Wait/readback belong only to this test.
#include "test_support.hpp"
#include "host_dx12/scale/scoped_bindings.hpp"
#include "host_dx12/observe/producers.hpp"
#include "host_dx12/observe/operation_snapshot.hpp"
#include "host_dx12/nr/native_fg.hpp"
#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#include <cmath>
namespace {
void __cdecl sink(const na_event_batch*,void*){}
class Provider final:public na::scale::BindingProvider {
public:
    std::shared_ptr<const na::scale::PreparedBindings> plan;
    bool active=true;
    ID3D12GraphicsCommandList* recording=nullptr;
    unsigned failures=0,cached_viewports=0;
    uint64_t viewport=~uint64_t(0),restore=~uint64_t(0);
    bool KeepNativeBindings()const noexcept override{return true;}
    std::shared_ptr<const na::scale::PreparedBindings> Acquire(ID3D12GraphicsCommandList* l,const na::observe::ShaderCall&)noexcept override{
        if(l!=recording)return {};
        if(active)return plan;
        na::observe::RestoreBindingState(l,false,false,0,true,true,true);
        viewport=restore=~uint64_t(0); // native boundary invalidates the retained private plan
        return {};
    }
    Changes BindingChanges(const std::shared_ptr<const na::scale::PreparedBindings>&,
        const na::observe::ShaderState& native,const na::observe::ShaderScope&)noexcept override{
        Changes changes;
        changes.viewports=viewport!=native.viewport_serial||restore!=native.restore_serial;
        if(!changes.viewports)++cached_viewports;
        viewport=native.viewport_serial;restore=native.restore_serial;
        return changes;
    }
    void Refused(ID3D12GraphicsCommandList* list,const na::observe::ShaderCall&,const char* why)noexcept override{
        na::observe::ShaderState state;const bool known=na::observe::ListShaderState(list,false,state);
        ++failures;std::printf("scene viewport binding refused: %s known=%u raster=%u root=%p expected=%p cbvs=%llu\n",why,known?1u:0u,state.raster_restorable?1u:0u,
            static_cast<void*>(state.root),static_cast<void*>(plan->Spec().root.Get()),static_cast<unsigned long long>(state.cbv_params));
        std::printf("root serial=%llu restore=%llu\n",static_cast<unsigned long long>(state.root_serial),static_cast<unsigned long long>(state.restore_serial));
    }
};
struct Readback {
    test::ComPtr<ID3D12Resource> resource;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    bool Prepare(ID3D12Device* dev,ID3D12Resource* texture){
        const auto desc=texture->GetDesc();UINT64 bytes=0;
        dev->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&bytes);
        resource=test::CreateBuffer(dev,bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);return !!resource;
    }
    void Record(ID3D12GraphicsCommandList* l,ID3D12Resource* texture,D3D12_RESOURCE_STATES state){
        auto b=test::Transition(texture,state,D3D12_RESOURCE_STATE_COPY_SOURCE);l->ResourceBarrier(1,&b);
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=texture;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource=resource.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=fp;
        l->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        std::swap(b.Transition.StateBefore,b.Transition.StateAfter);l->ResourceBarrier(1,&b);
    }
    int Check(UINT x,UINT y,float expected_x,float expected_y){
        const void* data=nullptr;REQUIRE(SUCCEEDED(resource->Map(0,nullptr,const_cast<void**>(&data))));
        const auto* p=reinterpret_cast<const float*>(static_cast<const char*>(data)+fp.Offset+y*fp.Footprint.RowPitch+x*16);
        std::printf("raster %u,%u = %.7f/%.7f expected %.7f/%.7f\n",x,y,p[0],p[1],expected_x,expected_y);
        CHECK(std::abs(p[0]-expected_x)<1e-5f&&std::abs(p[1]-expected_y)<1e-5f);
        resource->Unmap(0,nullptr);return 0;
    }
};
}
int main(){
    test::ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    test::Device d;if(!test::CreateDevice(d))return test::SkipNoDevice("scene_viewport_jitter_test");auto* dev=d.device.Get();
    test::ComPtr<ID3D12InfoQueue> info;dev->QueryInterface(IID_PPV_ARGS(&info));
    REQUIRE(na::observe::InstallProducers(dev,d.queue.Get(),true));
    REQUIRE(na::observe::InstallListResetTracking(dev,&na::nr::native::Reset));
    na::observe::SetOperationSnapshots(true);
    na::observe::SetRasterStateTracking(true);
    na_observe_config config{};config.size_bytes=sizeof config;REQUIRE(na_observe_start(&config,&sink,nullptr)==NA_OK);
    constexpr UINT W=64,H=32,RW=32,RH=16;constexpr auto format=DXGI_FORMAT_R32G32B32A32_FLOAT;
    auto color=test::CreateTexture(dev,W,H,format,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto depth=test::CreateTexture(dev,W,H,DXGI_FORMAT_D32_FLOAT,D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    REQUIRE(color&&depth);na::observe::SetLiveSelection(na::observe::IdOf(depth.Get()),na::observe::IdOf(color.Get()));
    test::ComPtr<ID3D12DescriptorHeap> rt,ds;D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rt))));hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&ds))));
    const auto rtv=rt->GetCPUDescriptorHandleForHeapStart(),dsv=ds->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(color.Get(),nullptr,rtv);dev->CreateDepthStencilView(depth.Get(),nullptr,dsv);
    test::ComPtr<ID3DBlob> vs,ps,blob,error;test::ComPtr<ID3D12RootSignature> root;
    D3D12_ROOT_PARAMETER parameter{};parameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameter.Constants={0,0,1};
    D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=1;rd.pParameters=&parameter;REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error)));
    REQUIRE(SUCCEEDED(dev->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root))));
    constexpr char code[]=R"(cbuffer C:register(b0){float marker;};struct V{float4 p:SV_Position;float2 uv:TEXCOORD;};
V VS(uint i:SV_VertexID){V v;v.uv=float2((i<<1)&2,i&2);v.p=float4(v.uv.x*2-1,1-v.uv.y*2,.5,1);return v;}
float4 PS(V v):SV_Target{return float4(v.uv,marker,1);})";
    REQUIRE(SUCCEEDED(D3DCompile(code,sizeof code-1,nullptr,nullptr,nullptr,"VS","vs_5_0",0,0,&vs,&error)));
    REQUIRE(SUCCEEDED(D3DCompile(code,sizeof code-1,nullptr,nullptr,nullptr,"PS","ps_5_0",0,0,&ps,&error)));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pd.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
    pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pd.RasterizerState.DepthClipEnable=TRUE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask=15;pd.SampleMask=UINT_MAX;pd.SampleDesc.Count=1;pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets=1;pd.RTVFormats[0]=format;pd.DSVFormat=DXGI_FORMAT_D32_FLOAT;
    pd.DepthStencilState.DepthEnable=TRUE;pd.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
    test::ComPtr<ID3D12PipelineState> scene,post;REQUIRE(SUCCEEDED(dev->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&scene))));
    pd.DepthStencilState.DepthEnable=FALSE;REQUIRE(SUCCEEDED(dev->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&post))));
    std::string why;std::shared_ptr<const na::scale::SceneGroup> group;auto provider=std::make_shared<Provider>();
    {const na::observe::HostWorkScope own;
        using namespace na::scale;
        group=SceneGroup::Create(dev,1,{W,H},{RW,RH},{{1,color.Get(),Role::Color,{RW,RH}},{2,depth.Get(),Role::Depth,{RW,RH}}},why);REQUIRE(group);
        BindingSpec s;s.group=group;s.root=root;s.native_extent={W,H};s.render_extent={RW,RH};s.viewports={{0,0,float(W),float(H),0,1}};s.scissors={{0,0,W,H}};
        ColorTarget target;target.member=1;target.view.Format=format;target.view.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;s.colors={target};
        s.depth.member=2;s.depth.view.Format=DXGI_FORMAT_D32_FLOAT;s.depth.view.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
        s.uses={{1,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_RENDER_TARGET},{2,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_DEPTH_WRITE}};
        provider->plan=PreparedBindings::Create(dev,std::move(s),why);REQUIRE(provider->plan);
    }
    na::observe::SetShaderOperationHook(na::observe::ShaderLane::Bindings,std::make_shared<na::scale::BindingHook>(provider));
    Readback reduced_pixels,native_pixels;REQUIRE(reduced_pixels.Prepare(dev,group->Find(1)->reduced.Get())&&native_pixels.Prepare(dev,color.Get()));
    test::List list;REQUIRE(list.Create(dev));REQUIRE(SUCCEEDED(list.list->Close()));
    const float samples[3][2]={{.25f,-.375f},{-.375f,.125f},{0,0}};
    for(UINT round=0;round<3;++round){
        list.Reset();auto* l=list.list.Get();provider->recording=l;provider->viewport=provider->restore=~uint64_t(0);const auto jx=samples[round][0],jy=samples[round][1];
        na::observe::SetSceneViewportJitter(round<2?round+1:0,jx,jy);provider->active=true;
        l->SetGraphicsRootSignature(root.Get());l->SetGraphicsRoot32BitConstant(0,0,0);l->SetPipelineState(scene.Get());l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        l->OMSetRenderTargets(1,&rtv,FALSE,&dsv);const D3D12_VIEWPORT vp{0,0,float(W),float(H),0,1};const D3D12_RECT sc{0,0,W,H};
        l->RSSetViewports(1,&vp);l->RSSetScissorRects(1,&sc);
        const auto before=na::observe::GetViewportJitterStats().sets;l->DrawInstanced(3,1,0,0);
        if(round<2)na::observe::SetSceneViewportJitter(99,-.125f,.4375f); // cannot change this recording's sample
        l->DrawInstanced(3,1,0,0);
        CHECK(na::observe::GetViewportJitterStats().sets-before==(round<2?1u:0u));
        provider->active=false;l->SetPipelineState(post.Get());l->DrawInstanced(3,1,0,0);
        provider->active=true;l->SetPipelineState(scene.Get());l->DrawInstanced(3,1,0,0);
        uint32_t frame=0;float x=0,y=0;const auto packed=na::observe::ListSceneViewportJitter(l,na::observe::IdOf(depth.Get()));
        if(round<2){CHECK(na::observe::DecodeSceneViewportJitter(packed,&frame,&x,&y));CHECK(frame==round+1&&x==jx&&y==jy);}
        else CHECK(!packed);
        {const na::observe::HostWorkScope own;reduced_pixels.Record(l,group->Find(1)->reduced.Get(),D3D12_RESOURCE_STATE_COMMON);native_pixels.Record(l,color.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET);}
        REQUIRE(SUCCEEDED(l->Close()));ID3D12CommandList* lists[]={l};
        {na::nr::native::Submission keep(d.queue.Get(),1,lists);d.queue->ExecuteCommandLists(1,lists);}d.WaitIdle();
        REQUIRE(reduced_pixels.Check(8,10,(8.5f-jx)/RW,(10.5f-jy)/RH)==0);REQUIRE(reduced_pixels.Check(24,10,(24.5f-jx)/RW,(10.5f-jy)/RH)==0);
        REQUIRE(native_pixels.Check(32,10,32.5f/W,10.5f/H)==0);
    }
    CHECK(provider->failures==0&&provider->cached_viewports>=3);
    na::observe::SetSceneViewportJitter(0,0,0);na::observe::SetShaderOperationHook(na::observe::ShaderLane::Bindings,{});
    if(info)for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T bytes=0;info->GetMessage(i,nullptr,&bytes);std::vector<uint8_t> data(bytes);auto* message=reinterpret_cast<D3D12_MESSAGE*>(data.data());
        if(SUCCEEDED(info->GetMessage(i,message,&bytes))&&message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::printf("D3D12 error: %s\n",message->pDescription);CHECK(false);}}
    CHECK(dev->GetDeviceRemovedReason()==S_OK);na_observe_stop();na::observe::SetProcessExiting();return test::finish("scene_viewport_jitter_test");
}
