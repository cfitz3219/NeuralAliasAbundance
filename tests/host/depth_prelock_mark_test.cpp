// Astra's regression (COMMS 2026-11-02, reproduced on a real DX12 device against db60492):
// a second thread submitting to the same queue must not mark the known-state store inside
// the first thread's serialized submission, or the carried depth state is reset
// (kResetOverlap / overlapped_submissions). Fixed in e8e6b4f by taking the queue lock
// before capture_submission/known_mark. Test by Astra; committed by Claude.
#include "test_support.hpp"
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>
#include "host_dx12/host_observe.h"
#include "host_dx12/observe/producers.hpp"
static void __cdecl Sink(const na_event_batch*,void*) {}
int main() {
    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("depth_prelock_mark_test");
    auto* dev=d.device.Get();
    REQUIRE(na::observe::InstallProducers(dev,d.queue.Get(),true));
    na_observe_config cfg{}; cfg.size_bytes=sizeof cfg;
    REQUIRE(na_observe_start(&cfg,&Sink,nullptr)==NA_OK);
    D3D12_HEAP_DESC hd{};
    hd.SizeInBytes=1<<20; hd.Properties.Type=D3D12_HEAP_TYPE_DEFAULT;
    hd.Flags=D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;
    test::ComPtr<ID3D12Heap> heap;
    REQUIRE(SUCCEEDED(dev->CreateHeap(&hd,IID_PPV_ARGS(&heap))));
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width=rd.Height=64;
    rd.DepthOrArraySize=rd.MipLevels=1; rd.Format=DXGI_FORMAT_R32_TYPELESS;
    rd.SampleDesc.Count=1; rd.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    test::ComPtr<ID3D12Resource> depth, alias;
    REQUIRE(SUCCEEDED(dev->CreatePlacedResource(heap.Get(),0,&rd,D3D12_RESOURCE_STATE_DEPTH_WRITE,nullptr,IID_PPV_ARGS(&depth))));
    REQUIRE(SUCCEEDED(dev->CreatePlacedResource(heap.Get(),0,&rd,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&alias))));
    const auto id=na::observe::IdOf(depth.Get());
    constexpr auto read=static_cast<D3D12_RESOURCE_STATES>(D3D12_RESOURCE_STATE_DEPTH_READ|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    test::List pass;
    REQUIRE(pass.Create(dev));
    REQUIRE(SUCCEEDED(pass.list->Close()));
    pass.Reset(); // begin a fully observed recording, as the game does after pool creation
    auto transition=test::Transition(depth.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,read);
    pass.list->ResourceBarrier(1,&transition);
    REQUIRE(SUCCEEDED(pass.list->Close()));
    ID3D12CommandList* lists[]={pass.list.Get()};
    d.queue->ExecuteCommandLists(1,lists); d.WaitIdle();
    uint32_t state=0;
    char evidence[256]{};
    na::observe::DescribeStateEvidence(pass.list.Get(),id,0,evidence,sizeof evidence);
    std::printf("depth %llu: %s\n",static_cast<unsigned long long>(id),evidence);
    REQUIRE(na::observe::KnownSubStateNow(id,0,&state));
    CHECK(state==read);

    // CPU observer-publication regression using the production Begin/End functions.
    // Initial resource/transition above really executed on DX12. The two wrappers
    // below isolate publication bookkeeping; they submit no additional GPU work.
    const auto before = na::observe::GetProducerStats();
    const auto a = na::observe::ExecuteBegin(d.queue.Get(), 1, lists);
    REQUIRE(a.submit_locked);
    std::atomic<bool> entered{false}, returned{false};
    std::thread other([&] {
        entered = true;
        const auto b = na::observe::ExecuteBegin(d.queue.Get(), 1, lists);
        returned = true;
        na::observe::ExecuteEnd(b, d.queue.Get(), 1, lists);
    });
    bool premarked = false;
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < limit) {
        na::observe::DescribeStateEvidence(pass.list.Get(), id, 0, evidence, sizeof evidence);
        if (std::strstr(evidence, "marks=2 ")) { premarked = true; break; }
        std::this_thread::yield();
    }
    std::printf("before A ends: Bentered=%d Breturned=%d premarked=%d %s\n",
                int(entered.load()), int(returned.load()), int(premarked), evidence);
    CHECK(!premarked); // the second submission waits for the lock before it marks
    na::observe::ExecuteEnd(a, d.queue.Get(), 1, lists);
    other.join();
    const bool known = na::observe::KnownSubStateNow(id, 0, &state);
    na::observe::DescribeStateEvidence(pass.list.Get(), id, 0, evidence, sizeof evidence);
    const auto after = na::observe::GetProducerStats();
    std::printf("after both serialized wrappers: known=%d ambiguous_delta=%llu serialized_delta=%llu %s\n",
                int(known), (unsigned long long)(after.ambiguous_executes-before.ambiguous_executes),
                (unsigned long long)(after.serialized_submissions-before.serialized_submissions), evidence);
    CHECK(known && state == read); // serialization should preserve the final established state
    na_observe_stop();
    return test::finish("depth_prelock_mark_test");
}
