// Real DX12 recording evidence: bounded resource membership, unchanged table
// reuse, descriptor mutation/copy and Reset. No game or model evaluation.
#include "test_support.hpp"
#include "host_dx12/host_observe.h"
#include "host_dx12/observe/producers.hpp"
#include <vector>
namespace { void __cdecl Sink(const na_event_batch*,void*) {} }
int main() {
    test::Device d;if(!test::CreateDevice(d))return test::SkipNoDevice("observe_bookkeeping_test");
    auto* dev=d.device.Get();REQUIRE(na::observe::InstallProducers(dev,d.queue.Get(),true));
    na_observe_config cfg{};cfg.size_bytes=sizeof(cfg);REQUIRE(na_observe_start(&cfg,Sink,nullptr)==NA_OK);
    test::List list;REQUIRE(list.Create(dev));REQUIRE(SUCCEEDED(list.list->Close()));
    const auto reset=[&]{return SUCCEEDED(list.alloc->Reset())&&SUCCEEDED(list.list->Reset(list.alloc.Get(),nullptr));};
    std::vector<test::ComPtr<ID3D12Resource>> resources;
    for(unsigned i=0;i<4097;++i){
        auto b=test::CreateBuffer(dev,256,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        REQUIRE(b);resources.push_back(std::move(b));
    }
    const auto id=[&](unsigned i){return na::observe::IdOf(resources[i].Get());};
    const auto use=[&](unsigned i){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;b.UAV.pResource=resources[i].Get();list.list->ResourceBarrier(1,&b);};
    REQUIRE(reset());
    // Reverse order forces compaction; saturated duplicates retain every ID.
    for(unsigned i=4096;i>0;--i)use(i);
    for(unsigned i=1;i<=4096;++i)use(i);
    use(0); // capacity exceeded: no eviction, no unbounded growth
    for(unsigned i=1;i<=4096;++i)CHECK(na::observe::ListReferences(list.list.Get(),id(i)));
    CHECK(!na::observe::ListReferences(list.list.Get(),id(0)));
    REQUIRE(SUCCEEDED(list.list->Close()));REQUIRE(reset());
    CHECK(!na::observe::ListReferences(list.list.Get(),id(4096)));
    // Repeated uses do not consume the unique-resource capacity.
    for(unsigned i=0;i<6000;++i)use(i%2);
    use(3);CHECK(na::observe::ListReferences(list.list.Get(),id(0)));CHECK(na::observe::ListReferences(list.list.Get(),id(1)));CHECK(na::observe::ListReferences(list.list.Get(),id(3)));

    test::ComPtr<ID3D12DescriptorHeap> heap,other,source;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,2,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap))));REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&other))));
    hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&source))));
    auto a=test::CreateTexture(dev,8,8,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_FLAG_NONE);
    auto b=test::CreateTexture(dev,8,8,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_FLAG_NONE);
    auto c=test::CreateTexture(dev,8,8,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_FLAG_NONE);REQUIRE(a&&b&&c);
    const auto aid=na::observe::IdOf(a.Get()),bid=na::observe::IdOf(b.Get()),cid=na::observe::IdOf(c.Get());
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};D3D12_ROOT_PARAMETER param{};param.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;param.DescriptorTable={1,&range};
    D3D12_ROOT_SIGNATURE_DESC rd{1,&param,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};test::ComPtr<ID3DBlob> blob,err;test::ComPtr<ID3D12RootSignature> root;
    REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&err)));REQUIRE(SUCCEEDED(dev->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root))));
    ID3D12DescriptorHeap* heaps[]={heap.Get()};list.list->SetDescriptorHeaps(1,heaps);list.list->SetGraphicsRootSignature(root.Get());
    dev->CreateShaderResourceView(a.Get(),nullptr,heap->GetCPUDescriptorHandleForHeapStart());
    for(unsigned i=0;i<4;++i)list.list->SetGraphicsRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());
    CHECK(na::observe::ListSamplesTexture(list.list.Get(),aid));CHECK(!na::observe::ListSamplesTexture(list.list.Get(),bid));
    // A changed descriptor and a copy into the same slot must both be noticed.
    dev->CreateShaderResourceView(b.Get(),nullptr,heap->GetCPUDescriptorHandleForHeapStart());list.list->SetGraphicsRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());
    CHECK(na::observe::ListSamplesTexture(list.list.Get(),aid));CHECK(na::observe::ListSamplesTexture(list.list.Get(),bid));
    dev->CreateShaderResourceView(c.Get(),nullptr,source->GetCPUDescriptorHandleForHeapStart());dev->CopyDescriptorsSimple(1,heap->GetCPUDescriptorHandleForHeapStart(),source->GetCPUDescriptorHandleForHeapStart(),hd.Type);
    list.list->SetGraphicsRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());CHECK(na::observe::ListSamplesTexture(list.list.Get(),cid));
    REQUIRE(SUCCEEDED(list.list->Close()));REQUIRE(reset());
    CHECK(!na::observe::ListSamplesTexture(list.list.Get(),aid));CHECK(!na::observe::ListSamplesTexture(list.list.Get(),cid));
    dev->CreateShaderResourceView(a.Get(),nullptr,other->GetCPUDescriptorHandleForHeapStart());heaps[0]=other.Get();list.list->SetDescriptorHeaps(1,heaps);list.list->SetGraphicsRootSignature(root.Get());
    list.list->SetGraphicsRootDescriptorTable(0,other->GetGPUDescriptorHandleForHeapStart());CHECK(na::observe::ListSamplesTexture(list.list.Get(),aid));CHECK(!na::observe::ListSamplesTexture(list.list.Get(),bid));
    REQUIRE(SUCCEEDED(list.list->Close()));na_observe_stop();na::observe::SetProcessExiting();
    return test::finish("observe_bookkeeping_test");
}
