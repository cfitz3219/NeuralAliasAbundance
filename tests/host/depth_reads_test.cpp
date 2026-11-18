// Depth reads end to end, with no game: a sloping depth drawn through jittered viewports, then read
// by a "lighting" draw the way a game reads its scene depth. Before the hand-over the read sees the
// jitter (and the widened viewport); after it (a view that existed before, and one made after) it
// sees the depth the game's own pixel-centre rays expect, whichever sample the frame used; turned
// off, the game's own depth is back. Also (Astra's audit of 2db2250e): a recording that is never
// submitted hands nothing over; a view with its own component mapping keeps it; a barrier inside a
// game query adds nothing to the query; a barrier before the game has bound its state still gets
// the corrected depth before the read. And (her note on 2886fbfb): with the game's predication
// bound across the barrier, the read still gets THIS frame's depth, and the predication is back as
// the game set it. Wait/readback belong only to this test.
#include "test_support.hpp"
#include "host_dx12/observe/producers.hpp"
#include "host_dx12/observe/operation_snapshot.hpp"
#include "host_dx12/observe/texture_operation.hpp"
#include "host_dx12/nr/native_fg.hpp"
#include "host_dx12/j2/depth_reads.hpp"
#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <vector>
namespace {
void __cdecl sink(const na_event_batch*,void*){}
constexpr UINT W=64,H=32;
// What the depth buffer holds at a pixel for a viewport moved by (jx,jy). The host gives a
// full-size scene viewport half a pixel on each side, hence the +1s.
float scene_depth(UINT x,UINT y,float jx,float jy){return .1f+.2f*(float(x)+1.f-jx)/float(W+1)+.2f*(float(y)+1.f-jy)/float(H+1);}
// What the game's own pixel-centre rays expect there: the scene through its own viewport, not moved, not widened.
float true_depth(UINT x,UINT y){return .1f+.2f*(float(x)+.5f)/float(W)+.2f*(float(y)+.5f)/float(H);}
}
int main(){
    test::ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    test::Device d;if(!test::CreateDevice(d))return test::SkipNoDevice("depth_reads_test");auto* dev=d.device.Get();
    test::ComPtr<ID3D12InfoQueue> info;dev->QueryInterface(IID_PPV_ARGS(&info));
    REQUIRE(na::observe::InstallProducers(dev,d.queue.Get(),true));
    REQUIRE(na::observe::InstallListResetTracking(dev,&na::nr::native::Reset));
    na::observe::SetOperationSnapshots(true);
    na::observe::SetRasterStateTracking(true);
    na_observe_config config{};config.size_bytes=sizeof config;REQUIRE(na_observe_start(&config,&sink,nullptr)==NA_OK);
    constexpr auto format=DXGI_FORMAT_R32G32B32A32_FLOAT;
    auto color=test::CreateTexture(dev,W,H,format,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto depth=test::CreateTexture(dev,W,H,DXGI_FORMAT_R32_TYPELESS,D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    REQUIRE(color&&depth);const auto depth_id=na::observe::IdOf(depth.Get());na::observe::SetLiveSelection(depth_id,na::observe::IdOf(color.Get()));
    test::ComPtr<ID3D12DescriptorHeap> rt,ds,views;D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rt))));hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&ds))));
    hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=3;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&views))));
    test::ComPtr<ID3D12DescriptorHeap> staging;hd.NumDescriptors=1;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&staging))));
    const auto rtv=rt->GetCPUDescriptorHandleForHeapStart(),dsv=ds->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(color.Get(),nullptr,rtv);
    D3D12_DEPTH_STENCIL_VIEW_DESC dd{};dd.Format=DXGI_FORMAT_D32_FLOAT;dd.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;dev->CreateDepthStencilView(depth.Get(),&dd,dsv);
    // The game's two views of its depth: slot 0 exists before anything is handed over, slot 1 is made after.
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=DXGI_FORMAT_R32_FLOAT;sd.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;sd.Texture2D.MipLevels=1;
    const UINT step=dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu0=views->GetCPUDescriptorHandleForHeapStart(),cpu1=cpu0;cpu1.ptr+=step;
    auto gpu0=views->GetGPUDescriptorHandleForHeapStart(),gpu1=gpu0;gpu1.ptr+=step;
    dev->CreateShaderResourceView(depth.Get(),&sd,cpu0);
    // Slot 2: a view whose red channel is the constant 0, made in a staging heap and copied over (it must stay 0).
    auto cpu2=cpu1;cpu2.ptr+=step;auto gpu2=gpu1;gpu2.ptr+=step;
    {auto zero=sd;zero.Shader4ComponentMapping=D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
            D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1);
        dev->CreateShaderResourceView(depth.Get(),&zero,staging->GetCPUDescriptorHandleForHeapStart());
        dev->CopyDescriptorsSimple(1,cpu2,staging->GetCPUDescriptorHandleForHeapStart(),D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);}
    // A pipeline-statistics query the "game" holds open across its barrier.
    test::ComPtr<ID3D12QueryHeap> queries;D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS,1,0};
    REQUIRE(SUCCEEDED(dev->CreateQueryHeap(&qd,IID_PPV_ARGS(&queries))));
    auto counted=test::CreateBuffer(dev,sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);REQUIRE(counted);
    test::ComPtr<ID3DBlob> vs,ps,rps,blob,error;test::ComPtr<ID3D12RootSignature> scene_root,read_root;
    D3D12_ROOT_SIGNATURE_DESC rd{};REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error)));
    REQUIRE(SUCCEEDED(dev->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&scene_root))));
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameter.DescriptorTable={1,&range};parameter.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    rd.NumParameters=1;rd.pParameters=&parameter;blob.Reset();REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error)));
    REQUIRE(SUCCEEDED(dev->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&read_root))));
    constexpr char code[]=R"(struct V{float4 p:SV_Position;float2 uv:TEXCOORD;};
