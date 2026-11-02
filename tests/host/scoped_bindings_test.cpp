// Actual private descriptor-table/root CBVs, SRV/UAV rebasing, native sampler
// preservation, dispatch extents, late camera data and executable GPU lifetime.
#include "test_support.hpp"
#include "host_dx12/scale/scoped_bindings.hpp"
#include "host_dx12/scale/depth_restore.hpp"
#include "host_dx12/scale/scene_frame.hpp"
#include "host_dx12/observe/producers.hpp"
#include "host_dx12/nr/native_fg.hpp"
#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#include <cmath>
namespace {
using namespace na::scale;
void __cdecl sink(const na_event_batch*, void*) {}
class Provider final : public BindingProvider {
public:
    std::shared_ptr<const PreparedBindings> plan;
    ID3D12GraphicsCommandList* recording = nullptr;
    unsigned refused = 0;
    std::shared_ptr<const PreparedBindings> Acquire(ID3D12GraphicsCommandList* list, const na::observe::ShaderCall&) noexcept override {
        return list == recording ? plan : nullptr;
    }
    void Refused(ID3D12GraphicsCommandList*, const na::observe::ShaderCall&, const char* reason) noexcept override {
        ++refused; std::printf("binding refusal %u: %s\n", refused, reason);
    }
};
struct Readback {
    test::ComPtr<ID3D12Resource> buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    void Prepare(ID3D12Device* device, ID3D12Resource* texture) {
        auto desc=texture->GetDesc();UINT64 bytes=0;device->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&bytes);
        buffer=test::CreateBuffer(device,bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    }
    void Record(ID3D12GraphicsCommandList* l, ID3D12Resource* texture, D3D12_RESOURCE_STATES state) {
        auto b=test::Transition(texture,state,D3D12_RESOURCE_STATE_COPY_SOURCE);l->ResourceBarrier(1,&b);
        D3D12_TEXTURE_COPY_LOCATION s{},d{};s.pResource=texture;s.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        d.pResource=buffer.Get();d.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;d.PlacedFootprint=fp;l->CopyTextureRegion(&d,0,0,0,&s,nullptr);
        std::swap(b.Transition.StateBefore,b.Transition.StateAfter);l->ResourceBarrier(1,&b);
    }
    void Check(UINT w,UINT h,float width,float height) {
        void* p=nullptr;CHECK(SUCCEEDED(buffer->Map(0,nullptr,&p)));if(!p)return;
        unsigned wrong=0;for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x){float v[4];
            std::memcpy(v,static_cast<char*>(p)+fp.Offset+y*fp.Footprint.RowPitch+x*16,16);
            wrong+=v[0]!=width||v[1]!=height||v[2]!=9||v[3]!=.75f;}
        std::printf("%ux%u output, wrong pixels %u\n",w,h,wrong);CHECK(wrong==0);buffer->Unmap(0,nullptr);
    }
};
int Raster(test::Device& d, int frame_mode=0)
{
    auto* dev=d.device.Get();constexpr UINT w=32,h=16,rw=21,rh=11;const auto fmt=DXGI_FORMAT_R32G32B32A32_FLOAT;
    na::observe::SetRasterStateTracking(true);
    auto color=test::CreateTexture(dev,w,h,fmt,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto depth=test::CreateTexture(dev,w,h,DXGI_FORMAT_D32_FLOAT,D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    auto history=test::CreateTexture(dev,w,h,fmt,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto cb=test::CreateBuffer(dev,256,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);REQUIRE(color&&depth&&cb);
    const float source[4]={float(w),float(h),3,0};void* mapped=nullptr;REQUIRE(SUCCEEDED(cb->Map(0,nullptr,&mapped)));std::memcpy(mapped,source,sizeof source);
    const char code[]=R"(
cbuffer C:register(b0){float width,height,marker,pad;};cbuffer R:register(b1){float rw,rh,rm,rp;};
float4 vs(uint i:SV_VertexID):SV_Position{return float4(float2((i<<1)&2,i&2)*2-1,.5,1);}
float4 ps():SV_Target{return float4(width,rh,marker,.75);})";
    test::ComPtr<ID3DBlob> vs,ps,blob,errors;REQUIRE(SUCCEEDED(D3DCompile(code,sizeof code-1,nullptr,nullptr,nullptr,"vs","vs_5_0",0,0,&vs,&errors)));
    REQUIRE(SUCCEEDED(D3DCompile(code,sizeof code-1,nullptr,nullptr,nullptr,"ps","ps_5_0",0,0,&ps,&errors)));
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_CBV,1,0,0,0};D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[0].DescriptorTable={1,&range};
    params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;params[1].Descriptor={1,0};
    D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=2;rd.pParameters=params;
    REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors)));
    test::ComPtr<ID3D12RootSignature> root;REQUIRE(SUCCEEDED(dev->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root))));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pd.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
    pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pd.RasterizerState.DepthClipEnable=TRUE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;pd.DepthStencilState.DepthEnable=TRUE;
    pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;pd.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.FrontFace=pd.DepthStencilState.BackFace={D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_COMPARISON_FUNC_ALWAYS};
    pd.SampleMask=UINT_MAX;pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;pd.NumRenderTargets=1;pd.RTVFormats[0]=fmt;pd.DSVFormat=DXGI_FORMAT_D32_FLOAT;pd.SampleDesc.Count=1;
    test::ComPtr<ID3D12PipelineState> pso;REQUIRE(SUCCEEDED(dev->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&pso))));
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};test::ComPtr<ID3D12DescriptorHeap> rt,ds,heap;
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rt))));hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&ds))));
    hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap))));
    const auto rtv=rt->GetCPUDescriptorHandleForHeapStart(),dsv=ds->GetCPUDescriptorHandleForHeapStart();dev->CreateRenderTargetView(color.Get(),nullptr,rtv);dev->CreateDepthStencilView(depth.Get(),nullptr,dsv);
    D3D12_CONSTANT_BUFFER_VIEW_DESC cv{cb->GetGPUVirtualAddress(),256};dev->CreateConstantBufferView(&cv,heap->GetCPUDescriptorHandleForHeapStart());
    std::string error;std::shared_ptr<const SceneGroup> group;std::shared_ptr<const PreparedBindings> plan;std::shared_ptr<const ColorRestore> promote;
    std::shared_ptr<SceneFrame> frame;PhysicalInputBridge bridge;
    {
        na::observe::HostWorkScope host;
        group=SceneGroup::Create(dev,13,{w,h},{rw,rh},{{1,color.Get(),Role::Color,{rw,rh}},{2,depth.Get(),Role::Depth,{rw,rh}},
            {3,history.Get(),Role::History,{rw,rh}}},error);REQUIRE(group);
        auto constants=GpuConstants::Create(dev,cb.Get(),0,source,sizeof source,{{0,Scalar::Float,Dimension::Width,{w,h},{rw,rh}},
            {4,Scalar::Float,Dimension::Height,{w,h},{rw,rh}}},error);REQUIRE(constants);
        BindingSpec s;s.group=group;s.root=root;s.native_extent={w,h};s.render_extent={rw,rh};s.viewports={{0,0,float(w),float(h),0,1}};s.scissors={{0,0,LONG(w),LONG(h)}};
        s.constants={{constants,D3D12_RESOURCE_STATE_GENERIC_READ}};s.root_cbvs={{1,0}};s.tables={{0,0}};ShaderView c;c.kind=ShaderView::Kind::PatchedCbv;s.views.push_back(c);
        ColorTarget target;target.member=1;target.view.Format=fmt;target.view.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;s.colors.push_back(target);
        s.depth.member=2;s.depth.view.Format=DXGI_FORMAT_D32_FLOAT;s.depth.view.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
        s.uses={{1,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_RENDER_TARGET},{2,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_DEPTH_WRITE}};
        plan=PreparedBindings::Create(dev,s,error);REQUIRE(plan);
        promote=ColorRestore::Create(dev,group->Find(1)->reduced.Get(),color.Get(),0,fmt,error);REQUIRE(promote);
        if(frame_mode){FrameSpec f;f.group=group;f.color=1;f.depth=2;f.members={{1,D3D12_RESOURCE_STATE_RENDER_TARGET},{2,D3D12_RESOURCE_STATE_DEPTH_WRITE},{3,D3D12_RESOURCE_STATE_RENDER_TARGET,true,D3D12_RESOURCE_STATE_RENDER_TARGET}};
            frame=SceneFrame::Create(dev,f,error);REQUIRE(frame);}
    }
    auto provider=std::make_shared<Provider>();provider->plan=plan;
    na::observe::SetShaderOperationHook(na::observe::ShaderLane::Bindings,std::make_shared<BindingHook>(provider));
    Readback cr,dr,nr,nd;cr.Prepare(dev,group->Find(1)->reduced.Get());dr.Prepare(dev,group->Find(2)->reduced.Get());nr.Prepare(dev,color.Get());nd.Prepare(dev,depth.Get());
    Readback history_seed,history_full;
    test::ComPtr<ID3D12DescriptorHeap> history_views;
    if(frame){na::observe::HostWorkScope host;
        D3D12_DESCRIPTOR_HEAP_DESC hh{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,2,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
        REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hh,IID_PPV_ARGS(&history_views))));
        auto hc=history_views->GetCPUDescriptorHandleForHeapStart();dev->CreateRenderTargetView(history.Get(),nullptr,hc);
        hc.ptr+=dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);dev->CreateRenderTargetView(group->Find(3)->reduced.Get(),nullptr,hc);
        history_seed.Prepare(dev,group->Find(3)->reduced.Get());history_full.Prepare(dev,history.Get());}
    test::List recording;REQUIRE(recording.Create(dev));auto* list=recording.list.Get();REQUIRE(SUCCEEDED(list->Close()));REQUIRE(SUCCEEDED(list->Reset(recording.alloc.Get(),nullptr)));
    provider->recording=list;
    if(frame){REQUIRE(bridge.Bind(list,frame));std::shared_ptr<const na::j2::PhysicalInput> input;const auto callbacks=bridge.Callbacks();
        CHECK(callbacks.lookup(callbacks.ctx,1,2,list,&input)==na::j2::PhysicalLookup::None);}
    const float clear[4]={0,0,0,0};list->ClearRenderTargetView(rtv,clear,0,nullptr);list->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,.2f,0,0,nullptr);
    list->SetGraphicsRootSignature(root.Get());list->SetPipelineState(pso.Get());ID3D12DescriptorHeap* heaps[]{heap.Get()};list->SetDescriptorHeaps(1,heaps);
    list->SetGraphicsRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());list->SetGraphicsRootConstantBufferView(1,cb->GetGPUVirtualAddress());
    list->OMSetRenderTargets(1,&rtv,FALSE,&dsv);const D3D12_VIEWPORT vp{0,0,float(w),float(h),0,1};const D3D12_RECT sc{0,0,w,h};
    list->RSSetViewports(1,&vp);list->RSSetScissorRects(1,&sc);list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    if(frame){
        {na::observe::HostWorkScope host;const float old_history[4]={99,77,4,.125f};
            list->ClearRenderTargetView(history_views->GetCPUDescriptorHandleForHeapStart(),old_history,0,nullptr);}
        REQUIRE(frame->SeedHistories(list)); // public frame operation owns its HostWorkScope
        {na::observe::HostWorkScope host;history_seed.Record(list,group->Find(3)->reduced.Get(),D3D12_RESOURCE_STATE_COMMON);}}
    list->DrawInstanced(3,1,0,0);provider->plan.reset(); // private complete21x11 colour/depth scene
    if(frame){na::observe::HostWorkScope host;auto b=test::Transition(group->Find(3)->reduced.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);list->ResourceBarrier(1,&b);
        auto hc=history_views->GetCPUDescriptorHandleForHeapStart();hc.ptr+=dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        const float new_history[4]={5,6,7,.5f};list->ClearRenderTargetView(hc,new_history,0,nullptr);
        std::swap(b.Transition.StateBefore,b.Transition.StateAfter);list->ResourceBarrier(1,&b);frame->Written(3);}
    if(frame){frame->Written(1);frame->Written(2);std::shared_ptr<const na::j2::PhysicalInput> input;const auto callbacks=bridge.Callbacks();
        CHECK(callbacks.lookup(callbacks.ctx,1,2,list,&input)==na::j2::PhysicalLookup::Unavailable);
        CHECK(input&&input->generation==13);callbacks.outcome(callbacks.ctx,13,na::j2::PhysicalOutcome::Unavailable,"not ready");
        if(frame_mode!=3){frame->Ready();CHECK(callbacks.lookup(callbacks.ctx,1,2,list,&input)==na::j2::PhysicalLookup::Provided);
            CHECK(input&&input->color==group->Find(1)->reduced.Get()&&input->depth==group->Find(2)->reduced.Get());
            CHECK(input&&input->output==color.Get()&&input->render_w==rw&&input->render_h==rh&&input->owner);
            callbacks.outcome(callbacks.ctx,13,na::j2::PhysicalOutcome::Failed,"test model failure");}
        {PhysicalInputBridge::SubmissionScope submission(bridge,list);
            CHECK(callbacks.lookup(callbacks.ctx,1,2,nullptr,&input)==(frame_mode==3?na::j2::PhysicalLookup::Unavailable:na::j2::PhysicalLookup::Provided));
            callbacks.outcome(callbacks.ctx,13,na::j2::PhysicalOutcome::Failed,"between-list failure");}
        CHECK(callbacks.lookup(callbacks.ctx,1,2,nullptr,&input)==na::j2::PhysicalLookup::None);
        CHECK(callbacks.lookup(callbacks.ctx,99,2,list,&input)==na::j2::PhysicalLookup::None&&!input);
    }
    if(frame){
            if(frame_mode==2){na::observe::HostWorkScope host;const float enhanced[4]={float(w+100),float(h+100),9,.75f};list->ClearRenderTargetView(rtv,enhanced,0,nullptr);
                frame->Outcome(na::j2::PhysicalOutcome::Evaluated);}
            REQUIRE(frame->Finish(list)); // caller need not create a host-work guard
    }else{na::observe::HostWorkScope host;auto b=test::Transition(group->Find(1)->reduced.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);list->ResourceBarrier(1,&b);
            REQUIRE(promote->Record(list));REQUIRE(na::observe::RestoreListState(list));std::swap(b.Transition.StateBefore,b.Transition.StateAfter);list->ResourceBarrier(1,&b);}
    const D3D12_RECT hud{29,13,32,16};list->RSSetScissorRects(1,&hud);list->DrawInstanced(3,1,0,0); // native dimensions, only9native HUD pixels
    {na::observe::HostWorkScope host;cr.Record(list,group->Find(1)->reduced.Get(),D3D12_RESOURCE_STATE_COMMON);dr.Record(list,group->Find(2)->reduced.Get(),D3D12_RESOURCE_STATE_COMMON);
        nr.Record(list,color.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET);nd.Record(list,depth.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE);}
    if(frame){na::observe::HostWorkScope host;history_full.Record(list,history.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET);}
    REQUIRE(SUCCEEDED(list->Close()));static_cast<float*>(mapped)[2]=9;cb->Unmap(0,nullptr);ID3D12CommandList* lists[]{list};
    {na::nr::native::Submission receipt(d.queue.Get(),1,lists);d.queue->ExecuteCommandLists(1,lists);}d.WaitIdle();cr.Check(rw,rh,rw,rh);
    void* p=nullptr;REQUIRE(SUCCEEDED(nr.buffer->Map(0,nullptr,&p)));unsigned wrong=0;
    for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x){float v[4];std::memcpy(v,static_cast<char*>(p)+nr.fp.Offset+y*nr.fp.Footprint.RowPitch+x*16,16);const bool in_hud=x>=29&&y>=13;
        wrong+=v[0]!=(in_hud?w:frame_mode==2?w+100:rw)||v[1]!=(in_hud?h:frame_mode==2?h+100:rh)||v[2]!=9||v[3]!=.75f;}nr.buffer->Unmap(0,nullptr);CHECK(wrong==0);
    for(int which=0;which<2;++which){auto& rb=which?nd:dr;REQUIRE(SUCCEEDED(rb.buffer->Map(0,nullptr,&p)));wrong=0;const UINT tw=which?w:rw,th=which?h:rh;
        for(UINT y=0;y<th;++y)for(UINT x=0;x<tw;++x){float value;std::memcpy(&value,static_cast<char*>(p)+rb.fp.Offset+y*rb.fp.Footprint.RowPitch+x*4,4);
            wrong+=value!=((frame_mode||!which||(x>=29&&y>=13))?.5f:.2f);}rb.buffer->Unmap(0,nullptr);CHECK(wrong==0);}
    if(frame)for(int which=0;which<2;++which){auto& rb=which?history_full:history_seed;REQUIRE(SUCCEEDED(rb.buffer->Map(0,nullptr,&p)));wrong=0;
        const UINT tw=which?w:rw,th=which?h:rh;const float old_history[4]={99,77,4,.125f},new_history[4]={5,6,7,.5f};
        for(UINT y=0;y<th;++y)for(UINT x=0;x<tw;++x){float pixel[4];std::memcpy(pixel,static_cast<char*>(p)+rb.fp.Offset+y*rb.fp.Footprint.RowPitch+x*16,16);
            for(int channel=0;channel<4;++channel)wrong+=std::fabs(pixel[channel]-(which?new_history[channel]:old_history[channel]))>.0001f;}
        rb.buffer->Unmap(0,nullptr);CHECK(wrong==0);}
    REQUIRE(SUCCEEDED(list->Reset(recording.alloc.Get(),nullptr)));REQUIRE(SUCCEEDED(list->Close()));na::nr::native::Maintain();
    if(frame){std::shared_ptr<const na::j2::PhysicalInput> input;const auto callbacks=bridge.Callbacks();
        CHECK(callbacks.lookup(callbacks.ctx,1,2,list,&input)==na::j2::PhysicalLookup::None);}
    na::observe::SetShaderOperationHook(na::observe::ShaderLane::Bindings,{});na::observe::SetRasterStateTracking(false);return 0;
}
}
int main()
{
    test::ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    test::Device d;if(!test::CreateDevice(d))return test::SkipNoDevice("scoped_bindings_test");auto* dev=d.device.Get();
    test::ComPtr<ID3D12InfoQueue> info;dev->QueryInterface(IID_PPV_ARGS(&info));
    REQUIRE(na::observe::InstallProducers(dev,d.queue.Get(),true));
    REQUIRE(na::observe::InstallListResetTracking(dev,&na::nr::native::Reset));
    na_observe_config cfg{};cfg.size_bytes=sizeof cfg;REQUIRE(na_observe_start(&cfg,sink,nullptr)==NA_OK);
    constexpr UINT w=32,h=16,rw=21,rh=11;const auto fmt=DXGI_FORMAT_R32G32B32A32_FLOAT;
    auto native=test::CreateTexture(dev,w,h,fmt,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto input=test::CreateTexture(dev,1,1,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto cb=test::CreateBuffer(dev,256,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);REQUIRE(native&&input&&cb);
    const float source[4]={float(w),float(h),3,0};void* mapped=nullptr;REQUIRE(SUCCEEDED(cb->Map(0,nullptr,&mapped)));std::memcpy(mapped,source,sizeof source);
    const char code[]=R"(
cbuffer TableSize:register(b0){float width,height,marker,pad;};
cbuffer RootSize:register(b1){float root_width,root_height,root_marker,root_pad;};
RWTexture2D<float4> target:register(u0);Texture2D<float> image:register(t0);SamplerState samp:register(s0);
[numthreads(8,4,1)]void main(uint3 p:SV_DispatchThreadID){
 if(p.x<uint(width)&&p.y<uint(height))target[p.xy]=float4(width,root_height,marker,image.SampleLevel(samp,float2(.5,.5),0));
})";
    test::ComPtr<ID3DBlob> cs,errors,blob;REQUIRE(SUCCEEDED(D3DCompile(code,sizeof code-1,nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&cs,&errors)));
    D3D12_DESCRIPTOR_RANGE ranges[4]={{D3D12_DESCRIPTOR_RANGE_TYPE_CBV,1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,1,0,0,0}};
    D3D12_ROOT_PARAMETER params[5]{};for(int i=0;i<4;++i){params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[i].DescriptorTable={1,&ranges[i]};}
    params[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;params[4].Descriptor={1,0};
    D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=5;rd.pParameters=params;
    REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors)));
    test::ComPtr<ID3D12RootSignature> root;REQUIRE(SUCCEEDED(dev->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root))));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={cs->GetBufferPointer(),cs->GetBufferSize()};
    test::ComPtr<ID3D12PipelineState> pso;REQUIRE(SUCCEEDED(dev->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pso))));
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,3,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
    test::ComPtr<ID3D12DescriptorHeap> heap,samplers,rtv;REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap))));
    hd={D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,1,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&samplers))));
    hd={D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtv))));
    dev->CreateRenderTargetView(input.Get(),nullptr,rtv->GetCPUDescriptorHandleForHeapStart());
    D3D12_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_POINT;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;sampler.MaxLOD=16;dev->CreateSampler(&sampler,samplers->GetCPUDescriptorHandleForHeapStart());
    const UINT inc=dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);auto cpu=heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CONSTANT_BUFFER_VIEW_DESC cv{cb->GetGPUVirtualAddress(),256};dev->CreateConstantBufferView(&cv,cpu);cpu.ptr+=inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};uv.Format=fmt;uv.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;dev->CreateUnorderedAccessView(native.Get(),nullptr,&uv,cpu);cpu.ptr+=inc;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};sv.Format=DXGI_FORMAT_R32_FLOAT;sv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;sv.Texture2D.MipLevels=1;sv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    dev->CreateShaderResourceView(input.Get(),&sv,cpu);
    std::string error;std::shared_ptr<const SceneGroup> group;std::shared_ptr<const GpuConstants> constants;std::shared_ptr<const PreparedBindings> plan,bad;
    {
        na::observe::HostWorkScope host;
        group=SceneGroup::Create(dev,12,{w,h},{rw,rh},{{17,native.Get(),Role::Color,{rw,rh}}},error);REQUIRE(group);
        constants=GpuConstants::Create(dev,cb.Get(),0,source,sizeof source,{{0,Scalar::Float,Dimension::Width,{w,h},{rw,rh}},
            {4,Scalar::Float,Dimension::Height,{w,h},{rw,rh}}},error);REQUIRE(constants);
        BindingSpec s;s.group=group;s.root=root;s.compute=true;s.native_extent={w,h};s.render_extent={rw,rh};s.threads_x=8;s.threads_y=4;
        s.constants.push_back({constants,D3D12_RESOURCE_STATE_GENERIC_READ});s.root_cbvs={{4,0}};s.tables={{0,0},{1,1},{2,2}};
        ShaderView c;c.kind=ShaderView::Kind::PatchedCbv;s.views.push_back(c);
        c={};c.kind=ShaderView::Kind::Uav;c.resource=group->Find(17)->reduced;c.uav=uv;s.views.push_back(c);
        c={};c.kind=ShaderView::Kind::Srv;c.resource=input;c.srv=sv;s.views.push_back(c);
        s.uses={{17,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_UNORDERED_ACCESS}};
        plan=PreparedBindings::Create(dev,s,error);REQUIRE(plan);
        s.tables.pop_back();bad=PreparedBindings::Create(dev,s,error);REQUIRE(bad); // incomplete heap rebasing must be refused BEFORE commands
    }
    auto provider=std::make_shared<Provider>();provider->plan=bad;
    na::observe::SetShaderOperationHook(na::observe::ShaderLane::Bindings,std::make_shared<BindingHook>(provider));
    Readback reduced_rb,native_rb;reduced_rb.Prepare(dev,group->Find(17)->reduced.Get());native_rb.Prepare(dev,native.Get());REQUIRE(reduced_rb.buffer&&native_rb.buffer);
    test::List recording;REQUIRE(recording.Create(dev));auto* list=recording.list.Get();REQUIRE(SUCCEEDED(list->Close()));REQUIRE(SUCCEEDED(list->Reset(recording.alloc.Get(),nullptr)));
    provider->recording=list;
    const float fill[4]={.75f,0,0,0};list->ClearRenderTargetView(rtv->GetCPUDescriptorHandleForHeapStart(),fill,0,nullptr);
    auto b=test::Transition(input.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);list->ResourceBarrier(1,&b);
    list->SetComputeRootSignature(root.Get());list->SetPipelineState(pso.Get());ID3D12DescriptorHeap* heaps[]{heap.Get(),samplers.Get()};list->SetDescriptorHeaps(2,heaps);
    const auto gpu=heap->GetGPUDescriptorHandleForHeapStart();for(UINT i=0;i<3;++i)list->SetComputeRootDescriptorTable(i,{gpu.ptr+uint64_t(i)*inc});
    list->SetComputeRootDescriptorTable(3,samplers->GetGPUDescriptorHandleForHeapStart());list->SetComputeRootConstantBufferView(4,cb->GetGPUVirtualAddress());
    na::observe::ShaderState shadow;REQUIRE(na::observe::ListShaderState(list,true,shadow));CHECK(shadow.count==4&&shadow.resources==heap.Get()&&shadow.samplers==samplers.Get());
    list->Dispatch(w/8,h/4,1); // incomplete private table map: original executes untouched
    CHECK(provider->refused==1);
    provider->plan=plan;list->Dispatch(w/8,h/4,1); // actual private21x11 dispatch/CBVs/SRV/UAV, native sampler
    provider->plan.reset();list->Dispatch(w/8,h/4,1); // all original tables, root CBV, sampler restored
    CHECK(provider->refused==1);
    {
        na::observe::HostWorkScope host;reduced_rb.Record(list,group->Find(17)->reduced.Get(),D3D12_RESOURCE_STATE_COMMON);
        native_rb.Record(list,native.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        // Make this executable recording replayable including its initial input state.
        b=test::Transition(input.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);list->ResourceBarrier(1,&b);
    }
    REQUIRE(SUCCEEDED(list->Close()));static_cast<float*>(mapped)[2]=9;cb->Unmap(0,nullptr); // late camera marker
    const std::weak_ptr<const PreparedBindings> lifetime=plan;plan.reset();bad.reset();constants.reset();CHECK(!lifetime.expired());
    ID3D12CommandList* lists[]{list};
    { na::nr::native::Submission receipt(d.queue.Get(),1,lists); d.queue->ExecuteCommandLists(1,lists); }
    d.WaitIdle();na::nr::native::Maintain();CHECK(!lifetime.expired()); // completed, but still executable
    test::ComPtr<ID3D12Fence> gate;REQUIRE(SUCCEEDED(dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate))));
    REQUIRE(SUCCEEDED(d.queue->Wait(gate.Get(),1)));
    { na::nr::native::Submission receipt(d.queue.Get(),1,lists); d.queue->ExecuteCommandLists(1,lists); }
    test::ComPtr<ID3D12CommandAllocator> fresh;REQUIRE(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&fresh))));
    REQUIRE(SUCCEEDED(list->Reset(fresh.Get(),nullptr)));REQUIRE(SUCCEEDED(list->Close()));na::nr::native::Maintain();CHECK(!lifetime.expired());
    gate->Signal(1);d.WaitIdle();na::nr::native::Maintain();CHECK(lifetime.expired());
    reduced_rb.Check(rw,rh,rw,rh);native_rb.Check(w,h,w,h);
    REQUIRE(Raster(d)==0);
    for(int mode=1;mode<=3;++mode)REQUIRE(Raster(d,mode)==0);
    na::observe::SetShaderOperationHook(na::observe::ShaderLane::Bindings,{});na_observe_stop();
    if(info)for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T n=0;info->GetMessage(i,nullptr,&n);std::vector<uint8_t> data(n);
        auto* m=reinterpret_cast<D3D12_MESSAGE*>(data.data());if(SUCCEEDED(info->GetMessage(i,m,&n))&&m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){
            std::printf("D3D12 error: %s\n",m->pDescription);CHECK(false);}}
    return test::finish("scoped_bindings_test");
}
