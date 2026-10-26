// Test-only activation/transport integration. Never uses a game installation.
#include "../host/test_support.hpp"
#include "host_dx12/host_boot/game_adapter.h"
#include "host_dx12/observe/producers.hpp"
#include "host_dx12/host_observe_internal.hpp"
#include <filesystem>
#include <fstream>
#include <string>
#include <d3dcompiler.h>

// Real GPU work through the production adapter. Test-only idle waits keep
// allocator reuse valid; the adapter itself must remain fully asynchronous.
static int RenderProbe(test::Device& d) {
    D3D12_ROOT_PARAMETER parameter{};parameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;
    D3D12_ROOT_SIGNATURE_DESC rd{1,&parameter,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    test::ComPtr<ID3DBlob> root_blob,error,vs,ps;
    REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&root_blob,&error)));
    test::ComPtr<ID3D12RootSignature> root;
    REQUIRE(SUCCEEDED(d.device->CreateRootSignature(0,root_blob->GetBufferPointer(),root_blob->GetBufferSize(),IID_PPV_ARGS(&root))));
    const char* shader=R"(
cbuffer Camera : register(b0) { row_major float4x4 vp; };
float4 VS(uint id:SV_VertexID):SV_Position {
    float2 uv=float2((id<<1)&2,id&2);return mul(vp,float4((uv*2-1)*4,3+uv.x,1));
}
float4 PS(float4 p:SV_Position):SV_Target {return float4(p.xy/128,0.25,1);}
)";
    REQUIRE(SUCCEEDED(D3DCompile(shader,std::strlen(shader),nullptr,nullptr,nullptr,"VS","vs_5_0",0,0,&vs,&error)));
    REQUIRE(SUCCEEDED(D3DCompile(shader,std::strlen(shader),nullptr,nullptr,nullptr,"PS","ps_5_0",0,0,&ps,&error)));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();
    pd.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pd.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask=UINT_MAX;pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable=TRUE;pd.DepthStencilState.DepthEnable=TRUE;
    pd.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS;
    pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;pd.NumRenderTargets=1;
    pd.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;pd.DSVFormat=DXGI_FORMAT_D32_FLOAT;pd.SampleDesc.Count=1;
    test::ComPtr<ID3D12PipelineState> pipeline;REQUIRE(SUCCEEDED(d.device->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&pipeline))));
    auto depth=test::CreateTexture(d.device.Get(),128,128,DXGI_FORMAT_D32_FLOAT,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    auto color=test::CreateTexture(d.device.Get(),128,128,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto camera=test::CreateBuffer(d.device.Get(),256,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    REQUIRE(depth && color && camera);
    void* cpu=nullptr;REQUIRE(SUCCEEDED(camera->Map(0,nullptr,&cpu)));std::memset(cpu,0,256);
    const float matrix[]={1.3f,0,0,0,0,1.8f,0,0,0,0,30.0f/29.9f,-3.0f/29.9f,0,0,1,0};
    std::memcpy(cpu,matrix,sizeof(matrix));
    test::ComPtr<ID3D12DescriptorHeap> rt_heap,ds_heap;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
    REQUIRE(SUCCEEDED(d.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rt_heap))));hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    REQUIRE(SUCCEEDED(d.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&ds_heap))));
    const auto rt=rt_heap->GetCPUDescriptorHandleForHeapStart(),ds=ds_heap->GetCPUDescriptorHandleForHeapStart();
    d.device->CreateRenderTargetView(color.Get(),nullptr,rt);d.device->CreateDepthStencilView(depth.Get(),nullptr,ds);
    test::ComPtr<ID3D12CommandAllocator> allocator;test::ComPtr<ID3D12GraphicsCommandList> list;
    REQUIRE(SUCCEEDED(d.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator))));
    REQUIRE(SUCCEEDED(d.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list))));
    REQUIRE(SUCCEEDED(list->Close()));
    const auto start=GetTickCount64();
    while(GetTickCount64()-start<4500) {
        REQUIRE(SUCCEEDED(allocator->Reset()));REQUIRE(SUCCEEDED(list->Reset(allocator.Get(),nullptr)));
        D3D12_RESOURCE_BARRIER barriers[]={test::Transition(depth.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_DEPTH_WRITE),
            test::Transition(color.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET)};
        list->ResourceBarrier(2,barriers);list->SetPipelineState(pipeline.Get());list->SetGraphicsRootSignature(root.Get());
        list->SetGraphicsRootConstantBufferView(0,camera->GetGPUVirtualAddress());
        D3D12_VIEWPORT viewport{0,0,128,128,0,1};D3D12_RECT scissor{0,0,128,128};
        list->RSSetViewports(1,&viewport);list->RSSetScissorRects(1,&scissor);list->OMSetRenderTargets(1,&rt,FALSE,&ds);
        const float clear[]={0,0,0,1};list->ClearRenderTargetView(rt,clear,0,nullptr);list->ClearDepthStencilView(ds,D3D12_CLEAR_FLAG_DEPTH,1,0,0,nullptr);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);list->DrawInstanced(3,1,0,0);
        for(auto& b:barriers) std::swap(b.Transition.StateBefore,b.Transition.StateAfter);
        list->ResourceBarrier(2,barriers);REQUIRE(SUCCEEDED(list->Close()));
        ID3D12CommandList* lists[]={list.Get()};d.queue->ExecuteCommandLists(1,lists);d.WaitIdle();Sleep(20);
    }
    camera->Unmap(0,nullptr);
    return 0;
}

