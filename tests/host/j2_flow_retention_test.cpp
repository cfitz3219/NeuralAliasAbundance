// GPU completion does not imply that the game's executable recording has been
// retired. Keep many old lists alive, then reuse one active list for >213 frames.
#include "test_support.hpp"
#include "host_dx12/j2/compute_flow.hpp"
#include "host_dx12/ffx_flow/ffx_flow.hpp"
#include "host_dx12/ffx_flow/ffx_flow_shaders.hpp"
#include "host_dx12/observe/producers.hpp"
#include "host_dx12/nr/native_fg.hpp"
#include "core/motion_arbiter_gpu.hpp"
#include <memory>
#include <vector>
#include <d3d12sdklayers.h>

int main()
{
    if(!na::ffx_flow::Built())return test::kSkip;
    test::ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    test::Device d;if(!test::CreateDevice(d))return test::kSkip;
    if(!na::ffx_flow::DeviceSupported(d.device.Get()))return test::kSkip;
    REQUIRE(na::observe::InstallListResetTracking(d.device.Get(),&na::nr::native::Reset));
    test::ComPtr<ID3D12InfoQueue> info;d.device.As(&info);
    constexpr unsigned w=256,h=64;constexpr auto S=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    test::List setup;REQUIRE(setup.Create(d.device.Get()));
    std::vector<test::ComPtr<ID3D12Resource>> uploads;
    auto make=[&](bool depth,unsigned dx=0){
        auto r=test::CreateTexture(d.device.Get(),w,h,depth?DXGI_FORMAT_R32_FLOAT:DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE);
        auto up=test::CreateBuffer(d.device.Get(),w*h*4,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* p=nullptr;up->Map(0,nullptr,&p);
        for(unsigned i=0;i<w*h;++i){unsigned t=((i%w+w-dx)%w)*1664525u+(i/w)*1013904223u;t^=t>>13;t*=2246822519u;t^=t>>16;const unsigned luma=60+t%190;
            unsigned v=depth?0x3f000000u:0xff000000u|luma*0x010101u;std::memcpy(static_cast<char*>(p)+i*4,&v,4);}up->Unmap(0,nullptr);
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=up.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint={depth?DXGI_FORMAT_R32_FLOAT:DXGI_FORMAT_R8G8B8A8_UNORM,w,h,1,w*4};dst.pResource=r.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        setup.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);auto b=test::Transition(r.Get(),D3D12_RESOURCE_STATE_COPY_DEST,S);setup.list->ResourceBarrier(1,&b);uploads.push_back(up);return r;
    };
    auto image=make(false),shifted=make(false,8),depth=make(true);REQUIRE(image&&shifted&&depth);REQUIRE(SUCCEEDED(setup.list->Close()));
    ID3D12CommandList* initial[]{setup.list.Get()};d.queue->ExecuteCommandLists(1,initial);d.WaitIdle();
    na::motion::gpu::Inputs camera;camera.depth=depth.Get();camera.tier2_valid_flag=true;
    for(unsigned i=0;i<16;++i)camera.inv_view_proj_current.m[i]=camera.view_proj_previous.m[i]=(i%5==0)?1.f:0.f;
    namespace cf=na::j2::compute_flow;cf::Frame frame;frame.generation=1;
    std::vector<std::unique_ptr<test::List>> held;
    std::vector<test::ComPtr<ID3D12Resource>> results;
    std::vector<std::vector<uint8_t>> baseline;
    auto submit=[&](test::List& list,cf::Ticket& ticket){
        CHECK(SUCCEEDED(list.list->Close()));ID3D12CommandList* ls[]{list.list.Get()};
        {na::nr::native::Submission receipt(d.queue.Get(),1,ls);d.queue->ExecuteCommandLists(1,ls);}d.WaitIdle();cf::Committed(ticket);
    };
    for(unsigned i=0;i<24;++i){
        auto list=std::make_unique<test::List>();REQUIRE(list->Create(d.device.Get()));
        frame.frame=i+1;frame.previous=i;frame.slot=i%6;cf::Tier3 tier;
        auto ticket=cf::Record(list->list.Get(),d.queue.Get(),i%2?shifted.Get():image.Get(),depth.Get(),camera,frame,tier);
        if(!ticket.context){std::printf("STALLED at retained recording %u: %s\n",i+1,cf::GetStats().reason);CHECK(false);return test::finish("j2_flow_retention_test");}
        CHECK(i==0 || tier.residual);
        auto rb=test::CreateBuffer(d.device.Get(),w*h*8,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);REQUIRE(rb);
        if(tier.residual){auto b=test::Transition(tier.residual,S,D3D12_RESOURCE_STATE_COPY_SOURCE);list->list->ResourceBarrier(1,&b);
            D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=tier.residual;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.pResource=rb.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint.Footprint={DXGI_FORMAT_R32G32_FLOAT,w,h,1,w*8};
            list->list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);std::swap(b.Transition.StateBefore,b.Transition.StateAfter);list->list->ResourceBarrier(1,&b);}
        submit(*list,ticket);std::vector<uint8_t> bytes;if(tier.residual){void* data=nullptr;REQUIRE(SUCCEEDED(rb->Map(0,nullptr,&data)));
            bytes.assign(static_cast<uint8_t*>(data),static_cast<uint8_t*>(data)+w*h*8);rb->Unmap(0,nullptr);}
        baseline.push_back(std::move(bytes));results.push_back(rb);held.push_back(std::move(list));
    }
    auto before=cf::GetStats();CHECK(before.recorded==24&&before.submitted==24&&before.pairs==23&&before.busy==0);
    test::List active;REQUIRE(active.Create(d.device.Get()));
    for(unsigned i=0;i<256;++i){
        frame.previous=frame.frame;++frame.frame;frame.slot=i%6;cf::Tier3 tier;
        auto ticket=cf::Record(active.list.Get(),d.queue.Get(),i%2?shifted.Get():image.Get(),depth.Get(),camera,frame,tier);REQUIRE(ticket.context&&tier.residual);
        submit(active,ticket);active.Reset(); // real observed Reset; no manual native::Reset shortcut
    }
    auto after=cf::GetStats();std::printf("retained=24 plus active=256: recorded=%llu submitted=%llu pairs=%llu busy=%llu\n",
        (unsigned long long)after.recorded,(unsigned long long)after.submitted,(unsigned long long)after.pairs,(unsigned long long)after.busy);
    CHECK(after.recorded==280&&after.submitted==280&&after.pairs==279&&after.busy==0);
    CHECK(after.samples>=250&&after.sample_frame==280&&after.submitted_pairs==279);
    CHECK(after.recording_slots<=25&&after.image_slots<=26); // resources plateau and recycle
    CHECK(after.total==w*h&&after.accepted+after.rejected_invalid+after.rejected_small+after.rejected_cost+after.rejected_offscreen+after.rejected_image+after.rejected_flicker==after.total);
    char line[500];cf::DescribeStats(after,line,sizeof line);CHECK(std::strstr(line,"Last completed sample")&&std::strstr(line,"frames behind"));
    CHECK(std::strstr(line,"image motion") && std::strstr(line,"object corrections"));
    CHECK(after.image_moving<=after.total);
    auto stale=after;stale.submit_age_ms=10000;cf::DescribeStats(stale,line,sizeof line);CHECK(std::strstr(line,"no new submission")&&!std::strstr(line,"moving pixels"));
    // Old executable work remains valid after later recordings change bindings.
    for(unsigned i=0;i<held.size();++i){auto& list=held[i];ID3D12CommandList* ls[]{list->list.Get()};{na::nr::native::Submission receipt(d.queue.Get(),1,ls);d.queue->ExecuteCommandLists(1,ls);}d.WaitIdle();
        if(!baseline[i].empty()){void* data=nullptr;REQUIRE(SUCCEEDED(results[i]->Map(0,nullptr,&data)));CHECK(std::memcmp(data,baseline[i].data(),baseline[i].size())==0);results[i]->Unmap(0,nullptr);}}
    held.clear();active.list->Close();active.list.Reset();na::nr::native::Maintain();
    CHECK(cf::GetStats().sample_frame==after.sample_frame); // retiring older samples cannot rewind the UI
    cf::Reset();CHECK(cf::GetStats().samples==0&&cf::GetStats().total==0);
    // A late submission notification from a discarded/reset generation must
    // not resurrect that generation's counters or stale sample.
    test::List pending;REQUIRE(pending.Create(d.device.Get()));frame.previous=0;frame.reset=true;frame.slot=999;++frame.frame;
    cf::Tier3 unused;auto old_ticket=cf::Record(pending.list.Get(),d.queue.Get(),image.Get(),depth.Get(),camera,frame,unused);REQUIRE(old_ticket.context);
    REQUIRE(SUCCEEDED(pending.list->Close()));cf::Reset();cf::Committed(old_ticket);
    CHECK(cf::GetStats().submitted==0&&cf::GetStats().samples==0&&cf::GetStats().total==0);
    pending.Reset();pending.list->Close();na::nr::native::Maintain();
    CHECK(d.device->GetDeviceRemovedReason()==S_OK);
    if(info)for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T n=0;info->GetMessage(i,nullptr,&n);std::vector<uint8_t> data(n);
        auto* message=reinterpret_cast<D3D12_MESSAGE*>(data.data());if(SUCCEEDED(info->GetMessage(i,message,&n))&&message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){
            std::printf("D3D12 error: %s\n",message->pDescription);CHECK(false);}}
    return test::finish("j2_flow_retention_test");
}
