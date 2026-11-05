// Offline scheduling/lifetime tests. The deliberate waits below are test-only.
#include "test_support.hpp"
#include "host_dx12/background/work.hpp"
#include "host_dx12/observe/producers.hpp"
#include <atomic>
#include <stdexcept>
int main(){
    namespace bg=na::background;
    test::Device d;if(!test::CreateDevice(d))return test::kSkip;
    REQUIRE(na::observe::InstallProducers(d.device.Get(),d.queue.Get(),false));
    const auto observed=na::observe::GetProducerStats().objects_created;
    HANDLE release=CreateEventW(nullptr,TRUE,FALSE,nullptr),started=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    REQUIRE(release&&started);std::atomic<unsigned> running{0},done{0},good_priority{0};
    REQUIRE(bg::Start());
    for(unsigned i=0;i<2;++i)REQUIRE(bg::Submit([&]{
        if(GetThreadPriority(GetCurrentThread())==THREAD_PRIORITY_BELOW_NORMAL)++good_priority;
        if(++running==2)SetEvent(started);
        WaitForSingleObject(release,10000);
        // The actual worker's HostWorkScope excludes our native creations.
        auto b=test::CreateBuffer(d.device.Get(),65536,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON);
        CHECK(b);CHECK(!bg::WaitIdle(1));CHECK(!bg::Stop(1));++done;
    }));
    REQUIRE(WaitForSingleObject(started,10000)==WAIT_OBJECT_0);
    for(unsigned i=0;i<30;++i)REQUIRE(bg::Submit([&]{++done;}));
    CHECK(bg::GetStats().pending==32&&bg::GetStats().active==2);
    CHECK(!bg::Submit([]{}));CHECK(!bg::WaitIdle(1));CHECK(!bg::Stop(1));CHECK(!bg::Submit([]{}));
    SetEvent(release);REQUIRE(bg::Stop(30000));
    auto stats=bg::GetStats();CHECK(done==32&&good_priority==2&&stats.workers==0&&stats.pending==0&&stats.peak_active==2);
    CHECK(na::observe::GetProducerStats().objects_created==observed);
    // An exceptional job doesn't poison the pool or prevent orderly restart/drain.
    REQUIRE(bg::Start());REQUIRE(bg::Submit([]{throw std::runtime_error("test-only failure");}));
    REQUIRE(bg::Submit([&]{++done;}));REQUIRE(bg::WaitIdle(10000));REQUIRE(bg::Stop(10000));
    stats=bg::GetStats();CHECK(stats.failed==1&&stats.accepted==34&&stats.completed==34&&done==33&&stats.workers==0);
    bg::RequestStop();CHECK(!bg::Start());CHECK(!bg::Submit([]{}));
    CloseHandle(release);CloseHandle(started);na::observe::SetProcessExiting();return test::finish("Taskflow background work");
}
