#include "test_support.hpp"
#include "host_dx12/observe/producers.hpp"
#include <d3dcompiler.h>
#include <atomic>
namespace {
std::atomic<unsigned> native_dispatches{0};
void __cdecl sink(const na_event_batch* batch,void*) {
    const auto* bytes=reinterpret_cast<const uint8_t*>(batch)+batch->header_bytes;
    for(uint32_t off=0;off<batch->payload_bytes;){
        const auto* h=reinterpret_cast<const na_event_header*>(bytes+off);
        if(h->size_bytes<sizeof *h)break;
        if(h->kind==NA_EV_LIST_OP){const auto* op=reinterpret_cast<const na_ev_list_op*>(h);if(op->op==NA_OP_DISPATCH)++native_dispatches;}
        off+=h->size_bytes;
    }
}
class Hook final:public na::observe::ShaderOperationHook {
public:
    bool reconstruct=false;int before=0,after=0,changed=0;
    explicit Hook(bool r):reconstruct(r){}
    na::observe::ShaderScope Before(ID3D12GraphicsCommandList* l,na::observe::ShaderCall& call) noexcept override {
        ++before;
        if(call.kind!=na::observe::ShaderKind::Dispatch)return {};
        if(reconstruct&&call.args[0]==3){
            l->SetComputeRoot32BitConstant(0,99,0);l->Dispatch(1,1,1);++changed;
            // Snapshot ownership must retain this hook through its After call.
            na::observe::SetShaderOperationHook(na::observe::ShaderLane::Reconstruction,{});
            return {na::observe::ShaderAction::Replaced,{}};
        }
        if(!reconstruct&&call.args[0]==2){l->SetComputeRoot32BitConstant(0,13,0);call.args[0]=1;++changed;}
        return {};
    }
    void After(ID3D12GraphicsCommandList* l,const na::observe::ShaderCall&,na::observe::ShaderScope&) noexcept override {
        ++after;na::observe::RestoreListState(l);
    }
};
}
int main()
{
    test::Device d;if(!test::CreateDevice(d))return test::SkipNoDevice("shader_operation_test");auto* dev=d.device.Get();
    REQUIRE(na::observe::InstallProducers(dev,d.queue.Get(),true));
    na_observe_config cfg{};cfg.size_bytes=sizeof cfg;REQUIRE(na_observe_start(&cfg,sink,nullptr)==NA_OK);
    auto original=std::make_shared<Hook>(true),bindings=std::make_shared<Hook>(false);
    na::observe::SetShaderOperationHook(na::observe::ShaderLane::Reconstruction,original);
    na::observe::SetShaderOperationHook(na::observe::ShaderLane::Bindings,bindings);
    const char source[]=R"(cbuffer C:register(b0){uint value;};RWStructuredBuffer<uint> result:register(u0);
[numthreads(1,1,1)]void main(uint3 i:SV_DispatchThreadID){result[i.x]=value;})";
    test::ComPtr<ID3DBlob> cs,errors,blob;REQUIRE(SUCCEEDED(D3DCompile(source,sizeof source-1,nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&cs,&errors)));
    D3D12_ROOT_PARAMETER params[2]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[0].Constants={0,0,1};
    params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC rs{};rs.NumParameters=2;rs.pParameters=params;
    REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors)));
    test::ComPtr<ID3D12RootSignature> root;REQUIRE(SUCCEEDED(dev->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root))));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={cs->GetBufferPointer(),cs->GetBufferSize()};
    test::ComPtr<ID3D12PipelineState> pso;REQUIRE(SUCCEEDED(dev->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pso))));
    auto output=test::CreateBuffer(dev,48,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto zero=test::CreateBuffer(dev,48,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    auto rb=test::CreateBuffer(dev,48,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);REQUIRE(output&&zero&&rb);
    void* p=nullptr;REQUIRE(SUCCEEDED(zero->Map(0,nullptr,&p)));std::memset(p,0,48);zero->Unmap(0,nullptr);
    test::List recording;REQUIRE(recording.Create(dev));auto* list=recording.list.Get();
    REQUIRE(SUCCEEDED(list->Close()));REQUIRE(SUCCEEDED(list->Reset(recording.alloc.Get(),nullptr)));
    list->CopyBufferRegion(output.Get(),0,zero.Get(),0,48);auto b=test::Transition(output.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);list->ResourceBarrier(1,&b);
    list->SetComputeRootSignature(root.Get());list->SetPipelineState(pso.Get());list->SetComputeRoot32BitConstant(0,7,0);
    list->SetComputeRootUnorderedAccessView(1,output->GetGPUVirtualAddress());list->Dispatch(2,1,1);
    list->SetComputeRootUnorderedAccessView(1,output->GetGPUVirtualAddress()+16);list->Dispatch(3,1,1);
    list->SetComputeRootUnorderedAccessView(1,output->GetGPUVirtualAddress()+32);list->Dispatch(4,1,1);
    b=test::Transition(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);list->ResourceBarrier(1,&b);
    list->CopyBufferRegion(rb.Get(),0,output.Get(),0,48);REQUIRE(SUCCEEDED(list->Close()));
    ID3D12CommandList* lists[]{list};d.queue->ExecuteCommandLists(1,lists);d.WaitIdle();
    const void* result=nullptr;REQUIRE(SUCCEEDED(rb->Map(0,nullptr,const_cast<void**>(&result))));uint32_t v[12];std::memcpy(v,result,sizeof v);rb->Unmap(0,nullptr);
    CHECK(v[0]==13&&v[1]==0&&v[2]==0&&v[3]==0); // rewritten dispatch count, scoped constant
    CHECK(v[4]==99&&v[5]==0&&v[6]==0&&v[7]==0); // original skipped, host dispatch not recursively intercepted
    CHECK(v[8]==7&&v[9]==7&&v[10]==7&&v[11]==7); // original forwarded exactly once, restored native constant
    CHECK(original->before==2&&original->after==2&&original->changed==1);
    CHECK(bindings->before==2&&bindings->after==2&&bindings->changed==1);
    REQUIRE(SUCCEEDED(list->Reset(recording.alloc.Get(),nullptr)));REQUIRE(SUCCEEDED(list->Close()));
    na::observe::SetShaderOperationHook(na::observe::ShaderLane::Bindings,{});na_observe_stop();
    CHECK(native_dispatches==2); // a replacement is not falsely reported as a native game write
    return test::finish("shader_operation_test");
}