int main() {
    wchar_t exe[MAX_PATH]{};GetModuleFileNameW(nullptr,exe,MAX_PATH);
    const auto dir=std::filesystem::path(exe).parent_path();
    const auto ini=dir/L"NeuralAliasHost.ini",log=dir/L"diagnostic-test.jsonl";
    // Refuse to overwrite a user's configuration beside the test executable.
    REQUIRE(!std::filesystem::exists(ini));
    CHECK(na_adapter_prepare()==0);
    struct Cleanup { std::filesystem::path path; ~Cleanup(){std::filesystem::remove(path);} } cleanup{ini};
    REQUIRE(WritePrivateProfileStringW(L"discovery",L"enabled",L"1",ini.c_str()));
    REQUIRE(WritePrivateProfileStringW(L"discovery",L"read_only",L"0",ini.c_str()));
    CHECK(na_adapter_prepare()==0);
    REQUIRE(WritePrivateProfileStringW(L"discovery",L"read_only",L"1",ini.c_str()));
    REQUIRE(WritePrivateProfileStringW(L"discovery",L"log",log.c_str(),ini.c_str()));
    CHECK(na_adapter_prepare()==1);
    test::Device d;if(!test::CreateDevice(d)) return test::SkipNoDevice("diagnostic_adapter");
    REQUIRE(na::observe::InstallProducers(d.device.Get(),d.queue.Get(),true));
    REQUIRE(na_adapter_start()==1);
    { std::ifstream live(log);CHECK(live.is_open()); }
    CHECK(na_adapter_start()==0);
    auto resource=test::CreateBuffer(d.device.Get(),4096,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON);
    REQUIRE(resource);
    auto candidate=test::CreateTexture(d.device.Get(),128,128,DXGI_FORMAT_D32_FLOAT,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    REQUIRE(candidate);
    // Mixed-status fixture through the real transport: 32 unavailable records
    // followed by a captured matrix must not suppress all structural scans.
    struct SnapshotFixture { na_ev_snapshot event; float matrix[16]; } fixture{};
    fixture.event.hdr.kind=NA_EV_SNAPSHOT;
    fixture.event.hdr.size_bytes=sizeof(na_ev_snapshot);
    fixture.event.resource=na::observe::IdOf(resource.Get());
    fixture.event.queue=na::observe::IdOf(d.queue.Get());
    fixture.event.queue_ticket=1;fixture.event.requested_bytes=64;
    fixture.event.sync=NA_SNAP_SYNC_APP_UNKNOWN;
    fixture.event.status=NA_SNAP_NOT_MAPPED;
    for(unsigned i=1;i<=32;++i){fixture.event.snapshot_id=i;REQUIRE(na::observe_internal::Emit(&fixture,NA_GROUP_SNAPSHOT));}
    fixture.event.snapshot_id=33;fixture.event.status=NA_SNAP_CAPTURED;
    fixture.event.captured_bytes=64;fixture.event.payload_offset=offsetof(SnapshotFixture,matrix);
    fixture.event.hdr.size_bytes=sizeof(fixture);
    fixture.matrix[0]=fixture.matrix[5]=fixture.matrix[10]=fixture.matrix[15]=1;
    REQUIRE(na::observe_internal::Emit(&fixture,NA_GROUP_SNAPSHOT));
    REQUIRE(RenderProbe(d)==0);
    // Stop drains the real transport, including its final batch, before freeing context.
    na_adapter_stop();na_adapter_stop();
    std::ifstream file(log);const std::string text((std::istreambuf_iterator<char>(file)),{});
    CHECK(text.find("\"read_only\":true")!=std::string::npos);
    CHECK(text.find("\"phase\":\"stopped\"")!=std::string::npos);
    CHECK(text.find("\"malformed\":0")!=std::string::npos);
    CHECK(text.find("\"confirmed_inputs\":0")!=std::string::npos);
    CHECK(text.find("\"hypotheses\":2")!=std::string::npos);
    CHECK(text.find("\"execution_qualified\":false")!=std::string::npos);
    CHECK(text.find("\"signal\":\"depth\"")!=std::string::npos);
    CHECK(text.find("\"signal\":\"scene_color\"")!=std::string::npos);
    CHECK(text.find("\"confidence\":\"candidate\"")!=std::string::npos);
    CHECK(text.find("\"matched_execution\":true")!=std::string::npos);
    CHECK(text.find("\"scalar_decoded\":true")!=std::string::npos);
    CHECK(text.find("\"type\":\"gpu_camera_hypothesis\"")!=std::string::npos);
    CHECK(text.find("\"type\":\"heuristics\"")!=std::string::npos);     // C13 rankings always reported
    CHECK(text.find("\"inverse_pairs\":")!=std::string::npos);
    CHECK(text.find("classifier_reasons")==std::string::npos);            // R41-3: rankings never claim classifier evidence
    return test::finish("diagnostic_adapter");
}
