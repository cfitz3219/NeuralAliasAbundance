// Controlled submission-boundary test: real D3D12 recordings, no GPU execution.
#include "../host/test_support.hpp"
#include "host_dx12/observe/producers.hpp"
#include "providers/discovery/capture_decoder.hpp"
#include <vector>
#include <mutex>
#include <set>
#include <tuple>
namespace {
std::mutex mutex;
std::vector<na::discovery::DecodedBatch> batches;
void __cdecl Sink(const na_event_batch* b, void*) {
    auto copy=na::discovery::DecodeBatch(b,size_t(b->header_bytes)+b->payload_bytes);
    std::lock_guard<std::mutex> lock(mutex);batches.push_back(std::move(copy));
}
struct Stop { bool active=false; ~Stop(){if(active)na_observe_stop();} };
}
int main() {
    test::Device d;
    if(!test::CreateDevice(d))return test::SkipNoDevice("snapshot_bindings");
    REQUIRE(na::observe::InstallProducers(d.device.Get(),d.queue.Get(),true));
    na_observe_config cfg{};cfg.size_bytes=sizeof(cfg);
    REQUIRE(na_observe_start(&cfg,Sink,nullptr)==NA_OK);Stop stop;stop.active=true;
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_CBV,2,2,0,0};
    D3D12_ROOT_PARAMETER params[3]{};
    for(UINT i=0;i<2;++i){params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;params[i].Descriptor={i,0};}
    params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[2].DescriptorTable={1,&range};
    D3D12_ROOT_SIGNATURE_DESC desc{3,params,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    test::ComPtr<ID3DBlob> blob,error;
    REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error)));
    test::ComPtr<ID3D12RootSignature> root;
    REQUIRE(SUCCEEDED(d.device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root))));
    auto upload=test::CreateBuffer(d.device.Get(),4096,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    REQUIRE(upload);
    void* cpu=nullptr;REQUIRE(SUCCEEDED(upload->Map(0,nullptr,&cpu)));std::memset(cpu,0x5a,4096);
    test::ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,2,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
    REQUIRE(SUCCEEDED(d.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap))));
    const UINT inc=d.device->GetDescriptorHandleIncrementSize(hd.Type);
    for(UINT i=0;i<2;++i){
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{upload->GetGPUVirtualAddress(),256u*(i+1)};
        d.device->CreateConstantBufferView(&cbv,{heap->GetCPUDescriptorHandleForHeapStart().ptr+i*inc});
    }
    test::ComPtr<ID3D12CommandAllocator> alloc[2];
    test::ComPtr<ID3D12GraphicsCommandList> list[2];
    for(UINT i=0;i<2;++i){
        REQUIRE(SUCCEEDED(d.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc[i]))));
        REQUIRE(SUCCEEDED(d.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc[i].Get(),nullptr,IID_PPV_ARGS(&list[i]))));
        REQUIRE(SUCCEEDED(list[i]->Close()));REQUIRE(SUCCEEDED(list[i]->Reset(alloc[i].Get(),nullptr)));
        list[i]->SetGraphicsRootSignature(root.Get());
        ID3D12DescriptorHeap* heaps[]={heap.Get()};list[i]->SetDescriptorHeaps(1,heaps);
        list[i]->SetGraphicsRootConstantBufferView(0,upload->GetGPUVirtualAddress());
        list[i]->SetGraphicsRootConstantBufferView(1,upload->GetGPUVirtualAddress());
        list[i]->SetGraphicsRootDescriptorTable(2,heap->GetGPUDescriptorHandleForHeapStart());
        REQUIRE(SUCCEEDED(list[i]->Close()));
    }
    ID3D12CommandList* lists[]={list[0].Get(),list[1].Get()};
    const auto token=na::observe::ExecuteBegin(d.queue.Get(),2,lists);
    na::observe::ExecuteEnd(token,d.queue.Get(),2,lists);
    std::memset(cpu,0xa5,4096); // retained payload must be independent of source memory
    upload->Unmap(0,nullptr);
    REQUIRE(na_observe_stop()==NA_OK);stop.active=false;
    const auto resource=na::observe::IdOf(upload.Get());
    const auto a=na::observe::IdOf(list[0].Get()),b=na::observe::IdOf(list[1].Get());
    CHECK(resource && a && b && a!=b);
    std::set<std::tuple<uint64_t,uint64_t,uint32_t,uint32_t>> associations;
    unsigned count=0,executes=0;
    for(const auto& batch:batches){
        CHECK(batch.valid);
        for(const auto& item:batch.events){
            const auto* raw=batch.bytes.data()+item.offset;
            if(item.header.kind==NA_EV_QUEUE_EXECUTE){++executes;continue;}
            if(item.header.kind!=NA_EV_SNAPSHOT)continue;
            na_ev_snapshot s{};std::memcpy(&s,raw,sizeof(s));++count;
            CHECK(batch.incomplete_capture);
            CHECK(s.resource==resource && s.offset==0 && (s.list==a || s.list==b));
            CHECK(s.queue==token.queue_id && s.queue_ticket==token.ticket && s.recording_generation==2);
            CHECK(s.status==NA_SNAP_CAPTURED && s.sync==NA_SNAP_SYNC_APP_UNKNOWN);
            CHECK(s.captured_bytes==s.requested_bytes && s.captured_bytes>0);
            CHECK((s.param==0 && s.op_index==3) || (s.param==1 && s.op_index==4) || (s.param==2 && s.op_index==5));
            if(s.param==2)CHECK(s.binding==NA_SNAP_TABLE_CBV && (s.requested_bytes==256 || s.requested_bytes==512));
            else CHECK(s.binding==NA_SNAP_ROOT_CBV);
            associations.emplace(s.list,s.op_index,s.param,s.requested_bytes);
            for(uint32_t i=0;i<s.captured_bytes;++i)CHECK(raw[s.payload_offset+i]==0x5a);
        }
    }
    CHECK(count==8 && associations.size()==8 && executes==1);
    std::printf("snapshot bindings: %u records, %zu distinct associations\n",count,associations.size());
    return test::finish("snapshot_bindings");
}
