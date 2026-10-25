#include "../host/test_support.hpp"
#include "host_dx12/observe/producers.hpp"
#include "providers/discovery/capture_decoder.hpp"
#include <mutex>
#include <vector>
namespace {
std::mutex mutex;
std::vector<std::vector<na_view_ref>> observed;
void __cdecl Sink(const na_event_batch* b,void*) {
    auto decoded=na::discovery::DecodeBatch(b,size_t(b->header_bytes)+b->payload_bytes);
    std::lock_guard<std::mutex> lock(mutex);
    for(const auto& e:decoded.events) {
        if(e.header.kind!=NA_EV_LIST_OP) continue;
        const auto* p=decoded.bytes.data()+e.offset;
        na_ev_list_op op{};std::memcpy(&op,p,sizeof(op));
        if(op.op!=NA_OP_OM_SET_RENDER_TARGETS) continue;
        std::vector<na_view_ref> refs;
        for(uint64_t i=0;i<op.a[0]+op.a[1];++i) {
            na_view_ref ref{};std::memcpy(&ref,p+op.payload_offset+i*op.payload_stride,sizeof(ref));refs.push_back(ref);
        }
        observed.push_back(refs);
    }
}
}
int main() {
    test::Device d;if(!test::CreateDevice(d)) return test::SkipNoDevice("nonzero_targets");
    REQUIRE(na::observe::InstallProducers(d.device.Get(),d.queue.Get(),true));
    REQUIRE(na_observe_start(nullptr,Sink,nullptr)==NA_OK);
    auto color=test::CreateTexture(d.device.Get(),32,32,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto other=test::CreateTexture(d.device.Get(),32,32,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto depth=test::CreateTexture(d.device.Get(),32,32,DXGI_FORMAT_D32_FLOAT,D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    REQUIRE(color && other && depth);
    test::ComPtr<ID3D12DescriptorHeap> rh,dh;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,5,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
    REQUIRE(SUCCEEDED(d.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rh))));
    hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;hd.NumDescriptors=3;
    REQUIRE(SUCCEEDED(d.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&dh))));
    const auto ri=d.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const auto di=d.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    auto r=rh->GetCPUDescriptorHandleForHeapStart();auto z=dh->GetCPUDescriptorHandleForHeapStart();
    d.device->CreateRenderTargetView(other.Get(),nullptr,r); // valid decoy at zero
    d.device->CreateDepthStencilView(depth.Get(),nullptr,z);
    r.ptr+=3*ri;z.ptr+=2*di;
    d.device->CreateRenderTargetView(color.Get(),nullptr,r);
    D3D12_CPU_DESCRIPTOR_HANDLE r2{r.ptr+ri};d.device->CreateRenderTargetView(other.Get(),nullptr,r2);
    d.device->CreateDepthStencilView(depth.Get(),nullptr,z);
    test::ComPtr<ID3D12CommandAllocator> a;test::ComPtr<ID3D12GraphicsCommandList> l;
    REQUIRE(SUCCEEDED(d.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&a))));
    REQUIRE(SUCCEEDED(d.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,a.Get(),nullptr,IID_PPV_ARGS(&l))));
    REQUIRE(SUCCEEDED(l->Close()));REQUIRE(SUCCEEDED(l->Reset(a.Get(),nullptr)));
    l->OMSetRenderTargets(2,&r,TRUE,&z);
    D3D12_CPU_DESCRIPTOR_HANDLE separate[]{r2,r};l->OMSetRenderTargets(2,separate,FALSE,&z);
    REQUIRE(SUCCEEDED(l->Close()));REQUIRE(na_observe_stop()==NA_OK);
    REQUIRE(observed.size()==2 && observed[0].size()==3 && observed[1].size()==3);
    CHECK(observed[0][0].slot==3 && observed[0][0].resource==na::observe::IdOf(color.Get()));
    CHECK(observed[0][1].slot==4 && observed[0][1].resource==na::observe::IdOf(other.Get()));
    CHECK(observed[0][2].slot==2 && observed[0][2].descriptor_kind==NA_DESC_DSV);
    CHECK(observed[1][0].slot==4 && observed[1][1].slot==3 && observed[1][2].slot==2);
    return test::finish("nonzero_targets");
}
