// Test-only real producer -> decoder -> validity integration. No game injection.
#include "../host/test_support.hpp"
#include "providers/discovery/capture_validity.hpp"
#include "host_dx12/observe/producers.hpp"
#include <chrono>
#include <condition_variable>
#include <mutex>
namespace {
struct Capture {
    std::mutex mutex;
    std::condition_variable cv;
    na::discovery::CaptureValidity validity;
    unsigned malformed{},reports{},resources{};
    uint64_t completed_epoch{};
    bool running{};
    ~Capture() { if (running) na_observe_stop(); }
    static void __cdecl Sink(const na_event_batch* b,void* user) {
        auto& c=*static_cast<Capture*>(user);
        std::lock_guard<std::mutex> lock(c.mutex);
        const size_t size=static_cast<size_t>(b->header_bytes)+b->payload_bytes;
        const auto decoded=na::discovery::DecodeBatch(b,size);
        if (!decoded.valid) ++c.malformed;
        na_observe_status status{}; status.size_bytes=sizeof(status);
        if (na_observe_query_status(&status)!=NA_OK) ++c.malformed;
        c.validity.Consume(b,size,status);
        for(const auto& e:decoded.events) {
            if(e.header.kind==NA_EV_CAPABILITY_REPORT) ++c.reports;
            if(e.header.kind==NA_EV_RESOURCE_CREATED) ++c.resources;
            if(e.header.kind==NA_EV_INVENTORY_COMPLETE) c.completed_epoch=e.header.epoch;
        }
        c.cv.notify_all();
    }
    bool AwaitInventory(uint64_t epoch) {
        std::unique_lock<std::mutex> lock(mutex);
        if(!cv.wait_for(lock,std::chrono::seconds(5),[&]{return completed_epoch>=epoch || malformed;})) return false;
        // Host clears inventory_invalid after the sink returns. Poll coherent
        // status on this test control thread, never wait inside a live callback.
        const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(std::chrono::steady_clock::now()<end) {
            na_observe_status s{};s.size_bytes=sizeof(s);
            if(na_observe_query_status(&s)!=NA_OK) return false;
            validity.ObserveStatus(s);
            if(!s.inventory_invalid) return !malformed && validity.InventoryConsistent();
            lock.unlock(); Sleep(1); lock.lock();
        }
        return false;
    }
};
bool CreateWarp(test::Device& d) {
    if(FAILED(CreateDXGIFactory2(0,IID_PPV_ARGS(&d.factory)))) return false;
    test::ComPtr<IDXGIAdapter> adapter;
    if(FAILED(d.factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))) ||
        FAILED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d.device)))) return false;
    D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    if(FAILED(d.device->CreateCommandQueue(&q,IID_PPV_ARGS(&d.queue)))) return false;
    d.warp=true; std::printf("device: WARP (software)\n");return true;
}
}
int main(int argc,char** argv) {
    test::Device d;
    const bool warp=argc>1 && std::strcmp(argv[1],"--warp")==0;
    if(!(warp?CreateWarp(d):test::CreateDevice(d))) return test::SkipNoDevice("capture_consumer");
    auto early=test::CreateBuffer(d.device.Get(),4096,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON);
    REQUIRE(early);
    REQUIRE(na::observe::InstallProducers(d.device.Get(),d.queue.Get(),true));
    Capture capture;
    REQUIRE(na_observe_start(nullptr,&Capture::Sink,&capture)==NA_OK);capture.running=true;
    REQUIRE(capture.AwaitInventory(1));
    {
        std::lock_guard<std::mutex> lock(capture.mutex);
        CHECK(capture.validity.Supports(NA_GROUP_RESOURCE));
        CHECK(!capture.validity.Supports(NA_GROUP_PASS)); // H1 cannot authorize A5 pass evidence.
    }
    auto observed=test::CreateBuffer(d.device.Get(),4096,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON);
    REQUIRE(observed);
    D3D12_DESCRIPTOR_HEAP_DESC desc{};desc.NumDescriptors=1;desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    test::ComPtr<ID3D12DescriptorHeap> heap;
    REQUIRE(SUCCEEDED(d.device->CreateDescriptorHeap(&desc,IID_PPV_ARGS(&heap))));
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_R32_UINT;
    srv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements=1024;
    d.device->CreateShaderResourceView(early.Get(),&srv,heap->GetCPUDescriptorHandleForHeapStart());
    REQUIRE(SUCCEEDED(d.device->GetDeviceRemovedReason()));
    REQUIRE(na_observe_resync()==NA_OK);
    REQUIRE(capture.AwaitInventory(2));
    REQUIRE(na_observe_stop()==NA_OK);capture.running=false;
    CHECK(capture.malformed==0 && capture.reports==2 && capture.resources>=1);
    CHECK(!capture.validity.InventoryConsistent()); // FINAL drains and closes readiness.
    return test::finish("capture_consumer");
}