V VS(uint i:SV_VertexID){V v;v.uv=float2((i<<1)&2,i&2);v.p=float4(v.uv.x*2-1,1-v.uv.y*2,.1+.2*v.uv.x+.2*v.uv.y,1);return v;}
V Far(uint i:SV_VertexID){V v=VS(i);v.p.z+=.4;return v;}
float4 PS(V v):SV_Target{return float4(-1,-1,-1,1);}
Texture2D<float> scene_depth:register(t0);
float4 Read(V v):SV_Target{return float4(scene_depth.Load(int3(v.p.xy,0)),v.uv,1);})";
    REQUIRE(SUCCEEDED(D3DCompile(code,sizeof code-1,nullptr,nullptr,nullptr,"VS","vs_5_0",0,0,&vs,&error)));
    REQUIRE(SUCCEEDED(D3DCompile(code,sizeof code-1,nullptr,nullptr,nullptr,"PS","ps_5_0",0,0,&ps,&error)));
    test::ComPtr<ID3DBlob> fvs;REQUIRE(SUCCEEDED(D3DCompile(code,sizeof code-1,nullptr,nullptr,nullptr,"Far","vs_5_0",0,0,&fvs,&error)));
    REQUIRE(SUCCEEDED(D3DCompile(code,sizeof code-1,nullptr,nullptr,nullptr,"Read","ps_5_0",0,0,&rps,&error)));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};pd.pRootSignature=scene_root.Get();pd.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pd.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
    pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pd.RasterizerState.DepthClipEnable=TRUE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask=15;pd.SampleMask=UINT_MAX;pd.SampleDesc.Count=1;pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets=1;pd.RTVFormats[0]=format;pd.DSVFormat=DXGI_FORMAT_D32_FLOAT;
    pd.DepthStencilState.DepthEnable=TRUE;pd.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
    test::ComPtr<ID3D12PipelineState> scene,scene_far,read;REQUIRE(SUCCEEDED(dev->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&scene))));
    // The same scene 0.4 further away: a frame whose depth is not the frame before's.
    pd.VS={fvs->GetBufferPointer(),fvs->GetBufferSize()};REQUIRE(SUCCEEDED(dev->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&scene_far))));
    pd.VS={vs->GetBufferPointer(),vs->GetBufferSize()};
    // The "game's" predicate: 64 zero bits. Which of the two operations skips work is the card's to say; the test uses both.
    auto predicate=test::CreateBuffer(dev,8,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);REQUIRE(predicate);
    {void* at=nullptr;REQUIRE(SUCCEEDED(predicate->Map(0,nullptr,&at)));std::memset(at,0,8);predicate->Unmap(0,nullptr);}
    constexpr float kFar=.4f,kUntouched=-7.f;
    pd.pRootSignature=read_root.Get();pd.PS={rps->GetBufferPointer(),rps->GetBufferSize()};pd.DSVFormat=DXGI_FORMAT_UNKNOWN;pd.DepthStencilState={};
    REQUIRE(SUCCEEDED(dev->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&read))));
    const auto cdesc=color->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes=0;dev->GetCopyableFootprints(&cdesc,0,1,0,&fp,nullptr,nullptr,&bytes);
    auto readback=test::CreateBuffer(dev,bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);REQUIRE(readback);
    test::List list;REQUIRE(list.Create(dev));REQUIRE(SUCCEEDED(list.list->Close()));
    uint64_t frame_number=0;
    // One frame the way a deferred game records it; returns what its lighting read at every pixel.
    // Usual: everything the lighting draw needs is bound BEFORE the barrier, so the host's work there must hand it all back.
    // Late: the barrier comes first, with the scene's state still bound and no heaps; the lighting state follows.
    // Query: as Usual, with a pipeline-statistics query open across the barrier alone.
    // Unsubmitted: recorded up to the barrier, closed, never given to the queue.
    // Predicated: the far scene; as Usual, with the game's predication bound across the barrier and taken off before its lighting draw.
    // PredicatedRead: as Predicated, but the lighting draw itself is under the predication (taken off after it).
    enum class Mode{Usual,Late,Query,Unsubmitted,Predicated,PredicatedRead};
    uint64_t counted_primitives=0;
    auto frame=[&](float jx,float jy,D3D12_GPU_DESCRIPTOR_HANDLE view,std::vector<float>& out,Mode mode=Mode::Usual,D3D12_PREDICATION_OP op=D3D12_PREDICATION_OP_EQUAL_ZERO)->bool{
        const bool predicated=mode==Mode::Predicated||mode==Mode::PredicatedRead;
        list.Reset();auto* l=list.list.Get();na::observe::SetSceneViewportJitter(++frame_number,jx,jy);
        const D3D12_VIEWPORT vp{0,0,float(W),float(H),0,1};const D3D12_RECT sc{0,0,LONG(W),LONG(H)};
        l->SetGraphicsRootSignature(scene_root.Get());l->SetPipelineState(predicated?scene_far.Get():scene.Get());l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        l->OMSetRenderTargets(1,&rtv,FALSE,&dsv);l->RSSetViewports(1,&vp);l->RSSetScissorRects(1,&sc);
        l->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,1.f,0,0,nullptr);
        l->DrawInstanced(3,1,0,0);
        auto b=test::Transition(depth.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        const auto lighting_state=[&]{
            ID3D12DescriptorHeap* heaps[]={views.Get()};l->SetDescriptorHeaps(1,heaps);
            l->SetGraphicsRootSignature(read_root.Get());l->SetGraphicsRootDescriptorTable(0,view);l->SetPipelineState(read.Get());
            l->OMSetRenderTargets(1,&rtv,FALSE,nullptr);l->RSSetViewports(1,&vp);l->RSSetScissorRects(1,&sc);};
        if(mode==Mode::Usual){lighting_state();l->ResourceBarrier(1,&b);}
        else if(predicated){
            const float untouched[4]={kUntouched,kUntouched,kUntouched,kUntouched};l->ClearRenderTargetView(rtv,untouched,0,nullptr);
            lighting_state();l->SetPredication(predicate.Get(),0,op);l->ResourceBarrier(1,&b);
            if(mode==Mode::Predicated)l->SetPredication(nullptr,0,D3D12_PREDICATION_OP_EQUAL_ZERO);
        }
        else{
            if(mode==Mode::Query){lighting_state();l->BeginQuery(queries.Get(),D3D12_QUERY_TYPE_PIPELINE_STATISTICS,0);}
            l->ResourceBarrier(1,&b);
            if(mode==Mode::Query){l->EndQuery(queries.Get(),D3D12_QUERY_TYPE_PIPELINE_STATISTICS,0);
                l->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_PIPELINE_STATISTICS,0,1,counted.Get(),0);}
            if(mode==Mode::Unsubmitted)return SUCCEEDED(l->Close());
            if(mode==Mode::Late)lighting_state();
        }
        l->DrawInstanced(3,1,0,0);
        if(mode==Mode::PredicatedRead)l->SetPredication(nullptr,0,D3D12_PREDICATION_OP_EQUAL_ZERO);
        std::swap(b.Transition.StateBefore,b.Transition.StateAfter);l->ResourceBarrier(1,&b);
        {const na::observe::HostWorkScope own;
            auto c=test::Transition(color.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);l->ResourceBarrier(1,&c);
            D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=color.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=fp;l->CopyTextureRegion(&to,0,0,0,&from,nullptr);
            std::swap(c.Transition.StateBefore,c.Transition.StateAfter);l->ResourceBarrier(1,&c);}
        if(FAILED(l->Close()))return false;ID3D12CommandList* lists[]={l};
        {na::nr::native::Submission keep(d.queue.Get(),1,lists);d.queue->ExecuteCommandLists(1,lists);}d.WaitIdle();
        if(mode==Mode::Query){const void* q=nullptr;if(FAILED(counted->Map(0,nullptr,const_cast<void**>(&q))))return false;
            counted_primitives=static_cast<const D3D12_QUERY_DATA_PIPELINE_STATISTICS*>(q)->IAPrimitives;counted->Unmap(0,nullptr);}
        const void* data=nullptr;if(FAILED(readback->Map(0,nullptr,const_cast<void**>(&data))))return false;
        out.resize(size_t(W)*H);
        for(UINT y=0;y<H;++y)for(UINT x=0;x<W;++x){
            const auto* p=reinterpret_cast<const float*>(static_cast<const char*>(data)+fp.Offset+y*fp.Footprint.RowPitch+x*16);
            out[size_t(y)*W+x]=p[0];
            if(predicated&&p[0]==kUntouched&&p[1]==kUntouched)continue; // the lighting draw did not run here
            // The lighting draw's own interpolant: wrong if its viewport, scissor or target came back wrong.
            if(std::abs(p[1]-(float(x)+.5f)/W)>1e-5f||std::abs(p[2]-(float(y)+.5f)/H)>1e-5f){std::printf("read draw state wrong at %u,%u: %.6f/%.6f\n",x,y,p[1],p[2]);readback->Unmap(0,nullptr);return false;}
        }
        readback->Unmap(0,nullptr);return true;
    };
    // Largest difference from what the buffer holds for a viewport moved by (jx,jy), away from the picture's rim.
    auto worst=[&](const std::vector<float>& got,float jx,float jy){float m=0;UINT wx=0,wy=0;
        for(UINT y=2;y+2<H;++y)for(UINT x=2;x+2<W;++x){const float e=std::abs(got[size_t(y)*W+x]-scene_depth(x,y,jx,jy));if(e>m){m=e;wx=x;wy=y;}}
        std::printf("  against the buffer at jitter %+.3f/%+.3f: worst %.3g at %u,%u (read %.7f, buffer %.7f)\n",jx,jy,m,wx,wy,got[size_t(wy)*W+wx],scene_depth(wx,wy,jx,jy));return m;};
    // Largest difference from what the game's pixel-centre rays expect, over the WHOLE picture (the rim is where the widening shows most).
    auto wrong=[&](const std::vector<float>& got,float further=0.f){float m=0;UINT wx=0,wy=0;
        for(UINT y=0;y<H;++y)for(UINT x=0;x<W;++x){const float e=std::abs(got[size_t(y)*W+x]-(true_depth(x,y)+further));if(e>m){m=e;wx=x;wy=y;}}
        std::printf("  against the game's rays: worst %.3g at %u,%u (read %.7f, expected %.7f)\n",m,wx,wy,got[size_t(wy)*W+wx],true_depth(wx,wy)+further);return m;};
    auto untouched=[&](const std::vector<float>& got){for(float v:got)if(v!=kUntouched)return false;return true;};
    auto all_zero=[&](const std::vector<float>& got){for(float v:got)if(v!=0.f)return false;return true;};
    auto tick=[&](bool want){const na::observe::HostWorkScope own;na::j2::depth_reads::Tick(d.queue.Get(),depth_id,want);};
    char line[512]{};
    auto status=[&](const char* expect){na::j2::depth_reads::Format(line,sizeof line);std::printf("%s\n",line);
        char want[32];std::snprintf(want,sizeof want,"state=%s ",expect);return std::strstr(line,want)!=nullptr;};
    auto count=[&](const char* name){const char* at=std::strstr(line,name);return at?std::strtoull(at+std::strlen(name),nullptr,10):0ull;};
    constexpr float tol=2e-5f;const float s1[2]={.25f,-.375f},s2[2]={-.375f,-.25f};
    std::vector<float> got;
    // 0. Nothing handed over: the read sees the buffer as it is, which is not what the game's rays expect.
    std::printf("off:\n");REQUIRE(frame(s1[0],s1[1],gpu0,got));CHECK(worst(got,s1[0],s1[1])<tol);CHECK(wrong(got)>1e-3f);
    REQUIRE(frame(s1[0],s1[1],gpu2,got));CHECK(all_zero(got));
    // 1. Asked for. A recording that reaches the barrier and is never submitted hands nothing over.
    tick(true);CHECK(status("filling"));
    REQUIRE(frame(s1[0],s1[1],gpu0,got,Mode::Unsubmitted));tick(true);tick(true);CHECK(status("filling"));
    // 2. A submitted, finished frame: the twin was brought up to date, the game's views still name its own depth.
    std::printf("filling:\n");REQUIRE(frame(s1[0],s1[1],gpu0,got));CHECK(worst(got,s1[0],s1[1])<tol);
    // 3. Handed over: the view that existed (slot 0) is rewritten; a view made now (slot 1) goes to the twin.
    tick(true);CHECK(status("on"));
    dev->CreateShaderResourceView(depth.Get(),&sd,cpu1);
    std::printf("on, the view that existed, second sample:\n");REQUIRE(frame(s2[0],s2[1],gpu0,got));CHECK(wrong(got)<tol);
    std::printf("on, a view made after, first sample:\n");REQUIRE(frame(s1[0],s1[1],gpu1,got));CHECK(wrong(got)<tol);
    tick(true);
    std::printf("on, a view made after, second sample:\n");REQUIRE(frame(s2[0],s2[1],gpu1,got));CHECK(wrong(got)<tol);
    // The view with its own component mapping still reads its constant.
    REQUIRE(frame(s1[0],s1[1],gpu2,got));CHECK(all_zero(got));
    CHECK(status("on"));const auto corrected=count("corrected=");CHECK(corrected>=4&&count("views_rewritten=")>=2&&count("views_created=")>=1&&count("upgraded=")==0);
    // 4. The barrier before the game has bound anything for its lighting: a plain copy there, the corrected depth before the read.
    std::printf("on, barrier first:\n");REQUIRE(frame(s2[0],s2[1],gpu0,got,Mode::Late));CHECK(wrong(got)<tol);
    CHECK(status("on"));CHECK(count("upgraded=")==1&&count("copied=")>=1&&count("corrected=")==corrected);
    // 5. A game query open across the barrier counts nothing of the host's; the read is still corrected.
    std::printf("on, barrier inside a game query:\n");REQUIRE(frame(s1[0],s1[1],gpu1,got,Mode::Query));CHECK(wrong(got)<tol);
    std::printf("  primitives the game's query counted across its barrier: %llu\n",static_cast<unsigned long long>(counted_primitives));CHECK(counted_primitives==0);
    CHECK(status("on"));CHECK(count("upgraded=")==2);
    // 5b. The game's predication bound across the barrier and taken off before its lighting draw, on a frame whose
    //     depth is 0.4 away from the frame before: the read is THIS frame's corrected depth, whichever way the predicate falls.
    const auto lifted=count("predication_lifted="),before_predication=count("corrected=");
    for(const auto op:{D3D12_PREDICATION_OP_EQUAL_ZERO,D3D12_PREDICATION_OP_NOT_EQUAL_ZERO}){
        std::printf("on, barrier under the game's predication (operation %d):\n",int(op));
        REQUIRE(frame(s2[0],s2[1],gpu0,got,Mode::Predicated,op));CHECK(wrong(got,kFar)<tol);}
    CHECK(status("on"));CHECK(count("predication_lifted=")==lifted+2&&count("corrected=")==before_predication+2);
    // 5c. The predication is back as the game set it: its own lighting draw under it is skipped for exactly one of
    //     the two operations, and reads this frame's corrected depth for the other.
    {int skipped=0,drawn=0;
        for(const auto op:{D3D12_PREDICATION_OP_EQUAL_ZERO,D3D12_PREDICATION_OP_NOT_EQUAL_ZERO}){
            REQUIRE(frame(s1[0],s1[1],gpu1,got,Mode::PredicatedRead,op));
            if(untouched(got)){++skipped;std::printf("on, lighting under the game's predication (operation %d): skipped, as the game asked\n",int(op));}
            else{std::printf("on, lighting under the game's predication (operation %d):\n",int(op));if(wrong(got,kFar)<tol)++drawn;}}
        CHECK(skipped==1&&drawn==1);}
    CHECK(status("on"));
    // 6. Turned off live: every view is back on the game's depth.
    tick(false);CHECK(status("off"));
    std::printf("off again:\n");REQUIRE(frame(s2[0],s2[1],gpu0,got));CHECK(worst(got,s2[0],s2[1])<tol);
    REQUIRE(frame(s1[0],s1[1],gpu1,got));CHECK(worst(got,s1[0],s1[1])<tol);
    REQUIRE(frame(s1[0],s1[1],gpu2,got));CHECK(all_zero(got));
    // 7. And on again (a second twin; the first is parked).
    tick(true);REQUIRE(frame(s1[0],s1[1],gpu0,got));tick(true);CHECK(status("on"));
    std::printf("on again:\n");REQUIRE(frame(s2[0],s2[1],gpu1,got));CHECK(wrong(got)<tol);
    tick(false);na::j2::depth_reads::Shutdown();
    na::observe::SetSceneViewportJitter(0,0,0);
    if(info)for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T size=0;info->GetMessage(i,nullptr,&size);std::vector<uint8_t> data(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(data.data());
        if(SUCCEEDED(info->GetMessage(i,message,&size))&&message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::printf("D3D12 error: %s\n",message->pDescription);CHECK(false);}}
    CHECK(dev->GetDeviceRemovedReason()==S_OK);na_observe_stop();na::observe::SetProcessExiting();return test::finish("depth_reads_test");
}
