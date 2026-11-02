#include "auto_scene.hpp"
#include "native_bindings.hpp"
#include "scoped_transfers.hpp"
#include "scene_frame.hpp"
#include "host_dx12/observe/producers.hpp"
#include "host_dx12/settings.hpp"
#include "host_dx12/dlss/dlss_backend.hpp"
#include "host_dx12/scale/settings_read_hook.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>
#include <cmath>
#include <tuple>
#include <deque>
#include <unordered_set>

namespace na::scale {
namespace {
using Microsoft::WRL::ComPtr;
using na::observe::NativeOperation;
using na::observe::TextureCall;
using na::observe::TextureKind;
uint64_t hash_bytes(uint64_t h,const void* data,size_t bytes){const auto* p=static_cast<const unsigned char*>(data);while(bytes--){h^=*p++;h*=1099511628211ull;}return h;}
template<class T>void hash_value(uint64_t& h,const T& value){h=hash_bytes(h,&value,sizeof value);}
uint64_t signature(const NativeOperation& op){
    uint64_t h=1469598103934665603ull;hash_value(h,op.pipeline_id);hash_value(h,op.compute);
    for(const auto& d:op.descriptors){hash_value(h,d.view.kind);hash_value(h,d.view.resource);hash_value(h,d.view.counter);hash_value(h,d.view.explicit_view);
        if(d.view.kind==NA_DESC_CBV)hash_value(h,d.view.cbv);else if(d.view.kind==NA_DESC_SRV)hash_value(h,d.view.srv);else hash_value(h,d.view.uav);}
    for(const auto& d:op.colors){hash_value(h,d.view.resource);hash_value(h,d.view.rtv);}
    hash_value(h,op.depth.view.resource);hash_value(h,op.depth.view.dsv);
    for(const auto& c:op.constants){hash_value(h,c.param);hash_value(h,c.descriptor);hash_value(h,c.resource_id);hash_value(h,c.offset);}
    for(const auto& v:op.viewports)hash_value(h,v);for(const auto& s:op.scissors)hash_value(h,s);
    return h;
}
struct TransferKey {
    TextureKind kind{};uint64_t source=0,destination=0;DXGI_FORMAT format{};
    bool operator<(const TransferKey& b)const{return std::tie(kind,source,destination,format)<std::tie(b.kind,b.source,b.destination,b.format);}
};
TransferKey transfer_key(const TextureCall& c){return {c.kind,c.source?na::observe::IdOf(c.source):0,
    c.destination?na::observe::IdOf(c.destination):c.target_id,c.format};}
struct MemberSeed {uint64_t id=0;ComPtr<ID3D12Resource> resource;Role role{};};
struct Built {
    std::shared_ptr<const SceneGroup> group;
    std::shared_ptr<SceneFrame> prototype;
    std::unordered_map<uint64_t,std::shared_ptr<const PreparedBindings>> bindings;
    std::map<TransferKey,std::shared_ptr<const PreparedTransfer>> transfers;
    std::unordered_map<uint64_t,std::string> failed;
    std::map<TransferKey,std::string> failed_transfers;
    uint64_t model=0,group_revision=0;int mode=0;bool optimal=false,optimal_available=false;
};
struct Run {
    std::shared_ptr<Built> built;std::shared_ptr<SceneFrame> frame;
    uint64_t epoch=0,color=0,depth=0;bool aborted=false,seeded=false;
};
struct Bound {std::shared_ptr<Run> run;na::nr::native::RecordingLease lease;};
struct Submission {std::shared_ptr<Run> run;ComPtr<ID3D12CommandQueue> queue;std::vector<ID3D12CommandList*> lists;};
thread_local std::shared_ptr<Submission> submission,reporting;

class Automatic final:public BindingProvider,public na::observe::TextureOperationHook,public std::enable_shared_from_this<Automatic>{
public:
    std::mutex mutex;bool enabled=false;uint64_t epoch=1,color=0,depth=0,model=0,next_generation=1;
    std::map<uint64_t,MemberSeed> members;
    std::map<uint64_t,std::pair<uint64_t,ComPtr<ID3D12Resource>>> copied_from;
    std::unordered_map<uint64_t,std::shared_ptr<const NativeOperation>> observed;
    std::unordered_map<uint64_t,uint64_t> last_seen;
    std::deque<uint64_t> pending;
    std::unordered_set<uint64_t> queued;
    std::atomic<bool> work_pending{false};
    uint64_t group_revision=1,worker_revision=0,failed_group_revision=0,last_novel_epoch=0;
    int worker_mode=-1,failed_group_mode=-1;
    bool worker_optimal=false,failed_group_optimal=false;
    std::map<TransferKey,TransferSpec> observed_transfers;
    std::shared_ptr<Built> built,prepared;std::shared_ptr<Run> current;
    std::map<uint64_t,uint64_t> list_queues;
    std::map<std::pair<uint64_t,uint64_t>,Bound> bound;
    AutomaticSceneStats stats;ComPtr<ID3D12CommandQueue> execution_queue;
    void note(const char* text){std::snprintf(stats.reason,sizeof stats.reason,"%s",text);}
    bool selected(uint64_t c,uint64_t d){return c&&d&&color==c&&depth==d;}
    void select(uint64_t c,uint64_t d){
        if(selected(c,d))return;color=c;depth=d;members.clear();copied_from.clear();observed.clear();last_seen.clear();observed_transfers.clear();built.reset();current.reset();++model;
        prepared.reset();execution_queue.Reset();list_queues.clear();pending.clear();queued.clear();++group_revision;work_pending.store(true);note("Identifying the scene's rendering operations.");
    }
    bool member(uint64_t id,ID3D12Resource* resource,Role role){
        if(!id||!resource||members.count(id))return false;
        const auto d=resource->GetDesc();if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||d.MipLevels!=1||d.DepthOrArraySize!=1||d.SampleDesc.Count!=1)return false;
        MemberSeed value;value.id=id;value.resource=resource;value.role=role;members.emplace(id,std::move(value));++model;++group_revision;work_pending.store(true);return true;
    }
    void dependency(uint64_t id){
        std::vector<uint64_t> chain;
        while(!members.count(id)&&chain.size()<32){
            if(std::find(chain.begin(),chain.end(),id)!=chain.end())return;
            const auto copy=copied_from.find(id);if(copy==copied_from.end())return;chain.push_back(id);id=copy->second.first;
        }
        if(!members.count(id))return;const auto role=members.at(id).role==Role::Depth?Role::Depth:Role::Intermediate;
        for(auto it=chain.rbegin();it!=chain.rend();++it)member(*it,copied_from.at(*it).second.Get(),role);
    }
    bool bind(ID3D12GraphicsCommandList* list,const std::shared_ptr<Run>& run){
        const auto key=std::make_pair(na::observe::IdOf(list),na::observe::ListRecording(list));if(!key.first||!key.second)return false;
        auto found=bound.find(key);if(found!=bound.end())return found->second.run==run;
        for(auto i=bound.begin();i!=bound.end();)if(i->second.lease.Retired())i=bound.erase(i);else++i;
        if(bound.size()>=1024)return false;
        auto lease=na::nr::native::RetainRecording(list,run);if(!lease)return false;
        bound.emplace(key,Bound{run,std::move(lease)});return true;
    }
    std::shared_ptr<Run> run_for(ID3D12GraphicsCommandList* list){
        const auto key=std::make_pair(na::observe::IdOf(list),na::observe::ListRecording(list));
        const auto found=bound.find(key);if(found!=bound.end())return found->second.run;
        if(!built||!built->failed.empty()||!built->failed_transfers.empty()||built->bindings.empty()||!na::j2::Enabled()||na::j2::Paused())return {};
        const auto q=list_queues.find(key.first);
        if(!execution_queue||q==list_queues.end()||q->second!=na::observe::IdOf(execution_queue.Get()))return {};
        if(!current||current->epoch!=epoch){
            if(built->mode!=static_cast<int>(na::settings::Get().j2_scale_mode.load()))return {};
            current=std::make_shared<Run>();current->built=built;current->frame=built->prototype->NewFrame();current->epoch=epoch;current->color=color;current->depth=depth;++stats.frames;
        }
        if(!bind(list,current))return {};return current;
    }
    std::vector<FrameMemberState> states(ID3D12GraphicsCommandList* list,const std::shared_ptr<Run>& run,bool seed=false){
        std::vector<FrameMemberState> result;
        for(const auto& m:run->built->group->Members()){
            uint32_t state=0;const UINT sub=seed&&m.role==Role::Depth&&m.native_desc.Format==DXGI_FORMAT_R32G8X24_TYPELESS?1:0;
            const auto lookup=na::observe::ListStateSub(list,m.id,sub,&state);
            if(lookup==na::observe::StateLookup::Known||(lookup==na::observe::StateLookup::Absent&&na::observe::KnownSubStateNow(m.id,sub,&state)))
                result.push_back({m.id,static_cast<D3D12_RESOURCE_STATES>(state)});
        }
        return result;
    }
    bool checkpoint(ID3D12GraphicsCommandList* list,const std::shared_ptr<Run>& run){
        if(!run||run->aborted)return true;
        if(!run->frame->HasPending(list))return true;
        if(!run->frame->Checkpoint(list,states(list,run)))return false;
        ++stats.checkpoints;return true;
    }
    void abort(ID3D12GraphicsCommandList* list,const std::shared_ptr<Run>& run,const char* why){
        if(run&&!run->aborted){checkpoint(list,run);run->aborted=true;}stats.active=false;++stats.misses;note(why);
    }
    std::shared_ptr<const PreparedBindings> Acquire(ID3D12GraphicsCommandList* list,const na::observe::ShaderCall& call)noexcept override{
        if(na::settings::Get().j2_scale_mode.load()==0)return {};
        uint64_t d=0,c=0;na::observe::GetLiveSelection(&d,&c);if(!c||!d)return {};
        NativeOperation op;const bool have=na::observe::ListNativeOperation(list,call.kind==na::observe::ShaderKind::Dispatch,op);
        std::lock_guard<std::mutex> lock(mutex);if(!enabled)return {};select(c,d);++stats.operations;
        auto run=run_for(list);
        if(!have){if(run)abort(list,run,"An operation's native bindings are not yet available.");return {};}
        bool scene=!op.compute&&op.depth.view.resource==d;
        if(!scene&&!op.compute)for(const auto& rt:op.colors)scene|=rt.view.resource==c;
        bool reads_depth=false,buffer_outputs=true,has_buffer_output=false;
        for(const auto& v:op.descriptors){reads_depth|=v.view.kind==NA_DESC_SRV&&v.view.resource==d;
            if(v.view.kind==NA_DESC_UAV&&v.resource){const bool buffer=v.resource->GetDesc().Dimension==D3D12_RESOURCE_DIMENSION_BUFFER;buffer_outputs&=buffer;has_buffer_output|=buffer;}}
        scene|=op.compute&&reads_depth&&buffer_outputs&&has_buffer_output;
        if(!scene){if(run&&!checkpoint(list,run))abort(list,run,"A native consumer could not restore its current scene.");return {};}
        ComPtr<ID3D12Resource> native_color;native_color.Attach(na::observe::AcquireResource(c));
        ComPtr<ID3D12Resource> native_depth;native_depth.Attach(na::observe::AcquireResource(d));
        if(!native_color||!native_depth)return {};
        const auto native_size=native_color->GetDesc();member(c,native_color.Get(),Role::Color);member(d,native_depth.Get(),Role::Depth);
        // A copied full-size picture is a scene dependency only when scene
        // work actually binds it. Presentation copies are native consumers.
        for(const auto& view:op.descriptors)if(view.view.kind==NA_DESC_SRV)dependency(view.view.resource);
        for(const auto& rt:op.colors)if(rt.resource){const auto size=rt.resource->GetDesc();
            if(size.Width==native_size.Width&&size.Height==native_size.Height)member(rt.view.resource,rt.resource.Get(),rt.view.resource==c?Role::Color:Role::Intermediate);}
        const uint64_t key=signature(op);
        last_seen[key]=epoch;
        if(!observed.count(key)&&observed.size()<16384){observed.emplace(key,std::make_shared<const NativeOperation>(std::move(op)));++model;
            pending.push_back(key);queued.insert(key);work_pending.store(true);last_novel_epoch=epoch;}
        if(!run||run->aborted)return {};
        const auto learned=observed.find(key);if(learned==observed.end()){abort(list,run,"The bounded scene plan cache is full.");return {};}
        for(const auto& descriptor:learned->second->descriptors)if(members.count(descriptor.view.resource)&&!run->built->group->Find(descriptor.view.resource)){
            abort(list,run,"Preparing a newly observed scene dependency.");return {};
        }
        const auto plan=run->built->bindings.find(key);
        if(plan==run->built->bindings.end()){abort(list,run,"Preparing a newly observed scene operation.");return {};}
        if(!run->seeded){run->frame->UpdateStates(states(list,run,true),true);
            if(!run->frame->SeedHistories(list)){abort(list,run,"The scene's initial contents are not available yet.");return {};}run->seeded=true;}
        return plan->second;
    }
    void Refused(ID3D12GraphicsCommandList* list,const na::observe::ShaderCall&,const char* why)noexcept override{
        std::lock_guard<std::mutex> lock(mutex);const auto found=bound.find({na::observe::IdOf(list),na::observe::ListRecording(list)});
        abort(list,found==bound.end()?nullptr:found->second.run,why);
    }
    void Applied(ID3D12GraphicsCommandList* list,const na::observe::ShaderCall& call,const std::shared_ptr<const PreparedBindings>&)noexcept override{
        NativeOperation op;if(!na::observe::ListNativeOperation(list,call.kind==na::observe::ShaderKind::Dispatch,op))return;
        std::lock_guard<std::mutex> lock(mutex);auto run=run_for(list);if(!run||run->aborted)return;
        for(const auto& rt:op.colors)run->frame->Written(rt.view.resource,list);
        const bool stencil=op.depth.view.dsv.Format==DXGI_FORMAT_D32_FLOAT_S8X24_UINT||op.depth.view.dsv.Format==DXGI_FORMAT_D24_UNORM_S8_UINT;
        if(op.depth.view.resource&&(!(op.depth.view.dsv.Flags&D3D12_DSV_FLAG_READ_ONLY_DEPTH)||(stencil&&!(op.depth.view.dsv.Flags&D3D12_DSV_FLAG_READ_ONLY_STENCIL))))
            run->frame->Written(op.depth.view.resource,list);
        for(const auto& view:op.descriptors)if(view.view.kind==NA_DESC_UAV&&run->built->group->Find(view.view.resource))run->frame->Written(view.view.resource,list);
        run->frame->Ready();++stats.redirected;stats.active=true;
        char text[192];std::snprintf(text,sizeof text,"Scene rendering at %u x %u; DLSS status is shown below.",run->built->group->Render().width,run->built->group->Render().height);note(text);
    }
    void Closing(ID3D12GraphicsCommandList* list)noexcept override{
        std::lock_guard<std::mutex> lock(mutex);const auto found=bound.find({na::observe::IdOf(list),na::observe::ListRecording(list)});
        if(found!=bound.end()&&!checkpoint(list,found->second.run))abort(list,found->second.run,"A scene list could not publish its full-size dependencies.");
    }
    void NativeBoundary(ID3D12GraphicsCommandList* list)noexcept override{
        std::lock_guard<std::mutex> lock(mutex);const auto found=bound.find({na::observe::IdOf(list),na::observe::ListRecording(list)});
        if(found!=bound.end())abort(list,found->second.run,"The scene uses an indirect, bundle or render-pass operation; using its current native picture.");
    }
    na::observe::ShaderScope Before(ID3D12GraphicsCommandList* list,const TextureCall& call)noexcept override{
        if(na::settings::Get().j2_scale_mode.load()==0)return {};
        const auto key=transfer_key(call);na::observe::OperationDescriptor descriptor;
        if(call.target.ptr)na::observe::NativeDescriptor(call.target,descriptor);
        std::lock_guard<std::mutex> lock(mutex);if(!enabled)return {};
        if((call.kind==TextureKind::Copy||call.kind==TextureKind::CopyRegion||call.kind==TextureKind::Resolve)&&(members.count(key.source)||copied_from.count(key.source))&&call.destination){
            const auto a=call.source->GetDesc(),b=call.destination->GetDesc();
            if(a.Width==b.Width&&a.Height==b.Height&&(copied_from.size()<256||copied_from.count(key.destination)))copied_from[key.destination]={key.source,call.destination};
        }
        if(members.count(key.destination)&&(key.source==0||members.count(key.source))&&!observed_transfers.count(key)){
            TransferSpec value;value.kind=key.kind;value.source=key.source;value.destination=key.destination;value.resolve_format=key.format;
            value.color=descriptor.view.rtv;value.depth=descriptor.view.dsv;
            if(!descriptor.view.explicit_view&&descriptor.resource){value.color.Format=value.depth.Format=descriptor.resource->GetDesc().Format;
                value.color.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;value.depth.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;}
            observed_transfers.emplace(key,value);++model;work_pending.store(true);last_novel_epoch=epoch;
        }
        auto run=run_for(list);if(!run||run->aborted)return {};
        if(!run->built->group->Find(key.destination)&&!run->built->group->Find(key.source))return {};
        // Publish pending scene writes before the native copy reads them. The
        // native operation still runs, keeping full-size clear/copy semantics.
        if(!checkpoint(list,run)&&run->seeded){abort(list,run,"A scene transfer could not publish its input.");return {};}
        const auto transfer=run->built->transfers.find(key);
        if(transfer==run->built->transfers.end()){
            if(run->built->group->Find(key.destination))abort(list,run,"Preparing a newly observed scene transfer.");return {};
        }
        const char* why=nullptr;
        if(!transfer->second->Record(list,call,&why)){abort(list,run,why?why:"The scene transfer changed.");return {};}
        if(call.kind==TextureKind::ClearDepth&&(call.flags&D3D12_CLEAR_FLAG_STENCIL))run->frame->StencilCleared(key.destination);
        return {na::observe::ShaderAction::Forward,run};
    }
    void After(ID3D12GraphicsCommandList* list,const TextureCall& call,na::observe::ShaderScope& scope)noexcept override{
        const auto run=std::static_pointer_cast<Run>(scope.state);if(!run)return;
        std::lock_guard<std::mutex> lock(mutex);const auto key=transfer_key(call);
        run->frame->Written(key.destination,list);run->frame->NativeCurrent(key.destination,list);
    }
};
std::shared_ptr<Automatic>& automatic(){static auto* value=new std::shared_ptr<Automatic>();return *value;}
na::j2::PhysicalLookup lookup(void*,uint64_t color,uint64_t depth,ID3D12GraphicsCommandList* list,std::shared_ptr<const na::j2::PhysicalInput>* out){
    auto a=automatic();if(!a||!out)return na::j2::PhysicalLookup::None;out->reset();std::shared_ptr<Run> run;
    {std::lock_guard<std::mutex> lock(a->mutex);
        if(list){const auto found=a->bound.find({na::observe::IdOf(list),na::observe::ListRecording(list)});if(found!=a->bound.end())run=found->second.run;}
        else if(submission)run=submission->run;
        if(!run||run->aborted||run->color!=color||run->depth!=depth)return na::j2::PhysicalLookup::None;
        if(submission)reporting=submission;else{reporting=std::make_shared<Submission>();reporting->run=run;}
    }
    if(list){std::vector<FrameMemberState> current;
        for(const auto& member:run->built->group->Members()){
            uint32_t state=0;const auto evidence=na::observe::ListStateSub(list,member.id,0,&state);
            if(evidence==na::observe::StateLookup::Known||(evidence==na::observe::StateLookup::Absent&&na::observe::KnownSubStateInCurrentSubmission(member.id,0,&state)))
                current.push_back({member.id,static_cast<D3D12_RESOURCE_STATES>(state)});
        }
        run->frame->UpdateStates(current);
    }
    return run->frame->Input(out);
}
void outcome(void*,uint64_t generation,na::j2::PhysicalOutcome result,const char*){
    auto report=std::move(reporting);if(report&&report->run&&report->run->built->group->Generation()==generation)report->run->frame->Outcome(result);
}
}
void ConfigureAutomaticScene(bool enabled){
    if(!enabled)return;
    auto a=std::make_shared<Automatic>();a->enabled=true;a->stats.enabled=true;a->note("Waiting for the game's scene selection.");automatic()=a;
    na::observe::SetOperationSnapshots(true);na::observe::SetRasterStateTracking(true);
    na::observe::SetShaderOperationHook(na::observe::ShaderLane::Bindings,std::make_shared<BindingHook>(a));
    na::observe::SetTextureOperationHook(a);na::j2::SetPhysicalInputProvider({&lookup,&outcome,nullptr,true});
}
void PollAutomaticScene(){
    auto a=automatic();if(!a)return;
    const bool optimal_available=na::dlss::GetCapabilities().optimal_settings;
    std::map<uint64_t,MemberSeed> members;std::unordered_map<uint64_t,std::shared_ptr<const NativeOperation>> observed;
    std::vector<std::shared_ptr<const NativeOperation>> retired_requests;
    std::map<TransferKey,TransferSpec> transfers;std::shared_ptr<Built> previous;uint64_t model=0,color=0,depth=0,generation=0,group_revision=0;int mode=0;
    {std::lock_guard<std::mutex> lock(a->mutex);mode=static_cast<int>(na::settings::Get().j2_scale_mode.load());
        // Material/streaming resources belong to executable recordings, not an
        // immortal discovery cache. Evicted plans remain alive through leases.
        for(auto it=a->last_seen.begin();it!=a->last_seen.end();)if(a->epoch-it->second>240){
            const auto old=a->observed.find(it->first);if(old!=a->observed.end()){retired_requests.push_back(std::move(old->second));a->observed.erase(old);}it=a->last_seen.erase(it);
        }else++it;
        if(!a->enabled||!a->color||!a->depth||a->members.size()<2||mode<=0){if(mode<=0){a->prepared.reset();a->failed_group_revision=0;}a->work_pending.store(false);return;}
        const auto latest=a->prepared?a->prepared:a->built;
        if(a->failed_group_revision==a->group_revision&&a->failed_group_mode==mode&&a->failed_group_optimal==optimal_available){a->work_pending.store(false);return;}
        const bool current_basis=latest&&latest->mode==mode&&latest->group_revision==a->group_revision&&latest->optimal_available==optimal_available;
        if(a->worker_revision!=a->group_revision||a->worker_mode!=mode||a->worker_optimal!=optimal_available){
            a->pending.clear();a->queued.clear();
            for(const auto& op:a->observed){a->pending.push_back(op.first);a->queued.insert(op.first);}
            a->worker_revision=a->group_revision;a->worker_mode=mode;a->worker_optimal=optimal_available;
        }
        // Only shared request pointers cross the control lock; descriptor
        // vectors/resources are immutable and prepared outside it.
        constexpr size_t budget=16;
        while(!a->pending.empty()&&observed.size()<budget){const auto key=a->pending.front();a->pending.pop_front();a->queued.erase(key);
            const auto request=a->observed.find(key);if(request==a->observed.end())continue;
            if(current_basis&&(latest->bindings.count(key)||latest->failed.count(key)))continue;
            observed.insert(*request);
        }
        for(const auto& transfer:a->observed_transfers){
            if(current_basis&&(latest->transfers.count(transfer.first)||latest->failed_transfers.count(transfer.first)))continue;
            if(transfers.size()+observed.size()>=budget)break;transfers.insert(transfer);
        }
        if(current_basis&&observed.empty()&&transfers.empty()){a->work_pending.store(false);return;}
        ++a->stats.poll_builds;
        members=a->members;previous=latest;model=a->model;color=a->color;depth=a->depth;generation=a->next_generation++;group_revision=a->group_revision;
    }
    const na::observe::HostWorkScope host;
    ComPtr<ID3D12Device> device;if(FAILED(members.at(color).resource->GetDevice(IID_PPV_ARGS(&device))))return;
    const auto native=members.at(color).resource->GetDesc();const Extent output{UINT(native.Width),native.Height};
    const double fraction=FractionForMode(mode);Extent render{UINT(std::max(1.0,std::round(output.width*fraction))),UINT(std::max(1.0,std::round(output.height*fraction)))};
    const na::dlss::Quality qualities[]={na::dlss::Quality::DLAA,na::dlss::Quality::MaxQuality,na::dlss::Quality::Balanced,na::dlss::Quality::MaxPerformance,na::dlss::Quality::UltraPerformance};
    na::dlss::OptimalSettings optimal;
    const bool optimal_used=mode>=1&&mode<=4&&na::dlss::IsBoundTo(device.Get())&&na::dlss::QueryOptimal(output.width,output.height,qualities[mode],&optimal)&&optimal.valid;
    if(optimal_used)
        render={optimal.render_w,optimal.render_h};
    auto next=std::make_shared<Built>();next->mode=mode;next->model=model;next->group_revision=group_revision;next->optimal=optimal_used;next->optimal_available=optimal_available;std::string error;
    bool same=previous&&previous->mode==mode&&previous->group->Render()==render&&previous->group->Output()==output&&previous->group->Members().size()==members.size();
    if(same)for(const auto& m:members)same&=previous->group->Find(m.first)!=nullptr;
    if(same){next->group=previous->group;next->prototype=previous->prototype;
        next->bindings=previous->bindings;next->failed=previous->failed;
        next->transfers=previous->transfers;next->failed_transfers=previous->failed_transfers;}
    else{
        std::vector<MemberRequest> requests;for(const auto& value:members)requests.push_back({value.first,value.second.resource.Get(),value.second.role,render});
        next->group=SceneGroup::Create(device.Get(),generation,output,render,requests,error);
        if(next->group){FrameSpec frame;frame.group=next->group;frame.color=color;frame.depth=depth;
            for(const auto& m:members){const auto state=m.second.role==Role::Depth?D3D12_RESOURCE_STATE_DEPTH_WRITE:D3D12_RESOURCE_STATE_RENDER_TARGET;
                const bool transfer_only=(m.second.resource->GetDesc().Flags&(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL|D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))==0;
                frame.members.push_back({m.first,state,false,state,transfer_only});}
            next->prototype=SceneFrame::Create(device.Get(),std::move(frame),error);}
    }
    if(next->prototype){
        for(const auto& op:observed)if(!next->bindings.count(op.first)&&!next->failed.count(op.first)){
            BindingSpec spec;if(!NativeBindingSpec(*op.second,next->group,spec,error)){next->failed.emplace(op.first,error);continue;}
            auto plan=PreparedBindings::Create(device.Get(),std::move(spec),error);if(plan)next->bindings.emplace(op.first,std::move(plan));else next->failed.emplace(op.first,error);
        }
        for(auto transfer:transfers)if(!next->transfers.count(transfer.first)&&!next->failed_transfers.count(transfer.first)){
            transfer.second.group=next->group;auto plan=PreparedTransfer::Create(device.Get(),std::move(transfer.second),error);if(plan)next->transfers.emplace(transfer.first,std::move(plan));else next->failed_transfers.emplace(transfer.first,error);
        }
    }
    std::lock_guard<std::mutex> lock(a->mutex);
    if(a->color!=color||a->depth!=depth||mode!=static_cast<int>(na::settings::Get().j2_scale_mode.load())||a->group_revision!=group_revision){a->worker_revision=0;return;}
    if(!next->prototype){a->failed_group_revision=group_revision;a->failed_group_mode=mode;a->failed_group_optimal=optimal_available;
        a->stats.failed_plans=1;a->work_pending.store(false);a->note(error.empty()?"Preparing scene bindings.":error.c_str());return;}
    a->prepared=std::move(next);a->stats.generation=a->prepared->group->Generation();a->stats.render_w=render.width;a->stats.render_h=render.height;
    a->stats.out_w=output.width;a->stats.out_h=output.height;a->stats.members=UINT(members.size());a->stats.plans=UINT(a->prepared->bindings.size());
    a->stats.failed_plans=a->prepared->failed.size()+a->prepared->failed_transfers.size();if(!same)++a->stats.group_builds;
    if(a->stats.failed_plans)a->note(!a->prepared->failed.empty()?a->prepared->failed.begin()->second.c_str():a->prepared->failed_transfers.begin()->second.c_str());
    else a->note("Reduced scene prepared; applying at the next frame boundary.");
    bool more=!a->pending.empty();for(const auto& transfer:a->observed_transfers)more|=!a->prepared->transfers.count(transfer.first)&&!a->prepared->failed_transfers.count(transfer.first);
    a->work_pending.store(more);
}
bool AutomaticSceneWorkPending(){auto a=automatic();return a&&a->work_pending.load();}
void AutomaticScenePresent(){auto a=automatic();if(!a)return;std::lock_guard<std::mutex> lock(a->mutex);++a->epoch;a->current.reset();
    if(na::settings::Get().j2_scale_mode.load()==0){a->built.reset();a->stats.active=false;a->note("Rendering at native resolution.");}else if(a->prepared)a->built=a->prepared;}
