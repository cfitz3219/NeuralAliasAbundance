// A game uploads camera/jitter AFTER recording. The resized operation must see
// that late value while a following native/HUD operation retains native sizes.
// Replay only after completion, using explicit buffer states at each boundary.
#include "test_support.hpp"
#include "host_dx12/scale/gpu_constants.hpp"
#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#include <array>
#include <cmath>
int run(test::Device& d,bool gpu_source)
{
    auto* dev=d.device.Get();
    auto upload=test::CreateBuffer(dev,256,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    auto original=gpu_source?test::CreateBuffer(dev,256,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON):upload;
    auto output=test::CreateBuffer(dev,32,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto readback=test::CreateBuffer(dev,32,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    REQUIRE(upload&&original&&output&&readback);
    const std::array<float,4> initial{128,64,3,7};
    void* mapped=nullptr;REQUIRE(SUCCEEDED(upload->Map(0,nullptr,&mapped)));std::memcpy(mapped,initial.data(),sizeof initial);
    std::string error;
    const auto copy=na::scale::GpuConstants::Create(dev,original.Get(),0,initial.data(),sizeof initial,
        {{0,na::scale::Scalar::Float,na::scale::Dimension::Width,{128,64},{86,43}},
         {4,na::scale::Scalar::Float,na::scale::Dimension::Height,{128,64},{86,43}}},error);
    if(!copy)std::printf("prepare: %s\n",error.c_str());REQUIRE(copy);
    constexpr char source[]=R"(
cbuffer Data:register(b0){float4 data;}; RWByteAddressBuffer result:register(u0);
[numthreads(1,1,1)] void main(){result.Store4(0,asuint(data));}
)";
    test::ComPtr<ID3DBlob> cs,errors,blob;
    REQUIRE(SUCCEEDED(D3DCompile(source,sizeof source-1,nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&cs,&errors)));
    D3D12_ROOT_PARAMETER params[2]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC rs{};rs.NumParameters=2;rs.pParameters=params;
    REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors)));
    test::ComPtr<ID3D12RootSignature> root;REQUIRE(SUCCEEDED(dev->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root))));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={cs->GetBufferPointer(),cs->GetBufferSize()};
    test::ComPtr<ID3D12PipelineState> pso;REQUIRE(SUCCEEDED(dev->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pso))));
    test::List recording;REQUIRE(recording.Create(dev));auto* list=recording.list.Get();
    auto b=test::Transition(output.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);list->ResourceBarrier(1,&b);
    if(gpu_source){b=test::Transition(original.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);list->ResourceBarrier(1,&b);}
    list->SetComputeRootSignature(root.Get());list->SetPipelineState(pso.Get());
    const auto source_state=gpu_source?D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER:D3D12_RESOURCE_STATE_GENERIC_READ;
    if(!gpu_source)CHECK(!copy->RecordBefore(list,D3D12_RESOURCE_STATE_COPY_DEST)); // never transitions an upload heap away from GENERIC_READ
    REQUIRE(copy->RecordBefore(list,source_state));
    list->SetComputeRootConstantBufferView(0,copy->Address());list->SetComputeRootUnorderedAccessView(1,output->GetGPUVirtualAddress());list->Dispatch(1,1,1);
    list->SetComputeRootConstantBufferView(0,original->GetGPUVirtualAddress());copy->RecordAfter(list);
    list->SetComputeRootUnorderedAccessView(1,output->GetGPUVirtualAddress()+16);list->Dispatch(1,1,1);
    b=test::Transition(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);list->ResourceBarrier(1,&b);
    list->CopyBufferRegion(readback.Get(),0,output.Get(),0,32);
    b=test::Transition(output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);list->ResourceBarrier(1,&b);
    if(gpu_source){b=test::Transition(original.Get(),D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,D3D12_RESOURCE_STATE_COMMON);list->ResourceBarrier(1,&b);}
    REQUIRE(SUCCEEDED(list->Close()));
    for(int frame=0;frame<2;++frame){
        const float late=frame?11.f:9.f;
        static_cast<float*>(mapped)[2]=late; // AFTER command recording and patch preparation
        if(gpu_source){
            test::List transfer;REQUIRE(transfer.Create(dev));
            b=test::Transition(original.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
            transfer.list->ResourceBarrier(1,&b);transfer.list->CopyBufferRegion(original.Get(),0,upload.Get(),0,16);
            b=test::Transition(original.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);transfer.list->ResourceBarrier(1,&b);
            REQUIRE(SUCCEEDED(transfer.list->Close()));ID3D12CommandList* one[]{transfer.list.Get()};d.queue->ExecuteCommandLists(1,one);d.WaitIdle();
        }
        // Legal replay: the previous execution completed before submitting this
        // same executable command list again (ExecuteCommandLists contract).
        ID3D12CommandList* lists[]{list};d.queue->ExecuteCommandLists(1,lists);d.WaitIdle();
        const void* result=nullptr;REQUIRE(SUCCEEDED(readback->Map(0,nullptr,const_cast<void**>(&result))));
        std::array<float,8> values{};std::memcpy(values.data(),result,sizeof values);readback->Unmap(0,nullptr);
        CHECK(values[0]==86&&values[1]==43&&values[2]==late&&values[3]==7);
        CHECK(values[4]==128&&values[5]==64&&values[6]==late&&values[7]==7);
        CHECK(static_cast<float*>(mapped)[0]==128&&static_cast<float*>(mapped)[1]==64);
    }
    upload->Unmap(0,nullptr);
    return 0;
}
int main(){test::ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    test::Device d;if(!test::CreateDevice(d))return test::SkipNoDevice("gpu_constants_test");
    test::ComPtr<ID3D12InfoQueue> info;d.device.As(&info);if(run(d,false)||run(d,true))return 1;
    if(info)for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T n=0;info->GetMessage(i,nullptr,&n);std::vector<uint8_t> data(n);
        auto* m=reinterpret_cast<D3D12_MESSAGE*>(data.data());if(SUCCEEDED(info->GetMessage(i,m,&n))&&m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){
            std::printf("D3D12 error: %s\n",m->pDescription);CHECK(false);}}
    return test::finish("gpu_constants_test");}
