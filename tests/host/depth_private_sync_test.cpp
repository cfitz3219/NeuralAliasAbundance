// Real DX12 reproduction of our private completion Signal invalidating the
// enclosing game's carried depth state. Offline fixture, waits/readback allowed.
#include "test_support.hpp"
#include "host_dx12/host_observe.h"
#include "host_dx12/overlay/submission_tracking.hpp"
#include <cstdio>
static void __cdecl Sink(const na_event_batch*,void*) {}
int main(){
    test::Device d;
    if(!test::CreateDevice(d))return test::SkipNoDevice("depth_private_sync_test");
    auto* dev=d.device.Get();auto* q=d.queue.Get();
    REQUIRE(na::observe::InstallProducers(dev,q,false));
    test::ComPtr<ID3D12CommandQueue> other_queue;
    D3D12_COMMAND_QUEUE_DESC desc{};desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    REQUIRE(SUCCEEDED(dev->CreateCommandQueue(&desc,IID_PPV_ARGS(&other_queue))));
    REQUIRE(na::observe::InstallProducers(dev,other_queue.Get(),false));
    na_observe_config cfg{};cfg.size_bytes=sizeof cfg;
    REQUIRE(na_observe_start(&cfg,&Sink,nullptr)==NA_OK);
    auto depth=test::CreateTexture(dev,64,64,DXGI_FORMAT_R32_TYPELESS,D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    REQUIRE(depth);
    const auto id=na::observe::IdOf(depth.Get());
    test::ComPtr<ID3D12Fence> private_fence;
    REQUIRE(SUCCEEDED(dev->CreateFence(1,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&private_fence))));
    na::gputime::SetDevice(dev);na::gputime::SetEnabled(true);
    test::List pass;REQUIRE(pass.Create(dev));REQUIRE(SUCCEEDED(pass.list->Close()));
    constexpr auto read=D3D12_RESOURCE_STATE_DEPTH_READ;
    auto state=D3D12_RESOURCE_STATE_DEPTH_WRITE;
    // 0 reproduces the old unscoped GPU-timing notification; 1 is the fixed
    // actual production helper; 2 adds a host wait; 3 proves genuine nested
    // game work still invalidates evidence instead of being silently ignored.
    for(int mode=0;mode<4;++mode){
        pass.Reset();const auto next=state==read?D3D12_RESOURCE_STATE_DEPTH_WRITE:read;
        auto b=test::Transition(depth.Get(),state,next);pass.list->ResourceBarrier(1,&b);state=next;
        {const na::observe::HostWorkScope host;na::gputime::Begin(pass.list.Get());na::gputime::End(pass.list.Get());}
        REQUIRE(SUCCEEDED(pass.list->Close()));ID3D12CommandList* lists[]={pass.list.Get()};
        const auto before=na::observe::GetProducerStats();
        const auto t=na::observe::ExecuteBegin(q,1,lists);
        if(mode==2){const na::observe::HostWorkScope host;REQUIRE(SUCCEEDED(q->Wait(private_fence.Get(),1)));}
        if(mode==3)REQUIRE(SUCCEEDED(q->Signal(private_fence.Get(),1)));
        q->ExecuteCommandLists(1,lists);
        if(mode==0)na::gputime::OnExecuted(q,1,lists);
        else na::overlay::AfterGameSubmission(q,1,lists);
        na::observe::ExecuteEnd(t,q,1,lists);
        d.WaitIdle();na::gputime::Poll(q);
        uint32_t observed=0;const bool known=na::observe::KnownSubStateNow(id,0,&observed);
        const auto after=na::observe::GetProducerStats();
        char evidence[256]{};na::observe::DescribeStateEvidence(nullptr,id,0,evidence,sizeof evidence);
        std::printf("mode=%d known=%d ambiguous_delta=%llu %s\n",mode,int(known),
            (unsigned long long)(after.ambiguous_executes-before.ambiguous_executes),evidence);
        CHECK(known==(mode==1 || mode==2));
        if(known)CHECK(observed==uint32_t(state));
        CHECK(na::observe::KnownSubStateOnQueueNow(q,id,0,&observed)==known);
        CHECK(!na::observe::KnownSubStateOnQueueNow(other_queue.Get(),id,0,&observed));
        CHECK((after.ambiguous_executes-before.ambiguous_executes)==uint64_t(mode==0 || mode==3));
    }
    CHECK(na::gputime::GetStats().samples==4);
    CHECK(dev->GetDeviceRemovedReason()==S_OK);
    na_observe_stop();
    // Match the production DLL's existing exit path: timing retains COM
    // objects until CRT teardown, after the observer registry may be destroyed.
    na::observe::SetProcessExiting();
    return test::finish("depth_private_sync_test");
}