AutomaticSceneStats GetAutomaticSceneStats(){auto a=automatic();if(!a)return {};std::lock_guard<std::mutex> lock(a->mutex);return a->stats;}
AutomaticSceneSubmission::AutomaticSceneSubmission(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList* const* lists){
    prior_=submission;auto a=automatic();if(!a){submission.reset();return;}
    auto value=std::make_shared<Submission>();value->queue=queue;value->lists.assign(lists,lists+count);
    std::lock_guard<std::mutex> lock(a->mutex);
    for(UINT i=0;i<count;++i){const auto key=std::make_pair(na::observe::IdOf(lists[i]),na::observe::ListRecording(lists[i]));
        const uint64_t queue_id=na::observe::IdOf(queue);const auto old=a->list_queues.find(key.first);
        if(old!=a->list_queues.end()&&old->second!=queue_id)old->second=0;else if(old==a->list_queues.end())a->list_queues.emplace(key.first,queue_id);
        if(!a->execution_queue&&queue->GetDesc().Type==D3D12_COMMAND_LIST_TYPE_DIRECT&&a->color&&a->depth&&na::observe::ListRendersTo(lists[i],a->color,a->depth))a->execution_queue=queue;
        const auto found=a->bound.find(key);
        if(found==a->bound.end())continue;if(value->run&&value->run!=found->second.run){value->run.reset();break;}value->run=found->second.run;}
    if(value->run){if(a->execution_queue&&a->execution_queue.Get()!=queue)value->run->aborted=true;else a->execution_queue=queue;}
    submission=std::move(value);
}
AutomaticSceneSubmission::~AutomaticSceneSubmission(){submission=std::static_pointer_cast<Submission>(prior_);}
}
