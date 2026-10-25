// Test-only GPU submission/readback wait. Production Pump never waits.
#include "../host/test_support.hpp"
#include "providers/classifier/temporal_depth.hpp"
#include "host_dx12/observe/producers.hpp"
namespace {
void __cdecl Sink(const na_event_batch*,void*){}
struct Stop { ~Stop(){na_observe_stop();} };
}
int main() {
    test::Device d; if(!test::CreateDevice(d)) return test::SkipNoDevice("sample_acquisition_host");
    REQUIRE(na::observe::InstallProducers(d.device.Get(),d.queue.Get(),true));
    na_observe_config cfg{}; cfg.size_bytes=sizeof(cfg);
    REQUIRE(na_observe_start(&cfg,Sink,nullptr)==NA_OK); Stop stop;
    na_observe_status scope{}; scope.size_bytes=sizeof(scope);
    REQUIRE(na_observe_query_status(&scope)==NA_OK);
    auto depth=test::CreateTexture(d.device.Get(),32,16,DXGI_FORMAT_D32_FLOAT,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    REQUIRE(depth);
    test::ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_DSV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
    REQUIRE(SUCCEEDED(d.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap))));
    d.device->CreateDepthStencilView(depth.Get(),nullptr,heap->GetCPUDescriptorHandleForHeapStart());
    na::discovery::HostSampleApi api; na::discovery::SampleAcquisition acquisition(api);
    acquisition.Scope(scope.session,scope.epoch);
    na::classifier::DepthFrame frames[2];
    for(unsigned frame=0;frame<2;++frame) {
        test::ComPtr<ID3D12CommandAllocator> alloc;
        test::ComPtr<ID3D12GraphicsCommandList> list;
        REQUIRE(SUCCEEDED(d.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc))));
        REQUIRE(SUCCEEDED(d.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc.Get(),nullptr,IID_PPV_ARGS(&list))));
        REQUIRE(SUCCEEDED(list->Close()));
        REQUIRE(SUCCEEDED(alloc->Reset()));
        REQUIRE(SUCCEEDED(list->Reset(alloc.Get(),nullptr))); // establish an observed recording generation
        REQUIRE(acquisition.Begin({na::observe::IdOf(depth.Get()),0,32,16,DXGI_FORMAT_D32_FLOAT}));
        if(frame) {
            auto b=test::Transition(depth.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1,&b);
        }
        auto b=test::Transition(depth.Get(),frame?D3D12_RESOURCE_STATE_COPY_SOURCE:D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_DEPTH_WRITE);
        list->ResourceBarrier(1,&b);
        list->ClearDepthStencilView(heap->GetCPUDescriptorHandleForHeapStart(),D3D12_CLEAR_FLAG_DEPTH,frame?0.45f:0.4f,0,0,nullptr);
        REQUIRE(SUCCEEDED(list->Close())); ID3D12CommandList* lists[]={list.Get()};
        d.queue->ExecuteCommandLists(1,lists);
        std::vector<na::discovery::AcquiredSample> samples;
        for(unsigned retry=0;retry<1000 && samples.empty();++retry) { samples=acquisition.Pump(); if(samples.empty()) Sleep(5); }
        REQUIRE(samples.size()==1);
        const auto& s=samples[0];
        std::printf("acquired frame %u: status %u host_status %u reason %u format %u queue %llu ticket %llu bytes %llu\n",frame,
            static_cast<unsigned>(s.status),s.info.status,s.info.reason,s.info.format,static_cast<unsigned long long>(s.info.queue),
            static_cast<unsigned long long>(s.info.queue_ticket),static_cast<unsigned long long>(s.info.data_bytes));
        REQUIRE(s.status==na::discovery::AcquisitionStatus::Ready);
        // This fixture owns both submissions and cameras, so their association
        // is known. Live host code must establish this, not infer it from time.
        na::classifier::CameraAssociation camera{};
        camera.session=scope.session;camera.epoch=scope.epoch;camera.queue=s.info.queue;camera.ticket=s.info.queue_ticket;
        camera.frame=frame+1;camera.qualified=camera.complete=true;
        camera.view_projection={{1,0,0,frame?-0.0625:0,0,1,0,0,0,0,1,frame?0.05:0,0,0,0,1}};
        REQUIRE(na::classifier::AcquireDepthFrame(s,camera,frames[frame]));
        d.WaitIdle(); // test-only allocator lifetime, never in production callback
    }
    const auto report=na::classifier::CheckTemporalDepth(frames[0],frames[1]);
    std::printf("temporal GPU depth: checked %zu inliers %zu excluded %zu verdict %u\n",
        report.checked,report.inliers,report.excluded,static_cast<unsigned>(report.verdict));
    CHECK(report.verdict==na::classifier::TemporalVerdict::Consistent && report.checked==496);
    CHECK(d.device->GetDeviceRemovedReason()==S_OK);
    return test::finish("sample_acquisition_host");
}
