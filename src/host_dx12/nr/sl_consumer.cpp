#include "host_dx12/nr/sl_consumer.hpp"
#ifdef NA_HAVE_NGX_SDK
#include "host_dx12/nr/native_fg.hpp"
#include "host_dx12/nr/fg_contract.hpp"
#include "host_dx12/nr/fg_states.hpp"
#include "host_dx12/sl_boundary/sl_boundary.hpp"
#include "host_dx12/sl_boundary/sl_tags.hpp"
#include "host_dx12/overlay/overlay.hpp"
#include "host_dx12/color/color_space.hpp"
#include "host_dx12/settings.hpp"
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <vector>

namespace na::nr::slconsumer {
namespace {
using Microsoft::WRL::ComPtr;
constexpr auto Read=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
struct Image { ComPtr<ID3D12Resource> texture; FgRegion region; };
struct Snapshot {
    std::shared_ptr<Image> image;
    native::RecordingLease producer;
    ComPtr<ID3D12Resource> deferred;
    slb::Tag tag;
    uint64_t sequence=0;
};
struct Tags { uint32_t token=0,viewport=0; std::array<Snapshot,5> inputs; slb::Tag backbuffer;uint64_t backbuffer_sequence=0; };
struct Staged { uint64_t sequence=0; Tags tags; };
struct Work {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    std::vector<std::shared_ptr<Image>> images;
};
struct Flight { std::shared_ptr<Work> work; native::RecordingLease lease; };
struct Stream {
    uint32_t proxy=0;uint64_t epoch=0,last_serial=0;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
};
std::mutex mutex;
Runtime runtime;
std::vector<Tags> frames;
std::vector<Staged> staged;
std::vector<Flight> flights;
std::vector<std::unique_ptr<Stream>> streams;
std::array<std::atomic<ID3D12Device*>,8> claimed{};
int Index(uint32_t type){
    switch(type){case slb::abi::kBufferHudlessColor:return 0;case slb::abi::kBufferDepth:return 1;
    case slb::abi::kBufferMotionVectors:return 2;case slb::abi::kBufferUiAlpha:return 3;
    case slb::abi::kBufferUiColorAndAlpha:return 4;default:return -1;}
}
void Barrier(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){
    if(before==after)return;
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};list->ResourceBarrier(1,&b);
}
std::shared_ptr<Image> Copy(ID3D12GraphicsCommandList* list,const slb::Tag& tag){
    if(!list || !tag.resource || !tag.resource_decoded || tag.resource_type || tag.state==UINT32_MAX)return {};
    if(!states::CanTransitionOn(list->GetType(),static_cast<D3D12_RESOURCE_STATES>(tag.state)))return {};
    auto desc=tag.resource->GetDesc();
    if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize!=1 || desc.MipLevels!=1 || desc.SampleDesc.Count!=1 || desc.Width>UINT32_MAX)return {};
    const auto x=tag.extent_left,y=tag.extent_top;
    if(x>=desc.Width || y>=desc.Height)return {};
    const uint32_t w=tag.extent_width?tag.extent_width:static_cast<uint32_t>(desc.Width)-x;
    const uint32_t h=tag.extent_height?tag.extent_height:desc.Height-y;
    if(!w || !h || w>desc.Width-x || h>desc.Height-y)return {};
    ComPtr<ID3D12Device> device;if(FAILED(list->GetDevice(IID_PPV_ARGS(&device))))return {};
    auto image=std::make_shared<Image>();image->region={x,y,w,h,15};
    // Preserve depth format/planes while removing the game's deny-SRV flag.
    desc.Flags=static_cast<D3D12_RESOURCE_FLAGS>(desc.Flags&D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&image->texture))))return {};
    Barrier(list,tag.resource,static_cast<D3D12_RESOURCE_STATES>(tag.state),D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(image->texture.Get(),tag.resource);
    Barrier(list,tag.resource,D3D12_RESOURCE_STATE_COPY_SOURCE,static_cast<D3D12_RESOURCE_STATES>(tag.state));
    Barrier(list,image->texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,Read);
    return image;
}
void OnTag(const slb::TagEvent& e,void*){
    if(!e.decoded || !e.has_frame || !e.has_viewport || (!settings::EffectsReady() || !settings::Get().nr_enabled.load()))return;
    std::lock_guard<std::mutex> lock(mutex);
    if(!runtime.create_feature || staged.size()>=32)return;
    Staged next;next.sequence=e.sequence;next.tags.token=e.frame_token;next.tags.viewport=e.viewport;
    for(uint32_t i=0;i<e.count;++i){
        const auto& tag=e.tags[i];
        if(tag.buffer_type==slb::abi::kBufferBackbuffer){next.tags.backbuffer=tag;next.tags.backbuffer_sequence=e.sequence;continue;}
        const int index=Index(tag.buffer_type);if(index<0)continue;
        auto& snapshot=next.tags.inputs[index];snapshot.sequence=e.sequence;snapshot.tag=tag;
        if(!tag.resource)continue; // explicit untag invalidates an older input
        if(tag.lifecycle==slb::abi::kValidUntilPresent && tag.resource_decoded){snapshot.deferred=tag.resource;}
        else if(e.command_list){
            // Register lifetime BEFORE commands are emitted. Failed allocation
            // merely leaves an empty owner; a rejected original tag still owns
            // any commands recorded before its result became known.
            auto owner=std::make_shared<std::vector<std::shared_ptr<Image>>>();
            snapshot.producer=native::RetainRecording(e.command_list,owner);
            if(!snapshot.producer)continue;
            snapshot.image=Copy(e.command_list,tag);if(snapshot.image)owner->push_back(snapshot.image);
        }
    }
    staged.push_back(std::move(next));
}
void AfterTag(const slb::TagEvent& e,int32_t result,void*){
    std::lock_guard<std::mutex> lock(mutex);
    auto pending=std::find_if(staged.begin(),staged.end(),[&](const auto& s){return s.sequence==e.sequence;});
    if(pending==staged.end())return;
    if(result==0){
        auto frame=std::find_if(frames.begin(),frames.end(),[&](const auto& f){return f.token==e.frame_token && f.viewport==e.viewport;});
        if(frame==frames.end()){
            if(frames.size()>=16)frames.erase(frames.begin());
            Tags fresh;fresh.token=e.frame_token;fresh.viewport=e.viewport;frames.push_back(std::move(fresh));frame=frames.end()-1;
        }
        if(pending->tags.backbuffer_sequence>frame->backbuffer_sequence){frame->backbuffer=pending->tags.backbuffer;frame->backbuffer_sequence=pending->tags.backbuffer_sequence;}
        for(size_t i=0;i<frame->inputs.size();++i)if(pending->tags.inputs[i].sequence>frame->inputs[i].sequence)
            frame->inputs[i]=std::move(pending->tags.inputs[i]);
    }
    staged.erase(pending);
}
void Consume(const slb::PresentEvent& e,ID3D12CommandQueue* bound_queue,DXGI_COLOR_SPACE_TYPE color_space){
    if(!settings::EffectsReady() || e.kind==slb::PresentKind::Test || !e.game_facing || !e.base)return;
    std::lock_guard<std::mutex> lock(mutex);
    native::Maintain();
    flights.erase(std::remove_if(flights.begin(),flights.end(),[](const auto& f){return f.lease.Retired();}),flights.end());
    Stream* existing=nullptr;for(auto& s:streams)if(s->proxy==e.proxy_id)existing=s.get();
    if(existing && existing->last_serial==e.real_serial && existing->epoch==e.stream_epoch)return;
    if(existing && (!settings::EffectsReady() || !settings::Get().nr_enabled.load())){
        native::Release(runtime.create_feature,existing);existing->last_serial=e.real_serial;
        native::ReportUnavailable("Neural Rendering is off for this real frame.");return;
    }
    if(!runtime.create_feature)return;
    if(e.association!=slb::Association::Unique){if(existing)native::ReportUnavailable("Neural Rendering is waiting for this frame's Streamline tags.");return;}
    auto input=std::find_if(frames.begin(),frames.end(),[&](const auto& f){return f.token==e.frame_token && f.viewport==e.viewport;});
    if(input==frames.end()){if(existing)native::ReportUnavailable("Neural Rendering is waiting for this real frame's inputs.");return;}
    ComPtr<IDXGISwapChain3> chain;if(FAILED(e.game_facing->QueryInterface(IID_PPV_ARGS(&chain))))return;
    ComPtr<ID3D12Resource> backbuffer;
    if(FAILED(chain->GetBuffer(chain->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&backbuffer))))return;
    ComPtr<ID3D12Device> device;if(FAILED(backbuffer->GetDevice(IID_PPV_ARGS(&device))))return;
    ComPtr<ID3D12CommandQueue> queue=bound_queue;
    Stream* stream=nullptr;for(auto& s:streams)if(s->proxy==e.proxy_id)stream=s.get();
    if(!stream){
        if(streams.size()>=claimed.size())return;
        auto s=std::make_unique<Stream>();s->proxy=e.proxy_id;s->device=device;s->queue=queue;
        claimed[streams.size()].store(device.Get());stream=s.get();streams.push_back(std::move(s));
    }
    if(!queue || queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT){native::ReportUnavailable("Neural Rendering needs the game-facing presentation queue.");return;}
    // A stream keeps this ownership across native FG suspension and user NR
    // toggles. The lower FG/base-Present paths must not enhance it a second time.
    if(stream->epoch!=e.stream_epoch || stream->device.Get()!=device.Get()){
        native::Release(runtime.create_feature,stream);stream->epoch=e.stream_epoch;
        stream->last_serial=0;stream->device=device;stream->queue=queue;
        for(size_t i=0;i<streams.size();++i)claimed[i].store(streams[i]->device.Get());
    }
    if(stream->last_serial==e.real_serial)return; // DXGI nonblocking retry, already submitted
    if((!settings::EffectsReady() || !settings::Get().nr_enabled.load())){native::Release(runtime.create_feature,stream);return;}
    // Temporal history was recorded on this queue. Changing to another queue
    // cannot implicitly order its in-flight evaluations.
    if(stream->queue.Get()!=queue.Get()){
        if(std::any_of(flights.begin(),flights.end(),[](const auto& f){return !f.lease.Retired();})){native::ReportUnavailable("Neural Rendering is retiring its previous game queue.");return;}
        native::Release(runtime.create_feature,stream);stream->queue=queue;
    }
    slb::ConstantsRecord constants;
    if(!slb::FindConstants(e.frame_token,e.viewport,&constants)){native::ReportUnavailable("Neural Rendering needs constants for this exact frame.");return;}
    const auto& camera=constants.camera;
    if(camera.camera_motion!=1 || camera.jittered!=0 || camera.inverted>1 || camera.reset>1){native::ReportUnavailable("Neural Rendering needs unjittered camera-inclusive motion.");return;}
    for(size_t i=0;i<3;++i){const auto& s=input->inputs[i];
        if(!s.image && !s.deferred){native::ReportUnavailable("Neural Rendering is waiting for preserved Streamline inputs.");return;}
        if(s.image && !s.producer.Ready(queue.Get())){native::ReportUnavailable("Neural Rendering is waiting for this frame's input submission.");return;}
    }
    if(flights.size()>=32){native::ReportUnavailable("Neural Rendering is waiting for submitted work to finish.");return;}
    auto work=std::make_shared<Work>();
    if(FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&work->allocator))) ||
       FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,work->allocator.Get(),nullptr,IID_PPV_ARGS(&work->list))))return;
    auto receipt=native::RetainRecording(work->list.Get(),work);if(!receipt)return;
    std::array<std::shared_ptr<Image>,5> images;
    for(size_t i=0;i<images.size();++i){auto& s=input->inputs[i];
        if(s.image && s.producer.Ready(queue.Get()))images[i]=s.image;
        else if(s.deferred)images[i]=Copy(work->list.Get(),s.tag);
        if(images[i])work->images.push_back(images[i]);
    }
    bool ok=images[0] && images[1] && images[2];
    const auto desc=backbuffer->GetDesc();
    FgContract f;f.color={backbuffer.Get(),{0,0,static_cast<uint32_t>(desc.Width),desc.Height,15}};
    if(input->backbuffer_sequence){const auto& tag=input->backbuffer;
        if(tag.resource && tag.resource!=backbuffer.Get())ok=false;
        if(tag.extent_width && tag.extent_height)f.color.region={tag.extent_left,tag.extent_top,tag.extent_width,tag.extent_height,15};
    }
    auto surface=[](const std::shared_ptr<Image>& image){return image?FgSurface{image->texture.Get(),image->region}:FgSurface{};};
    f.hudless=surface(images[0]);f.depth=surface(images[1]);f.motion=surface(images[2]);
    f.alpha=surface(images[3]);f.ui=surface(images[4]);
    f.frame_id=e.real_serial;f.has_frame_id=true;f.has_hdr=true;
    f.hdr=color::IsHdr(color::Classify(desc.Format,color_space));
    f.camera_motion=camera.camera_motion;f.motion_jittered=camera.jittered;
    f.depth_inverted=camera.inverted;f.reset=camera.reset;
    f.has_motion_x=f.has_motion_y=true;f.motion_x=camera.motion_scale[0];f.motion_y=camera.motion_scale[1];
    native::FrameStates states;for(auto& state:states.values)state=Read;
    states.values[0]=D3D12_RESOURCE_STATE_PRESENT;states.preserve_additive_ui=true;
    native::PrepareDevice(device.Get());
    if(!ok)native::ReportUnavailable("Neural Rendering could not preserve this frame's input regions.");
    if(ok)ok=native::RecordFrame(work->list.Get(),runtime.create_feature,stream,f,runtime,states);
    if(ok){
        slb::Tag tag{};tag.resource_decoded=true;tag.resource=f.hudless.resource;tag.buffer_type=slb::abi::kBufferHudlessColor;
        tag.state=Read;tag.lifecycle=slb::abi::kOnlyValidNow;tag.width=static_cast<uint32_t>(tag.resource->GetDesc().Width);
        tag.height=tag.resource->GetDesc().Height;tag.native_format=tag.resource->GetDesc().Format;
        tag.extent_left=f.hudless.region.x;tag.extent_top=f.hudless.region.y;
        tag.extent_width=f.hudless.region.width;tag.extent_height=f.hudless.region.height;
        ok=slb::RetagForFrame(e.frame_token,e.viewport,&tag,1,work->list.Get())==0;
        if(!ok)native::ReportUnavailable("Streamline did not accept the enhanced HUD-free image.");
    }
    if(FAILED(work->list->Close())){ok=false;native::ReportUnavailable("Neural Rendering could not close its command list.");}
    if(ok){
        ID3D12CommandList* lists[]={work->list.Get()};
        // The production queue hook also observes this call; Submission's
        // recursion guard ensures the receipt is published only once.
        {native::Submission submission(queue.Get(),1,lists);queue->ExecuteCommandLists(1,lists);}
        stream->last_serial=e.real_serial;
    } else {const auto failure=native::GetStatus();native::After(runtime.create_feature,stream,-1);native::ReportUnavailable(failure.reason);}
    // End executable ownership without resetting an allocator still in flight.
    native::Reset(work->list.Get());flights.push_back({work,std::move(receipt)});
    // Deferred references are valid only through this Present. Owned snapshots
    // and recorded payloads have independent GPU lifetimes.
    frames.erase(input);
}
void BeforePresent(const slb::PresentEvent& e,void*){
    if(!e.base)return;
    ComPtr<ID3D12CommandQueue> queue=static_cast<ID3D12CommandQueue*>(e.present_queue);
    if(!queue)queue.Attach(overlay::AcquirePresentQueue(e.game_facing));
    Consume(e,queue.Get(),static_cast<DXGI_COLOR_SPACE_TYPE>(overlay::SwapChainColorSpace(e.base)));
}
void AfterPresent(const slb::PresentEvent& e,long result,void*){
    std::lock_guard<std::mutex> lock(mutex);
    for(auto& frame:frames)for(auto& input:frame.inputs)
        if(input.sequence<=e.last_tag_sequence)input.deferred.Reset();
    if(SUCCEEDED(result) || result==DXGI_ERROR_WAS_STILL_DRAWING)return;
    for(auto& s:streams)if(s->proxy==e.proxy_id)native::After(runtime.create_feature,s.get(),-1);
}
void Released(uint32_t id,void*){
    std::lock_guard<std::mutex> lock(mutex);
    for(auto it=streams.begin();it!=streams.end();++it)if((*it)->proxy==id){
        native::Release(runtime.create_feature,it->get());streams.erase(it);break;
    }
    for(size_t i=0;i<claimed.size();++i)claimed[i].store(i<streams.size()?streams[i]->device.Get():nullptr);
}

}
void ForgetStream(uint32_t id){Released(id,nullptr);}
void ConsumeForTests(const slb::PresentEvent& e,ID3D12CommandQueue* queue,DXGI_COLOR_SPACE_TYPE cs){Consume(e,queue,cs);}
bool Start(){slb::Callbacks callbacks;callbacks.on_tag=OnTag;callbacks.after_tag=AfterTag;callbacks.before_present=BeforePresent;callbacks.after_present=AfterPresent;callbacks.stream_released=Released;return slb::SetCallbacks(callbacks);}
void SetRuntime(const Runtime& rt){std::lock_guard<std::mutex> lock(mutex);runtime=rt;}
void Shutdown(const Runtime& rt){
    std::lock_guard<std::mutex> lock(mutex);native::CoreShutdown(rt.create_feature);
    if(runtime.create_feature!=rt.create_feature)return;
    runtime={};frames.clear();staged.clear();streams.clear();for(auto& device:claimed)device.store(nullptr);
}
bool OwnsAny(){for(auto& entry:claimed)if(entry.load())return true;return false;}
bool OwnsDevice(ID3D12Device* device){for(auto& entry:claimed)if(entry.load()==device && device)return true;return false;}
}
#else
namespace na::nr::slconsumer {
void ConsumeForTests(const slb::PresentEvent&,ID3D12CommandQueue*,DXGI_COLOR_SPACE_TYPE){}
void ForgetStream(uint32_t){}
bool Start(){return false;}void SetRuntime(const Runtime&){}void Shutdown(const Runtime&){}
bool OwnsAny(){return false;}
bool OwnsDevice(ID3D12Device*){return false;}
}
#endif
